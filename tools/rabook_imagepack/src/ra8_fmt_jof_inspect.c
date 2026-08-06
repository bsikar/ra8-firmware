/**
 * @file ra8_fmt_jof_inspect.c
 * @brief JOF structure dump, duplicate detection and round-trip verification.
 *
 * @details
 * The diagnostic half of the atlas verbs. `inspect` reports every header field,
 * every tile-index entry and the footer, then cross-checks the properties a
 * duplicated-content rendering bug would violate: exact stream coverage (no gap
 * and no overlap), each tile's decoded size matching its edge-clamped geometry,
 * and no two tiles decoding to identical NON-UNIFORM content. The uniformity
 * qualifier is load-bearing: solid-colour gutter bands legitimately repeat
 * throughout a real long-strip page, so flagging every identical pair would
 * report duplication on most genuine files. `verify` reassembles the banded atlas and
 * compares it against a single-tile encode of the same source -- the reference
 * decode -- so a mismatch localises the defect to the tiling path.
 *
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 *
 *

 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_fmt.h"
#include "ra8_fmt_internal.h"
#include "ra8_jof.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_fmt_jof_ins";

/**
 * @enum ra8_fmt_ins_const_t
 * @brief Inspection sizing, hashing and sink-margin constants.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_fmt_fnv_basis      = 2166136261U, /**< FNV-1a 32-bit offset basis.       */
  k_fmt_fnv_mul        = 16777619U,   /**< FNV-1a 32-bit prime.              */
  k_fmt_sink_floor     = 1048576U,    /**< Minimum atlas sink capacity.      */
  k_fmt_sink_scale     = 64U,         /**< Sink capacity multiple of source. */
  k_fmt_hdr_dump_bytes = 32U,         /**< Header bytes shown in the dump.   */
  k_fmt_ftr_dump_bytes = 16U,         /**< Footer bytes shown in the dump.   */
  k_fmt_ppm_maxval     = 255U,        /**< PPM maximum channel value.        */
} ra8_fmt_ins_const_t;

/**
 * @struct fmt_tile_rec_t
 * @brief One tile's index entry plus derived geometry, for cross-checking.
 * @details Collected for every tile so the coverage, duplication and geometry
 *          checks can run over the whole table rather than one entry at a time.
 * @invariant `offset + length <= atlas index_off` for a valid container.
 * @since 0.1.0
 */
typedef struct {
  uint32_t offset;  /**< Absolute stored-stream offset.        */
  uint32_t length;  /**< Stored stream length, bytes.          */
  uint32_t payload; /**< Decoded payload size (tw * th * bpp). */
  uint32_t hash;    /**< FNV-1a over the DECODED payload.      */
  uint16_t tw;      /**< Edge-clamped tile width.              */
  uint16_t th;      /**< Edge-clamped tile height.             */
  bool     uniform; /**< Every decoded byte is identical.      */
} fmt_tile_rec_t;

/**
 * @struct fmt_decode_buf_t
 * @brief Reusable decode buffers so inspection allocates once, not per tile.
 * @details `ra8_jof_read_tile` needs a pixel destination and (for the
 *          deflate codec) a stored-stream staging buffer. Both are sized from
 *          the atlas geometry and shared across every tile.
 * @invariant `cell_cap >= tile_w * tile_h * bpp` for the atlas being read.
 * @since 0.1.0
 */
typedef struct {
  uint8_t* cell;        /**< Decoded-pixel destination. */
  uint32_t cell_cap;    /**< Capacity of `cell`.        */
  uint8_t* scratch;     /**< Stored-stream staging.     */
  uint32_t scratch_cap; /**< Capacity of `scratch`.     */
} fmt_decode_buf_t;

size_t ra8_fmt_jof_sink_cap(size_t src_len)
{
  const size_t scaled = src_len * (size_t)k_fmt_sink_scale;
  return (scaled < (size_t)k_fmt_sink_floor) ? (size_t)k_fmt_sink_floor : scaled;
}

