/**
 * @file test_ra8_ble_c6link.c
 * @brief Integration test: `ra8_ble` over `port/nimble/src/ble_hci_c6link.c` and `ra8_c6link`.
 *
 * @details
 * Drives `ra8_ble` public HCI calls (GAP command wrappers, raw HCI commands,
 * ACL data, and `ra8_ble_dispatch`) across `ble_hci_c6link`, `ra8_c6link` SPI
 * `ESP_HCI_IF` framing, and `ra8_c6_model`, checking wire-level checksum and
 * header byte 11 (`hci_pkt_type`) on TX plus intact inbound H4 event/ACL
 * delivery on RX.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ble_hci_c6link.h"
#include "ra8_attributes.h"
#include "ra8_ble.h"
#include "ra8_ble_internal.h"
#include "ra8_c6_model.h"
#include "ra8_c6link.h"
#include "ra8_c6link_model_test_internal.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "unity_minimal.h"

/** @enum t_ble_c6_const_t @brief Constants owned by the BLE-over-c6link test. */
typedef enum : uint32_t {
  k_t_scan_interval = 0x0040U, /**< LE_Set_Scan_Parameters interval (40 ms). */
  k_t_scan_window   = 0x0020U, /**< LE_Set_Scan_Parameters window (20 ms).   */
  k_t_acl_handle    = 0x0042U, /**< Test ACL connection handle.              */
} t_ble_c6_const_t;

static int32_t  s_evt_count;
static uint8_t  s_evt_code;
static uint8_t  s_evt_plen;
static uint8_t  s_evt_param0;
static int32_t  s_acl_count;
static uint16_t s_acl_handle;
static uint16_t s_acl_len;
static uint8_t  s_acl_first;

RA8_INTERNAL static void on_hci_evt(void* ctx, uint8_t evt_code, const uint8_t* params, uint8_t len)
{
  (void)ctx;
  s_evt_count++;
  s_evt_code   = evt_code;
  s_evt_plen   = len;
  s_evt_param0 = (len > 0U) ? params[0] : 0U;
}

RA8_INTERNAL static void
on_hci_acl(void* ctx, uint16_t handle, const uint8_t* payload, uint16_t len)
{
  (void)ctx;
  s_acl_count++;
  s_acl_handle = handle;
  s_acl_len    = len;
  s_acl_first  = (len > 0U) ? payload[0] : 0U;
}

RA8_INTERNAL static void prep_stack(void)
{
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ble_hci_c6link_deinit());
  (void)ra8_ble_close();
  priv_c6link_test_bringup();

  const ra8_ble_config_t cfg = {.use_external_osc = 0U, .deep_sleep_enable = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_open(&cfg));
  ra8_ble_test_reset_capture();
  TEST_ASSERT_EQ(k_ra8_ok, ble_hci_c6link_init(priv_c6link_test_link()));
  TEST_ASSERT(ble_hci_c6link_is_bound());

  s_evt_count  = 0;
  s_evt_code   = 0U;
  s_evt_plen   = 0U;
  s_evt_param0 = 0U;
  s_acl_count  = 0;
  s_acl_handle = 0U;
  s_acl_len    = 0U;
  s_acl_first  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_attach_event_handler(on_hci_evt, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_attach_acl_handler(on_hci_acl, nullptr));
}

/**
 * @par MC/DC:
 * (exercises error guards in `ble_hci_c6link_init` for null and closed link)
 */
RA8_INTERNAL static void test_init_guards(void)
{
  TEST_BEGIN("ble_hci_c6link init/deinit guards");
  TEST_ASSERT_EQ(k_ra8_ok, ble_hci_c6link_deinit());
  TEST_ASSERT(!ble_hci_c6link_is_bound());
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ble_hci_c6link_init(nullptr));

  ra8_c6link_t closed_link = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ble_hci_c6link_init(&closed_link));
  TEST_ASSERT(!ble_hci_c6link_is_bound());
  TEST_END("ble_hci_c6link init/deinit guards");
}

/**
 * @par MC/DC:
 * (exercises full TX path from `ra8_ble` commands and ACL packets into
 * `ra8_c6_model` SPI framing with valid esp-hosted checksums and H4 octet 11)
 */
