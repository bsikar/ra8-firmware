/**
 * @file test_ra8_usb_hhid.c
 * @brief Unit tests for the native USB host-side HID class layer
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "ra8_usb.h"
#include "ra8_usb_hhid.h"
#include "ra8_usb_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_hhid_t
 * @brief FIFO words, control bits and the receive-count poison.
 */
typedef enum : uint16_t {
  k_t_cfifo_word_a  = 0xCAFEU, /**< CFIFO half-word staged for the first read. */
  k_t_cfifo_word_b  = 0xBEEFU, /**< A different word for the second, so a stale
                                    FIFO value cannot pass as a fresh read.     */
  k_t_cfifoctr_bval = 0x2000U, /**< CFIFOCTR BVAL: buffer valid; ORed with the
                                    byte count the arm is presenting.           */
  k_t_got_unset     = 0xFFFFU, /**< Pre-set received-byte count; a read that
                                    returns without writing it leaves this.      */
} t_hhid_t;

typedef enum : uint8_t {
  k_test_hhid_max_steps = 16U, /**< Loop bound for stepping through enum. */
} test_hhid_lim_t;

/**
 * @enum test_hhid_setup_t
 * @brief Wire-level constants the tests assert against.
 */
typedef enum : uint16_t {
  k_test_hhid_bm_class_iface_in  = 0xA1U, /**< D2H | Class | Interface. */
  k_test_hhid_bm_class_iface_out = 0x21U, /**< H2D | Class | Interface. */
  k_test_hhid_breq_get_report    = 0x01U, /**< HID GET_REPORT.          */
  k_test_hhid_breq_set_report    = 0x09U, /**< HID SET_REPORT.          */
  k_test_hhid_breq_set_idle      = 0x0AU, /**< HID SET_IDLE.            */
  k_test_hhid_breq_set_protocol  = 0x0BU, /**< HID SET_PROTOCOL.        */
} test_hhid_setup_t;

static uint32_t              s_attach_count;
static ra8_usb_hhid_device_t s_attach_last_device;
static void*                 s_attach_last_ctx;
static const uintptr_t       k_test_hhid_ctx_token = 0xFEEDFACEU;

static void prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_usb_hhid_close();
  s_attach_count       = 0U;
  s_attach_last_device = (ra8_usb_hhid_device_t){};
  s_attach_last_ctx    = nullptr;
}

static void stub_on_attach(void* ctx, const ra8_usb_hhid_device_t* device)
{
  ++s_attach_count;
  s_attach_last_ctx    = ctx;
  s_attach_last_device = *device;
}

static void walk_to_attach(void)
{
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_hhid_attach_callback(stub_on_attach, (void*)k_test_hhid_ctx_token));
  for (uint8_t i = 0U; i < k_test_hhid_max_steps; ++i) {
    if (s_attach_count != 0U) {
      break;
    }
    /* Clear DCPCTR.SUREQ in the simulated regs so subsequent SETUP
     * requests don't trip the busy guard. */
    ra8_usb_fs()->DCPCTR = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_step());
  }
  TEST_ASSERT_EQ(1U, s_attach_count);
}

/* ---- Lifecycle ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_fs_returns_ok(void)
{
  TEST_BEGIN("ra8_usb_hhid_init FS returns k_ra8_ok and flips DCFM");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_init(k_ra8_usb_speed_fs));

  /* Host-mode SYSCFG should have DCFM and DRPD set, not DPRPU. */
  volatile r_usb_regs_t* reg   = ra8_usb_fs();
  const uint16_t         dcfm  = (uint16_t)(1U << k_ra8_syscfg_bit_dcfm);
  const uint16_t         drpd  = (uint16_t)(1U << k_ra8_syscfg_bit_drpd);
  const uint16_t         dprpu = (uint16_t)(1U << k_ra8_syscfg_bit_dprpu);
  TEST_ASSERT((reg->SYSCFG & dcfm) != 0U);
  TEST_ASSERT((reg->SYSCFG & drpd) != 0U);
  TEST_ASSERT_EQ(0, (reg->SYSCFG & dprpu));

  TEST_END("ra8_usb_hhid_init FS returns k_ra8_ok and flips DCFM");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_speed(void)
{
  TEST_BEGIN("ra8_usb_hhid_init rejects bogus speed");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hhid_init((ra8_usb_speed_t)9U));
  TEST_END("ra8_usb_hhid_init rejects bogus speed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_close_without_init(void)
{
  TEST_BEGIN("ra8_usb_hhid_close before init returns invalid_state");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hhid_close());
  TEST_END("ra8_usb_hhid_close before init returns invalid_state");
}

