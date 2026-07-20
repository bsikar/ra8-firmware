/**
 * @file test_ra8_jof.c
 * @brief Host tests for the JOF atlas reader: structural validation, tile
 *        decode, memstore, and hostile-input rejection (#231).
 *
 * @details
 * Builds atlases by hand (header / tile streams / index / footer serialized
 * field by field) so every validation branch in `ra8_jof_parse()` and
 * `ra8_jof_read_tile()` can be driven with a surgically corrupted
 * atlas. Round-trips both codecs (raw + deflate) and proves byte parity
 * between paged tiles and the generator pattern.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_io_compress.h"
#include "ra8_jof.h"
#include "unity_minimal.h"

/** @brief Test atlas geometry + buffer sizing. */
enum : uint32_t {
  k_t_img_w     = 70U,                   /**< Test image width (edge tiles).   */
  k_t_img_h     = 50U,                   /**< Test image height (edge tiles).  */
  k_t_tile      = 32U,                   /**< Tile edge, pixels.               */
  k_t_bpp       = 3U,                    /**< RGB888 for the round-trip atlas. */
  k_t_store_cap = 96U * 1024U,           /**< Memstore backing capacity.       */
  k_t_tile_px   = k_t_tile * k_t_tile,   /**< Full-tile pixel count.           */
  k_t_payload   = k_t_tile_px * k_t_bpp, /**< Full-tile payload bytes.         */
  k_t_cell_cap  = k_t_payload,           /**< Tile output buffer capacity.     */
  k_t_hdr       = 32U,                   /**< JOF header bytes.                */
  k_t_ftr       = 16U,                   /**< JOF footer bytes.                */
  k_t_entry     = 8U,                    /**< Index entry bytes.               */
};

/** @brief Memstore backing bytes. */
static uint8_t s_store_buf[k_t_store_cap];
/** @brief The store under test. */
static ra8_jof_memstore_t s_store;
/** @brief Tile output buffer. */
static uint8_t s_cell[k_t_cell_cap];
/** @brief Deflate staging scratch. */
static uint8_t s_scratch[k_t_payload + (k_t_payload / 8U) + 256U];
/** @brief One packed source tile (staging for the hand producer). */
static uint8_t s_tilebuf[k_t_payload];
/** @brief One compressed tile (staging for the hand producer). */
static uint8_t s_cmpbuf[k_t_payload + (k_t_payload / 8U) + 256U];
/** @brief Deflate compressor scratch (miniz tdefl). */
alignas(8) static uint8_t s_dfl[k_ra8_io_compress_scratch_bytes];

/** @brief Deterministic source pixel channel at (x, y, c). */
static uint8_t pix(uint32_t x, uint32_t y, uint32_t c)
{
  return (uint8_t)(((x * 3U) + (y * 7U) + (c * 11U)) & 0xFFU);
}

/** @brief Append little-endian u16 to the store (hand producer). */
static void put_u16(uint16_t v)
{
  const uint8_t b[2] = {(uint8_t)(v & 0xFFU), (uint8_t)(v >> 8U)};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jof_memstore_sink(&s_store, b, sizeof(b)));
}

/** @brief Append little-endian u32 to the store (hand producer). */
static void put_u32(uint32_t v)
{
  const uint8_t b[4] = {(uint8_t)(v & 0xFFU),
                        (uint8_t)((v >> 8U) & 0xFFU),
                        (uint8_t)((v >> 16U) & 0xFFU),
                        (uint8_t)((v >> 24U) & 0xFFU)};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jof_memstore_sink(&s_store, b, sizeof(b)));
}

/** @brief Append raw bytes to the store (hand producer). */
static void put_bytes(const uint8_t* p, size_t n)
{
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jof_memstore_sink(&s_store, p, n));
}

