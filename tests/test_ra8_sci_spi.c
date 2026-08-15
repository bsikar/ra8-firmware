/**
 * @file test_ra8_sci_spi.c
 * @brief Unit tests for the SCI Simple-SPI controller driver
 *        (libs/ra8_hal/src/ra8_sci_spi.c).
 *
 * @details
 * Exercises all five public entry points of the SCI_B Simple-SPI driver
 * against the ``ra8_fake_mmap``-backed register window:
 *
 *  - ``ra8_sci_spi_init``      -- MSTP enable + Simple-SPI CCR programming;
 *  - ``ra8_sci_spi_deinit``    -- TE/RE clear + MSTP release;
 *  - ``ra8_sci_spi_set_clock`` -- runtime CCR2 (CKS/BRR) reprogramme;
 *  - ``ra8_sci_spi_xfer8``     -- single-frame full-duplex exchange;
 *  - ``ra8_sci_spi_xfer``      -- multi-byte full-duplex exchange.
 *
 * Determinism note: the xfer paths poll CSR.TDRE then CSR.RDRF via the
 * bounded ``ra8_hw_wait_flag_set32`` helper. The fake backs MMIO with
 * ordinary RAM, so a test makes the poll succeed simply by pre-seeding the
 * relevant status bits in CSR before the call -- the very first read then
 * observes "ready" and the loop body runs. Timeout cases are produced by
 * leaving the awaited bit clear, which lets the bounded loop run to its
 * budget and return ``k_ra8_err_hw_timeout``. No SIGALRM / setitimer
 * injection is used anywhere in this file.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_mstp.h"
#include "ra8_sci_regs.h"
#include "ra8_sci_spi.h"
#include "unity_minimal.h"

/**
 * @enum ra8_sci_spi_test_const_t
 * @brief Channel ids, rates, and frame bytes used by the tests.
 */
typedef enum : uint32_t {
  k_spi_test_ch        = 0U,        /**< SCI0 routes Simple-SPI on the EVM. */
  k_spi_test_ch_alt    = 3U,        /**< Second channel for deinit tests.   */
  k_spi_test_ch_bad    = 99U,       /**< Out-of-range channel (rejected).   */
  k_spi_test_baud      = 1000000U,  /**< Normal 1 MHz bit-rate.             */
  k_spi_test_pclk      = 60000000U, /**< PCLKA feeding the BRG.             */
  k_spi_test_baud_slow = 1U,        /**< Unreachably slow -> clamp path.    */
  k_spi_test_baud_fast = 20000000U, /**< Exceeds CKS=0 ceiling -> BRR=0.    */
  k_spi_test_baud_zero = 0U,        /**< Illegal baud (rejected).           */
} ra8_sci_spi_test_const_t;

/**
 * @enum ra8_sci_spi_test_byte_t
 * @brief Frame bytes exchanged by the xfer tests.
 */
typedef enum : uint8_t {
  k_spi_test_tx_byte = 0xA5U, /**< Byte shifted out on COPI.     */
  k_spi_test_rx_byte = 0x5AU, /**< Byte the fake returns in RDR. */
} ra8_sci_spi_test_byte_t;

/**
 * @brief Standard mode-0 / 1 MHz Simple-SPI descriptor.
 */
static const ra8_sci_spi_cfg_t k_spi_cfg = {
  .baud_hz   = (uint32_t)k_spi_test_baud,
  .pclk_hz   = (uint32_t)k_spi_test_pclk,
  .mode      = k_ra8_spi_mode_0,
  .lsb_first = false,
};

/**
 * @brief Reset the fake MMIO window and the MSTP model before a test.
 *
 * @details Mirrors the ``prep`` helper in the sibling SCI tests: a clean
 * peripheral RAM image plus an initialized module-stop model so every
 * register reads back the values the driver writes.
 * @pre The host MMIO substrate is linked into the test binary.
 * @pre ``ra8_mstp_init`` is safe to call repeatedly.
 * @post All SCI registers in the window read as zero.
 * @post No MMIO wait fault is armed (the T1-01 seam is transparent).
 * @post The MSTP model is initialized.
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  (void)ra8_mstp_init();
}

/**
 * @brief Pre-seed CSR so the TDRE + RDRF polls both succeed immediately.
 *
 * @details The Simple-SPI xfer waits for TDRE (transmit empty) then RDRF
 * (receive full). Setting both bits in the RAM-backed CSR makes the first
 * read of each poll observe "ready", so the loop body executes without any
 * timer injection.
 * @param[in] channel SCI channel to seed.
 * @pre ``channel`` is a valid index whose window is mapped.
 * @post CSR for ``channel`` has TDRE and RDRF set.
 */