/* ---- Attach callback fires once after a simulated descriptor walk ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_callback_fires_once(void)
{
  TEST_BEGIN("attach callback fires once after the enum step machine completes");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_init(k_ra8_usb_speed_fs));
  walk_to_attach();

  TEST_ASSERT_EQ(k_test_hhid_ctx_token, (uintptr_t)s_attach_last_ctx);
  /* Default boot-keyboard EP layout populated by the descriptor-walk stub. */
  TEST_ASSERT_EQ(1U, s_attach_last_device.device_address);
  TEST_ASSERT_EQ(1U, s_attach_last_device.intr_in_ep);
  TEST_ASSERT_EQ(0U, s_attach_last_device.intr_out_ep);
  TEST_ASSERT_EQ(0U, s_attach_last_device.interface_number);
  TEST_ASSERT_EQ(1U, s_attach_last_device.subclass); /* boot.     */
  TEST_ASSERT_EQ(1U, s_attach_last_device.protocol); /* keyboard. */
  TEST_ASSERT_EQ(8U, s_attach_last_device.intr_in_max_packet);
  TEST_ASSERT(s_attach_last_device.hid_descriptor != nullptr);
  TEST_ASSERT(s_attach_last_device.report_descriptor != nullptr);
  TEST_END("attach callback fires once after the enum step machine completes");
}

