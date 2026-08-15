/**
 * @file test_ra8_jof_edges.c
 * @brief Partial-edge-tile round-trip and tile-distinctness guards (#231, #289).
 *
 * @details
 * A rendering defect that duplicates or tears image content has two candidate
 * homes: the producer wrote a bad container, or the consumer read a good one
 * wrongly. These tests pin down the producer half so the question can be
 * settled by elimination.
 *
 * The source is 90x70 cut into 32x32 tiles, so NEITHER dimension is a whole
 * multiple of the tile edge: the grid is 3 cols x 3 rows, the right column is
 * 26 px wide (90 - 2*32) and the bottom row is 6 px tall (70 - 2*32). Four of
 * the nine tiles are partial, and the corner tile is partial in both
 * directions -- the geometry that exercises every edge-clamp arm at once.
 *
 * Three properties are asserted, because a defect can hide in any one of them
 * independently:
 *   1. Each tile reports and decodes its edge-clamped extent. An edge tile that
 *      silently carried a full `tile_w`/`tile_h` payload would tear the image
 *      at every band boundary downstream.
 *   2. Every tile decodes to the source pixels at its true canvas position --
 *      the image -> atlas -> decode round trip.
 *   3. No two distinct tiles decode to identical payloads. The source pattern
 *      is chosen so every tile really is unique (see ::internal_t_pix), so a repeated
 *      payload can only mean the producer emitted one band into two index
 *      slots: duplication baked into the file.
 *
 * The PNG source is built in-test with an encoder independent of the decoder
 * under test (filters off, IDAT via miniz `mz_compress`), matching the sibling
 * producer tests.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_jof.h"
#include "ra8_jof_produce.h"
#include "unity_minimal.h"

/**
 * @enum t_edge_geom_t
 * @brief Source geometry, tiling and buffer sizing for the edge tests.
 *
 * @details 90x70 over a 32x32 tile grid gives 3x3 tiles with a 26-px right
 *          column and a 6-px bottom row, so the partial-edge path runs in both
 *          axes and, at the corner, in both at once.
 *
 * @invariant `k_te_w % k_te_tile != 0` and `k_te_h % k_te_tile != 0` -- the
 *            whole point of this fixture is that neither divides evenly.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_te_w         = 90U,          /**< Source width, pixels (2*32 + 26).           */
  k_te_h         = 70U,          /**< Source height, pixels (2*32 + 6).           */
  k_te_tile      = 32U,          /**< Tile edge, pixels.                          */
  k_te_cols      = 3U,           /**< Expected tile columns.                      */
  k_te_rows      = 3U,           /**< Expected tile rows.                         */
  k_te_tiles     = 9U,           /**< Expected tile count.                        */
  k_te_edge_col  = 2U,           /**< Grid column that is clamped.                */
  k_te_edge_row  = 2U,           /**< Grid row that is clamped.                   */
  k_te_src_cap   = 256U * 1024U, /**< Synthesized PNG capacity.                   */
  k_te_store_cap = 512U * 1024U, /**< Atlas memstore capacity.                    */
  k_te_work_cap  = 900U * 1024U, /**< Producer work arena.                        */
  k_te_cell_cap  = 64U * 1024U,  /**< Tile page-back buffer.                      */
  k_te_scr_cap   = 96U * 1024U,  /**< Stored-tile staging.                        */
  k_te_mul_x     = 7U,           /**< Pattern x coefficient (see internal_t_pix). */
  k_te_mul_y     = 29U,          /**< Pattern y coefficient (see internal_t_pix). */
  k_te_png_hdr   = 13U,          /**< IHDR payload length.                        */
  k_te_bitdepth  = 8U,           /**< PNG bit depth.                              */
  k_te_ct_gray   = 0U,           /**< PNG colour type 0 (grayscale).              */
  k_te_chunk_ovh = 12U,          /**< PNG chunk length+type+crc bytes.            */
  k_te_byte_mask = 0xFFU,        /**< Low-byte mask.                              */
  k_te_sh8       = 8U,           /**< One-byte shift.                             */
  k_te_sh16      = 16U,          /**< Two-byte shift.                             */
  k_te_sh24      = 24U,          /**< Three-byte shift.                           */
} t_edge_geom_t;

