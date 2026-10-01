/**
 * @file test_ra8_imgdec_mux.c
 * @brief Host tests for routing a decode across a set of backends (RA8FW-308).
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

/** @brief Fixture sizes (no magic numbers). */
enum : uint32_t {
  k_dst_bytes  = 64,    /**< Destination surface every request writes into. */
  k_src_bytes  = 16,    /**< Encoded-source fixture length.                 */
  k_fake_dim   = 4096,  /**< dim_max every fake backend advertises.         */
  k_fake_w     = 2,     /**< Width a fake decode reports.                   */
  k_fake_h     = 1,     /**< Height a fake decode reports.                  */
};

/**
 * @struct fake_backend_t
 * @brief A backend that advertises a set and records that it was reached.
 */
typedef struct {
  uint32_t            formats;      /**< Formats it claims to open.           */
  uint32_t            pixels;       /**< Pixel layouts it claims to write.    */
  uint32_t            scratch;      /**< Published scratch budget.            */
  uint32_t            decode_calls; /**< Times the fabric dispatched into it. */
  ra8_imgdec_format_t seen;         /**< Format the last dispatch carried.    */
} fake_backend_t;

RA8_INTERNAL static ra8_err_t internal_fake_caps(void* ctx, ra8_imgdec_caps_t* out) {
  const fake_backend_t* const be = (const fake_backend_t*)ctx;
  out->formats                   = be->formats;
  out->pixels                    = be->pixels;
  out->scratch_bytes             = be->scratch;
  out->scratch_align             = 0U;
  out->dim_max                   = (uint32_t)k_fake_dim;
  out->streams                   = false;
  return k_ra8_ok;
}

RA8_INTERNAL static ra8_err_t internal_fake_decode(void*                   ctx,
                                                   const ra8_imgdec_req_t* req,
                                                   ra8_imgdec_image_t*     out) {
  fake_backend_t* const be = (fake_backend_t*)ctx;
  be->decode_calls += 1U;
  be->seen = req->format;

  out->width_px   = (uint32_t)k_fake_w;
  out->height_px  = (uint32_t)k_fake_h;
  out->stride     = (uint32_t)k_fake_w * ra8_imgdec_pixel_bytes(req->want);
  out->used_bytes = out->stride * (uint32_t)k_fake_h;
  out->format     = req->format;
  out->pixel      = req->want;
  out->had_alpha  = false;
  return k_ra8_ok;
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
  out->dim_max       = (uint32_t)k_fake_dim;
  out->streams       = false;
  return k_ra8_ok;
}

static const ra8_imgdec_iface_t s_broken_iface = {
    .get_caps = internal_broken_caps,
    .decode   = internal_fake_decode,
};

static uint8_t g_dst[k_dst_bytes];

/** @brief A real PNG signature followed by filler, so the sniff answers PNG. */
static const uint8_t s_png[k_src_bytes] = {0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU,
                                           0x1AU, 0x0AU, 0x00U, 0x00U, 0x00U, 0x0DU,
                                           0x49U, 0x48U, 0x44U, 0x52U};

/** @brief Bytes matching no container signature at all. */
static const uint8_t s_junk[k_src_bytes] = {0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U,
                                            0x77U, 0x88U, 0x99U, 0xAAU, 0xBBU, 0xCCU,
                                            0xDDU, 0xEEU, 0xFFU, 0x00U};

/**
 * @brief Build a request over the shared destination.
 *
 * @param[in] bytes  Encoded source.
 * @param[in] format Declared container, or `_none` to have the mux sniff.
 * @param[in] want   Destination layout.
 *
 * @return ra8_imgdec_req_t The request.
 */
RA8_INTERNAL static ra8_imgdec_req_t internal_req(const uint8_t*      bytes,
                                                  ra8_imgdec_format_t format,
                                                  ra8_imgdec_pixel_t  want) {
  (void)memset(g_dst, 0, sizeof(g_dst));
  return (ra8_imgdec_req_t){
      .bytes      = bytes,
      .byte_count = (uint32_t)k_src_bytes,
      .arena      = nullptr,
      .dst        = g_dst,
      .dst_bytes  = (uint32_t)sizeof(g_dst),
      .dst_stride = 0U,
      .format     = format,
      .want       = want,
  };
}

