/**
 * @file test_ra8_flash_extra.c
 * @brief Unit tests for the ra8_flash configuration-set / extra-MRAM
 *        surface: OFS config-set writes, extra-MRAM program/erase,
 *        anti-rollback counters, zeroize/MSAR/MSUINITR, ECC controls,
 *        and update-transfer (MCTR*)
 *
 * @details Split from test_ra8_flash.c along the test-group seam; see
 * that file for the host-vs-target reachability notes (unmapped
 * extra-MRAM counter pages segfault on host, so only validation paths
 * run here). Shared fixture constants live in
 * support/flash_test_util.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>

#include "ra8_err.h"
#include "ra8_flash.h"
#include "ra8_flash_internal.h"
#include "ra8_flash_regs.h"
#include "ra8_sim_mmap.h"
#include "ra8_sim_mmio.h"
#include "support/flash_test_util.h"
#include "unity_minimal.h"

/**
 * @enum flash_extra_test_lit_t
 * @brief Named constants for the register stamp patterns and literal
 *        test vectors previously inlined in this file's test bodies.
 */
typedef enum : uint32_t {
  k_flash_extra_stamp_mcntdtr1  = 0x80000000UL, /**< Flash extra stamp mcntdtr1.  */
  k_flash_extra_stamp_mcntdtr0  = 0xFFFFFFFFUL, /**< Flash extra stamp mcntdtr0.  */
  k_flash_extra_stamp_mcntdtr12 = 0xFFFFFFFFUL, /**< Flash extra stamp mcntdtr12. */
  k_flash_extra_stamp_mrcrtea   = 0x11111111UL, /**< Flash extra stamp mrcrtea.   */
  k_flash_extra_stamp_mrcrdea   = 0x22222222UL, /**< Flash extra stamp mrcrdea.   */
  k_flash_extra_stamp_mrertea   = 0x33333333UL, /**< Flash extra stamp mrertea.   */
  k_flash_extra_stamp_mrerdea   = 0x44444444UL, /**< Flash extra stamp mrerdea.   */
  k_flash_extra_stamp_mrcpea    = 0xCAFEBABEUL, /**< Flash extra stamp mrcpea.    */
} flash_extra_test_lit_t;

/* ---------------------------------------------------------------------------
 * Configuration-set / extra MRAM
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * ------------------------------------------------------------------------ */

static void test_config_set_write_validation(void)
{
  TEST_BEGIN("flash config_set_write validation");
  ra8_sim_mmap_reset();

  uint16_t buf[k_ra8_mram_config_set_word_count] = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_flash_config_set_write(0x02C9F040UL, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_flash_config_set_write(0x00000000UL, buf));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_flash_config_set_write(0xFFFFFFFFUL, buf));

  /* Happy path: pre-stage MSTATR so the wait succeeds and no errors. */
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mstatr) = (uint32_t)k_ra8_mstatr_mask_mrdy;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_config_set_write((uint32_t)k_test_addr_extra_in, buf));
  TEST_END("flash config_set_write validation");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_config_set_write_ofs_window(void)
{
  TEST_BEGIN("flash config_set_write OFS window");
  ra8_sim_mmap_reset();
  uint16_t buf[k_ra8_mram_config_set_word_count]   = {};
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mstatr) = (uint32_t)k_ra8_mstatr_mask_mrdy;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_config_set_write((uint32_t)k_ra8_flash_ofs_start, buf));
  TEST_ASSERT_EQ(k_ra8_flash_ofs_start, (*ra8_mram_reg32((uint16_t)k_ra8_mram_off_msaddr)));
  TEST_END("flash config_set_write OFS window");
}

/**
 * @test test_mcdc_config_set_write_extra_window
 *
 * @par MC/DC:
 * Decision: ``in_extra = (target_addr >= k_ra8_flash_extra_start) &&
 *                       (target_addr < extra_end)`` (2 conds, line 1428).
 * Pre-existing test_config_set_write_validation supplies F,- (addr 0)
 * and T,F (addr 0xFFFFFFFF). This test adds the T,T vector by writing
 * inside the extra-MRAM window so both conditions are true and in_extra
 * resolves to T -- the in_ofs OR at line 1429 evaluates F,F = F (proceeds
 * past the rejection path) and the MACI sequence completes via the
 * pre-staged MRDY bit.
 *   V_T_T: target_addr = 0x02E07600 (start of extra MRAM, sim-mmap backed).
 * Combined with the existing F,- and T,F vectors, both C1-pair and
 * C2-pair are covered (3 vectors for N=2 conditions: minimal MC/DC).
 */
