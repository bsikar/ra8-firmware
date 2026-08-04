/**
 * @file test_ra8_jpeg_sw_parity.c
 * @brief Byte-parity goldens for the software JPEG codec.
 *
 * @details
 * Pins the exact byte stream of `ra8_jpeg_sw_encode()` and the exact
 * RGB output of `ra8_jpeg_sw_decode()` / `ra8_jpeg_sw_decode_stripes()`
 * with FNV-1a-32 hashes over a matrix of image sizes (MCU-aligned and
 * edge-cropped) and the full quality range. The golden values were
 * captured from the codec before the function-size decomposition of
 * the `ra8_jpeg_sw*` translation units; any refactor that changes a
 * single output byte -- header emission order, DCT rounding, chroma
 * sub-sampling, entropy packing, stripe windowing -- trips this suite.
 *
 * The codec is pure integer arithmetic, so the hashes are identical
 * on the x86_64 test host, in ra8_emulator, and on the Cortex-M85 target.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_jpeg_sw.h"
#include "unity_minimal.h"

/**
 * @enum jpeg_sw_parity_fixture_t
 * @brief The byte-level helpers.
 */
typedef enum : uint8_t {
  k_byte_mask = 0xFFU, /**< Truncates a generated or shifted value back into a byte. */
} jpeg_sw_parity_fixture_t;

/** @brief Buffer capacities and hash constants. */
enum : uint32_t {
  k_pt_max_w      = 160U,             /**< Largest golden width.   */
  k_pt_max_h      = 120U,             /**< Largest golden height.  */
  k_pt_rgb_cap    = 160U * 120U * 3U, /**< RGB888 buffer size.     */
  k_pt_jpeg_cap   = 32768U,           /**< Encoder output cap.     */
  k_pt_fnv_offset = 2166136261U,      /**< FNV-1a-32 offset basis. */
  k_pt_fnv_prime  = 16777619U,        /**< FNV-1a-32 prime.        */
  k_pt_window_cap = 131072U,          /**< Stream decode window.   */
};

/** @brief Gradient generator coefficients (arbitrary but frozen). */
enum : uint8_t {
  k_pt_gr_rx = 7U, /**< Red x coefficient.     */
  k_pt_gr_gy = 5U, /**< Green y coefficient.   */
  k_pt_gr_b  = 3U, /**< Blue (x^y) multiplier. */
};

/**
 * @struct pt_golden_t
 * @brief One pinned encoder/decoder golden vector.
 *
 * @details Captured from the pre-refactor codec on the deterministic
 *          gradient input; see the table below.
 *
 * @invariant `enc_len <= k_pt_jpeg_cap`.
 * @since 0.1.0
 */
typedef struct {
  uint16_t w;       /**< Image width in pixels.              */
  uint16_t h;       /**< Image height in pixels.             */
  uint8_t  quality; /**< Encoder quality setting.            */
  uint32_t enc_len; /**< Exact encoded stream length, bytes. */
  uint32_t enc_fnv; /**< FNV-1a-32 of the encoded stream.    */
  uint32_t dec_fnv; /**< FNV-1a-32 of the decoded RGB888.    */
} pt_golden_t;

/**
 * @brief Pinned golden vectors (5 geometries x 6 qualities).
 *
 * @details
 * Geometries cover a single MCU (16x16), horizontal-only crop (24x17),
 * both-axis crop (33x21), a multi-MCU aligned frame (64x48) and the
 * ereader cover-thumbnail shape (160x120). Qualities cover both sides
 * of the IJG pivot (50) plus both extremes.
 *
 * @note Regenerate ONLY on an intentional codec output change; the
 *       whole point of the table is that refactors must not move it.
 * @since 0.1.0
 */
static const pt_golden_t k_pt_goldens[] = {
  {16, 16, 1, 626, 0x81B00954U, 0xEDE5FA05U},     {16, 16, 10, 632, 0xBB0CBA32U, 0x19D7D1FBU},
  {16, 16, 50, 640, 0x59E04B54U, 0x607F851DU},    {16, 16, 75, 651, 0x7091960CU, 0xA56A5396U},
  {16, 16, 90, 674, 0x44977EA0U, 0x572AAE73U},    {16, 16, 100, 779, 0x309D6043U, 0x3E5A6F09U},
  {24, 17, 1, 641, 0xDA4A7B50U, 0x6F6199ADU},     {24, 17, 10, 655, 0x8992A77CU, 0x9C11C5A2U},
  {24, 17, 50, 681, 0x4878102BU, 0x065FB9CBU},    {24, 17, 75, 712, 0xECD39F51U, 0x86D59225U},
  {24, 17, 90, 765, 0x8BD4BF0FU, 0x6494E059U},    {24, 17, 100, 1017, 0x11E92C93U, 0xC3EFC60FU},
  {33, 21, 1, 650, 0x290C5C06U, 0x99E82AD2U},     {33, 21, 10, 674, 0x4C65DB0FU, 0x655A9967U},
  {33, 21, 50, 723, 0xB40FB441U, 0x94AF8297U},    {33, 21, 75, 762, 0xD5BFCBBBU, 0x97D01565U},
  {33, 21, 90, 865, 0x3E9A8BC5U, 0xA97C673EU},    {33, 21, 100, 1318, 0x5B6D106AU, 0x28FB6286U},
  {64, 48, 1, 698, 0x6B8A0A94U, 0x7F6E8492U},     {64, 48, 10, 793, 0x360CA4CEU, 0x22D19309U},
  {64, 48, 50, 1043, 0x1ED6E914U, 0x57F6B8DCU},   {64, 48, 75, 1293, 0xEB409B20U, 0xC0EEBF52U},
  {64, 48, 90, 1706, 0x3A643C49U, 0xABC8A28FU},   {64, 48, 100, 3459, 0xD4BB2A79U, 0xF89642B6U},
  {160, 120, 1, 1205, 0x98156E13U, 0x09F98FA9U},  {160, 120, 10, 1886, 0x05B3E6CCU, 0x0874B58AU},
  {160, 120, 50, 3884, 0x440CE10AU, 0x45CB3C9DU}, {160, 120, 75, 5728, 0x72956370U, 0x4DA980B0U},
  {160, 120, 90, 8785, 0x9902E0C9U, 0x10D4681BU}, {160, 120, 100, 21141, 0x6276E54FU, 0xC7D07ACFU},
};

