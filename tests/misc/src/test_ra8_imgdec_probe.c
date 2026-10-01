/**
 * @file test_ra8_imgdec_probe.c
 * @brief Host tests for asking a bound backend about an image before decoding it (RA8FW-308).
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
#include "unity_minimal.h"

/** @brief Fixture sizes (no magic numbers). */
enum : uint32_t {
  k_src_bytes = 32,   /**< Fixture length.     */
  k_wide_dim  = 4096, /**< A roomy dim_max.    */
  k_tight_dim = 32,   /**< A narrow dim_max.   */
  k_png_w     = 64,   /**< Fixture width.      */
  k_png_h     = 48,   /**< Fixture height.     */
  k_short_len = 8,    /**< Signature, no IHDR. */
};

/**
 * @struct fake_backend_t
 * @brief A backend that advertises a set and counts every hook it is asked for.
 */
typedef struct {
  uint32_t formats;      /**< Formats it opens.   */
  uint32_t pixels;       /**< Layouts it writes.  */
  uint32_t dim_max;      /**< Size limit.         */
  uint32_t caps_calls;   /**< Caps queries seen.  */
  uint32_t decode_calls; /**< Decodes dispatched. */
} fake_backend_t;

RA8_INTERNAL static ra8_err_t internal_fake_caps(void* ctx, ra8_imgdec_caps_t* out) {
  fake_backend_t* const be = (fake_backend_t*)ctx;
  be->caps_calls += 1U;
  out->formats       = be->formats;
  out->pixels        = be->pixels;
  out->scratch_bytes = 0U;
  out->scratch_align = 0U;
  out->dim_max       = be->dim_max;
  out->streams       = false;
  return k_ra8_ok;
}

RA8_INTERNAL static ra8_err_t internal_fake_decode(void*                   ctx,
                                                   const ra8_imgdec_req_t* req,
                                                   ra8_imgdec_image_t*     out) {
  fake_backend_t* const be = (fake_backend_t*)ctx;
  be->decode_calls += 1U;
  (void)req;
  (void)out;
  return k_ra8_err_not_supported;
}

/** @brief One shared vtable; the fakes differ only in their context. */
static const ra8_imgdec_iface_t s_fake_iface = {
    .get_caps = internal_fake_caps,
    .decode   = internal_fake_decode,
};

/** @brief A backend whose capability record is unusable (no formats). */
RA8_INTERNAL static ra8_err_t internal_broken_caps(void* ctx, ra8_imgdec_caps_t* out) {
  (void)ctx;
  out->formats       = 0U;
  out->pixels        = (uint32_t)k_ra8_imgdec_pixel_rgba8888;
  out->scratch_bytes = 0U;
  out->scratch_align = 0U;
  out->dim_max       = (uint32_t)k_wide_dim;
  out->streams       = false;
  return k_ra8_ok;
}

static const ra8_imgdec_iface_t s_broken_iface = {
    .get_caps = internal_broken_caps,
    .decode   = internal_fake_decode,
};

/** @brief PNG signature then an IHDR declaring 64x48, 8-bit RGB. */
static const uint8_t s_png[k_src_bytes] = {
    0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU, /* signature        */
    0x00U, 0x00U, 0x00U, 0x0DU,                             /* IHDR length      */
    0x49U, 0x48U, 0x44U, 0x52U,                             /* "IHDR"           */
    0x00U, 0x00U, 0x00U, 0x40U,                             /* width  = 64      */
    0x00U, 0x00U, 0x00U, 0x30U,                             /* height = 48      */
    0x08U, 0x02U, 0x00U, 0x00U, 0x00U,                      /* depth/colour/... */
    0x00U, 0x00U, 0x00U,                                    /* filler           */
};

/** @brief GIF89a declaring a 64x48 logical screen. */
static const uint8_t s_gif[k_src_bytes] = {
    0x47U, 0x49U, 0x46U, 0x38U, 0x39U, 0x61U, /* "GIF89a"       */
    0x40U, 0x00U,                             /* width  = 64 LE */
    0x30U, 0x00U,                             /* height = 48 LE */
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};