static void test_mcdc_config_set_write_extra_window(void)
{
  TEST_BEGIN("flash config_set_write MC/DC: in_extra T,T pair");
  ra8_sim_mmap_reset();
  uint16_t buf[k_ra8_mram_config_set_word_count] = {};
  /* Pre-stage MRDY so the MACI wait at the end of the function succeeds. */
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mstatr) = (uint32_t)k_ra8_mstatr_mask_mrdy;
  /* V_T_T: address inside extra MRAM -- in_extra = T,T = T. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_config_set_write((uint32_t)k_ra8_flash_extra_start, buf));
  TEST_END("flash config_set_write MC/DC: in_extra T,T pair");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_extra_mram_write_validation(void)
{
  TEST_BEGIN("flash extra_mram_write validation");
  ra8_sim_mmap_reset();
  const uint8_t buf[k_ra8_mram_write_size_bytes] = {};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_flash_extra_mram_write((uint32_t)k_test_addr_extra_in, nullptr, 4U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_flash_extra_mram_write((uint32_t)k_test_addr_extra_in, buf, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_flash_extra_mram_write((uint32_t)k_test_addr_extra_in, buf, 33U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_flash_extra_mram_write(0x00000000UL, buf, 4U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_flash_extra_mram_write((uint32_t)k_test_addr_extra_bad, buf, 4U));
  /* page-spanning */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_flash_extra_mram_write((uint32_t)k_ra8_flash_extra_start + 30U, buf, 4U));
  TEST_END("flash extra_mram_write validation");
}

/**
 * @test test_extra_mram_write_rejects_locked_otp
 *
 * @details
 * OTP-misuse guard (#397): the general-purpose write path must refuse any
 * target at or above ``k_ra8_flash_extra_locked_start`` -- the permanent,
 * irreversible option-setting structures (PBPS, POFSPS, REVOKE, HUK-zeroize,
 * anti-rollback). Those need the deliberate ``ra8_flash_config_set_write``.
 * The general-purpose OTP sub-range (``k_ra8_flash_gpotp_start``) stays
 * writable through this path.
 *
 * @par MC/DC:
 * Decision: single-condition guard ``end_excl > k_ra8_flash_extra_locked_start``.
 * - Vector F (allowed): a write into the GPOTP sub-range -- end_excl below the
 *   boundary -- returns k_ra8_ok (covered here and by the happy-path cases).
 * - Vector T (rejected): a write starting at the boundary (PBPS_SEC) -- end_excl
 *   above it -- returns k_ra8_err_invalid_arg. Two vectors for one condition.
 */
