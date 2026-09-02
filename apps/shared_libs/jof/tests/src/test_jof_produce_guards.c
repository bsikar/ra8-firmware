/**
 * @file test_jof_produce_guards.c
 * @brief Producer guard-arm MC/DC vectors: work-arena calculator overflow,
 *        bump-carve guards, config/cap clamps, decoder-driven geometry
 *        rejections and the carve-boundary sweep (#231).
 *
 * @details
 * Complements `test_jof_produce.c` (happy paths + parity) and
 * `test_jof_png_hostile.c` (hostile PNG streams): this suite
 * drives the producer's own guard decisions to both outcomes -- the 64-bit
 * overflow checks in `jof_work_bytes()` and the band carve, the
 * `priv_jof_bump_take()` argument/exhaustion guards through the module
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

#include "jof.h"
#include "jof_internal.h"
#include "jof_produce.h"
#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
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

/**
 * @enum t_rows_geom_t
 * @brief The tiny synthetic geometry the row-sink contract vectors bind.
 *
 * @details
 * `priv_jof_on_rows` is driven directly here rather than through a decoder,
 * because no in-tree decoder can present a mismatched width or an unbound
 * geometry -- that is exactly what the guard exists to catch. The numbers are
 * deliberately tiny: one band of ::k_t_rows_tile_h rows at
 * ::k_t_rows_width x ::k_t_rows_bpp is ::k_t_rows_band_bytes bytes, so the
 * whole fixture is a stack-free static array.
 *
 * @invariant ::k_t_rows_band_bytes == width * bpp * tile_h.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_t_rows_width      = 4U,    /**< Bound source width, pixels.            */
  k_t_rows_height     = 4U,    /**< Bound source height, rows.             */
  k_t_rows_bpp        = 1U,    /**< Bound output bytes per pixel.          */
  k_t_rows_tile_h     = 4U,    /**< Band height, rows.                     */
  k_t_rows_tile_w     = 4U,    /**< Tile width, pixels.                    */
  k_t_rows_band_bytes = 16U,   /**< 4 px * 1 bpp * 4 rows.                 */
  k_t_rows_one        = 1U,    /**< One delivered row.                     */
  k_t_rows_none       = 0U,    /**< Zero delivered rows.                   */
  k_t_rows_fill       = 0xA5U, /**< Poison byte the delivered row carries. */
} t_rows_geom_t;

/** @brief Crafted source bytes. */
static uint8_t s_src[k_g_src_cap];
/** @brief Crafted source length. */
static size_t s_src_len;
/** @brief Work arena. */
static uint8_t s_work[k_g_work_cap];

/**
 * @struct g_pull_t
 * @brief Memory pull source over `s_src`.
 */
typedef struct {
  size_t pos; /**< Read cursor. */
} g_pull_t;