/**
 * @brief FNV-1a 32-bit hash over a byte window.
 * @details Cheap content fingerprint over a tile's DECODED payload, used to
 *          spot two tiles that decode to identical pixels.
 * @param[in] buf Bytes to hash (non-nullptr when @p len > 0).
 * @param[in] len Byte count.
 * @return The 32-bit digest.
 * @retval k_fmt_fnv_basis @p len was zero (the unmixed basis).
 * @pre @p buf holds @p len readable bytes.
 * @pre @p len is a real decoded-payload length.
 * @post No state is mutated.
 * @post Equal inputs always produce equal digests.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_fnv1a(const uint8_t* buf, size_t len)
{
  uint32_t h = (uint32_t)k_fmt_fnv_basis;
  for (size_t i = 0U; i < len; ++i) { /* bounded by len */
    h ^= (uint32_t)buf[i];
    h *= (uint32_t)k_fmt_fnv_mul;
  }
  return h;
}

/**
 * @brief Print the parsed header and footer geometry of an atlas.
 * @details Writes the decoded JOF geometry -- image and tile dimensions, grid,
 *          bpp, codec, tile count, index offset and total size -- as a labelled
 *          block, and flags the long-strip case where a tile spans the full
 *          image width. This is the header portion of an `inspect` dump.
 * @param[in] out  Report sink (non-nullptr).
 * @param[in] info Parsed atlas geometry (non-nullptr).
 * @param[in] len  Backing size in bytes.
 * @pre @p out is an open stream and @p info came from a successful parse.
 * @pre @p len is the real container length.
 * @post The geometry block was written to @p out.
 * @post No state other than the stream is mutated.
 * @note Not thread-safe (writes one stream).
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_print_geom(FILE* out, const ra8_jof_info_t* info, size_t len)
{
  (void)fprintf(out, "JOF atlas: %zu bytes\n", len);
  (void)fprintf(out, "  image      : %u x %u px\n", (unsigned)info->width, (unsigned)info->height);
  (void)fprintf(out, "  tile       : %u x %u px\n", (unsigned)info->tile_w, (unsigned)info->tile_h);
  (void)fprintf(out,
                "  grid       : %u cols x %u rows\n",
                (unsigned)info->tile_cols,
                (unsigned)info->tile_rows);
  (void)fprintf(out, "  bpp        : %u\n", (unsigned)info->bpp);
  (void)fprintf(out,
                "  codec      : %u (%s)\n",
                (unsigned)info->codec,
                (info->codec == (uint8_t)k_ra8_jof_codec_deflate) ? "deflate" : "raw");
  (void)fprintf(out, "  tile_count : %u\n", (unsigned)info->tile_count);
  (void)fprintf(out, "  index_off  : %u\n", (unsigned)info->index_off);
  (void)fprintf(out, "  total_size : %u\n", (unsigned)info->total_size);
  (void)fprintf(out,
                "  longstrip  : %s (tile_w %s width)\n",
                (info->tile_w == info->width) ? "YES" : "NO",
                (info->tile_w == info->width) ? "==" : "!=");
}

/**
 * @brief Decode one tile and record its content hash and uniformity.
 * @details Hashes the DECODED payload rather than the stored stream, because
 *          content equality is the property the duplication check is about.
 *          Uniformity (every byte identical) is recorded separately: a solid
 *          colour band -- a gutter between panels -- legitimately repeats many
 *          times in a real page, so identical uniform tiles are normal and must
 *          not be reported as duplication.
 * @param[in]  atlas Container bytes (non-nullptr).
 * @param[in]  info  Parsed geometry (non-nullptr).
 * @param[in]  idx   Tile index in row-major order.
 * @param[in]  buf   Reusable decode buffers (non-nullptr).
 * @param[in,out] rec Record whose `hash` and `uniform` are filled.
 * @return Result code propagated from `ra8_jof_read_tile()`.
 * @retval k_ra8_ok Tile decoded; hash and uniformity recorded.
 * @pre `rec->tw`/`rec->th`/`rec->payload` are already set.
 * @pre @p buf covers this atlas's worst-case tile.
 * @post On success `rec->hash` covers exactly `rec->payload` decoded bytes.
 * @post On any error @p rec is not relied upon.
 * @note Not thread-safe (writes the shared buffers).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_tile_content(const ra8_fmt_blob_t*   atlas,
                                   const ra8_jof_info_t*   info,
                                   uint32_t                idx,
                                   const fmt_decode_buf_t* buf,
                                   fmt_tile_rec_t*         rec)
{
  ra8_jof_memstore_t store = {.buf = atlas->bytes, .cap = atlas->len, .len = atlas->len};
  uint16_t           tw    = 0U;
  uint16_t           th    = 0U;
  const ra8_err_t    rc    = ra8_jof_read_tile(ra8_jof_memstore_pread,
                                         &store,
                                         info,
                                         (uint16_t)(idx % (uint32_t)info->tile_cols),
                                         (uint16_t)(idx / (uint32_t)info->tile_cols),
                                         buf->scratch,
                                         buf->scratch_cap,
                                         buf->cell,
                                         buf->cell_cap,
                                         &tw,
                                         &th);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rec->hash           = priv_fnv1a(buf->cell, (size_t)rec->payload);
  bool          same  = true;
  const uint8_t first = buf->cell[0];
  for (uint32_t i = 1U; (i < rec->payload) && same; ++i) { /* bounded by payload */
    same = (buf->cell[i] == first);
  }
  rec->uniform = same;
  return k_ra8_ok;
}