static void test_extra_mram_write_rejects_locked_otp(void)
{
  TEST_BEGIN("flash extra_mram_write rejects permanent-OTP structures");
  ra8_sim_mmap_reset();
  const uint8_t buf[k_ra8_mram_write_size_bytes]   = {};
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mstatr) = (uint32_t)k_ra8_mstatr_mask_mrdy;

  /* F: general-purpose OTP sub-range is permitted (end below the guard). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_extra_mram_write((uint32_t)k_ra8_flash_gpotp_start, buf, 16U));

  /* T: the permanent-structure block is refused. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_flash_extra_mram_write((uint32_t)k_ra8_flash_extra_locked_start, buf, 16U));
  TEST_END("flash extra_mram_write rejects permanent-OTP structures");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_extra_mram_write_success_pads_payload(void)
{
  TEST_BEGIN("flash extra_mram_write success pads payload");
  ra8_sim_mmap_reset();
  const uint8_t buf[3]                             = {0xA5U, 0x5AU, 0xC3U};
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mstatr) = (uint32_t)k_ra8_mstatr_mask_mrdy;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_flash_extra_mram_write((uint32_t)k_ra8_flash_extra_start, buf, sizeof(buf)));
  TEST_ASSERT_EQ(k_ra8_flash_extra_start, (*ra8_mram_reg32((uint16_t)k_ra8_mram_off_msaddr)));
  TEST_END("flash extra_mram_write success pads payload");
}

/**
 * @test test_extra_mram_write_emits_program_opcode
 *
 * @details
 * Regression guard for #170 (recon seed T1-17): the extra-MRAM DATA write path
 * MUST issue the MACI Program command (opener 0xE8, HUM Ch 59.7.4.5 "Program
 * Command" Figure 59.13 p 3591), NOT the Configuration Set command (opener
 * 0x40), which is valid only for the OFS config area. Config-Set against the
 * plain data area raises MSTATR.CFGSETERR on silicon and leaves the cells
 * blank, so a later read bus-faults on the blank-MRAM ECC error.
 *
 * Drives the real consumer entry ``ra8_flash_extra_mram_write`` for one 16-byte
 * program unit and captures the exact byte / halfword stream the driver emits
 * to the single-address MACI command port (``g_ra8_flash_maci_cmd8_log`` /
 * ``g_ra8_flash_maci_cmd16_log``). Asserts MSADDR latched the target, the
 * command bytes are ``0xE8, 0x08(N), 0xD0``, and the eight payload halfwords
 * are the little-endian pairs of the source bytes.
 *
 * @par MC/DC:
 * No compound decision is uniquely exercised. ``ra8_flash_extra_mram_write``
 * validates ``len == 0 || len > 32`` (F,F on this valid 16-byte call -- the T
 * legs are covered by test_extra_mram_write_validation) and loops ``done < len``
 * (single condition). This case asserts the emitted opcode, not a decision.
 */
static void test_extra_mram_write_emits_program_opcode(void)
{
  TEST_BEGIN("flash extra_mram_write emits 0xE8 Program opcode");
  ra8_sim_mmap_reset();
  ra8_flash_internal_maci_log_reset();

  /* One full 16-byte program unit (8 halfwords) with distinct source bytes. */
  uint8_t buf[16] = {};
  for (uint32_t i = 0U; i < 16U; ++i) {
    buf[i] = (uint8_t)(0x10U + i); /* 0x10, 0x11, ... 0x1F */
  }

  /* MRDY pre-staged so the post-command MACI wait succeeds. */
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mstatr) = (uint32_t)k_ra8_mstatr_mask_mrdy;

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_flash_extra_mram_write((uint32_t)k_ra8_flash_extra_start, buf, sizeof(buf)));

  /* MSADDR latched the extra-MRAM destination (HUM Ch 59.5.19 p 3573). */
  TEST_ASSERT_EQ(k_ra8_flash_extra_start, (*ra8_mram_reg32((uint16_t)k_ra8_mram_off_msaddr)));

  /* Command bytes: 0xE8 (Program opener), 0x08 (N halfword count), 0xD0 (trailer). */
  TEST_ASSERT_EQ(3U, g_ra8_flash_maci_cmd8_len);
  TEST_ASSERT_EQ(k_ra8_maci_cmd_program, g_ra8_flash_maci_cmd8_log[0]);
  TEST_ASSERT_EQ(k_ra8_maci_cmd_word_count_n, g_ra8_flash_maci_cmd8_log[1]);
  TEST_ASSERT_EQ(k_ra8_maci_cmd_final, g_ra8_flash_maci_cmd8_log[2]);

  /* Payload: eight little-endian halfwords of the source bytes. */
  TEST_ASSERT_EQ(8U, g_ra8_flash_maci_cmd16_len);
  for (uint32_t i = 0U; i < 8U; ++i) {
    const uint16_t expect =
      (uint16_t)((uint16_t)buf[(size_t)i * 2U] | ((uint16_t)buf[(i * 2U) + 1U] << 8U));
    TEST_ASSERT_EQ(expect, g_ra8_flash_maci_cmd16_log[i]);
  }

  TEST_END("flash extra_mram_write emits 0xE8 Program opcode");
}

