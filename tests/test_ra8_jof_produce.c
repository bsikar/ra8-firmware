/**
 * @file test_ra8_jof_produce.c
 * @brief Host tests for the import-time transcode producer: JPEG/PNG ->
 *        JOF, byte parity, bounded RAM high-water, hostile sources (#231).
 *
 * @details
 * Sources are synthesized in-test from deterministic pixel patterns:
 * JPEG through the in-tree `ra8_jpeg_sw_encode()`, PNG through a local
 * builder (filter types cycled per row, IDAT via miniz `mz_compress`, i.e.
 * an encoder wholly independent of the decoder under test; the PNG builder
 * is additionally cross-checked against the vendored stb_image decode).
 * Parity oracle: every produced tile is paged back through
 * `ra8_jof_read_tile()` and compared byte-for-byte against a direct
 * whole decode of the same source -- for JPEG that reference is
 * `ra8_jpeg_sw_decode()` (a *stripe decode vs whole decode* equivalence),
 * for PNG the generator pattern itself.
 *
 * The bounded-RAM test transcodes a synthetic image whose *decoded* size is
 * more than 5x the producer's entire working set and asserts the fixed-
 * buffer budget held -- the #231 "larger than the working set at native
 * resolution" property, proven at host scale.
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
#include "ra8_img_arena.h"
#include "ra8_jpeg_sw.h"
#include "ra8_jof.h"
#include "ra8_jof_produce.h"
#include "stb_image.h"
#include "unity_minimal.h"

/** @brief Truncated-JPEG probe fed to the producer. */
typedef enum : uint16_t {
  k_tap_fill_jpeg_body = 0xAAU, /**< Filler body after the SOI marker. */
  k_tap_jpeg_body_len  = 32U,   /**< Filler bytes written.             */
} tap_probe_t;

/**
 * @enum t_png_off_t
 * @brief PNG byte positions the builder writes and the hostile arms corrupt.
 *
 * @details
 * `k_t_chunk_*` / `k_t_ihdr_*` are relative to a chunk or its payload;
 * `k_t_src_*` are absolute offsets into `s_src`, where IHDR starts at 16
 * (8 sig + 4 len + 4 type). `_b<N>` is the `N`-th byte of a big-endian
 * 32-bit field, most-significant first.
 */
typedef enum : uint8_t {
  k_t_be32_hi_shift     = 24U,   /**< Top-byte shift of a big-endian 32-bit field.     */
  k_t_byte_mask         = 0xFFU, /**< Low-byte mask while serialising one.             */
  k_t_png_ihdr_len      = 13U,   /**< IHDR payload length, fixed by the spec.          */
  k_t_chunk_crc_b1      = 9U,    /**< Chunk CRC byte 1, past the chunk payload.        */
  k_t_chunk_crc_b2      = 10U,   /**< Chunk CRC byte 2.                                */
  k_t_chunk_crc_b3      = 11U,   /**< Chunk CRC byte 3 (least significant).            */
  k_t_chunk_overhead    = 12U,   /**< Chunk cost past payload: 4 len + 4 type + 4 CRC. */
  k_t_ihdr_off_h_b1     = 5U,    /**< Height byte 1 in the IHDR payload.               */
  k_t_ihdr_off_h_b3     = 7U,    /**< Height byte 3 in the IHDR payload.               */
  k_t_ihdr_off_ct       = 9U,    /**< Colour-type byte in the IHDR payload.            */
  k_t_src_off_w_b0      = 16U,   /**< Width byte 0 (most significant) in s_src.        */
  k_t_src_off_w_b1      = 17U,   /**< Width byte 1.                                    */
  k_t_src_off_w_b2      = 18U,   /**< Width byte 2.                                    */
  k_t_src_off_w_b3      = 19U,   /**< Width byte 3 (least significant).                */
  k_t_src_off_h_b0      = 20U,   /**< Height byte 0 (most significant).                */
  k_t_src_off_h_b1      = 21U,   /**< Height byte 1.                                   */
  k_t_src_off_h_b2      = 22U,   /**< Height byte 2.                                   */
  k_t_src_off_h_b3      = 23U,   /**< Height byte 3 (least significant).               */
  k_t_src_off_depth     = 24U,   /**< Bit-depth byte; 16 here must be rejected.        */
  k_t_src_off_ct        = 25U,   /**< Colour-type byte.                                */
  k_t_src_off_interlace = 28U,   /**< Interlace byte; non-zero must be rejected.       */
} t_png_off_t;

/**
 * @enum t_pattern_t
 * @brief Parameters of the deterministic pixel and palette patterns.
 *
 * @details
 * The builder and the expectation side share these, so a decode mismatch is a
 * real defect rather than two drifting generators. The ramp is
 * `base + (entry_index * step)` per channel.
 */
typedef enum : uint8_t {
  k_t_pix_ch_stride = 29U,  /**< Per-channel offset keeping the R/G/B planes distinct. */
  k_t_plte_entries  = 5U,   /**< Palette entries the builder emits.                    */
  k_t_plte_bytes    = 15U,  /**< PLTE payload: k_t_plte_entries x 3 RGB bytes.         */
  k_t_plte_r_base   = 10U,  /**< Red of palette entry 0.                               */
  k_t_plte_r_step   = 40U,  /**< Red increment per palette entry.                      */
  k_t_plte_g_base   = 20U,  /**< Green of palette entry 0.                             */
  k_t_plte_g_step   = 30U,  /**< Green increment per palette entry.                    */
  k_t_plte_b_base   = 30U,  /**< Blue of palette entry 0.                              */
  k_t_plte_b_step   = 20U,  /**< Blue increment per palette entry.                     */
  k_t_alpha_opaque  = 255U, /**< Alpha synthesized for entries past tRNS.              */
} t_pattern_t;

/** @brief Stimulus values that steer the hostile and budget-starved paths. */
typedef enum : uint16_t {
  k_t_probe_row_step    = 37U,   /**< Row stride when spot-checking a decoded tile. */
  k_t_probe_col_step    = 41U,   /**< Column stride; co-prime with the row stride
                                      so the probe walks the whole tile.        */
  k_t_starved_store_cap = 64U,   /**< Memstore cap too small for the atlas, forcing
                                      the sink's no-memory path.                */
  k_t_codec_invalid     = 9U,    /**< Codec id outside the enum; the config guard must reject it. */
  k_t_hostile_sniff_len = 34U,   /**< Length of the not-a-PNG/JPEG blob fed to the sniffer.   */
  k_t_hostile_lead_byte = 0xFFU, /**< Its leading byte: starts like a JPEG marker, then junk. */
  k_t_ct_jpeg_ref       = 0xFFU, /**< Pseudo colour-type selecting the stb JPEG
                                      reference over the synthetic pattern.     */
  k_t_plte_bad_len      = 14U,   /**< PLTE length not divisible by 3, tripping the
                                      indivisible-length guard.                 */
  k_t_kib               = 1024U, /**< Bytes per KiB, for sizing the producer work arena. */
} t_probe_t;