/**
 * @brief Append the 32-byte JOF atlas header to the shared memstore.
 *
 * @param[in] codec Tile codec byte (raw or deflate) stored in the header.
 * @param[in] count Total tile count written to the header.
 *
 * @return None.
 * @pre `s_store` is initialised and empty.
 * @post 32 header bytes are appended to `s_store`.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void build_atlas_header(uint8_t codec, uint32_t count)
{
  const uint8_t magic[4] = {'J', 'O', 'F', '1'};
  put_bytes(magic, sizeof(magic));
  put_u16((uint16_t)k_t_img_w);
  put_u16((uint16_t)k_t_img_h);
  put_u16((uint16_t)k_t_tile);
  put_u16((uint16_t)k_t_tile);
  const uint8_t bpp_codec[2] = {(uint8_t)k_t_bpp, codec};
  put_bytes(bpp_codec, sizeof(bpp_codec));
  put_u16(0U);
  put_u32(count);
  const uint8_t zeros[12] = {};
  put_bytes(zeros, sizeof(zeros));
}

/**
 * @brief Encode one tile at (@p x0, @p y0) and append its stream to the store.
 *
 * @details Fills `s_tilebuf` with the generator pattern for the (clamped) tile
 *          rectangle, then appends either the raw bytes or a deflate stream.
 *
 * @param[in] x0    Left pixel column of the tile.
 * @param[in] y0    Top pixel row of the tile.
 * @param[in] codec Tile codec (raw or deflate).
 *
 * @return The number of bytes appended to the store for this tile.
 * @pre `s_store` has the header already written.
 * @post The tile stream is appended and its byte length returned.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static uint32_t build_atlas_encode_tile(uint32_t x0, uint32_t y0, uint8_t codec)
{
  const uint32_t tw = ((k_t_img_w - x0) < k_t_tile) ? (k_t_img_w - x0) : k_t_tile;
  const uint32_t th = ((k_t_img_h - y0) < k_t_tile) ? (k_t_img_h - y0) : k_t_tile;
  uint32_t       o  = 0U;
  for (uint32_t r = 0U; r < th; r++) {
    for (uint32_t c = 0U; c < tw; c++) {
      for (uint32_t ch = 0U; ch < k_t_bpp; ch++) {
        s_tilebuf[o] = pix(x0 + c, y0 + r, ch);
        o++;
      }
    }
  }
  if (codec == (uint8_t)k_ra8_jof_codec_raw) {
    put_bytes(s_tilebuf, o);
    return o;
  }
  uint32_t clen = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_compress(s_tilebuf,
                                 o,
                                 s_cmpbuf,
                                 (uint32_t)sizeof(s_cmpbuf),
                                 s_dfl,
                                 (uint32_t)sizeof(s_dfl),
                                 &clen));
  put_bytes(s_cmpbuf, clen);
  return clen;
}

/**
 * @brief Hand-build a full JOF atlas of the pattern into the memstore.
 * @param[in] codec ::ra8_jof_codec_t member to encode tiles with.
 * @pre The shared store buffers are available.
 * @pre @p codec is raw or deflate.
 * @post `s_store` holds a structurally complete atlas.
 * @post The store length equals the footer's total_size.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void build_atlas(uint8_t codec)
{
  s_store = (ra8_jof_memstore_t){.buf = s_store_buf, .cap = sizeof(s_store_buf), .len = 0U};
  const uint32_t cols  = (k_t_img_w + k_t_tile - 1U) / k_t_tile;
  const uint32_t rows  = (k_t_img_h + k_t_tile - 1U) / k_t_tile;
  const uint32_t count = cols * rows;

  build_atlas_header(codec, count);

  /* Tile streams + index bookkeeping. */
  uint32_t offs[16] = {};
  uint32_t lens[16] = {};
  uint32_t n        = 0U;
  for (uint32_t ty = 0U; ty < rows; ty++) {
    for (uint32_t tx = 0U; tx < cols; tx++) {
      offs[n] = (uint32_t)s_store.len;
      lens[n] = build_atlas_encode_tile(tx * k_t_tile, ty * k_t_tile, codec);
      n++;
    }
  }

  /* Index + footer. */
  const uint32_t index_off = (uint32_t)s_store.len;
  for (uint32_t i = 0U; i < count; i++) {
    put_u32(offs[i]);
    put_u32(lens[i]);
  }
  put_u32(index_off);
  put_u32(count);
  put_u32((uint32_t)s_store.len + 8U); /* total = current + remaining 8 footer bytes */
  const uint8_t fmagic[4] = {'J', 'O', 'F', 'E'};
  put_bytes(fmagic, sizeof(fmagic));
}

/** @brief Parse the store under test into @p info, returning the code. */
static ra8_err_t parse_store(ra8_jof_info_t* info)
{
  return ra8_jof_parse(ra8_jof_memstore_pread, &s_store, (uint64_t)s_store.len, info);
}

