/**
 * @file test_ra8_imgdec_mux_probe.c
 * @brief Host tests for asking a set of backends about an image (RA8FW-308).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_imgdec.h"
#include "ra8_imgdec_backend.h"
#include "ra8_imgdec_mux.h"
#include "unity_minimal.h"

/** @brief Fixture sizes and offsets (no magic numbers). */
enum : uint32_t {
  k_png_bytes    = 33,   /* A PNG signature plus a whole IHDR chunk.       */
  k_png_ihdr_w   = 16,   /* Big-endian width inside the IHDR payload.      */
  k_png_ihdr_h   = 20,   /* Big-endian height inside the IHDR payload.     */
  k_png_truncate = 14,   /* Short of the IHDR payload, past the sniff.     */
  k_gif_bytes    = 13,   /* Header plus the logical screen descriptor.     */
  k_gif_w        = 6,    /* Little-endian width in the screen descriptor.  */
  k_gif_h        = 8,    /* Little-endian height in the screen descriptor. */
  k_img_w        = 640,  /* Declared width every fixture image carries.    */
  k_img_h        = 480,  /* Declared height every fixture image carries.   */
  k_big_dim      = 4096, /* dim_max wide enough for the fixture image.     */
  k_small_dim    = 64,   /* dim_max too small for the fixture image.       */
};

/**
 * @struct fake_backend_t
 * @brief A backend that advertises a matrix and never decodes.
 */
typedef struct {
  uint32_t formats; /**< Formats it claims to open.        */
  uint32_t pixels;  /**< Pixel layouts it claims to write. */
  uint32_t dim_max; /**< Largest dimension it accepts.     */
} fake_backend_t;

RA8_INTERNAL static ra8_err_t internal_fake_caps(void* ctx, ra8_imgdec_caps_t* out) {
  const fake_backend_t* const be = (const fake_backend_t*)ctx;
  out->formats                   = be->formats;
  out->pixels                    = be->pixels;
  out->scratch_bytes             = 0U;
  out->scratch_align             = 0U;
  out->dim_max                   = be->dim_max;
  out->streams                   = false;
  return k_ra8_ok;
}

RA8_INTERNAL static ra8_err_t internal_fake_decode(void*                   ctx,
                                                   const ra8_imgdec_req_t* req,
                                                   ra8_imgdec_image_t*     out) {
  (void)ctx;
  (void)req;
  (void)out;
  return k_ra8_err_not_supported; /* a probe must never reach a decode */
}

/** @brief One shared vtable; the fakes differ only in their context. */
static const ra8_imgdec_iface_t s_fake_iface = {
    .get_caps = internal_fake_caps,
    .decode   = internal_fake_decode,
};

static uint8_t g_png[k_png_bytes];
static uint8_t g_gif[k_gif_bytes];

/**
 * @brief Lay a PNG signature and IHDR chunk carrying @p w by @p h into g_png.
 *
 * @param[in] w Declared width in pixels.
 * @param[in] h Declared height in pixels.
 *
 * @return None.
 */
RA8_INTERNAL static void internal_make_png(uint32_t w, uint32_t h) {
  static const uint8_t k_sig[8] = {0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU};
  static const uint8_t k_len[4] = {0x00U, 0x00U, 0x00U, 0x0DU};
  static const uint8_t k_typ[4] = {0x49U, 0x48U, 0x44U, 0x52U};

  (void)memset(g_png, 0, sizeof(g_png));
  (void)memcpy(&g_png[0], k_sig, sizeof(k_sig));
  (void)memcpy(&g_png[8], k_len, sizeof(k_len));
  (void)memcpy(&g_png[12], k_typ, sizeof(k_typ));

  g_png[k_png_ihdr_w + 0U] = (uint8_t)((w >> 24U) & 0xFFU);
  g_png[k_png_ihdr_w + 1U] = (uint8_t)((w >> 16U) & 0xFFU);
  g_png[k_png_ihdr_w + 2U] = (uint8_t)((w >> 8U) & 0xFFU);
  g_png[k_png_ihdr_w + 3U] = (uint8_t)(w & 0xFFU);
  g_png[k_png_ihdr_h + 0U] = (uint8_t)((h >> 24U) & 0xFFU);
  g_png[k_png_ihdr_h + 1U] = (uint8_t)((h >> 16U) & 0xFFU);
  g_png[k_png_ihdr_h + 2U] = (uint8_t)((h >> 8U) & 0xFFU);
  g_png[k_png_ihdr_h + 3U] = (uint8_t)(h & 0xFFU);
}