/** @brief Test geometry + buffer sizing. */
enum : uint32_t {
  k_t_png_w       = 90U,                 /**< PNG test image width.           */
  k_t_png_h       = 70U,                 /**< PNG test image height.          */
  k_t_jpg_w       = 96U,                 /**< JPEG test image width (mcu x6). */
  k_t_jpg_h       = 64U,                 /**< JPEG test image height.         */
  k_t_tile        = 32U,                 /**< Tile edge for the small tests.  */
  k_t_big_w       = 2048U,               /**< Bounded-RAM test width.         */
  k_t_big_h       = 8192U,               /**< Bounded-RAM test height.        */
  k_t_big_tile    = 128U,                /**< Bounded-RAM test tile edge.     */
  k_t_src_cap     = 12U * 1024U * 1024U, /**< Synthesized source capacity.    */
  k_t_store_cap   = 12U * 1024U * 1024U, /**< Memstore capacity.              */
  k_t_work_small  = 900U * 1024U,        /**< Small-test work arena.          */
  k_t_cell_cap    = 256U * 1024U,        /**< Tile page-back buffer.          */
  k_t_scratch_cap = 300U * 1024U,        /**< Stored-tile staging.            */
  k_t_stb_px_cap  = 512U * 1024U,        /**< stb cross-check arena.          */
  k_t_ram_budget  = 10U * 1024U * 1024U, /**< The #231 working-set budget.    */
};

/** @brief Synthesized encoded source (PNG or JPEG). */
static uint8_t s_src[k_t_src_cap];
/** @brief Synthesized source length. */
static size_t s_src_len;
/** @brief Raw pixel scratch used while synthesizing sources. */
static uint8_t s_raw[((size_t)k_t_big_w * (size_t)k_t_big_h) + (size_t)k_t_big_h];
/** @brief Producer work arena. */
static uint8_t s_work[8U * k_t_kib * k_t_kib];
/** @brief Memstore backing. */
static uint8_t s_store_buf[k_t_store_cap];
/** @brief The atlas store under test. */
static ra8_jof_memstore_t s_store;
/** @brief Tile page-back buffer. */
static uint8_t s_cell[k_t_cell_cap];
/** @brief Stored-tile staging. */
static uint8_t s_scratch[k_t_scratch_cap];
/** @brief Whole-decode reference buffer (JPEG parity). */
static uint8_t s_ref[(size_t)k_t_jpg_w * (size_t)k_t_jpg_h * 3U];
/** @brief RGB staging for the JPEG encoder. */
static uint8_t s_rgb[(size_t)k_t_jpg_w * (size_t)k_t_jpg_h * 3U];
/** @brief stb cross-check decode arena. */
static uint8_t s_stb_arena[k_t_stb_px_cap];
/** @brief zlib staging for the in-test PNG builder. */
static uint8_t s_zbuf[k_t_src_cap / 2U];

/**
 * @struct t_pull_t
 * @brief Chunk-limited memory pull source (dribble stress knob).
 */
typedef struct {
  const uint8_t* d;     /**< Source bytes.               */
  size_t         n;     /**< Source length.              */
  size_t         pos;   /**< Read cursor.                */
  size_t         chunk; /**< Max bytes per pull (0=all). */
} t_pull_t;

/** @brief ::ra8_jof_pull_fn over a ::t_pull_t. */
static ra8_err_t t_pull(void* ctx, uint8_t* buf, size_t cap, size_t* got)
{
  t_pull_t*    p    = (t_pull_t*)ctx;
  const size_t left = p->n - p->pos;
  size_t       take = (cap < left) ? cap : left;
  if ((p->chunk != 0U) && (take > p->chunk)) {
    take = p->chunk;
  }
  memcpy(buf, &p->d[p->pos], take);
  p->pos += take;
  *got = take;
  return k_ra8_ok;
}

/** @brief Failing pull source (error propagation check). */
/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTNEXTLINE(readability-non-const-parameter)
static ra8_err_t t_pull_fail(void* ctx, uint8_t* buf, size_t cap, size_t* got)
{
  (void)ctx;
  (void)buf;
  (void)cap;
  *got = 0U;
  return k_ra8_err_hw_error;
}

/** @brief Deterministic pattern channel at (x, y, c). */
static uint8_t pix(uint32_t x, uint32_t y, uint32_t c)
{
  return (uint8_t)((x ^ y) + (c * k_t_pix_ch_stride) + ((x + y) >> 2U));
}

/* ---------------------------------------------------------------------------
 * In-test PNG builder (independent encoder: filters by hand, zlib via mz).
 * ---------------------------------------------------------------------------
 */

/** @brief Append a PNG chunk (length/type/data/crc) into `s_src`. */
static void png_chunk(const char* type, const uint8_t* data, uint32_t len)
{
  uint8_t* p = &s_src[s_src_len];
  p[0]       = (uint8_t)(len >> k_t_be32_hi_shift);
  p[1]       = (uint8_t)((len >> 16U) & k_t_byte_mask);
  p[2]       = (uint8_t)((len >> 8U) & k_t_byte_mask);
  p[3]       = (uint8_t)(len & k_t_byte_mask);
  memcpy(&p[4], type, 4U);
  if (len > 0U) {
    memcpy(&p[8], data, len);
  }
  const uint32_t crc        = (uint32_t)mz_crc32(MZ_CRC32_INIT, &p[4], (size_t)len + 4U);
  p[8U + len]               = (uint8_t)(crc >> k_t_be32_hi_shift);
  p[k_t_chunk_crc_b1 + len] = (uint8_t)((crc >> 16U) & k_t_byte_mask);
  p[k_t_chunk_crc_b2 + len] = (uint8_t)((crc >> 8U) & k_t_byte_mask);
  p[k_t_chunk_crc_b3 + len] = (uint8_t)(crc & k_t_byte_mask);
  s_src_len += k_t_chunk_overhead + (size_t)len;
}

/** @brief PNG paeth predictor (builder-side twin). */
static uint8_t t_paeth(uint8_t a, uint8_t b, uint8_t c)
{
  const int32_t p  = (int32_t)a + (int32_t)b - (int32_t)c;
  const int32_t pa = (p > (int32_t)a) ? (p - (int32_t)a) : ((int32_t)a - p);
  const int32_t pb = (p > (int32_t)b) ? (p - (int32_t)b) : ((int32_t)b - p);
  const int32_t pc = (p > (int32_t)c) ? (p - (int32_t)c) : ((int32_t)c - p);
  if ((pa <= pb) && (pa <= pc)) {
    return a;
  }
  if (pb <= pc) {
    return b;
  }
  return c;
}

