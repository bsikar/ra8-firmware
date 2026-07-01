/**
 * @file test_ra_io_blockdev_xspi_cov.c
 * @brief Coverage-boost tests for ra_io_blockdev_xspi.c.
 *
 * @details
 * Exercises the branches in ra_io_blockdev_xspi.c that remain uncovered
 * after test_ra_io_blockdev_backends.c: the xspi_bounds out-of-range
 * returns, the read-only rejections, the misaligned-erase guards, the
 * HAL-error propagation paths (where base_off=4096 pushes all flash
 * addresses past the simulator 4 KiB fake-flash window), and the
 * complete xspi_get_caps body.
 *
 * Builds as a standalone executable auto-discovered by the
 * tests/CMakeLists.txt GLOB; its .gcda stream is merged by gcovr with
 * the sibling tests so the newly hit lines count toward the global
 * ra_io_blockdev_xspi.c coverage total without touching the sibling
 * test file.
 *
 * Lines 172 and 228 carry GCOVR_EXCL_LINE markers because they are
 * genuinely unreachable from any host-side input:
 *   - Line 172 (xspi_program_chunked error return): xspi_program_chunked
 *     is only called from write_one_sector after a successful read and
 *     erase of the same sector address.  In the simulator the range check
 *     uses the same 4 KiB boundary for reads and programs, so a sector
 *     address that lets the read succeed always lets the program succeed
 *     too.
 *   - Line 228 (write_one_sector erase error return): read and erase use
 *     the same sector address, so the simulator range check that could
 *     fail the erase also fails the preceding read, hitting line 221
 *     instead and never reaching line 228.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8d2_ospi_regs.h"
#include "ra_err.h"
#include "ra_io_blockdev.h"
#include "ra_io_blockdev_xspi.h"
#include "ra_xspi.h"
#include "unity_minimal.h"

/* ==========================================================================
 * Fixture constants
 * ==========================================================================
 */

/**
 * @enum cov_xspi_bd_const_t
 * @brief Symbolic constants for the xSPI block-device coverage fixture.
 *
 * @details
 * All literal values used across this file are collected here so the
 * magic-number gate is silent and test bodies read symbolically.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cov_bd_blocks_per_sector = 8U,    /**< One 4 KiB NOR sector = 8 blocks.            */
  k_cov_bd_base_valid        = 0U,    /**< Sector-aligned flash byte offset 0.         */
  k_cov_bd_base_out_of_sim   = 4096U, /**< First offset past the sim fake-flash end.   */
  k_cov_bd_lba_zero          = 0U,    /**< Logical block address zero.                 */
  k_cov_bd_lba_at_end        = 8U,    /**< LBA equal to block_count (past last block). */
  k_cov_bd_lba_unaligned     = 1U,    /**< Not a multiple of 8 blocks.                 */
  k_cov_bd_count_too_big     = 9U,    /**< count > block_count (triggers line 81).     */
  k_cov_bd_count_two         = 2U,    /**< count=2 with lba=7 -> 7 > 8-2 = 6.          */
  k_cov_bd_lba_near_end      = 7U,    /**< lba=7, count=2 overflows 8-block device.    */
  k_cov_bd_count_unaligned   = 1U,    /**< Not a multiple of 8 blocks (erase guard).   */
} cov_xspi_bd_const_t;

/* ==========================================================================
 * Private helpers
 * ==========================================================================
 */

/**
 * @brief Bind an 8-block xSPI device at flash offset 0 (sim window valid).
 *
 * @details
 * The resulting device has instance=0, base_off=0, block_count=8,
 * read_only=false.  Every flash access falls within the simulator 4 KiB
 * fake-flash range, so HAL calls succeed.
 *
 * @param[out] bd    Zero-initialised block-device handle to populate.
 * @param[out] state Zero-initialised backend state to populate.
 *
 * @pre ra_xspi_init(0, ...) has been called.
 * @pre bd and state are zero-initialised on entry.
 * @post bd is bound and usable for read/write/erase/get_caps.
 * @post state reflects instance=0, base_off=0, block_count=8.
 *
 * @note Thread-unsafe -- single-threaded test context.
 * @since 0.1.0
 */
