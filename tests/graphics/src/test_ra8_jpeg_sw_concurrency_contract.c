/**
 * @file test_ra8_jpeg_sw_concurrency_contract.c
 * @brief Pins the documented concurrency contract of the software JPEG codec.
 *
 * @details
 * `ra8_jpeg_sw.h` used to advertise `ra8_jpeg_sw_decode()` as
 * "thread-safe (re-entrant)" and `ra8_jpeg_sw_encode()` as
 * "thread-safe" while both keep their working state in module-static
 * objects (`s_d`, `s_e`, the `s_*_strip` buffers, `s_js`). #893
 * resolved that contradiction in favour of the implementation: the
 * statics stay, because the project budgets stack with
 * `-Wstack-usage` and forbids the heap, and the header now states the
 * real serialisation requirement.
 *
 * This suite is the regression test for that contract. It cannot
 * assert the negative half directly (a host unit test has one thread,
 * and racing the codec deliberately would be undefined behaviour), so
 * it pins the two halves that ARE observable and that any future
 * refactor would break:
 *
 *   1. Sequential reuse leaks no state. Every entry point zeroes its
 *      context on entry, so interleaving whole images in any order
 *      must give byte-identical results to running them alone. If
 *      someone moves state out of the per-call zeroing, this fails.
 *   2. `ra8_jpeg_sw_get_dimensions()` really is re-entrant. It is the
 *      only entry point the header still promises is safe to call
 *      concurrently, so it is exercised from inside a live
 *      `ra8_jpeg_sw_decode_stripes()` session, where a single byte of
 *      shared mutable state would derail the outer decode. If someone
 *      gives the dimension probe a static cache, this fails.
 *
 * Results are compared with FNV-1a-32 over the produced bytes, the
 * same hash the parity suite uses.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_jpeg_sw.h"
#include "unity_minimal.h"

/** @brief Fixture capacities and hash constants. */
enum : uint32_t {
  k_cc_a_w         = 64U,            /**< Image A width.            */
  k_cc_a_h         = 48U,            /**< Image A height.           */
  k_cc_b_w         = 32U,            /**< Image B width.            */
  k_cc_b_h         = 32U,            /**< Image B height.           */
  k_cc_rgb_cap     = 64U * 48U * 3U, /**< Largest RGB888 buffer.    */
  k_cc_jpeg_cap    = 32768U,         /**< Encoder output cap.       */
  k_cc_window_cap  = 131072U,        /**< Stream window (min).      */
  k_cc_stripe_cap  = 16U * 64U * 3U, /**< Stream stripe buffer.     */
  k_cc_fnv_offset  = 2166136261U,    /**< FNV-1a-32 offset basis.   */
  k_cc_fnv_prime   = 16777619U,      /**< FNV-1a-32 prime.          */
  k_cc_quality     = 80U,            /**< Encoder quality used.     */
  k_cc_repeat      = 3U,             /**< Interleaved round count.  */
};

/** @brief Gradient generator coefficients (arbitrary but frozen). */
enum : uint8_t {
  k_cc_gr_rx = 7U, /**< Red x coefficient.    */
  k_cc_gr_gy = 5U, /**< Green y coefficient.  */
  k_cc_gr_b  = 3U, /**< Blue (x^y) multiplier. */
  k_cc_byte  = 0xFFU, /**< Byte mask.          */
};

static uint8_t s_cc_rgb_a[k_cc_rgb_cap];  /**< Source pixels, image A.   */
static uint8_t s_cc_rgb_b[k_cc_rgb_cap];  /**< Source pixels, image B.   */
static uint8_t s_cc_jpeg_a[k_cc_jpeg_cap]; /**< Encoded bytes, image A.  */
static uint8_t s_cc_jpeg_b[k_cc_jpeg_cap]; /**< Encoded bytes, image B.  */
static uint8_t s_cc_dec[k_cc_rgb_cap];     /**< Decode scratch.          */
static uint8_t s_cc_window[k_cc_window_cap]; /**< Stream pull window.    */
static uint8_t s_cc_stripe[k_cc_stripe_cap]; /**< Stream stripe buffer.  */
static uint32_t s_cc_len_a;                /**< Encoded length, image A. */
static uint32_t s_cc_len_b;                /**< Encoded length, image B. */

