/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_spi_b_target.c
 * @brief Unit tests for the SPI_B target (peripheral) mode driver.
 *
 * @details
 * Tests ``ra8_spi_b_target_init`` and ``ra8_spi_b_target_xfer`` from
 * ``libs/ra8_hal/src/ra8_spi_b_target.c`` against the RA8D2 SPI_B register
 * model exposed by the fake (``RA8_OFF_TARGET``).
 *
 * Coverage plan:
 *
 *  1. ``test_target_init_register_image`` -- verifies the SPCR image after
 *     a successful init: MSTR=0, MODFEN=1, SPE=1.
 *  2. ``test_target_xfer_roundtrip`` -- happy-path xfer; pre-seeds SPSR so
 *     both SPTEF and SPRF waits succeed; verifies ``k_ra8_ok`` and that
 *     the SPDR write (TX byte) is observable and the rx pointer is
 *     populated.
 *  3. ``test_target_init_null_cfg`` -- null ``cfg`` rejected (precondition).
 *  4. ``test_target_init_oor_channel`` -- out-of-range channel rejected.
 *  5. ``test_target_xfer_oor_channel`` -- out-of-range channel in xfer
 *     triggers ``k_ra8_err_null_ptr`` via RA8_CHECK_NULL_PTR.
 *  6. ``test_target_xfer_null_rx`` -- null ``rx`` pointer is accepted;
 *     result is discarded silently.
 *  7. ``test_target_xfer_timeout_sptef`` -- the ra8_fake_mmio fault seam arms
 *     the SPSR wait to never satisfy, so the first (SPTEF) poll times out.
 *  8. ``test_target_xfer_timeout_sprf`` -- the same SPSR wait armed to fail
 *     surfaces the xfer timeout early-return and leaves rx untouched.
 *
 * @par Decision coverage notes:
 * All compound decisions in the tested code are avoided by design
 * (no &&/|| in ra8_spi_b_target.c). The following single-condition
 * decisions are exercised:
 *
 *  - ``internal_target_wait_spsr``: the ra8_hw_wait_flag_set32 poll returns
 *    k_ra8_ok while the seam is transparent (tests 2, 6) and k_ra8_err_hw_timeout
 *    when the SPSR word is armed to fail (tests 7, 8).
 *  - ``ra8_spi_b_target_xfer``: ``if (rx != nullptr)``
 *    -> V1: rx non-NULL (test 2) / V2: rx NULL (test 6).
 *  - ``ra8_spi_b_target_xfer``: timeout early-return true leg (tests 7, 8);
 *    happy-path false leg (tests 2, 6).
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_mstp.h"
#include "ra8_spi.h"
#include "ra8_spi_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_spi_target_t
 * @brief Received-byte out-parameter seed.
 */
typedef enum : uint8_t {
  k_t_rx_unset = 0xEEU, /**< Pre-set received byte; a transfer that fails must
                             leave it rather than report data.                   */
} t_spi_target_t;

/* =============================================================================
 * Test constants
 * =============================================================================
 */

/**
 * @enum ra8_spi_b_target_test_t
 * @brief Local test constants.
 *
 * @details All integer literals required by the tests live here so the
 *          magic-number gate is satisfied.
 */
typedef enum : uint32_t {
  k_test_channel_0   = 0U,    /**< First SPI_B channel.           */
  k_test_channel_1   = 1U,    /**< Second SPI_B channel.          */
  k_test_channel_oor = 2U,    /**< Out-of-range channel index.    */
  k_test_tx_byte     = 0xA5U, /**< TX data for xfer tests.        */
  k_test_tx_alt      = 0x5AU, /**< Alternate TX byte for variety. */
} ra8_spi_b_target_test_t;

/* =============================================================================
 * Shared test setup
 * =============================================================================
 */

/**
 * @brief Reset fake state and re-enable MSTP between tests.
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  (void)ra8_mstp_init();
}

/**
 * @brief Configure a standard SPI_B target on channel 0.
 *
 * @details Shared helper used by multiple tests to bring the channel up in
 *          target mode with SPI mode 0 (CPOL=0, CPHA=0), MSB-first.
 */
