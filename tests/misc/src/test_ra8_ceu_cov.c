/**
 * @file test_ra8_ceu_cov.c
 * @brief Additional line-coverage tests for ra8_ceu.c.
 *
 * @par Tag
 * [Ring 3 / Test] {World: NS}
 *
 * @details
 * Companion to test_ra8_ceu.c, covering what the primary suite leaves
 * untouched: the per-field rejection returns of internal_validate_buffers,
 * the bundle-2 register writes of internal_program_addresses, every
 * override branch of internal_plane_b_apply_overrides, and the COWS leg
 * of ra8_ceu_byte_swap_set.
 *
 * It also covers libs/ra8_camera/src/ra8_camera_source_ceu.c, the CEU
 * capture backend that sits directly on top of this driver: argument
 * and geometry rejection, the arm-when-busy and bounded-poll-expiry
 * legs, the software-reset failure legs, the capture-end byte-count
 * rules, and the completed frame view.
 *
 * Builds as an independent test executable (auto-discovered by the
 * GLOB in tests/CMakeLists.txt); its gcda file merges with the one
 * from test_ra8_ceu so gcovr sees combined coverage.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_camera.h"
#include "ra8_camera_internal.h"
#include "ra8_camera_source_ceu.h"
#include "ra8_ceu.h"
#include "ra8_ceu_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/* Modelled CEU peripheral for the camera-source white-box copy. */

/** @enum test_cam_model_t @brief Sentinel for "no modelled capture-end is armed". @details The modelled completion poll index is compared with `>=`, so a disarmed model must sit above every reachable poll index; the backend's poll budget is a uint32_t, hence UINT32_MAX. @invariant No armed poll index ever equals this value. @see ra8_fake_mmio_satisfy_after @since Version 0.1.0 */
typedef enum : uint32_t {
  k_cam_model_disarmed = 0xFFFFFFFFUL, /**< Model is transparent. */
} test_cam_model_t;

/** @brief Zero-based status poll on which the modelled CEU latches CPE. */
static uint32_t s_cam_model_complete_at = (uint32_t)k_cam_model_disarmed;

/** @brief CDSSR byte count the modelled CEU reports alongside CPE. */
static uint32_t s_cam_model_data_size;

/** @brief Zero-based index of the next modelled status poll. */
static uint32_t s_cam_model_poll;

/** @brief Error the modelled status read fails with, or `k_ra8_ok` when healthy. */
static ra8_err_t s_cam_model_status_err = k_ra8_ok;

/**
 * @brief Model the CEU status one backend poll observes.
 *
 * @details Delegates to the real `ra8_ceu_status_snapshot`, then -- only while
 * armed -- overlays the capture-end flag and CDSSR byte count the peripheral
 * would latch. The overlay is required because `ra8_ceu_capture_start_ex`
 * clears CETCR as it arms the engine, so no value staged in the RAM-backed
 * register window can survive into the completion poll that follows it. While
 * disarmed the shim is fully transparent, exactly like an unarmed
 * `ra8_fake_mmio` register.
 *
 * @param[out] out Receives the observed CEU status snapshot.
 * @return ra8_err_t Error code forwarded from the real snapshot.
 * @retval k_ra8_ok A status snapshot was produced.
 * @retval k_ra8_err_null_ptr @p out was `nullptr`.
 * @pre The fake register window is mapped.
 * @pre @p out addresses writable storage.
 * @post The modelled poll counter advanced by exactly one.
 * @post No CEU register was modified by the overlay.
 * @note Test-only and not thread-safe; the suite is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cam_model_snapshot(ra8_ceu_status_t* out)
{
  if (s_cam_model_status_err != k_ra8_ok) {
    return s_cam_model_status_err;
  }
  const ra8_err_t err = ra8_ceu_status_snapshot(out);
  if (err != k_ra8_ok) {
    return err;
  }
  if (s_cam_model_poll >= s_cam_model_complete_at) {
    out->events |= (uint32_t)k_ra8_ceu_evt_cpe;
    out->data_size = s_cam_model_data_size;
  }
  s_cam_model_poll += 1U;
  return k_ra8_ok;
}

/* White-box copy of the CEU camera source. Its two public symbols are renamed
 * so they stay distinct from the production copy linked through ra8_core_hal,
 * which the library-instance case at the end of this file still drives, and its
 * status poll is routed through the peripheral model above. */
// NOLINTBEGIN(readability-identifier-naming) -- external ABI or interposition seam fixes these symbol spellings.
/** @brief Rename the CEU source initializer in the white-box copy. */
#define ra8_camera_source_ceu_init ra8_camera_source_ceu_init_cov
/** @brief Rename the CEU event accessor in the white-box copy. */
#define ra8_camera_source_ceu_get_last_events ra8_camera_source_ceu_get_last_events_cov
/** @brief Route the copy's completion polls through the peripheral model. */
#define ra8_ceu_status_snapshot internal_cam_model_snapshot
// NOLINTEND(readability-identifier-naming)
#include "ra8_camera_source_ceu.c" // NOLINT(bugprone-suspicious-include) -- white-box copy
#undef ra8_camera_source_ceu_init
#undef ra8_camera_source_ceu_get_last_events
#undef ra8_ceu_status_snapshot

/** @enum test_cov_ceu_buf_t @brief SDRAM-range CEU buffer addresses plus one misaligned sentinel. @details Every address lies in the 0x68xxxxxx window ra8_fake_mmap exposes and has its lower three bits clear except k_test_cov_unaligned, which drives the rejection path in internal_validate_buffers. @invariant (k_test_cov_unaligned & 7) != 0. @see ra8_ceu_buffers_t @since Version 0.1.0 */
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