/**
 * @brief Fill one tile record from the index entry and the atlas geometry.
 * @details Reads the tile's stored offset and length from its index-table entry,
 *          derives its pixel dimensions and decoded-payload size from the grid
 *          geometry, and bounds-checks that the stored window lies inside the
 *          index region -- rejecting a container whose table points past its own
 *          payload.
 * @param[in]  atlas Container bytes (non-nullptr).
 * @param[in]  info  Parsed geometry (non-nullptr).
 * @param[in]  idx   Tile index in row-major order.
 * @param[out] rec   Receives the entry plus its derived geometry.
 * @return Result code.
 * @retval k_ra8_ok                    Record filled.
 * @retval k_ra8_err_validation_failed The entry window falls outside the atlas.
 * @pre @p idx is below `info->tile_count`.
 * @pre @p rec is writable.
 * @post On success `rec->offset + rec->length <= info->index_off`.
 * @post On any error @p rec is not relied upon.
 * @note Not thread-safe (writes @p rec).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_tile_rec(const ra8_fmt_blob_t* atlas,
                               const ra8_jof_info_t* info,
                               uint32_t              idx,
                               fmt_tile_rec_t*       rec)
{
  const size_t ent = (size_t)info->index_off + ((size_t)idx * (size_t)k_ra8_jof_index_entry);
  if ((ent + (size_t)k_ra8_jof_index_entry) > atlas->len) {
    return k_ra8_err_validation_failed;
  }
  rec->offset         = ra8_fmt_rd_u32(&atlas->bytes[ent + (size_t)k_ra8_jof_idx_ofs_offset]);
  rec->length         = ra8_fmt_rd_u32(&atlas->bytes[ent + (size_t)k_ra8_jof_idx_ofs_length]);
  const uint16_t  tx  = (uint16_t)(idx % (uint32_t)info->tile_cols);
  const uint16_t  ty  = (uint16_t)(idx / (uint32_t)info->tile_cols);
  const ra8_err_t drc = ra8_jof_tile_dims(info, tx, ty, &rec->tw, &rec->th);
  if (drc != k_ra8_ok) {
    return drc;
  }
  rec->payload = (uint32_t)rec->tw * (uint32_t)rec->th * (uint32_t)info->bpp;
  if (((size_t)rec->offset + (size_t)rec->length) > (size_t)info->index_off) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Cross-check the tile table for gaps, overlaps and identical payloads.
 * @details Tiles are emitted back-to-back in row-major order, so a correct
 *          table has `offset[n] == offset[n-1] + length[n-1]` throughout. Any
 *          break is a gap or an overlap; any repeated hash is duplicated
 *          content inside the file itself.
 * @param[in]  recs  Tile records (non-nullptr, `count` entries).
 * @param[in]  count Tile count.
 * @param[in]  first Offset the first tile must start at (header length).
 * @param[out] out   Report sink for the findings (non-nullptr).
 * @return Result code.
 * @retval k_ra8_ok                    Coverage exact and no duplicate payloads.
 * @retval k_ra8_err_validation_failed A gap, overlap or duplicate was found.
 * @pre @p recs holds @p count filled records.
 * @pre @p out is an open stream.
 * @post Every anomaly found was written to @p out.
 * @post No state other than the stream is mutated.
 * @note Not thread-safe (writes one stream).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_check_table(const fmt_tile_rec_t* recs, uint32_t count, uint32_t first, FILE* out)
{
  ra8_err_t verdict = k_ra8_ok;
  uint32_t  cursor  = first;
  for (uint32_t i = 0U; i < count; ++i) { /* bounded by tile_count */
    if (recs[i].offset != cursor) {
      (void)fprintf(out,
                    "  ANOMALY tile %u: offset %u but previous tile ends at %u (%s)\n",
                    (unsigned)i,
                    (unsigned)recs[i].offset,
                    (unsigned)cursor,
                    (recs[i].offset < cursor) ? "OVERLAP" : "GAP");
      verdict = k_ra8_err_validation_failed;
    }
    cursor = recs[i].offset + recs[i].length;
  }
  /* Identical CONTENT is only suspicious when the band actually carries detail.
   * A solid-colour band -- the gutter between panels -- is uniform, and a real
   * long-strip page repeats those many times over. Reporting them would cry
   * wolf on most genuine pages, which is worse for debugging than not checking:
   * the reader would learn to ignore the verdict. */
  uint32_t uniform_pairs = 0U;
  for (uint32_t i = 1U; i < count; ++i) { /* bounded by tile_count */
    for (uint32_t j = 0U; j < i; ++j) {   /* bounded by i          */
      if ((recs[i].hash != recs[j].hash) || (recs[i].payload != recs[j].payload)) {
        continue;
      }
      if (recs[i].uniform && recs[j].uniform) {
        uniform_pairs++;
        continue;
      }
      (void)fprintf(out,
                    "  ANOMALY tiles %u and %u decode to IDENTICAL non-uniform "
                    "content (%u bytes, hash=%08X) -- duplication is in the FILE\n",
                    (unsigned)j,
                    (unsigned)i,
                    (unsigned)recs[i].payload,
                    (unsigned)recs[i].hash);
      verdict = k_ra8_err_validation_failed;
    }
  }
  if (uniform_pairs > 0U) {
    (void)fprintf(out,
                  "  note: %u tile pair(s) share solid-colour content "
                  "(gutter bands -- legitimate, not duplication)\n",
                  (unsigned)uniform_pairs);
  }
  return verdict;
}

