/**
 * @file test_reflow_image.c
 * @brief Host unit tests + MC/DC for apps/shared_libs/reflow/src/reflow_image.c (#106).
 *
 * @details
 * Exercises the zero-heap raster decode + nearest-neighbour scale + blit path
 * end to end against the real ra8_gfx framebuffer:
 *  - ra8_img_probe_size() reads a baked 2x2 PNG's dimensions.
 *  - ra8_img_decode_blit() decodes + blits it 1:1 and 2x, and the test reads the
 *    framebuffer back to assert exact pixels.
 *  - The caller-bound bump arena fully drains after every call (success AND
 *    failure), proving the decode reaches no `malloc` (NASA P10 Rule 3).
 *  - MC/DC for the new compound decisions: the public argument-precondition
 *    (3-condition OR, driven through the real API) plus mirror helpers for the
 *    two TU-private decisions (fit-box branch, decode-failure classify).
 *  - The WebP arm (#637): the committed 8x8 lossless fixture probes and blits
 *    through the same public entry points, bit-exact against the fixture's
 *    documented source pattern, and the two conditions of the RIFF/WEBP
 *    signature test are driven independently (a non-RIFF buffer, and a RIFF
 *    buffer whose form tag is not WEBP -- which must fall through to stb_image
 *    and be rejected there, not mis-routed into the WebP facade).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "reflow_image.h"
#include "unity_minimal.h"

/**
 * @enum t_img_buf_t
 * @brief Decode arena and the deliberately-too-small destination.
 */
typedef enum : uint16_t {
  k_t_kib          = 1024U, /**< Bytes per KiB.             */
  k_t_scratch_kib  = 64U,   /**< Decode arena size, in KiB. */
  k_t_tiny_dst_cap = 48U,   /**< A destination far below one decoded row, so
                                 the out-of-room path is the one taken.        */
} t_img_buf_t;

/**
 * @brief A 2x2 RGB PNG: TL red, TR green, BL blue, BR white.
 * @details Baked from PIL; the smallest fixture that proves channel order and
 * row order survive decode + blit.
 */
static const uint8_t s_png_2x2[] = {
  137, 80,  78,  71, 13,  10,  26,  10,  0,   0,   0,   13,  73,  72,  68,  82,  0,   0,  0,   2,
  0,   0,   0,   2,  8,   2,   0,   0,   0,   253, 212, 154, 115, 0,   0,   0,   22,  73, 68,  65,
  84,  120, 156, 99, 248, 207, 192, 192, 240, 159, 129, 145, 129, 225, 255, 255, 255, 12, 0,   30,
  246, 4,   253, 9,  237, 52,  62,  0,   0,   0,   0,   73,  69,  78,  68,  174, 66,  96, 130,
};

/** @brief Eight bytes that are not any image format stb_image accepts. */
static const uint8_t s_junk[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};

/**
 * @brief The committed 8x8 VP8L (lossless) WebP, tests/fixtures/webp/fixture_lossless.webp.
 * @details Embedded inline (52 bytes) so the test stays free of file I/O, the
 * same convention tests/graphics/src/test_ra8_webp.c uses for the same file.
 * Its pixel at `(x, y)` is `(r, g, b, a) = ((x*32) & 255, (y*32) & 255,
 * ((x+y)*16) & 255, 255)`; being lossless, the decode is bit-exact, so the blit
 * can be compared against that pattern rather than merely checked for success.
 */
static const uint8_t s_webp_lossless_8x8[] = {
  0x52, 0x49, 0x46, 0x46, 0x2C, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56,
  0x50, 0x38, 0x4C, 0x1F, 0x00, 0x00, 0x00, 0x2F, 0x07, 0xC0, 0x01, 0x00, 0xCD,
  0x65, 0x44, 0xFF, 0x63, 0x17, 0x85, 0x28, 0x78, 0xFF, 0x03, 0x42, 0x02, 0xC2,
  0x14, 0xFF, 0x77, 0x6A, 0x0E, 0x0C, 0x48, 0xC4, 0x04, 0x80, 0xAD, 0x0D, 0x00,
};

/**
 * @brief A RIFF container whose form tag is `WAVE`, not `WEBP`.
 * @details The second condition of the signature test in isolation: the `RIFF`
 * tag matches and the form tag does not, so this must NOT reach the WebP facade
 * -- it falls through to stb_image, which rejects it as an unknown format.
 */
