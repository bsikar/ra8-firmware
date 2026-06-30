/**
 * @file test_ra_sci_lin.c
 * @brief Unit tests for the LIN commander driver (libs/ra_hal/src/ra_sci_lin.c).
 *
 * @details
 * Exercises ``ra_sci_lin_init`` (the Simple-LIN register image: CCR3.MOD,
 * XCR0 break-field enable + timer clock, XCR2 break length, CCR0 TE/RE),
 * the break / header emission path (XCR1.TCST trigger and the TDR byte
 * sequence observed through the simulated MMIO window), and the pure PID
 * parity + classic / enhanced checksum helpers against known LIN vectors.
 * Status-flag polls are driven by pre-seeding CSR.TDRE; no SIGALRM
 * injection is used.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_sci_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sci.h"
#include "ra_sci_lin.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_lin_test_channel_t
 * @brief Channel ids used by the LIN tests.
 */
typedef enum : uint8_t {
  k_lin_test_channel     = 2U,  /**< Valid SCI channel under test. */
  k_lin_test_channel_bad = 99U, /**< Far out-of-range channel.     */
} ra_lin_test_channel_t;

/**
 * @enum ra_lin_test_const_t
 * @brief Register-image expectations and PID/checksum vectors.
 */
typedef enum : uint32_t {
  k_lin_test_break_len  = 0x00A0U,     /**< XCR2.BFLW value programmed. */
  k_lin_test_xcr2_image = 0x00A00000U, /**< break_len << 16.            */
  k_lin_test_tcss_div16 = 0x2U,        /**< TCSS field for div16.       */
  k_lin_test_ccr0_te_re = 0x00000011U, /**< CCR0 = TE(bit4) | RE(bit0). */
  k_lin_test_bad_break  = 0xFFFFU,     /**< Prohibited BFLW (0xFFFF).   */
  k_lin_test_bad_clk_lo = 0U,          /**< timer_clk below div4.       */
  k_lin_test_bad_clk_hi = 4U,          /**< timer_clk above div64.      */
} ra_lin_test_const_t;

/**
 * @enum ra_lin_test_pid_t
 * @brief Known LIN id -> protected-identifier vectors.
 *
 * @details PID = id | (P0 << 6) | (P1 << 7), P0 = ID0^ID1^ID2^ID4,
 * P1 = NOT(ID1^ID3^ID4^ID5). These six pairs are the canonical LIN
 * examples (including the 0x3C/0x3D diagnostic frame ids).
 */
typedef enum : uint8_t {
  k_lin_id_00     = 0x00U,
  k_lin_pid_00    = 0x80U, /**< id 0  -> 0x80. */
  k_lin_id_01     = 0x01U,
  k_lin_pid_01    = 0xC1U, /**< id 1  -> 0xC1. */
  k_lin_id_02     = 0x02U,
  k_lin_pid_02    = 0x42U, /**< id 2  -> 0x42. */
  k_lin_id_03     = 0x03U,
  k_lin_pid_03    = 0x03U, /**< id 3  -> 0x03. */
  k_lin_id_3c     = 0x3CU,
  k_lin_pid_3c    = 0x3CU, /**< id 60 -> 0x3C. */
  k_lin_id_3d     = 0x3DU,
  k_lin_pid_3d    = 0x7DU, /**< id 61 -> 0x7D.           */
  k_lin_id_masked = 0xBCU, /**< 0xBC & 0x3F = 0x3C.      */
  k_lin_id_bad    = 0x40U, /**< 64: above the 6-bit max. */
} ra_lin_test_pid_t;

/**
 * @enum ra_lin_test_csum_t
 * @brief Known LIN checksum vectors (LIN 2.0 spec example + a simple one).
 */
typedef enum : uint8_t {
  k_lin_csum_pid       = 0x4AU, /**< Example PID for the enhanced sum.   */
  k_lin_csum_classic_a = 0x31U, /**< classic {0x55,0x93,0xE5}.           */
  k_lin_csum_enhanced  = 0xE6U, /**< enhanced PID 0x4A {0x55,0x93,0xE5}. */
  k_lin_csum_classic_b = 0xF5U, /**< classic {0x01,0x02,0x03,0x04}.      */
  k_lin_csum_empty     = 0xFFU, /**< classic, zero-length data.          */
} ra_lin_test_csum_t;

