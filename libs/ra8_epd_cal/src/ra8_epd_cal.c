/**
 * @file ra8_epd_cal.c
 * @brief Per-device e-paper VCOM calibration -- record codec and resolver
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details
 * Implements ra8_epd_cal.h. Two responsibilities, deliberately kept apart:
 *
 *  - a self-describing, CRC-protected record codec (the storage format is
 *    unsigned and lives outside the DFU-signed image, so it carries its
 *    own integrity check), and
 *  - a resolver that walks controller -> per-device record -> operator
 *    value and **fails rather than guesses**.
 *
 * No HAL header is included: the controller and the non-volatile store are
 * reached only through the injected seams in the config struct, which is
 * what lets every branch below run on the host.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_epd_cal.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"

/**
 * @var s_tag
 * @brief Logging tag used by every error path in this TU.
 */
static const char* const s_tag = "EPD_CAL";

/**
 * @enum ra8_epd_cal_crc_t
 * @brief IEEE 802.3 CRC-32 parameters.
 *
 * @details
 * Bit-banged rather than table-driven so the module has no dependency on a
 * CRC library and no 1 KiB table in a build that only ever checks 32 bytes
 * once per boot. Mirrors the constants in ``ra8_touch_cal.c`` so both
 * calibration records use the same polynomial.
 */
typedef enum : uint32_t {
  k_ra8_epd_cal_crc_init = 0xFFFFFFFFU, /**< CRC seed / final XOR.     */
  k_ra8_epd_cal_crc_poly = 0xEDB88320U, /**< Reversed IEEE 802.3 poly. */
} ra8_epd_cal_crc_t;

/**
 * @enum ra8_epd_cal_bits_t
 * @brief Byte / word packing constants (no magic numbers).
 */
typedef enum : uint8_t {
  k_ra8_epd_cal_bits_per_byte = 8U,    /**< CRC inner-loop bound.     */
  k_ra8_epd_cal_byte_shift    = 8U,    /**< Bits per byte.            */
  k_ra8_epd_cal_byte_mask     = 0xFFU, /**< Low-byte extraction mask. */
  k_ra8_epd_cal_off_b0        = 0U,    /**< Little-endian byte 0.     */
  k_ra8_epd_cal_off_b1        = 1U,    /**< Little-endian byte 1.     */
  k_ra8_epd_cal_off_b2        = 2U,    /**< Little-endian byte 2.     */
  k_ra8_epd_cal_off_b3        = 3U,    /**< Little-endian byte 3.     */
} ra8_epd_cal_bits_t;

/* ===========================================================================
 * Local helpers
 * ===========================================================================
 */

/**
 * @brief Compute the IEEE 802.3 CRC-32 of a byte span.
 *
 * @details
 * Bit-banged reflected-polynomial form; no lookup table.
 *
 * @param[in] data Bytes to checksum; non-NULL.
 * @param[in] len  Number of bytes.
 *
 * @return The CRC-32 of ``data[0 .. len)``.
 *
 * @pre  ``data`` holds at least ``len`` readable bytes.
 * @post No state mutated.
 */
RA8_INTERNAL
[[nodiscard]] static uint32_t internal_ra8_epd_cal_crc32(const uint8_t* data, size_t len)
{
  uint32_t crc = (uint32_t)k_ra8_epd_cal_crc_init;
  for (size_t i = 0U; i < len; i++) {
    crc ^= (uint32_t)data[i];
    for (uint8_t bit = 0U; bit < (uint8_t)k_ra8_epd_cal_bits_per_byte; bit++) {
      const uint32_t mask = (uint32_t)0U - (crc & 1U);
      crc                 = (crc >> 1U) ^ ((uint32_t)k_ra8_epd_cal_crc_poly & mask);
    }
  }
  return crc ^ (uint32_t)k_ra8_epd_cal_crc_init;
}