/**
 * @brief Print the per-tile table, eliding beyond ::k_ra8_fmt_max_dump_rows.
 * @details Writes one row per tile -- index, stored offset and length, decoded
 *          payload size, tile pixel dimensions and content hash -- capping the
 *          output at ::k_ra8_fmt_max_dump_rows and printing an elision note so a
 *          large atlas does not flood the report.
 * @param[in] out   Report sink (non-nullptr).
 * @param[in] recs  Tile records (non-nullptr).
 * @param[in] count Tile count.
 * @pre @p out is an open stream and @p recs holds @p count records.
 * @pre The caller already ran the cross-checks.
 * @post At most ::k_ra8_fmt_max_dump_rows rows were written.
 * @post No state other than the stream is mutated.
 * @note Not thread-safe (writes one stream).
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_print_table(FILE* out, const fmt_tile_rec_t* recs, uint32_t count)
{
  (void)fprintf(out, "  tile      offset    length   payload   tw    th   hash\n");
  uint32_t shown = count;
  if (shown > (uint32_t)k_ra8_fmt_max_dump_rows) {
    shown = (uint32_t)k_ra8_fmt_max_dump_rows;
  }
  for (uint32_t i = 0U; i < shown; ++i) { /* bounded by k_ra8_fmt_max_dump_rows */
    (void)fprintf(out,
                  "  %6u %9u %9u %9u %5u %5u   %08X\n",
                  (unsigned)i,
                  (unsigned)recs[i].offset,
                  (unsigned)recs[i].length,
                  (unsigned)recs[i].payload,
                  (unsigned)recs[i].tw,
                  (unsigned)recs[i].th,
                  (unsigned)recs[i].hash);
  }
  if (shown < count) {
    (void)fprintf(out, "  ... %u more tiles elided\n", (unsigned)(count - shown));
  }
}

