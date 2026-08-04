/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_spi_b_target_cov.c
 * @brief Coverage tests for the SPI_B target-mode SPCMD encoding paths.
 *
 * @details
 * Companion to ``test_ra8_spi_b_target.c``.  The existing suite exercises
 * ``ra8_spi_b_target_init`` and ``ra8_spi_b_target_xfer`` with SPI mode 0
 * (CPOL=0, CPHA=0) and LSB-last ordering.  The switch arms for modes 1,
 * 2, and 3 inside ``internal_spcmd_target`` and the ``lsb_first`` true
 * branch are therefore never executed by the first suite.  This file
 * covers those four paths.
 *
 * Coverage targets (lines in ``ra8_spi_b_target.c``):
 *
 *  - 217-218 : ``case k_ra8_spi_mode_1:`` -- sets CPHA (bit 0).
 *  - 220-221 : ``case k_ra8_spi_mode_2:`` -- sets CPOL (bit 1).
 *  - 223-225 : ``case k_ra8_spi_mode_3:`` -- sets CPHA | CPOL.
 *  - 231-232 : ``if (cfg->lsb_first)`` true branch -- sets LSBF (bit 12).
 *
 * Each test calls ``ra8_spi_b_target_init`` and reads the SPCMD[0] word
 * back from the plain-RAM fake window to assert the expected bits.
 *
 * @par MC/DC: switch arm isolation
 * ``internal_spcmd_target`` uses a switch rather than compound boolean
 * conditions, so each case label is an independent branch with no Boolean
 * sub-conditions.  One test per case is sufficient for full MC/DC
 * coverage of the switch.
 *
 * @par MC/DC: lsb_first decision
 * Decision: ``if (cfg->lsb_first)`` (1 condition).
 * - Vector 1: lsb_first=false -> branch NOT taken (test_target_init_register_image
 *             in the companion suite).
 * - Vector 2: lsb_first=true  -> branch taken (test_target_spcmd_lsb_first, here).
 * V1 and V2 together prove lsb_first independently affects the outcome.
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_spi.h"
#include "ra8_spi_regs.h"
#include "unity_minimal.h"

/* =============================================================================
 * Test constants
 * =============================================================================
 */

/**
 * @enum ra8_spi_b_target_cov_test_t
 * @brief Integer constants used by coverage tests (magic-number gate).
 *
 * @details
 * The SPB field in SPCMD[0] is programmed with ``k_ra8_spcmd_spb_8bit``
 * (value 7) shifted left to position ``k_ra8_spcmd_bit_spb_lo`` (bit 16),
 * giving ``0x00070000``.  Each mode adds its CPOL/CPHA bits on top of
 * that fixed SPB encoding.  The LSB-first bit (LSBF, bit 12,
 * ``k_ra8_spcmd_mask_lsbf = 0x00001000``) is orthogonal and tested with
 * mode 0 so only one variable changes at a time.
 */
typedef enum : uint32_t {
  /** Channel index used by every test in this file. */
  k_cov_channel_0 = 0U,
  /**
   * SPB field encoding for 8-bit frames, pre-shifted.
   * (k_ra8_spcmd_spb_8bit=7) << (k_ra8_spcmd_bit_spb_lo=16) = 0x00070000.
   */
  k_cov_spb_8bit_shifted = 0x00070000U,
  /**
   * Expected SPCMD[0] for mode 1: CPHA (bit 0) set, CPOL clear.
   * k_ra8_spcmd_mask_cpha=0x1 + k_cov_spb_8bit_shifted.
   */
  k_cov_spcmd_mode1 = 0x00070001U,
  /**
   * Expected SPCMD[0] for mode 2: CPOL (bit 1) set, CPHA clear.
   * k_ra8_spcmd_mask_cpol=0x2 + k_cov_spb_8bit_shifted.
   */
  k_cov_spcmd_mode2 = 0x00070002U,
  /**
   * Expected SPCMD[0] for mode 3: CPHA (bit 0) and CPOL (bit 1) set.
   * (0x1 | 0x2) + k_cov_spb_8bit_shifted.
   */
  k_cov_spcmd_mode3 = 0x00070003U,
  /**
   * Expected SPCMD[0] for mode 0 + LSB-first.
   * k_ra8_spcmd_mask_lsbf=0x1000 + k_cov_spb_8bit_shifted.
   */
  k_cov_spcmd_mode0_lsbf = 0x00071000U,
} ra8_spi_b_target_cov_test_t;