/**
 * @brief Store a little-endian 16-bit value.
 *
 * @param[out] dst Destination for two bytes; non-NULL.
 * @param[in]  val Value to pack.
 *
 * @pre  ``dst`` has room for two bytes.
 * @post ``dst`` holds ``val`` little-endian.
 */
RA8_INTERNAL
static void internal_ra8_epd_cal_pack_le16(uint8_t* dst, uint16_t val)
{
  dst[k_ra8_epd_cal_off_b0] = (uint8_t)(val & (uint16_t)k_ra8_epd_cal_byte_mask);
  dst[k_ra8_epd_cal_off_b1] =
    (uint8_t)((val >> (uint16_t)k_ra8_epd_cal_byte_shift) & (uint16_t)k_ra8_epd_cal_byte_mask);
}

/**
 * @brief Load a little-endian 16-bit value.
 *
 * @param[in] src Two source bytes; non-NULL.
 * @return The unpacked value.
 *
 * @pre  ``src`` holds at least two readable bytes.
 * @post No state mutated.
 */
RA8_INTERNAL
[[nodiscard]] static uint16_t internal_ra8_epd_cal_unpack_le16(const uint8_t* src)
{
  return (uint16_t)((uint16_t)src[k_ra8_epd_cal_off_b0] |
                    ((uint16_t)src[k_ra8_epd_cal_off_b1] << (uint16_t)k_ra8_epd_cal_byte_shift));
}

/**
 * @brief Store a little-endian 32-bit value.
 *
 * @param[out] dst Destination for four bytes; non-NULL.
 * @param[in]  val Value to pack.
 *
 * @pre  ``dst`` has room for four bytes.
 * @post ``dst`` holds ``val`` little-endian.
 */
RA8_INTERNAL
static void internal_ra8_epd_cal_pack_le32(uint8_t* dst, uint32_t val)
{
  dst[k_ra8_epd_cal_off_b0] = (uint8_t)(val & (uint32_t)k_ra8_epd_cal_byte_mask);
  dst[k_ra8_epd_cal_off_b1] =
    (uint8_t)((val >> (uint32_t)k_ra8_epd_cal_byte_shift) & (uint32_t)k_ra8_epd_cal_byte_mask);
  dst[k_ra8_epd_cal_off_b2] = (uint8_t)((val >> ((uint32_t)k_ra8_epd_cal_byte_shift * 2U)) &
                                        (uint32_t)k_ra8_epd_cal_byte_mask);
  dst[k_ra8_epd_cal_off_b3] = (uint8_t)((val >> ((uint32_t)k_ra8_epd_cal_byte_shift * 3U)) &
                                        (uint32_t)k_ra8_epd_cal_byte_mask);
}

/**
 * @brief Load a little-endian 32-bit value.
 *
 * @param[in] src Four source bytes; non-NULL.
 * @return The unpacked value.
 *
 * @pre  ``src`` holds at least four readable bytes.
 * @post No state mutated.
 */
RA8_INTERNAL
[[nodiscard]] static uint32_t internal_ra8_epd_cal_unpack_le32(const uint8_t* src)
{
  return (uint32_t)src[k_ra8_epd_cal_off_b0] |
         ((uint32_t)src[k_ra8_epd_cal_off_b1] << (uint32_t)k_ra8_epd_cal_byte_shift) |
         ((uint32_t)src[k_ra8_epd_cal_off_b2] << ((uint32_t)k_ra8_epd_cal_byte_shift * 2U)) |
         ((uint32_t)src[k_ra8_epd_cal_off_b3] << ((uint32_t)k_ra8_epd_cal_byte_shift * 3U));
}

/**
 * @brief Report whether a serialised record carries the 'EVCM' magic.
 *
 * @param[in] src Serialised record; non-NULL, >= blob size.
 * @return ``true`` when all four magic bytes match.
 *
 * @pre  ``src`` holds at least ::k_ra8_epd_cal_blob_size bytes.
 * @post No state mutated.
 */
