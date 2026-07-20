/**
 * @file test_ra8_tileatlas_produce_reject.c
 * @brief Producer entry-point rejection vectors: format sniff, pull failure,
 *        config guards, PNG IHDR malformations and starved budgets (#231).
 *
 * @details
 * Complements `test_ra8_tileatlas_produce.c` (happy paths, byte parity and the
 * bounded-RAM high-water proof), `test_ra8_tileatlas_produce_guards.c` (the
 * producer's *internal* calculator and carve guards) and
 * `test_ra8_tileatlas_png_hostile.c` (hostile PNG *streams* through the decoder):
 * this suite drives `ra8_tileatlas_produce()` itself with untrusted input and
 * asserts every rejection is fail-closed with the contracted error code. EPUB
 * and CBZ content is attacker-controlled, so a producer that limps on past a
 * malformed header is a defect, not a robustness feature.
 *
 * Sources are synthesized by a local PNG builder that is deliberately smaller
 * than the parity suite's: these arms build a *valid* stream only to corrupt
 * one field of it, so the builder needs spec-correct framing (signature, CRCs,
 * a real zlib IDAT pair) but no pattern fidelity -- nothing here compares
 * decoded pixels against an oracle. Following the sibling suites, the fixture
 * is self-contained: this translation unit shares no state with them.
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
#include "ra8_tileatlas.h"
#include "ra8_tileatlas_produce.h"
#include "unity_minimal.h"

/** @brief Truncated-JPEG probe fed to the producer. */
typedef enum : uint16_t {
  k_tap_fill_jpeg_body = 0xAAU, /**< Filler body after the SOI marker. */
  k_tap_jpeg_body_len  = 32U,   /**< Filler bytes written.             */
} tap_probe_t;

/**
 * @enum t_png_off_t
 * @brief PNG byte positions the builder writes and the reject arms corrupt.
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
  k_t_ihdr_off_depth    = 8U,    /**< Bit-depth byte in the IHDR payload.              */
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
 * @enum t_palette_t
 * @brief Palette the builder emits for colour-type-3 fixtures.
 *
 * @details
 * The reject arms never compare decoded pixels, so the ramp only has to be a
 * spec-legal PLTE: `k_t_plte_entries` entries of three bytes, which is what the
 * indivisible-length and missing-PLTE vectors corrupt.
 */
typedef enum : uint8_t {
  k_t_plte_entries = 5U,  /**< Palette entries the builder emits.            */
  k_t_plte_bytes   = 15U, /**< PLTE payload: k_t_plte_entries x 3 RGB bytes. */
  k_t_plte_r_base  = 10U, /**< Red of palette entry 0.                       */
  k_t_plte_r_step  = 40U, /**< Red increment per palette entry.              */
  k_t_plte_g_base  = 20U, /**< Green of palette entry 0.                     */
  k_t_plte_g_step  = 30U, /**< Green increment per palette entry.            */
  k_t_plte_b_base  = 30U, /**< Blue of palette entry 0.                      */
  k_t_plte_b_step  = 20U, /**< Blue increment per palette entry.             */
} t_palette_t;

/** @brief Stimulus values that steer the rejection and budget-starved paths. */
typedef enum : uint16_t {
  k_t_starved_store_cap = 64U,   /**< Memstore cap too small for the atlas, forcing
                                      the sink's no-memory path.                */
  k_t_codec_invalid     = 9U,    /**< Codec id outside the enum; the config guard must reject it. */
  k_t_hostile_sniff_len = 34U,   /**< Length of the not-a-PNG/JPEG blob fed to the sniffer.   */
  k_t_hostile_lead_byte = 0xFFU, /**< Its leading byte: starts like a JPEG marker, then junk. */
  k_t_plte_bad_len      = 14U,   /**< PLTE length not divisible by 3, tripping the
                                      indivisible-length guard.                 */
  k_t_kib               = 1024U, /**< Bytes per KiB, for sizing the producer work arena. */
} t_probe_t;

