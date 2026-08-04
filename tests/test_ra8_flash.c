/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_flash.c
 * @brief Unit tests for ra8_flash.c (full HUM Ch 7 + Ch 59 coverage)
 *
 * @details
 * The code-MRAM data window (``0x02000000`` .. ``0x02100000``) is
 * NOT backed by ``ra8_fake_mmap`` -- attempting a real STR into that
 * range from the host test would segfault. Tests therefore exercise:
 *
 *   - the controller-register paths (init / deinit / status / rww);
 *   - parameter-validation failure modes of every public entry point;
 *   - the IRQ dispatcher with a fake callback;
 *   - happy-path sequences for the MACI helpers (force_stop, reset,
 *     enter/exit P/E mode, MSUINITR kick, zeroize, MSAR, MCTR*).
 *
 * The end-to-end write loop and the actual ARC counters that live in
 * unmapped extra-MRAM (``0x02C9F000`` and above) are exercised on the
 * embedded target only -- on the host they segfault because
 * ``ra8_fake_mmap.c`` only backs ``0x02C00000 .. 0x02D00000`` and the
 * NSEC/SEC counter pages live past that window.
 *
 * This binary owns init/status/programming-validation/protection/
 * P-E-mode/force-stop/startup-area; the config-set + extra-MRAM + ARC +
 * zeroize/ECC/update surface lives in the sibling
 * test_ra8_flash_extra.c and the IRQ + r_mram-parity + suspend/lock +
 * MC/DC surface in test_ra8_flash_ops.c. Shared fixture constants live
 * in support/flash_test_util.h.
 */

#include <stdint.h>
#include <stdio.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_flash.h"
#include "ra8_flash_internal.h"
#include "ra8_flash_regs.h"
#include "support/flash_test_util.h"
#include "unity_minimal.h"

/**
 * @enum flash_test_lit_t
 * @brief Named constants for the register stamp patterns and literal
 *        test vectors previously inlined in this file's test bodies.
 */
typedef enum : uint32_t {
  k_flash_cfg_mrefreq_mhz = 0xFFU, /**< Flash config mrefreq mhz. */
  k_flash_stamp_mrcps     = 0x21U, /**< Flash stamp mrcps.        */
  k_flash_stamp_mrcps2    = 0xFFU, /**< Flash stamp mrcps2.       */
} flash_test_lit_t;

/* ---------------------------------------------------------------------------
 * Init / deinit
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * ------------------------------------------------------------------------ */

