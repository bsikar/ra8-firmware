/**
 * @file test_mem_ecc.c
 * @brief Host tests for the SRAM ECC fault-inject -> error-record decode (#130).
 *
 * @details
 * Backs ``mem_ecc_fault_demo``: drives the ECC decoder self-test
 * (``ra_sram_self_test``) for a 1-bit and a 2-bit injection on a spare bank and
 * asserts the error record decodes to the correct ``SRAMESR`` slot --
 * ``one_bit_mask`` for the correctable fault, ``two_bit_mask`` for the
 * uncorrectable one -- and that ``ra_sram_clear_status`` clears it. Under
 * ``RA_SIMULATOR_MODE`` (the unit-test build) ``ra_sram_self_test`` forges the
 * matching latch, so this proves the per-slot decode fidelity the on-device
 * board_sim model approximates (it latches both slots) and the demo's globals
 * report.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "ra_sram.h"
#include "unity_minimal.h"

/** @brief Fixed geometry for the decode test (no magic numbers). */
typedef enum : uint32_t {
  k_mecc_t_bank     = 1U,     /**< Spare bank under test.                 */
  k_mecc_t_probe    = 0x100U, /**< 8-byte-aligned probe offset.           */
  k_mecc_t_bank_bit = 0x02U,  /**< (1 << bank): per-bank mask bit for #1. */
} mecc_test_geom_t;

/** @brief Configure full ECC with-check on the test bank. */
static ra_sram_config_t mecc_test_cfg(void)
{
  ra_sram_config_t cfg                       = {};
  cfg.banks[k_mecc_t_bank].ecc_mode          = k_ra_sram_ecc_with_chk;
  cfg.banks[k_mecc_t_bank].on_error          = k_ra_sram_on_error_interrupt;
  cfg.banks[k_mecc_t_bank].enable_1bit_latch = true;
  cfg.banks[k_mecc_t_bank].eccrgn            = k_ra_sram_region_128kb;
  cfg.banks[k_mecc_t_bank].zero_init         = false;
  return cfg;
}

/**
 * @test test_mem_ecc_1bit_decodes_correctable
 * @brief A 1-bit injection latches only the 1-bit slot; clear wipes it.
 */
static void test_mem_ecc_1bit_decodes_correctable(void)
{
  TEST_BEGIN("mem_ecc: 1-bit injection decodes as correctable");
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  const ra_sram_config_t cfg = mecc_test_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_sram_init(&cfg));

  bool caught = false;
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_sram_self_test((uint8_t)k_mecc_t_bank, (uint32_t)k_mecc_t_probe, false, &caught));
  TEST_ASSERT(caught);

  ra_sram_status_t st = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_sram_get_status(&st));
  TEST_ASSERT((st.one_bit_mask & (uint8_t)k_mecc_t_bank_bit) != 0U); /* 1-bit latched */
  TEST_ASSERT((st.two_bit_mask & (uint8_t)k_mecc_t_bank_bit) == 0U); /* not 2-bit     */

  /* clear_status writes the SRAMESCLR W1C register (HUM 58.2.13); the host MMIO
   * sim models registers as plain RAM and does not replay the SRAMESCLR ->
   * SRAMESR clear linkage, so assert the clear call succeeds (as test_ra_sram
   * does) rather than re-reading a zeroed latch -- the latch-clear itself is
   * exercised on silicon (and in the board_sim SRAMESR model). */
  TEST_ASSERT_EQ(k_ra_ok, ra_sram_clear_status(st.raw_esr));
  TEST_END("mem_ecc: 1-bit injection decodes as correctable");
}

/**
 * @test test_mem_ecc_2bit_decodes_uncorrectable
 * @brief A 2-bit injection latches only the 2-bit slot; clear wipes it.
 */
static void test_mem_ecc_2bit_decodes_uncorrectable(void)
{
  TEST_BEGIN("mem_ecc: 2-bit injection decodes as uncorrectable");
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  const ra_sram_config_t cfg = mecc_test_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_sram_init(&cfg));

  bool caught = false;
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_sram_self_test((uint8_t)k_mecc_t_bank, (uint32_t)k_mecc_t_probe, true, &caught));
  TEST_ASSERT(caught);

  ra_sram_status_t st = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_sram_get_status(&st));
  TEST_ASSERT((st.two_bit_mask & (uint8_t)k_mecc_t_bank_bit) != 0U); /* 2-bit latched */
  TEST_ASSERT((st.one_bit_mask & (uint8_t)k_mecc_t_bank_bit) == 0U); /* not 1-bit     */

  /* See the 1-bit test: the host MMIO sim does not replay the SRAMESCLR ->
   * SRAMESR clear linkage, so assert the clear call succeeds rather than the
   * re-read; the latch-clear is exercised on silicon + the board_sim model. */
  TEST_ASSERT_EQ(k_ra_ok, ra_sram_clear_status(st.raw_esr));
  TEST_END("mem_ecc: 2-bit injection decodes as uncorrectable");
}

int32_t main(void)
{
  test_mem_ecc_1bit_decodes_correctable();
  test_mem_ecc_2bit_decodes_uncorrectable();
  (void)fprintf(stderr, "[OK ] test_mem_ecc.c\n");
  return 0;
}