/** @brief Test geometry + buffer sizing. */
enum : uint32_t {
  k_t_png_w        = 90U,           /**< PNG test image width.                        */
  k_t_png_h        = 70U,           /**< PNG test image height.                       */
  k_t_tile         = 32U,           /**< Tile edge for the reject runs.               */
  k_t_cap_w        = 2048U,         /**< Producer width cap; oversize IHDR must trip. */
  k_t_cap_h        = 8192U,         /**< Producer height cap.                         */
  k_t_src_cap      = 256U * 1024U,  /**< Synthesized source capacity.                 */
  k_t_store_cap    = 256U * 1024U,  /**< Memstore capacity.                           */
  k_t_raw_cap      = 64U * 1024U,   /**< Raw scanline staging capacity.               */
  k_t_work_small   = 900U * 1024U,  /**< Work arena the passing config requests.      */
  k_t_work_starved = 64U * 1024U,   /**< Work arena too small: fail-closed budget.    */
  k_t_work_cap     = 1024U * 1024U, /**< Work arena backing store.                    */
};

/** @brief Synthesized encoded source. */
static uint8_t s_src[k_t_src_cap];
/** @brief Synthesized source length. */
static size_t s_src_len;
/** @brief Raw (filtered) scanline staging for the PNG builder. */
static uint8_t s_raw[k_t_raw_cap];
/** @brief zlib staging for the in-test PNG builder. */
static uint8_t s_zbuf[k_t_src_cap];
/** @brief Producer work arena. */
static uint8_t s_work[k_t_work_cap];
/** @brief Memstore backing. */
static uint8_t s_store_buf[k_t_store_cap];
/** @brief The atlas store under test. */
static ra8_tileatlas_memstore_t s_store;

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

/** @brief ::ra8_tileatlas_pull_fn over a ::t_pull_t. */
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

/* ---------------------------------------------------------------------------
 * In-test PNG builder. Spec-correct framing, deliberately no filter support:
 * every arm below builds with filter type 0 and then corrupts a header field.
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

/**
 * @brief Fill `s_raw` with unfiltered scanlines for a @p ch -channel image.
 *
 * @details
 * Each row is a zero filter byte followed by @p w * @p ch samples. Palette rows
 * carry indices inside the emitted PLTE so the decoder has a legal image to
 * accept before an arm corrupts it; truecolour rows carry a cheap ramp.
 *
 * @param[in] w       Image width in pixels.
 * @param[in] h       Image height in pixels.
 * @param[in] ch      Channels per pixel.
 * @param[in] palette True to emit palette indices, false for a sample ramp.
 *
 * @return Bytes written into `s_raw`.
 *
 * @pre `s_raw` is at least `h * ((w * ch) + 1)` bytes.
 * @pre @p ch is non-zero.
 * @post Every emitted row starts with filter type 0.
 * @post Palette samples are below ::k_t_plte_entries.
 *
 * @note Not thread-safe; writes the file-scope staging buffer.
 * @since 0.1.0
 */
static size_t png_fill_raw(uint32_t w, uint32_t h, uint32_t ch, bool palette)
{
  size_t o = 0U;
  for (uint32_t y = 0U; y < h; y++) {
    s_raw[o] = 0U; /* filter type 0 (None) */
    o++;
    for (uint32_t x = 0U; x < w; x++) {
      for (uint32_t c = 0U; c < ch; c++) {
        s_raw[o] = palette ? (uint8_t)((x + y) % k_t_plte_entries) : (uint8_t)((x ^ y) + c);
        o++;
      }
    }
  }
  return o;
}

