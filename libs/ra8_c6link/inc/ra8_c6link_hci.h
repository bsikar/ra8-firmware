/**
 * @file ra8_c6link_hci.h
 * @brief The HCI channel on the C6 link: H4 packets over `ESP_HCI_IF`.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The RA8 has no Bluetooth radio, so the BLE controller lives on the ESP32-C6
 * and everything below HCI is the co-processor's job. This header is the
 * transport that carries HCI across the companion link, in the same shape the
 * Wi-Fi and 802.3 paths already use: one interface type on the shared frame,
 * one staged payload per transaction, no queue and no hidden thread.
 *
 * @par The one thing HCI does differently on this wire
 * Every other interface type puts the whole payload after the header. HCI does
 * not. The H4 packet-indicator octet (Bluetooth Core 5.3 Vol 4 Part A 2) is
 * lifted out of the payload and transmitted in the header's trailing union
 * field as `hci_pkt_type`, and the declared length counts the bytes that
 * follow it. That is not this library's invention: it is what the vendored
 * host driver does in `spi_drv.c` when `if_type == ESP_HCI_IF`, and the
 * co-processor's HCI path decodes it that way, so a conformant frame must do
 * the same or the slave reads the indicator as the first byte of an opcode.
 *
 * Because the indicator sits inside the checksummed header, it has to be
 * chosen before the frame is sealed rather than patched in afterwards; the
 * staging call below is what records it.
 *
 * @par What this is not
 * This is the transport half of issue #493 and nothing above it. No host
 * stack, no advertising state, no controller bring-up sequence: the HCI seam
 * in `ra8_ble.h` is what binds NimBLE to this channel, and moving that seam
 * off its in-memory loopback is the next step on the same issue. Nothing here
 * has run against a C6; see the pull request for what the bench still owes.
 *
 * @par Example:
 * @code
 * static const uint8_t reset[] = {0x01U, 0x03U, 0x0CU, 0x00U};
 * if (ra8_c6link_hci_send(&link, reset, (uint16_t)sizeof reset) != k_ra8_ok) {
 *   return;
 * }
 * @endcode
 *
 * @see ra8_c6link_hci_send
 * @see ra8_c6link_hci_attach
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_c6link.h"
#include "ra8_err.h"

/* =============================================================================
 * Geometry
 * =============================================================================
 */

/**
 * @enum ra8_c6link_hci_geometry_t
 * @brief Bounds the HCI channel works to.
 *
 * @details
 * The indicator octet is carried in the header, so one link frame holds one
 * more HCI byte than it holds bytes of any other interface type. The floor is
 * the smallest packet H4 can express at all: an indicator plus the shortest
 * header the three packet types define, which is the two-octet event header.
 *
 * @invariant ::k_ra8_c6link_hci_max_packet is one more than
 *            ::k_ra8_c6link_max_payload, because exactly one octet of every
 *            H4 packet travels in the frame header.
 *
 * @see ra8_c6link_hci_send
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_c6link_hci_max_packet = 1589U,
  /**< Largest H4 packet one frame carries: indicator plus payload. */
  k_ra8_c6link_hci_min_packet = 3U,
  /**< Smallest H4 packet with a complete header: indicator plus two octets. */
} ra8_c6link_hci_geometry_t;

/**
 * @enum ra8_c6link_hci_pkt_t
 * @brief H4 packet-indicator octets (Bluetooth Core 5.3 Vol 4 Part A 2).
 *
 * @details
 * Protocol constants, not registers. Declared here as well as on the BLE seam
 * because this channel has to recognise them to reject a packet type the
 * co-processor's HCI path does not carry, and because the host-side seam and
 * the transport must agree on the octet or the two disagree silently.
 *
 * @note Synchronous data (0x03) is deliberately absent: the C6 controller this
 *       tree targets is BLE-only, and a frame claiming SCO is refused rather
 *       than forwarded to a controller that cannot answer it.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_c6link_hci_cmd = 0x01U, /**< HCI command packet.  */
  k_ra8_c6link_hci_acl = 0x02U, /**< HCI ACL data packet. */
  k_ra8_c6link_hci_evt = 0x04U, /**< HCI event packet.    */
} ra8_c6link_hci_pkt_t;

/* =============================================================================
 * Receive sink
 * =============================================================================
 */

/**
 * @typedef ra8_c6link_hci_cb_t
 * @brief Called once per well-formed HCI packet the co-processor sent.
 *
 * @details
 * The packet is delivered reassembled: @p packet[0] is the H4 indicator this
 * library restored from the frame header, and the controller's bytes follow.
 * That is deliberately the shape the vendored `hci_rx_handler` expects, so a
 * host stack bound to this channel needs no second reassembly step.
 *
 * @param[in] ctx Context registered with ::ra8_c6link_hci_attach.
 * @param[in] packet Reassembled H4 packet; valid only for this call.
 * @param[in] len Octets readable at @p packet, at least
 *                ::k_ra8_c6link_hci_min_packet.
 * @since 0.1.0
 */