/** @brief Byte offsets of the fields inside a PNG IHDR payload. */
typedef enum : uint8_t {
  k_te_ihdr_off_width     = 0U, /**< Big-endian 32-bit image width.  */
  k_te_ihdr_off_height    = 4U, /**< Big-endian 32-bit image height. */
  k_te_ihdr_off_bitdepth  = 8U, /**< Bits per sample.                */
  k_te_ihdr_off_colortype = 9U, /**< PNG colour-type code.           */
} t_edge_ihdr_off_t;

/** @brief Synthesized PNG source bytes. */
static uint8_t s_src[k_te_src_cap];
/** @brief Length of the synthesized PNG in ::s_src. */
static size_t s_src_len;
/** @brief Unfiltered scanline scratch for the PNG builder. */
static uint8_t s_raw[(size_t)k_te_h * ((size_t)k_te_w + 1U)];
/** @brief zlib staging for the PNG builder. */
static uint8_t s_zbuf[k_te_src_cap / 2U];
/** @brief Producer work arena. */
static uint8_t s_work[k_te_work_cap];
/** @brief Atlas memstore backing. */
static uint8_t s_store_buf[k_te_store_cap];
/** @brief The atlas store under test. */
static ra8_jof_memstore_t s_store;
/** @brief Tile page-back buffer. */
static uint8_t s_cell[k_te_cell_cap];
/** @brief Stored-tile staging for the deflate codec. */
static uint8_t s_scratch[k_te_scr_cap];
/** @brief Decoded payload of each tile, for the distinctness comparison. */
static uint8_t s_tile_px[k_te_tiles][(size_t)k_te_tile * (size_t)k_te_tile];
/** @brief Decoded payload length of each tile in ::s_tile_px. */
static uint32_t s_tile_len[k_te_tiles];

/**
 * @brief Deterministic source pixel at (x, y): asymmetric by construction.
 *
 * @details `7x + 29y` is deliberately NOT symmetric in x and y. A symmetric
 *          pattern (such as `x ^ y`) makes tiles either side of the grid
 *          diagonal identical in the SOURCE, which would fail the distinctness
 *          property for a reason that is not a producer defect. With these
 *          coefficients the per-tile base offsets (`7*32*tx + 29*32*ty` mod 256)
 *          are distinct for every same-size tile pair in this 3x3 grid, so no
 *          two tiles can legitimately coincide.
 *
 * @param[in] x Column, `[0, k_te_w)`.
 * @param[in] y Row, `[0, k_te_h)`.
 *
 * @return The 8-bit grayscale sample at that position.
 * @retval 0 Only where `7x + 29y` is a multiple of 256.
 *
 * @pre @p x and @p y are inside the source extent.
 * @pre The caller uses the same oracle for both build and check.
 * @post No state is mutated.
 * @post Equal coordinates always produce equal samples.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t internal_t_pix(uint32_t x, uint32_t y)
{
  return (uint8_t)((x * (uint32_t)k_te_mul_x) + (y * (uint32_t)k_te_mul_y));
}

/**
 * @brief Store @p v as a PNG big-endian 32-bit field at @p p.
 * @details Every 32-bit field in a PNG -- chunk length, chunk CRC, IHDR width
 *          and height -- is network byte order, so they all go through here
 *          rather than repeating four shift-and-mask stores per field.
 * @param[out] p Four writable bytes.
 * @param[in]  v Value to store.
 * @pre @p p points at four writable bytes.
 * @pre @p p is not aliased by @p v's storage.
 * @post @p p holds @p v most-significant byte first.
 * @post No other memory is touched.
 * @note Not thread-safe with respect to @p p.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_t_put_be32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)((v >> (uint32_t)k_te_sh24) & (uint32_t)k_te_byte_mask);
  p[1] = (uint8_t)((v >> (uint32_t)k_te_sh16) & (uint32_t)k_te_byte_mask);
  p[2] = (uint8_t)((v >> (uint32_t)k_te_sh8) & (uint32_t)k_te_byte_mask);
  p[3] = (uint8_t)(v & (uint32_t)k_te_byte_mask);
}

/**
 * @brief Append one PNG chunk (length, type, data, CRC) to ::s_src.
 * @param[in] type Four-character chunk type (non-NULL).
 * @param[in] data Chunk payload (may be NULL when @p len is 0).
 * @param[in] len  Payload length, bytes.
 * @pre ::s_src has room for `len + 12` more bytes.
 * @pre @p type points at 4 readable characters.
 * @post ::s_src_len advanced by `len + 12`.
 * @post The appended chunk carries a valid CRC-32.
 * @note Not thread-safe (shared fixture).
 * @since 0.1.0 @details Exercises the t png chunk path with bounded caller-owned fixture state and verifies its documented result. */