/**
 * @brief Build a spec-valid PNG of @p color_type into `s_src`.
 *
 * @details
 * Emits signature, IHDR, an optional PLTE (+ tRNS) for palette images, the
 * zlib stream split across two IDAT chunks (chunk-crossing coverage) and IEND.
 * The arms below call this to get a stream the producer *would* accept, then
 * corrupt exactly one field to prove the corresponding guard fires.
 *
 * @param[in] w          Image width in pixels.
 * @param[in] h          Image height in pixels.
 * @param[in] color_type PNG colour type (0 greyscale, 3 palette used here).
 * @param[in] with_trns  True to append a tRNS chunk after the PLTE.
 *
 * @pre `s_src` is large enough for the compressed image plus chunk overhead.
 * @pre @p color_type is 0 or 3 (the types these arms build).
 * @post `s_src` holds a complete PNG and `s_src_len` its length.
 * @post The stream carries two IDAT chunks.
 *
 * @note Not thread-safe; writes the file-scope source buffer.
 * @since 0.1.0
 */
static void png_build(uint32_t w, uint32_t h, uint8_t color_type, bool with_trns)
{
  static const uint8_t sig[8]  = {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU};
  const bool           palette = (color_type == 3U);
  const uint32_t       ch      = 1U; /* both built types are one byte per pixel */
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
  ihdr[k_t_ihdr_off_depth]       = 8U;
  ihdr[k_t_ihdr_off_ct]          = color_type;
  png_chunk("IHDR", ihdr, sizeof(ihdr));
  if (palette) {
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
  const size_t rawn = png_fill_raw(w, h, ch, palette);
  mz_ulong     zlen = (mz_ulong)sizeof(s_zbuf);
  TEST_ASSERT_EQ(MZ_OK, mz_compress(s_zbuf, &zlen, s_raw, (mz_ulong)rawn));
  /* Split the zlib stream across two IDAT chunks (chunk-crossing coverage). */
  const uint32_t half = (uint32_t)zlen / 2U;
  png_chunk("IDAT", s_zbuf, half);
  png_chunk("IDAT", &s_zbuf[half], (uint32_t)zlen - half);
  png_chunk("IEND", NULL, 0U);
}

/** @brief Run the producer over `s_src` into a fresh memstore. */
static ra8_err_t
produce(uint16_t tile_w, uint16_t tile_h, uint8_t codec, size_t chunk, ra8_tileatlas_info_t* info)
{
  static t_pull_t s_pull;
  s_pull  = (t_pull_t){.d = s_src, .n = s_src_len, .pos = 0U, .chunk = chunk};
  s_store = (ra8_tileatlas_memstore_t){.buf = s_store_buf, .cap = sizeof(s_store_buf), .len = 0U};
  const ra8_tileatlas_produce_cfg_t cfg = {
    .pull       = t_pull,
    .pull_ctx   = &s_pull,
    .sink       = ra8_tileatlas_memstore_sink,
    .sink_ctx   = &s_store,
    .tile_w     = tile_w,
    .tile_h     = tile_h,
    .codec      = codec,
    .max_width  = (uint16_t)k_t_cap_w,
    .max_height = (uint16_t)k_t_cap_h,
    .work       = s_work,
    .work_cap   = (size_t)k_t_work_small,
  };
  return ra8_tileatlas_produce(&cfg, info);
}

/**
 * @brief Assert the producer rejects the current `s_src` with @p want.
 *
 * @param[in] want The contracted rejection code for this source.
 *
 * @pre `s_src` / `s_src_len` hold the crafted source.
 * @pre The shared store buffers are free.
 * @post The producer returned @p want (test exits otherwise).
 * @post Shared state may hold a partial atlas (discarded).
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void expect_produce_err(ra8_err_t want)
{
  ra8_tileatlas_info_t info = {};
  TEST_ASSERT_EQ(want,
                 produce((uint16_t)k_t_tile,
                         (uint16_t)k_t_tile,
                         (uint8_t)k_ra8_tileatlas_codec_deflate,
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
static void produce_reject_sniff(void)
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
static void produce_reject_pull_and_cfg(void)
{
  ra8_tileatlas_info_t info = {};
  s_store = (ra8_tileatlas_memstore_t){.buf = s_store_buf, .cap = sizeof(s_store_buf), .len = 0U};
  const ra8_tileatlas_produce_cfg_t cfg = {
    .pull     = t_pull_fail,
    .pull_ctx = NULL,
    .sink     = ra8_tileatlas_memstore_sink,
    .sink_ctx = &s_store,
    .tile_w   = (uint16_t)k_t_tile,
    .tile_h   = (uint16_t)k_t_tile,
    .codec    = (uint8_t)k_ra8_tileatlas_codec_deflate,
    .work     = s_work,
    .work_cap = (size_t)k_t_work_small,
  };
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_tileatlas_produce(&cfg, &info));
  /* Config guards. */
  ra8_tileatlas_produce_cfg_t bad = cfg;
  bad.tile_w                      = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tileatlas_produce(&bad, &info));
  bad       = cfg;
  bad.codec = (uint8_t)k_t_codec_invalid;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tileatlas_produce(&bad, &info));
  bad      = cfg;
  bad.pull = NULL;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tileatlas_produce(&bad, &info));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tileatlas_produce(&cfg, NULL));
}