/** @brief Number of golden vectors. */
enum : uint32_t {
  k_pt_golden_count =
    (uint32_t)(sizeof(k_pt_goldens) / sizeof(k_pt_goldens[0])), /**< Pt golden count. */
};

/** @brief Shared scratch buffers (large; keep off the stack). */
static uint8_t s_pt_rgb[k_pt_rgb_cap];
/** @brief Encoder output scratch. */
static uint8_t s_pt_jpeg[k_pt_jpeg_cap];
/** @brief Decoder output scratch. */
static uint8_t s_pt_dec[k_pt_rgb_cap];
/** @brief Streaming decoder sliding window. */
static uint8_t s_pt_window[k_pt_window_cap];
/** @brief Streaming decoder stripe buffer (worst case 4:2:0). */
static uint8_t s_pt_stripe[(uint32_t)k_pt_max_w * 16U * 3U];

/** @brief Fold FNV-1a-32 over `n` bytes starting from `h`. */
static uint32_t pt_fnv1a32(uint32_t h, const uint8_t* p, uint32_t n)
{
  for (uint32_t i = 0U; i < n; i++) {
    h ^= p[i];
    h *= (uint32_t)k_pt_fnv_prime;
  }
  return h;
}

/** @brief Fill the shared RGB buffer with the frozen deterministic gradient. */
static void pt_fill_gradient(uint16_t w, uint16_t h)
{
  for (uint16_t y = 0U; y < h; y++) {
    for (uint16_t x = 0U; x < w; x++) {
      uint32_t i       = (((uint32_t)y * w) + x) * 3U;
      s_pt_rgb[i]      = (uint8_t)((((uint32_t)x * k_pt_gr_rx) + y) & k_byte_mask);
      s_pt_rgb[i + 1U] = (uint8_t)((((uint32_t)y * k_pt_gr_gy) + (2U * (uint32_t)x)) & k_byte_mask);
      s_pt_rgb[i + 2U] = (uint8_t)((((uint32_t)x ^ y) * k_pt_gr_b) & k_byte_mask);
    }
  }
}

/** @brief Encode golden vector `g` into `s_pt_jpeg`; returns the length. */
static uint32_t pt_encode(const pt_golden_t* g)
{
  pt_fill_gradient(g->w, g->h);
  uint32_t len = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_jpeg_sw_encode(s_pt_rgb, g->w, g->h, g->quality, s_pt_jpeg, sizeof(s_pt_jpeg), &len));
  return len;
}

/**
 * @test parity_encoder_bytes
 * @brief The encoder byte stream is byte-identical to the pinned goldens.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- a golden byte-parity check: it pins
 * ra8_jpeg_sw_encode's exact output length and FNV-1a-32 hash across 30
 * geometry/quality vectors. It drives no decision to independent influence; the
 * codec's own compound decisions are covered by the ra8_jpeg_sw unit tests.)
 */
static void test_parity_encoder_bytes(void)
{
  for (uint32_t i = 0U; i < k_pt_golden_count; i++) {
    const pt_golden_t* g   = &k_pt_goldens[i];
    uint32_t           len = pt_encode(g);
    TEST_ASSERT_EQ(g->enc_len, len);
    TEST_ASSERT_EQ(g->enc_fnv, pt_fnv1a32((uint32_t)k_pt_fnv_offset, s_pt_jpeg, len));
  }
  TEST_END("jpeg_sw parity: encoder byte stream matches goldens");
}

/**
 * @test parity_decoder_pixels
 * @brief Decoded RGB output is byte-identical to the pinned goldens.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- a golden pixel-parity check: it pins
 * ra8_jpeg_sw_get_dimensions and ra8_jpeg_sw_decode's exact RGB888 output
 * (FNV-1a-32) across the same 30 vectors. It drives no decision to independent
 * influence; the codec's own compound decisions are covered by the ra8_jpeg_sw
 * unit tests.)
 */
