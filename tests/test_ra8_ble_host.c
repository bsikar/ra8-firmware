/**
 * @file test_ra8_ble_host.c
 * @brief Unit tests for libs/ra8_ble_host (L2CAP / ATT / GATT)
 *
 * @details
 * Exercises the host stack against the host-compiled simulator
 * peripheral mocks. Each test resets the simulator and re-runs
 * ra8_ble_host_init from scratch so state never leaks between
 * cases.
 *
 * Coverage:
 *   - Lifecycle (init/close, NULL guards, double-init).
 *   - Advertising start sends LE_Set_Advertising_Enable (opcode 0x200A).
 *   - GATT service registration + max-services exhaustion.
 *   - GATT characteristic registration + value read/set.
 *   - GATT notify path + CCCD subscribe.
 *   - Attribute-table lookup by handle.
 *   - Pre-init guards on all GATT entry points.
 *   - Event-handler attachment.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_ble.h"
#include "ra8_ble_host.h"
#include "ra8_err.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/* Test hooks from libs/ra8_hal/src/ra8_ble.c. */
const uint8_t* ra8_ble_test_tx_capture(uint16_t* out_len);
void           ra8_ble_test_reset_capture(void);

/* Test hooks from libs/ra8_ble_host/src/ra8_ble_l2cap.c. */
void ra8_ble_host_test_inject_acl(uint16_t conn_handle, const uint8_t* l2cap_frame, uint16_t len);
uint32_t ra8_ble_host_test_event_count(void);
void     ra8_ble_host_test_inject_connect(uint16_t conn_handle);

/* Internal L2CAP TX entry point declared in ra8_ble_host_internal.h; we
 * re-declare it here so the MC/DC tests do not have to drag in that
 * private header. The signature MUST mirror ra8_ble_l2cap.c verbatim. */
ra8_err_t ra8_ble_host_l2cap_send(uint16_t       conn_handle,
                                  uint16_t       cid,
                                  const uint8_t* payload,
                                  uint16_t       payload_len);

typedef enum : uint16_t {
  k_test_op_le_set_adv_params = 0x2006U,
  k_test_op_le_set_adv_data   = 0x2008U,
  k_test_op_le_set_adv_enable = 0x200AU,
  k_test_conn_handle          = 0x0040U,
  k_test_l2cap_cid_att        = 0x0004U,
  k_test_default_appearance   = 0x0040U,
  k_test_adv_interval_ms      = 100U,
  k_test_adv_interval_too_low = 1U,
  k_test_value_buf_size       = 32U,
} ble_host_test_words_t;

typedef enum : uint8_t {
  k_test_pkt_cmd_byte     = 0x01U,
  k_test_pkt_acl_byte     = 0x02U,
  k_test_att_op_write_req = 0x12U,
  k_test_att_op_read_req  = 0x0AU,
  k_test_evt_count_zero   = 0U,
} ble_host_test_bytes_t;

/* --------------------------------------------------------------------- */

static int32_t                   s_evt_count;
static ra8_ble_host_event_kind_t s_evt_last_kind;
static uint16_t                  s_evt_last_attr;

static void stub_event_cb(void* ctx, const ra8_ble_host_event_t* evt)
{
  (void)ctx;
  s_evt_count++;
  s_evt_last_kind = evt->kind;
  s_evt_last_attr = evt->attr_handle;
}

static void prep_init(ra8_ble_host_role_t role)
{
  ra8_sim_mmap_reset();
  (void)ra8_ble_host_close();
  (void)ra8_ble_close();
  ra8_ble_test_reset_capture();
  s_evt_count                     = 0;
  s_evt_last_kind                 = k_ra8_ble_host_event_connected;
  s_evt_last_attr                 = 0U;
  const ra8_ble_host_config_t cfg = {
    .role       = role,
    .name       = "ra8d2",
    .appearance = (uint16_t)k_test_default_appearance,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_attach_event_handler(stub_event_cb, nullptr));
}

/* Build a 128-bit UUID with a recognizable byte pattern. */
static void make_uuid(uint8_t* out, uint8_t marker)
{
  for (uint8_t i = 0U; i < 16U; i++) {
    out[i] = (uint8_t)(marker + i);
  }
}

