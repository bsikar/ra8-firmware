/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_fw_version/src/c6_fwver_priv.c
 * @brief The ESP_PRIV_IF control channel: host capabilities out, INIT in.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * ``ESP_PRIV_IF`` carries the two frames that bracket an esp-hosted link's
 * bring-up. The co-processor queues an ``ESP_PRIV_EVENT_INIT`` event at boot
 * announcing its capabilities, its chip id and its firmware version; the host
 * answers with an event of the same shape carrying its own capabilities, the
 * chip id it expects, and the flow-control thresholds it wants. Both are
 * flat TLV lists behind a two-byte event header, and both are built here
 * field for field from the two vendored functions that own them:
 * ``send_slave_config()``  LEGACY-OK: upstream esp-hosted function name
 * and ``process_init_event()``.
 *
 * The INIT event matters beyond politeness: its ``ESP_PRIV_FIRMWARE_VERSION``
 * tag carries the co-processor's own version as a little-endian 32-bit word.
 * That is a SECOND, independent reading of the number the RPC round-trip
 * asks for, arriving by a different mechanism (an unsolicited event rather
 * than a request/response) and decoded by different code. When both agree,
 * the answer is not an artefact of either decoder.
 *
 * It is genuinely optional, though: the co-processor queues it once per boot
 * and holds it until a transaction drains it, so a run that follows an
 * earlier run without power-cycling the C6 will not see one. This module
 * therefore records it when it arrives and never requires it.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "c6_fwver.h"
#include "esp_hosted_host_fw_ver.h"
#include "esp_hosted_interface.h"
#include "esp_hosted_transport.h"
#include "esp_hosted_transport_init.h"
#include "port_esp_hosted_host_config_features.h"
#include "transport_drv.h"

/**
 * @enum c6_fwver_priv_t
 * @brief Layout and values of the ESP_PRIV_IF event this host sends.
 * @details The TLV tags themselves come from the vendored
 * ``esp_hosted_transport.h`` and ``esp_hosted_transport_init.h``; what is
 * named here is the geometry -- how wide a tag's value is, where the event
 * header ends -- plus the one policy value the host chooses.
 * @invariant ::k_c6_fwver_priv_tlv_len is one for every tag this host emits,
 *            so the frame length is a fixed multiple of the TLV stride.
 * @invariant ::k_c6_fwver_priv_host_cap is zero, matching the vendored call
 *            ``send_slave_config(0, ...)``  LEGACY-OK: upstream function name
 *            -- this host advertises no optional transport capability.
 * @par Example:
 * @code
 * out[0] = (uint8_t)ESP_PRIV_EVENT_INIT;
 * out[1] = (uint8_t)tlv_bytes;
 * @endcode
 * @see c6_fwver_priv_host_caps
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_c6_fwver_priv_evt_type   = 0U, /**< Offset of the event type byte.            */
  k_c6_fwver_priv_evt_len    = 1U, /**< Offset of the event length byte.          */
  k_c6_fwver_priv_evt_hdr    = 2U, /**< Bytes before the first TLV.               */
  k_c6_fwver_priv_tlv_tag    = 0U, /**< Offset of a TLV's tag, from its start.    */
  k_c6_fwver_priv_tlv_size   = 1U, /**< Offset of a TLV's length, from its start. */
  k_c6_fwver_priv_tlv_value  = 2U, /**< Offset of a TLV's value, from its start.  */
  k_c6_fwver_priv_tlv_len    = 1U, /**< Value width of every tag this host emits. */
  k_c6_fwver_priv_tlv_stride = 3U, /**< Bytes one one-byte-valued TLV occupies.   */
  k_c6_fwver_priv_tag_count  = 5U, /**< TLVs this host emits.                     */
  k_c6_fwver_priv_host_cap   = 0U, /**< Host capability word; zero, as upstream.  */
} c6_fwver_priv_t;

/**
 * @enum c6_fwver_priv_ver_t
 * @brief Byte layout of the ESP_PRIV_FIRMWARE_VERSION tag's value.
 * @details A little-endian 32-bit word, assembled here one byte at a time so
 * the decode does not depend on the alignment of a payload the co-processor
 * chose the offset of.
 * @invariant The four offsets are consecutive and cover exactly four bytes.
 * @invariant ::k_c6_fwver_priv_ver_bytes equals the tag length the
 *            co-processor advertises for this tag.
 * @par Example:
 * @code
 * ver = (uint32_t)value[k_c6_fwver_priv_ver_b0];
 * @endcode
 * @see c6_fwver_priv_consume
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_c6_fwver_priv_ver_b0    = 0U,  /**< Least significant byte. */
  k_c6_fwver_priv_ver_b1    = 1U,  /**< Second byte.            */
  k_c6_fwver_priv_ver_b2    = 2U,  /**< Third byte.             */
  k_c6_fwver_priv_ver_b3    = 3U,  /**< Most significant byte.  */
  k_c6_fwver_priv_ver_bytes = 4U,  /**< Width of the tag value. */
  k_c6_fwver_priv_shift_1   = 8U,  /**< Shift for the 2nd byte. */
  k_c6_fwver_priv_shift_2   = 16U, /**< Shift for the 3rd byte. */
  k_c6_fwver_priv_shift_3   = 24U, /**< Shift for the 4th byte. */
} c6_fwver_priv_ver_t;

