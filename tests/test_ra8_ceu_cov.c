/**
 * @file test_ra8_ceu_cov.c
 * @brief Additional line-coverage tests for ra8_ceu.c.
 *
 * @par Tag
 * [Ring 3 / Test] {World: NS}
 *
 * @details
 * Companion to test_ra8_ceu.c.  Targets the branches that were still
 * uncovered after the primary test suite ran:
 *
 * - Lines 223, 226, 229, 232, 235, 238:
 *   internal_validate_buffers rejection returns for y_bottom, c_bottom,
 *   y_top_2, c_top_2, y_bottom_2, and c_bottom_2 when any of those
 *   pointers is non-null and not 8-byte aligned.
 *
 * - Lines 276, 277, 279, 280, 282, 283, 285, 286:
 *   internal_program_addresses writes to CDAYR2 / CDACR2 / CDBYR2 /
 *   CDBCR2 when the corresponding bundle-2 buffer fields are non-null.
 *
 * - Lines 632-662:
 *   internal_plane_b_apply_overrides non-null branches for c_top,
 *   y_bottom, c_bottom, y_top_2, c_top_2, y_bottom_2, c_bottom_2, and
 *   the bundle_size_bytes non-zero path.
 *
 * - Lines 716, 717:
 *   ra8_ceu_byte_swap_set with swap_16_bit = true (COWS bit).
 *
 * Builds as an independent test executable (auto-discovered by the
 * GLOB in tests/CMakeLists.txt); its gcda file merges with the one
 * from test_ra8_ceu so gcovr sees combined coverage.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_ceu.h"
#include "ra8_ceu_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/* ---------------------------------------------------------------------------
 * Shared typed constants
 * --------------------------------------------------------------------------- */

/**
 * @enum test_cov_ceu_buf_t
 * @brief 8-byte-aligned SDRAM addresses and one misaligned sentinel.
 *
 * @details
 * Addresses lie in the SDRAM range (0x68xxxxxx) that ra8_fake_mmap
 * exposes for the host build.  Every address has its lower 3 bits
 * clear (8-byte aligned) except k_test_cov_unaligned which exercises
 * the rejection path in internal_validate_buffers.
 *
 * @invariant k_test_cov_unaligned != 0 and (k_test_cov_unaligned & 7) != 0.
 *
 * @code
 * uint8_t* p = (uint8_t*)(uintptr_t)k_test_cov_buf_a;
 * @endcode
 *
 * @see ra8_ceu_buffers_t
 * @since 0.1.0
 */
typedef enum : uintptr_t {
  k_test_cov_buf_a     = 0x68000040UL, /**< Aligned buffer A (y_top default).   */
  k_test_cov_buf_b     = 0x68001000UL, /**< Aligned buffer B (c_top / plane B). */
  k_test_cov_buf_c     = 0x68002000UL, /**< Aligned buffer C (y_bottom).        */
  k_test_cov_buf_d     = 0x68003000UL, /**< Aligned buffer D (c_bottom).        */
  k_test_cov_buf_e     = 0x68004000UL, /**< Aligned buffer E (y_top_2).         */
  k_test_cov_buf_f     = 0x68005000UL, /**< Aligned buffer F (c_top_2).         */
  k_test_cov_buf_g     = 0x68006000UL, /**< Aligned buffer G (y_bottom_2).      */
  k_test_cov_buf_h     = 0x68007000UL, /**< Aligned buffer H (c_bottom_2).      */
  k_test_cov_unaligned = 0x68000043UL, /**< Lower 3 bits != 0 (misaligned).     */
} test_cov_ceu_buf_t;

/**
 * @enum test_cov_bundle_sz_t
 * @brief Bundle size used in the plane-B all-overrides test.
 *
 * @details
 * 1024 bytes is already 8-byte aligned, so no bits are masked off by
 * the `bundle_size_bytes & ~k_ra8_ceu_bundle_align_mask` operation in
 * internal_plane_b_apply_overrides.  The register readback therefore
 * equals this constant exactly.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_test_cov_bundle_sz = 1024U, /**< 8-byte-aligned bundle size. */
} test_cov_bundle_sz_t;

/* ---------------------------------------------------------------------------
 * Fixture helper
 * --------------------------------------------------------------------------- */

