/**
 * @file test_ra8_jpeg_sw_stream.c
 * @brief Host tests for the streaming JPEG stripe decoder: whole-vs-stream
 *        parity, window sliding, and fail-closed error paths (#231).
 *
 * @details
 * Sources are encoded in-test with `ra8_jpeg_sw_encode()` (4:2:0 colour) so
 * the whole-buffer decoder `ra8_jpeg_sw_decode()` can serve as the byte
 * oracle: the streaming decoder must reassemble the identical frame from
 * MCU-row stripes, regardless of how the source is dribbled into the
 * window. Truncation, callback aborts, undersized buffers and hostile
 * marker structure are all driven to their contracted error codes.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_jpeg_sw.h"
#include "unity_minimal.h"

/** @brief JPEG quantisation-table geometry. */
typedef enum : uint8_t {
  k_jpeg_quant_entries  = 64U, /**< Coefficients in one 8x8 quant table. */
  k_jpeg_quant_all_ones = 1U,  /**< Fill value of the all-ones table.    */
} jpeg_quant_t;

/**
 * @enum jpeg_sw_stream_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_jpeg_pattern_c_mul =
    77U, /**< Channel multiplier of that generator, large and odd so the three channels never collide. */
  k_jpeg_marker_eoi    = 0xD9U, /**< 0xD9: the End Of Image marker code. */
  k_jpeg_marker_prefix = 0xFFU, /**< 0xFF: the byte that introduces every JPEG marker. */
  k_jpeg_chunk_bytes =
    64U, /**< Bytes fed per streaming step, so the decoder must resume across chunk boundaries. */
  k_jpeg_pattern_x_mul =
    5U, /**< Column multiplier of the source generator; also the denominator that truncates the stream to three fifths of its length for the short-input case. */
} jpeg_sw_stream_uint8_const_t;

/** @brief Test geometry + buffer sizing. */
enum : uint32_t {
  k_t_w         = 200U,               /**< Odd-MCU image width.         */
  k_t_h         = 90U,                /**< Odd-MCU image height.        */
  k_t_big_w     = 512U,               /**< Window-slide stress width.   */
  k_t_big_h     = 512U,               /**< Window-slide stress height.  */
  k_t_win_cap   = 131072U,            /**< Minimum-size sliding window. */
  k_t_src_cap   = 2U * 1024U * 1024U, /**< Encoded source capacity.     */
  k_t_frame_cap = 3U * 1024U * 1024U, /**< Reassembled frame capacity.  */
};

/** @brief Encoded JPEG under test. */
static uint8_t s_src[k_t_src_cap];
/** @brief Encoded length. */
static uint32_t s_src_len;
/** @brief RGB staging for the encoder. */
static uint8_t s_rgb[(size_t)k_t_big_w * (size_t)k_t_big_h * 3U];
/** @brief Whole-buffer reference frame. */
static uint8_t s_ref[k_t_frame_cap];
/** @brief Stream-reassembled frame. */
static uint8_t s_got[k_t_frame_cap];
/** @brief The sliding window. */
static uint8_t s_win[k_t_win_cap];
/** @brief The consumer stripe buffer. */
static uint8_t s_stripe[(size_t)k_t_big_w * 16U * 3U];

/**
 * @struct t_src_t
 * @brief Dribbling pull source over the encoded buffer.
 */
typedef struct {
  size_t pos;   /**< Read cursor.                */
  size_t chunk; /**< Max bytes per pull (0=all). */
} t_src_t;

/**
 * @struct t_sink_t
 * @brief Stripe reassembly sink state.
 */
typedef struct {
  uint16_t  w;        /**< Expected width.               */
  uint16_t  h;        /**< Expected height.              */
  uint8_t   ch;       /**< Expected channels.            */
  uint32_t  next_y;   /**< Strict in-order row check.    */
  ra8_err_t abort_at; /**< Error to inject (0 = never).  */
  uint16_t  abort_y;  /**< Row that triggers the inject. */
} t_sink_t;

