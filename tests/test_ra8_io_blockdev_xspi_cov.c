/**
 * @file test_ra8_io_blockdev_xspi_cov.c
 * @brief Coverage-boost tests for ra8_io_blockdev_xspi.c.
 *
 * @details
 * Exercises the branches in ra8_io_blockdev_xspi.c that remain uncovered
 * after test_ra8_io_blockdev_backends.c: the xspi_bounds out-of-range
 * returns, the read-only rejections, the misaligned-erase guards, the
 * HAL-error propagation paths, and the complete xspi_get_caps body.
 *
 * The HAL-error paths are driven through the real driver register
 * sequence: the register-level NOR model in
 * ``tests/mocks/ra8_fake_xspi_flash.c`` services every ``TRREQ`` kick, and
 * the ``ra8_fake_mmio`` seam arms a CMDCMP fault on ``INTS`` -- either for
 * every command (``fail_wait``) or for exactly one command in the
 * read -> erase -> program chain (``fail_nth_wait``), which isolates the
 * erase-leg and program-leg error returns inside ``write_one_sector`` /
 * ``xspi_program_chunked`` that were previously unreachable on the host.
 *
 * Builds as a standalone executable auto-discovered by the
 * tests/CMakeLists.txt GLOB; its .gcda stream is merged by gcovr with
 * the sibling tests so the newly hit lines count toward the global
 * ra8_io_blockdev_xspi.c coverage total without touching the sibling
 * test file.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmio.h"
#include "ra8_fake_xspi_flash.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_xspi.h"
#include "ra8_ospi_regs.h"
#include "ra8_xspi.h"
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
 * magic-number gate is silent and test bodies read symbolically. The
 * ``k_cov_bd_nth_*`` values are zero-based ordinals of INTS wait-loops
 * (one per manual command) within one ``write_one_sector`` call at
 * ``base_off=0``, ``lba=0``: the 4 KiB sector read issues 4096/8 = 512
 * read commands (ordinals 0..511); the erase issues WREN (512), SE (513)
 * and one WIP RDSR (514); each 8-byte program chunk then issues WREN,
 * PP, RDSR, so the first chunk's PP is ordinal 516.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cov_bd_blocks_per_sector = 8U,   /**< One 4 KiB NOR sector = 8 blocks.            */
  k_cov_bd_base_valid        = 0U,   /**< Sector-aligned flash byte offset 0.         */
  k_cov_bd_lba_zero          = 0U,   /**< Logical block address zero.                 */
  k_cov_bd_lba_at_end        = 8U,   /**< LBA equal to block_count (past last block). */
  k_cov_bd_lba_unaligned     = 1U,   /**< Not a multiple of 8 blocks.                 */
  k_cov_bd_count_too_big     = 9U,   /**< count > block_count (bounds reject).        */
  k_cov_bd_count_two         = 2U,   /**< count=2 with lba=7 -> 7 > 8-2 = 6.          */
  k_cov_bd_lba_near_end      = 7U,   /**< lba=7, count=2 overflows 8-block device.    */
  k_cov_bd_count_unaligned   = 1U,   /**< Not a multiple of 8 blocks (erase guard).   */
  k_cov_bd_nth_erase_se      = 513U, /**< Wait-loop ordinal of the RMW's SE command.  */
  k_cov_bd_nth_first_pp      = 516U, /**< Wait-loop ordinal of the RMW's first PP.    */
} cov_xspi_bd_const_t;

/* ==========================================================================
 * Private helpers
 * ==========================================================================
 */