/** @enum test_cov_bundle_sz_t @brief Bundle size used in the plane-B all-overrides case. @details 1024 is already 8-byte aligned, so the bundle_size_bytes alignment mask removes no bits and the register readback equals this constant exactly. @invariant (k_test_cov_bundle_sz & 7) == 0. @see ra8_ceu_plane_b_program @since Version 0.1.0 */
typedef enum : uint32_t {
  k_test_cov_bundle_sz = 1024U, /**< 8-byte-aligned bundle size. */
} test_cov_bundle_sz_t;

/** @brief Reset the fake register map, the MMIO fault seam and CEU module state. @details Zeroes the ra8_fake_mmap window, disarms every armed MMIO wait, reinitialises the MSTP model and detaches any stale CEU event callback. @pre May be called at any point; no prerequisites. @pre No CEU operation is in flight. @post Every register in the fake window reads zero. @post No MMIO wait is armed and no CEU callback is attached. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
static void prep(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  (void)ra8_mstp_init();
  (void)ra8_ceu_attach_handler(nullptr, nullptr);
}

/** @brief Return a minimal valid CEU configuration for image-capture mode. @details Fills every required field so ra8_ceu_init succeeds without further setup; a case needing an override mutates the returned struct. @return ra8_ceu_config_t Populated configuration. @pre None. @pre The caller mutates only the field its case is about. @post No global state is modified. @post The result is accepted by ra8_ceu_init. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
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

/* internal_validate_buffers -- per-field misalignment returns. It checks each
 * non-null pointer in declaration order and a null pointer passes, so each case
 * below misaligns exactly one field and leaves the preceding ones null or
 * aligned. ra8_ceu_plane_b_program is the entry point because it delegates
 * straight to validation without the extra y_top guard capture_start_ex adds. */

/** @brief Reject a misaligned y_bottom buffer. @details y_top and c_top are null, which passes the alignment rule, so the third sequential check is the one that returns. @par MC/DC: Single-condition alignment check; this case takes its true branch. @pre None. @pre The plane-B entry point delegates straight to buffer validation. @post No CEU register is modified. @post Validation fails before any address is programmed. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
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

/** @brief Reject a misaligned c_bottom buffer. @details The three preceding fields are null and pass their checks, so the fourth sequential check is the one that returns. @par MC/DC: Single-condition alignment check; this case takes its true branch. @pre None. @pre The plane-B entry point delegates straight to buffer validation. @post No CEU register is modified. @post Validation fails before any address is programmed. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
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

/** @brief Reject a misaligned y_top_2 buffer. @details Every bundle-1 field is null, so the fifth sequential check is the one that returns. @par MC/DC: Single-condition alignment check; this case takes its true branch. @pre None. @pre The plane-B entry point delegates straight to buffer validation. @post No CEU register is modified. @post Validation fails before any address is programmed. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
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

/** @brief Reject a misaligned c_top_2 buffer. @details y_top_2 is a valid aligned address so its check passes, leaving the sixth sequential check to return. @par MC/DC: Single-condition alignment check; this case takes its true branch. @pre None. @pre The plane-B entry point delegates straight to buffer validation. @post No CEU register is modified. @post Validation fails before any address is programmed. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
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

/** @brief Reject a misaligned y_bottom_2 buffer. @details y_top_2 and c_top_2 are aligned, leaving the seventh sequential check to return. @par MC/DC: Single-condition alignment check; this case takes its true branch. @pre None. @pre The plane-B entry point delegates straight to buffer validation. @post No CEU register is modified. @post Validation fails before any address is programmed. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
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

/** @brief Reject a misaligned c_bottom_2 buffer. @details All six preceding fields pass their checks, so the eighth and final check is the one that returns. @par MC/DC: Single-condition alignment check; this case takes its true branch. @pre None. @pre The plane-B entry point delegates straight to buffer validation. @post No CEU register is modified. @post Validation fails before any address is programmed. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
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

/* internal_program_addresses -- the bundle-2 destination register writes. */

/** @brief Program every bundle-2 destination register. @details Supplying all eight buffer fields drives each non-null branch of the address programmer, including the CDAYR2, CDACR2, CDBYR2 and CDBCR2 writes, which the case reads back individually. @par MC/DC: Eight independent single-condition null checks, each taken true here. @pre The CEU module-stop gate is open. @pre Every supplied address is eight-byte aligned. @post The four bundle-2 registers hold the supplied addresses. @post The capture engine is armed. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
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

/* internal_plane_b_apply_overrides -- every non-null override path. */

/** @brief Apply every plane-B override. @details Plane B is first mirrored from plane A, then each supplied buffer field and the non-zero bundle size override their mirrored value; the case reads all nine back. @par MC/DC: Nine independent single-condition checks, each taken true here. @pre The fake register window is zeroed. @pre The bundle size is already eight-byte aligned. @post Every plane-B shadow register holds its override. @post CBDSR equals the supplied bundle size exactly. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
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

/* ra8_ceu_byte_swap_set -- the swap_16_bit true path (COWS). */

/** @brief Set the 16-bit byte-swap control bit. @details The sibling suite covers swap_16_bit false, leaving the COWS assignment untested; this case supplies true and confirms the neighbouring COBS and COLS bits stay clear. @par MC/DC: Single-condition swap_16_bit check; the false branch lives in the sibling suite, the true branch here. @pre The fake register window is zeroed. @pre CDOCR starts at zero. @post CDOCR has COWS set. @post COBS and COLS remain clear. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
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

/* ra8_camera_source_ceu.c -- the CEU capture backend on top of this driver. */

