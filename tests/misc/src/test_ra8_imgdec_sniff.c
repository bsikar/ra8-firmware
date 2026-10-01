/**
 * @file test_ra8_imgdec_sniff.c
 * @brief Host tests for the shared container sniff and the fabric that uses it (RA8FW-308).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ra8_arena.h"
#include "ra8_err.h"
#include "ra8_imgdec.h"
#include "ra8_imgdec_backend.h"
#include "unity_minimal.h"

/** @brief Fixture sizes (no magic numbers). */
enum {
  k_fixture_bytes = 32,  /**< Longest encoded fixture used here.  */
  k_dst_bytes     = 64,  /**< Destination surface for the fabric. */
  k_dim_max       = 4096 /**< dim_max the stand-in advertises.    */
};

/**
 * @struct sniff_spy_t
 * @brief Stand-in backend recording what the fabric handed it.
 */
typedef struct {
  uint32_t            calls;     /**< Times decode() was entered.       */
  ra8_imgdec_format_t seen;      /**< Format on the request it saw.     */
  uint32_t            caps_fmt;  /**< Formats this stand-in advertises. */
} sniff_spy_t;

static ra8_err_t spy_caps(void* ctx, ra8_imgdec_caps_t* out) {
  const sniff_spy_t* spy = (const sniff_spy_t*)ctx;
  out->formats           = spy->caps_fmt;
  out->pixels            = (uint32_t)k_ra8_imgdec_pixel_rgba8888;
  out->scratch_bytes     = 0U;
  out->scratch_align     = 0U;
  out->dim_max           = (uint32_t)k_dim_max;
  out->streams           = false;
  return k_ra8_ok;
}

static ra8_err_t spy_decode(void* ctx, const ra8_imgdec_req_t* req, ra8_imgdec_image_t* out) {
  sniff_spy_t* spy = (sniff_spy_t*)ctx;
  spy->calls += 1U;
  spy->seen  = req->format;

  out->width_px   = 1U;
  out->height_px  = 1U;
  out->stride     = ra8_imgdec_pixel_bytes(req->want);
  out->used_bytes = out->stride;
  out->format     = req->format;
  out->pixel      = req->want;
  out->had_alpha  = false;
  return k_ra8_ok;
}

static const ra8_imgdec_iface_t k_spy_iface = {
    .get_caps = spy_caps,
    .decode   = spy_decode,
};

/** @brief Build a request over @p bytes with the format left unstated. */
static ra8_imgdec_req_t make_req(const uint8_t* bytes, uint32_t count, uint8_t* dst) {
  const ra8_imgdec_req_t req = {
      .bytes      = bytes,
      .byte_count = count,
      .arena      = nullptr,
      .dst        = dst,
      .dst_bytes  = (uint32_t)k_dst_bytes,
      .dst_stride = 0U,
      .format     = k_ra8_imgdec_format_none,
      .want       = k_ra8_imgdec_pixel_rgba8888,
  };
  return req;
}

RA8_INTERNAL static void internal_test_png_and_jpeg(void) {
  TEST_BEGIN("sniff png and jpeg");

  const uint8_t png[]  = {0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU, 0x00U};
  const uint8_t jpeg[] = {0xFFU, 0xD8U, 0xFFU, 0xE0U, 0x00U, 0x10U};

  ra8_imgdec_format_t got = k_ra8_imgdec_format_none;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_sniff(png, (uint32_t)sizeof png, &got));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_png, got);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_sniff(jpeg, (uint32_t)sizeof jpeg, &got));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_jpeg, got);

  /* One byte short of the PNG signature is not a PNG. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_imgdec_sniff(png, 7U, &got));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_none, got);

  TEST_END("sniff png and jpeg");
}

RA8_INTERNAL static void internal_test_webp_needs_both_fourccs(void) {
  TEST_BEGIN("sniff webp needs both fourccs");

  uint8_t buf[k_fixture_bytes] = {};
  memcpy(&buf[0], "RIFF", 4U);
  memcpy(&buf[4], "\x20\x00\x00\x00", 4U);
  memcpy(&buf[8], "WEBP", 4U);

  ra8_imgdec_format_t got = k_ra8_imgdec_format_none;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_sniff(buf, (uint32_t)sizeof buf, &got));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_webp, got);

  /* A RIFF that is not a WEBP (WAVE) must not be routed to a WebP decoder. */
  memcpy(&buf[8], "WAVE", 4U);
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_imgdec_sniff(buf, (uint32_t)sizeof buf, &got));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_none, got);

  /* Eleven bytes cannot carry the form tag, so it is not yet a WebP. */
  memcpy(&buf[8], "WEBP", 4U);
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_imgdec_sniff(buf, 11U, &got));

  TEST_END("sniff webp needs both fourccs");
}

