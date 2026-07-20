/**
 * @file test_ra8_jof_png_hostile.c
 * @brief Hostile-stream corpus for the streaming PNG decoder: every
 *        fail-closed arm of the chunk and pixel layers (#231).
 *
 * @details
 * Complements `test_ra8_jof_produce.c` (which proves the happy paths
 * byte-for-byte): this suite hand-crafts pathological PNG byte streams --
 * bad filters, out-of-range palette indices, row-count lies, truncations at
 * every chunk phase, oversize fields, empty-IDAT floods, chunk-budget
 * exhaustion, streams that end mid-zlib, and trailing garbage -- and drives
 * each to its contracted rejection through the real producer entry. A few
 * arms only a failing transport can reach are driven through the module
 * seam `ra8_jof_priv_png_rows()` directly (the CLAUDE.md "test access to
 * internal symbols" allowance).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "ra8_err.h"
#include "ra8_jof.h"
#include "ra8_jof_internal.h"
#include "ra8_jof_produce.h"
#include "unity_minimal.h"

/** @brief Trailing-garbage probe appended after a complete zlib stream. */
typedef enum : uint16_t {
  k_png_hostile_fill_trailing = 0xEEU, /**< Must be refused, not decoded. */
  k_png_hostile_trailing_len  = 64U,   /**< Trailing garbage bytes.       */
} png_hostile_trailing_t;

/**
 * @enum t_png_layout_t
 * @brief PNG serialisation offsets and field widths the crafters poke.
 *
 * @details
 * `begin_png()` takes an IHDR byte index in its `extra_field` argument, so the
 * `k_t_ihdr_off_*` names double as "which IHDR field is being corrupted".
 * Offsets ending `_b<N>` are the `N`-th byte of a big-endian 32-bit field.
 */
typedef enum : uint8_t {
  k_t_be32_hi_shift        = 24U, /**< Shift selecting the top byte of a big-endian 32-bit field. */
  k_t_byte_mask            = 0xFFU, /**< Low-byte mask used while serialising a big-endian field. */
  k_t_png_ihdr_len         = 13U,   /**< IHDR payload length, fixed by the PNG spec.      */
  k_t_ihdr_short_len       = 12U,   /**< One byte short of a legal IHDR; must be refused. */
  k_t_ihdr_off_h_b1        = 5U,    /**< Height byte 1 within the IHDR payload.           */
  k_t_ihdr_off_h_b3        = 7U,    /**< Height byte 3 within the IHDR payload.           */
  k_t_ihdr_off_ct          = 9U,    /**< Colour-type byte within the IHDR payload.        */
  k_t_ihdr_off_compression = 10U,   /**< Compression-method byte; only 0 is legal.        */
  k_t_ihdr_off_filter      = 11U,   /**< Filter-method byte; only 0 is legal.             */
  k_t_zlib_cmf             = 0x78U, /**< zlib CMF: deflate with a 32 KiB window.          */
  k_t_filter_invalid       = 9U,    /**< Row filter outside the legal 0..4 range.         */
} t_png_layout_t;

/**
 * @enum t_trunc_t
 * @brief Tail bytes removed to truncate a stream mid-payload.
 *
 * @details
 * Each value is chosen so the cut lands inside a specific chunk's payload
 * rather than on a chunk boundary, which is what exercises the partial-read
 * path instead of the clean end-of-stream path.
 */
typedef enum : uint8_t {
  k_t_trunc_plte_bytes = 7U, /**< Cut that lands mid-PLTE payload. */
  k_t_trunc_trns_bytes = 5U, /**< Cut that lands mid-tRNS payload. */
} t_trunc_t;

/**
 * @enum t_junk_t
 * @brief Filler bytes appended after a well-formed stream.
 *
 * @details
 * Arbitrary by design -- their only requirement is that no two are equal, so a
 * decoder that runs past the stream end reveals which byte it consumed.
 */
typedef enum : uint8_t {
  k_t_trailer_junk_b0 = 0xAAU, /**< First byte past the zlib stream.  */
  k_t_trailer_junk_b1 = 0xBBU, /**< Second byte past the zlib stream. */
  k_t_trailer_junk_b2 = 0xCCU, /**< Third byte past the zlib stream.  */
} t_junk_t;

/**
 * @enum t_corpus_t
 * @brief Geometry and buffer sizing for the crafted corpora.
 */
typedef enum : uint16_t {
  k_t_kib               = 1024U, /**< Bytes per KiB.                               */
  k_t_arena_kib         = 512U,  /**< Direct-seam bump-arena size, in KiB.         */
  k_t_raw_cap           = 4096U, /**< Unfiltered-scanline scratch capacity, bytes. */
  k_t_zbuf_cap          = 4200U, /**< Stored-deflate output capacity: just over one 4 KiB window. */
  k_t_zbig_kib          = 64U,   /**< Oversized zlib staging buffer, in KiB.                  */
  k_t_frame_dim         = 300U,  /**< Edge of the square frame used for the large-image arms. */
  k_t_frame_stride      = 301U,  /**< Its row stride: k_t_frame_dim pixels + 1 filter byte.   */
  k_t_window_frame_w    = 15U,   /**< Width of the frame whose zlib output is exactly one window. */
  k_t_window_frame_h    = 255U,  /**< Its height; 15x255 gray encodes to exactly 4096 bytes. */
  k_t_plte_oversize_len = 771U,  /**< PLTE payload one entry past the 256-entry maximum.     */
  k_t_trns_oversize_len = 300U,  /**< tRNS payload past the 256-entry maximum.               */
} t_corpus_t;