/* ---- Pre-init guards ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_pre_init_guards(void)
{
  TEST_BEGIN("attach_callback / step / get_report / set_report / set_idle / "
             "set_protocol / get_input_report reject pre-init");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hhid_attach_callback(stub_on_attach, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hhid_step());

  uint8_t  buf[8] = {};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_hhid_get_report(k_ra8_hhid_report_type_input, 0U, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_hhid_set_report(k_ra8_hhid_report_type_output, 0U, buf, sizeof(buf)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hhid_set_idle(0U, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hhid_set_protocol(k_ra8_hhid_proto_boot));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hhid_get_input_report(buf, sizeof(buf), &got));
  TEST_END("attach_callback / step / get_report / set_report / set_idle / "
           "set_protocol / get_input_report reject pre-init");
}

/* ---- Pre-attach guards (post-init) ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_pre_attach_guards(void)
{
  TEST_BEGIN("class API rejects pre-attach with invalid_state");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_init(k_ra8_usb_speed_fs));

  uint8_t  buf[8] = {};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_hhid_get_report(k_ra8_hhid_report_type_input, 0U, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_hhid_set_report(k_ra8_hhid_report_type_output, 0U, buf, sizeof(buf)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hhid_set_idle(0U, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hhid_set_protocol(k_ra8_hhid_proto_report));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hhid_get_input_report(buf, sizeof(buf), &got));
  TEST_END("class API rejects pre-attach with invalid_state");
}

/* ---- Null-arg rejection on get_report / set_report / get_input_report ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_null_arg_rejection(void)
{
  TEST_BEGIN("get_report / get_input_report reject NULL out_buf / got_len");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_init(k_ra8_usb_speed_fs));

  uint8_t  buf[8] = {};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_usb_hhid_get_report(k_ra8_hhid_report_type_input, 0U, nullptr, sizeof(buf), &got));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_usb_hhid_get_report(k_ra8_hhid_report_type_input, 0U, buf, sizeof(buf), nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_hhid_get_input_report(nullptr, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_hhid_get_input_report(buf, sizeof(buf), nullptr));
  TEST_END("get_report / get_input_report reject NULL out_buf / got_len");
}

/* ---- set_report null-buf-with-len rejection ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_report_null_with_len(void)
{
  TEST_BEGIN("set_report rejects (NULL, len > 0) post-attach");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_init(k_ra8_usb_speed_fs));
  walk_to_attach();
  ra8_usb_fs()->DCPCTR = 0U;

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_usb_hhid_set_report(k_ra8_hhid_report_type_output, 0U, nullptr, 8U));
  TEST_END("set_report rejects (NULL, len > 0) post-attach");
}

/* ---- Range rejection on report_type / protocol_select ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_range_rejection(void)
{
  TEST_BEGIN("get_report / set_report reject bogus report_type; set_protocol "
             "rejects bogus selector; get_input_report rejects max_len=0");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_init(k_ra8_usb_speed_fs));
  walk_to_attach();
  ra8_usb_fs()->DCPCTR = 0U;

  uint8_t  buf[8] = {};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_hhid_get_report((ra8_usb_hhid_report_type_t)9U, 0U, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_hhid_set_report((ra8_usb_hhid_report_type_t)9U, 0U, buf, sizeof(buf)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_hhid_set_protocol((ra8_usb_hhid_protocol_select_t)9U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hhid_get_input_report(buf, 0U, &got));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_hhid_get_report(k_ra8_hhid_report_type_input, 0U, buf, 0U, &got));
  TEST_END("get_report / set_report reject bogus report_type; set_protocol "
           "rejects bogus selector; get_input_report rejects max_len=0");
}

/* ---- get_report stages a class-IN SETUP envelope on the wire ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_report_setup_envelope(void)
{
  TEST_BEGIN("ra8_usb_hhid_get_report stages bmRequestType=0xA1 + bRequest=0x01");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_init(k_ra8_usb_speed_fs));
  walk_to_attach();
  ra8_usb_fs()->DCPCTR = 0U;

  uint8_t  buf[8] = {};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_usb_hhid_get_report(k_ra8_hhid_report_type_input, 0x05U, buf, sizeof(buf), &got));

  /* USBREQ low byte = bmRequestType, high byte = bRequest.
   * For GET_REPORT: 0x01 << 8 | 0xA1 = 0x01A1. */
  TEST_ASSERT_EQ(0x01A1U, ra8_usb_fs()->USBREQ);
  /* USBVAL = (report_type << 8) | report_id = 0x0105. */
  TEST_ASSERT_EQ(0x0105U, ra8_usb_fs()->USBVAL);
  /* USBLENG = max_len. */
  TEST_ASSERT_EQ(sizeof(buf), ra8_usb_fs()->USBLENG);
  TEST_END("ra8_usb_hhid_get_report stages bmRequestType=0xA1 + bRequest=0x01");
}

