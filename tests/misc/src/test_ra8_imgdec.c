/**
 * @file test_ra8_imgdec.c
 * @brief Host tests for the ra8_imgdec fabric (RA8FW-308).
 *
 * @par Tag
 * [Ring 3 / Imaging] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdint.h>

#include "ra8_arena.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_imgdec.h"
#include "ra8_imgdec_backend.h"
#include "unity_minimal.h"

/* -----------------------------------------------------------------------------
 * A recording stand-in backend: it counts decodes and remembers the last
 * request, so "the fabric refused before the backend was reached" is an
 * assertion rather than a claim.
 * -----------------------------------------------------------------------------
 */

typedef struct {
  ra8_imgdec_caps_t caps;
  ra8_err_t         caps_result;
  ra8_err_t         decode_result;
  uint32_t          caps_calls;
  uint32_t          decode_calls;
  ra8_imgdec_req_t  last_req;
} spy_t;

RA8_INTERNAL static ra8_err_t spy_caps(void* ctx, ra8_imgdec_caps_t* out) {
  spy_t* spy = (spy_t*)ctx;
  spy->caps_calls++;
  if (spy->caps_result != k_ra8_ok) {
    return spy->caps_result;
  }
  *out = spy->caps;
  return k_ra8_ok;
}

RA8_INTERNAL static ra8_err_t spy_decode(void*                   ctx,
                                         const ra8_imgdec_req_t* req,
                                         ra8_imgdec_image_t*     out) {
  spy_t* spy = (spy_t*)ctx;
  spy->decode_calls++;
  spy->last_req = *req;
  if (spy->decode_result != k_ra8_ok) {
    return spy->decode_result;
  }
  out->width_px   = 4U;
  out->height_px  = 2U;
  out->stride     = 4U * ra8_imgdec_pixel_bytes(req->want);
  out->used_bytes = out->stride * 2U;
  out->format     = k_ra8_imgdec_format_png;
  out->pixel      = req->want;
  out->had_alpha  = false;
  return k_ra8_ok;
}

static const ra8_imgdec_iface_t k_spy_iface = {
    .get_caps = spy_caps,
    .decode   = spy_decode,
};

RA8_INTERNAL static void spy_reset(spy_t* spy) {
  const spy_t empty = {};
  *spy              = empty;
  spy->caps.formats =
      (uint32_t)k_ra8_imgdec_format_png | (uint32_t)k_ra8_imgdec_format_webp;
  spy->caps.pixels =
      (uint32_t)k_ra8_imgdec_pixel_rgba8888 | (uint32_t)k_ra8_imgdec_pixel_grey8;
  spy->caps.scratch_bytes = 0U;
  spy->caps.scratch_align = 4U;
  spy->caps.dim_max       = 4096U;
  spy->caps.streams       = false;
}

/* The eight-byte PNG signature (PNG 5.2), so a `_none` request can sniff.
   The spy never parses past it; the remaining bytes stay zero. */
static uint8_t   s_encoded[16] = {0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU};
static uint8_t   s_surface[256];
static uint8_t   s_arena_backing[512];

RA8_INTERNAL static ra8_imgdec_req_t make_req(ra8_arena_t* arena) {
  const ra8_imgdec_req_t req = {
      .bytes      = s_encoded,
      .byte_count = (uint32_t)sizeof s_encoded,
      .arena      = arena,
      .dst        = s_surface,
      .dst_bytes  = (uint32_t)sizeof s_surface,
      .dst_stride = 0U,
      .format     = k_ra8_imgdec_format_png,
      .want       = k_ra8_imgdec_pixel_rgba8888,
  };
  return req;
}

/* -------------------------------------------------------------------------- */

RA8_INTERNAL static void internal_test_pixel_bytes(void) {
  TEST_BEGIN("pixel bytes per layout");

  TEST_ASSERT_EQ(1U, ra8_imgdec_pixel_bytes(k_ra8_imgdec_pixel_grey8));
  TEST_ASSERT_EQ(3U, ra8_imgdec_pixel_bytes(k_ra8_imgdec_pixel_rgb888));
  TEST_ASSERT_EQ(4U, ra8_imgdec_pixel_bytes(k_ra8_imgdec_pixel_rgba8888));
  TEST_ASSERT_EQ(0U, ra8_imgdec_pixel_bytes(k_ra8_imgdec_pixel_none));
  /* A two-bit value is not one layout. */
  TEST_ASSERT_EQ(0U,
                 ra8_imgdec_pixel_bytes((ra8_imgdec_pixel_t)((uint32_t)k_ra8_imgdec_pixel_grey8 |
                                                             (uint32_t)k_ra8_imgdec_pixel_rgb888)));

  TEST_END("pixel bytes per layout");
}