/** @brief Corpus geometry + buffer sizing. */
enum : uint32_t {
  k_t_w         = 8U,            /**< Tiny test image width.     */
  k_t_h         = 8U,            /**< Tiny test image height.    */
  k_t_tile      = 8U,            /**< Tile edge under test.      */
  k_t_src_cap   = 256U * 1024U,  /**< Crafted source capacity.   */
  k_t_store_cap = 128U * 1024U,  /**< Memstore capacity.         */
  k_t_work_cap  = 1024U * 1024U, /**< Producer work arena.       */
  k_t_many      = 4100U,         /**< Chunk-budget flood count.  */
  k_t_zout_cap  = 512U,          /**< make_zlib caller capacity. */
};

/** @brief Crafted source bytes. */
static uint8_t s_src[k_t_src_cap];
/** @brief Crafted source length. */
static size_t s_src_len;
/** @brief Producer work arena. */
static uint8_t s_work[k_t_work_cap];
/** @brief Memstore backing. */
static uint8_t s_store_buf[k_t_store_cap];
/** @brief Raw (filtered) scanline staging. */
static uint8_t s_raw[k_t_raw_cap];

/**
 * @struct t_pull_t
 * @brief Memory pull source with an optional hard-failure trigger.
 */
typedef struct {
  size_t pos;     /**< Read cursor.                                   */
  size_t fail_at; /**< Byte offset that returns an error (0 = never). */
} t_pull_t;

/** @brief ::ra8_jof_pull_fn over `s_src` with failure injection. */
static ra8_err_t t_pull(void* ctx, uint8_t* buf, size_t cap, size_t* got)
{
  t_pull_t* p = (t_pull_t*)ctx;
  if ((p->fail_at != 0U) && (p->pos >= p->fail_at)) {
    *got = 0U;
    return k_ra8_err_hw_error;
  }
  size_t take = s_src_len - p->pos;
  if (take > cap) {
    take = cap;
  }
  if ((p->fail_at != 0U) && ((p->pos + take) > p->fail_at)) {
    take = p->fail_at - p->pos;
  }
  memcpy(buf, &s_src[p->pos], take);
  p->pos += take;
  *got = take;
  return k_ra8_ok;
}

/** @brief Append raw bytes to the crafted source. */
static void put(const uint8_t* d, size_t n)
{
  memcpy(&s_src[s_src_len], d, n);
  s_src_len += n;
}

/** @brief Append one PNG chunk (big-endian length/type + payload + CRC). */
static void put_chunk(const char* type, const uint8_t* data, uint32_t len)
{
  uint8_t hdr[8];
  hdr[0] = (uint8_t)(len >> k_t_be32_hi_shift);
  hdr[1] = (uint8_t)((len >> 16U) & k_t_byte_mask);
  hdr[2] = (uint8_t)((len >> 8U) & k_t_byte_mask);
  hdr[3] = (uint8_t)(len & k_t_byte_mask);
  memcpy(&hdr[4], type, 4U);
  put(hdr, sizeof(hdr));
  if (len > 0U) {
    put(data, len);
  }
  const uint8_t crc[4] = {0U, 0U, 0U, 0U}; /* CRCs are not verified */
  put(crc, sizeof(crc));
}

/** @brief Start a crafted stream: signature + a gray8 IHDR of (w, h). */
static void begin_png(uint32_t w, uint32_t h, uint8_t color, uint8_t extra_field, uint8_t extra_val)
{
  static const uint8_t sig[8] = {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU};
  s_src_len                   = 0U;
  put(sig, sizeof(sig));
  uint8_t ihdr[k_t_png_ihdr_len] = {};
  ihdr[0]                        = (uint8_t)(w >> k_t_be32_hi_shift);
  ihdr[1]                        = (uint8_t)((w >> 16U) & k_t_byte_mask);
  ihdr[2]                        = (uint8_t)((w >> 8U) & k_t_byte_mask);
  ihdr[3]                        = (uint8_t)(w & k_t_byte_mask);
  ihdr[4]                        = (uint8_t)(h >> k_t_be32_hi_shift);
  ihdr[k_t_ihdr_off_h_b1]        = (uint8_t)((h >> 16U) & k_t_byte_mask);
  ihdr[6]                        = (uint8_t)((h >> 8U) & k_t_byte_mask);
  ihdr[k_t_ihdr_off_h_b3]        = (uint8_t)(h & k_t_byte_mask);
  ihdr[8]                        = 8U;
  ihdr[k_t_ihdr_off_ct]          = color;
  if (extra_field != 0U) {
    ihdr[extra_field] = extra_val; /* corrupt one method/interlace byte */
  }
  put_chunk("IHDR", ihdr, sizeof(ihdr));
}

/**
 * @brief zlib-compress a filtered gray8 frame with a chosen filter byte.
 * @param[in]  w      Frame width, pixels.
 * @param[in]  h      Rows to emit.
 * @param[in]  filter Filter byte stamped on every row.
 * @param[out] out    Receives the zlib stream.
 * @return Compressed length.
 * @retval >0 zlib stream length.
 * @pre The staging buffers cover the frame.
 * @pre @p out has ::k_t_zout_cap capacity.
 * @post @p out holds a valid zlib stream of the filtered rows.
 * @post No other state mutated.
 * @note Not thread-safe (shared staging).
 * @since 0.1.0
 */