/**
 * @test test_config_set_write_ofs_emits_config_set_opcode
 *
 * @details
 * Control for test_extra_mram_write_emits_program_opcode: the OFS configuration
 * area MUST still use the Configuration Set command (opener 0x40, HUM
 * Ch 59.7.4.8 p 3594), NOT the Program command. Proves the region-dispatched
 * opener in ``ra8_flash_config_set_write`` leaves the working OFS byte stream
 * bit-identical while the extra-MRAM path switches to 0xE8.
 *
 * @par MC/DC:
 * No compound decision uniquely exercised. The ``!in_ofs && !in_extra``
 * rejection compound resolves F,- here (in_ofs true, so it proceeds); its
 * other vectors are covered by test_config_set_write_validation. This case
 * asserts the emitted opener byte, not a decision.
 */
static void test_config_set_write_ofs_emits_config_set_opcode(void)
{
  TEST_BEGIN("flash config_set_write OFS emits 0x40 Config-Set opcode");
  ra8_sim_mmap_reset();
  ra8_flash_internal_maci_log_reset();

  uint16_t words[k_ra8_mram_config_set_word_count] = {};
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mstatr) = (uint32_t)k_ra8_mstatr_mask_mrdy;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_config_set_write((uint32_t)k_ra8_flash_ofs_start, words));

  /* OFS target keeps the Configuration Set opener (0x40), not Program (0xE8). */
  TEST_ASSERT_EQ(3U, g_ra8_flash_maci_cmd8_len);
  TEST_ASSERT_EQ(k_ra8_maci_cmd_config_set, g_ra8_flash_maci_cmd8_log[0]);
  TEST_ASSERT_EQ(k_ra8_maci_cmd_word_count_n, g_ra8_flash_maci_cmd8_log[1]);
  TEST_ASSERT_EQ(k_ra8_maci_cmd_final, g_ra8_flash_maci_cmd8_log[2]);
  TEST_END("flash config_set_write OFS emits 0x40 Config-Set opcode");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_extra_mram_erase_validation(void)
{
  TEST_BEGIN("flash extra_mram_erase validation");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_flash_extra_mram_erase(0x02C9F005UL));
  TEST_END("flash extra_mram_erase validation");
}

/* ---------------------------------------------------------------------------
 * Anti-rollback counters (validation paths only on host)
 * ------------------------------------------------------------------------ */

static void test_arc_argument_validation(void)
{
  TEST_BEGIN("flash arc validation");
  ra8_sim_mmap_reset();
  /* sim_mmap_reset only clears simulated MMIO; the driver's TU-static
   * init flag leaks across tests, so explicitly deinit before testing
   * the not_initialized paths. */
  (void)ra8_flash_deinit();

  uint32_t out = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_flash_arc_read(k_ra8_flash_arc_sec, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_flash_arc_read((ra8_flash_arc_id_t)99U, &out));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_flash_arc_increment((ra8_flash_arc_id_t)99U));

  /* Without init these should report not_initialized. */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_flash_arc_read(k_ra8_flash_arc_sec, &out));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_flash_arc_increment(k_ra8_flash_arc_sec));
  TEST_END("flash arc validation");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_arc_oembl_read_increment_paths(void)
{
  TEST_BEGIN("flash ARC OEMBL read and increment paths");
  ra8_sim_mmap_reset();
  const ra8_flash_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_init(&cfg));

  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mstatr)   = (uint32_t)k_ra8_mstatr_mask_mrdy;
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mcntdtr0) = 0x00000003UL;
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mcntdtr1) = k_flash_extra_stamp_mcntdtr1;
  uint32_t count                                     = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_arc_read(k_ra8_flash_arc_oembl, &count));
  TEST_ASSERT_EQ(3, count);
  TEST_ASSERT_EQ(k_ra8_mcntselr_oembl,
                 (*ra8_mram_reg8((uint16_t)k_ra8_mram_off_mcntselr) & k_ra8_mcntselr_mask));

  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mstatr)   = (uint32_t)k_ra8_mstatr_mask_mrdy;
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mcntdtr0) = 0x00000001UL;
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mcntdtr1) = 0x00000000UL;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_arc_increment(k_ra8_flash_arc_oembl));

  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mstatr)   = (uint32_t)k_ra8_mstatr_mask_mrdy;
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mcntdtr0) = k_flash_extra_stamp_mcntdtr0;
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mcntdtr1) = k_flash_extra_stamp_mcntdtr12;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_flash_arc_increment(k_ra8_flash_arc_oembl));

  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mstatr) = 0U;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_flash_arc_read(k_ra8_flash_arc_oembl, &count));
  TEST_END("flash ARC OEMBL read and increment paths");
}

