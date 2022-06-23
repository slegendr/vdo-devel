// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "delta-index.h"
#include "memory-alloc.h"
#include "random.h"
#include "testPrototypes.h"
#include "type-defs.h"

static const unsigned int MEAN_DELTA = 4096;
static const unsigned int NUM_PAYLOAD_BITS = 10;

/* Read a bit field from an arbitrary bit boundary. */
static INLINE unsigned int
getField(const byte *memory, uint64_t offset, int size)
{
	const void *addr = memory + offset / CHAR_BIT;

	return ((get_unaligned_le32(addr) >> (offset % CHAR_BIT)) &
		((1 << size) - 1));
}

/*
 * Compare bits between two fields.
 *
 * @param mem1     The base memory byte address (first field)
 * @param offset1  Bit offset into the memory for the start (first field)
 * @param mem2     The base memory byte address (second field)
 * @param offset2  Bit offset into the memory for the start (second field)
 * @param size     The number of bits in the field
 *
 * @return true if fields are the same, false if different
 */
static bool sameBits(const byte *mem1, uint64_t offset1, const byte *mem2,
                     uint64_t offset2, int size)
{
  unsigned int field1;
  unsigned int field2;

  enum { FIELD_BITS = 16 };
  while (size >= FIELD_BITS) {
    field1 = getField(mem1, offset1, FIELD_BITS);
    field2 = getField(mem2, offset2, FIELD_BITS);
    if (field1 != field2) {
      return false;
    }

    offset1 += FIELD_BITS;
    offset2 += FIELD_BITS;
    size -= FIELD_BITS;
  }

  if (size > 0) {
    field1 = getField(mem1, offset1, size);
    field2 = getField(mem2, offset2, size);
    if (field1 != field2) {
      return false;
    }
  }

  return true;
}

/**
 * Test move_bits
 **/
static void moveBitsTest(void)
{
  enum { NUM_LENGTHS = 2 * (sizeof(uint64_t) + sizeof(uint32_t)) * CHAR_BIT };
  enum { NUM_OFFSETS = sizeof(uint32_t) * CHAR_BIT };
  enum { MEM_SIZE = (NUM_LENGTHS + 6 * CHAR_BIT - 1) / CHAR_BIT };
  enum { MEM_BITS = MEM_SIZE * CHAR_BIT };
  enum { POST_FIELD_GUARD_BYTES = sizeof(uint64_t) - 1 };
  byte memory[MEM_SIZE + POST_FIELD_GUARD_BYTES];
  byte data[MEM_SIZE + POST_FIELD_GUARD_BYTES];
  memset(memory, 0, sizeof(memory));

  int offset1, offset2, size;
  for (size = 1; size <= NUM_LENGTHS; size++) {
    for (offset1 = 10; offset1 < 10 + NUM_OFFSETS; offset1++) {
      for (offset2 = 10; offset2 < 10 + NUM_OFFSETS; offset2++) {
        fill_randomly(data, sizeof(data));
        memcpy(memory, data, sizeof(memory));
        move_bits(memory, offset1, memory, offset2, size);
        CU_ASSERT_TRUE(sameBits(data, offset1, memory, offset2, size));
      }
    }
  }
}

/**
 * Set up a delta list
 *
 * @param pdl       The delta lists
 * @param index     The index of the delta list to set up
 * @param gapSize   Number of bits in the gap before this delta list
 * @param listSize  Number of bits in the delta list bit stream
 **/
static void setupDeltaList(struct delta_list *pdl, int index,
                           unsigned int gapSize, unsigned int listSize)
{
  pdl[index].start_offset = get_delta_list_end(&pdl[index - 1]) + gapSize;
  pdl[index].size         = listSize;
}

/**
 * Test extend_delta_memory
 *
 * @param pdl           The delta lists
 * @param numLists      The number of delta lists
 * @param initialValue  Value used to initialize the delta memory (0 or 0xFF)
 **/
static void testExtend(struct delta_list *pdl, int numLists, int initialValue)
{
  struct delta_memory *dm, *random;
  int initSize = get_delta_list_end(&pdl[numLists + 1]) / CHAR_BIT;
  size_t pdlSize = (numLists + 2) * sizeof(struct delta_list);

  // Get some random bits
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(1, struct delta_memory, __func__, &random));
  UDS_ASSERT_SUCCESS(initialize_delta_memory(random, initSize, 0, numLists,
                                             MEAN_DELTA, NUM_PAYLOAD_BITS));
  fill_randomly(random->memory, random->size);

  // Get the delta memory corresponding to the delta lists
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(1, struct delta_memory, __func__, &dm));
  UDS_ASSERT_SUCCESS(initialize_delta_memory(dm, initSize, 0, numLists,
                                             MEAN_DELTA, NUM_PAYLOAD_BITS));
  memset(dm->memory, initialValue, dm->size);
  memcpy(dm->delta_lists, pdl, pdlSize);
  validateDeltaLists(dm);

  // Copy the random bits into the delta lists
  uint64_t randomOffset = 0;
  int i;
  for (i = 1; i <= numLists; i++) {
    unsigned int size = get_delta_list_size(&dm->delta_lists[i]);
    move_bits(random->memory, randomOffset, dm->memory,
              get_delta_list_start(&dm->delta_lists[i]), size);
    randomOffset += size;
  }

  // Balance the delta lists - this will move them around evenly (if
  // possible), but should always leave the delta lists in a usable state.
  UDS_ASSERT_ERROR2(UDS_SUCCESS, UDS_OVERFLOW, extend_delta_memory(dm, 0, 0));
  validateDeltaLists(dm);

  // Verify the random data in the delta lists
  randomOffset = 0;
  for (i = 1; i <= numLists; i++) {
    unsigned int size = get_delta_list_size(&dm->delta_lists[i]);
    CU_ASSERT_TRUE(sameBits(random->memory, randomOffset, dm->memory,
                            get_delta_list_start(&dm->delta_lists[i]), size));
    randomOffset += size;
  }

  uninitialize_delta_memory(dm);
  uninitialize_delta_memory(random);
  UDS_FREE(dm);
  UDS_FREE(random);
}