static uint32_t make_zlib(uint32_t w, uint32_t h, uint8_t filter, uint8_t* out)
{
  size_t o = 0U;
  for (uint32_t y = 0U; y < h; y++) {
    s_raw[o] = filter;
    o++;
    for (uint32_t x = 0U; x < w; x++) {
      s_raw[o] = (uint8_t)(x + y);
      o++;
    }
  }
  mz_ulong zlen = (mz_ulong)k_t_zout_cap;
  TEST_ASSERT_EQ(MZ_OK, mz_compress(out, &zlen, s_raw, (mz_ulong)o));
  return (uint32_t)zlen;
}

/** @brief Run the producer over the crafted source; return its code. */
static ra8_err_t craft_produce(size_t fail_at)
{
  static t_pull_t           s_pull;
  static ra8_jof_memstore_t s_store;
  s_pull  = (t_pull_t){.pos = 0U, .fail_at = fail_at};
  s_store = (ra8_jof_memstore_t){.buf = s_store_buf, .cap = sizeof(s_store_buf), .len = 0U};
  const ra8_jof_produce_cfg_t cfg = {
    .pull       = t_pull,
    .pull_ctx   = &s_pull,
    .sink       = ra8_jof_memstore_sink,
    .sink_ctx   = &s_store,
    .tile_w     = (uint16_t)k_t_tile,
    .tile_h     = (uint16_t)k_t_tile,
    .codec      = (uint8_t)k_ra8_jof_codec_raw,
    .max_width  = 512U,
    .max_height = 512U,
    .work       = s_work,
    .work_cap   = sizeof(s_work),
  };
  ra8_jof_info_t info = {};
  return ra8_jof_produce(&cfg, &info);
}

/** @brief Run the producer with custom tile geometry / codec / arena size. */
static ra8_err_t
craft_produce_with(uint16_t tile_w, uint16_t tile_h, uint8_t codec, size_t work_cap)
{
  static t_pull_t           s_pull;
  static ra8_jof_memstore_t s_store;
  s_pull  = (t_pull_t){.pos = 0U, .fail_at = 0U};
  s_store = (ra8_jof_memstore_t){.buf = s_store_buf, .cap = sizeof(s_store_buf), .len = 0U};
  const ra8_jof_produce_cfg_t cfg = {
    .pull       = t_pull,
    .pull_ctx   = &s_pull,
    .sink       = ra8_jof_memstore_sink,
    .sink_ctx   = &s_store,
    .tile_w     = tile_w,
    .tile_h     = tile_h,
    .codec      = codec,
    .max_width  = 512U,
    .max_height = 512U,
    .work       = s_work,
    .work_cap   = work_cap,
  };
  ra8_jof_info_t info = {};
  return ra8_jof_produce(&cfg, &info);
}

/**
 * @test test_png_hostile_pixel_layer
 * @brief Bad filters, palette overruns and row-count lies fail closed.
 *
 * @par MC/DC:
 * Decision (strict termination): `rows_done != h || rowfill != 0`
 * (2 conditions)
 * - Vector 1: exact rows, no residue -> false (happy paths elsewhere)
 * - Vector 2: fewer rows than IHDR   -> true via rows_done (short-frame case)
 * - Vector 3: rowfill residue        -> true via rowfill (mid-scanline end)
 */
static void test_png_hostile_pixel_layer(void)
{
  TEST_BEGIN("png hostile: filters / palette / row-count lies");
  /* Unknown filter byte (9). */
  begin_png(k_t_w, k_t_h, 0U, 0U, 0U);
  uint8_t  z[k_t_zout_cap];
  uint32_t zlen = make_zlib(k_t_w, k_t_h, k_t_filter_invalid, z);
  put_chunk("IDAT", z, zlen);
  put_chunk("IEND", nullptr, 0U);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, craft_produce(0U));

  /* Palette index past a 2-entry PLTE. */
  begin_png(k_t_w, k_t_h, 3U, 0U, 0U);
  const uint8_t plte[6] = {1U, 2U, 3U, 4U, 5U, 6U};
  put_chunk("PLTE", plte, sizeof(plte));
  zlen = make_zlib(k_t_w, k_t_h, 0U, z); /* indices reach 14 > 1 */
  put_chunk("IDAT", z, zlen);
  put_chunk("IEND", nullptr, 0U);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, craft_produce(0U));

  /* More scanlines than IHDR declared (frame lies tall). */
  begin_png(k_t_w, k_t_h, 0U, 0U, 0U);
  zlen = make_zlib(k_t_w, k_t_h + 1U, 0U, z);
  put_chunk("IDAT", z, zlen);
  put_chunk("IEND", nullptr, 0U);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, craft_produce(0U));

  /* Fewer scanlines than IHDR declared (short frame). */
  begin_png(k_t_w, k_t_h, 0U, 0U, 0U);
  zlen = make_zlib(k_t_w, k_t_h - 2U, 0U, z);
  put_chunk("IDAT", z, zlen);
  put_chunk("IEND", nullptr, 0U);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, craft_produce(0U));

  /* Trailing garbage inside the IDAT after the zlib stream. */
  begin_png(k_t_w, k_t_h, 0U, 0U, 0U);
  zlen         = make_zlib(k_t_w, k_t_h, 0U, z);
  z[zlen]      = k_t_trailer_junk_b0;
  z[zlen + 1U] = k_t_trailer_junk_b1;
  z[zlen + 2U] = k_t_trailer_junk_b2;
  put_chunk("IDAT", z, zlen + 3U);
  put_chunk("IEND", nullptr, 0U);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, craft_produce(0U));
  TEST_END("png hostile: filters / palette / row-count lies");
}