/**
 * @enum test_cam_fixture_t
 * @brief Storage bounds, byte counts and poll limits for the CEU-source cases.
 *
 * @details The uncompressed fixture is a 16x16 UYVY frame: 32 bytes per row
 *          times 16 rows is exactly `k_cam_frame_bytes`, so a completed frame
 *          satisfies `ra8_camera_frame_validate` without slack.
 * @invariant k_cam_frame_bytes == k_cam_stride * k_cam_height.
 * @invariant k_cam_oversize_bytes > k_cam_frame_bytes.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cam_frame_bytes    = 512U,         /**< Capture bound of the UYVY fixture.      */
  k_cam_stride         = 32U,          /**< Packed UYVY row stride in bytes.        */
  k_cam_jpeg_bytes     = 256U,         /**< JPEG capture bound and CEU image area.  */
  k_cam_data_size      = 200U,         /**< CDSSR count a data-enable frame writes. */
  k_cam_oversize_bytes = 4096U,        /**< CDSSR count larger than the buffer.     */
  k_cam_poll_ms        = 1U,           /**< Completion poll interval.               */
  k_cam_poll_attempts  = 3U,           /**< Bounded completion poll budget.         */
  k_cam_first_poll     = 0U,           /**< Poll index the model completes on.      */
  k_cam_poison_events  = 0xA5A5A5A5UL, /**< Poison in an event out-param.           */
} test_cam_fixture_t;

/** @enum test_cam_dim_t @brief Pixel geometry of the uncompressed capture fixture. @details Width is even so the UYVY row-size rule accepts it. @invariant (k_cam_width % 2) == 0. @see ra8_camera_frame_validate @since Version 0.1.0 */
typedef enum : uint16_t {
  k_cam_width  = 16U, /**< Fixture frame width in pixels.  */
  k_cam_height = 16U, /**< Fixture frame height in pixels. */
} test_cam_dim_t;

/** @brief Reset the register window, the MMIO fault seam and the CEU model. @details Wraps prep() so a camera case can never inherit an armed completion, a poll counter or an injected status error from the case before it. @pre May be called at any point; no prerequisites. @pre No CEU operation is in flight. @post The modelled CEU is disarmed and its poll counter is zero. @post Every register in the fake window reads zero. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
RA8_INTERNAL static void internal_cam_prep(void)
{
  prep();
  s_cam_model_complete_at = (uint32_t)k_cam_model_disarmed;
  s_cam_model_data_size   = 0U;
  s_cam_model_poll        = 0U;
  s_cam_model_status_err  = k_ra8_ok;
}

/** @brief Build an accepted CEU camera-source configuration. @details Pairs the local CEU descriptor with 16x16 UYVY output metadata and a short bounded poll budget so a timeout case finishes immediately. @return ra8_camera_source_ceu_cfg_t Populated configuration. @pre None. @pre The caller mutates only the field its case is about. @post No global state is modified. @post The result passes every ra8_camera_source_ceu_init guard. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
RA8_INTERNAL static ra8_camera_source_ceu_cfg_t internal_cam_make_cfg(void)
{
  const ra8_camera_source_ceu_cfg_t cfg = {
    .ceu              = make_cfg(),
    .output           = {.frame_bytes_max = (uint32_t)k_cam_frame_bytes,
                         .stride_bytes    = (uint32_t)k_cam_stride,
                         .width           = (uint16_t)k_cam_width,
                         .height          = (uint16_t)k_cam_height,
                         .format          = k_ra8_camera_format_uyvy422},
    .poll_interval_ms = (uint32_t)k_cam_poll_ms,
    .poll_attempts    = (uint32_t)k_cam_poll_attempts,
  };
  return cfg;
}

/**
 * @brief Build a hand-made backend state that no public configuration allows.
 * @details The byte-count rejections downstream of a completed capture need a
 *          `frame_bytes_max` or capture format that `ra8_camera_source_ceu_init`
 *          rejects up front, so those cases assemble the state directly.
 * @param[in] frame_bytes_max Capture bound the state advertises.
 * @param[in] format CEU capture format the state records.
 * @return ra8_camera_source_ceu_state_t Initialized backend state.
 * @pre The CEU has been initialized by `ra8_ceu_init`.
 * @pre The caller supplies the buffer the capture writes into.
 * @post The returned state reports itself initialized.
 * @post No global state is modified.
 * @note Not thread-safe; single-threaded test binary only.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_camera_source_ceu_state_t
internal_cam_bare_state(uint32_t frame_bytes_max, ra8_ceu_capture_format_t format)
{
  const ra8_camera_source_ceu_state_t state = {
    .info             = {.frame_bytes_max = frame_bytes_max},
    .capture_format   = format,
    .poll_interval_ms = (uint32_t)k_cam_poll_ms,
    .poll_attempts    = (uint32_t)k_cam_poll_attempts,
    .last_events      = 0U,
    .initialized      = true,
  };
  return state;
}

/** @brief Check the null-argument guards and every zero-valued bound. @details Binds the source once so the frame_bytes_max rejection has a live handle to tear down, then walks width/height/poll_interval_ms/poll_attempts. @param[in,out] source Source handle rebuilt by each call. @param[in,out] state Backend state rebuilt by each call. @param[in,out] cfg Configuration rebuilt from internal_cam_make_cfg before each vector. @pre The fake register window is available. @pre No CEU capture is in flight. @post No rejected configuration leaves the source handle bound. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
RA8_INTERNAL static void
internal_test_cam_init_rejects_null_and_bounds(ra8_camera_source_t*           source,
                                               ra8_camera_source_ceu_state_t* state,
                                               ra8_camera_source_ceu_cfg_t*   cfg)
{
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_source_ceu_init_cov(nullptr, state, cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_source_ceu_init_cov(source, nullptr, cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_source_ceu_init_cov(source, state, nullptr));

  /* Bind first, so the next rejection has a live handle to tear down. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_ceu_init_cov(source, state, cfg));
  TEST_ASSERT_NOT_NULL(source->iface);
  *cfg                        = internal_cam_make_cfg();
  cfg->output.frame_bytes_max = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_camera_source_ceu_init_cov(source, state, cfg));
  TEST_ASSERT_NULL(source->iface);
  TEST_ASSERT(!state->initialized);

  *cfg              = internal_cam_make_cfg();
  cfg->output.width = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_camera_source_ceu_init_cov(source, state, cfg));

  *cfg               = internal_cam_make_cfg();
  cfg->output.height = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_camera_source_ceu_init_cov(source, state, cfg));

  *cfg                  = internal_cam_make_cfg();
  cfg->poll_interval_ms = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_camera_source_ceu_init_cov(source, state, cfg));

  *cfg               = internal_cam_make_cfg();
  cfg->poll_attempts = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_camera_source_ceu_init_cov(source, state, cfg));
}

/** @brief Check both halves of the format-pairing rule and the JPEG-only rules. @details Supplies MC/DC vectors 2 and 3 of the format-pairing decision now enclosed by internal_ceu_cfg_valid, then the row-stride and image-area JPEG-only rejections. @param[in,out] source Source handle rebuilt by each call. @param[in,out] state Backend state rebuilt by each call. @param[in,out] cfg Configuration rebuilt from internal_cam_make_cfg before each vector. @pre The fake register window is available. @pre No CEU capture is in flight. @post No rejected configuration leaves the source handle bound. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
RA8_INTERNAL static void
internal_test_cam_init_rejects_format_pairing(ra8_camera_source_t*           source,
                                              ra8_camera_source_ceu_state_t* state,
                                              ra8_camera_source_ceu_cfg_t*   cfg)
{
  /* MC/DC vector 2: data-enable capture paired with an uncompressed output. */
  *cfg                     = internal_cam_make_cfg();
  cfg->ceu.capture_format  = k_ra8_ceu_fmt_data_enable;
  cfg->ceu.image_area_size = (uint32_t)k_cam_frame_bytes;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_camera_source_ceu_init_cov(source, state, cfg));

  /* MC/DC vector 3: JPEG output paired with a fixed-frame capture format. */
  *cfg               = internal_cam_make_cfg();
  cfg->output.format = k_ra8_camera_format_jpeg;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_camera_source_ceu_init_cov(source, state, cfg));

  /* JPEG is a byte stream: a non-zero row stride is a contradiction. */
  *cfg                        = internal_cam_make_cfg();
  cfg->output.format          = k_ra8_camera_format_jpeg;
  cfg->output.frame_bytes_max = (uint32_t)k_cam_jpeg_bytes;
  cfg->ceu.capture_format     = k_ra8_ceu_fmt_data_enable;
  cfg->ceu.image_area_size    = (uint32_t)k_cam_jpeg_bytes;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_camera_source_ceu_init_cov(source, state, cfg));

  /* The CEU firewall bound must equal the advertised capture bound. */
  cfg->output.stride_bytes = 0U;
  cfg->ceu.image_area_size = (uint32_t)k_cam_jpeg_bytes + 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_camera_source_ceu_init_cov(source, state, cfg));
}

