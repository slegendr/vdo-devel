// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "data-vio.h"

#include <linux/atomic.h>
#include <linux/bio.h>
#include <linux/delay.h>
#include <linux/device-mapper.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/lz4.h>
#include <linux/minmax.h>
#include <linux/murmurhash3.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#ifndef VDO_UPSTREAM
#include <linux/version.h>
#endif /* VDO_UPSTREAM */
#include <linux/wait.h>

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "block-allocator.h"
#include "block-map.h"
#include "dump.h"
#include "int-map.h"
#include "io-submitter.h"
#include "logical-zone.h"
#include "packer.h"
#include "recovery-journal.h"
#include "slab-depot.h"
#include "slab-journal.h"
#include "status-codes.h"
#include "types.h"
#include "vdo.h"
#include "vdo-component.h"
#include "vdo-component-states.h"
#include "vio.h"

/**
 * DOC: Bio flags.
 *
 * For certain flags set on user bios, if the user bio has not yet been acknowledged, setting those
 * flags on our own bio(s) for that request may help underlying layers better fulfill the user
 * bio's needs. This constant contains the aggregate of those flags; VDO strips all the other
 * flags, as they convey incorrect information.
 *
 * These flags are always irrelevant if we have already finished the user bio as they are only
 * hints on IO importance. If VDO has finished the user bio, any remaining IO done doesn't care how
 * important finishing the finished bio was.
 *
 * Note that bio.c contains the complete list of flags we believe may be set; the following list
 * explains the action taken with each of those flags VDO could receive:
 *
 * * REQ_SYNC: Passed down if the user bio is not yet completed, since it indicates the user bio
 *   completion is required for further work to be done by the issuer.
 * * REQ_META: Passed down if the user bio is not yet completed, since it may mean the lower layer
 *   treats it as more urgent, similar to REQ_SYNC.
 * * REQ_PRIO: Passed down if the user bio is not yet completed, since it indicates the user bio is
 *   important.
 * * REQ_NOMERGE: Set only if the incoming bio was split; irrelevant to VDO IO.
 * * REQ_IDLE: Set if the incoming bio had more IO quickly following; VDO's IO pattern doesn't
 *   match incoming IO, so this flag is incorrect for it.
 * * REQ_FUA: Handled separately, and irrelevant to VDO IO otherwise.
 * * REQ_RAHEAD: Passed down, as, for reads, it indicates trivial importance.
 * * REQ_BACKGROUND: Not passed down, as VIOs are a limited resource and VDO needs them recycled
 *   ASAP to service heavy load, which is the only place where REQ_BACKGROUND might aid in load
 *   prioritization.
 */
static unsigned int PASSTHROUGH_FLAGS = (REQ_PRIO | REQ_META | REQ_SYNC | REQ_RAHEAD);

/**
 * DOC:
 *
 * The data_vio_pool maintains the pool of data_vios which a vdo uses to service incoming bios. For
 * correctness, and in order to avoid potentially expensive or blocking memory allocations during
 * normal operation, the number of concurrently active data_vios is capped. Furthermore, in order
 * to avoid starvation of reads and writes, at most 75% of the data_vios may be used for
 * discards. The data_vio_pool is responsible for enforcing these limits. Threads submitting bios
 * for which a data_vio or discard permit are not available will block until the necessary
 * resources are available. The pool is also responsible for distributing resources to blocked
 * threads and waking them. Finally, the pool attempts to batch the work of recycling data_vios by
 * performing the work of actually assigning resources to blocked threads or placing data_vios back
 * into the pool on a single cpu at a time.
 *
 * The pool contains two "limiters", one for tracking data_vios and one for tracking discard
 * permits. The limiters also provide safe cross-thread access to pool statistics without the need
 * to take the pool's lock. When a thread submits a bio to a vdo device, it will first attempt to
 * get a discard permit if it is a discard, and then to get a data_vio. If the necessary resources
 * are available, the incoming bio will be assigned to the acquired data_vio, and it will be
 * launched. However, if either of these are unavailable, the arrival time of the bio is recorded
 * in the bio's bi_private field, the bio and its submitter are both queued on the appropriate
 * limiter and the submitting thread will then put itself to sleep. (note that this mechanism will
 * break if jiffies are only 32 bits.)
 *
 * Whenever a data_vio has completed processing for the bio it was servicing, release_data_vio()
 * will be called on it. This function will add the data_vio to a funnel queue, and then check the
 * state of the pool. If the pool is not currently processing released data_vios, the pool's
 * completion will be enqueued on a cpu queue. This obviates the need for the releasing threads to
 * hold the pool's lock, and also batches release work while avoiding starvation of the cpu
 * threads.
 *
 * Whenever the pool's completion is run on a cpu thread, it calls process_release_callback() which
 * processes a batch of returned data_vios (currently at most 32) from the pool's funnel queue. For
 * each data_vio, it first checks whether that data_vio was processing a discard. If so, and there
 * is a blocked bio waiting for a discard permit, that permit is notionally transferred to the
 * eldest discard waiter, and that waiter is moved to the end of the list of discard bios waiting
 * for a data_vio. If there are no discard waiters, the discard permit is returned to the pool.
 * Next, the data_vio is assigned to the oldest blocked bio which either has a discard permit, or
 * doesn't need one and relaunched. If neither of these exist, the data_vio is returned to the
 * pool. Finally, if any waiting bios were launched, the threads which blocked trying to submit
 * them are awakened.
 */

enum {
	DATA_VIO_RELEASE_BATCH_SIZE = 128,
};

static const unsigned int VDO_SECTORS_PER_BLOCK_MASK = VDO_SECTORS_PER_BLOCK - 1;
static const u32 COMPRESSION_STATUS_MASK = 0xff;
static const u32 MAY_NOT_COMPRESS_MASK = 0x80000000;

struct limiter;
typedef void assigner(struct limiter *limiter);

/*
 * Bookkeeping structure for a single type of resource.
 */
struct limiter {
	/* The data_vio_pool to which this limiter belongs */
	struct data_vio_pool *pool;
	/* The maximum number of data_vios available */
	data_vio_count_t limit;
	/* The number of resources in use */
	data_vio_count_t busy;
	/* The maximum number of resources ever simultaneously in use */
	data_vio_count_t max_busy;
	/* The number of resources to release */
	data_vio_count_t release_count;
	/* The number of waiters to wake */
	data_vio_count_t wake_count;
	/* The list of waiting bios which are known to process_release_callback() */
	struct bio_list waiters;
	/* The list of waiting bios which are not yet known to process_release_callback() */
	struct bio_list new_waiters;
	/* The list of waiters which have their permits */
	struct bio_list *permitted_waiters;
	/* The function for assigning a resource to a waiter */
	assigner *assigner;
	/* The queue of blocked threads */
	wait_queue_head_t blocked_threads;
	/* The arrival time of the eldest waiter */
	u64 arrival;
};

/*
 * A data_vio_pool is a collection of preallocated data_vios which may be acquired from any thread,
 * and are released in batches.
 */
struct data_vio_pool {
	/* Completion for scheduling releases */
	struct vdo_completion completion;
	/* The administrative state of the pool */
	struct admin_state state;
	/* Lock protecting the pool */
	spinlock_t lock;
	/* The main limiter controlling the total data_vios in the pool. */
	struct limiter limiter;
	/* The limiter controlling data_vios for discard */
	struct limiter discard_limiter;
	/* The list of bios which have discard permits but still need a data_vio */
	struct bio_list permitted_discards;
	/* The list of available data_vios */
	struct list_head available;
	/* The queue of data_vios waiting to be returned to the pool */
	struct funnel_queue *queue;
	/* Whether the pool is processing, or scheduled to process releases */
	atomic_t processing;
	/* The data vios in the pool */
	struct data_vio data_vios[];
};

static const char * const ASYNC_OPERATION_NAMES[] = {
	"launch",
	"acknowledge_write",
	"acquire_hash_lock",
	"attempt_logical_block_lock",
	"lock_duplicate_pbn",
	"check_for_duplication",
	"cleanup",
	"compress_data_vio",
	"decrement_reference_count",
	"increment_reference_count",
	"find_block_map_slot",
	"get_mapped_block_for_read",
	"get_mapped_block_for_write",
	"hash_data_vio",
	"journal_mapping_for_optimization",
	"journal_mapping_for_write",
	"journal_unmapping",
	"vdo_attempt_packing",
	"put_mapped_block",
	"read_data_vio",
	"update_dedupe_index",
	"verify_duplication",
	"write_data_vio",
};

/* The steps taken cleaning up a VIO, in the order they are performed. */
enum data_vio_cleanup_stage {
	VIO_CLEANUP_START,
	VIO_RELEASE_HASH_LOCK = VIO_CLEANUP_START,
	VIO_RELEASE_ALLOCATED,
	VIO_RELEASE_RECOVERY_LOCKS,
	VIO_RELEASE_LOGICAL,
	VIO_CLEANUP_DONE
};

/**
 * as_data_vio_pool() - Convert a vdo_completion to a data_vio_pool.
 * @completion: The completion to convert.
 *
 * Return: The completion as a data_vio_pool.
 */
static inline struct data_vio_pool * __must_check
as_data_vio_pool(struct vdo_completion *completion)
{
	vdo_assert_completion_type(completion->type, VDO_DATA_VIO_POOL_COMPLETION);
	return container_of(completion, struct data_vio_pool, completion);
}

static inline u64 get_arrival_time(struct bio *bio)
{
	return (u64) bio->bi_private;
}

/**
 * check_for_drain_complete_locked() - Check whether a data_vio_pool has no outstanding data_vios
 *				       or waiters while holding the pool's lock.
 * @pool: The pool to check.
 *
 * Return: true if the pool has no busy data_vios or waiters.
 */
