/**
 * @file ra8_jof_png_chunk.c
 * @brief PNG chunk layer for the streaming decoder: prologue, palette
 *        tables, ancillary skipping and the post-IDAT walk (#231).
 *
 * @details
 * The byte-source primitives and every chunk-structure concern of the
 * bounded-RAM PNG decoder live here; the pixel layer (inflate + unfilter +
 * translate) drives them from `ra8_jof_png.c` through the prototypes
 * in `ra8_jof_png_internal.h`. All structural anomalies fail closed:
 * this parser feeds on untrusted EPUB content. Spec citations reference the
 * W3C PNG specification (second edition), abbreviated `PNG sec N`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Domain]
 * {World: NS}
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_jof_internal.h"
#include "ra8_jof_png_internal.h"

/** @brief PNG signature (PNG sec 5.2). */
static const uint8_t s_png_sig[k_ra8_png_sig_bytes] =
  {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU}; /* MAGIC-OK: the 8 spec signature bytes */

/**
 * @brief Pull exactly @p len bytes; EOF mid-read is a protocol error.
 * @details Bounded by @p len (each non-EOF pull delivers >= 1 byte).
 * @param[in,out] st  Decoder state (source position advances).
 * @param[out]    buf Destination buffer.
 * @param[in]     len Bytes required.
 * @return Result code.
 * @retval k_ra8_ok                 Exactly @p len bytes delivered.
 * @retval k_ra8_err_protocol_error The source ended early.
 * @retval other                    Propagated from the pull callback.
 * @pre @p buf holds @p len writable bytes.
 * @pre @p st->pull is bound.
 * @post On success the source advanced by @p len bytes.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_jof_png_pull_exact(ra8_png_state_t* st, uint8_t* buf, uint32_t len)
{
  uint32_t done = 0U;
  while (done < len) {
    size_t          got = 0U;
    const ra8_err_t err = st->pull(st->pull_ctx, &buf[done], (size_t)(len - done), &got);
    if (err != k_ra8_ok) {
      return err;
    }
    if (got == 0U) {
      return k_ra8_err_protocol_error;
    }
    done += (uint32_t)got;
  }
  return k_ra8_ok;
}

/**
 * @brief Discard exactly @p len source bytes (unknown / ancillary chunks).
 * @details Bounded by @p len over a fixed-size stack scratch.
 * @param[in,out] st  Decoder state (source position advances).
 * @param[in]     len Bytes to discard.
 * @return Result code.
 * @retval k_ra8_ok                 Bytes discarded.
 * @retval k_ra8_err_protocol_error The source ended early.
 * @retval other                    Propagated from the pull callback.
 * @pre @p st->pull is bound.
 * @pre @p len came from a validated chunk-length field.
 * @post On success the source advanced by @p len bytes.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_jof_png_skip(ra8_png_state_t* st, uint32_t len)
{
  uint8_t  scratch[k_ra8_png_skip_chunk];
  uint32_t left = len;
  while (left > 0U) {
    const uint32_t take =
      (left < (uint32_t)k_ra8_png_skip_chunk) ? left : (uint32_t)k_ra8_png_skip_chunk;
    const ra8_err_t err = priv_jof_png_pull_exact(st, scratch, take);
    if (err != k_ra8_ok) {
      return err;
    }
    left -= take;
  }
  return k_ra8_ok;
}

/**
 * @brief Read one 8-byte chunk header (length + type, both big-endian).
 * @details Assembles the big-endian length and type fields and enforces the spec length cap.
 * @param[in,out] st       Decoder state (source position advances).
 * @param[out]    out_len  Receives the payload length.
 * @param[out]    out_type Receives the chunk type value.
 * @return Result code.
 * @retval k_ra8_ok                 Header read; length within the spec cap.
 * @retval k_ra8_err_protocol_error Truncated header or oversize length.
 * @retval other                    Propagated from the pull callback.
 * @pre The source is positioned at a chunk boundary.
 * @pre Both outputs are writable.
 * @post On success the source sits at the chunk payload.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_jof_png_chunk_hdr(ra8_png_state_t* st,
                                          uint32_t*        out_len,
                                          uint32_t*        out_type)
{
  uint8_t         hdr[k_ra8_png_chunk_hdr] = {};
  const ra8_err_t err                      = priv_jof_png_pull_exact(st, hdr, sizeof(hdr));
  if (err != k_ra8_ok) {
    return err;
  }
  const uint32_t len  = ((uint32_t)hdr[0] << k_ra8_png_be_sh24) |
                        ((uint32_t)hdr[1] << k_ra8_png_be_sh16) |
                        ((uint32_t)hdr[2] << k_ra8_png_be_sh8) | (uint32_t)hdr[3];
  const uint32_t type = ((uint32_t)hdr[4] << k_ra8_png_be_sh24) |
                        ((uint32_t)hdr[5] << k_ra8_png_be_sh16) |
                        ((uint32_t)hdr[6] << k_ra8_png_be_sh8) | (uint32_t)hdr[7];
  if (len > (uint32_t)k_ra8_png_max_len) {
    return k_ra8_err_protocol_error;
  }
  *out_len  = len;
  *out_type = type;
  return k_ra8_ok;
}

/**
 * @brief Map an IHDR colour type to its source bytes-per-pixel, or 0.
 * @details The bit depth is fixed at 8, so bytes per pixel equals the channel count.
 * @param[in] color_type IHDR colour-type byte.
 * @return Source channel count for 8-bit depth.
 * @retval 1-4 Legal colour type's bytes per pixel.
 * @retval 0   Unsupported / illegal colour type.
 * @pre The bit depth was validated as 8.
 * @pre None (total over uint8_t).
 * @post No state mutated.
 * @post Return is 0 or a legal bpp.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static uint8_t internal_png_src_channels(uint8_t color_type)
{
  switch (color_type) {
    case k_ra8_png_color_gray:
      return (uint8_t)k_ra8_png_ch_1;
    case k_ra8_png_color_rgb:
      return (uint8_t)k_ra8_png_ch_3;
    case k_ra8_png_color_pal:
      return (uint8_t)k_ra8_png_ch_1;
    case k_ra8_png_color_ga:
      return (uint8_t)k_ra8_png_ch_2;
    case k_ra8_png_color_rgba:
      return (uint8_t)k_ra8_png_ch_4;
    default:
      return 0U;
  }
}

/**
 * @brief Validate the 13 IHDR payload bytes and bind the geometry fields.
 * @details Rejects, fail-closed: zero / over-cap dimensions, non-8-bit
 *          depth, unknown colour types, non-zero compression/filter methods
 *          and interlacing (Adam7 breaks streaming).
 * @param[in,out] st    Decoder state (geometry fields written).
 * @param[in]     ihdr  The IHDR payload bytes.
 * @param[in]     max_w Fail-closed width cap.
 * @param[in]     max_h Fail-closed height cap.
 * @return Result code.
 * @retval k_ra8_ok                IHDR accepted.
 * @retval k_ra8_err_invalid_size  Dimension zero / over the caps.
 * @retval k_ra8_err_not_supported Depth / colour / method / interlace.
 * @pre @p ihdr holds ::k_ra8_png_ihdr_len bytes.
 * @pre @p st is zero-initialised apart from the bound callbacks.
 * @post On success `w`/`h`/`color_type`/`src_ch` are set.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t
internal_png_check_ihdr(ra8_png_state_t* st, const uint8_t* ihdr, uint16_t max_w, uint16_t max_h)
{
  const uint32_t w32 = ((uint32_t)ihdr[0] << k_ra8_png_be_sh24) |
                       ((uint32_t)ihdr[1] << k_ra8_png_be_sh16) |
                       ((uint32_t)ihdr[2] << k_ra8_png_be_sh8) | (uint32_t)ihdr[3];
  const uint32_t h32 = ((uint32_t)ihdr[4] << k_ra8_png_be_sh24) |
                       ((uint32_t)ihdr[5] << k_ra8_png_be_sh16) |
                       ((uint32_t)ihdr[6] << k_ra8_png_be_sh8) | (uint32_t)ihdr[7];
  if ((w32 == 0U) || (w32 > (uint32_t)max_w)) {
    return k_ra8_err_invalid_size;
  }
  if ((h32 == 0U) || (h32 > (uint32_t)max_h)) {
    return k_ra8_err_invalid_size;
  }
  if (ihdr[k_ra8_png_ihdr_ofs_depth] != (uint8_t)k_ra8_png_depth_8) {
    return k_ra8_err_not_supported;
  }
  st->src_ch = internal_png_src_channels(ihdr[k_ra8_png_ihdr_ofs_color]);
  if (st->src_ch == 0U) {
    return k_ra8_err_not_supported;
  }
  if (ihdr[k_ra8_png_ihdr_ofs_compress] != 0U) {
    return k_ra8_err_not_supported;
  }
  if (ihdr[k_ra8_png_ihdr_ofs_filter] != 0U) {
    return k_ra8_err_not_supported;
  }
  if (ihdr[k_ra8_png_ihdr_ofs_interlace] != 0U) {
    return k_ra8_err_not_supported; /* Adam7 breaks streaming; fail closed */
  }
  st->w          = (uint16_t)w32;
  st->h          = (uint16_t)h32;
  st->color_type = ihdr[k_ra8_png_ihdr_ofs_color];
  return k_ra8_ok;
}