RA8_INTERNAL static void internal_test_unbound_handle(void) {
  TEST_BEGIN("an unbound handle decodes nothing");

  ra8_imgdec_t      dec  = {};
  ra8_imgdec_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_imgdec_get_caps(&dec, &caps));
  TEST_ASSERT_EQ(0U, caps.formats);

  bool ok = true;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_imgdec_supports(&dec, k_ra8_imgdec_format_png,
                                     k_ra8_imgdec_pixel_rgba8888, &ok));
  TEST_ASSERT(!ok);

  const ra8_imgdec_req_t req = make_req(nullptr);
  ra8_imgdec_image_t     out = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_imgdec_decode(&dec, &req, &out));

  TEST_END("an unbound handle decodes nothing");
}

RA8_INTERNAL static void internal_test_null_arguments(void) {
  TEST_BEGIN("null arguments are refused");

  spy_t spy = {};
  spy_reset(&spy);
  ra8_imgdec_t dec = {.iface = &k_spy_iface, .ctx = &spy};

  ra8_imgdec_caps_t  caps = {};
  ra8_imgdec_image_t out  = {};
  ra8_imgdec_req_t   req  = make_req(nullptr);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_get_caps(nullptr, &caps));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_get_caps(&dec, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_decode(nullptr, &req, &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_decode(&dec, nullptr, &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_decode(&dec, &req, nullptr));

  req.bytes = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_decode(&dec, &req, &out));
  req       = make_req(nullptr);
  req.dst   = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_decode(&dec, &req, &out));

  TEST_ASSERT_EQ(0U, spy.decode_calls);

  TEST_END("null arguments are refused");
}

RA8_INTERNAL static void internal_test_request_shape(void) {
  TEST_BEGIN("a malformed request never reaches the backend");

  spy_t spy = {};
  spy_reset(&spy);
  ra8_imgdec_t       dec = {.iface = &k_spy_iface, .ctx = &spy};
  ra8_imgdec_image_t out = {};

  ra8_imgdec_req_t req = make_req(nullptr);
  req.byte_count       = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_imgdec_decode(&dec, &req, &out));

  req           = make_req(nullptr);
  req.dst_bytes = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_imgdec_decode(&dec, &req, &out));

  /* A stride narrower than one RGBA pixel cannot hold a row. */
  req            = make_req(nullptr);
  req.dst_stride = 3U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_imgdec_decode(&dec, &req, &out));

  /* A destination smaller than one pixel. */
  req           = make_req(nullptr);
  req.dst_bytes = 2U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_imgdec_decode(&dec, &req, &out));

  req      = make_req(nullptr);
  req.want = k_ra8_imgdec_pixel_none;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_imgdec_decode(&dec, &req, &out));

  /* Two format bits is a set, not a declaration. */
  req        = make_req(nullptr);
  req.format = (ra8_imgdec_format_t)((uint32_t)k_ra8_imgdec_format_png |
                                     (uint32_t)k_ra8_imgdec_format_webp);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_imgdec_decode(&dec, &req, &out));

  TEST_ASSERT_EQ(0U, spy.decode_calls);

  TEST_END("a malformed request never reaches the backend");
}

RA8_INTERNAL static void internal_test_capability_gate(void) {
  TEST_BEGIN("an unadvertised format or layout is refused, not attempted");

  spy_t spy = {};
  spy_reset(&spy);
  ra8_imgdec_t       dec = {.iface = &k_spy_iface, .ctx = &spy};
  ra8_imgdec_image_t out = {};

  ra8_imgdec_req_t req = make_req(nullptr);
  req.format           = k_ra8_imgdec_format_jpeg; /* spy opens png + webp only */
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_imgdec_decode(&dec, &req, &out));

  req      = make_req(nullptr);
  req.want = k_ra8_imgdec_pixel_rgb888; /* spy writes rgba + grey only */
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_imgdec_decode(&dec, &req, &out));

  TEST_ASSERT_EQ(0U, spy.decode_calls);

  TEST_END("an unadvertised format or layout is refused, not attempted");
}

