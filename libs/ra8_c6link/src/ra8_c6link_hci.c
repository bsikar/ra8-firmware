/**
 * @file ra8_c6link_hci.c
 * @brief The HCI channel: H4 packets across the C6 link on `ESP_HCI_IF`.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Three things live here and nothing else: the guard that decides whether an
 * H4 packet can be carried at all, the split that moves its indicator octet
 * into the frame header before the frame is sealed, and the reassembly that
 * puts the octet back on the receive side.
 *
 * The split is the whole reason this file exists rather than a two-line call
 * into ::ra8_c6link_eth_send. Upstream's host driver treats `ESP_HCI_IF`
 * specially when it forms the transmit header -- it copies `payload[0]` into
 * `payload_header->hci_pkt_type`, decrements the declared length, and copies
 * from `payload[1]` onwards (`spi_drv.c`, the `if_type == ESP_HCI_IF` arm of
 * its header-forming path) -- and only then computes the checksum over header
 * plus payload. The indicator is therefore inside the checksummed span, which
 * is why the staging slot below carries it rather than the sender patching the
 * sealed frame.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_c6link_hci.h"

#include <stdint.h>

#include "esp_hosted_interface.h"
#include "ra8_attributes.h"
#include "ra8_c6link.h"
#include "ra8_c6link_internal.h"
#include "ra8_err.h"

/**
 * @enum ra8_c6link_hci_const_t
 * @brief File-local shape constants for the H4 packets this channel carries.
 *
 * @details
 * The minimum-header figures are the shortest complete header each H4 packet
 * type defines (Bluetooth Core 5.3 Vol 4 Part A 2 and Part E 5.4): a command
 * carries a two-octet opcode plus a one-octet parameter length, an event a
 * one-octet code plus a one-octet parameter length, and an ACL packet a
 * two-octet handle plus a two-octet length. They bound the payload that must
 * follow the indicator, not the payload's declared contents.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_hci_indicator_octets = 1U, /**< Octets of every H4 packet held in the header. */
  k_hci_body_min_cmd     = 3U, /**< Opcode plus parameter length.                */
  k_hci_body_min_evt     = 2U, /**< Event code plus parameter length.            */
  k_hci_body_min_acl     = 4U, /**< Handle plus data length.                     */
} ra8_c6link_hci_const_t;

/**
 * @brief Report the shortest body a given H4 indicator can legally precede.
 *
 * @details
 * Returns zero for an indicator this channel does not carry, which makes the
 * "is this a packet type we transmit" question and the "how short is too
 * short" question one lookup instead of two parallel switch statements that
 * can drift apart.
 *
 * @param[in] pkt_type H4 indicator octet.
 * @return Minimum body octets that must follow the indicator.
 * @retval 0 @p pkt_type is not an indicator this channel carries.
 * @pre None; every input value is answered.
 * @pre No link state is consulted.
 * @post No state is modified.
 * @post A non-zero result is the minimum for exactly that packet type.
 * @note Pure function.
 * @par MC/DC:
 * The switch is decision-free per arm; the three carried types and one
 * refused type are covered by the four vectors in the channel's tests.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t internal_c6link_hci_body_min(uint8_t pkt_type)
{
  switch (pkt_type) {
    case (uint8_t)k_ra8_c6link_hci_cmd:
      return (uint16_t)k_hci_body_min_cmd;
    case (uint8_t)k_ra8_c6link_hci_evt:
      return (uint16_t)k_hci_body_min_evt;
    case (uint8_t)k_ra8_c6link_hci_acl:
      return (uint16_t)k_hci_body_min_acl;
    default:
      return 0U;
  }
}

ra8_err_t ra8_c6link_hci_attach(ra8_c6link_t* link, ra8_c6link_hci_cb_t cb, void* ctx)
{
  if (link == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!link->open) {
    return k_ra8_err_not_initialized;
  }
  link->hci_cb  = cb;
  link->hci_ctx = ctx;
  return k_ra8_ok;
}

ra8_err_t ra8_c6link_hci_send(ra8_c6link_t* link, const uint8_t* packet, uint16_t len)
{
  if ((link == nullptr) || (packet == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (!link->open) {
    return k_ra8_err_not_initialized;
  }
  if ((len < (uint16_t)k_ra8_c6link_hci_min_packet) ||
      (len > (uint16_t)k_ra8_c6link_hci_max_packet)) {
    return k_ra8_err_invalid_size;
  }
  const uint16_t body     = (uint16_t)(len - (uint16_t)k_hci_indicator_octets);
  const uint16_t body_min = internal_c6link_hci_body_min(packet[0]);
  if ((body_min == 0U) || (body < body_min)) {
    return k_ra8_err_invalid_arg;
  }
  if (link->tx_len != 0U) {
    return k_ra8_err_busy;
  }

  for (uint16_t i = 0U; i < body; i++) {
    link->tx[(uint16_t)k_ra8_c6link_header_bytes + i] =
      packet[i + (uint16_t)k_hci_indicator_octets];
  }
  link->tx_len      = body;
  link->tx_if       = (uint8_t)ESP_HCI_IF;
  link->tx_pkt_type = packet[0];

  ra8_c6link_stats_t local  = {};
  const ra8_err_t    pumped = priv_c6link_pump(link, (uint16_t)k_ra8_c6link_hs_giveup, &local);
  if (pumped != k_ra8_ok) {
    link->tx_len = 0U;
    return pumped;
  }
  if (link->tx_len != 0U) {
    link->tx_len = 0U;
    return k_ra8_err_hw_timeout;
  }
  return k_ra8_ok;
}

uint16_t ra8_c6link_hci_reassemble(uint8_t        pkt_type,
                                   const uint8_t* payload,
                                   uint16_t       len,
                                   uint8_t*       out,
                                   uint16_t       cap)
{
  if (out == nullptr) {
    return 0U;
  }
  const uint16_t body_min = internal_c6link_hci_body_min(pkt_type);
  if ((body_min == 0U) || (len < body_min)) {
    return 0U;
  }
  if (payload == nullptr) {
    return 0U;
  }
  const uint16_t total = (uint16_t)(len + (uint16_t)k_hci_indicator_octets);
  if (cap < total) {
    return 0U;
  }

  out[0] = pkt_type;
  for (uint16_t i = 0U; i < len; i++) {
    out[i + (uint16_t)k_hci_indicator_octets] = payload[i];
  }
  return total;
}

RA8_PRIV bool priv_c6link_hci_consume(ra8_c6link_t*  link,
                                      uint8_t        pkt_type,
                                      const uint8_t* payload,
                                      uint16_t       len)
{
  if ((link == nullptr) || (payload == nullptr)) {
    return false;
  }
  if (link->stats != nullptr) {
    link->stats->hci_in++;
  }
  if (link->hci_cb == nullptr) {
    return false;
  }

  static uint8_t s_packet[(size_t)k_ra8_c6link_hci_max_packet];
  const uint16_t total = ra8_c6link_hci_reassemble(pkt_type,
                                                   payload,
                                                   len,
                                                   s_packet,
                                                   (uint16_t)k_ra8_c6link_hci_max_packet);
  if (total == 0U) {
    if (link->stats != nullptr) {
      link->stats->hci_dropped++;
    }
    return false;
  }
  link->hci_cb(link->hci_ctx, s_packet, total);
  return false;
}