/**
 * @brief Reset the fake register map and CEU module state.
 *
 * @details
 * Zeroes the ra8_fake_mmap window so no leftover register state from a
 * prior test leaks in, reinitialises the MSTP model to its powered-off
 * state, and detaches any stale CEU event callback registered by a
 * previous test.
 *
 * @pre May be called at any point; no prerequisites.
 * @post ra8_fake_mmap is zeroed; MSTP model is in its default state; any
 *       previously registered CEU callback has been cleared.
 *
 * @note Not thread-safe; used only in the single-threaded test binary.
 * @since 0.1.0
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_ceu_attach_handler(nullptr, nullptr);
}

/* ---------------------------------------------------------------------------
 * Helper: build a minimal valid ra8_ceu_config_t
 * --------------------------------------------------------------------------- */

/**
 * @brief Return a minimal valid CEU configuration for image-capture mode.
 *
 * @details
 * Fills every required field with sensible defaults so that ra8_ceu_init
 * succeeds without further setup.  Tests that need a specific override
 * mutate the returned struct before passing it to ra8_ceu_init.
 *
 * @return ra8_ceu_config_t Populated config struct.
 *
 * @pre None.
 * @post No global state is modified.
 *
 * @note Not thread-safe; used only in single-threaded test binary.
 * @since 0.1.0
 */
static ra8_ceu_config_t make_cfg(void)
{
  const ra8_ceu_config_t cfg = {
    .width_px        = 640U,
    .height_px       = 480U,
    .x_start_px      = 0U,
    .y_start_px      = 0U,
    .x_capture_px    = 640U,
    .y_capture_lines = 480U,
    .dst_stride      = 1280U,
    .frame_drop      = 0U,
    .bytes_per_pixel = 2U,
    .interrupts      = 0U,
    .capture_format  = k_ra8_ceu_fmt_image_capture,
    .capture_mode    = k_ra8_ceu_capture_single,
    .data_bus        = k_ra8_ceu_bus_8_bit,
    .hsync_polarity  = k_ra8_ceu_pol_high_active,
    .vsync_polarity  = k_ra8_ceu_pol_high_active,
    .field_polarity  = k_ra8_ceu_pol_high_active,
    .input_order     = k_ra8_ceu_input_cb0_y0_cr0_y1,
    .output_format   = k_ra8_ceu_output_ycbcr_422,
    .burst_mode      = k_ra8_ceu_burst_64,
    .first_field     = k_ra8_ceu_field_immediate,
    .edge            = {.data  = k_ra8_ceu_edge_rising,
                        .hsync = k_ra8_ceu_edge_rising,
                        .vsync = k_ra8_ceu_edge_rising,
                        .field = k_ra8_ceu_edge_rising},
    .byte_swap       = {.swap_8_bit = false, .swap_16_bit = false, .swap_32_bit = false},
    .scale           = {.h_mantissa    = 0U,
                        .h_fraction    = 0U,
                        .v_mantissa    = 0U,
                        .v_fraction    = 0U,
                        .h_output_clip = 0U,
                        .v_output_clip = 0U},
    .interlace       = false,
    .one_field_only  = false,
    .bundle_write    = false,
    .low_pass_filter = false,
    .image_area_size = 0U,
  };
  return cfg;
}

/* ---------------------------------------------------------------------------
 * Tests: internal_validate_buffers -- per-field misalignment returns
 *
 * internal_validate_buffers checks each non-null pointer in declaration
 * order: y_top -> c_top -> y_bottom -> c_bottom -> y_top_2 -> c_top_2 ->
 * y_bottom_2 -> c_bottom_2.  A null pointer passes the alignment check
 * (internal_is_aligned returns true for nullptr so the caller can leave
 * an unused field unset).  Each test below sets exactly one field to the
 * unaligned address while all preceding fields are null (or aligned), so
 * precisely the one targeted return statement executes.
 *
 * ra8_ceu_plane_b_program is used as the entry point because it delegates
 * directly to internal_validate_buffers without adding an extra non-null
 * guard on y_top (unlike ra8_ceu_capture_start_ex).
 * --------------------------------------------------------------------------- */