static void test_init_happy(void)
{
  TEST_BEGIN("flash init happy");
  ra8_fake_mmap_reset();

  const ra8_flash_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_init(&cfg));

  /* Both program gates locked: low byte of MRCPC0/MRCPC1 must be 0. */
  TEST_ASSERT_EQ(0, (*ra8_mram_reg16((uint16_t)k_ra8_mram_off_mrcpc0) & 0x01U));
  TEST_ASSERT_EQ(0, (*ra8_mram_reg16((uint16_t)k_ra8_mram_off_mrcpc1) & 0x01U));
  /* MRPSC.MHSPEN clear. */
  TEST_ASSERT_EQ(0, (*ra8_mram_reg8((uint16_t)k_ra8_mram_off_mrpsc)));
  /* prefetch_en=true means MRCPFB == 1. */
  TEST_ASSERT_EQ(1, (*ra8_mram_reg8((uint16_t)k_ra8_mram_off_mrcpfb)));
  /* ECC enables programmed. */
  const uint16_t mrceecc = *ra8_mram_reg16((uint16_t)k_ra8_mram_off_mrceecc);
  TEST_ASSERT_EQ(1, (mrceecc & (uint16_t)k_ra8_mrceecc_mask_eccen));
  TEST_END("flash init happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null_cfg(void)
{
  TEST_BEGIN("flash init null cfg");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_flash_init(nullptr));
  TEST_END("flash init null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_mrcfreq(void)
{
  TEST_BEGIN("flash init bad mrcfreq");
  ra8_fake_mmap_reset();
  ra8_flash_cfg_t cfg = make_cfg();
  cfg.mrcfreq_mhz     = (uint16_t)k_test_bad_freq;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_flash_init(&cfg));
  TEST_END("flash init bad mrcfreq");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_mrefreq(void)
{
  TEST_BEGIN("flash init bad mrefreq");
  ra8_fake_mmap_reset();
  ra8_flash_cfg_t cfg = make_cfg();
  cfg.mrefreq_mhz     = (uint8_t)k_test_bad_efreq; /* truncates to 0 only if uint8_t */
  /* Use a value > 0x7D to actually trigger validation. */
  cfg.mrefreq_mhz = k_flash_cfg_mrefreq_mhz;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_flash_init(&cfg));
  TEST_END("flash init bad mrefreq");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_init_all_optional_features_disabled(void)
{
  TEST_BEGIN("flash init optional features disabled");
  ra8_fake_mmap_reset();
  ra8_flash_cfg_t cfg    = make_cfg();
  cfg.prefetch_en        = false;
  cfg.ecc_encoder_enable = false;
  cfg.ecc_decoder_enable = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_init(&cfg));
  TEST_ASSERT_EQ(0, (*ra8_mram_reg8((uint16_t)k_ra8_mram_off_mrcpfb)));
  TEST_ASSERT_EQ(
    0,
    (*ra8_mram_reg16((uint16_t)k_ra8_mram_off_mrceecc) & (uint16_t)k_ra8_mrceecc_mask_eccen));
  TEST_ASSERT_EQ(
    0,
    (*ra8_mram_reg16((uint16_t)k_ra8_mram_off_mrcdecc) & (uint16_t)k_ra8_mrcdecc_mask_dececen));
  TEST_END("flash init optional features disabled");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit_locks_everything(void)
{
  TEST_BEGIN("flash deinit locks");
  ra8_fake_mmap_reset();
  const ra8_flash_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_init(&cfg));
  /* Pretend a sticky error bit is set. */
  *ra8_mram_reg8((uint16_t)k_ra8_mram_off_mrcps) = (uint8_t)k_ra8_mrcps_mask_errors;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_deinit());

  /* After deinit MRPSC=0, MRCPFB=1, MRCPC* gates locked. */
  TEST_ASSERT_EQ(0, (*ra8_mram_reg8((uint16_t)k_ra8_mram_off_mrpsc)));
  TEST_ASSERT_EQ(1, (*ra8_mram_reg8((uint16_t)k_ra8_mram_off_mrcpfb)));
  TEST_ASSERT_EQ(0, (*ra8_mram_reg16((uint16_t)k_ra8_mram_off_mrcpc0) & 0x01U));
  TEST_ASSERT_EQ(0, (*ra8_mram_reg16((uint16_t)k_ra8_mram_off_mrcpc1) & 0x01U));
  TEST_END("flash deinit locks");
}

/* ---------------------------------------------------------------------------
 * Status registers
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * ------------------------------------------------------------------------ */

