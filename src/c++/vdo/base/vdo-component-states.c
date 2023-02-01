// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "vdo-component-states.h"

#include "buffer.h"
#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "constants.h"
#include "header.h"
#include "slab-depot-format.h"
#include "status-codes.h"
#include "types.h"
#include "vdo-component.h"
#include "vdo-layout.h"

enum {
	PAGE_HEADER_4_1_SIZE = 8 + 8 + 8 + 1 + 1 + 1 + 1,
};

static const struct version_number BLOCK_MAP_4_1 = {
	.major_version = 4,
	.minor_version = 1,
};

const struct header VDO_BLOCK_MAP_HEADER_2_0 = {
	.id = VDO_BLOCK_MAP,
	.version = {
		.major_version = 2,
		.minor_version = 0,
	},
	.size = sizeof(struct block_map_state_2_0),
};

const struct header VDO_RECOVERY_JOURNAL_HEADER_7_0 = {
	.id = VDO_RECOVERY_JOURNAL,
	.version = {
			.major_version = 7,
			.minor_version = 0,
		},
	.size = sizeof(struct recovery_journal_state_7_0),
};

const struct version_number VDO_VOLUME_VERSION_67_0 = {
	.major_version = 67,
	.minor_version = 0,
};

struct block_map_page *vdo_format_block_map_page(void *buffer,
						 nonce_t nonce,
						 physical_block_number_t pbn,
						 bool initialized)
{
	struct block_map_page *page = (struct block_map_page *) buffer;

	memset(buffer, 0, VDO_BLOCK_SIZE);
	page->version = vdo_pack_version_number(BLOCK_MAP_4_1);
	page->header.nonce = __cpu_to_le64(nonce);
	page->header.pbn = __cpu_to_le64(pbn);
	page->header.initialized = initialized;
	return page;
}

enum block_map_page_validity
vdo_validate_block_map_page(struct block_map_page *page,
			    nonce_t nonce,
			    physical_block_number_t pbn)
{
	STATIC_ASSERT_SIZEOF(struct block_map_page_header, PAGE_HEADER_4_1_SIZE);

	if (!vdo_are_same_version(BLOCK_MAP_4_1, vdo_unpack_version_number(page->version)) ||
	    !page->header.initialized ||
	    (nonce != __le64_to_cpu(page->header.nonce)))
		return VDO_BLOCK_MAP_PAGE_INVALID;

	if (pbn != vdo_get_block_map_page_pbn(page))
		return VDO_BLOCK_MAP_PAGE_BAD;

	return VDO_BLOCK_MAP_PAGE_VALID;
}

EXTERNAL_STATIC int
decode_block_map_state_2_0(struct buffer *buffer, struct block_map_state_2_0 *state)
{
	size_t initial_length, decoded_size;
	block_count_t flat_page_count, root_count;
	physical_block_number_t flat_page_origin, root_origin;
	struct header header;
	int result;

	result = vdo_decode_header(buffer, &header);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_validate_header(&VDO_BLOCK_MAP_HEADER_2_0, &header, true, __func__);
	if (result != VDO_SUCCESS)
		return result;

	initial_length = content_length(buffer);

	result = get_u64_le_from_buffer(buffer, &flat_page_origin);
	if (result != UDS_SUCCESS)
		return result;

	result = ASSERT(flat_page_origin == VDO_BLOCK_MAP_FLAT_PAGE_ORIGIN,
			"Flat page origin must be %u (recorded as %llu)",
			VDO_BLOCK_MAP_FLAT_PAGE_ORIGIN,
			(unsigned long long) state->flat_page_origin);
	if (result != UDS_SUCCESS)
		return result;

	result = get_u64_le_from_buffer(buffer, &flat_page_count);
	if (result != UDS_SUCCESS)
		return result;

	result = ASSERT(flat_page_count == 0,
			"Flat page count must be 0 (recorded as %llu)",
			(unsigned long long) state->flat_page_count);
	if (result != UDS_SUCCESS)
		return result;

	result = get_u64_le_from_buffer(buffer, &root_origin);
	if (result != UDS_SUCCESS)
		return result;

	result = get_u64_le_from_buffer(buffer, &root_count);
	if (result != UDS_SUCCESS)
		return result;

	decoded_size = initial_length - content_length(buffer);
	result = ASSERT(VDO_BLOCK_MAP_HEADER_2_0.size == decoded_size,
			"decoded block map component size must match header size");
	if (result != VDO_SUCCESS)
		return result;

	*state = (struct block_map_state_2_0) {
		.flat_page_origin = flat_page_origin,
		.flat_page_count = flat_page_count,
		.root_origin = root_origin,
		.root_count = root_count,
	};

	return VDO_SUCCESS;
}

