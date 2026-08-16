/**
 * @file test_ra8_sci_lin.c
 * @brief Unit tests for the LIN commander + responder driver
 *        (libs/ra8_hal/src/ra8_sci_lin.c).
 *
 * @details
 * Exercises ``ra8_sci_lin_init`` for both roles (the Simple-LIN register
 * image: CCR3.MOD, XCR0 break-field enable + timer clock, XCR2 break
 * length, CCR0 TE/RE, and the role-specific XCR1 -- idle for a commander,
 * SDST + BMEN for a responder), the commander break / header emission path
 * (XCR1.TCST trigger and the TDR byte sequence), the responder break
 * detection path (XSR0.BFDF read / wait, XFCLR clear), the response (data)
 * phase send / read primitives, the pure PID parity + classic / enhanced
 * checksum helpers against known LIN vectors, and the pure header-validation
 * predicate (whose ``sync_ok && pid_ok`` compound decision carries an MC/DC
 * vector set). Status-flag polls are driven by pre-seeding CSR.TDRE /
 * CSR.RDRF / XSR0.BFDF or by arming the ra8_fake_mmio wait seam; no SIGALRM
 * injection is used.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_mstp.h"
#include "ra8_sci.h"
#include "ra8_sci_lin.h"
#include "ra8_sci_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_lin_t
 * @brief LIN identifier out-parameter seed.
 */
typedef enum : uint8_t {
  k_t_id_unset = 0xFFU, /**< Pre-set LIN id; a header read that fails must leave
                             it rather than report a real identifier.            */
} t_lin_t;

/**
 * @enum ra8_lin_test_channel_t
 * @brief Channel ids used by the LIN tests.
 */
typedef enum : uint8_t {
  k_lin_test_channel     = 2U,  /**< Valid SCI channel under test. */
  k_lin_test_channel_bad = 99U, /**< Far out-of-range channel.     */
} ra8_lin_test_channel_t;

/**
 * @enum ra8_lin_test_const_t
 * @brief Register-image expectations and PID/checksum vectors.
 */
typedef enum : uint32_t {
  k_lin_test_break_len      = 0x00A0U,     /**< XCR2.BFLW value programmed.       */
  k_lin_test_xcr2_image     = 0x00A00000U, /**< break_len << 16.                  */
  k_lin_test_tcss_div16     = 0x2U,        /**< TCSS field for div16.             */
  k_lin_test_ccr0_te_re     = 0x00000011U, /**< CCR0 = TE(bit4) | RE(bit0).       */
  k_lin_test_xcr1_responder = 0x00000030U, /**< XCR1 = SDST(bit4) | BMEN(bit5).   */
  k_lin_test_bad_break      = 0xFFFFU,     /**< Prohibited BFLW (0xFFFF).         */
  k_lin_test_bad_clk_lo     = 0U,          /**< timer_clk below div4.             */
  k_lin_test_bad_clk_hi     = 4U,          /**< timer_clk above div64.            */
  k_lin_test_bad_role       = 2U,          /**< role above responder (undefined). */
} ra8_lin_test_const_t;

/**
 * @enum ra8_lin_test_resp_t
 * @brief Response (data) phase and header-validation test vectors.
 *
 * @details ``s_csum_data_b`` {0x01,0x02,0x03,0x04} has a classic checksum
 * of 0xF5 (``k_lin_csum_classic_b``); ``send_response`` clocks it plus that
 * checksum byte out, and ``read_response`` reads the uniform RX byte the
 * host MMIO window returns for every RDR read.
 */
typedef enum : uint8_t {
  k_lin_resp_len      = 4U,    /**< Data-field length used by the phase tests.    */
  k_lin_resp_rx_byte  = 0xABU, /**< Uniform byte staged in RDR for reads.         */
  k_lin_resp_bad_len0 = 0U,    /**< Zero length -> invalid_arg.                   */
  k_lin_resp_bad_len9 = 9U,    /**< Length above the 8-byte LIN maximum.          */
  k_lin_hdr_sync_good = 0x55U, /**< The canonical LIN SYNC field byte.            */
  k_lin_hdr_sync_bad  = 0x00U, /**< A SYNC field that is not 0x55.                */
  k_lin_hdr_pid_bad   = 0x00U, /**< PID byte for id 0 with wrong parity (!=0x80). */
} ra8_lin_test_resp_t;

