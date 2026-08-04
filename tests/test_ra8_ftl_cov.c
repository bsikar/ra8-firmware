/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_ftl_cov.c
 * @brief Branch coverage supplement for the Flash Translation Layer (ra8_ftl.c).
 *
 * @details
 * Drives error-injection paths, bounds-check branches, capability-validation
 * arms, and the sync forward-path that are not reached by test_ra8_ftl.c.
 * A single configurable RAM-backed fake device (cov_fake_t) supports
 * read/write/erase/get_caps failure injection via boolean flags and lets each
 * test override the advertised medium capabilities.
 *
 * Source lines targeted (all from the uncovered list supplied by gcovr):
 *   88  -- ftl_reclaim_stale: erase failure return
 *   186 -- ftl_alloc_blank: reclaim failure propagation
 *   191 -- ftl_alloc_blank: no_mem after reclaim no-op
 *   198 -- ftl_alloc_blank: erase failure return
 *   284 -- ftl_write_one: alloc failure propagation
 *   288 -- ftl_write_one: program failure return
 *   331 -- ftl_bounds: count > logical_blocks
 *   334 -- ftl_bounds: lba past end
 *   373 -- ftl_dev_read: bounds-error return
 *   379 -- ftl_dev_read: read-error return
 *   420 -- ftl_dev_write: bounds-error return
 *   426 -- ftl_dev_write: write-error return
 *   465 -- ftl_dev_erase: bounds-error return
 *   539-545 -- ftl_dev_sync: full function body
 *   598 -- ftl_check_caps: read-only device
 *   601 -- ftl_check_caps: wrong erase unit
 *   604 -- ftl_check_caps: device too small
 *   697 -- ftl_validate_init_args: physical_blocks ceiling exceeded
 *   718 -- ra8_ftl_init: get_caps failure
 *   740 -- ra8_ftl_as_blockdev: raw not set
 *   753 -- ra8_ftl_wear_stats: pblocks not set
 *
 * Line 194 (defensive ftl_alloc_blank guard) is marked GCOVR_EXCL_LINE in
 * the source: ftl_pick_free with non-null arguments returns only k_ra8_ok or
 * k_ra8_err_no_data, so the guard is provably unreachable.
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_ftl.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_internal.h"
#include "unity_minimal.h"

/**
 * @enum ftl_cov_fill_t
 * @brief Per-case payload fill bytes.
 *
 * @details
 * Each case writes a distinct byte so a block that leaks between cases is
 * attributable to the case that wrote it. The values are arbitrary but must
 * stay distinct from each other and from the 0xFF erased-flash state.
 */
typedef enum : uint8_t {
  k_ftl_fill_mapped_write  = 0x55U, /**< Write that establishes a live mapping.  */
  k_ftl_fill_program_fail  = 0xAAU, /**< Payload for the program-step failure.   */
  k_ftl_fill_erase_fail    = 0xBBU, /**< Payload for the erase-step failure.     */
  k_ftl_fill_all_logical   = 0xCCU, /**< Payload written to every logical block. */
  k_ftl_fill_reclaim_fail  = 0xDDU, /**< Payload for the reclaim failure.        */
  k_ftl_fill_no_free_block = 0xEEU, /**< Payload for the exhausted free list.    */
} ftl_cov_fill_t;

/* =============================================================================
 * Sizing constants
 * =============================================================================
 */

/**
 * @enum cov_const_t
 * @brief Coverage-test fixture sizes.
 *
 * @details
 * Four logical blocks are presented by the FTL over six physical blocks,
 * giving two spare blocks. This minimum-overhead sizing lets tests exhaust
 * spare capacity quickly to drive reclamation and no-mem paths.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cov_logical    = 4,    /**< Logical blocks presented by the FTL.      */
  k_cov_phys       = 6,    /**< Physical blocks in the configurable fake. */
  k_cov_block      = 512,  /**< Bytes per block.                          */
  k_cov_erase_byte = 0xFF, /**< Medium erase value.                       */
} cov_const_t;

/* =============================================================================
 * Configurable fake block device
 * =============================================================================
 */