/**
 * @brief Lay a GIF89a header carrying @p w by @p h into g_gif.
 *
 * @param[in] w Declared width in pixels.
 * @param[in] h Declared height in pixels.
 *
 * @return None.
 */
RA8_INTERNAL static void internal_make_gif(uint32_t w, uint32_t h) {
  static const uint8_t k_hdr[6] = {0x47U, 0x49U, 0x46U, 0x38U, 0x39U, 0x61U};

  (void)memset(g_gif, 0, sizeof(g_gif));
  (void)memcpy(&g_gif[0], k_hdr, sizeof(k_hdr));

  g_gif[k_gif_w + 0U] = (uint8_t)(w & 0xFFU);
  g_gif[k_gif_w + 1U] = (uint8_t)((w >> 8U) & 0xFFU);
  g_gif[k_gif_h + 0U] = (uint8_t)(h & 0xFFU);
  g_gif[k_gif_h + 1U] = (uint8_t)((h >> 8U) & 0xFFU);
}

/**
 * @brief Build a mux holding the given fakes, in order.
 *
 * @param[out]    mux Mux to initialise and fill.
 * @param[in,out] a   First member, or NULL.
 * @param[in,out] b   Second member, or NULL.
 *
 * @return None.
 */
RA8_INTERNAL static void internal_build(ra8_imgdec_mux_t* mux,
                                        fake_backend_t*   a,
                                        fake_backend_t*   b) {
  *mux = (ra8_imgdec_mux_t){};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_init(mux));
  if (a != nullptr) {
    const ra8_imgdec_t dec = {.iface = &s_fake_iface, .ctx = a};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_add(mux, &dec));
  }
  if (b != nullptr) {
    const ra8_imgdec_t dec = {.iface = &s_fake_iface, .ctx = b};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_add(mux, &dec));
  }
}

RA8_INTERNAL static void internal_test_answers_geometry_and_member(void) {
  TEST_BEGIN("a probe names the container, its size, and who would decode it");

  internal_make_png(k_img_w, k_img_h);

  fake_backend_t gif_only = {.formats = (uint32_t)k_ra8_imgdec_format_gif,
                             .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgba8888,
                             .dim_max = (uint32_t)k_big_dim};
  fake_backend_t png_only = {.formats = (uint32_t)k_ra8_imgdec_format_png,
                             .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgba8888,
                             .dim_max = (uint32_t)k_big_dim};

  ra8_imgdec_mux_t mux = {};
  internal_build(&mux, &gif_only, &png_only);

  ra8_imgdec_geom_t         geom   = {};
  const ra8_imgdec_t* member = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_imgdec_mux_probe(&mux,
                                      g_png,
                                      (uint32_t)sizeof(g_png),
                                      k_ra8_imgdec_pixel_rgba8888,
                                      &geom,
                                      &member));

  TEST_ASSERT(geom.format == k_ra8_imgdec_format_png);
  TEST_ASSERT(geom.width_px == (uint32_t)k_img_w);
  TEST_ASSERT(geom.height_px == (uint32_t)k_img_h);
  TEST_ASSERT_NOT_NULL(member);
  TEST_ASSERT(member->ctx == (void*)&png_only); /* skipped the GIF-only member */

  TEST_END("a probe names the container, its size, and who would decode it");
}

RA8_INTERNAL static void internal_test_member_is_optional(void) {
  TEST_BEGIN("a caller wanting only the geometry passes no member out");

  internal_make_gif(k_img_w, k_img_h);

  fake_backend_t gif = {.formats = (uint32_t)k_ra8_imgdec_format_gif,
                        .pixels  = (uint32_t)k_ra8_imgdec_pixel_grey8,
                        .dim_max = (uint32_t)k_big_dim};

  ra8_imgdec_mux_t mux = {};
  internal_build(&mux, &gif, nullptr);

  ra8_imgdec_geom_t geom = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_imgdec_mux_probe(&mux,
                                      g_gif,
                                      (uint32_t)sizeof(g_gif),
                                      k_ra8_imgdec_pixel_grey8,
                                      &geom,
                                      nullptr));
  TEST_ASSERT(geom.format == k_ra8_imgdec_format_gif);
  TEST_ASSERT(geom.width_px == (uint32_t)k_img_w);
  TEST_ASSERT(geom.height_px == (uint32_t)k_img_h);

  TEST_END("a caller wanting only the geometry passes no member out");
}

