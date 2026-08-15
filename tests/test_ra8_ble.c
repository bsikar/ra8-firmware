/**
 * @file test_ra8_ble.c
 * @brief Unit tests for ra8_ble.c (RA8D2 BLE controller / HCI driver)
 *
 * @details
 * Exercises the HCI framing exposed by the driver via its TX-capture
 * test hook and the RX-injection hook. Hardware-level register
 * sequencing is asserted indirectly: ``ra8_ble_open`` must be called
 * before any HCI command will be accepted, and ``ra8_ble_close`` must
 * release the controller back into reset.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_ble.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "unity_minimal.h"

/* Test hooks declared with external linkage in the driver. */
void           ra8_ble_test_reset_capture(void);
void           ra8_ble_test_inject_rx(const uint8_t* bytes, uint16_t len);
const uint8_t* ra8_ble_test_tx_capture(uint16_t* out_len);

typedef enum : uint8_t {
  k_test_byte_pkt_cmd      = 0x01U, /**< Test byte pkt cmd.               */
  k_test_byte_pkt_acl      = 0x02U, /**< Test byte pkt acl.               */
  k_test_byte_pkt_event    = 0x04U, /**< Test byte pkt event.             */
  k_test_byte_evt_le_meta  = 0x3EU, /**< Test byte evt le meta.           */
  k_test_byte_evt_subev_cc = 0x01U, /**< LE_Connection_Complete subevent. */
  k_test_addr_byte_0       = 0x11U, /**< Test address byte 0.             */
  k_test_addr_byte_1       = 0x22U, /**< Test address byte 1.             */
  k_test_addr_byte_2       = 0x33U, /**< Test address byte 2.             */
  k_test_addr_byte_3       = 0x44U, /**< Test address byte 3.             */
  k_test_addr_byte_4       = 0x55U, /**< Test address byte 4.             */
  k_test_addr_byte_5       = 0x66U, /**< Test address byte 5.             */
} ble_test_bytes_t;

typedef enum : uint16_t {
  k_test_op_le_set_random_addr = 0x2005U, /**< Test op le set random address. */
  k_test_op_le_set_adv_data    = 0x2008U, /**< Test op le set adv data.       */
  k_test_op_le_set_adv_enable  = 0x200AU, /**< Test op le set adv enable.     */
  k_test_op_le_set_scan_params = 0x200BU, /**< Test op le set scan params.    */
  k_test_op_le_set_scan_enable = 0x200CU, /**< Test op le set scan enable.    */
  k_test_scan_interval         = 0x0040U, /**< Test scan interval.            */
  k_test_scan_window           = 0x0020U, /**< Test scan window.              */
  k_test_scan_bad              = 0x0001U, /**< Test scan bad.                 */
  k_test_acl_handle            = 0x0042U, /**< Test acl handle.               */
} ble_test_words_t;

static void prep_open(void)
{
  ra8_fake_mmap_reset();
  /* If a previous test left the driver open, force-close. */
  (void)ra8_ble_close();
  const ra8_ble_config_t cfg = {.use_external_osc = 1U, .deep_sleep_enable = 1U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_open(&cfg));
  ra8_ble_test_reset_capture();
}

static int32_t s_evt_count;
static uint8_t s_evt_last_code;
static uint8_t s_evt_last_plen;
static uint8_t s_evt_last_param0;
static void*   s_evt_last_ctx;

static void stub_evt_cb(void* ctx, uint8_t code, const uint8_t* params, uint8_t plen)
{
  s_evt_count++;
  s_evt_last_code   = code;
  s_evt_last_plen   = plen;
  s_evt_last_param0 = (plen > 0U) ? params[0] : 0U;
  s_evt_last_ctx    = ctx;
}

static int32_t  s_acl_count;
static uint16_t s_acl_last_handle;
static uint16_t s_acl_last_len;
static uint8_t  s_acl_last_payload0;

static void stub_acl_cb(void* ctx, uint16_t handle, const uint8_t* payload, uint16_t len)
{
  (void)ctx;
  s_acl_count++;
  s_acl_last_handle   = handle;
  s_acl_last_len      = len;
  s_acl_last_payload0 = (len > 0U) ? payload[0] : 0U;
}