/** @brief Bytes matching no container signature at all. */
static const uint8_t s_junk[k_src_bytes] = {
    0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U, 0x88U,
    0x99U, 0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU, 0x00U,
};

/** @brief RIFF whose form type is WAVE, so the sniff must not call it WebP. */
static const uint8_t s_riff_wave[k_src_bytes] = {
    0x52U, 0x49U, 0x46U, 0x46U, /* "RIFF" */
    0x10U, 0x00U, 0x00U, 0x00U, /* size   */
    0x57U, 0x41U, 0x56U, 0x45U, /* "WAVE" */
    0x66U, 0x6DU, 0x74U, 0x20U, /* "fmt " */
};

/**
 * @brief Bind a fake that opens @p formats up to @p dim_max.
 */
RA8_INTERNAL static ra8_imgdec_t internal_bind(fake_backend_t* be,
                                               uint32_t        formats,
                                               uint32_t        dim_max) {
  be->formats      = formats;
  be->pixels       = (uint32_t)k_ra8_imgdec_pixel_rgb888;
  be->dim_max      = dim_max;
  be->caps_calls   = 0U;
  be->decode_calls = 0U;
  return (ra8_imgdec_t){.iface = &s_fake_iface, .ctx = be};
}

RA8_INTERNAL static void internal_test_probe_answers_geometry(void) {
  TEST_BEGIN("a probe answers the container and its declared size");

  fake_backend_t     be  = {};
  const ra8_imgdec_t dec = internal_bind(&be, (uint32_t)k_ra8_imgdec_format_png,
                                         (uint32_t)k_wide_dim);

  ra8_imgdec_geom_t geom = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_probe(&dec, s_png, (uint32_t)k_src_bytes, &geom));
  TEST_ASSERT(geom.format == k_ra8_imgdec_format_png);
  TEST_ASSERT(geom.width_px == (uint32_t)k_png_w);
  TEST_ASSERT(geom.height_px == (uint32_t)k_png_h);

  /* Nothing was decoded to learn that. */
  TEST_ASSERT(be.decode_calls == 0U);

  TEST_END("a probe answers the container and its declared size");
}

RA8_INTERNAL static void internal_test_format_gate(void) {
  TEST_BEGIN("a container the backend does not open is refused, not described");

  /* The bytes are a perfectly readable GIF; this backend only opens PNG. */
  fake_backend_t     be  = {};
  const ra8_imgdec_t dec = internal_bind(&be, (uint32_t)k_ra8_imgdec_format_png,
                                         (uint32_t)k_wide_dim);

  ra8_imgdec_geom_t geom = {};
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_imgdec_probe(&dec, s_gif, (uint32_t)k_src_bytes, &geom));
  TEST_ASSERT(geom.format == k_ra8_imgdec_format_none);
  TEST_ASSERT(geom.width_px == 0U);
  TEST_ASSERT(geom.height_px == 0U);

  /* The same bytes through a backend that does open GIF answer fully, so the
   * refusal above is the capability gate and not a failure to read them. */
  fake_backend_t     gif_be  = {};
  const ra8_imgdec_t gif_dec = internal_bind(&gif_be, (uint32_t)k_ra8_imgdec_format_gif,
                                             (uint32_t)k_wide_dim);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_probe(&gif_dec, s_gif, (uint32_t)k_src_bytes, &geom));
  TEST_ASSERT(geom.format == k_ra8_imgdec_format_gif);
  TEST_ASSERT(geom.width_px == (uint32_t)k_png_w);

  TEST_END("a container the backend does not open is refused, not described");
}

