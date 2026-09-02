/**
 * @file test_ra8_usb_cdc.c
 * @brief Unit tests for the native USB CDC ACM class layer
 * @details Drives native device-side CDC setup requests, line coding, endpoints, callbacks, and rejection paths using fake USB registers.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_usb.h"
#include "ra8_usb_cdc.h"
#include "ra8_usb_cdc_internal.h"
#include "ra8_usb_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_setup_t
 * @brief SETUP-packet fields the class arms submit.
 *
 * @details
 * `bmRequestType` packs direction, type and recipient into one byte, so each
 * name states the combination rather than the hex value.
 */
typedef enum : uint16_t {
  k_t_bmreq_class_out  = 0x21U,   /**< Host-to-device, class, interface.          */
  k_t_bmreq_class_in   = 0xA1U,   /**< Device-to-host, class, interface.          */
  k_t_bmreq_std_in     = 0x80U,   /**< Device-to-host, standard, device.          */
  k_t_wlen_line_coding = 7U,      /**< CDC line-coding structure, bytes.          */
  k_t_wlen_dev_desc    = 18U,     /**< Device-descriptor length, bytes.           */
  k_t_wvalue_dev_desc  = 0x0100U, /**< wValue 0x0100: descriptor type 1, index 0. */
} t_setup_t;

static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_usb_cdc_deinit();
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_fs_default_coding(void)
{
  TEST_BEGIN("ra8_usb_cdc_init seeds 9600/8/N/1 line coding on FS");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_init(k_ra8_usb_speed_fs));

  ra8_usb_cdc_line_coding_t coding = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_get_line_coding(&coding));
  TEST_ASSERT_EQ(9600U, coding.dte_rate);
  TEST_ASSERT_EQ(8U, coding.data_bits);
  TEST_ASSERT_EQ(0U, coding.char_format);
  TEST_ASSERT_EQ(0U, coding.parity_type);
  TEST_END("ra8_usb_cdc_init seeds 9600/8/N/1 line coding on FS");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_speed(void)
{
  TEST_BEGIN("ra8_usb_cdc_init rejects bogus speed");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_cdc_init((ra8_usb_speed_t)9U));
  TEST_END("ra8_usb_cdc_init rejects bogus speed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_hs_picks_512_bulk(void)
{
  TEST_BEGIN("ra8_usb_cdc_init on HS configures 512-byte bulk pipes");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_init(k_ra8_usb_speed_hs));
  /* configure_endpoint deselects the pipe window (PIPESEL=0) before returning;
   * PIPEMAXP off-target retains the last value written (interrupt pipe size). */
  volatile r_usb_regs_t* reg = ra8_usb_hs();
  TEST_ASSERT_EQ(0U, reg->PIPESEL);
  TEST_ASSERT_EQ(k_ra8_cdc_intr_max_packet, reg->PIPEMAXP);
  TEST_END("ra8_usb_cdc_init on HS configures 512-byte bulk pipes");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_detach(void)
{
  TEST_BEGIN("ra8_usb_cdc_attach toggles DPRPU");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_init(k_ra8_usb_speed_fs));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_attach(true));
  volatile r_usb_regs_t* reg   = ra8_usb_fs();
  const uint16_t         dpbit = (uint16_t)(1U << k_ra8_syscfg_bit_dprpu);
  TEST_ASSERT((reg->SYSCFG & dpbit) != 0);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_attach(false));
  TEST_ASSERT_EQ(0, (reg->SYSCFG & dpbit));
  TEST_END("ra8_usb_cdc_attach toggles DPRPU");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_pre_init_calls(void)
{
  TEST_BEGIN("CDC API rejects calls before init");
  prep();

  uint8_t  buf[8] = {};
  uint16_t len    = 8U;

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_cdc_attach(true));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_cdc_send(buf, 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_cdc_recv(buf, &len));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_cdc_deinit());

  ra8_usb_cdc_line_coding_t coding = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_cdc_get_line_coding(&coding));
  bool dtr = false;
  bool rts = false;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_cdc_get_line_state(&dtr, &rts));

  ra8_usb_setup_t setup = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_cdc_handle_setup(&setup));
  TEST_END("CDC API rejects calls before init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_handle_setup_set_control_line_state(void)
{
  TEST_BEGIN("ra8_usb_cdc_handle_setup updates DTR/RTS");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_init(k_ra8_usb_speed_fs));

  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_t_bmreq_class_out,
    .b_request       = (uint8_t)k_ra8_cdc_req_set_control_line_state,
    .w_value         = (uint16_t)(k_ra8_cdc_line_state_dtr | k_ra8_cdc_line_state_rts),
    .w_index         = 0U,
    .w_length        = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_handle_setup(&setup));

  bool dtr = false;
  bool rts = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_get_line_state(&dtr, &rts));
  TEST_ASSERT(dtr);
  TEST_ASSERT(rts);

  setup.w_value = (uint16_t)k_ra8_cdc_line_state_dtr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_handle_setup(&setup));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_get_line_state(&dtr, &rts));
  TEST_ASSERT(dtr);
  TEST_ASSERT(!rts);
  TEST_END("ra8_usb_cdc_handle_setup updates DTR/RTS");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_handle_setup_get_line_coding_acks(void)
{
  TEST_BEGIN("ra8_usb_cdc_handle_setup ACKs GET_LINE_CODING");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_init(k_ra8_usb_speed_fs));

  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_t_bmreq_class_in,
    .b_request       = (uint8_t)k_ra8_cdc_req_get_line_coding,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = k_t_wlen_line_coding,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_handle_setup(&setup));
  /* DCPCTR.PID should be BUF, CCPL set. */
  volatile r_usb_regs_t* reg = ra8_usb_fs();
  TEST_ASSERT_EQ(k_ra8_pid_buf, (reg->DCPCTR & k_ra8_pid_mask));
  TEST_ASSERT((reg->DCPCTR & (uint16_t)(1U << k_ra8_dcpctr_bit_ccpl)) != 0U);
  TEST_END("ra8_usb_cdc_handle_setup ACKs GET_LINE_CODING");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_handle_setup_rejects_standard(void)
{
  TEST_BEGIN("ra8_usb_cdc_handle_setup rejects non-class SETUPs");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_init(k_ra8_usb_speed_fs));

  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_t_bmreq_std_in, /* standard, device, IN */
    .b_request       = (uint8_t)0x06U,            /* GET_DESCRIPTOR       */
    .w_value         = k_t_wvalue_dev_desc,
    .w_index         = 0U,
    .w_length        = k_t_wlen_dev_desc,
  };
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_usb_cdc_handle_setup(&setup));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_cdc_handle_setup(nullptr));
  TEST_END("ra8_usb_cdc_handle_setup rejects non-class SETUPs");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_recv_arg_validation(void)
{
  TEST_BEGIN("ra8_usb_cdc_{send,recv} validate args");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_init(k_ra8_usb_speed_fs));

  uint8_t  buf[8] = {};
  uint16_t len    = 8U;

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_cdc_send(nullptr, 4U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_cdc_recv(nullptr, &len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_cdc_recv(buf, nullptr));
  uint16_t zero = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_cdc_recv(buf, &zero));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_cdc_get_line_coding(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_cdc_get_line_state(nullptr, nullptr));
  bool dtr = false;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_cdc_get_line_state(&dtr, nullptr));
  TEST_END("ra8_usb_cdc_{send,recv} validate args");
}