EXTERNAL_STATIC int
encode_block_map_state_2_0(struct block_map_state_2_0 state, struct buffer *buffer)
{
	size_t initial_length, encoded_size;
	int result;

	result = vdo_encode_header(&VDO_BLOCK_MAP_HEADER_2_0, buffer);
	if (result != UDS_SUCCESS)
		return result;

	initial_length = content_length(buffer);

	result = put_u64_le_into_buffer(buffer, state.flat_page_origin);
	if (result != UDS_SUCCESS)
		return result;

	result = put_u64_le_into_buffer(buffer, state.flat_page_count);
	if (result != UDS_SUCCESS)
		return result;

	result = put_u64_le_into_buffer(buffer, state.root_origin);
	if (result != UDS_SUCCESS)
		return result;

	result = put_u64_le_into_buffer(buffer, state.root_count);
	if (result != UDS_SUCCESS)
		return result;

	encoded_size = content_length(buffer) - initial_length;
	return ASSERT(VDO_BLOCK_MAP_HEADER_2_0.size == encoded_size,
		      "encoded block map component size must match header size");
}

/**
 * vdo_compute_new_forest_pages() - Compute the number of pages which must be allocated at each
 *                                  level in order to grow the forest to a new number of entries.
 * @entries: The new number of entries the block map must address.
 *
 * Return: The total number of non-leaf pages required.
 */
block_count_t vdo_compute_new_forest_pages(root_count_t root_count,
					   struct boundary *old_sizes,
					   block_count_t entries,
					   struct boundary *new_sizes)
{
	page_count_t leaf_pages = max(vdo_compute_block_map_page_count(entries), 1U);
	page_count_t level_size = DIV_ROUND_UP(leaf_pages, root_count);
	block_count_t total_pages = 0;
	height_t height;

	for (height = 0; height < VDO_BLOCK_MAP_TREE_HEIGHT; height++) {
		block_count_t new_pages;

		level_size = DIV_ROUND_UP(level_size, VDO_BLOCK_MAP_ENTRIES_PER_PAGE);
		new_sizes->levels[height] = level_size;
		new_pages = level_size;
		if (old_sizes != NULL)
			new_pages -= old_sizes->levels[height];
		total_pages += (new_pages * root_count);
	}

	return total_pages;
}

/**
 * encode_recovery_journal_state_7_0() - Encode the state of a recovery journal.
 * @state: The recovery journal state.
 * @buffer: The buffer to encode into.
 *
 * Return: VDO_SUCCESS or an error code.
 */
EXTERNAL_STATIC int __must_check
encode_recovery_journal_state_7_0(struct recovery_journal_state_7_0 state, struct buffer *buffer)
{
	size_t initial_length, encoded_size;
	int result;

	result = vdo_encode_header(&VDO_RECOVERY_JOURNAL_HEADER_7_0, buffer);
	if (result != UDS_SUCCESS)
		return result;

	initial_length = content_length(buffer);

	result = put_u64_le_into_buffer(buffer, state.journal_start);
	if (result != UDS_SUCCESS)
		return result;

	result = put_u64_le_into_buffer(buffer, state.logical_blocks_used);
	if (result != UDS_SUCCESS)
		return result;

	result = put_u64_le_into_buffer(buffer, state.block_map_data_blocks);
	if (result != UDS_SUCCESS)
		return result;

	encoded_size = content_length(buffer) - initial_length;
	return ASSERT(VDO_RECOVERY_JOURNAL_HEADER_7_0.size == encoded_size,
		      "encoded recovery journal component size must match header size");
}

/**
 * decode_recovery_journal_state_7_0() - Decode the state of a recovery journal saved in a buffer.
 * @buffer: The buffer containing the saved state.
 * @state: A pointer to a recovery journal state to hold the result of a successful decode.
 *
 * Return: VDO_SUCCESS or an error code.
 */