RA8_INTERNAL static void internal_test_gif_and_bmp(void) {
  TEST_BEGIN("sniff gif and bmp");

  const uint8_t gif87[] = {'G', 'I', 'F', '8', '7', 'a', 0x00U};
  const uint8_t gif89[] = {'G', 'I', 'F', '8', '9', 'a', 0x00U};
  const uint8_t gif_bad[] = {'G', 'I', 'F', '9', '9', 'z', 0x00U};
  const uint8_t bmp[]   = {'B', 'M', 0x36U, 0x00U};

  ra8_imgdec_format_t got = k_ra8_imgdec_format_none;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_sniff(gif87, (uint32_t)sizeof gif87, &got));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_gif, got);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_sniff(gif89, (uint32_t)sizeof gif89, &got));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_gif, got);
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_imgdec_sniff(gif_bad, (uint32_t)sizeof gif_bad, &got));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_sniff(bmp, (uint32_t)sizeof bmp, &got));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_bmp, got);

  TEST_END("sniff gif and bmp");
}

RA8_INTERNAL static void internal_test_sniff_refusals(void) {
  TEST_BEGIN("sniff refusals");

  const uint8_t bytes[] = {0x00U, 0x01U, 0x02U, 0x03U};
  ra8_imgdec_format_t got = k_ra8_imgdec_format_png;

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_sniff(nullptr, 4U, &got));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_none, got);

  got = k_ra8_imgdec_format_png;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_sniff(bytes, 4U, nullptr));

  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_imgdec_sniff(bytes, 0U, &got));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_none, got);

  /* A TGA has no signature, so it is never sniffed; it must be declared. */
  const uint8_t tga[] = {0x00U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U};
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_imgdec_sniff(tga, (uint32_t)sizeof tga, &got));

  TEST_END("sniff refusals");
}

RA8_INTERNAL static void internal_test_fabric_resolves_none(void) {
  TEST_BEGIN("fabric resolves an unstated format");

  sniff_spy_t  spy = {.calls = 0U, .seen = k_ra8_imgdec_format_none,
                      .caps_fmt = (uint32_t)k_ra8_imgdec_format_png};
  ra8_imgdec_t dec = {.iface = &k_spy_iface, .ctx = &spy};

  const uint8_t png[] = {0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU, 0x00U};
  uint8_t       dst[k_dst_bytes] = {};

  const ra8_imgdec_req_t req = make_req(png, (uint32_t)sizeof png, dst);
  ra8_imgdec_image_t     out = {};

  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_decode(&dec, &req, &out));
  TEST_ASSERT_EQ(1U, spy.calls);
  /* The backend was told which container it holds, never `_none`. */
  TEST_ASSERT_EQ(k_ra8_imgdec_format_png, spy.seen);
  TEST_ASSERT_EQ(k_ra8_imgdec_format_png, out.format);

  TEST_END("fabric resolves an unstated format");
}