/**
 * @brief Cover line 223: y_bottom misaligned rejection.
 *
 * @details
 * y_top and c_top are null (null passes alignment); y_bottom is the
 * unaligned address, so internal_validate_buffers returns at the third
 * check (line 223).
 *
 * @par MC/DC:
 * (single-condition `if (!internal_is_aligned(bufs->y_bottom))` --
 * no compound decision; this test takes the true branch)
 *
 * @pre None.
 * @post No CEU register is modified; validation fails before programming.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_cov_validate_y_bottom_misaligned(void)
{
  TEST_BEGIN("ceu cov: validate_buffers y_bottom misaligned");
  prep();
  const ra8_ceu_buffers_t bufs = {
    .y_top             = nullptr,
    .c_top             = nullptr,
    .y_bottom          = (uint8_t*)(uintptr_t)k_test_cov_unaligned,
    .c_bottom          = nullptr,
    .y_top_2           = nullptr,
    .c_top_2           = nullptr,
    .y_bottom_2        = nullptr,
    .c_bottom_2        = nullptr,
    .bundle_size_bytes = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ceu_plane_b_program(&bufs));
  TEST_END("ceu cov: validate_buffers y_bottom misaligned");
}

/**
 * @brief Cover line 226: c_bottom misaligned rejection.
 *
 * @details
 * y_top, c_top, and y_bottom are all null, passing their alignment
 * checks.  c_bottom is the unaligned address, so the fourth check
 * (line 226) triggers the return.
 *
 * @par MC/DC:
 * (single-condition check; this test takes the true branch for c_bottom)
 *
 * @pre None.
 * @post No CEU register is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_cov_validate_c_bottom_misaligned(void)
{
  TEST_BEGIN("ceu cov: validate_buffers c_bottom misaligned");
  prep();
  const ra8_ceu_buffers_t bufs = {
    .y_top             = nullptr,
    .c_top             = nullptr,
    .y_bottom          = nullptr,
    .c_bottom          = (uint8_t*)(uintptr_t)k_test_cov_unaligned,
    .y_top_2           = nullptr,
    .c_top_2           = nullptr,
    .y_bottom_2        = nullptr,
    .c_bottom_2        = nullptr,
    .bundle_size_bytes = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ceu_plane_b_program(&bufs));
  TEST_END("ceu cov: validate_buffers c_bottom misaligned");
}

/**
 * @brief Cover line 229: y_top_2 misaligned rejection.
 *
 * @details
 * All bundle-1 fields are null; y_top_2 is the unaligned address.
 * The fifth sequential check (line 229) causes the return.
 *
 * @par MC/DC:
 * (single-condition check; true branch for y_top_2 misalignment)
 *
 * @pre None.
 * @post No CEU register is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_cov_validate_y_top_2_misaligned(void)
{
  TEST_BEGIN("ceu cov: validate_buffers y_top_2 misaligned");
  prep();
  const ra8_ceu_buffers_t bufs = {
    .y_top             = nullptr,
    .c_top             = nullptr,
    .y_bottom          = nullptr,
    .c_bottom          = nullptr,
    .y_top_2           = (uint8_t*)(uintptr_t)k_test_cov_unaligned,
    .c_top_2           = nullptr,
    .y_bottom_2        = nullptr,
    .c_bottom_2        = nullptr,
    .bundle_size_bytes = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ceu_plane_b_program(&bufs));
  TEST_END("ceu cov: validate_buffers y_top_2 misaligned");
}

/**
 * @brief Cover line 232: c_top_2 misaligned rejection.
 *
 * @details
 * y_top_2 is a valid aligned pointer so its check passes; c_top_2 is
 * the unaligned address triggering the sixth return (line 232).
 *
 * @par MC/DC:
 * (single-condition check; true branch for c_top_2 misalignment)
 *
 * @pre None.
 * @post No CEU register is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_cov_validate_c_top_2_misaligned(void)
{
  TEST_BEGIN("ceu cov: validate_buffers c_top_2 misaligned");
  prep();
  const ra8_ceu_buffers_t bufs = {
    .y_top             = nullptr,
    .c_top             = nullptr,
    .y_bottom          = nullptr,
    .c_bottom          = nullptr,
    .y_top_2           = (uint8_t*)(uintptr_t)k_test_cov_buf_e,
    .c_top_2           = (uint8_t*)(uintptr_t)k_test_cov_unaligned,
    .y_bottom_2        = nullptr,
    .c_bottom_2        = nullptr,
    .bundle_size_bytes = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ceu_plane_b_program(&bufs));
  TEST_END("ceu cov: validate_buffers c_top_2 misaligned");
}

/**
 * @brief Cover line 235: y_bottom_2 misaligned rejection.
 *
 * @details
 * y_top_2 and c_top_2 are aligned; y_bottom_2 is unaligned, triggering
 * the seventh sequential check (line 235).
 *
 * @par MC/DC:
 * (single-condition check; true branch for y_bottom_2 misalignment)
 *
 * @pre None.
 * @post No CEU register is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_cov_validate_y_bottom_2_misaligned(void)
{
  TEST_BEGIN("ceu cov: validate_buffers y_bottom_2 misaligned");
  prep();
  const ra8_ceu_buffers_t bufs = {
    .y_top             = nullptr,
    .c_top             = nullptr,
    .y_bottom          = nullptr,
    .c_bottom          = nullptr,
    .y_top_2           = (uint8_t*)(uintptr_t)k_test_cov_buf_e,
    .c_top_2           = (uint8_t*)(uintptr_t)k_test_cov_buf_f,
    .y_bottom_2        = (uint8_t*)(uintptr_t)k_test_cov_unaligned,
    .c_bottom_2        = nullptr,
    .bundle_size_bytes = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ceu_plane_b_program(&bufs));
  TEST_END("ceu cov: validate_buffers y_bottom_2 misaligned");
}

/**
 * @brief Cover line 238: c_bottom_2 misaligned rejection.
 *
 * @details
 * All six preceding fields pass their alignment checks; c_bottom_2 is
 * the unaligned address, triggering the final (eighth) check (line 238).
 *
 * @par MC/DC:
 * (single-condition check; true branch for c_bottom_2 misalignment)
 *
 * @pre None.
 * @post No CEU register is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_cov_validate_c_bottom_2_misaligned(void)
{
  TEST_BEGIN("ceu cov: validate_buffers c_bottom_2 misaligned");
  prep();
  const ra8_ceu_buffers_t bufs = {
    .y_top             = nullptr,
    .c_top             = nullptr,
    .y_bottom          = nullptr,
    .c_bottom          = nullptr,
    .y_top_2           = (uint8_t*)(uintptr_t)k_test_cov_buf_e,
    .c_top_2           = (uint8_t*)(uintptr_t)k_test_cov_buf_f,
    .y_bottom_2        = (uint8_t*)(uintptr_t)k_test_cov_buf_g,
    .c_bottom_2        = (uint8_t*)(uintptr_t)k_test_cov_unaligned,
    .bundle_size_bytes = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ceu_plane_b_program(&bufs));
  TEST_END("ceu cov: validate_buffers c_bottom_2 misaligned");
}

/* ---------------------------------------------------------------------------
 * Test: internal_program_addresses -- bundle-2 field writes (lines 276-286)
 * --------------------------------------------------------------------------- */