/** @brief ::jof_pull_fn over `s_src`. @details Exercises the g pull path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] ctx Injected callback context whose ownership remains with the test. @param[out] buf Byte buffer read or written by the exercised callback. @param[in] cap Capacity of the associated byte buffer in bytes. @param[out] got Receives the number of bytes transferred. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_g_pull(void* ctx, uint8_t* buf, size_t cap, size_t* got)
{
  g_pull_t*    p    = (g_pull_t*)ctx;
  const size_t left = s_src_len - p->pos;
  const size_t take = (cap < left) ? cap : left;
  (void)memcpy(buf, &s_src[p->pos], take);
  p->pos += take;
  *got = take;
  return k_ra8_ok;
}

/** @brief Append raw bytes to the crafted source. @details Exercises the put path with bounded caller-owned fixture state and verifies its documented result. @param[in] d Readable fixture byte sequence. @param[in] n Number of bytes or elements supplied. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_put(const uint8_t* d, size_t n)
{
  (void)memcpy(&s_src[s_src_len], d, n);
  s_src_len += n;
}

/** @brief Append one big-endian u32 to the crafted source. @details Exercises the put be32 path with bounded caller-owned fixture state and verifies its documented result. @param[in] v Integer value serialized into the fixture stream. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_put_be32(uint32_t v)
{
  const uint8_t b[4] = {(uint8_t)(v >> 24U),
                        (uint8_t)((v >> 16U) & 0xFFU),
                        (uint8_t)((v >> 8U) & 0xFFU),
                        (uint8_t)(v & 0xFFU)};
  internal_put(b, sizeof(b));
}

/** @brief Append one PNG chunk (unverified CRC; the decoder skips it). @details Exercises the put chunk path with bounded caller-owned fixture state and verifies its documented result. @param[in] type Four-byte PNG chunk type. @param[in] data Readable fixture payload bytes. @param[in] len Length of the associated byte sequence. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_put_chunk(const char* type, const uint8_t* data, uint32_t len)
{
  internal_put_be32(len);
  internal_put((const uint8_t*)type, 4U);
  if (len > 0U) {
    internal_put(data, len);
  }
  const uint8_t crc[4] = {};
  internal_put(crc, sizeof(crc));
}

/** @brief Start a crafted PNG: signature + an IHDR of (w, h, color). @details Exercises the begin png path with bounded caller-owned fixture state and verifies its documented result. @param[in] w Image width value or receiver exercised by this helper. @param[in] h Image height value or receiver exercised by this helper. @param[in] color PNG color-type selector. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_begin_png(uint32_t w, uint32_t h, uint8_t color)
{
  static const uint8_t sig[8] = {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU};
  s_src_len                   = 0U;
  internal_put(sig, sizeof(sig));
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
  internal_put_chunk("IHDR", ihdr, sizeof(ihdr));
}

/** @brief Build a complete gray8 PNG of (w, h) with a deterministic pattern. @details Exercises the build gray png path with bounded caller-owned fixture state and verifies its documented result. @param[in] w Image width value or receiver exercised by this helper. @param[in] h Image height value or receiver exercised by this helper. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_build_gray_png(uint32_t w, uint32_t h)
{
  static uint8_t s_raw[k_g_raw_cap];
  static uint8_t s_zbuf[k_g_src_cap];
  internal_begin_png(w, h, 0U);
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
  internal_put_chunk("IDAT", s_zbuf, (uint32_t)zlen);
  internal_put_chunk("IEND", nullptr, 0U);
}

/** @brief Craft a JPEG head: SOI + a single-component SOF0 of (w, h). @details Exercises the build jpeg sof path with bounded caller-owned fixture state and verifies its documented result. @param[in] w Image width value or receiver exercised by this helper. @param[in] h Image height value or receiver exercised by this helper. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_build_jpeg_sof(uint32_t w, uint32_t h)
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
  internal_put(head, sizeof(head));
}

/** @brief Run the producer over the crafted source with explicit knobs. @details Exercises the produce with path with bounded caller-owned fixture state and verifies its documented result. @param[in] tile_w Tile width in pixels. @param[in] tile_h Tile height in pixels. @param[in] max_w Maximum accepted image width in pixels. @param[in] max_h Maximum accepted image height in pixels. @param[in] work_cap Capacity of the caller-owned work arena in bytes. @param[out] out_info Receives parsed image metadata. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_produce_with(uint16_t    tile_w,
                                                    uint16_t    tile_h,
                                                    uint16_t    max_w,
                                                    uint16_t    max_h,
                                                    size_t      work_cap,
                                                    jof_info_t* out_info)
{
  static uint8_t        s_store_buf[k_g_store_cap];
  static g_pull_t       s_pull;
  static jof_memstore_t s_store;
  s_pull  = (g_pull_t){.pos = 0U};
  s_store = (jof_memstore_t){.buf = s_store_buf, .cap = sizeof(s_store_buf), .len = 0U};
  const jof_produce_cfg_t cfg = {
    .pull       = internal_g_pull,
    .pull_ctx   = &s_pull,
    .sink       = jof_memstore_sink,
    .sink_ctx   = &s_store,
    .tile_w     = tile_w,
    .tile_h     = tile_h,
    .codec      = (uint8_t)k_jof_codec_raw,
    .max_width  = max_w,
    .max_height = max_h,
    .work       = s_work,
    .work_cap   = work_cap,
  };
  return jof_produce(&cfg, out_info);
}

/**
 * @test internal_test_guards_work_bytes_overflow
 * @brief The calculator's 64-bit overflow arms reject independently.
 *
 * @par MC/DC:
 * Decision: `stage > UINT32_MAX || total > UINT32_MAX` (2 conditions)
 * - Vector 1: sane geometry          -> false (every sized arena in-tree)
 * - Vector 2: 32768^2 tile (4 GiB stage) -> true via stage
 * - Vector 3: 16384x32768 tile (1 GiB stage, > 4 GiB total) -> true via
 *   total while the stage term stays under the cap @details Executes the guards work bytes overflow scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_guards_work_bytes_overflow(void)
{
  TEST_BEGIN("produce guards: work-arena calculator overflow arms");
  TEST_ASSERT(jof_work_bytes(1024U, 1024U, 128U, 128U) > 0U);
  TEST_ASSERT_EQ(0U,
                 jof_work_bytes((uint16_t)k_g_max_dim,
                                (uint16_t)k_g_max_dim,
                                (uint16_t)k_g_max_dim,
                                (uint16_t)k_g_max_dim));
  TEST_ASSERT_EQ(0U,
                 jof_work_bytes((uint16_t)k_g_max_dim,
                                (uint16_t)k_g_max_dim,
                                (uint16_t)(k_g_max_dim / 2U),
                                (uint16_t)k_g_max_dim));
  TEST_END("produce guards: work-arena calculator overflow arms");
}

/**
 * @test internal_test_guards_bump_take
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
 * - Vector 3: length past the remainder (len > cap)   -> true via len @details Executes the guards bump take scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_guards_bump_take(void)
{
  TEST_BEGIN("produce guards: bump-carve argument + exhaustion arms");
  static uint8_t s_backing[k_t_arena_cap];
  jof_bump_t     bump = {.base = s_backing, .cap = sizeof(s_backing), .off = 0U};

  TEST_ASSERT(priv_jof_bump_take(&bump, 8U) != nullptr);
  TEST_ASSERT_NULL(priv_jof_bump_take(nullptr, 8U));
  TEST_ASSERT_NULL(priv_jof_bump_take(&bump, 0U));

  /* Length overruns the remainder (aligned stays inside). */
  TEST_ASSERT_NULL(priv_jof_bump_take(&bump, sizeof(s_backing)));

  /* Alignment alone overruns a nearly-full arena. */
  jof_bump_t tight = {.base = s_backing, .cap = k_t_arena_cap_low, .off = k_t_arena_off};
  TEST_ASSERT_NULL(priv_jof_bump_take(&tight, 1U));
  TEST_END("produce guards: bump-carve argument + exhaustion arms");
}