/** @brief Pull callback: dribble the encoded source. */
static ra8_err_t t_pull(void* ctx, uint8_t* buf, size_t cap, size_t* got)
{
  t_src_t*     p    = (t_src_t*)ctx;
  const size_t left = (size_t)s_src_len - p->pos;
  size_t       take = (cap < left) ? cap : left;
  if ((p->chunk != 0U) && (take > p->chunk)) {
    take = p->chunk;
  }
  memcpy(buf, &s_src[p->pos], take);
  p->pos += take;
  *got = take;
  return k_ra8_ok;
}

/** @brief Geometry callback: validate and hand back the shared stripe. */
static ra8_err_t t_geom(void*     ctx,
                        uint16_t  width,
                        uint16_t  height,
                        uint8_t   channels,
                        uint16_t  stripe_rows,
                        uint8_t** out_stripe,
                        uint32_t* out_stripe_cap)
{
  t_sink_t* s = (t_sink_t*)ctx;
  TEST_ASSERT_EQ(s->w, width);
  TEST_ASSERT_EQ(s->h, height);
  TEST_ASSERT_EQ(s->ch, channels);
  TEST_ASSERT(stripe_rows <= (uint16_t)k_ra8_jpeg_sw_stream_mcu_rows_max);
  *out_stripe     = s_stripe;
  *out_stripe_cap = (uint32_t)sizeof(s_stripe);
  return k_ra8_ok;
}

/** @brief Geometry callback that rejects the image (budget hook). */
/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTBEGIN(readability-non-const-parameter)
static ra8_err_t t_geom_abort(void*     ctx,
                              uint16_t  width,
                              uint16_t  height,
                              uint8_t   channels,
                              uint16_t  stripe_rows,
                              uint8_t** out_stripe,
                              uint32_t* out_stripe_cap)
// NOLINTEND(readability-non-const-parameter)
{
  (void)ctx;
  (void)width;
  (void)height;
  (void)channels;
  (void)stripe_rows;
  (void)out_stripe;
  (void)out_stripe_cap;
  return k_ra8_err_no_mem;
}

/** @brief Geometry callback that hands back a too-small stripe. */
static ra8_err_t t_geom_tiny(void*     ctx,
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
  *out_stripe     = s_stripe;
  *out_stripe_cap = 8U;
  return k_ra8_ok;
}

/** @brief Rows callback: copy each stripe into the reassembly frame. */
static ra8_err_t
t_rows(void* ctx, const uint8_t* px, uint16_t width, uint16_t y0, uint16_t nrows, uint8_t channels)
{
  t_sink_t* s = (t_sink_t*)ctx;
  TEST_ASSERT_EQ(s->w, width);
  TEST_ASSERT_EQ(s->ch, channels);
  TEST_ASSERT_EQ(s->next_y, y0);
  TEST_ASSERT(nrows >= 1U);
  if ((s->abort_at != k_ra8_ok) && (y0 >= s->abort_y)) {
    return s->abort_at;
  }
  memcpy(&s_got[(size_t)y0 * (size_t)width * (size_t)channels],
         px,
         (size_t)nrows * (size_t)width * (size_t)channels);
  s->next_y += nrows;
  return k_ra8_ok;
}

/** @brief Encode the deterministic pattern at (w, h) into `s_src`. */
static void encode_pattern(uint32_t w, uint32_t h, uint8_t quality)
{
  for (uint32_t y = 0U; y < h; y++) {
    for (uint32_t x = 0U; x < w; x++) {
      for (uint32_t c = 0U; c < 3U; c++) {
        s_rgb[(((y * w) + x) * 3U) + c] =
          (uint8_t)((x * k_jpeg_pattern_x_mul) ^ (y * 3U) ^ (c * k_jpeg_pattern_c_mul));
      }
    }
  }
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_jpeg_sw_encode(s_rgb,
                                    (uint16_t)w,
                                    (uint16_t)h,
                                    quality,
                                    s_src,
                                    (uint32_t)sizeof(s_src),
                                    &s_src_len));
}