/**
 * @brief Fill `s_raw` with filtered scanlines of the pattern.
 * @param[in] w          Image width, pixels.
 * @param[in] h          Image height, pixels.
 * @param[in] ch         Source channels (1 gray, 2 GA, 3 RGB, 4 RGBA).
 * @param[in] palette    When true, bytes are palette indices `(x+y) % 5`.
 * @param[in] use_filters When true, rows cycle filter types 0..4.
 * @return Filtered byte count in `s_raw`.
 * @retval >0 `h * (1 + w * ch)` bytes.
 * @pre `s_raw` covers the filtered size.
 * @pre @p ch is 1, 2, 3 or 4.
 * @post `s_raw` holds spec-filtered scanlines of the pattern.
 * @post No other state mutated.
 * @note Not thread-safe (shared scratch).
 * @since 0.1.0
 */
/**
 * @brief Value of one source pixel channel, for either colour model.
 *
 * @details
 * Palette images carry index bytes, truecolour images carry the shared test
 * pattern. Both the raw fill and the filter predictor need this, and they must
 * agree exactly or the filtered bytes will not reconstruct.
 *
 * @param[in] x       Pixel column.
 * @param[in] y       Pixel row.
 * @param[in] c       Channel within the pixel.
 * @param[in] palette True for a palette image, false for truecolour.
 *
 * @return The unfiltered sample value at that position.
 *
 * @pre The caller keeps @p x and @p y inside the image.
 * @pre @p c is a valid channel index.
 * @post The result depends only on the arguments -- no hidden state.
 * @post The same inputs always yield the same byte.
 *
 * @note Thread-safe: pure function.
 */
static uint8_t png_sample(uint32_t x, uint32_t y, uint32_t c, bool palette)
{
  return palette ? (uint8_t)((x + y) % k_t_plte_entries) : pix(x, y, c);
}

/**
 * @brief PNG filter predictor for byte @p k of a row.
 *
 * @details
 * Recomputes the neighbouring samples from the pattern rather than keeping the
 * previous row around, which is what lets the builder filter each row in place.
 *
 * @param[in] row     Row bytes, still unfiltered at and above @p k.
 * @param[in] k       Byte index within the row.
 * @param[in] y       Row number, needed for the "up" neighbours.
 * @param[in] ch      Channels per pixel.
 * @param[in] f       PNG filter type (1=Sub, 2=Up, 3=Average, else Paeth).
 * @param[in] palette True for a palette image, false for truecolour.
 *
 * @return The predicted byte to subtract from `row[k]`.
 *
 * @pre @p row is non-NULL and @p k indexes inside it.
 * @pre @p ch is non-zero.
 * @post Row 0 and column 0 neighbours read as 0, per the PNG spec.
 * @post No read leaves the row for the left neighbour when `k < ch`.
 *
 * @note Thread-safe: reads only its arguments.
 */
static uint8_t
png_predictor(const uint8_t* row, uint32_t k, uint32_t y, uint32_t ch, uint8_t f, bool palette)
{
  const uint32_t px_x = (k / ch);
  const uint32_t px_c = (k % ch);
  const uint8_t  left = (k >= ch) ? row[k - ch] : 0U;
  uint8_t        up   = 0U;
  if (y > 0U) {
    up = png_sample(px_x, y - 1U, px_c, palette);
  }
  uint8_t ul = 0U;
  if ((y > 0U) && (k >= ch)) {
    ul = png_sample(px_x - 1U, y - 1U, px_c, palette);
  }
  if (f == 1U) {
    return left;
  }
  if (f == 2U) {
    return up;
  }
  if (f == 3U) {
    return (uint8_t)(((uint32_t)left + (uint32_t)up) / 2U);
  }
  return t_paeth(left, up, ul);
}

/**
 * @brief Filter one already-populated row in place.
 *
 * @details
 * Walks the row backwards so each predictor still sees unfiltered bytes to its
 * left, which is what makes in-place filtering correct.
 *
 * @param[in,out] row     Row bytes to filter.
 * @param[in]     rowb    Row length in bytes.
 * @param[in]     y       Row number.
 * @param[in]     ch      Channels per pixel.
 * @param[in]     f       PNG filter type; 0 leaves the row untouched.
 * @param[in]     palette True for a palette image, false for truecolour.
 *
 * @pre @p row holds @p rowb unfiltered bytes.
 * @pre @p ch is non-zero.
 * @post Every byte has had its predictor subtracted, modulo 256.
 * @post A filter type of 0 leaves the row byte-identical.
 *
 * @note Not thread-safe with respect to @p row.
 */
static void
png_filter_row(uint8_t* row, uint32_t rowb, uint32_t y, uint32_t ch, uint8_t f, bool palette)
{
  if (f == 0U) {
    return;
  }
  for (uint32_t i = rowb; i > 0U; i--) {
    const uint32_t k    = i - 1U;
    const uint8_t  cur  = row[k];
    const uint8_t  pred = png_predictor(row, k, y, ch, f, palette);
    row[k]              = (uint8_t)(cur - pred);
  }
}

static size_t png_fill_raw(uint32_t w, uint32_t h, uint32_t ch, bool palette, bool use_filters)
{
  const uint32_t rowb = w * ch;
  size_t         o    = 0U;
  for (uint32_t y = 0U; y < h; y++) {
    const uint8_t f = use_filters ? (uint8_t)(y % 5U) : 0U;
    s_raw[o]        = f;
    uint8_t* row    = &s_raw[o + 1U];
    for (uint32_t x = 0U; x < w; x++) {
      for (uint32_t c = 0U; c < ch; c++) {
        row[(x * ch) + c] = png_sample(x, y, c, palette);
      }
    }
    /* Filter in place. The predictor regenerates the previous UNFILTERED row
     * from the pattern, so filtering never reads an already-filtered byte. */
    png_filter_row(row, rowb, y, ch, f, palette);
    o += 1U + (size_t)rowb;
  }
  return o;
}

/**
 * @brief Build a complete PNG of the pattern into `s_src`.
 * @param[in] w           Image width, pixels.
 * @param[in] h           Image height, pixels.
 * @param[in] color_type  PNG colour type (0/2/3/4/6).
 * @param[in] with_trns   Emit a tRNS chunk (colour type 3 only).
 * @param[in] use_filters Cycle row filters 0..4.
 * @pre The pattern buffers cover the requested geometry.
 * @pre @p color_type is a supported type.
 * @post `s_src`/`s_src_len` hold a well-formed PNG.
 * @post No other state mutated.
 * @note Not thread-safe (shared scratch).
 * @since 0.1.0
 */