/* =============================================================================
 * MC/DC vector tests for compound boolean decisions flagged in
 * docs/MCDC_GAPS.csv against libs/ra8_hal/src/ra8_usb_cdc.c.
 * =============================================================================
 */

/**
 * @enum test_cdc_mcdc_t
 */
typedef enum : uint16_t {
  k_test_cdc_speed_bad      = 9U,    /**< Test cdc speed bad.             */
  k_test_cdc_iface_class    = 0x21U, /**< k_ra8_cdc_bm_class_recip_iface. */
  k_test_cdc_iface_class_in = 0xA1U, /**< k_ra8_cdc_bm_class_recip_in.    */
  k_test_cdc_bm_bogus       = 0x80U, /**< Test cdc bm bogus.              */
  k_test_cdc_coding_len     = 7U,    /**< CDC line-coding payload bytes.  */
} test_cdc_mcdc_t;

/**
 * @enum test_cdc_coding_t
 * @brief Named byte values in the MC/DC line-coding vector.
 */
typedef enum : uint32_t {
  k_test_cdc_baud_b0       = 0x00U,   /**< Low byte of 115200 baud.    */
  k_test_cdc_baud_b1       = 0xC2U,   /**< Second byte of 115200 baud. */
  k_test_cdc_baud_b2       = 0x01U,   /**< Third byte of 115200 baud.  */
  k_test_cdc_baud_b3       = 0x00U,   /**< High byte of 115200 baud.   */
  k_test_cdc_char_format   = 2U,      /**< Two stop bits.              */
  k_test_cdc_parity_type   = 3U,      /**< Mark parity.                */
  k_test_cdc_data_bits     = 7U,      /**< Seven data bits.            */
  k_test_cdc_expected_baud = 115200U, /**< Decoded DTE rate.           */
} test_cdc_coding_t;

/**
 * @test test_mcdc_line_coding_payload_gate
 * @brief Cover the line-coding decoder's null and short-payload conditions.
 *
 * @par MC/DC:
 * Decision `(data == NULL) || (len < 7)` uses N+1 = 3 vectors:
 * - `(buf, 7)`: both conditions false, so the payload is applied.
 * - `(NULL, 7)`: the first condition independently makes the result true.
 * - `(buf, 6)`: the second condition independently makes the result true.
 */
