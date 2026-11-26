/**
 * @file port/nimble/src/ble_hci_c6link.c
 * @brief `ra8_ble` HCI transport binding over `ra8_c6link` ESP32-C6 SPI.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Bridges `ra8_ble_bind_transport` (TX + RX poll hooks) and
 * `ra8_c6link_set_hci_rx` (inbound `ESP_HCI_IF` H4 frames pushed via
 * `ra8_ble_rx_push`). Inbound idle polls (`k_ra8_err_hw_timeout`) normalize to
 * `k_ra8_ok` so periodic `ra8_ble_dispatch()` calls do not surface spurious
 * errors when the C6 has no pending HCI frames.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ble_hci_c6link.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_ble.h"
#include "ra8_c6link.h"
#include "ra8_err.h"
#include "ra8_log.h"

/** @enum ble_hci_c6link_const_t @brief Bounded poll transaction budget. */
typedef enum : uint16_t {
  k_ble_hci_c6link_poll_budget = 8U, /**< Max SPI frames pumped per dispatch poll. */
} ble_hci_c6link_const_t;

/** @brief Currently bound companion-link handle, or nullptr when unbound. */
static ra8_c6link_t* s_c6link;

/** @brief Forward inbound `ESP_HCI_IF` H4 packet into the `ra8_ble` RX queue. */
RA8_INTERNAL static void internal_c6link_hci_rx(void* ctx, const uint8_t* frame, uint16_t len)
{
  (void)ctx;
  if ((frame == nullptr) || (len == 0U)) {
    return;
  }
  const ra8_err_t err = ra8_ble_rx_push(frame, len);
  if (err != k_ra8_ok) {
    ra8_log_emit_error_val("BLE", "c6link RX push failed", (uint32_t)err);
  }
}

/** @brief Transmit one outgoing H4 HCI packet over `ra8_c6link`. */
RA8_INTERNAL static ra8_err_t
internal_c6link_hci_send(void* ctx, const uint8_t* h4_pkt, uint16_t len)
{
  ra8_c6link_t* const link = (ra8_c6link_t*)ctx;
  if ((link == nullptr) || (h4_pkt == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  return ra8_c6link_hci_send(link, h4_pkt, len);
}

/** @brief Pump pending `ra8_c6link` transactions and ignore idle handshake timeout. */
RA8_INTERNAL static ra8_err_t internal_c6link_hci_poll(void* ctx)
{
  ra8_c6link_t* const link = (ra8_c6link_t*)ctx;
  if (link == nullptr) {
    return k_ra8_err_null_ptr;
  }
  ra8_c6link_stats_t stats = {};
  const ra8_err_t    err   = ra8_c6link_poll(link, (uint16_t)k_ble_hci_c6link_poll_budget, &stats);
  if (err == k_ra8_err_hw_timeout) {
    return k_ra8_ok;
  }
  return err;
}

/* Bind `ra8_ble` onto an open `ra8_c6link_t` companion link. */
ra8_err_t ble_hci_c6link_init(ra8_c6link_t* link)
{
  if (link == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!ra8_c6link_is_open(link)) {
    return k_ra8_err_not_initialized;
  }
  if ((s_c6link != nullptr) && (s_c6link != link) && ra8_c6link_is_open(s_c6link)) {
    (void)ra8_c6link_set_hci_rx(s_c6link, nullptr, nullptr);
  }
  const ra8_err_t rx_err = ra8_c6link_set_hci_rx(link, internal_c6link_hci_rx, link);
  if (rx_err != k_ra8_ok) {
    return rx_err;
  }
  const ra8_ble_transport_t transport = {
    .send = internal_c6link_hci_send,
    .poll = internal_c6link_hci_poll,
    .ctx  = link,
  };
  const ra8_err_t bind_err = ra8_ble_bind_transport(&transport);
  if (bind_err != k_ra8_ok) {
    (void)ra8_c6link_set_hci_rx(link, nullptr, nullptr);
    return bind_err;
  }
  s_c6link = link;
  return k_ra8_ok;
}

/* Unbind `ra8_ble` from the companion link and restore loopback mode. */
ra8_err_t ble_hci_c6link_deinit(void)
{
  const ra8_err_t unbind_err = ra8_ble_unbind_transport();
  if ((s_c6link != nullptr) && ra8_c6link_is_open(s_c6link)) {
    (void)ra8_c6link_set_hci_rx(s_c6link, nullptr, nullptr);
  }
  s_c6link = nullptr;
  return unbind_err;
}

/* Query whether `ra8_ble` is currently bound to a companion link. */
bool ble_hci_c6link_is_bound(void)
{
  return s_c6link != nullptr;
}