static void test_get_status_paths(void)
{
  TEST_BEGIN("flash get_status paths");
  ra8_fake_mmap_reset();

  /* NULL out -> rejected. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_flash_get_status(nullptr));

  /* Stash a known value and read it back. */
  const uint8_t expected = (uint8_t)(k_ra8_mrcps_mask_prgerrc | k_ra8_mrcps_mask_abufemp);
  *ra8_mram_reg8((uint16_t)k_ra8_mram_off_mrcps) = expected;

  uint8_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_get_status(&got));
  TEST_ASSERT_EQ(expected, got);
  TEST_END("flash get_status paths");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_extended_status(void)
{
  TEST_BEGIN("flash get_extended_status");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_flash_get_extended_status(nullptr));

  *ra8_mram_reg8((uint16_t)k_ra8_mram_off_mrcps)   = k_flash_stamp_mrcps;
  *ra8_mram_reg8((uint16_t)k_ra8_mram_off_mastat)  = (uint8_t)k_ra8_mastat_mask_cmdlk;
  *ra8_mram_reg8((uint16_t)k_ra8_mram_off_mrezs)   = (uint8_t)k_ra8_mrezs_mask_whukzf;
  *ra8_mram_reg16((uint16_t)k_ra8_mram_off_mcmdr)  = (uint16_t)k_ra8_mcmdr_mask_cmd_progress;
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mstatr) = (uint32_t)k_ra8_mstatr_mask_mrdy;

  ra8_flash_status_ext_t s = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_get_extended_status(&s));
  TEST_ASSERT_EQ(0x21, s.mrcps);
  TEST_ASSERT_EQ(k_ra8_mastat_mask_cmdlk, s.mastat);
  TEST_ASSERT_EQ(k_ra8_mrezs_mask_whukzf, s.mrezs);
  TEST_ASSERT_EQ(k_ra8_mcmdr_mask_cmd_progress, s.mcmdr);
  TEST_ASSERT_EQ(k_ra8_mstatr_mask_mrdy, s.mstatr);
  TEST_END("flash get_extended_status");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_status_paths(void)
{
  TEST_BEGIN("flash clear_status paths");
  ra8_fake_mmap_reset();

  /* Reject a mask that includes non-clearable bits. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_flash_clear_status((uint8_t)k_ra8_mrcps_mask_prgbsyc));

  /* Valid mask: PRGERRC + ECCERRC. */
  *ra8_mram_reg8((uint16_t)k_ra8_mram_off_mrcps) = k_flash_stamp_mrcps2;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_clear_status((uint8_t)k_ra8_mrcps_mask_errors));
  /* The driver writes the W1C mask; the fake mmap stores whatever we wrote. */
  TEST_ASSERT_EQ(k_ra8_mrcps_mask_errors, (*ra8_mram_reg8((uint16_t)k_ra8_mram_off_mrcps)));
  TEST_END("flash clear_status paths");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_rww_disable(void)
{
  TEST_BEGIN("flash set_rww_disable");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_set_rww_disable(true));
  TEST_ASSERT_EQ(0, (*ra8_mram_reg8((uint16_t)k_ra8_mram_off_mrcpfb)));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_set_rww_disable(false));
  TEST_ASSERT_EQ(1, (*ra8_mram_reg8((uint16_t)k_ra8_mram_off_mrcpfb)));
  TEST_END("flash set_rww_disable");
}

/* ---------------------------------------------------------------------------
 * Direct programming validation
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * ------------------------------------------------------------------------ */

static void test_write_block_validation(void)
{
  TEST_BEGIN("flash write_block validation");
  ra8_fake_mmap_reset();

  const uint8_t buf[k_ra8_mram_write_size_bytes] = {};

  /* NULL src */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_flash_write_block((uint32_t)k_test_addr_in_mram, nullptr, 1U, k_ra8_flash_world_ns));

  /* len = 0 */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_flash_write_block((uint32_t)k_test_addr_in_mram, buf, 0U, k_ra8_flash_world_ns));

  /* len > 32 */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_flash_write_block((uint32_t)k_test_addr_in_mram, buf, 33U, k_ra8_flash_world_ns));

  /* address below window */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_flash_write_block((uint32_t)k_test_addr_below_mram, buf, 4U, k_ra8_flash_world_ns));

  /* address at-or-above end */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_flash_write_block((uint32_t)k_test_addr_above_mram, buf, 4U, k_ra8_flash_world_ns));

  /* spans page boundary: start at offset 30 of a page, write 4 bytes */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_flash_write_block((uint32_t)k_ra8_flash_code_start + 30U, buf, 4U, k_ra8_flash_world_ns));
  TEST_END("flash write_block validation");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_erase_block_alignment(void)
{
  TEST_BEGIN("flash erase_block alignment");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_flash_erase_block((uint32_t)k_test_addr_misaligned, k_ra8_flash_world_ns));
  TEST_END("flash erase_block alignment");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_write_block_off_target_reachable_paths(void)
{
  TEST_BEGIN("flash write_block fake reachable paths");
  ra8_fake_mmap_reset();
  const uint8_t buf[4] = {0x11U, 0x22U, 0x33U, 0x44U};

  *ra8_mram_reg8((uint16_t)k_ra8_mram_off_mrcps) = (uint8_t)k_ra8_mrcps_mask_abufemp;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_flash_write_block((uint32_t)k_test_addr_in_mram, buf, sizeof(buf), k_ra8_flash_world_s));
  TEST_ASSERT_EQ(0x11, (*(volatile uint8_t*)(uintptr_t)k_test_addr_in_mram));
  TEST_ASSERT_EQ(k_ra8_mrcpc1_key_disable, (*ra8_mram_reg16((uint16_t)k_ra8_mram_off_mrcpc1)));
  TEST_ASSERT_EQ(0, (*ra8_mram_reg8((uint16_t)k_ra8_mram_off_mrpsc)));
  TEST_ASSERT_EQ(1, (*ra8_mram_reg8((uint16_t)k_ra8_mram_off_mrcpfb)));

  *ra8_mram_reg8((uint16_t)k_ra8_mram_off_mrcps) =
    (uint8_t)(k_ra8_mrcps_mask_abufemp | k_ra8_mrcps_mask_prgerrc);
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 ra8_flash_write_block((uint32_t)k_test_addr_in_mram + 4U,
                                       buf,
                                       sizeof(buf),
                                       k_ra8_flash_world_ns));
  TEST_ASSERT_EQ(k_ra8_mrcpc0_key_disable, (*ra8_mram_reg16((uint16_t)k_ra8_mram_off_mrcpc0)));
  TEST_END("flash write_block fake reachable paths");
}

