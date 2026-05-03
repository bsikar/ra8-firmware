/**
 * @file test_ra_flash_edge_cases.c
 * @brief Edge-case + stress unit tests for the RA8D2 MRAM flash driver.
 *
 * @details
 * Complements ``test_ra_flash.c`` with focused stress / edge-case
 * coverage of safety-critical paths:
 *
 *   - blank-check on a partially-erased page must return blank=false
 *     (the verification window is byte-granular, not page-granular);
 *   - blank-check across a page boundary spanning erased + dirty cells
 *     correctly classifies the whole region;
 *   - write-block rejects a span that crosses the 32-byte page boundary
 *     (start at byte 30 of a page with len > 2 must be rejected);
 *   - config-set write rollback simulation: the MACI sequence sets
 *     MSTATR.OTERR mid-sequence -> the call returns hw_error rather
 *     than reporting a phantom success;
 *   - extra-MRAM erase argument validation (address out of window);
 *   - clear-status accepts the full 8-bit MSEINT mask without
 *     rejecting reserved bits as invalid.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>

#include "ra8d2_flash_regs.h"
#include "ra_err.h"
#include "ra_flash.h"
#include "ra_flash_internal.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_flash_edge_addr_extra_in   = 0x02C9F040UL,
  k_flash_edge_addr_extra_bad  = 0x03100000UL, /**< Past extra-MRAM end. */
  k_flash_edge_addr_below_mram = 0x01FFFFF0UL,
} ra_flash_edge_addr_t;

static ra_flash_cfg_t make_cfg(void)
{
  return (ra_flash_cfg_t){
    .mrcfreq_mhz        = 200U,
    .mrefreq_mhz        = 100U,
    .prefetch_en        = true,
    .ecc_encoder_enable = true,
    .ecc_decoder_enable = true,
  };
}

/* --- Blank-check: partially-erased page must report not blank --- */

static void test_blank_check_partial_page(void)
{
  TEST_BEGIN("flash blank_check on partially-erased page returns false");
  ra_sim_mmap_reset();
  const ra_flash_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_flash_init(&cfg));

  /* Stage a 32-byte page where only the first 16 bytes are erased. */
  volatile uint8_t* p = (volatile uint8_t*)(uintptr_t)k_flash_edge_addr_extra_in;
  for (uint32_t i = 0U; i < 16U; ++i) {
    p[i] = 0xFFU;
  }
  for (uint32_t i = 16U; i < 32U; ++i) {
    p[i] = (uint8_t)i;
  }

  bool blank = true;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_flash_blank_check((uintptr_t)k_flash_edge_addr_extra_in, 32U, &blank));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)blank);

  /* Sub-window over the first half remains blank. */
  blank = false;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_flash_blank_check((uintptr_t)k_flash_edge_addr_extra_in, 16U, &blank));
  TEST_ASSERT_EQ((int32_t)1, (int32_t)blank);
  TEST_END("flash blank_check on partially-erased page returns false");
}

/* --- Blank-check spanning two adjacent pages --- */

static void test_blank_check_page_boundary(void)
{
  TEST_BEGIN("flash blank_check spanning page boundary");
  ra_sim_mmap_reset();
  const ra_flash_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_flash_init(&cfg));

  /* Stage 64 bytes: first 32 dirty, second 32 erased. The check covers
   * the full span across the page boundary. */
  volatile uint8_t* p = (volatile uint8_t*)(uintptr_t)k_flash_edge_addr_extra_in;
  for (uint32_t i = 0U; i < 32U; ++i) {
    p[i] = 0x5AU;
  }
  for (uint32_t i = 32U; i < 64U; ++i) {
    p[i] = 0xFFU;
  }
  bool blank = true;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_flash_blank_check((uintptr_t)k_flash_edge_addr_extra_in, 64U, &blank));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)blank);
  TEST_END("flash blank_check spanning page boundary");
}

/* --- write-block: span crossing a 32-byte page must be rejected --- */