/**
 * @test test_png_hostile_chunk_layer
 * @brief IHDR method bytes, oversize fields, tRNS misuse, stray IEND/IDAT
 *        and ancillary walks all take their contracted arms.
 *
 * @par MC/DC:
 * Decision (IHDR chunk accept): `type != IHDR || len != 13` (2 conditions)
 * - Vector 1: IHDR/13     -> false (every valid stream)
 * - Vector 2: "JUNK" first -> true via type
 * - Vector 3: IHDR len 12  -> true via len
 */
static void test_png_hostile_chunk_layer(void)
{
  TEST_BEGIN("png hostile: chunk-layer structure arms");
  uint8_t z[k_t_zout_cap];

  /* Non-zero compression method. */
  begin_png(k_t_w, k_t_h, 0U, k_t_ihdr_off_compression, 1U);
  TEST_ASSERT_EQ(k_ra8_err_not_supported, craft_produce(0U));
  /* Non-zero filter method. */
  begin_png(k_t_w, k_t_h, 0U, k_t_ihdr_off_filter, 1U);
  TEST_ASSERT_EQ(k_ra8_err_not_supported, craft_produce(0U));

  /* First chunk is not an IHDR. */
  static const uint8_t sig[8] = {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU};
  s_src_len                   = 0U;
  put(sig, sizeof(sig));
  put_chunk("JUNK", z, k_t_png_ihdr_len);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, craft_produce(0U));

  /* Oversize chunk length field (> 2^31 - 1). */
  begin_png(k_t_w, k_t_h, 0U, 0U, 0U);
  const uint8_t huge[8] = {0x80U, 0U, 0U, 0U, 'j', 'U', 'N', 'K'};
  put(huge, sizeof(huge));
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, craft_produce(0U));

  /* tRNS with an oversize payload. */
  begin_png(k_t_w, k_t_h, 3U, 0U, 0U);
  const uint8_t plte[6] = {1U, 2U, 3U, 4U, 5U, 6U};
  put_chunk("PLTE", plte, sizeof(plte));
  static uint8_t s_big_trns[k_t_trns_oversize_len];
  put_chunk("tRNS", s_big_trns, sizeof(s_big_trns));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, craft_produce(0U));

  /* IEND before any IDAT. */
  begin_png(k_t_w, k_t_h, 0U, 0U, 0U);
  put_chunk("IEND", nullptr, 0U);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, craft_produce(0U));

  TEST_END("png hostile: chunk-layer structure arms");
}

/**
 * @test test_png_hostile_idat_tail
 * @brief The IDAT tail arms: ancillary skip, stray second IDAT, and a
 *        stream that ends mid-zlib before a parked ancillary chunk.
 *
 * @par MC/DC:
 * (single-condition arms behind the chunk-accept decision vectored in
 * test_png_hostile_chunk_layer: each crafted tail drives one refill /
 * finish early-return independently.)
 */
static void test_png_hostile_idat_tail(void)
{
  TEST_BEGIN("png hostile: IDAT tail arms");
  uint8_t  z[k_t_zout_cap];
  uint32_t zlen = 0U;

  /* Ancillary chunk between the last IDAT and IEND (skipped cleanly). */
  begin_png(k_t_w, k_t_h, 0U, 0U, 0U);
  zlen = make_zlib(k_t_w, k_t_h, 0U, z);
  put_chunk("IDAT", z, zlen);
  const uint8_t text[4] = {'o', 'k', 'o', 'k'};
  put_chunk("tEXt", text, sizeof(text));
  put_chunk("IEND", nullptr, 0U);
  TEST_ASSERT_EQ(k_ra8_ok, craft_produce(0U));

  /* A second IDAT after the zlib stream completed. */
  begin_png(k_t_w, k_t_h, 0U, 0U, 0U);
  zlen = make_zlib(k_t_w, k_t_h, 0U, z);
  put_chunk("IDAT", z, zlen);
  put_chunk("IDAT", z, 4U);
  put_chunk("IEND", nullptr, 0U);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, craft_produce(0U));

  /* IDATs end mid-zlib-stream, then a tEXt: the refill parks the pending
   * chunk and the final flush reports the truncation. */
  begin_png(k_t_w, k_t_h, 0U, 0U, 0U);
  zlen = make_zlib(k_t_w, k_t_h, 0U, z);
  put_chunk("IDAT", z, zlen / 2U);
  put_chunk("tEXt", text, sizeof(text));
  put_chunk("IEND", nullptr, 0U);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, craft_produce(0U));
  TEST_END("png hostile: IDAT tail arms");
}

/**
 * @test test_png_hostile_truncation
 * @brief Truncations / transport failures at every phase fail closed.
 *
 * @par MC/DC:
 * (single-condition arms: each truncation point exercises one early-return
 * independently -- signature, IHDR payload, PLTE payload, tRNS payload,
 * IDAT payload, the last IDAT's CRC, and the post-CRC chunk header.)
 */