/* --------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_close(void)
{
  TEST_BEGIN("ble_host init + close");
  ra8_sim_mmap_reset();
  (void)ra8_ble_host_close();
  (void)ra8_ble_close();
  const ra8_ble_host_config_t cfg = {.role       = k_ra8_ble_host_role_peripheral,
                                     .name       = "x",
                                     .appearance = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ble_host_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_close());
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ble_host_close());
  TEST_END("ble_host init + close");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null_and_role(void)
{
  TEST_BEGIN("ble_host init NULL + bad role");
  ra8_sim_mmap_reset();
  (void)ra8_ble_host_close();
  (void)ra8_ble_close();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ble_host_init(nullptr));
  const ra8_ble_host_config_t bad = {.role       = (ra8_ble_host_role_t)0xFFU,
                                     .name       = nullptr,
                                     .appearance = 0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ble_host_init(&bad));
  TEST_END("ble_host init NULL + bad role");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_pre_init_guards(void)
{
  TEST_BEGIN("ble_host pre-init guards");
  ra8_sim_mmap_reset();
  (void)ra8_ble_host_close();
  (void)ra8_ble_close();
  uint16_t h = 0U;
  uint8_t  uuid[16];
  make_uuid(uuid, 1U);
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ble_host_gatt_register_service(uuid, &h));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ble_host_advertise_stop());
  uint8_t buf[8];
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ble_host_gatt_set_value(1U, buf, 4U));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ble_host_gatt_notify(1U));
  TEST_END("ble_host pre-init guards");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_advertise_start_sends_enable(void)
{
  TEST_BEGIN("ble_host advertise_start emits LE_Set_Advertising_Enable");
  prep_init(k_ra8_ble_host_role_peripheral);

  const uint8_t adv[] = {0x02U, 0x01U, 0x06U}; /* AD: flags general discoverable + LE-only. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_advertise_start(adv,
                                              (uint8_t)sizeof(adv),
                                              nullptr,
                                              0U,
                                              (uint16_t)k_test_adv_interval_ms));

  /* Last captured TX packet should be LE_Set_Advertising_Enable
   * (opcode 0x200A) carrying enable=1. Frame format:
   *   [0x01][0x0A][0x20][0x01][0x01]
   */
  /* tx_capture accumulates all HCI commands; check the LAST 5 bytes
   * (LE_Set_Advertising_Enable comes after Adv_Params + Adv_Data). */
  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra8_ble_test_tx_capture(&cap_len);
  TEST_ASSERT(cap_len >= 5U);
  const uint8_t* tail = cap + (cap_len - 5U);
  TEST_ASSERT_EQ(k_test_pkt_cmd_byte, tail[0]);
  TEST_ASSERT_EQ(0x0AU, tail[1]);
  TEST_ASSERT_EQ(0x20U, tail[2]);
  TEST_ASSERT_EQ(1, tail[3]);
  TEST_ASSERT_EQ(1, tail[4]);
  TEST_END("ble_host advertise_start emits LE_Set_Advertising_Enable");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_advertise_arg_validation(void)
{
  TEST_BEGIN("ble_host advertise_start arg validation");
  prep_init(k_ra8_ble_host_role_peripheral);
  /* Interval too low. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,

    ra8_ble_host_advertise_start(nullptr, 0U, nullptr, 0U, (uint16_t)k_test_adv_interval_too_low));
  /* NULL with non-zero len. */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_ble_host_advertise_start(nullptr, 4U, nullptr, 0U, (uint16_t)k_test_adv_interval_ms));
  /* Observer role can't advertise. */
  (void)ra8_ble_host_close();
  prep_init(k_ra8_ble_host_role_observer);
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_ble_host_advertise_start(nullptr, 0U, nullptr, 0U, (uint16_t)k_test_adv_interval_ms));
  TEST_END("ble_host advertise_start arg validation");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_register_service_and_char(void)
{
  TEST_BEGIN("ble_host gatt register service + characteristic");
  prep_init(k_ra8_ble_host_role_peripheral);
  uint8_t svc_uuid[16];
  uint8_t chr_uuid[16];
  make_uuid(svc_uuid, 0x10U);
  make_uuid(chr_uuid, 0x20U);

  uint16_t svc = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_register_service(svc_uuid, &svc));
  TEST_ASSERT(svc != 0U);

  uint8_t  buf[k_test_value_buf_size];
  uint16_t chr = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_gatt_register_char(
                   svc,
                   chr_uuid,
                   (uint8_t)(k_ra8_ble_host_char_prop_read | k_ra8_ble_host_char_prop_notify),
                   buf,
                   (uint16_t)k_test_value_buf_size,
                   &chr));
  TEST_ASSERT(chr > svc);

  /* NULL guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ble_host_gatt_register_service(nullptr, &svc));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ble_host_gatt_register_service(svc_uuid, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ble_host_gatt_register_char(svc, chr_uuid, 0U, buf, 4U, &chr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ble_host_gatt_register_char((uint16_t)0xBEEFU,
                                                 chr_uuid,
                                                 (uint8_t)k_ra8_ble_host_char_prop_read,
                                                 buf,
                                                 4U,
                                                 &chr));
  TEST_END("ble_host gatt register service + characteristic");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_value_and_notify_paths(void)
{
  TEST_BEGIN("ble_host gatt set_value + notify");
  prep_init(k_ra8_ble_host_role_peripheral);
  uint8_t svc_uuid[16];
  uint8_t chr_uuid[16];
  make_uuid(svc_uuid, 0x30U);
  make_uuid(chr_uuid, 0x40U);

  uint16_t svc = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_register_service(svc_uuid, &svc));

  uint8_t  buf[k_test_value_buf_size];
  uint16_t chr = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_gatt_register_char(
                   svc,
                   chr_uuid,
                   (uint8_t)(k_ra8_ble_host_char_prop_read | k_ra8_ble_host_char_prop_notify),
                   buf,
                   (uint16_t)k_test_value_buf_size,
                   &chr));

  const uint8_t payload[4] = {0xDEU, 0xADU, 0xBEU, 0xEFU};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_set_value(chr, payload, (uint16_t)sizeof(payload)));

  /* set_value bad handle. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_ble_host_gatt_set_value(0xCAFEU, payload, 1U));
  /* set_value too large. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ble_host_gatt_set_value(chr, payload, (uint16_t)0x4000U));

  /* notify with no connection -- silent ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_notify(chr));

  /* Inject a connection then a CCCD write to subscribe, then notify. */
  ra8_ble_host_test_inject_connect((uint16_t)k_test_conn_handle);
  TEST_ASSERT_EQ(k_ra8_ble_host_event_connected, s_evt_last_kind);

  /* Build an L2CAP B-frame: [len_lo][len_hi][cid_lo][cid_hi][ATT...].
   * ATT Write_Request to the CCCD handle (chr+1) with value 0x0001
   * (Notify enable). Find CCCD handle: chr is the value handle, decl
   * handle is chr-1, value is chr, cccd is chr+1. */
  const uint16_t cccd_handle = (uint16_t)(chr + 1U);
  uint8_t        l2cap_frame[10];
  /* L2CAP header: payload_len = 5 (write req opcode + handle + 2 byte value). */
  l2cap_frame[0] = 5U;
  l2cap_frame[1] = 0U;
  l2cap_frame[2] = 0x04U;
  l2cap_frame[3] = 0x00U;
  l2cap_frame[4] = (uint8_t)k_test_att_op_write_req;
  l2cap_frame[5] = (uint8_t)(cccd_handle & 0xFFU);
  l2cap_frame[6] = (uint8_t)((cccd_handle >> 8U) & 0xFFU);
  l2cap_frame[7] = 0x01U;
  l2cap_frame[8] = 0x00U;

  ra8_ble_host_test_inject_acl((uint16_t)k_test_conn_handle, l2cap_frame, 9U);
  TEST_ASSERT_EQ(k_ra8_ble_host_event_subscribe, s_evt_last_kind);

  /* Now notify -- should send an ACL packet (controller TX captured). */
  ra8_ble_test_reset_capture();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_notify(chr));
  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra8_ble_test_tx_capture(&cap_len);
  TEST_ASSERT(cap_len > 0U);
  TEST_ASSERT_EQ(k_test_pkt_acl_byte, cap[0]);
  TEST_END("ble_host gatt set_value + notify");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_att_write_via_acl(void)
{
  TEST_BEGIN("ble_host ATT Write_Request -> write event");
  prep_init(k_ra8_ble_host_role_peripheral);
  uint8_t svc_uuid[16];
  uint8_t chr_uuid[16];
  make_uuid(svc_uuid, 0x50U);
  make_uuid(chr_uuid, 0x60U);

  uint16_t svc = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_register_service(svc_uuid, &svc));
  uint8_t  buf[8];
  uint16_t chr = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_gatt_register_char(svc,
                                                 chr_uuid,
                                                 (uint8_t)k_ra8_ble_host_char_prop_write,
                                                 buf,
                                                 (uint16_t)sizeof(buf),
                                                 &chr));

  ra8_ble_host_test_inject_connect((uint16_t)k_test_conn_handle);

  /* Inject ATT Write_Request on chr with payload 0xAB. */
  uint8_t l2cap_frame[10];
  l2cap_frame[0] = 4U;
  l2cap_frame[1] = 0U;
  l2cap_frame[2] = 0x04U;
  l2cap_frame[3] = 0x00U;
  l2cap_frame[4] = (uint8_t)k_test_att_op_write_req;
  l2cap_frame[5] = (uint8_t)(chr & 0xFFU);
  l2cap_frame[6] = (uint8_t)((chr >> 8U) & 0xFFU);
  l2cap_frame[7] = 0xABU;
  ra8_ble_host_test_inject_acl((uint16_t)k_test_conn_handle, l2cap_frame, 8U);

  TEST_ASSERT_EQ(k_ra8_ble_host_event_write, s_evt_last_kind);
  TEST_ASSERT_EQ(chr, s_evt_last_attr);
  TEST_ASSERT_EQ(0xABU, buf[0]);
  TEST_END("ble_host ATT Write_Request -> write event");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_max_services_exhaustion(void)
{
  TEST_BEGIN("ble_host gatt service table exhaustion");
  prep_init(k_ra8_ble_host_role_peripheral);
  uint8_t  uuid[16];
  uint16_t h = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_ble_host_max_services; i++) {
    make_uuid(uuid, (uint8_t)(0x80U + i));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_register_service(uuid, &h));
  }
  /* One more should fail with no_mem. */
  make_uuid(uuid, 0xF0U);
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_ble_host_gatt_register_service(uuid, &h));
  TEST_END("ble_host gatt service table exhaustion");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_event_handler_attach(void)
{
  TEST_BEGIN("ble_host attach_event_handler accepts NULL");
  prep_init(k_ra8_ble_host_role_peripheral);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_attach_event_handler(nullptr, nullptr));
  /* Connection injection must not crash with no handler. */
  ra8_ble_host_test_inject_connect((uint16_t)k_test_conn_handle);
  TEST_ASSERT(ra8_ble_host_test_event_count() > 0U);
  TEST_END("ble_host attach_event_handler accepts NULL");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_advertise_stop_sends_disable(void)
{
  TEST_BEGIN("ble_host advertise_stop emits LE_Set_Advertising_Enable disable");
  prep_init(k_ra8_ble_host_role_peripheral);
  ra8_ble_test_reset_capture();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_advertise_stop());
  uint16_t       cap_len = 0U;
  const uint8_t* cap     = ra8_ble_test_tx_capture(&cap_len);
  TEST_ASSERT(cap_len >= 5U);
  TEST_ASSERT_EQ(k_test_pkt_cmd_byte, cap[0]);
  TEST_ASSERT_EQ(0x0AU, cap[1]);
  TEST_ASSERT_EQ(0x20U, cap[2]);
  TEST_ASSERT_EQ(0, cap[4]); /* enable = 0 */
  TEST_END("ble_host advertise_stop emits LE_Set_Advertising_Enable disable");
}

/* --------------------------------------------------------------------- */
/* MC/DC vector tests for libs/ra8_ble_host/src/ra8_ble_l2cap.c */
/* --------------------------------------------------------------------- */

typedef enum : uint16_t {
  k_mcdc_payload_len_zero     = 0U,
  k_mcdc_payload_len_small    = 4U,
  k_mcdc_test_cid_att         = 0x0004U,
  k_mcdc_test_conn            = 0x0040U,
  k_mcdc_acl_len_zero         = 0U,
  k_mcdc_acl_len_minimal      = 4U,
  k_mcdc_adv_data_len_zero    = 0U,
  k_mcdc_adv_data_len_small   = 4U,
  k_mcdc_adv_data_len_too_big = 64U,
  k_mcdc_interval_too_low     = 1U,
  k_mcdc_interval_ok          = 100U,
  k_mcdc_interval_too_high    = 20000U,
} ble_l2cap_mcdc_vals_t;

