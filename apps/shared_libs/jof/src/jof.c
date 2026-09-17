/**
 * @file jof.c
 * @brief JOF atlas reader: parse/validate + bounded per-tile decode (#231).
 *
 * @details
 * Implements the fail-closed structural validation (`jof_parse`)
 * and the bounded decode-one-tile primitive (`jof_read_tile`)
 * documented in `jof.h`. Deflate tiles inflate through the
 * zero-heap `ra8_decompress()`; raw tiles are a single positioned read.
 * The memstore helpers give tests and apps a RAM-backed sink/pread pair.
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
#include <string.h>

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

/**
 * @enum jof_bound_t
 * @brief Worst-case stored-stream expansion terms (see the header contract).
 */
typedef enum : uint16_t {
  k_jof_bound_div = 8U,   /**< Expansion denominator (raw / 8). */
  k_jof_bound_add = 256U, /**< Fixed expansion margin, bytes.   */
} jof_bound_t;

/* ---------------------------------------------------------------------------
 * Small pure helpers.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Read a little-endian uint16 from @p buf at @p off.
 * @details Assembles the two little-endian bytes at @p off.
 * @param[in] buf Source bytes.
 * @param[in] off Field offset.
 * @return The assembled 16-bit value.
 * @retval 0-65535 Little-endian uint16 from the two bytes at @p off.
 * @pre @p buf holds at least `off + 2` readable bytes.
 * @pre @p off is a header/footer field offset.
 * @post No state mutated.
 * @post Return depends solely on the two bytes read.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static uint16_t internal_rd_u16(const uint8_t* buf, uint8_t off)
{
  return (uint16_t)((uint16_t)buf[off + k_jof_le_b0] |
                    (uint16_t)((uint16_t)buf[off + k_jof_le_b1] << k_jof_le_sh8));
}

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
 * @brief Ceil-divide @p n by @p d (d > 0).
 * @details Integer ceiling division for tile-grid geometry.
 * @param[in] n Numerator.
 * @param[in] d Denominator (> 0).
 * @return `ceil(n / d)`.
 * @retval 0-UINT32_MAX The ceiling of the division.
 * @pre @p d is non-zero.
 * @pre `n + d - 1` does not overflow uint32_t (true for pixel dims).
 * @post No state mutated.
 * @post Return times @p d is >= @p n.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static uint32_t internal_ceil_div(uint32_t n, uint32_t d)
{
  return (n + d - 1U) / d;
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
 * Memstore sink / pread.
 * ---------------------------------------------------------------------------
 */

ra8_err_t jof_memstore_sink(void* ctx, const uint8_t* buf, size_t len)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  jof_memstore_t* store = (jof_memstore_t*)ctx;
  RA8_CHECK_NULL_PTR(store->buf, s_tag, "store->buf must not be nullptr");
  if (len > (store->cap - store->len)) {
    return k_ra8_err_no_mem;
  }
  (void)memcpy(&store->buf[store->len], buf, len);
  store->len += len;
  return k_ra8_ok;
}

ra8_err_t jof_memstore_pread(void* ctx, uint64_t offset, uint8_t* buf, size_t len, size_t* got)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  RA8_CHECK_NULL_PTR(got, s_tag, "got must not be nullptr");
  *got                  = 0U;
  jof_memstore_t* store = (jof_memstore_t*)ctx;
  RA8_CHECK_NULL_PTR(store->buf, s_tag, "store->buf must not be nullptr");
  if (offset >= (uint64_t)store->len) {
    return k_ra8_ok; /* at/after end: 0-byte read, mirrors entry pread */
  }
  const size_t avail = store->len - (size_t)offset;
  const size_t take  = (len < avail) ? len : avail;
  (void)memcpy(buf, &store->buf[(size_t)offset], take);
  *got = take;
  return k_ra8_ok;
}

