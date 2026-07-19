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
 * The MC/DC vector tests live in test_ra8_ble_host_adv_mcdc.c (L2CAP /
 * init / advertising) and test_ra8_ble_host_gatt_mcdc.c (ATT / GATT).
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

/**
 * @enum ble_host_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_ble_cap_tail_bytes    = 5U,    /**< Bytes of the LE_Set_Advertising_Enable command at
                                        the tail of the HCI capture:
                                        [0x01][0x0A][0x20][0x01][0x01].              */
  k_sub_l2cap_payload_len = 5U,    /**< L2CAP length field: ATT opcode + handle + a
                                        2-byte value.                                */
  k_byte_mask             = 0xFFU, /**< Low-byte mask used by the put16 helper. */
  k_att_write_value =
    0xABU, /**< A recognizable byte written through the ATT write path and read back from the characteristic. */
  k_uuid_notify_svc = 0x30U, /**< Service of the notify fixture.                       */
  k_uuid_notify_chr = 0x40U, /**< Its characteristic.                                  */
  k_uuid_write_svc  = 0x50U, /**< Service of the ATT-write fixture.                    */
  k_uuid_write_chr  = 0x60U, /**< Its characteristic.                                  */
  k_uuid_single_svc = 0xF0U, /**< The lone service of the single-registration fixture. */
  k_sub_frame_len =
    9U, /**< Bytes actually injected: the 4-byte L2CAP header plus its 5-byte payload. */
  k_sub_frame_cap     = 10, /**< Capacity of the frame staging buffer.               */
  k_att_off_handle_lo = 5,  /**< Low byte of the ATT handle within the staged frame. */
  k_att_off_value_lo  = 7,  /**< First byte of the ATT value that follows it.        */
  k_uuid_fill_base =
    0x80U, /**< Base marker of the table-filling loop; `+ i` gives each registration its own UUID so the table fills with distinct entries. */
} ble_host_uint8_const_t;

/* Test hooks from libs/ra8_hal/src/ra8_ble.c. */
const uint8_t* ra8_ble_test_tx_capture(uint16_t* out_len);
void           ra8_ble_test_reset_capture(void);

/* Test hooks from libs/ra8_ble_host/src/ra8_ble_l2cap.c. */

/* Internal L2CAP TX entry point declared in ra8_ble_host_internal.h; we
 * re-declare it here so the MC/DC tests do not have to drag in that
 * private header. The signature MUST mirror ra8_ble_l2cap.c verbatim. */
ra8_err_t ra8_ble_host_l2cap_send(uint16_t       conn_handle,
                                  uint16_t       cid,
                                  const uint8_t* payload,
                                  uint16_t       payload_len);

typedef enum : uint16_t {
  k_test_op_le_set_adv_params = 0x2006U, /**< Test op le set adv params. */
  k_test_op_le_set_adv_data   = 0x2008U, /**< Test op le set adv data.   */
  k_test_op_le_set_adv_enable = 0x200AU, /**< Test op le set adv enable. */
  k_test_conn_handle          = 0x0040U, /**< Test conn handle.          */
  k_test_l2cap_cid_att        = 0x0004U, /**< Test L2CAP cid ATT.        */
  k_test_default_appearance   = 0x0040U, /**< Test default appearance.   */
  k_test_adv_interval_ms      = 100U,    /**< Test adv interval ms.      */
  k_test_adv_interval_too_low = 1U,      /**< Test adv interval too low. */
  k_test_value_buf_size       = 32U,     /**< Test value buffer size.    */
} ble_host_test_words_t;

typedef enum : uint8_t {
  k_test_pkt_cmd_byte     = 0x01U, /**< Test pkt cmd byte.     */
  k_test_pkt_acl_byte     = 0x02U, /**< Test pkt acl byte.     */
  k_test_att_op_write_req = 0x12U, /**< Test ATT op write req. */
  k_test_att_op_read_req  = 0x0AU, /**< Test ATT op read req.  */
  k_test_evt_count_zero   = 0U,    /**< Test evt count zero.   */
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
  const uint8_t* tail = cap + (cap_len - k_ble_cap_tail_bytes);
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
 * @brief Register a notify-capable service + characteristic; return the value handle.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static uint16_t setup_notify_char(uint8_t* buf)
{
  prep_init(k_ra8_ble_host_role_peripheral);
  uint8_t svc_uuid[16];
  uint8_t chr_uuid[16];
  make_uuid(svc_uuid, k_uuid_notify_svc);
  make_uuid(chr_uuid, k_uuid_notify_chr);

  uint16_t svc = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_register_service(svc_uuid, &svc));

  uint16_t chr = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_gatt_register_char(
                   svc,
                   chr_uuid,
                   (uint8_t)(k_ra8_ble_host_char_prop_read | k_ra8_ble_host_char_prop_notify),
                   buf,
                   (uint16_t)k_test_value_buf_size,
                   &chr));
  return chr;
}

static void test_set_value_and_notify_paths(void)
{
  TEST_BEGIN("ble_host gatt set_value + notify");
  uint8_t        buf[k_test_value_buf_size];
  const uint16_t chr = setup_notify_char(buf);

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
  uint8_t        l2cap_frame[k_sub_frame_cap];
  /* L2CAP header: payload_len = 5 (write req opcode + handle + 2 byte value). */
  l2cap_frame[0]                   = k_sub_l2cap_payload_len;
  l2cap_frame[1]                   = 0U;
  l2cap_frame[2]                   = 0x04U;
  l2cap_frame[3]                   = 0x00U;
  l2cap_frame[4]                   = (uint8_t)k_test_att_op_write_req;
  l2cap_frame[k_att_off_handle_lo] = (uint8_t)(cccd_handle & k_byte_mask);
  l2cap_frame[6]                   = (uint8_t)((cccd_handle >> 8U) & k_byte_mask);
  l2cap_frame[k_att_off_value_lo]  = 0x01U;
  l2cap_frame[8]                   = 0x00U;

  ra8_ble_host_test_inject_acl((uint16_t)k_test_conn_handle, l2cap_frame, k_sub_frame_len);
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
  make_uuid(svc_uuid, k_uuid_write_svc);
  make_uuid(chr_uuid, k_uuid_write_chr);

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
  uint8_t l2cap_frame[k_sub_frame_cap];
  l2cap_frame[0]                   = 4U;
  l2cap_frame[1]                   = 0U;
  l2cap_frame[2]                   = 0x04U;
  l2cap_frame[3]                   = 0x00U;
  l2cap_frame[4]                   = (uint8_t)k_test_att_op_write_req;
  l2cap_frame[k_att_off_handle_lo] = (uint8_t)(chr & k_byte_mask);
  l2cap_frame[6]                   = (uint8_t)((chr >> 8U) & k_byte_mask);
  l2cap_frame[k_att_off_value_lo]  = k_att_write_value;
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
    make_uuid(uuid, (uint8_t)(k_uuid_fill_base + i));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_register_service(uuid, &h));
  }
  /* One more should fail with no_mem. */
  make_uuid(uuid, k_uuid_single_svc);
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
  return 0;
}