static const uint8_t s_riff_not_webp[16] = {
  0x52, 0x49, 0x46, 0x46, 0x08, 0x00, 0x00, 0x00,
  0x57, 0x41, 0x56, 0x45, 0x00, 0x00, 0x00, 0x00,
};

/**
 * @enum t_webp_geom_t
 * @brief Geometry of the 8x8 WebP fixture and its framebuffer (no magic numbers).
 */
typedef enum : uint16_t {
  k_t_webp_dim      = 8U,  /**< Fixture width and height, pixels.          */
  k_t_webp_r_step   = 32U, /**< Red step per source column in the pattern. */
  k_t_webp_g_step   = 32U, /**< Green step per source row in the pattern.  */
  k_t_webp_b_step   = 16U, /**< Blue step per (x + y) in the pattern.      */
  k_t_webp_byte_max = 255U /**< Channel mask for the pattern arithmetic.   */
} t_webp_geom_t;

/* RGB565 colour helpers: ra8_gfx packs 0x00RRGGBB -> 565; reading back, compare
 * against the same quantisation the framebuffer stores. */
enum : uint32_t {
  k_red   = 0xFF0000U, /**< Red.   */
  k_green = 0x00FF00U, /**< Green. */
  k_blue  = 0x0000FFU, /**< Blue.  */
  k_white = 0xFFFFFFU, /**< White. */
};

/** @brief Read an RGB888 framebuffer pixel back as 0x00RRGGBB.
 * @details Exercises the fb px path and preserves each documented result and bound.
 * @param[in] fb Caller-supplied fb value used by the scenario.
 * @param[in] w Caller-supplied w value used by the scenario.
 * @param[in] x Caller-supplied x value used by the scenario.
 * @param[in] y Caller-supplied y value used by the scenario.
 * @return The scalar result computed for the requested reflow scenario.
 * @retval 0 The helper produced its zero-valued result.
 * @retval nonzero The helper produced a nonzero result.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_fb_px(const uint8_t* fb, int32_t w, int32_t x, int32_t y)
{
  const size_t i = (((size_t)y * (size_t)w) + (size_t)x) * 3U;
  return ((uint32_t)fb[i] << 16) | ((uint32_t)fb[i + 1U] << 8) | (uint32_t)fb[i + 2U];
}

/**
 * @test internal_test_probe_size_png
 * @brief ra8_img_probe_size reads 2x2 and rejects NULL arguments.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- drives ra8_img_probe_size's single-condition
 * null guards (bytes, out_w), the single-condition SVG dispatch, and the
 * single-condition `stbi_info_from_memory(...) == 0` reject (2x2 PNG -> k_ra8_ok;
 * junk -> not_supported); no && or || is reached on this path.)
 * @details Exercises the probe size png path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_probe_size_png(void)
{
  TEST_BEGIN("ra8_img_probe_size: 2x2 PNG + null guards");
  int32_t w = 0;
  int32_t h = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_img_probe_size(s_png_2x2, sizeof s_png_2x2, &w, &h));
  TEST_ASSERT_EQ(2, w);
  TEST_ASSERT_EQ(2, h);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_img_probe_size(NULL, 1U, &w, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_img_probe_size(s_png_2x2, sizeof s_png_2x2, NULL, &h));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_img_probe_size(s_junk, sizeof s_junk, &w, &h));
  TEST_END("ra8_img_probe_size: 2x2 PNG + null guards");
}

/**
 * @test internal_test_decode_blit_pixels
 * @brief Decode + blit the 2x2 PNG 1:1 and 2x; assert exact framebuffer pixels.
 *
 * @par MC/DC:
 * Decision: `(len == 0) || (box_w < 1) || (box_h < 1)` (3 conditions, OR; function
 * `ra8_img_decode_blit`). Both decode+blit calls here (2x2 PNG, box 2x2 then 4x4)
 * hold all three conditions false:
 * - V1 len=N, box_w>=1, box_h>=1 -> C1 F, C2 F, C3 F -> F (decode proceeds ->
 *   k_ra8_ok, exact framebuffer pixels asserted 1:1 and 2x).
 * This is the all-false control of the OR; the three true arms (each condition
 * individually) that complete N+1 = 4 are driven by
 * internal_test_decode_blit_precondition_mcdc.
 * @details Exercises the decode blit pixels path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_decode_blit_pixels(void)
{
  TEST_BEGIN("ra8_img_decode_blit: exact pixels 1:1 and 2x");
  static uint8_t  s_scratch[k_t_scratch_kib * k_t_kib];
  ra8_img_arena_t arena = {.base = s_scratch, .cap = sizeof s_scratch, .offset = 0U, .live = 0U};

  static uint8_t s_fb[4 * 4 * 3];
  (void)memset(s_fb, 0, sizeof s_fb);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_init(s_fb, 4, 4, k_ra8_gfx_format_rgb888));

  int32_t ow = 0;
  int32_t oh = 0;
  /* 1:1 into a 2x2 box. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_img_decode_blit(&arena, s_png_2x2, sizeof s_png_2x2, 0, 0, 2, 2, &ow, &oh));
  TEST_ASSERT_EQ(2, ow);
  TEST_ASSERT_EQ(2, oh);
  TEST_ASSERT_EQ(k_red, internal_fb_px(s_fb, 4, 0, 0));
  TEST_ASSERT_EQ(k_green, internal_fb_px(s_fb, 4, 1, 0));
  TEST_ASSERT_EQ(k_blue, internal_fb_px(s_fb, 4, 0, 1));
  TEST_ASSERT_EQ(k_white, internal_fb_px(s_fb, 4, 1, 1));
  /* Arena fully drained -> zero heap. */
  TEST_ASSERT_EQ(0, arena.offset);
  TEST_ASSERT_EQ(0, arena.live);

  /* 2x nearest-neighbour upscale into a 4x4 box: corners preserved. */
  (void)memset(s_fb, 0, sizeof s_fb);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_img_decode_blit(&arena, s_png_2x2, sizeof s_png_2x2, 0, 0, 4, 4, &ow, &oh));
  TEST_ASSERT_EQ(4, ow);
  TEST_ASSERT_EQ(4, oh);
  TEST_ASSERT_EQ(k_red, internal_fb_px(s_fb, 4, 0, 0));
  TEST_ASSERT_EQ(k_green, internal_fb_px(s_fb, 4, 3, 0));
  TEST_ASSERT_EQ(k_blue, internal_fb_px(s_fb, 4, 0, 3));
  TEST_ASSERT_EQ(k_white, internal_fb_px(s_fb, 4, 3, 3));
  TEST_END("ra8_img_decode_blit: exact pixels 1:1 and 2x");
}

