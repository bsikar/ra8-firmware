/**
 * @file test_ra_usb_hhid.c
 * @brief Unit tests for the native USB host-side HID class layer
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_usb_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "ra_usb.h"
#include "ra_usb_hhid.h"
#include "unity_minimal.h"

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

static uint32_t             s_attach_count;
static ra_usb_hhid_device_t s_attach_last_device;
static void*                s_attach_last_ctx;
static const uintptr_t      k_test_hhid_ctx_token = 0xFEEDFACEU;

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  (void)ra_usb_hhid_close();
  s_attach_count       = 0U;
  s_attach_last_device = (ra_usb_hhid_device_t){};
  s_attach_last_ctx    = nullptr;
}

static void stub_on_attach(void* ctx, const ra_usb_hhid_device_t* device)
{
  ++s_attach_count;
  s_attach_last_ctx    = ctx;
  s_attach_last_device = *device;
}

static void walk_to_attach(void)
{
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_usb_hhid_attach_callback(stub_on_attach, (void*)k_test_hhid_ctx_token));
  for (uint8_t i = 0U; i < k_test_hhid_max_steps; ++i) {
    if (s_attach_count != 0U) {
      break;
    }
    /* Clear DCPCTR.SUREQ in the simulated regs so subsequent SETUP
     * requests don't trip the busy guard. */
    ra_usb_fs()->DCPCTR = 0U;
    TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhid_step());
  }
  TEST_ASSERT_EQ((int32_t)1U, (int32_t)s_attach_count);
}

/* ---- Lifecycle ---- */

static void test_init_fs_returns_ok(void)
{
  TEST_BEGIN("ra_usb_hhid_init FS returns k_ra_ok and flips DCFM");
  prep();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhid_init(k_ra_usb_speed_fs));

  /* Host-mode SYSCFG should have DCFM and DRPD set, not DPRPU. */
  volatile r_usb_regs_t* reg   = ra_usb_fs();
  const uint16_t         dcfm  = (uint16_t)(1U << k_ra_syscfg_bit_dcfm);
  const uint16_t         drpd  = (uint16_t)(1U << k_ra_syscfg_bit_drpd);
  const uint16_t         dprpu = (uint16_t)(1U << k_ra_syscfg_bit_dprpu);
  TEST_ASSERT((reg->SYSCFG & dcfm) != 0U);
  TEST_ASSERT((reg->SYSCFG & drpd) != 0U);
  TEST_ASSERT_EQ(0, (int)(reg->SYSCFG & dprpu));

  TEST_END("ra_usb_hhid_init FS returns k_ra_ok and flips DCFM");
}

static void test_init_bad_speed(void)
{
  TEST_BEGIN("ra_usb_hhid_init rejects bogus speed");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_hhid_init((ra_usb_speed_t)9U));
  TEST_END("ra_usb_hhid_init rejects bogus speed");
}

static void test_close_without_init(void)
{
  TEST_BEGIN("ra_usb_hhid_close before init returns invalid_state");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_hhid_close());
  TEST_END("ra_usb_hhid_close before init returns invalid_state");
}

/* ---- Attach callback fires once after a simulated descriptor walk ---- */

static void test_attach_callback_fires_once(void)
{
  TEST_BEGIN("attach callback fires once after the enum step machine completes");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhid_init(k_ra_usb_speed_fs));
  walk_to_attach();

  TEST_ASSERT_EQ((int32_t)k_test_hhid_ctx_token, (int32_t)(uintptr_t)s_attach_last_ctx);
  /* Default boot-keyboard EP layout populated by the descriptor-walk stub. */
  TEST_ASSERT_EQ((int32_t)1U, (int32_t)s_attach_last_device.device_address);
  TEST_ASSERT_EQ((int32_t)1U, (int32_t)s_attach_last_device.intr_in_ep);
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)s_attach_last_device.intr_out_ep);
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)s_attach_last_device.interface_number);
  TEST_ASSERT_EQ((int32_t)1U, (int32_t)s_attach_last_device.subclass); /* boot. */
  TEST_ASSERT_EQ((int32_t)1U, (int32_t)s_attach_last_device.protocol); /* keyboard. */
  TEST_ASSERT_EQ((int32_t)8U, (int32_t)s_attach_last_device.intr_in_max_packet);
  TEST_ASSERT(s_attach_last_device.hid_descriptor != nullptr);
  TEST_ASSERT(s_attach_last_device.report_descriptor != nullptr);
  TEST_END("attach callback fires once after the enum step machine completes");
}