EXTERNAL_STATIC int __must_check
decode_recovery_journal_state_7_0(struct buffer *buffer, struct recovery_journal_state_7_0 *state)
{
	struct header header;
	int result;
	size_t initial_length, decoded_size;
	sequence_number_t journal_start;
	block_count_t logical_blocks_used, block_map_data_blocks;

	result = vdo_decode_header(buffer, &header);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_validate_header(&VDO_RECOVERY_JOURNAL_HEADER_7_0, &header, true, __func__);
	if (result != VDO_SUCCESS)
		return result;

	initial_length = content_length(buffer);

	result = get_u64_le_from_buffer(buffer, &journal_start);
	if (result != UDS_SUCCESS)
		return result;

	result = get_u64_le_from_buffer(buffer, &logical_blocks_used);
	if (result != UDS_SUCCESS)
		return result;

	result = get_u64_le_from_buffer(buffer, &block_map_data_blocks);
	if (result != UDS_SUCCESS)
		return result;

	decoded_size = initial_length - content_length(buffer);
	result = ASSERT(VDO_RECOVERY_JOURNAL_HEADER_7_0.size == decoded_size,
			"decoded recovery journal component size must match header size");
	if (result != UDS_SUCCESS)
		return result;

	*state = (struct recovery_journal_state_7_0) {
		.journal_start = journal_start,
		.logical_blocks_used = logical_blocks_used,
		.block_map_data_blocks = block_map_data_blocks,
	};

	return VDO_SUCCESS;
}

/**
 * vdo_get_journal_operation_name() - Get the name of a journal operation.
 * @operation: The operation to name.
 *
 * Return: The name of the operation.
 */
const char *vdo_get_journal_operation_name(enum journal_operation operation)
{
	switch (operation) {
	case VDO_JOURNAL_DATA_DECREMENT:
		return "data decrement";

	case VDO_JOURNAL_DATA_INCREMENT:
		return "data increment";

	case VDO_JOURNAL_BLOCK_MAP_DECREMENT:
		return "block map decrement";

	case VDO_JOURNAL_BLOCK_MAP_INCREMENT:
		return "block map increment";

	default:
		return "unknown journal operation";
	}
}

/**
 * vdo_decode_slab_journal_entry() - Decode a slab journal entry.
 * @block: The journal block holding the entry.
 * @entry_count: The number of the entry.
 *
 * Return: The decoded entry.
 */
struct slab_journal_entry
vdo_decode_slab_journal_entry(struct packed_slab_journal_block *block,
			      journal_entry_count_t entry_count)
{
	struct slab_journal_entry entry =
		vdo_unpack_slab_journal_entry(&block->payload.entries[entry_count]);
	if (block->header.has_block_map_increments &&
	    ((block->payload.full_entries.entry_types[entry_count / 8] &
	      ((u8)1 << (entry_count % 8))) != 0))
		entry.operation = VDO_JOURNAL_BLOCK_MAP_INCREMENT;
	return entry;
}

/**
 * vdo_destroy_component_states() - Clean up any allocations in a vdo_component_states.
 * @states: The component states to destroy.
 */
void vdo_destroy_component_states(struct vdo_component_states *states)
{
	if (states == NULL)
		return;

	vdo_free_fixed_layout(UDS_FORGET(states->layout));
}