RA8_INTERNAL
[[nodiscard]] static bool internal_ra8_epd_cal_magic_ok(const uint8_t* src)
{
  const uint8_t* m = &src[k_ra8_epd_cal_off_magic];
  return (m[k_ra8_epd_cal_off_b0] == (uint8_t)k_ra8_epd_cal_magic_b0) &&
         (m[k_ra8_epd_cal_off_b1] == (uint8_t)k_ra8_epd_cal_magic_b1) &&
         (m[k_ra8_epd_cal_off_b2] == (uint8_t)k_ra8_epd_cal_magic_b2) &&
         (m[k_ra8_epd_cal_off_b3] == (uint8_t)k_ra8_epd_cal_magic_b3);
}

/**
 * @brief Read and validate the per-device record through the store seam.
 *
 * @details
 * Any failure -- unbound seam, read fault, blank flash, corrupt record --
 * reports "no record" so the resolver falls through to the next source.
 * The distinction between the failure modes is logged, not returned,
 * because the resolver's behaviour is the same for all of them.
 *
 * @param[in]  store   Store seam; non-NULL.
 * @param[out] out_rec Receives the decoded record; non-NULL.
 *
 * @return ``k_ra8_ok`` when a valid record was decoded, else an error.
 *
 * @pre  ``store`` and ``out_rec`` are readable / writable.
 * @post On success ``out_rec->vcom_mv`` is non-zero.
 */
RA8_INTERNAL
[[nodiscard]] static ra8_err_t internal_ra8_epd_cal_read_record(const ra8_epd_cal_store_t* store,
                                                                ra8_epd_cal_record_t*      out_rec)
{
  if (store->read == nullptr) {
    return k_ra8_err_not_supported;
  }
  uint8_t         blob[k_ra8_epd_cal_blob_size] = {};
  const ra8_err_t rerr = store->read(store->ctx, blob, (size_t)k_ra8_epd_cal_blob_size);
  if (rerr != k_ra8_ok) {
    ra8_log_warn(s_tag, "calibration record read failed");
    return rerr;
  }
  const ra8_err_t derr = ra8_epd_cal_deserialize(blob, (size_t)k_ra8_epd_cal_blob_size, out_rec);
  if (derr == k_ra8_err_crc_mismatch) {
    /* Magic present but the body does not check out: the record was
     * written and then damaged. Worth shouting about -- it means durable
     * calibration was lost, not merely never provisioned. */
    ra8_log_error(s_tag, "calibration record CRC mismatch -- record corrupt");
  }
  return derr;
}

/* ===========================================================================
 * Public API -- record codec
 * ===========================================================================
 */

[[nodiscard]] bool ra8_epd_cal_vcom_in_range(uint16_t mv, const ra8_epd_cal_limits_mv_t* limits)
{
  if (limits == nullptr) {
    return false;
  }
  if ((mv < limits->min_mv) || (mv > limits->max_mv)) {
    return false;
  }
  return true;
}