RA8_INTERNAL static void internal_t_png_chunk(const char* type, const uint8_t* data, uint32_t len)
{
  uint8_t* p = &s_src[s_src_len];
  internal_t_put_be32(p, len);
  memcpy(&p[4], type, 4U);
  if (len > 0U) {
    memcpy(&p[8], data, len);
  }
  const uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, &p[4], (size_t)len + 4U);
  internal_t_put_be32(&p[8U + len], crc);
  s_src_len += (size_t)k_te_chunk_ovh + (size_t)len;
}

/**
 * @brief Build the 90x70 grayscale PNG source into ::s_src.
 * @details Filter type 0 on every row (the filtering itself is covered by the
 *          sibling producer tests); IDAT compressed with miniz, an encoder
 *          independent of the inflater under test.
 * @pre ::s_src and ::s_zbuf are large enough for this geometry.
 * @pre ::s_raw covers `h * (1 + w)` bytes.
 * @post ::s_src holds a complete, spec-valid PNG of ::internal_t_pix.
 * @post ::s_src_len is the PNG byte length.
 * @note Not thread-safe (shared fixtures).
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_t_build_png(void)
{
  size_t o = 0U;
  for (uint32_t y = 0U; y < (uint32_t)k_te_h; y++) {
    s_raw[o] = 0U; /* filter type 0 (None) */
    o++;
    for (uint32_t x = 0U; x < (uint32_t)k_te_w; x++) {
      s_raw[o] = internal_t_pix(x, y);
      o++;
    }
  }
  mz_ulong zlen = (mz_ulong)sizeof(s_zbuf);
  TEST_ASSERT_EQ(MZ_OK, mz_compress(s_zbuf, &zlen, s_raw, (mz_ulong)o));

  static const uint8_t sig[8] = {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU};
  s_src_len                   = sizeof(sig);
  memcpy(s_src, sig, sizeof(sig));

  uint8_t ihdr[k_te_png_hdr] = {};
  internal_t_put_be32(&ihdr[k_te_ihdr_off_width], (uint32_t)k_te_w);
  internal_t_put_be32(&ihdr[k_te_ihdr_off_height], (uint32_t)k_te_h);
  ihdr[k_te_ihdr_off_bitdepth]  = (uint8_t)k_te_bitdepth;
  ihdr[k_te_ihdr_off_colortype] = (uint8_t)k_te_ct_gray;
  internal_t_png_chunk("IHDR", ihdr, (uint32_t)k_te_png_hdr);
  internal_t_png_chunk("IDAT", s_zbuf, (uint32_t)zlen);
  internal_t_png_chunk("IEND", nullptr, 0U);
}

/**
 * @struct t_pull_ctx_t
 * @brief Read cursor handing ::s_src to the producer pull seam.
 * @invariant `pos <= n` at all times.
 * @since 0.1.0
 */