/**
 * @brief Read the IHDR chunk (must be first) and validate its payload.
 * @details Enforces the IHDR-first rule and the exact 13-byte payload
 *          length, then delegates the field checks to ::internal_png_check_ihdr and
 *          skips the trailing CRC.
 * @param[in,out] st    Decoder state (geometry fields written).
 * @param[in]     max_w Fail-closed width cap.
 * @param[in]     max_h Fail-closed height cap.
 * @return Result code.
 * @retval k_ra8_ok                 IHDR accepted.
 * @retval k_ra8_err_protocol_error Structural mismatch (order, length).
 * @retval other                    Propagated from the field validation.
 * @pre The source sits right after the PNG signature.
 * @pre @p st is zero-initialised apart from the bound callbacks.
 * @post On success the source sits at the first post-IHDR chunk.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t
internal_png_parse_ihdr(ra8_png_state_t* st, uint16_t max_w, uint16_t max_h)
{
  uint32_t  len  = 0U;
  uint32_t  type = 0U;
  ra8_err_t err  = priv_jof_png_chunk_hdr(st, &len, &type);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((type != (uint32_t)k_ra8_png_type_ihdr) || (len != (uint32_t)k_ra8_png_ihdr_len)) {
    return k_ra8_err_protocol_error;
  }
  uint8_t ihdr[k_ra8_png_ihdr_len] = {};
  err                              = priv_jof_png_pull_exact(st, ihdr, sizeof(ihdr));
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_png_check_ihdr(st, ihdr, max_w, max_h);
  if (err != k_ra8_ok) {
    return err;
  }
  return priv_jof_png_skip(st, (uint32_t)k_ra8_png_crc_bytes);
}

/**
 * @brief Parse a PLTE chunk into the palette table (colour type 3).
 * @details Validates the payload shape, stores the RGB triples, and skips the trailing CRC.
 * @param[in,out] st  Decoder state (palette written).
 * @param[in]     len PLTE payload length.
 * @return Result code.
 * @retval k_ra8_ok                    Palette stored.
 * @retval k_ra8_err_validation_failed Length not a multiple of 3 / over cap
 *                                     / duplicate PLTE.
 * @retval other                       Propagated from the pull callback.
 * @pre IHDR has been parsed.
 * @pre @p len came from a validated chunk header.
 * @post On success `plte_count` reflects the stored entries.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_png_parse_plte(ra8_png_state_t* st, uint32_t len)
{
  if ((len == 0U) || ((len % (uint32_t)k_ra8_png_ch_3) != 0U) ||
      (len > (uint32_t)k_ra8_png_plte_max) || (st->has_plte != 0U)) {
    return k_ra8_err_validation_failed;
  }
  const ra8_err_t err = priv_jof_png_pull_exact(st, st->palette, len);
  if (err != k_ra8_ok) {
    return err;
  }
  st->plte_count = (uint16_t)(len / (uint32_t)k_ra8_png_ch_3);
  st->has_plte   = 1U;
  return priv_jof_png_skip(st, (uint32_t)k_ra8_png_crc_bytes);
}

/**
 * @brief Parse a tRNS chunk (palette alpha; colour type 3 only).
 * @details A tRNS on colour types 0/2 (colour-key transparency) is rejected
 *          `k_ra8_err_not_supported` rather than silently flattened -- this
 *          producer never alters pixels it cannot represent.
 * @param[in,out] st  Decoder state (alpha table written).
 * @param[in]     len tRNS payload length.
 * @return Result code.
 * @retval k_ra8_ok                    Alpha table stored.
 * @retval k_ra8_err_not_supported     tRNS on a non-palette colour type.
 * @retval k_ra8_err_validation_failed Oversize / missing PLTE / duplicate.
 * @retval other                       Propagated from the pull callback.
 * @pre IHDR has been parsed.
 * @pre @p len came from a validated chunk header.
 * @post On success `trns_count`/`has_trns` reflect the table.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_png_parse_trns(ra8_png_state_t* st, uint32_t len)
{
  if (st->color_type != (uint8_t)k_ra8_png_color_pal) {
    return k_ra8_err_not_supported;
  }
  if ((len > (uint32_t)k_ra8_png_trns_max) || (st->has_plte == 0U) || (st->has_trns != 0U)) {
    return k_ra8_err_validation_failed;
  }
  const ra8_err_t err = priv_jof_png_pull_exact(st, st->trns, len);
  if (err != k_ra8_ok) {
    return err;
  }
  st->trns_count = (uint16_t)len;
  st->has_trns   = 1U;
  return priv_jof_png_skip(st, (uint32_t)k_ra8_png_crc_bytes);
}

/**
 * @brief Verify the 8-byte signature, then parse + validate the IHDR.
 * @param[in,out] st    Decoder state (geometry fields written).
 * @param[in]     max_w Fail-closed width cap.
 * @param[in]     max_h Fail-closed height cap.
 * @return Result code.
 * @retval k_ra8_ok                 Prologue accepted; chunk walk may start.
 * @retval k_ra8_err_protocol_error Bad signature / IHDR structure.
 * @retval other                    Propagated from the IHDR validation.
 * @pre The source is positioned at byte 0 of the PNG stream.
 * @pre @p st holds the bound callbacks.
 * @post On success the source sits at the first post-IHDR chunk.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_jof_png_prologue(ra8_png_state_t* st, uint16_t max_w, uint16_t max_h)
{
  uint8_t         sig[k_ra8_png_sig_bytes] = {};
  const ra8_err_t err                      = priv_jof_png_pull_exact(st, sig, sizeof(sig));
  if (err != k_ra8_ok) {
    return err;
  }
  if (memcmp(sig, s_png_sig, sizeof(sig)) != 0) {
    return k_ra8_err_protocol_error;
  }
  return internal_png_parse_ihdr(st, max_w, max_h);
}

/**
 * @brief Dispatch one pre-IDAT chunk (PLTE / tRNS / ancillary / stray IEND).
 * @param[in,out] st   Decoder state.
 * @param[in]     len  Chunk payload length.
 * @param[in]     type Chunk type value.
 * @return Result code.
 * @retval k_ra8_ok                 Chunk consumed; keep walking.
 * @retval k_ra8_err_protocol_error IEND arrived before any IDAT.
 * @retval other                    Propagated from the chunk parsers.
 * @pre The IHDR has been parsed.
 * @pre @p len came from a validated chunk header.
 * @post On success the source sits at the next chunk boundary.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_jof_png_pre_idat(ra8_png_state_t* st, uint32_t len, uint32_t type)
{
  if (type == (uint32_t)k_ra8_png_type_plte) {
    return internal_png_parse_plte(st, len);
  }
  if (type == (uint32_t)k_ra8_png_type_trns) {
    return internal_png_parse_trns(st, len);
  }
  if (type == (uint32_t)k_ra8_png_type_iend) {
    return k_ra8_err_protocol_error; /* IEND before any IDAT */
  }
  return priv_jof_png_skip(st, len + (uint32_t)k_ra8_png_crc_bytes);
}