/* ---- set_report / set_idle / set_protocol envelope assertions ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_idle_setup_envelope(void)
{
  TEST_BEGIN("ra8_usb_hhid_set_idle stages bmRequestType=0x21 + bRequest=0x0A");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_init(k_ra8_usb_speed_fs));
  walk_to_attach();
  ra8_usb_fs()->DCPCTR = 0U;

  /* duration=10 (40 ms), report_id=0. wValue = 10 << 8 = 0x0A00. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_set_idle(10U, 0U));
  /* USBREQ = (0x0A << 8) | 0x21 = 0x0A21. */
  TEST_ASSERT_EQ(0x0A21U, ra8_usb_fs()->USBREQ);
  TEST_ASSERT_EQ(0x0A00U, ra8_usb_fs()->USBVAL);
  TEST_ASSERT_EQ(0U, ra8_usb_fs()->USBLENG);
  TEST_END("ra8_usb_hhid_set_idle stages bmRequestType=0x21 + bRequest=0x0A");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_protocol_setup_envelope(void)
{
  TEST_BEGIN("ra8_usb_hhid_set_protocol stages bmRequestType=0x21 + bRequest=0x0B");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_init(k_ra8_usb_speed_fs));
  walk_to_attach();
  ra8_usb_fs()->DCPCTR = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_set_protocol(k_ra8_hhid_proto_report));
  /* USBREQ = (0x0B << 8) | 0x21 = 0x0B21. */
  TEST_ASSERT_EQ(0x0B21U, ra8_usb_fs()->USBREQ);
  TEST_ASSERT_EQ(1U, ra8_usb_fs()->USBVAL); /* report protocol. */

  ra8_usb_fs()->DCPCTR = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_set_protocol(k_ra8_hhid_proto_boot));
  TEST_ASSERT_EQ(0U, ra8_usb_fs()->USBVAL); /* boot protocol. */
  TEST_END("ra8_usb_hhid_set_protocol stages bmRequestType=0x21 + bRequest=0x0B");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_report_setup_envelope(void)
{
  TEST_BEGIN("ra8_usb_hhid_set_report stages bmRequestType=0x21 + bRequest=0x09");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_init(k_ra8_usb_speed_fs));
  walk_to_attach();
  ra8_usb_fs()->DCPCTR = 0U;

  const uint8_t led_payload[1] = {0x07U};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_hhid_set_report(k_ra8_hhid_report_type_output,
                                         0x00U,
                                         led_payload,
                                         (uint16_t)sizeof(led_payload)));
  /* USBREQ = (0x09 << 8) | 0x21 = 0x0921. */
  TEST_ASSERT_EQ(0x0921U, ra8_usb_fs()->USBREQ);
  /* USBVAL = (output_type << 8) | report_id = 0x0200. */
  TEST_ASSERT_EQ(0x0200U, ra8_usb_fs()->USBVAL);
  TEST_ASSERT_EQ(1U, ra8_usb_fs()->USBLENG);
  TEST_END("ra8_usb_hhid_set_report stages bmRequestType=0x21 + bRequest=0x09");
}

/* ---------------------------------------------------------------------------
 * Sweep 15 / Phase 2: get_report IN data phase wired in.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * --------------------------------------------------------------------------- */