static void seed_ready(uint8_t channel)
{
  volatile r_sci_regs_t* reg = ra8_sci(channel);
  reg->CSR = (1U << (uint8_t)k_ra8_sci_csr_bit_tdre) | (1U << (uint8_t)k_ra8_sci_csr_bit_rdrf);
}

/* =========================================================================
 * ra8_sci_spi_init
 * =========================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the happy-path init
 * contract; the driver's init guards are single-condition `if` checks)
 */
static void test_init_happy_path(void)
{
  TEST_BEGIN("ra8_sci_spi_init: mode 0 enables TE+RE in Simple-SPI mode");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_spi_init((uint8_t)k_spi_test_ch, &k_spi_cfg));

  volatile const r_sci_regs_t* reg = ra8_sci((uint8_t)k_spi_test_ch);
  /* CCR0 must enable both transmit and receive. */
  TEST_ASSERT((reg->CCR0 & (1U << (uint8_t)k_ra8_sci_ccr0_bit_te)) != 0U);
  TEST_ASSERT((reg->CCR0 & (1U << (uint8_t)k_ra8_sci_ccr0_bit_re)) != 0U);
  /* CCR3.MOD must select Simple-SPI (011b). */
  const uint32_t mod = (reg->CCR3 >> (uint32_t)k_ra8_sci_ccr3_shift_mod) & 0x7U;
  TEST_ASSERT_EQ(k_ra8_sci_ccr3_mod_simple_spi, mod);
  /* Mode 0 leaves CPOL + CPHA clear and (lsb_first=false) LSBF clear. */
  TEST_ASSERT_EQ(0, (reg->CCR3 & (1U << (uint8_t)k_ra8_sci_ccr3_bit_cpol)));
  TEST_ASSERT_EQ(0, (reg->CCR3 & (1U << (uint8_t)k_ra8_sci_ccr3_bit_cpha)));
  TEST_ASSERT_EQ(0, (reg->CCR3 & (1U << (uint8_t)k_ra8_sci_ccr3_bit_lsbf)));
  TEST_END("ra8_sci_spi_init: mode 0 enables TE+RE in Simple-SPI mode");
}

/**
 * @par MC/DC:
 * (no compound decisions added here -- drives the three single-condition
 * mode bits in internal_ccr3 by selecting mode 3 + LSB-first so CPOL,
 * CPHA and LSBF are all asserted in one config)
 */
static void test_init_mode3_lsb_first(void)
{
  TEST_BEGIN("ra8_sci_spi_init: mode 3 + LSB-first sets CPOL, CPHA, LSBF");
  prep();

  const ra8_sci_spi_cfg_t cfg = {
    .baud_hz   = (uint32_t)k_spi_test_baud,
    .pclk_hz   = (uint32_t)k_spi_test_pclk,
    .mode      = k_ra8_spi_mode_3,
    .lsb_first = true,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_spi_init((uint8_t)k_spi_test_ch, &cfg));

  volatile const r_sci_regs_t* reg = ra8_sci((uint8_t)k_spi_test_ch);
  TEST_ASSERT((reg->CCR3 & (1U << (uint8_t)k_ra8_sci_ccr3_bit_cpol)) != 0U);
  TEST_ASSERT((reg->CCR3 & (1U << (uint8_t)k_ra8_sci_ccr3_bit_cpha)) != 0U);
  TEST_ASSERT((reg->CCR3 & (1U << (uint8_t)k_ra8_sci_ccr3_bit_lsbf)) != 0U);
  TEST_END("ra8_sci_spi_init: mode 3 + LSB-first sets CPOL, CPHA, LSBF");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the three independent
 * single-condition init rejection guards: null cfg, bad channel, zero baud)
 */