RA8_INTERNAL static void test_ble_tx_over_c6link(void)
{
  TEST_BEGIN("ble_hci_c6link outbound command and ACL framing");
  prep_stack();

  const uint8_t bd_addr[6] = {0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0xC6U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_set_random_address(bd_addr));
  TEST_ASSERT_EQ(0x01U, ra8_c6_model()->hci_tx_pkt_type);
  TEST_ASSERT_EQ(9U, ra8_c6_model()->hci_tx_len);
  TEST_ASSERT_EQ(0x05U, ra8_c6_model()->hci_tx[0]);
  TEST_ASSERT_EQ(0x20U, ra8_c6_model()->hci_tx[1]);
  TEST_ASSERT_EQ(6U, ra8_c6_model()->hci_tx[2]);
  TEST_ASSERT_EQ(0, memcmp(&ra8_c6_model()->hci_tx[3], bd_addr, sizeof bd_addr));

  const uint8_t adv_payload[3] = {0x02U, 0x01U, 0x06U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_set_advertising_data(adv_payload, (uint8_t)sizeof adv_payload));
  TEST_ASSERT_EQ(0x01U, ra8_c6_model()->hci_tx_pkt_type);
  TEST_ASSERT_EQ(0x08U, ra8_c6_model()->hci_tx[0]);
  TEST_ASSERT_EQ(0x20U, ra8_c6_model()->hci_tx[1]);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_scan_start(1U, (uint16_t)k_t_scan_interval, (uint16_t)k_t_scan_window));
  TEST_ASSERT_EQ(0x01U, ra8_c6_model()->hci_tx_pkt_type);
  TEST_ASSERT_EQ(0x0CU, ra8_c6_model()->hci_tx[0]);
  TEST_ASSERT_EQ(0x20U, ra8_c6_model()->hci_tx[1]);
  TEST_ASSERT_EQ(2U, ra8_c6_model()->hci_tx[2]);
  TEST_ASSERT_EQ(1U, ra8_c6_model()->hci_tx[3]);

  const uint8_t acl_data[3] = {0xDEU, 0xADU, 0xBEU};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ble_hci_send_acl_data((uint16_t)k_t_acl_handle, acl_data, (uint16_t)sizeof acl_data));
  TEST_ASSERT_EQ(0x02U, ra8_c6_model()->hci_tx_pkt_type);
  TEST_ASSERT_EQ(7U, ra8_c6_model()->hci_tx_len);
  TEST_ASSERT_EQ(0x42U, ra8_c6_model()->hci_tx[0]);
  TEST_ASSERT_EQ(0x00U, ra8_c6_model()->hci_tx[1]);
  TEST_ASSERT_EQ(3U, ra8_c6_model()->hci_tx[2]);
  TEST_ASSERT_EQ(0, memcmp(&ra8_c6_model()->hci_tx[4], acl_data, sizeof acl_data));

  TEST_ASSERT_EQ(k_ra8_ok, ble_hci_c6link_deinit());
  TEST_ASSERT(!ble_hci_c6link_is_bound());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_close());
  TEST_END("ble_hci_c6link outbound command and ACL framing");
}

/**
 * @par MC/DC:
 * (exercises inbound H4 event and ACL delivery via `ra8_ble_dispatch`, idle
 * handshake timeout suppression on poll, and SPI error propagation)
 */
RA8_INTERNAL static void test_ble_rx_and_faults_over_c6link(void)
{
  TEST_BEGIN("ble_hci_c6link inbound dispatch and transport faults");
  prep_stack();

  const uint8_t evt_cc[7] = {0x04U, 0x0EU, 0x04U, 0x01U, 0x0AU, 0x20U, 0x00U};
  const uint8_t acl_rx[8] = {0x02U, 0x42U, 0x00U, 0x03U, 0x00U, 0x5AU, 0x6BU, 0x7CU};
  ra8_c6_model_emit_hci(evt_cc, (uint16_t)sizeof evt_cc);
  ra8_c6_model_emit_hci(acl_rx, (uint16_t)sizeof acl_rx);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_dispatch());
  TEST_ASSERT_EQ(1, s_evt_count);
  TEST_ASSERT_EQ(0x0EU, s_evt_code);
  TEST_ASSERT_EQ(4U, s_evt_plen);
  TEST_ASSERT_EQ(0x01U, s_evt_param0);
  TEST_ASSERT_EQ(1, s_acl_count);
  TEST_ASSERT_EQ((uint16_t)k_t_acl_handle, s_acl_handle);
  TEST_ASSERT_EQ(3U, s_acl_len);
  TEST_ASSERT_EQ(0x5AU, s_acl_first);

  /* Idle handshake miss during dispatch poll returns k_ra8_ok cleanly. */
  ra8_c6_model()->handshake = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_dispatch());
  /* Outbound TX preserves handshake timeout error so callers know TX failed. */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_ble_set_advertising_enable(1U));

  /* Real SPI bus faults propagate on both poll and TX paths. */
  ra8_c6_model()->handshake     = true;
  ra8_c6_model()->fail_transfer = true;
  TEST_ASSERT_EQ(k_ra8_err_spi_error, ra8_ble_dispatch());
  TEST_ASSERT_EQ(k_ra8_err_spi_error, ra8_ble_set_advertising_enable(0U));

  /* After deinit, ra8_ble falls back cleanly to loopback mode. */
  ra8_c6_model()->fail_transfer = false;
  TEST_ASSERT_EQ(k_ra8_ok, ble_hci_c6link_deinit());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_set_advertising_enable(1U));
  uint16_t loop_len = 0U;
  (void)ra8_ble_test_tx_capture(&loop_len);
  TEST_ASSERT_EQ(5U, loop_len);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_close());
  TEST_END("ble_hci_c6link inbound dispatch and transport faults");
}

int main(void)
{
  test_init_guards();
  test_ble_tx_over_c6link();
  test_ble_rx_and_faults_over_c6link();
  return 0;
}