/**
 * @var s_c6_fwver_priv_version
 * @brief Version word the boot INIT event announced, or zero.
 * @details Packed the same way ``ESP_HOSTED_VERSION_VAL`` packs it, so it can
 * be compared with the RPC answer without either side being re-derived.
 * @note Written only from the pump thread, inside ::c6_fwver_priv_consume.
 * @warning Zero means "no INIT event carrying the tag was seen", not version
 *          0.0.0; the co-processor queues the event once per boot.
 * @since 0.1.0
 */
static uint32_t s_c6_fwver_priv_version;

/**
 * @brief Append one one-byte-valued TLV at a cursor.
 * @param[out] out Buffer being filled; must be non-null.
 * @param[in] at Offset to write at.
 * @param[in] tag TLV tag.
 * @param[in] value TLV value.
 * @return The offset just past the TLV written.
 * @pre At least ::k_c6_fwver_priv_tlv_stride bytes are writable at
 *      ``out + at``.
 * @pre The caller has already checked the buffer capacity.
 * @post Exactly ::k_c6_fwver_priv_tlv_stride bytes were written.
 * @post The returned offset is @p at plus that stride.
 * @note Every tag this host emits has a one-byte value, which is why the
 *       length is not a parameter.
 * @since 0.1.0
 */
static uint16_t c6_fwver_priv_put_tlv(uint8_t* out, uint16_t at, uint8_t tag, uint8_t value)
{
  out[at + (uint16_t)k_c6_fwver_priv_tlv_tag]   = tag;
  out[at + (uint16_t)k_c6_fwver_priv_tlv_size]  = (uint8_t)k_c6_fwver_priv_tlv_len;
  out[at + (uint16_t)k_c6_fwver_priv_tlv_value] = value;
  return (uint16_t)(at + (uint16_t)k_c6_fwver_priv_tlv_stride);
}