/**
 * @test test_jof_roundtrip
 * @brief Both codecs round-trip: every tile pages back byte-identical to the
 *        generator pattern, edge tiles clamped.
 *
 * @par MC/DC:
 * (byte-parity oracle over the full tile grid; the reader's compound
 * decisions get their vectors in the hostile-atlas tests below.)
 */
static void test_jof_roundtrip(void)
{
  TEST_BEGIN("jof: raw + deflate round-trip, byte parity");
  const uint8_t codecs[2] = {(uint8_t)k_ra8_jof_codec_raw, (uint8_t)k_ra8_jof_codec_deflate};
  for (uint32_t ci = 0U; ci < 2U; ci++) {
    build_atlas(codecs[ci]);
    ra8_jof_info_t info = {};
    TEST_ASSERT_EQ(k_ra8_ok, parse_store(&info));
    TEST_ASSERT_EQ(k_t_img_w, info.width);
    TEST_ASSERT_EQ(k_t_img_h, info.height);
    TEST_ASSERT_EQ(3U, info.tile_cols);
    TEST_ASSERT_EQ(2U, info.tile_rows);
    TEST_ASSERT_EQ(6U, info.tile_count);
    TEST_ASSERT_EQ(codecs[ci], info.codec);
    for (uint16_t ty = 0U; ty < info.tile_rows; ty++) {
      for (uint16_t tx = 0U; tx < info.tile_cols; tx++) {
        uint16_t w = 0U;
        uint16_t h = 0U;
        TEST_ASSERT_EQ(k_ra8_ok,
                       ra8_jof_read_tile(ra8_jof_memstore_pread,
                                         &s_store,
                                         &info,
                                         tx,
                                         ty,
                                         s_scratch,
                                         (uint32_t)sizeof(s_scratch),
                                         s_cell,
                                         (uint32_t)sizeof(s_cell),
                                         &w,
                                         &h));
        const uint32_t x0 = (uint32_t)tx * k_t_tile;
        const uint32_t y0 = (uint32_t)ty * k_t_tile;
        TEST_ASSERT_EQ(((k_t_img_w - x0) < k_t_tile) ? (k_t_img_w - x0) : k_t_tile, w);
        TEST_ASSERT_EQ(((k_t_img_h - y0) < k_t_tile) ? (k_t_img_h - y0) : k_t_tile, h);
        for (uint32_t r = 0U; r < h; r++) {
          for (uint32_t c = 0U; c < w; c++) {
            for (uint32_t ch = 0U; ch < k_t_bpp; ch++) {
              TEST_ASSERT_EQ(pix(x0 + c, y0 + r, ch), s_cell[(((r * w) + c) * k_t_bpp) + ch]);
            }
          }
        }
      }
    }
  }
  TEST_END("jof: raw + deflate round-trip, byte parity");
}

/** @brief Corrupt one byte of the stored atlas at @p off (XOR flip). */
static void flip(size_t off)
{
  s_store_buf[off] ^= 0xFFU;
}

/**
 * @brief Reject undersize / oversize backing lengths and a corrupt header magic.
 * @param[in,out] info Scratch parse-info sink.
 * @return None.
 * @pre The shared store buffers are available.
 * @post Each malformed length/magic returned its rejection code.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void hostile_header_size_and_magic(ra8_jof_info_t* info)
{
  /* Undersize + oversize backing. */
  build_atlas((uint8_t)k_ra8_jof_codec_raw);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_jof_parse(ra8_jof_memstore_pread, &s_store, 4U, info));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_jof_parse(ra8_jof_memstore_pread, &s_store, ((uint64_t)UINT32_MAX) + 64U, info));

  /* Header magic. */
  flip(0U);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, parse_store(info));
}

/**
 * @brief Reject zero and over-cap width / height / tile geometry fields.
 * @param[in,out] info Scratch parse-info sink.
 * @return None.
 * @pre The shared store buffers are available.
 * @post Every zero or over-cap dimension returned k_ra8_err_validation_failed.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void hostile_header_dims(ra8_jof_info_t* info)
{
  /* Zero width / height / tile_w / tile_h. */
  const uint8_t zero_fields[4] = {4U, 6U, 8U, 10U};
  for (uint32_t i = 0U; i < 4U; i++) {
    build_atlas((uint8_t)k_ra8_jof_codec_raw);
    s_store_buf[zero_fields[i]]      = 0U;
    s_store_buf[zero_fields[i] + 1U] = 0U;
    TEST_ASSERT_EQ(k_ra8_err_validation_failed, parse_store(info));
  }

  /* Over-cap dimensions (33000 > 32768). */
  build_atlas((uint8_t)k_ra8_jof_codec_raw);
  s_store_buf[4] = (uint8_t)(33000U & 0xFFU);
  s_store_buf[5] = (uint8_t)(33000U >> 8U);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, parse_store(info));
  build_atlas((uint8_t)k_ra8_jof_codec_raw);
  s_store_buf[6] = (uint8_t)(33000U & 0xFFU);
  s_store_buf[7] = (uint8_t)(33000U >> 8U);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, parse_store(info));
}

