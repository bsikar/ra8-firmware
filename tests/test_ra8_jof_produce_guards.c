/**
 * @file test_ra8_jof_produce_guards.c
 * @brief Producer guard-arm MC/DC vectors: work-arena calculator overflow,
 *        bump-carve guards, config/cap clamps, decoder-driven geometry
 *        rejections and the carve-boundary sweep (#231).
 *
 * @details
 * Complements `test_ra8_jof_produce.c` (happy paths + parity) and
 * `test_ra8_jof_png_hostile.c` (hostile PNG streams): this suite
 * drives the producer's own guard decisions to both outcomes -- the 64-bit
 * overflow checks in `ra8_jof_work_bytes()` and the band carve, the
 * `ra8_jof_priv_bump_take()` argument/exhaustion guards through the module
 * seam (the CLAUDE.md "test access to internal symbols" allowance), the
 * tile-geometry and cap-clamp arms of the configuration path, the
 * geometry-hook rejections only a hostile JPEG SOF can reach, and an
 * ascending work-arena sweep that lands in every carve-failure window of
 * both the PNG decoder bind and the producer pixel path.
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

/**
 * @enum t_png_layout_t
 * @brief PNG IHDR serialisation offsets used by the guard fixtures.
 *
 * @details
 * Offsets ending `_b<N>` are the `N`-th byte of a big-endian 32-bit field,
 * most-significant first.
 */
typedef enum : uint8_t {
  k_t_png_ihdr_len  = 13U,   /**< IHDR payload length, fixed by the PNG spec.     */
  k_t_ihdr_off_h_b1 = 5U,    /**< Height byte 1 within the IHDR payload.          */
  k_t_ihdr_off_h_b3 = 7U,    /**< Height byte 3 within the IHDR payload.          */
  k_t_ihdr_off_ct   = 9U,    /**< Colour-type byte within the IHDR payload.       */
  k_t_be32_hi_shift = 24U,   /**< Shift selecting the top byte of a 32-bit field. */
  k_t_byte_mask     = 0xFFU, /**< Low-byte mask while serialising it.             */
} t_png_layout_t;

/**
 * @enum t_guard_t
 * @brief Arena sizes and JPEG geometries that trip the producer's guards.
 */
typedef enum : uint8_t {
  k_t_arena_cap     = 64U, /**< Bump-arena backing store, bytes. */
  k_t_arena_cap_low = 7U,  /**< A capacity below the offset below, so the next
                                allocation must fail rather than wrap.         */
  k_t_arena_off     = 5U,  /**< Pre-set arena offset for that arm. */
  k_t_jpeg_big_edge = 64U, /**< The over-cap JPEG edge, applied to width in one
                                arm and height in the other.                   */
} t_guard_t;

/** @brief Suite geometry + buffer sizing. */
enum : uint32_t {
  k_g_dim       = 512U,          /**< Sweep source width/height.         */
  k_g_tile      = 32U,           /**< Sweep tile edge.                   */
  k_g_step      = 256U,          /**< Sweep cap step (< every carve).    */
  k_g_src_cap   = 320U * 1024U,  /**< Crafted source capacity.           */
  k_g_store_cap = 320U * 1024U,  /**< Memstore capacity.                 */
  k_g_work_cap  = 1024U * 1024U, /**< Work arena capacity.               */
  k_g_raw_cap   = 300U * 1024U,  /**< Raw scanline staging capacity.     */
  k_g_max_dim   = 32768U,        /**< Format dimension cap (mirror).     */
  k_g_over_dim  = 40000U,        /**< A cap request past the format.     */
  k_g_band_tile = 65535U,        /**< Tile height forcing band overflow. */
};

/** @brief Crafted source bytes. */
static uint8_t s_src[k_g_src_cap];
/** @brief Crafted source length. */
static size_t s_src_len;
/** @brief Work arena. */
static uint8_t s_work[k_g_work_cap];
/** @brief Memstore backing. */
static uint8_t s_store_buf[k_g_store_cap];
/** @brief Raw (filtered) scanline staging for the PNG builder. */
static uint8_t s_raw[k_g_raw_cap];
/** @brief zlib staging for the PNG builder. */
static uint8_t s_zbuf[k_g_src_cap];

/**
 * @struct g_pull_t
 * @brief Memory pull source over `s_src`.
 */
typedef struct {
  size_t pos; /**< Read cursor. */
} g_pull_t;