/**
 * @struct cov_fake_t
 * @brief State of the RAM-backed configurable fake device.
 *
 * @details
 * The `fail_*` flags inject failures in the corresponding vtable callbacks.
 * The `caps_*` fields override the capabilities reported by cov_get_caps so
 * individual tests can drive the capability-validation paths of ra8_ftl_init
 * without multiple separate fake implementations.
 *
 * @since 0.1.0
 */
typedef struct {
  /** Backing store. */
  uint8_t  store[(size_t)k_cov_phys * (size_t)k_cov_block];
  uint32_t erases;           /**< Running erase count.                          */
  bool     fail_erase;       /**< Return k_ra8_err_invalid_state from erase.    */
  bool     fail_write;       /**< Return k_ra8_err_invalid_state from write.    */
  bool     fail_read;        /**< Return k_ra8_err_invalid_state from read.     */
  bool     fail_get_caps;    /**< Return k_ra8_err_not_supported from get_caps. */
  bool     caps_read_only;   /**< Reported caps.read_only value.                */
  uint32_t caps_erase_unit;  /**< Reported erase_unit_blocks (0 => 1).          */
  uint32_t caps_block_count; /**< Reported block_count (0 => k_cov_phys).       */
} cov_fake_t;

/** @brief Singleton fake state shared across all tests in this file. */
static cov_fake_t s_cov;

/**
 * @par MC/DC:
 * (two independent single-condition early returns; no compound decision)
 */
static ra8_err_t cov_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  cov_fake_t* st = (cov_fake_t*)ctx;
  if (st->fail_read) {
    return k_ra8_err_invalid_state;
  }
  if (lba + count > (uint32_t)k_cov_phys) {
    return k_ra8_err_out_of_range;
  }
  (void)memcpy(buf,
               &st->store[(size_t)lba * (size_t)k_cov_block],
               (size_t)count * (size_t)k_cov_block);
  return k_ra8_ok;
}

/**
 * @par MC/DC:
 * (each guard is an independent single-condition return; no compound decision)
 */
static ra8_err_t cov_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  cov_fake_t* st = (cov_fake_t*)ctx;
  if (st->fail_write) {
    return k_ra8_err_invalid_state;
  }
  if (lba + count > (uint32_t)k_cov_phys) {
    return k_ra8_err_out_of_range;
  }
  /* Erase-before-write contract: every target byte must read back as 0xFF. */
  const size_t base = (size_t)lba * (size_t)k_cov_block;
  const size_t n    = (size_t)count * (size_t)k_cov_block;
  for (size_t i = 0; i < n; ++i) {
    if (st->store[base + i] != (uint8_t)k_cov_erase_byte) {
      return k_ra8_err_invalid_state; /* program to non-blank sector rejected */
    }
  }
  (void)memcpy(&st->store[base], buf, n);
  return k_ra8_ok;
}

/**
 * @par MC/DC:
 * (two independent single-condition early returns; no compound decision)
 */
static ra8_err_t cov_erase(void* ctx, uint32_t lba, uint32_t count)
{
  cov_fake_t* st = (cov_fake_t*)ctx;
  if (st->fail_erase) {
    return k_ra8_err_invalid_state;
  }
  if (lba + count > (uint32_t)k_cov_phys) {
    return k_ra8_err_out_of_range;
  }
  (void)memset(&st->store[(size_t)lba * (size_t)k_cov_block],
               (int)k_cov_erase_byte,
               (size_t)count * (size_t)k_cov_block);
  st->erases += count;
  return k_ra8_ok;
}

/**
 * @par MC/DC:
 * (single-condition fail_get_caps guard; no compound decision)
 */
static ra8_err_t cov_get_caps(const void* ctx, ra8_io_blockdev_caps_t* out)
{
  const cov_fake_t* st = (const cov_fake_t*)ctx;
  if (st->fail_get_caps) {
    return k_ra8_err_not_supported;
  }
  const uint32_t bc = (st->caps_block_count != 0U) ? st->caps_block_count : (uint32_t)k_cov_phys;
  out->block_count  = bc;
  out->erase_unit_blocks       = st->caps_erase_unit;
  out->program_size_bytes      = (uint32_t)k_cov_block;
  out->logical_block_bytes     = (uint16_t)k_cov_block;
  out->erase_value             = (uint8_t)k_cov_erase_byte;
  out->must_erase_before_write = true;
  out->read_only               = st->caps_read_only;
  return k_ra8_ok;
}