static const uint8_t s_csum_data_a[] = {0x55U, 0x93U, 0xE5U};
static const uint8_t s_csum_data_b[] = {0x01U, 0x02U, 0x03U, 0x04U};

static const ra_sci_lin_cfg_t k_lin_cfg = {
  .uart =
    {
      .baud      = 19200U,
      .data_bits = k_ra_sci_data_8,
      .parity    = k_ra_sci_parity_none,
      .stop_bits = k_ra_sci_stop_1,
      .pclk_hz   = 60000000U,
    },
  .timer_clk       = k_ra_sci_lin_clk_div16,
  .break_field_len = (uint16_t)k_lin_test_break_len,
};

/**
 * @brief Reset the simulated MMIO window and the MSTP model before a test.
 *
 * @pre The host MMIO substrate is linked into the test binary.
 * @pre ``ra_mstp_init`` is safe to call repeatedly.
 * @post All SCI registers in the window read as zero.
 * @post The MSTP model is initialized.
 */
static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the LIN init register
 * image: CCR3.MOD, XCR0 BFE + TCSS, XCR2 BFLW, CCR0 TE/RE)
 */
static void test_lin_init_register_image(void)
{
  TEST_BEGIN("ra_sci_lin_init: Simple-LIN register image");
  prep();

  TEST_ASSERT_EQ(k_ra_ok, ra_sci_lin_init((uint8_t)k_lin_test_channel, &k_lin_cfg));
  volatile const r_sci_regs_t* reg = ra_sci((uint8_t)k_lin_test_channel);
  TEST_ASSERT_NOT_NULL((void*)reg);

  /* CCR3.MOD == Simple LIN (110b), framing bits preserved from ra_sci_init. */
  const uint32_t mod =
    (reg->CCR3 & (uint32_t)k_ra_sci_ccr3_mask_mod) >> (uint8_t)k_ra_sci_ccr3_shift_mod;
  TEST_ASSERT_EQ((uint32_t)k_ra_sci_ccr3_mod_simple_lin, mod);
  TEST_ASSERT((reg->CCR3 & (1U << (uint8_t)k_ra_sci_ccr3_bit_lsbf)) != 0U);
  TEST_ASSERT((reg->CCR3 & (1U << (uint8_t)k_ra_sci_ccr3_bit_bpen)) != 0U);

  /* XCR0: break-field enable + TCSS = div16. */
  TEST_ASSERT((reg->XCR0 & (1U << (uint8_t)k_ra_sci_xcr0_bit_bfe)) != 0U);
  TEST_ASSERT_EQ((uint32_t)k_lin_test_tcss_div16,
                 reg->XCR0 &
                   ((uint32_t)k_ra_sci_xcr0_tcss_div64 << (uint8_t)k_ra_sci_xcr0_shift_tcss));

  /* XCR2: break-field length in BFLW[31:16]. */
  TEST_ASSERT_EQ((uint32_t)k_lin_test_xcr2_image, reg->XCR2);

  /* CCR0: transmitter + receiver re-enabled after the mode switch. */
  TEST_ASSERT_EQ((uint32_t)k_lin_test_ccr0_te_re, reg->CCR0);
  TEST_END("ra_sci_lin_init: Simple-LIN register image");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the four independent
 * validation guards of ra_sci_lin_init, each on its own call)
 */
static void test_lin_init_validation(void)
{
  TEST_BEGIN("ra_sci_lin_init: validation guards");
  prep();

  /* NULL cfg. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_sci_lin_init((uint8_t)k_lin_test_channel, nullptr));
  /* Out-of-range channel. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_sci_lin_init((uint8_t)k_lin_test_channel_bad, &k_lin_cfg));

  /* Prohibited break-field length (0xFFFF). */
  ra_sci_lin_cfg_t bad_break = k_lin_cfg;
  bad_break.break_field_len  = (uint16_t)k_lin_test_bad_break;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_sci_lin_init((uint8_t)k_lin_test_channel, &bad_break));

  /* timer_clk below the lowest divider. */
  ra_sci_lin_cfg_t bad_clk_lo = k_lin_cfg;
  bad_clk_lo.timer_clk        = (ra_sci_lin_timer_clk_t)k_lin_test_bad_clk_lo;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_sci_lin_init((uint8_t)k_lin_test_channel, &bad_clk_lo));

  /* timer_clk above the highest divider. */
  ra_sci_lin_cfg_t bad_clk_hi = k_lin_cfg;
  bad_clk_hi.timer_clk        = (ra_sci_lin_timer_clk_t)k_lin_test_bad_clk_hi;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_sci_lin_init((uint8_t)k_lin_test_channel, &bad_clk_hi));
  TEST_END("ra_sci_lin_init: validation guards");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the break-field
 * trigger write to XCR1.TCST and the bad-channel guard)
 */