/* ---- Pre-init guards ---- */

static void test_pre_init_guards(void)
{
  TEST_BEGIN("attach_callback / step / get_report / set_report / set_idle / "
             "set_protocol / get_input_report reject pre-init");
  prep();

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_hhid_attach_callback(stub_on_attach, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_hhid_step());

  uint8_t  buf[8] = {};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_state,
    (int32_t)ra_usb_hhid_get_report(k_ra_hhid_report_type_input, 0U, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_state,
    (int32_t)ra_usb_hhid_set_report(k_ra_hhid_report_type_output, 0U, buf, sizeof(buf)));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_hhid_set_idle(0U, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_hhid_set_protocol(k_ra_hhid_proto_boot));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_hhid_get_input_report(buf, sizeof(buf), &got));
  TEST_END("attach_callback / step / get_report / set_report / set_idle / "
           "set_protocol / get_input_report reject pre-init");
}

/* ---- Pre-attach guards (post-init) ---- */

static void test_pre_attach_guards(void)
{
  TEST_BEGIN("class API rejects pre-attach with invalid_state");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhid_init(k_ra_usb_speed_fs));

  uint8_t  buf[8] = {};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_state,
    (int32_t)ra_usb_hhid_get_report(k_ra_hhid_report_type_input, 0U, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_state,
    (int32_t)ra_usb_hhid_set_report(k_ra_hhid_report_type_output, 0U, buf, sizeof(buf)));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_hhid_set_idle(0U, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_hhid_set_protocol(k_ra_hhid_proto_report));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_hhid_get_input_report(buf, sizeof(buf), &got));
  TEST_END("class API rejects pre-attach with invalid_state");
}

/* ---- Null-arg rejection on get_report / set_report / get_input_report ---- */

static void test_null_arg_rejection(void)
{
  TEST_BEGIN("get_report / get_input_report reject NULL out_buf / got_len");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhid_init(k_ra_usb_speed_fs));

  uint8_t  buf[8] = {};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_null_ptr,
    (int32_t)ra_usb_hhid_get_report(k_ra_hhid_report_type_input, 0U, nullptr, sizeof(buf), &got));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_null_ptr,
    (int32_t)ra_usb_hhid_get_report(k_ra_hhid_report_type_input, 0U, buf, sizeof(buf), nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_usb_hhid_get_input_report(nullptr, sizeof(buf), &got));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_usb_hhid_get_input_report(buf, sizeof(buf), nullptr));
  TEST_END("get_report / get_input_report reject NULL out_buf / got_len");
}

/* ---- set_report null-buf-with-len rejection ---- */

static void test_set_report_null_with_len(void)
{
  TEST_BEGIN("set_report rejects (NULL, len > 0) post-attach");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhid_init(k_ra_usb_speed_fs));
  walk_to_attach();
  ra_usb_fs()->DCPCTR = 0U;

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_usb_hhid_set_report(k_ra_hhid_report_type_output, 0U, nullptr, 8U));
  TEST_END("set_report rejects (NULL, len > 0) post-attach");
}

/* ---- Range rejection on report_type / protocol_select ---- */

static void test_range_rejection(void)
{
  TEST_BEGIN("get_report / set_report reject bogus report_type; set_protocol "
             "rejects bogus selector; get_input_report rejects max_len=0");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhid_init(k_ra_usb_speed_fs));
  walk_to_attach();
  ra_usb_fs()->DCPCTR = 0U;

  uint8_t  buf[8] = {};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_usb_hhid_get_report((ra_usb_hhid_report_type_t)9U, 0U, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_usb_hhid_set_report((ra_usb_hhid_report_type_t)9U, 0U, buf, sizeof(buf)));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_hhid_set_protocol((ra_usb_hhid_protocol_select_t)9U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_hhid_get_input_report(buf, 0U, &got));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_hhid_get_report(k_ra_hhid_report_type_input, 0U, buf, 0U, &got));
  TEST_END("get_report / set_report reject bogus report_type; set_protocol "
           "rejects bogus selector; get_input_report rejects max_len=0");
}