RA8_INTERNAL static void internal_test_empty_mux_answers_nothing(void) {
  TEST_BEGIN("an empty mux is uninitialised, not a set that supports nothing");

  ra8_imgdec_mux_t mux = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_init(&mux));

  bool ok = true;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_imgdec_mux_supports(&mux, k_ra8_imgdec_format_png,
                                         k_ra8_imgdec_pixel_rgba8888, &ok));
  TEST_ASSERT(!ok);

  uint32_t formats = 0xFFFFU;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_imgdec_mux_formats(&mux, k_ra8_imgdec_pixel_rgba8888, &formats));
  TEST_ASSERT(formats == (uint32_t)k_ra8_imgdec_format_none);

  ra8_imgdec_req_t   req = internal_req(s_png, k_ra8_imgdec_format_none,
                                        k_ra8_imgdec_pixel_rgba8888);
  ra8_imgdec_image_t img = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_imgdec_mux_decode(&mux, &req, &img));

  TEST_END("an empty mux is uninitialised, not a set that supports nothing");
}

RA8_INTERNAL static void internal_test_add_guards(void) {
  TEST_BEGIN("a member is checked when it is added, not at the first decode");

  ra8_imgdec_mux_t mux = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_init(&mux));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_mux_add(&mux, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_mux_add(nullptr, nullptr));

  /* Never bound: the mux refuses it rather than storing a dead member. */
  const ra8_imgdec_t unbound = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_imgdec_mux_add(&mux, &unbound));

  /* Bound, but reporting a record the fabric calls unusable. */
  fake_backend_t     broken_ctx = {};
  const ra8_imgdec_t broken     = {.iface = &s_broken_iface, .ctx = &broken_ctx};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_imgdec_mux_add(&mux, &broken));

  /* Neither refusal grew the set. */
  uint32_t formats = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_imgdec_mux_formats(&mux, k_ra8_imgdec_pixel_rgba8888, &formats));

  TEST_END("a member is checked when it is added, not at the first decode");
}

RA8_INTERNAL static void internal_test_set_is_bounded(void) {
  TEST_BEGIN("the member set is bounded and a full mux refuses rather than drops");

  ra8_imgdec_mux_t mux = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_init(&mux));

  fake_backend_t be = {.formats = (uint32_t)k_ra8_imgdec_format_png,
                       .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgba8888};
  const ra8_imgdec_t dec = {.iface = &s_fake_iface, .ctx = &be};

  for (uint32_t i = 0U; i < (uint32_t)k_ra8_imgdec_mux_max; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_add(&mux, &dec));
  }
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_imgdec_mux_add(&mux, &dec));

  TEST_END("the member set is bounded and a full mux refuses rather than drops");
}

RA8_INTERNAL static void internal_test_union_is_per_pair(void) {
  TEST_BEGIN("the openable set is unioned per layout, never across layouts");

  fake_backend_t webp = {.formats = (uint32_t)k_ra8_imgdec_format_webp,
                         .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgba8888};
  fake_backend_t gif  = {.formats = (uint32_t)k_ra8_imgdec_format_gif,
                         .pixels  = (uint32_t)k_ra8_imgdec_pixel_grey8};

  const ra8_imgdec_t webp_dec = {.iface = &s_fake_iface, .ctx = &webp};
  const ra8_imgdec_t gif_dec  = {.iface = &s_fake_iface, .ctx = &gif};

  ra8_imgdec_mux_t mux = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_init(&mux));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_add(&mux, &webp_dec));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_add(&mux, &gif_dec));

  uint32_t rgba = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_formats(&mux, k_ra8_imgdec_pixel_rgba8888, &rgba));
  TEST_ASSERT(rgba == (uint32_t)k_ra8_imgdec_format_webp);

  uint32_t grey = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_formats(&mux, k_ra8_imgdec_pixel_grey8, &grey));
  TEST_ASSERT(grey == (uint32_t)k_ra8_imgdec_format_gif);

  /* The cross pairs are exactly what a flat union of both records would have
   * claimed, and no member can serve either of them. */
  bool ok = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_supports(&mux, k_ra8_imgdec_format_webp,
                                                   k_ra8_imgdec_pixel_grey8, &ok));
  TEST_ASSERT(!ok);
  ok = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_supports(&mux, k_ra8_imgdec_format_gif,
                                                   k_ra8_imgdec_pixel_rgba8888, &ok));
  TEST_ASSERT(!ok);

  ok = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_supports(&mux, k_ra8_imgdec_format_webp,
                                                   k_ra8_imgdec_pixel_rgba8888, &ok));
  TEST_ASSERT(ok);

  TEST_END("the openable set is unioned per layout, never across layouts");
}

