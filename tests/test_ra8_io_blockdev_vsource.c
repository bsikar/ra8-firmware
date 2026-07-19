/**
 * @file test_ra8_io_blockdev_vsource.c
 * @brief Unit tests for the block-device -> ra8_vsource read adapter (#147/#155).
 *
 * @details
 * Proves the Ring-4 adapter (`ra8_io_blockdev_vsource.h`) reads back bytes
 * identical to ::ra8_io_blockdev_read over a RAM block device. Blocks are written
 * through the block device; the same bytes are then read through the vsource
 * adapter at byte offsets -- aligned, unaligned head, unaligned tail, and a
 * multi-sector unaligned span -- and compared with memcmp. Also exercises the
 * validation guards and the end-to-end wiring into ::ra8_vsource_add_paged so the
 * registry's read callback resolves to the adapter.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_ram.h"
#include "ra8_io_blockdev_vsource.h"
#include "ra8_vsource.h"
#include "unity_minimal.h"

/**
 * @enum io_blockdev_vsource_fixture_t
 * @brief The payload generators and their seeds, plus the byte-level helpers.
 */
typedef enum : uint8_t {
  k_vsrc_pattern_stride = 13U, /**< Stride of the golden generator, `i * 13 + 7`. */
  k_vsrc_pattern_bias   = 7U,  /**< Its bias, so index 0 is not byte 0.           */
  k_byte_mask = 0xFFU, /**< Low-byte mask used to split the connection handle little-endian. */
} io_blockdev_vsource_fixture_t;

/**
 * @enum io_blockdev_vsource_fixture2_t
 * @brief Poison values written into out-parameters before a call, so one that fails without assigning is detectable.
 */
typedef enum : uint32_t {
  k_vsrc_oid_poison =
    0xFFFFFFFFU, /**< Poison object id written before a registration, so a call that fails without assigning one is detectable. */
} io_blockdev_vsource_fixture2_t;

/**
 * @enum t_bdvs_const_t
 * @brief Fixture sizes.
 */
typedef enum : uint32_t {
  k_t_blocks    = 8U,    /**< RAM disk size in 512-byte blocks. */
  k_t_objs      = 2U,    /**< vsource registry capacity.        */
  k_t_total     = 4096U, /**< 8 blocks * 512 bytes.             */
  k_t_span_off  = 500U,  /**< Cross-sector unaligned offset.    */
  k_t_span_len  = 600U,  /**< Cross-sector unaligned length.    */
  k_t_head_off  = 100U,  /**< Mid-sector head offset.           */
  k_t_head_len  = 50U,   /**< Bytes within one sector.          */
  k_t_block_len = 512U,  /**< Logical block size.               */
} t_bdvs_const_t;

/** @brief Backing buffer for the RAM block device. */
static uint8_t s_disk[(size_t)k_t_total];

/** @brief Golden reference of the disk contents (written via the block device). */
static uint8_t s_golden[(size_t)k_t_total];

static ra8_io_blockdev_t             s_bd;
static ra8_io_blockdev_ram_state_t   s_state;
static ra8_io_blockdev_vsource_ctx_t s_ctx;
static ra8_vsource_obj_t             s_objs[(size_t)k_t_objs];

/** @brief Bind a RAM block device and fill it with a deterministic pattern. */
static void t_setup_disk(void)
{
  s_bd    = (ra8_io_blockdev_t){};
  s_state = (ra8_io_blockdev_ram_state_t){};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_ram_init(&s_bd, &s_state, s_disk, k_t_blocks, false));
  for (uint32_t i = 0; i < (uint32_t)k_t_total; ++i) {
    s_golden[i] = (uint8_t)(((i * k_vsrc_pattern_stride) + k_vsrc_pattern_bias) & k_byte_mask);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(&s_bd, 0U, k_t_blocks, s_golden));
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- init rejects each NULL argument via
 * independent single-condition guards, then binds the device)
 */
static void test_init_validation(void)
{
  TEST_BEGIN("bdvs init validation");
  ra8_io_blockdev_vsource_ctx_t ctx = {};
  ra8_io_blockdev_t             bd  = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_blockdev_vsource_init(nullptr, &bd));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_blockdev_vsource_init(&ctx, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_vsource_init(&ctx, &bd));
  TEST_END("bdvs init validation");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- the read callback rejects each NULL
 * argument independently)
 */