/**
 * @brief Fill every tile record: index entry, geometry, content hash, uniformity.
 * @details Owns the shared decode buffers for the whole walk so inspection
 *          allocates once rather than per tile, and releases them on every exit
 *          path including the error ones.
 * @param[in,out] arena Bump allocator for decode buffers.
 * @param[in]     atlas Container bytes (non-nullptr).
 * @param[in]  info  Parsed geometry (non-nullptr).
 * @param[out] recs  Array of `info->tile_count` records to fill (non-nullptr).
 * @param[in]  opts  Options carrying the report sink (non-nullptr).
 * @return Result code.
 * @retval k_ra8_ok         Every record filled.
 * @retval k_ra8_err_no_mem A decode buffer could not be allocated.
 * @retval other            Propagated from the index walk or the tile decode.
 * @pre @p recs holds `info->tile_count` writable records.
 * @pre @p info came from a successful parse over @p atlas.
 * @post On success every record carries a decoded-content hash.
 * @post No decode buffer outlives the call.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_collect_tiles(ra8_arena_t*          arena,
                                    const ra8_fmt_blob_t* atlas,
                                    const ra8_jof_info_t* info,
                                    fmt_tile_rec_t*       recs,
                                    const ra8_fmt_opts_t* opts)
{
  const uint32_t   tmax = (uint32_t)info->tile_w * (uint32_t)info->tile_h * (uint32_t)info->bpp;
  fmt_decode_buf_t buf  = {.cell =
                             (uint8_t*)ra8_arena_alloc(arena, (uint32_t)tmax, k_ra8_arena_align),
                           .cell_cap    = tmax,
                           .scratch     = nullptr,
                           .scratch_cap = ra8_jof_stored_bound(tmax)};
  buf.scratch = (uint8_t*)ra8_arena_alloc(arena, (uint32_t)buf.scratch_cap, k_ra8_arena_align);
  if ((buf.cell == nullptr) || (buf.scratch == nullptr)) {
    return k_ra8_err_no_mem;
  }
  ra8_err_t rc = k_ra8_ok;
  for (uint32_t i = 0U; (rc == k_ra8_ok) && (i < info->tile_count); ++i) { /* bounded */
    rc = priv_tile_rec(atlas, info, i, &recs[i]);
    if (rc == k_ra8_ok) {
      rc = priv_tile_content(atlas, info, i, &buf, &recs[i]);
    }
    if (rc != k_ra8_ok) {
      (void)fprintf(opts->report, "  tile %u could not be read or decoded\n", (unsigned)i);
    }
  }
  return rc;
}