static bool check_for_drain_complete_locked(struct data_vio_pool *pool)
{
	if (pool->limiter.busy > 0)
		return false;

	ASSERT_LOG_ONLY((pool->discard_limiter.busy == 0), "no outstanding discard permits");

	return (bio_list_empty(&pool->limiter.new_waiters) &&
		bio_list_empty(&pool->discard_limiter.new_waiters));
}

/**
 * initialize_lbn_lock() - Initialize the LBN lock of a data_vio.
 * @data_vio: The data_vio to initialize.
 * @lbn: The lbn on which the data_vio will operate.
 *
 * In addition to recording the LBN on which the data_vio will operate, it will also find the
 * logical zone associated with the LBN.
 */
static void initialize_lbn_lock(struct data_vio *data_vio, logical_block_number_t lbn)
{
	struct vdo *vdo = vdo_from_data_vio(data_vio);
	zone_count_t zone_number;
	struct lbn_lock *lock = &data_vio->logical;

	lock->lbn = lbn;
	lock->locked = false;
	initialize_wait_queue(&lock->waiters);
	zone_number = vdo_compute_logical_zone(data_vio);
	lock->zone = &vdo->logical_zones->zones[zone_number];
}

/**
 * launch_locked_request() - Launch a request which has acquired an LBN lock.
 * @data_vio: The data_vio which has just acquired a lock.
 */