/**
 * @brief Reject every configuration the CEU camera source refuses to bind.
 *
 * @details Walks the null-argument guards, each zero-valued geometry and poll
 * bound, both halves of the format-pairing rule, the two JPEG-only rules, and a
 * `ra8_ceu_init` failure forced through the MMIO fault seam. Every rejection
 * must also leave the handle unbound, which is what stops a later capture from
 * dispatching into half-configured state.
 *
 * @par MC/DC:
 * Decision: `(cfg->ceu.capture_format == k_ra8_ceu_fmt_data_enable) !=
 *            (cfg->output.format == k_ra8_camera_format_jpeg)`
 * (2 conditions, libs/ra8_camera/src/ra8_camera_source_ceu.c@internal_ceu_cfg_valid)
 * - Vector 1: image_capture, uyvy422 -> C1=F, C2=F -> decision F -> accepted
 *   (test_cam_init_binds_and_reports_info supplies this vector).
 * - Vector 2: data_enable, uyvy422   -> C1=T, C2=F -> decision T -> invalid_arg.
 * - Vector 3: image_capture, jpeg    -> C1=F, C2=T -> decision T -> invalid_arg.
 * Vectors 1+2 flip the outcome varying C1 only; vectors 1+3 flip it varying C2
 * only. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 *
 * @pre The fake register window and MMIO fault seam are available.
 * @pre No CEU capture is in flight.
 * @post No rejected configuration leaves the source handle bound.
 * @post The MMIO fault seam is disarmed on return.
 * @note Not thread-safe; single-threaded test binary only.
 * @since 0.1.0
 */
static void test_cam_init_rejects_invalid_config(void)
{
  TEST_BEGIN("camera ceu: init rejects invalid configuration");
  internal_cam_prep();
  ra8_camera_source_t           source = {};
  ra8_camera_source_ceu_state_t state  = {};
  ra8_camera_source_ceu_cfg_t   cfg    = internal_cam_make_cfg();

  internal_test_cam_init_rejects_null_and_bounds(&source, &state, &cfg);
  internal_test_cam_init_rejects_format_pairing(&source, &state, &cfg);

  /* A CEU that never reports itself idle is forwarded, not swallowed. */
  cfg = internal_cam_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_ceu_init_cov(&source, &state, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait(ra8_ceu_reg32(k_ra8_ceu_off_cstsr)));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_camera_source_ceu_init_cov(&source, &state, &cfg));
  TEST_ASSERT_NULL(source.iface);
  TEST_ASSERT(!state.initialized);
  ra8_fake_mmio_reset();

  TEST_END("camera ceu: init rejects invalid configuration");
}