/**
 * @brief Cover lines 276-286: bundle-2 register writes in internal_program_addresses.
 *
 * @details
 * ra8_ceu_capture_start_ex with all eight buffer fields set to distinct
 * aligned addresses causes internal_program_addresses to take every
 * non-null branch, including the four bundle-2 writes:
 *
 *   line 276-277  CDAYR2 = y_top_2
 *   line 279-280  CDACR2 = c_top_2
 *   line 282-283  CDBYR2 = y_bottom_2
 *   line 285-286  CDBCR2 = c_bottom_2
 *
 * The test reads back each register to confirm the correct address was
 * written.
 *
 * @par MC/DC:
 * (no compound decisions; each `if (field != nullptr)` is a single-
 * condition check independently varied by setting all fields non-null)
 *
 * @pre ra8_ceu_init has been called so the MSTP model is live.
 * @post CDAYR2/CDACR2/CDBYR2/CDBCR2 hold the supplied bundle-2 addresses.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_cov_program_addresses_bundle2(void)
{
  TEST_BEGIN("ceu cov: program_addresses bundle-2 writes");
  prep();
  const ra8_ceu_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&cfg));

  const ra8_ceu_buffers_t bufs = {
    .y_top             = (uint8_t*)(uintptr_t)k_test_cov_buf_a,
    .c_top             = (uint8_t*)(uintptr_t)k_test_cov_buf_b,
    .y_bottom          = (uint8_t*)(uintptr_t)k_test_cov_buf_c,
    .c_bottom          = (uint8_t*)(uintptr_t)k_test_cov_buf_d,
    .y_top_2           = (uint8_t*)(uintptr_t)k_test_cov_buf_e,
    .c_top_2           = (uint8_t*)(uintptr_t)k_test_cov_buf_f,
    .y_bottom_2        = (uint8_t*)(uintptr_t)k_test_cov_buf_g,
    .c_bottom_2        = (uint8_t*)(uintptr_t)k_test_cov_buf_h,
    .bundle_size_bytes = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_capture_start_ex(&bufs));

  /* Confirm bundle-2 registers received the correct addresses. */
  TEST_ASSERT_EQ(k_test_cov_buf_e, *ra8_ceu_reg32(k_ra8_ceu_off_cdayr2));
  TEST_ASSERT_EQ(k_test_cov_buf_f, *ra8_ceu_reg32(k_ra8_ceu_off_cdacr2));
  TEST_ASSERT_EQ(k_test_cov_buf_g, *ra8_ceu_reg32(k_ra8_ceu_off_cdbyr2));
  TEST_ASSERT_EQ(k_test_cov_buf_h, *ra8_ceu_reg32(k_ra8_ceu_off_cdbcr2));

  TEST_END("ceu cov: program_addresses bundle-2 writes");
}