ra8_err_t
ra8_fmt_jof_inspect(ra8_arena_t* arena, const ra8_fmt_blob_t* src, const ra8_fmt_opts_t* opts)
{
  RA8_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  RA8_CHECK_NULL_PTR(opts, s_tag, "opts must not be nullptr");
  ra8_jof_memstore_t store = {.buf = src->bytes, .cap = src->len, .len = src->len};
  ra8_jof_info_t     info  = {};
  ra8_err_t          rc = ra8_jof_parse(ra8_jof_memstore_pread, &store, (uint64_t)src->len, &info);
  if (rc != k_ra8_ok) {
    (void)fprintf(opts->report, "JOF parse FAILED (rc=%d) -- container is invalid\n", (int)rc);
    ra8_fmt_hex_dump(opts->report,
                     "header",
                     src->bytes,
                     (src->len < (size_t)k_fmt_hdr_dump_bytes) ? src->len
                                                               : (size_t)k_fmt_hdr_dump_bytes,
                     0U);
    return rc;
  }
  priv_print_geom(opts->report, &info, src->len);
  fmt_tile_rec_t* recs =
    (fmt_tile_rec_t*)ra8_arena_calloc(arena, (uint32_t)info.tile_count, sizeof(*recs));
  if (recs == nullptr) {
    return k_ra8_err_no_mem;
  }
  rc = priv_collect_tiles(arena, src, &info, recs, opts);
  if (rc != k_ra8_ok) {
    return rc;
  }
  if (opts->verbose) {
    ra8_fmt_hex_dump(opts->report, "header", src->bytes, (size_t)k_fmt_hdr_dump_bytes, 0U);
    ra8_fmt_hex_dump(opts->report,
                     "footer",
                     &src->bytes[src->len - (size_t)k_fmt_ftr_dump_bytes],
                     (size_t)k_fmt_ftr_dump_bytes,
                     (uint64_t)(src->len - (size_t)k_fmt_ftr_dump_bytes));
    priv_print_table(opts->report, recs, info.tile_count);
  }
  const ra8_err_t verdict =
    priv_check_table(recs, info.tile_count, (uint32_t)k_ra8_jof_hdr_bytes, opts->report);
  (void)fprintf(opts->report,
                "verdict: %s\n",
                (verdict == k_ra8_ok) ? "VALID (coverage exact, no duplicate tiles)"
                                      : "INVALID (see anomalies above)");
  return verdict;
}

/**
 * @brief Copy one decoded tile into its place in the full-page raster.
 *
 * @details
 * Walks the tile's rows, mapping each to its canvas row. An edge tile is
 * narrower or shorter than the nominal tile size, so the row length comes from
 * the decoded @p tw / @p th rather than from the atlas geometry.
 *
 * @param[out] px     Full-page raster to write into.
 * @param[in]  stride Raster stride in bytes.
 * @param[in]  info   Atlas geometry (tile size and bytes per pixel).
 * @param[in]  tx     Tile column in the grid.
 * @param[in]  ty     Tile row in the grid.
 * @param[in]  cell   Decoded tile payload, @p tw by @p th.
 * @param[in]  tw     Decoded tile width in pixels, already edge-clamped.
 * @param[in]  th     Decoded tile height in pixels, already edge-clamped.
 * @return None.
 *
 * @pre All pointer arguments are non-null.
 * @pre The tile at (@p tx, @p ty) with extent @p tw by @p th lies inside @p px.
 * @post The tile's pixels occupy their canvas position in @p px.
 * @post @p cell is unmodified.
 *
 * @note Not thread-safe with respect to @p px.
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_blit_tile(uint8_t*              px,
                           size_t                stride,
                           const ra8_jof_info_t* info,
                           uint32_t              tx,
                           uint32_t              ty,
                           const uint8_t*        cell,
                           uint16_t              tw,
                           uint16_t              th)
{
  const size_t x0 = (size_t)tx * (size_t)info->tile_w * (size_t)info->bpp;
  const size_t y0 = (size_t)ty * (size_t)info->tile_h;
  for (uint32_t r = 0U; r < th; ++r) { /* bounded by tile_h */
    (void)memcpy(&px[((y0 + (size_t)r) * stride) + x0],
                 &cell[(size_t)r * (size_t)tw * (size_t)info->bpp],
                 (size_t)tw * (size_t)info->bpp);
  }
}

