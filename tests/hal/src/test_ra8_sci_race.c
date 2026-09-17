/**
 * @file test_ra8_sci_race.c
 * @brief Concurrency-race regression tests for the SCI_B driver.
 *
 * @details
 * Covers recon finding T1-02: the runtime-reconfigure CCR2
 * read-modify-write (``ra8_sci_set_baud``) and the channel teardown
 * (``ra8_sci_deinit``) are wrapped in the existing ``ra8_register_guard``
 * IRQ-mask primitive so a TXI/RXI interrupt cannot tear the register or
 * the in-flight async descriptor mid-update. The guard is a host no-op
 * under ``RA8_OFF_TARGET``, so these tests assert the *result* of the
 * guarded sequence: the reconfigure must compute the correct final
 * CCR2 value while leaving the ISR-owned CCR0 untouched, and the
 * teardown must clear CCR0 plus every per-channel descriptor field so a
 * late ISR dispatch is a harmless no-op. Register state is observed
 * through the fake MMIO window provided by ``ra8_fake_mmap``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_sci.h"
#include "ra8_sci_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_race_byte_t
 * @brief Transmit payload and the byte staged in RDR.
 *
 * @details
 * The transmit bytes ascend by 0x11 so a byte delivered at the wrong index is
 * immediately identifiable; the receive byte is disjoint from that run so a
 * loopback that echoes the transmit buffer cannot pass by accident.
 */
typedef enum : uint8_t {
  k_t_tx_b0   = 0x11U, /**< Transmit payload byte 0.                */
  k_t_tx_b1   = 0x22U, /**< Transmit payload byte 1.                */
  k_t_tx_b2   = 0x33U, /**< Transmit payload byte 2.                */
  k_t_tx_b3   = 0x44U, /**< Transmit payload byte 3.                */
  k_t_rx_byte = 0xA5U, /**< Byte staged in RDR for the receive arm. */
} t_race_byte_t;

/**
 * @enum ra8_sci_race_channel_t
 * @brief Channel ids exercised by the race-regression tests.
 */
typedef enum : uint8_t {
  k_sci_race_ch_baud   = 7U,  /**< Channel for the set_baud test.   */
  k_sci_race_ch_deinit = 9U,  /**< Channel for the deinit test.     */
  k_sci_race_ch_bad    = 99U, /**< Out-of-range channel (rejected). */
} ra8_sci_race_channel_t;

/**
 * @enum ra8_sci_race_len_t
 * @brief Async transfer length used to arm in-flight RX/TX state.
 */
typedef enum : uint8_t {
  k_sci_race_len = 4U, /**< Bytes armed before teardown. */
} ra8_sci_race_len_t;

/**
 * @enum ra8_sci_race_const_t
 * @brief CCR2 seed pattern and baud/clock values for the tests.
 *
 * @details
 * ``k_sci_race_ccr2_seed`` deliberately sets bits both inside and
 * outside the BRR field (CCR2[15:8]) so a correct guarded reconfigure
 * is observable: the BRR byte must change while every other bit is
 * preserved.
 */
typedef enum : uint32_t {
  k_sci_race_ccr2_seed   = 0xFF0055AAUL, /**< MDDR=FF, BRR=55, low=AA seed. */
  k_sci_race_baud_target = 9600UL,       /**< Reconfigure target baud.      */
  k_sci_race_pclk_hz     = 60000000UL,   /**< PCLKB feeding the BRG.        */
  k_sci_race_baud_zero   = 0UL,          /**< Illegal baud (rejected).      */
} ra8_sci_race_const_t;

/**
 * @brief Standard 115200-8N1 descriptor used to open a channel.
 */
static const ra8_sci_cfg_t k_race_cfg = {
  .baud      = 115200U,
  .data_bits = k_ra8_sci_data_8,
  .parity    = k_ra8_sci_parity_none,
  .stop_bits = k_ra8_sci_stop_1,
  .pclk_hz   = 60000000U,
};

/**
 * @brief Reset the fake MMIO window and the MSTP model before a test.
 *
 * @details Mirrors the ``prep`` helper in ``test_ra8_sci.c``: a clean
 * peripheral RAM image plus an initialized module-stop model so every
 * register reads back the values the driver writes.
 * @pre The host MMIO substrate is linked into the test binary.
 * @pre ``ra8_mstp_init`` is safe to call repeatedly.
 * @post All SCI registers in the window read as zero.
 * @post The MSTP model is initialized.
 */
static void prep_race(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the guarded CCR2
 * read-modify-write result; the wrapped reconfigure adds no `&&` / `||`
 * decision of its own)
 */