/**
 * @brief FNV-1a-32 over a byte range.
 * @param[in] buf Bytes to hash (non-NULL).
 * @param[in] len Byte count.
 * @return The 32-bit digest.
 * @pre `buf` covers `len` readable bytes.
 * @post No state outside the return value is touched.
 * @note File-local helper; no ownership escapes this test executable.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_cc_fnv(const uint8_t* buf, uint32_t len)
{
  uint32_t h = (uint32_t)k_cc_fnv_offset;
  for (uint32_t i = 0U; i < len; i++) {
    h ^= (uint32_t)buf[i];
    h *= (uint32_t)k_cc_fnv_prime;
  }
  return h;
}

/**
 * @brief Fill a buffer with a deterministic RGB888 gradient.
 * @param[out] dst  Destination pixels (non-NULL).
 * @param[in]  w    Image width in pixels.
 * @param[in]  h    Image height in pixels.
 * @param[in]  seed Per-image offset so the two fixtures differ.
 * @pre `dst` covers `w * h * 3` writable bytes.
 * @post Every byte of the image is written.
 * @note File-local helper; no ownership escapes this test executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_cc_gradient(uint8_t* dst, uint16_t w, uint16_t h, uint8_t seed)
{
  uint32_t i = 0U;
  for (uint16_t y = 0U; y < h; y++) {
    for (uint16_t x = 0U; x < w; x++) {
      dst[i]      = (uint8_t)(((uint32_t)x * k_cc_gr_rx + seed) & k_cc_byte);
      dst[i + 1U] = (uint8_t)(((uint32_t)y * k_cc_gr_gy + seed) & k_cc_byte);
      dst[i + 2U] = (uint8_t)((((uint32_t)x ^ (uint32_t)y) * k_cc_gr_b) & k_cc_byte);
      i += 3U;
    }
  }
}

/**
 * @brief Encode one fixture image into a caller buffer.
 * @param[in]  rgb     Source pixels (non-NULL).
 * @param[in]  w       Image width.
 * @param[in]  h       Image height.
 * @param[out] dst     Encoded output (non-NULL).
 * @param[out] out_len Receives the encoded length (non-NULL).
 * @return The FNV-1a-32 digest of the encoded bytes.
 * @pre `dst` covers `k_cc_jpeg_cap` writable bytes.
 * @post `*out_len` holds the produced length on success.
 * @note File-local helper; no ownership escapes this test executable.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t
internal_cc_encode(const uint8_t* rgb, uint16_t w, uint16_t h, uint8_t* dst, uint32_t* out_len)
{
  uint32_t produced = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_jpeg_sw_encode(rgb,
                                    w,
                                    h,
                                    (uint8_t)k_cc_quality,
                                    dst,
                                    (uint32_t)k_cc_jpeg_cap,
                                    &produced));
  *out_len = produced;
  return internal_cc_fnv(dst, produced);
}

/**
 * @brief Decode one encoded fixture and hash the RGB output.
 * @param[in] jpeg Encoded bytes (non-NULL).
 * @param[in] len  Encoded length.
 * @return The FNV-1a-32 digest of the decoded pixels.
 * @pre `jpeg` covers `len` readable bytes holding a baseline JPEG.
 * @post Only the file-local decode scratch buffer is written.
 * @note File-local helper; no ownership escapes this test executable.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_cc_decode(const uint8_t* jpeg, uint32_t len)
{
  uint16_t dw = 0U;
  uint16_t dh = 0U;
  memset(s_cc_dec, 0, sizeof(s_cc_dec));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_jpeg_sw_decode(jpeg, len, s_cc_dec, (uint32_t)sizeof(s_cc_dec), &dw, &dh));
  return internal_cc_fnv(s_cc_dec, (uint32_t)dw * (uint32_t)dh * 3U);
}

/**
 * @test contract_sequential_reuse_leaks_no_state
 * @brief Interleaved whole-image calls match the same calls run alone.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it pins an equivalence: encoding and
 * decoding image A around image B must give the digests A gives on its own.
 * It drives no decision to independent influence.) @details Encodes and
 * decodes two distinct fixtures in an interleaved order and asserts every
 * repetition reproduces the first digest exactly. @pre Fixed-capacity fixture
 * storage required by this operation is available. @post Mutations remain
 * confined to file-local fixture state. @note File-local helper; no ownership
 * escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_contract_sequential_reuse_leaks_no_state(void)
{
  const uint32_t enc_a = internal_cc_encode(s_cc_rgb_a,
                                            (uint16_t)k_cc_a_w,
                                            (uint16_t)k_cc_a_h,
                                            s_cc_jpeg_a,
                                            &s_cc_len_a);
  const uint32_t enc_b = internal_cc_encode(s_cc_rgb_b,
                                            (uint16_t)k_cc_b_w,
                                            (uint16_t)k_cc_b_h,
                                            s_cc_jpeg_b,
                                            &s_cc_len_b);
  const uint32_t dec_a = internal_cc_decode(s_cc_jpeg_a, s_cc_len_a);
  const uint32_t dec_b = internal_cc_decode(s_cc_jpeg_b, s_cc_len_b);

  for (uint32_t round = 0U; round < (uint32_t)k_cc_repeat; round++) {
    uint32_t len = 0U;

    /* B between two A encodes: the encoder context and the strip
     * buffers must not carry B's geometry into A. */
    TEST_ASSERT_EQ(enc_b,
                   internal_cc_encode(s_cc_rgb_b,
                                      (uint16_t)k_cc_b_w,
                                      (uint16_t)k_cc_b_h,
                                      s_cc_jpeg_b,
                                      &len));
    TEST_ASSERT_EQ(s_cc_len_b, len);
    TEST_ASSERT_EQ(enc_a,
                   internal_cc_encode(s_cc_rgb_a,
                                      (uint16_t)k_cc_a_w,
                                      (uint16_t)k_cc_a_h,
                                      s_cc_jpeg_a,
                                      &len));
    TEST_ASSERT_EQ(s_cc_len_a, len);

    /* Same for the decoder: B's Huffman and quantisation tables must
     * not survive into A's decode. */
    TEST_ASSERT_EQ(dec_b, internal_cc_decode(s_cc_jpeg_b, s_cc_len_b));
    TEST_ASSERT_EQ(dec_a, internal_cc_decode(s_cc_jpeg_a, s_cc_len_a));
  }
  TEST_END("jpeg_sw contract: sequential reuse leaks no state between calls");
}