/**
 * @test internal_test_guards_cfg_and_caps
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
 *   clamping to the format cap while the transcode still succeeds @details Executes the guards cfg and caps scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_guards_cfg_and_caps(void)
{
  TEST_BEGIN("produce guards: tile-geometry + format-cap clamp arms");
  jof_info_t info = {};
  internal_build_gray_png(16U, 16U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 internal_produce_with(8U, 0U, 512U, 512U, sizeof(s_work), &info));

  /* Cap requests past the format cap clamp to it and still transcode. */
  internal_build_gray_png(16U, 16U);
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_produce_with(8U,
                                       8U,
                                       (uint16_t)k_g_over_dim,
                                       (uint16_t)k_g_over_dim,
                                       sizeof(s_work),
                                       &info));
  TEST_ASSERT_EQ(16U, info.width);
  TEST_ASSERT_EQ(16U, info.height);
  TEST_END("produce guards: tile-geometry + format-cap clamp arms");
}

/**
 * @test internal_test_guards_jpeg_geometry
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
 * - Vector 5: height 64, cap 16 -> true via the height cap @details Executes the guards jpeg geometry scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_guards_jpeg_geometry(void)
{
  TEST_BEGIN("produce guards: hostile JPEG SOF geometry arms");
  jof_info_t info = {};

  internal_build_jpeg_sof(0U, 16U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 internal_produce_with(8U, 8U, 512U, 512U, sizeof(s_work), &info));

  internal_build_jpeg_sof(k_t_jpeg_big_edge, 16U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 internal_produce_with(8U, 8U, 16U, 512U, sizeof(s_work), &info));

  internal_build_jpeg_sof(16U, 0U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 internal_produce_with(8U, 8U, 512U, 512U, sizeof(s_work), &info));

  internal_build_jpeg_sof(16U, k_t_jpeg_big_edge);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 internal_produce_with(8U, 8U, 512U, 16U, sizeof(s_work), &info));
  TEST_END("produce guards: hostile JPEG SOF geometry arms");
}

/**
 * @test internal_test_guards_band_overflow
 * @brief The 64-bit band-size overflow arm: a full-cap-wide RGBA source
 *        with a 65535-tall tile pushes the band past 4 GiB before any
 *        carve happens.
 *
 * @par MC/DC:
 * (drives the band arm of the carve overflow decision true; the stage arm
 * is deactivated in-source -- stage_bytes <= band_bytes by construction,
 * so it cannot flip independently.) @details Executes the guards band overflow scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_guards_band_overflow(void)
{
  TEST_BEGIN("produce guards: 64-bit band-size overflow arm");
  internal_begin_png(k_g_max_dim, 1U, 6U); /* RGBA, full-cap width */
  const uint8_t one = 0U;
  internal_put_chunk("IDAT", &one, 1U);
  internal_put_chunk("IEND", nullptr, 0U);
  jof_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 internal_produce_with((uint16_t)k_g_max_dim,
                                       (uint16_t)k_g_band_tile,
                                       0U,
                                       0U,
                                       sizeof(s_work),
                                       &info));
  TEST_END("produce guards: 64-bit band-size overflow arm");
}