static void test_png_hostile_truncation(void)
{
  TEST_BEGIN("png hostile: truncation / transport failures per phase");
  uint8_t  z[k_t_zout_cap];
  uint32_t zlen = make_zlib(k_t_w, k_t_h, 0U, z);

  /* Full valid stream as the base for offset-based failure injection. */
  begin_png(k_t_w, k_t_h, 0U, 0U, 0U);
  const size_t after_ihdr = s_src_len;
  put_chunk("IDAT", z, zlen);
  const size_t after_idat_payload = s_src_len - 4U; /* before the CRC  */
  const size_t after_idat_crc     = s_src_len;      /* before IEND hdr */
  put_chunk("IEND", nullptr, 0U);

  /* Transport hard-fails inside the IHDR CRC skip. */
  TEST_ASSERT_EQ(k_ra8_err_hw_error, craft_produce(after_ihdr - 2U));
  /* Transport hard-fails mid-IDAT payload. */
  TEST_ASSERT_EQ(k_ra8_err_hw_error, craft_produce(after_ihdr + 20U));
  /* Truncated at the last IDAT's CRC (finish path, pending unset). */
  TEST_ASSERT_EQ(k_ra8_err_hw_error, craft_produce(after_idat_payload + 2U));
  /* Truncated at the post-CRC chunk header (finish path). */
  TEST_ASSERT_EQ(k_ra8_err_hw_error, craft_produce(after_idat_crc + 2U));

  /* Truncated streams (clean EOF instead of a transport error). */
  const size_t full = s_src_len;
  s_src_len         = after_ihdr - 2U; /* inside the IHDR CRC */
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, craft_produce(0U));
  s_src_len = after_idat_payload + 2U; /* inside the last CRC */
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, craft_produce(0U));
  s_src_len = after_idat_crc + 3U; /* inside the IEND header */
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, craft_produce(0U));
  s_src_len = full;

  /* Truncated mid-IHDR payload. */
  begin_png(k_t_w, k_t_h, 0U, 0U, 0U);
  s_src_len = 8U + 8U + 6U;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, craft_produce(0U));

  /* Truncated mid-PLTE payload. */
  begin_png(k_t_w, k_t_h, 3U, 0U, 0U);
  const uint8_t plte[6] = {1U, 2U, 3U, 4U, 5U, 6U};
  put_chunk("PLTE", plte, sizeof(plte));
  s_src_len -= k_t_trunc_plte_bytes;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, craft_produce(0U));

  /* Truncated mid-tRNS payload. */
  begin_png(k_t_w, k_t_h, 3U, 0U, 0U);
  put_chunk("PLTE", plte, sizeof(plte));
  const uint8_t trns[2] = {9U, 9U};
  put_chunk("tRNS", trns, sizeof(trns));
  s_src_len -= k_t_trunc_trns_bytes;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, craft_produce(0U));
  TEST_END("png hostile: truncation / transport failures per phase");
}

/**
 * @test test_png_hostile_budgets
 * @brief Chunk-budget floods (pre-IDAT, post-IDAT, empty-IDAT) are cut off.
 *
 * @par MC/DC:
 * (each flood exhausts one bounded walk independently: the pre-IDAT
 * ancillary walk, the post-IDAT ancillary walk, and the empty-IDAT refill
 * loop -- NASA Rule 2 loop-bound proof by test.)
 */
static void test_png_hostile_budgets(void)
{
  TEST_BEGIN("png hostile: chunk-budget floods");
  uint8_t       z[k_t_zout_cap];
  const uint8_t text[1] = {'x'};

  /* Flood of ancillary chunks before any IDAT. */
  begin_png(k_t_w, k_t_h, 0U, 0U, 0U);
  for (uint32_t i = 0U; i < (uint32_t)k_t_many; i++) {
    put_chunk("tEXt", text, sizeof(text));
  }
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, craft_produce(0U));

  /* Flood of empty IDATs (the refill loop's own budget). */
  begin_png(k_t_w, k_t_h, 0U, 0U, 0U);
  for (uint32_t i = 0U; i < (uint32_t)k_t_many; i++) {
    put_chunk("IDAT", nullptr, 0U);
  }
  put_chunk("IEND", nullptr, 0U);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, craft_produce(0U));

  /* Flood of ancillary chunks after the stream completed. */
  begin_png(k_t_w, k_t_h, 0U, 0U, 0U);
  const uint32_t zlen = make_zlib(k_t_w, k_t_h, 0U, z);
  put_chunk("IDAT", z, zlen);
  for (uint32_t i = 0U; i < (uint32_t)k_t_many; i++) {
    put_chunk("tEXt", text, sizeof(text));
  }
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, craft_produce(0U));
  TEST_END("png hostile: chunk-budget floods");
}

/**
 * @test test_png_hostile_geometry
 * @brief Producer-level geometry rejections behind a valid PNG prologue.
 *
 * @par MC/DC:
 * (drives the producer geometry hook's tile-grid cap and the pixel-path
 * arena exhaustion, both single-condition rejections behind the compound
 * cap decision already vectored in test_ra8_jof_produce.c.)
 */