/* ---------------------------------------------------------------------------
 * Test: internal_plane_b_apply_overrides -- all non-null paths (lines 632-662)
 * --------------------------------------------------------------------------- */

/**
 * @brief Cover lines 632-662: all override branches in internal_plane_b_apply_overrides.
 *
 * @details
 * ra8_ceu_plane_b_program with every buffer field non-null and
 * bundle_size_bytes non-zero causes internal_plane_b_apply_overrides
 * to take all eight conditional write branches and the bundle branch:
 *
 *   lines 632-634  c_top    -> CDACR (Plane B)
 *   lines 636-638  y_bottom -> CDBYR (Plane B)
 *   lines 640-642  c_bottom -> CDBCR (Plane B)
 *   lines 644-646  y_top_2  -> CDAYR2 (Plane B)
 *   lines 648-650  c_top_2  -> CDACR2 (Plane B)
 *   lines 652-654  y_bottom_2 -> CDBYR2 (Plane B)
 *   lines 656-658  c_bottom_2 -> CDBCR2 (Plane B)
 *   lines 660-662  bundle_size_bytes -> CBDSR (Plane B)
 *
 * The Plane-A mirror step in internal_plane_b_mirror_from_a runs first
 * and zeroes Plane B (mmap is already clear from prep()), then each
 * override writes a distinct address which the test reads back.
 *
 * @par MC/DC:
 * (no compound decisions; each `if` in apply_overrides is a single
 * pointer-null or uint32-zero check, independently varied here by
 * supplying all non-null fields simultaneously)
 *
 * @pre None; called immediately after prep().
 * @post Plane-B shadow registers reflect all supplied overrides.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_cov_plane_b_all_overrides(void)
{
  TEST_BEGIN("ceu cov: plane_b_apply_overrides all fields non-null");
  prep();

  const ra8_ceu_buffers_t bufs = {
    .y_top             = (uint8_t*)(uintptr_t)k_test_cov_buf_a,
    .c_top             = (uint8_t*)(uintptr_t)k_test_cov_buf_b,
    .y_bottom          = (uint8_t*)(uintptr_t)k_test_cov_buf_c,
    .c_bottom          = (uint8_t*)(uintptr_t)k_test_cov_buf_d,
    .y_top_2           = (uint8_t*)(uintptr_t)k_test_cov_buf_e,
    .c_top_2           = (uint8_t*)(uintptr_t)k_test_cov_buf_f,
    .y_bottom_2        = (uint8_t*)(uintptr_t)k_test_cov_buf_g,
    .c_bottom_2        = (uint8_t*)(uintptr_t)k_test_cov_buf_h,
    .bundle_size_bytes = (uint32_t)k_test_cov_bundle_sz,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_plane_b_program(&bufs));

  /* Verify every Plane-B override was applied. */
  TEST_ASSERT_EQ(k_test_cov_buf_a,
                 *ra8_ceu_reg32_plane(k_ra8_ceu_off_cdayr, k_ra8_ceu_plane_b_off));
  TEST_ASSERT_EQ(k_test_cov_buf_b,
                 *ra8_ceu_reg32_plane(k_ra8_ceu_off_cdacr, k_ra8_ceu_plane_b_off));
  TEST_ASSERT_EQ(k_test_cov_buf_c,
                 *ra8_ceu_reg32_plane(k_ra8_ceu_off_cdbyr, k_ra8_ceu_plane_b_off));
  TEST_ASSERT_EQ(k_test_cov_buf_d,
                 *ra8_ceu_reg32_plane(k_ra8_ceu_off_cdbcr, k_ra8_ceu_plane_b_off));
  TEST_ASSERT_EQ(k_test_cov_buf_e,
                 *ra8_ceu_reg32_plane(k_ra8_ceu_off_cdayr2, k_ra8_ceu_plane_b_off));
  TEST_ASSERT_EQ(k_test_cov_buf_f,
                 *ra8_ceu_reg32_plane(k_ra8_ceu_off_cdacr2, k_ra8_ceu_plane_b_off));
  TEST_ASSERT_EQ(k_test_cov_buf_g,
                 *ra8_ceu_reg32_plane(k_ra8_ceu_off_cdbyr2, k_ra8_ceu_plane_b_off));
  TEST_ASSERT_EQ(k_test_cov_buf_h,
                 *ra8_ceu_reg32_plane(k_ra8_ceu_off_cdbcr2, k_ra8_ceu_plane_b_off));
  /* Bundle size: 1024 is already 8-byte aligned so the mask leaves it unchanged. */
  TEST_ASSERT_EQ(k_test_cov_bundle_sz,
                 *ra8_ceu_reg32_plane(k_ra8_ceu_off_cbdsr, k_ra8_ceu_plane_b_off));

  TEST_END("ceu cov: plane_b_apply_overrides all fields non-null");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_ceu_byte_swap_set -- swap_16_bit true path (lines 716-717)
 * --------------------------------------------------------------------------- */

