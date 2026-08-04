/**
 * @file ra8_devcfg.c
 * @brief Per-device configuration record -- codec, two-copy resolver, commit.
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details
 * Implements the hardware-agnostic half of ``ra8_devcfg.h``: a self-describing,
 * CRC-protected record codec; a boot-time resolver that walks the two copies
 * and picks the surviving / newest one or reports UNPROVISIONED; and a
 * header-last commit. No HAL header is included -- the durable medium is reached
 * only through the injected ::ra8_devcfg_store_t seam, which is what lets every
 * branch below run on the host. The extra-MRAM backing lives in the companion
 * translation unit ``ra8_devcfg_store_extra_mram.c``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_devcfg.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"

/**
 * @var s_tag
 * @brief Logging tag used by every diagnostic path in this TU.
 * @details File-private component identifier for ``ra8_log_*`` / ``RA8_CHECK_*``.
 * @warning Do not modify.
 * @since 0.1.0
 */
static const char* const s_tag = "DEVCFG";

/**
 * @enum ra8_devcfg_crc_t
 * @brief IEEE 802.3 CRC-32 parameters.
 *
 * @details
 * Bit-banged rather than table-driven so the module carries no CRC table for a
 * checksum it runs over 96 bytes a handful of times per boot. The polynomial
 * matches ``ra8_epd_cal.c`` and ``ra8_touch_cal.c`` so every calibration record
 * in the tree shares one CRC.
 */
typedef enum : uint32_t {
  k_ra8_devcfg_crc_init = 0xFFFFFFFFU, /**< CRC seed / final XOR.     */
  k_ra8_devcfg_crc_poly = 0xEDB88320U, /**< Reversed IEEE 802.3 poly. */
} ra8_devcfg_crc_t;

/**
 * @enum ra8_devcfg_bits_t
 * @brief Byte / word packing constants (no magic numbers).
 */
typedef enum : uint8_t {
  k_ra8_devcfg_bits_per_byte = 8U,    /**< CRC inner-loop bound.     */
  k_ra8_devcfg_byte_shift    = 8U,    /**< Bits per byte.            */
  k_ra8_devcfg_byte_mask     = 0xFFU, /**< Low-byte extraction mask. */
  k_ra8_devcfg_off_b0        = 0U,    /**< Little-endian byte 0.     */
  k_ra8_devcfg_off_b1        = 1U,    /**< Little-endian byte 1.     */
  k_ra8_devcfg_off_b2        = 2U,    /**< Little-endian byte 2.     */
  k_ra8_devcfg_off_b3        = 3U,    /**< Little-endian byte 3.     */
} ra8_devcfg_bits_t;

/**
 * @enum ra8_devcfg_state_t
 * @brief Module cache state established by ::ra8_devcfg_load.
 *
 * @dot
 * digraph devcfg_state {
 *   rankdir=LR;
 *   node [shape=circle];
 *   unloaded [label="unloaded"];
 *   loaded [label="loaded"];
 *   unprov [label="unprovisioned"];
 *   unloaded -> loaded  [label="load: a copy valid"];
 *   unloaded -> unprov  [label="load: neither valid"];
 *   loaded  -> loaded   [label="load: a copy valid"];
 *   loaded  -> unprov   [label="load: neither valid"];
 *   unprov  -> loaded   [label="load: a copy valid"];
 *   loaded  -> unloaded [label="reset"];
 *   unprov  -> unloaded [label="reset"];
 * }
 * @enddot
 */
typedef enum : uint8_t {
  k_ra8_devcfg_state_unloaded      = 0U, /**< No load attempted this boot.      */
  k_ra8_devcfg_state_loaded        = 1U, /**< A valid record is cached.         */
  k_ra8_devcfg_state_unprovisioned = 2U, /**< Load ran; neither copy was valid. */
} ra8_devcfg_state_t;

/**
 * @var s_state
 * @brief Current cache state.
 * @warning Boot / provisioning path only; not thread-safe.
 * @since 0.1.0
 */
static ra8_devcfg_state_t s_state = k_ra8_devcfg_state_unloaded;