typedef struct {
  const uint8_t* d;   /**< Source bytes.  */
  size_t         n;   /**< Source length. */
  size_t         pos; /**< Read cursor.   */
} t_pull_ctx_t;

/** @brief ::ra8_jof_pull_fn over a ::t_pull_ctx_t. @details Exercises the t pull path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] ctx Injected callback context whose ownership remains with the test. @param[out] buf Byte buffer read or written by the exercised callback. @param[in] cap Capacity of the associated byte buffer in bytes. @param[out] got Receives the number of bytes transferred. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_t_pull(void* ctx, uint8_t* buf, size_t cap, size_t* got)
{
  t_pull_ctx_t* p    = (t_pull_ctx_t*)ctx;
  const size_t  left = p->n - p->pos;
  const size_t  take = (cap < left) ? cap : left;
  memcpy(buf, &p->d[p->pos], take);
  p->pos += take;
  *got = take;
  return k_ra8_ok;
}

/**
 * @brief Transcode ::s_src into ::s_store with the 32x32 tiling under test.
 * @param[out] info Receives the produced atlas geometry (non-NULL).
 * @return Result code from `ra8_jof_produce()`.
 * @retval k_ra8_ok The store holds a complete atlas.
 * @pre ::s_src holds the built PNG.
 * @pre @p info is writable.
 * @post On success ::s_store holds the produced atlas.
 * @post The work arena is not referenced after return.
 * @note Not thread-safe (shared fixtures).
 * @since 0.1.0 @details Exercises the t produce path with bounded caller-owned fixture state and verifies its documented result. */
RA8_INTERNAL static ra8_err_t internal_t_produce(ra8_jof_info_t* info)
{
  static t_pull_ctx_t local_pull;
  local_pull = (t_pull_ctx_t){.d = s_src, .n = s_src_len, .pos = 0U};
  s_store    = (ra8_jof_memstore_t){.buf = s_store_buf, .cap = sizeof(s_store_buf), .len = 0U};
  const ra8_jof_produce_cfg_t cfg = {
    .pull       = internal_t_pull,
    .pull_ctx   = &local_pull,
    .sink       = ra8_jof_memstore_sink,
    .sink_ctx   = &s_store,
    .tile_w     = (uint16_t)k_te_tile,
    .tile_h     = (uint16_t)k_te_tile,
    .codec      = (uint8_t)k_ra8_jof_codec_deflate,
    .max_width  = (uint16_t)k_te_w,
    .max_height = (uint16_t)k_te_h,
    .work       = s_work,
    .work_cap   = (size_t)k_te_work_cap,
  };
  return ra8_jof_produce(&cfg, info);
}

/**
 * @brief The edge-clamped width this grid column must report.
 * @param[in] tx Grid column index.
 * @return The clamped tile width, pixels.
 * @retval k_te_tile @p tx is not the clamped column.
 * @pre @p tx is below ::k_te_cols.
 * @pre The fixture geometry is 90 wide over 32-px tiles.
 * @post No state is mutated.
 * @post The result is in `[1, k_te_tile]`.
 * @note Pure; thread-safe.
 * @since 0.1.0 @details Exercises the t want w path with bounded caller-owned fixture state and verifies its documented result. */
RA8_INTERNAL static uint16_t internal_t_want_w(uint16_t tx)
{
  return (tx == (uint16_t)k_te_edge_col)
           ? (uint16_t)((uint32_t)k_te_w - ((uint32_t)k_te_edge_col * (uint32_t)k_te_tile))
           : (uint16_t)k_te_tile;
}

/**
 * @brief The edge-clamped height this grid row must report.
 * @param[in] ty Grid row index.
 * @return The clamped tile height, pixels.
 * @retval k_te_tile @p ty is not the clamped row.
 * @pre @p ty is below ::k_te_rows.
 * @pre The fixture geometry is 70 tall over 32-px tiles.
 * @post No state is mutated.
 * @post The result is in `[1, k_te_tile]`.
 * @note Pure; thread-safe.
 * @since 0.1.0 @details Exercises the t want h path with bounded caller-owned fixture state and verifies its documented result. */