/** @brief ::ra8_jof_pull_fn over `s_src`. */
static ra8_err_t g_pull(void* ctx, uint8_t* buf, size_t cap, size_t* got)
{
  g_pull_t*    p    = (g_pull_t*)ctx;
  const size_t left = s_src_len - p->pos;
  const size_t take = (cap < left) ? cap : left;
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

/** @brief Append one big-endian u32 to the crafted source. */
static void put_be32(uint32_t v)
{
  const uint8_t b[4] = {(uint8_t)(v >> 24U),
                        (uint8_t)((v >> 16U) & 0xFFU),
                        (uint8_t)((v >> 8U) & 0xFFU),
                        (uint8_t)(v & 0xFFU)};
  put(b, sizeof(b));
}

/** @brief Append one PNG chunk (unverified CRC; the decoder skips it). */
static void put_chunk(const char* type, const uint8_t* data, uint32_t len)
{
  put_be32(len);
  put((const uint8_t*)type, 4U);
  if (len > 0U) {
    put(data, len);
  }
  const uint8_t crc[4] = {};
  put(crc, sizeof(crc));
}

/** @brief Start a crafted PNG: signature + an IHDR of (w, h, color). */
static void begin_png(uint32_t w, uint32_t h, uint8_t color)
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
  put_chunk("IHDR", ihdr, sizeof(ihdr));
}

/** @brief Build a complete gray8 PNG of (w, h) with a deterministic pattern. */
static void build_gray_png(uint32_t w, uint32_t h)
{
  begin_png(w, h, 0U);
  size_t o = 0U;
  for (uint32_t y = 0U; y < h; y++) {
    s_raw[o] = 0U;
    o++;
    for (uint32_t x = 0U; x < w; x++) {
      s_raw[o] = (uint8_t)((x * 3U) ^ y);
      o++;
    }
  }
  mz_ulong zlen = (mz_ulong)sizeof(s_zbuf);
  TEST_ASSERT_EQ(MZ_OK, mz_compress(s_zbuf, &zlen, s_raw, (mz_ulong)o));
  put_chunk("IDAT", s_zbuf, (uint32_t)zlen);
  put_chunk("IEND", nullptr, 0U);
}

/** @brief Craft a JPEG head: SOI + a single-component SOF0 of (w, h). */
static void build_jpeg_sof(uint32_t w, uint32_t h)
{
  s_src_len              = 0U;
  const uint8_t head[15] = {0xFFU,
                            0xD8U, /* SOI */
                            0xFFU,
                            0xC0U, /* SOF0 */
                            0x00U,
                            0x0BU, /* segment length 11 */
                            0x08U, /* precision 8       */
                            (uint8_t)(h >> 8U),
                            (uint8_t)(h & 0xFFU),
                            (uint8_t)(w >> 8U),
                            (uint8_t)(w & 0xFFU),
                            0x01U, /* 1 component  */
                            0x01U, /* component id */
                            0x11U, /* 1x1 sampling */
                            0x00U /* quant table 0 */};
  put(head, sizeof(head));
}

/** @brief Run the producer over the crafted source with explicit knobs. */
static ra8_err_t produce_with(uint16_t              tile_w,
                              uint16_t              tile_h,
                              uint16_t              max_w,
                              uint16_t              max_h,
                              size_t                work_cap,
                              ra8_jof_info_t* out_info)
{
  static g_pull_t                 s_pull;
  static ra8_jof_memstore_t s_store;
  s_pull  = (g_pull_t){.pos = 0U};
  s_store = (ra8_jof_memstore_t){.buf = s_store_buf, .cap = sizeof(s_store_buf), .len = 0U};
  const ra8_jof_produce_cfg_t cfg = {
    .pull       = g_pull,
    .pull_ctx   = &s_pull,
    .sink       = ra8_jof_memstore_sink,
    .sink_ctx   = &s_store,
    .tile_w     = tile_w,
    .tile_h     = tile_h,
    .codec      = (uint8_t)k_ra8_jof_codec_raw,
    .max_width  = max_w,
    .max_height = max_h,
    .work       = s_work,
    .work_cap   = work_cap,
  };
  return ra8_jof_produce(&cfg, out_info);
}

/**
 * @test test_guards_work_bytes_overflow
 * @brief The calculator's 64-bit overflow arms reject independently.
 *
 * @par MC/DC:
 * Decision: `stage > UINT32_MAX || total > UINT32_MAX` (2 conditions)
 * - Vector 1: sane geometry          -> false (every sized arena in-tree)
 * - Vector 2: 32768^2 tile (4 GiB stage) -> true via stage
 * - Vector 3: 16384x32768 tile (1 GiB stage, > 4 GiB total) -> true via
 *   total while the stage term stays under the cap
 */