/**
 * @brief Walk the post-IDAT chunks until IEND (ancillary chunks skipped).
 * @details When the zlib stream ended inside the last IDAT (no pending
 *          chunk parked by the refill path), that IDAT's trailing CRC is
 *          still unread: consume it, then resume the chunk walk.
 * @param[in,out] st Decoder state (pending chunk consumed first).
 * @return Result code.
 * @retval k_ra8_ok                 IEND reached.
 * @retval k_ra8_err_protocol_error Chunk budget exhausted / truncation /
 *                                  a stray IDAT after the stream ended.
 * @retval other                    Propagated from the pull callback.
 * @pre The inflate phase finished (`rows_done == h`).
 * @pre `st->idat_rem == 0`.
 * @post On success the datastream is fully consumed through IEND.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_jof_png_finish(ra8_png_state_t* st)
{
  uint32_t len  = st->pending_len;
  uint32_t type = st->pending_type;
  if (st->pending_valid == 0U) {
    ra8_err_t err = priv_jof_png_skip(st, (uint32_t)k_ra8_png_crc_bytes); /* last IDAT's CRC */
    if (err != k_ra8_ok) {
      return err;
    }
    err = priv_jof_png_chunk_hdr(st, &len, &type);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  for (uint32_t guard = 0U; guard < (uint32_t)k_ra8_png_max_chunks; guard++) {
    if (type == (uint32_t)k_ra8_png_type_iend) {
      return k_ra8_ok;
    }
    if (type == (uint32_t)k_ra8_png_type_idat) {
      return k_ra8_err_protocol_error; /* IDAT after the stream completed */
    }
    ra8_err_t err = priv_jof_png_skip(st, len + (uint32_t)k_ra8_png_crc_bytes);
    if (err != k_ra8_ok) {
      return err;
    }
    err = priv_jof_png_chunk_hdr(st, &len, &type);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_err_protocol_error;
}