RA8_INTERNAL static uint16_t internal_t_want_h(uint16_t ty)
{
  return (ty == (uint16_t)k_te_edge_row)
           ? (uint16_t)((uint32_t)k_te_h - ((uint32_t)k_te_edge_row * (uint32_t)k_te_tile))
           : (uint16_t)k_te_tile;
}

/**
 * @brief Page one tile back through the reader into ::s_cell.
 * @param[in]  info  Produced atlas geometry (non-NULL).
 * @param[in]  tx    Tile column.
 * @param[in]  ty    Tile row.
 * @param[out] out_w Receives the decoded tile width.
 * @param[out] out_h Receives the decoded tile height.
 * @pre ::s_store holds the produced atlas.
 * @pre @p out_w and @p out_h are writable.
 * @post ::s_cell holds `(*out_w) * (*out_h) * bpp` valid bytes.
 * @post The store is unmodified.
 * @note Not thread-safe (shared fixtures).
 * @since 0.1.0 @details Exercises the t read path with bounded caller-owned fixture state and verifies its documented result. */
RA8_INTERNAL static void internal_t_read(const ra8_jof_info_t* info,
                                         uint16_t              tx,
                                         uint16_t              ty,
                                         uint16_t*             out_w,
                                         uint16_t*             out_h)
{
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_jof_read_tile(ra8_jof_memstore_pread,
                                   &s_store,
                                   info,
                                   tx,
                                   ty,
                                   s_scratch,
                                   (uint32_t)sizeof(s_scratch),
                                   s_cell,
                                   (uint32_t)sizeof(s_cell),
                                   out_w,
                                   out_h));
}