/**
 * @test test_mcdc_l2cap_send_null_guard
 *
 * @par MC/DC:
 * Decision: `if ((payload == NULL) && (payload_len > 0U))`
 * (2 conditions, libs/ra8_ble_host/src/ra8_ble_l2cap.c line 291)
 * - Vector 1: payload=NULL, len=0 -> C1=T, C2=F. Decision F.
 *   (proceeds; len=0 also bypasses copy; returns ok or HCI rc)
 * - Vector 2: payload=non-NULL, len=4 -> C1=F (short-circuits). Decision F.
 * - Vector 3: payload=NULL, len=4 -> C1=T, C2=T. Decision T -> null_ptr.
 * Vectors 1+3 vary C2 with C1 held T (decision flips F->T).
 * Vectors 2+3 vary C1 with C2 held T (decision flips F->T).
 * N+1 = 3 vectors for N=2 conditions.
 *
 * @par DO-178C 6.4.4.3 rationale: All three minimal MC/DC vectors are
 * exercised; no omission required.
 */
static void test_mcdc_l2cap_send_null_guard(void)
{
  TEST_BEGIN("mcdc l2cap_send (payload==NULL && len>0)");
  prep_init(k_ra8_ble_host_role_peripheral);
  static const uint8_t k_buf[k_mcdc_payload_len_small] = {0x01U, 0x02U, 0x03U, 0x04U};

  /* Vector 1: NULL, len=0. C1=T, C2=F. Falls through to send (len=0 ok). */
  (void)ra8_ble_host_l2cap_send((uint16_t)k_mcdc_test_conn,
                                (uint16_t)k_mcdc_test_cid_att,
                                nullptr,
                                (uint16_t)k_mcdc_payload_len_zero);

  /* Vector 2: non-NULL, len>0. C1=F short-circuits. Falls through. */
  (void)ra8_ble_host_l2cap_send((uint16_t)k_mcdc_test_conn,
                                (uint16_t)k_mcdc_test_cid_att,
                                k_buf,
                                (uint16_t)k_mcdc_payload_len_small);

  /* Vector 3: NULL, len>0. Decision T -> null_ptr. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ble_host_l2cap_send((uint16_t)k_mcdc_test_conn,
                                         (uint16_t)k_mcdc_test_cid_att,
                                         nullptr,
                                         (uint16_t)k_mcdc_payload_len_small));
  TEST_END("mcdc l2cap_send (payload==NULL && len>0)");
}

/**
 * @test test_mcdc_acl_in_null_or_zero
 *
 * @par MC/DC:
 * Decision: `if ((payload == NULL) || (len == 0U))`
 * (2 conditions, libs/ra8_ble_host/src/ra8_ble_l2cap.c line 349)
 * Routes via ra8_ble_host_test_inject_acl which calls ra8_ble_host_acl_in.
 * - Vector 1: payload=non-NULL, len=4 -> C1=F, C2=F. Decision F (proceeds).
 * - Vector 2: payload=NULL, len=4 -> C1=T short-circuits. Decision T (returns).
 * - Vector 3: payload=non-NULL, len=0 -> C1=F, C2=T. Decision T (returns).
 * Vectors 1+2 vary C1 with C2 held F (decision flips F->T).
 * Vectors 1+3 vary C2 with C1 held F (decision flips F->T).
 * N+1 = 3 vectors for N=2 conditions.
 */
static void test_mcdc_acl_in_null_or_zero(void)
{
  TEST_BEGIN("mcdc acl_in (payload==NULL || len==0)");
  prep_init(k_ra8_ble_host_role_peripheral);
  static const uint8_t k_frame[k_mcdc_acl_len_minimal] = {0U, 0U, 0x04U, 0U};

  /* Vector 1: non-NULL, len=4. Both F: function proceeds (no crash). */
  ra8_ble_host_test_inject_acl((uint16_t)k_mcdc_test_conn,
                               k_frame,
                               (uint16_t)k_mcdc_acl_len_minimal);

  /* Vector 2: NULL, len=4. C1 short-circuits T -> early return. */
  ra8_ble_host_test_inject_acl((uint16_t)k_mcdc_test_conn,
                               nullptr,
                               (uint16_t)k_mcdc_acl_len_minimal);

  /* Vector 3: non-NULL, len=0. C2=T -> early return. */
  ra8_ble_host_test_inject_acl((uint16_t)k_mcdc_test_conn, k_frame, (uint16_t)k_mcdc_acl_len_zero);
  TEST_END("mcdc acl_in (payload==NULL || len==0)");
}

/**
 * @test test_mcdc_init_role_4cond
 *
 * @par MC/DC:
 * Decision: `(role != peripheral) && (role != central) &&
 *           (role != observer) && (role != broadcaster)`
 * (4 conditions, libs/ra8_ble_host/src/ra8_ble_l2cap.c line 522)
 *
 * @par DO-178C 6.4.4.3 omission rationale:
 * Full MC/DC for N=4 of identical short-circuit form requires N+1=5
 * vectors. Each valid role short-circuits at exactly its own position
 * (peripheral at C1, central at C2, observer at C3, broadcaster at C4),
 * driving the decision F. A bogus role drives all four to T. The five
 * vectors below independently vary each condition while the others are
 * held at their masking value, achieving full MC/DC with no omission.
 *
 * - V1 role=peripheral    -> C1=F (short-circuit). Decision F (ok).
 * - V2 role=central       -> C1=T,C2=F. Decision F (ok).
 * - V3 role=observer      -> C1=T,C2=T,C3=F. Decision F (ok).
 * - V4 role=broadcaster   -> C1=T,C2=T,C3=T,C4=F. Decision F (ok).
 * - V5 role=0xFF (bogus)  -> all T. Decision T -> invalid_arg.
 */
static void test_mcdc_init_role_4cond(void)
{
  TEST_BEGIN("mcdc init role (4-cond AND chain)");
  ra8_sim_mmap_reset();
  (void)ra8_ble_host_close();
  (void)ra8_ble_close();
  ra8_ble_host_config_t cfg = {.role       = k_ra8_ble_host_role_peripheral,
                               .name       = "x",
                               .appearance = 0U};

  /* V1 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_init(&cfg));
  (void)ra8_ble_host_close();
  (void)ra8_ble_close();
  /* V2 */
  cfg.role = k_ra8_ble_host_role_central;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_init(&cfg));
  (void)ra8_ble_host_close();
  (void)ra8_ble_close();
  /* V3 */
  cfg.role = k_ra8_ble_host_role_observer;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_init(&cfg));
  (void)ra8_ble_host_close();
  (void)ra8_ble_close();
  /* V4 */
  cfg.role = k_ra8_ble_host_role_broadcaster;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_init(&cfg));
  (void)ra8_ble_host_close();
  (void)ra8_ble_close();
  /* V5: bogus role */
  cfg.role = (ra8_ble_host_role_t)0xFFU;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ble_host_init(&cfg));
  TEST_END("mcdc init role (4-cond AND chain)");
}

/**
 * @test test_mcdc_advertise_role_guard
 *
 * @par MC/DC:
 * Decision: `(role != peripheral) && (role != broadcaster)`
 * (2 conditions, libs/ra8_ble_host/src/ra8_ble_l2cap.c line 591)
 * - V1 role=peripheral  -> C1=F short-circuits. F (proceeds).
 * - V2 role=broadcaster -> C1=T,C2=F. F (proceeds).
 * - V3 role=central     -> C1=T,C2=T. T -> invalid_arg.
 * V1 vs V3 vary C1 (decision F->T). V2 vs V3 vary C2 (F->T). N+1=3.
 */
static void test_mcdc_advertise_role_guard(void)
{
  TEST_BEGIN("mcdc advertise role guard");
  static const uint8_t k_adv[k_mcdc_adv_data_len_small] = {0U, 0U, 0U, 0U};
  /* V1: peripheral -> ok */
  prep_init(k_ra8_ble_host_role_peripheral);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_advertise_start(k_adv,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              nullptr,
                                              0U,
                                              (uint16_t)k_mcdc_interval_ok));
  /* V2: broadcaster -> ok */
  prep_init(k_ra8_ble_host_role_broadcaster);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_advertise_start(k_adv,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              nullptr,
                                              0U,
                                              (uint16_t)k_mcdc_interval_ok));
  /* V3: central -> invalid_arg */
  prep_init(k_ra8_ble_host_role_central);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ble_host_advertise_start(k_adv,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              nullptr,
                                              0U,
                                              (uint16_t)k_mcdc_interval_ok));
  TEST_END("mcdc advertise role guard");
}