/** @brief Run a full streamed decode of `s_src`, returning the code. */
static ra8_err_t stream_decode(uint32_t w, uint32_t h, size_t chunk, t_sink_t* sink)
{
  static t_src_t s_pull_src;
  s_pull_src   = (t_src_t){.pos = 0U, .chunk = chunk};
  sink->w      = (uint16_t)w;
  sink->h      = (uint16_t)h;
  sink->ch     = 3U;
  sink->next_y = 0U;
  return ra8_jpeg_sw_decode_stripes(t_pull,
                                    &s_pull_src,
                                    s_win,
                                    (uint32_t)sizeof(s_win),
                                    t_geom,
                                    t_rows,
                                    sink);
}

/**
 * @test test_jpeg_stream_parity
 * @brief Streamed stripes reassemble byte-identically to the whole-buffer
 *        decode, across odd MCU-edge dims and a 1-byte dribble source.
 *
 * @par MC/DC:
 * Decision (refill): `eof == 0 && win_len < win_cap` (2 conditions)
 * - Vector 1: mid-stream refill    -> true, loop pulls (dribble run)
 * - Vector 2: source exhausted     -> false via eof (every run's tail)
 * - Vector 3: window already full  -> false via win_len (big-image run,
 *   where the 240 KiB source overfills the 128 KiB window)
 */
static void test_jpeg_stream_parity(void)
{
  TEST_BEGIN("jpeg stream: stripes == whole decode (odd dims + dribble)");
  encode_pattern(k_t_w, k_t_h, (uint8_t)k_ra8_jpeg_sw_quality_default);
  uint16_t rw = 0U;
  uint16_t rh = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_jpeg_sw_decode(s_src, s_src_len, s_ref, (uint32_t)sizeof(s_ref), &rw, &rh));
  TEST_ASSERT_EQ(k_t_w, rw);
  TEST_ASSERT_EQ(k_t_h, rh);
  const size_t chunks[3] = {0U, 1U, 13U};
  for (uint32_t i = 0U; i < 3U; i++) {
    t_sink_t sink = {};
    memset(s_got, 0, (size_t)k_t_w * (size_t)k_t_h * 3U);
    TEST_ASSERT_EQ(k_ra8_ok, stream_decode(k_t_w, k_t_h, chunks[i], &sink));
    TEST_ASSERT_EQ(k_t_h, sink.next_y);
    TEST_ASSERT_EQ(0, memcmp(s_got, s_ref, (size_t)k_t_w * (size_t)k_t_h * 3U));
  }
  TEST_END("jpeg stream: stripes == whole decode (odd dims + dribble)");
}

/**
 * @test test_jpeg_stream_window_slide
 * @brief A source larger than the window decodes via mid-scan slides and
 *        still matches the whole-buffer decode byte for byte.
 *
 * @par MC/DC:
 * Decision (scan margin): `eof == 0 && (win_len - pos) < margin`
 * (2 conditions)
 * - Vector 1: early scan, plenty ahead -> false via the margin condition
 * - Vector 2: deep scan, margin low    -> true (slide + refill taken)
 * - Vector 3: after source EOF         -> false via the eof condition
 * All three occur within this run (compressed size >> window).
 */
static void test_jpeg_stream_window_slide(void)
{
  TEST_BEGIN("jpeg stream: window slides on a source larger than the window");
  encode_pattern(k_t_big_w, k_t_big_h, (uint8_t)k_ra8_jpeg_sw_quality_max);
  TEST_ASSERT(s_src_len > (uint32_t)k_t_win_cap); /* the regime under test */
  uint16_t rw = 0U;
  uint16_t rh = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_jpeg_sw_decode(s_src, s_src_len, s_ref, (uint32_t)sizeof(s_ref), &rw, &rh));
  t_sink_t sink = {};
  memset(s_got, 0, (size_t)k_t_big_w * (size_t)k_t_big_h * 3U);
  TEST_ASSERT_EQ(k_ra8_ok, stream_decode(k_t_big_w, k_t_big_h, 4096U, &sink));
  TEST_ASSERT_EQ(0, memcmp(s_got, s_ref, (size_t)k_t_big_w * (size_t)k_t_big_h * 3U));
  TEST_END("jpeg stream: window slides on a source larger than the window");
}