/**
 * @enum ra8_lin_test_pid_t
 * @brief Known LIN id -> protected-identifier vectors.
 *
 * @details PID = id | (P0 << 6) | (P1 << 7), P0 = ID0^ID1^ID2^ID4,
 * P1 = NOT(ID1^ID3^ID4^ID5). These six pairs are the canonical LIN
 * examples (including the 0x3C/0x3D diagnostic frame ids).
 */
typedef enum : uint8_t {
  k_lin_id_00     = 0x00U, /**< Lin ID 00.               */
  k_lin_pid_00    = 0x80U, /**< id 0  -> 0x80.           */
  k_lin_id_01     = 0x01U, /**< Lin ID 01.               */
  k_lin_pid_01    = 0xC1U, /**< id 1  -> 0xC1.           */
  k_lin_id_02     = 0x02U, /**< Lin ID 02.               */
  k_lin_pid_02    = 0x42U, /**< id 2  -> 0x42.           */
  k_lin_id_03     = 0x03U, /**< Lin ID 03.               */
  k_lin_pid_03    = 0x03U, /**< id 3  -> 0x03.           */
  k_lin_id_3c     = 0x3CU, /**< Lin ID 3c.               */
  k_lin_pid_3c    = 0x3CU, /**< id 60 -> 0x3C.           */
  k_lin_id_3d     = 0x3DU, /**< Lin ID 3d.               */
  k_lin_pid_3d    = 0x7DU, /**< id 61 -> 0x7D.           */
  k_lin_id_masked = 0xBCU, /**< 0xBC & 0x3F = 0x3C.      */
  k_lin_id_bad    = 0x40U, /**< 64: above the 6-bit max. */
} ra8_lin_test_pid_t;

/**
 * @enum ra8_lin_test_csum_t
 * @brief Known LIN checksum vectors (LIN 2.0 spec example + a simple one).
 */
typedef enum : uint8_t {
  k_lin_csum_pid       = 0x4AU, /**< Example PID for the enhanced sum.   */
  k_lin_csum_classic_a = 0x31U, /**< classic {0x55,0x93,0xE5}.           */
  k_lin_csum_enhanced  = 0xE6U, /**< enhanced PID 0x4A {0x55,0x93,0xE5}. */
  k_lin_csum_classic_b = 0xF5U, /**< classic {0x01,0x02,0x03,0x04}.      */
  k_lin_csum_empty     = 0xFFU, /**< classic, zero-length data.          */
} ra8_lin_test_csum_t;

static const uint8_t s_csum_data_a[] = {0x55U, 0x93U, 0xE5U};
static const uint8_t s_csum_data_b[] = {0x01U, 0x02U, 0x03U, 0x04U};

static const ra8_sci_lin_cfg_t k_lin_cfg = {
  .uart =
    {
      .baud      = 19200U,
      .data_bits = k_ra8_sci_data_8,
      .parity    = k_ra8_sci_parity_none,
      .stop_bits = k_ra8_sci_stop_1,
      .pclk_hz   = 60000000U,
    },
  .role            = k_ra8_sci_lin_role_commander,
  .timer_clk       = k_ra8_sci_lin_clk_div16,
  .break_field_len = (uint16_t)k_lin_test_break_len,
};

/** @brief Responder-role variant of ::k_lin_cfg (SDST + BMEN at init). */
static const ra8_sci_lin_cfg_t k_lin_cfg_responder = {
  .uart =
    {
      .baud      = 19200U,
      .data_bits = k_ra8_sci_data_8,
      .parity    = k_ra8_sci_parity_none,
      .stop_bits = k_ra8_sci_stop_1,
      .pclk_hz   = 60000000U,
    },
  .role            = k_ra8_sci_lin_role_responder,
  .timer_clk       = k_ra8_sci_lin_clk_div16,
  .break_field_len = (uint16_t)k_lin_test_break_len,
};

/**
 * @brief Reset the fake MMIO window and the MSTP model before a test.
 *
 * @pre The host MMIO substrate is linked into the test binary.
 * @pre ``ra8_mstp_init`` is safe to call repeatedly.
 * @post All SCI registers in the window read as zero.
 * @post No MMIO wait fault is armed; the seam is transparent.
 * @post The MSTP model is initialized.
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  (void)ra8_mstp_init();
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the LIN init register
 * image: CCR3.MOD, XCR0 BFE + TCSS, XCR2 BFLW, CCR0 TE/RE)
 */