/* ---- get_report stages a class-IN SETUP envelope on the wire ---- */

static void test_get_report_setup_envelope(void)
{
  TEST_BEGIN("ra_usb_hhid_get_report stages bmRequestType=0xA1 + bRequest=0x01");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhid_init(k_ra_usb_speed_fs));
  walk_to_attach();
  ra_usb_fs()->DCPCTR = 0U;

  uint8_t  buf[8] = {};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_usb_hhid_get_report(k_ra_hhid_report_type_input, 0x05U, buf, sizeof(buf), &got));

  /* USBREQ low byte = bmRequestType, high byte = bRequest.
   * For GET_REPORT: 0x01 << 8 | 0xA1 = 0x01A1. */
  TEST_ASSERT_EQ((int32_t)0x01A1U, (int32_t)ra_usb_fs()->USBREQ);
  /* USBVAL = (report_type << 8) | report_id = 0x0105. */
  TEST_ASSERT_EQ((int32_t)0x0105U, (int32_t)ra_usb_fs()->USBVAL);
  /* USBLENG = max_len. */
  TEST_ASSERT_EQ((int32_t)sizeof(buf), (int32_t)ra_usb_fs()->USBLENG);
  TEST_END("ra_usb_hhid_get_report stages bmRequestType=0xA1 + bRequest=0x01");
}

/* ---- set_report / set_idle / set_protocol envelope assertions ---- */

static void test_set_idle_setup_envelope(void)
{
  TEST_BEGIN("ra_usb_hhid_set_idle stages bmRequestType=0x21 + bRequest=0x0A");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhid_init(k_ra_usb_speed_fs));
  walk_to_attach();
  ra_usb_fs()->DCPCTR = 0U;

  /* duration=10 (40 ms), report_id=0. wValue = 10 << 8 = 0x0A00. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhid_set_idle(10U, 0U));
  /* USBREQ = (0x0A << 8) | 0x21 = 0x0A21. */
  TEST_ASSERT_EQ((int32_t)0x0A21U, (int32_t)ra_usb_fs()->USBREQ);
  TEST_ASSERT_EQ((int32_t)0x0A00U, (int32_t)ra_usb_fs()->USBVAL);
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)ra_usb_fs()->USBLENG);
  TEST_END("ra_usb_hhid_set_idle stages bmRequestType=0x21 + bRequest=0x0A");
}

static void test_set_protocol_setup_envelope(void)
{
  TEST_BEGIN("ra_usb_hhid_set_protocol stages bmRequestType=0x21 + bRequest=0x0B");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhid_init(k_ra_usb_speed_fs));
  walk_to_attach();
  ra_usb_fs()->DCPCTR = 0U;

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhid_set_protocol(k_ra_hhid_proto_report));
  /* USBREQ = (0x0B << 8) | 0x21 = 0x0B21. */
  TEST_ASSERT_EQ((int32_t)0x0B21U, (int32_t)ra_usb_fs()->USBREQ);
  TEST_ASSERT_EQ((int32_t)1U, (int32_t)ra_usb_fs()->USBVAL); /* report protocol. */

  ra_usb_fs()->DCPCTR = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhid_set_protocol(k_ra_hhid_proto_boot));
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)ra_usb_fs()->USBVAL); /* boot protocol. */
  TEST_END("ra_usb_hhid_set_protocol stages bmRequestType=0x21 + bRequest=0x0B");
}

static void test_set_report_setup_envelope(void)
{
  TEST_BEGIN("ra_usb_hhid_set_report stages bmRequestType=0x21 + bRequest=0x09");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhid_init(k_ra_usb_speed_fs));
  walk_to_attach();
  ra_usb_fs()->DCPCTR = 0U;

  const uint8_t led_payload[1] = {0x07U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_usb_hhid_set_report(k_ra_hhid_report_type_output,
                                                 0x00U,
                                                 led_payload,
                                                 (uint16_t)sizeof(led_payload)));
  /* USBREQ = (0x09 << 8) | 0x21 = 0x0921. */
  TEST_ASSERT_EQ((int32_t)0x0921U, (int32_t)ra_usb_fs()->USBREQ);
  /* USBVAL = (output_type << 8) | report_id = 0x0200. */
  TEST_ASSERT_EQ((int32_t)0x0200U, (int32_t)ra_usb_fs()->USBVAL);
  TEST_ASSERT_EQ((int32_t)1U, (int32_t)ra_usb_fs()->USBLENG);
  TEST_END("ra_usb_hhid_set_report stages bmRequestType=0x21 + bRequest=0x09");
}