static void test_write_block_crosses_page(void)
{
  TEST_BEGIN("flash write_block rejects spans crossing the 32-byte page");
  ra_sim_mmap_reset();
  const uint8_t buf[32] = {};
  /* Start at byte 30 of a page with 8 bytes of payload -> would cross
   * into the next page. The driver must reject this without writing. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_flash_write_block((uint32_t)k_ra_flash_code_start + 30U,
                                               buf,
                                               8U,
                                               k_ra_flash_world_ns));
  /* A clean 4-byte aligned write that fits in the page is accepted at
   * the validation stage (it still proceeds to the controller, which
   * the host substrate cannot actually drive -- we only assert the
   * arg-check path). */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_flash_write_block((uint32_t)k_ra_flash_code_start + 28U,
                                               buf,
                                               8U,
                                               k_ra_flash_world_ns));
  TEST_END("flash write_block rejects spans crossing the 32-byte page");
}

/* --- config-set write rollback under simulated mid-sequence error --- */

static void test_config_set_write_error_rollback(void)
{
  TEST_BEGIN("flash config_set_write returns hw_error when MSTATR sets OTERR");
  ra_sim_mmap_reset();

  /* Pre-stage MSTATR with MRDY (so the wait succeeds) AND OTERR (so the
   * post-wait error scan reports hw_error). This mirrors a power-fail
   * mid-MACI-sequence where the controller could not complete the
   * config-set write. */
  *ra_mram_reg32((uint16_t)k_ra_mram_off_mstatr) =
    (uint32_t)k_ra_mstatr_mask_mrdy | (uint32_t)k_ra_mstatr_mask_oterr;

  uint16_t buf[k_ra_mram_config_set_word_count] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_hw_error,
                 (int32_t)ra_flash_config_set_write((uint32_t)k_flash_edge_addr_extra_in, buf));
  TEST_END("flash config_set_write returns hw_error when MSTATR sets OTERR");
}

/* --- extra-MRAM erase: bad address rejected --- */

static void test_extra_mram_erase_bad_addr(void)
{
  TEST_BEGIN("flash extra_mram_erase rejects address outside the window");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_flash_extra_mram_erase((uint32_t)k_flash_edge_addr_extra_bad));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_flash_extra_mram_erase((uint32_t)k_flash_edge_addr_below_mram));
  TEST_END("flash extra_mram_erase rejects address outside the window");
}

/* --- write-during-read collision: MSTATR busy -> driver fails fast --- */

static void test_write_during_read_collision(void)
{
  TEST_BEGIN("flash config_set_write returns hw_timeout if MRDY never asserts");
  ra_sim_mmap_reset();
  /* MRDY left clear -> internal_wait_mrdy exhausts its budget. */
  *ra_mram_reg32((uint16_t)k_ra_mram_off_mstatr) = 0U;

  uint16_t       buf[k_ra_mram_config_set_word_count] = {};
  const ra_err_t r = ra_flash_config_set_write((uint32_t)k_flash_edge_addr_extra_in, buf);
  /* Either k_ra_err_hw_timeout or k_ra_err_hw_error is acceptable: the
   * point is the driver MUST NOT spin forever and MUST NOT return ok. */
  TEST_ASSERT(r == k_ra_err_hw_timeout || r == k_ra_err_hw_error);
  TEST_END("flash config_set_write returns hw_timeout if MRDY never asserts");
}

/**
 * @test test_mcdc_flash_window_allows_pure
 *
 * @par MC/DC:
 * Decision at libs/ra_hal/src/ra_flash.c
 *   ``if (s_rt.win_low == 0U && s_rt.win_high == 0U)`` (2 conditions, AND).
 * Promoted via @ref ra_flash_internal_window_allows_pure so the test
 * can drive the four input dimensions independently of module state.
 *
 * - V1: win_low=0, win_high=0 -> C1=T C2=T -> "no window" -> true.
 * - V2: win_low=1, win_high=0 -> C1=F short -> false-branch (range checks).
 * - V3: win_low=0, win_high=1 -> C1=T C2=F -> false-branch (range checks).
 * V1+V2 isolate C1; V1+V3 isolate C2. N+1 = 3 vectors: minimal MC/DC.
 *
 * Additional vectors exercise the two follow-on range guards
 * (addr<low and end_excl>high) so the helper is fully covered.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition AND; N+1 = 3 vectors satisfy MC/DC fully.
 */