/* ---------------------------------------------------------------------------
 * Block protection
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * ------------------------------------------------------------------------ */

static void test_block_protect_set(void)
{
  TEST_BEGIN("flash block_protect_set");
  ra8_fake_mmap_reset();

  /* Permanent + unlock is rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_flash_block_protect_set(k_ra8_flash_world_ns, false, true));

  /* NS lock writes the keyed value. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_block_protect_set(k_ra8_flash_world_ns, true, false));
  TEST_ASSERT_EQ(k_ra8_mrcbprot0_key_lock, (*ra8_mram_reg16((uint16_t)k_ra8_mram_off_mrcbprot0)));

  /* S unlock writes the unlock-keyed value. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_block_protect_set(k_ra8_flash_world_s, false, false));
  TEST_ASSERT_EQ(k_ra8_mrcbprot1_key_unlock, (*ra8_mram_reg16((uint16_t)k_ra8_mram_off_mrcbprot1)));

  /* Permanent + lock is accepted (warning logged). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_block_protect_set(k_ra8_flash_world_s, true, true));
  TEST_END("flash block_protect_set");
}

/* ---------------------------------------------------------------------------
 * P/E mode transitions
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * ------------------------------------------------------------------------ */

static void test_pe_mode_round_trip(void)
{
  TEST_BEGIN("flash pe_mode round trip");
  ra8_fake_mmap_reset();

  /* Pre-stage MENTRYR so enter sees PE bit immediately. */
  *ra8_mram_reg16((uint16_t)k_ra8_mram_off_mentryr) = (uint16_t)k_ra8_mentryr_mask_pe_mode;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_enter_pe_mode());

  /* Pre-stage MENTRYR so exit sees PE bit clear immediately. */
  *ra8_mram_reg16((uint16_t)k_ra8_mram_off_mentryr) = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_exit_pe_mode());
  TEST_END("flash pe_mode round trip");
}

/* ---------------------------------------------------------------------------
 * Force stop / reset
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * ------------------------------------------------------------------------ */

static void test_force_stop_happy(void)
{
  TEST_BEGIN("flash force_stop happy");
  ra8_fake_mmap_reset();

  /* Pre-stage MSTATR.MRDY so the wait loop returns immediately. */
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mstatr) = (uint32_t)k_ra8_mstatr_mask_mrdy;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_force_stop());
  TEST_END("flash force_stop happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_force_stop_cmdlk(void)
{
  TEST_BEGIN("flash force_stop cmdlk");
  ra8_fake_mmap_reset();
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mstatr) = (uint32_t)k_ra8_mstatr_mask_mrdy;
  *ra8_mram_reg8((uint16_t)k_ra8_mram_off_mastat)  = (uint8_t)k_ra8_mastat_mask_cmdlk;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_flash_force_stop());
  TEST_END("flash force_stop cmdlk");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_force_stop_timeout(void)
{
  TEST_BEGIN("flash force_stop timeout");
  ra8_fake_mmap_reset();
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mstatr) = 0U;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_flash_force_stop());
  TEST_END("flash force_stop timeout");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_reset_happy(void)
{
  TEST_BEGIN("flash reset happy");
  ra8_fake_mmap_reset();
  const ra8_flash_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_init(&cfg));

  /* Pre-stage so all spin loops return immediately. */
  *ra8_mram_reg16((uint16_t)k_ra8_mram_off_mentryr) = (uint16_t)k_ra8_mentryr_mask_pe_mode;
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mstatr)  = (uint32_t)k_ra8_mstatr_mask_mrdy;

  /* Force the exit-PE check to pass too: clearing MENTRYR via writes is fine. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_reset());
  TEST_END("flash reset happy");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_reset_not_initialized_and_stop_error(void)
{
  TEST_BEGIN("flash reset not initialized and stop error");
  ra8_fake_mmap_reset();
  (void)ra8_flash_deinit();
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_flash_reset());

  const ra8_flash_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_init(&cfg));
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mstatr) = (uint32_t)k_ra8_mstatr_mask_mrdy;
  *ra8_mram_reg8((uint16_t)k_ra8_mram_off_mastat)  = (uint8_t)k_ra8_mastat_mask_cmdlk;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_flash_reset());
  TEST_END("flash reset not initialized and stop error");
}

/* ---------------------------------------------------------------------------
 * Start-up area
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * ------------------------------------------------------------------------ */

