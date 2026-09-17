/**
 * @file flash_test_util.h
 * @brief Shared fixture for the test_ra8_flash* suite: frequency and
 *        MRAM-address test constants plus the default driver config
 *
 * @details Header-only (all definitions `static`) so each split
 * test_ra8_flash* binary carries its own private copy; the
 * tests/CMakeLists.txt auto-glob stays free of non-test .c files.
 * Split out of test_ra8_flash.c when the suite was divided into
 * core / extra / ops binaries.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#include "ra8_flash.h"

typedef enum : uint32_t {
  k_test_mrcfreq_mhz = 200U,    /**< Test mrcfreq MHz.   */
  k_test_mrefreq_mhz = 100U,    /**< Test mrefreq MHz.   */
  k_test_bad_freq    = 0xFFFFU, /**< Test bad frequency. */
  k_test_bad_efreq   = 0x0200U, /**< Test bad efreq.     */
} ra8_flash_test_const_t;

typedef enum : uint32_t {
  k_test_addr_below_mram = 0x01FFFFF0UL, /**< Just below the MRAM window.   */
  k_test_addr_in_mram    = 0x02000020UL, /**< Page-aligned valid offset.    */
  k_test_addr_misaligned = 0x02000005UL, /**< Mid-page address.             */
  k_test_addr_above_mram = 0x02100000UL, /**< Past the 1 MiB end.           */
  k_test_addr_extra_in   = 0x02C9F040UL, /**< Inside OFS config_set window. */
  k_test_addr_extra_bad  = 0x03100000UL, /**< Past extra-MRAM end.          */
} ra8_flash_test_addr_t;

static inline ra8_flash_cfg_t make_cfg(void)
{
  const ra8_flash_cfg_t cfg = {
    .mrcfreq_mhz        = (uint16_t)k_test_mrcfreq_mhz,
    .mrefreq_mhz        = (uint8_t)k_test_mrefreq_mhz,
    .prefetch_en        = true,
    .ecc_encoder_enable = true,
    .ecc_decoder_enable = true,
  };
  return cfg;
}