RA8_INTERNAL static void internal_test_routing_is_per_pair(void) {
  TEST_BEGIN("a member opening the container but not the layout does not serve it");

  internal_make_png(k_img_w, k_img_h);

  fake_backend_t png_grey = {.formats = (uint32_t)k_ra8_imgdec_format_png,
                             .pixels  = (uint32_t)k_ra8_imgdec_pixel_grey8,
                             .dim_max = (uint32_t)k_big_dim};

  ra8_imgdec_mux_t mux = {};
  internal_build(&mux, &png_grey, nullptr);

  ra8_imgdec_geom_t         geom   = {};
  const ra8_imgdec_t* member = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_imgdec_mux_probe(&mux,
                                      g_png,
                                      (uint32_t)sizeof(g_png),
                                      k_ra8_imgdec_pixel_rgba8888,
                                      &geom,
                                      &member));
  TEST_ASSERT(geom.format == k_ra8_imgdec_format_none);
  TEST_ASSERT(geom.width_px == 0U);
  TEST_ASSERT_NULL(member);

  /* The same set answers the pair it does cover. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_imgdec_mux_probe(&mux,
                                      g_png,
                                      (uint32_t)sizeof(g_png),
                                      k_ra8_imgdec_pixel_grey8,
                                      &geom,
                                      &member));
  TEST_ASSERT(member->ctx == (void*)&png_grey);

  TEST_END("a member opening the container but not the layout does not serve it");
}

RA8_INTERNAL static void internal_test_dim_max_does_not_fall_through(void) {
  TEST_BEGIN("a routed member too small for the image refuses, it does not fall through");

  internal_make_png(k_img_w, k_img_h);

  /* Both cover PNG into RGBA; only the second is big enough for the image. */
  fake_backend_t small = {.formats = (uint32_t)k_ra8_imgdec_format_png,
                          .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgba8888,
                          .dim_max = (uint32_t)k_small_dim};
  fake_backend_t big   = {.formats = (uint32_t)k_ra8_imgdec_format_png,
                          .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgba8888,
                          .dim_max = (uint32_t)k_big_dim};

  ra8_imgdec_mux_t mux = {};
  internal_build(&mux, &small, &big);

  ra8_imgdec_geom_t         geom   = {};
  const ra8_imgdec_t* member = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_imgdec_mux_probe(&mux,
                                      g_png,
                                      (uint32_t)sizeof(g_png),
                                      k_ra8_imgdec_pixel_rgba8888,
                                      &geom,
                                      &member));
  TEST_ASSERT_NULL(member);

  /* Reverse the priority order and the big member is routed, so it answers. */
  internal_build(&mux, &big, &small);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_imgdec_mux_probe(&mux,
                                      g_png,
                                      (uint32_t)sizeof(g_png),
                                      k_ra8_imgdec_pixel_rgba8888,
                                      &geom,
                                      &member));
  TEST_ASSERT(member->ctx == (void*)&big);

  TEST_END("a routed member too small for the image refuses, it does not fall through");
}

RA8_INTERNAL static void internal_test_unrecognised_bytes(void) {
  TEST_BEGIN("bytes carrying no signature are refused the way a decode refuses them");

  static const uint8_t k_junk[16] = {
      0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U,
      0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU,
  };

  fake_backend_t any = {.formats = (uint32_t)k_ra8_imgdec_format_mask,
                        .pixels  = (uint32_t)k_ra8_imgdec_pixel_mask,
                        .dim_max = (uint32_t)k_big_dim};

  ra8_imgdec_mux_t mux = {};
  internal_build(&mux, &any, nullptr);

  ra8_imgdec_geom_t         geom   = {};
  const ra8_imgdec_t* member = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_imgdec_mux_probe(&mux,
                                      k_junk,
                                      (uint32_t)sizeof(k_junk),
                                      k_ra8_imgdec_pixel_rgba8888,
                                      &geom,
                                      &member));
  TEST_ASSERT(geom.format == k_ra8_imgdec_format_none);
  TEST_ASSERT_NULL(member);

  TEST_END("bytes carrying no signature are refused the way a decode refuses them");
}

RA8_INTERNAL static void internal_test_truncated_header(void) {
  TEST_BEGIN("a signature the geometry reader cannot finish is not an ok probe");

  internal_make_png(k_img_w, k_img_h);

  fake_backend_t any = {.formats = (uint32_t)k_ra8_imgdec_format_mask,
                        .pixels  = (uint32_t)k_ra8_imgdec_pixel_mask,
                        .dim_max = (uint32_t)k_big_dim};

  ra8_imgdec_mux_t mux = {};
  internal_build(&mux, &any, nullptr);

  ra8_imgdec_geom_t         geom   = {};
  const ra8_imgdec_t* member = nullptr;
  /* Past the 8-byte signature the sniff reads, short of the IHDR payload. */
  const ra8_err_t err = ra8_imgdec_mux_probe(&mux,
                                             g_png,
                                             (uint32_t)k_png_truncate,
                                             k_ra8_imgdec_pixel_rgba8888,
                                             &geom,
                                             &member);
  TEST_ASSERT(err != k_ra8_ok);
  TEST_ASSERT(geom.format == k_ra8_imgdec_format_none);
  TEST_ASSERT(geom.width_px == 0U);
  TEST_ASSERT_NULL(member);

  TEST_END("a signature the geometry reader cannot finish is not an ok probe");
}