typedef void (*ra8_c6link_hci_cb_t)(void* ctx, const uint8_t* packet, uint16_t len);

/* =============================================================================
 * Operations
 * =============================================================================
 */

/**
 * @brief Register the sink that receives HCI packets from the co-processor.
 *
 * @details
 * Until a sink is registered, an arriving HCI frame is counted and dropped
 * rather than buffered: this link holds one receive transaction and the next
 * transfer overwrites it, so a packet nobody consumes has nowhere to wait.
 *
 * @param[in,out] link Open handle; must be non-null.
 * @param[in] cb Sink to call, or nullptr to detach.
 * @param[in] ctx Context passed back to @p cb unchanged.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The sink was recorded.
 * @retval k_ra8_err_null_ptr @p link was null.
 * @retval k_ra8_err_not_initialized @p link is not open.
 * @pre @p link has been opened with ::ra8_c6link_open.
 * @pre No pump is running on @p link.
 * @post The recorded sink and context are exactly the arguments given.
 * @post No frame is transmitted and no counter advances.
 * @note Not thread-safe with a running pump.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_c6link_hci_attach(ra8_c6link_t* link, ra8_c6link_hci_cb_t cb, void* ctx);

/**
 * @brief Send one H4 HCI packet to the co-processor's controller.
 *
 * @details
 * Stages the packet as an `ESP_HCI_IF` frame -- indicator octet into the
 * header's `hci_pkt_type`, the remaining bytes as the payload, the declared
 * length counting only those -- and clocks transactions until it has gone out.
 * Same staging and pumping contract as ::ra8_c6link_eth_send, including its
 * one-staged-payload exclusivity.
 *
 * @param[in,out] link Open handle; must be non-null.
 * @param[in] packet H4 packet, indicator octet first; must be non-null.
 * @param[in] len Octets readable at @p packet.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The frame was clocked out.
 * @retval k_ra8_err_null_ptr @p link or @p packet was null.
 * @retval k_ra8_err_not_initialized @p link is not open.
 * @retval k_ra8_err_invalid_size @p len is outside
 *         ::k_ra8_c6link_hci_min_packet .. ::k_ra8_c6link_hci_max_packet.
 * @retval k_ra8_err_invalid_arg @p packet[0] is not an H4 indicator this
 *         channel carries.
 * @retval k_ra8_err_busy Another payload is already staged.
 * @retval k_ra8_err_hw_timeout The co-processor never took the frame.
 * @pre @p link has been opened with ::ra8_c6link_open.
 * @pre @p packet addresses at least @p len readable octets.
 * @post On success nothing is staged and the frame has been transmitted.
 * @post On any failure the staging slot is left empty.
 * @note Not thread-safe; one sender per link.
 * @warning The indicator octet is checksummed as part of the header, so a
 *          caller must not patch it after this call returns.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_c6link_hci_send(ra8_c6link_t* link, const uint8_t* packet, uint16_t len);

/**
 * @brief Reassemble an H4 packet from a received HCI frame's two parts.
 *
 * @details
 * A pure function over buffers: it prepends @p pkt_type, the indicator this
 * library read out of the frame header, to @p payload. It exists separately
 * from the dispatch path so the reassembly the sink depends on can be pinned
 * by a test with no transport, and so a caller draining frames itself can
 * reuse it.
 *
 * @param[in] pkt_type Indicator octet from the frame header.
 * @param[in] payload Controller bytes that followed the header; may be null
 *                    only when @p len is zero.
 * @param[in] len Octets readable at @p payload.
 * @param[out] out Destination for the reassembled packet; must be non-null.
 * @param[in] cap Octets writable at @p out.
 * @return Octets written to @p out, or zero when the input was refused.
 * @retval 0 The indicator is not one this channel carries, the payload is
 *           shorter than a complete H4 header, or @p cap is too small.
 * @pre @p out addresses at least @p cap writable octets.
 * @pre @p payload and @p out do not overlap.
 * @post A non-zero return means @p out[0] is @p pkt_type and the payload
 *       follows it in order.
 * @post A zero return leaves @p out unmodified.
 * @note Bounded copy; no allocation and no retained pointer.
 * @since 0.1.0
 */
[[nodiscard]] uint16_t ra8_c6link_hci_reassemble(uint8_t        pkt_type,
                                                 const uint8_t* payload,
                                                 uint16_t       len,
                                                 uint8_t*       out,
                                                 uint16_t       cap);

#ifdef __cplusplus
}
#endif