static void test_png_hostile_geometry(void)
{
  TEST_BEGIN("png hostile: grid cap + pixel-path arena exhaustion");

  /* 300x300 at 1x1 tiles = 90000 tiles > the 65536 format cap. */
  static uint8_t s_frame[(k_t_frame_dim * k_t_frame_stride)];
  size_t         fo = 0U;
  for (uint32_t y = 0U; y < k_t_frame_dim; y++) {
    s_frame[fo] = 0U;
    fo++;
    for (uint32_t x = 0U; x < k_t_frame_dim; x++) {
      s_frame[fo] = (uint8_t)(x ^ y);
      fo++;
    }
  }
  static uint8_t s_zbig[k_t_zbig_kib * k_t_kib];
  mz_ulong       zl = (mz_ulong)sizeof(s_zbig);
  TEST_ASSERT_EQ(MZ_OK, mz_compress(s_zbig, &zl, s_frame, (mz_ulong)fo));
  begin_png(k_t_frame_dim, k_t_frame_dim, 0U, 0U, 0U);
  put_chunk("IDAT", s_zbig, (uint32_t)zl);
  put_chunk("IEND", nullptr, 0U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 craft_produce_with(1U, 1U, (uint8_t)k_ra8_jof_codec_raw, sizeof(s_work)));

  /* Arena large enough for the deflate scratch but not the PNG carve set
   * (the pixel-path bind fails, not the codec scratch). */
  uint8_t        zz[k_t_zout_cap];
  const uint32_t zzl = make_zlib(k_t_w, k_t_h, 0U, zz);
  begin_png(k_t_w, k_t_h, 0U, 0U, 0U);
  put_chunk("IDAT", zz, zzl);
  put_chunk("IEND", nullptr, 0U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 craft_produce_with((uint16_t)k_t_tile,
                                    (uint16_t)k_t_tile,
                                    (uint8_t)k_ra8_jof_codec_deflate,
                                    (size_t)340U * 1024U /* deflate scratch fits; ring set no */));
  TEST_END("png hostile: grid cap + pixel-path arena exhaustion");
}

/**
 * @test test_png_hostile_ihdr_arms
 * @brief IHDR geometry / length arms behind a good signature.
 *
 * @par MC/DC:
 * Decision (height accept): `h32 == 0 || h32 > max_h` (2 conditions)
 * - Vector 1: valid dims  -> false (every accepted stream)
 * - Vector 2: height 0    -> true via h32 == 0
 * - Vector 3: height 600  -> true via the cap (produce suite's cap tests)
 * Decision (IHDR accept): `type != IHDR || len != 13` (2 conditions)
 * - Vector 1: IHDR/13     -> false (every accepted stream)
 * - Vector 2: "JUNK" first -> true via type (chunk-layer suite)
 * - Vector 3: IHDR len 12 -> true via len (this test)
 */
static void test_png_hostile_ihdr_arms(void)
{
  TEST_BEGIN("png hostile: IHDR height-zero + short-payload arms");

  /* Height 0 with a valid width: the h32 == 0 arm. */
  begin_png(k_t_w, 0U, 0U, 0U, 0U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, craft_produce(0U));

  /* An IHDR-typed first chunk with a 12-byte payload (len != 13). */
  static const uint8_t sig[8] = {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU};
  s_src_len                   = 0U;
  put(sig, sizeof(sig));
  uint8_t short_ihdr[k_t_ihdr_short_len] = {};
  put_chunk("IHDR", short_ihdr, sizeof(short_ihdr));
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, craft_produce(0U));
  TEST_END("png hostile: IHDR height-zero + short-payload arms");
}

/**
 * @test test_png_hostile_palette_arms
 * @brief Every PLTE / tRNS acceptance arm fails closed independently.
 *
 * @par MC/DC:
 * Decision (PLTE accept): `len==0 || len%3 || len>768 || has_plte` (4)
 * - Vector 1: valid PLTE   -> false (colour-type-3 parity tests)
 * - Vector 2: len 0        -> true via len==0
 * - Vector 3: len 6 -> %3 covered by the pixel-layer suite's tRNS craft
 * - Vector 4: len 771      -> true via the cap
 * - Vector 5: second PLTE  -> true via has_plte
 * Decision (tRNS accept): `len>256 || !has_plte || has_trns` (3)
 * - Vector 1: valid tRNS       -> false (colour-type-3 + tRNS parity)
 * - Vector 2: len 300          -> true via the cap (chunk-layer suite)
 * - Vector 3: tRNS before PLTE -> true via has_plte
 * - Vector 4: second tRNS      -> true via has_trns
 */
static void test_png_hostile_palette_arms(void)
{
  TEST_BEGIN("png hostile: PLTE/tRNS ordering + shape arms");
  const uint8_t plte[6] = {1U, 2U, 3U, 4U, 5U, 6U};
  const uint8_t trns[2] = {9U, 9U};

  /* Empty PLTE payload. */
  begin_png(k_t_w, k_t_h, 3U, 0U, 0U);
  put_chunk("PLTE", nullptr, 0U);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, craft_produce(0U));

  /* PLTE longer than 256 entries (771 stays a multiple of 3). */
  begin_png(k_t_w, k_t_h, 3U, 0U, 0U);
  static uint8_t s_big_plte[k_t_plte_oversize_len];
  put_chunk("PLTE", s_big_plte, sizeof(s_big_plte));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, craft_produce(0U));

  /* A second PLTE after a valid one. */
  begin_png(k_t_w, k_t_h, 3U, 0U, 0U);
  put_chunk("PLTE", plte, sizeof(plte));
  put_chunk("PLTE", plte, sizeof(plte));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, craft_produce(0U));

  /* tRNS before any PLTE. */
  begin_png(k_t_w, k_t_h, 3U, 0U, 0U);
  put_chunk("tRNS", trns, sizeof(trns));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, craft_produce(0U));

  /* A second tRNS after a valid one. */
  begin_png(k_t_w, k_t_h, 3U, 0U, 0U);
  put_chunk("PLTE", plte, sizeof(plte));
  put_chunk("tRNS", trns, sizeof(trns));
  put_chunk("tRNS", trns, sizeof(trns));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, craft_produce(0U));
  TEST_END("png hostile: PLTE/tRNS ordering + shape arms");
}