/**
 * @struct cc_pull_ctx_t
 * @brief Pull-source cursor over the encoded image A bytes.
 */
typedef struct {
  uint32_t len;   /**< Total bytes available.                       */
  uint32_t pos;   /**< Bytes already delivered.                     */
  uint32_t probes; /**< Re-entrant get_dimensions calls made so far. */
  uint32_t probe_fail; /**< Probes that returned the wrong answer.  */
} cc_pull_ctx_t;

/**
 * @struct cc_sink_t
 * @brief Row sink accumulating the streamed pixels.
 */
typedef struct {
  uint32_t hash; /**< Running FNV-1a-32 over emitted rows. */
  uint32_t rows; /**< Rows emitted so far.                 */
} cc_sink_t;

/**
 * @brief Pull callback that probes dimensions of a DIFFERENT image mid-session.
 * @param[in]  ctx      The `cc_pull_ctx_t` cursor (non-NULL).
 * @param[out] out_buf  Destination for delivered bytes (non-NULL).
 * @param[in]  want     Bytes requested.
 * @param[out] out_got  Receives bytes delivered (non-NULL).
 * @return Result code; always `k_ra8_ok` for this fixture.
 * @pre `out_buf` covers `want` writable bytes.
 * @post `*out_got` is 0 at end of stream.
 * @note File-local helper; no ownership escapes this test executable.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_cc_pull(void* ctx, uint8_t* out_buf, size_t want, size_t* out_got)
{
  cc_pull_ctx_t* pc = (cc_pull_ctx_t*)ctx;

  /* The header promises ra8_jpeg_sw_get_dimensions() is re-entrant, so
   * calling it here -- inside a live streaming decode, on a different
   * image -- must not disturb the session in progress. */
  uint16_t  pw = 0U;
  uint16_t  ph = 0U;
  ra8_err_t pe = ra8_jpeg_sw_get_dimensions(s_cc_jpeg_b, s_cc_len_b, &pw, &ph);
  pc->probes++;
  if (pe != k_ra8_ok || pw != (uint16_t)k_cc_b_w || ph != (uint16_t)k_cc_b_h) {
    pc->probe_fail++;
  }

  size_t left = (size_t)(pc->len - pc->pos);
  size_t give = (want < left) ? want : left;
  if (give > 0U) {
    memcpy(out_buf, &s_cc_jpeg_a[pc->pos], give);
    pc->pos += (uint32_t)give;
  }
  *out_got = give;
  return k_ra8_ok;
}