/* ---------------------------------------------------------------------------
 * Parse + validate.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Validate the header geometry fields into @p info (fail-closed).
 * @details Single-condition guards so each rejection is independently
 *          testable; the compound cross-checks live in the caller.
 * @param[in]     hdr  The 32 header bytes.
 * @param[in,out] info Geometry output (fields filled before checks).
 * @return Result code.
 * @retval k_ra8_ok                    Geometry fields legal.
 * @retval k_ra8_err_validation_failed A field is zero, over-cap, or illegal.
 * @pre @p hdr holds ::k_jof_hdr_bytes bytes.
 * @pre @p info is writable.
 * @post On success width/height/tile geometry + bpp/codec are populated.
 * @post On failure @p info is partially written and must not be used.
 * @note Thread-safe (pure over its inputs).
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_parse_hdr_fields(const uint8_t* hdr, jof_info_t* info)
{
  info->width  = internal_rd_u16(hdr, (uint8_t)k_jof_ofs_width);
  info->height = internal_rd_u16(hdr, (uint8_t)k_jof_ofs_height);
  info->tile_w = internal_rd_u16(hdr, (uint8_t)k_jof_ofs_tile_w);
  info->tile_h = internal_rd_u16(hdr, (uint8_t)k_jof_ofs_tile_h);
  info->bpp    = hdr[k_jof_ofs_bpp];
  info->codec  = hdr[k_jof_ofs_codec];
  if (info->width == 0U) {
    return k_ra8_err_validation_failed;
  }
  if ((uint32_t)info->width > (uint32_t)k_jof_max_dim) {
    return k_ra8_err_validation_failed;
  }
  if (info->height == 0U) {
    return k_ra8_err_validation_failed;
  }
  if ((uint32_t)info->height > (uint32_t)k_jof_max_dim) {
    return k_ra8_err_validation_failed;
  }
  if (info->tile_w == 0U) {
    return k_ra8_err_validation_failed;
  }
  if (info->tile_h == 0U) {
    return k_ra8_err_validation_failed;
  }
  /* bpp 1 (gray8), 3 (RGB888) or 4 (RGBA8888); 2 is not a defined layout. */
  if ((info->bpp == 0U) || (info->bpp == 2U) || ((uint32_t)info->bpp > (uint32_t)k_jof_bpp_max)) {
    return k_ra8_err_validation_failed;
  }
  if (info->codec > (uint8_t)k_jof_codec_deflate) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Reject any non-zero reserved header byte (strict, fail-closed).
 * @details Walks both reserved header runs; any non-zero byte rejects the atlas.
 * @param[in] hdr The 32 header bytes.
 * @return Result code.
 * @retval k_ra8_ok                    All reserved bytes are zero.
 * @retval k_ra8_err_validation_failed A reserved byte is non-zero.
 * @pre @p hdr holds ::k_jof_hdr_bytes bytes.
 * @pre The fixed-field region has already been parsed.
 * @post No state mutated.
 * @post Return depends solely on the reserved bytes.
 * @note Thread-safe (pure).
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_check_reserved(const uint8_t* hdr)
{
  for (uint8_t i = (uint8_t)k_jof_ofs_reserved; i < (uint8_t)k_jof_ofs_tile_count; ++i) {
    if (hdr[i] != 0U) {
      return k_ra8_err_validation_failed;
    }
  }
  for (uint8_t i = (uint8_t)k_jof_ofs_reserved2; i < (uint8_t)k_jof_hdr_bytes; ++i) {
    if (hdr[i] != 0U) {
      return k_ra8_err_validation_failed;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Cross-check grid, tile-count, footer and index-window invariants.
 * @details The compound decisions here carry MC/DC vectors in
 *          `test_jof.c` (hostile-atlas suite).
 * @param[in,out] info       Geometry (grid fields written here).
 * @param[in]     hdr_count  Header tile-count field.
 * @param[in]     ftr        The 16 footer bytes.
 * @param[in]     total_size Caller-supplied backing size.
 * @return Result code.
 * @retval k_ra8_ok                    All invariants hold; info completed.
 * @retval k_ra8_err_validation_failed A cross-check failed.
 * @pre @p info holds validated header geometry fields.
 * @pre @p ftr holds ::k_jof_footer_bytes bytes.
 * @post On success grid/tile_count/index_off/total_size are populated.
 * @post On failure @p info must not be used.
 * @note Thread-safe (pure over its inputs).
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t
internal_check_cross(jof_info_t* info, uint32_t hdr_count, const uint8_t* ftr, uint64_t total_size)
{
  const uint32_t cols = internal_ceil_div(info->width, info->tile_w);
  const uint32_t rows = internal_ceil_div(info->height, info->tile_h);
  const uint32_t want = cols * rows;
  if (want > (uint32_t)k_jof_max_tiles) {
    return k_ra8_err_validation_failed;
  }
  if (hdr_count != want) {
    return k_ra8_err_validation_failed;
  }
  const uint32_t ftr_count = internal_rd_u32(ftr, (uint8_t)k_jof_ftr_tile_count);
  const uint32_t ftr_total = internal_rd_u32(ftr, (uint8_t)k_jof_ftr_total_size);
  const uint32_t index_off = internal_rd_u32(ftr, (uint8_t)k_jof_ftr_index_off);
  if (ftr_count != want) {
    return k_ra8_err_validation_failed;
  }
  if ((uint64_t)ftr_total != total_size) {
    return k_ra8_err_validation_failed;
  }
  if (index_off < (uint32_t)k_jof_hdr_bytes) {
    return k_ra8_err_validation_failed;
  }
  const uint64_t index_end = (uint64_t)index_off + ((uint64_t)want * (uint64_t)k_jof_index_entry) +
                             (uint64_t)k_jof_footer_bytes;
  if (index_end != total_size) {
    return k_ra8_err_validation_failed;
  }
  info->tile_cols  = (uint16_t)cols;
  info->tile_rows  = (uint16_t)rows;
  info->tile_count = want;
  info->index_off  = index_off;
  info->total_size = ftr_total;
  return k_ra8_ok;
}

/**
 * @brief Read + validate the 32-byte header region into @p out_info.
 * @details Magic, geometry fields and reserved runs; grid cross-checks stay
 *          with the footer phase.
 * @param[in]  pread     Backing read seam.
 * @param[in]  pread_ctx Context for @p pread.
 * @param[out] hdr       Receives the raw header bytes (for the count field).
 * @param[out] out_info  Geometry fields filled on success.
 * @return Result code.
 * @retval k_ra8_ok                    Header structurally valid.
 * @retval k_ra8_err_validation_failed Magic / geometry / reserved rejection.
 * @retval other                       Propagated from @p pread.
 * @pre @p hdr covers ::k_jof_hdr_bytes bytes.
 * @pre The backing size was validated by the caller.
 * @post On success the header geometry fields are populated.
 * @post On failure @p out_info must not be used.
 * @note Thread-safe for distinct outputs.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_parse_header_region(jof_pread_fn pread,
                                                           void*        pread_ctx,
                                                           uint8_t*     hdr,
                                                           jof_info_t*  out_info)
{
  static const uint8_t s_magic_hdr[k_jof_magic_len] = {'J', 'O', 'F', '1'};
  ra8_err_t err = internal_pread_exact(pread, pread_ctx, 0U, hdr, (size_t)k_jof_hdr_bytes);
  if (err != k_ra8_ok) {
    return err;
  }
  if (memcmp(&hdr[k_jof_ofs_magic], s_magic_hdr, sizeof(s_magic_hdr)) != 0) {
    return k_ra8_err_validation_failed;
  }
  err = internal_parse_hdr_fields(hdr, out_info);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_check_reserved(hdr);
}

ra8_err_t jof_parse(jof_pread_fn pread, void* pread_ctx, uint64_t total_size, jof_info_t* out_info)
{
  static const uint8_t s_magic_ftr[k_jof_magic_len] = {'J', 'O', 'F', 'E'};
  RA8_CHECK_NULL_PTR(pread, s_tag, "pread must not be nullptr");
  RA8_CHECK_NULL_PTR(out_info, s_tag, "out_info must not be nullptr");
  const uint64_t min_size = (uint64_t)k_jof_hdr_bytes + (uint64_t)k_jof_footer_bytes;
  if ((total_size < min_size) || (total_size > (uint64_t)UINT32_MAX)) {
    return k_ra8_err_invalid_size;
  }
  uint8_t   hdr[k_jof_hdr_bytes] = {};
  ra8_err_t err                  = internal_parse_header_region(pread, pread_ctx, hdr, out_info);
  if (err != k_ra8_ok) {
    return err;
  }
  uint8_t ftr[k_jof_footer_bytes] = {};
  err                             = internal_pread_exact(pread,
                                                         pread_ctx,
                                                         total_size - (uint64_t)k_jof_footer_bytes,
                                                         ftr,
                                                         sizeof(ftr));
  if (err != k_ra8_ok) {
    return err;
  }
  if (memcmp(&ftr[k_jof_ftr_magic], s_magic_ftr, sizeof(s_magic_ftr)) != 0) {
    return k_ra8_err_validation_failed;
  }
  const uint32_t hdr_count = internal_rd_u32(hdr, (uint8_t)k_jof_ofs_tile_count);
  return internal_check_cross(out_info, hdr_count, ftr, total_size);
}

/* ---------------------------------------------------------------------------
 * Tile access.
 * ---------------------------------------------------------------------------
 */

ra8_err_t jof_tile_dims(const jof_info_t* info,
                        uint16_t          tile_x,
                        uint16_t          tile_y,
                        uint16_t*         out_w,
                        uint16_t*         out_h)
{
  RA8_CHECK_NULL_PTR(info, s_tag, "info must not be nullptr");
  RA8_CHECK_NULL_PTR(out_w, s_tag, "out_w must not be nullptr");
  RA8_CHECK_NULL_PTR(out_h, s_tag, "out_h must not be nullptr");
  if (tile_x >= info->tile_cols) {
    return k_ra8_err_out_of_range;
  }
  if (tile_y >= info->tile_rows) {
    return k_ra8_err_out_of_range;
  }
  const uint32_t px     = (uint32_t)tile_x * (uint32_t)info->tile_w;
  const uint32_t py     = (uint32_t)tile_y * (uint32_t)info->tile_h;
  const uint32_t rest_w = (uint32_t)info->width - px;
  const uint32_t rest_h = (uint32_t)info->height - py;
  *out_w = (uint16_t)((rest_w < (uint32_t)info->tile_w) ? rest_w : (uint32_t)info->tile_w);
  *out_h = (uint16_t)((rest_h < (uint32_t)info->tile_h) ? rest_h : (uint32_t)info->tile_h);
  return k_ra8_ok;
}

/** @brief Implementation of `jof_stored_bound()` -- raw + raw/8 + 256. */
uint32_t jof_stored_bound(uint32_t raw_bytes)
{
  return raw_bytes + (raw_bytes / (uint32_t)k_jof_bound_div) + (uint32_t)k_jof_bound_add;
}

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