RA8_INTERNAL static void internal_test_dim_max_is_enforced_here(void) {
  TEST_BEGIN("a size past this backend's own dim_max is refused");

  /* 64x48 against a backend that tops out at 32 in each direction. Both the
   * width and the height exceed it, so run the narrow case twice: once where
   * only the width is over, once where only the height is. */
  fake_backend_t     tight  = {};
  const ra8_imgdec_t narrow = internal_bind(&tight, (uint32_t)k_ra8_imgdec_format_png,
                                            (uint32_t)k_tight_dim);

  ra8_imgdec_geom_t geom = {};
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_imgdec_probe(&narrow, s_png, (uint32_t)k_src_bytes, &geom));
  TEST_ASSERT(geom.format == k_ra8_imgdec_format_none);

  /* Width only: 64 wide, 16 tall, against dim_max 32. */
  uint8_t only_wide[k_src_bytes];
  (void)memcpy(only_wide, s_png, sizeof(only_wide));
  only_wide[23] = 0x10U; /* height = 16 */
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_imgdec_probe(&narrow, only_wide, (uint32_t)k_src_bytes, &geom));

  /* Height only: 16 wide, 48 tall, against dim_max 32. */
  uint8_t only_tall[k_src_bytes];
  (void)memcpy(only_tall, s_png, sizeof(only_tall));
  only_tall[19] = 0x10U; /* width = 16 */
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_imgdec_probe(&narrow, only_tall, (uint32_t)k_src_bytes, &geom));

  /* Both inside the limit: 16x16 is accepted by the same narrow backend, so
   * the three refusals above are the limit and not a broken fixture. */
  uint8_t small[k_src_bytes];
  (void)memcpy(small, s_png, sizeof(small));
  small[19] = 0x10U;
  small[23] = 0x10U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_probe(&narrow, small, (uint32_t)k_src_bytes, &geom));
  TEST_ASSERT(geom.width_px == 16U);
  TEST_ASSERT(geom.height_px == 16U);

  TEST_END("a size past this backend's own dim_max is refused");
}

RA8_INTERNAL static void internal_test_unreadable_bytes_answer_like_a_decode(void) {
  TEST_BEGIN("bytes the seam cannot open are not_supported, as a decode says");

  fake_backend_t     be  = {};
  const ra8_imgdec_t dec = internal_bind(&be,
                                         (uint32_t)k_ra8_imgdec_format_png |
                                             (uint32_t)k_ra8_imgdec_format_webp,
                                         (uint32_t)k_wide_dim);

  ra8_imgdec_geom_t geom = {};

  /* No signature at all. ra8_imgdec_dims calls this not_found on its own; the
   * probe folds it into the code ra8_imgdec_decode gives the same buffer. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_imgdec_probe(&dec, s_junk, (uint32_t)k_src_bytes, &geom));
  TEST_ASSERT(geom.format == k_ra8_imgdec_format_none);

  /* A RIFF that is not a WebP is the same answer, not a mis-read WebP. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_imgdec_probe(&dec, s_riff_wave, (uint32_t)k_src_bytes, &geom));

  /* Recognised container, header truncated before the geometry. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_imgdec_probe(&dec, s_png, (uint32_t)k_short_len, &geom));

  /* And the raw probe still distinguishes the two, for a caller that wants it. */
  TEST_ASSERT_EQ(k_ra8_err_not_found,
                 ra8_imgdec_dims(s_junk, (uint32_t)k_src_bytes, &geom));

  TEST_ASSERT(be.decode_calls == 0U);

  TEST_END("bytes the seam cannot open are not_supported, as a decode says");
}