static void test_guards_work_bytes_overflow(void)
{
  TEST_BEGIN("produce guards: work-arena calculator overflow arms");
  TEST_ASSERT(ra8_jof_work_bytes(1024U, 1024U, 128U, 128U) > 0U);
  TEST_ASSERT_EQ(0U,
                 ra8_jof_work_bytes((uint16_t)k_g_max_dim,
                                          (uint16_t)k_g_max_dim,
                                          (uint16_t)k_g_max_dim,
                                          (uint16_t)k_g_max_dim));
  TEST_ASSERT_EQ(0U,
                 ra8_jof_work_bytes((uint16_t)k_g_max_dim,
                                          (uint16_t)k_g_max_dim,
                                          (uint16_t)(k_g_max_dim / 2U),
                                          (uint16_t)k_g_max_dim));
  TEST_END("produce guards: work-arena calculator overflow arms");
}

/**
 * @test test_guards_bump_take
 * @brief The carve seam's argument and exhaustion guards, both arms each.
 *
 * @par MC/DC:
 * Decision (arguments): `bump == nullptr || len == 0` (2 conditions)
 * - Vector 1: valid take     -> false (every carve in-tree)
 * - Vector 2: null bump      -> true via bump
 * - Vector 3: zero length    -> true via len
 * Decision (exhaustion): `aligned > cap || len > cap - aligned` (2)
 * - Vector 1: fitting take   -> false
 * - Vector 2: alignment past the cap (off 5, cap 7)   -> true via aligned
 * - Vector 3: length past the remainder (len > cap)   -> true via len
 */
static void test_guards_bump_take(void)
{
  TEST_BEGIN("produce guards: bump-carve argument + exhaustion arms");
  static uint8_t s_backing[k_t_arena_cap];
  ra8_jof_bump_t  bump = {.base = s_backing, .cap = sizeof(s_backing), .off = 0U};

  TEST_ASSERT(ra8_jof_priv_bump_take(&bump, 8U) != nullptr);
  TEST_ASSERT_NULL(ra8_jof_priv_bump_take(nullptr, 8U));
  TEST_ASSERT_NULL(ra8_jof_priv_bump_take(&bump, 0U));

  /* Length overruns the remainder (aligned stays inside). */
  TEST_ASSERT_NULL(ra8_jof_priv_bump_take(&bump, sizeof(s_backing)));

  /* Alignment alone overruns a nearly-full arena. */
  ra8_jof_bump_t tight = {.base = s_backing, .cap = k_t_arena_cap_low, .off = k_t_arena_off};
  TEST_ASSERT_NULL(ra8_jof_priv_bump_take(&tight, 1U));
  TEST_END("produce guards: bump-carve argument + exhaustion arms");
}

/**
 * @test test_guards_cfg_and_caps
 * @brief Tile-geometry rejection arms and the format-cap clamp arms.
 *
 * @par MC/DC:
 * Decision (tiles): `tile_w == 0 || tile_h == 0` (2 conditions)
 * - Vector 1: 32x32       -> false (every transcode in-tree)
 * - Vector 2: tile_w 0    -> true via tile_w (produce suite)
 * - Vector 3: tile_h 0    -> true via tile_h (this test)
 * Decisions (cap clamps): `max != 0 && max < format cap` (2 each)
 * - Vector 1: max 0       -> false via the first condition (produce suite)
 * - Vector 2: max 512     -> true (both true; every capped transcode)
 * - Vector 3: max 40000   -> true/false: the second condition alone flips,
 *   clamping to the format cap while the transcode still succeeds
 */
static void test_guards_cfg_and_caps(void)
{
  TEST_BEGIN("produce guards: tile-geometry + format-cap clamp arms");
  ra8_jof_info_t info = {};
  build_gray_png(16U, 16U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, produce_with(8U, 0U, 512U, 512U, sizeof(s_work), &info));

  /* Cap requests past the format cap clamp to it and still transcode. */
  build_gray_png(16U, 16U);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    produce_with(8U, 8U, (uint16_t)k_g_over_dim, (uint16_t)k_g_over_dim, sizeof(s_work), &info));
  TEST_ASSERT_EQ(16U, info.width);
  TEST_ASSERT_EQ(16U, info.height);
  TEST_END("produce guards: tile-geometry + format-cap clamp arms");
}

/**
 * @test test_guards_jpeg_geometry
 * @brief Geometry-hook arms only a hostile JPEG SOF can reach: the shared
 *        JPEG parser forwards SOF dimensions unchecked, so the producer's
 *        hook is the validating layer for all four arms.
 *
 * @par MC/DC:
 * Decision: `w == 0 || w > cap_w || h == 0 || h > cap_h` (4 conditions)
 * - Vector 1: in-cap dims  -> false (every accepted source)
 * - Vector 2: width 0      -> true via w == 0
 * - Vector 3: width 64, cap 16  -> true via the width cap
 * - Vector 4: height 0     -> true via h == 0
 * - Vector 5: height 64, cap 16 -> true via the height cap
 */