/**
 * @brief Reject bad bpp, unknown codec, non-zero reserved, and count mismatch.
 * @param[in,out] info Scratch parse-info sink.
 * @return None.
 * @pre The shared store buffers are available.
 * @post Every corrupt encoding/reserved/count field was rejected.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void hostile_header_encoding(ra8_jof_info_t* info)
{
  /* bpp MC/DC vectors 2..4 (vector 1 is the valid round-trip above). */
  const uint8_t bad_bpp[3] = {0U, 2U, 5U};
  for (uint32_t i = 0U; i < 3U; i++) {
    build_atlas((uint8_t)k_ra8_jof_codec_raw);
    s_store_buf[12] = bad_bpp[i];
    TEST_ASSERT_EQ(k_ra8_err_validation_failed, parse_store(info));
  }

  /* Unknown codec. */
  build_atlas((uint8_t)k_ra8_jof_codec_raw);
  s_store_buf[13] = 2U;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, parse_store(info));

  /* Non-zero reserved bytes (both runs). */
  build_atlas((uint8_t)k_ra8_jof_codec_raw);
  s_store_buf[14] = 1U;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, parse_store(info));
  build_atlas((uint8_t)k_ra8_jof_codec_raw);
  s_store_buf[25] = 1U;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, parse_store(info));

  /* Header tile-count mismatch. */
  build_atlas((uint8_t)k_ra8_jof_codec_raw);
  s_store_buf[16] = 7U;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, parse_store(info));
}

/**
 * @brief Reject footer magic/count/total/index corruption and a sub-header index.
 * @param[in,out] info Scratch parse-info sink.
 * @return None.
 * @pre The shared store buffers are available.
 * @post Every footer corruption and out-of-range index was rejected.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void hostile_header_footer(ra8_jof_info_t* info)
{
  /* Footer corruption: magic, count, total, index_off. */
  build_atlas((uint8_t)k_ra8_jof_codec_raw);
  flip(s_store.len - 1U);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, parse_store(info));
  build_atlas((uint8_t)k_ra8_jof_codec_raw);
  flip(s_store.len - (size_t)k_t_ftr + 4U); /* footer tile count */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, parse_store(info));
  build_atlas((uint8_t)k_ra8_jof_codec_raw);
  flip(s_store.len - (size_t)k_t_ftr + 8U); /* footer total size */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, parse_store(info));
  build_atlas((uint8_t)k_ra8_jof_codec_raw);
  flip(s_store.len - (size_t)k_t_ftr); /* index offset */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, parse_store(info));

  /* Index offset below the header start. */
  build_atlas((uint8_t)k_ra8_jof_codec_raw);
  s_store_buf[s_store.len - (size_t)k_t_ftr]      = 8U;
  s_store_buf[s_store.len - (size_t)k_t_ftr + 1U] = 0U;
  s_store_buf[s_store.len - (size_t)k_t_ftr + 2U] = 0U;
  s_store_buf[s_store.len - (size_t)k_t_ftr + 3U] = 0U;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, parse_store(info));
}

