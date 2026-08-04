/**
 * @file vin_test_util.h
 * @brief Shared fixture helpers for the test_ra8_vin_* sibling suites
 *
 * @details
 * The original test_ra8_vin.c suite was split along its test-group
 * seams into test_ra8_vin_capture.c, test_ra8_vin_config.c and
 * test_ra8_vin_mcdc.c. All three siblings share the same numeric
 * test-constant enum and the known-good config builder below, so they
 * live here once (header-only, ``static inline``) instead of being
 * copied three times.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_vin.h"

/**
 * @enum ra8_vin_test_const_t
 * @brief Magic-free constants for the test cases.
 *
 * @details
 * Centralises the framebuffer addresses and image-stride values used
 * across multiple cases so a single edit moves them all.
 */
typedef enum : uint32_t {
  k_ra8_vin_test_fb1     = 0x68000000UL, /**< First MB1 (SDRAM head).         */
  k_ra8_vin_test_fb2     = 0x68040000UL, /**< MB2 for continuous mode.        */
  k_ra8_vin_test_fb3     = 0x68080000UL, /**< MB3 for continuous mode.        */
  k_ra8_vin_test_fb_alt  = 0x680C0000UL, /**< Alternate MB1 base.             */
  k_ra8_vin_test_uv_off  = 0x68100000UL, /**< UV plane offset (aligned).      */
  k_ra8_vin_test_uv_bad  = 0x68100001UL, /**< UV plane offset (mis-aligned).  */
  k_ra8_vin_test_stride  = 1280U,        /**< Pixels per scanline.            */
  k_ra8_vin_test_ie_mask = 0x00000022UL, /**< IE: enable EFE + FME.           */
  k_ra8_vin_test_ctx     = 0x12345678UL, /**< Sentinel ctx for callback test. */
  k_ra8_vin_test_status  = 0x00000080UL, /**< Arbitrary non-W1C INTS bit.     */
  k_ra8_vin_test_si_val  = 600U,         /**< Scanline compare line.          */
} ra8_vin_test_const_t;

/**
 * @brief Build a known-good config with two MB buffers populated.
 */
static inline ra8_vin_config_t make_cfg(void)
{
  const ra8_vin_config_t cfg = {
    .input_fmt          = k_ra8_vin_input_rgb888,
    .bypass_csc         = true,
    .big_endian         = false,
    .interlace_mode     = (uint8_t)k_ra8_vin_im_odd_even,
    .pixel_clip_mode    = (uint8_t)k_ra8_vin_clp_default,
    .image_stride_px    = (uint16_t)k_ra8_vin_test_stride,
    .framebuffer_addr_1 = (uint32_t)k_ra8_vin_test_fb1,
    .framebuffer_addr_2 = (uint32_t)k_ra8_vin_test_fb2,
    .framebuffer_addr_3 = (uint32_t)k_ra8_vin_test_fb3,
    .interrupt_enable   = (uint32_t)k_ra8_vin_test_ie_mask,
    .scanline_compare   = (uint16_t)k_ra8_vin_test_si_val,
  };
  return cfg;
}