/**
 * @test internal_test_decode_blit_precondition_mcdc
 *
 * @par MC/DC:
 * Decision: `if (len == 0 || box_w < 1 || box_h < 1)` (3 conditions, OR;
 * apps/shared_libs/reflow/src/reflow_image.c@ra8_img_decode_blit). Driven directly
 * through the public API by its return code -- production-source MC/DC.
 *
 * Vectors (Chilenski masking-MC/DC, N+1 = 4 for N=3):
 *  - V1: len=N, box_w=2, box_h=2 -> all F -> decision F (decodes -> k_ra8_ok).
 *  - V2: len=0, box_w=2, box_h=2 -> C1 T -> decision T (k_ra8_err_invalid_arg).
 *  - V3: len=N, box_w=0, box_h=2 -> C2 T -> decision T (k_ra8_err_invalid_arg).
 *  - V4: len=N, box_w=2, box_h=0 -> C3 T -> decision T (k_ra8_err_invalid_arg).
 *
 * Independence: V1 vs V2/V3/V4 each flip exactly one condition and the outcome,
 * with the other two held at their false (control) value.
 * @brief Verify decode blit precondition mcdc behavior against the reflow contract.
 * @details Exercises the decode blit precondition mcdc path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_decode_blit_precondition_mcdc(void)
{
  TEST_BEGIN("ra8_img_decode_blit precondition MC/DC: len==0||box_w<1||box_h<1");
  static uint8_t  s_scratch[k_t_scratch_kib * k_t_kib];
  ra8_img_arena_t arena = {.base = s_scratch, .cap = sizeof s_scratch, .offset = 0U, .live = 0U};
  static uint8_t  s_fb[4 * 4 * 3];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_init(s_fb, 4, 4, k_ra8_gfx_format_rgb888));

  /* V1: control -- all conditions false -> decodes. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_img_decode_blit(&arena, s_png_2x2, sizeof s_png_2x2, 0, 0, 2, 2, NULL, NULL));
  /* V2: len == 0. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_img_decode_blit(&arena, s_png_2x2, 0U, 0, 0, 2, 2, NULL, NULL));
  /* V3: box_w < 1. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_img_decode_blit(&arena, s_png_2x2, sizeof s_png_2x2, 0, 0, 0, 2, NULL, NULL));
  /* V4: box_h < 1. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_img_decode_blit(&arena, s_png_2x2, sizeof s_png_2x2, 0, 0, 2, 0, NULL, NULL));
  /* Null guards (separate earlier decisions). */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_img_decode_blit(NULL, s_png_2x2, sizeof s_png_2x2, 0, 0, 2, 2, NULL, NULL));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_img_decode_blit(&arena, NULL, sizeof s_png_2x2, 0, 0, 2, 2, NULL, NULL));
  TEST_END("ra8_img_decode_blit precondition MC/DC: len==0||box_w<1||box_h<1");
}