/**
 * @var s_record
 * @brief The resolved record cached by the last successful ::ra8_devcfg_load.
 * @warning Valid only while ::s_state is ::k_ra8_devcfg_state_loaded.
 * @since 0.1.0
 */
static ra8_devcfg_record_t s_record = {};

/* ===========================================================================
 * Local helpers -- byte codec
 * ===========================================================================
 */

/**
 * @brief Compute the IEEE 802.3 CRC-32 of a byte span.
 *
 * @details Bit-banged reflected-polynomial form; no lookup table. Used for the
 *          record body checksum stored in the header.
 *
 * @param[in] data Bytes to checksum; non-NULL.
 * @param[in] len  Number of bytes.
 *
 * @return The CRC-32 of ``data[0 .. len)``.
 * @retval 0xFFFFFFFF..0x00000000 The reflected CRC-32 over the span.
 *
 * @pre  ``data`` holds at least ``len`` readable bytes.
 * @pre  ``len`` is the exact span length to checksum.
 * @post No state mutated.
 * @post The result depends only on ``data`` and ``len``.
 *
 * @note Thread-safe (pure; no statics).
 * @since 0.1.0
 */
RA8_INTERNAL
[[nodiscard]] static uint32_t internal_devcfg_crc32(const uint8_t* data, uint32_t len)
{
  uint32_t crc = (uint32_t)k_ra8_devcfg_crc_init;
  for (uint32_t i = 0U; i < len; i++) {
    crc ^= (uint32_t)data[i];
    for (uint8_t bit = 0U; bit < (uint8_t)k_ra8_devcfg_bits_per_byte; bit++) {
      const uint32_t mask = (uint32_t)0U - (crc & 1U);
      crc                 = (crc >> 1U) ^ ((uint32_t)k_ra8_devcfg_crc_poly & mask);
    }
  }
  return crc ^ (uint32_t)k_ra8_devcfg_crc_init;
}

/**
 * @brief Store a little-endian 16-bit value into a blob.
 *
 * @details The record is serialised byte-at-a-time in an explicit byte order so
 *          a blob written on the host deserialises identically on the target.
 *
 * @param[out] dst Destination for two bytes; non-NULL.
 * @param[in]  val Value to pack.
 *
 * @pre  ``dst`` has room for two bytes.
 * @pre  ``dst`` points inside the serialisation blob.
 * @post ``dst`` holds ``val`` little-endian.
 * @post Exactly two bytes are written.
 *
 * @note Not thread-safe; called only from the serialiser.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_devcfg_pack_le16(uint8_t* dst, uint16_t val)
{
  dst[k_ra8_devcfg_off_b0] = (uint8_t)(val & (uint16_t)k_ra8_devcfg_byte_mask);
  dst[k_ra8_devcfg_off_b1] =
    (uint8_t)((val >> (uint16_t)k_ra8_devcfg_byte_shift) & (uint16_t)k_ra8_devcfg_byte_mask);
}

/**
 * @brief Store a little-endian 32-bit value into a blob.
 *
 * @details The 32-bit counterpart of ::internal_devcfg_pack_le16, used for the
 *          magic word, sequence, CRC and flags. Same explicit byte order.
 *
 * @param[out] dst Destination for four bytes; non-NULL.
 * @param[in]  val Value to pack.
 *
 * @pre  ``dst`` has room for four bytes.
 * @pre  ``dst`` points inside the serialisation blob.
 * @post ``dst`` holds ``val`` little-endian.
 * @post Exactly four bytes are written.
 *
 * @note Not thread-safe; called only from the serialiser.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_devcfg_pack_le32(uint8_t* dst, uint32_t val)
{
  const uint32_t sh        = (uint32_t)k_ra8_devcfg_byte_shift;
  dst[k_ra8_devcfg_off_b0] = (uint8_t)(val & (uint32_t)k_ra8_devcfg_byte_mask);
  dst[k_ra8_devcfg_off_b1] = (uint8_t)((val >> sh) & (uint32_t)k_ra8_devcfg_byte_mask);
  dst[k_ra8_devcfg_off_b2] = (uint8_t)((val >> (sh * 2U)) & (uint32_t)k_ra8_devcfg_byte_mask);
  dst[k_ra8_devcfg_off_b3] = (uint8_t)((val >> (sh * 3U)) & (uint32_t)k_ra8_devcfg_byte_mask);
}

/**
 * @brief Load a little-endian 16-bit value from a blob.
 *
 * @details Inverse of ::internal_devcfg_pack_le16; reads the two bytes in the
 *          explicit record byte order.
 *
 * @param[in] src Two source bytes; non-NULL.
 *
 * @return The unpacked value.
 * @retval 0x0000..0xFFFF The little-endian value at ``src``.
 *
 * @pre  ``src`` holds at least two readable bytes.
 * @pre  ``src`` points inside a serialised record.
 * @post No state mutated.
 * @post The result depends only on ``src[0..1]``.
 *
 * @note Thread-safe (pure; no statics).
 * @since 0.1.0
 */