RA8_INTERNAL static void internal_test_guards(void) {
  TEST_BEGIN("every guard refuses before a member is consulted");

  internal_make_png(k_img_w, k_img_h);

  fake_backend_t any = {.formats = (uint32_t)k_ra8_imgdec_format_mask,
                        .pixels  = (uint32_t)k_ra8_imgdec_pixel_mask,
                        .dim_max = (uint32_t)k_big_dim};

  ra8_imgdec_mux_t mux = {};
  internal_build(&mux, &any, nullptr);

  ra8_imgdec_geom_t         geom   = {};
  const ra8_imgdec_t* member = nullptr;
  const uint32_t            len    = (uint32_t)sizeof(g_png);

  TEST_ASSERT_EQ(
      k_ra8_err_null_ptr,
      ra8_imgdec_mux_probe(&mux, nullptr, len, k_ra8_imgdec_pixel_rgba8888, &geom, &member));
  TEST_ASSERT_EQ(
      k_ra8_err_null_ptr,
      ra8_imgdec_mux_probe(&mux, g_png, len, k_ra8_imgdec_pixel_rgba8888, nullptr, &member));
  TEST_ASSERT_EQ(
      k_ra8_err_null_ptr,
      ra8_imgdec_mux_probe(nullptr, g_png, len, k_ra8_imgdec_pixel_rgba8888, &geom, &member));

  /* A layout naming no bit, and one naming two, are both malformed. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_imgdec_mux_probe(&mux, g_png, len, k_ra8_imgdec_pixel_none, &geom, &member));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_imgdec_mux_probe(&mux,
                                      g_png,
                                      len,
                                      (ra8_imgdec_pixel_t)((uint32_t)k_ra8_imgdec_pixel_grey8 |
                                                           (uint32_t)k_ra8_imgdec_pixel_rgb888),
                                      &geom,
                                      &member));

  TEST_ASSERT_EQ(
      k_ra8_err_invalid_size,
      ra8_imgdec_mux_probe(&mux, g_png, 0U, k_ra8_imgdec_pixel_rgba8888, &geom, &member));

  ra8_imgdec_mux_t empty = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_init(&empty));
  TEST_ASSERT_EQ(
      k_ra8_err_not_initialized,
      ra8_imgdec_mux_probe(&empty, g_png, len, k_ra8_imgdec_pixel_rgba8888, &geom, &member));

  TEST_ASSERT(geom.format == k_ra8_imgdec_format_none);
  TEST_ASSERT_NULL(member);

  TEST_END("every guard refuses before a member is consulted");
}

RA8_INTERNAL static void internal_test_probe_agrees_with_single_backend(void) {
  TEST_BEGIN("a one-member set answers exactly what probing that backend answers");

  internal_make_png(k_img_w, k_img_h);

  fake_backend_t png = {.formats = (uint32_t)k_ra8_imgdec_format_png,
                        .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgba8888,
                        .dim_max = (uint32_t)k_big_dim};

  ra8_imgdec_mux_t mux = {};
  internal_build(&mux, &png, nullptr);

  const ra8_imgdec_t dec       = {.iface = &s_fake_iface, .ctx = &png};
  ra8_imgdec_geom_t  direct    = {};
  ra8_imgdec_geom_t  through   = {};
  const uint32_t     len       = (uint32_t)sizeof(g_png);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_probe(&dec, g_png, len, &direct));
  TEST_ASSERT_EQ(
      k_ra8_ok,
      ra8_imgdec_mux_probe(&mux, g_png, len, k_ra8_imgdec_pixel_rgba8888, &through, nullptr));

  TEST_ASSERT(through.format == direct.format);
  TEST_ASSERT(through.width_px == direct.width_px);
  TEST_ASSERT(through.height_px == direct.height_px);

  TEST_END("a one-member set answers exactly what probing that backend answers");
}

int main(void) {
  internal_test_answers_geometry_and_member();
  internal_test_member_is_optional();
  internal_test_routing_is_per_pair();
  internal_test_dim_max_does_not_fall_through();
  internal_test_unrecognised_bytes();
  internal_test_truncated_header();
  internal_test_guards();
  internal_test_probe_agrees_with_single_backend();
  return 0;
}