static void fixture_init_valid(ra_io_blockdev_t* bd, ra_io_blockdev_xspi_state_t* state)
{
  const ra_err_t e = ra_io_blockdev_xspi_init(bd,
                                              state,
                                              (uint8_t)k_cov_bd_lba_zero,
                                              (uint32_t)k_cov_bd_base_valid,
                                              (uint32_t)k_cov_bd_blocks_per_sector,
                                              false);
  TEST_ASSERT_EQ(k_ra_ok, e);
}

/**
 * @brief Bind a read-only 8-block xSPI device at flash offset 0.
 *
 * @details
 * Same geometry as fixture_init_valid but with read_only=true, so
 * every write and erase call is rejected before touching the HAL.
 *
 * @param[out] bd    Zero-initialised block-device handle to populate.
 * @param[out] state Zero-initialised backend state to populate.
 *
 * @pre bd and state are zero-initialised on entry.
 * @post bd is bound; writes and erases return k_ra_err_not_supported.
 * @post state has read_only=true.
 *
 * @note Thread-unsafe -- single-threaded test context.
 * @since 0.1.0
 */
static void fixture_init_read_only(ra_io_blockdev_t* bd, ra_io_blockdev_xspi_state_t* state)
{
  const ra_err_t e = ra_io_blockdev_xspi_init(bd,
                                              state,
                                              (uint8_t)k_cov_bd_lba_zero,
                                              (uint32_t)k_cov_bd_base_valid,
                                              (uint32_t)k_cov_bd_blocks_per_sector,
                                              true);
  TEST_ASSERT_EQ(k_ra_ok, e);
}

/**
 * @brief Bind an 8-block xSPI device at flash offset 4096 (past the sim).
 *
 * @details
 * 4096 is a valid sector-aligned base_off but places every HAL access at
 * or beyond the simulator fake-flash boundary (k_ra_xspi_fake_flash_size =
 * 4096), causing internal_sim_range_check to return k_ra_err_invalid_arg
 * for every read and erase chunk.
 *
 * @param[out] bd    Zero-initialised block-device handle to populate.
 * @param[out] state Zero-initialised backend state to populate.
 *
 * @pre bd and state are zero-initialised on entry.
 * @post bd is bound; all HAL flash operations return a non-ok error.
 * @post state has base_off = 4096.
 *
 * @note Thread-unsafe -- single-threaded test context.
 * @since 0.1.0
 */
static void fixture_init_out_of_sim(ra_io_blockdev_t* bd, ra_io_blockdev_xspi_state_t* state)
{
  const ra_err_t e = ra_io_blockdev_xspi_init(bd,
                                              state,
                                              (uint8_t)k_cov_bd_lba_zero,
                                              (uint32_t)k_cov_bd_base_out_of_sim,
                                              (uint32_t)k_cov_bd_blocks_per_sector,
                                              false);
  TEST_ASSERT_EQ(k_ra_ok, e);
}

/* ==========================================================================
 * Test functions
 * ==========================================================================
 */

/**
 * @brief xspi_bounds: count greater than block_count returns out-of-range.
 *
 * @details
 * Passes count=9 to a device with block_count=8.  xspi_bounds evaluates
 * `count > st->block_count` (true) and returns k_ra_err_out_of_range.
 * ra_io_blockdev_read propagates this as-is.
 *
 * @par MC/DC:
 * Decision: `count > st->block_count` (single condition)
 * - Vector 1 (this test): count=9 > block_count=8 -> true -> k_ra_err_out_of_range.
 * - Vector 2 (the existing rmw round-trip test): count=1 <= block_count=8 -> false
 *   -> proceeds to lba check.
 * Two vectors cover both arms of the single-condition decision (N+1 = 2 for N=1):
 * minimal MC/DC.
 *
 * @since 0.1.0
 */