RA8_INTERNAL static void internal_test_order_is_priority(void) {
  TEST_BEGIN("overlap is resolved by the order members were added");

  fake_backend_t first  = {.formats = (uint32_t)k_ra8_imgdec_format_jpeg,
                           .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgba8888};
  fake_backend_t second = {.formats = (uint32_t)k_ra8_imgdec_format_jpeg |
                                      (uint32_t)k_ra8_imgdec_format_bmp,
                           .pixels = (uint32_t)k_ra8_imgdec_pixel_rgba8888};

  const ra8_imgdec_t first_dec  = {.iface = &s_fake_iface, .ctx = &first};
  const ra8_imgdec_t second_dec = {.iface = &s_fake_iface, .ctx = &second};

  ra8_imgdec_mux_t mux = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_init(&mux));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_add(&mux, &first_dec));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_add(&mux, &second_dec));

  const ra8_imgdec_t* routed = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_route(&mux, k_ra8_imgdec_format_jpeg,
                                                k_ra8_imgdec_pixel_rgba8888, &routed));
  TEST_ASSERT_NOT_NULL(routed);
  TEST_ASSERT(routed->ctx == (void*)&first);

  /* The format only the later member opens still reaches it. */
  routed = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_route(&mux, k_ra8_imgdec_format_bmp,
                                                k_ra8_imgdec_pixel_rgba8888, &routed));
  TEST_ASSERT_NOT_NULL(routed);
  TEST_ASSERT(routed->ctx == (void*)&second);

  TEST_END("overlap is resolved by the order members were added");
}

RA8_INTERNAL static void internal_test_decode_routes_by_sniff(void) {
  TEST_BEGIN("an unstated container is sniffed once, then routed");

  fake_backend_t png  = {.formats = (uint32_t)k_ra8_imgdec_format_png,
                         .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgba8888};
  fake_backend_t webp = {.formats = (uint32_t)k_ra8_imgdec_format_webp,
                         .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgba8888};

  const ra8_imgdec_t png_dec  = {.iface = &s_fake_iface, .ctx = &png};
  const ra8_imgdec_t webp_dec = {.iface = &s_fake_iface, .ctx = &webp};

  ra8_imgdec_mux_t mux = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_init(&mux));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_add(&mux, &webp_dec));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_add(&mux, &png_dec));

  ra8_imgdec_req_t   req = internal_req(s_png, k_ra8_imgdec_format_none,
                                        k_ra8_imgdec_pixel_rgba8888);
  ra8_imgdec_image_t img = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_decode(&mux, &req, &img));

  /* The PNG member ran; the WebP member sitting ahead of it never did. */
  TEST_ASSERT(png.decode_calls == 1U);
  TEST_ASSERT(webp.decode_calls == 0U);

  /* The member was handed the resolved format, never `_none`. */
  TEST_ASSERT(png.seen == k_ra8_imgdec_format_png);
  TEST_ASSERT(img.format == k_ra8_imgdec_format_png);
  TEST_ASSERT(img.pixel == k_ra8_imgdec_pixel_rgba8888);

  TEST_END("an unstated container is sniffed once, then routed");
}

RA8_INTERNAL static void internal_test_unroutable_never_reaches_a_backend(void) {
  TEST_BEGIN("nothing is dispatched when no member can serve the pair");

  fake_backend_t png = {.formats = (uint32_t)k_ra8_imgdec_format_png,
                        .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgba8888};
  const ra8_imgdec_t png_dec = {.iface = &s_fake_iface, .ctx = &png};

  ra8_imgdec_mux_t mux = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_init(&mux));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_add(&mux, &png_dec));

  /* Recognised container, but no member opens it. */
  ra8_imgdec_req_t   req = internal_req(s_png, k_ra8_imgdec_format_bmp,
                                        k_ra8_imgdec_pixel_rgba8888);
  ra8_imgdec_image_t img = {};
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_imgdec_mux_decode(&mux, &req, &img));
  TEST_ASSERT(png.decode_calls == 0U);

  /* Right container, layout no member writes. */
  req = internal_req(s_png, k_ra8_imgdec_format_png, k_ra8_imgdec_pixel_grey8);
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_imgdec_mux_decode(&mux, &req, &img));
  TEST_ASSERT(png.decode_calls == 0U);

  /* Bytes carrying no signature at all, with the format left unstated. */
  req = internal_req(s_junk, k_ra8_imgdec_format_none, k_ra8_imgdec_pixel_rgba8888);
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_imgdec_mux_decode(&mux, &req, &img));
  TEST_ASSERT(png.decode_calls == 0U);

  TEST_END("nothing is dispatched when no member can serve the pair");
}

