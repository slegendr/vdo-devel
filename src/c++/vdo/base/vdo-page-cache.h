/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VDO_PAGE_CACHE_H
#define VDO_PAGE_CACHE_H

#include <linux/list.h>

#ifndef __KERNEL__
#include <stdint.h>
#endif

#include "admin-state.h"
#include "block-map-tree.h"
#include "completion.h"
#include "dirty-lists.h"
#include "int-map.h"
#include "statistics.h"
#include "types.h"
#include "wait-queue.h"

/*
 * Generation counter for page references.
 */
typedef u32 vdo_page_generation;

static const physical_block_number_t NO_PAGE = 0xFFFFFFFFFFFFFFFF;

/* The VDO Page Cache abstraction. */
struct vdo_page_cache {
	/* the VDO which owns this cache */
	struct vdo *vdo;
	/* number of pages in cache */
	page_count_t page_count;
	/* number of pages to write in the current batch */
	page_count_t pages_in_batch;
	/* Whether the VDO is doing a read-only rebuild */
	bool rebuilding;

	/* array of page information entries */
	struct page_info *infos;
	/* raw memory for pages */
	char *pages;
	/* cache last found page info */
	struct page_info *last_found;
	/* map of page number to info */
	struct int_map *page_map;
	/* main LRU list (all infos) */
	struct list_head lru_list;
	/* dirty pages by period */
	struct dirty_lists *dirty_lists;
	/* free page list (oldest first) */
	struct list_head free_list;
	/* outgoing page list */
	struct list_head outgoing_list;
	/* number of read I/O operations pending */
	page_count_t outstanding_reads;
	/* number of write I/O operations pending */
	page_count_t outstanding_writes;
	/* number of pages covered by the current flush */
	page_count_t pages_in_flush;
	/* number of pages waiting to be included in the next flush */
	page_count_t pages_to_flush;
	/* number of discards in progress */
	unsigned int discard_count;
	/* how many VPCs waiting for free page */
	unsigned int waiter_count;
	/* queue of waiters who want a free page */
	struct wait_queue free_waiters;
	/*
	 * Statistics are only updated on the logical zone thread, but are accessed from other
	 * threads.
	 */
	struct block_map_statistics stats;
	/* counter for pressure reports */
	u32 pressure_report;
	/* the block map zone to which this cache belongs */
	struct block_map_zone *zone;
};

/*
 * The state of a page buffer. If the page buffer is free no particular page is bound to it,
 * otherwise the page buffer is bound to particular page whose absolute pbn is in the pbn field. If
 * the page is resident or dirty the page data is stable and may be accessed. Otherwise the page is
 * in flight (incoming or outgoing) and its data should not be accessed.
 *
 * @note Update the static data in get_page_state_name() if you change this enumeration.
 */
enum vdo_page_buffer_state {
	/* this page buffer is not being used */
	PS_FREE,
	/* this page is being read from store */
	PS_INCOMING,
	/* attempt to load this page failed */
	PS_FAILED,
	/* this page is valid and un-modified */
	PS_RESIDENT,
	/* this page is valid and modified */
	PS_DIRTY,
	/* this page is being written and should not be used */
	PS_OUTGOING,
	/* not a state */
	PAGE_STATE_COUNT,
} __packed;

/*
 * The write status of page
 */
enum vdo_page_write_status {
	WRITE_STATUS_NORMAL,
	WRITE_STATUS_DISCARD,
	WRITE_STATUS_DEFERRED,
} __packed;

/* Per-page-slot information. */
struct page_info {
	/* Preallocated page struct vio */
	struct vio *vio;
	/* back-link for references */
	struct vdo_page_cache *cache;
	/* the pbn of the page */
	physical_block_number_t pbn;
	/* page is busy (temporarily locked) */
	u16 busy;
	/* the write status the page */
	enum vdo_page_write_status write_status;
	/* page state */
	enum vdo_page_buffer_state state;
	/* queue of completions awaiting this item */
	struct wait_queue waiting;
	/* state linked list entry */
	struct list_head state_entry;
	/* LRU entry */
	struct list_head lru_entry;
	/*
	 * The earliest recovery journal block containing uncommitted updates to the block map page
	 * associated with this page_info. A reference (lock) is held on that block to prevent it
	 * from being reaped. When this value changes, the reference on the old value must be
	 * released and a reference on the new value must be acquired.
	 */
	sequence_number_t recovery_lock;
};

int __must_check vdo_make_page_cache(struct vdo *vdo,
				     page_count_t page_count,
				     block_count_t maximum_age,
				     struct block_map_zone *zone,
				     struct vdo_page_cache **cache_ptr);

void vdo_free_page_cache(struct vdo_page_cache *cache);

void vdo_set_page_cache_initial_period(struct vdo_page_cache *cache, sequence_number_t period);

void vdo_set_page_cache_rebuild_mode(struct vdo_page_cache *cache, bool rebuilding);

bool __must_check vdo_is_page_cache_active(struct vdo_page_cache *cache);

void vdo_advance_page_cache_period(struct vdo_page_cache *cache, sequence_number_t period);

/* ASYNC */

/*
 * A completion awaiting a specific page. Also a live reference into the page once completed, until
 * freed.
 */
struct vdo_page_completion {
	/* The generic completion */
	struct vdo_completion completion;
	/* The cache involved */
	struct vdo_page_cache *cache;
	/* The waiter for the pending list */
	struct waiter waiter;
	/* The absolute physical block number of the page on disk */
	physical_block_number_t pbn;
	/* Whether the page may be modified */
	bool writable;
	/* Whether the page is available */
	bool ready;
	/* The info structure for the page, only valid when ready */
	struct page_info *info;
};

void vdo_init_page_completion(struct vdo_page_completion *page_completion,
			      struct vdo_page_cache *cache,
			      physical_block_number_t pbn,
			      bool writable,
			      void *parent,
			      vdo_action *callback,
			      vdo_action *error_handler);

static inline struct vdo_page_completion *as_vdo_page_completion(struct vdo_completion *completion)
{
	vdo_assert_completion_type(completion->type, VDO_PAGE_COMPLETION);
	return container_of(completion, struct vdo_page_completion, completion);
}

void vdo_release_page_completion(struct vdo_completion *completion);

void vdo_get_page(struct vdo_completion *completion);

void vdo_mark_completed_page_dirty(struct vdo_completion *completion,
				   sequence_number_t old_dirty_period,
				   sequence_number_t new_dirty_period);

void vdo_request_page_write(struct vdo_completion *completion);

const void *vdo_dereference_readable_page(struct vdo_completion *completion);

void *vdo_dereference_writable_page(struct vdo_completion *completion);

void vdo_drain_page_cache(struct vdo_page_cache *cache);

int __must_check vdo_invalidate_page_cache(struct vdo_page_cache *cache);

/* STATISTICS & TESTING */

struct block_map_statistics __must_check
vdo_get_page_cache_statistics(const struct vdo_page_cache *cache);

#endif /* VDO_PAGE_CACHE_H */