RA8_INTERNAL static void internal_test_supports_query(void) {
  TEST_BEGIN("supports answers the pair, not one half of it");

  spy_t spy = {};
  spy_reset(&spy);
  ra8_imgdec_t dec = {.iface = &k_spy_iface, .ctx = &spy};
  bool         ok  = false;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_supports(&dec, k_ra8_imgdec_format_webp,
                                               k_ra8_imgdec_pixel_grey8, &ok));
  TEST_ASSERT(ok);

  /* Format advertised, layout not. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_supports(&dec, k_ra8_imgdec_format_webp,
                                               k_ra8_imgdec_pixel_rgb888, &ok));
  TEST_ASSERT(!ok);

  /* Layout advertised, format not. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_supports(&dec, k_ra8_imgdec_format_gif,
                                               k_ra8_imgdec_pixel_grey8, &ok));
  TEST_ASSERT(!ok);

  ok = true;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_imgdec_supports(&dec, k_ra8_imgdec_format_none,
                                     k_ra8_imgdec_pixel_grey8, &ok));
  TEST_ASSERT(!ok);

  TEST_END("supports answers the pair, not one half of it");
}

RA8_INTERNAL static void internal_test_scratch_contract(void) {
  TEST_BEGIN("scratch is a published budget the fabric enforces");

  spy_t spy = {};
  spy_reset(&spy);
  spy.caps.scratch_bytes = 256U;
  ra8_imgdec_t       dec = {.iface = &k_spy_iface, .ctx = &spy};
  ra8_imgdec_image_t out = {};

  /* Says it needs scratch, handed none. */
  ra8_imgdec_req_t req = make_req(nullptr);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_imgdec_decode(&dec, &req, &out));
  TEST_ASSERT_EQ(0U, spy.decode_calls);

  /* An arena too small for the published budget. */
  ra8_arena_t small = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_init(&small, s_arena_backing, 64U));
  req = make_req(&small);
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_imgdec_decode(&dec, &req, &out));
  TEST_ASSERT_EQ(0U, spy.decode_calls);

  /* An arena that covers it. */
  ra8_arena_t big = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_init(&big, s_arena_backing, (uint32_t)sizeof s_arena_backing));
  req = make_req(&big);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_decode(&dec, &req, &out));
  TEST_ASSERT_EQ(1U, spy.decode_calls);
  TEST_ASSERT(spy.last_req.arena == &big);

  TEST_END("scratch is a published budget the fabric enforces");
}

RA8_INTERNAL static void internal_test_bad_backend_record(void) {
  TEST_BEGIN("a backend that advertises nonsense is refused");

  spy_t spy = {};
  spy_reset(&spy);
  ra8_imgdec_t       dec  = {.iface = &k_spy_iface, .ctx = &spy};
  ra8_imgdec_caps_t  caps = {};
  ra8_imgdec_image_t out  = {};

  spy.caps.formats = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_imgdec_get_caps(&dec, &caps));

  spy_reset(&spy);
  spy.caps.pixels = 0x80U; /* undefined bit */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_imgdec_get_caps(&dec, &caps));

  spy_reset(&spy);
  spy.caps.dim_max = 99999U; /* past the fabric limit */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_imgdec_get_caps(&dec, &caps));

  spy_reset(&spy);
  spy.caps_result      = k_ra8_err_busy;
  const ra8_imgdec_req_t req = make_req(nullptr);
  TEST_ASSERT_EQ(k_ra8_err_busy, ra8_imgdec_decode(&dec, &req, &out));
  TEST_ASSERT_EQ(0U, spy.decode_calls);

  /* A vtable missing its decode entry is a state error, not a NULL call. */
  spy_reset(&spy);
  static const ra8_imgdec_iface_t k_half = {.get_caps = spy_caps, .decode = nullptr};
  ra8_imgdec_t                    half   = {.iface = &k_half, .ctx = &spy};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_imgdec_decode(&half, &req, &out));

  TEST_END("a backend that advertises nonsense is refused");
}

RA8_INTERNAL static void internal_test_decode_result(void) {
  TEST_BEGIN("a good decode passes through, a failed one reports nothing");

  spy_t spy = {};
  spy_reset(&spy);
  ra8_imgdec_t       dec = {.iface = &k_spy_iface, .ctx = &spy};
  ra8_imgdec_image_t out = {};

  const ra8_imgdec_req_t req = make_req(nullptr);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_decode(&dec, &req, &out));
  TEST_ASSERT_EQ(4U, out.width_px);
  TEST_ASSERT_EQ(2U, out.height_px);
  TEST_ASSERT_EQ(16U, out.stride);
  TEST_ASSERT_EQ(32U, out.used_bytes);
  TEST_ASSERT_EQ(k_ra8_imgdec_pixel_rgba8888, out.pixel);
  TEST_ASSERT_EQ(1U, spy.decode_calls);

  /* Sniffing is permitted: _none is not checked against the format set. */
  spy_reset(&spy);
  ra8_imgdec_req_t sniff = make_req(nullptr);
  sniff.format           = k_ra8_imgdec_format_none;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_decode(&dec, &sniff, &out));
  TEST_ASSERT_EQ(1U, spy.decode_calls);

  /* A backend failure leaves the result record empty, not half-written. */
  spy_reset(&spy);
  spy.decode_result = k_ra8_err_crc_mismatch;
  TEST_ASSERT_EQ(k_ra8_err_crc_mismatch, ra8_imgdec_decode(&dec, &req, &out));
  TEST_ASSERT_EQ(0U, out.width_px);
  TEST_ASSERT_EQ(0U, out.used_bytes);

  TEST_END("a good decode passes through, a failed one reports nothing");
}

int main(void) {
  internal_test_pixel_bytes();
  internal_test_unbound_handle();
  internal_test_null_arguments();
  internal_test_request_shape();
  internal_test_capability_gate();
  internal_test_supports_query();
  internal_test_scratch_contract();
  internal_test_bad_backend_record();
  internal_test_decode_result();
  return 0;
}