/**
 * @brief Decode every tile of the atlas into the full-page raster.
 *
 * @details
 * Walks the tile grid in row-major order, decoding each tile through
 * ::ra8_jof_read_tile into @p cell and blitting it into @p px. The walk
 * stops at the first tile that fails to decode, so a truncated or corrupt
 * container reports its own error rather than a partial raster.
 *
 * @param[in,out] store     Memstore reader over the atlas bytes.
 * @param[in]     info      Parsed atlas geometry.
 * @param[out]    px        Full-page raster to fill.
 * @param[in]     stride    Raster stride in bytes.
 * @param[out]    cell      Scratch buffer for one decoded tile.
 * @param[in]     tile_max  Capacity of @p cell in bytes.
 * @param[out]    scratch   Scratch buffer for the stored (undecoded) tile.
 * @param[in]     scratch_c Capacity of @p scratch in bytes.
 * @return Error code from the first failing tile, or success.
 * @retval k_ra8_ok Every tile decoded and landed in @p px.
 *
 * @pre All pointer arguments are non-null.
 * @pre @p cell and @p scratch are sized by the caller from @p info.
 * @post On success @p px holds the whole decoded page.
 * @post On failure @p px holds an unspecified partial result.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_reassemble_tiles(ra8_jof_memstore_t*   store,
                                       const ra8_jof_info_t* info,
                                       uint8_t*              px,
                                       size_t                stride,
                                       uint8_t*              cell,
                                       uint32_t              tile_max,
                                       uint8_t*              scratch,
                                       uint32_t              scratch_c)
{
  for (uint32_t ty = 0U; ty < info->tile_rows; ++ty) {
    for (uint32_t tx = 0U; tx < info->tile_cols; ++tx) {
      uint16_t        tw = 0U;
      uint16_t        th = 0U;
      const ra8_err_t rc = ra8_jof_read_tile(ra8_jof_memstore_pread,
                                             store,
                                             info,
                                             (uint16_t)tx,
                                             (uint16_t)ty,
                                             scratch,
                                             scratch_c,
                                             cell,
                                             tile_max,
                                             &tw,
                                             &th);
      if (rc != k_ra8_ok) {
        return rc;
      }
      priv_blit_tile(px, stride, info, tx, ty, cell, tw, th);
    }
  }
  return k_ra8_ok;
}

/**
 * @struct fmt_reassemble_ws_t
 * @brief Everything one reassembly pass needs: the reader, the sizes, the buffers.
 *
 * @details
 * The three buffers are not independent -- each is sized from the same parsed
 * geometry, and none is useful without the others -- so they travel with the
 * sizes that produced them rather than as six loose arguments. Grouping them
 * also makes the ownership split visible: @ref cell and @ref scratch die with
 * the pass, while @ref px is what the caller walks away with.
 *
 * @invariant After a successful ::priv_open_reassemble_ws all three pointers
 *            are non-nullptr; after a failed one all three are nullptr.
 * @invariant @ref raster equals @ref stride times the atlas height.
 *
 * @par Example:
 * @code
 * fmt_reassemble_ws_t ws = {};
 * if (priv_open_reassemble_ws(arena, atlas, info, &ws)) {
 *   // ... use ws.px ...
 * }
 * @endcode
 *
 * @see priv_open_reassemble_ws()  Builds one.
 * @see priv_reassemble_tiles()    Consumes one.
 */
typedef struct {
  ra8_jof_memstore_t store;     /**< Reader over the atlas bytes.           */
  size_t             stride;    /**< Raster stride in bytes.                */
  size_t             raster;    /**< Full-page raster size in bytes.        */
  uint32_t           tile_max;  /**< Decoded-tile buffer size in bytes.     */
  uint32_t           scratch_c; /**< Stored-tile scratch size in bytes.     */
  uint8_t*           px;        /**< Zeroed full-page raster; caller keeps. */
  uint8_t*           cell;      /**< One decoded tile.                      */
  uint8_t*           scratch;   /**< One stored tile.                       */
} fmt_reassemble_ws_t;