RA8_INTERNAL static void internal_test_backend_guards(void) {
  TEST_BEGIN("an unbound or unusable backend is caught before the bytes are read");

  ra8_imgdec_geom_t geom = {};

  /* Never bound. */
  const ra8_imgdec_t unbound = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_imgdec_probe(&unbound, s_png, (uint32_t)k_src_bytes, &geom));
  TEST_ASSERT(geom.format == k_ra8_imgdec_format_none);

  /* Bound, but reporting a record the fabric calls unusable. */
  fake_backend_t     broken_ctx = {};
  const ra8_imgdec_t broken     = {.iface = &s_broken_iface, .ctx = &broken_ctx};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_imgdec_probe(&broken, s_png, (uint32_t)k_src_bytes, &geom));

  /* Bound, but with no decode entry: a probe decodes nothing, so this is fine
   * and the geometry still comes back. */
  static const ra8_imgdec_iface_t s_caps_only = {.get_caps = internal_fake_caps};
  fake_backend_t                  caps_ctx    = {};
  (void)internal_bind(&caps_ctx, (uint32_t)k_ra8_imgdec_format_png, (uint32_t)k_wide_dim);
  const ra8_imgdec_t caps_only = {.iface = &s_caps_only, .ctx = &caps_ctx};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_imgdec_probe(&caps_only, s_png, (uint32_t)k_src_bytes, &geom));
  TEST_ASSERT(geom.width_px == (uint32_t)k_png_w);

  TEST_END("an unbound or unusable backend is caught before the bytes are read");
}

RA8_INTERNAL static void internal_test_argument_guards(void) {
  TEST_BEGIN("every pointer and an empty buffer are refused, and out is cleared");

  fake_backend_t     be  = {};
  const ra8_imgdec_t dec = internal_bind(&be, (uint32_t)k_ra8_imgdec_format_png,
                                         (uint32_t)k_wide_dim);

  ra8_imgdec_geom_t geom = {.format    = k_ra8_imgdec_format_webp,
                            .width_px  = 7U,
                            .height_px = 9U};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_imgdec_probe(nullptr, s_png, (uint32_t)k_src_bytes, &geom));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_imgdec_probe(&dec, nullptr, (uint32_t)k_src_bytes, &geom));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_imgdec_probe(&dec, s_png, (uint32_t)k_src_bytes, nullptr));

  /* A NULL out is refused before *out is touched, so the stale record above is
   * still intact; every other refusal clears it. */
  TEST_ASSERT(geom.width_px == 7U);

  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_imgdec_probe(&dec, s_png, 0U, &geom));
  TEST_ASSERT(geom.format == k_ra8_imgdec_format_none);
  TEST_ASSERT(geom.width_px == 0U);
  TEST_ASSERT(geom.height_px == 0U);

  TEST_ASSERT(be.decode_calls == 0U);

  TEST_END("every pointer and an empty buffer are refused, and out is cleared");
}

RA8_INTERNAL static void internal_test_probe_is_not_the_layout_question(void) {
  TEST_BEGIN("the probe answers about the image, ra8_imgdec_supports about the layout");

  /* This backend opens PNG but writes only rgb888. The probe still says yes,
   * because the image is one it opens; whether the caller's destination layout
   * is available is a separate query that needs no bytes. */
  fake_backend_t     be  = {};
  const ra8_imgdec_t dec = internal_bind(&be, (uint32_t)k_ra8_imgdec_format_png,
                                         (uint32_t)k_wide_dim);

  ra8_imgdec_geom_t geom = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_probe(&dec, s_png, (uint32_t)k_src_bytes, &geom));
  TEST_ASSERT(geom.format == k_ra8_imgdec_format_png);

  bool ok = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_supports(&dec, k_ra8_imgdec_format_png,
                                                k_ra8_imgdec_pixel_rgba8888, &ok));
  TEST_ASSERT(!ok);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_supports(&dec, k_ra8_imgdec_format_png,
                                                k_ra8_imgdec_pixel_rgb888, &ok));
  TEST_ASSERT(ok);

  TEST_END("the probe answers about the image, ra8_imgdec_supports about the layout");
}

int main(void) {
  internal_test_probe_answers_geometry();
  internal_test_format_gate();
  internal_test_dim_max_is_enforced_here();
  internal_test_unreadable_bytes_answer_like_a_decode();
  internal_test_backend_guards();
  internal_test_argument_guards();
  internal_test_probe_is_not_the_layout_question();
  return 0;
}