/**
 * @brief Reject the two null-pointer parse arguments.
 * @param[in,out] info Scratch parse-info sink.
 * @return None.
 * @pre The shared store buffers are available.
 * @post Both null arguments returned k_ra8_err_null_ptr.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void hostile_header_null_guards(ra8_jof_info_t* info)
{
  build_atlas((uint8_t)k_ra8_jof_codec_raw);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_jof_parse(NULL, &s_store, (uint64_t)s_store.len, info));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_jof_parse(ra8_jof_memstore_pread, &s_store, (uint64_t)s_store.len, NULL));
}

/**
 * @test test_jof_hostile_header
 * @brief Every header/footer/cross-check rejection fires fail-closed.
 *
 * @par MC/DC:
 * Decision (parse): `bpp == 0 || bpp == 2 || bpp > 4` (3 conditions)
 * - Vector 1: bpp=3   -> false (baseline: valid atlas parses)
 * - Vector 2: bpp=0   -> true  (varies condition 1 only)
 * - Vector 3: bpp=2   -> true  (varies condition 2 only)
 * - Vector 4: bpp=5   -> true  (varies condition 3 only)
 * N+1 = 4 vectors for N=3 conditions: minimal MC/DC.
 *
 * Decision (parse): `total_size < min || total_size > UINT32_MAX`
 * (2 conditions)
 * - Vector 1: valid size          -> false (baseline)
 * - Vector 2: size = 4            -> true  (varies condition 1)
 * - Vector 3: size = 2^32 + 64    -> true  (varies condition 2)
 */
static void test_jof_hostile_header(void)
{
  TEST_BEGIN("jof: hostile header / footer / cross-checks rejected");
  ra8_jof_info_t info = {};

  hostile_header_size_and_magic(&info);
  hostile_header_dims(&info);
  hostile_header_encoding(&info);
  hostile_header_footer(&info);
  hostile_header_null_guards(&info);

  TEST_END("jof: hostile header / footer / cross-checks rejected");
}

/**
 * @brief Read tile (@p col, @p row) from the store under test via the memstore.
 *
 * @details Thin wrapper over `ra8_jof_read_tile` binding the shared
 *          memstore reader and store so the hostile-tile vectors vary only the
 *          grid coordinate and the scratch / output budgets.
 *
 * @param[in]  info        Parsed atlas info.
 * @param[in]  col         Tile column index.
 * @param[in]  row         Tile row index.
 * @param[in]  scratch     Inflate scratch buffer (may be NULL to test the guard).
 * @param[in]  scratch_cap Scratch capacity in bytes.
 * @param[out] out         Decoded cell output buffer.
 * @param[in]  out_cap     Output capacity in bytes.
 * @param[out] w           Receives the tile width in pixels.
 * @param[out] h           Receives the tile height in pixels.
 *
 * @return The `ra8_jof_read_tile` result code.
 * @pre `s_store` holds a parsed atlas.
 * @post No state beyond @p out / @p w / @p h is modified.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static ra8_err_t hostile_read_tile(ra8_jof_info_t* info,
                                   uint16_t        col,
                                   uint16_t        row,
                                   uint8_t*        scratch,
                                   uint32_t        scratch_cap,
                                   uint8_t*        out,
                                   uint32_t        out_cap,
                                   uint16_t*       w,
                                   uint16_t*       h)
{
  return ra8_jof_read_tile(ra8_jof_memstore_pread,
                           &s_store,
                           info,
                           col,
                           row,
                           scratch,
                           scratch_cap,
                           out,
                           out_cap,
                           w,
                           h);
}

/**
 * @test test_jof_hostile_tiles
 * @brief Per-tile stream corruption is rejected at read time, fail-closed.
 *
 * @par MC/DC:
 * Decision (index fetch): `toff < 32 || toff + tlen > index_off`
 * (2 conditions)
 * - Vector 1: valid entry                  -> false (round-trip baseline)
 * - Vector 2: toff = 8                     -> true  (varies condition 1)
 * - Vector 3: tlen inflated past the index -> true  (varies condition 2)
 */