/**
 * @par MC/DC:
 * (no decision -- forwards the call and returns k_ra8_ok)
 */
static ra8_err_t cov_sync(void* ctx)
{
  (void)ctx;
  return k_ra8_ok;
}

/** @brief Configurable fake vtable. */
static const ra8_io_blockdev_iface_t k_cov_iface = {
  .read     = cov_read,
  .write    = cov_write,
  .erase    = cov_erase,
  .get_caps = cov_get_caps,
  .sync     = cov_sync,
};

/**
 * @brief Reset the fake state to a known blank configuration and bind `bd`.
 *
 * @param[out] bd Block-device handle to bind the fake into.
 */
static void cov_fake_bind(ra8_io_blockdev_t* bd)
{
  (void)memset(s_cov.store, (int)k_cov_erase_byte, sizeof(s_cov.store));
  s_cov.erases           = 0U;
  s_cov.fail_erase       = false;
  s_cov.fail_write       = false;
  s_cov.fail_read        = false;
  s_cov.fail_get_caps    = false;
  s_cov.caps_read_only   = false;
  s_cov.caps_erase_unit  = 1U;
  s_cov.caps_block_count = 0U;
  bd->iface              = &k_cov_iface;
  bd->ctx                = &s_cov;
}

/* =============================================================================
 * Tests
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (each arm tests one independent single-condition path in ftl_check_caps
 * or ftl_validate_init_args; no compound decisions in the target code)
 */
static void test_cov_cap_errors(void)
{
  TEST_BEGIN("ftl cap errors");
  ra8_io_blockdev_t raw = {};
  ra8_ftl_t         ftl = {};
  uint16_t          map[(size_t)k_cov_logical];
  ra8_ftl_pblock_t  pb[(size_t)k_cov_phys] = {};
  uint8_t           scratch[(size_t)k_cov_block];

  /* read_only device: ftl_check_caps line 598. */
  cov_fake_bind(&raw);
  s_cov.caps_read_only = true;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_ftl_init(&ftl, &raw, map, (uint32_t)k_cov_logical, pb, (uint32_t)k_cov_phys, scratch));

  /* erase_unit_blocks != 1: ftl_check_caps line 601. */
  cov_fake_bind(&raw);
  s_cov.caps_erase_unit = 2U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_ftl_init(&ftl, &raw, map, (uint32_t)k_cov_logical, pb, (uint32_t)k_cov_phys, scratch));

  /* caps.block_count < physical_blocks: ftl_check_caps line 604. */
  cov_fake_bind(&raw);
  s_cov.caps_block_count = (uint32_t)k_cov_phys - 1U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_ftl_init(&ftl, &raw, map, (uint32_t)k_cov_logical, pb, (uint32_t)k_cov_phys, scratch));

  /* physical_blocks > k_ra8_ftl_max_pblocks: ftl_validate_init_args line 697.
   * The value 0xFFFF exceeds the max-pblocks ceiling (0xFFFE); the function
   * returns before accessing the pb array. */
  cov_fake_bind(&raw);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_ftl_init(&ftl, &raw, map, (uint32_t)k_cov_logical, pb, 0xFFFFU, scratch));

  /* get_caps failure: ra8_ftl_init line 718. */
  cov_fake_bind(&raw);
  s_cov.fail_get_caps = true;
  TEST_ASSERT_EQ(
    k_ra8_err_not_supported,
    ra8_ftl_init(&ftl, &raw, map, (uint32_t)k_cov_logical, pb, (uint32_t)k_cov_phys, scratch));

  TEST_END("ftl cap errors");
}

/**
 * @par MC/DC:
 * (each assertion exercises one independent single-condition check on a
 * zero-initialised FTL handle; no compound decisions)
 */
static void test_cov_not_init(void)
{
  TEST_BEGIN("ftl not initialized paths");
  ra8_ftl_t ftl_zero = {};

  /* ra8_ftl_as_blockdev: raw == nullptr => line 740. */
  ra8_io_blockdev_t bd = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ftl_as_blockdev(&ftl_zero, &bd));

  /* ra8_ftl_wear_stats: pblocks == nullptr => line 753. */
  uint32_t hi = 0;
  uint32_t lo = 0;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ftl_wear_stats(&ftl_zero, &hi, &lo));

  TEST_END("ftl not initialized paths");
}