/**
 * @brief Bind the CEU source and report its metadata. @details Accepts the paired image-capture / UYVY configuration (MC/DC vector 1 of the format rule), checks the handle and state the bind produced, reads the metadata back through the facade, and confirms both not-initialized legs of the metadata query. @pre The fake register window is available. @pre No CEU capture is in flight. @post The bound source reports exactly the configured geometry. @post An unbound context or a cleared state flag is refused. @par MC/DC: Single-condition decisions only: the metadata query's unbound-context and cleared-initialized guards each run in both directions (bound success first, then each guard taken), as do the diagnostic accessor's two null guards. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
static void test_cam_init_binds_and_reports_info(void)
{
  TEST_BEGIN("camera ceu: init binds source and reports info");
  internal_cam_prep();
  ra8_camera_source_t               source = {};
  ra8_camera_source_ceu_state_t     state  = {};
  const ra8_camera_source_ceu_cfg_t cfg    = internal_cam_make_cfg();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_ceu_init_cov(&source, &state, &cfg));
  TEST_ASSERT(state.initialized);
  TEST_ASSERT_EQ(k_cam_poll_attempts, state.poll_attempts);
  TEST_ASSERT_EQ(k_cam_poll_ms, state.poll_interval_ms);
  TEST_ASSERT_EQ(k_ra8_ceu_fmt_image_capture, state.capture_format);
  TEST_ASSERT_NOT_NULL(source.iface);
  TEST_ASSERT(source.ctx == &state);
  /* The bound HAL descriptor really reached the CEU: CDWDR holds its stride. */
  TEST_ASSERT_EQ(cfg.ceu.dst_stride, *ra8_ceu_reg32(k_ra8_ceu_off_cdwdr));

  ra8_camera_source_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_get_info(&source, &info));
  TEST_ASSERT_EQ(k_cam_frame_bytes, info.frame_bytes_max);
  TEST_ASSERT_EQ(k_cam_stride, info.stride_bytes);
  TEST_ASSERT_EQ(k_cam_width, info.width);
  TEST_ASSERT_EQ(k_cam_height, info.height);
  TEST_ASSERT_EQ(k_ra8_camera_format_uyvy422, info.format);

  uint32_t events = (uint32_t)k_cam_poison_events;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_source_ceu_get_last_events_cov(nullptr, &events));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_source_ceu_get_last_events_cov(&state, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_ceu_get_last_events_cov(&state, &events));
  TEST_ASSERT_EQ(0U, events);

  ra8_camera_source_t unbound = source;
  unbound.ctx                 = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_camera_source_get_info(&unbound, &info));
  TEST_ASSERT_EQ(0U, info.frame_bytes_max);

  state.initialized = false;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_camera_source_get_info(&source, &info));
  events = (uint32_t)k_cam_poison_events;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_camera_source_ceu_get_last_events_cov(&state, &events));
  TEST_ASSERT_EQ(0U, events);
  TEST_END("camera ceu: init binds source and reports info");
}

/**
 * @brief Refuse a capture whose storage the backend cannot use. @details Covers a buffer smaller than the advertised bound, a buffer that breaks the eight-byte CEU alignment rule, an unbound context, a cleared initialized flag, and the cache-maintenance rejection of a null range reached below the facade. @pre The CEU source is bound to caller-owned state. @pre The fake register window is available. @post No rejection arms the capture engine. @post Every rejection reports its own specific error code. @par MC/DC: Single-condition decisions only: capacity-too-small, alignment, unbound-context, cleared-initialized and null-buffer guards are each taken here; their not-taken directions run in the completed-frame case of this suite. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
static void test_cam_capture_rejects_bad_buffers(void)
{
  TEST_BEGIN("camera ceu: capture rejects unusable storage");
  internal_cam_prep();
  ra8_camera_source_t               source = {};
  ra8_camera_source_ceu_state_t     state  = {};
  const ra8_camera_source_ceu_cfg_t cfg    = internal_cam_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_ceu_init_cov(&source, &state, &cfg));

  uint8_t* const            data  = (uint8_t*)(uintptr_t)k_test_cov_buf_a;
  ra8_camera_frame_t        frame = {};
  const ra8_camera_buffer_t small = {.data = data, .capacity = (uint32_t)k_cam_frame_bytes - 1U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_camera_source_capture(&source, &small, &frame));

  const ra8_camera_buffer_t skewed = {.data     = (uint8_t*)(uintptr_t)k_test_cov_unaligned,
                                      .capacity = (uint32_t)k_cam_frame_bytes};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_camera_source_capture(&source, &skewed, &frame));

  const ra8_camera_buffer_t good    = {.data = data, .capacity = (uint32_t)k_cam_frame_bytes};
  ra8_camera_source_t       unbound = source;
  unbound.ctx                       = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_camera_source_capture(&unbound, &good, &frame));

  state.initialized = false;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_camera_source_capture(&source, &good, &frame));
  state.initialized = true;

  /* The facade screens a null buffer, so the cache-maintenance rejection the
   * backend forwards is only observable one level below it. */
  const ra8_camera_buffer_t empty = {.data = nullptr, .capacity = (uint32_t)k_cam_frame_bytes};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, source.iface->capture(source.ctx, &empty, &frame));

  /* Nothing above armed the engine: CDAYR and CAPSR are untouched. */
  TEST_ASSERT_EQ(0U, *ra8_ceu_reg32(k_ra8_ceu_off_cdayr));
  TEST_ASSERT_EQ(0U, *ra8_ceu_reg32(k_ra8_ceu_off_capsr));
  TEST_END("camera ceu: capture rejects unusable storage");
}