[[nodiscard]] ra8_err_t
ra8_epd_cal_serialize(const ra8_epd_cal_record_t* rec, uint8_t* dst, size_t dst_size)
{
  RA8_CHECK_NULL_PTR(rec, s_tag, "serialize: rec null");
  RA8_CHECK_NULL_PTR(dst, s_tag, "serialize: dst null");
  if (dst_size < (size_t)k_ra8_epd_cal_blob_size) {
    return k_ra8_err_invalid_size;
  }
  if (rec->vcom_mv == 0U) {
    ra8_log_error(s_tag, "serialize: refusing a zero VCOM");
    return k_ra8_err_invalid_arg;
  }

  for (size_t i = 0U; i < (size_t)k_ra8_epd_cal_blob_size; i++) {
    dst[i] = 0U;
  }
  dst[(size_t)k_ra8_epd_cal_off_magic + k_ra8_epd_cal_off_b0] = (uint8_t)k_ra8_epd_cal_magic_b0;
  dst[(size_t)k_ra8_epd_cal_off_magic + k_ra8_epd_cal_off_b1] = (uint8_t)k_ra8_epd_cal_magic_b1;
  dst[(size_t)k_ra8_epd_cal_off_magic + k_ra8_epd_cal_off_b2] = (uint8_t)k_ra8_epd_cal_magic_b2;
  dst[(size_t)k_ra8_epd_cal_off_magic + k_ra8_epd_cal_off_b3] = (uint8_t)k_ra8_epd_cal_magic_b3;
  dst[(size_t)k_ra8_epd_cal_off_version] = (uint8_t)k_ra8_epd_cal_schema_version;
  internal_ra8_epd_cal_pack_le16(&dst[(size_t)k_ra8_epd_cal_off_payload_len],
                                 (uint16_t)k_ra8_epd_cal_payload_len);
  internal_ra8_epd_cal_pack_le16(&dst[(size_t)k_ra8_epd_cal_off_vcom_mv], rec->vcom_mv);

  const uint32_t crc = internal_ra8_epd_cal_crc32(dst, (size_t)k_ra8_epd_cal_off_crc32);
  internal_ra8_epd_cal_pack_le32(&dst[(size_t)k_ra8_epd_cal_off_crc32], crc);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t
ra8_epd_cal_deserialize(const uint8_t* src, size_t src_size, ra8_epd_cal_record_t* out_rec)
{
  RA8_CHECK_NULL_PTR(src, s_tag, "deserialize: src null");
  RA8_CHECK_NULL_PTR(out_rec, s_tag, "deserialize: out null");
  if (src_size < (size_t)k_ra8_epd_cal_blob_size) {
    return k_ra8_err_invalid_size;
  }
  if (!internal_ra8_epd_cal_magic_ok(src)) {
    /* Blank / never-written storage lands here. Not an error worth logging
     * on every boot of an unprovisioned device. */
    return k_ra8_err_not_found;
  }
  const uint8_t version = src[(size_t)k_ra8_epd_cal_off_version];
  if (version > (uint8_t)k_ra8_epd_cal_schema_version) {
    ra8_log_error(s_tag, "deserialize: record schema newer than this build");
    return k_ra8_err_not_supported;
  }
  const uint16_t payload_len =
    internal_ra8_epd_cal_unpack_le16(&src[(size_t)k_ra8_epd_cal_off_payload_len]);
  if (payload_len != (uint16_t)k_ra8_epd_cal_payload_len) {
    return k_ra8_err_validation_failed;
  }
  const uint32_t want = internal_ra8_epd_cal_unpack_le32(&src[(size_t)k_ra8_epd_cal_off_crc32]);
  const uint32_t have = internal_ra8_epd_cal_crc32(src, (size_t)k_ra8_epd_cal_off_crc32);
  if (want != have) {
    return k_ra8_err_crc_mismatch;
  }
  const uint16_t vcom = internal_ra8_epd_cal_unpack_le16(&src[(size_t)k_ra8_epd_cal_off_vcom_mv]);
  if (vcom == 0U) {
    /* A CRC-valid record cannot legitimately hold zero -- serialize
     * refuses to write one -- so this is a writer from another schema. */
    return k_ra8_err_validation_failed;
  }
  out_rec->vcom_mv        = vcom;
  out_rec->schema_version = version;
  return k_ra8_ok;
}

/* ===========================================================================
 * Public API -- resolution
 * ===========================================================================
 */

[[nodiscard]] ra8_err_t ra8_epd_cal_resolve(const ra8_epd_cal_cfg_t* cfg,
                                            ra8_epd_cal_result_t*    out_result)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "resolve: cfg null");
  RA8_CHECK_NULL_PTR(out_result, s_tag, "resolve: out null");

  *out_result = (ra8_epd_cal_result_t){.vcom_mv = 0U, .source = k_ra8_epd_cal_src_none};
  if ((cfg->limits.min_mv == 0U) || (cfg->limits.min_mv > cfg->limits.max_mv)) {
    ra8_log_error(s_tag, "resolve: VCOM limits are zero or inverted");
    return k_ra8_err_invalid_arg;
  }

  /* Source 1: whatever the controller's own driver board persisted. */
  if (cfg->panel.get != nullptr) {
    uint16_t panel_mv = 0U;
    if (cfg->panel.get(cfg->panel.ctx, &panel_mv) == k_ra8_ok) {
      if (ra8_epd_cal_vcom_in_range(panel_mv, &cfg->limits)) {
        out_result->vcom_mv = panel_mv;
        out_result->source  = k_ra8_epd_cal_src_panel;
        return k_ra8_ok;
      }
      ra8_log_warn(s_tag, "controller VCOM out of range -- ignored");
    }
  }

  /* Source 2: the per-device record in storage outside the DFU banks. */
  ra8_epd_cal_record_t rec = {};
  if (internal_ra8_epd_cal_read_record(&cfg->store, &rec) == k_ra8_ok) {
    if (ra8_epd_cal_vcom_in_range(rec.vcom_mv, &cfg->limits)) {
      out_result->vcom_mv = rec.vcom_mv;
      out_result->source  = k_ra8_epd_cal_src_record;
      return k_ra8_ok;
    }
    ra8_log_warn(s_tag, "stored VCOM out of range -- ignored");
  }

  /* Source 3: an explicit value the operator supplied for this boot. */
  if (cfg->has_provisioned && ra8_epd_cal_vcom_in_range(cfg->provisioned_mv, &cfg->limits)) {
    out_result->vcom_mv = cfg->provisioned_mv;
    out_result->source  = k_ra8_epd_cal_src_provisioned;
    return k_ra8_ok;
  }

  /* Nothing trusted. Fail safe: the caller must leave the panel dark
   * rather than drive it at an invented bias. */
  ra8_log_error(s_tag, "no trusted VCOM -- refusing to drive the panel");
  return k_ra8_err_not_found;
}