/** @brief Mirror of internal_fit_box's branch selector (size-clamp decision).
 * @details Exercises the mirror fit width limited path and preserves each documented result and bound.
 * @param[in] box_w Caller-supplied box w value used by the scenario.
 * @param[in] src_h Caller-supplied src h value used by the scenario.
 * @param[in] box_h Caller-supplied box h value used by the scenario.
 * @param[in] src_w Caller-supplied src w value used by the scenario.
 * @return The scalar result computed for the requested reflow scenario.
 * @retval 0 The helper produced its zero-valued result.
 * @retval nonzero The helper produced a nonzero result.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t
internal_mirror_fit_width_limited(int32_t box_w, int32_t src_h, int32_t box_h, int32_t src_w)
{
  if (((int64_t)box_w * (int64_t)src_h) <= ((int64_t)box_h * (int64_t)src_w)) {
    return 1U; /* width-limited branch */
  }
  return 0U; /* height-limited branch */
}

/**
 * @test internal_test_fit_box_branch_mcdc
 *
 * @par MC/DC:
 * Decision: `if (box_w*src_h <= box_h*src_w)` (1 condition; the size-clamp
 * branch in apps/shared_libs/reflow/src/reflow_image.c@internal_fit_box). A
 * single-condition decision needs 2 vectors (each value, each outcome). The
 * mirror has operand-identical int64 semantics; real behaviour is also pinned
 * below via decode out_w/out_h for a square source.
 *
 * Vectors:
 *  - V1: box 2x4, src 2x2 -> 2*2 <= 4*2 (4<=8) T -> width-limited.
 *  - V2: box 4x2, src 2x2 -> 4*2 <= 2*2 (8<=4) F -> height-limited.
 * @brief Verify fit box branch mcdc behavior against the reflow contract.
 * @details Exercises the fit box branch mcdc path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_fit_box_branch_mcdc(void)
{
  TEST_BEGIN("internal_fit_box branch MC/DC: box_w*src_h <= box_h*src_w");
  TEST_ASSERT_EQ(1, internal_mirror_fit_width_limited(2, 2, 4, 2)); /* V1: width-limited  */
  TEST_ASSERT_EQ(0, internal_mirror_fit_width_limited(4, 2, 2, 2)); /* V2: height-limited */

  /* Real path: a square 2x2 into a wide and a tall box both fit to 2x2. */
  static uint8_t  s_scratch[k_t_scratch_kib * k_t_kib];
  ra8_img_arena_t arena = {.base = s_scratch, .cap = sizeof s_scratch, .offset = 0U, .live = 0U};
  static uint8_t  s_fb[8 * 8 * 3];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_init(s_fb, 8, 8, k_ra8_gfx_format_rgb888));
  int32_t ow = 0;
  int32_t oh = 0;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_img_decode_blit(&arena, s_png_2x2, sizeof s_png_2x2, 0, 0, 2, 8, &ow, &oh));
  TEST_ASSERT_EQ(2, ow); /* width-limited: capped at box_w */
  TEST_ASSERT_EQ(2, oh);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_img_decode_blit(&arena, s_png_2x2, sizeof s_png_2x2, 0, 0, 8, 2, &ow, &oh));
  TEST_ASSERT_EQ(2, ow); /* height-limited: capped at box_h */
  TEST_ASSERT_EQ(2, oh);
  TEST_END("internal_fit_box branch MC/DC: box_w*src_h <= box_h*src_w");
}