RA8_INTERNAL
[[nodiscard]] static uint16_t internal_devcfg_unpack_le16(const uint8_t* src)
{
  return (uint16_t)((uint16_t)src[k_ra8_devcfg_off_b0] |
                    ((uint16_t)src[k_ra8_devcfg_off_b1] << (uint16_t)k_ra8_devcfg_byte_shift));
}

/**
 * @brief Load a little-endian 32-bit value from a blob.
 *
 * @details Inverse of ::internal_devcfg_pack_le32; reads the four bytes in the
 *          explicit record byte order.
 *
 * @param[in] src Four source bytes; non-NULL.
 *
 * @return The unpacked value.
 * @retval 0x00000000..0xFFFFFFFF The little-endian value at ``src``.
 *
 * @pre  ``src`` holds at least four readable bytes.
 * @pre  ``src`` points inside a serialised record.
 * @post No state mutated.
 * @post The result depends only on ``src[0..3]``.
 *
 * @note Thread-safe (pure; no statics).
 * @since 0.1.0
 */
RA8_INTERNAL
[[nodiscard]] static uint32_t internal_devcfg_unpack_le32(const uint8_t* src)
{
  const uint32_t sh = (uint32_t)k_ra8_devcfg_byte_shift;
  return (uint32_t)src[k_ra8_devcfg_off_b0] | ((uint32_t)src[k_ra8_devcfg_off_b1] << sh) |
         ((uint32_t)src[k_ra8_devcfg_off_b2] << (sh * 2U)) |
         ((uint32_t)src[k_ra8_devcfg_off_b3] << (sh * 3U));
}

/* ===========================================================================
 * Local helpers -- record codec
 * ===========================================================================
 */