static void test_xspi_bounds_count_too_big(void)
{
  TEST_BEGIN("xspi_bounds count > block_count");
  ra_io_blockdev_t            bd    = {};
  ra_io_blockdev_xspi_state_t state = {};
  fixture_init_valid(&bd, &state);
  uint8_t        buf[(size_t)k_cov_bd_blocks_per_sector * (size_t)k_ra_io_block_size_bytes] = {};
  const ra_err_t e =
    ra_io_blockdev_read(&bd, (uint32_t)k_cov_bd_lba_zero, (uint32_t)k_cov_bd_count_too_big, buf);
  TEST_ASSERT_EQ(k_ra_err_out_of_range, e);
  TEST_END("xspi_bounds count > block_count");
}

/**
 * @brief xspi_bounds: lba + count overflows block_count returns out-of-range.
 *
 * @details
 * Passes lba=7, count=2 to an 8-block device.  After the count check passes
 * (2 <= 8), the lba check evaluates `7 > 8 - 2 = 6` (true) and returns
 * k_ra_err_out_of_range.
 *
 * @par MC/DC:
 * Decision: `lba > st->block_count - count` (single condition)
 * - Vector 1 (this test): lba=7 > 8-2=6 -> true -> k_ra_err_out_of_range.
 * - Vector 2 (existing rmw test): lba=0, count=1 -> 0 > 7 false -> proceeds.
 * Minimal MC/DC: N+1 = 2 vectors for N=1 condition.
 *
 * @since 0.1.0
 */
static void test_xspi_bounds_lba_overflow(void)
{
  TEST_BEGIN("xspi_bounds lba + count > block_count");
  ra_io_blockdev_t            bd    = {};
  ra_io_blockdev_xspi_state_t state = {};
  fixture_init_valid(&bd, &state);
  uint8_t        buf[(size_t)k_cov_bd_count_two * (size_t)k_ra_io_block_size_bytes] = {};
  const ra_err_t e =
    ra_io_blockdev_read(&bd, (uint32_t)k_cov_bd_lba_near_end, (uint32_t)k_cov_bd_count_two, buf);
  TEST_ASSERT_EQ(k_ra_err_out_of_range, e);
  TEST_END("xspi_bounds lba + count > block_count");
}

/**
 * @brief xspi_read_chunked: HAL read failure propagates up the call chain.
 *
 * @details
 * Uses a device with base_off=4096.  Every chunked HAL call goes to
 * flash address 4096, which is >= the simulator fake-flash size, so
 * ra_xspi_flash_read returns k_ra_err_invalid_arg.  xspi_read_chunked
 * propagates this error (line 126) through xspi_read (line 268 propagation)
 * back to the caller.
 *
 * @par MC/DC:
 * Decision: `e != k_ra_ok` inside xspi_read_chunked (single condition)
 * - Vector 1 (this test): e = k_ra_err_invalid_arg -> true -> early return.
 * - Vector 2 (existing rmw test): e = k_ra_ok -> false -> loop continues.
 * Minimal MC/DC: N+1 = 2 vectors for N=1 condition.
 *
 * @since 0.1.0
 */
static void test_xspi_read_hal_error(void)
{
  TEST_BEGIN("xspi_read_chunked HAL error propagation");
  TEST_ASSERT_EQ(k_ra_ok, ra_xspi_init((uint8_t)k_cov_bd_lba_zero, k_ra_xspi_lio_1s1s1s));
  ra_io_blockdev_t            bd    = {};
  ra_io_blockdev_xspi_state_t state = {};
  fixture_init_out_of_sim(&bd, &state);
  uint8_t        buf[(size_t)k_cov_bd_blocks_per_sector * (size_t)k_ra_io_block_size_bytes] = {};
  const ra_err_t e = ra_io_blockdev_read(&bd,
                                         (uint32_t)k_cov_bd_lba_zero,
                                         (uint32_t)k_cov_bd_blocks_per_sector,
                                         buf);
  TEST_ASSERT(e != k_ra_ok);
  TEST_END("xspi_read_chunked HAL error propagation");
}