static void test_set_baud_guarded_preserves_ccr2_and_ccr0(void)
{
  TEST_BEGIN("ra8_sci_set_baud: guarded RMW updates BRR, preserves CCR2/CCR0");
  prep_race();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init((uint8_t)k_sci_race_ch_baud, &k_race_cfg));

  volatile r_sci_regs_t* reg = ra8_sci((uint8_t)k_sci_race_ch_baud);
  TEST_ASSERT_NOT_NULL((void*)reg);
  /* Seed CCR2 with sentinel bits outside the BRR field, plus a bogus BRR. */
  reg->CCR2                  = (uint32_t)k_sci_race_ccr2_seed;
  const uint32_t ccr0_before = reg->CCR0;

  /* The driver always programs CKS=0, so the BRR it writes matches the
   * n=0 result reported by the public baud calculator. */
  uint16_t expect_brr = 0U;
  uint8_t  clk_div    = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_baud_calculate((uint32_t)k_sci_race_baud_target,
                                        (uint32_t)k_sci_race_pclk_hz,
                                        &expect_brr,
                                        &clk_div));
  TEST_ASSERT_EQ(0U, clk_div);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_set_baud((uint8_t)k_sci_race_ch_baud,
                                  (uint32_t)k_sci_race_baud_target,
                                  (uint32_t)k_sci_race_pclk_hz));

  const uint32_t brr_mask   = (uint32_t)k_ra8_sci_ccr2_mask_brr_field;
  const uint32_t ccr2_after = reg->CCR2;
  /* BRR field holds the recomputed value. */
  TEST_ASSERT_EQ(((uint32_t)expect_brr << (uint8_t)k_ra8_sci_ccr2_shift_brr),
                 (ccr2_after & brr_mask));
  /* Every non-BRR bit is byte-identical to the seed (no lost update). */
  TEST_ASSERT_EQ(((uint32_t)k_sci_race_ccr2_seed & ~brr_mask), (ccr2_after & ~brr_mask));
  /* CCR0 is owned by the TXI/RXI ISR; set_baud must not disturb it. */
  TEST_ASSERT_EQ(ccr0_before, reg->CCR0);
  TEST_END("ra8_sci_set_baud: guarded RMW updates BRR, preserves CCR2/CCR0");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the two single-condition
 * validation guards of ra8_sci_set_baud individually)
 */
static void test_set_baud_validation(void)
{
  TEST_BEGIN("ra8_sci_set_baud: bad channel + zero baud rejected");
  prep_race();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init((uint8_t)k_sci_race_ch_baud, &k_race_cfg));

  /* Out-of-range channel -> internal_reg() returns NULL. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sci_set_baud((uint8_t)k_sci_race_ch_bad,
                                  (uint32_t)k_sci_race_baud_target,
                                  (uint32_t)k_sci_race_pclk_hz));
  /* Zero baud rejected before any register access. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sci_set_baud((uint8_t)k_sci_race_ch_baud,
                                  (uint32_t)k_sci_race_baud_zero,
                                  (uint32_t)k_sci_race_pclk_hz));
  TEST_END("ra8_sci_set_baud: bad channel + zero baud rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the guarded teardown
 * result; the wrapped deinit adds no `&&` / `||` decision of its own)
 */
static void test_deinit_guarded_teardown_clears_ccr0_and_state(void)
{
  TEST_BEGIN("ra8_sci_deinit: guarded teardown clears CCR0 + async state");
  prep_race();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init((uint8_t)k_sci_race_ch_deinit, &k_race_cfg));

  /* Arm in-flight RX then TX so CCR0 carries TE|RE|RIE|TIE and the
   * per-channel descriptor holds live buffers/lengths. */
  uint8_t rx_buf[k_sci_race_len] = {};
  uint8_t tx_buf[k_sci_race_len] = {k_t_tx_b0, k_t_tx_b1, k_t_tx_b2, k_t_tx_b3};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_read((uint8_t)k_sci_race_ch_deinit, rx_buf, (uint32_t)k_sci_race_len));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_write((uint8_t)k_sci_race_ch_deinit, tx_buf, (uint32_t)k_sci_race_len));

  volatile r_sci_regs_t* reg = ra8_sci((uint8_t)k_sci_race_ch_deinit);
  TEST_ASSERT((reg->CCR0 & (1U << (uint8_t)k_ra8_sci_ccr0_bit_rie)) != 0U);
  TEST_ASSERT((reg->CCR0 & (1U << (uint8_t)k_ra8_sci_ccr0_bit_tie)) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_deinit((uint8_t)k_sci_race_ch_deinit));

  /* CCR0 fully cleared (TE/RE/RIE/TIE all dropped) by the guarded path. */
  TEST_ASSERT_EQ(0U, reg->CCR0);
  /* MSTP gate re-asserted. */
  bool stopped = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_is_stopped(k_ra8_mstp_sci9, &stopped));
  TEST_ASSERT(stopped);

  /* The descriptor is fully torn down: a late ISR dispatch must be a
   * harmless no-op rather than a write through a half-nulled buffer. */
  reg->RDR = k_t_rx_byte;
  ra8_sci_dispatch_rxi((uint8_t)k_sci_race_ch_deinit);
  ra8_sci_dispatch_txi((uint8_t)k_sci_race_ch_deinit);
  TEST_ASSERT_EQ(0U, reg->CCR0);
  TEST_ASSERT_EQ(0U, rx_buf[0]);

  /* initialized cleared -> async API now rejects the channel. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sci_read((uint8_t)k_sci_race_ch_deinit, rx_buf, (uint32_t)k_sci_race_len));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sci_write((uint8_t)k_sci_race_ch_deinit, tx_buf, (uint32_t)k_sci_race_len));
  TEST_END("ra8_sci_deinit: guarded teardown clears CCR0 + async state");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the single-condition
 * bad-channel guard of ra8_sci_deinit)
 */
static void test_deinit_validation(void)
{
  TEST_BEGIN("ra8_sci_deinit: bad channel rejected");
  prep_race();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_deinit((uint8_t)k_sci_race_ch_bad));
  TEST_END("ra8_sci_deinit: bad channel rejected");
}

int main(void)
{
  test_set_baud_guarded_preserves_ccr2_and_ccr0();
  test_set_baud_validation();
  test_deinit_guarded_teardown_clears_ccr0_and_state();
  test_deinit_validation();
  return 0;
}