/** @brief Mirror of internal_decode_fail's OOM classify (2-condition AND).
 * @details Exercises the mirror decode is oom path and preserves each documented result and bound.
 * @param[in] reason Caller-supplied reason value used by the scenario.
 * @return The scalar result computed for the requested reflow scenario.
 * @retval 0 The helper produced its zero-valued result.
 * @retval nonzero The helper produced a nonzero result.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t internal_mirror_decode_is_oom(const char* reason)
{
  if ((reason != NULL) && (strstr(reason, "outofmem") != NULL)) {
    return 1U; /* -> k_ra8_err_no_mem */
  }
  return 0U; /* -> k_ra8_err_not_supported */
}

/**
 * @test internal_test_decode_fail_classify_mcdc
 *
 * @par MC/DC:
 * Decision: `if (reason != NULL && strstr(reason,"outofmem") != NULL)`
 * (2 conditions, AND; apps/shared_libs/reflow/src/reflow_image.c@internal_decode_fail).
 *
 * Vectors (N+1 = 3 for N=2):
 *  - V1: reason="outofmem"      -> C1 T, C2 T -> decision T (no_mem).
 *  - V2: reason=NULL            -> C1 F shorts -> decision F (not_supported).
 *  - V3: reason="bad png sig"   -> C1 T, C2 F -> decision F (not_supported).
 * V1 vs V2 vary C1 (C2 held T); V1 vs V3 vary C2 (C1 held T).
 * @brief Verify decode fail classify mcdc behavior against the reflow contract.
 * @details Exercises the decode fail classify mcdc path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_decode_fail_classify_mcdc(void)
{
  TEST_BEGIN("internal_decode_fail MC/DC: reason!=NULL && strstr(outofmem)");
  TEST_ASSERT_EQ(1, internal_mirror_decode_is_oom("outofmem"));
  TEST_ASSERT_EQ(0, internal_mirror_decode_is_oom(NULL));
  TEST_ASSERT_EQ(0, internal_mirror_decode_is_oom("bad png sig"));
  TEST_END("internal_decode_fail MC/DC: reason!=NULL && strstr(outofmem)");
}

/**
 * @test internal_test_arena_drained_and_no_mem
 * @brief Arena drains on the decode-failure path; a tiny arena yields no_mem.
 *
 * @par MC/DC:
 * The test's own assertion is a 2-condition OR tolerating either failure code:
 * `(e == k_ra8_err_no_mem) || (e == k_ra8_err_not_supported)`. Which arm holds is
 * decided by the production AND in `internal_decode_fail`,
 * `(reason != nullptr) && (strstr(reason, "outofmem") != nullptr)`, driven here
 * through the real API:
 * - junk bytes + large arena -> reason set, no "outofmem" (C2 F) -> F ->
 *   k_ra8_err_not_supported (asserted exactly).
 * - valid PNG + 48-byte arena -> reason "outofmem" (C2 T) -> T -> k_ra8_err_no_mem
 *   (the tolerant OR's first arm; the tiny arena is deterministic on this host).
 * The two vectors vary C2 and flip the outcome; C1 (`reason != nullptr`) is
 * MC/DC-deactivated (DO-178C 6.4.4.3) -- stb always sets a reason on failure, per
 * the production `mcdc-deactivated` note. The dedicated block for this production
 * decision is internal_test_decode_fail_real_paths_mcdc.
 * @details Exercises the arena drained and no mem path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_arena_drained_and_no_mem(void)
{
  TEST_BEGIN("ra8_img_decode_blit: arena drain on failure + s_tiny-arena no_mem");
  static uint8_t  s_scratch[k_t_scratch_kib * k_t_kib];
  ra8_img_arena_t arena = {.base = s_scratch, .cap = sizeof s_scratch, .offset = 0U, .live = 0U};
  static uint8_t  s_fb[4 * 4 * 3];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_init(s_fb, 4, 4, k_ra8_gfx_format_rgb888));

  /* Undecodable bytes -> not_supported, arena still drained. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_img_decode_blit(&arena, s_junk, sizeof s_junk, 0, 0, 4, 4, NULL, NULL));
  TEST_ASSERT_EQ(0, arena.offset);
  TEST_ASSERT_EQ(0, arena.live);

  /* Arena far too small for the decode -> no_mem (or not_supported), drained. */
  static uint8_t  s_tiny[k_t_tiny_dst_cap];
  ra8_img_arena_t small = {.base = s_tiny, .cap = sizeof s_tiny, .offset = 0U, .live = 0U};
  const ra8_err_t e =
    ra8_img_decode_blit(&small, s_png_2x2, sizeof s_png_2x2, 0, 0, 4, 4, NULL, NULL);
  TEST_ASSERT((e == k_ra8_err_no_mem) || (e == k_ra8_err_not_supported));
  TEST_ASSERT_EQ(0, small.offset);
  TEST_ASSERT_EQ(0, small.live);
  TEST_END("ra8_img_decode_blit: arena drain on failure + s_tiny-arena no_mem");
}