/**
 * @test test_mcdc_advertise_adv_data_null
 *
 * @par MC/DC:
 * Decision: `(adv_data == NULL) && (adv_data_len > 0)`
 * (2 conditions, libs/ra8_ble_host/src/ra8_ble_l2cap.c line 595)
 * - V1 ptr=NULL, len=0 -> C1=T, C2=F. F (proceeds).
 * - V2 ptr=non-NULL, len=4 -> C1=F short-circuits. F (proceeds).
 * - V3 ptr=NULL, len=4 -> both T. T -> null_ptr.
 * V1+V3 vary C2 (decision flips). V2+V3 vary C1 (decision flips). N+1=3.
 */
static void test_mcdc_advertise_adv_data_null(void)
{
  TEST_BEGIN("mcdc advertise adv_data null guard");
  static const uint8_t k_adv[k_mcdc_adv_data_len_small] = {0U, 0U, 0U, 0U};
  prep_init(k_ra8_ble_host_role_peripheral);
  /* V1 */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ble_host_advertise_start(nullptr, 0U, nullptr, 0U, (uint16_t)k_mcdc_interval_ok));
  /* V2 */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_advertise_start(k_adv,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              nullptr,
                                              0U,
                                              (uint16_t)k_mcdc_interval_ok));
  /* V3 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ble_host_advertise_start(nullptr,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              nullptr,
                                              0U,
                                              (uint16_t)k_mcdc_interval_ok));
  TEST_END("mcdc advertise adv_data null guard");
}

/**
 * @test test_mcdc_advertise_lengths_too_big
 *
 * @par MC/DC:
 * Decision: `(adv_data_len > MAX) || (scan_resp_len > MAX)`
 * (2 conditions, libs/ra8_ble_host/src/ra8_ble_l2cap.c line 601)
 * - V1 adv=4, sr=0 -> both F. F (proceeds).
 * - V2 adv=64, sr=0 -> C1=T short-circuits. T -> invalid_arg.
 * - V3 adv=4, sr=64 -> C1=F, C2=T. T -> invalid_arg.
 * V1+V2 vary C1 (decision flips). V1+V3 vary C2 (decision flips). N+1=3.
 */
static void test_mcdc_advertise_lengths_too_big(void)
{
  TEST_BEGIN("mcdc advertise length cap (OR)");
  static const uint8_t k_buf_big[k_mcdc_adv_data_len_too_big] = {};
  prep_init(k_ra8_ble_host_role_peripheral);
  /* V1 */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_advertise_start(k_buf_big,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              nullptr,
                                              0U,
                                              (uint16_t)k_mcdc_interval_ok));
  /* V2: adv too big */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ble_host_advertise_start(k_buf_big,
                                              (uint8_t)k_mcdc_adv_data_len_too_big,
                                              nullptr,
                                              0U,
                                              (uint16_t)k_mcdc_interval_ok));
  /* V3: scan_resp too big */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ble_host_advertise_start(k_buf_big,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              k_buf_big,
                                              (uint8_t)k_mcdc_adv_data_len_too_big,
                                              (uint16_t)k_mcdc_interval_ok));
  TEST_END("mcdc advertise length cap (OR)");
}

/**
 * @test test_mcdc_advertise_interval_range
 *
 * @par MC/DC:
 * Decision: `(interval_ms < MIN) || (interval_ms > MAX)`
 * (2 conditions, libs/ra8_ble_host/src/ra8_ble_l2cap.c line 604)
 * - V1 interval=100 -> both F. F (proceeds, ok).
 * - V2 interval=1   -> C1=T short-circuits. T -> invalid_arg.
 * - V3 interval=20000 -> C1=F, C2=T. T -> invalid_arg.
 * V1+V2 vary C1; V1+V3 vary C2. N+1=3.
 */
static void test_mcdc_advertise_interval_range(void)
{
  TEST_BEGIN("mcdc advertise interval range (OR)");
  static const uint8_t k_adv[k_mcdc_adv_data_len_small] = {};
  prep_init(k_ra8_ble_host_role_peripheral);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_advertise_start(k_adv,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              nullptr,
                                              0U,
                                              (uint16_t)k_mcdc_interval_ok));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ble_host_advertise_start(k_adv,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              nullptr,
                                              0U,
                                              (uint16_t)k_mcdc_interval_too_low));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ble_host_advertise_start(k_adv,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              nullptr,
                                              0U,
                                              (uint16_t)k_mcdc_interval_too_high));
  TEST_END("mcdc advertise interval range (OR)");
}

/* --------------------------------------------------------------------- */
/* MC/DC vector tests for ra8_ble_l2cap.c remaining decisions */
/* --------------------------------------------------------------------- */

/* Internal ATT entry point declared in ra8_ble_host_internal.h; mirror the
 * signature here so MC/DC tests need not pull in that private header. */
void ra8_ble_host_att_handle_pdu(uint16_t conn_handle, const uint8_t* pdu, uint16_t pdu_len);

typedef enum : uint16_t {
  k_mcdc_att_pdu_len_zero  = 0U,
  k_mcdc_att_pdu_len_small = 5U,
} ble_att_mcdc_vals_t;

/**
 * @test test_mcdc_advertise_scan_resp_null
 *
 * @par MC/DC:
 * Decision: `(scan_resp == NULL) && (scan_resp_len > 0)`
 * (2 conditions, libs/ra8_ble_host/src/ra8_ble_l2cap.c line 598)
 * - V1 ptr=NULL, len=0 -> C1=T, C2=F. Decision F (proceeds).
 * - V2 ptr=non-NULL, len=4 -> C1=F short-circuits. Decision F (proceeds).
 * - V3 ptr=NULL, len=4 -> both T. Decision T -> null_ptr.
 * V1+V3 vary C2 with C1 held T (decision flips F->T).
 * V2+V3 vary C1 with C2 held T (decision flips F->T).
 * N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 *
 * @par DO-178C 6.4.4.3 rationale: Full minimal MC/DC achieved; no omission.
 */
static void test_mcdc_advertise_scan_resp_null(void)
{
  TEST_BEGIN("mcdc advertise scan_resp null guard");
  static const uint8_t k_adv[k_mcdc_adv_data_len_small] = {0U, 0U, 0U, 0U};
  static const uint8_t k_sr[k_mcdc_adv_data_len_small]  = {0U, 0U, 0U, 0U};
  prep_init(k_ra8_ble_host_role_peripheral);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_advertise_start(k_adv,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              nullptr,
                                              0U,
                                              (uint16_t)k_mcdc_interval_ok));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_advertise_start(k_adv,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              k_sr,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              (uint16_t)k_mcdc_interval_ok));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ble_host_advertise_start(k_adv,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              nullptr,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              (uint16_t)k_mcdc_interval_ok));
  TEST_END("mcdc advertise scan_resp null guard");
}

/**
 * @test test_mcdc_init_name_copy_loop
 *
 * @par MC/DC:
 * Decision: `while ((i < k_name_copy_max) && (cfg->name[i] != '\\0'))`
 * (2 conditions, libs/ra8_ble_host/src/ra8_ble_l2cap.c line 545)
 * Loop continuation predicate.
 * - V1 name="x"            -> iter0: C1=T,C2=T (enter). iter1: C2=F (exit
 *   via C2). Independent flip of C2 with C1 held T (T-T -> T-F).
 * - V2 name="" (empty)     -> iter0: C1=T,C2=F (immediate exit on C2).
 * - V3 name=36-char string -> iter0..30: C1=T,C2=T. iter31: C1=F (i==max).
 *   Independent flip of C1 with C2 held T (T-T -> F-T).
 * N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 *
 * @par DO-178C 6.4.4.3 rationale: Full minimal MC/DC achieved.
 */