/**
 * @par MC/DC:
 * (each ra8_io_blockdev_* call exercises one independent branch in ftl_bounds;
 * the two ftl_bounds conditions are single-condition guards, not compound)
 *
 * Decision in ftl_bounds: count > logical_blocks (line 331)
 * - Vector A: count=5, logical=4 -> true  -> out_of_range
 * - Vector B: count=1, logical=4 -> false -> proceed
 * Single-condition, no N+1 pairing needed.
 *
 * Decision: lba > logical_blocks - count (line 334)
 * - Vector A: lba=4, count=1, logical=4 -> 4>3 -> true  -> out_of_range
 * - Vector B: lba=0, count=1, logical=4 -> 0>3 -> false -> proceed
 */
static void test_cov_bounds(void)
{
  TEST_BEGIN("ftl bounds checking");
  ra8_io_blockdev_t raw = {};
  cov_fake_bind(&raw);
  ra8_ftl_t        ftl = {};
  uint16_t         map[(size_t)k_cov_logical];
  ra8_ftl_pblock_t pb[(size_t)k_cov_phys] = {};
  uint8_t          scratch[(size_t)k_cov_block];
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ftl_init(&ftl, &raw, map, (uint32_t)k_cov_logical, pb, (uint32_t)k_cov_phys, scratch));
  ra8_io_blockdev_t bd = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_as_blockdev(&ftl, &bd));

  /* One-block read buffer is sufficient; the bounds check rejects before I/O. */
  uint8_t buf[(size_t)k_cov_block];
  (void)memset(buf, 0x00, sizeof(buf));

  /* count > logical_blocks -> line 331; propagated through read -> line 373. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_io_blockdev_read(&bd, 0U, (uint32_t)k_cov_logical + 1U, buf));

  /* lba at capacity -> line 334; propagated through read -> line 373. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_io_blockdev_read(&bd, (uint32_t)k_cov_logical, 1U, buf));

  /* count > logical_blocks -> line 331; propagated through write -> line 420. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_io_blockdev_write(&bd, 0U, (uint32_t)k_cov_logical + 1U, buf));

  /* count > logical_blocks -> line 331; propagated through erase -> line 465. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_io_blockdev_erase(&bd, 0U, (uint32_t)k_cov_logical + 1U));

  TEST_END("ftl bounds checking");
}

/**
 * @par MC/DC:
 * (no compound decision; the read-error path is a single-condition return
 * in the loop of ftl_dev_read, line 379)
 */
static void test_cov_read_error(void)
{
  TEST_BEGIN("ftl read underlying error");
  ra8_io_blockdev_t raw = {};
  cov_fake_bind(&raw);
  ra8_ftl_t        ftl = {};
  uint16_t         map[(size_t)k_cov_logical];
  ra8_ftl_pblock_t pb[(size_t)k_cov_phys] = {};
  uint8_t          scratch[(size_t)k_cov_block];
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ftl_init(&ftl, &raw, map, (uint32_t)k_cov_logical, pb, (uint32_t)k_cov_phys, scratch));
  ra8_io_blockdev_t bd = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_as_blockdev(&ftl, &bd));

  /* Write logical block 0 so it has a live physical mapping. */
  uint8_t wbuf[(size_t)k_cov_block];
  (void)memset(wbuf, k_ftl_fill_mapped_write, sizeof(wbuf));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(&bd, 0U, 1U, wbuf));

  /* Inject read failure; ftl_read_one forwards the error -> line 379. */
  s_cov.fail_read = true;
  uint8_t rbuf[(size_t)k_cov_block];
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_io_blockdev_read(&bd, 0U, 1U, rbuf));
  s_cov.fail_read = false;

  TEST_END("ftl read underlying error");
}

/**
 * @par MC/DC:
 * (no compound decision; the program-failure path is a single-condition return
 * in ftl_write_one line 288, propagated to ftl_dev_write line 426)
 */
