/**
 * @file jof.c
 * @brief JOF atlas reader: parse/validate + bounded per-tile decode.
 *
 * @details
 * Implements the bounded decode-one-tile primitive (`jof_read_tile`)
 * documented in `jof.h`. Deflate tiles inflate through the
 * zero-heap `ra8_decompress()`; raw tiles are a single positioned read.
 * The memstore helpers (RA8FW-1147) and the structural validation
 * (`jof_parse`, RA8FW-1148) live in `jof_abi.zig`; this TU keeps the
 * bounded per-tile decode.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Domain]
 * {World: NS}
 */

#include "jof.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_compress.h"
#include "ra8_err.h"

/** @brief Module log tag. */
static const char* const s_tag = "jof";

/**
 * @enum jof_le_t
 * @brief Little-endian assembly constants for the header/footer fields.
 */
typedef enum : uint8_t {
  k_jof_le_b0   = 0U,  /**< Byte 0 (LSB) offset. */
  k_jof_le_b1   = 1U,  /**< Byte 1 offset.       */
  k_jof_le_b2   = 2U,  /**< Byte 2 offset.       */
  k_jof_le_b3   = 3U,  /**< Byte 3 (MSB) offset. */
  k_jof_le_sh8  = 8U,  /**< 8-bit shift.         */
  k_jof_le_sh16 = 16U, /**< 16-bit shift.        */
  k_jof_le_sh24 = 24U, /**< 24-bit shift.        */
} jof_le_t;


/* ---------------------------------------------------------------------------
 * Small pure helpers.
 * ---------------------------------------------------------------------------
 */


/**
 * @brief Read a little-endian uint32 from @p buf at @p off.
 * @details Assembles the four little-endian bytes at @p off.
 * @param[in] buf Source bytes.
 * @param[in] off Field offset.
 * @return The assembled 32-bit value.
 * @retval 0-UINT32_MAX Little-endian uint32 from the four bytes at @p off.
 * @pre @p buf holds at least `off + 4` readable bytes.
 * @pre @p off is a header/footer/index field offset.
 * @post No state mutated.
 * @post Return depends solely on the four bytes read.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static uint32_t internal_rd_u32(const uint8_t* buf, uint8_t off)
{
  return (uint32_t)buf[off + k_jof_le_b0] | ((uint32_t)buf[off + k_jof_le_b1] << k_jof_le_sh8) |
         ((uint32_t)buf[off + k_jof_le_b2] << k_jof_le_sh16) |
         ((uint32_t)buf[off + k_jof_le_b3] << k_jof_le_sh24);
}


/**
 * @brief Exact positioned read: EOF or a short read is a validation failure.
 * @details Wraps the pread seam so structural reads treat any short read as corruption.
 * @param[in]  pread     Backing read seam.
 * @param[in]  pread_ctx Context for @p pread.
 * @param[in]  off       Byte offset to read at.
 * @param[out] buf       Destination buffer.
 * @param[in]  len       Bytes required.
 * @return Result code.
 * @retval k_ra8_ok                    Exactly @p len bytes were read.
 * @retval k_ra8_err_validation_failed The backing came up short.
 * @retval other                       Propagated from @p pread.
 * @pre @p pread is non-NULL (caller-validated).
 * @pre @p buf holds @p len writable bytes.
 * @post On success @p buf holds @p len backing bytes from @p off.
 * @post On any error @p buf content is unspecified.
 * @note Not thread-safe (delegates to the backing).
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t
internal_pread_exact(jof_pread_fn pread, void* pread_ctx, uint64_t off, uint8_t* buf, size_t len)
{
  size_t          got = 0U;
  const ra8_err_t err = pread(pread_ctx, off, buf, len, &got);
  if (err != k_ra8_ok) {
    return err;
  }
  if (got != len) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/* ---------------------------------------------------------------------------
 * Parse + validate.
 * ---------------------------------------------------------------------------
 */