/* @brief Exact-length zlib stream from stored deflate blocks (byte-precise). */
/**
 * @brief Fill `s_raw` with unfiltered scanlines; return the byte count.
 *
 * @param[in] w Image width, pixels (one byte per pixel).
 * @param[in] h Image height, rows.
 *
 * @return Bytes written, including one filter byte per row.
 *
 * @pre `s_raw` is large enough for `h * (w + 1)` bytes.
 * @pre @p w and @p h are non-zero.
 * @post Every row begins with filter type 0 (none).
 * @post The return value is exactly `h * (w + 1)`.
 *
 * @note Not thread-safe; the fixture is file-scope state.
 */
static size_t png_raw_scanlines(uint32_t w, uint32_t h)
{
  size_t raw = 0U;
  for (uint32_t y = 0U; y < h; y++) {
    s_raw[raw] = 0U; /* filter: none */
    raw++;
    for (uint32_t x = 0U; x < w; x++) {
      s_raw[raw] = (uint8_t)(x * y);
      raw++;
    }
  }
  return raw;
}

/**
 * @brief Emit one DEFLATE stored block header (BFINAL/BTYPE, LEN, NLEN).
 *
 * @param[out] out   Output buffer at the header position.
 * @param[in]  len   Payload length this block carries.
 * @param[in]  final True when this is the last block of the stream.
 *
 * @return Bytes written (always the fixed 5-byte stored header).
 *
 * @pre @p out has room for five bytes.
 * @pre @p len fits in 16 bits, as a stored block requires.
 * @post LEN and NLEN are written little-endian and are one's complements.
 * @post BTYPE is 00 (stored), so the payload follows uncompressed.
 *
 * @note Thread-safe: writes only through @p out.
 */
static size_t deflate_stored_header(uint8_t* out, uint16_t len, bool final)
{
  size_t o = 0U;
  out[o]   = final ? 1U : 0U; /* BFINAL + BTYPE 00 (stored) */
  o++;
  out[o] = (uint8_t)(len & k_t_byte_mask);
  o++;
  out[o] = (uint8_t)(len >> 8U);
  o++;
  const uint16_t nlen = (uint16_t)~len;
  out[o]              = (uint8_t)(nlen & k_t_byte_mask);
  o++;
  out[o] = (uint8_t)(nlen >> 8U);
  o++;
  return o;
}

/**
 * @brief Append a big-endian Adler-32 trailer over the raw bytes.
 *
 * @param[out] out Output buffer at the trailer position.
 * @param[in]  raw Number of `s_raw` bytes the checksum covers.
 *
 * @return Bytes written (always four).
 *
 * @pre @p out has room for four bytes.
 * @pre `s_raw` holds at least @p raw bytes.
 * @post The checksum is stored most-significant byte first, per RFC 1950.
 * @post The stream is complete once this returns.
 *
 * @note Not thread-safe; reads the file-scope `s_raw`.
 */
static size_t zlib_adler_trailer(uint8_t* out, size_t raw)
{
  const mz_ulong adler = mz_adler32(mz_adler32(0U, nullptr, 0U), s_raw, (mz_ulong)raw);
  size_t         o     = 0U;
  out[o]               = (uint8_t)((adler >> k_t_be32_hi_shift) & k_t_byte_mask);
  o++;
  out[o] = (uint8_t)((adler >> 16U) & k_t_byte_mask);
  o++;
  out[o] = (uint8_t)((adler >> 8U) & k_t_byte_mask);
  o++;
  out[o] = (uint8_t)(adler & k_t_byte_mask);
  o++;
  return o;
}

static uint32_t make_zlib_stored(uint32_t w, uint32_t h, uint8_t* out)
{
  const size_t raw = png_raw_scanlines(w, h);

  size_t o = 0U;
  out[o]   = k_t_zlib_cmf; /* zlib CMF */
  o++;
  out[o] = 0x01U; /* zlib FLG (checksum-valid, no dict) */
  o++;

  /* Two stored blocks (half the payload each) so the decoder must cross a
   * block boundary mid-image. */
  size_t left = raw;
  size_t pos  = 0U;
  while (left > 0U) {
    const size_t blk = (left > (raw / 2U)) ? (raw / 2U) : left;
    o += deflate_stored_header(&out[o], (uint16_t)blk, blk == left);
    memcpy(&out[o], &s_raw[pos], blk);
    o += blk;
    pos += blk;
    left -= blk;
  }
  o += zlib_adler_trailer(&out[o], raw);
  return (uint32_t)o;
}

/**
 * @test test_png_hostile_residue
 * @brief Trailing-residue termination arms: a partial extra scanline and
 *        unread IDAT bytes past a window-aligned zlib stream.
 *
 * @par MC/DC:
 * Decision (row termination): `rows_done != h || rowfill != 0` (2)
 * - Vector 1: exact frame     -> false (every accepted stream)
 * - Vector 2: short frame     -> true via rows_done (pixel-layer suite)
 * - Vector 3: +3 stray bytes  -> true via rowfill (this test)
 * Decision (IDAT termination): `in_pos != in_avail || idat_rem != 0` (2)
 * - Vector 1: clean end            -> false (every accepted stream)
 * - Vector 2: in-window garbage    -> true via in_pos (chunk-layer suite)
 * - Vector 3: garbage past a 4096-aligned stream -> true via idat_rem
 */