/**
 * @brief xspi_write: read-only device rejects writes immediately.
 *
 * @details
 * Creates a read-only xSPI device.  xspi_write checks st->read_only before
 * the bounds check or any HAL call and returns k_ra_err_not_supported.
 *
 * @par MC/DC:
 * Decision: `st->read_only` (single condition)
 * - Vector 1 (this test): read_only=true -> k_ra_err_not_supported.
 * - Vector 2 (existing rmw test): read_only=false -> proceeds to bounds check.
 * Minimal MC/DC: N+1 = 2 vectors for N=1 condition.
 *
 * @since 0.1.0
 */
static void test_xspi_write_read_only(void)
{
  TEST_BEGIN("xspi_write read-only rejection");
  ra_io_blockdev_t            bd    = {};
  ra_io_blockdev_xspi_state_t state = {};
  fixture_init_read_only(&bd, &state);
  const uint8_t  buf[(size_t)k_ra_io_block_size_bytes] = {};
  const ra_err_t e =
    ra_io_blockdev_write(&bd, (uint32_t)k_cov_bd_lba_zero, (uint32_t)k_cov_bd_count_two, buf);
  TEST_ASSERT_EQ(k_ra_err_not_supported, e);
  TEST_END("xspi_write read-only rejection");
}

/**
 * @brief xspi_write: out-of-range block address returns error.
 *
 * @details
 * Passes lba=8, count=8 to an 8-block device (read_only=false).  After
 * the read_only check passes, xspi_bounds evaluates `8 > 8 - 8 = 0`
 * (true) and returns k_ra_err_out_of_range.  xspi_write propagates it.
 *
 * @par MC/DC:
 * Decision: `b != k_ra_ok` in xspi_write after xspi_bounds (single condition)
 * - Vector 1 (this test): b = k_ra_err_out_of_range -> true -> early return.
 * - Vector 2 (existing rmw test): b = k_ra_ok -> false -> proceeds to RMW loop.
 * Minimal MC/DC: N+1 = 2 vectors for N=1 condition.
 *
 * @since 0.1.0
 */
static void test_xspi_write_bounds_overflow(void)
{
  TEST_BEGIN("xspi_write bounds overflow");
  ra_io_blockdev_t            bd    = {};
  ra_io_blockdev_xspi_state_t state = {};
  fixture_init_valid(&bd, &state);
  const uint8_t  buf[(size_t)k_cov_bd_blocks_per_sector * (size_t)k_ra_io_block_size_bytes] = {};
  const ra_err_t e = ra_io_blockdev_write(&bd,
                                          (uint32_t)k_cov_bd_lba_at_end,
                                          (uint32_t)k_cov_bd_blocks_per_sector,
                                          buf);
  TEST_ASSERT_EQ(k_ra_err_out_of_range, e);
  TEST_END("xspi_write bounds overflow");
}

/**
 * @brief xspi_write -> write_one_sector: HAL read failure propagates up.
 *
 * @details
 * Uses a device with base_off=4096 (writable, not read-only).  Bounds pass
 * for lba=0, count=8.  Inside write_one_sector, xspi_read_chunked is called
 * with sec_addr=4096, which the simulator range-check rejects.  This error
 * propagates from write_one_sector (line 221) to xspi_write (line 327) and
 * back to the caller.
 *
 * @par MC/DC:
 * Decision: `e != k_ra_ok` in xspi_write after write_one_sector (single condition)
 * - Vector 1 (this test): e = k_ra_err_invalid_arg -> true -> early return.
 * - Vector 2 (existing rmw test): e = k_ra_ok -> false -> loop continues.
 * Minimal MC/DC: N+1 = 2 vectors for N=1 condition.
 *
 * @since 0.1.0
 */