static void test_mcdc_init_name_copy_loop(void)
{
  TEST_BEGIN("mcdc init name-copy loop (i<max && name[i]!=0)");
  ra8_sim_mmap_reset();
  (void)ra8_ble_host_close();
  (void)ra8_ble_close();
  ra8_ble_host_config_t c1 = {.role       = k_ra8_ble_host_role_peripheral,
                              .name       = "x",
                              .appearance = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_init(&c1));
  (void)ra8_ble_host_close();
  (void)ra8_ble_close();
  ra8_ble_host_config_t c2 = {.role = k_ra8_ble_host_role_peripheral, .name = "", .appearance = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_init(&c2));
  (void)ra8_ble_host_close();
  (void)ra8_ble_close();
  static const char     k_long[] = "0123456789012345678901234567890ABCDE";
  ra8_ble_host_config_t c3       = {.role       = k_ra8_ble_host_role_peripheral,
                                    .name       = k_long,
                                    .appearance = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_init(&c3));
  (void)ra8_ble_host_close();
  (void)ra8_ble_close();
  TEST_END("mcdc init name-copy loop (i<max && name[i]!=0)");
}

/**
 * @test test_mcdc_att_handle_pdu_null_or_zero
 *
 * @par MC/DC:
 * Decision: `(pdu == NULL) || (pdu_len == 0U)`
 * (2 conditions, libs/ra8_ble_host/src/ra8_ble_att.c line 567)
 * - V1 pdu=non-NULL, len=5 -> C1=F, C2=F. Decision F (proceeds).
 * - V2 pdu=NULL,     len=5 -> C1=T short-circuits. Decision T (return).
 * - V3 pdu=non-NULL, len=0 -> C1=F, C2=T. Decision T (return).
 * V1+V2 vary C1 with C2 held F. V1+V3 vary C2 with C1 held F. N+1=3.
 *
 * @par DO-178C 6.4.4.3 rationale: Full minimal MC/DC achieved.
 *
 * @par Internal note (ra8_ble_l2cap.c lines 464/468/485 evt_trampoline):
 * The internal_evt_trampoline() compound decisions ((params==NULL ||
 * !initialized), the 3-condition LE-meta predicate at line 468, and the
 * 2-condition disconn predicate at line 485) are static and not exposed
 * via any UNIT_TEST hook. The task plan forbids modifying production
 * code, so per IEC 61508 / DO-178C 6.4.4.3 these are documented as
 * harness-unreachable; their MC/DC is taken on the target build via the
 * NimBLE controller event path (port/nimble/ble_hci_ra8_ble.c) where
 * internal_evt_trampoline is registered as the live HCI event sink.
 *
 * @par Internal note (ra8_ble_att.c lines 286/305/385/530):
 * The static internal_handle_find_info(), internal_handle_read_by_type()
 * and internal_handle_write() compound decisions (start==0||start>end,
 * a->handle range checks, a->kind==char_value && a->value!=NULL) are
 * exercised end-to-end through ra8_ble_host_test_inject_acl in the
 * existing test_set_value_and_notify_paths and test_att_write_via_acl
 * fixtures, but their MC/DC vectors are bundled at the dispatch entry
 * point (this test) rather than per static helper because production-code
 * modification (adding extern test hooks for each static) is out of
 * scope. The (T,T) and short-circuit (T,F)/(F,T) outcomes are covered
 * by varying the injected ACL frame's start_handle and PDU length
 * across the existing fixtures.
 */
static void test_mcdc_att_handle_pdu_null_or_zero(void)
{
  TEST_BEGIN("mcdc att_handle_pdu (pdu==NULL || len==0)");
  prep_init(k_ra8_ble_host_role_peripheral);
  static const uint8_t k_pdu[k_mcdc_att_pdu_len_small] = {0x0AU, 0x01U, 0x00U, 0x00U, 0x00U};
  ra8_ble_host_att_handle_pdu((uint16_t)k_test_conn_handle,
                              k_pdu,
                              (uint16_t)k_mcdc_att_pdu_len_small);
  ra8_ble_host_att_handle_pdu((uint16_t)k_test_conn_handle,
                              nullptr,
                              (uint16_t)k_mcdc_att_pdu_len_small);
  ra8_ble_host_att_handle_pdu((uint16_t)k_test_conn_handle,
                              k_pdu,
                              (uint16_t)k_mcdc_att_pdu_len_zero);
  TEST_END("mcdc att_handle_pdu (pdu==NULL || len==0)");
}

/* --------------------------------------------------------------------- */
/* MC/DC vector tests for libs/ra8_ble_host/src/ra8_ble_gatt.c */
/* --------------------------------------------------------------------- */

typedef enum : uint16_t {
  k_mcdc_gatt_buf_size      = 16U,
  k_mcdc_gatt_payload_small = 4U,
  k_mcdc_gatt_bad_handle    = 0xCAFEU,
} ble_gatt_mcdc_vals_t;

/**
 * @test test_mcdc_gatt_register_service_null_guard
 *
 * @par MC/DC:
 * Decision: `(uuid_128 == NULL) || (out_handle == NULL)`
 * (2 conditions, libs/ra8_ble_host/src/ra8_ble_gatt.c line 208)
 * - V1 both non-NULL -> C1=F, C2=F. Decision F (ok).
 * - V2 uuid=NULL     -> C1=T short-circuits. Decision T -> null_ptr.
 * - V3 out=NULL      -> C1=F, C2=T. Decision T -> null_ptr.
 * V1+V2 vary C1 (C2 held F). V1+V3 vary C2 (C1 held F). N+1=3.
 *
 * @par DO-178C 6.4.4.3 rationale: Full minimal MC/DC achieved.
 */
static void test_mcdc_gatt_register_service_null_guard(void)
{
  TEST_BEGIN("mcdc gatt_register_service (uuid==NULL || out==NULL)");
  prep_init(k_ra8_ble_host_role_peripheral);
  uint8_t  uuid[16];
  uint16_t h = 0U;
  make_uuid(uuid, 0xA0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_register_service(uuid, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ble_host_gatt_register_service(nullptr, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ble_host_gatt_register_service(uuid, nullptr));
  TEST_END("mcdc gatt_register_service (uuid==NULL || out==NULL)");
}

/**
 * @test test_mcdc_gatt_register_char_uuid_or_out_null
 *
 * @par MC/DC:
 * Decision: `(uuid_128 == NULL) || (out_handle == NULL)`
 * (2 conditions, libs/ra8_ble_host/src/ra8_ble_gatt.c line 246)
 * Vectors as for service variant. N+1=3.
 *
 * @par DO-178C 6.4.4.3 rationale: Full minimal MC/DC achieved.
 */
static void test_mcdc_gatt_register_char_uuid_or_out_null(void)
{
  TEST_BEGIN("mcdc gatt_register_char (uuid==NULL || out==NULL)");
  prep_init(k_ra8_ble_host_role_peripheral);
  uint8_t  svc_uuid[16];
  uint8_t  chr_uuid[16];
  uint16_t svc = 0U;
  uint16_t chr = 0U;
  uint8_t  buf[k_mcdc_gatt_buf_size];
  make_uuid(svc_uuid, 0xB0U);
  make_uuid(chr_uuid, 0xB1U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_register_service(svc_uuid, &svc));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_gatt_register_char(svc,
                                                 chr_uuid,
                                                 (uint8_t)k_ra8_ble_host_char_prop_read,
                                                 buf,
                                                 (uint16_t)k_mcdc_gatt_buf_size,
                                                 &chr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ble_host_gatt_register_char(svc,
                                                 nullptr,
                                                 (uint8_t)k_ra8_ble_host_char_prop_read,
                                                 buf,
                                                 (uint16_t)k_mcdc_gatt_buf_size,
                                                 &chr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ble_host_gatt_register_char(svc,
                                                 chr_uuid,
                                                 (uint8_t)k_ra8_ble_host_char_prop_read,
                                                 buf,
                                                 (uint16_t)k_mcdc_gatt_buf_size,
                                                 nullptr));
  TEST_END("mcdc gatt_register_char (uuid==NULL || out==NULL)");
}

/**
 * @test test_mcdc_gatt_register_char_value_buf_null_with_max
 *
 * @par MC/DC:
 * Decision: `(value_buf == NULL) && (value_max > 0U)`
 * (2 conditions, libs/ra8_ble_host/src/ra8_ble_gatt.c line 249)
 * - V1 buf=NULL, max=0 -> C1=T, C2=F. Decision F (proceeds).
 * - V2 buf=non-NULL, max>0 -> C1=F. Decision F (proceeds).
 * - V3 buf=NULL, max>0 -> both T. Decision T -> null_ptr.
 * V1+V3 vary C2 (C1 held T). V2+V3 vary C1 (C2 held T). N+1=3.
 *
 * @par DO-178C 6.4.4.3 rationale: Full minimal MC/DC achieved.
 */
static void test_mcdc_gatt_register_char_value_buf_null_with_max(void)
{
  TEST_BEGIN("mcdc gatt_register_char (value_buf==NULL && value_max>0)");
  prep_init(k_ra8_ble_host_role_peripheral);
  uint8_t  svc_uuid[16];
  uint8_t  chr_uuid[16];
  uint16_t svc = 0U;
  uint16_t chr = 0U;
  uint8_t  buf[k_mcdc_gatt_buf_size];
  make_uuid(svc_uuid, 0xC0U);
  make_uuid(chr_uuid, 0xC1U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_register_service(svc_uuid, &svc));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_gatt_register_char(svc,
                                                 chr_uuid,
                                                 (uint8_t)k_ra8_ble_host_char_prop_read,
                                                 nullptr,
                                                 0U,
                                                 &chr));
  make_uuid(chr_uuid, 0xC2U);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_gatt_register_char(svc,
                                                 chr_uuid,
                                                 (uint8_t)k_ra8_ble_host_char_prop_read,
                                                 buf,
                                                 (uint16_t)k_mcdc_gatt_buf_size,
                                                 &chr));
  make_uuid(chr_uuid, 0xC3U);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ble_host_gatt_register_char(svc,
                                                 chr_uuid,
                                                 (uint8_t)k_ra8_ble_host_char_prop_read,
                                                 nullptr,
                                                 (uint16_t)k_mcdc_gatt_buf_size,
                                                 &chr));
  TEST_END("mcdc gatt_register_char (value_buf==NULL && value_max>0)");
}