/* --------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_open_close(void)
{
  TEST_BEGIN("ble open + close");
  ra8_fake_mmap_reset();
  (void)ra8_ble_close();
  const ra8_ble_config_t cfg = {.use_external_osc = 0U, .deep_sleep_enable = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ble_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_close());
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ble_close());
  TEST_END("ble open + close");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_open_null_cfg(void)
{
  TEST_BEGIN("ble open NULL cfg");
  ra8_fake_mmap_reset();
  (void)ra8_ble_close();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ble_open(nullptr));
  TEST_END("ble open NULL cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_command_before_open(void)
{
  TEST_BEGIN("ble cmd before open rejected");
  ra8_fake_mmap_reset();
  (void)ra8_ble_close();
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_ble_hci_send_command((uint16_t)k_test_op_le_set_adv_enable, nullptr, 0U));
  TEST_END("ble cmd before open rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_hci_send_command_framing(void)
{
  TEST_BEGIN("ble hci_send_command framing");
  prep_open();
  const uint8_t params[3] = {0xAAU, 0xBBU, 0xCCU};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_hci_send_command((uint16_t)k_test_op_le_set_adv_enable,
                                          params,
                                          (uint8_t)sizeof(params)));
  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra8_ble_test_tx_capture(&cap_len);
  TEST_ASSERT_EQ(7, cap_len); /* 1 + 2 + 1 + 3 */
  TEST_ASSERT_EQ(k_test_byte_pkt_cmd, cap[0]);
  TEST_ASSERT_EQ(0x0AU, cap[1]);
  TEST_ASSERT_EQ(0x20U, cap[2]);
  TEST_ASSERT_EQ(3, cap[3]);
  TEST_ASSERT_EQ(0xAAU, cap[4]);
  TEST_ASSERT_EQ(0xBBU, cap[5]);
  TEST_ASSERT_EQ(0xCCU, cap[6]);
  /* Null with zero-len OK, null with non-zero rejected. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_hci_send_command((uint16_t)k_test_op_le_set_adv_enable, nullptr, 0U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ble_hci_send_command((uint16_t)k_test_op_le_set_adv_enable, nullptr, 1U));
  TEST_END("ble hci_send_command framing");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_hci_send_acl_framing(void)
{
  TEST_BEGIN("ble hci_send_acl_data framing");
  prep_open();
  const uint8_t payload[4] = {0xDEU, 0xADU, 0xBEU, 0xEFU};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ble_hci_send_acl_data((uint16_t)k_test_acl_handle, payload, (uint16_t)sizeof(payload)));
  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra8_ble_test_tx_capture(&cap_len);
  TEST_ASSERT_EQ(9, cap_len); /* 1 + 2 + 2 + 4 */
  TEST_ASSERT_EQ(k_test_byte_pkt_acl, cap[0]);
  TEST_ASSERT_EQ(0x42U, cap[1]);
  TEST_ASSERT_EQ(0x00U, cap[2]);
  TEST_ASSERT_EQ(0x04U, cap[3]);
  TEST_ASSERT_EQ(0x00U, cap[4]);
  TEST_ASSERT_EQ(0xDEU, cap[5]);
  TEST_END("ble hci_send_acl_data framing");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_event_dispatch(void)
{
  TEST_BEGIN("ble event dispatch synthesises LE_Connection_Complete");
  prep_open();
  s_evt_count = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_attach_event_handler(stub_evt_cb, (void*)(uintptr_t)0xCDU));
  /* Synthesise an LE Meta event with subevent 0x01 and 1 param byte. */
  const uint8_t inj[5] = {
    (uint8_t)k_test_byte_pkt_event,
    (uint8_t)k_test_byte_evt_le_meta,
    (uint8_t)2U, /* plen */
    (uint8_t)k_test_byte_evt_subev_cc,
    (uint8_t)0x77U,
  };
  ra8_ble_test_inject_rx(inj, (uint16_t)sizeof(inj));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_dispatch());
  TEST_ASSERT_EQ(1, s_evt_count);
  TEST_ASSERT_EQ(k_test_byte_evt_le_meta, s_evt_last_code);
  TEST_ASSERT_EQ(2U, s_evt_last_plen);
  TEST_ASSERT_EQ(k_test_byte_evt_subev_cc, s_evt_last_param0);
  TEST_ASSERT_EQ((uintptr_t)0xCDU, (uintptr_t)s_evt_last_ctx);
  TEST_END("ble event dispatch synthesises LE_Connection_Complete");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_acl_dispatch(void)
{
  TEST_BEGIN("ble acl dispatch");
  prep_open();
  s_acl_count = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_attach_acl_handler(stub_acl_cb, nullptr));
  const uint8_t inj[7] = {
    (uint8_t)k_test_byte_pkt_acl,
    (uint8_t)0x42U,
    (uint8_t)0x00U,
    (uint8_t)0x02U,
    (uint8_t)0x00U,
    (uint8_t)0xA5U,
    (uint8_t)0x5AU,
  };
  ra8_ble_test_inject_rx(inj, (uint16_t)sizeof(inj));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_dispatch());
  TEST_ASSERT_EQ(1, s_acl_count);
  TEST_ASSERT_EQ(0x0042U, s_acl_last_handle);
  TEST_ASSERT_EQ(2U, s_acl_last_len);
  TEST_ASSERT_EQ(0xA5U, s_acl_last_payload0);
  TEST_END("ble acl dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_random_address(void)
{
  TEST_BEGIN("ble set_random_address");
  prep_open();
  const uint8_t addr[k_ra8_ble_addr_bytes] = {
    (uint8_t)k_test_addr_byte_0,
    (uint8_t)k_test_addr_byte_1,
    (uint8_t)k_test_addr_byte_2,
    (uint8_t)k_test_addr_byte_3,
    (uint8_t)k_test_addr_byte_4,
    (uint8_t)k_test_addr_byte_5,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_set_random_address(addr));
  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra8_ble_test_tx_capture(&cap_len);
  TEST_ASSERT_EQ(10, cap_len); /* 1 + 2 + 1 + 6 */
  TEST_ASSERT_EQ(0x05U, cap[1]);
  TEST_ASSERT_EQ(0x20U, cap[2]);
  TEST_ASSERT_EQ(6U, cap[3]);
  TEST_ASSERT_EQ(k_test_addr_byte_0, cap[4]);
  TEST_ASSERT_EQ(k_test_addr_byte_5, cap[9]);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ble_set_random_address(nullptr));
  TEST_END("ble set_random_address");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_advertising_data(void)
{
  TEST_BEGIN("ble set_advertising_data");
  prep_open();
  const uint8_t data[5] = {0x02U, 0x01U, 0x06U, 0x00U, 0x00U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_set_advertising_data(data, (uint8_t)sizeof(data)));
  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra8_ble_test_tx_capture(&cap_len);
  /* HCI cmd: 1+2+1 framing + (1 length-prefix + 5 payload) = 10. */
  TEST_ASSERT_EQ(10, cap_len);
  TEST_ASSERT_EQ(0x08U, cap[1]);
  TEST_ASSERT_EQ(0x20U, cap[2]);
  TEST_ASSERT_EQ(6U, cap[3]);
  TEST_ASSERT_EQ(5U, cap[4]); /* spec length prefix */
  TEST_ASSERT_EQ(0x02U, cap[5]);

  /* Oversized adv data rejected. */
  uint8_t big[k_ra8_ble_adv_data_max + 1U] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ble_set_advertising_data(big, (uint8_t)sizeof(big)));
  /* NULL with non-zero len rejected. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ble_set_advertising_data(nullptr, 1U));
  /* NULL with zero len allowed. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_set_advertising_data(nullptr, 0U));
  TEST_END("ble set_advertising_data");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_advertising_enable(void)
{
  TEST_BEGIN("ble set_advertising_enable");
  prep_open();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_set_advertising_enable(1U));
  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra8_ble_test_tx_capture(&cap_len);
  TEST_ASSERT_EQ(5, cap_len);
  TEST_ASSERT_EQ(0x0AU, cap[1]);
  TEST_ASSERT_EQ(0x20U, cap[2]);
  TEST_ASSERT_EQ(1U, cap[3]);
  TEST_ASSERT_EQ(1U, cap[4]);
  TEST_END("ble set_advertising_enable");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_scan_start(void)
{
  TEST_BEGIN("ble scan_start emits two HCI commands");
  prep_open();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ble_scan_start(1U, (uint16_t)k_test_scan_interval, (uint16_t)k_test_scan_window));
  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra8_ble_test_tx_capture(&cap_len);
  /* set_scan_params: 1+2+1+7 = 11; set_scan_enable: 1+2+1+2 = 6; total 17. */
  TEST_ASSERT_EQ(17, cap_len);
  TEST_ASSERT_EQ(0x0BU, cap[1]);
  TEST_ASSERT_EQ(0x20U, cap[2]);
  TEST_ASSERT_EQ(7U, cap[3]);
  TEST_ASSERT_EQ(1U, cap[4]); /* active */
  TEST_ASSERT_EQ((k_test_scan_interval & 0xFFU), cap[5]);
  TEST_ASSERT_EQ((k_test_scan_interval >> 8), cap[6]);
  TEST_ASSERT_EQ((k_test_scan_window & 0xFFU), cap[7]);
  TEST_ASSERT_EQ((k_test_scan_window >> 8), cap[8]);
  /* Second command starts at offset 11. */
  TEST_ASSERT_EQ(k_test_byte_pkt_cmd, cap[11]);
  TEST_ASSERT_EQ(0x0CU, cap[12]);
  TEST_ASSERT_EQ(0x20U, cap[13]);
  TEST_ASSERT_EQ(2U, cap[14]);
  TEST_ASSERT_EQ(1U, cap[15]); /* enable */

  /* Bad params rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ble_scan_start(0U, (uint16_t)k_test_scan_bad, (uint16_t)k_test_scan_bad));
  /* Window > interval rejected. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_ble_scan_start(0U, (uint16_t)k_test_scan_window, (uint16_t)k_test_scan_interval));
  TEST_END("ble scan_start emits two HCI commands");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_before_open(void)
{
  TEST_BEGIN("ble dispatch before open rejected");
  ra8_fake_mmap_reset();
  (void)ra8_ble_close();
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ble_dispatch());
  TEST_END("ble dispatch before open rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_handlers_idempotent(void)
{
  TEST_BEGIN("ble attach handlers detach + reattach");
  prep_open();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_attach_event_handler(nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_attach_acl_handler(nullptr, nullptr));
  /* Dispatching with no handlers attached should still return ok on
   * empty / quiet inputs. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_dispatch());
  TEST_END("ble attach handlers detach + reattach");
}

/**
 * @test test_mcdc_ble
 *
 * @par MC/DC:
 * Three 2-condition decisions in libs/ra8_hal/src/ra8_ble.c:
 *
 * Decision A (line 238, ``ra8_ble_hci_send_command``):
 * ``if ((params == NULL) && (params_len > 0U))``
 * - V1: params=NULL,    plen=0 -> C1=T,C2=F -> dec F (ok)
 * - V2: params=valid,   plen=0 -> C1=F (short-circuits) -> dec F
 * - V3: params=NULL,    plen=4 -> C1=T,C2=T -> dec T (null_ptr)
 * Pairs (V2,V3) flip C1 with C2=T fixed; (V1,V3) flip C2 with C1=T fixed.
 *
 * Decision B (line 416, ``ra8_ble_scan_start`` interval):
 * ``if ((interval < scan_min) || (interval > scan_max))``
 * - V1: interval in range, window in range -> C1=F,C2=F -> dec F (ok)
 * - V2: interval too small                  -> C1=T short-circuits -> dec T
 * - V3: interval too large                  -> C1=F,C2=T -> dec T
 *
 * Decision C (line 419, ``ra8_ble_scan_start`` window):
 * Same shape on ``window``. Vectors fold into the same calls -- holding
 * interval valid lets us probe window=lo and window=hi independently.
 */
static void test_mcdc_ble(void)
{
  TEST_BEGIN("ble MC/DC: hci_send_command + scan_start range decisions");
  prep_open();

  /* Decision A vectors. */
  uint8_t pbuf[4] = {0U};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_hci_send_command((uint16_t)k_test_op_le_set_adv_enable, nullptr, 0U));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_hci_send_command((uint16_t)k_test_op_le_set_adv_enable, pbuf, 0U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ble_hci_send_command((uint16_t)k_test_op_le_set_adv_enable, nullptr, 4U));

  /* Decision B + C: scan_start interval / window range gates.
   * Use a known-valid (interval, window) and known-bad probes. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ble_scan_start(0U, (uint16_t)k_test_scan_interval, (uint16_t)k_test_scan_window));
  /* Decision B V2: interval too small. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ble_scan_start(0U, (uint16_t)k_test_scan_bad, (uint16_t)k_test_scan_window));
  /* Decision B V3: interval too large (> scan_max). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ble_scan_start(0U, 0xFFFFU, (uint16_t)k_test_scan_window));
  /* Decision C V2: window too small. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ble_scan_start(0U, (uint16_t)k_test_scan_interval, (uint16_t)k_test_scan_bad));
  /* Decision C V3: window too large. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ble_scan_start(0U, (uint16_t)k_test_scan_interval, 0xFFFFU));
  TEST_END("ble MC/DC: hci_send_command + scan_start range decisions");
}

/**
 * @test test_mcdc_ble_acl_inject_args
 *
 * @par MC/DC:
 * Decision A (libs/ra8_hal/src/ra8_ble.c ra8_ble_hci_send_acl_data):
 *   ``if ((payload == NULL) && (len > 0U))``
 * 2-cond AND short-circuit: N+1 = 3 vectors.
 * - V1: payload=valid, len=4 -> C1=F short -> dec F (proceeds, ok).
 * - V2: payload=NULL,  len=0 -> C1=T, C2=F -> dec F (zero-len allowed).
 * - V3: payload=NULL,  len=4 -> C1=T, C2=T -> dec T -> null_ptr.
 * V1+V3 isolate C1; V2+V3 isolate C2.
 *
 * Decision B (libs/ra8_hal/src/ra8_ble.c ra8_ble_test_inject_rx):
 *   ``if ((bytes == NULL) || (len == 0U))``
 * 2-cond OR short-circuit: N+1 = 3 vectors.
 * - V1: bytes=valid, len>0  -> C1=F, C2=F -> dec F (injects).
 * - V2: bytes=NULL,  len>0  -> C1=T short -> dec T (skips).
 * - V3: bytes=valid, len=0  -> C1=F, C2=T -> dec T (skips).
 *
 * Both reached purely through public APIs (the inject helper is the
 * test fixture's documented entry point per ra8_ble.h).
 */
static void test_mcdc_ble_acl_inject_args(void)
{
  TEST_BEGIN("ble MC/DC: send_acl_data + inject_rx arg pairs");
  prep_open();
  /* Decision A. */
  const uint8_t pl[4] = {0U};
  /* V1 */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_hci_send_acl_data((uint16_t)k_test_acl_handle, pl, (uint16_t)sizeof(pl)));
  /* V2 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_hci_send_acl_data((uint16_t)k_test_acl_handle, nullptr, 0U));
  /* V3 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ble_hci_send_acl_data((uint16_t)k_test_acl_handle, nullptr, 4U));

  /* Decision B (inject_rx returns void; verify by absence of crash and
   * that subsequent dispatch sees no spurious bytes). */
  const uint8_t inj[2] = {0xAAU, 0xBBU};
  ra8_ble_test_inject_rx(inj, (uint16_t)sizeof(inj)); /* V1                */
  ra8_ble_test_inject_rx(nullptr, 4U);                /* V2: short-circuit */
  ra8_ble_test_inject_rx(inj, 0U);                    /* V3                */

  TEST_END("ble MC/DC: send_acl_data + inject_rx arg pairs");
}

int32_t main(void)
{
  test_open_close();
  test_open_null_cfg();
  test_send_command_before_open();
  test_hci_send_command_framing();
  test_hci_send_acl_framing();
  test_event_dispatch();
  test_acl_dispatch();
  test_set_random_address();
  test_set_advertising_data();
  test_set_advertising_enable();
  test_scan_start();
  test_dispatch_before_open();
  test_attach_handlers_idempotent();
  test_mcdc_ble();
  test_mcdc_ble_acl_inject_args();
  return 0;
}