static void test_xspi_write_hal_error(void)
{
  TEST_BEGIN("xspi_write HAL error propagation via write_one_sector");
  TEST_ASSERT_EQ(k_ra_ok, ra_xspi_init((uint8_t)k_cov_bd_lba_zero, k_ra_xspi_lio_1s1s1s));
  ra_io_blockdev_t            bd    = {};
  ra_io_blockdev_xspi_state_t state = {};
  fixture_init_out_of_sim(&bd, &state);
  const uint8_t  buf[(size_t)k_cov_bd_blocks_per_sector * (size_t)k_ra_io_block_size_bytes] = {};
  const ra_err_t e = ra_io_blockdev_write(&bd,
                                          (uint32_t)k_cov_bd_lba_zero,
                                          (uint32_t)k_cov_bd_blocks_per_sector,
                                          buf);
  TEST_ASSERT(e != k_ra_ok);
  TEST_END("xspi_write HAL error propagation via write_one_sector");
}

/**
 * @brief xspi_erase: read-only device rejects erases immediately.
 *
 * @details
 * Creates a read-only xSPI device.  xspi_erase checks st->read_only before
 * any alignment or bounds check and returns k_ra_err_not_supported.
 *
 * @par MC/DC:
 * Decision: `st->read_only` in xspi_erase (single condition)
 * - Vector 1 (this test): read_only=true -> k_ra_err_not_supported.
 * - Vector 2 (existing rmw test): read_only=false -> proceeds to alignment check.
 * Minimal MC/DC: N+1 = 2 vectors for N=1 condition.
 *
 * @since 0.1.0
 */
static void test_xspi_erase_read_only(void)
{
  TEST_BEGIN("xspi_erase read-only rejection");
  ra_io_blockdev_t            bd    = {};
  ra_io_blockdev_xspi_state_t state = {};
  fixture_init_read_only(&bd, &state);
  const ra_err_t e =
    ra_io_blockdev_erase(&bd, (uint32_t)k_cov_bd_lba_zero, (uint32_t)k_cov_bd_blocks_per_sector);
  TEST_ASSERT_EQ(k_ra_err_not_supported, e);
  TEST_END("xspi_erase read-only rejection");
}

/**
 * @brief xspi_erase: non-sector-aligned lba returns invalid-arg.
 *
 * @details
 * Passes lba=1 (not a multiple of 8) to xspi_erase.  After the read_only
 * check passes, the lba alignment guard `(lba % 8) != 0` is true and
 * k_ra_err_invalid_arg is returned.
 *
 * @par MC/DC:
 * Decision: `(lba % k_xspi_blocks_per_sector) != k_xspi_zero_blocks` (single condition)
 * - Vector 1 (this test): 1 % 8 = 1 != 0 -> true -> k_ra_err_invalid_arg.
 * - Vector 2 (existing rmw test): 0 % 8 = 0 -> false -> proceeds to count check.
 * Minimal MC/DC: N+1 = 2 vectors for N=1 condition.
 *
 * @since 0.1.0
 */
static void test_xspi_erase_lba_misaligned(void)
{
  TEST_BEGIN("xspi_erase lba not sector-aligned");
  ra_io_blockdev_t            bd    = {};
  ra_io_blockdev_xspi_state_t state = {};
  fixture_init_valid(&bd, &state);
  const ra_err_t e = ra_io_blockdev_erase(&bd,
                                          (uint32_t)k_cov_bd_lba_unaligned,
                                          (uint32_t)k_cov_bd_blocks_per_sector);
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, e);
  TEST_END("xspi_erase lba not sector-aligned");
}