/**
 * @test internal_test_guards_carve_sweep
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
 * skipped; the final full-arena run supplies the all-false vector. @details Executes the guards carve sweep scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_guards_carve_sweep(void)
{
  TEST_BEGIN("produce guards: carve-boundary sweep (bind + pixel path)");
  internal_build_gray_png(k_g_dim, k_g_dim);
  const uint32_t need =
    jof_work_bytes((uint16_t)k_g_dim, (uint16_t)k_g_dim, (uint16_t)k_g_tile, (uint16_t)k_g_tile);
  TEST_ASSERT(need > 0U);
  TEST_ASSERT(need <= (uint32_t)sizeof(s_work));

  jof_info_t info    = {};
  bool       seen_ok = false;
  for (uint32_t cap = (uint32_t)k_g_step; cap <= need; cap += (uint32_t)k_g_step) {
    const ra8_err_t err = internal_produce_with((uint16_t)k_g_tile,
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
                 internal_produce_with((uint16_t)k_g_tile,
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
 * @brief Bind a minimal 4x4x1 producer state over file-static row storage.
 *
 * @details
 * Both row-sink contract cases need the same thing: a `jof_prod_state_t`
 * whose geometry is bound, whose band can hold one tile row, and a pixel
 * buffer to hand it. Building that inline in each case would put the fixture
 * and the vectors on one display page; extracting it keeps each case to the
 * vectors it owns, and guarantees the two cases start from byte-identical
 * state rather than from two hand-written copies that could drift.
 *
 * The band and pixel buffers are function-static because they must out-live
 * the call while staying invisible to everything else in the suite. They are
 * re-zeroed (and re-poisoned) on every call, so a second bind is a full
 * reset, not a continuation.
 *
 * @param[out] st  Producer state to populate; zero-initialised by the caller.
 * @param[out] cfg Configuration the state will point at; must out-live @p st.
 *
 * @return The pixel buffer to deliver rows from.
 *
 * @pre @p st and @p cfg are non-NULL and were zero-initialised (`= {}`).
 * @pre @p cfg out-lives every `priv_jof_on_rows` call made with @p st.
 * @post `st->geom_done` is 1 and `st->rows_seen` / `st->band_fill` are 0.
 * @post The returned buffer holds ::k_t_rows_band_bytes readable bytes.
 *
 * @note Not thread-safe; the storage is file-lifetime state.
 * @since 0.1.0
 */
RA8_INTERNAL static const uint8_t* internal_rows_guard_bind(jof_prod_state_t*  st,
                                                            jof_produce_cfg_t* cfg)
{
  static uint8_t s_rows_band[k_t_rows_band_bytes];
  static uint8_t s_rows_px[k_t_rows_band_bytes];
  (void)memset(s_rows_band, 0, sizeof(s_rows_band));
  (void)memset(s_rows_px, (int)k_t_rows_fill, sizeof(s_rows_px));

  cfg->tile_w = (uint16_t)k_t_rows_tile_w;
  cfg->tile_h = (uint16_t)k_t_rows_tile_h;

  st->cfg       = cfg;
  st->w         = (uint16_t)k_t_rows_width;
  st->h         = (uint16_t)k_t_rows_height;
  st->bpp       = (uint8_t)k_t_rows_bpp;
  st->band      = s_rows_band;
  st->geom_done = 1U;
  st->rows_seen = 0U;
  st->band_fill = 0U;
  return s_rows_px;
}