/**
 * @brief Derive the reassembly geometry and acquire its buffers, all or nothing.
 *
 * @details
 * Sizes every buffer from the parsed geometry, then allocates all three. A
 * partial allocation frees whatever succeeded and reports failure, so the
 * caller's cleanup never has to tell "never allocated" from "allocated then
 * failed". The raster is zeroed because a decode that stops early must not
 * expose uninitialised memory as image data.
 *
 * @param[in,out] arena Bump allocator for the workspace buffers.
 * @param[in]     atlas Container bytes the reader is opened over (non-nullptr).
 * @param[in]     info  Parsed geometry the sizes come from (non-nullptr).
 * @param[out] ws    Receives the reader, the sizes, and the three buffers.
 *
 * @return Whether the workspace is ready to use.
 * @retval true  All three buffers are allocated and @p ws is complete.
 * @retval false Nothing is allocated; @p ws holds nullptr buffer pointers.
 *
 * @pre @p ws is non-nullptr.
 * @pre @p info came from a successful parse over @p atlas.
 * @post On success the caller owns `ws->px`, `ws->cell` and `ws->scratch`.
 * @post On failure no allocation outlives the call.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_open_reassemble_ws(ra8_arena_t*          arena,
                                    const ra8_fmt_blob_t* atlas,
                                    const ra8_jof_info_t* info,
                                    fmt_reassemble_ws_t*  ws)
{
  ws->store     = (ra8_jof_memstore_t){.buf = atlas->bytes, .cap = atlas->len, .len = atlas->len};
  ws->stride    = (size_t)info->width * (size_t)info->bpp;
  ws->raster    = ws->stride * (size_t)info->height;
  ws->tile_max  = (uint32_t)info->tile_w * (uint32_t)info->tile_h * (uint32_t)info->bpp;
  ws->scratch_c = ra8_jof_stored_bound(ws->tile_max);

  ws->px      = (uint8_t*)ra8_arena_calloc(arena, (uint32_t)ws->raster, 1U);
  ws->cell    = (uint8_t*)ra8_arena_alloc(arena, (uint32_t)ws->tile_max, k_ra8_arena_align);
  ws->scratch = (uint8_t*)ra8_arena_alloc(arena, (uint32_t)ws->scratch_c, k_ra8_arena_align);
  if ((ws->px != nullptr) && (ws->cell != nullptr) && (ws->scratch != nullptr)) {
    return true;
  }
  ws->px      = nullptr;
  ws->cell    = nullptr;
  ws->scratch = nullptr;
  return false;
}

ra8_err_t ra8_fmt_jof_reassemble(ra8_arena_t*          arena,
                                 const ra8_fmt_blob_t* atlas,
                                 const ra8_jof_info_t* info,
                                 uint8_t**             out_px,
                                 size_t*               out_len)
{
  RA8_CHECK_NULL_PTR(atlas, s_tag, "atlas must not be nullptr");
  RA8_CHECK_NULL_PTR(info, s_tag, "info must not be nullptr");
  RA8_CHECK_NULL_PTR(out_px, s_tag, "out_px must not be nullptr");
  RA8_CHECK_NULL_PTR(out_len, s_tag, "out_len must not be nullptr");
  *out_px                = nullptr;
  fmt_reassemble_ws_t ws = {};
  if (!priv_open_reassemble_ws(arena, atlas, info, &ws)) {
    return k_ra8_err_no_mem;
  }
  const ra8_err_t rc = priv_reassemble_tiles(&ws.store,
                                             info,
                                             ws.px,
                                             ws.stride,
                                             ws.cell,
                                             ws.tile_max,
                                             ws.scratch,
                                             ws.scratch_c);
  if (rc != k_ra8_ok) {
    return rc;
  }
  *out_px  = ws.px;
  *out_len = ws.raster;
  return k_ra8_ok;
}