static void test_lin_send_break(void)
{
  TEST_BEGIN("ra_sci_lin_send_break: XCR1.TCST trigger");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_sci_lin_init((uint8_t)k_lin_test_channel, &k_lin_cfg));

  TEST_ASSERT_EQ(k_ra_ok, ra_sci_lin_send_break((uint8_t)k_lin_test_channel));
  volatile const r_sci_regs_t* reg = ra_sci((uint8_t)k_lin_test_channel);
  TEST_ASSERT((reg->XCR1 & (1U << (uint8_t)k_ra_sci_xcr1_bit_tcst)) != 0U);

  /* Bad channel -> null-guard. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_sci_lin_send_break((uint8_t)k_lin_test_channel_bad));
  TEST_END("ra_sci_lin_send_break: XCR1.TCST trigger");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the header happy path:
 * break trigger + final PID byte landing in TDR)
 */
static void test_lin_send_header_sequence(void)
{
  TEST_BEGIN("ra_sci_lin_send_header: break + sync + PID");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_sci_lin_init((uint8_t)k_lin_test_channel, &k_lin_cfg));

  /* Pre-seed CSR.TDRE so each putc poll completes on the first iteration. */
  volatile r_sci_regs_t* reg = ra_sci((uint8_t)k_lin_test_channel);
  reg->CSR                   = (1U << (uint8_t)k_ra_sci_csr_bit_tdre);

  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sci_lin_send_header((uint8_t)k_lin_test_channel, (uint8_t)k_lin_id_3d));
  /* Break field was triggered. */
  TEST_ASSERT((reg->XCR1 & (1U << (uint8_t)k_ra_sci_xcr1_bit_tcst)) != 0U);
  /* TDR holds the last byte written: the protected identifier of 0x3D. */
  TEST_ASSERT_EQ((uint32_t)k_lin_pid_3d, reg->TDR & (uint32_t)k_ra_sci_tdr_mask_data8);
  TEST_END("ra_sci_lin_send_header: break + sync + PID");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the header validation
 * guards: bad channel, out-of-range id, and a TDRE-poll timeout)
 */
static void test_lin_send_header_guards(void)
{
  TEST_BEGIN("ra_sci_lin_send_header: guards + timeout");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_sci_lin_init((uint8_t)k_lin_test_channel, &k_lin_cfg));

  /* Bad channel -> null-guard. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_sci_lin_send_header((uint8_t)k_lin_test_channel_bad, (uint8_t)k_lin_id_00));
  /* id above the 6-bit maximum. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_sci_lin_send_header((uint8_t)k_lin_test_channel, (uint8_t)k_lin_id_bad));

  /* CSR cleared -> the SYNC byte's TDRE poll never completes -> timeout. */
  ra_sci((uint8_t)k_lin_test_channel)->CSR = 0U;
  TEST_ASSERT_EQ(k_ra_err_hw_timeout,
                 ra_sci_lin_send_header((uint8_t)k_lin_test_channel, (uint8_t)k_lin_id_00));
  TEST_END("ra_sci_lin_send_header: guards + timeout");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises ra_sci_lin_pid against
 * known LIN id/PID vectors and the 6-bit input mask)
 */