/**
 * @brief Fetch + window-validate the index entry for tile @p n.
 * @details The tile-stream window must lie fully inside
 *          `[header end, index start)` -- a hostile index cannot alias the
 *          header, the index, or the footer. The compound decision carries
 *          MC/DC vectors in `test_jof.c`.
 * @param[in]  pread      Backing read seam.
 * @param[in]  pread_ctx  Context for @p pread.
 * @param[in]  info       Parsed atlas geometry.
 * @param[in]  n          Linear tile number.
 * @param[out] out_off    Receives the tile stream's absolute offset.
 * @param[out] out_len    Receives the tile stream's byte length.
 * @return Result code.
 * @retval k_ra8_ok                    Entry read and window-validated.
 * @retval k_ra8_err_validation_failed Short read or window out of bounds.
 * @retval other                       Propagated from @p pread.
 * @pre @p n is < `info->tile_count` (caller-bounded).
 * @pre @p out_off and @p out_len are writable.
 * @post On success the window lies inside the tile-stream region.
 * @post On any error the outputs are unspecified.
 * @note Not thread-safe (delegates to the backing).
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_index_entry(jof_pread_fn      pread,
                                                   void*             pread_ctx,
                                                   const jof_info_t* info,
                                                   uint32_t          n,
                                                   uint32_t*         out_off,
                                                   uint32_t*         out_len)
{
  uint8_t         entry[k_jof_index_entry] = {};
  const uint64_t  eoff = (uint64_t)info->index_off + ((uint64_t)n * (uint64_t)k_jof_index_entry);
  const ra8_err_t err  = internal_pread_exact(pread, pread_ctx, eoff, entry, sizeof(entry));
  if (err != k_ra8_ok) {
    return err;
  }
  const uint32_t toff = internal_rd_u32(entry, (uint8_t)k_jof_idx_ofs_offset);
  const uint32_t tlen = internal_rd_u32(entry, (uint8_t)k_jof_idx_ofs_length);
  const uint64_t tend = (uint64_t)toff + (uint64_t)tlen;
  if ((toff < (uint32_t)k_jof_hdr_bytes) || (tend > (uint64_t)info->index_off)) {
    return k_ra8_err_validation_failed;
  }
  *out_off = toff;
  *out_len = tlen;
  return k_ra8_ok;
}

/**
 * @brief Decode a validated tile stream into @p out_px.
 * @details Raw streams must match the payload size exactly and are pread
 *          straight into the destination; deflate streams stage in
 *          @p scratch and inflate through `ra8_decompress()`, then the
 *          inflated size is checked against the payload size.
 * @param[in]  pread       Backing read seam.
 * @param[in]  pread_ctx   Context for @p pread.
 * @param[in]  info        Parsed atlas geometry (codec source).
 * @param[in]  toff        Tile stream offset (window-validated).
 * @param[in]  tlen        Tile stream length (window-validated).
 * @param[in]  payload     Exact decoded payload size for this tile.
 * @param[out] scratch     Deflate staging buffer (unused for raw).
 * @param[in]  scratch_cap Capacity of @p scratch.
 * @param[out] out_px      Destination pixels.
 * @return Result code.
 * @retval k_ra8_ok                    Tile decoded; exactly @p payload bytes.
 * @retval k_ra8_err_invalid_size      @p scratch_cap too small (deflate).
 * @retval k_ra8_err_null_ptr          @p scratch NULL for a deflate atlas.
 * @retval k_ra8_err_validation_failed Size mismatch or corrupt stream.
 * @retval other                       Propagated from @p pread.
 * @pre The stream window was validated by the index fetch.
 * @pre @p out_px holds at least @p payload writable bytes.
 * @post On success @p out_px holds the packed tile pixels.
 * @post On any error @p out_px content is unspecified.
 * @note Not thread-safe (delegates to the backing).
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_decode_stream(jof_pread_fn      pread,
                                                     void*             pread_ctx,
                                                     const jof_info_t* info,
                                                     uint32_t          toff,
                                                     uint32_t          tlen,
                                                     uint32_t          payload,
                                                     uint8_t*          scratch,
                                                     uint32_t          scratch_cap,
                                                     uint8_t*          out_px)
{
  if (info->codec == (uint8_t)k_jof_codec_raw) {
    if (tlen != payload) {
      return k_ra8_err_validation_failed;
    }
    return internal_pread_exact(pread, pread_ctx, toff, out_px, payload);
  }
  RA8_CHECK_NULL_PTR(scratch, s_tag, "scratch must not be nullptr for deflate");
  if (tlen > scratch_cap) {
    return k_ra8_err_invalid_size;
  }
  const ra8_err_t err = internal_pread_exact(pread, pread_ctx, toff, scratch, tlen);
  if (err != k_ra8_ok) {
    return err;
  }
  uint32_t got = 0U;
  if (ra8_decompress(scratch, tlen, out_px, payload, &got) != k_ra8_ok) {
    return k_ra8_err_validation_failed;
  }
  if (got != payload) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Fetch tile @p n's index entry, then read + decode its stream.
 * @details Combines the two bounded backing operations of a tile read so the
 *          public entry stays within the statement budget.
 * @param[in]  pread       Backing read seam.
 * @param[in]  pread_ctx   Context for @p pread.
 * @param[in]  info        Parsed atlas geometry.
 * @param[in]  n           Linear tile number.
 * @param[in]  payload     Exact decoded payload size for this tile.
 * @param[out] scratch     Deflate staging buffer (unused for raw).
 * @param[in]  scratch_cap Capacity of @p scratch.
 * @param[out] out_px      Destination pixels.
 * @return Result code.
 * @retval k_ra8_ok Tile decoded; exactly @p payload bytes valid.
 * @retval other    Propagated from the index fetch / stream decode.
 * @pre @p n is < `info->tile_count` (caller-bounded).
 * @pre @p out_px holds at least @p payload writable bytes.
 * @post On success @p out_px holds the packed tile pixels.
 * @post On any error @p out_px content is unspecified.
 * @note Not thread-safe (delegates to the backing).
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_fetch_decode(jof_pread_fn      pread,
                                                    void*             pread_ctx,
                                                    const jof_info_t* info,
                                                    uint32_t          n,
                                                    uint32_t          payload,
                                                    uint8_t*          scratch,
                                                    uint32_t          scratch_cap,
                                                    uint8_t*          out_px)
{
  uint32_t        toff = 0U;
  uint32_t        tlen = 0U;
  const ra8_err_t err  = internal_index_entry(pread, pread_ctx, info, n, &toff, &tlen);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_decode_stream(pread,
                                pread_ctx,
                                info,
                                toff,
                                tlen,
                                payload,
                                scratch,
                                scratch_cap,
                                out_px);
}

/**
 * @brief Reject any NULL `jof_read_tile` pointer argument.
 * @details Split out so the public entry stays under the statement budget.
 * @param[in] pread  Backing read seam to validate.
 * @param[in] info   Atlas geometry pointer to validate.
 * @param[in] out_px Destination pixels pointer to validate.
 * @param[in] out_w  Width output pointer to validate.
 * @param[in] out_h  Height output pointer to validate.
 * @return Result code.
 * @retval k_ra8_ok           Every required pointer is non-NULL.
 * @retval k_ra8_err_null_ptr Some pointer is NULL.
 * @pre The caller forwards its own arguments.
 * @pre No pointer is dereferenced here.
 * @post No state mutated.
 * @post Return depends solely on the inputs.
 * @note Thread-safe (pure).
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_read_args_ok(jof_pread_fn      pread,
                                                    const jof_info_t* info,
                                                    const uint8_t*    out_px,
                                                    const uint16_t*   out_w,
                                                    const uint16_t*   out_h)
{
  RA8_CHECK_NULL_PTR(pread, s_tag, "pread must not be nullptr");
  RA8_CHECK_NULL_PTR(info, s_tag, "info must not be nullptr");
  RA8_CHECK_NULL_PTR(out_px, s_tag, "out_px must not be nullptr");
  RA8_CHECK_NULL_PTR(out_w, s_tag, "out_w must not be nullptr");
  RA8_CHECK_NULL_PTR(out_h, s_tag, "out_h must not be nullptr");
  return k_ra8_ok;
}

ra8_err_t jof_read_tile(jof_pread_fn      pread,
                        void*             pread_ctx,
                        const jof_info_t* info,
                        uint16_t          tile_x,
                        uint16_t          tile_y,
                        uint8_t*          scratch,
                        uint32_t          scratch_cap,
                        uint8_t*          out_px,
                        uint32_t          out_cap,
                        uint16_t*         out_w,
                        uint16_t*         out_h)
{
  const ra8_err_t nz = internal_read_args_ok(pread, info, out_px, out_w, out_h);
  if (nz != k_ra8_ok) {
    return nz;
  }
  uint16_t  tw  = 0U;
  uint16_t  th  = 0U;
  ra8_err_t err = jof_tile_dims(info, tile_x, tile_y, &tw, &th);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint32_t payload = (uint32_t)tw * (uint32_t)th * (uint32_t)info->bpp;
  if (payload > out_cap) {
    return k_ra8_err_invalid_size;
  }
  const uint32_t n = ((uint32_t)tile_y * (uint32_t)info->tile_cols) + (uint32_t)tile_x;
  err = internal_fetch_decode(pread, pread_ctx, info, n, payload, scratch, scratch_cap, out_px);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_w = tw;
  *out_h = th;
  return k_ra8_ok;
}