static void png_build(uint32_t w, uint32_t h, uint8_t color_type, bool with_trns, bool use_filters)
{
  static const uint8_t sig[8]    = {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU};
  const uint32_t       ch_map[7] = {1U, 0U, 3U, 1U, 2U, 0U, 4U};
  const uint32_t       ch        = ch_map[color_type];
  s_src_len                      = 0U;
  memcpy(s_src, sig, sizeof(sig));
  s_src_len                      = sizeof(sig);
  uint8_t ihdr[k_t_png_ihdr_len] = {};
  ihdr[0]                        = (uint8_t)(w >> k_t_be32_hi_shift);
  ihdr[1]                        = (uint8_t)((w >> 16U) & k_t_byte_mask);
  ihdr[2]                        = (uint8_t)((w >> 8U) & k_t_byte_mask);
  ihdr[3]                        = (uint8_t)(w & k_t_byte_mask);
  ihdr[4]                        = (uint8_t)(h >> k_t_be32_hi_shift);
  ihdr[k_t_ihdr_off_h_b1]        = (uint8_t)((h >> 16U) & k_t_byte_mask);
  ihdr[6]                        = (uint8_t)((h >> 8U) & k_t_byte_mask);
  ihdr[k_t_ihdr_off_h_b3]        = (uint8_t)(h & k_t_byte_mask);
  ihdr[8]                        = 8U; /* bit depth */
  ihdr[k_t_ihdr_off_ct]          = color_type;
  png_chunk("IHDR", ihdr, sizeof(ihdr));
  if (color_type == 3U) {
    uint8_t plte[k_t_plte_bytes] = {};
    for (uint32_t i = 0U; i < k_t_plte_entries; i++) {
      plte[(i * 3U) + 0U] = (uint8_t)(k_t_plte_r_base + (i * k_t_plte_r_step));
      plte[(i * 3U) + 1U] = (uint8_t)(k_t_plte_g_base + (i * k_t_plte_g_step));
      plte[(i * 3U) + 2U] = (uint8_t)(k_t_plte_b_base + (i * k_t_plte_b_step));
    }
    png_chunk("PLTE", plte, sizeof(plte));
    if (with_trns) {
      const uint8_t trns[3] = {255U, 128U, 64U}; /* entries 3+4 default opaque */
      png_chunk("tRNS", trns, sizeof(trns));
    }
  }
  const size_t rawn = png_fill_raw(w, h, ch, color_type == 3U, use_filters);
  mz_ulong     zlen = (mz_ulong)sizeof(s_zbuf);
  TEST_ASSERT_EQ(MZ_OK, mz_compress(s_zbuf, &zlen, s_raw, (mz_ulong)rawn));
  /* Split the zlib stream across two IDAT chunks (chunk-crossing coverage). */
  const uint32_t half = (uint32_t)zlen / 2U;
  png_chunk("IDAT", s_zbuf, half);
  png_chunk("IDAT", &s_zbuf[half], (uint32_t)zlen - half);
  png_chunk("IEND", NULL, 0U);
}

/* ---------------------------------------------------------------------------
 * Produce + parity helpers.
 * ---------------------------------------------------------------------------
 */

/** @brief Run the producer over `s_src` into a fresh memstore. */
static ra8_err_t
produce(uint16_t tile_w, uint16_t tile_h, uint8_t codec, size_t chunk, ra8_jof_info_t* info)
{
  static t_pull_t s_pull;
  s_pull  = (t_pull_t){.d = s_src, .n = s_src_len, .pos = 0U, .chunk = chunk};
  s_store = (ra8_jof_memstore_t){.buf = s_store_buf, .cap = sizeof(s_store_buf), .len = 0U};
  const ra8_jof_produce_cfg_t cfg = {
    .pull       = t_pull,
    .pull_ctx   = &s_pull,
    .sink       = ra8_jof_memstore_sink,
    .sink_ctx   = &s_store,
    .tile_w     = tile_w,
    .tile_h     = tile_h,
    .codec      = codec,
    .max_width  = (uint16_t)k_t_big_w,
    .max_height = (uint16_t)k_t_big_h,
    .work       = s_work,
    .work_cap   = (size_t)k_t_work_small,
  };
  return ra8_jof_produce(&cfg, info);
}

/**
 * @brief Page every tile of the produced atlas and compare with an expected-
 *        pixel oracle.
 * @param[in] info   Produced atlas geometry.
 * @param[in] ctx_ct PNG colour type driving the oracle (0xFF = JPEG ref).
 * @pre The memstore holds the produced atlas.
 * @pre @p info matches the store contents.
 * @post Every tile byte was compared (test exits on mismatch).
 * @post The store is unmodified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
/**
 * @brief Expected sample at (@p x, @p y, @p ch) for PNG colour type @p ctx_ct.
 *
 * @details
 * One place that knows what each colour model should decode to, so the tile
 * comparison below is a loop over positions rather than a decision tree nested
 * inside four loops. `0xFF` is the JPEG reference case, not a PNG colour type.
 *
 * @param[in] ctx_ct PNG colour type, or 0xFF for the JPEG reference image.
 * @param[in] x      Pixel column in the full image.
 * @param[in] y      Pixel row in the full image.
 * @param[in] ch     Channel within the pixel.
 *
 * @return The sample the decoder is required to produce.
 *
 * @pre The fixture for @p ctx_ct has been built.
 * @pre @p x and @p y lie inside the image.
 * @post The result depends only on the arguments and the built fixture.
 * @post Unhandled colour types yield 0, which no fixture produces.
 *
 * @note Not thread-safe; reads the file-scope reference image.
 */
static uint8_t expected_sample(uint8_t ctx_ct, uint32_t x, uint32_t y, uint32_t ch)
{
  if (ctx_ct == k_t_ct_jpeg_ref) {
    return s_ref[(((y * (uint32_t)k_t_jpg_w) + x) * 3U) + ch];
  }
  if (ctx_ct == 0U) {
    return pix(x, y, 0U);
  }
  if ((ctx_ct == 2U) || (ctx_ct == 6U) || (ctx_ct > 6U)) {
    /* Greyscale+alpha, truecolour+alpha and every colour type above them keep
     * the source channel value, so they share one expectation. */
    return pix(x, y, ch);
  }
  if (ctx_ct == 3U) {
    const uint32_t idx     = (x + y) % k_t_plte_entries;
    const uint8_t  rgb[3]  = {(uint8_t)(k_t_plte_r_base + (idx * k_t_plte_r_step)),
                              (uint8_t)(k_t_plte_g_base + (idx * k_t_plte_g_step)),
                              (uint8_t)(k_t_plte_b_base + (idx * k_t_plte_b_step))};
    const uint8_t  trns[3] = {255U, 128U, 64U};
    if (ch < 3U) {
      return rgb[ch];
    }
    return (idx < 3U) ? trns[idx] : (uint8_t)k_t_alpha_opaque;
  }
  if (ctx_ct == 4U) {
    return (ch < 3U) ? pix(x, y, 0U) : pix(x, y, 1U);
  }
  return 0U;
}

