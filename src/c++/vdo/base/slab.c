// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "slab.h"

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "admin-state.h"
#include "completion.h"
#include "constants.h"
#include "data-vio.h"
#include "physical-zone.h"
#include "recovery-journal.h"
#include "ref-counts.h"
#include "slab-depot.h"
#include "slab-journal.h"
#include "slab-summary.h"
#include "vdo-component-states.h"

/**
 * vdo_make_slab() - Construct a new, empty slab.
 * @slab_origin: The physical block number within the block allocator partition of the first block
 *               in the slab.
 * @allocator: The block allocator to which the slab belongs.
 * @translation: The translation from the depot's partition to the physical storage.
 * @recovery_journal: The recovery journal of the VDO.
 * @slab_number: The slab number of the slab.
 * @is_new: true if this slab is being allocated as part of a resize.
 * @slab_ptr: A pointer to receive the new slab.
 *
 * Return: VDO_SUCCESS or an error code.
 */
int vdo_make_slab(physical_block_number_t slab_origin,
		  struct block_allocator *allocator,
		  physical_block_number_t translation,
		  struct recovery_journal *recovery_journal,
		  slab_count_t slab_number,
		  bool is_new,
		  struct vdo_slab **slab_ptr)
{
	const struct slab_config *slab_config = &allocator->depot->slab_config;
	struct vdo_slab *slab;
	int result;

	result = UDS_ALLOCATE(1, struct vdo_slab, __func__, &slab);
	if (result != VDO_SUCCESS)
		return result;

	slab->allocator = allocator;
	slab->start = slab_origin;
	slab->end = slab->start + slab_config->slab_blocks;
	slab->slab_number = slab_number;
	INIT_LIST_HEAD(&slab->allocq_entry);

	slab->ref_counts_origin = slab_origin + slab_config->data_blocks + translation;
	slab->journal_origin =
		(vdo_get_slab_journal_start_block(slab_config, slab_origin) + translation);

	result = vdo_make_slab_journal(allocator, slab, recovery_journal, &slab->journal);
	if (result != VDO_SUCCESS) {
		vdo_free_slab(slab);
		return result;
	}

	if (is_new) {
		vdo_set_admin_state_code(&slab->state, VDO_ADMIN_STATE_NEW);
		result = vdo_allocate_ref_counts_for_slab(slab);
		if (result != VDO_SUCCESS) {
			vdo_free_slab(slab);
			return result;
		}
	} else {
		vdo_set_admin_state_code(&slab->state, VDO_ADMIN_STATE_NORMAL_OPERATION);
	}

	*slab_ptr = slab;
	return VDO_SUCCESS;
}

/**
 * vdo_allocate_ref_counts_for_slab() - Allocate the reference counts for a slab.
 * @slab: The slab whose reference counts need allocation.
 *
 * Return: VDO_SUCCESS or an error code.
 */
int vdo_allocate_ref_counts_for_slab(struct vdo_slab *slab)
{
	const struct slab_config *slab_config = &slab->allocator->depot->slab_config;
	int result;

	result = ASSERT(slab->reference_counts == NULL,
			"vdo_slab %u doesn't allocate refcounts twice",
			slab->slab_number);
	if (result != VDO_SUCCESS)
		return result;

	return vdo_make_ref_counts(slab_config->data_blocks,
				   slab,
				   slab->ref_counts_origin,
				   &slab->reference_counts);
}

/**
 * vdo_free_slab() - Destroy a slab.
 * @slab: The slab to destroy.
 */
void vdo_free_slab(struct vdo_slab *slab)
{
	if (slab == NULL)
		return;

	list_del(&slab->allocq_entry);
	vdo_free_slab_journal(UDS_FORGET(slab->journal));
	vdo_free_ref_counts(UDS_FORGET(slab->reference_counts));
	UDS_FREE(slab);
}

/**
 * get_slab_free_block_count() - Get the current number of free blocks in a slab.
 * @slab: The slab to query.
 *
 * Return: The number of free blocks in the slab.
 */
block_count_t get_slab_free_block_count(const struct vdo_slab *slab)
{
	return vdo_get_unreferenced_block_count(slab->reference_counts);
}