/**
 * @brief Cover lines 716-717: ra8_ceu_byte_swap_set with swap_16_bit true.
 *
 * @details
 * The sibling test_ra8_ceu.c exercises ra8_ceu_byte_swap_set with
 * swap_16_bit = false, leaving the true branch (the `current |= cows`
 * statement at lines 716-717) uncovered.  This test supplies
 * swap_16_bit = true, causing the COWS bit to be set in CDOCR, and
 * confirms the other swap bits are untouched.
 *
 * @par MC/DC:
 * (single-condition `if (swap->swap_16_bit)` -- no compound decision;
 * the false branch is covered by test_ra8_ceu.c, the true branch here)
 *
 * @pre None.
 * @post CDOCR has only the COWS bit set; COBS and COLS remain clear.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_cov_byte_swap_16_bit(void)
{
  TEST_BEGIN("ceu cov: byte_swap_set swap_16_bit true");
  prep();
  *ra8_ceu_reg32(k_ra8_ceu_off_cdocr) = 0U;
  const ra8_ceu_byte_swap_t sw        = {
    .swap_8_bit  = false,
    .swap_16_bit = true,
    .swap_32_bit = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_byte_swap_set(&sw));
  const uint32_t cdocr = *ra8_ceu_reg32(k_ra8_ceu_off_cdocr);
  /* COWS set -- lines 716-717 covered. */
  TEST_ASSERT((cdocr & (uint32_t)k_ra8_ceu_cdocr_mask_cows) != 0U);
  /* COBS and COLS must remain clear. */
  TEST_ASSERT((cdocr & (uint32_t)k_ra8_ceu_cdocr_mask_cobs) == 0U);
  TEST_ASSERT((cdocr & (uint32_t)k_ra8_ceu_cdocr_mask_cols) == 0U);
  TEST_END("ceu cov: byte_swap_set swap_16_bit true");
}

/* ---------------------------------------------------------------------------
 * Entry point
 * --------------------------------------------------------------------------- */

/**
 * @brief Test binary entry point.
 *
 * @details
 * Runs each coverage-boost test in declaration order and prints a final
 * OK line to stderr.  Any assertion failure calls exit(1) immediately
 * via the TEST_ASSERT macros, so this function returns 0 only if every
 * test passes.
 *
 * @return int32_t 0 on success (never returns on failure).
 *
 * @pre Linked against ra8_core_hal OBJECT library with RA8_OFF_TARGET
 *      defined at compile time.
 * @post All targeted uncovered source lines have been executed.
 *
 * @note Not thread-safe; runs as a single-threaded process.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_cov_validate_y_bottom_misaligned();
  test_cov_validate_c_bottom_misaligned();
  test_cov_validate_y_top_2_misaligned();
  test_cov_validate_c_top_2_misaligned();
  test_cov_validate_y_bottom_2_misaligned();
  test_cov_validate_c_bottom_2_misaligned();
  test_cov_program_addresses_bundle2();
  test_cov_plane_b_all_overrides();
  test_cov_byte_swap_16_bit();
  return 0;
}