static void test_lin_init_register_image(void)
{
  TEST_BEGIN("ra8_sci_lin_init: Simple-LIN register image");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_lin_init((uint8_t)k_lin_test_channel, &k_lin_cfg));
  volatile const r_sci_regs_t* reg = ra8_sci((uint8_t)k_lin_test_channel);
  TEST_ASSERT_NOT_NULL((void*)reg);

  /* CCR3.MOD == Simple LIN (110b), framing bits preserved from ra8_sci_init. */
  const uint32_t mod =
    (reg->CCR3 & (uint32_t)k_ra8_sci_ccr3_mask_mod) >> (uint8_t)k_ra8_sci_ccr3_shift_mod;
  TEST_ASSERT_EQ(k_ra8_sci_ccr3_mod_simple_lin, mod);
  TEST_ASSERT((reg->CCR3 & (1U << (uint8_t)k_ra8_sci_ccr3_bit_lsbf)) != 0U);
  TEST_ASSERT((reg->CCR3 & (1U << (uint8_t)k_ra8_sci_ccr3_bit_bpen)) != 0U);

  /* XCR0: break-field enable + TCSS = div16. */
  TEST_ASSERT((reg->XCR0 & (1U << (uint8_t)k_ra8_sci_xcr0_bit_bfe)) != 0U);
  TEST_ASSERT_EQ(k_lin_test_tcss_div16,
                 reg->XCR0 &
                   ((uint32_t)k_ra8_sci_xcr0_tcss_div64 << (uint8_t)k_ra8_sci_xcr0_shift_tcss));

  /* XCR2: break-field length in BFLW[31:16]. */
  TEST_ASSERT_EQ(k_lin_test_xcr2_image, reg->XCR2);

  /* CCR0: transmitter + receiver re-enabled after the mode switch. */
  TEST_ASSERT_EQ(k_lin_test_ccr0_te_re, reg->CCR0);
  TEST_END("ra8_sci_lin_init: Simple-LIN register image");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the four independent
 * validation guards of ra8_sci_lin_init, each on its own call)
 */
static void test_lin_init_validation(void)
{
  TEST_BEGIN("ra8_sci_lin_init: validation guards");
  prep();

  /* NULL cfg. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sci_lin_init((uint8_t)k_lin_test_channel, nullptr));
  /* Out-of-range channel. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sci_lin_init((uint8_t)k_lin_test_channel_bad, &k_lin_cfg));

  /* Prohibited break-field length (0xFFFF). */
  ra8_sci_lin_cfg_t bad_break = k_lin_cfg;
  bad_break.break_field_len   = (uint16_t)k_lin_test_bad_break;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_lin_init((uint8_t)k_lin_test_channel, &bad_break));

  /* timer_clk below the lowest divider. */
  ra8_sci_lin_cfg_t bad_clk_lo = k_lin_cfg;
  bad_clk_lo.timer_clk         = (ra8_sci_lin_timer_clk_t)k_lin_test_bad_clk_lo;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_lin_init((uint8_t)k_lin_test_channel, &bad_clk_lo));

  /* timer_clk above the highest divider. */
  ra8_sci_lin_cfg_t bad_clk_hi = k_lin_cfg;
  bad_clk_hi.timer_clk         = (ra8_sci_lin_timer_clk_t)k_lin_test_bad_clk_hi;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_lin_init((uint8_t)k_lin_test_channel, &bad_clk_hi));

  /* role above the highest defined role (undefined). */
  ra8_sci_lin_cfg_t bad_role = k_lin_cfg;
  bad_role.role              = (ra8_sci_lin_role_t)k_lin_test_bad_role;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_lin_init((uint8_t)k_lin_test_channel, &bad_role));
  TEST_END("ra8_sci_lin_init: validation guards");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the break-field
 * trigger write to XCR1.TCST and the bad-channel guard)
 */