static void test_set_startup_area_temporary(void)
{
  TEST_BEGIN("flash set_startup_area temporary");
  ra8_fake_mmap_reset();

  /* MENTRYR is read back as PE; MSTATR mrdy stays high. */
  *ra8_mram_reg16((uint16_t)k_ra8_mram_off_mentryr) = (uint16_t)k_ra8_mentryr_mask_pe_mode;
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mstatr)  = (uint32_t)k_ra8_mstatr_mask_mrdy;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_set_startup_area(k_ra8_flash_startup_alternate, true));
  /* MSUACR holds key + alternate bit. */
  const uint16_t want =
    (uint16_t)((uint16_t)k_ra8_msuacr_key | (uint16_t)k_ra8_msuacr_swap_alternate);
  TEST_ASSERT_EQ(want, (*ra8_mram_reg16((uint16_t)k_ra8_mram_off_msuacr)));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_flash_set_startup_area((ra8_flash_startup_t)99U, true));
  TEST_END("flash set_startup_area temporary");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_set_startup_area_default_and_permanent(void)
{
  TEST_BEGIN("flash set_startup_area default and permanent");
  ra8_fake_mmap_reset();
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mstatr) = (uint32_t)k_ra8_mstatr_mask_mrdy;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_set_startup_area(k_ra8_flash_startup_default, true));
  TEST_ASSERT_EQ((k_ra8_msuacr_key | k_ra8_msuacr_swap_default),
                 (*ra8_mram_reg16((uint16_t)k_ra8_mram_off_msuacr)));

  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mstatr) = (uint32_t)k_ra8_mstatr_mask_mrdy;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_set_startup_area(k_ra8_flash_startup_default, false));
  TEST_ASSERT_EQ(k_ra8_msaddr_config_set_startup,
                 (*ra8_mram_reg32((uint16_t)k_ra8_mram_off_msaddr)));
  TEST_END("flash set_startup_area default and permanent");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_startup_area(void)
{
  TEST_BEGIN("flash get_startup_area");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_flash_get_startup_area(nullptr, nullptr));

  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_msuasmon) =
    (uint32_t)((uint32_t)k_ra8_msuasmon_mask_btflg | (uint32_t)k_ra8_msuasmon_mask_fspr);
  uint8_t btflg = 0U;
  uint8_t fspr  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_get_startup_area(&btflg, &fspr));
  TEST_ASSERT_EQ(1, btflg);
  TEST_ASSERT_EQ(1, fspr);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_flash_get_startup_area(&btflg, nullptr));
  TEST_END("flash get_startup_area");
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
  test_init_happy,
  test_init_null_cfg,
  test_init_bad_mrcfreq,
  test_init_bad_mrefreq,
  test_init_all_optional_features_disabled,
  test_deinit_locks_everything,
  test_get_status_paths,
  test_get_extended_status,
  test_clear_status_paths,
  test_set_rww_disable,
  test_write_block_validation,
  test_erase_block_alignment,
  test_write_block_off_target_reachable_paths,
  test_block_protect_set,
  test_pe_mode_round_trip,
  test_force_stop_happy,
  test_force_stop_cmdlk,
  test_force_stop_timeout,
  test_reset_happy,
  test_reset_not_initialized_and_stop_error,
  test_set_startup_area_temporary,
  test_set_startup_area_default_and_permanent,
  test_get_startup_area,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK  ] test_ra8_flash.c\n");
  return 0;
}