/* =============================================================================
 * Shared helpers
 * =============================================================================
 */

/**
 * @brief Reset fake and MSTP state between tests.
 *
 * @details Mirrors the ``prep`` helper in ``test_ra8_spi_b_target.c`` so
 *          each test starts from a clean register image.
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
}

/* =============================================================================
 * Test 1: SPCMD[0] encoding for SPI mode 1 (CPHA=1, CPOL=0)
 * =============================================================================
 */

/**
 * @test test_target_spcmd_mode1
 * @brief Mode 1 init writes CPHA bit into SPCMD[0]; CPOL remains clear.
 *
 * @details
 * Calls ``ra8_spi_b_target_init`` with ``k_ra8_spi_mode_1`` and reads the
 * SPCMD[0] register back from the fake RAM window.  The expected
 * value is ``k_cov_spcmd_mode1``: only CPHA (bit 0) and the 8-bit SPB
 * encoding (bits 20:16 = 0x7) are set.
 *
 * Covers ``ra8_spi_b_target.c`` lines 217-218 (``case k_ra8_spi_mode_1``
 * body and break).
 *
 * @par MC/DC: switch arm k_ra8_spi_mode_1
 * One condition per arm; a single vector per arm satisfies MC/DC.
 * Vector: cfg.mode = k_ra8_spi_mode_1 -> CPHA set, CPOL clear.
 */
static void test_target_spcmd_mode1(void)
{
  TEST_BEGIN("spi_b_target_cov: mode 1 -> SPCMD[0] has CPHA set");
  prep();

  const ra8_spi_cfg_t cfg = {
    .baud_hz   = 0U,
    .pclka_hz  = 0U,
    .mode      = k_ra8_spi_mode_1,
    .lsb_first = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_b_target_init((uint8_t)k_cov_channel_0, &cfg));

  const uint32_t spcmd0 = ra8_spi((uint8_t)k_cov_channel_0)->SPCMD[0];
  TEST_ASSERT_EQ(k_cov_spcmd_mode1, spcmd0);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_deinit((uint8_t)k_cov_channel_0));
  TEST_END("spi_b_target_cov: mode 1 -> SPCMD[0] has CPHA set");
}

/* =============================================================================
 * Test 2: SPCMD[0] encoding for SPI mode 2 (CPHA=0, CPOL=1)
 * =============================================================================
 */

/**
 * @test test_target_spcmd_mode2
 * @brief Mode 2 init writes CPOL bit into SPCMD[0]; CPHA remains clear.
 *
 * @details
 * Calls ``ra8_spi_b_target_init`` with ``k_ra8_spi_mode_2`` and asserts
 * that SPCMD[0] equals ``k_cov_spcmd_mode2``: only CPOL (bit 1) and
 * the 8-bit SPB encoding are set; CPHA (bit 0) is clear.
 *
 * Covers ``ra8_spi_b_target.c`` lines 220-221 (``case k_ra8_spi_mode_2``
 * body and break).
 *
 * @par MC/DC: switch arm k_ra8_spi_mode_2
 * Vector: cfg.mode = k_ra8_spi_mode_2 -> CPOL set, CPHA clear.
 */
static void test_target_spcmd_mode2(void)
{
  TEST_BEGIN("spi_b_target_cov: mode 2 -> SPCMD[0] has CPOL set");
  prep();

  const ra8_spi_cfg_t cfg = {
    .baud_hz   = 0U,
    .pclka_hz  = 0U,
    .mode      = k_ra8_spi_mode_2,
    .lsb_first = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_b_target_init((uint8_t)k_cov_channel_0, &cfg));

  const uint32_t spcmd0 = ra8_spi((uint8_t)k_cov_channel_0)->SPCMD[0];
  TEST_ASSERT_EQ(k_cov_spcmd_mode2, spcmd0);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_deinit((uint8_t)k_cov_channel_0));
  TEST_END("spi_b_target_cov: mode 2 -> SPCMD[0] has CPOL set");
}