/**
 * @test internal_test_mcdc_on_rows_geometry_guard
 * @brief Drive the row-sink geometry contract guard to all four MC/DC
 *        outcomes through the RA8_PRIV seam.
 *
 * @details
 * The guard exists because a decoder arm could deliver rows before the
 * geometry hook bound the frame, or deliver rows whose width or channel
 * count disagrees with what was bound. No in-tree decoder can do that -- the
 * previous deactivation rationale was right about the DECODERS -- but
 * `priv_jof_on_rows` is a ::jof_rows_fn seam, so the contract it enforces is
 * a property of the sink, not of today's two callers. Calling it directly is
 * the documented "test access to internal symbols" route, and it makes the
 * guard testable without inventing a hostile decoder.
 *
 * Each rejecting vector returns before a single byte is copied, so the
 * fixture needs only one band of backing store.
 *
 * @par MC/DC:
 * Decision: `(st->geom_done == 0U) || (width != st->w) || (channels != st->bpp)`
 * in apps/shared_libs/jof/src/jof_produce.c@priv_jof_on_rows (3 conditions).
 * - V1: geom_done=1, width=st->w, channels=st->bpp -> C1=F, C2=F, C3=F ->
 *   decision F. The row is accepted, copied into the band and `rows_seen`
 *   advances; the call returns k_ra8_ok. (Control.)
 * - V2: geom_done=0, width=st->w, channels=st->bpp -> C1=T, short-circuits ->
 *   decision T -> k_ra8_err_validation_failed. Pair (V1,V2) varies C1 alone.
 * - V3: geom_done=1, width=st->w + 1, channels=st->bpp -> C1=F, C2=T ->
 *   decision T. Pair (V1,V3) varies C2 alone.
 * - V4: geom_done=1, width=st->w, channels=st->bpp + 1 -> C1=F, C2=F, C3=T ->
 *   decision T. Pair (V1,V4) varies C3 alone.
 * N = 3 conditions, N+1 = 4 vectors: minimal MC/DC. Every rejecting vector
 * leaves `rows_seen` at its prior value, which is what distinguishes them
 * from V1 beyond the return code.
 *
 * @pre The fixture band is at least ::k_t_rows_band_bytes bytes.
 * @pre The state is rebuilt between vectors so no vector inherits another.
 * @post `rows_seen` advanced only for V1.
 * @post No fixture byte outside the band was written.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_on_rows_geometry_guard(void)
{
  TEST_BEGIN("jof produce MC/DC: on_rows geometry contract guard");
  jof_produce_cfg_t cfg = {};
  jof_prod_state_t  st  = {};
  const uint8_t*    px  = internal_rows_guard_bind(&st, &cfg);

  /* V1: every condition false -- the row is accepted. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 priv_jof_on_rows(&st,
                                  px,
                                  (uint16_t)k_t_rows_width,
                                  0U,
                                  (uint16_t)k_t_rows_one,
                                  (uint8_t)k_t_rows_bpp));
  TEST_ASSERT_EQ((uint32_t)k_t_rows_one, st.rows_seen);
  TEST_ASSERT_EQ((uint32_t)k_t_rows_one, st.band_fill);

  /* V2: geometry never bound -- C1 alone flips. */
  st.geom_done = 0U;
  st.rows_seen = 0U;
  st.band_fill = 0U;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 priv_jof_on_rows(&st,
                                  px,
                                  (uint16_t)k_t_rows_width,
                                  0U,
                                  (uint16_t)k_t_rows_one,
                                  (uint8_t)k_t_rows_bpp));
  TEST_ASSERT_EQ(0U, st.rows_seen);

  /* V3: width disagrees with the bound geometry -- C2 alone flips. */
  st.geom_done = 1U;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 priv_jof_on_rows(&st,
                                  px,
                                  (uint16_t)k_t_rows_width + 1U,
                                  0U,
                                  (uint16_t)k_t_rows_one,
                                  (uint8_t)k_t_rows_bpp));
  TEST_ASSERT_EQ(0U, st.rows_seen);

  /* V4: channel count disagrees -- C3 alone flips. */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 priv_jof_on_rows(&st,
                                  px,
                                  (uint16_t)k_t_rows_width,
                                  0U,
                                  (uint16_t)k_t_rows_one,
                                  (uint8_t)k_t_rows_bpp + 1U));
  TEST_ASSERT_EQ(0U, st.rows_seen);
  TEST_END("jof produce MC/DC: on_rows geometry contract guard");
}