/**
 * @brief Report a busy engine and a completion that never arrives. @details Drives the arm-time rejection produced by CSTSR.CPTON and then the bounded-poll expiry, checking that the expiry still programmed the destination address and issued the software reset the contract promises. @pre The CEU source is bound to caller-owned state. @pre No modelled capture-end is armed. @post The armed destination address is observable in CDAYR. @post The frame view is zeroed on both failures. @par MC/DC: Single-condition decisions only: the CSTSR.CPTON busy-at-arm guard and the bounded-poll expiry each run in the taken direction here; their not-taken directions run in the completed-frame case of this suite. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
static void test_cam_capture_busy_and_timeout(void)
{
  TEST_BEGIN("camera ceu: capture reports busy engine and poll expiry");
  internal_cam_prep();
  ra8_camera_source_t               source = {};
  ra8_camera_source_ceu_state_t     state  = {};
  const ra8_camera_source_ceu_cfg_t cfg    = internal_cam_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_ceu_init_cov(&source, &state, &cfg));

  uint8_t* const            data   = (uint8_t*)(uintptr_t)k_test_cov_buf_a;
  const ra8_camera_buffer_t buffer = {.data = data, .capacity = (uint32_t)k_cam_frame_bytes};
  ra8_camera_frame_t        frame  = {};

  *ra8_ceu_reg32(k_ra8_ceu_off_cstsr) = (uint32_t)k_ra8_ceu_cstsr_mask_cpton;
  TEST_ASSERT_EQ(k_ra8_err_busy, ra8_camera_source_capture(&source, &buffer, &frame));
  TEST_ASSERT_EQ(0U, *ra8_ceu_reg32(k_ra8_ceu_off_cdayr));
  *ra8_ceu_reg32(k_ra8_ceu_off_cstsr) = 0U;

  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_camera_source_capture(&source, &buffer, &frame));
  TEST_ASSERT_EQ(k_test_cov_buf_a, *ra8_ceu_reg32(k_ra8_ceu_off_cdayr));
  TEST_ASSERT((*ra8_ceu_reg32(k_ra8_ceu_off_capsr) & (uint32_t)k_ra8_ceu_capsr_mask_cpkil) != 0U);
  TEST_ASSERT_NULL(frame.data);
  TEST_ASSERT_EQ(0U, frame.bytes);

  uint32_t events = (uint32_t)k_cam_poison_events;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_ceu_get_last_events_cov(&state, &events));
  TEST_ASSERT_EQ(0U, events);
  TEST_END("camera ceu: capture reports busy engine and poll expiry");
}

/**
 * @brief Derive the captured byte count from the observed capture-end.
 *
 * @details A fixed-frame capture always reports the configured bound; a
 * data-enable capture reports CDSSR when the peripheral latched one and falls
 * back to the configured bound when it did not. The observed events are cleared
 * from CETCR before the wait returns.
 *
 * @par MC/DC:
 * Decision: `if (state->capture_format == k_ra8_ceu_fmt_data_enable)` guarding
 * `if (status.data_size != 0U)`
 * (libs/ra8_camera/src/ra8_camera_source_ceu.c@internal_ceu_frame_bytes)
 * - Vector 1: image_capture, CDSSR=200 -> outer F -> bound reported (200 ignored).
 * - Vector 2: data_enable, CDSSR=200   -> outer T, inner T -> CDSSR reported.
 * - Vector 3: data_enable, CDSSR=0     -> outer T, inner F -> bound reported.
 * Vectors 1+2 flip the outcome varying the format only; vectors 2+3 flip it
 * varying CDSSR only. Nested single-condition decisions, both directions taken.
 *
 * @pre The fake register window is available.
 * @pre No modelled capture-end is armed.
 * @post Each vector reports the exact byte count its rule prescribes.
 * @post CETCR no longer holds the observed capture-end flag.
 * @note Not thread-safe; single-threaded test binary only.
 * @since 0.1.0
 */
static void test_cam_wait_reports_capture_end_bytes(void)
{
  TEST_BEGIN("camera ceu: capture-end byte counts");
  internal_cam_prep();
  ra8_camera_source_ceu_state_t state =
    internal_cam_bare_state((uint32_t)k_cam_frame_bytes, k_ra8_ceu_fmt_image_capture);
  *ra8_ceu_reg32(k_ra8_ceu_off_cetcr) = (uint32_t)k_ra8_ceu_evt_cpe;
  *ra8_ceu_reg32(k_ra8_ceu_off_cdssr) = (uint32_t)k_cam_data_size;
  uint32_t bytes                      = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, internal_ceu_wait_for_frame(&state, &bytes));
  TEST_ASSERT_EQ(k_cam_frame_bytes, bytes);
  TEST_ASSERT_EQ(k_ra8_ceu_evt_cpe, state.last_events);
  TEST_ASSERT_EQ(0U, *ra8_ceu_reg32(k_ra8_ceu_off_cetcr));

  internal_cam_prep();
  state = internal_cam_bare_state((uint32_t)k_cam_frame_bytes, k_ra8_ceu_fmt_data_enable);
  *ra8_ceu_reg32(k_ra8_ceu_off_cetcr) = (uint32_t)k_ra8_ceu_evt_cpe;
  *ra8_ceu_reg32(k_ra8_ceu_off_cdssr) = (uint32_t)k_cam_data_size;
  bytes                               = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, internal_ceu_wait_for_frame(&state, &bytes));
  TEST_ASSERT_EQ(k_cam_data_size, bytes);

  internal_cam_prep();
  state = internal_cam_bare_state((uint32_t)k_cam_frame_bytes, k_ra8_ceu_fmt_data_enable);
  *ra8_ceu_reg32(k_ra8_ceu_off_cetcr) = (uint32_t)k_ra8_ceu_evt_cpe;
  bytes                               = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, internal_ceu_wait_for_frame(&state, &bytes));
  TEST_ASSERT_EQ(k_cam_frame_bytes, bytes);
  TEST_END("camera ceu: capture-end byte counts");
}

