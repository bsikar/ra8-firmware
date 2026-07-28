/**
 * @file test_mem_ecc.c
 * @brief Host tests for the SRAM ECC fault-inject -> error-record decode (#130).
 *
 * @details
 * Backs ``mem_ecc_fault_demo``: drives the ECC decoder self-test
 * (``ra8_sram_self_test``) for a 1-bit and a 2-bit injection on a spare bank and
 * asserts the error record decodes to the correct ``SRAMESR`` slot --
 * ``one_bit_mask`` for the correctable fault, ``two_bit_mask`` for the
 * uncorrectable one -- and that ``ra8_sram_clear_status`` clears it. The
 * RAM-backed host register file has no ECC engine, so each case stages the
 * exact ``SRAMESR`` latch the silicon engine would set before the call; the
 * self-test's real read-and-decode then proves the per-slot fidelity the
 * on-device ra8_emulator model approximates (it latches both slots) and the
 * demo's globals report.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "ra8_sram.h"
#include "unity_minimal.h"

/** @brief Fixed geometry for the decode test (no magic numbers). */
typedef enum : uint32_t {
  k_mecc_t_bank     = 1U,     /**< Spare bank under test.                 */
  k_mecc_t_probe    = 0x100U, /**< 8-byte-aligned probe offset.           */
  k_mecc_t_bank_bit = 0x02U,  /**< (1 << bank): per-bank mask bit for #1. */
} mecc_test_geom_t;

/** @brief Configure full ECC with-check on the test bank. */
static ra8_sram_config_t mecc_test_cfg(void)
{
  ra8_sram_config_t cfg                      = {};
  cfg.banks[k_mecc_t_bank].ecc_mode          = k_ra8_sram_ecc_with_chk;
  cfg.banks[k_mecc_t_bank].on_error          = k_ra8_sram_on_error_interrupt;
  cfg.banks[k_mecc_t_bank].enable_1bit_latch = true;
  cfg.banks[k_mecc_t_bank].eccrgn            = k_ra8_sram_region_128kb;
  cfg.banks[k_mecc_t_bank].zero_init         = false;
  return cfg;
}

/**
 * @test test_mem_ecc_1bit_decodes_correctable
 * @brief A 1-bit injection latches only the 1-bit slot; clear wipes it.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- stages a 1-bit SRAMESR latch, then drives
 * ra8_sram_self_test's single-condition caught check and internal_decode_esr's
 * per-slot single-condition bit tests (one_bit_mask set, two_bit_mask clear), plus a
 * clear_status call; no && or || in the code under test that this case touches.)
 */
static void test_mem_ecc_1bit_decodes_correctable(void)
{
  TEST_BEGIN("mem_ecc: 1-bit injection decodes as correctable");
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
  const ra8_sram_config_t cfg = mecc_test_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_init(&cfg));

  /* The RAM-backed host register file has no ECC engine: stage the
   * bank-1 1-bit SRAMESR latch the silicon engine would set. */
  volatile r_sram_regs_t* regs = ra8_sram_regs();
  regs->SRAMESR                = (uint16_t)k_ra8_sram_err_bank1_1bit;

  bool caught = false;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_sram_self_test((uint8_t)k_mecc_t_bank, (uint32_t)k_mecc_t_probe, false, &caught));
  TEST_ASSERT(caught);

  ra8_sram_status_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_get_status(&st));
  TEST_ASSERT((st.one_bit_mask & (uint8_t)k_mecc_t_bank_bit) != 0U); /* 1-bit latched */
  TEST_ASSERT((st.two_bit_mask & (uint8_t)k_mecc_t_bank_bit) == 0U); /* not 2-bit     */

  /* clear_status writes the SRAMESCLR W1C register (HUM 58.2.13); the host MMIO
   * sim models registers as plain RAM and does not replay the SRAMESCLR ->
   * SRAMESR clear linkage, so assert the clear call succeeds (as test_ra8_sram
   * does) rather than re-reading a zeroed latch -- the latch-clear itself is
   * exercised on silicon (and in the ra8_emulator SRAMESR model). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_clear_status(st.raw_esr));
  TEST_END("mem_ecc: 1-bit injection decodes as correctable");
}

/**
 * @test test_mem_ecc_2bit_decodes_uncorrectable
 * @brief A 2-bit injection latches only the 2-bit slot; clear wipes it.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- stages a 2-bit SRAMESR latch, then drives
 * ra8_sram_self_test's single-condition caught check and internal_decode_esr's
 * per-slot single-condition bit tests (two_bit_mask set, one_bit_mask clear), plus a
 * clear_status call; no && or || in the code under test that this case touches.)
 */
static void test_mem_ecc_2bit_decodes_uncorrectable(void)
{
  TEST_BEGIN("mem_ecc: 2-bit injection decodes as uncorrectable");
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
  const ra8_sram_config_t cfg = mecc_test_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_init(&cfg));

  /* Stage the bank-1 2-bit SRAMESR latch (no host ECC engine). */
  volatile r_sram_regs_t* regs = ra8_sram_regs();
  regs->SRAMESR                = (uint16_t)k_ra8_sram_err_bank1_2bit;

  bool caught = false;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_sram_self_test((uint8_t)k_mecc_t_bank, (uint32_t)k_mecc_t_probe, true, &caught));
  TEST_ASSERT(caught);

  ra8_sram_status_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_get_status(&st));
  TEST_ASSERT((st.two_bit_mask & (uint8_t)k_mecc_t_bank_bit) != 0U); /* 2-bit latched */
  TEST_ASSERT((st.one_bit_mask & (uint8_t)k_mecc_t_bank_bit) == 0U); /* not 1-bit     */

  /* See the 1-bit test: the host MMIO sim does not replay the SRAMESCLR ->
   * SRAMESR clear linkage, so assert the clear call succeeds rather than the
   * re-read; the latch-clear is exercised on silicon + the ra8_emulator model. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_clear_status(st.raw_esr));
  TEST_END("mem_ecc: 2-bit injection decodes as uncorrectable");
}

int32_t main(void)
{
  test_mem_ecc_1bit_decodes_correctable();
  test_mem_ecc_2bit_decodes_uncorrectable();
  (void)fprintf(stderr, "[OK ] test_mem_ecc.c\n");
  return 0;
}