/* ---------------------------------------------------------------------------
 * Zeroize / MSAR / MSUINITR / ECC controls
 * ------------------------------------------------------------------------ */

static void test_zeroize_huk_paths(void)
{
  TEST_BEGIN("flash zeroize_huk paths");
  ra8_sim_mmap_reset();
  (void)ra8_flash_deinit();
  /* Without init -> not_initialized. */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_flash_zeroize_huk());

  const ra8_flash_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_init(&cfg));

  /* Sim resets MREZS to 0, so the WHUKEXE check passes immediately. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_zeroize_huk());
  TEST_ASSERT_EQ(k_ra8_mrezc_full_zero, (*ra8_mram_reg16((uint16_t)k_ra8_mram_off_mrezc)));
  TEST_END("flash zeroize_huk paths");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_zeroize_huk_timeout(void)
{
  TEST_BEGIN("flash zeroize_huk timeout");
  ra8_sim_mmap_reset();
  const ra8_flash_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_init(&cfg));
  *ra8_mram_reg8((uint16_t)k_ra8_mram_off_mrezs) = (uint8_t)k_ra8_mrezs_mask_whukexe;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_flash_zeroize_huk());
  TEST_END("flash zeroize_huk timeout");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_security_attribution(void)
{
  TEST_BEGIN("flash set_security_attribution");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_set_security_attribution(0x55AAU));
  TEST_ASSERT_EQ(0x55AAU, (*ra8_mram_reg16((uint16_t)k_ra8_mram_off_msar)));
  TEST_END("flash set_security_attribution");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_msuinitr_kick(void)
{
  TEST_BEGIN("flash msuinitr_kick");
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  /* Happy path: the SUINIT auto-clear wait is satisfied by the MMIO
   * fault seam on its first poll (nothing armed). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_msuinitr_kick());

  /* Retry leg: the sequencer "finishes" on the 2nd poll. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sim_mmio_satisfy_after(
                   (const volatile void*)ra8_mram_reg16((uint16_t)k_ra8_mram_off_msuinitr),
                   2U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_msuinitr_kick());
  ra8_sim_mmio_reset();

  /* Timeout leg: SUINIT never auto-clears -> the bounded poll runs to
   * its budget and reports the stuck sequencer. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sim_mmio_fail_wait(
                   (const volatile void*)ra8_mram_reg16((uint16_t)k_ra8_mram_off_msuinitr)));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_flash_msuinitr_kick());
  ra8_sim_mmio_reset();
  TEST_END("flash msuinitr_kick");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_ecc_encoder_decoder_enable(void)
{
  TEST_BEGIN("flash set_ecc_*_enable");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_set_ecc_encoder_enable(true));
  TEST_ASSERT_EQ(
    1,
    (*ra8_mram_reg16((uint16_t)k_ra8_mram_off_mrceecc) & (uint16_t)k_ra8_mrceecc_mask_eccen));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_set_ecc_decoder_enable(false));
  TEST_ASSERT_EQ(
    0,
    (*ra8_mram_reg16((uint16_t)k_ra8_mram_off_mrcdecc) & (uint16_t)k_ra8_mrcdecc_mask_dececen));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_set_ecc_encoder_enable(false));
  TEST_ASSERT_EQ(
    0,
    (*ra8_mram_reg16((uint16_t)k_ra8_mram_off_mrceecc) & (uint16_t)k_ra8_mrceecc_mask_eccen));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_set_ecc_decoder_enable(true));
  TEST_ASSERT_EQ(
    k_ra8_mrcdecc_mask_dececen,
    (*ra8_mram_reg16((uint16_t)k_ra8_mram_off_mrcdecc) & (uint16_t)k_ra8_mrcdecc_mask_dececen));
  TEST_END("flash set_ecc_*_enable");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_ecc_error_addr(void)
{
  TEST_BEGIN("flash get_ecc_error_addr");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_flash_get_ecc_error_addr(nullptr, nullptr, nullptr, nullptr));

  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mrcrtea) = k_flash_extra_stamp_mrcrtea;
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mrcrdea) = k_flash_extra_stamp_mrcrdea;
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mrertea) = k_flash_extra_stamp_mrertea;
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mrerdea) = k_flash_extra_stamp_mrerdea;
  uint32_t a                                        = 0;
  uint32_t b                                        = 0;
  uint32_t c                                        = 0;
  uint32_t d                                        = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_get_ecc_error_addr(&a, &b, &c, &d));
  TEST_ASSERT_EQ(0x11111111, a);
  TEST_ASSERT_EQ(0x22222222, b);
  TEST_ASSERT_EQ(0x33333333, c);
  TEST_ASSERT_EQ(0x44444444, d);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_flash_get_ecc_error_addr(&a, nullptr, &c, &d));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_flash_get_ecc_error_addr(&a, &b, nullptr, &d));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_flash_get_ecc_error_addr(&a, &b, &c, nullptr));
  TEST_END("flash get_ecc_error_addr");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_program_error_addr(void)
{
  TEST_BEGIN("flash get_program_error_addr");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_flash_get_program_error_addr(nullptr));
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mrcpea) = k_flash_extra_stamp_mrcpea;
  uint32_t addr                                    = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_get_program_error_addr(&addr));
  TEST_ASSERT_EQ(0xCAFEBABEUL, addr);
  TEST_END("flash get_program_error_addr");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_update_clock_freq(void)
{
  TEST_BEGIN("flash update_clock_freq");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_flash_update_clock_freq(0xFFFFU, 100U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_flash_update_clock_freq(100U, 0xFFU));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_update_clock_freq(150U, 75U));
  /* Verify the keyed value landed. */
  const uint32_t want_mrcfreq = ((uint32_t)0x1EU << 24) | 150U;
  TEST_ASSERT_EQ(want_mrcfreq, (*ra8_mram_reg32((uint16_t)k_ra8_mram_off_mrcfreq)));
  TEST_END("flash update_clock_freq");
}