/**
 * @test edge tiles report and decode their clamped extents
 *
 * @details Property 1: `ra8_jof_tile_dims()` and the decoder must agree
 *          on the clamped extent for every tile, including the corner tile that
 *          is partial in both axes. @brief Provide the file-local t edge dims test helper. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_t_edge_dims(void)
{
  TEST_BEGIN("jof_edges: clamped extents in both axes");
  internal_t_build_png();
  ra8_jof_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_t_produce(&info));
  TEST_ASSERT_EQ(k_te_cols, info.tile_cols);
  TEST_ASSERT_EQ(k_te_rows, info.tile_rows);
  TEST_ASSERT_EQ(k_te_tiles, info.tile_count);
  for (uint16_t ty = 0U; ty < info.tile_rows; ty++) {
    for (uint16_t tx = 0U; tx < info.tile_cols; tx++) {
      uint16_t tw = 0U;
      uint16_t th = 0U;
      TEST_ASSERT_EQ(k_ra8_ok, ra8_jof_tile_dims(&info, tx, ty, &tw, &th));
      TEST_ASSERT_EQ(internal_t_want_w(tx), tw);
      TEST_ASSERT_EQ(internal_t_want_h(ty), th);
      uint16_t gw = 0U;
      uint16_t gh = 0U;
      internal_t_read(&info, tx, ty, &gw, &gh);
      TEST_ASSERT_EQ(internal_t_want_w(tx), gw);
      TEST_ASSERT_EQ(internal_t_want_h(ty), gh);
    }
  }
  TEST_END("jof_edges: clamped extents in both axes");
}

/**
 * @brief Compare the decoded tile in ::s_cell against the ::internal_t_pix oracle.
 *
 * @details Maps each cell coordinate back to its canvas position -- the tile
 *          origin plus the offset within the tile -- so a tile written to the
 *          wrong grid slot mismatches on its very first pixel.
 *
 * @param[in] tx Tile column in the grid.
 * @param[in] ty Tile row in the grid.
 * @param[in] tw Decoded tile width, already edge-clamped.
 * @param[in] th Decoded tile height, already edge-clamped.
 * @pre ::s_cell holds the decoded payload of tile (@p tx, @p ty).
 * @pre `tw * th` is within ::s_cell's capacity.
 * @post Every decoded pixel compared equal to the oracle.
 * @post No fixture state is mutated.
 * @note Not thread-safe (reads the shared ::s_cell).
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_t_assert_tile_pixels(uint16_t tx, uint16_t ty, uint16_t tw, uint16_t th)
{
  for (uint16_t r = 0U; r < th; r++) {
    for (uint16_t c = 0U; c < tw; c++) {
      const uint32_t cx = ((uint32_t)tx * (uint32_t)k_te_tile) + (uint32_t)c;
      const uint32_t cy = ((uint32_t)ty * (uint32_t)k_te_tile) + (uint32_t)r;
      TEST_ASSERT_EQ(internal_t_pix(cx, cy), s_cell[((size_t)r * (size_t)tw) + (size_t)c]);
    }
  }
}

/**
 * @test every tile round-trips to the source pixels at its canvas position
 *
 * @details Property 2: the image -> atlas -> decode round trip, compared
 *          against the ::internal_t_pix oracle at each tile's true canvas origin. An
 *          off-by-one in the tile cut or a swapped band fails here. @brief Provide the file-local t edge roundtrip test helper. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_t_edge_roundtrip(void)
{
  TEST_BEGIN("jof_edges: round-trip pixels at true canvas positions");
  internal_t_build_png();
  ra8_jof_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_t_produce(&info));
  for (uint16_t ty = 0U; ty < info.tile_rows; ty++) {
    for (uint16_t tx = 0U; tx < info.tile_cols; tx++) {
      uint16_t tw = 0U;
      uint16_t th = 0U;
      internal_t_read(&info, tx, ty, &tw, &th);
      internal_t_assert_tile_pixels(tx, ty, tw, th);
    }
  }
  TEST_END("jof_edges: round-trip pixels at true canvas positions");
}

/**
 * @test no two distinct tiles decode to identical payloads
 *
 * @details Property 3: with a source whose every tile is unique (::internal_t_pix is
 *          asymmetric and its per-tile base offsets are distinct for every
 *          same-size pair), two tiles decoding to the same bytes can only mean
 *          the producer emitted one band into two index slots -- duplication
 *          baked into the file, which is the failure a duplicated render would
 *          imply if the defect were on the producer side. @brief Provide the file-local t edge distinct test helper. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_t_edge_distinct(void)
{
  TEST_BEGIN("jof_edges: distinct tiles hold distinct payloads");
  internal_t_build_png();
  ra8_jof_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_t_produce(&info));
  for (uint32_t n = 0U; n < info.tile_count; n++) {
    uint16_t tw = 0U;
    uint16_t th = 0U;
    internal_t_read(&info,
                    (uint16_t)(n % info.tile_cols),
                    (uint16_t)(n / info.tile_cols),
                    &tw,
                    &th);
    s_tile_len[n] = (uint32_t)tw * (uint32_t)th * (uint32_t)info.bpp;
    memcpy(s_tile_px[n], s_cell, (size_t)s_tile_len[n]);
    for (uint32_t m = 0U; m < n; m++) {
      const bool same_len = (s_tile_len[m] == s_tile_len[n]);
      const bool same_px =
        same_len && (memcmp(s_tile_px[m], s_tile_px[n], (size_t)s_tile_len[n]) == 0);
      TEST_ASSERT(!same_px); /* duplicated payload -> duplication in the FILE */
    }
  }
  TEST_END("jof_edges: distinct tiles hold distinct payloads");
}

/**
 * @brief Host test entry point.
 *
 * @return 0 on success; any assertion `exit(1)`s before returning.
 */
int32_t main(void)
{
  internal_t_edge_dims();
  internal_t_edge_roundtrip();
  internal_t_edge_distinct();
  return 0;
}