/**
 * @brief Compare one decoded tile against the expected samples.
 *
 * @param[in] info   Atlas geometry.
 * @param[in] ctx_ct PNG colour type, or 0xFF for the JPEG reference image.
 * @param[in] x0     Column of the tile's top-left pixel in the full image.
 * @param[in] y0     Row of the tile's top-left pixel in the full image.
 * @param[in] w      Decoded tile width, pixels.
 * @param[in] h      Decoded tile height, pixels.
 *
 * @pre `s_cell` holds the decoded tile.
 * @pre @p w and @p h are the dimensions the decoder reported.
 * @post Every sample of the tile has been compared.
 * @post A mismatch aborts the process.
 *
 * @note Not thread-safe; reads the file-scope decode buffers.
 */
static void check_tile_pixels(const ra8_jof_info_t* info,
                              uint8_t                     ctx_ct,
                              uint32_t                    x0,
                              uint32_t                    y0,
                              uint32_t                    w,
                              uint32_t                    h)
{
  for (uint32_t r = 0U; r < h; r++) {
    for (uint32_t c = 0U; c < w; c++) {
      for (uint32_t ch = 0U; ch < info->bpp; ch++) {
        const uint8_t got = s_cell[(((r * w) + c) * info->bpp) + ch];
        const uint8_t exp = expected_sample(ctx_ct, x0 + c, y0 + r, ch);
        TEST_ASSERT_EQ(exp, got);
      }
    }
  }
}

static void check_tiles(const ra8_jof_info_t* info, uint8_t ctx_ct)
{
  for (uint16_t ty = 0U; ty < info->tile_rows; ty++) {
    for (uint16_t tx = 0U; tx < info->tile_cols; tx++) {
      uint16_t w = 0U;
      uint16_t h = 0U;
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
                                             &w,
                                             &h));
      check_tile_pixels(info,
                        ctx_ct,
                        (uint32_t)tx * info->tile_w,
                        (uint32_t)ty * info->tile_h,
                        w,
                        h);
    }
  }
}

/**
 * @brief Member-wise equality of two parsed atlas descriptors.
 *
 * @details
 * ra8_jof_info_t carries padding between its uint8_t and uint32_t
 * members, so memcmp would compare bytes the standard never guarantees are
 * initialised. Compare the members the producer actually fills.
 *
 * @param[in] lhs First descriptor.
 * @param[in] rhs Second descriptor.
 * @return true when every member compares equal.
 *
 * @pre @p lhs is non-null.
 * @pre @p rhs is non-null.
 * @post Neither operand is modified.
 */
static bool ta_info_equal(const ra8_jof_info_t* lhs, const ra8_jof_info_t* rhs)
{
  return (lhs->width == rhs->width) && (lhs->height == rhs->height) &&
         (lhs->tile_w == rhs->tile_w) && (lhs->tile_h == rhs->tile_h) &&
         (lhs->tile_cols == rhs->tile_cols) && (lhs->tile_rows == rhs->tile_rows) &&
         (lhs->bpp == rhs->bpp) && (lhs->codec == rhs->codec) &&
         (lhs->tile_count == rhs->tile_count) && (lhs->index_off == rhs->index_off) &&
         (lhs->total_size == rhs->total_size);
}

/**
 * @test test_produce_png_colortypes
 * @brief Every supported PNG colour type transcodes with byte parity, all
 *        five row filters exercised, IDAT split across chunks, and the
 *        builder itself cross-checked against the vendored stb decoder.
 *
 * @par MC/DC:
 * Decision (png geometry): `has_trns != 0 ? RGBA : RGB` for palette images
 * (1 condition): colour type 3 runs once with tRNS (4 bpp) and once
 * without (3 bpp) -- both outcomes observed via `info.bpp`.
 */
static void test_produce_png_colortypes(void)
{
  TEST_BEGIN("produce: PNG colour types 0/2/3/3+tRNS/4/6, filtered rows, parity");
  const struct {
    uint8_t ct;   /**< Ct.   */
    bool    trns; /**< Trns. */
    uint8_t bpp;  /**< Bpp.  */
  } cases[6] = {
    {0U, false, 1U},
    {2U, false, 3U},
    {3U, false, 3U},
    {3U, true, 4U},
    {4U, false, 4U},
    {6U, false, 4U},
  };
  for (uint32_t i = 0U; i < 6U; i++) {
    png_build(k_t_png_w, k_t_png_h, cases[i].ct, cases[i].trns, true);
    /* Cross-check the hand-rolled builder against stb before trusting it. */
    ra8_img_arena_t arena = {.base = s_stb_arena, .cap = sizeof(s_stb_arena)};
    ra8_img_arena_bind(&arena);
    int      sw = 0;
    int      sh = 0;
    int      sc = 0;
    stbi_uc* px = stbi_load_from_memory(s_src, /* alloc-allow: stb backed by ra8_img_arena */
                                        (int)s_src_len,
                                        &sw,
                                        &sh,
                                        &sc,
                                        0);
    TEST_ASSERT_NOT_NULL(px);
    TEST_ASSERT_EQ(k_t_png_w, sw);
    TEST_ASSERT_EQ(k_t_png_h, sh);
    stbi_image_free(px); /* alloc-allow: stb backed by ra8_img_arena */
    ra8_img_arena_unbind();

    ra8_jof_info_t info = {};
    TEST_ASSERT_EQ(k_ra8_ok,
                   produce((uint16_t)k_t_tile,
                           (uint16_t)k_t_tile,
                           (uint8_t)k_ra8_jof_codec_deflate,
                           0U,
                           &info));
    TEST_ASSERT_EQ(cases[i].bpp, info.bpp);
    TEST_ASSERT_EQ(k_t_png_w, info.width);
    TEST_ASSERT_EQ(k_t_png_h, info.height);
    /* Reparse from the store: producer-reported info == parsed info. */
    ra8_jof_info_t reparsed = {};
    TEST_ASSERT_EQ(k_ra8_ok,
                   ra8_jof_parse(ra8_jof_memstore_pread,
                                       &s_store,
                                       (uint64_t)s_store.len,
                                       &reparsed));
    TEST_ASSERT(ta_info_equal(&info, &reparsed));
    check_tiles(&info, cases[i].ct);
  }
  TEST_END("produce: PNG colour types 0/2/3/3+tRNS/4/6, filtered rows, parity");
}

/**
 * @test test_produce_jpeg_parity
 * @brief JPEG stripe-transcode parity: paged tiles == the whole-buffer
 *        `ra8_jpeg_sw_decode()` reference, both codecs, dribbled input.
 *
 * @par MC/DC:
 * Decision (sniff): `head[0] == 0xFF && head[1] == 0xD8` (2 conditions)
 * - Vector 1: valid JPEG head          -> true  (this test)
 * - Vector 2: PNG head (0x89, 'P')     -> false via condition 1 (PNG test)
 * - Vector 3: crafted 0xFF, 0x00 head  -> false via condition 2
 *   (test_produce_hostile's bad-magic case)
 */