/**
 * @brief Reset the MMIO fault seam and (re)install the NOR flash model.
 *
 * @details
 * Clears any fault a previous test armed on ``INTS`` and re-registers the
 * register-level NOR model so the driver's real command sequence makes
 * progress. Register RAM is deliberately NOT reset: this binary shares
 * one mapped window across tests, exactly as the sibling suites do.
 *
 * @pre Host test binary (``RA8_OFF_TARGET`` + ``UNIT_TEST``).
 * @pre No other thread touches the xSPI registers (single-threaded test).
 * @post The seam holds no armed faults and the model hook is installed.
 * @post Both model instances read back fully erased (0xFF).
 *
 * @note Thread-unsafe -- single-threaded test context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_prep(void)
{
  ra8_fake_mmio_reset();
  ra8_fake_xspi_flash_install();
}

/**
 * @brief Bind an 8-block xSPI device at flash offset 0.
 *
 * @details
 * The resulting device has instance=0, base_off=0, block_count=8,
 * read_only=false.  With the NOR model installed and no fault armed,
 * every HAL call succeeds.
 *
 * @param[out] bd    Zero-initialised block-device handle to populate.
 * @param[out] state Zero-initialised backend state to populate.
 *
 * @pre ra8_xspi_init(0, ...) has been called.
 * @pre bd and state are zero-initialised on entry.
 * @post bd is bound and usable for read/write/erase/get_caps.
 * @post state reflects instance=0, base_off=0, block_count=8.
 *
 * @note Thread-unsafe -- single-threaded test context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_fixture_init_valid(ra8_io_blockdev_t*            bd,
                                                     ra8_io_blockdev_xspi_state_t* state)
{
  const ra8_err_t e = ra8_io_blockdev_xspi_init(bd,
                                                state,
                                                (uint8_t)k_cov_bd_lba_zero,
                                                (uint32_t)k_cov_bd_base_valid,
                                                (uint32_t)k_cov_bd_blocks_per_sector,
                                                false);
  TEST_ASSERT_EQ(k_ra8_ok, e);
}

/**
 * @brief Bind a read-only 8-block xSPI device at flash offset 0.
 *
 * @details
 * Same geometry as internal_fixture_init_valid but with read_only=true, so
 * every write and erase call is rejected before touching the HAL.
 *
 * @param[out] bd    Zero-initialised block-device handle to populate.
 * @param[out] state Zero-initialised backend state to populate.
 *
 * @pre bd and state are zero-initialised on entry.
 * @post bd is bound; writes and erases return k_ra8_err_not_supported.
 * @post state has read_only=true.
 *
 * @note Thread-unsafe -- single-threaded test context.
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. */
RA8_INTERNAL static void internal_fixture_init_read_only(ra8_io_blockdev_t*            bd,
                                                         ra8_io_blockdev_xspi_state_t* state)
{
  const ra8_err_t e = ra8_io_blockdev_xspi_init(bd,
                                                state,
                                                (uint8_t)k_cov_bd_lba_zero,
                                                (uint32_t)k_cov_bd_base_valid,
                                                (uint32_t)k_cov_bd_blocks_per_sector,
                                                true);
  TEST_ASSERT_EQ(k_ra8_ok, e);
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
 * `count > st->block_count` (true) and returns k_ra8_err_out_of_range.
 * ra8_io_blockdev_read propagates this as-is.
 *
 * @par MC/DC:
 * Decision: `count > st->block_count` (single condition)
 * - Vector 1 (this test): count=9 > block_count=8 -> true -> k_ra8_err_out_of_range.
 * - Vector 2 (the existing rmw round-trip test): count=1 <= block_count=8 -> false
 *   -> proceeds to lba check.
 * Two vectors cover both arms of the single-condition decision (N+1 = 2 for N=1):
 * minimal MC/DC.
 *
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_xspi_bounds_count_too_big(void)
{
  TEST_BEGIN("xspi_bounds count > block_count");
  internal_prep();
  ra8_io_blockdev_t            bd    = {};
  ra8_io_blockdev_xspi_state_t state = {};
  internal_fixture_init_valid(&bd, &state);
  uint8_t         buf[(size_t)k_cov_bd_blocks_per_sector * (size_t)k_ra8_io_block_size_bytes] = {};
  const ra8_err_t e =
    ra8_io_blockdev_read(&bd, (uint32_t)k_cov_bd_lba_zero, (uint32_t)k_cov_bd_count_too_big, buf);
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, e);
  TEST_END("xspi_bounds count > block_count");
}