/**
 * @test internal_test_decode_fail_real_paths_mcdc
 *
 * @par MC/DC:
 * Two production-source decisions are driven here through the real
 * ra8_img_decode_blit() API (no mirror), by their returned ra8_err_t:
 *
 * (A) internal_decode_fail(): `if (reason != NULL && strstr(reason,"outofmem"))`
 *     (apps/shared_libs/reflow/src/reflow_image.c@internal_decode_fail; 2 cond, AND).
 *      - The OOM arm (C1 T, C2 T -> decision T -> k_ra8_err_no_mem) is exercised
 *        by handing a *valid* 2x2 PNG to an arena far too small to hold even the
 *        first stb allocation: stb's allocator returns NULL, stb sets its
 *        failure reason to the tag "outofmem", and internal_decode_fail()
 *        classifies it as no_mem. This is the previously-uncovered true arm.
 *      - The non-OOM arm (decision F -> k_ra8_err_not_supported) is exercised by
 *        handing undecodable bytes to a large arena (reason != "outofmem").
 *
 * (B) ra8_img_decode_blit()'s decode-failure guard
 *     `if ((pixels == NULL) || (sx <= 0) || (sy <= 0))` (3 cond, OR). The
 *     reachable condition C1 (pixels == NULL) is driven true by *both* failure
 *     routes below, taking the failure block to its two distinct error returns.
 *     C2/C3 (sx<=0 / sy<=0) are defensive: stb never returns a non-NULL buffer
 *     with a non-positive dimension, so they are not reachable via the API.
 *
 * Vectors:
 *  - V1: valid PNG + 48-byte arena  -> A:C1 T,C2 T -> no_mem.
 *  - V2: junk bytes  + 64 KiB arena -> A:C2 F      -> not_supported.
 * Both drive B:C1 (pixels == NULL) true; A's C1 stays true in both (a reason
 * string is always set on a stb failure), so V1 vs V2 vary A's C2 and flip the
 * no_mem-vs-not_supported outcome -- C2 independently affects the result.
 * @brief Verify decode fail real paths mcdc behavior against the reflow contract.
 * @details Exercises the decode fail real paths mcdc path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_decode_fail_real_paths_mcdc(void)
{
  TEST_BEGIN("ra8_img_decode_blit decode-fail MC/DC: no_mem vs not_supported");
  static uint8_t s_fb[4 * 4 * 3];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_init(s_fb, 4, 4, k_ra8_gfx_format_rgb888));

  /* V1: a valid PNG into an arena too small for the first stb allocation
     deterministically fails with the "outofmem" tag -> no_mem (true arm). */
  static uint8_t  s_tiny[k_t_tiny_dst_cap];
  ra8_img_arena_t small = {.base = s_tiny, .cap = sizeof s_tiny, .offset = 0U, .live = 0U};
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_img_decode_blit(&small, s_png_2x2, sizeof s_png_2x2, 0, 0, 4, 4, NULL, NULL));
  TEST_ASSERT_EQ(0, small.offset);
  TEST_ASSERT_EQ(0, small.live);

  /* V2: undecodable bytes into a large arena -> reason != "outofmem"
     -> not_supported (false arm). */
  static uint8_t  s_scratch[k_t_scratch_kib * k_t_kib];
  ra8_img_arena_t big = {.base = s_scratch, .cap = sizeof s_scratch, .offset = 0U, .live = 0U};
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_img_decode_blit(&big, s_junk, sizeof s_junk, 0, 0, 4, 4, NULL, NULL));
  TEST_ASSERT_EQ(0, big.offset);
  TEST_ASSERT_EQ(0, big.live);
  TEST_END("ra8_img_decode_blit decode-fail MC/DC: no_mem vs not_supported");
}