/**
 * @brief Separate fatal CEU faults from harmless sync traffic. @details A failed status read is forwarded untouched, a CRAM overflow abandons the frame even when a capture-end flag arrives with it, HD and VD alone never terminate the bounded poll, and a software reset that never completes is reported in place of the fault it was clearing -- on the fault leg and on the expiry leg alike. @pre The fake register window and MMIO fault seam are available. @pre No modelled capture-end is armed. @post Every observed event bit is retained in caller-owned diagnostic state. @post The MMIO fault seam is disarmed on return. @par MC/DC: Drives the event classification both ways per condition: a fatal overflow (with capture-end present) abandons the frame, sync-only traffic does not terminate the poll, and the failed software reset substitutes its own status on the fault and expiry legs alike; the failed status read forwards before any event is recorded. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
static void test_cam_wait_faults_and_reset_failure(void)
{
  TEST_BEGIN("camera ceu: fatal faults and reset failure");
  internal_cam_prep();
  ra8_camera_source_ceu_state_t state =
    internal_cam_bare_state((uint32_t)k_cam_frame_bytes, k_ra8_ceu_fmt_image_capture);
  const uint32_t fatal = (uint32_t)k_ra8_ceu_evt_cram_overflow | (uint32_t)k_ra8_ceu_evt_cpe;
  *ra8_ceu_reg32(k_ra8_ceu_off_cetcr) = fatal;
  uint32_t bytes                      = 0U;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_ceu_wait_for_frame(&state, &bytes));
  TEST_ASSERT_EQ(0U, bytes);
  TEST_ASSERT_EQ(fatal, state.last_events);
  TEST_ASSERT_EQ(0U, *ra8_ceu_reg32(k_ra8_ceu_off_cetcr));

  internal_cam_prep();
  state = internal_cam_bare_state((uint32_t)k_cam_frame_bytes, k_ra8_ceu_fmt_image_capture);
  const uint32_t sync                 = (uint32_t)k_ra8_ceu_evt_hd | (uint32_t)k_ra8_ceu_evt_vd;
  *ra8_ceu_reg32(k_ra8_ceu_off_cetcr) = sync;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, internal_ceu_wait_for_frame(&state, &bytes));
  TEST_ASSERT_EQ(sync, state.last_events);

  internal_cam_prep();
  state = internal_cam_bare_state((uint32_t)k_cam_frame_bytes, k_ra8_ceu_fmt_image_capture);
  *ra8_ceu_reg32(k_ra8_ceu_off_cetcr) = (uint32_t)k_ra8_ceu_evt_firewall;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait(ra8_ceu_reg32(k_ra8_ceu_off_cstsr)));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, internal_ceu_wait_for_frame(&state, &bytes));
  TEST_ASSERT_EQ(k_ra8_ceu_evt_firewall, state.last_events);
  ra8_fake_mmio_reset();

  internal_cam_prep();
  state = internal_cam_bare_state((uint32_t)k_cam_frame_bytes, k_ra8_ceu_fmt_image_capture);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait(ra8_ceu_reg32(k_ra8_ceu_off_cstsr)));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, internal_ceu_wait_for_frame(&state, &bytes));
  TEST_ASSERT_EQ(0U, state.last_events);
  ra8_fake_mmio_reset();

  /* A status read that fails is forwarded before any event is recorded. */
  internal_cam_prep();
  state = internal_cam_bare_state((uint32_t)k_cam_frame_bytes, k_ra8_ceu_fmt_image_capture);
  s_cam_model_status_err = k_ra8_err_hw_unmapped;
  TEST_ASSERT_EQ(k_ra8_err_hw_unmapped, internal_ceu_wait_for_frame(&state, &bytes));
  TEST_ASSERT_EQ(0U, state.last_events);
  s_cam_model_status_err = k_ra8_ok;
  TEST_END("camera ceu: fatal faults and reset failure");
}

/**
 * @brief Publish a completed frame and reject impossible byte counts. @details With the peripheral modelled as latching capture-end on the first poll, the backend must return a frame view aliasing the caller's buffer with exactly the configured geometry; a zero-byte capture and a capture larger than the supplied storage must both be refused as invalid sizes. @pre The CEU has been initialized. @pre The capture buffer is eight-byte aligned. @post A published frame aliases the caller's storage and validates. @post Neither byte-count rejection publishes a frame view. @par MC/DC: Decision: the published-byte-count validity guard (zero bytes OR larger than the supplied storage). Vector 1: zero-byte capture -> first condition true, refused; Vector 2: data-enable capture larger than the buffer -> second condition true, refused; Vector 3: the completed frame -> both false, published. Pairs 1+3 and 2+3 show each condition's independent influence. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
static void test_cam_capture_frame_and_byte_count_rules(void)
{
  TEST_BEGIN("camera ceu: completed frame and byte-count rules");
  internal_cam_prep();
  ra8_camera_source_t               source = {};
  ra8_camera_source_ceu_state_t     state  = {};
  const ra8_camera_source_ceu_cfg_t cfg    = internal_cam_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_ceu_init_cov(&source, &state, &cfg));

  uint8_t* const            data   = (uint8_t*)(uintptr_t)k_test_cov_buf_a;
  const ra8_camera_buffer_t buffer = {.data = data, .capacity = (uint32_t)k_cam_frame_bytes};
  ra8_camera_frame_t        frame  = {};
  s_cam_model_complete_at          = (uint32_t)k_cam_first_poll;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_capture(&source, &buffer, &frame));
  TEST_ASSERT(frame.data == data);
  TEST_ASSERT_EQ(k_cam_frame_bytes, frame.bytes);
  TEST_ASSERT_EQ(k_cam_stride, frame.stride_bytes);
  TEST_ASSERT_EQ(k_cam_width, frame.width);
  TEST_ASSERT_EQ(k_cam_height, frame.height);
  TEST_ASSERT_EQ(k_ra8_camera_format_uyvy422, frame.format);
  TEST_ASSERT_EQ(k_test_cov_buf_a, *ra8_ceu_reg32(k_ra8_ceu_off_cdayr));
  TEST_ASSERT_EQ(k_ra8_ceu_evt_cpe, state.last_events);

  /* A capture that produced nothing cannot be published as a frame. */
  internal_cam_prep();
  const ra8_ceu_config_t ceu_cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&ceu_cfg));
  ra8_camera_source_ceu_state_t empty_state =
    internal_cam_bare_state(0U, k_ra8_ceu_fmt_image_capture);
  s_cam_model_complete_at = (uint32_t)k_cam_first_poll;
  frame                   = (ra8_camera_frame_t){};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, internal_ceu_capture(&empty_state, &buffer, &frame));
  TEST_ASSERT_NULL(frame.data);

  /* A data-enable capture larger than the supplied storage is refused. */
  internal_cam_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ceu_init(&ceu_cfg));
  ra8_camera_source_ceu_state_t wide_state =
    internal_cam_bare_state((uint32_t)k_cam_frame_bytes, k_ra8_ceu_fmt_data_enable);
  s_cam_model_complete_at = (uint32_t)k_cam_first_poll;
  s_cam_model_data_size   = (uint32_t)k_cam_oversize_bytes;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, internal_ceu_capture(&wide_state, &buffer, &frame));
  TEST_ASSERT_NULL(frame.data);
  TEST_END("camera ceu: completed frame and byte-count rules");
}