static void test_lin_send_break(void)
{
  TEST_BEGIN("ra8_sci_lin_send_break: XCR1.TCST trigger");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_lin_init((uint8_t)k_lin_test_channel, &k_lin_cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_lin_send_break((uint8_t)k_lin_test_channel));
  volatile const r_sci_regs_t* reg = ra8_sci((uint8_t)k_lin_test_channel);
  TEST_ASSERT((reg->XCR1 & (1U << (uint8_t)k_ra8_sci_xcr1_bit_tcst)) != 0U);

  /* Bad channel -> null-guard. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sci_lin_send_break((uint8_t)k_lin_test_channel_bad));
  TEST_END("ra8_sci_lin_send_break: XCR1.TCST trigger");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the header happy path:
 * break trigger + final PID byte landing in TDR)
 */
static void test_lin_send_header_sequence(void)
{
  TEST_BEGIN("ra8_sci_lin_send_header: break + sync + PID");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_lin_init((uint8_t)k_lin_test_channel, &k_lin_cfg));

  /* Pre-seed CSR.TDRE so each putc poll completes on the first iteration. */
  volatile r_sci_regs_t* reg = ra8_sci((uint8_t)k_lin_test_channel);
  reg->CSR                   = (1U << (uint8_t)k_ra8_sci_csr_bit_tdre);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_lin_send_header((uint8_t)k_lin_test_channel, (uint8_t)k_lin_id_3d));
  /* Break field was triggered. */
  TEST_ASSERT((reg->XCR1 & (1U << (uint8_t)k_ra8_sci_xcr1_bit_tcst)) != 0U);
  /* TDR holds the last byte written: the protected identifier of 0x3D. */
  TEST_ASSERT_EQ(k_lin_pid_3d, reg->TDR & (uint32_t)k_ra8_sci_tdr_mask_data8);
  TEST_END("ra8_sci_lin_send_header: break + sync + PID");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the header validation
 * guards: bad channel, out-of-range id, and a TDRE-poll timeout)
 */
static void test_lin_send_header_guards(void)
{
  TEST_BEGIN("ra8_sci_lin_send_header: guards + timeout");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_lin_init((uint8_t)k_lin_test_channel, &k_lin_cfg));

  /* Bad channel -> null-guard. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_sci_lin_send_header((uint8_t)k_lin_test_channel_bad, (uint8_t)k_lin_id_00));
  /* id above the 6-bit maximum. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sci_lin_send_header((uint8_t)k_lin_test_channel, (uint8_t)k_lin_id_bad));

  /* Arm the CSR wait to fail -> the SYNC byte's TDRE poll never completes,
   * so ra8_sci_putc_polling runs to its budget and reports a hardware timeout. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_fake_mmio_fail_wait((const volatile void*)&ra8_sci((uint8_t)k_lin_test_channel)->CSR));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_sci_lin_send_header((uint8_t)k_lin_test_channel, (uint8_t)k_lin_id_00));
  TEST_END("ra8_sci_lin_send_header: guards + timeout");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises ra8_sci_lin_pid against
 * known LIN id/PID vectors and the 6-bit input mask)
 */
static void test_lin_pid_vectors(void)
{
  TEST_BEGIN("ra8_sci_lin_pid: known LIN vectors");
  prep();

  TEST_ASSERT_EQ(k_lin_pid_00, ra8_sci_lin_pid((uint8_t)k_lin_id_00));
  TEST_ASSERT_EQ(k_lin_pid_01, ra8_sci_lin_pid((uint8_t)k_lin_id_01));
  TEST_ASSERT_EQ(k_lin_pid_02, ra8_sci_lin_pid((uint8_t)k_lin_id_02));
  TEST_ASSERT_EQ(k_lin_pid_03, ra8_sci_lin_pid((uint8_t)k_lin_id_03));
  TEST_ASSERT_EQ(k_lin_pid_3c, ra8_sci_lin_pid((uint8_t)k_lin_id_3c));
  TEST_ASSERT_EQ(k_lin_pid_3d, ra8_sci_lin_pid((uint8_t)k_lin_id_3d));
  /* High caller bits are masked off: 0xBC & 0x3F == 0x3C -> 0x3C. */
  TEST_ASSERT_EQ(k_lin_pid_3c, ra8_sci_lin_pid((uint8_t)k_lin_id_masked));
  TEST_END("ra8_sci_lin_pid: known LIN vectors");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the classic and
 * enhanced checksum paths against known LIN vectors)
 */