/**
 * @brief Serialise a record into a ::k_ra8_devcfg_record_len byte buffer.
 *
 * @details
 * Writes the body at its field offsets, then the header (magic, schema,
 * record_len, ``seq``, CRC over the body, flags). The CRC covers only the body
 * span, matching the header-last commit discipline.
 *
 * @param[in]  rec Record whose body and flags are serialised; non-NULL.
 * @param[in]  seq Sequence number to stamp into the header.
 * @param[out] buf Destination of ::k_ra8_devcfg_record_len bytes; non-NULL.
 *
 * @pre  ``buf`` has room for ::k_ra8_devcfg_record_len bytes.
 * @pre  ``rec`` points at a fully-populated record.
 * @post ``buf`` holds a CRC-valid serialised record with sequence ``seq``.
 * @post The header CRC matches the serialised body span.
 *
 * @note Not thread-safe; called only from the commit path.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_devcfg_serialize(const ra8_devcfg_record_t* rec, uint32_t seq, uint8_t* buf)
{
  (void)memset(buf, 0, (size_t)k_ra8_devcfg_record_len);
  const ra8_devcfg_body_t* b = &rec->body;
  (void)memcpy(&buf[k_ra8_devcfg_off_serial], b->serial, (size_t)k_ra8_devcfg_serial_len);
  (void)memcpy(&buf[k_ra8_devcfg_off_panel_serial],
               b->panel_serial,
               (size_t)k_ra8_devcfg_panel_serial_len);
  (void)memcpy(&buf[k_ra8_devcfg_off_panel_lut],
               b->panel_lut_id,
               (size_t)k_ra8_devcfg_panel_lut_len);
  (void)memcpy(&buf[k_ra8_devcfg_off_touch_cal], b->touch_cal, (size_t)k_ra8_devcfg_touch_cal_len);
  internal_devcfg_pack_le32(&buf[k_ra8_devcfg_off_mfg_date], b->mfg_date);
  internal_devcfg_pack_le32(&buf[k_ra8_devcfg_off_key_id], b->device_key_id);
  internal_devcfg_pack_le16(&buf[k_ra8_devcfg_off_hw_rev], b->hw_rev);
  internal_devcfg_pack_le16(&buf[k_ra8_devcfg_off_fixture], b->fixture_id);
  internal_devcfg_pack_le16(&buf[k_ra8_devcfg_off_vcom], b->panel_vcom_mv);

  const uint32_t crc =
    internal_devcfg_crc32(&buf[k_ra8_devcfg_hdr_bytes], (uint32_t)k_ra8_devcfg_body_bytes);
  internal_devcfg_pack_le32(&buf[k_ra8_devcfg_off_magic], (uint32_t)k_ra8_devcfg_magic);
  internal_devcfg_pack_le16(&buf[k_ra8_devcfg_off_schema], (uint16_t)k_ra8_devcfg_schema_ver);
  internal_devcfg_pack_le16(&buf[k_ra8_devcfg_off_reclen], (uint16_t)k_ra8_devcfg_record_len);
  internal_devcfg_pack_le32(&buf[k_ra8_devcfg_off_seq], seq);
  internal_devcfg_pack_le32(&buf[k_ra8_devcfg_off_crc], crc);
  internal_devcfg_pack_le32(&buf[k_ra8_devcfg_off_flags], rec->flags);
}

/**
 * @brief Decode a validated record buffer into a ::ra8_devcfg_record_t.
 *
 * @details Inverse of ::internal_devcfg_serialize; unpacks each body field and
 *          the header discriminators. Assumes the buffer already passed
 *          validation, so no integrity check is repeated here.
 *
 * @param[in]  buf Validated ::k_ra8_devcfg_record_len byte buffer; non-NULL.
 * @param[out] out Receives the decoded record; non-NULL.
 *
 * @pre  ``buf`` passed ::internal_devcfg_copy_valid.
 * @pre  ``out`` is a writable record sink.
 * @post ``out`` mirrors the body, flags, seq and schema of ``buf``.
 * @post No integrity gate is re-evaluated.
 *
 * @note Not thread-safe; called only from the resolver.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_devcfg_deserialize(const uint8_t* buf, ra8_devcfg_record_t* out)
{
  ra8_devcfg_body_t* b = &out->body;
  (void)memcpy(b->serial, &buf[k_ra8_devcfg_off_serial], (size_t)k_ra8_devcfg_serial_len);
  (void)memcpy(b->panel_serial,
               &buf[k_ra8_devcfg_off_panel_serial],
               (size_t)k_ra8_devcfg_panel_serial_len);
  (void)memcpy(b->panel_lut_id,
               &buf[k_ra8_devcfg_off_panel_lut],
               (size_t)k_ra8_devcfg_panel_lut_len);
  (void)memcpy(b->touch_cal, &buf[k_ra8_devcfg_off_touch_cal], (size_t)k_ra8_devcfg_touch_cal_len);
  b->mfg_date         = internal_devcfg_unpack_le32(&buf[k_ra8_devcfg_off_mfg_date]);
  b->device_key_id    = internal_devcfg_unpack_le32(&buf[k_ra8_devcfg_off_key_id]);
  b->hw_rev           = internal_devcfg_unpack_le16(&buf[k_ra8_devcfg_off_hw_rev]);
  b->fixture_id       = internal_devcfg_unpack_le16(&buf[k_ra8_devcfg_off_fixture]);
  b->panel_vcom_mv    = internal_devcfg_unpack_le16(&buf[k_ra8_devcfg_off_vcom]);
  out->flags          = internal_devcfg_unpack_le32(&buf[k_ra8_devcfg_off_flags]);
  out->seq            = internal_devcfg_unpack_le32(&buf[k_ra8_devcfg_off_seq]);
  out->schema_version = internal_devcfg_unpack_le16(&buf[k_ra8_devcfg_off_schema]);
}

/**
 * @brief Report whether a serialised copy passes every integrity gate.
 *
 * @details
 * Four sequential, independent gates -- magic word, known schema, expected
 * ``record_len``, CRC-32 over the body. Any one failing rejects the copy; all
 * four passing accepts it. Each gate is a single condition, so the copy is
 * accepted only on the AND of all four.
 *
 * @param[in] buf ::k_ra8_devcfg_record_len byte buffer to test; non-NULL.
 *
 * @return ``true`` iff the copy is a valid, current-schema record.
 * @retval true  All four gates passed.
 * @retval false Magic, schema, ``record_len`` or CRC failed.
 *
 * @pre  ``buf`` holds at least ::k_ra8_devcfg_record_len bytes.
 * @pre  ``buf`` was filled by a backing-store read.
 * @post No state mutated.
 * @post The verdict depends only on ``buf``.
 *
 * @note Thread-safe (pure; no statics).
 * @since 0.1.0
 */