/**
 * @brief Reject the unsupported / out-of-range PNG IHDR malformations.
 * @pre The shared source/store buffers are available.
 * @post Interlace, depth, colour-type, geometry and chunk faults were rejected.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void produce_reject_png_ihdr(void)
{
  /* Interlaced PNG (IHDR interlace byte at sig 8 + chunk hdr 8 + offset 12). */
  png_build(k_t_png_w, k_t_png_h, 0U, false);
  s_src[k_t_src_off_interlace] = 1U;
  expect_produce_err(k_ra8_err_not_supported);
  /* 16-bit depth. */
  png_build(k_t_png_w, k_t_png_h, 0U, false);
  s_src[k_t_src_off_depth] = 16U;
  expect_produce_err(k_ra8_err_not_supported);
  /* Unknown colour type. */
  png_build(k_t_png_w, k_t_png_h, 0U, false);
  s_src[k_t_src_off_ct] = k_t_plte_entries;
  expect_produce_err(k_ra8_err_not_supported);
  /* Oversize width / height vs the caps (big-endian IHDR fields). */
  png_build(k_t_png_w, k_t_png_h, 0U, false);
  s_src[k_t_src_off_w_b0] = 0x00U;
  s_src[k_t_src_off_w_b1] = 0x01U;
  s_src[k_t_src_off_w_b2] = 0x00U;
  s_src[k_t_src_off_w_b3] = 0x00U; /* width = 65536 */
  expect_produce_err(k_ra8_err_invalid_size);
  png_build(k_t_png_w, k_t_png_h, 0U, false);
  s_src[k_t_src_off_h_b0] = 0x00U;
  s_src[k_t_src_off_h_b1] = 0x01U;
  s_src[k_t_src_off_h_b2] = 0x00U;
  s_src[k_t_src_off_h_b3] = 0x00U; /* height = 65536 */
  expect_produce_err(k_ra8_err_invalid_size);
  /* Zero width (PNG IHDR direct-geometry vector). */
  png_build(k_t_png_w, k_t_png_h, 0U, false);
  s_src[k_t_src_off_w_b0] = 0U;
  s_src[k_t_src_off_w_b1] = 0U;
  s_src[k_t_src_off_w_b2] = 0U;
  s_src[k_t_src_off_w_b3] = 0U;
  expect_produce_err(k_ra8_err_invalid_size);
  /* Truncated IDAT (cut the source in half). */
  png_build(k_t_png_w, k_t_png_h, 0U, false);
  s_src_len = s_src_len / 2U;
  expect_produce_err(k_ra8_err_protocol_error);
  /* Palette image with no PLTE: strip it by renaming the chunk type. */
  png_build(k_t_png_w, k_t_png_h, 3U, false);
  memcpy(&s_src[8U + 8U + (size_t)k_t_png_ihdr_len + 4U + 4U], "yLTE", 4U); /* PLTE -> ancillary */
  expect_produce_err(k_ra8_err_validation_failed);
  /* tRNS on a non-palette image. */
  png_build(k_t_png_w, k_t_png_h, 3U, true);
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
static void produce_reject_budget(void)
{
  /* Work arena too small (fail-closed budget). */
  {
    png_build(k_t_png_w, k_t_png_h, 0U, false);
    static t_pull_t s_pull;
    s_pull  = (t_pull_t){.d = s_src, .n = s_src_len, .pos = 0U, .chunk = 0U};
    s_store = (ra8_tileatlas_memstore_t){.buf = s_store_buf, .cap = sizeof(s_store_buf), .len = 0U};
    ra8_tileatlas_info_t              info = {};
    const ra8_tileatlas_produce_cfg_t cfg  = {
      .pull     = t_pull,
      .pull_ctx = &s_pull,
      .sink     = ra8_tileatlas_memstore_sink,
      .sink_ctx = &s_store,
      .tile_w   = (uint16_t)k_t_tile,
      .tile_h   = (uint16_t)k_t_tile,
      .codec    = (uint8_t)k_ra8_tileatlas_codec_deflate,
      .work     = s_work,
      .work_cap = (size_t)k_t_work_starved,
    };
    TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_tileatlas_produce(&cfg, &info));
  }
  /* Sink runs out of room (store cap tiny) -> no_mem propagates. */
  {
    png_build(k_t_png_w, k_t_png_h, 0U, false);
    static t_pull_t s_pull;
    s_pull = (t_pull_t){.d = s_src, .n = s_src_len, .pos = 0U, .chunk = 0U};
    s_store =
      (ra8_tileatlas_memstore_t){.buf = s_store_buf, .cap = k_t_starved_store_cap, .len = 0U};
    ra8_tileatlas_info_t              info = {};
    const ra8_tileatlas_produce_cfg_t cfg  = {
      .pull     = t_pull,
      .pull_ctx = &s_pull,
      .sink     = ra8_tileatlas_memstore_sink,
      .sink_ctx = &s_store,
      .tile_w   = (uint16_t)k_t_tile,
      .tile_h   = (uint16_t)k_t_tile,
      .codec    = (uint8_t)k_ra8_tileatlas_codec_deflate,
      .work     = s_work,
      .work_cap = (size_t)k_t_work_small,
    };
    TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_tileatlas_produce(&cfg, &info));
  }
}