/* ---------------------------------------------------------------------------
 * Sweep 15 / Phase 2: get_report IN data phase wired in.
 * --------------------------------------------------------------------------- */

static void test_get_report_drains_in_data_phase(void)
{
  TEST_BEGIN("ra_usb_hhid_get_report drains EP0 IN FIFO into out_buf");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhid_init(k_ra_usb_speed_fs));
  walk_to_attach();
  ra_usb_fs()->DCPCTR = 0U;

  /* Stage two bytes into the DCP CFIFO via CFIFOCTR.DTLN + CFIFO.
   * The simulated mmap backs CFIFOCTR/CFIFO with simple 32-bit cells,
   * so we set DTLN=2 (FRDY left set) and stage 0xCAFE LE. */
  volatile r_usb_regs_t* reg = ra_usb_fs();
  /* FRDY (0x2000) | DTLN=2 -> drain helper sees "2 bytes ready". */
  reg->CFIFOCTR = (uint16_t)(0x2000U | 2U);
  reg->CFIFO    = (uint16_t)0xCAFEU;

  uint8_t  buf[8] = {};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_usb_hhid_get_report(k_ra_hhid_report_type_input, 0U, buf, sizeof(buf), &got));
  /* The drain helper reads 16-bit LE: low byte 0xFE -> buf[0],
   * high byte 0xCA -> buf[1]. */
  TEST_ASSERT_EQ((int32_t)2U, (int32_t)got);
  TEST_ASSERT_EQ((int32_t)0xFEU, (int32_t)buf[0]);
  TEST_ASSERT_EQ((int32_t)0xCAU, (int32_t)buf[1]);
  TEST_END("ra_usb_hhid_get_report drains EP0 IN FIFO into out_buf");
}

static void test_get_report_returns_zero_when_no_data(void)
{
  TEST_BEGIN("ra_usb_hhid_get_report returns got_len=0 when FIFO never ready");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhid_init(k_ra_usb_speed_fs));
  walk_to_attach();
  ra_usb_fs()->DCPCTR = 0U;

  /* Clear FRDY so the drain helper short-circuits with 0 bytes. */
  ra_usb_fs()->CFIFOCTR = 0U;

  uint8_t  buf[8] = {};
  uint16_t got    = 0xFFFFU;
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_usb_hhid_get_report(k_ra_hhid_report_type_input, 0U, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)got);
  TEST_END("ra_usb_hhid_get_report returns got_len=0 when FIFO never ready");
}

static void test_get_report_caps_at_max_len(void)
{
  TEST_BEGIN("ra_usb_hhid_get_report caps drained byte count at max_len");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_hhid_init(k_ra_usb_speed_fs));
  walk_to_attach();
  ra_usb_fs()->DCPCTR = 0U;

  /* Stage 4 bytes available but only ask for 1.  Caller buffer must
   * not be over-written and got_len must equal max_len. */
  volatile r_usb_regs_t* reg = ra_usb_fs();
  reg->CFIFOCTR              = (uint16_t)(0x2000U | 4U);
  reg->CFIFO                 = (uint16_t)0xBEEFU;

  uint8_t  buf[1] = {0U};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_usb_hhid_get_report(k_ra_hhid_report_type_input, 0U, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ((int32_t)1U, (int32_t)got);
  /* Drained byte was the trailing odd-byte path: buf[0] == 0xEF. */
  TEST_ASSERT_EQ((int32_t)0xEFU, (int32_t)buf[0]);
  TEST_END("ra_usb_hhid_get_report caps drained byte count at max_len");
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
  (void)fprintf(stderr, "[OK ] test_ra_usb_hhid.c\n");
  return 0;
}