static void test_cov_write_prog_fail(void)
{
  TEST_BEGIN("ftl write program failure");
  ra8_io_blockdev_t raw = {};
  cov_fake_bind(&raw);
  ra8_ftl_t        ftl = {};
  uint16_t         map[(size_t)k_cov_logical];
  ra8_ftl_pblock_t pb[(size_t)k_cov_phys] = {};
  uint8_t          scratch[(size_t)k_cov_block];
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ftl_init(&ftl, &raw, map, (uint32_t)k_cov_logical, pb, (uint32_t)k_cov_phys, scratch));
  ra8_io_blockdev_t bd = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_as_blockdev(&ftl, &bd));

  /* The FTL picks a free physical block and erases it (erase succeeds because
   * fail_erase is false). The subsequent program step fails at line 288.
   * The error propagates through ftl_dev_write's loop at line 426. */
  s_cov.fail_write = true;
  uint8_t buf[(size_t)k_cov_block];
  (void)memset(buf, k_ftl_fill_program_fail, sizeof(buf));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_io_blockdev_write(&bd, 0U, 1U, buf));
  s_cov.fail_write = false;

  TEST_END("ftl write program failure");
}

/**
 * @par MC/DC:
 * (no compound decision; the erase-failure path is a single-condition return
 * in ftl_alloc_blank line 198, propagated to ftl_write_one line 284 and
 * ftl_dev_write line 426)
 */
static void test_cov_alloc_erase_fail(void)
{
  TEST_BEGIN("ftl alloc erase failure");
  ra8_io_blockdev_t raw = {};
  cov_fake_bind(&raw);
  ra8_ftl_t        ftl = {};
  uint16_t         map[(size_t)k_cov_logical];
  ra8_ftl_pblock_t pb[(size_t)k_cov_phys] = {};
  uint8_t          scratch[(size_t)k_cov_block];
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ftl_init(&ftl, &raw, map, (uint32_t)k_cov_logical, pb, (uint32_t)k_cov_phys, scratch));
  ra8_io_blockdev_t bd = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_as_blockdev(&ftl, &bd));

  /* All physical blocks are FREE. ftl_alloc_blank finds one via pick_free (line
   * 182) but the erase that follows immediately fails -> line 198. This propagates
   * through ftl_write_one (line 284) and ftl_dev_write (line 426). */
  s_cov.fail_erase = true;
  uint8_t buf[(size_t)k_cov_block];
  (void)memset(buf, k_ftl_fill_erase_fail, sizeof(buf));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_io_blockdev_write(&bd, 0U, 1U, buf));
  s_cov.fail_erase = false;

  TEST_END("ftl alloc erase failure");
}

/**
 * @par MC/DC:
 * (no compound decision; the reclaim erase failure is a single-condition return
 * in ftl_reclaim_stale line 88, propagated to ftl_alloc_blank line 186,
 * ftl_write_one line 284, and ftl_dev_write line 426)
 */
static void test_cov_reclaim_erase_fail(void)
{
  TEST_BEGIN("ftl reclaim erase failure");
  ra8_io_blockdev_t raw = {};
  cov_fake_bind(&raw);
  ra8_ftl_t        ftl = {};
  uint16_t         map[(size_t)k_cov_logical];
  ra8_ftl_pblock_t pb[(size_t)k_cov_phys] = {};
  uint8_t          scratch[(size_t)k_cov_block];
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ftl_init(&ftl, &raw, map, (uint32_t)k_cov_logical, pb, (uint32_t)k_cov_phys, scratch));
  ra8_io_blockdev_t bd = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_as_blockdev(&ftl, &bd));

  /* Write all four logical blocks: physical blocks 0-3 become LIVE, 4-5 FREE. */
  uint8_t buf[(size_t)k_cov_block];
  (void)memset(buf, k_ftl_fill_all_logical, sizeof(buf));
  for (uint32_t i = 0U; i < (uint32_t)k_cov_logical; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(&bd, i, 1U, buf));
  }

  /* First overwrite of logical 0: block 4 (FREE) is allocated, block 0 -> STALE.
   * State: [0]=STALE [1]=LIVE [2]=LIVE [3]=LIVE [4]=LIVE [5]=FREE. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(&bd, 0U, 1U, buf));

  /* Second overwrite of logical 0: block 5 (FREE) is allocated, block 4 -> STALE.
   * State: [0]=STALE [1]=LIVE [2]=LIVE [3]=LIVE [4]=STALE [5]=LIVE. 0 FREE. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(&bd, 0U, 1U, buf));

  /* With no FREE blocks, the next write triggers reclamation. Injecting erase
   * failure causes ftl_reclaim_stale to fail (line 88), which propagates through
   * ftl_alloc_blank (line 186), ftl_write_one (line 284), and the loop in
   * ftl_dev_write (line 426). */
  s_cov.fail_erase = true;
  (void)memset(buf, k_ftl_fill_reclaim_fail, sizeof(buf));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_io_blockdev_write(&bd, 1U, 1U, buf));
  s_cov.fail_erase = false;

  TEST_END("ftl reclaim erase failure");
}