/**
 * @test internal_test_probe_size_webp
 * @brief ra8_img_probe_size reads an 8x8 WebP through the libwebp arm (#637).
 *
 * @par MC/DC:
 * Signature test `(len >= 12) && (RIFF match) && (WEBP match)`: this vector
 * passes all three, and internal_test_webp_signature_falls_through drives the
 * false arms of the two tag conditions, so each independently decides whether
 * the WebP facade or stb_image sees the bytes.
 *
 * @brief Verify the WebP probe arm reports the container's canvas size.
 * @details Probes the committed lossless fixture and asserts the declared 8x8 canvas.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_probe_size_webp(void)
{
  TEST_BEGIN("ra8_img_probe_size reads a WebP header");
  int32_t w = 0;
  int32_t h = 0;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_img_probe_size(s_webp_lossless_8x8, sizeof s_webp_lossless_8x8, &w, &h));
  TEST_ASSERT_EQ((int32_t)k_t_webp_dim, w);
  TEST_ASSERT_EQ((int32_t)k_t_webp_dim, h);
  TEST_END("ra8_img_probe_size reads a WebP header");
}

/**
 * @test internal_test_decode_blit_webp_pixels
 * @brief A lossless WebP decodes and blits 1:1, bit-exact, and drains the arena.
 *
 * @par MC/DC:
 * (no compound decision is uniquely proven here -- it blits the golden 8x8
 * lossless fixture through the WebP arm and asserts every pixel plus a fully
 * drained arena)
 *
 * @brief Verify the WebP blit arm draws the fixture's documented pattern.
 * @details Blits the fixture at 1:1 into an 8x8 framebuffer and compares every pixel.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_decode_blit_webp_pixels(void)
{
  TEST_BEGIN("ra8_img_decode_blit blits a WebP bit-exact");
  static uint8_t s_fb[k_t_webp_dim * k_t_webp_dim * 3U];
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_init(s_fb,
                              (int32_t)k_t_webp_dim,
                              (int32_t)k_t_webp_dim,
                              k_ra8_gfx_format_rgb888));

  static uint8_t  s_scratch[k_t_scratch_kib * k_t_kib];
  ra8_img_arena_t arena = {.base = s_scratch, .cap = sizeof s_scratch, .offset = 0U, .live = 0U};
  int32_t         out_w = 0;
  int32_t         out_h = 0;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_img_decode_blit(&arena,
                                     s_webp_lossless_8x8,
                                     sizeof s_webp_lossless_8x8,
                                     0,
                                     0,
                                     (int32_t)k_t_webp_dim,
                                     (int32_t)k_t_webp_dim,
                                     &out_w,
                                     &out_h));
  TEST_ASSERT_EQ((int32_t)k_t_webp_dim, out_w);
  TEST_ASSERT_EQ((int32_t)k_t_webp_dim, out_h);

  /* Lossless: every RGB channel must match the fixture's source pattern, so an
     alpha byte read as colour or a 4-byte stride read as 3 would both fail. */
  for (int32_t y = 0; y < (int32_t)k_t_webp_dim; y++) {
    for (int32_t x = 0; x < (int32_t)k_t_webp_dim; x++) {
      const uint32_t er = ((uint32_t)x * (uint32_t)k_t_webp_r_step) & (uint32_t)k_t_webp_byte_max;
      const uint32_t eg = ((uint32_t)y * (uint32_t)k_t_webp_g_step) & (uint32_t)k_t_webp_byte_max;
      const uint32_t eb = (((uint32_t)x + (uint32_t)y) * (uint32_t)k_t_webp_b_step) &
                          (uint32_t)k_t_webp_byte_max;
      const uint32_t want = (er << 16) | (eg << 8) | eb;
      TEST_ASSERT_EQ(want, internal_fb_px(s_fb, (int32_t)k_t_webp_dim, x, y));
    }
  }

  /* NASA P10 R3: the shared arena backed both the canvas and libwebp's own
     scratch, and must be fully drained again on return. */
  TEST_ASSERT_EQ(0, arena.offset);
  TEST_ASSERT_EQ(0, arena.live);
  TEST_END("ra8_img_decode_blit blits a WebP bit-exact");
}