static void test_guards_jpeg_geometry(void)
{
  TEST_BEGIN("produce guards: hostile JPEG SOF geometry arms");
  ra8_jof_info_t info = {};

  build_jpeg_sof(0U, 16U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, produce_with(8U, 8U, 512U, 512U, sizeof(s_work), &info));

  build_jpeg_sof(k_t_jpeg_big_edge, 16U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, produce_with(8U, 8U, 16U, 512U, sizeof(s_work), &info));

  build_jpeg_sof(16U, 0U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, produce_with(8U, 8U, 512U, 512U, sizeof(s_work), &info));

  build_jpeg_sof(16U, k_t_jpeg_big_edge);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, produce_with(8U, 8U, 512U, 16U, sizeof(s_work), &info));
  TEST_END("produce guards: hostile JPEG SOF geometry arms");
}

/**
 * @test test_guards_band_overflow
 * @brief The 64-bit band-size overflow arm: a full-cap-wide RGBA source
 *        with a 65535-tall tile pushes the band past 4 GiB before any
 *        carve happens.
 *
 * @par MC/DC:
 * (drives the band arm of the carve overflow decision true; the stage arm
 * is deactivated in-source -- stage_bytes <= band_bytes by construction,
 * so it cannot flip independently.)
 */
static void test_guards_band_overflow(void)
{
  TEST_BEGIN("produce guards: 64-bit band-size overflow arm");
  begin_png(k_g_max_dim, 1U, 6U); /* RGBA, full-cap width */
  const uint8_t one = 0U;
  put_chunk("IDAT", &one, 1U);
  put_chunk("IEND", nullptr, 0U);
  ra8_jof_info_t info = {};
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    produce_with((uint16_t)k_g_max_dim, (uint16_t)k_g_band_tile, 0U, 0U, sizeof(s_work), &info));
  TEST_END("produce guards: 64-bit band-size overflow arm");
}

/**
 * @test test_guards_carve_sweep
 * @brief An ascending work-cap sweep lands in every carve-failure window:
 *        each PNG bind buffer (inflate state, ring, input window, row,
 *        previous row, translate) and each producer buffer (band, stage,
 *        index) fails alone at some cap, and success is monotone.
 *
 * @par MC/DC:
 * Decision (PNG bind): 6-way OR over the carve results -- the sweep flips
 * each condition true in its own cap window (the carves are sequential, so
 * the first failing buffer short-circuits the rest).
 * Decision (pixel path): `band || stage || idx` -- same construction.
 * Every window is >= 512 bytes wide and the step is 256, so no window is
 * skipped; the final full-arena run supplies the all-false vector.
 */
static void test_guards_carve_sweep(void)
{
  TEST_BEGIN("produce guards: carve-boundary sweep (bind + pixel path)");
  build_gray_png(k_g_dim, k_g_dim);
  const uint32_t need = ra8_jof_work_bytes((uint16_t)k_g_dim,
                                                 (uint16_t)k_g_dim,
                                                 (uint16_t)k_g_tile,
                                                 (uint16_t)k_g_tile);
  TEST_ASSERT(need > 0U);
  TEST_ASSERT(need <= (uint32_t)sizeof(s_work));

  ra8_jof_info_t info    = {};
  bool                 seen_ok = false;
  for (uint32_t cap = (uint32_t)k_g_step; cap <= need; cap += (uint32_t)k_g_step) {
    const ra8_err_t err = produce_with((uint16_t)k_g_tile,
                                       (uint16_t)k_g_tile,
                                       (uint16_t)k_g_dim,
                                       (uint16_t)k_g_dim,
                                       (size_t)cap,
                                       &info);
    if (err == k_ra8_ok) {
      seen_ok = true; /* threshold reached: every larger cap also fits */
      break;
    }
    TEST_ASSERT_EQ(k_ra8_err_invalid_size, err);
  }
  TEST_ASSERT(seen_ok);

  /* The calculator-sized arena always succeeds (the documented contract). */
  TEST_ASSERT_EQ(k_ra8_ok,
                 produce_with((uint16_t)k_g_tile,
                              (uint16_t)k_g_tile,
                              (uint16_t)k_g_dim,
                              (uint16_t)k_g_dim,
                              (size_t)need,
                              &info));
  TEST_ASSERT_EQ(k_g_dim, info.width);
  TEST_ASSERT_EQ(k_g_dim, info.height);
  TEST_END("produce guards: carve-boundary sweep (bind + pixel path)");
}

/**
 * @brief Test entry point -- runs the producer guard suite in order.
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
  test_guards_work_bytes_overflow();
  test_guards_bump_take();
  test_guards_cfg_and_caps();
  test_guards_jpeg_geometry();
  test_guards_band_overflow();
  test_guards_carve_sweep();
  (void)fprintf(stderr, "[OK  ] test_ra8_jof_produce_guards.c\n");
  return 0;
}