/**
 * @par MC/DC:
 * (no compound decision; the no-mem path is a single-condition return in
 * ftl_alloc_blank line 191, propagated to ftl_write_one line 284)
 */
static void test_cov_no_mem(void)
{
  TEST_BEGIN("ftl no memory after reclaim");
  ra8_io_blockdev_t raw = {};
  cov_fake_bind(&raw);
  ra8_ftl_t        ftl = {};
  uint16_t         map[(size_t)k_cov_logical];
  ra8_ftl_pblock_t pb[(size_t)k_cov_phys] = {};
  uint8_t          scratch[(size_t)k_cov_block];
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ftl_init(&ftl, &raw, map, (uint32_t)k_cov_logical, pb, (uint32_t)k_cov_phys, scratch));
  ra8_io_blockdev_t bd = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_as_blockdev(&ftl, &bd));

  /* Manually mark every physical block LIVE: no FREE and no STALE blocks.
   * Reclamation is a no-op; the second pick_free still returns no_data,
   * causing ftl_alloc_blank to return k_ra8_err_no_mem (line 191).
   * No I/O is issued to the underlying fake device. */
  for (uint32_t i = 0U; i < (uint32_t)k_cov_phys; ++i) {
    pb[i].state = (uint8_t)k_ra8_ftl_pstate_live;
  }
  uint8_t buf[(size_t)k_cov_block];
  (void)memset(buf, k_ftl_fill_no_free_block, sizeof(buf));
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_io_blockdev_write(&bd, 0U, 1U, buf));

  TEST_END("ftl no memory after reclaim");
}

/**
 * @par MC/DC:
 * (no compound decision; the sync function is a pure forward -- both
 * RA8_CHECK_NULL_PTR guards evaluate with non-null pointers so the
 * true branch inside each macro is not taken; the function body is linear)
 */
static void test_cov_sync(void)
{
  TEST_BEGIN("ftl sync forward");
  ra8_io_blockdev_t raw = {};
  cov_fake_bind(&raw);
  ra8_ftl_t        ftl = {};
  uint16_t         map[(size_t)k_cov_logical];
  ra8_ftl_pblock_t pb[(size_t)k_cov_phys] = {};
  uint8_t          scratch[(size_t)k_cov_block];
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ftl_init(&ftl, &raw, map, (uint32_t)k_cov_logical, pb, (uint32_t)k_cov_phys, scratch));
  ra8_io_blockdev_t bd = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ftl_as_blockdev(&ftl, &bd));

  /* Calling sync on the FTL-presented block device exercises all of
   * ftl_dev_sync (lines 539-545), which forwards through ra8_io_blockdev_sync
   * to the underlying cov_sync callback. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_sync(&bd));

  TEST_END("ftl sync forward");
}

/* =============================================================================
 * Entry point
 * =============================================================================
 */

int32_t main(void)
{
  test_cov_cap_errors();
  test_cov_not_init();
  test_cov_bounds();
  test_cov_read_error();
  test_cov_write_prog_fail();
  test_cov_alloc_erase_fail();
  test_cov_reclaim_erase_fail();
  test_cov_no_mem();
  test_cov_sync();
  (void)fprintf(stderr, "[OK  ] test_ra8_ftl_cov.c\n");
  return 0;
}