[[nodiscard]] ra8_err_t ra8_epd_cal_apply(const ra8_epd_cal_cfg_t*    cfg,
                                          const ra8_epd_cal_result_t* result)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "apply: cfg null");
  RA8_CHECK_NULL_PTR(result, s_tag, "apply: result null");
  if (result->source == k_ra8_epd_cal_src_none) {
    return k_ra8_err_invalid_state;
  }
  if (cfg->panel.set == nullptr) {
    return k_ra8_err_not_supported;
  }
  if (!ra8_epd_cal_vcom_in_range(result->vcom_mv, &cfg->limits)) {
    ra8_log_error(s_tag, "apply: VCOM out of range at the point of use");
    return k_ra8_err_range_check_failed;
  }
  return cfg->panel.set(cfg->panel.ctx, result->vcom_mv);
}

[[nodiscard]] ra8_err_t ra8_epd_cal_provision(const ra8_epd_cal_cfg_t* cfg, uint16_t vcom_mv)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "provision: cfg null");
  if (cfg->store.write == nullptr) {
    return k_ra8_err_not_supported;
  }
  if (!ra8_epd_cal_vcom_in_range(vcom_mv, &cfg->limits)) {
    ra8_log_error(s_tag, "provision: VCOM outside the panel's window");
    return k_ra8_err_range_check_failed;
  }
  const ra8_epd_cal_record_t rec = {.vcom_mv        = vcom_mv,
                                    .schema_version = (uint8_t)k_ra8_epd_cal_schema_version};
  uint8_t                    blob[k_ra8_epd_cal_blob_size] = {};
  const ra8_err_t serr = ra8_epd_cal_serialize(&rec, blob, (size_t)k_ra8_epd_cal_blob_size);
  if (serr != k_ra8_ok) {
    return serr;
  }
  return cfg->store.write(cfg->store.ctx, blob, (size_t)k_ra8_epd_cal_blob_size);
}