static void test_parity_decoder_pixels(void)
{
  for (uint32_t i = 0U; i < k_pt_golden_count; i++) {
    const pt_golden_t* g   = &k_pt_goldens[i];
    uint32_t           len = pt_encode(g);

    uint16_t dw = 0U;
    uint16_t dh = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_jpeg_sw_get_dimensions(s_pt_jpeg, len, &dw, &dh));
    TEST_ASSERT_EQ(g->w, dw);
    TEST_ASSERT_EQ(g->h, dh);

    memset(s_pt_dec, 0, sizeof(s_pt_dec));
    TEST_ASSERT_EQ(k_ra8_ok,
                   ra8_jpeg_sw_decode(s_pt_jpeg, len, s_pt_dec, sizeof(s_pt_dec), &dw, &dh));
    uint32_t nbytes = (uint32_t)dw * dh * 3U;
    TEST_ASSERT_EQ(g->dec_fnv, pt_fnv1a32((uint32_t)k_pt_fnv_offset, s_pt_dec, nbytes));
  }
  TEST_END("jpeg_sw parity: decoder RGB output matches goldens");
}

/** @brief Streaming pull source over the encoded scratch buffer. */
typedef struct {
  uint32_t len; /**< Total encoded bytes.    */
  uint32_t pos; /**< Bytes delivered so far. */
} pt_pull_ctx_t;

/** @brief Streaming sink state: running hash + row accounting. */
typedef struct {
  uint32_t hash; /**< Running FNV-1a-32 of emitted rows. */
  uint32_t rows; /**< Total rows emitted.                */
} pt_sink_t;

/** @brief Pull callback: hands out `s_pt_jpeg` strictly in order. */
static ra8_err_t pt_pull(void* ctx, uint8_t* buf, size_t cap, size_t* got)
{
  pt_pull_ctx_t* p    = (pt_pull_ctx_t*)ctx;
  size_t         left = (size_t)(p->len - p->pos);
  size_t         n    = (left < cap) ? left : cap;
  memcpy(buf, &s_pt_jpeg[p->pos], n);
  p->pos += (uint32_t)n;
  *got = n;
  return k_ra8_ok;
}

/** @brief Geometry callback: hands the decoder the static stripe buffer. */
static ra8_err_t pt_on_geom(void*     ctx,
                            uint16_t  width,
                            uint16_t  height,
                            uint8_t   channels,
                            uint16_t  stripe_rows,
                            uint8_t** out_stripe,
                            uint32_t* out_stripe_cap)
{
  (void)ctx;
  (void)width;
  (void)height;
  (void)channels;
  (void)stripe_rows;
  *out_stripe     = s_pt_stripe;
  *out_stripe_cap = (uint32_t)sizeof(s_pt_stripe);
  return k_ra8_ok;
}

/** @brief Row sink: folds every emitted stripe into the running hash. */
static ra8_err_t pt_on_rows(void*          ctx,
                            const uint8_t* px,
                            uint16_t       width,
                            uint16_t       y0,
                            uint16_t       nrows,
                            uint8_t        channels)
{
  (void)y0;
  pt_sink_t* s = (pt_sink_t*)ctx;
  s->hash      = pt_fnv1a32(s->hash, px, (uint32_t)width * nrows * channels);
  s->rows += nrows;
  return k_ra8_ok;
}

/**
 * @test parity_stream_equals_whole_buffer
 * @brief The streaming stripe decoder emits byte-identical pixels.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- a golden parity check: the streaming
 * stripe decoder ra8_jpeg_sw_decode_stripes must emit the same row count and
 * pixels (FNV-1a-32) as the whole-buffer goldens. It drives no decision to
 * independent influence; the streaming decoder's compound decisions are covered
 * in test_ra8_jpeg_sw_stream.c.)
 */
static void test_parity_stream_equals_whole_buffer(void)
{
  for (uint32_t i = 0U; i < k_pt_golden_count; i++) {
    const pt_golden_t* g   = &k_pt_goldens[i];
    uint32_t           len = pt_encode(g);

    pt_pull_ctx_t pc = {len, 0U};
    pt_sink_t     sk = {(uint32_t)k_pt_fnv_offset, 0U};
    TEST_ASSERT_EQ(k_ra8_ok,
                   ra8_jpeg_sw_decode_stripes(pt_pull,
                                              &pc,
                                              s_pt_window,
                                              (uint32_t)sizeof(s_pt_window),
                                              pt_on_geom,
                                              pt_on_rows,
                                              &sk));
    TEST_ASSERT_EQ(g->h, sk.rows);
    TEST_ASSERT_EQ(g->dec_fnv, sk.hash);
  }
  TEST_END("jpeg_sw parity: streaming decode matches whole-buffer goldens");
}

int32_t main(void)
{
  ra8_fake_mmap_reset();
  test_parity_encoder_bytes();
  test_parity_decoder_pixels();
  test_parity_stream_equals_whole_buffer();
  (void)fprintf(stderr, "[OK ] test_ra8_jpeg_sw_parity.c\n");
  return 0;
}
