/**
 * @file test_ra8_jpeg_imgdec.c
 * @brief Host tests for the software JPEG codec bound as an `ra8_imgdec`
 *        backend (RA8FW-308).
 *
 * @details Every case drives the backend through ::ra8_imgdec_decode rather
 * than calling the vtable directly, because the division of labour is the
 * point: the fabric owns the pointer and capability gates, this module owns
 * the geometry ceiling, the destination fit and the stride refusal. The
 * fixture JPEG is produced by the in-tree encoder, so no binary fixture file
 * or network input is needed.
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
#include "ra8_jpeg_imgdec.h"
#include "ra8_jpeg_sw.h"
#include "unity_minimal.h"

/** @brief Fixture sizes (no magic numbers). */
enum : uint32_t {
  k_t_w            = 16U,              /**< Frame width.        */
  k_t_h            = 16U,              /**< Frame height.       */
  k_t_bpp          = 3U,               /**< RGB888 bytes/pixel. */
  k_t_stride       = (16U * 3U),       /**< One packed row.     */
  k_t_rgb_bytes    = (16U * 16U * 3U), /**< Packed surface.     */
  k_t_jpeg_cap     = 4096U,            /**< Encoder out cap.    */
  k_t_mse_max      = 65U,              /**< MSE for ~30 dB.     */
  k_t_pad_stride   = (16U * 3U) + 4U,  /**< A padded stride.    */
  k_t_short_stride = 8U,               /**< Under one row.      */
  k_t_byte_mask    = 0xFFU,            /**< Truncates to byte.  */
};

/** @brief The fixture surface, encoded once and reused by every case. */
static uint8_t s_jpeg[k_t_jpeg_cap];

/** @brief Encoded length of ::s_jpeg. */
static uint32_t s_jpeg_len;

/** @brief Source pixels the fixture was encoded from. */
static uint8_t s_rgb_in[k_t_rgb_bytes];

/** @brief Destination the decodes write into. */
static uint8_t s_rgb_out[k_t_rgb_bytes];