/**
 * decode_components() - Decode the components now that we know the component data is a version we
 *                       understand.
 * @buffer: The buffer being decoded.
 * @states: An object to hold the successfully decoded state.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int __must_check
decode_components(struct buffer *buffer, struct vdo_component_states *states)
{
	int result;

	result = vdo_decode_component(buffer, &states->vdo);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_decode_fixed_layout(buffer, &states->layout);
	if (result != VDO_SUCCESS)
		return result;

	result = decode_recovery_journal_state_7_0(buffer, &states->recovery_journal);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_decode_slab_depot_state_2_0(buffer, &states->slab_depot);
	if (result != VDO_SUCCESS)
		return result;

	result = decode_block_map_state_2_0(buffer, &states->block_map);
	if (result != VDO_SUCCESS)
		return result;

	ASSERT_LOG_ONLY((content_length(buffer) == 0), "All decoded component data was used");
	return VDO_SUCCESS;
}

/**
 * vdo_decode_component_states() - Decode the payload of a super block.
 * @buffer: The buffer containing the encoded super block contents.
 * @expected_release_version: The required release version.
 * @states: A pointer to hold the decoded states.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_decode_component_states(struct buffer *buffer,
				release_version_number_t expected_release_version,
				struct vdo_component_states *states)
{
	int result;

	/* Get and check the release version against the one from the geometry. */
	result = get_u32_le_from_buffer(buffer, &states->release_version);
	if (result != VDO_SUCCESS)
		return result;

	if (states->release_version != expected_release_version)
		return uds_log_error_strerror(VDO_UNSUPPORTED_VERSION,
					      "Geometry release version %u does not match super block release version %u",
					      expected_release_version,
					      states->release_version);

	/* Check the VDO volume version */
	result = vdo_decode_version_number(buffer, &states->volume_version);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_validate_version(VDO_VOLUME_VERSION_67_0, states->volume_version, "volume");
	if (result != VDO_SUCCESS)
		return result;

	result = decode_components(buffer, states);
	if (result != VDO_SUCCESS) {
		vdo_free_fixed_layout(UDS_FORGET(states->layout));
		return result;
	}

	return VDO_SUCCESS;
}

/**
 * vdo_validate_component_states() - Validate the decoded super block configuration.
 * @states: The state decoded from the super block.
 * @geometry_nonce: The nonce from the geometry block.
 * @physical_size: The minimum block count of the underlying storage.
 * @logical_size: The expected logical size of the VDO, or 0 if the logical size may be
 *                unspecified.
 *
 * Return: VDO_SUCCESS or an error if the configuration is invalid.
 */
int vdo_validate_component_states(struct vdo_component_states *states,
				  nonce_t geometry_nonce,
				  block_count_t physical_size,
				  block_count_t logical_size)
{
	if (geometry_nonce != states->vdo.nonce)
		return uds_log_error_strerror(VDO_BAD_NONCE,
					      "Geometry nonce %llu does not match superblock nonce %llu",
					      (unsigned long long) geometry_nonce,
					      (unsigned long long) states->vdo.nonce);

	return vdo_validate_config(&states->vdo.config, physical_size, logical_size);
}

/**
 * get_component_data_size() - Get the component data size of a vdo.
 * @layout: The layout of the vdo.
 *
 * Return: The component data size of the vdo.
 */
static size_t __must_check get_component_data_size(struct fixed_layout *layout)
{
	return (sizeof(release_version_number_t) +
		sizeof(struct packed_version_number) +
		vdo_get_component_encoded_size() +
		vdo_get_fixed_layout_encoded_size(layout) +
		RECOVERY_JOURNAL_COMPONENT_ENCODED_SIZE +
		vdo_get_slab_depot_encoded_size() +
		BLOCK_MAP_COMPONENT_ENCODED_SIZE);
}

/**
 * vdo_encode_component_states() - Encode the state of all vdo components for writing in the super
 *                                 block.
 * @buffer: The buffer to encode into.
 * @states: The states to encode.
 */
int vdo_encode_component_states(struct buffer *buffer, const struct vdo_component_states *states)
{
	size_t expected_size;
	int result;

	result = reset_buffer_end(buffer, 0);
	if (result != UDS_SUCCESS)
		return result;

	result = put_u32_le_into_buffer(buffer, states->release_version);
	if (result != UDS_SUCCESS)
		return result;

	result = vdo_encode_version_number(states->volume_version, buffer);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_encode_component(states->vdo, buffer);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_encode_fixed_layout(states->layout, buffer);
	if (result != VDO_SUCCESS)
		return result;

	result = encode_recovery_journal_state_7_0(states->recovery_journal, buffer);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_encode_slab_depot_state_2_0(states->slab_depot, buffer);
	if (result != VDO_SUCCESS)
		return result;

	result = encode_block_map_state_2_0(states->block_map, buffer);
	if (result != VDO_SUCCESS)
		return result;

	expected_size = get_component_data_size(states->layout);
	ASSERT_LOG_ONLY((content_length(buffer) == expected_size),
			"All super block component data was encoded");
	return VDO_SUCCESS;
}