/**
 * @test test_jpeg_stream_argument_guards
 * @brief The stripe decoder rejects null seams and an undersized window before
 *        it touches the bitstream.
 *
 * @details These are caller mistakes rather than bad data, so they are checked
 *          apart from the malformed-bitstream cases: the source here is a
 *          perfectly valid JPEG, which proves the guards fire on their own
 *          terms and not as a side effect of decoding failing anyway.
 */
static void test_jpeg_stream_argument_guards(void)
{
  TEST_BEGIN("jpeg stream: null and sizing guards");
  encode_pattern(k_t_w, k_t_h, (uint8_t)k_ra8_jpeg_sw_quality_default);
  t_sink_t sink = {};

  static t_src_t s_pull_src;
  s_pull_src = (t_src_t){.pos = 0U, .chunk = 0U};
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_jpeg_sw_decode_stripes(NULL, &s_pull_src, s_win, k_t_win_cap, t_geom, t_rows, &sink));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_jpeg_sw_decode_stripes(t_pull, &s_pull_src, NULL, k_t_win_cap, t_geom, t_rows, &sink));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_jpeg_sw_decode_stripes(t_pull, &s_pull_src, s_win, 1024U, t_geom, t_rows, &sink));
  TEST_END("jpeg stream: null and sizing guards");
}

/**
 * @test test_jpeg_stream_rejects_malformed
 * @brief Three shapes of bad bitstream each fail closed with protocol_error.
 *
 * @details Covers the whole span from "never looked like a JPEG" through
 *          "well-formed header, no scan" to "valid headers, entropy data cut
 *          short", so a decoder that accepted any of them is caught at the
 *          stage it went wrong.
 *
 * @par MC/DC:
 * Decision (geometry latch): `got_sof && !geom_done` (2 conditions)
 * - Vector 1: SOF0 parsed, first time -> true (the truncated-entropy case,
 *   which parses every header before failing in the scan)
 * - Vector 2: pre-SOF markers        -> false via got_sof
 * - Vector 3: post-SOF markers (DHT after SOF0 in the encoder's layout)
 *   -> false via geom_done
 */
static void test_jpeg_stream_rejects_malformed(void)
{
  TEST_BEGIN("jpeg stream: malformed bitstreams fail closed");
  encode_pattern(k_t_w, k_t_h, (uint8_t)k_ra8_jpeg_sw_quality_default);
  const uint32_t full_len = s_src_len;
  t_sink_t       sink     = {.w = k_t_w, .h = k_t_h, .ch = 3U};
  static t_src_t s_pull_src;

  /* Not a JPEG. */
  s_src[0]   = 0x00U;
  s_pull_src = (t_src_t){.pos = 0U, .chunk = 0U};
  TEST_ASSERT_EQ(
    k_ra8_err_protocol_error,
    ra8_jpeg_sw_decode_stripes(t_pull, &s_pull_src, s_win, k_t_win_cap, t_geom, t_rows, &sink));
  s_src[0] = k_jpeg_marker_prefix;

  /* SOI+EOI only: EOI before any scan. */
  s_src_len  = 4U;
  s_src[2]   = k_jpeg_marker_prefix;
  s_src[3]   = k_jpeg_marker_eoi;
  s_pull_src = (t_src_t){.pos = 0U, .chunk = 0U};
  TEST_ASSERT_EQ(
    k_ra8_err_protocol_error,
    ra8_jpeg_sw_decode_stripes(t_pull, &s_pull_src, s_win, k_t_win_cap, t_geom, t_rows, &sink));

  /* Truncated entropy stream (cut at 60%). */
  encode_pattern(k_t_w, k_t_h, (uint8_t)k_ra8_jpeg_sw_quality_default);
  s_src_len = (full_len * 3U) / k_jpeg_pattern_x_mul;
  sink      = (t_sink_t){};
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, stream_decode(k_t_w, k_t_h, 0U, &sink));
  TEST_END("jpeg stream: malformed bitstreams fail closed");
}