static void init_target_ch0(void)
{
  const ra8_spi_cfg_t cfg = {
    .baud_hz   = 0U, /* irrelevant in target mode */
    .pclka_hz  = 0U, /* irrelevant in target mode */
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_b_target_init((uint8_t)k_test_channel_0, &cfg));
}

/* =============================================================================
 * Test 1: register image after init
 * =============================================================================
 */

/**
 * @test test_target_init_register_image
 * @brief SPCR image after init has MSTR=0, MODFEN=1, SPE=1.
 *
 * @details
 * After ``ra8_spi_b_target_init`` the SPCR register must satisfy:
 *  - MSTR (bit 30) == 0 -- peripheral/target role.
 *  - MODFEN (bit 14) == 1 -- mode fault error detection enabled.
 *  - SPE (bit 0) == 1 -- SPI function enabled.
 *  - SCKASE (bit 12) == 0 -- controller-only; must not be set in target mode.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- ra8_spi_b_target_init and the SPCR
 * builder internal_target_spcr contain no `&&`/`||`; it checks the MSTR / MODFEN
 * / SPE / SCKASE bits of the SPCR image after a successful init)
 */
static void test_target_init_register_image(void)
{
  TEST_BEGIN("spi_b_target: SPCR register image after init");
  prep();
  init_target_ch0();

  const uint32_t spcr = ra8_spi((uint8_t)k_test_channel_0)->SPCR;
  TEST_ASSERT((spcr & (uint32_t)k_ra8_spcr_mask_spe) != 0U);
  TEST_ASSERT((spcr & (uint32_t)k_ra8_spcr_mask_modfen) != 0U);
  TEST_ASSERT_EQ(0, (spcr & (uint32_t)k_ra8_spcr_mask_mstr));
  TEST_ASSERT_EQ(0, (spcr & (uint32_t)k_ra8_spcr_mask_sckase));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_deinit((uint8_t)k_test_channel_0));
  TEST_END("spi_b_target: SPCR register image after init");
}

/* =============================================================================
 * Test 2: happy-path xfer round-trip
 * =============================================================================
 */

/**
 * @test test_target_xfer_roundtrip
 * @brief Pre-seeded SPSR allows a successful polled target xfer.
 *
 * @details
 * Pre-stages SPSR with SPTEF | SPRF (both flags set). Under
 * ``RA8_OFF_TARGET`` the single-shot poll succeeds immediately.
 * The SPDR is verified to hold the TX byte after the pre-load write,
 * and the rx byte is populated (fake echoes TX because the TX
 * write to SPDR overwrites the same backing word as the RX read).
 *
 * @par Single-condition coverage (rx != nullptr):
 * V1: rx is non-NULL -> ``*rx`` is written (this test).
 * V2: rx is NULL     -> ``*rx`` is NOT written (test_target_xfer_null_rx).
 *
 * @par MC/DC:
 * (no compound decisions in this test -- neither ra8_spi_b_target_xfer nor the
 * ra8_hw_wait_flag_set32 it polls through has any `&&`/`||`; the only decision it
 * drives is the single-condition `rx != nullptr` guard, its true leg V1)
 */
static void test_target_xfer_roundtrip(void)
{
  TEST_BEGIN("spi_b_target: xfer roundtrip -- SPSR pre-staged");
  prep();
  init_target_ch0();

  ra8_spi((uint8_t)k_test_channel_0)->SPSR =
    (uint32_t)k_ra8_spsr_mask_sptef | (uint32_t)k_ra8_spsr_mask_sprf;

  uint8_t rx = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_spi_b_target_xfer((uint8_t)k_test_channel_0, (uint8_t)k_test_tx_byte, &rx));

  /* SPDR holds the last write (TX byte) in the plain-RAM fake. */
  TEST_ASSERT_EQ(k_test_tx_byte, ra8_spi((uint8_t)k_test_channel_0)->SPDR);

  /* Fake echoes TX as RX (single backing word for TX/RX FIFO). */
  TEST_ASSERT_EQ(k_test_tx_byte, rx);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_deinit((uint8_t)k_test_channel_0));
  TEST_END("spi_b_target: xfer roundtrip -- SPSR pre-staged");
}

/* =============================================================================
 * Test 3: null cfg rejected by init
 * =============================================================================
 */