/* =============================================================================
 * Test 3: SPCMD[0] encoding for SPI mode 3 (CPHA=1, CPOL=1)
 * =============================================================================
 */

/**
 * @test test_target_spcmd_mode3
 * @brief Mode 3 init writes both CPHA and CPOL bits into SPCMD[0].
 *
 * @details
 * Calls ``ra8_spi_b_target_init`` with ``k_ra8_spi_mode_3`` and asserts
 * that SPCMD[0] equals ``k_cov_spcmd_mode3``: both CPHA (bit 0) and
 * CPOL (bit 1) are set, along with the 8-bit SPB encoding.
 *
 * Covers ``ra8_spi_b_target.c`` lines 223-225 (``case k_ra8_spi_mode_3``
 * body lines and break).
 *
 * @par MC/DC: switch arm k_ra8_spi_mode_3
 * Vector: cfg.mode = k_ra8_spi_mode_3 -> CPHA set, CPOL set.
 */
static void test_target_spcmd_mode3(void)
{
  TEST_BEGIN("spi_b_target_cov: mode 3 -> SPCMD[0] has CPHA and CPOL set");
  prep();

  const ra8_spi_cfg_t cfg = {
    .baud_hz   = 0U,
    .pclka_hz  = 0U,
    .mode      = k_ra8_spi_mode_3,
    .lsb_first = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_b_target_init((uint8_t)k_cov_channel_0, &cfg));

  const uint32_t spcmd0 = ra8_spi((uint8_t)k_cov_channel_0)->SPCMD[0];
  TEST_ASSERT_EQ(k_cov_spcmd_mode3, spcmd0);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_deinit((uint8_t)k_cov_channel_0));
  TEST_END("spi_b_target_cov: mode 3 -> SPCMD[0] has CPHA and CPOL set");
}

/* =============================================================================
 * Test 4: SPCMD[0] encoding when lsb_first=true
 * =============================================================================
 */

/**
 * @test test_target_spcmd_lsb_first
 * @brief lsb_first=true sets the LSBF bit (bit 12) in SPCMD[0].
 *
 * @details
 * Calls ``ra8_spi_b_target_init`` with mode 0 and ``lsb_first=true``.
 * The expected SPCMD[0] is ``k_cov_spcmd_mode0_lsbf``: LSBF (bit 12)
 * set and the 8-bit SPB encoding present; CPHA and CPOL are clear.
 *
 * Covers ``ra8_spi_b_target.c`` lines 231-232 (the ``lsb_first`` true
 * branch body and its closing brace).
 *
 * @par MC/DC: decision ``if (cfg->lsb_first)``
 * - Vector 1: lsb_first=false -> branch NOT taken (companion suite).
 * - Vector 2: lsb_first=true  -> branch taken (this test).
 * Vectors 1 and 2 together prove lsb_first independently controls the
 * outcome of the single-condition decision.
 */
static void test_target_spcmd_lsb_first(void)
{
  TEST_BEGIN("spi_b_target_cov: lsb_first=true -> SPCMD[0] has LSBF set");
  prep();

  const ra8_spi_cfg_t cfg = {
    .baud_hz   = 0U,
    .pclka_hz  = 0U,
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = true,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_b_target_init((uint8_t)k_cov_channel_0, &cfg));

  const uint32_t spcmd0 = ra8_spi((uint8_t)k_cov_channel_0)->SPCMD[0];
  TEST_ASSERT_EQ(k_cov_spcmd_mode0_lsbf, spcmd0);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_deinit((uint8_t)k_cov_channel_0));
  TEST_END("spi_b_target_cov: lsb_first=true -> SPCMD[0] has LSBF set");
}

/* =============================================================================
 * Entry point
 * =============================================================================
 */

int32_t main(void)
{
  ra8_fake_mmap_reset();
  test_target_spcmd_mode1();
  test_target_spcmd_mode2();
  test_target_spcmd_mode3();
  test_target_spcmd_lsb_first();
  (void)fprintf(stderr, "[OK ] test_ra8_spi_b_target_cov.c\n");
  return 0;
}