/**
 * @test internal_test_webp_signature_falls_through
 * @brief Bytes that are not a WebP container never reach the WebP facade.
 *
 * @par MC/DC:
 * Signature test `(len >= 12) && (RIFF match) && (WEBP match)`, false arms:
 *  - V1: 8 junk bytes            -> C1 false (too short to hold both tags).
 *  - V2: 16 bytes, `RIFF`+`WAVE` -> C1 true, C2 true, C3 false.
 * Both must be rejected by stb_image as unsupported, which is the observable
 * difference from the WebP arm (whose header rejection logs a WebP reason and
 * would return the same code for a different reason). V1 and V2 together with
 * the two passing vectors above give each condition independent influence.
 *
 * @brief Verify non-WebP bytes are not mis-routed into the WebP decoder.
 * @details Drives a too-short buffer and a RIFF container with a non-WEBP form tag.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_webp_signature_falls_through(void)
{
  TEST_BEGIN("WebP signature: non-WebP bytes fall through to stb_image");
  int32_t w = 0;
  int32_t h = 0;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_img_probe_size(s_junk, sizeof s_junk, &w, &h));
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_img_probe_size(s_riff_not_webp, sizeof s_riff_not_webp, &w, &h));

  static uint8_t s_fb[4 * 4 * 3];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_init(s_fb, 4, 4, k_ra8_gfx_format_rgb888));
  static uint8_t  s_scratch[k_t_scratch_kib * k_t_kib];
  ra8_img_arena_t arena = {.base = s_scratch, .cap = sizeof s_scratch, .offset = 0U, .live = 0U};
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_img_decode_blit(&arena,
                                     s_riff_not_webp,
                                     sizeof s_riff_not_webp,
                                     0,
                                     0,
                                     4,
                                     4,
                                     NULL,
                                     NULL));
  TEST_ASSERT_EQ(0, arena.offset);
  TEST_ASSERT_EQ(0, arena.live);
  TEST_END("WebP signature: non-WebP bytes fall through to stb_image");
}

/**
 * @test internal_test_webp_canvas_no_mem
 * @brief An arena too small for the decoded WebP canvas reports no_mem, drained.
 *
 * @par MC/DC:
 * (no compound decision is uniquely proven here -- it drives the canvas
 * allocation's single-condition failure arm)
 *
 * @brief Verify the WebP arm reports a canvas that cannot fit the arena.
 * @details Offers an arena far below the 8x8 RGBA canvas and asserts no_mem plus a drain.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_webp_canvas_no_mem(void)
{
  TEST_BEGIN("ra8_img_decode_blit: WebP canvas larger than the arena -> no_mem");
  static uint8_t s_fb[k_t_webp_dim * k_t_webp_dim * 3U];
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_init(s_fb,
                              (int32_t)k_t_webp_dim,
                              (int32_t)k_t_webp_dim,
                              k_ra8_gfx_format_rgb888));

  /* The 8x8 RGBA canvas alone needs 256 bytes; this arena cannot hold it. */
  static uint8_t  s_tiny[k_t_tiny_dst_cap];
  ra8_img_arena_t arena = {.base = s_tiny, .cap = sizeof s_tiny, .offset = 0U, .live = 0U};
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_img_decode_blit(&arena,
                                     s_webp_lossless_8x8,
                                     sizeof s_webp_lossless_8x8,
                                     0,
                                     0,
                                     (int32_t)k_t_webp_dim,
                                     (int32_t)k_t_webp_dim,
                                     NULL,
                                     NULL));
  TEST_ASSERT_EQ(0, arena.offset);
  TEST_ASSERT_EQ(0, arena.live);
  TEST_END("ra8_img_decode_blit: WebP canvas larger than the arena -> no_mem");
}

/**
 * @brief Test entry point.
 * @return 0 on success; unity macros exit(1) on the first failure.
 */
int main(void)
{
  internal_test_probe_size_png();
  internal_test_decode_blit_pixels();
  internal_test_decode_blit_precondition_mcdc();
  internal_test_fit_box_branch_mcdc();
  internal_test_decode_fail_classify_mcdc();
  internal_test_decode_fail_real_paths_mcdc();
  internal_test_arena_drained_and_no_mem();
  internal_test_probe_size_webp();
  internal_test_decode_blit_webp_pixels();
  internal_test_webp_signature_falls_through();
  internal_test_webp_canvas_no_mem();
  return 0;
}