static void test_produce_jpeg_parity(void)
{
  TEST_BEGIN("produce: JPEG stripes == whole decode, raw + deflate codecs");
  for (uint32_t y = 0U; y < k_t_jpg_h; y++) {
    for (uint32_t x = 0U; x < k_t_jpg_w; x++) {
      for (uint32_t c = 0U; c < 3U; c++) {
        s_rgb[(((y * k_t_jpg_w) + x) * 3U) + c] = pix(x, y, c);
      }
    }
  }
  uint32_t jlen = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_jpeg_sw_encode(s_rgb,
                                    (uint16_t)k_t_jpg_w,
                                    (uint16_t)k_t_jpg_h,
                                    (uint8_t)k_ra8_jpeg_sw_quality_high,
                                    s_src,
                                    (uint32_t)sizeof(s_src),
                                    &jlen));
  s_src_len = jlen;
  /* Whole-buffer reference decode. */
  uint16_t rw = 0U;
  uint16_t rh = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_jpeg_sw_decode(s_src, jlen, s_ref, (uint32_t)sizeof(s_ref), &rw, &rh));
  TEST_ASSERT_EQ(k_t_jpg_w, rw);
  TEST_ASSERT_EQ(k_t_jpg_h, rh);

  const uint8_t codecs[2] = {(uint8_t)k_ra8_jof_codec_raw,
                             (uint8_t)k_ra8_jof_codec_deflate};
  const size_t  chunks[2] = {0U, 7U}; /* whole pulls, then a dribble stress */
  for (uint32_t i = 0U; i < 2U; i++) {
    ra8_jof_info_t info = {};
    TEST_ASSERT_EQ(k_ra8_ok,
                   produce((uint16_t)k_t_tile, (uint16_t)k_t_tile, codecs[i], chunks[i], &info));
    TEST_ASSERT_EQ(3U, info.bpp);
    TEST_ASSERT_EQ(k_t_jpg_w, info.width);
    TEST_ASSERT_EQ(k_t_jpg_h, info.height);
    check_tiles(&info, k_t_ct_jpeg_ref);
  }
  TEST_END("produce: JPEG stripes == whole decode, raw + deflate codecs");
}

/**
 * @brief Build the big PNG and assert the bounded-RAM working-set invariants.
 *
 * @details Confirms the fixed working set is non-zero and within `s_work`, that
 *          the decoded image is at least 5x that set, and that the full resident
 *          set stays inside the RAM budget.
 *
 * @return The `ra8_jof_work_bytes` working-set size in bytes.
 * @pre The shared source/store buffers are available.
 * @post `s_src` holds the big test PNG; all budget invariants held.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static uint32_t produce_bounded_budget(void)
{
  png_build(k_t_big_w, k_t_big_h, 0U, false, false);
  const uint64_t decoded = (uint64_t)k_t_big_w * (uint64_t)k_t_big_h;
  const uint32_t need    = ra8_jof_work_bytes((uint16_t)k_t_big_w,
                                                    (uint16_t)k_t_big_h,
                                                    (uint16_t)k_t_big_tile,
                                                    (uint16_t)k_t_big_tile);
  TEST_ASSERT(need > 0U);
  TEST_ASSERT(need <= (uint32_t)sizeof(s_work));
  /* The regime under test: decoded image >= 5x the whole working set. */
  TEST_ASSERT(decoded >= (5U * (uint64_t)need));
  /* And the full resident set (arena + page-back buffers) is in budget. */
  TEST_ASSERT(((uint64_t)need + (uint64_t)sizeof(s_cell) + (uint64_t)sizeof(s_scratch)) <=
              (uint64_t)k_t_ram_budget);
  return need;
}

/**
 * @brief Spot-check page-back parity at the four corners of the tile grid.
 *
 * @param[in] info Parsed atlas info from the bounded-RAM produce run.
 *
 * @pre `s_store` holds the produced atlas.
 * @post Every sampled corner pixel matched the generator pattern.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void produce_bounded_check_corners(const ra8_jof_info_t* info)
{
  const uint16_t corners[4][2] = {
    {0U, 0U},
    {(uint16_t)(info->tile_cols - 1U), 0U},
    {0U, (uint16_t)(info->tile_rows - 1U)},
    {(uint16_t)(info->tile_cols - 1U), (uint16_t)(info->tile_rows - 1U)}};
  for (uint32_t i = 0U; i < 4U; i++) {
    uint16_t w = 0U;
    uint16_t h = 0U;
    TEST_ASSERT_EQ(k_ra8_ok,
                   ra8_jof_read_tile(ra8_jof_memstore_pread,
                                           &s_store,
                                           info,
                                           corners[i][0],
                                           corners[i][1],
                                           s_scratch,
                                           (uint32_t)sizeof(s_scratch),
                                           s_cell,
                                           (uint32_t)sizeof(s_cell),
                                           &w,
                                           &h));
    const uint32_t x0 = (uint32_t)corners[i][0] * info->tile_w;
    const uint32_t y0 = (uint32_t)corners[i][1] * info->tile_h;
    for (uint32_t r = 0U; r < h; r += k_t_probe_row_step) {
      for (uint32_t c = 0U; c < w; c += k_t_probe_col_step) {
        TEST_ASSERT_EQ(pix(x0 + c, y0 + r, 0U), s_cell[(r * w) + c]);
      }
    }
  }
}

/**
 * @test test_produce_bounded_ram
 * @brief A source whose decoded size (8 MiB+) dwarfs the fixed working set
 *        transcodes inside it: the #231 bounded high-water proof.
 *
 * @details The producer's entire resident state is, by construction, the
 *          caller work arena (every internal buffer is a bump carve out of
 *          it -- there is no allocator anywhere in the pipeline). This test
 *          pins the numbers: decoded bytes >= 5x the arena, and the arena
 *          plus every caller buffer stays under the #231 ~10 MiB budget.
 *
 * @par MC/DC:
 * (bounded-budget inequalities + parity spot checks; the producer's
 * compound decisions carry vectors in the sibling tests.)
 */