/**
 * @brief Geometry callback; hands the decoder the file-local stripe buffer.
 * @param[in]  ctx            The `cc_sink_t` accumulator (non-NULL).
 * @param[in]  width          Image width, pixels.
 * @param[in]  height         Image height, pixels.
 * @param[in]  channels       Output channels per pixel.
 * @param[in]  stripe_rows    Rows per emitted stripe.
 * @param[out] out_stripe     Receives the stripe buffer (non-NULL).
 * @param[out] out_stripe_cap Receives that buffer's capacity (non-NULL).
 * @return `k_ra8_ok`, or `k_ra8_err_invalid_size` if the fixture buffer is short.
 * @pre `ctx` points at a live sink.
 * @post On success both outputs reference the file-local stripe buffer.
 * @note File-local helper; no ownership escapes this test executable.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cc_on_geom(void*     ctx,
                                                 uint16_t  width,
                                                 uint16_t  height,
                                                 uint8_t   channels,
                                                 uint16_t  stripe_rows,
                                                 uint8_t** out_stripe,
                                                 uint32_t* out_stripe_cap)
{
  (void)ctx;
  (void)height;
  const uint32_t need = (uint32_t)stripe_rows * (uint32_t)width * (uint32_t)channels;
  if (need > (uint32_t)sizeof(s_cc_stripe)) {
    return k_ra8_err_invalid_size;
  }
  *out_stripe     = s_cc_stripe;
  *out_stripe_cap = (uint32_t)sizeof(s_cc_stripe);
  return k_ra8_ok;
}

/**
 * @brief Row callback folding emitted pixels into the sink digest.
 * @param[in] ctx      The `cc_sink_t` accumulator (non-NULL).
 * @param[in] px       Emitted stripe pixels (non-NULL).
 * @param[in] width    Row width in pixels.
 * @param[in] y0       Image row of the stripe's first row.
 * @param[in] nrows    Row count in this stripe.
 * @param[in] channels Bytes per pixel.
 * @return Always `k_ra8_ok`.
 * @pre `px` covers `nrows * width * channels` readable bytes.
 * @post The sink's digest and row count advance by this stripe.
 * @note File-local helper; no ownership escapes this test executable.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cc_on_rows(void*          ctx,
                                                 const uint8_t* px,
                                                 uint16_t       width,
                                                 uint16_t       y0,
                                                 uint16_t       nrows,
                                                 uint8_t        channels)
{
  (void)y0;
  cc_sink_t*     sk     = (cc_sink_t*)ctx;
  const uint8_t* rows   = px;
  uint16_t       count  = nrows;
  uint32_t       nbytes = (uint32_t)count * (uint32_t)width * (uint32_t)channels;
  uint32_t   h      = sk->hash;
  for (uint32_t i = 0U; i < nbytes; i++) {
    h ^= (uint32_t)rows[i];
    h *= (uint32_t)k_cc_fnv_prime;
  }
  sk->hash = h;
  sk->rows += (uint32_t)count;
  return k_ra8_ok;
}

/**
 * @test contract_get_dimensions_is_reentrant
 * @brief The dimension probe is safe to call from inside a live stream session.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it pins an equivalence: a streaming
 * decode that probes a second image from its pull callback must emit the same
 * rows and pixels as the whole-buffer decode of the same image. It drives no
 * decision to independent influence.) @details Runs
 * `ra8_jpeg_sw_decode_stripes()` over image A while every pull calls
 * `ra8_jpeg_sw_get_dimensions()` on image B, then asserts both the probe
 * answers and the streamed pixels. @pre Fixed-capacity fixture storage
 * required by this operation is available. @post Mutations remain confined to
 * file-local fixture state. @note File-local helper; no ownership escapes this
 * focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_contract_get_dimensions_is_reentrant(void)
{
  const uint32_t want_hash = internal_cc_decode(s_cc_jpeg_a, s_cc_len_a);

  cc_pull_ctx_t pc = {s_cc_len_a, 0U, 0U, 0U};
  cc_sink_t     sk = {(uint32_t)k_cc_fnv_offset, 0U};

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_jpeg_sw_decode_stripes(internal_cc_pull,
                                            &pc,
                                            s_cc_window,
                                            (uint32_t)sizeof(s_cc_window),
                                            internal_cc_on_geom,
                                            internal_cc_on_rows,
                                            &sk));

  /* The probe actually ran, always answered correctly, and the outer
   * streaming session was unaffected by it. */
  TEST_ASSERT(pc.probes > 0U);
  TEST_ASSERT_EQ(0U, pc.probe_fail);
  TEST_ASSERT_EQ((uint32_t)k_cc_a_h, sk.rows);
  TEST_ASSERT_EQ(want_hash, sk.hash);

  /* And the stream session left nothing behind for the next caller. */
  TEST_ASSERT_EQ(want_hash, internal_cc_decode(s_cc_jpeg_a, s_cc_len_a));
  TEST_END("jpeg_sw contract: get_dimensions is re-entrant inside a live stream");
}

int main(void)
{
  ra8_fake_mmap_reset();
  internal_cc_gradient(s_cc_rgb_a, (uint16_t)k_cc_a_w, (uint16_t)k_cc_a_h, 0U);
  internal_cc_gradient(s_cc_rgb_b, (uint16_t)k_cc_b_w, (uint16_t)k_cc_b_h, 129U);
  internal_test_contract_sequential_reuse_leaks_no_state();
  internal_test_contract_get_dimensions_is_reentrant();
  return 0;
}