static void test_lin_pid_vectors(void)
{
  TEST_BEGIN("ra_sci_lin_pid: known LIN vectors");
  prep();

  TEST_ASSERT_EQ((uint8_t)k_lin_pid_00, ra_sci_lin_pid((uint8_t)k_lin_id_00));
  TEST_ASSERT_EQ((uint8_t)k_lin_pid_01, ra_sci_lin_pid((uint8_t)k_lin_id_01));
  TEST_ASSERT_EQ((uint8_t)k_lin_pid_02, ra_sci_lin_pid((uint8_t)k_lin_id_02));
  TEST_ASSERT_EQ((uint8_t)k_lin_pid_03, ra_sci_lin_pid((uint8_t)k_lin_id_03));
  TEST_ASSERT_EQ((uint8_t)k_lin_pid_3c, ra_sci_lin_pid((uint8_t)k_lin_id_3c));
  TEST_ASSERT_EQ((uint8_t)k_lin_pid_3d, ra_sci_lin_pid((uint8_t)k_lin_id_3d));
  /* High caller bits are masked off: 0xBC & 0x3F == 0x3C -> 0x3C. */
  TEST_ASSERT_EQ((uint8_t)k_lin_pid_3c, ra_sci_lin_pid((uint8_t)k_lin_id_masked));
  TEST_END("ra_sci_lin_pid: known LIN vectors");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the classic and
 * enhanced checksum paths against known LIN vectors)
 */
static void test_lin_checksum_vectors(void)
{
  TEST_BEGIN("ra_sci_lin_checksum: classic + enhanced vectors");
  prep();

  uint8_t cs = 0U;
  /* Classic {0x55,0x93,0xE5} -> 0x31. */
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sci_lin_checksum(k_ra_sci_lin_checksum_classic,
                                     (uint8_t)k_lin_csum_pid,
                                     s_csum_data_a,
                                     (uint8_t)sizeof(s_csum_data_a),
                                     &cs));
  TEST_ASSERT_EQ((uint8_t)k_lin_csum_classic_a, cs);

  /* Enhanced PID 0x4A {0x55,0x93,0xE5} -> 0xE6 (LIN 2.0 spec example). */
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sci_lin_checksum(k_ra_sci_lin_checksum_enhanced,
                                     (uint8_t)k_lin_csum_pid,
                                     s_csum_data_a,
                                     (uint8_t)sizeof(s_csum_data_a),
                                     &cs));
  TEST_ASSERT_EQ((uint8_t)k_lin_csum_enhanced, cs);

  /* Classic {0x01,0x02,0x03,0x04} -> 0xF5. */
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_sci_lin_checksum(k_ra_sci_lin_checksum_classic,
                                     (uint8_t)k_lin_csum_pid,
                                     s_csum_data_b,
                                     (uint8_t)sizeof(s_csum_data_b),
                                     &cs));
  TEST_ASSERT_EQ((uint8_t)k_lin_csum_classic_b, cs);
  TEST_END("ra_sci_lin_checksum: classic + enhanced vectors");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the checksum validation
 * guards: NULL out, NULL data with len>0, zero-length, and bad mode)
 */
static void test_lin_checksum_validation(void)
{
  TEST_BEGIN("ra_sci_lin_checksum: validation guards");
  prep();

  uint8_t cs = 0U;
  /* NULL out pointer. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_sci_lin_checksum(k_ra_sci_lin_checksum_classic,
                                     (uint8_t)k_lin_csum_pid,
                                     s_csum_data_a,
                                     (uint8_t)sizeof(s_csum_data_a),
                                     nullptr));
  /* NULL data with non-zero length. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_sci_lin_checksum(k_ra_sci_lin_checksum_classic,
                                     (uint8_t)k_lin_csum_pid,
                                     nullptr,
                                     (uint8_t)sizeof(s_csum_data_a),
                                     &cs));
  /* NULL data with zero length is allowed; classic sum of nothing -> 0xFF. */
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_sci_lin_checksum(k_ra_sci_lin_checksum_classic, (uint8_t)k_lin_csum_pid, nullptr, 0U, &cs));
  TEST_ASSERT_EQ((uint8_t)k_lin_csum_empty, cs);

  /* Undefined checksum mode. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_sci_lin_checksum((ra_sci_lin_checksum_mode_t)(k_lin_test_bad_clk_hi),
                                     (uint8_t)k_lin_csum_pid,
                                     s_csum_data_a,
                                     (uint8_t)sizeof(s_csum_data_a),
                                     &cs));
  TEST_END("ra_sci_lin_checksum: validation guards");
}

int32_t main(void)
{
  test_lin_init_register_image();
  test_lin_init_validation();
  test_lin_send_break();
  test_lin_send_header_sequence();
  test_lin_send_header_guards();
  test_lin_pid_vectors();
  test_lin_checksum_vectors();
  test_lin_checksum_validation();
  (void)fprintf(stderr, "[OK ] test_ra_sci_lin.c\n");
  return 0;
}