/**
 * @test test_jpeg_stream_geometry_hook_failures
 * @brief The geometry hook is a fail-closed seam: both ways it can refuse stop
 *        the decode, and each refusal keeps its own error code.
 *
 * @details A hook that rejects outright must surface no_mem, and one that hands
 *          back a stripe too small for the image must surface invalid_size.
 *          Distinct codes matter because the caller's remedy differs: raise the
 *          budget, versus fix the stripe arithmetic.
 */
static void test_jpeg_stream_geometry_hook_failures(void)
{
  TEST_BEGIN("jpeg stream: geometry hook refusals");
  encode_pattern(k_t_w, k_t_h, (uint8_t)k_ra8_jpeg_sw_quality_default);
  t_sink_t       sink = {};
  static t_src_t s_pull_src;

  /* Geometry hook rejects (the fail-closed budget seam). */
  s_pull_src = (t_src_t){.pos = 0U, .chunk = 0U};
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_jpeg_sw_decode_stripes(t_pull,
                                            &s_pull_src,
                                            s_win,
                                            k_t_win_cap,
                                            t_geom_abort,
                                            t_rows,
                                            &sink));

  /* Geometry hook hands back an undersized stripe. */
  s_pull_src = (t_src_t){.pos = 0U, .chunk = 0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_jpeg_sw_decode_stripes(t_pull,
                                            &s_pull_src,
                                            s_win,
                                            k_t_win_cap,
                                            t_geom_tiny,
                                            t_rows,
                                            &sink));
  TEST_END("jpeg stream: geometry hook refusals");
}

/**
 * @test test_jpeg_stream_propagates_sink_abort
 * @brief A consumer that aborts mid-image gets its own error code back, verbatim.
 *
 * @details The decoder must not translate, swallow, or overwrite a sink's
 *          refusal -- the consumer's code is what the caller acts on, so it has
 *          to survive the unwind unchanged.
 */
static void test_jpeg_stream_propagates_sink_abort(void)
{
  TEST_BEGIN("jpeg stream: sink abort propagates verbatim");
  encode_pattern(k_t_w, k_t_h, (uint8_t)k_ra8_jpeg_sw_quality_default);
  t_sink_t        sink = {.abort_at = k_ra8_err_busy, .abort_y = 32U};
  const ra8_err_t err  = stream_decode(k_t_w, k_t_h, 0U, &sink);
  TEST_ASSERT_EQ(k_ra8_err_busy, err);
  TEST_END("jpeg stream: sink abort propagates verbatim");
}

/**
 * @var s_jpeg_gray_hdr
 * @brief SOI + DQT(table 0) prefix of the minimal grayscale JFIF fixture.
 * @details Followed at build time by 64 all-ones quantisation entries.
 * @note Read-only host-test fixture.
 * @since 0.1.0
 */
static const uint8_t s_jpeg_gray_hdr[] = {
  0xFFU,
  0xD8U, /* SOI */
  0xFFU,
  0xDBU,
  0x00U,
  0x43U,
  0x00U, /* DQT, table 0 */
  /* 64 quantisation entries of 1 follow. */
};

/**
 * @var s_jpeg_gray_body
 * @brief SOF0 + DHT(DC0/AC0) + SOS + scan + EOI body of the grayscale fixture.
 * @note Read-only host-test fixture.
 * @since 0.1.0
 */