/* ---------------------------------------------------------------------------
 * Update-transfer (MCTR*)
 * ------------------------------------------------------------------------ */

static void test_set_update_transfer(void)
{
  TEST_BEGIN("flash set_update_transfer");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_flash_set_update_transfer(99U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_set_update_transfer(0x05U));
  TEST_ASSERT_EQ(0x05, (*ra8_mram_reg8((uint16_t)k_ra8_mram_off_mctrlsr)));
  TEST_END("flash set_update_transfer");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_update_status(void)
{
  TEST_BEGIN("flash get_update_status");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_flash_get_update_status(nullptr, nullptr, nullptr));

  *ra8_mram_reg16((uint16_t)k_ra8_mram_off_mctrstatr) =
    (uint16_t)((uint16_t)k_ra8_mctrstatr_mask_busy | (uint16_t)k_ra8_mctrstatr_mask_done |
               (uint16_t)k_ra8_mctrstatr_mask_err);
  uint8_t b = 0U;
  uint8_t d = 0U;
  uint8_t e = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_get_update_status(&b, &d, &e));
  TEST_ASSERT_EQ(1, b);
  TEST_ASSERT_EQ(1, d);
  TEST_ASSERT_EQ(1, e);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_flash_get_update_status(&b, nullptr, &e));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_flash_get_update_status(&b, &d, nullptr));
  TEST_END("flash get_update_status");
}

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Order is significant: cases run top to bottom, exactly as before.
 */
static void (*const s_test_roster[])(void) = {
  test_config_set_write_validation,
  test_config_set_write_ofs_window,
  test_mcdc_config_set_write_extra_window,
  test_extra_mram_write_validation,
  test_extra_mram_write_rejects_locked_otp,
  test_extra_mram_write_success_pads_payload,
  test_extra_mram_write_emits_program_opcode,
  test_config_set_write_ofs_emits_config_set_opcode,
  test_extra_mram_erase_validation,
  test_arc_argument_validation,
  test_arc_oembl_read_increment_paths,
  test_zeroize_huk_paths,
  test_zeroize_huk_timeout,
  test_set_security_attribution,
  test_msuinitr_kick,
  test_set_ecc_encoder_decoder_enable,
  test_get_ecc_error_addr,
  test_get_program_error_addr,
  test_update_clock_freq,
  test_set_update_transfer,
  test_get_update_status,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK  ] test_ra8_flash_extra.c\n");
  return 0;
}