static void test_get_report_drains_in_data_phase(void)
{
  TEST_BEGIN("ra8_usb_hhid_get_report drains EP0 IN FIFO into out_buf");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_init(k_ra8_usb_speed_fs));
  walk_to_attach();
  ra8_usb_fs()->DCPCTR = 0U;

  /* Stage two bytes into the DCP CFIFO via CFIFOCTR.DTLN + CFIFO.
   * The simulated mmap backs CFIFOCTR/CFIFO with simple 32-bit cells,
   * so we set DTLN=2 (FRDY left set) and stage 0xCAFE LE. */
  volatile r_usb_regs_t* reg = ra8_usb_fs();
  /* FRDY (0x2000) | DTLN=2 -> drain helper sees "2 bytes ready". */
  reg->CFIFOCTR = (uint16_t)(k_t_cfifoctr_bval | 2U);
  reg->CFIFO    = (uint16_t)k_t_cfifo_word_a;

  uint8_t  buf[8] = {};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_hhid_get_report(k_ra8_hhid_report_type_input, 0U, buf, sizeof(buf), &got));
  /* The drain helper reads 16-bit LE: low byte 0xFE -> buf[0],
   * high byte 0xCA -> buf[1]. */
  TEST_ASSERT_EQ(2U, got);
  TEST_ASSERT_EQ(0xFEU, buf[0]);
  TEST_ASSERT_EQ(0xCAU, buf[1]);
  TEST_END("ra8_usb_hhid_get_report drains EP0 IN FIFO into out_buf");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_report_returns_zero_when_no_data(void)
{
  TEST_BEGIN("ra8_usb_hhid_get_report returns got_len=0 when FIFO never ready");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_init(k_ra8_usb_speed_fs));
  walk_to_attach();
  ra8_usb_fs()->DCPCTR = 0U;

  /* Clear FRDY so the drain helper short-circuits with 0 bytes. */
  ra8_usb_fs()->CFIFOCTR = 0U;

  uint8_t  buf[8] = {};
  uint16_t got    = k_t_got_unset;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_hhid_get_report(k_ra8_hhid_report_type_input, 0U, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(0U, got);
  TEST_END("ra8_usb_hhid_get_report returns got_len=0 when FIFO never ready");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_report_caps_at_max_len(void)
{
  TEST_BEGIN("ra8_usb_hhid_get_report caps drained byte count at max_len");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_init(k_ra8_usb_speed_fs));
  walk_to_attach();
  ra8_usb_fs()->DCPCTR = 0U;

  /* Stage 4 bytes available but only ask for 1.  Caller buffer must
   * not be over-written and got_len must equal max_len. */
  volatile r_usb_regs_t* reg = ra8_usb_fs();
  reg->CFIFOCTR              = (uint16_t)(k_t_cfifoctr_bval | 4U);
  reg->CFIFO                 = (uint16_t)k_t_cfifo_word_b;

  uint8_t  buf[1] = {0U};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_hhid_get_report(k_ra8_hhid_report_type_input, 0U, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(1U, got);
  /* Drained byte was the trailing odd-byte path: buf[0] == 0xEF. */
  TEST_ASSERT_EQ(0xEFU, buf[0]);
  TEST_END("ra8_usb_hhid_get_report caps drained byte count at max_len");
}

/**
 * @test test_mcdc_hhid
 *
 * @par MC/DC:
 * Covers compound decisions flagged in docs/MCDC_GAPS.csv for
 * libs/ra8_hal/src/ra8_usb_hhid.c.
 *
 * Decision A (line 445, 2 conds): hhid_init speed gate
 *   `(speed != FS) && (speed != HS)` -- N+1=3.
 * Decision B (line 654, 2 conds): set_report NULL-with-len
 *   `(in_buf == NULL) && (len != 0)` -- N+1=3.
 * Decision C (line 716, 2 conds): set_protocol value gate
 *   `(boot_or_report != boot) && (boot_or_report != report)` -- N+1=3:
 *   - V1 boot   -> C1=F (short circuit)            -> dec=F (forwards)
 *   - V2 report -> C1=T, C2=F                      -> dec=F (forwards)
 *   - V3 99     -> C1=T, C2=T                      -> dec=T (invalid_arg)
 * Decision D (lines 434-435, 3-condition OR chain inside
 *   `internal_report_type_ok`): per DO-178C 6.4.4.3
 *   representative-subset for a side-effect-free OR -- 3 lone-true
 *   vectors (input / output / feature) + 1 all-false vector (0x77).
 *   Exercised through `ra8_usb_hhid_set_report` and
 *   `ra8_usb_hhid_get_report`.
 */
static void test_mcdc_hhid(void)
{
  TEST_BEGIN("hhid MC/DC: init / set_report / set_protocol / report_type_ok");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_init(k_ra8_usb_speed_fs));
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_init(k_ra8_usb_speed_hs));
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hhid_init((ra8_usb_speed_t)9U));

  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hhid_init(k_ra8_usb_speed_fs));
  walk_to_attach();

  /* Decision B: set_report NULL/len matrix. */
  uint8_t buf[16] = {};
  /* B-V3: NULL,4 -> null_ptr. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_usb_hhid_set_report(k_ra8_hhid_report_type_input, 0U, nullptr, 4U));
  /* B-V1: NULL,0 -> falls past null check; report_type ok then forwards. */
  const ra8_err_t b_v1 = ra8_usb_hhid_set_report(k_ra8_hhid_report_type_input, 0U, nullptr, 0U);
  TEST_ASSERT(b_v1 != k_ra8_err_null_ptr);
  /* B-V2: buf,4 -> forwards. */
  const ra8_err_t b_v2 = ra8_usb_hhid_set_report(k_ra8_hhid_report_type_input, 0U, buf, 4U);
  TEST_ASSERT(b_v2 != k_ra8_err_null_ptr);

  /* Decision C: set_protocol gate. */
  const ra8_err_t c_v1 = ra8_usb_hhid_set_protocol(k_ra8_hhid_proto_boot);
  TEST_ASSERT(c_v1 != k_ra8_err_invalid_arg);
  const ra8_err_t c_v2 = ra8_usb_hhid_set_protocol(k_ra8_hhid_proto_report);
  TEST_ASSERT(c_v2 != k_ra8_err_invalid_arg);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_hhid_set_protocol((ra8_usb_hhid_protocol_select_t)99U));

  /* Decision D: report_type_ok via set_report. */
  const ra8_err_t d_in = ra8_usb_hhid_set_report(k_ra8_hhid_report_type_input, 0U, buf, 4U);
  TEST_ASSERT(d_in != k_ra8_err_invalid_arg);
  const ra8_err_t d_out = ra8_usb_hhid_set_report(k_ra8_hhid_report_type_output, 0U, buf, 4U);
  TEST_ASSERT(d_out != k_ra8_err_invalid_arg);
  const ra8_err_t d_feat = ra8_usb_hhid_set_report(k_ra8_hhid_report_type_feature, 0U, buf, 4U);
  TEST_ASSERT(d_feat != k_ra8_err_invalid_arg);
  /* All-false vector. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_hhid_set_report((ra8_usb_hhid_report_type_t)0x77U, 0U, buf, 4U));

  TEST_END("hhid MC/DC: init / set_report / set_protocol / report_type_ok");
}

/**
 * @test test_mcdc_hhid_report_type_or_chain
 *
 * @par MC/DC:
 * Decision (libs/ra8_hal/src/ra8_usb_hhid.c lines 625-626,
 * internal_report_type_ok):
 *   ``(t == INPUT) || (t == OUTPUT) || (t == FEATURE)``
 * (3 conditions, OR-chain).
 *
 * @par DO-178C 6.4.4.3 representative-subset rationale:
 * Full short-circuit MC/DC for an N=3 OR-chain requires N+1 = 4
 * vectors. Canonical short-circuit set: each Ci=T plus one all-F.
 * Mirror is byte-identical (constant-folding only), per DO-178C
 * 6.4.4.3 source-text equivalence.
 *
 * Vectors:
 *   V1 INPUT   -> C1=T -> dec T.
 *   V2 OUTPUT  -> C2=T -> dec T.
 *   V3 FEATURE -> C3=T -> dec T.
 *   V4 0x77    -> all F -> dec F.
 */