RA8_INTERNAL
[[nodiscard]] static bool internal_devcfg_copy_valid(const uint8_t* buf)
{
  if (internal_devcfg_unpack_le32(&buf[k_ra8_devcfg_off_magic]) != (uint32_t)k_ra8_devcfg_magic) {
    return false; /* not our record / blank window */
  }
  if (internal_devcfg_unpack_le16(&buf[k_ra8_devcfg_off_schema]) >
      (uint16_t)k_ra8_devcfg_schema_ver) {
    return false; /* written by a newer, unknown schema -- reject, never guess */
  }
  if (internal_devcfg_unpack_le16(&buf[k_ra8_devcfg_off_reclen]) !=
      (uint16_t)k_ra8_devcfg_record_len) {
    return false; /* length not what this schema expects */
  }
  const uint32_t stored = internal_devcfg_unpack_le32(&buf[k_ra8_devcfg_off_crc]);
  const uint32_t calc =
    internal_devcfg_crc32(&buf[k_ra8_devcfg_hdr_bytes], (uint32_t)k_ra8_devcfg_body_bytes);
  return stored == calc;
}

/**
 * @brief Read one copy through the store and validate it.
 *
 * @details Reads ::k_ra8_devcfg_record_len bytes at ``offset`` through the store
 *          seam and runs the integrity gate. A read fault or blank window is
 *          treated as "copy absent" so the resolver falls through rather than
 *          failing the whole load.
 *
 * @param[in]  store  Store seam; non-NULL with non-NULL ``read``.
 * @param[in]  offset Copy offset in the devcfg region.
 * @param[out] out    Receives the decoded record when valid; non-NULL.
 *
 * @return ``true`` when the copy read and validated; ``false`` otherwise.
 * @retval true  ``out`` holds the decoded copy.
 * @retval false Read fault, blank window, or failed integrity gate.
 *
 * @pre  ``store->read`` is callable.
 * @pre  ``out`` is a writable record sink.
 * @post On ``true`` ``out`` holds the decoded copy; on ``false`` ``out`` is
 *       untouched.
 * @post No backing store is programmed.
 *
 * @note Not thread-safe; boot / provisioning path only.
 * @since 0.1.0
 */
RA8_INTERNAL
[[nodiscard]] static bool
internal_devcfg_probe(const ra8_devcfg_store_t* store, uint32_t offset, ra8_devcfg_record_t* out)
{
  uint8_t buf[k_ra8_devcfg_record_len] = {};
  if (store->read(offset, buf, (uint32_t)k_ra8_devcfg_record_len) != k_ra8_ok) {
    return false; /* read fault / blank -> copy absent, resolver falls through */
  }
  if (!internal_devcfg_copy_valid(buf)) {
    return false;
  }
  internal_devcfg_deserialize(buf, out);
  return true;
}