/**
 * vdo_slab_block_number_from_pbn() - Determine the index within the slab of a particular physical
 *                                    block number.
 * @slab: The slab.
 * @physical_block_number: The physical block number.
 * @slab_block_number_ptr: A pointer to the slab block number.
 *
 * Return: VDO_SUCCESS or an error code.
 */
int vdo_slab_block_number_from_pbn(struct vdo_slab *slab,
				   physical_block_number_t physical_block_number,
				   slab_block_number *slab_block_number_ptr)
{
	u64 slab_block_number;

	if (physical_block_number < slab->start)
		return VDO_OUT_OF_RANGE;

	slab_block_number = physical_block_number - slab->start;
	if (slab_block_number >= slab->allocator->depot->slab_config.data_blocks)
		return VDO_OUT_OF_RANGE;

	*slab_block_number_ptr = slab_block_number;
	return VDO_SUCCESS;
}

/**
 * initiate_slab_action() - Initiate a slab action.
 *
 * Implements vdo_admin_initiator.
 */
static void initiate_slab_action(struct admin_state *state)
{
	struct vdo_slab *slab = container_of(state, struct vdo_slab, state);

	if (vdo_is_state_draining(state)) {
		const struct admin_state_code *operation = vdo_get_admin_state_code(state);

		if (operation == VDO_ADMIN_STATE_SCRUBBING)
			slab->status = VDO_SLAB_REBUILDING;

		vdo_drain_slab_journal(slab->journal);

		if (slab->reference_counts != NULL)
			vdo_drain_ref_counts(slab->reference_counts);

		vdo_check_if_slab_drained(slab);
		return;
	}

	if (vdo_is_state_loading(state)) {
		vdo_decode_slab_journal(slab->journal);
		return;
	}

	if (vdo_is_state_resuming(state)) {
		vdo_queue_slab(slab);
		vdo_finish_resuming(state);
		return;
	}

	vdo_finish_operation(state, VDO_INVALID_ADMIN_STATE);
}

/**
 * vdo_start_slab_action() - Start an administrative operation on a slab.
 * @slab: The slab to load.
 * @operation: The type of load to perform.
 * @parent: The object to notify when the operation is complete.
 */
void vdo_start_slab_action(struct vdo_slab *slab,
			   const struct admin_state_code *operation,
			   struct vdo_completion *parent)
{
	vdo_start_operation_with_waiter(&slab->state, operation, parent, initiate_slab_action);
}

/**
 * vdo_notify_slab_journal_is_loaded() - Inform a slab that its journal has been loaded.
 * @slab: The slab whose journal has been loaded.
 * @result: The result of the load operation.
 */
void vdo_notify_slab_journal_is_loaded(struct vdo_slab *slab, int result)
{
	if ((result == VDO_SUCCESS) && vdo_is_state_clean_load(&slab->state))
		/*
		 * Since this is a normal or new load, we don't need the memory to read and process
		 * the recovery journal, so we can allocate reference counts now.
		 */
		result = vdo_allocate_ref_counts_for_slab(slab);

	vdo_finish_loading_with_result(&slab->state, result);
}

/**
 * vdo_is_slab_open() - Check whether a slab is open, i.e. is neither quiescent nor quiescing.
 * @slab: The slab to check.
 *
 * Return: true if the slab is open.
 */
bool vdo_is_slab_open(struct vdo_slab *slab)
{
	return (!vdo_is_state_quiescing(&slab->state) && !vdo_is_state_quiescent(&slab->state));
}

/**
 * vdo_check_if_slab_drained() - Check whether a slab has drained, and if so, send a notification
 *                               thereof.
 * @slab: The slab to check.
 */
void vdo_check_if_slab_drained(struct vdo_slab *slab)
{
	bool read_only;

	if (!vdo_is_state_draining(&slab->state) || vdo_is_slab_journal_active(slab->journal))
		return;

	if ((slab->reference_counts != NULL) && vdo_are_ref_counts_active(slab->reference_counts))
		return;

	read_only = vdo_is_read_only(slab->allocator->depot->vdo);
	vdo_finish_draining_with_result(&slab->state, (read_only ? VDO_READ_ONLY : VDO_SUCCESS));
}