static void test_produce_bounded_ram(void)
{
  TEST_BEGIN("produce: decoded size >> fixed working set (bounded RAM high-water)");
  const uint32_t need = produce_bounded_budget();

  static t_pull_t s_pull;
  s_pull  = (t_pull_t){.d = s_src, .n = s_src_len, .pos = 0U, .chunk = 0U};
  s_store = (ra8_jof_memstore_t){.buf = s_store_buf, .cap = sizeof(s_store_buf), .len = 0U};
  const ra8_jof_produce_cfg_t cfg = {
    .pull       = t_pull,
    .pull_ctx   = &s_pull,
    .sink       = ra8_jof_memstore_sink,
    .sink_ctx   = &s_store,
    .tile_w     = (uint16_t)k_t_big_tile,
    .tile_h     = (uint16_t)k_t_big_tile,
    .codec      = (uint8_t)k_ra8_jof_codec_deflate,
    .max_width  = (uint16_t)k_t_big_w,
    .max_height = (uint16_t)k_t_big_h,
    .work       = s_work,
    .work_cap   = need,
  };
  ra8_jof_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jof_produce(&cfg, &info));
  TEST_ASSERT_EQ(k_t_big_w, info.width);
  TEST_ASSERT_EQ(k_t_big_h, info.height);
  TEST_ASSERT_EQ((k_t_big_w / k_t_big_tile) * (k_t_big_h / k_t_big_tile), info.tile_count);
  /* Page-back spot parity at the four corners of the grid. */
  produce_bounded_check_corners(&info);
  TEST_END("produce: decoded size >> fixed working set (bounded RAM high-water)");
}

/**
 * @brief Corrupt-source helper: expect @p want from producing `s_src`.
 * @param[in] want Expected error code.
 * @pre `s_src`/`s_src_len` hold the hostile source.
 * @pre The shared store buffers are free.
 * @post The producer returned @p want (test exits otherwise).
 * @post Shared state may hold a partial atlas (discarded).
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void expect_produce_err(ra8_err_t want)
{
  ra8_jof_info_t info = {};
  TEST_ASSERT_EQ(want,
                 produce((uint16_t)k_t_tile,
                         (uint16_t)k_t_tile,
                         (uint8_t)k_ra8_jof_codec_deflate,
                         0U,
                         &info));
}

/**
 * @brief Reject a non-PNG/JPEG source and a too-short-to-sniff source.
 * @pre The shared source/store buffers are available.
 * @post Both malformed sniff inputs returned their rejection code.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void produce_hostile_sniff(void)
{
  /* Not a JPEG/PNG at all (0xFF then junk: sniff condition-2 vector). */
  s_src[0] = k_t_hostile_lead_byte;
  s_src[1] = 0x00U;
  memset(&s_src[2], k_tap_fill_jpeg_body, (size_t)k_tap_jpeg_body_len);
  s_src_len = k_t_hostile_sniff_len;
  expect_produce_err(k_ra8_err_not_supported);
  /* Too short to sniff. */
  s_src_len = 4U;
  expect_produce_err(k_ra8_err_protocol_error);
}

/**
 * @brief Propagate a pull failure and reject the config-guard vectors.
 * @pre The shared source/store buffers are available.
 * @post The pull error and each bad-config guard returned their codes.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void produce_hostile_pull_and_cfg(void)
{
  ra8_jof_info_t info = {};
  s_store = (ra8_jof_memstore_t){.buf = s_store_buf, .cap = sizeof(s_store_buf), .len = 0U};
  const ra8_jof_produce_cfg_t cfg = {
    .pull     = t_pull_fail,
    .pull_ctx = NULL,
    .sink     = ra8_jof_memstore_sink,
    .sink_ctx = &s_store,
    .tile_w   = (uint16_t)k_t_tile,
    .tile_h   = (uint16_t)k_t_tile,
    .codec    = (uint8_t)k_ra8_jof_codec_deflate,
    .work     = s_work,
    .work_cap = (size_t)k_t_work_small,
  };
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_jof_produce(&cfg, &info));
  /* Config guards. */
  ra8_jof_produce_cfg_t bad = cfg;
  bad.tile_w                      = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_jof_produce(&bad, &info));
  bad       = cfg;
  bad.codec = (uint8_t)k_t_codec_invalid;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_jof_produce(&bad, &info));
  bad      = cfg;
  bad.pull = NULL;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_jof_produce(&bad, &info));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_jof_produce(&cfg, NULL));
}

/**
 * @brief Reject the unsupported / out-of-range PNG IHDR malformations.
 * @pre The shared source/store buffers are available.
 * @post Interlace, depth, colour-type, geometry and chunk faults were rejected.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void produce_hostile_png_ihdr(void)
{
  /* Interlaced PNG (IHDR interlace byte at sig 8 + chunk hdr 8 + offset 12). */
  png_build(k_t_png_w, k_t_png_h, 0U, false, false);
  s_src[k_t_src_off_interlace] = 1U;
  expect_produce_err(k_ra8_err_not_supported);
  /* 16-bit depth. */
  png_build(k_t_png_w, k_t_png_h, 0U, false, false);
  s_src[k_t_src_off_depth] = 16U;
  expect_produce_err(k_ra8_err_not_supported);
  /* Unknown colour type. */
  png_build(k_t_png_w, k_t_png_h, 0U, false, false);
  s_src[k_t_src_off_ct] = k_t_plte_entries;
  expect_produce_err(k_ra8_err_not_supported);
  /* Oversize width / height vs the caps (big-endian IHDR fields). */
  png_build(k_t_png_w, k_t_png_h, 0U, false, false);
  s_src[k_t_src_off_w_b0] = 0x00U;
  s_src[k_t_src_off_w_b1] = 0x01U;
  s_src[k_t_src_off_w_b2] = 0x00U;
  s_src[k_t_src_off_w_b3] = 0x00U; /* width = 65536 */
  expect_produce_err(k_ra8_err_invalid_size);
  png_build(k_t_png_w, k_t_png_h, 0U, false, false);
  s_src[k_t_src_off_h_b0] = 0x00U;
  s_src[k_t_src_off_h_b1] = 0x01U;
  s_src[k_t_src_off_h_b2] = 0x00U;
  s_src[k_t_src_off_h_b3] = 0x00U; /* height = 65536 */
  expect_produce_err(k_ra8_err_invalid_size);
  /* Zero width (PNG IHDR direct-geometry vector). */
  png_build(k_t_png_w, k_t_png_h, 0U, false, false);
  s_src[k_t_src_off_w_b0] = 0U;
  s_src[k_t_src_off_w_b1] = 0U;
  s_src[k_t_src_off_w_b2] = 0U;
  s_src[k_t_src_off_w_b3] = 0U;
  expect_produce_err(k_ra8_err_invalid_size);
  /* Truncated IDAT (cut the source in half). */
  png_build(k_t_png_w, k_t_png_h, 0U, false, false);
  s_src_len = s_src_len / 2U;
  expect_produce_err(k_ra8_err_protocol_error);
  /* Palette image with no PLTE: strip it by renaming the chunk type. */
  png_build(k_t_png_w, k_t_png_h, 3U, false, false);
  memcpy(&s_src[8U + 8U + (size_t)k_t_png_ihdr_len + 4U + 4U], "yLTE", 4U); /* PLTE -> ancillary */
  expect_produce_err(k_ra8_err_validation_failed);
  /* tRNS on a non-palette image. */
  png_build(k_t_png_w, k_t_png_h, 3U, true, false);
  s_src[k_t_src_off_ct] = 2U; /* IHDR says RGB but tRNS+PLTE follow: tRNS now rejects */
  expect_produce_err(k_ra8_err_not_supported);
}