static void test_lin_checksum_vectors(void)
{
  TEST_BEGIN("ra8_sci_lin_checksum: classic + enhanced vectors");
  prep();

  uint8_t cs = 0U;
  /* Classic {0x55,0x93,0xE5} -> 0x31. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_lin_checksum(k_ra8_sci_lin_checksum_classic,
                                      (uint8_t)k_lin_csum_pid,
                                      s_csum_data_a,
                                      (uint8_t)sizeof(s_csum_data_a),
                                      &cs));
  TEST_ASSERT_EQ(k_lin_csum_classic_a, cs);

  /* Enhanced PID 0x4A {0x55,0x93,0xE5} -> 0xE6 (LIN 2.0 spec example). */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_lin_checksum(k_ra8_sci_lin_checksum_enhanced,
                                      (uint8_t)k_lin_csum_pid,
                                      s_csum_data_a,
                                      (uint8_t)sizeof(s_csum_data_a),
                                      &cs));
  TEST_ASSERT_EQ(k_lin_csum_enhanced, cs);

  /* Classic {0x01,0x02,0x03,0x04} -> 0xF5. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_lin_checksum(k_ra8_sci_lin_checksum_classic,
                                      (uint8_t)k_lin_csum_pid,
                                      s_csum_data_b,
                                      (uint8_t)sizeof(s_csum_data_b),
                                      &cs));
  TEST_ASSERT_EQ(k_lin_csum_classic_b, cs);
  TEST_END("ra8_sci_lin_checksum: classic + enhanced vectors");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the checksum validation
 * guards: NULL out, NULL data with len>0, zero-length, and bad mode)
 */
static void test_lin_checksum_validation(void)
{
  TEST_BEGIN("ra8_sci_lin_checksum: validation guards");
  prep();

  uint8_t cs = 0U;
  /* NULL out pointer. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_sci_lin_checksum(k_ra8_sci_lin_checksum_classic,
                                      (uint8_t)k_lin_csum_pid,
                                      s_csum_data_a,
                                      (uint8_t)sizeof(s_csum_data_a),
                                      nullptr));
  /* NULL data with non-zero length. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_sci_lin_checksum(k_ra8_sci_lin_checksum_classic,
                                      (uint8_t)k_lin_csum_pid,
                                      nullptr,
                                      (uint8_t)sizeof(s_csum_data_a),
                                      &cs));
  /* NULL data with zero length is allowed; classic sum of nothing -> 0xFF. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_lin_checksum(k_ra8_sci_lin_checksum_classic,
                                      (uint8_t)k_lin_csum_pid,
                                      nullptr,
                                      0U,
                                      &cs));
  TEST_ASSERT_EQ(k_lin_csum_empty, cs);

  /* Undefined checksum mode. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sci_lin_checksum((ra8_sci_lin_checksum_mode_t)(k_lin_test_bad_clk_hi),
                                      (uint8_t)k_lin_csum_pid,
                                      s_csum_data_a,
                                      (uint8_t)sizeof(s_csum_data_a),
                                      &cs));
  TEST_END("ra8_sci_lin_checksum: validation guards");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the responder init
 * register image: CCR3.MOD == Simple LIN, XCR0 BFE + TCSS, and the
 * responder-specific XCR1 == SDST | BMEN)
 */
static void test_lin_init_responder_image(void)
{
  TEST_BEGIN("ra8_sci_lin_init: responder register image");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_lin_init((uint8_t)k_lin_test_channel, &k_lin_cfg_responder));
  volatile const r_sci_regs_t* reg = ra8_sci((uint8_t)k_lin_test_channel);
  TEST_ASSERT_NOT_NULL((void*)reg);

  /* Same Simple-LIN mode + break-field enable as the commander. */
  const uint32_t mod =
    (reg->CCR3 & (uint32_t)k_ra8_sci_ccr3_mask_mod) >> (uint8_t)k_ra8_sci_ccr3_shift_mod;
  TEST_ASSERT_EQ(k_ra8_sci_ccr3_mod_simple_lin, mod);
  TEST_ASSERT((reg->XCR0 & (1U << (uint8_t)k_ra8_sci_xcr0_bit_bfe)) != 0U);

  /* Responder-specific XCR1: Start-Frame detection + bit-rate measurement. */
  TEST_ASSERT_EQ(k_lin_test_xcr1_responder, reg->XCR1);
  TEST_ASSERT((reg->XCR1 & (1U << (uint8_t)k_ra8_sci_xcr1_bit_sdst)) != 0U);
  TEST_ASSERT((reg->XCR1 & (1U << (uint8_t)k_ra8_sci_xcr1_bit_bmen)) != 0U);
  TEST_END("ra8_sci_lin_init: responder register image");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises ra8_sci_lin_break_detected
 * for both XSR0.BFDF states plus its null-out and bad-channel guards)
 */