/**
 * @test test_target_init_null_cfg
 * @brief NULL cfg pointer returns k_ra8_err_null_ptr.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it returns at the single-condition
 * RA8_CHECK_NULL_PTR(cfg) guard; ra8_spi_b_target_init has no `&&`/`||`)
 */
static void test_target_init_null_cfg(void)
{
  TEST_BEGIN("spi_b_target: init null cfg -> k_ra8_err_null_ptr");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_spi_b_target_init((uint8_t)k_test_channel_0, nullptr));
  TEST_END("spi_b_target: init null cfg -> k_ra8_err_null_ptr");
}

/* =============================================================================
 * Test 4: out-of-range channel rejected by init
 * =============================================================================
 */

/**
 * @test test_target_init_oor_channel
 * @brief Out-of-range channel returns k_ra8_err_invalid_arg from init.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it returns at the single-condition
 * `channel >= k_ra8_spi_b_channel_count` range guard; no `&&`/`||` on the path)
 */
static void test_target_init_oor_channel(void)
{
  TEST_BEGIN("spi_b_target: init OOR channel -> k_ra8_err_invalid_arg");
  prep();
  const ra8_spi_cfg_t cfg = {
    .baud_hz   = 0U,
    .pclka_hz  = 0U,
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = false,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_spi_b_target_init((uint8_t)k_test_channel_oor, &cfg));
  TEST_END("spi_b_target: init OOR channel -> k_ra8_err_invalid_arg");
}

/* =============================================================================
 * Test 5: out-of-range channel rejected by xfer
 * =============================================================================
 */

/**
 * @test test_target_xfer_oor_channel
 * @brief Out-of-range channel returns k_ra8_err_null_ptr from xfer.
 *
 * @details ra8_spi() returns nullptr for OOR; RA8_CHECK_NULL_PTR fires
 *          with k_ra8_err_null_ptr.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the single-condition
 * RA8_CHECK_NULL_PTR(reg) guard returns; ra8_spi_b_target_xfer has no `&&`/`||`)
 */
static void test_target_xfer_oor_channel(void)
{
  TEST_BEGIN("spi_b_target: xfer OOR channel -> k_ra8_err_null_ptr");
  prep();
  uint8_t rx = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_spi_b_target_xfer((uint8_t)k_test_channel_oor, (uint8_t)k_test_tx_byte, &rx));
  TEST_END("spi_b_target: xfer OOR channel -> k_ra8_err_null_ptr");
}

/* =============================================================================
 * Test 6: null rx pointer is accepted
 * =============================================================================
 */

/**
 * @test test_target_xfer_null_rx
 * @brief rx=NULL is accepted; received byte is silently discarded.
 *
 * @par Single-condition coverage (rx != nullptr):
 * V2: rx is NULL (complements V1 in test_target_xfer_roundtrip).
 *
 * @par MC/DC:
 * (no compound decisions in this test -- no `&&`/`||` in ra8_spi_b_target_xfer;
 * it supplies V2, the false leg, of the single-condition `rx != nullptr` guard)
 */
static void test_target_xfer_null_rx(void)
{
  TEST_BEGIN("spi_b_target: xfer null rx -> k_ra8_ok (discard)");
  prep();
  init_target_ch0();

  ra8_spi((uint8_t)k_test_channel_0)->SPSR =
    (uint32_t)k_ra8_spsr_mask_sptef | (uint32_t)k_ra8_spsr_mask_sprf;

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_spi_b_target_xfer((uint8_t)k_test_channel_0, (uint8_t)k_test_tx_alt, nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_deinit((uint8_t)k_test_channel_0));
  TEST_END("spi_b_target: xfer null rx -> k_ra8_ok (discard)");
}

/* =============================================================================
 * Test 7: timeout on SPTEF wait
 * =============================================================================
 */