static void test_init_rejects(void)
{
  TEST_BEGIN("ra8_sci_spi_init: null cfg / bad channel / zero baud rejected");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sci_spi_init((uint8_t)k_spi_test_ch, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_spi_init((uint8_t)k_spi_test_ch_bad, &k_spi_cfg));

  const ra8_sci_spi_cfg_t cfg_zero = {
    .baud_hz   = (uint32_t)k_spi_test_baud_zero,
    .pclk_hz   = (uint32_t)k_spi_test_pclk,
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = false,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_spi_init((uint8_t)k_spi_test_ch, &cfg_zero));
  TEST_END("ra8_sci_spi_init: null cfg / bad channel / zero baud rejected");
}

/* =========================================================================
 * ra8_sci_spi_set_clock (drives internal_resolve_rate across all branches)
 * =========================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- walks the bit-rate resolver
 * through its normal-divider, clamp-slow, and exceeds-ceiling branches)
 */
static void test_set_clock_rate_branches(void)
{
  TEST_BEGIN("ra8_sci_spi_set_clock: normal / clamp / fast rate resolution");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_spi_init((uint8_t)k_spi_test_ch, &k_spi_cfg));

  volatile r_sci_regs_t* reg = ra8_sci((uint8_t)k_spi_test_ch);

  /* Clear CCR2 so set_clock's write is observable: ra8_sci_spi_init already */
  /* programmed it for the same cfg baud, so comparing against that value */
  /* would be a no-op. */
  reg->CCR2 = 0U;
  /* Normal divider: a reachable rate yields N <= 255 at some CKS. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_spi_set_clock((uint8_t)k_spi_test_ch,
                                       (uint32_t)k_spi_test_baud,
                                       (uint32_t)k_spi_test_pclk));
  TEST_ASSERT(reg->CCR2 != 0U);

  /* Unreachably slow: even CKS=3 overflows BRR, so the resolver clamps. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_spi_set_clock((uint8_t)k_spi_test_ch,
                                       (uint32_t)k_spi_test_baud_slow,
                                       (uint32_t)k_spi_test_pclk));

  /* Faster than the CKS=0 ceiling: the resolver picks BRR = 0. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_spi_set_clock((uint8_t)k_spi_test_ch,
                                       (uint32_t)k_spi_test_baud_fast,
                                       (uint32_t)k_spi_test_pclk));
  TEST_END("ra8_sci_spi_set_clock: normal / clamp / fast rate resolution");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the two single-condition
 * set_clock validation guards individually)
 */
static void test_set_clock_rejects(void)
{
  TEST_BEGIN("ra8_sci_spi_set_clock: bad channel + zero baud rejected");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_spi_init((uint8_t)k_spi_test_ch, &k_spi_cfg));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sci_spi_set_clock((uint8_t)k_spi_test_ch_bad,
                                       (uint32_t)k_spi_test_baud,
                                       (uint32_t)k_spi_test_pclk));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sci_spi_set_clock((uint8_t)k_spi_test_ch,
                                       (uint32_t)k_spi_test_baud_zero,
                                       (uint32_t)k_spi_test_pclk));
  TEST_END("ra8_sci_spi_set_clock: bad channel + zero baud rejected");
}

/* =========================================================================
 * ra8_sci_spi_deinit
 * =========================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the deinit happy path
 * and the single-condition bad-channel guard)
 */
static void test_deinit(void)
{
  TEST_BEGIN("ra8_sci_spi_deinit: clears CCR0 and releases MSTP");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_spi_init((uint8_t)k_spi_test_ch_alt, &k_spi_cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_spi_deinit((uint8_t)k_spi_test_ch_alt));
  volatile const r_sci_regs_t* reg = ra8_sci((uint8_t)k_spi_test_ch_alt);
  TEST_ASSERT_EQ(0U, reg->CCR0);

  bool stopped = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_is_stopped(k_ra8_mstp_sci3, &stopped));
  TEST_ASSERT(stopped);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_spi_deinit((uint8_t)k_spi_test_ch_bad));
  TEST_END("ra8_sci_spi_deinit: clears CCR0 and releases MSTP");
}

/* =========================================================================
 * ra8_sci_spi_xfer8
 * =========================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- a single full-duplex frame with
 * the status flags pre-seeded so both polls succeed immediately)
 */