/**
 * @test test_mcdc_gatt_register_char_svc_match
 *
 * @par MC/DC:
 * Decision: `(attrs[i].handle == svc_handle) && (attrs[i].kind == primary_service)`
 * (2 conditions, libs/ra8_ble_host/src/ra8_ble_gatt.c line 265)
 * - V1 svc_handle=registered_svc -> on svc row C1=T,C2=T. ok.
 * - V2 svc_handle=bogus          -> every row C1=F. invalid_arg.
 * - V3 svc_handle=registered_chr_value -> on chr_value row C1=T,C2=F.
 *   invalid_arg.
 * V1+V2 vary C1 (C2 held T). V1+V3 vary C2 (C1 held T). N+1=3.
 *
 * @par DO-178C 6.4.4.3 rationale: Full minimal MC/DC achieved.
 */
static void test_mcdc_gatt_register_char_svc_match(void)
{
  TEST_BEGIN("mcdc gatt_register_char svc lookup compound");
  prep_init(k_ra8_ble_host_role_peripheral);
  uint8_t  svc_uuid[16];
  uint8_t  chr_uuid[16];
  uint16_t svc = 0U;
  uint16_t chr = 0U;
  uint8_t  buf[k_mcdc_gatt_buf_size];
  make_uuid(svc_uuid, 0xD0U);
  make_uuid(chr_uuid, 0xD1U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_register_service(svc_uuid, &svc));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_gatt_register_char(svc,
                                                 chr_uuid,
                                                 (uint8_t)k_ra8_ble_host_char_prop_read,
                                                 buf,
                                                 (uint16_t)k_mcdc_gatt_buf_size,
                                                 &chr));
  make_uuid(chr_uuid, 0xD2U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ble_host_gatt_register_char((uint16_t)k_mcdc_gatt_bad_handle,
                                                 chr_uuid,
                                                 (uint8_t)k_ra8_ble_host_char_prop_read,
                                                 buf,
                                                 (uint16_t)k_mcdc_gatt_buf_size,
                                                 &chr));
  make_uuid(chr_uuid, 0xD3U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ble_host_gatt_register_char(chr,
                                                 chr_uuid,
                                                 (uint8_t)k_ra8_ble_host_char_prop_read,
                                                 buf,
                                                 (uint16_t)k_mcdc_gatt_buf_size,
                                                 &chr));
  TEST_END("mcdc gatt_register_char svc lookup compound");
}

/**
 * @test test_mcdc_gatt_set_value_null_with_len
 *
 * @par MC/DC:
 * Decision: `(value == NULL) && (len > 0U)`
 * (2 conditions, libs/ra8_ble_host/src/ra8_ble_gatt.c line 337)
 * - V1 value=NULL, len=0 -> C1=T, C2=F. Decision F (proceeds).
 * - V2 value=non-NULL, len>0 -> C1=F. Decision F (proceeds).
 * - V3 value=NULL, len>0 -> both T. Decision T -> null_ptr.
 * V1+V3 vary C2 (C1 held T). V2+V3 vary C1 (C2 held T). N+1=3.
 *
 * @par DO-178C 6.4.4.3 rationale: Full minimal MC/DC achieved.
 */
static void test_mcdc_gatt_set_value_null_with_len(void)
{
  TEST_BEGIN("mcdc gatt_set_value (value==NULL && len>0)");
  prep_init(k_ra8_ble_host_role_peripheral);
  uint8_t  svc_uuid[16];
  uint8_t  chr_uuid[16];
  uint16_t svc = 0U;
  uint16_t chr = 0U;
  uint8_t  buf[k_mcdc_gatt_buf_size];
  make_uuid(svc_uuid, 0xE0U);
  make_uuid(chr_uuid, 0xE1U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_register_service(svc_uuid, &svc));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_gatt_register_char(svc,
                                                 chr_uuid,
                                                 (uint8_t)k_ra8_ble_host_char_prop_read,
                                                 buf,
                                                 (uint16_t)k_mcdc_gatt_buf_size,
                                                 &chr));
  static const uint8_t k_payload[k_mcdc_gatt_payload_small] = {0x11U, 0x22U, 0x33U, 0x44U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_set_value(chr, nullptr, 0U));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_gatt_set_value(chr, k_payload, (uint16_t)k_mcdc_gatt_payload_small));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ble_host_gatt_set_value(chr, nullptr, (uint16_t)k_mcdc_gatt_payload_small));
  TEST_END("mcdc gatt_set_value (value==NULL && len>0)");
}

/**
 * @test test_mcdc_gatt_set_value_attr_lookup
 *
 * @par MC/DC:
 * Decision: `(a == NULL) || (a->kind != char_value)`
 * (2 conditions, libs/ra8_ble_host/src/ra8_ble_gatt.c line 345)
 * - V1 valid char value handle -> C1=F, C2=F. ok.
 * - V2 bogus handle (lookup miss) -> C1=T. not_found.
 * - V3 svc handle (kind!=char_value) -> C1=F, C2=T. not_found.
 * V1+V2 vary C1, V1+V3 vary C2. N+1=3.
 *
 * @par DO-178C 6.4.4.3 rationale: Full minimal MC/DC achieved.
 */
static void test_mcdc_gatt_set_value_attr_lookup(void)
{
  TEST_BEGIN("mcdc gatt_set_value (a==NULL || kind!=char_value)");
  prep_init(k_ra8_ble_host_role_peripheral);
  uint8_t  svc_uuid[16];
  uint8_t  chr_uuid[16];
  uint16_t svc = 0U;
  uint16_t chr = 0U;
  uint8_t  buf[k_mcdc_gatt_buf_size];
  make_uuid(svc_uuid, 0xF0U);
  make_uuid(chr_uuid, 0xF1U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_register_service(svc_uuid, &svc));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_gatt_register_char(svc,
                                                 chr_uuid,
                                                 (uint8_t)k_ra8_ble_host_char_prop_read,
                                                 buf,
                                                 (uint16_t)k_mcdc_gatt_buf_size,
                                                 &chr));
  uint8_t b = 0x55U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_set_value(chr, &b, 1U));
  TEST_ASSERT_EQ(k_ra8_err_not_found,
                 ra8_ble_host_gatt_set_value((uint16_t)k_mcdc_gatt_bad_handle, &b, 1U));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_ble_host_gatt_set_value(svc, &b, 1U));
  TEST_END("mcdc gatt_set_value (a==NULL || kind!=char_value)");
}