/**
 * @test test_target_xfer_timeout_sptef
 * @brief An armed SPSR fault times out the first (SPTEF) wait.
 *
 * @details
 * ``internal_target_wait_spsr`` polls SPSR through ``ra8_hw_wait_flag_set32``,
 * whose bounded loop is consulted by the ra8_fake_mmio fault seam on the host
 * test build (T1-01). Arming the SPSR word with ``ra8_fake_mmio_fail_wait``
 * makes every poll report "not satisfied", so the SPTEF wait runs to its
 * budget and returns ``k_ra8_err_hw_timeout``; the xfer surfaces it on the
 * SPTEF leg -- the first SPSR poll.
 *
 * @par Single-condition coverage (internal_target_wait_spsr):
 * V2: wait never satisfied -> k_ra8_err_hw_timeout.
 * (V1: wait satisfied -> k_ra8_ok is covered by tests 2 and 6.)
 *
 * @par MC/DC:
 * (no compound decisions in this test -- internal_target_wait_spsr and
 * ra8_hw_wait_flag_set32 contain no `&&`/`||`; it drives the single-condition
 * wait-timeout leg (`err != k_ra8_ok` true) surfaced on the SPTEF poll)
 */
static void test_target_xfer_timeout_sptef(void)
{
  TEST_BEGIN("spi_b_target: SPSR wait armed to fail -> timeout on SPTEF");
  prep();
  init_target_ch0();

  /* Arm the SPSR word the target-mode wait polls so its bounded loop never
   * satisfies -> internal_target_wait_spsr returns k_ra8_err_hw_timeout. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_fake_mmio_fail_wait((const volatile void*)&ra8_spi((uint8_t)k_test_channel_0)->SPSR));

  uint8_t rx = 0U;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_spi_b_target_xfer((uint8_t)k_test_channel_0, (uint8_t)k_test_tx_byte, &rx));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_deinit((uint8_t)k_test_channel_0));
  TEST_END("spi_b_target: SPSR wait armed to fail -> timeout on SPTEF");
}

/* =============================================================================
 * Test 8: timeout on SPRF wait (SPTEF was set, SPRF was not)
 * =============================================================================
 */

/**
 * @test test_target_xfer_timeout_sprf
 * @brief A failing SPSR wait returns k_ra8_err_hw_timeout without writing rx.
 *
 * @details
 * Post-T1-01 the bounded SPSR poll is driven by the ra8_fake_mmio fault seam,
 * not by the register's RAM value. Both the SPTEF and the SPRF waits poll the
 * same SPSR word, and the seam keys a fault on the register address, so it
 * cannot fail the second poll of SPSR while letting the first succeed -- the
 * SPRF-specific leg is not isolable on host. Arming the SPSR word to fail
 * therefore exhausts the first (SPTEF) poll's budget; the xfer takes its
 * timeout early-return and never reaches the rx write, so ``rx`` keeps its
 * initial value. This case pins that timeout postcondition.
 *
 * @par Single-condition coverage (ra8_spi_b_target_xfer):
 * The wait-timeout early-return true leg (this test); the false leg is the
 * happy-path xfer (tests 2 and 6). The post-check confirms the rx-write guard
 * is skipped on the timeout path.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- no `&&`/`||` on the xfer path; it drives
 * the single-condition wait-timeout early-return true leg and checks the
 * rx-write guard is skipped)
 */
static void test_target_xfer_timeout_sprf(void)
{
  TEST_BEGIN("spi_b_target: SPSR wait armed to fail -> timeout, rx untouched");
  prep();
  init_target_ch0();

  /* Same SPSR word the target-mode wait polls, armed to fail. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_fake_mmio_fail_wait((const volatile void*)&ra8_spi((uint8_t)k_test_channel_0)->SPSR));

  uint8_t rx = k_t_rx_unset;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_spi_b_target_xfer((uint8_t)k_test_channel_0, (uint8_t)k_test_tx_byte, &rx));
  /* Timeout path must not touch the caller's rx byte. */
  TEST_ASSERT_EQ(0xEEU, rx);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_deinit((uint8_t)k_test_channel_0));
  TEST_END("spi_b_target: SPSR wait armed to fail -> timeout, rx untouched");
}

/* =============================================================================
 * Entry point
 * =============================================================================
 */

int32_t main(void)
{
  ra8_fake_mmap_reset();
  test_target_init_register_image();
  test_target_xfer_roundtrip();
  test_target_init_null_cfg();
  test_target_init_oor_channel();
  test_target_xfer_oor_channel();
  test_target_xfer_null_rx();
  test_target_xfer_timeout_sptef();
  test_target_xfer_timeout_sprf();
  (void)fprintf(stderr, "[OK ] test_ra8_spi_b_target.c\n");
  return 0;
}
