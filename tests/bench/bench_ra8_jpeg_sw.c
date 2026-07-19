/**
 * @file bench_ra8_jpeg_sw.c
 * @brief Microbenchmark: software JPEG decode of a known fixture.
 *
 * @details
 * Reuses the 8x8 baseline-JPEG strategy from
 * `tests/test_ra8_jpeg_sw.c`: a small RGB gradient is encoded once
 * outside the timed region (since the encoder also has its own
 * tests), then the encoded byte stream is decoded inside the timed
 * loop. We chose the decode side as the hot-path because the
 * camera-capture pipeline on the EVM will spend its cycles decoding
 * incoming frames, not encoding them.
 *
 * The bytes-per-iteration figure used for the MB/s column is the
 * length of the JPEG byte stream, not the pixel payload. This keeps
 * the throughput number directly comparable to network / flash read
 * rates that feed the decoder.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "ra8_bench.h"
#include "ra8_err.h"
#include "ra8_jpeg_sw.h"

/**
 * @enum jpeg_sw_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_byte_mask = 0xFFU, /**< Truncates a generated or shifted value back into a byte. */
} jpeg_sw_uint8_const_t;

/**
 * @enum bench_jpeg_sizes_t
 * @brief Bench fixture dimensions.
 */
typedef enum : uint16_t {
  k_bench_jpeg_w       = 64U, /**< Bench JPEG w.       */
  k_bench_jpeg_h       = 64U, /**< Bench JPEG h.       */
  k_bench_jpeg_quality = 75U, /**< Bench JPEG quality. */
} bench_jpeg_sizes_t;

/**
 * @enum bench_jpeg_buffers_t
 * @brief Bench fixture byte counts.
 */
typedef enum : uint32_t {
  k_bench_jpeg_rgb_bytes =
    (uint32_t)k_bench_jpeg_w * (uint32_t)k_bench_jpeg_h * 3U, /**< Bench JPEG RGB bytes. */
  k_bench_jpeg_buf_cap =
    (uint32_t)k_bench_jpeg_w * (uint32_t)k_bench_jpeg_h * 3U, /**< Bench JPEG buffer cap. */
} bench_jpeg_buffers_t;

static uint8_t s_rgb_in[(uint32_t)k_bench_jpeg_rgb_bytes];
static uint8_t s_rgb_out[(uint32_t)k_bench_jpeg_rgb_bytes];
static uint8_t s_jpeg[(uint32_t)k_bench_jpeg_buf_cap];

/**
 * @brief Smooth gradient identical to the unit-test fixture.
 */
static void fill_gradient(void)
{
  for (uint16_t y = 0U; y < (uint16_t)k_bench_jpeg_h; y++) {
    for (uint16_t x = 0U; x < (uint16_t)k_bench_jpeg_w; x++) {
      uint32_t i       = (((uint32_t)y * (uint32_t)k_bench_jpeg_w) + (uint32_t)x) * 3U;
      s_rgb_in[i + 0U] = (uint8_t)((x * 4U) & k_byte_mask);
      s_rgb_in[i + 1U] = (uint8_t)((y * 4U) & k_byte_mask);
      s_rgb_in[i + 2U] = (uint8_t)(((x + y) * 2U) & k_byte_mask);
    }
  }
}

/**
 * @brief Bench entry.
 */
/* The nesting this reports is entirely inside RA8_BENCH_TIME's expansion --
 * the harness wraps every measured body in do{ while{ for{ ... } } } to pick an
 * iteration count that reaches a minimum wall time. The body below is flat
 * straight-line code and contributes no nesting of its own.
 *
 * It is not reducible: an adaptive iteration count needs a calibration loop
 * around the measured loop, and the body must stay lexically inline -- routing
 * it through a callback would add an indirect call per iteration and inflate
 * exactly the number the benchmark exists to report. clang-tidy offers no
 * per-aspect suppression, so this also masks the statement count for this
 * function; keep it short.
 */
// NOLINTNEXTLINE(readability-function-size)
int main(void)
{
  ra8_bench_print_header("bench_ra8_jpeg_sw");
  fill_gradient();

  /* Encode once (untimed) -- decode is the hot path. */
  uint32_t  produced = 0U;
  ra8_err_t e        = ra8_jpeg_sw_encode(s_rgb_in,
                                          (uint16_t)k_bench_jpeg_w,
                                          (uint16_t)k_bench_jpeg_h,
                                          (uint8_t)k_bench_jpeg_quality,
                                          s_jpeg,
                                          (uint32_t)k_bench_jpeg_buf_cap,
                                          &produced);
  if ((e != k_ra8_ok) || (produced == 0U)) {
    (void)fprintf(stderr, "jpeg encode failed (%d)\n", (int)e);
    return 1;
  }
  (void)fprintf(stdout,
                "# encoded %ux%u JPEG @ q=%u -> %u bytes\n",
                (unsigned)k_bench_jpeg_w,
                (unsigned)k_bench_jpeg_h,
                (unsigned)k_bench_jpeg_quality,
                (unsigned)produced);

  /* Time the decode. Bytes/iter = JPEG byte-stream length. */
  uint16_t dw = 0U;
  uint16_t dh = 0U;
  RA8_BENCH_TIME("jpeg_decode_64x64_q75", produced, {
    (void)
      ra8_jpeg_sw_decode(s_jpeg, produced, s_rgb_out, (uint32_t)k_bench_jpeg_rgb_bytes, &dw, &dh);
  });
  return 0;
}