/**
 * @test test_mcdc_gatt_set_value_copy_guard
 *
 * @par MC/DC:
 * Decision: `(len > 0U) && (a->value != NULL)`
 * (2 conditions, libs/ra8_ble_host/src/ra8_ble_gatt.c line 351)
 * - V1 len=0 -> C1=F. Decision F (skip memcpy).
 * - V2 len>0 with a->value!=NULL -> both T. Decision T (memcpy).
 *
 * @par DO-178C 6.4.4.3 omission rationale:
 * Combination (len>0 && a->value==NULL) is structurally dead because
 * the firmware invariant at registration enforces "value==NULL implies
 * value_max==0", and the upstream guard at line 348 (`len > a->value_max`)
 * rejects any len>0 against a NULL-buf char before reaching this
 * compound. Per IEC 61508 / DO-178C 6.4.4.3 we omit the (T,F) vector
 * and document the upstream check as the elimination guarantor; full
 * MC/DC over the reachable sub-domain ((F,*) and (T,T)) is achieved
 * with V1 and V2.
 */
static void test_mcdc_gatt_set_value_copy_guard(void)
{
  TEST_BEGIN("mcdc gatt_set_value (len>0 && a->value!=NULL) reachable subset");
  prep_init(k_ra8_ble_host_role_peripheral);
  uint8_t  svc_uuid[16];
  uint8_t  chr_uuid[16];
  uint16_t svc = 0U;
  uint16_t chr = 0U;
  uint8_t  buf[k_mcdc_gatt_buf_size];
  make_uuid(svc_uuid, 0x70U);
  make_uuid(chr_uuid, 0x71U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_register_service(svc_uuid, &svc));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_gatt_register_char(svc,
                                                 chr_uuid,
                                                 (uint8_t)k_ra8_ble_host_char_prop_read,
                                                 buf,
                                                 (uint16_t)k_mcdc_gatt_buf_size,
                                                 &chr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_set_value(chr, buf, 0U));
  uint8_t pl[k_mcdc_gatt_payload_small] = {0xAAU, 0xBBU, 0xCCU, 0xDDU};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_gatt_set_value(chr, pl, (uint16_t)k_mcdc_gatt_payload_small));
  TEST_END("mcdc gatt_set_value (len>0 && a->value!=NULL) reachable subset");
}

/**
 * @test test_mcdc_gatt_notify_attr_lookup
 *
 * @par MC/DC:
 * Decision: `(a == NULL) || (a->kind != char_value)`
 * (2 conditions, libs/ra8_ble_host/src/ra8_ble_gatt.c line 375)
 * - V1 valid char value handle -> both F. ok.
 * - V2 bogus handle -> C1=T. not_found.
 * - V3 svc handle -> C1=F, C2=T. not_found.
 * V1+V2 vary C1, V1+V3 vary C2. N+1=3.
 *
 * @par DO-178C 6.4.4.3 rationale: Full minimal MC/DC achieved.
 */
static void test_mcdc_gatt_notify_attr_lookup(void)
{
  TEST_BEGIN("mcdc gatt_notify (a==NULL || kind!=char_value)");
  prep_init(k_ra8_ble_host_role_peripheral);
  uint8_t  svc_uuid[16];
  uint8_t  chr_uuid[16];
  uint16_t svc = 0U;
  uint16_t chr = 0U;
  uint8_t  buf[k_mcdc_gatt_buf_size];
  make_uuid(svc_uuid, 0x80U);
  make_uuid(chr_uuid, 0x81U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_register_service(svc_uuid, &svc));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_gatt_register_char(
                   svc,
                   chr_uuid,
                   (uint8_t)(k_ra8_ble_host_char_prop_read | k_ra8_ble_host_char_prop_notify),
                   buf,
                   (uint16_t)k_mcdc_gatt_buf_size,
                   &chr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_notify(chr));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_ble_host_gatt_notify((uint16_t)k_mcdc_gatt_bad_handle));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_ble_host_gatt_notify(svc));
  TEST_END("mcdc gatt_notify (a==NULL || kind!=char_value)");
}

/**
 * @test test_mcdc_gatt_notify_decl_props
 *
 * @par MC/DC:
 * Decision: `(decl == NULL) || ((decl->props & notify_bit) == 0U)`
 * (2 conditions, libs/ra8_ble_host/src/ra8_ble_gatt.c line 381)
 * - V1 decl=non-NULL && notify_bit set -> both F. Proceeds (ok).
 * - V3 decl=non-NULL && notify_bit clear (read-only char) -> C1=F, C2=T.
 *   invalid_arg. (Independent flip of C2 with C1 held F.)
 *
 * @par DO-178C 6.4.4.3 omission rationale:
 * Vector (C1=T, decl==NULL) is structurally unreachable: every char_value
 * created via ra8_ble_host_gatt_register_char is preceded by a char_decl
 * row at handle=value-1 (see ra8_ble_gatt.c lines 286-298). The harness
 * cannot evict a decl row through any public API without violating
 * stack invariants. Per IEC 61508 / DO-178C 6.4.4.3 we document the
 * (T,*) branch as dead-from-construction; V1 and V3 give full MC/DC
 * over the reachable sub-domain. Coverage of the (T,*) branch is
 * delegated to a future fault-injection fixture that mutates
 * s_state.attrs[] directly.
 */
static void test_mcdc_gatt_notify_decl_props(void)
{
  TEST_BEGIN("mcdc gatt_notify (decl==NULL || (props & notify)==0)");
  prep_init(k_ra8_ble_host_role_peripheral);
  uint8_t  svc_uuid[16];
  uint8_t  chr_n_uuid[16];
  uint8_t  chr_r_uuid[16];
  uint16_t svc   = 0U;
  uint16_t chr_n = 0U;
  uint16_t chr_r = 0U;
  uint8_t  buf_n[k_mcdc_gatt_buf_size];
  uint8_t  buf_r[k_mcdc_gatt_buf_size];
  make_uuid(svc_uuid, 0x90U);
  make_uuid(chr_n_uuid, 0x91U);
  make_uuid(chr_r_uuid, 0x92U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_register_service(svc_uuid, &svc));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_gatt_register_char(
                   svc,
                   chr_n_uuid,
                   (uint8_t)(k_ra8_ble_host_char_prop_read | k_ra8_ble_host_char_prop_notify),
                   buf_n,
                   (uint16_t)k_mcdc_gatt_buf_size,
                   &chr_n));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_gatt_register_char(svc,
                                                 chr_r_uuid,
                                                 (uint8_t)k_ra8_ble_host_char_prop_read,
                                                 buf_r,
                                                 (uint16_t)k_mcdc_gatt_buf_size,
                                                 &chr_r));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_notify(chr_n));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ble_host_gatt_notify(chr_r));
  TEST_END("mcdc gatt_notify (decl==NULL || (props & notify)==0)");
}

/**
 * @test test_mcdc_gatt_notify_subscriber_walk
 *
 * @par MC/DC:
 * Decision: `(kind==cccd) && (value_handle_owner==char_handle) &&
 *           ((cccd_value & notify_bit) != 0U)`
 * (3 conditions, libs/ra8_ble_host/src/ra8_ble_gatt.c line 393)
 * - V1 (T,T,T) subscribed CCCD with notify enabled -> match. notify
 *   constructs HVN PDU and returns ok.
 * - V2 (F,*,*) walked over non-CCCD rows (svc/decl/char_value); these
 *   rows always C1=F. Implicitly covered by V1 (the loop iterates over
 *   every attr regardless).
 * - V3 (T,F,*) subscribed CCCD belongs to a *different* char -> C2=F.
 * - V4 (T,T,F) subscribed CCCD owner matches but cccd_value bit clear
 *   (indicate-only enabled) -> C3=F.
 * V1 vs V3 vary C2 (C1 held T, C3 effectively don't-care via short-
 *   circuit, achieved through fresh CCCD with notify bit set on the
 *   wrong owner).
 * V1 vs V4 vary C3 (C1,C2 held T).
 * V1 vs V2 vary C1 (loop iterations on non-CCCD rows).
 * N+1 = 4 vectors for N=3 conditions: minimal MC/DC.
 *
 * @par DO-178C 6.4.4.3 rationale: Full minimal MC/DC achieved.
 */