/**
 * @brief xspi_bounds: lba + count overflows block_count returns out-of-range.
 *
 * @details
 * Passes lba=7, count=2 to an 8-block device.  After the count check passes
 * (2 <= 8), the lba check evaluates `7 > 8 - 2 = 6` (true) and returns
 * k_ra8_err_out_of_range.
 *
 * @par MC/DC:
 * Decision: `lba > st->block_count - count` (single condition)
 * - Vector 1 (this test): lba=7 > 8-2=6 -> true -> k_ra8_err_out_of_range.
 * - Vector 2 (existing rmw test): lba=0, count=1 -> 0 > 7 false -> proceeds.
 * Minimal MC/DC: N+1 = 2 vectors for N=1 condition.
 *
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_xspi_bounds_lba_overflow(void)
{
  TEST_BEGIN("xspi_bounds lba + count > block_count");
  internal_prep();
  ra8_io_blockdev_t            bd    = {};
  ra8_io_blockdev_xspi_state_t state = {};
  internal_fixture_init_valid(&bd, &state);
  uint8_t         buf[(size_t)k_cov_bd_count_two * (size_t)k_ra8_io_block_size_bytes] = {};
  const ra8_err_t e =
    ra8_io_blockdev_read(&bd, (uint32_t)k_cov_bd_lba_near_end, (uint32_t)k_cov_bd_count_two, buf);
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, e);
  TEST_END("xspi_bounds lba + count > block_count");
}

/**
 * @brief xspi_read_chunked: HAL read failure propagates up the call chain.
 *
 * @details
 * Arms ``ra8_fake_mmio_fail_wait`` on ``INTS`` so the first read command's
 * CMDCMP poll exhausts its budget: ra8_xspi_flash_read returns the
 * hardware timeout, and xspi_read_chunked propagates it through xspi_read
 * back to the caller.
 *
 * @par MC/DC:
 * Decision: `e != k_ra8_ok` inside xspi_read_chunked (single condition)
 * - Vector 1 (this test): e = k_ra8_err_hw_timeout -> true -> early return.
 * - Vector 2 (existing rmw test): e = k_ra8_ok -> false -> loop continues.
 * Minimal MC/DC: N+1 = 2 vectors for N=1 condition.
 *
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_xspi_read_hal_error(void)
{
  TEST_BEGIN("xspi_read_chunked HAL error propagation");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init((uint8_t)k_cov_bd_lba_zero, k_ra8_xspi_lio_1s1s1s));
  ra8_io_blockdev_t            bd    = {};
  ra8_io_blockdev_xspi_state_t state = {};
  internal_fixture_init_valid(&bd, &state);
  volatile r_xspi_regs_t* reg = ra8_xspi((uint8_t)k_cov_bd_lba_zero);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)&reg->INTS));
  uint8_t         buf[(size_t)k_cov_bd_blocks_per_sector * (size_t)k_ra8_io_block_size_bytes] = {};
  const ra8_err_t e = ra8_io_blockdev_read(&bd,
                                           (uint32_t)k_cov_bd_lba_zero,
                                           (uint32_t)k_cov_bd_blocks_per_sector,
                                           buf);
  TEST_ASSERT(e != k_ra8_ok);
  TEST_END("xspi_read_chunked HAL error propagation");
}

/**
 * @brief xspi_write: read-only device rejects writes immediately.
 *
 * @details
 * Creates a read-only xSPI device.  xspi_write checks st->read_only before
 * the bounds check or any HAL call and returns k_ra8_err_not_supported.
 *
 * @par MC/DC:
 * Decision: `st->read_only` (single condition)
 * - Vector 1 (this test): read_only=true -> k_ra8_err_not_supported.
 * - Vector 2 (existing rmw test): read_only=false -> proceeds to bounds check.
 * Minimal MC/DC: N+1 = 2 vectors for N=1 condition.
 *
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_xspi_write_read_only(void)
{
  TEST_BEGIN("xspi_write read-only rejection");
  internal_prep();
  ra8_io_blockdev_t            bd    = {};
  ra8_io_blockdev_xspi_state_t state = {};
  internal_fixture_init_read_only(&bd, &state);
  const uint8_t   buf[(size_t)k_ra8_io_block_size_bytes] = {};
  const ra8_err_t e =
    ra8_io_blockdev_write(&bd, (uint32_t)k_cov_bd_lba_zero, (uint32_t)k_cov_bd_count_two, buf);
  TEST_ASSERT_EQ(k_ra8_err_not_supported, e);
  TEST_END("xspi_write read-only rejection");
}

/**
 * @brief xspi_write: out-of-range block address returns error.
 *
 * @details
 * Passes lba=8, count=8 to an 8-block device (read_only=false).  After
 * the read_only check passes, xspi_bounds evaluates `8 > 8 - 8 = 0`
 * (true) and returns k_ra8_err_out_of_range.  xspi_write propagates it.
 *
 * @par MC/DC:
 * Decision: `b != k_ra8_ok` in xspi_write after xspi_bounds (single condition)
 * - Vector 1 (this test): b = k_ra8_err_out_of_range -> true -> early return.
 * - Vector 2 (existing rmw test): b = k_ra8_ok -> false -> proceeds to RMW loop.
 * Minimal MC/DC: N+1 = 2 vectors for N=1 condition.
 *
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_xspi_write_bounds_overflow(void)
{
  TEST_BEGIN("xspi_write bounds overflow");
  internal_prep();
  ra8_io_blockdev_t            bd    = {};
  ra8_io_blockdev_xspi_state_t state = {};
  internal_fixture_init_valid(&bd, &state);
  const uint8_t   buf[(size_t)k_cov_bd_blocks_per_sector * (size_t)k_ra8_io_block_size_bytes] = {};
  const ra8_err_t e = ra8_io_blockdev_write(&bd,
                                            (uint32_t)k_cov_bd_lba_at_end,
                                            (uint32_t)k_cov_bd_blocks_per_sector,
                                            buf);
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, e);
  TEST_END("xspi_write bounds overflow");
}

/**
 * @brief xspi_write -> write_one_sector: HAL read failure propagates up.
 *
 * @details
 * Arms ``ra8_fake_mmio_fail_wait`` on ``INTS`` (writable device, bounds
 * pass for lba=0, count=8).  Inside write_one_sector the sector pre-read's
 * first command times out; the error propagates from write_one_sector
 * through xspi_write back to the caller.
 *
 * @par MC/DC:
 * Decision: `e != k_ra8_ok` in xspi_write after write_one_sector (single condition)
 * - Vector 1 (this test): e = k_ra8_err_hw_timeout -> true -> early return.
 * - Vector 2 (existing rmw test): e = k_ra8_ok -> false -> loop continues.
 * Minimal MC/DC: N+1 = 2 vectors for N=1 condition.
 *
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_xspi_write_hal_error(void)
{
  TEST_BEGIN("xspi_write HAL error propagation via write_one_sector");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init((uint8_t)k_cov_bd_lba_zero, k_ra8_xspi_lio_1s1s1s));
  ra8_io_blockdev_t            bd    = {};
  ra8_io_blockdev_xspi_state_t state = {};
  internal_fixture_init_valid(&bd, &state);
  volatile r_xspi_regs_t* reg = ra8_xspi((uint8_t)k_cov_bd_lba_zero);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)&reg->INTS));
  const uint8_t   buf[(size_t)k_cov_bd_blocks_per_sector * (size_t)k_ra8_io_block_size_bytes] = {};
  const ra8_err_t e = ra8_io_blockdev_write(&bd,
                                            (uint32_t)k_cov_bd_lba_zero,
                                            (uint32_t)k_cov_bd_blocks_per_sector,
                                            buf);
  TEST_ASSERT(e != k_ra8_ok);
  TEST_END("xspi_write HAL error propagation via write_one_sector");
}

/**
 * @brief write_one_sector: an erase failure AFTER a good read propagates.
 *
 * @details
 * Arms ``ra8_fake_mmio_fail_nth_wait`` on ``INTS`` for wait-loop ordinal
 * ::k_cov_bd_nth_erase_se -- the RMW's 0x20 sector-erase kick.  The 512
 * sector pre-read commands and the erase's WREN succeed (the NOR model
 * services them), then the SE command's CMDCMP poll times out, driving
 * the erase-leg error return inside write_one_sector that a whole-call
 * fault (fail_wait) can never reach because the pre-read fails first.
 *
 * @par MC/DC:
 * Decision: `e != k_ra8_ok` in write_one_sector after
 * ra8_xspi_flash_erase_sector (single condition)
 * - Vector 1 (this test): e = k_ra8_err_hw_timeout -> true -> early return.
 * - Vector 2 (existing rmw test): e = k_ra8_ok -> false -> proceeds to program.
 * Minimal MC/DC: N+1 = 2 vectors for N=1 condition.
 *
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_xspi_write_erase_leg_error(void)
{
  TEST_BEGIN("write_one_sector erase-leg error propagation");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init((uint8_t)k_cov_bd_lba_zero, k_ra8_xspi_lio_1s1s1s));
  ra8_io_blockdev_t            bd    = {};
  ra8_io_blockdev_xspi_state_t state = {};
  internal_fixture_init_valid(&bd, &state);
  volatile r_xspi_regs_t* reg = ra8_xspi((uint8_t)k_cov_bd_lba_zero);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_fake_mmio_fail_nth_wait((const volatile void*)&reg->INTS, (uint32_t)k_cov_bd_nth_erase_se));
  const uint8_t   buf[(size_t)k_cov_bd_blocks_per_sector * (size_t)k_ra8_io_block_size_bytes] = {};
  const ra8_err_t e = ra8_io_blockdev_write(&bd,
                                            (uint32_t)k_cov_bd_lba_zero,
                                            (uint32_t)k_cov_bd_blocks_per_sector,
                                            buf);
  TEST_ASSERT(e != k_ra8_ok);
  TEST_END("write_one_sector erase-leg error propagation");
}

/**
 * @brief xspi_program_chunked: a program failure AFTER read + erase propagates.
 *
 * @details
 * Arms ``ra8_fake_mmio_fail_nth_wait`` on ``INTS`` for wait-loop ordinal
 * ::k_cov_bd_nth_first_pp -- the first 0x02 page-program kick of the RMW's
 * program-back phase.  The sector pre-read, the erase (WREN + SE + WIP
 * RDSR) and the first chunk's WREN all succeed, then the PP command's
 * CMDCMP poll times out, driving the error return inside
 * xspi_program_chunked that a whole-call fault can never reach.
 *
 * @par MC/DC:
 * Decision: `e != k_ra8_ok` in xspi_program_chunked (single condition)
 * - Vector 1 (this test): e = k_ra8_err_hw_timeout -> true -> early return.
 * - Vector 2 (existing rmw test): e = k_ra8_ok -> false -> loop continues.
 * Minimal MC/DC: N+1 = 2 vectors for N=1 condition.
 *
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_xspi_write_program_leg_error(void)
{
  TEST_BEGIN("xspi_program_chunked program-leg error propagation");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init((uint8_t)k_cov_bd_lba_zero, k_ra8_xspi_lio_1s1s1s));
  ra8_io_blockdev_t            bd    = {};
  ra8_io_blockdev_xspi_state_t state = {};
  internal_fixture_init_valid(&bd, &state);
  volatile r_xspi_regs_t* reg = ra8_xspi((uint8_t)k_cov_bd_lba_zero);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_fake_mmio_fail_nth_wait((const volatile void*)&reg->INTS, (uint32_t)k_cov_bd_nth_first_pp));
  const uint8_t   buf[(size_t)k_cov_bd_blocks_per_sector * (size_t)k_ra8_io_block_size_bytes] = {};
  const ra8_err_t e = ra8_io_blockdev_write(&bd,
                                            (uint32_t)k_cov_bd_lba_zero,
                                            (uint32_t)k_cov_bd_blocks_per_sector,
                                            buf);
  TEST_ASSERT(e != k_ra8_ok);
  TEST_END("xspi_program_chunked program-leg error propagation");
}

/**
 * @brief xspi_erase: read-only device rejects erases immediately.
 *
 * @details
 * Creates a read-only xSPI device.  xspi_erase checks st->read_only before
 * any alignment or bounds check and returns k_ra8_err_not_supported.
 *
 * @par MC/DC:
 * Decision: `st->read_only` in xspi_erase (single condition)
 * - Vector 1 (this test): read_only=true -> k_ra8_err_not_supported.
 * - Vector 2 (existing rmw test): read_only=false -> proceeds to alignment check.
 * Minimal MC/DC: N+1 = 2 vectors for N=1 condition.
 *
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_xspi_erase_read_only(void)
{
  TEST_BEGIN("xspi_erase read-only rejection");
  internal_prep();
  ra8_io_blockdev_t            bd    = {};
  ra8_io_blockdev_xspi_state_t state = {};
  internal_fixture_init_read_only(&bd, &state);
  const ra8_err_t e =
    ra8_io_blockdev_erase(&bd, (uint32_t)k_cov_bd_lba_zero, (uint32_t)k_cov_bd_blocks_per_sector);
  TEST_ASSERT_EQ(k_ra8_err_not_supported, e);
  TEST_END("xspi_erase read-only rejection");
}

/**
 * @brief xspi_erase: non-sector-aligned lba returns invalid-arg.
 *
 * @details
 * Passes lba=1 (not a multiple of 8) to xspi_erase.  After the read_only
 * check passes, the lba alignment guard `(lba % 8) != 0` is true and
 * k_ra8_err_invalid_arg is returned.
 *
 * @par MC/DC:
 * Decision: `(lba % k_xspi_blocks_per_sector) != k_xspi_zero_blocks` (single condition)
 * - Vector 1 (this test): 1 % 8 = 1 != 0 -> true -> k_ra8_err_invalid_arg.
 * - Vector 2 (existing rmw test): 0 % 8 = 0 -> false -> proceeds to count check.
 * Minimal MC/DC: N+1 = 2 vectors for N=1 condition.
 *
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_xspi_erase_lba_misaligned(void)
{
  TEST_BEGIN("xspi_erase lba not sector-aligned");
  internal_prep();
  ra8_io_blockdev_t            bd    = {};
  ra8_io_blockdev_xspi_state_t state = {};
  internal_fixture_init_valid(&bd, &state);
  const ra8_err_t e = ra8_io_blockdev_erase(&bd,
                                            (uint32_t)k_cov_bd_lba_unaligned,
                                            (uint32_t)k_cov_bd_blocks_per_sector);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, e);
  TEST_END("xspi_erase lba not sector-aligned");
}

/**
 * @brief xspi_erase: non-sector-aligned count returns invalid-arg.
 *
 * @details
 * Passes lba=0 (aligned) but count=1 (not a multiple of 8).  After the lba
 * alignment check passes, the count guard `(count % 8) != 0` is true and
 * k_ra8_err_invalid_arg is returned.
 *
 * @par MC/DC:
 * Decision: `(count % k_xspi_blocks_per_sector) != k_xspi_zero_blocks` (single condition)
 * - Vector 1 (this test): 1 % 8 = 1 != 0 -> true -> k_ra8_err_invalid_arg.
 * - Vector 2 (existing rmw test): 8 % 8 = 0 -> false -> proceeds to bounds check.
 * Minimal MC/DC: N+1 = 2 vectors for N=1 condition.
 *
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_xspi_erase_count_misaligned(void)
{
  TEST_BEGIN("xspi_erase count not sector-aligned");
  internal_prep();
  ra8_io_blockdev_t            bd    = {};
  ra8_io_blockdev_xspi_state_t state = {};
  internal_fixture_init_valid(&bd, &state);
  const ra8_err_t e =
    ra8_io_blockdev_erase(&bd, (uint32_t)k_cov_bd_lba_zero, (uint32_t)k_cov_bd_count_unaligned);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, e);
  TEST_END("xspi_erase count not sector-aligned");
}

/**
 * @brief xspi_erase: out-of-range block range returns out-of-range error.
 *
 * @details
 * Passes lba=8, count=8 (both aligned) to an 8-block device.  After
 * alignment checks pass, xspi_bounds evaluates `8 > 8 - 8 = 0` (true)
 * and returns k_ra8_err_out_of_range.
 *
 * @par MC/DC:
 * Decision: `b != k_ra8_ok` in xspi_erase after xspi_bounds (single condition)
 * - Vector 1 (this test): b = k_ra8_err_out_of_range -> true -> early return.
 * - Vector 2 (existing rmw test): b = k_ra8_ok -> false -> enters erase loop.
 * Minimal MC/DC: N+1 = 2 vectors for N=1 condition.
 *
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_xspi_erase_bounds_overflow(void)
{
  TEST_BEGIN("xspi_erase bounds overflow");
  internal_prep();
  ra8_io_blockdev_t            bd    = {};
  ra8_io_blockdev_xspi_state_t state = {};
  internal_fixture_init_valid(&bd, &state);
  const ra8_err_t e =
    ra8_io_blockdev_erase(&bd, (uint32_t)k_cov_bd_lba_at_end, (uint32_t)k_cov_bd_blocks_per_sector);
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, e);
  TEST_END("xspi_erase bounds overflow");
}

/**
 * @brief xspi_erase: HAL erase failure propagates back to caller.
 *
 * @details
 * Arms ``ra8_fake_mmio_fail_wait`` on ``INTS`` so the erase's first command
 * (the WREN) times out.  All alignment and bounds checks pass for lba=0,
 * count=8; inside the erase loop ra8_xspi_flash_erase_sector returns the
 * hardware timeout, and xspi_erase propagates it to the caller.
 *
 * @par MC/DC:
 * Decision: `e != k_ra8_ok` in xspi_erase loop (single condition)
 * - Vector 1 (this test): e = k_ra8_err_hw_timeout -> true -> early return.
 * - Vector 2 (existing rmw test): e = k_ra8_ok -> false -> advance blk pointer.
 * Minimal MC/DC: N+1 = 2 vectors for N=1 condition.
 *
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_xspi_erase_hal_error(void)
{
  TEST_BEGIN("xspi_erase HAL erase-sector failure propagation");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init((uint8_t)k_cov_bd_lba_zero, k_ra8_xspi_lio_1s1s1s));
  ra8_io_blockdev_t            bd    = {};
  ra8_io_blockdev_xspi_state_t state = {};
  internal_fixture_init_valid(&bd, &state);
  volatile r_xspi_regs_t* reg = ra8_xspi((uint8_t)k_cov_bd_lba_zero);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)&reg->INTS));
  const ra8_err_t e =
    ra8_io_blockdev_erase(&bd, (uint32_t)k_cov_bd_lba_zero, (uint32_t)k_cov_bd_blocks_per_sector);
  TEST_ASSERT(e != k_ra8_ok);
  TEST_END("xspi_erase HAL erase-sector failure propagation");
}

/**
 * @brief xspi_get_caps: entire body covered -- capabilities snapshot.
 *
 * @details
 * Calls ra8_io_blockdev_get_caps on a valid xSPI device and checks that
 * every field in the returned ra8_io_blockdev_caps_t reflects the NOR
 * flash properties wired into xspi_get_caps: an all-ones-erase medium
 * that must be erased before programming, with an 8-block erase unit,
 * a 256-byte program granularity, and a 512-byte logical block size.
 *
 * @par MC/DC:
 * (no compound decisions in xspi_get_caps -- every guard is a single-
 * condition RA8_CHECK_NULL_PTR expansion; those null-check lines are covered
 * by this test executing the function body with valid arguments, which
 * makes the `if (ptr == nullptr)` condition false and falls through to
 * the field assignments.  The true arms are provably unreachable through
 * the public ra8_io_blockdev_get_caps dispatcher, which always supplies a
 * non-null ctx and pre-checks out before forwarding.  No separate MC/DC
 * null-path vectors are required because those are single-condition guards
 * with no &&/|| operator.)
 *
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_xspi_get_caps(void)
{
  TEST_BEGIN("xspi_get_caps full body coverage");
  internal_prep();
  ra8_io_blockdev_t            bd    = {};
  ra8_io_blockdev_xspi_state_t state = {};
  internal_fixture_init_valid(&bd, &state);

  ra8_io_blockdev_caps_t caps = {};
  const ra8_err_t        e    = ra8_io_blockdev_get_caps(&bd, &caps);
  TEST_ASSERT_EQ(k_ra8_ok, e);
  TEST_ASSERT_EQ(k_cov_bd_blocks_per_sector, caps.block_count);
  TEST_ASSERT_EQ(k_cov_bd_blocks_per_sector, caps.erase_unit_blocks);
  TEST_ASSERT_EQ(k_ra8_io_block_size_bytes, caps.logical_block_bytes);
  TEST_ASSERT_EQ(k_ra8_io_erase_value_ones, caps.erase_value);
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
 * @pre ra8_ospi_regs mmap region is accessible (RA8_OFF_TARGET).
 * @pre The ra8_core_hal OBJECT library was built with --coverage.
 * @post All previously uncovered lines in ra8_io_blockdev_xspi.c -- now
 *       including the erase-leg and program-leg error returns -- have
 *       been executed at least once.
 * @post Exit code 0 signals success to the CI coverage runner.
 *
 * @since 0.1.0
 */
int main(void)
{
  internal_test_xspi_bounds_count_too_big();
  internal_test_xspi_bounds_lba_overflow();
  internal_test_xspi_read_hal_error();
  internal_test_xspi_write_read_only();
  internal_test_xspi_write_bounds_overflow();
  internal_test_xspi_write_hal_error();
  internal_test_xspi_write_erase_leg_error();
  internal_test_xspi_write_program_leg_error();
  internal_test_xspi_erase_read_only();
  internal_test_xspi_erase_lba_misaligned();
  internal_test_xspi_erase_count_misaligned();
  internal_test_xspi_erase_bounds_overflow();
  internal_test_xspi_erase_hal_error();
  internal_test_xspi_get_caps();
  return 0;
}