RA8_INTERNAL static void internal_test_fabric_gates_sniffed_format(void) {
  TEST_BEGIN("fabric gates a sniffed format");

  /* The stand-in opens WebP only; the bytes are a PNG. */
  sniff_spy_t  spy = {.calls = 0U, .seen = k_ra8_imgdec_format_none,
                      .caps_fmt = (uint32_t)k_ra8_imgdec_format_webp};
  ra8_imgdec_t dec = {.iface = &k_spy_iface, .ctx = &spy};

  const uint8_t png[] = {0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU, 0x00U};
  uint8_t       dst[k_dst_bytes] = {};

  const ra8_imgdec_req_t req = make_req(png, (uint32_t)sizeof png, dst);
  ra8_imgdec_image_t     out = {};

  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_imgdec_decode(&dec, &req, &out));
  TEST_ASSERT_EQ(0U, spy.calls);
  TEST_ASSERT_EQ(0U, out.width_px);

  TEST_END("fabric gates a sniffed format");
}

RA8_INTERNAL static void internal_test_fabric_refuses_unrecognised(void) {
  TEST_BEGIN("fabric refuses unrecognised bytes");

  sniff_spy_t  spy = {.calls = 0U, .seen = k_ra8_imgdec_format_none,
                      .caps_fmt = (uint32_t)k_ra8_imgdec_format_mask};
  ra8_imgdec_t dec = {.iface = &k_spy_iface, .ctx = &spy};

  const uint8_t junk[] = {0x12U, 0x34U, 0x56U, 0x78U};
  uint8_t       dst[k_dst_bytes] = {};

  const ra8_imgdec_req_t req = make_req(junk, (uint32_t)sizeof junk, dst);
  ra8_imgdec_image_t     out = {};

  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_imgdec_decode(&dec, &req, &out));
  TEST_ASSERT_EQ(0U, spy.calls);

  TEST_END("fabric refuses unrecognised bytes");
}

RA8_INTERNAL static void internal_test_declared_format_is_not_sniffed(void) {
  TEST_BEGIN("a declared format is taken as given");

  sniff_spy_t  spy = {.calls = 0U, .seen = k_ra8_imgdec_format_none,
                      .caps_fmt = (uint32_t)k_ra8_imgdec_format_tga};
  ra8_imgdec_t dec = {.iface = &k_spy_iface, .ctx = &spy};

  /* Bare geometry: no signature exists for TGA, so only a declaration opens it. */
  const uint8_t tga[] = {0x00U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U};
  uint8_t       dst[k_dst_bytes] = {};

  ra8_imgdec_req_t req = make_req(tga, (uint32_t)sizeof tga, dst);
  req.format           = k_ra8_imgdec_format_tga;
  ra8_imgdec_image_t out = {};

  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_decode(&dec, &req, &out));
  TEST_ASSERT_EQ(1U, spy.calls);
  TEST_ASSERT_EQ(k_ra8_imgdec_format_tga, spy.seen);

  TEST_END("a declared format is taken as given");
}

RA8_INTERNAL static void internal_test_sniff_window_is_honest(void) {
  TEST_BEGIN("sniff reads no more than its published window");

  /* Twelve bytes is the widest signature (RIFF + size + WEBP). */
  TEST_ASSERT_EQ(12U, (uint32_t)k_ra8_imgdec_sniff_bytes);

  uint8_t buf[k_ra8_imgdec_sniff_bytes] = {};
  memcpy(&buf[0], "RIFF", 4U);
  memcpy(&buf[8], "WEBP", 4U);

  ra8_imgdec_format_t got = k_ra8_imgdec_format_none;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_sniff(buf, (uint32_t)k_ra8_imgdec_sniff_bytes, &got));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_webp, got);

  TEST_END("sniff reads no more than its published window");
}

int main(void) {
  internal_test_png_and_jpeg();
  internal_test_webp_needs_both_fourccs();
  internal_test_gif_and_bmp();
  internal_test_sniff_refusals();
  internal_test_fabric_resolves_none();
  internal_test_fabric_gates_sniffed_format();
  internal_test_fabric_refuses_unrecognised();
  internal_test_declared_format_is_not_sniffed();
  internal_test_sniff_window_is_honest();
  return 0;
}