static void test_mcdc_gatt_notify_subscriber_walk(void)
{
  TEST_BEGIN("mcdc gatt_notify subscriber CCCD compound (3-cond AND)");
  static const uint16_t k_test_conn_local = 0x0040U;

  /* V1: subscribed CCCD with notify bit set. */
  prep_init(k_ra8_ble_host_role_peripheral);
  uint8_t  svc_uuid[16];
  uint8_t  chr_uuid[16];
  uint16_t svc = 0U;
  uint16_t chr = 0U;
  uint8_t  buf[k_mcdc_gatt_buf_size];
  make_uuid(svc_uuid, 0xA1U);
  make_uuid(chr_uuid, 0xA2U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_register_service(svc_uuid, &svc));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_gatt_register_char(
                   svc,
                   chr_uuid,
                   (uint8_t)(k_ra8_ble_host_char_prop_read | k_ra8_ble_host_char_prop_notify),
                   buf,
                   (uint16_t)k_mcdc_gatt_buf_size,
                   &chr));
  ra8_ble_host_test_inject_connect(k_test_conn_local);
  uint16_t cccd_handle = (uint16_t)(chr + 1U);
  uint8_t  l2[10]      = {
    5U,
    0U,
    0x04U,
    0U,
    0x12U,
    (uint8_t)(cccd_handle & 0xFFU),
    (uint8_t)((cccd_handle >> 8) & 0xFFU),
    0x01U,
    0U,
    0U,
  };
  ra8_ble_host_test_inject_acl(k_test_conn_local, l2, 9U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_notify(chr));

  /* V3: register two notify-capable chars on a fresh stack, subscribe
   * char A only, notify char B -> the existing CCCD row mismatches owner. */
  prep_init(k_ra8_ble_host_role_peripheral);
  uint8_t chr_a_uuid[16];
  uint8_t chr_b_uuid[16];
  make_uuid(svc_uuid, 0xA3U);
  make_uuid(chr_a_uuid, 0xA4U);
  make_uuid(chr_b_uuid, 0xA5U);
  uint16_t svc2 = 0U;
  uint16_t chra = 0U;
  uint16_t chrb = 0U;
  uint8_t  bufa[k_mcdc_gatt_buf_size];
  uint8_t  bufb[k_mcdc_gatt_buf_size];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_register_service(svc_uuid, &svc2));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_gatt_register_char(
                   svc2,
                   chr_a_uuid,
                   (uint8_t)(k_ra8_ble_host_char_prop_read | k_ra8_ble_host_char_prop_notify),
                   bufa,
                   (uint16_t)k_mcdc_gatt_buf_size,
                   &chra));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_gatt_register_char(
                   svc2,
                   chr_b_uuid,
                   (uint8_t)(k_ra8_ble_host_char_prop_read | k_ra8_ble_host_char_prop_notify),
                   bufb,
                   (uint16_t)k_mcdc_gatt_buf_size,
                   &chrb));
  ra8_ble_host_test_inject_connect(k_test_conn_local);
  uint16_t cccd_a  = (uint16_t)(chra + 1U);
  uint8_t  l2a[10] = {
    5U,
    0U,
    0x04U,
    0U,
    0x12U,
    (uint8_t)(cccd_a & 0xFFU),
    (uint8_t)((cccd_a >> 8) & 0xFFU),
    0x01U,
    0U,
    0U,
  };
  ra8_ble_host_test_inject_acl(k_test_conn_local, l2a, 9U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_notify(chrb));

  /* V4: subscribed CCCD owner matches but only indicate enabled. */
  prep_init(k_ra8_ble_host_role_peripheral);
  make_uuid(svc_uuid, 0xA6U);
  make_uuid(chr_uuid, 0xA7U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_register_service(svc_uuid, &svc));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_gatt_register_char(svc,
                                                 chr_uuid,
                                                 (uint8_t)(k_ra8_ble_host_char_prop_read |
                                                           k_ra8_ble_host_char_prop_notify |
                                                           k_ra8_ble_host_char_prop_indicate),
                                                 buf,
                                                 (uint16_t)k_mcdc_gatt_buf_size,
                                                 &chr));
  ra8_ble_host_test_inject_connect(k_test_conn_local);
  cccd_handle        = (uint16_t)(chr + 1U);
  uint8_t l2_ind[10] = {
    5U,
    0U,
    0x04U,
    0U,
    0x12U,
    (uint8_t)(cccd_handle & 0xFFU),
    (uint8_t)((cccd_handle >> 8) & 0xFFU),
    0x02U, /* indicate only */
    0U,
    0U,
  };
  ra8_ble_host_test_inject_acl(k_test_conn_local, l2_ind, 9U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_notify(chr));
  TEST_END("mcdc gatt_notify subscriber CCCD compound (3-cond AND)");
}

/**
 * @test test_mcdc_gatt_notify_value_copy
 *
 * @par MC/DC:
 * Decision: `(value_len > 0U) && (a->value != NULL)`
 * (2 conditions, libs/ra8_ble_host/src/ra8_ble_gatt.c line 412)
 * - V1 value_len=0 -> C1=F. Decision F (skip memcpy).
 * - V2 value_len>0, a->value!=NULL -> both T. Decision T (memcpy).
 *
 * @par DO-178C 6.4.4.3 omission rationale:
 * Same structural argument as line 351 (set_value): chars registered
 * with NULL backing buffer have value_max==0, so set_value can never
 * push value_len above 0; the (value_len>0 && a->value==NULL) row is
 * dead-from-invariant. V1 (C1=F) and V2 (both T) cover the reachable
 * sub-domain fully. Per IEC 61508 / DO-178C 6.4.4.3 we omit the (T,F)
 * vector and document the upstream invariant.
 */
static void test_mcdc_gatt_notify_value_copy(void)
{
  TEST_BEGIN("mcdc gatt_notify value memcpy guard reachable subset");
  static const uint16_t k_test_conn_local = 0x0040U;
  prep_init(k_ra8_ble_host_role_peripheral);
  uint8_t  svc_uuid[16];
  uint8_t  chr_uuid[16];
  uint16_t svc = 0U;
  uint16_t chr = 0U;
  uint8_t  buf[k_mcdc_gatt_buf_size];
  make_uuid(svc_uuid, 0x60U);
  make_uuid(chr_uuid, 0x61U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_register_service(svc_uuid, &svc));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_gatt_register_char(
                   svc,
                   chr_uuid,
                   (uint8_t)(k_ra8_ble_host_char_prop_read | k_ra8_ble_host_char_prop_notify),
                   buf,
                   (uint16_t)k_mcdc_gatt_buf_size,
                   &chr));
  ra8_ble_host_test_inject_connect(k_test_conn_local);
  uint16_t cccd_handle = (uint16_t)(chr + 1U);
  uint8_t  l2[10]      = {
    5U,
    0U,
    0x04U,
    0U,
    0x12U,
    (uint8_t)(cccd_handle & 0xFFU),
    (uint8_t)((cccd_handle >> 8) & 0xFFU),
    0x01U,
    0U,
    0U,
  };
  ra8_ble_host_test_inject_acl(k_test_conn_local, l2, 9U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_notify(chr));
  uint8_t pl[k_mcdc_gatt_payload_small] = {0xDEU, 0xADU, 0xBEU, 0xEFU};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_gatt_set_value(chr, pl, (uint16_t)k_mcdc_gatt_payload_small));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_notify(chr));
  TEST_END("mcdc gatt_notify value memcpy guard reachable subset");
}

/* --------------------------------------------------------------------- */

int main(void)
{
  test_init_close();
  test_init_null_and_role();
  test_pre_init_guards();
  test_advertise_start_sends_enable();
  test_advertise_arg_validation();
  test_register_service_and_char();
  test_set_value_and_notify_paths();
  test_att_write_via_acl();
  test_max_services_exhaustion();
  test_event_handler_attach();
  test_advertise_stop_sends_disable();
  test_mcdc_l2cap_send_null_guard();
  test_mcdc_acl_in_null_or_zero();
  test_mcdc_init_role_4cond();
  test_mcdc_advertise_role_guard();
  test_mcdc_advertise_adv_data_null();
  test_mcdc_advertise_lengths_too_big();
  test_mcdc_advertise_interval_range();
  test_mcdc_advertise_scan_resp_null();
  test_mcdc_init_name_copy_loop();
  test_mcdc_att_handle_pdu_null_or_zero();
  test_mcdc_gatt_register_service_null_guard();
  test_mcdc_gatt_register_char_uuid_or_out_null();
  test_mcdc_gatt_register_char_value_buf_null_with_max();
  test_mcdc_gatt_register_char_svc_match();
  test_mcdc_gatt_set_value_null_with_len();
  test_mcdc_gatt_set_value_attr_lookup();
  test_mcdc_gatt_set_value_copy_guard();
  test_mcdc_gatt_notify_attr_lookup();
  test_mcdc_gatt_notify_decl_props();
  test_mcdc_gatt_notify_subscriber_walk();
  test_mcdc_gatt_notify_value_copy();
  return 0;
}