static void launch_locked_request(struct data_vio *data_vio)
{
	data_vio->logical.locked = true;
	if (data_vio->write) {
		struct vdo *vdo = vdo_from_data_vio(data_vio);

		if (vdo_is_read_only(vdo->read_only_notifier)) {
			continue_data_vio_with_error(data_vio, VDO_READ_ONLY);
			return;
		}
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_FIND_BLOCK_MAP_SLOT;
	vdo_find_block_map_slot(data_vio);
}

static void acknowledge_data_vio(struct data_vio *data_vio)
{
	struct vdo *vdo = vdo_from_data_vio(data_vio);
	struct bio *bio = data_vio->user_bio;
	int error = vdo_map_to_system_error(data_vio->vio.completion.result);
#ifdef VDO_INTERNAL
	u64 latency_jiffies;
	unsigned int ack_msecs;
	struct vdo_histograms *histograms = &vdo->histograms;
#endif /* VDO_INTERNAL */

	if (bio == NULL)
		return;

	ASSERT_LOG_ONLY((data_vio->remaining_discard <=
			 (u32) (VDO_BLOCK_SIZE - data_vio->offset)),
			"data_vio to acknowledge is not an incomplete discard");

	data_vio->user_bio = NULL;
	vdo_count_bios(&vdo->stats.bios_acknowledged, bio);
	if (data_vio->is_partial)
		vdo_count_bios(&vdo->stats.bios_acknowledged_partial, bio);

#ifdef VDO_INTERNAL
	latency_jiffies = jiffies - data_vio->arrival_jiffies;
	ack_msecs = jiffies_to_msecs(latency_jiffies);
	if (bio_data_dir(bio) != WRITE)
		enter_histogram_sample(histograms->read_ack_histogram, latency_jiffies);
	else if (bio_op(bio) == REQ_OP_DISCARD)
		enter_histogram_sample(histograms->discard_ack_histogram, latency_jiffies);
	else
		enter_histogram_sample(histograms->write_ack_histogram, latency_jiffies);

	if (ack_msecs > 30000) {
		static DEFINE_RATELIMIT_STATE(latency_limiter,
					      DEFAULT_RATELIMIT_INTERVAL,
					      DEFAULT_RATELIMIT_BURST);

		if (__ratelimit(&latency_limiter)) {
			uds_log_info("Acknowledgement Latency Violation: %u msecs, error %d",
				     ack_msecs, -error);
			dump_data_vio(data_vio);
		}
	}

#endif
	bio->bi_status = errno_to_blk_status(error);
	bio_endio(bio);
}

static void copy_to_bio(struct bio *bio, char *data_ptr)
{
	struct bio_vec biovec;
	struct bvec_iter iter;

	bio_for_each_segment(biovec, bio, iter) {
		memcpy_to_bvec(&biovec, data_ptr);
		data_ptr += biovec.bv_len;
	}
}

/**
 * get_vio_compression_state() - Get the compression status of a data_vio.
 * @data_vio: The data_vio.
 *
 * Return: The compression status.
 */
struct data_vio_compression_status
get_data_vio_compression_status(struct data_vio *data_vio)
{
	u32 packed = atomic_read(&data_vio->compression.status);

	/* pairs with cmpxchg in set_data_vio_compression_status */
	smp_rmb();
	return (struct data_vio_compression_status) {
		.stage = packed & COMPRESSION_STATUS_MASK,
		.may_not_compress = ((packed & MAY_NOT_COMPRESS_MASK) != 0),
	};
}

/**
 * pack_status() - Convert a data_vio_compression_status into a u32 which may be stored
 *                 atomically.
 * @status: The state to convert.
 *
 * Return: The compression state packed into a u32.
 */
static u32 __must_check pack_status(struct data_vio_compression_status status)
{
	return status.stage | (status.may_not_compress ? MAY_NOT_COMPRESS_MASK : 0);
}

/**
 * set_data_vio_compression_status() - Set the compression status of a data_vio.
 * @data_vio: The data_vio whose compression status is to be set.
 * @state: The expected current status of the data_vio.
 * @new_state: The status to set.
 *
 * Return: true if the new status was set, false if the data_vio's compression status did not
 *         match the expected state, and so was left unchanged.
 */
EXTERNAL_STATIC bool __must_check
set_data_vio_compression_status(struct data_vio *data_vio,
				struct data_vio_compression_status status,
				struct data_vio_compression_status new_status)
{
	u32 actual;
	u32 expected = pack_status(status);
	u32 replacement = pack_status(new_status);

	/*
	 * Extra barriers because this was original developed using a CAS operation that implicitly
	 * had them.
	 */
	smp_mb__before_atomic();
	actual = atomic_cmpxchg(&data_vio->compression.status, expected, replacement);
	/* same as before_atomic */
	smp_mb__after_atomic();
	return (expected == actual);
}

/**
 * advance_data_vio_compression_status() - Advance to the next compression status along the
 *		                           compression path.
 * @data_vio: The data_vio to advance.
 *
 * Return: The new compression status of the data_vio.
 */
struct data_vio_compression_status advance_data_vio_compression_stage(struct data_vio *data_vio)
{
	for (;;) {
		struct data_vio_compression_status status =
			get_data_vio_compression_status(data_vio);
		struct data_vio_compression_status new_status = status;

		if (status.stage == DATA_VIO_POST_PACKER)
			/* We're already in the last stage. */
			return status;

		if (status.may_not_compress)
			/*
			 * Compression has been dis-allowed for this VIO, so skip the rest of the
			 * path and go to the end.
			 */
			new_status.stage = DATA_VIO_POST_PACKER;
		else
			/* Go to the next state. */
			new_status.stage++;

		if (set_data_vio_compression_status(data_vio, status, new_status))
			return new_status;

		/* Another thread changed the status out from under us so try again. */
	}
}

/**
 * cancel_data_vio_compression() - Prevent this data_vio from being compressed or packed.
 * @data_vio: The data_vio to cancel.
 *
 * Return: true if the data_vio is in the packer and the caller was the first caller to cancel it.
 */
bool cancel_data_vio_compression(struct data_vio *data_vio)
{
	struct data_vio_compression_status status, new_status;

	for (;;) {
		status = get_data_vio_compression_status(data_vio);
		if (status.may_not_compress || (status.stage == DATA_VIO_POST_PACKER))
			/* This data_vio is already set up to not block in the packer. */
			break;

		new_status.stage = status.stage;
		new_status.may_not_compress = true;

		if (set_data_vio_compression_status(data_vio, status, new_status))
			break;
	}

	return ((status.stage == DATA_VIO_PACKING) && !status.may_not_compress);
}

/**
 * attempt_logical_block_lock() - Attempt to acquire the lock on a logical block.
 * @completion: The data_vio for an external data request as a completion.
 *
 * This is the start of the path for all external requests. It is registered in launch_data_vio().
 */
static void attempt_logical_block_lock(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	struct lbn_lock *lock = &data_vio->logical;
	struct vdo *vdo = vdo_from_data_vio(data_vio);
	struct data_vio *lock_holder;
	int result;

	assert_data_vio_in_logical_zone(data_vio);

	if (data_vio->logical.lbn >= vdo->states.vdo.config.logical_blocks) {
		continue_data_vio_with_error(data_vio, VDO_OUT_OF_RANGE);
		return;
	}

	result = int_map_put(lock->zone->lbn_operations,
			     lock->lbn,
			     data_vio,
			     false,
			     (void **) &lock_holder);
	if (result != VDO_SUCCESS) {
		continue_data_vio_with_error(data_vio, result);
		return;
	}

	if (lock_holder == NULL) {
		/* We got the lock */
		launch_locked_request(data_vio);
		return;
	}

	result = ASSERT(lock_holder->logical.locked, "logical block lock held");
	if (result != VDO_SUCCESS) {
		continue_data_vio_with_error(data_vio, result);
		return;
	}

	/*
	 * If the new request is a pure read request (not read-modify-write) and the lock_holder is
	 * writing and has received an allocation (VDO-2683), service the read request immediately
	 * by copying data from the lock_holder to avoid having to flush the write out of the
	 * packer just to prevent the read from waiting indefinitely. If the lock_holder does not
	 * yet have an allocation, prevent it from blocking in the packer and wait on it.
	 */
	if (!data_vio->write && READ_ONCE(lock_holder->allocation_succeeded)) {
		copy_to_bio(data_vio->user_bio, (lock_holder->vio.data + data_vio->offset));
		acknowledge_data_vio(data_vio);
		complete_data_vio(completion);
		return;
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_ATTEMPT_LOGICAL_BLOCK_LOCK;
	enqueue_data_vio(&lock_holder->logical.waiters, data_vio);

	/*
	 * Prevent writes and read-modify-writes from blocking indefinitely on lock holders in the
	 * packer.
	 */
	if (lock_holder->write && cancel_data_vio_compression(lock_holder)) {
		data_vio->compression.lock_holder = lock_holder;
		launch_data_vio_packer_callback(data_vio, vdo_remove_lock_holder_from_packer);
	}
}

/**
 * launch_data_vio() - (Re)initialize a data_vio to have a new logical block number, keeping the
 *		       same parent and other state and send it on its way.
 * @data_vio: The data_vio to initialize.
 * @lbn: The logical block number of the data_vio.
 * @operation: The operation this data_vio will perform.
 */
static void launch_data_vio(struct data_vio *data_vio, logical_block_number_t lbn)
{
	struct vdo_completion *completion = &data_vio->vio.completion;

	/*
	 * Clearing the tree lock must happen before initializing the LBN lock, which also adds
	 * information to the tree lock.
	 */
	memset(&data_vio->tree_lock, 0, sizeof(data_vio->tree_lock));
	initialize_lbn_lock(data_vio, lbn);
	INIT_LIST_HEAD(&data_vio->hash_lock_entry);
	INIT_LIST_HEAD(&data_vio->write_entry);

	memset(&data_vio->allocation, 0, sizeof(data_vio->allocation));

	data_vio->is_duplicate = false;

	memset(&data_vio->record_name, 0, sizeof(data_vio->record_name));
	memset(&data_vio->duplicate, 0, sizeof(data_vio->duplicate));
	vdo_reset_completion(completion);
	completion->error_handler = handle_data_vio_error;
	set_data_vio_logical_callback(data_vio, attempt_logical_block_lock);
	vdo_invoke_completion_callback_with_priority(completion, VDO_DEFAULT_Q_MAP_BIO_PRIORITY);
}

/* Return true if a data block contains all zeros. */
EXTERNAL_STATIC bool is_zero_block(char *block)
{
	int i;

#ifdef INTERNAL
	STATIC_ASSERT(VDO_BLOCK_SIZE % sizeof(u64) == 0);
	ASSERT_LOG_ONLY((uintptr_t) block % sizeof(u64) == 0,
			"Data blocks are expected to be aligned");

#endif	/* INTERNAL */
	for (i = 0; i < VDO_BLOCK_SIZE; i += sizeof(u64))
		if (*((u64 *) &block[i]))
			return false;
	return true;
}

static void copy_from_bio(struct bio *bio, char *data_ptr)
{
	struct bio_vec biovec;
	struct bvec_iter iter;

	bio_for_each_segment(biovec, bio, iter) {
		memcpy_from_bvec(data_ptr, &biovec);
		data_ptr += biovec.bv_len;
	}
}

static void launch_bio(struct vdo *vdo, struct data_vio *data_vio, struct bio *bio)
{
	logical_block_number_t lbn;
#ifdef VDO_INTERNAL
	u64 arrival = get_arrival_time(bio);
	u64 startup_jiffies = jiffies - arrival;

	data_vio->arrival_jiffies = arrival;
	if (unlikely(startup_jiffies > 1))
		enter_histogram_sample(vdo->histograms.start_request_histogram, startup_jiffies);
#endif /* VDO_INTERNAL */
	/*
	 * Zero out the fields which don't need to be preserved (i.e. which are not pointers to
	 * separately allocated objects).
	 */
	memset(data_vio, 0, offsetof(struct data_vio, vio));
	memset(&data_vio->compression, 0, offsetof(struct compression_state, block));

	data_vio->user_bio = bio;
	data_vio->offset = to_bytes(bio->bi_iter.bi_sector & VDO_SECTORS_PER_BLOCK_MASK);
	data_vio->is_partial = (bio->bi_iter.bi_size < VDO_BLOCK_SIZE) || (data_vio->offset != 0);

	/*
	 * Discards behave very differently than other requests when coming in from device-mapper.
	 * We have to be able to handle any size discards and various sector offsets within a
	 * block.
	 */
	if (bio_op(bio) == REQ_OP_DISCARD) {
		data_vio->remaining_discard = bio->bi_iter.bi_size;
		data_vio->write = true;
		data_vio->is_trim = true;
		if (data_vio->is_partial) {
			vdo_count_bios(&vdo->stats.bios_in_partial, bio);
			data_vio->read = true;
		}
	} else if (data_vio->is_partial) {
		vdo_count_bios(&vdo->stats.bios_in_partial, bio);
		data_vio->read = true;
		if (bio_data_dir(bio) == WRITE)
			data_vio->write = true;
	} else if (bio_data_dir(bio) == READ) {
		data_vio->read = true;
	} else {
		/*
		 * Copy the bio data to a char array so that we can continue to use the data after
		 * we acknowledge the bio.
		 */
		copy_from_bio(bio, data_vio->vio.data);
		data_vio->is_zero = is_zero_block(data_vio->vio.data);
		data_vio->write = true;
	}

	if (data_vio->user_bio->bi_opf & REQ_FUA)
		data_vio->fua = true;

	lbn = (bio->bi_iter.bi_sector - vdo->starting_sector_offset) / VDO_SECTORS_PER_BLOCK;
	launch_data_vio(data_vio, lbn);
}

static void assign_data_vio(struct limiter *limiter, struct data_vio *data_vio)
{
	struct bio *bio = bio_list_pop(limiter->permitted_waiters);

	launch_bio(limiter->pool->completion.vdo, data_vio, bio);
	limiter->wake_count++;

	bio = bio_list_peek(limiter->permitted_waiters);
	limiter->arrival = ((bio == NULL) ? U64_MAX : get_arrival_time(bio));
}

static void assign_discard_permit(struct limiter *limiter)
{
	struct bio *bio = bio_list_pop(&limiter->waiters);

	if (limiter->arrival == U64_MAX)
		limiter->arrival = get_arrival_time(bio);

	bio_list_add(limiter->permitted_waiters, bio);
}

static void get_waiters(struct limiter *limiter)
{
	bio_list_merge(&limiter->waiters, &limiter->new_waiters);
	bio_list_init(&limiter->new_waiters);
}

static inline
struct data_vio *get_available_data_vio(struct data_vio_pool *pool)
{
	struct data_vio *data_vio =
		list_first_entry(&pool->available, struct data_vio, pool_entry);

	list_del_init(&data_vio->pool_entry);
	return data_vio;
}

static void assign_data_vio_to_waiter(struct limiter *limiter)
{
	assign_data_vio(limiter, get_available_data_vio(limiter->pool));
}

static void update_limiter(struct limiter *limiter)
{
	struct bio_list *waiters = &limiter->waiters;
	data_vio_count_t available = limiter->limit - limiter->busy;

	ASSERT_LOG_ONLY((limiter->release_count <= limiter->busy),
			"Release count %u is not more than busy count %u",
			limiter->release_count,
			limiter->busy);

	get_waiters(limiter);
	for (; (limiter->release_count > 0) && !bio_list_empty(waiters); limiter->release_count--)
		limiter->assigner(limiter);

	if (limiter->release_count > 0) {
		WRITE_ONCE(limiter->busy, limiter->busy - limiter->release_count);
		limiter->release_count = 0;
		return;
	}

	for (; (available > 0) && !bio_list_empty(waiters); available--)
		limiter->assigner(limiter);

	WRITE_ONCE(limiter->busy, limiter->limit - available);
	if (limiter->max_busy < limiter->busy)
		WRITE_ONCE(limiter->max_busy, limiter->busy);
}

/**
 * schedule_releases() - Ensure that release processing is scheduled.
 * @pool: The data_vio_pool which has resources to release.
 *
 * If this call switches the state to processing, enqueue. Otherwise, some other thread has already
 * done so.
 */
static void schedule_releases(struct data_vio_pool *pool)
{
	/* Pairs with the barrier in process_release_callback(). */
	smp_mb__before_atomic();
	if (atomic_cmpxchg(&pool->processing, false, true))
		return;

	pool->completion.requeue = true;
	vdo_invoke_completion_callback_with_priority(&pool->completion,
						     CPU_Q_COMPLETE_VIO_PRIORITY);
}

static void reuse_or_release_resources(struct data_vio_pool *pool,
				       struct data_vio *data_vio,
				       struct list_head *returned)
{
	if (data_vio->remaining_discard > 0) {
		if (bio_list_empty(&pool->discard_limiter.waiters))
			/* Return the data_vio's discard permit. */
			pool->discard_limiter.release_count++;
		else
			assign_discard_permit(&pool->discard_limiter);
	}

	if (pool->limiter.arrival < pool->discard_limiter.arrival) {
		assign_data_vio(&pool->limiter, data_vio);
	} else if (pool->discard_limiter.arrival < U64_MAX) {
		assign_data_vio(&pool->discard_limiter, data_vio);
	} else {
		list_add(&data_vio->pool_entry, returned);
		pool->limiter.release_count++;
	}
}

/**
 * process_release_callback() - Process a batch of data_vio releases.
 * @completion: The pool with data_vios to release.
 */
static void process_release_callback(struct vdo_completion *completion)
{
	struct data_vio_pool *pool = as_data_vio_pool(completion);
	bool reschedule;
	bool drained;
	data_vio_count_t processed;
	data_vio_count_t to_wake;
	data_vio_count_t discards_to_wake;
	LIST_HEAD(returned);

	spin_lock(&pool->lock);
	get_waiters(&pool->discard_limiter);
	get_waiters(&pool->limiter);
	spin_unlock(&pool->lock);

	if (pool->limiter.arrival == U64_MAX) {
		struct bio *bio = bio_list_peek(&pool->limiter.waiters);

		if (bio != NULL)
			pool->limiter.arrival = get_arrival_time(bio);
	}

	for (processed = 0; processed < DATA_VIO_RELEASE_BATCH_SIZE; processed++) {
		struct data_vio *data_vio;
		struct funnel_queue_entry *entry = funnel_queue_poll(pool->queue);

		if (entry == NULL)
			break;

		data_vio = as_data_vio(container_of(entry,
						    struct vdo_completion,
						    work_queue_entry_link));
		acknowledge_data_vio(data_vio);
		reuse_or_release_resources(pool, data_vio, &returned);
	}

	spin_lock(&pool->lock);
	/*
	 * There is a race where waiters could be added while we are in the unlocked section above.
	 * Those waiters could not see the resources we are now about to release, so we assign
	 * those resources now as we have no guarantee of being rescheduled. This is handled in
	 * update_limiter().
	 */
	update_limiter(&pool->discard_limiter);
	list_splice(&returned, &pool->available);
	update_limiter(&pool->limiter);
	to_wake = pool->limiter.wake_count;
	pool->limiter.wake_count = 0;
	discards_to_wake = pool->discard_limiter.wake_count;
	pool->discard_limiter.wake_count = 0;

	atomic_set(&pool->processing, false);
	/* Pairs with the barrier in schedule_releases(). */
	smp_mb();

	reschedule = !is_funnel_queue_empty(pool->queue);
	drained = (!reschedule &&
		   vdo_is_state_draining(&pool->state) &&
		   check_for_drain_complete_locked(pool));
	spin_unlock(&pool->lock);

	if (to_wake > 0)
		wake_up_nr(&pool->limiter.blocked_threads, to_wake);

	if (discards_to_wake > 0)
		wake_up_nr(&pool->discard_limiter.blocked_threads,
			   discards_to_wake);

	if (reschedule)
		schedule_releases(pool);
	else if (drained)
		vdo_finish_draining(&pool->state);
}

static void initialize_limiter(struct limiter *limiter,
			       struct data_vio_pool *pool,
			       assigner *assigner,
			       data_vio_count_t limit)
{
	limiter->pool = pool;
	limiter->assigner = assigner;
	limiter->limit = limit;
	limiter->arrival = U64_MAX;
	init_waitqueue_head(&limiter->blocked_threads);
}

/**
 * initialize_data_vio() - Allocate the components of a data_vio.
 * @data_vio: The data_vio being constructed.
 * @vdo: The vdo on which the data_vio will operate
 *
 * The caller is responsible for cleaning up the data_vio on error.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int initialize_data_vio(struct data_vio *data_vio, struct vdo *vdo)
{
	struct bio *bio;
	int result;

	STATIC_ASSERT(VDO_BLOCK_SIZE <= PAGE_SIZE);
	result = uds_allocate_memory(VDO_BLOCK_SIZE, 0, "data_vio data", &data_vio->vio.data);
	if (result != VDO_SUCCESS)
		return uds_log_error_strerror(result, "data_vio data allocation failure");

	result = uds_allocate_memory(VDO_BLOCK_SIZE,
				     0,
				     "compressed block",
				     &data_vio->compression.block);
	if (result != VDO_SUCCESS)
		return uds_log_error_strerror(result,
					      "data_vio compressed block allocation failure");

	result = uds_allocate_memory(VDO_BLOCK_SIZE, 0, "vio scratch", &data_vio->scratch_block);
	if (result != VDO_SUCCESS)
		return uds_log_error_strerror(result, "data_vio scratch allocation failure");

	result = vdo_create_bio(&bio);
	if (result != VDO_SUCCESS)
		return uds_log_error_strerror(result, "data_vio data bio allocation failure");

	initialize_vio(&data_vio->vio, bio, 1, VIO_TYPE_DATA, VIO_PRIORITY_DATA, vdo);
	return VDO_SUCCESS;
}

static void destroy_data_vio(struct data_vio *data_vio)
{
	if (data_vio == NULL)
		return;

	vdo_free_bio(UDS_FORGET(data_vio->vio.bio));
	UDS_FREE(UDS_FORGET(data_vio->vio.data));
	UDS_FREE(UDS_FORGET(data_vio->compression.block));
	UDS_FREE(UDS_FORGET(data_vio->scratch_block));
}

/**
 * make_data_vio_pool() - Initialize a data_vio pool.
 * @vdo: The vdo to which the pool will belong.
 * @pool_size: The number of data_vios in the pool.
 * @discard_limit: The maximum number of data_vios which may be used for discards.
 * @pool: A pointer to hold the newly allocated pool.
 */
int make_data_vio_pool(struct vdo *vdo,
		       data_vio_count_t pool_size,
		       data_vio_count_t discard_limit,
		       struct data_vio_pool **pool_ptr)
{
	int result;
	struct data_vio_pool *pool;
	data_vio_count_t i;

	result = UDS_ALLOCATE_EXTENDED(struct data_vio_pool,
				       pool_size,
				       struct data_vio,
				       __func__,
				       &pool);
	if (result != UDS_SUCCESS)
		return result;

	ASSERT_LOG_ONLY((discard_limit <= pool_size), "discard limit does not exceed pool size");
	initialize_limiter(&pool->discard_limiter, pool, assign_discard_permit, discard_limit);
	pool->discard_limiter.permitted_waiters = &pool->permitted_discards;
	initialize_limiter(&pool->limiter, pool, assign_data_vio_to_waiter, pool_size);
	pool->limiter.permitted_waiters = &pool->limiter.waiters;
	INIT_LIST_HEAD(&pool->available);
	spin_lock_init(&pool->lock);
	vdo_set_admin_state_code(&pool->state, VDO_ADMIN_STATE_NORMAL_OPERATION);
	vdo_initialize_completion(&pool->completion, vdo, VDO_DATA_VIO_POOL_COMPLETION);
	vdo_prepare_completion(&pool->completion,
			       process_release_callback,
			       process_release_callback,
			       vdo->thread_config->cpu_thread,
			       NULL);

	result = make_funnel_queue(&pool->queue);
	if (result != UDS_SUCCESS) {
		free_data_vio_pool(UDS_FORGET(pool));
		return result;
	}

	for (i = 0; i < pool_size; i++) {
		struct data_vio *data_vio = &pool->data_vios[i];

		result = initialize_data_vio(data_vio, vdo);
		if (result != VDO_SUCCESS) {
			destroy_data_vio(data_vio);
			free_data_vio_pool(pool);
			return result;
		}

		list_add(&data_vio->pool_entry, &pool->available);
	}

	*pool_ptr = pool;
	return VDO_SUCCESS;
}

/**
 * free_data_vio_pool() - Free a data_vio_pool and the data_vios in it.
 * @pool: The pool to free (may be NULL).
 *
 * All data_vios must be returned to the pool before calling this function.
 */
void free_data_vio_pool(struct data_vio_pool *pool)
{
	if (pool == NULL)
		return;

	/*
	 * Pairs with the barrier in process_release_callback(). Possibly not needed since it
	 * caters to an enqueue vs. free race.
	 */
	smp_mb();
	BUG_ON(atomic_read(&pool->processing));

	spin_lock(&pool->lock);
	ASSERT_LOG_ONLY((pool->limiter.busy == 0),
			"data_vio pool must not have %u busy entries when being freed",
			pool->limiter.busy);
	ASSERT_LOG_ONLY((bio_list_empty(&pool->limiter.waiters) &&
			 bio_list_empty(&pool->limiter.new_waiters)),
			"data_vio pool must not have threads waiting to read or write when being freed");
	ASSERT_LOG_ONLY((bio_list_empty(&pool->discard_limiter.waiters) &&
			 bio_list_empty(&pool->discard_limiter.new_waiters)),
			"data_vio pool must not have threads waiting to discard when being freed");
	spin_unlock(&pool->lock);

	while (!list_empty(&pool->available)) {
		struct data_vio *data_vio =
			list_first_entry(&pool->available, struct data_vio, pool_entry);

		list_del_init(pool->available.next);
		destroy_data_vio(data_vio);
	}

	free_funnel_queue(UDS_FORGET(pool->queue));
	UDS_FREE(pool);
}

static bool acquire_permit(struct limiter *limiter, struct bio *bio)
{
	if (limiter->busy >= limiter->limit) {
		DEFINE_WAIT(wait);

		bio_list_add(&limiter->new_waiters, bio);
		prepare_to_wait_exclusive(&limiter->blocked_threads, &wait, TASK_UNINTERRUPTIBLE);
		spin_unlock(&limiter->pool->lock);
		io_schedule();
		finish_wait(&limiter->blocked_threads, &wait);
		return false;
	}

	WRITE_ONCE(limiter->busy, limiter->busy + 1);
	if (limiter->max_busy < limiter->busy)
		WRITE_ONCE(limiter->max_busy, limiter->busy);

	return true;
}

/**
 * vdo_launch_bio() - Acquire a data_vio from the pool, assign the bio to it, and launch it.
 * @pool: The pool from which to acquire a data_vio.
 * @bio: The bio to launch.
 *
 * This will block if data_vios or discard permits are not available.
 */
void vdo_launch_bio(struct data_vio_pool *pool, struct bio *bio)
{
	struct data_vio *data_vio;

	ASSERT_LOG_ONLY(!vdo_is_state_quiescent(&pool->state),
			"data_vio_pool not quiescent on acquire");

	bio->bi_private = (void *) jiffies;
	spin_lock(&pool->lock);
	if ((bio_op(bio) == REQ_OP_DISCARD) && !acquire_permit(&pool->discard_limiter, bio))
		return;

	if (!acquire_permit(&pool->limiter, bio))
		return;

	data_vio = get_available_data_vio(pool);
	spin_unlock(&pool->lock);
	launch_bio(pool->completion.vdo, data_vio, bio);
}

/**
 * initiate_drain() - Initiate a drain.
 *
 * Implements vdo_admin_initiator.
 */
static void initiate_drain(struct admin_state *state)
{
	bool drained;
	struct data_vio_pool *pool = container_of(state, struct data_vio_pool, state);

	spin_lock(&pool->lock);
	drained = check_for_drain_complete_locked(pool);
	spin_unlock(&pool->lock);

	if (drained)
		vdo_finish_draining(state);
}

/**
 * drain_data_vio_pool() - Wait asynchronously for all data_vios to be returned to the pool.
 * @pool: The data_vio_pool to drain.
 * @completion: The completion to notify when the pool has drained.
 */
void drain_data_vio_pool(struct data_vio_pool *pool, struct vdo_completion *completion)
{
	assert_on_vdo_cpu_thread(completion->vdo, __func__);
	vdo_start_draining(&pool->state, VDO_ADMIN_STATE_SUSPENDING, completion, initiate_drain);
}

/**
 * resume_data_vio_pool() - Resume a data_vio pool.
 * @pool: The pool to resume.
 * @completion: The completion to notify when the pool has resumed.
 */
void resume_data_vio_pool(struct data_vio_pool *pool, struct vdo_completion *completion)
{
	assert_on_vdo_cpu_thread(completion->vdo, __func__);
	vdo_continue_completion(completion, vdo_resume_if_quiescent(&pool->state));
}

static void dump_limiter(const char *name, struct limiter *limiter)
{
	uds_log_info("%s: %u of %u busy (max %u), %s",
		     name,
		     limiter->busy,
		     limiter->limit,
		     limiter->max_busy,
		     ((bio_list_empty(&limiter->waiters) &&
		       bio_list_empty(&limiter->new_waiters)) ? "no waiters" : "has waiters"));
}

/**
 * dump_data_vio_pool() - Dump a data_vio pool to the log.
 * @pool: The pool to dump.
 * @dump_vios: Whether to dump the details of each busy data_vio as well.
 */
void dump_data_vio_pool(struct data_vio_pool *pool, bool dump_vios)
{
	/*
	 * In order that syslog can empty its buffer, sleep after 35 elements for 4ms (till the
	 * second clock tick).  These numbers were picked based on experiments with lab machines.
	 */
	enum { ELEMENTS_PER_BATCH = 35 };
	enum { SLEEP_FOR_SYSLOG = 4000 };

	if (pool == NULL)
		return;

	spin_lock(&pool->lock);
	dump_limiter("data_vios", &pool->limiter);
	dump_limiter("discard permits", &pool->discard_limiter);
	if (dump_vios) {
		int i;
		int dumped = 0;

		for (i = 0; i < pool->limiter.limit; i++) {
			struct data_vio *data_vio = &pool->data_vios[i];

			if (!list_empty(&data_vio->pool_entry))
				continue;

			dump_data_vio(data_vio);
			if (++dumped >= ELEMENTS_PER_BATCH) {
				spin_unlock(&pool->lock);
				dumped = 0;
				fsleep(SLEEP_FOR_SYSLOG);
				spin_lock(&pool->lock);
			}
		}
	}

	spin_unlock(&pool->lock);
}

data_vio_count_t get_data_vio_pool_active_discards(struct data_vio_pool *pool)
{
	return READ_ONCE(pool->discard_limiter.busy);
}

data_vio_count_t get_data_vio_pool_discard_limit(struct data_vio_pool *pool)
{
	return READ_ONCE(pool->discard_limiter.limit);
}

data_vio_count_t get_data_vio_pool_maximum_discards(struct data_vio_pool *pool)
{
	return READ_ONCE(pool->discard_limiter.max_busy);
}

int set_data_vio_pool_discard_limit(struct data_vio_pool *pool, data_vio_count_t limit)
{
	if (get_data_vio_pool_request_limit(pool) < limit)
		// The discard limit may not be higher than the data_vio limit.
		return -EINVAL;

	spin_lock(&pool->lock);
	pool->discard_limiter.limit = limit;
	spin_unlock(&pool->lock);

	return VDO_SUCCESS;
}

data_vio_count_t get_data_vio_pool_active_requests(struct data_vio_pool *pool)
{
	return READ_ONCE(pool->limiter.busy);
}

data_vio_count_t get_data_vio_pool_request_limit(struct data_vio_pool *pool)
{
	return READ_ONCE(pool->limiter.limit);
}

data_vio_count_t get_data_vio_pool_maximum_requests(struct data_vio_pool *pool)
{
	return READ_ONCE(pool->limiter.max_busy);
}

static void update_data_vio_error_stats(struct data_vio *data_vio)
{
	u8 index = 0;
	static const char * const operations[] = {
		[0] = "empty",
		[1] = "read",
		[2] = "write",
		[3] = "read-modify-write",
		[5] = "read+fua",
		[6] = "write+fua",
		[7] = "read-modify-write+fua",
	};

	if (data_vio->read)
		index = 1;

	if (data_vio->write)
		index += 2;

	if (data_vio->fua)
		index += 4;

	update_vio_error_stats(&data_vio->vio,
			       "Completing %s vio for LBN %llu with error after %s",
			       operations[index],
			       (unsigned long long) data_vio->logical.lbn,
			       get_data_vio_operation_name(data_vio));
}

static void perform_cleanup_stage(struct data_vio *data_vio, enum data_vio_cleanup_stage stage);

/**
 * release_allocated_lock() - Release the PBN lock and/or the reference on the allocated block at
 *			      the end of processing a data_vio.
 * @completion: The data_vio.
 */
static void release_allocated_lock(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_allocated_zone(data_vio);
	release_data_vio_allocation_lock(data_vio, false);
	perform_cleanup_stage(data_vio, VIO_RELEASE_RECOVERY_LOCKS);
}

/**
 * release_lock() - Release an uncontended LBN lock.
 * @data_vio: The data_vio holding the lock.
 * @lock: The lock to release
 */
static void release_lock(struct data_vio *data_vio, struct lbn_lock *lock)
{
	struct int_map *lock_map = lock->zone->lbn_operations;
	struct data_vio *lock_holder;

	if (!lock->locked) {
		/*  The lock is not locked, so it had better not be registered in the lock map. */
		struct data_vio *lock_holder = int_map_get(lock_map, lock->lbn);

		ASSERT_LOG_ONLY((data_vio != lock_holder),
				"no logical block lock held for block %llu",
				(unsigned long long) lock->lbn);
		return;
	}

	/* Release the lock by removing the lock from the map. */
	lock_holder = int_map_remove(lock_map, lock->lbn);
	ASSERT_LOG_ONLY((data_vio == lock_holder),
			"logical block lock mismatch for block %llu",
			(unsigned long long) lock->lbn);
	lock->locked = false;
}

/**
 * transfer_lock() - Transfer a contended LBN lock to the eldest waiter.
 * @data_vio: The data_vio holding the lock
 * @lock: The lock to transfer
 */
static void transfer_lock(struct data_vio *data_vio, struct lbn_lock *lock)
{
	struct data_vio *lock_holder, *next_lock_holder;
	int result;

	ASSERT_LOG_ONLY(lock->locked, "lbn_lock with waiters is not locked");

	/* Another data_vio is waiting for the lock, transfer it in a single lock map operation. */
	next_lock_holder = waiter_as_data_vio(dequeue_next_waiter(&lock->waiters));

	/* Transfer the remaining lock waiters to the next lock holder. */
	transfer_all_waiters(&lock->waiters, &next_lock_holder->logical.waiters);

	result = int_map_put(lock->zone->lbn_operations,
			     lock->lbn,
			     next_lock_holder,
			     true,
			     (void **) &lock_holder);
	if (result != VDO_SUCCESS) {
		continue_data_vio_with_error(next_lock_holder, result);
		return;
	}

	ASSERT_LOG_ONLY((lock_holder == data_vio),
			"logical block lock mismatch for block %llu",
			(unsigned long long) lock->lbn);
	lock->locked = false;

	/*
	 * If there are still waiters, other data_vios must be trying to get the lock we just
	 * transferred. We must ensure that the new lock holder doesn't block in the packer.
	 */
	if (has_waiters(&next_lock_holder->logical.waiters))
		cancel_data_vio_compression(next_lock_holder);

	/*
	 * Avoid stack overflow on lock transfer.
	 * FIXME: this is only an issue in the 1 thread config.
	 */
	next_lock_holder->vio.completion.requeue = true;
	launch_locked_request(next_lock_holder);
}

/**
 * release_logical_lock() - Release the logical block lock and flush generation lock at the end of
 *			    processing a data_vio.
 * @completion: The data_vio.
 */
static void release_logical_lock(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	struct lbn_lock *lock = &data_vio->logical;

	assert_data_vio_in_logical_zone(data_vio);

	if (has_waiters(&lock->waiters))
		transfer_lock(data_vio, lock);
	else
		release_lock(data_vio, lock);

	vdo_release_flush_generation_lock(data_vio);
	perform_cleanup_stage(data_vio, VIO_CLEANUP_DONE);
}

/**
 * clean_hash_lock() - Release the hash lock at the end of processing a data_vio.
 * @completion: The data_vio.
 */
static void clean_hash_lock(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_hash_zone(data_vio);
	if (completion->result != VDO_SUCCESS) {
		vdo_clean_failed_hash_lock(data_vio);
		return;
	}

	vdo_release_hash_lock(data_vio);
	perform_cleanup_stage(data_vio, VIO_RELEASE_LOGICAL);
}

/**
 * finish_cleanup() - Make some assertions about a data_vio which has finished cleaning up.
 * @data_vio: The data_vio which has finished cleaning up.
 *
 * If it is part of a multi-block discard, starts on the next block, otherwise, returns it to the
 * pool.
 */
static void finish_cleanup(struct data_vio *data_vio)
{
	struct vdo_completion *completion = &data_vio->vio.completion;

	ASSERT_LOG_ONLY(data_vio->allocation.lock == NULL,
			"complete data_vio has no allocation lock");
	ASSERT_LOG_ONLY(data_vio->hash_lock == NULL, "complete data_vio has no hash lock");
	if ((data_vio->remaining_discard <= VDO_BLOCK_SIZE) ||
	    (completion->result != VDO_SUCCESS)) {
		struct data_vio_pool *pool = completion->vdo->data_vio_pool;

#ifdef INTERNAL
		release_data_vio_hook(data_vio);
#endif /* INTERNAL */
		funnel_queue_put(pool->queue, &completion->work_queue_entry_link);
		schedule_releases(pool);
		return;
	}

	data_vio->remaining_discard -= min_t(u32,
					     data_vio->remaining_discard,
					     VDO_BLOCK_SIZE - data_vio->offset);
	data_vio->is_partial = (data_vio->remaining_discard < VDO_BLOCK_SIZE);
	data_vio->read = data_vio->is_partial;
	data_vio->offset = 0;
	completion->requeue = true;
	launch_data_vio(data_vio, data_vio->logical.lbn + 1);
}

/**
 * perform_cleanup_stage() - Perform the next step in the process of cleaning up a data_vio.
 * @data_vio: The data_vio to clean up.
 * @stage: The cleanup stage to perform.
 */
static void perform_cleanup_stage(struct data_vio *data_vio, enum data_vio_cleanup_stage stage)
{
	switch (stage) {
	case VIO_RELEASE_HASH_LOCK:
		if (data_vio->hash_lock != NULL) {
			launch_data_vio_hash_zone_callback(data_vio, clean_hash_lock);
			return;
		}
		fallthrough;

	case VIO_RELEASE_ALLOCATED:
		if (data_vio_has_allocation(data_vio)) {
			launch_data_vio_allocated_zone_callback(data_vio, release_allocated_lock);
			return;
		}
		fallthrough;

	case VIO_RELEASE_RECOVERY_LOCKS:
		if ((data_vio->recovery_sequence_number > 0) &&
		    !vdo_is_or_will_be_read_only(vdo_from_data_vio(data_vio)->read_only_notifier) &&
		    (data_vio->vio.completion.result != VDO_READ_ONLY))
			uds_log_warning("VDO not read-only when cleaning data_vio with RJ lock");
		fallthrough;

	case VIO_RELEASE_LOGICAL:
		launch_data_vio_logical_callback(data_vio, release_logical_lock);
		return;

	default:
		finish_cleanup(data_vio);
	}
}

/**
 * complete_data_vio() - Complete the processing of a data_vio.
 * @completion: The completion of the vio to complete.
 */
void complete_data_vio(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	completion->error_handler = NULL;
	data_vio->last_async_operation = VIO_ASYNC_OP_CLEANUP;
	perform_cleanup_stage(data_vio,
			      (data_vio->write ? VIO_CLEANUP_START : VIO_RELEASE_LOGICAL));
}

static void enter_read_only_mode(struct vdo_completion *completion)
{
	struct read_only_notifier *notifier = completion->vdo->read_only_notifier;

	if (vdo_is_read_only(notifier))
		return;

	if (completion->result != VDO_READ_ONLY) {
		struct data_vio *data_vio = as_data_vio(completion);

		uds_log_error_strerror(completion->result,
				       "Preparing to enter read-only mode: data_vio for LBN %llu (becoming mapped to %llu, previously mapped to %llu, allocated %llu) is completing with a fatal error after operation %s",
				       (unsigned long long) data_vio->logical.lbn,
				       (unsigned long long) data_vio->new_mapped.pbn,
				       (unsigned long long) data_vio->mapped.pbn,
				       (unsigned long long) data_vio->allocation.pbn,
				       get_data_vio_operation_name(data_vio));
	}

	vdo_enter_read_only_mode(notifier, completion->result);
}

/**
 * handle_data_vio_error() - The error handler for data_vios.
 * @completion: The data_vio which has an error
 */
void handle_data_vio_error(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	if ((completion->result == VDO_READ_ONLY) || (data_vio->user_bio == NULL))
		enter_read_only_mode(completion);

	update_data_vio_error_stats(data_vio);
	complete_data_vio(completion);
}

/**
 * get_data_vio_operation_name() - Get the name of the last asynchronous operation performed on a
 *				   data_vio.
 * @data_vio: The data_vio in question.
 *
 * Return: The name of the last operation performed on the data_vio.
 */
const char *get_data_vio_operation_name(struct data_vio *data_vio)
{
	STATIC_ASSERT((MAX_VIO_ASYNC_OPERATION_NUMBER - MIN_VIO_ASYNC_OPERATION_NUMBER) ==
		      ARRAY_SIZE(ASYNC_OPERATION_NAMES));

	return ((data_vio->last_async_operation < MAX_VIO_ASYNC_OPERATION_NUMBER) ?
		ASYNC_OPERATION_NAMES[data_vio->last_async_operation] :
		"unknown async operation");
}

/**
 * data_vio_allocate_data_block() - Allocate a data block.
 *
 * @data_vio: The data_vio which needs an allocation.
 * @write_lock_type: The type of write lock to obtain on the block.
 * @callback: The callback which will attempt an allocation in the current zone and continue if it
 *	      succeeds.
 * @error_handler: The handler for errors while allocating.
 */
void data_vio_allocate_data_block(struct data_vio *data_vio,
				  enum pbn_lock_type write_lock_type,
				  vdo_action *callback,
				  vdo_action *error_handler)
{
	struct allocation *allocation = &data_vio->allocation;

	ASSERT_LOG_ONLY((allocation->pbn == VDO_ZERO_BLOCK),
			"data_vio does not have an allocation");
	allocation->write_lock_type = write_lock_type;
	allocation->zone = vdo_get_next_allocation_zone(data_vio->logical.zone);
	allocation->first_allocation_zone = allocation->zone->zone_number;

	set_data_vio_error_handler(data_vio, error_handler);
	launch_data_vio_allocated_zone_callback(data_vio, callback);
}

void release_data_vio_allocation_lock(struct data_vio *data_vio, bool reset)
{
	struct allocation *allocation = &data_vio->allocation;
	physical_block_number_t locked_pbn = allocation->pbn;

	assert_data_vio_in_allocated_zone(data_vio);

	if (reset || vdo_pbn_lock_has_provisional_reference(allocation->lock))
		allocation->pbn = VDO_ZERO_BLOCK;

	vdo_release_physical_zone_pbn_lock(allocation->zone,
					   locked_pbn,
					   UDS_FORGET(allocation->lock));
}

/**
 * uncompress_data_vio() - A function to uncompress the data a data_vio has just read.
 * @data_vio: The data_vio to uncompress.
 * @mapping_state: The mapping state indicating which fragment to decompress.
 * @buffer: The buffer to receive the uncompressed data.
 */
int uncompress_data_vio(struct data_vio *data_vio,
			enum block_mapping_state mapping_state,
			char *buffer)
{
	int size;
	u16 fragment_offset, fragment_size;
	struct compressed_block *block = data_vio->compression.block;
	int result = vdo_get_compressed_block_fragment(mapping_state,
						       block,
						       &fragment_offset,
						       &fragment_size);

	if (result != VDO_SUCCESS) {
		uds_log_debug("%s: compressed fragment error %d", __func__, result);
		return result;
	}

	size = LZ4_decompress_safe((block->data + fragment_offset),
				   buffer,
				   fragment_size,
				   VDO_BLOCK_SIZE);
	if (size != VDO_BLOCK_SIZE) {
		uds_log_debug("%s: lz4 error", __func__);
		return VDO_INVALID_FRAGMENT;
	}

	return VDO_SUCCESS;
}

/**
 * modify_for_partial_write() - Do the modify-write part of a read-modify-write cycle.
 * @completion: The data_vio which has just finished its read.
 *
 * This callback is registered in read_block().
 */
static void modify_for_partial_write(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	char *data = data_vio->vio.data;
	struct bio *bio = data_vio->user_bio;

	assert_data_vio_on_cpu_thread(data_vio);

	if (bio_op(bio) == REQ_OP_DISCARD) {
		memset(data + data_vio->offset, '\0', min_t(u32,
							    data_vio->remaining_discard,
							    VDO_BLOCK_SIZE - data_vio->offset));
	} else {
		copy_from_bio(bio, data + data_vio->offset);
	}

	data_vio->is_zero = is_zero_block(data);
	data_vio->read = false;
	launch_data_vio_logical_callback(data_vio, continue_data_vio_with_block_map_slot);
}

static void complete_read(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	char *data = data_vio->vio.data;
	bool compressed = vdo_is_state_compressed(data_vio->mapped.state);

	assert_data_vio_on_cpu_thread(data_vio);

	if (compressed) {
		int result = uncompress_data_vio(data_vio, data_vio->mapped.state, data);

		if (result != VDO_SUCCESS) {
			continue_data_vio_with_error(data_vio, result);
			return;
		}
	}

	if (data_vio->write) {
		modify_for_partial_write(completion);
		return;
	}

	if (compressed || data_vio->is_partial)
		copy_to_bio(data_vio->user_bio, data + data_vio->offset);

	acknowledge_data_vio(data_vio);
	complete_data_vio(completion);
}

static void read_endio(struct bio *bio)
{
	struct data_vio *data_vio = vio_as_data_vio(bio->bi_private);
	int result = blk_status_to_errno(bio->bi_status);

	vdo_count_completed_bios(bio);
	if (result != VDO_SUCCESS) {
		continue_data_vio_with_error(data_vio, result);
		return;
	}

	launch_data_vio_cpu_callback(data_vio, complete_read, CPU_Q_COMPLETE_READ_PRIORITY);
}

static void complete_zero_read(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_on_cpu_thread(data_vio);

	if (data_vio->is_partial) {
		memset(data_vio->vio.data, 0, VDO_BLOCK_SIZE);
		if (data_vio->write) {
			modify_for_partial_write(completion);
			return;
		}
	} else {
		zero_fill_bio(data_vio->user_bio);
	}

	complete_read(completion);
}

/**
 * read_block() - Read a block asynchronously.
 * @completion: The data_vio to read.
 *
 * This is the callback registered in read_block_mapping().
 */
static void read_block(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	struct vio *vio = as_vio(completion);
	int result = VDO_SUCCESS;

	if (data_vio->mapped.pbn == VDO_ZERO_BLOCK) {
		launch_data_vio_cpu_callback(data_vio,
					     complete_zero_read,
					     CPU_Q_COMPLETE_VIO_PRIORITY);
		return;
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_READ_DATA_VIO;
	if (vdo_is_state_compressed(data_vio->mapped.state)) {
		result = prepare_data_vio_for_io(data_vio,
						 (char *) data_vio->compression.block,
						 read_endio,
						 REQ_OP_READ,
						 data_vio->mapped.pbn);
	} else {
		int opf = ((data_vio->user_bio->bi_opf & PASSTHROUGH_FLAGS) | REQ_OP_READ);

		if (data_vio->is_partial) {
			result = prepare_data_vio_for_io(data_vio,
							 vio->data,
							 read_endio,
							 opf,
							 data_vio->mapped.pbn);
		} else {
			/* A full 4k read. Use the incoming bio to avoid having to copy the data */
#ifndef VDO_UPSTREAM
#undef VDO_USE_ALTERNATE
#ifdef RHEL_RELEASE_CODE
#if (RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 1))
#define VDO_USE_ALTERNATE
#endif
#else /* !RHEL_RELEASE_CODE */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0))
#define VDO_USE_ALTERNATE
#endif
#endif /* !RHEL_RELEASE_CODE */
#endif /* !VDO_UPSTREAM */
#ifdef VDO_USE_ALTERNATE
			bio_reset(vio->bio);
			__bio_clone_fast(vio->bio, data_vio->user_bio);
#else
			bio_reset(vio->bio, vio->bio->bi_bdev, opf);
			bio_init_clone(data_vio->user_bio->bi_bdev,
				       vio->bio,
				       data_vio->user_bio,
				       GFP_KERNEL);
#endif

			/* Copy over the original bio iovec and opflags. */
			vdo_set_bio_properties(vio->bio,
					       vio,
					       read_endio,
					       opf,
					       data_vio->mapped.pbn);
		}
	}

	if (result != VDO_SUCCESS) {
		continue_data_vio_with_error(data_vio, result);
		return;
	}

	submit_data_vio_io(data_vio);
}

/**
 * update_block_map() - Update the block map now that we've added both entries to the recovery
 *                      journal.
 * @completion: The completion of the write in progress.
 */
static void update_block_map(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_logical_zone(data_vio);

	if (data_vio->hash_lock != NULL)
		set_data_vio_hash_zone_callback(data_vio, vdo_continue_hash_lock);
	else
		completion->callback = complete_data_vio;
	data_vio->last_async_operation = VIO_ASYNC_OP_PUT_MAPPED_BLOCK;
	vdo_put_mapped_block(data_vio);
}

/**
 * journal_increment() - Make a recovery journal increment.
 * @data_vio: The data_vio.
 * @lock: The pbn_lock on the block being incremented.
 */
static void journal_increment(struct data_vio *data_vio, struct pbn_lock *lock)
{
	vdo_set_up_reference_operation_with_lock(VDO_JOURNAL_DATA_INCREMENT,
						 data_vio->new_mapped.pbn,
						 data_vio->new_mapped.state,
						 lock,
						 &data_vio->operation);
	vdo_add_recovery_journal_entry(vdo_from_data_vio(data_vio)->recovery_journal, data_vio);
}

/**
 * journal_decrement() - Make a recovery journal decrement entry.
 * @data_vio: The data_vio.
 */
static void journal_decrement(struct data_vio *data_vio)
{
	vdo_set_up_reference_operation_with_zone(VDO_JOURNAL_DATA_DECREMENT,
						 data_vio->mapped.pbn,
						 data_vio->mapped.state,
						 data_vio->mapped.zone,
						 &data_vio->operation);
	vdo_add_recovery_journal_entry(vdo_from_data_vio(data_vio)->recovery_journal, data_vio);
}

/**
 * update_reference_count() - Make a reference count change.
 * @data_vio: The data_vio.
 */
static void update_reference_count(struct data_vio *data_vio)
{
	struct slab_depot *depot = vdo_from_data_vio(data_vio)->depot;
	physical_block_number_t pbn = data_vio->operation.pbn;
	int result = ASSERT(vdo_is_physical_data_block(depot, pbn),
			    "Adding slab journal entry for impossible PBN %llu for LBN %llu",
			    (unsigned long long) pbn,
			    (unsigned long long) data_vio->logical.lbn);

	if (result != VDO_SUCCESS) {
		continue_data_vio_with_error(data_vio, result);
		return;
	}

	vdo_add_slab_journal_entry(vdo_get_slab(depot, pbn)->journal, data_vio);
}

static void decrement_reference_count(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_mapped_zone(data_vio);

	set_data_vio_logical_callback(data_vio, update_block_map);
	data_vio->last_async_operation = VIO_ASYNC_OP_DECREMENT_REFERENCE_COUNT;
	update_reference_count(data_vio);
}

/**
 * journal_unmapping() - Write the appropriate journal entry for removing the mapping of logical to
 *                       mapped.
 * @completion: The completion of the write in progress.
 */
static void journal_unmapping(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_journal_zone(data_vio);

	if (data_vio->mapped.pbn == VDO_ZERO_BLOCK)
		set_data_vio_logical_callback(data_vio, update_block_map);
	else
		set_data_vio_mapped_zone_callback(data_vio, decrement_reference_count);
	data_vio->last_async_operation = VIO_ASYNC_OP_JOURNAL_UNMAPPING;
	journal_decrement(data_vio);
}

/**
 * read_old_block_mapping() - Get the prevoius PBN/LBN mapping.
 * @completion: The completion of the write in progress.
 *
 * Gets the previous PBN mapped to this LBN from the block map, so as to make an appropriate
 * journal entry referencing the removal of this LBN->PBN mapping.
 */
static void read_old_block_mapping(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_logical_zone(data_vio);

	data_vio->last_async_operation = VIO_ASYNC_OP_GET_MAPPED_BLOCK_FOR_WRITE;
	set_data_vio_journal_callback(data_vio, journal_unmapping);
	vdo_get_mapped_block(data_vio);
}

/**
 * increment_reference_count() - Increment the reference count now that the new mapping is
 *                               journaled.
 * @completion: The completion of the write in progress.
 */
static void increment_reference_count(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_new_mapped_zone(data_vio);

	set_data_vio_logical_callback(data_vio, read_old_block_mapping);
	data_vio->last_async_operation = VIO_ASYNC_OP_INCREMENT_REFERENCE_COUNT;
	update_reference_count(data_vio);
}

/**
 * journal_optimized_data_vio_mapping() - Add a recovery journal entry for the increment of a
 *					  compressed or deduplicated block.
 * @completion: The data_vio which has been compressed.
 */
void journal_optimized_data_vio_mapping(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_journal_zone(data_vio);

	set_data_vio_new_mapped_zone_callback(data_vio, increment_reference_count);
	data_vio->last_async_operation = VIO_ASYNC_OP_JOURNAL_MAPPING_FOR_OPTIMIZATION;
	journal_increment(data_vio, vdo_get_duplicate_lock(data_vio));
}

/**
 * pack_compressed_data() - Attempt to pack the compressed data_vio into a block.
 * @completion: The completion of a compressed data_vio.
 *
 * This is the callback registered in launch_compress_data_vio().
 */
static void pack_compressed_data(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_packer_zone(data_vio);

	if (!vdo_get_compressing(vdo_from_data_vio(data_vio)) ||
	    get_data_vio_compression_status(data_vio).may_not_compress) {
		write_data_vio(data_vio);
		return;
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_ATTEMPT_PACKING;
	vdo_attempt_packing(data_vio);
}

/**
 * compress_data_vio() - Do the actual work of compressing the data on a CPU queue.
 * @completion: The completion of the write in progress.
 *
 * This callback is registered in launch_compress_data_vio().
 */
static void compress_data_vio(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	int size;

	assert_data_vio_on_cpu_thread(data_vio);

	/*
	 * By putting the compressed data at the start of the compressed block data field, we won't
	 * need to copy it if this data_vio becomes a compressed write agent.
	 */
	size = LZ4_compress_default(data_vio->vio.data,
				    data_vio->compression.block->data,
				    VDO_BLOCK_SIZE,
				    VDO_MAX_COMPRESSED_FRAGMENT_SIZE,
				    (char *) get_work_queue_private_data());
	if ((size > 0) && (size < VDO_COMPRESSED_BLOCK_DATA_SIZE)) {
		data_vio->compression.size = size;
		launch_data_vio_packer_callback(data_vio, pack_compressed_data);
		return;
	}

	write_data_vio(data_vio);
}

/**
 * launch_compress_data_vio() - Continue a write by attempting to compress the data.
 * @data_vio: The data_vio to be compressed.
 *
 * This is a re-entry point to vio_write used by hash locks.
 */
void launch_compress_data_vio(struct data_vio *data_vio)
{
	ASSERT_LOG_ONLY(!data_vio->is_duplicate, "compressing a non-duplicate block");
	ASSERT_LOG_ONLY(data_vio->hash_lock != NULL, "data_vio to compress has a hash_lock");
	ASSERT_LOG_ONLY(data_vio_has_allocation(data_vio),
			"data_vio to compress has an allocation");

	/*
	 * There are 4 reasons why a data_vio which has reached this point will not be eligible for
	 * compression:
	 *
	 * 1) Since data_vios can block indefinitely in the packer, it would be bad to do so if the
	 * write request also requests FUA.
	 *
	 * 2) A data_vio should not be compressed when compression is disabled for the vdo.
	 *
	 * 3) A data_vio could be doing a partial write on behalf of a larger discard which has not
	 * yet been acknowledged and hence blocking in the packer would be bad.
	 *
	 * 4) Some other data_vio may be waiting on this data_vio in which case blocking in the
	 * packer would also be bad.
	 */
	if (data_vio->fua ||
	    !vdo_get_compressing(vdo_from_data_vio(data_vio)) ||
	    ((data_vio->user_bio != NULL) && (bio_op(data_vio->user_bio) == REQ_OP_DISCARD)) ||
	    (advance_data_vio_compression_stage(data_vio).stage != DATA_VIO_COMPRESSING)) {
		write_data_vio(data_vio);
		return;
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_COMPRESS_DATA_VIO;
	launch_data_vio_cpu_callback(data_vio, compress_data_vio, CPU_Q_COMPRESS_BLOCK_PRIORITY);
}

/**
 * hash_data_vio() - Hash the data in a data_vio and set the hash zone (which also flags the record
 *		     name as set).
 * @completion: The data_vio to hash.

 * This callback is registered in prepare_for_dedupe().
 */
static void hash_data_vio(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_on_cpu_thread(data_vio);
	ASSERT_LOG_ONLY(!data_vio->is_zero, "zero blocks should not be hashed");

	murmurhash3_128(data_vio->vio.data,
			VDO_BLOCK_SIZE,
			0x62ea60be,
			&data_vio->record_name);

	data_vio->hash_zone = vdo_select_hash_zone(vdo_from_data_vio(data_vio)->hash_zones,
						   &data_vio->record_name);
	data_vio->last_async_operation = VIO_ASYNC_OP_ACQUIRE_VDO_HASH_LOCK;
	launch_data_vio_hash_zone_callback(data_vio, vdo_acquire_hash_lock);
}

/**
 * prepare_for_dedupe() - Prepare for the dedupe path after attempting to get an allocation.
 * @data_vio: The data_vio to deduplicate.
 */
static void prepare_for_dedupe(struct data_vio *data_vio)
{
	/* We don't care what thread we are on. */
	ASSERT_LOG_ONLY(!data_vio->is_zero, "must not prepare to dedupe zero blocks");

	/*
	 * Before we can dedupe, we need to know the record name, so the first
	 * step is to hash the block data.
	 */
	data_vio->last_async_operation = VIO_ASYNC_OP_HASH_DATA_VIO;
	launch_data_vio_cpu_callback(data_vio, hash_data_vio, CPU_Q_HASH_BLOCK_PRIORITY);
}

/**
 * increment_for_write() - Do the incref after a successful block write.
 * @completion: The completion of the write in progress.
 *
 * This is the callback registered by finish_block_write().
 */
static void increment_for_write(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_allocated_zone(data_vio);

	/*
	 * Now that the data has been written, it's safe to deduplicate against the block.
	 * Downgrade the allocation lock to a read lock so it can be used later by the hash lock.
	 */
	vdo_downgrade_pbn_write_lock(data_vio->allocation.lock, false);
	increment_reference_count(completion);
}

/**
 * finish_block_write() - Add an entry in the recovery journal after a successful block write.
 * @completion: The completion of the write in progress.
 *
 * This is the callback registered by write_block(). It is also registered in
 * allocate_block_for_write().
 */
static void finish_block_write(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_journal_zone(data_vio);

	if (data_vio->new_mapped.pbn == VDO_ZERO_BLOCK)
		set_data_vio_logical_callback(data_vio, read_old_block_mapping);
	else
		set_data_vio_allocated_zone_callback(data_vio, increment_for_write);

	data_vio->last_async_operation = VIO_ASYNC_OP_JOURNAL_MAPPING_FOR_WRITE;
	journal_increment(data_vio, data_vio->allocation.lock);
}

/**
 * write_bio_finished() - This is the bio_end_io functon registered in write_block() to be called
 *			  when a data_vio's write to he underlying storage has completed.
 * @bio: The bio which has just completed.
 */
static void write_bio_finished(struct bio *bio)
{
	struct data_vio *data_vio = vio_as_data_vio((struct vio *) bio->bi_private);

	vdo_count_completed_bios(bio);
	vdo_set_completion_result(&data_vio->vio.completion, vdo_get_bio_result(bio));
	launch_data_vio_journal_callback(data_vio, finish_block_write);
}

/**
 * write_data_vio() - Write a data block to storage without compression.
 * @data_vio: The data_vio to write.
 */
void write_data_vio(struct data_vio *data_vio)
{
	struct data_vio_compression_status status, new_status;
	int result;

	if (!data_vio_has_allocation(data_vio)) {
		/*
		 * There was no space to write this block and we failed to deduplicate or compress
		 * it.
		 */
		continue_data_vio_with_error(data_vio, VDO_NO_SPACE);
		return;
	}

	new_status = (struct data_vio_compression_status) {
		.stage = DATA_VIO_POST_PACKER,
		.may_not_compress = true,
	};

	do {
		status = get_data_vio_compression_status(data_vio);
	} while ((status.stage != DATA_VIO_POST_PACKER) &&
		 !set_data_vio_compression_status(data_vio, status, new_status));

	/* Write the data from the data block buffer. */
	result = prepare_data_vio_for_io(data_vio,
					 data_vio->vio.data,
					 write_bio_finished,
					 REQ_OP_WRITE,
					 data_vio->allocation.pbn);
	if (result != VDO_SUCCESS) {
		continue_data_vio_with_error(data_vio, result);
		return;
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_WRITE_DATA_VIO;
	submit_data_vio_io(data_vio);
}

/**
 * acknowledge_write_callback() - Acknowledge a write to the requestor.
 * @completion: The data_vio being acknowledged.
 *
 * This callback is registered in allocate_block() and continue_write_with_block_map_slot().
 */
static void acknowledge_write_callback(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	struct vdo *vdo = completion->vdo;

	ASSERT_LOG_ONLY((!vdo_uses_bio_ack_queue(vdo) ||
			 (vdo_get_callback_thread_id() == vdo->thread_config->bio_ack_thread)),
			"%s() called on bio ack queue",
			__func__);
	ASSERT_LOG_ONLY(data_vio_has_flush_generation_lock(data_vio),
			"write VIO to be acknowledged has a flush generation lock");
	acknowledge_data_vio(data_vio);
	if (data_vio->new_mapped.pbn == VDO_ZERO_BLOCK) {
		/* This is a zero write or discard */
		launch_data_vio_journal_callback(data_vio, finish_block_write);
		return;
	}

	prepare_for_dedupe(data_vio);
}

/**
 * allocate_block() - Attempt to allocate a block in the current allocation zone.
 * @completion: The data_vio needing an allocation.
 *
 * This callback is registered in continue_write_with_block_map_slot().
 */
static void allocate_block(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_allocated_zone(data_vio);

	if (!vdo_allocate_block_in_zone(data_vio))
		return;

	completion->error_handler = handle_data_vio_error;
	WRITE_ONCE(data_vio->allocation_succeeded, true);
	data_vio->new_mapped = (struct zoned_pbn) {
		.zone = data_vio->allocation.zone,
		.pbn = data_vio->allocation.pbn,
		.state = VDO_MAPPING_STATE_UNCOMPRESSED,
	};

	if (data_vio->fua) {
		prepare_for_dedupe(data_vio);
		return;
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_ACKNOWLEDGE_WRITE;
	launch_data_vio_on_bio_ack_queue(data_vio, acknowledge_write_callback);
}

/**
 * handle_allocation_error() - Handle an error attempting to allocate a block.
 * @completion: The data_vio needing an allocation.
 *
 * This error handler is registered in continue_write_with_block_map_slot().
 */
static void handle_allocation_error(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	if (completion->result == VDO_NO_SPACE) {
		/* We failed to get an allocation, but we can try to dedupe. */
		vdo_reset_completion(completion);
		completion->error_handler = handle_data_vio_error;
		prepare_for_dedupe(data_vio);
		return;
	}

	/* We got a "real" error, not just a failure to allocate, so fail the request. */
	handle_data_vio_error(completion);
}

static int assert_is_trim(struct data_vio *data_vio)
{
	int result = ASSERT(data_vio->is_trim, "data_vio with no block map page is a trim");

	return ((result == VDO_SUCCESS) ? result : VDO_READ_ONLY);
}

/**
 * continue_read_with_block_map_slot() - Read the data_vio's mapping from the block map.
 * @completion: The data_vio to be read.
 *
 * This callback is registered in launch_read_data_vio().
 */
void continue_data_vio_with_block_map_slot(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_logical_zone(data_vio);
	if (data_vio->read) {
		set_data_vio_logical_callback(data_vio, read_block);
		data_vio->last_async_operation = VIO_ASYNC_OP_GET_MAPPED_BLOCK_FOR_READ;
		vdo_get_mapped_block(data_vio);
		return;
	}

	vdo_acquire_flush_generation_lock(data_vio);

	if (data_vio->tree_lock.tree_slots[0].block_map_slot.pbn == VDO_ZERO_BLOCK) {
		/*
		 * This is a trim for a block on a block map page which has not been allocated, so
		 * there's nothing more we need to do.
		 */
		completion->callback = complete_data_vio;
		continue_data_vio_with_error(data_vio, assert_is_trim(data_vio));
		return;
	}

	/*
	 * We need an allocation if this is neither a full-block trim nor a
	 * full-block zero write.
	 */
	if (!data_vio->is_zero && (!data_vio->is_trim || data_vio->is_partial)) {
		data_vio_allocate_data_block(data_vio,
					     VIO_WRITE_LOCK,
					     allocate_block,
					     handle_allocation_error);
		return;
	}


	/*
	 * We don't need to writqe any data, so skip allocation and just update the block map and
	 * reference counts (via the journal).
	 */
	data_vio->new_mapped.pbn = VDO_ZERO_BLOCK;
	if (data_vio->is_zero)
		data_vio->new_mapped.state = VDO_MAPPING_STATE_UNCOMPRESSED;

	if (data_vio->remaining_discard > VDO_BLOCK_SIZE) {
		/* This is not the final block of a discard so we can't acknowledge it yet. */
		launch_data_vio_journal_callback(data_vio, finish_block_write);
		return;
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_ACKNOWLEDGE_WRITE;
	launch_data_vio_on_bio_ack_queue(data_vio, acknowledge_write_callback);
}