static void test_lin_break_detect(void)
{
  TEST_BEGIN("ra8_sci_lin_break_detected: XSR0.BFDF read");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_lin_init((uint8_t)k_lin_test_channel, &k_lin_cfg_responder));
  volatile r_sci_regs_t* reg = ra8_sci((uint8_t)k_lin_test_channel);

  bool detected = true;
  /* BFDF clear -> not detected. */
  reg->XSR0 = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_lin_break_detected((uint8_t)k_lin_test_channel, &detected));
  TEST_ASSERT(!detected);

  /* BFDF set -> detected. */
  reg->XSR0 = (1U << (uint8_t)k_ra8_sci_xsr0_bit_bfdf);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_lin_break_detected((uint8_t)k_lin_test_channel, &detected));
  TEST_ASSERT(detected);

  /* Null out pointer + bad channel guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_sci_lin_break_detected((uint8_t)k_lin_test_channel, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_sci_lin_break_detected((uint8_t)k_lin_test_channel_bad, &detected));
  TEST_END("ra8_sci_lin_break_detected: XSR0.BFDF read");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the bounded XSR0.BFDF
 * wait: success when the flag is staged, timeout when the seam is armed,
 * and the bad-channel guard)
 */
static void test_lin_wait_break(void)
{
  TEST_BEGIN("ra8_sci_lin_wait_break: poll XSR0.BFDF");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_lin_init((uint8_t)k_lin_test_channel, &k_lin_cfg_responder));
  volatile r_sci_regs_t* reg = ra8_sci((uint8_t)k_lin_test_channel);

  /* Break already detected -> the wait succeeds on its first poll. */
  reg->XSR0 = (1U << (uint8_t)k_ra8_sci_xsr0_bit_bfdf);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_lin_wait_break((uint8_t)k_lin_test_channel));

  /* Arm the XSR0 wait to fail -> the poll runs to its budget and times out. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_fake_mmio_fail_wait((const volatile void*)&ra8_sci((uint8_t)k_lin_test_channel)->XSR0));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_sci_lin_wait_break((uint8_t)k_lin_test_channel));

  /* Bad channel -> null-guard. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sci_lin_wait_break((uint8_t)k_lin_test_channel_bad));
  TEST_END("ra8_sci_lin_wait_break: poll XSR0.BFDF");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the XFCLR clear-all
 * write and the bad-channel guard)
 */
static void test_lin_clear_status(void)
{
  TEST_BEGIN("ra8_sci_lin_clear_status: XFCLR clear-all");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_lin_init((uint8_t)k_lin_test_channel, &k_lin_cfg_responder));
  volatile r_sci_regs_t* reg = ra8_sci((uint8_t)k_lin_test_channel);

  reg->XFCLR = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_lin_clear_status((uint8_t)k_lin_test_channel));
  /* The clear-all LIN mask (bits 8..15) was written to XFCLR. */
  TEST_ASSERT_EQ(k_ra8_sci_xfclr_default, reg->XFCLR);

  /* Bad channel -> null-guard. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sci_lin_clear_status((uint8_t)k_lin_test_channel_bad));
  TEST_END("ra8_sci_lin_clear_status: XFCLR clear-all");
}

/**
 * @test test_mcdc_check_header
 *
 * @par MC/DC:
 * Decision ``*out_valid = sync_ok && pid_ok;`` (2 conditions) in
 * libs/ra8_hal/src/ra8_sci_lin.c@ra8_sci_lin_check_header, covered with an
 * N+1 = 3 vector subset (DO-178C 6.4.4.3). ``sync_ok`` is
 * ``(sync == 0x55)`` and ``pid_ok`` is
 * ``(ra8_sci_lin_pid(pid & 0x3F) == pid)``; id 0 has PID 0x80.
 * - V1: sync=0x55, pid=0x80 -> C1=T, C2=T -> valid=T (control)
 * - V2: sync=0x00, pid=0x80 -> C1=F, C2=T -> valid=F (varies sync_ok)
 * - V3: sync=0x55, pid=0x00 -> C1=T, C2=F -> valid=F (varies pid_ok)
 * V1+V2 prove sync_ok independently drives the outcome (pid_ok held T);
 * V1+V3 prove the same for pid_ok (sync_ok held T). Also exercises the
 * two null-out guards.
 */