/**
 * Finish delta list setup and run the extend_delta_memory tests
 *
 * @param pdl           The delta lists
 * @param numLists      The number of delta lists
 * @param gapSize       The minimum gap to leave before the guard list
 **/
static void guardAndTest(struct delta_list *pdl, int numLists,
                         unsigned int gapSize)
{
  enum { GUARD_BITS = (sizeof(uint64_t) - 1) * CHAR_BIT };
  struct delta_list *deltaListsCopy;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(numLists + 2, struct delta_list, __func__,
                                  &deltaListsCopy));

  // Set the tail guard list, which starts and ends on a byte boundary
  pdl[numLists + 1].start_offset
    = ((get_delta_list_end(&pdl[numLists]) + gapSize + CHAR_BIT - 1)
       & (-CHAR_BIT));
  pdl[numLists + 1].size = GUARD_BITS;

  memcpy(deltaListsCopy, pdl, (numLists + 2) * sizeof(struct delta_list));
  testExtend(deltaListsCopy, numLists, 0x00);

  memcpy(deltaListsCopy, pdl, (numLists + 2) * sizeof(struct delta_list));
  testExtend(deltaListsCopy, numLists, 0xFF);
  UDS_FREE(deltaListsCopy);
}

/**
 * Test with different sized blocks.
 *
 * @param increasing    Use increasing sizes
 **/
static void diffBlocks(bool increasing)
{
  enum {
    NUM_SIZES = 2048,
    NUM_LISTS = NUM_SIZES,
  };
  struct delta_list *deltaLists;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(NUM_LISTS + 2, struct delta_list, __func__,
                                  &deltaLists));

  unsigned int gapSize, i, offset;
  for (gapSize = 0; gapSize < 2 * CHAR_BIT; gapSize++) {
    for (offset = 0; offset < CHAR_BIT; offset++) {
      // Zero the first (guard) delta list
      memset(deltaLists, 0, sizeof(struct delta_list));
      // Set the size of the head guard delta list.  This artifice will let
      // us test each list at each bit offset within the byte stream.
      deltaLists[0].size = offset;
      for (i = 0; i < NUM_SIZES; i++) {
        // Each delta list is one bit longer than the preceding list
        setupDeltaList(deltaLists, i + 1, gapSize,
                       increasing ? i : NUM_SIZES - i);
      }
      deltaLists[0].size = 0;

      guardAndTest(deltaLists, NUM_LISTS, gapSize);
    }
  }
  UDS_FREE(deltaLists);
}

/**
 * Test with blocks that decrease in size
 **/
static void largeToSmallTest(void)
{
  diffBlocks(false);
}

/**
 * Test with blocks that increase in size
 **/
static void smallToLargeTest(void)
{
  diffBlocks(true);
}

/**
 * Test with blocks that are random size
 **/
static void randomTest(void)
{
  enum { NUM_LISTS = 8 * 1024 };
  struct delta_list *deltaLists;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(NUM_LISTS + 2, struct delta_list, __func__,
                                  &deltaLists));
  unsigned int i;
  for (i = 1; i <= NUM_LISTS; i++) {
    setupDeltaList(deltaLists, i,
                   random_in_range(0, sizeof(uint16_t) * CHAR_BIT),
                   random_in_range(0, 8 * 1024));
  }

  guardAndTest(deltaLists, NUM_LISTS,
               random_in_range(0, sizeof(uint16_t) * CHAR_BIT));
  UDS_FREE(deltaLists);
}

/**********************************************************************/

static const CU_TestInfo deltaMemoryTests[] = {
  {"Move Bits",    moveBitsTest },
  {"SmallToLarge", smallToLargeTest },
  {"LargeToSmall", largeToSmallTest },
  {"Random",       randomTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo deltaMemorySuite = {
  .name  = "DeltaMemory_t2",
  .tests = deltaMemoryTests,
};

/**
 * Entry point required by the module loader.
 *
 * @return      a pointer to the const CU_SuiteInfo structure.
 **/
const CU_SuiteInfo *initializeModule(void)
{
  return &deltaMemorySuite;
}