/** @brief Check the production source's capture, diagnostic, and unbound legs. @details Exercises every capture rejection, the diagnostic accessor's guards, and the not-initialized legs left over once the caller's handle is unbound or the backend state is cleared. @param[in,out] source Bound production source under test. @param[in,out] state Bound production backend state under test. @pre @p source and @p state were bound by a successful ra8_camera_source_ceu_init. @pre No CEU capture is in flight. @post No rejected capture publishes a frame view. @post Every not-initialized leg is exercised. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
RA8_INTERNAL static void
internal_test_cam_source_production_capture(ra8_camera_source_t*           source,
                                            ra8_camera_source_ceu_state_t* state)
{
  uint8_t* const            data  = (uint8_t*)(uintptr_t)k_test_cov_buf_a;
  ra8_camera_frame_t        frame = {};
  const ra8_camera_buffer_t small = {.data = data, .capacity = (uint32_t)k_cam_frame_bytes - 1U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_camera_source_capture(source, &small, &frame));
  const ra8_camera_buffer_t skewed = {.data     = (uint8_t*)(uintptr_t)k_test_cov_unaligned,
                                      .capacity = (uint32_t)k_cam_frame_bytes};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_camera_source_capture(source, &skewed, &frame));
  const ra8_camera_buffer_t empty = {.data = nullptr, .capacity = (uint32_t)k_cam_frame_bytes};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, source->iface->capture(source->ctx, &empty, &frame));

  const ra8_camera_buffer_t buffer = {.data = data, .capacity = (uint32_t)k_cam_frame_bytes};
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_camera_source_capture(source, &buffer, &frame));
  TEST_ASSERT_EQ(k_test_cov_buf_a, *ra8_ceu_reg32(k_ra8_ceu_off_cdayr));
  TEST_ASSERT_NULL(frame.data);

  uint32_t events = (uint32_t)k_cam_poison_events;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_source_ceu_get_last_events(nullptr, &events));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_source_ceu_get_last_events(state, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_ceu_get_last_events(state, &events));
  TEST_ASSERT_EQ(0U, events);

  ra8_camera_source_t      unbound = *source;
  ra8_camera_source_info_t info    = {};
  unbound.ctx                      = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_camera_source_get_info(&unbound, &info));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_camera_source_capture(&unbound, &buffer, &frame));

  state->initialized = false;
  events             = (uint32_t)k_cam_poison_events;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_camera_source_ceu_get_last_events(state, &events));
  TEST_ASSERT_EQ(0U, events);
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_camera_source_get_info(source, &info));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_camera_source_capture(source, &buffer, &frame));
}

/**
 * @brief Drive the production copy of the CEU source linked through ra8_core_hal. @details Repeats the contract against the unrenamed symbols so the shipped translation unit -- not only the white-box copy above -- executes its guards, its metadata query, its storage rejections, the bounded poll it runs against the real CETCR, and its diagnostic accessor. @pre The fake register window is available. @pre No CEU capture is in flight. @post The production copy reports the same specific error codes as the copy. @post The bounded poll expiry leaves CDAYR holding the armed address. @par MC/DC: Single-condition decisions only, re-run against the production translation unit: the three init null guards and the zero-poll-attempts guard are each taken, then the bound success takes the not-taken directions; the shared capture/diagnostic legs run via the helper this test calls. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
static void test_cam_source_production_instance(void)
{
  TEST_BEGIN("camera ceu: production source instance");
  internal_cam_prep();
  ra8_camera_source_t           source = {};
  ra8_camera_source_ceu_state_t state  = {};
  ra8_camera_source_ceu_cfg_t   cfg    = internal_cam_make_cfg();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_source_ceu_init(nullptr, &state, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_source_ceu_init(&source, nullptr, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_source_ceu_init(&source, &state, nullptr));
  cfg.poll_attempts = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_camera_source_ceu_init(&source, &state, &cfg));

  cfg = internal_cam_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_ceu_init(&source, &state, &cfg));
  ra8_camera_source_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_get_info(&source, &info));
  TEST_ASSERT_EQ(k_cam_frame_bytes, info.frame_bytes_max);
  TEST_ASSERT_EQ(k_ra8_camera_format_uyvy422, info.format);

  internal_test_cam_source_production_capture(&source, &state);
  TEST_END("camera ceu: production source instance");
}

/** @brief Test binary entry point. @details Runs every case in declaration order; an assertion failure exits the process immediately, so a return of zero means all of them passed. @return int32_t Zero on success; never returns on failure. @pre Linked against the ra8_core_hal object library with RA8_OFF_TARGET defined. @pre The fake register window is mappable on this host. @post Every registered case has run to completion. @post The process exit status reflects the suite result. @note Not thread-safe; single-threaded test binary only. @since Version 0.1.0 */
int main(void)
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
  test_cam_init_rejects_invalid_config();
  test_cam_init_binds_and_reports_info();
  test_cam_capture_rejects_bad_buffers();
  test_cam_capture_busy_and_timeout();
  test_cam_wait_reports_capture_end_bytes();
  test_cam_wait_faults_and_reset_failure();
  test_cam_capture_frame_and_byte_count_rules();
  test_cam_source_production_instance();
  return 0;
}