static void test_read_validation(void)
{
  TEST_BEGIN("bdvs read validation");
  uint8_t buf[(size_t)k_t_block_len] = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_blockdev_vsource_read(nullptr, 0U, buf, k_t_block_len));
  t_setup_disk();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_vsource_init(&s_ctx, &s_bd));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_blockdev_vsource_read(&s_ctx, 0U, nullptr, k_t_block_len));
  TEST_END("bdvs read validation");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a sector-aligned read of one whole block
 * matches the golden reference byte-for-byte)
 */
static void test_aligned_block(void)
{
  TEST_BEGIN("bdvs aligned whole block");
  t_setup_disk();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_vsource_init(&s_ctx, &s_bd));
  uint8_t got[(size_t)k_t_block_len] = {};
  /* Read block index 2 (byte offset 1024) through the adapter. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_blockdev_vsource_read(&s_ctx, (uint64_t)k_t_block_len * 2U, got, k_t_block_len));
  TEST_ASSERT_EQ(0, memcmp(got, &s_golden[(size_t)k_t_block_len * 2U], (size_t)k_t_block_len));
  TEST_END("bdvs aligned whole block");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- an unaligned read entirely inside one
 * sector goes through the bounce buffer and matches the golden bytes)
 */
static void test_unaligned_head_only(void)
{
  TEST_BEGIN("bdvs unaligned head within one sector");
  t_setup_disk();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_vsource_init(&s_ctx, &s_bd));
  uint8_t got[(size_t)k_t_head_len] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_vsource_read(&s_ctx, k_t_head_off, got, k_t_head_len));
  TEST_ASSERT_EQ(0, memcmp(got, &s_golden[(size_t)k_t_head_off], (size_t)k_t_head_len));
  TEST_END("bdvs unaligned head within one sector");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- an unaligned span crossing sector
 * boundaries exercises head + aligned middle + tail and matches the golden bytes)
 */
static void test_unaligned_span(void)
{
  TEST_BEGIN("bdvs unaligned multi-sector span");
  t_setup_disk();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_vsource_init(&s_ctx, &s_bd));
  uint8_t got[(size_t)k_t_span_len] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_vsource_read(&s_ctx, k_t_span_off, got, k_t_span_len));
  TEST_ASSERT_EQ(0, memcmp(got, &s_golden[(size_t)k_t_span_off], (size_t)k_t_span_len));
  TEST_END("bdvs unaligned multi-sector span");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a read past the device end is rejected by
 * the underlying block device, not silently truncated)
 */
static void test_out_of_range(void)
{
  TEST_BEGIN("bdvs read past device end");
  t_setup_disk();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_vsource_init(&s_ctx, &s_bd));
  uint8_t got[(size_t)k_t_block_len] = {};
  /* Aligned read one block past the end. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_io_blockdev_vsource_read(&s_ctx,
                                              (uint64_t)k_t_block_len * (uint64_t)k_t_blocks,
                                              got,
                                              k_t_block_len));
  TEST_END("bdvs read past device end");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- registering the adapter as a paged source
 * and paging through ::ra8_vsource_loader yields the golden bytes)
 */
static void test_vsource_wiring(void)
{
  TEST_BEGIN("bdvs wired into ra8_vsource_add_paged");
  t_setup_disk();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_vsource_init(&s_ctx, &s_bd));

  ra8_vsource_t vs = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_init(&vs, s_objs, k_t_objs));
  uint32_t oid = k_vsrc_oid_poison;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_vsource_add_paged(&vs, ra8_io_blockdev_vsource_read, &s_ctx, 0U, k_t_total, &oid));

  /* Page a 512-byte frame in at an aligned object offset via the loader. */
  uint8_t frame[(size_t)k_t_block_len] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_vsource_loader(&vs, oid, (uint64_t)k_t_block_len * 3U, frame, k_t_block_len));
  TEST_ASSERT_EQ(0, memcmp(frame, &s_golden[(size_t)k_t_block_len * 3U], (size_t)k_t_block_len));
  TEST_END("bdvs wired into ra8_vsource_add_paged");
}

int32_t main(void)
{
  test_init_validation();
  test_read_validation();
  test_aligned_block();
  test_unaligned_head_only();
  test_unaligned_span();
  test_out_of_range();
  test_vsource_wiring();
  (void)fprintf(stderr, "[OK  ] test_ra8_io_blockdev_vsource.c\n");
  return 0;
}