/**
 * @brief Fill a packed RGB888 buffer with a smooth gradient.
 * @details Gives the encoder real content, so a decode that silently writes
 *          nothing is visible in the error metric rather than passing.
 * @param[out] rgb Packed RGB888 pixels to fill.
 * @param[in]  w   Frame width in pixels.
 * @param[in]  h   Frame height in pixels.
 * @pre @p rgb holds `w * h * 3` bytes.
 * @post Every byte of @p rgb is written.
 * @note File-local helper; no ownership escapes this executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_fill_gradient(uint8_t* rgb, uint32_t w, uint32_t h) {
  for (uint32_t y = 0U; y < h; ++y) {
    for (uint32_t x = 0U; x < w; ++x) {
      const uint32_t i = ((y * w) + x) * (uint32_t)k_t_bpp;
      rgb[i + 0U]      = (uint8_t)((x * 16U) & (uint32_t)k_t_byte_mask);
      rgb[i + 1U]      = (uint8_t)((y * 16U) & (uint32_t)k_t_byte_mask);
      rgb[i + 2U]      = (uint8_t)(((x + y) * 8U) & (uint32_t)k_t_byte_mask);
    }
  }
}

/**
 * @brief Mean squared error between two packed RGB888 buffers.
 * @details MSE rather than PSNR so the check needs no `log10()` on the link
 *          line.
 * @param[in] a First buffer.
 * @param[in] b Second buffer.
 * @param[in] n Bytes to compare.
 * @return The mean squared error over @p n bytes.
 * @retval value Zero when the buffers are identical.
 * @pre @p a and @p b each hold @p n bytes, and @p n is non-zero.
 * @post No state mutated.
 * @note File-local helper; no ownership escapes this executable.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_mse(const uint8_t* a, const uint8_t* b, uint32_t n) {
  uint64_t acc = 0U;
  for (uint32_t i = 0U; i < n; ++i) {
    const int32_t d = (int32_t)a[i] - (int32_t)b[i];
    acc += (uint64_t)(d * d);
  }
  return (uint32_t)(acc / (uint64_t)n);
}

/**
 * @brief Build the request every case starts from.
 * @details Declares no container, so the fabric sniffs, which is how a real
 *          consumer of the seam calls it.
 * @param[in] dst_bytes  Writable bytes at the destination.
 * @param[in] dst_stride Requested row stride, 0 for packed.
 * @param[in] want       Destination pixel layout.
 * @return The assembled request.
 * @retval value A request over the fixture JPEG and ::s_rgb_out.
 * @pre The fixture has been encoded.
 * @post No state mutated.
 * @note File-local helper; no ownership escapes this executable.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_imgdec_req_t
internal_req(uint32_t dst_bytes, uint32_t dst_stride, ra8_imgdec_pixel_t want) {
  const ra8_imgdec_req_t req = {
    .bytes      = s_jpeg,
    .byte_count = s_jpeg_len,
    .arena      = nullptr,
    .dst        = s_rgb_out,
    .dst_bytes  = dst_bytes,
    .dst_stride = dst_stride,
    .format     = k_ra8_imgdec_format_none,
    .want       = want,
  };
  return req;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it asserts the published capability
 * record, which holds no decision) @brief Verify the advertised capabilities.
 * @details Reads the record back through the fabric, which also proves it
 *          passes the fabric's own validity gate.
 * @pre The fixture has been encoded.
 * @post No state mutated beyond file-local fixtures.
 * @note File-local case; no ownership escapes this executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_caps_are_jpeg_rgb888_only(void) {
  TEST_BEGIN("jpeg backend advertises baseline JPEG into packed RGB888 only");

  ra8_imgdec_t dec = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jpeg_imgdec_bind(&dec));

  ra8_imgdec_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_get_caps(&dec, &caps));
  TEST_ASSERT_EQ((uint32_t)k_ra8_imgdec_format_jpeg, caps.formats);
  TEST_ASSERT_EQ((uint32_t)k_ra8_imgdec_pixel_rgb888, caps.pixels);
  TEST_ASSERT_EQ(0U, caps.scratch_bytes);
  TEST_ASSERT_EQ(0U, caps.scratch_align);
  TEST_ASSERT_EQ((uint32_t)k_ra8_imgdec_dim_max, caps.dim_max);
  TEST_ASSERT(!caps.streams);

  TEST_END("jpeg backend advertises baseline JPEG into packed RGB888 only");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the per-pair query it drives lives in
 * the fabric and carries its own vectors) @brief Verify the supported pairs.
 * @details One pair in, three neighbouring pairs out, so the advertised
 *          matrix is pinned rather than assumed.
 * @pre The backend binds.
 * @post No state mutated beyond file-local fixtures.
 * @note File-local case; no ownership escapes this executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_supported_pairs(void) {
  TEST_BEGIN("jpeg backend opens exactly one format/layout pair");

  ra8_imgdec_t dec = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jpeg_imgdec_bind(&dec));

  bool ok = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_supports(&dec, k_ra8_imgdec_format_jpeg,
                                                k_ra8_imgdec_pixel_rgb888, &ok));
  TEST_ASSERT(ok);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_supports(&dec, k_ra8_imgdec_format_jpeg,
                                                k_ra8_imgdec_pixel_rgba8888, &ok));
  TEST_ASSERT(!ok);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_supports(&dec, k_ra8_imgdec_format_png,
                                                k_ra8_imgdec_pixel_rgb888, &ok));
  TEST_ASSERT(!ok);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_supports(&dec, k_ra8_imgdec_format_webp,
                                                k_ra8_imgdec_pixel_rgb888, &ok));
  TEST_ASSERT(!ok);

  TEST_END("jpeg backend opens exactly one format/layout pair");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it is the happy path through the
 * fabric and the codec) @brief Verify a sniffed decode through the seam.
 * @details The request declares no container, so this also proves the
 *          fabric's sniff routes the encoder's own output to this backend.
 * @pre The fixture has been encoded.
 * @post ::s_rgb_out holds the decoded surface.
 * @note File-local case; no ownership escapes this executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_sniffed_decode_reports_the_surface(void) {
  TEST_BEGIN("a sniffed JPEG decodes and the result describes what was written");

  ra8_imgdec_t dec = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jpeg_imgdec_bind(&dec));

  (void)memset(s_rgb_out, 0, sizeof s_rgb_out);
  ra8_imgdec_req_t req =
    internal_req((uint32_t)k_t_rgb_bytes, 0U, k_ra8_imgdec_pixel_rgb888);
  ra8_imgdec_image_t img = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_decode(&dec, &req, &img));

  TEST_ASSERT_EQ((uint32_t)k_t_w, img.width_px);
  TEST_ASSERT_EQ((uint32_t)k_t_h, img.height_px);
  TEST_ASSERT_EQ((uint32_t)k_t_stride, img.stride);
  TEST_ASSERT_EQ((uint32_t)k_t_rgb_bytes, img.used_bytes);
  TEST_ASSERT_EQ((uint32_t)k_ra8_imgdec_format_jpeg, (uint32_t)img.format);
  TEST_ASSERT_EQ((uint32_t)k_ra8_imgdec_pixel_rgb888, (uint32_t)img.pixel);
  TEST_ASSERT(!img.had_alpha);

  const uint32_t mse = internal_mse(s_rgb_in, s_rgb_out, (uint32_t)k_t_rgb_bytes);
  TEST_ASSERT(mse < (uint32_t)k_t_mse_max);

  TEST_END("a sniffed JPEG decodes and the result describes what was written");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- a declared format skips the sniff in
 * the fabric, which carries its own vectors) @brief Verify a declared decode.
 * @details A caller that already knows the container must reach the same
 *          surface as one that leaves the fabric to sniff it.
 * @pre The fixture has been encoded.
 * @post ::s_rgb_out holds the decoded surface.
 * @note File-local case; no ownership escapes this executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_declared_format_matches_sniffed(void) {
  TEST_BEGIN("declaring JPEG reaches the same surface as sniffing it");

  ra8_imgdec_t dec = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jpeg_imgdec_bind(&dec));

  (void)memset(s_rgb_out, 0, sizeof s_rgb_out);
  ra8_imgdec_req_t req =
    internal_req((uint32_t)k_t_rgb_bytes, 0U, k_ra8_imgdec_pixel_rgb888);
  req.format             = k_ra8_imgdec_format_jpeg;
  ra8_imgdec_image_t img = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_decode(&dec, &req, &img));
  TEST_ASSERT_EQ((uint32_t)k_t_w, img.width_px);
  TEST_ASSERT_EQ((uint32_t)k_t_rgb_bytes, img.used_bytes);

  TEST_END("declaring JPEG reaches the same surface as sniffing it");
}

/**
 * @par MC/DC:
 * `internal_dst_ok` in `libs/ra8_jpeg/src/ra8_jpeg_imgdec.c`, the compound
 * `(dst_stride != 0) && (dst_stride > stride)`.
 * V1 stride 0 (C1=F, packed, accepted). V2 stride 52 (C1=T C2=T,
 * not_supported). V3 stride 8 (C1=T C2=F, caught by the narrower-than-a-row
 * refusal above it, invalid_size). N+1=3.
 * @brief Verify how the stride request is answered.
 * @details A padded stride is refused rather than ignored, because the codec
 *          writes packed rows and a sheared surface is worse than a refusal.
 * @pre The fixture has been encoded.
 * @post No surface is trusted after a refusal.
 * @note File-local case; no ownership escapes this executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_stride_requests(void) {
  TEST_BEGIN("a padded destination stride is refused, not silently ignored");

  ra8_imgdec_t dec = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jpeg_imgdec_bind(&dec));
  ra8_imgdec_image_t img = {};

  ra8_imgdec_req_t packed =
    internal_req((uint32_t)k_t_rgb_bytes, 0U, k_ra8_imgdec_pixel_rgb888);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_decode(&dec, &packed, &img));

  ra8_imgdec_req_t padded = internal_req((uint32_t)k_t_rgb_bytes,
                                         (uint32_t)k_t_pad_stride,
                                         k_ra8_imgdec_pixel_rgb888);
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_imgdec_decode(&dec, &padded, &img));
  TEST_ASSERT_EQ(0U, img.width_px);

  ra8_imgdec_req_t narrow = internal_req((uint32_t)k_t_rgb_bytes,
                                         (uint32_t)k_t_short_stride,
                                         k_ra8_imgdec_pixel_rgb888);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_imgdec_decode(&dec, &narrow, &img));
  TEST_ASSERT_EQ(0U, img.used_bytes);

  TEST_END("a padded destination stride is refused, not silently ignored");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the destination-size guard is a
 * single condition) @brief Verify a destination too small is refused.
 * @details One byte short of the packed surface, so the refusal is the size
 *          check and not some coarser rejection.
 * @pre The fixture has been encoded.
 * @post No surface is trusted after the refusal.
 * @note File-local case; no ownership escapes this executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_destination_too_small(void) {
  TEST_BEGIN("a destination one byte short of the surface is refused");

  ra8_imgdec_t dec = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jpeg_imgdec_bind(&dec));

  ra8_imgdec_req_t req =
    internal_req((uint32_t)k_t_rgb_bytes - 1U, 0U, k_ra8_imgdec_pixel_rgb888);
  ra8_imgdec_image_t img = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_imgdec_decode(&dec, &req, &img));
  TEST_ASSERT_EQ(0U, img.used_bytes);

  TEST_END("a destination one byte short of the surface is refused");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the capability gate it trips lives in
 * the fabric and carries its own vectors) @brief Verify an unadvertised layout
 * never reaches the codec.
 * @details RGBA8888 is refused by the gate, so the codec is never asked to
 *          write a layout it cannot produce.
 * @pre The fixture has been encoded.
 * @post The destination is left untouched.
 * @note File-local case; no ownership escapes this executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_unadvertised_layout_refused(void) {
  TEST_BEGIN("an RGBA destination is refused before the codec runs");

  ra8_imgdec_t dec = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jpeg_imgdec_bind(&dec));

  (void)memset(s_rgb_out, 0, sizeof s_rgb_out);
  ra8_imgdec_req_t req =
    internal_req((uint32_t)k_t_rgb_bytes, 0U, k_ra8_imgdec_pixel_rgba8888);
  ra8_imgdec_image_t img = {};
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_imgdec_decode(&dec, &req, &img));
  TEST_ASSERT_EQ(0U, s_rgb_out[0]);

  TEST_END("an RGBA destination is refused before the codec runs");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it drives the codec's own marker
 * walk, which carries its own vectors) @brief Verify a declared lie is caught.
 * @details A PNG declared as JPEG must be refused by the codec's marker walk,
 *          which is the backend's documented duty: a signature is not a
 *          container.
 * @pre The backend binds.
 * @post No surface is trusted after the refusal.
 * @note File-local case; no ownership escapes this executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_declared_format_is_still_verified(void) {
  TEST_BEGIN("a PNG declared as JPEG is refused by the codec, not accepted");

  ra8_imgdec_t dec = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jpeg_imgdec_bind(&dec));

  static const uint8_t s_png[] = {0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU,
                                  0x1AU, 0x0AU, 0x00U, 0x00U, 0x00U, 0x0DU};
  const ra8_imgdec_req_t req   = {
      .bytes      = s_png,
      .byte_count = (uint32_t)sizeof s_png,
      .arena      = nullptr,
      .dst        = s_rgb_out,
      .dst_bytes  = (uint32_t)k_t_rgb_bytes,
      .dst_stride = 0U,
      .format     = k_ra8_imgdec_format_jpeg,
      .want       = k_ra8_imgdec_pixel_rgb888,
  };
  ra8_imgdec_image_t img = {};
  TEST_ASSERT(ra8_imgdec_decode(&dec, &req, &img) != k_ra8_ok);
  TEST_ASSERT_EQ(0U, img.width_px);

  TEST_END("a PNG declared as JPEG is refused by the codec, not accepted");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the binder holds one null guard)
 * @brief Verify the binder's null guard and what it writes.
 * @details Also pins the deliberate NULL context: the codec's state is
 *          module-static, so a per-handle context would claim an
 *          independence it does not have.
 * @pre None.
 * @post No state mutated beyond the local handle.
 * @note File-local case; no ownership escapes this executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_bind_guards(void) {
  TEST_BEGIN("bind refuses a null handle and binds a stateless one");

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_jpeg_imgdec_bind(nullptr));

  ra8_imgdec_t dec = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jpeg_imgdec_bind(&dec));
  TEST_ASSERT_NOT_NULL(dec.iface);
  TEST_ASSERT_NULL(dec.ctx);

  ra8_imgdec_t again = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jpeg_imgdec_bind(&again));
  TEST_ASSERT(again.iface == dec.iface);

  TEST_END("bind refuses a null handle and binds a stateless one");
}

int main(void) {
  internal_fill_gradient(s_rgb_in, (uint32_t)k_t_w, (uint32_t)k_t_h);
  const ra8_err_t enc = ra8_jpeg_sw_encode(s_rgb_in, (uint16_t)k_t_w, (uint16_t)k_t_h,
                                           (uint8_t)k_ra8_jpeg_sw_quality_high, s_jpeg,
                                           (uint32_t)k_t_jpeg_cap, &s_jpeg_len);
  TEST_ASSERT_EQ(k_ra8_ok, enc);

  internal_test_caps_are_jpeg_rgb888_only();
  internal_test_supported_pairs();
  internal_test_sniffed_decode_reports_the_surface();
  internal_test_declared_format_matches_sniffed();
  internal_test_stride_requests();
  internal_test_destination_too_small();
  internal_test_unadvertised_layout_refused();
  internal_test_declared_format_is_still_verified();
  internal_test_bind_guards();
  return 0;
}