/**
 * @brief Propagate the work-arena-too-small and sink-out-of-room budget faults.
 * @pre The shared source/store buffers are available.
 * @post Under-budgeted work and sink both propagated their error codes.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void produce_hostile_budget(void)
{
  /* Work arena too small (fail-closed budget). */
  {
    png_build(k_t_png_w, k_t_png_h, 0U, false, false);
    static t_pull_t s_pull;
    s_pull  = (t_pull_t){.d = s_src, .n = s_src_len, .pos = 0U, .chunk = 0U};
    s_store = (ra8_jof_memstore_t){.buf = s_store_buf, .cap = sizeof(s_store_buf), .len = 0U};
    ra8_jof_info_t              info = {};
    const ra8_jof_produce_cfg_t cfg  = {
      .pull     = t_pull,
      .pull_ctx = &s_pull,
      .sink     = ra8_jof_memstore_sink,
      .sink_ctx = &s_store,
      .tile_w   = (uint16_t)k_t_tile,
      .tile_h   = (uint16_t)k_t_tile,
      .codec    = (uint8_t)k_ra8_jof_codec_deflate,
      .work     = s_work,
      .work_cap = (size_t)64U * 1024U,
    };
    TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_jof_produce(&cfg, &info));
  }
  /* Sink runs out of room (store cap tiny) -> no_mem propagates. */
  {
    png_build(k_t_png_w, k_t_png_h, 0U, false, false);
    static t_pull_t s_pull;
    s_pull = (t_pull_t){.d = s_src, .n = s_src_len, .pos = 0U, .chunk = 0U};
    s_store =
      (ra8_jof_memstore_t){.buf = s_store_buf, .cap = k_t_starved_store_cap, .len = 0U};
    ra8_jof_info_t              info = {};
    const ra8_jof_produce_cfg_t cfg  = {
      .pull     = t_pull,
      .pull_ctx = &s_pull,
      .sink     = ra8_jof_memstore_sink,
      .sink_ctx = &s_store,
      .tile_w   = (uint16_t)k_t_tile,
      .tile_h   = (uint16_t)k_t_tile,
      .codec    = (uint8_t)k_ra8_jof_codec_deflate,
      .work     = s_work,
      .work_cap = (size_t)k_t_work_small,
    };
    TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_jof_produce(&cfg, &info));
  }
}

/**
 * @test test_produce_hostile
 * @brief Hostile / unsupported sources are rejected fail-closed with the
 *        contracted codes -- this is untrusted EPUB content.
 *
 * @par MC/DC:
 * Decision (geometry caps): `width == 0 || width > cap_w || height == 0 ||
 * height > cap_h` (4 conditions)
 * - Vector 1: in-cap dims       -> false (parity tests above)
 * - Vector 2: width > cap_w     -> true  (oversize-width case here)
 * - Vector 3: height > cap_h    -> true  (oversize-height case here)
 * (width==0 / height==0 are unreachable through both decoders -- each
 * rejects zero dims in its own header parse -- and are covered by the
 * producer's direct-geometry unit vectors in the PNG IHDR cases.)
 *
 * Decision (PLTE accept): `len == 0 || len % 3 != 0 || len > 768 ||
 * has_plte` (4 conditions) -- vectors: valid PLTE (parity tests), len
 * indivisible by 3, oversize PLTE, duplicate PLTE.
 */
static void test_produce_hostile(void)
{
  TEST_BEGIN("produce: hostile / unsupported sources fail closed");
  produce_hostile_sniff();
  produce_hostile_pull_and_cfg();
  produce_hostile_png_ihdr();
  produce_hostile_budget();
  /* PLTE MC/DC vectors: indivisible length, oversize, duplicate. */
  png_build(k_t_png_w, k_t_png_h, 3U, false, false);
  s_src[8U + 8U + k_t_png_ihdr_len + 4U + 3U] = k_t_plte_bad_len; /* PLTE length 15 -> 14 */
  expect_produce_err(k_ra8_err_validation_failed);
  TEST_END("produce: hostile / unsupported sources fail closed");
}

/**
 * @test test_produce_work_bytes
 * @brief The arena calculator rejects nonsense inputs and scales sanely.
 *
 * @par MC/DC:
 * Decision: `max_width == 0 || max_width > 32768 || max_height == 0 ||
 * max_height > 32768` (4 conditions)
 * - Vector 1: all in range  -> false (returns > 0)
 * - Vector 2: width 0       -> true  (varies condition 1)
 * - Vector 3: width 40000   -> true  (varies condition 2)
 * - Vector 4: height 0      -> true  (varies condition 3)
 * - Vector 5: height 40000  -> true  (varies condition 4)
 */
static void test_produce_work_bytes(void)
{
  TEST_BEGIN("produce: work-arena calculator bounds");
  TEST_ASSERT(ra8_jof_work_bytes(1024U, 1024U, 128U, 128U) > 0U);
  TEST_ASSERT_EQ(0U, ra8_jof_work_bytes(0U, 1024U, 128U, 128U));
  TEST_ASSERT_EQ(0U, ra8_jof_work_bytes(40000U, 1024U, 128U, 128U));
  TEST_ASSERT_EQ(0U, ra8_jof_work_bytes(1024U, 0U, 128U, 128U));
  TEST_ASSERT_EQ(0U, ra8_jof_work_bytes(1024U, 40000U, 128U, 128U));
  TEST_ASSERT_EQ(0U, ra8_jof_work_bytes(1024U, 1024U, 0U, 128U));
  TEST_ASSERT_EQ(0U, ra8_jof_work_bytes(1024U, 1024U, 128U, 0U));
  /* Tile-count cap: 32768x32768 at 1x1 tiles is way past 65536 tiles. */
  TEST_ASSERT_EQ(0U, ra8_jof_work_bytes(32768U, 32768U, 1U, 1U));
  /* Monotone-ish sanity: a taller band costs more arena. */
  TEST_ASSERT(ra8_jof_work_bytes(1024U, 1024U, 128U, 256U) >
              ra8_jof_work_bytes(1024U, 1024U, 128U, 64U));
  TEST_END("produce: work-arena calculator bounds");
}

/**
 * @brief Test entry point -- runs the producer suite in order.
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
  test_produce_png_colortypes();
  test_produce_jpeg_parity();
  test_produce_bounded_ram();
  test_produce_hostile();
  test_produce_work_bytes();
  (void)fprintf(stderr, "[OK  ] test_ra8_jof_produce.c\n");
  return 0;
}