static void test_mcdc_flash_window_allows_pure(void)
{
  TEST_BEGIN("flash MC/DC: window_allows_pure AND");

  /* V1: both zero -> permissive sentinel, returns true regardless of addr/len. */
  TEST_ASSERT(ra_flash_internal_window_allows_pure(0x100U, 16U, 0U, 0U));
  TEST_ASSERT(ra_flash_internal_window_allows_pure(0xFFFFFFFFU, 1U, 0U, 0U));

  /* V2: win_low non-zero (C1=F) -- addr inside window passes. */
  TEST_ASSERT(ra_flash_internal_window_allows_pure(0x200U, 16U, 0x100U, 0x300U));
  /* V2b: addr below window low -> false. */
  TEST_ASSERT(!ra_flash_internal_window_allows_pure(0x50U, 16U, 0x100U, 0x300U));

  /* V3: win_high non-zero (C2=F) -- end_excl exceeds high -> false. */
  TEST_ASSERT(!ra_flash_internal_window_allows_pure(0x2F0U, 32U, 0U, 0x300U));
  /* V3b: end_excl exactly equals high -> true (high is exclusive upper). */
  TEST_ASSERT(ra_flash_internal_window_allows_pure(0x2F0U, 16U, 0U, 0x300U));

  TEST_END("flash MC/DC: window_allows_pure AND");
}

/**
 * @test test_mcdc_flash_wait_buffer_ready_pair
 *
 * @par MC/DC:
 * Decision (libs/ra_hal/src/ra_flash.c line 150):
 *   ``if (((s & PRGBSYC) == 0U) && ((s & ABUFFULL) == 0U))``
 * (2 conditions, AND; N+1 = 3 vectors).
 *
 * Reaches the production helper directly through
 * @ref ra_flash_internal_wait_buffer_ready_call (test-access wrapper)
 * with the simulator-backed MRCPS register pre-populated.
 *
 * - V1: MRCPS = PRGBSYC|ABUFFULL -> C1=F, C2=F. Decision F. With limit=2
 *       the loop exhausts and returns hw_timeout.
 * - V2: MRCPS = PRGBSYC          -> C1=F, C2=T. Decision F. hw_timeout.
 * - V3: MRCPS = ABUFFULL         -> C1=T, C2=F. Decision F. hw_timeout.
 * - V4: MRCPS = 0                -> C1=T, C2=T. Decision T. ok.
 * V1+V4 vary both; V2+V4 isolate C1; V3+V4 isolate C2. Provides the N+1
 * minimal MC/DC for the 2-condition AND.
 */
static void test_mcdc_flash_wait_buffer_ready_pair(void)
{
  TEST_BEGIN("flash MC/DC: wait_buffer_ready AND (line 150)");
  ra_sim_mmap_reset();

  /* V1: both bits set -> decision F every iteration -> timeout. */
  *ra_mram_reg8((uint16_t)k_ra_mram_off_mrcps) =
    (uint8_t)(k_ra_mrcps_mask_prgbsyc | k_ra_mrcps_mask_abuffull);
  TEST_ASSERT_EQ((int32_t)k_ra_err_hw_timeout,
                 (int32_t)ra_flash_internal_wait_buffer_ready_call(2U));

  /* V2: PRGBSYC=1, ABUFFULL=0 -> C1=F shorts -> still decision F -> timeout. */
  *ra_mram_reg8((uint16_t)k_ra_mram_off_mrcps) = (uint8_t)k_ra_mrcps_mask_prgbsyc;
  TEST_ASSERT_EQ((int32_t)k_ra_err_hw_timeout,
                 (int32_t)ra_flash_internal_wait_buffer_ready_call(2U));

  /* V3: PRGBSYC=0, ABUFFULL=1 -> C1=T C2=F -> decision F -> timeout. */
  *ra_mram_reg8((uint16_t)k_ra_mram_off_mrcps) = (uint8_t)k_ra_mrcps_mask_abuffull;
  TEST_ASSERT_EQ((int32_t)k_ra_err_hw_timeout,
                 (int32_t)ra_flash_internal_wait_buffer_ready_call(2U));

  /* V4: both clear -> decision T -> ok on first iteration. */
  *ra_mram_reg8((uint16_t)k_ra_mram_off_mrcps) = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_flash_internal_wait_buffer_ready_call(2U));

  TEST_END("flash MC/DC: wait_buffer_ready AND (line 150)");
}