static void test_xfer8_round_trip(void)
{
  TEST_BEGIN("ra8_sci_spi_xfer8: full-duplex frame round trip");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_spi_init((uint8_t)k_spi_test_ch, &k_spi_cfg));

  volatile r_sci_regs_t* reg = ra8_sci((uint8_t)k_spi_test_ch);
  seed_ready((uint8_t)k_spi_test_ch);
  reg->RDR = (uint32_t)k_spi_test_rx_byte;

  uint8_t rx = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_spi_xfer8((uint8_t)k_spi_test_ch, (uint8_t)k_spi_test_tx_byte, &rx));
  /* The transmitted byte landed in TDR and the received byte came from RDR. */
  TEST_ASSERT_EQ(k_spi_test_tx_byte, (reg->TDR & 0xFFU));
  TEST_ASSERT_EQ(k_spi_test_rx_byte, rx);

  /* rx == NULL discards the received byte but still succeeds. */
  seed_ready((uint8_t)k_spi_test_ch);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_spi_xfer8((uint8_t)k_spi_test_ch, (uint8_t)k_spi_test_tx_byte, nullptr));
  TEST_END("ra8_sci_spi_xfer8: full-duplex frame round trip");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the channel-out-of-range
 * guard via the NULL register pointer returned by ra8_sci(99))
 */
static void test_xfer8_bad_channel(void)
{
  TEST_BEGIN("ra8_sci_spi_xfer8: bad channel rejected");
  prep();
  uint8_t rx = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_sci_spi_xfer8((uint8_t)k_spi_test_ch_bad, (uint8_t)k_spi_test_tx_byte, &rx));
  TEST_END("ra8_sci_spi_xfer8: bad channel rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the TDRE poll never sees the flag,
 * so the bounded wait expires and the first poll reports a timeout)
 */
static void test_xfer8_timeout_tdre(void)
{
  TEST_BEGIN("ra8_sci_spi_xfer8: TDRE poll times out");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_spi_init((uint8_t)k_spi_test_ch, &k_spi_cfg));

  /* The xfer polls ra8_hw_wait_flag_set32(&reg->CSR, ...) for TDRE. Arm that CSR
   * wait to fail (T1-01 seam) so the bounded transmit-empty poll runs to its
   * budget and returns a timeout -- an unstaged flag alone no longer times out. */
  ra8_sci((uint8_t)k_spi_test_ch)->CSR = 0U;
  uint8_t rx                           = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_fake_mmio_fail_wait((const volatile void*)&ra8_sci((uint8_t)k_spi_test_ch)->CSR));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_sci_spi_xfer8((uint8_t)k_spi_test_ch, (uint8_t)k_spi_test_tx_byte, &rx));
  TEST_END("ra8_sci_spi_xfer8: TDRE poll times out");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- both the TDRE and RDRF polls key on
 * ra8_sci(ch)->CSR, so arming that CSR wait to fail forces a deterministic
 * timeout even though a TDRE bit is staged: the armed fault overrides the
 * stray staged value, which is the seam behaviour this case pins)
 */
static void test_xfer8_timeout_rdrf(void)
{
  TEST_BEGIN("ra8_sci_spi_xfer8: CSR poll times out despite a staged bit");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_spi_init((uint8_t)k_spi_test_ch, &k_spi_cfg));

  /* TDRE staged set, but the CSR wait is armed to fail: the armed fault ignores
   * the staged bit, so the bounded poll runs to its budget and returns a
   * timeout (both CSR polls in the xfer share this same register key). */
  ra8_sci((uint8_t)k_spi_test_ch)->CSR = (1U << (uint8_t)k_ra8_sci_csr_bit_tdre);
  uint8_t rx                           = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_fake_mmio_fail_wait((const volatile void*)&ra8_sci((uint8_t)k_spi_test_ch)->CSR));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_sci_spi_xfer8((uint8_t)k_spi_test_ch, (uint8_t)k_spi_test_tx_byte, &rx));
  TEST_END("ra8_sci_spi_xfer8: CSR poll times out despite a staged bit");
}

/* =========================================================================
 * ra8_sci_spi_xfer
 * =========================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- a multi-byte exchange drives the
 * per-frame loop with both buffers, then with a NULL tx (idle fill) and a
 * NULL rx (discard))
 */