/**
 * @brief Choose the copy offset a new record overwrites (the stale slot).
 *
 * @details
 * Prefers an invalid slot so a valid record is never clobbered; when both are
 * valid, targets the lower ``seq`` so the newest survives the write window.
 *
 * @param[in] valid0 Copy-0 validity.
 * @param[in] seq0   Copy-0 sequence (meaningful only when ``valid0``).
 * @param[in] valid1 Copy-1 validity.
 * @param[in] seq1   Copy-1 sequence (meaningful only when ``valid1``).
 *
 * @return The offset of the slot to program.
 * @retval k_ra8_devcfg_copy0_off Copy 0 is the stale / free slot.
 * @retval k_ra8_devcfg_copy1_off Copy 1 is the stale / free slot.
 *
 * @pre  ``seqN`` reflects a read copy when ``validN`` is true.
 * @pre  At least one slot is available to program.
 * @post No state mutated.
 * @post The chosen slot is never the sole newest valid copy.
 *
 * @note Thread-safe (pure; no statics).
 * @since 0.1.0
 */
RA8_INTERNAL
[[nodiscard]] static uint32_t
internal_devcfg_target(bool valid0, uint32_t seq0, bool valid1, uint32_t seq1)
{
  if (!valid0) {
    return (uint32_t)k_ra8_devcfg_copy0_off;
  }
  if (!valid1) {
    return (uint32_t)k_ra8_devcfg_copy1_off;
  }
  return (seq0 <= seq1) ? (uint32_t)k_ra8_devcfg_copy0_off : (uint32_t)k_ra8_devcfg_copy1_off;
}

/**
 * @brief Probe both copies and choose the commit target slot and next sequence.
 *
 * @details Reads the two existing copies to find the newest valid sequence
 *          (0 when neither is valid), stamps the new record one past it, and
 *          selects the stale slot via ::internal_devcfg_target so the newest
 *          good copy is never the write target.
 *
 * @param[in]  store      Store seam; non-NULL with non-NULL ``read``.
 * @param[out] out_target Receives the slot offset to program; non-NULL.
 * @param[out] out_seq    Receives the sequence to stamp; non-NULL.
 *
 * @pre  ``store->read`` is callable.
 * @pre  ``out_target`` and ``out_seq`` are writable sinks.
 * @post ``*out_seq`` exceeds every valid copy's sequence.
 * @post ``*out_target`` is the stale / free slot offset.
 *
 * @note Not thread-safe; provisioning path only.
 * @since 0.1.0
 */
RA8_INTERNAL
static void
internal_devcfg_plan(const ra8_devcfg_store_t* store, uint32_t* out_target, uint32_t* out_seq)
{
  ra8_devcfg_record_t cur0 = {};
  ra8_devcfg_record_t cur1 = {};
  const bool     valid0    = internal_devcfg_probe(store, (uint32_t)k_ra8_devcfg_copy0_off, &cur0);
  const bool     valid1    = internal_devcfg_probe(store, (uint32_t)k_ra8_devcfg_copy1_off, &cur1);
  const uint32_t seq0      = valid0 ? cur0.seq : 0U;
  const uint32_t seq1      = valid1 ? cur1.seq : 0U;
  const uint32_t newest    = (seq0 >= seq1) ? seq0 : seq1;
  *out_seq                 = newest + 1U;
  *out_target              = internal_devcfg_target(valid0, seq0, valid1, seq1);
}

/**
 * @brief Program a serialised record to a slot with the header page written last.
 *
 * @details Writes the body span first and the 32-byte header page last, so a
 *          power cut before the header lands leaves that slot header-invalid and
 *          the other copy the sole survivor -- there is never a window with zero
 *          valid records.
 *
 * @param[in] store  Store seam; non-NULL with non-NULL ``write``.
 * @param[in] target Slot offset to program.
 * @param[in] buf    Serialised ::k_ra8_devcfg_record_len byte record; non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Body and header both programmed.
 * @retval other    Forwarded from the first failing ``store->write``.
 *
 * @pre  ``buf`` holds a CRC-valid serialised record.
 * @pre  ``store->write`` is callable.
 * @post On ``k_ra8_ok`` the slot holds the full record with its header last.
 * @post On error the slot may be partially written; the other copy is untouched.
 *
 * @note Not thread-safe; provisioning path only.
 * @since 0.1.0
 */