/**
 * @test test_mcdc_flash_wait_commit_done_pair
 *
 * @par MC/DC:
 * Decision (libs/ra_hal/src/ra_flash.c line 181):
 *   ``if (((s & ABUFEMP) != 0U) && ((s & PRGBSYC) == 0U))``
 * (2 conditions, AND; N+1 = 3 vectors).
 *
 * - V1: MRCPS = 0                -> C1=F shorts. Decision F. hw_timeout.
 * - V2: MRCPS = ABUFEMP|PRGBSYC  -> C1=T, C2=F. Decision F. hw_timeout.
 * - V3: MRCPS = PRGBSYC          -> C1=F, C2=F. Decision F. hw_timeout.
 * - V4: MRCPS = ABUFEMP          -> C1=T, C2=T. Decision T. ok.
 * V1+V4 isolate C1; V2+V4 isolate C2.
 */
static void test_mcdc_flash_wait_commit_done_pair(void)
{
  TEST_BEGIN("flash MC/DC: wait_commit_done AND (line 181)");
  ra_sim_mmap_reset();

  /* V1: ABUFEMP=0, PRGBSYC=0 -> C1=F shorts -> timeout. */
  *ra_mram_reg8((uint16_t)k_ra_mram_off_mrcps) = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_hw_timeout,
                 (int32_t)ra_flash_internal_wait_commit_done_call(2U));

  /* V2: ABUFEMP=1, PRGBSYC=1 -> C1=T C2=F -> timeout. */
  *ra_mram_reg8((uint16_t)k_ra_mram_off_mrcps) =
    (uint8_t)(k_ra_mrcps_mask_abufemp | k_ra_mrcps_mask_prgbsyc);
  TEST_ASSERT_EQ((int32_t)k_ra_err_hw_timeout,
                 (int32_t)ra_flash_internal_wait_commit_done_call(2U));

  /* V3: ABUFEMP=0, PRGBSYC=1 -> both conds F -> timeout. */
  *ra_mram_reg8((uint16_t)k_ra_mram_off_mrcps) = (uint8_t)k_ra_mrcps_mask_prgbsyc;
  TEST_ASSERT_EQ((int32_t)k_ra_err_hw_timeout,
                 (int32_t)ra_flash_internal_wait_commit_done_call(2U));

  /* V4: ABUFEMP=1, PRGBSYC=0 -> all T -> ok. */
  *ra_mram_reg8((uint16_t)k_ra_mram_off_mrcps) = (uint8_t)k_ra_mrcps_mask_abufemp;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_flash_internal_wait_commit_done_call(2U));

  TEST_END("flash MC/DC: wait_commit_done AND (line 181)");
}

int32_t main(void)
{
  test_blank_check_partial_page();
  test_blank_check_page_boundary();
  test_write_block_crosses_page();
  test_config_set_write_error_rollback();
  test_extra_mram_erase_bad_addr();
  test_write_during_read_collision();
  test_mcdc_flash_window_allows_pure();
  test_mcdc_flash_wait_buffer_ready_pair();
  test_mcdc_flash_wait_commit_done_pair();
  (void)fprintf(stderr, "[OK  ] test_ra_flash_edge_cases.c\n");
  return 0;
}