static void test_png_hostile_residue(void)
{
  TEST_BEGIN("png hostile: partial-row + past-window IDAT residue");

  /* h full rows plus 3 stray bytes: rowfill residue at termination. */
  begin_png(k_t_w, k_t_h, 0U, 0U, 0U);
  size_t o = 0U;
  for (uint32_t y = 0U; y < (uint32_t)k_t_h; y++) {
    s_raw[o] = 0U;
    o++;
    for (uint32_t x = 0U; x < (uint32_t)k_t_w; x++) {
      s_raw[o] = (uint8_t)(x + y);
      o++;
    }
  }
  s_raw[o] = 0U; /* stray partial-row bytes */
  o++;
  s_raw[o] = 1U;
  o++;
  s_raw[o] = 2U;
  o++;
  uint8_t  z[k_t_zout_cap];
  mz_ulong zlen = (mz_ulong)k_t_zout_cap;
  TEST_ASSERT_EQ(MZ_OK, mz_compress(z, &zlen, s_raw, (mz_ulong)o));
  put_chunk("IDAT", z, (uint32_t)zlen);
  put_chunk("IEND", nullptr, 0U);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, craft_produce(0U));

  /* A 15x255 gray frame built from stored deflate blocks is exactly 4096
   * zlib bytes -- one full input window. Garbage after it in the same IDAT
   * is never windowed, so termination sees it via idat_rem alone. */
  static uint8_t s_zbuf[k_t_zbuf_cap + k_png_hostile_trailing_len];
  const uint32_t zl = make_zlib_stored(15U, 255U, s_zbuf);
  TEST_ASSERT_EQ(4096U, zl);
  memset(&s_zbuf[zl], k_png_hostile_fill_trailing, (size_t)k_png_hostile_trailing_len);
  begin_png(k_t_window_frame_w, k_t_window_frame_h, 0U, 0U, 0U);
  put_chunk("IDAT", s_zbuf, zl + k_png_hostile_trailing_len);
  put_chunk("IEND", nullptr, 0U);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, craft_produce(0U));
  TEST_END("png hostile: partial-row + past-window IDAT residue");
}

/** @brief No-op ::ra8_jof_geom_fn for the direct-seam arms. */
static ra8_err_t t_geom_ok(void* ctx, uint16_t width, uint16_t height, uint8_t channels)
{
  (void)ctx;
  (void)width;
  (void)height;
  (void)channels;
  return k_ra8_ok;
}

/** @brief No-op ::ra8_jof_rows_fn for the direct-seam arms. */
static ra8_err_t t_rows_ok(void*          ctx,
                           const uint8_t* px,
                           uint16_t       width,
                           uint16_t       y0,
                           uint16_t       nrows,
                           uint8_t        channels)
{
  (void)ctx;
  (void)px;
  (void)width;
  (void)y0;
  (void)nrows;
  (void)channels;
  return k_ra8_ok;
}

/**
 * @test test_png_hostile_direct_seam
 * @brief Arms only a broken transport can reach, driven through the module
 *        seam directly (signature pull failure, bad signature, null guards).
 *
 * @par MC/DC:
 * (single-condition guards; one vector each through the RA8_PRIV seam per
 * the CLAUDE.md internal-symbol test allowance.)
 */
static void test_png_hostile_direct_seam(void)
{
  TEST_BEGIN("png hostile: direct seam (signature + null guards)");
  static uint8_t  s_arena[k_t_arena_kib * k_t_kib];
  ra8_jof_bump_t  bump = {.base = s_arena, .cap = sizeof(s_arena), .off = 0U};
  static t_pull_t s_pull;

  /* Transport fails during the signature read. */
  s_src_len                 = 0U;
  const uint8_t half_sig[4] = {0x89U, 'P', 'N', 'G'};
  put(half_sig, sizeof(half_sig));
  s_pull = (t_pull_t){.pos = 0U, .fail_at = 2U};
  TEST_ASSERT_EQ(
    k_ra8_err_hw_error,
    ra8_jof_priv_png_rows(t_pull, &s_pull, &bump, 512U, 512U, t_geom_ok, t_rows_ok, nullptr));

  /* Wrong signature bytes. */
  s_src_len                = 0U;
  const uint8_t bad_sig[8] = {'N', 'O', 'T', 'A', 'P', 'N', 'G', '!'};
  put(bad_sig, sizeof(bad_sig));
  s_pull = (t_pull_t){.pos = 0U, .fail_at = 0U};
  TEST_ASSERT_EQ(
    k_ra8_err_protocol_error,
    ra8_jof_priv_png_rows(t_pull, &s_pull, &bump, 512U, 512U, t_geom_ok, t_rows_ok, nullptr));

  /* Null guards on the seam itself: pull, bump, and each hook in turn. */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_jof_priv_png_rows(nullptr, &s_pull, &bump, 512U, 512U, t_geom_ok, t_rows_ok, nullptr));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_jof_priv_png_rows(t_pull, &s_pull, nullptr, 512U, 512U, t_geom_ok, t_rows_ok, nullptr));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_jof_priv_png_rows(t_pull, &s_pull, &bump, 512U, 512U, nullptr, t_rows_ok, nullptr));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_jof_priv_png_rows(t_pull, &s_pull, &bump, 512U, 512U, t_geom_ok, nullptr, nullptr));
  TEST_END("png hostile: direct seam (signature + null guards)");
}

/**
 * @brief Test entry point -- runs the hostile-PNG corpus in order.
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
  test_png_hostile_pixel_layer();
  test_png_hostile_chunk_layer();
  test_png_hostile_idat_tail();
  test_png_hostile_truncation();
  test_png_hostile_budgets();
  test_png_hostile_geometry();
  test_png_hostile_ihdr_arms();
  test_png_hostile_palette_arms();
  test_png_hostile_residue();
  test_png_hostile_direct_seam();
  (void)fprintf(stderr, "[OK  ] test_ra8_jof_png_hostile.c\n");
  return 0;
}