/**
 * @test test_produce_reject
 * @brief Hostile / unsupported sources are rejected fail-closed with the
 *        contracted codes -- this is untrusted EPUB content.
 *
 * @par MC/DC:
 * Decision (geometry caps): `width == 0 || width > cap_w || height == 0 ||
 * height > cap_h` (4 conditions)
 * - Vector 1: in-cap dims       -> false (parity tests in the sibling suite)
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
static void test_produce_reject(void)
{
  TEST_BEGIN("produce: hostile / unsupported sources fail closed");
  produce_reject_sniff();
  produce_reject_pull_and_cfg();
  produce_reject_png_ihdr();
  produce_reject_budget();
  /* PLTE MC/DC vectors: indivisible length, oversize, duplicate. */
  png_build(k_t_png_w, k_t_png_h, 3U, false);
  s_src[8U + 8U + k_t_png_ihdr_len + 4U + 3U] = k_t_plte_bad_len; /* PLTE length 15 -> 14 */
  expect_produce_err(k_ra8_err_validation_failed);
  TEST_END("produce: hostile / unsupported sources fail closed");
}

/**
 * @brief Test entry point -- runs the producer rejection suite.
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
  test_produce_reject();
  (void)fprintf(stderr, "[OK  ] test_ra8_tileatlas_produce_reject.c\n");
  return 0;
}
