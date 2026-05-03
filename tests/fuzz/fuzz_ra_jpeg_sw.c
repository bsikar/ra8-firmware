/**
 * @file fuzz_ra_jpeg_sw.c
 * @brief libFuzzer harness for ra_jpeg_sw_decode().
 *
 * @details
 * Coverage-guided fuzzer entry point that feeds arbitrary byte streams
 * to the baseline JPEG decoder. The decoder must never crash, read out
 * of bounds, or otherwise trigger an ASan / UBSan diagnostic regardless
 * of how malformed the input is. A non-zero error return is the
 * expected outcome for almost every random input.
 *
 * Build via tests/fuzz/CMakeLists.txt with -DRA_FUZZ=ON.
 * Run for a longer fuzz session via scripts/utils/run_fuzz.sh.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra_err.h"
#include "ra_jpeg_sw.h"

/*
 * Output buffer is fixed-size and statically allocated. We probe a few
 * very small dimension caps via ra_jpeg_sw_get_dimensions() before
 * decoding, but the decoder itself enforces buffer sizing.
 */
enum : uint32_t {
  k_fuzz_jpeg_max_pixels = 256U * 256U,
  k_fuzz_jpeg_out_bytes  = k_fuzz_jpeg_max_pixels * 3U,
};

static uint8_t s_out_buf[k_fuzz_jpeg_out_bytes];

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size == 0U || size > (1U << 20)) {
    return 0;
  }
  uint16_t w     = 0U;
  uint16_t h     = 0U;
  ra_err_t e_dim = ra_jpeg_sw_get_dimensions(data, (uint32_t)size, &w, &h);
  (void)e_dim;
  /* Bound the dimensions so a malicious SOF0 cannot make the decoder
   * try to fill a multi-gigabyte output buffer. The decoder will return
   * k_ra_err_invalid_size in that case. */
  if (w == 0U || h == 0U || ((uint32_t)w * (uint32_t)h) > k_fuzz_jpeg_max_pixels) {
    return 0;
  }
  uint16_t out_w = 0U;
  uint16_t out_h = 0U;
  ra_err_t e = ra_jpeg_sw_decode(data, (uint32_t)size, s_out_buf, sizeof s_out_buf, &out_w, &out_h);
  (void)e;
  return 0;
}