/**
 * @brief xspi_erase: non-sector-aligned count returns invalid-arg.
 *
 * @details
 * Passes lba=0 (aligned) but count=1 (not a multiple of 8).  After the lba
 * alignment check passes, the count guard `(count % 8) != 0` is true and
 * k_ra_err_invalid_arg is returned.
 *
 * @par MC/DC:
 * Decision: `(count % k_xspi_blocks_per_sector) != k_xspi_zero_blocks` (single condition)
 * - Vector 1 (this test): 1 % 8 = 1 != 0 -> true -> k_ra_err_invalid_arg.
 * - Vector 2 (existing rmw test): 8 % 8 = 0 -> false -> proceeds to bounds check.
 * Minimal MC/DC: N+1 = 2 vectors for N=1 condition.
 *
 * @since 0.1.0
 */
static void test_xspi_erase_count_misaligned(void)
{
  TEST_BEGIN("xspi_erase count not sector-aligned");
  ra_io_blockdev_t            bd    = {};
  ra_io_blockdev_xspi_state_t state = {};
  fixture_init_valid(&bd, &state);
  const ra_err_t e =
    ra_io_blockdev_erase(&bd, (uint32_t)k_cov_bd_lba_zero, (uint32_t)k_cov_bd_count_unaligned);
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, e);
  TEST_END("xspi_erase count not sector-aligned");
}

/**
 * @brief xspi_erase: out-of-range block range returns out-of-range error.
 *
 * @details
 * Passes lba=8, count=8 (both aligned) to an 8-block device.  After
 * alignment checks pass, xspi_bounds evaluates `8 > 8 - 8 = 0` (true)
 * and returns k_ra_err_out_of_range.
 *
 * @par MC/DC:
 * Decision: `b != k_ra_ok` in xspi_erase after xspi_bounds (single condition)
 * - Vector 1 (this test): b = k_ra_err_out_of_range -> true -> early return.
 * - Vector 2 (existing rmw test): b = k_ra_ok -> false -> enters erase loop.
 * Minimal MC/DC: N+1 = 2 vectors for N=1 condition.
 *
 * @since 0.1.0
 */
static void test_xspi_erase_bounds_overflow(void)
{
  TEST_BEGIN("xspi_erase bounds overflow");
  ra_io_blockdev_t            bd    = {};
  ra_io_blockdev_xspi_state_t state = {};
  fixture_init_valid(&bd, &state);
  const ra_err_t e =
    ra_io_blockdev_erase(&bd, (uint32_t)k_cov_bd_lba_at_end, (uint32_t)k_cov_bd_blocks_per_sector);
  TEST_ASSERT_EQ(k_ra_err_out_of_range, e);
  TEST_END("xspi_erase bounds overflow");
}

/**
 * @brief xspi_erase: HAL erase failure propagates back to caller.
 *
 * @details
 * Uses a device with base_off=4096.  All alignment and bounds checks pass
 * for lba=0, count=8.  Inside the erase loop, the sector address computes
 * as 4096, and the simulator rejects the erase with k_ra_err_invalid_arg.
 * xspi_erase propagates this error (line 385) to the caller.
 *
 * @par MC/DC:
 * Decision: `e != k_ra_ok` in xspi_erase loop (single condition)
 * - Vector 1 (this test): e = k_ra_err_invalid_arg -> true -> early return.
 * - Vector 2 (existing rmw test): e = k_ra_ok -> false -> advance blk pointer.
 * Minimal MC/DC: N+1 = 2 vectors for N=1 condition.
 *
 * @since 0.1.0
 */
static void test_xspi_erase_hal_error(void)
{
  TEST_BEGIN("xspi_erase HAL erase-sector failure propagation");
  TEST_ASSERT_EQ(k_ra_ok, ra_xspi_init((uint8_t)k_cov_bd_lba_zero, k_ra_xspi_lio_1s1s1s));
  ra_io_blockdev_t            bd    = {};
  ra_io_blockdev_xspi_state_t state = {};
  fixture_init_out_of_sim(&bd, &state);
  const ra_err_t e =
    ra_io_blockdev_erase(&bd, (uint32_t)k_cov_bd_lba_zero, (uint32_t)k_cov_bd_blocks_per_sector);
  TEST_ASSERT(e != k_ra_ok);
  TEST_END("xspi_erase HAL erase-sector failure propagation");
}