static void test_mcdc_check_header(void)
{
  TEST_BEGIN("ra8_sci_lin_check_header: SYNC + PID MC/DC");
  prep();

  uint8_t id    = k_t_id_unset;
  bool    valid = false;

  /* V1: good SYNC + good PID -> accepted, id extracted. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_sci_lin_check_header((uint8_t)k_lin_hdr_sync_good, (uint8_t)k_lin_pid_00, &id, &valid));
  TEST_ASSERT(valid);
  TEST_ASSERT_EQ(k_lin_id_00, id);

  /* V2: bad SYNC, good PID -> rejected (sync_ok varies). */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_sci_lin_check_header((uint8_t)k_lin_hdr_sync_bad, (uint8_t)k_lin_pid_00, &id, &valid));
  TEST_ASSERT(!valid);

  /* V3: good SYNC, bad PID parity -> rejected (pid_ok varies). */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_lin_check_header((uint8_t)k_lin_hdr_sync_good,
                                          (uint8_t)k_lin_hdr_pid_bad,
                                          &id,
                                          &valid));
  TEST_ASSERT(!valid);

  /* Null-out guards. */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_sci_lin_check_header((uint8_t)k_lin_hdr_sync_good, (uint8_t)k_lin_pid_00, nullptr, &valid));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_sci_lin_check_header((uint8_t)k_lin_hdr_sync_good, (uint8_t)k_lin_pid_00, &id, nullptr));
  TEST_END("ra8_sci_lin_check_header: SYNC + PID MC/DC");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises ra8_sci_lin_send_response:
 * the happy path leaves the checksum byte in TDR, plus the length / channel /
 * checksum-propagation guards and a mid-frame TDRE timeout)
 */
static void test_lin_send_response(void)
{
  TEST_BEGIN("ra8_sci_lin_send_response: data + checksum");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_lin_init((uint8_t)k_lin_test_channel, &k_lin_cfg));
  volatile r_sci_regs_t* reg = ra8_sci((uint8_t)k_lin_test_channel);

  /* Happy path: pre-seed CSR.TDRE so each putc completes immediately. */
  reg->CSR = (1U << (uint8_t)k_ra8_sci_csr_bit_tdre);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_lin_send_response((uint8_t)k_lin_test_channel,
                                           k_ra8_sci_lin_checksum_classic,
                                           (uint8_t)k_lin_csum_pid,
                                           s_csum_data_b,
                                           (uint8_t)k_lin_resp_len));
  /* TDR holds the last byte written: the classic checksum 0xF5. */
  TEST_ASSERT_EQ(k_lin_csum_classic_b, reg->TDR & (uint32_t)k_ra8_sci_tdr_mask_data8);

  /* Guards: bad channel, zero length, over-length. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_sci_lin_send_response((uint8_t)k_lin_test_channel_bad,
                                           k_ra8_sci_lin_checksum_classic,
                                           (uint8_t)k_lin_csum_pid,
                                           s_csum_data_b,
                                           (uint8_t)k_lin_resp_len));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sci_lin_send_response((uint8_t)k_lin_test_channel,
                                           k_ra8_sci_lin_checksum_classic,
                                           (uint8_t)k_lin_csum_pid,
                                           s_csum_data_b,
                                           (uint8_t)k_lin_resp_bad_len0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sci_lin_send_response((uint8_t)k_lin_test_channel,
                                           k_ra8_sci_lin_checksum_classic,
                                           (uint8_t)k_lin_csum_pid,
                                           s_csum_data_b,
                                           (uint8_t)k_lin_resp_bad_len9));
  /* Checksum-stage error propagates: NULL data with non-zero length. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_sci_lin_send_response((uint8_t)k_lin_test_channel,
                                           k_ra8_sci_lin_checksum_classic,
                                           (uint8_t)k_lin_csum_pid,
                                           nullptr,
                                           (uint8_t)k_lin_resp_len));

  /* Mid-frame timeout: arm CSR.TDRE to never assert. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_fake_mmio_fail_wait((const volatile void*)&ra8_sci((uint8_t)k_lin_test_channel)->CSR));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_sci_lin_send_response((uint8_t)k_lin_test_channel,
                                           k_ra8_sci_lin_checksum_classic,
                                           (uint8_t)k_lin_csum_pid,
                                           s_csum_data_b,
                                           (uint8_t)k_lin_resp_len));
  TEST_END("ra8_sci_lin_send_response: data + checksum");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises ra8_sci_lin_read_response:
 * the happy path drains data + checksum from RDR, plus the null / length /
 * channel guards and a mid-frame RDRF timeout)
 */