static void test_xfer_multibyte(void)
{
  TEST_BEGIN("ra8_sci_spi_xfer: multi-byte full-duplex loop");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_spi_init((uint8_t)k_spi_test_ch, &k_spi_cfg));

  volatile r_sci_regs_t* reg = ra8_sci((uint8_t)k_spi_test_ch);
  /* Both flags stay set across frames (the fake does not auto-clear W1C),
   * so a single seed covers every iteration of the loop. */
  seed_ready((uint8_t)k_spi_test_ch);
  reg->RDR = (uint32_t)k_spi_test_rx_byte;

  const uint8_t tx[4] = {0x11U, 0x22U, 0x33U, 0x44U};
  uint8_t       rx[4] = {0U, 0U, 0U, 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_spi_xfer((uint8_t)k_spi_test_ch, tx, rx, 4U));
  /* Last byte transmitted is the final TDR write; every rx slot got RDR. */
  TEST_ASSERT_EQ(0x44U, (reg->TDR & 0xFFU));
  for (uint32_t i = 0U; i < 4U; ++i) {
    TEST_ASSERT_EQ(k_spi_test_rx_byte, rx[i]);
  }

  /* NULL tx -> idle 0xFF fill; rx still captured. */
  seed_ready((uint8_t)k_spi_test_ch);
  reg->RDR       = (uint32_t)k_spi_test_rx_byte;
  uint8_t rx2[3] = {0U, 0U, 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_spi_xfer((uint8_t)k_spi_test_ch, nullptr, rx2, 3U));
  TEST_ASSERT_EQ(0xFFU, (reg->TDR & 0xFFU));
  TEST_ASSERT_EQ(k_spi_test_rx_byte, rx2[0]);

  /* NULL rx -> received bytes discarded; transfer still succeeds. */
  seed_ready((uint8_t)k_spi_test_ch);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_spi_xfer((uint8_t)k_spi_test_ch, tx, nullptr, 2U));
  TEST_END("ra8_sci_spi_xfer: multi-byte full-duplex loop");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- a zero-length transfer is a no-op
 * success and a bad channel is rejected before the loop)
 */
static void test_xfer_len_zero_and_bad_channel(void)
{
  TEST_BEGIN("ra8_sci_spi_xfer: len 0 no-op + bad channel rejected");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_spi_init((uint8_t)k_spi_test_ch, &k_spi_cfg));

  uint8_t       rx[1] = {0U};
  const uint8_t tx[1] = {0x77U};
  /* len == 0 returns success without touching the bus. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_spi_xfer((uint8_t)k_spi_test_ch, tx, rx, 0U));
  /* Bad channel is rejected up-front. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_spi_xfer((uint8_t)k_spi_test_ch_bad, tx, rx, 1U));
  TEST_END("ra8_sci_spi_xfer: len 0 no-op + bad channel rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the per-frame xfer8 times out and
 * ra8_sci_spi_xfer forwards that error, exercising its error-propagation
 * branch)
 */
static void test_xfer_propagates_timeout(void)
{
  TEST_BEGIN("ra8_sci_spi_xfer: forwards per-frame timeout");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_spi_init((uint8_t)k_spi_test_ch, &k_spi_cfg));

  /* CSR all-zero -> the first frame's TDRE poll times out. Arm the seam on the
   * CSR the per-frame wait polls: unarmed waits now succeed, so the timeout is
   * driven deterministically via fail_wait rather than a cleared RAM flag. */
  ra8_sci((uint8_t)k_spi_test_ch)->CSR = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_fake_mmio_fail_wait((const volatile void*)&ra8_sci((uint8_t)k_spi_test_ch)->CSR));
  const uint8_t tx[2] = {0x01U, 0x02U};
  uint8_t       rx[2] = {0U, 0U};
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_sci_spi_xfer((uint8_t)k_spi_test_ch, tx, rx, 2U));
  TEST_END("ra8_sci_spi_xfer: forwards per-frame timeout");
}

int32_t main(void)
{
  test_init_happy_path();
  test_init_mode3_lsb_first();
  test_init_rejects();
  test_set_clock_rate_branches();
  test_set_clock_rejects();
  test_deinit();
  test_xfer8_round_trip();
  test_xfer8_bad_channel();
  test_xfer8_timeout_tdre();
  test_xfer8_timeout_rdrf();
  test_xfer_multibyte();
  test_xfer_len_zero_and_bad_channel();
  test_xfer_propagates_timeout();
  return 0;
}