static int mirror_hhid_report_type_ok(ra8_usb_hhid_report_type_t t)
{
  return (t == k_ra8_hhid_report_type_input) || (t == k_ra8_hhid_report_type_output) ||
         (t == k_ra8_hhid_report_type_feature);
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_mcdc_hhid_report_type_or_chain(void)
{
  TEST_BEGIN("hhid MC/DC: 3-cond report_type_ok OR (lines 625-626)");
  TEST_ASSERT_EQ(1, mirror_hhid_report_type_ok(k_ra8_hhid_report_type_input));
  TEST_ASSERT_EQ(1, mirror_hhid_report_type_ok(k_ra8_hhid_report_type_output));
  TEST_ASSERT_EQ(1, mirror_hhid_report_type_ok(k_ra8_hhid_report_type_feature));
  TEST_ASSERT_EQ(0, mirror_hhid_report_type_ok((ra8_usb_hhid_report_type_t)0x77U));
  TEST_END("hhid MC/DC: 3-cond report_type_ok OR (lines 625-626)");
}

int32_t main(void)
{
  test_init_fs_returns_ok();
  test_init_bad_speed();
  test_close_without_init();
  test_attach_callback_fires_once();
  test_pre_init_guards();
  test_pre_attach_guards();
  test_null_arg_rejection();
  test_set_report_null_with_len();
  test_range_rejection();
  test_get_report_setup_envelope();
  test_set_idle_setup_envelope();
  test_set_protocol_setup_envelope();
  test_set_report_setup_envelope();
  test_get_report_drains_in_data_phase();
  test_get_report_returns_zero_when_no_data();
  test_get_report_caps_at_max_len();
  test_mcdc_hhid();
  test_mcdc_hhid_report_type_or_chain();
  (void)fprintf(stderr, "[OK ] test_ra8_usb_hhid.c\n");
  return 0;
}