RA8_INTERNAL static void internal_test_query_guards(void) {
  TEST_BEGIN("a query naming more than one bit is refused, outputs cleared");

  fake_backend_t png = {.formats = (uint32_t)k_ra8_imgdec_format_png,
                        .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgba8888};
  const ra8_imgdec_t png_dec = {.iface = &s_fake_iface, .ctx = &png};

  ra8_imgdec_mux_t mux = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_init(&mux));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_add(&mux, &png_dec));

  const ra8_imgdec_format_t two_formats =
      (ra8_imgdec_format_t)((uint32_t)k_ra8_imgdec_format_png |
                            (uint32_t)k_ra8_imgdec_format_gif);

  bool ok = true;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_imgdec_mux_supports(&mux, two_formats, k_ra8_imgdec_pixel_rgba8888, &ok));
  TEST_ASSERT(!ok);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_imgdec_mux_supports(&mux, k_ra8_imgdec_format_png,
                                         k_ra8_imgdec_pixel_none, &ok));
  TEST_ASSERT(!ok);

  const ra8_imgdec_t* routed = &png_dec;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_imgdec_mux_route(&mux, k_ra8_imgdec_format_none,
                                      k_ra8_imgdec_pixel_rgba8888, &routed));
  TEST_ASSERT(routed == nullptr);

  uint32_t formats = 0xFFFFU;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_imgdec_mux_formats(&mux, k_ra8_imgdec_pixel_none, &formats));
  TEST_ASSERT(formats == (uint32_t)k_ra8_imgdec_format_none);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_imgdec_mux_supports(&mux, k_ra8_imgdec_format_png,
                                         k_ra8_imgdec_pixel_rgba8888, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_imgdec_mux_formats(nullptr, k_ra8_imgdec_pixel_rgba8888, &formats));

  TEST_END("a query naming more than one bit is refused, outputs cleared");
}

RA8_INTERNAL static void internal_test_decode_guards(void) {
  TEST_BEGIN("a malformed decode is refused before any member is chosen");

  fake_backend_t png = {.formats = (uint32_t)k_ra8_imgdec_format_png,
                        .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgba8888};
  const ra8_imgdec_t png_dec = {.iface = &s_fake_iface, .ctx = &png};

  ra8_imgdec_mux_t mux = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_init(&mux));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_add(&mux, &png_dec));

  ra8_imgdec_image_t img = {};
  ra8_imgdec_req_t   req = internal_req(s_png, k_ra8_imgdec_format_png,
                                        k_ra8_imgdec_pixel_rgba8888);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_mux_decode(&mux, nullptr, &img));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_mux_decode(&mux, &req, nullptr));

  req.bytes = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_mux_decode(&mux, &req, &img));

  req       = internal_req(s_png, k_ra8_imgdec_format_png, k_ra8_imgdec_pixel_rgba8888);
  req.dst   = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_mux_decode(&mux, &req, &img));

  req            = internal_req(s_png, k_ra8_imgdec_format_png, k_ra8_imgdec_pixel_rgba8888);
  req.byte_count = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_imgdec_mux_decode(&mux, &req, &img));

  req      = internal_req(s_png, k_ra8_imgdec_format_png, k_ra8_imgdec_pixel_rgba8888);
  req.want = k_ra8_imgdec_pixel_none;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_imgdec_mux_decode(&mux, &req, &img));

  TEST_ASSERT(png.decode_calls == 0U);

  TEST_END("a malformed decode is refused before any member is chosen");
}

RA8_INTERNAL static void internal_test_member_gate_still_applies(void) {
  TEST_BEGIN("routing does not bypass the chosen member's own gate");

  /* The member advertises a scratch budget and the request supplies no arena.
   * The mux routes to it because the pair matches; the single-backend fabric
   * is what refuses, which is the division of labour this file relies on. */
  fake_backend_t hungry = {.formats = (uint32_t)k_ra8_imgdec_format_png,
                           .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgba8888,
                           .scratch = 128U};
  const ra8_imgdec_t hungry_dec = {.iface = &s_fake_iface, .ctx = &hungry};

  ra8_imgdec_mux_t mux = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_init(&mux));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_add(&mux, &hungry_dec));

  const ra8_imgdec_t* routed = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_route(&mux, k_ra8_imgdec_format_png,
                                                k_ra8_imgdec_pixel_rgba8888, &routed));
  TEST_ASSERT_NOT_NULL(routed);

  ra8_imgdec_req_t   req = internal_req(s_png, k_ra8_imgdec_format_none,
                                        k_ra8_imgdec_pixel_rgba8888);
  ra8_imgdec_image_t img = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_imgdec_mux_decode(&mux, &req, &img));
  TEST_ASSERT(hungry.decode_calls == 0U);
  TEST_ASSERT(img.width_px == 0U);

  TEST_END("routing does not bypass the chosen member's own gate");
}

int main(void) {
  internal_test_empty_mux_answers_nothing();
  internal_test_add_guards();
  internal_test_set_is_bounded();
  internal_test_union_is_per_pair();
  internal_test_order_is_priority();
  internal_test_decode_routes_by_sniff();
  internal_test_unroutable_never_reaches_a_backend();
  internal_test_query_guards();
  internal_test_decode_guards();
  internal_test_member_gate_still_applies();
  return 0;
}