/**
 * @test internal_test_mcdc_on_rows_order_guard
 * @brief Drive the zero-row condition of the row-ORDERING contract guard,
 *        the second deactivated decision in the same sink.
 *
 * @details
 * `priv_jof_on_rows` carries two contract guards back to back. The geometry
 * one above rejects rows that disagree with the bound frame; this one rejects
 * rows that arrive out of order -- a zero-length delivery, a `y0` that is not
 * the next unseen row, or a group that would overrun the frame. Only the
 * first of its three conditions is constructible without also violating the
 * geometry guard ahead of it, so this case owns that one condition and says
 * so rather than claiming the decision is complete.
 *
 * @par MC/DC:
 * Decision: `(nrows == 0U) || ((uint32_t)y0 != st->rows_seen) ||
 * (((uint32_t)y0 + (uint32_t)nrows) > (uint32_t)st->h)` in
 * apps/shared_libs/jof/src/jof_produce.c@priv_jof_on_rows (3 conditions).
 * - V1: nrows=1, y0=0 == rows_seen, y0+nrows=1 <= h -> C1=F, C2=F, C3=F ->
 *   decision F, the row is accepted. (Control.)
 * - V2: nrows=0, everything else identical -> C1=T, short-circuits ->
 *   decision T -> k_ra8_err_validation_failed. Pair (V1,V2) varies C1 alone
 *   and produces opposite outcomes, so C1 independently affects the decision.
 * That is 2 of the N+1 = 4 vectors this decision needs. C2 and C3 are NOT
 * driven here and this case does not clear that deactivation: a `y0` out of
 * step, or a group overrunning `h`, is reachable only by a caller that has
 * already satisfied the geometry guard with a frame it then contradicts, and
 * no such construction is attempted.
 *
 * @pre The state is freshly bound, so `rows_seen` is zero.
 * @pre The fixture band holds at least one row.
 * @post The rejected delivery left `rows_seen` unchanged.
 * @post No fixture byte outside the band was written.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_on_rows_order_guard(void)
{
  TEST_BEGIN("jof produce MC/DC: on_rows row-ordering guard (zero rows)");
  jof_produce_cfg_t cfg = {};
  jof_prod_state_t  st  = {};
  const uint8_t*    px  = internal_rows_guard_bind(&st, &cfg);

  /* V1: an in-order single row passes both guards. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 priv_jof_on_rows(&st,
                                  px,
                                  (uint16_t)k_t_rows_width,
                                  0U,
                                  (uint16_t)k_t_rows_one,
                                  (uint8_t)k_t_rows_bpp));
  TEST_ASSERT_EQ((uint32_t)k_t_rows_one, st.rows_seen);

  /* V2: a zero-row delivery -- C1 alone flips. Rebound first so the ordering
     condition C2 stays false and cannot be what rejects the call. */
  px = internal_rows_guard_bind(&st, &cfg);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 priv_jof_on_rows(&st,
                                  px,
                                  (uint16_t)k_t_rows_width,
                                  0U,
                                  (uint16_t)k_t_rows_none,
                                  (uint8_t)k_t_rows_bpp));
  TEST_ASSERT_EQ(0U, st.rows_seen);
  TEST_END("jof produce MC/DC: on_rows row-ordering guard (zero rows)");
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
int main(void)
{
  internal_test_guards_work_bytes_overflow();
  internal_test_guards_bump_take();
  internal_test_guards_cfg_and_caps();
  internal_test_guards_jpeg_geometry();
  internal_test_guards_band_overflow();
  internal_test_guards_carve_sweep();
  internal_test_mcdc_on_rows_geometry_guard();
  internal_test_mcdc_on_rows_order_guard();
  return 0;
}