static void test_lin_read_response(void)
{
  TEST_BEGIN("ra8_sci_lin_read_response: data + checksum");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_lin_init((uint8_t)k_lin_test_channel, &k_lin_cfg_responder));
  volatile r_sci_regs_t* reg = ra8_sci((uint8_t)k_lin_test_channel);

  /* Happy path: RDRF set + a uniform byte staged in RDR. */
  reg->CSR                     = (1U << (uint8_t)k_ra8_sci_csr_bit_rdrf);
  reg->RDR                     = (uint32_t)k_lin_resp_rx_byte;
  uint8_t data[k_lin_resp_len] = {};
  uint8_t checksum             = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_lin_read_response((uint8_t)k_lin_test_channel,
                                           data,
                                           (uint8_t)k_lin_resp_len,
                                           &checksum));
  for (uint8_t i = 0U; i < (uint8_t)k_lin_resp_len; ++i) {
    TEST_ASSERT_EQ(k_lin_resp_rx_byte, data[i]);
  }
  TEST_ASSERT_EQ(k_lin_resp_rx_byte, checksum);

  /* Guards: bad channel, null buffers, zero / over length. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_sci_lin_read_response((uint8_t)k_lin_test_channel_bad,
                                           data,
                                           (uint8_t)k_lin_resp_len,
                                           &checksum));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_sci_lin_read_response((uint8_t)k_lin_test_channel,
                                           nullptr,
                                           (uint8_t)k_lin_resp_len,
                                           &checksum));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_sci_lin_read_response((uint8_t)k_lin_test_channel, data, (uint8_t)k_lin_resp_len, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sci_lin_read_response((uint8_t)k_lin_test_channel,
                                           data,
                                           (uint8_t)k_lin_resp_bad_len0,
                                           &checksum));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sci_lin_read_response((uint8_t)k_lin_test_channel,
                                           data,
                                           (uint8_t)k_lin_resp_bad_len9,
                                           &checksum));

  /* Mid-frame timeout: arm CSR.RDRF to never assert. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_fake_mmio_fail_wait((const volatile void*)&ra8_sci((uint8_t)k_lin_test_channel)->CSR));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_sci_lin_read_response((uint8_t)k_lin_test_channel,
                                           data,
                                           (uint8_t)k_lin_resp_len,
                                           &checksum));
  TEST_END("ra8_sci_lin_read_response: data + checksum");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises ra8_sci_lin_check_response
 * for a matching and a mismatching checksum, plus the null-out and bad-mode
 * guards)
 */
static void test_lin_check_response(void)
{
  TEST_BEGIN("ra8_sci_lin_check_response: verify checksum");
  prep();

  bool valid = false;
  /* Classic {0x01,0x02,0x03,0x04} -> 0xF5: matching checksum accepted. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_lin_check_response(k_ra8_sci_lin_checksum_classic,
                                            (uint8_t)k_lin_csum_pid,
                                            s_csum_data_b,
                                            (uint8_t)k_lin_resp_len,
                                            (uint8_t)k_lin_csum_classic_b,
                                            &valid));
  TEST_ASSERT(valid);

  /* A wrong checksum byte is rejected. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_lin_check_response(k_ra8_sci_lin_checksum_classic,
                                            (uint8_t)k_lin_csum_pid,
                                            s_csum_data_b,
                                            (uint8_t)k_lin_resp_len,
                                            (uint8_t)k_lin_hdr_pid_bad,
                                            &valid));
  TEST_ASSERT(!valid);

  /* Null out pointer. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_sci_lin_check_response(k_ra8_sci_lin_checksum_classic,
                                            (uint8_t)k_lin_csum_pid,
                                            s_csum_data_b,
                                            (uint8_t)k_lin_resp_len,
                                            (uint8_t)k_lin_csum_classic_b,
                                            nullptr));
  /* Bad checksum mode propagates from ra8_sci_lin_checksum. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sci_lin_check_response((ra8_sci_lin_checksum_mode_t)k_lin_test_bad_clk_hi,
                                            (uint8_t)k_lin_csum_pid,
                                            s_csum_data_b,
                                            (uint8_t)k_lin_resp_len,
                                            (uint8_t)k_lin_csum_classic_b,
                                            &valid));
  TEST_END("ra8_sci_lin_check_response: verify checksum");
}

int main(void)
{
  test_lin_init_register_image();
  test_lin_init_validation();
  test_lin_init_responder_image();
  test_lin_send_break();
  test_lin_send_header_sequence();
  test_lin_send_header_guards();
  test_lin_break_detect();
  test_lin_wait_break();
  test_lin_clear_status();
  test_lin_pid_vectors();
  test_lin_checksum_vectors();
  test_lin_checksum_validation();
  test_mcdc_check_header();
  test_lin_send_response();
  test_lin_read_response();
  test_lin_check_response();
  return 0;
}