static void test_jof_hostile_tiles(void)
{
  TEST_BEGIN("jof: hostile tile streams / index entries rejected");
  ra8_jof_info_t info = {};
  uint16_t       w    = 0U;
  uint16_t       h    = 0U;
  const uint32_t scap = (uint32_t)sizeof(s_scratch);
  const uint32_t ccap = (uint32_t)sizeof(s_cell);

  /* Vector 2: entry offset points into the header. */
  build_atlas((uint8_t)k_ra8_jof_codec_raw);
  TEST_ASSERT_EQ(k_ra8_ok, parse_store(&info));
  s_store_buf[info.index_off] = 8U; /* entry 0 offset lo byte */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 hostile_read_tile(&info, 0U, 0U, s_scratch, scap, s_cell, ccap, &w, &h));

  /* Vector 3: entry length runs past the tile-stream region. */
  build_atlas((uint8_t)k_ra8_jof_codec_raw);
  TEST_ASSERT_EQ(k_ra8_ok, parse_store(&info));
  s_store_buf[info.index_off + 6U] = 0xFFU; /* entry 0 length high bytes */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 hostile_read_tile(&info, 0U, 0U, s_scratch, scap, s_cell, ccap, &w, &h));

  /* Raw codec: stored length != payload. */
  build_atlas((uint8_t)k_ra8_jof_codec_raw);
  TEST_ASSERT_EQ(k_ra8_ok, parse_store(&info));
  s_store_buf[info.index_off + 4U] ^= 0x01U; /* off-by-one length */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 hostile_read_tile(&info, 0U, 0U, s_scratch, scap, s_cell, ccap, &w, &h));

  /* Deflate codec: corrupt stream body -> inflate failure. */
  build_atlas((uint8_t)k_ra8_jof_codec_deflate);
  TEST_ASSERT_EQ(k_ra8_ok, parse_store(&info));
  flip((size_t)k_t_hdr + 2U);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 hostile_read_tile(&info, 0U, 0U, s_scratch, scap, s_cell, ccap, &w, &h));

  /* Deflate codec: scratch too small for the stored stream. */
  build_atlas((uint8_t)k_ra8_jof_codec_deflate);
  TEST_ASSERT_EQ(k_ra8_ok, parse_store(&info));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 hostile_read_tile(&info, 0U, 0U, s_scratch, 2U, s_cell, ccap, &w, &h));
  /* Deflate codec: NULL scratch is rejected. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 hostile_read_tile(&info, 0U, 0U, NULL, 0U, s_cell, ccap, &w, &h));

  /* Output too small for the payload. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 hostile_read_tile(&info, 0U, 0U, s_scratch, scap, s_cell, 16U, &w, &h));

  /* Grid bounds + dims helper. */
  TEST_ASSERT_EQ(
    k_ra8_err_out_of_range,
    hostile_read_tile(&info, (uint16_t)info.tile_cols, 0U, s_scratch, scap, s_cell, ccap, &w, &h));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_jof_tile_dims(&info, 0U, (uint16_t)info.tile_rows, &w, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_jof_tile_dims(NULL, 0U, 0U, &w, &h));
  TEST_END("jof: hostile tile streams / index entries rejected");
}

/**
 * @test test_jof_memstore
 * @brief The memstore sink/pread pair enforces its bounds.
 *
 * @par MC/DC:
 * (single-condition guards: full-store append, at/after-end read, and the
 * NULL rejections -- one vector each.)
 */
static void test_jof_memstore(void)
{
  TEST_BEGIN("jof: memstore sink/pread bounds");
  uint8_t            tiny_buf[8] = {};
  ra8_jof_memstore_t tiny        = {.buf = tiny_buf, .cap = sizeof(tiny_buf), .len = 0U};
  const uint8_t      five[5]     = {1U, 2U, 3U, 4U, 5U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jof_memstore_sink(&tiny, five, sizeof(five)));
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_jof_memstore_sink(&tiny, five, sizeof(five)));
  TEST_ASSERT_EQ(5U, tiny.len);
  uint8_t out[8] = {};
  size_t  got    = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jof_memstore_pread(&tiny, 3U, out, sizeof(out), &got));
  TEST_ASSERT_EQ(2U, got);
  TEST_ASSERT_EQ(4U, out[0]);
  TEST_ASSERT_EQ(5U, out[1]);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jof_memstore_pread(&tiny, 9U, out, sizeof(out), &got));
  TEST_ASSERT_EQ(0U, got);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_jof_memstore_sink(NULL, five, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_jof_memstore_pread(NULL, 0U, out, 1U, &got));
  /* Bound sanity: always payload + margin. */
  TEST_ASSERT(ra8_jof_stored_bound(1000U) >= 1256U);
  TEST_END("jof: memstore sink/pread bounds");
}

/**
 * @brief Test entry point -- runs the atlas reader suite in order.
 * @return 0 on success; unity_minimal.h exits non-zero on first failure.
 * @pre None.
 * @pre None.
 * @post All tests executed (or the process exited on first failure).
 * @post stderr carries a per-test RUN/PASS log.
 * @note Not thread-safe. No SIGALRM / timers used.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_jof_roundtrip();
  test_jof_hostile_header();
  test_jof_hostile_tiles();
  test_jof_memstore();
  (void)fprintf(stderr, "[OK  ] test_ra8_jof.c\n");
  return 0;
}