static const uint8_t s_jpeg_gray_body[] = {
  0xFFU,
  0xC0U,
  0x00U,
  0x0BU,
  0x08U,
  0x00U,
  0x08U,
  0x00U,
  0x08U,
  0x01U,
  0x01U,
  0x11U,
  0x00U, /* SOF0: 8x8, 1 comp, q0 */
  /* DHT DC0: one code of length 2 -> symbol 0. */
  0xFFU,
  0xC4U,
  0x00U,
  0x14U,
  0x00U,
  0x00U,
  0x01U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  /* DHT AC0: one code of length 2 -> symbol 0 (EOB). */
  0xFFU,
  0xC4U,
  0x00U,
  0x14U,
  0x10U,
  0x00U,
  0x01U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  /* SOS: 1 comp, DC0/AC0. */
  0xFFU,
  0xDAU,
  0x00U,
  0x08U,
  0x01U,
  0x01U,
  0x00U,
  0x00U,
  0x3FU,
  0x00U,
  /* Scan: DC cat 0 ('00') + EOB ('00') + padding -> one byte. */
  0x0FU,
  0xFFU,
  0xD9U, /* EOI */
};

/**
 * @test test_jpeg_stream_grayscale
 * @brief A grayscale baseline stream emits 1-channel stripes.
 *
 * @details Uses a hand-built minimal 8x8 grayscale JFIF (flat DC-only block
 *          against all-ones quantisation and the T.81 K.3 Huffman tables) so
 *          the grayscale emit path is covered without a colour transform.
 *
 * @par MC/DC:
 * Decision (emit): `channels == 1` (single condition; both outcomes
 * observed -- gray here, RGB in the parity tests).
 */
static void test_jpeg_stream_grayscale(void)
{
  TEST_BEGIN("jpeg stream: grayscale stripes (1 channel)");
  /* Build: SOI, DQT(all 1s), SOF0 8x8 1-comp, DHT(DC0: code '00' for cat 0),
   * DHT(AC0: code '10' for EOB via a 2-entry table), SOS, scan, EOI. */
  s_src_len = 0U;
  memcpy(s_src, s_jpeg_gray_hdr, sizeof(s_jpeg_gray_hdr));
  s_src_len = sizeof(s_jpeg_gray_hdr);
  memset(&s_src[s_src_len],
         k_jpeg_quant_all_ones,
         (size_t)k_jpeg_quant_entries); /* all-ones quant table */
  s_src_len += k_jpeg_chunk_bytes;
  memcpy(&s_src[s_src_len], s_jpeg_gray_body, sizeof(s_jpeg_gray_body));
  s_src_len += (uint32_t)sizeof(s_jpeg_gray_body);

  static t_src_t s_pull_src;
  s_pull_src    = (t_src_t){.pos = 0U, .chunk = 0U};
  t_sink_t sink = {.w = 8U, .h = 8U, .ch = 1U};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_jpeg_sw_decode_stripes(t_pull, &s_pull_src, s_win, k_t_win_cap, t_geom, t_rows, &sink));
  TEST_ASSERT_EQ(8U, sink.next_y);
  /* Whole-buffer twin agrees (its RGB output replicates Y into 3 bytes). */
  uint16_t rw = 0U;
  uint16_t rh = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_jpeg_sw_decode(s_src, s_src_len, s_ref, (uint32_t)sizeof(s_ref), &rw, &rh));
  for (uint32_t i = 0U; i < k_jpeg_chunk_bytes; i++) {
    TEST_ASSERT_EQ(s_ref[(size_t)i * 3U], s_got[i]);
  }
  TEST_END("jpeg stream: grayscale stripes (1 channel)");
}

/**
 * @brief Test entry point -- runs the streaming JPEG suite in order.
 * @return 0 on success; unity_minimal.h exits non-zero on first failure.
 * @pre None.
 * @pre None.
 * @post All tests executed (or the process exited on first failure).
 * @post stderr carries a per-test RUN/PASS log.
 * @note Not thread-safe. No SIGALRM / timers used.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_jpeg_stream_parity();
  test_jpeg_stream_window_slide();
  test_jpeg_stream_argument_guards();
  test_jpeg_stream_rejects_malformed();
  test_jpeg_stream_geometry_hook_failures();
  test_jpeg_stream_propagates_sink_abort();
  test_jpeg_stream_grayscale();
  (void)fprintf(stderr, "[OK  ] test_ra8_jpeg_sw_stream.c\n");
  return 0;
}