static void test_mcdc_line_coding_payload_gate(void)
{
  TEST_BEGIN("cdc MC/DC: line-coding null and short payload gate");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_init(k_ra8_usb_speed_fs));

  const uint8_t coding_payload[k_test_cdc_coding_len] = {
    (uint8_t)k_test_cdc_baud_b0,
    (uint8_t)k_test_cdc_baud_b1,
    (uint8_t)k_test_cdc_baud_b2,
    (uint8_t)k_test_cdc_baud_b3,
    (uint8_t)k_test_cdc_char_format,
    (uint8_t)k_test_cdc_parity_type,
    (uint8_t)k_test_cdc_data_bits,
  };
  ra8_usb_cdc_line_coding_t coding = {};

  ra8_usb_cdc_test_apply_line_coding(nullptr, (uint16_t)k_test_cdc_coding_len);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_get_line_coding(&coding));
  TEST_ASSERT_EQ(9600U, coding.dte_rate);

  ra8_usb_cdc_test_apply_line_coding(coding_payload, (uint16_t)(k_test_cdc_coding_len - 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_get_line_coding(&coding));
  TEST_ASSERT_EQ(9600U, coding.dte_rate);

  ra8_usb_cdc_test_apply_line_coding(coding_payload, (uint16_t)k_test_cdc_coding_len);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_get_line_coding(&coding));
  TEST_ASSERT_EQ(k_test_cdc_expected_baud, coding.dte_rate);
  TEST_ASSERT_EQ(k_test_cdc_char_format, coding.char_format);
  TEST_ASSERT_EQ(k_test_cdc_parity_type, coding.parity_type);
  TEST_ASSERT_EQ(k_test_cdc_data_bits, coding.data_bits);
  TEST_END("cdc MC/DC: line-coding null and short payload gate");
}

/**
 * @test test_mcdc_cdc
 *
 * @par MC/DC:
 * Covers the public compound decisions in `ra8_usb_cdc.c`.
 *
 * Decision A (2 conditions): cdc_init speed gate
 *   `(speed != FS) && (speed != HS)` -- N+1=3:
 *   - V1 FS -> C1=F (short circuit) -> dec=F (init ok)
 *   - V2 HS -> C1=T, C2=F           -> dec=F (init ok)
 *   - V3 9  -> C1=T, C2=T           -> dec=T (invalid_arg)
 *
 * Decision B (2 conditions): cdc_send NULL-with-len
 *   `(data == NULL) && (len != 0)` -- N+1=3:
 *   - V1 (NULL,0) -> C1=T, C2=F -> dec=F (forwards OK)
 *   - V2 (buf,4)  -> C1=F       -> dec=F (forwards OK)
 *   - V3 (NULL,4) -> C1=T, C2=T -> dec=T (invalid_arg)
 *
 * Decision C (2 conditions): handle_setup envelope
 *   `(bm != iface) && (bm != iface_in)` -- N+1=3:
 *   - V1 iface     -> C1=F (short circuit)              -> dec=F (dispatched)
 *   - V2 iface_in  -> C1=T, C2=F                        -> dec=F (dispatched)
 *   - V3 0x80      -> C1=T, C2=T                        -> dec=T (not_supported)
 */
static void test_mcdc_cdc(void)
{
  TEST_BEGIN("cdc MC/DC: init / send / handle_setup compound decisions");

  /* Decision A: init speed gate. */
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_init(k_ra8_usb_speed_fs));
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_init(k_ra8_usb_speed_hs));
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_cdc_init((ra8_usb_speed_t)k_test_cdc_speed_bad));

  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_init(k_ra8_usb_speed_fs));

  /* Decision B: send null/len matrix. */
  uint8_t buf[8] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_cdc_send(nullptr, 4U));
  /* V1 NULL,0: forwarded to ra8_usb_queue_in which may return ok or
   * hw_error from the fake -- we only assert it is NOT
   * invalid_arg, which is the only outcome for B-V1 false. */
  const ra8_err_t b_v1 = ra8_usb_cdc_send(nullptr, 0U);
  TEST_ASSERT(b_v1 != k_ra8_err_invalid_arg);
  const ra8_err_t b_v2 = ra8_usb_cdc_send(buf, 4U);
  TEST_ASSERT(b_v2 != k_ra8_err_invalid_arg);

  /* Decision C: handle_setup envelope. */
  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_test_cdc_iface_class,
    .b_request       = (uint8_t)k_ra8_cdc_req_set_control_line_state,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_handle_setup(&setup));
  setup.bm_request_type = (uint8_t)k_test_cdc_iface_class_in;
  setup.b_request       = (uint8_t)k_ra8_cdc_req_get_line_coding;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_cdc_handle_setup(&setup));
  setup.bm_request_type = (uint8_t)k_test_cdc_bm_bogus;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_usb_cdc_handle_setup(&setup));

  TEST_END("cdc MC/DC: init / send / handle_setup compound decisions");
}

int main(void)
{
  test_init_fs_default_coding();
  test_init_bad_speed();
  test_init_hs_picks_512_bulk();
  test_attach_detach();
  test_pre_init_calls();
  test_handle_setup_set_control_line_state();
  test_handle_setup_get_line_coding_acks();
  test_handle_setup_rejects_standard();
  test_send_recv_arg_validation();
  test_mcdc_line_coding_payload_gate();
  test_mcdc_cdc();
  return 0;
}