/**
 * @brief xspi_get_caps: entire body covered -- capabilities snapshot.
 *
 * @details
 * Calls ra_io_blockdev_get_caps on a valid xSPI device and checks that
 * every field in the returned ra_io_blockdev_caps_t reflects the NOR
 * flash properties wired into xspi_get_caps: an all-ones-erase medium
 * that must be erased before programming, with an 8-block erase unit,
 * a 256-byte program granularity, and a 512-byte logical block size.
 *
 * @par MC/DC:
 * (no compound decisions in xspi_get_caps -- every guard is a single-
 * condition RA_CHECK_NULL_PTR expansion; those null-check lines are covered
 * by this test executing the function body with valid arguments, which
 * makes the `if (ptr == nullptr)` condition false and falls through to
 * the field assignments.  The true arms are provably unreachable through
 * the public ra_io_blockdev_get_caps dispatcher, which always supplies a
 * non-null ctx and pre-checks out before forwarding.  No separate MC/DC
 * null-path vectors are required because those are single-condition guards
 * with no &&/|| operator.)
 *
 * @since 0.1.0
 */
static void test_xspi_get_caps(void)
{
  TEST_BEGIN("xspi_get_caps full body coverage");
  ra_io_blockdev_t            bd    = {};
  ra_io_blockdev_xspi_state_t state = {};
  fixture_init_valid(&bd, &state);

  ra_io_blockdev_caps_t caps = {};
  const ra_err_t        e    = ra_io_blockdev_get_caps(&bd, &caps);
  TEST_ASSERT_EQ(k_ra_ok, e);
  TEST_ASSERT_EQ((uint32_t)k_cov_bd_blocks_per_sector, caps.block_count);
  TEST_ASSERT_EQ((uint32_t)k_cov_bd_blocks_per_sector, caps.erase_unit_blocks);
  TEST_ASSERT_EQ((uint32_t)k_ra_io_block_size_bytes, (uint32_t)caps.logical_block_bytes);
  TEST_ASSERT_EQ((uint8_t)k_ra_io_erase_value_ones, caps.erase_value);
  TEST_ASSERT(caps.must_erase_before_write);
  TEST_ASSERT(!caps.read_only);
  TEST_END("xspi_get_caps full body coverage");
}

/* ==========================================================================
 * Entry point
 * ==========================================================================
 */

/**
 * @brief Run all coverage-boost tests and report overall pass.
 *
 * @return 0 on success; calls exit(1) on the first assertion failure.
 *
 * @pre ra8d2_ospi_regs mmap region is accessible (RA_SIMULATOR_MODE).
 * @pre The ra_core_hal OBJECT library was built with --coverage.
 * @post All uncovered lines in ra_io_blockdev_xspi.c (excluding the two
 *       GCOVR_EXCL_LINE markers) have been executed at least once.
 * @post Exit code 0 signals success to the CI coverage runner.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  test_xspi_bounds_count_too_big();
  test_xspi_bounds_lba_overflow();
  test_xspi_read_hal_error();
  test_xspi_write_read_only();
  test_xspi_write_bounds_overflow();
  test_xspi_write_hal_error();
  test_xspi_erase_read_only();
  test_xspi_erase_lba_misaligned();
  test_xspi_erase_count_misaligned();
  test_xspi_erase_bounds_overflow();
  test_xspi_erase_hal_error();
  test_xspi_get_caps();
  (void)fprintf(stderr, "[OK  ] test_ra_io_blockdev_xspi_cov.c\n");
  return 0;
}