ra8_err_t c6_fwver_priv_host_caps(uint8_t* out, uint16_t cap, uint16_t* out_len)
{
  if ((out == nullptr) || (out_len == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  const uint16_t tlv_bytes =
    (uint16_t)((uint16_t)k_c6_fwver_priv_tag_count * (uint16_t)k_c6_fwver_priv_tlv_stride);
  const uint16_t total = (uint16_t)(tlv_bytes + (uint16_t)k_c6_fwver_priv_evt_hdr);
  if (cap < total) {
    *out_len = 0U;
    return k_ra8_err_invalid_size;
  }

  out[k_c6_fwver_priv_evt_type] = (uint8_t)ESP_PRIV_EVENT_INIT;
  out[k_c6_fwver_priv_evt_len]  = (uint8_t)tlv_bytes;

  uint16_t at = (uint16_t)k_c6_fwver_priv_evt_hdr;
  at =
    c6_fwver_priv_put_tlv(out, at, (uint8_t)HOST_CAPABILITIES, (uint8_t)k_c6_fwver_priv_host_cap);
  at = c6_fwver_priv_put_tlv(out,
                             at,
                             (uint8_t)RCVD_ESP_FIRMWARE_CHIP_ID,
                             (uint8_t)ESP_PRIV_FIRMWARE_CHIP_ESP32C6);
  at = c6_fwver_priv_put_tlv(out, at, (uint8_t)SLV_CONFIG_TEST_RAW_TP, (uint8_t)H_TEST_RAW_TP_DIR);
  at = c6_fwver_priv_put_tlv(out,
                             at,
                             (uint8_t)SLV_CONFIG_THROTTLE_HIGH_THRESHOLD,
                             (uint8_t)H_WIFI_TX_DATA_THROTTLE_HIGH_THRESHOLD);
  at = c6_fwver_priv_put_tlv(out,
                             at,
                             (uint8_t)SLV_CONFIG_THROTTLE_LOW_THRESHOLD,
                             (uint8_t)H_WIFI_TX_DATA_THROTTLE_LOW_THRESHOLD);

  *out_len = at;
  return k_ra8_ok;
}

/**
 * @brief Decode and report one TLV of an INIT event.
 * @param[in] tag TLV tag.
 * @param[in] value TLV value bytes; must be non-null.
 * @param[in] len TLV value length in bytes.
 * @return Nothing.
 * @pre @p len bytes are readable at @p value.
 * @pre The console is up.
 * @post Recognised tags were printed; unrecognised ones were named with
 *       their tag number rather than dropped silently.
 * @post ::s_c6_fwver_priv_version is updated only by the version tag.
 * @note An unrecognised tag is printed, not ignored: a co-processor that
 *       announces something this host does not know about is exactly the
 *       thing a bring-up wants to see.
 * @since 0.1.0
 */
static void c6_fwver_priv_tlv(uint8_t tag, const uint8_t* value, uint8_t len)
{
  c6_fwver_puts("c6_fwver: priv tlv tag=0x");
  c6_fwver_put_hex((uint32_t)tag, (uint8_t)k_c6_fwver_hex_byte);

  if ((tag == (uint8_t)ESP_PRIV_FIRMWARE_VERSION) && (len == (uint8_t)k_c6_fwver_priv_ver_bytes)) {
    s_c6_fwver_priv_version =
      (uint32_t)value[k_c6_fwver_priv_ver_b0] |
      ((uint32_t)value[k_c6_fwver_priv_ver_b1] << (uint32_t)k_c6_fwver_priv_shift_1) |
      ((uint32_t)value[k_c6_fwver_priv_ver_b2] << (uint32_t)k_c6_fwver_priv_shift_2) |
      ((uint32_t)value[k_c6_fwver_priv_ver_b3] << (uint32_t)k_c6_fwver_priv_shift_3);
    c6_fwver_puts(" firmware_version=");
    c6_fwver_put_u32(ESP_HOSTED_VERSION_MAJOR(s_c6_fwver_priv_version));
    c6_fwver_puts(".");
    c6_fwver_put_u32(ESP_HOSTED_VERSION_MINOR(s_c6_fwver_priv_version));
    c6_fwver_puts(".");
    c6_fwver_put_u32(ESP_HOSTED_VERSION_PATCH(s_c6_fwver_priv_version));
  } else if (len == (uint8_t)k_c6_fwver_priv_tlv_len) {
    c6_fwver_puts(" value=0x");
    c6_fwver_put_hex((uint32_t)value[0], (uint8_t)k_c6_fwver_hex_byte);
  } else {
    c6_fwver_puts(" len=");
    c6_fwver_put_u32((uint32_t)len);
  }
  c6_fwver_puts("\r\n");
}

bool c6_fwver_priv_consume(uint8_t if_type, uint8_t if_num, const uint8_t* payload, uint16_t len)
{
  (void)if_num;
  if ((if_type != (uint8_t)ESP_PRIV_IF) || (payload == nullptr) ||
      (len < (uint16_t)k_c6_fwver_priv_evt_hdr)) {
    return false;
  }

  const uint8_t  event_type = payload[k_c6_fwver_priv_evt_type];
  const uint16_t event_len  = (uint16_t)payload[k_c6_fwver_priv_evt_len];
  c6_fwver_puts("c6_fwver: priv event type=0x");
  c6_fwver_put_hex((uint32_t)event_type, (uint8_t)k_c6_fwver_hex_byte);
  c6_fwver_puts(" tlv_bytes=");
  c6_fwver_put_u32((uint32_t)event_len);
  c6_fwver_puts("\r\n");

  if ((event_type != (uint8_t)ESP_PRIV_EVENT_INIT) ||
      (event_len > (uint16_t)(len - (uint16_t)k_c6_fwver_priv_evt_hdr))) {
    return false;
  }

  uint16_t at = (uint16_t)k_c6_fwver_priv_evt_hdr;
  for (uint16_t seen = 0U; seen < event_len; seen++) {
    if ((uint16_t)(at + (uint16_t)k_c6_fwver_priv_tlv_value) > len) {
      break;
    }
    const uint8_t  tag     = payload[at + (uint16_t)k_c6_fwver_priv_tlv_tag];
    const uint8_t  tag_len = payload[at + (uint16_t)k_c6_fwver_priv_tlv_size];
    const uint16_t next    = (uint16_t)(at + (uint16_t)k_c6_fwver_priv_tlv_value + tag_len);
    if (next > len) {
      break;
    }
    c6_fwver_priv_tlv(tag, &payload[at + (uint16_t)k_c6_fwver_priv_tlv_value], tag_len);
    at = next;
    if (at >= (uint16_t)(event_len + (uint16_t)k_c6_fwver_priv_evt_hdr)) {
      break;
    }
  }
  return false;
}

uint32_t c6_fwver_priv_init_version(void)
{
  return s_c6_fwver_priv_version;
}