RA8_INTERNAL
[[nodiscard]] static ra8_err_t
internal_devcfg_write_record(const ra8_devcfg_store_t* store, uint32_t target, const uint8_t* buf)
{
  const ra8_err_t body_err = store->write(target + (uint32_t)k_ra8_devcfg_hdr_bytes,
                                          &buf[k_ra8_devcfg_hdr_bytes],
                                          (uint32_t)k_ra8_devcfg_body_bytes);
  RA8_RETURN_ON_ERROR(body_err, s_tag, "commit: body write failed");
  const ra8_err_t hdr_err = store->write(target, buf, (uint32_t)k_ra8_devcfg_hdr_bytes);
  RA8_RETURN_ON_ERROR(hdr_err, s_tag, "commit: header write failed");
  return k_ra8_ok;
}

/* ===========================================================================
 * Public API
 * ===========================================================================
 */

[[nodiscard]] ra8_err_t ra8_devcfg_load(const ra8_devcfg_store_t* store)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "load: store null");
  RA8_CHECK_NULL_PTR(store->read, s_tag, "load: store->read null");

  ra8_devcfg_record_t rec0 = {};
  ra8_devcfg_record_t rec1 = {};
  const bool valid0        = internal_devcfg_probe(store, (uint32_t)k_ra8_devcfg_copy0_off, &rec0);
  const bool valid1        = internal_devcfg_probe(store, (uint32_t)k_ra8_devcfg_copy1_off, &rec1);

  if (valid0 && valid1) {
    /* Both survive -- the higher sequence is the newer record; a tie takes 0. */
    s_record = (rec0.seq >= rec1.seq) ? rec0 : rec1;
  } else if (valid0) {
    s_record = rec0;
  } else if (valid1) {
    s_record = rec1;
  } else {
    s_state = k_ra8_devcfg_state_unprovisioned;
    ra8_log_warn(s_tag, "no valid device config -- UNPROVISIONED");
    return k_ra8_err_validation_failed;
  }
  s_state = k_ra8_devcfg_state_loaded;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_devcfg_get_vcom_mv(uint16_t* out_mv)
{
  RA8_CHECK_NULL_PTR(out_mv, s_tag, "get_vcom: out_mv null");
  if (s_state != k_ra8_devcfg_state_loaded) {
    return k_ra8_err_not_initialized;
  }
  if ((s_record.flags & (uint32_t)k_ra8_devcfg_flag_vcom_valid) == 0U) {
    ra8_log_error(s_tag, "VCOM not marked valid -- refuse the panel");
    return k_ra8_err_validation_failed;
  }
  const uint16_t mv = s_record.body.panel_vcom_mv;
  if ((mv < (uint16_t)k_ra8_devcfg_vcom_min_mv) || (mv > (uint16_t)k_ra8_devcfg_vcom_max_mv)) {
    ra8_log_error(s_tag, "VCOM out of plausible range -- refuse the panel");
    return k_ra8_err_validation_failed;
  }
  *out_mv = mv;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_devcfg_get_body(const ra8_devcfg_body_t** out_body)
{
  RA8_CHECK_NULL_PTR(out_body, s_tag, "get_body: out_body null");
  if (s_state != k_ra8_devcfg_state_loaded) {
    return k_ra8_err_not_initialized;
  }
  *out_body = &s_record.body;
  return k_ra8_ok;
}

[[nodiscard]] bool ra8_devcfg_is_blank(void)
{
  return s_state != k_ra8_devcfg_state_loaded;
}

[[nodiscard]] ra8_err_t ra8_devcfg_commit(const ra8_devcfg_store_t*  store,
                                          const ra8_devcfg_record_t* rec)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "commit: store null");
  RA8_CHECK_NULL_PTR(store->read, s_tag, "commit: store->read null");
  RA8_CHECK_NULL_PTR(store->write, s_tag, "commit: store->write null");
  RA8_CHECK_NULL_PTR(rec, s_tag, "commit: rec null");

  uint32_t target  = 0U;
  uint32_t new_seq = 0U;
  internal_devcfg_plan(store, &target, &new_seq);

  uint8_t buf[k_ra8_devcfg_record_len] = {};
  internal_devcfg_serialize(rec, new_seq, buf);
  return internal_devcfg_write_record(store, target, buf);
}

void ra8_devcfg_reset(void)
{
  s_state  = k_ra8_devcfg_state_unloaded;
  s_record = (ra8_devcfg_record_t){};
}
