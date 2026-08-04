/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file fuzz_ra8_jpeg_sw.c
 * @brief libFuzzer harness for ra8_jpeg_sw_decode().
 *
 * @details
 * Coverage-guided fuzzer entry point that feeds arbitrary byte streams
 * to the baseline JPEG decoder. The decoder must never crash, read out
 * of bounds, or otherwise trigger an ASan / UBSan diagnostic regardless
 * of how malformed the input is. A non-zero error return is the
 * expected outcome for almost every random input.
 *
 * Build via tests/fuzz/CMakeLists.txt with -DRA8_FUZZ=ON.
 * Run for a longer fuzz session via scripts/checks/run_fuzz.sh.
 */

#include <stddef.h>
#include <stdint.h>

#include "fuzz_entry.h"
#include "ra8_err.h"
#include "ra8_jpeg_sw.h"

/**
 * @enum jpeg_fuzz_bound_t
 * @brief Log2 of the largest input this harness accepts, so a run stays bounded.
 */
typedef enum : uint8_t {
  k_fuzz_input_cap_log2 =
    20, /**< Log2 of the largest input accepted; longer cases drop so a run stays bounded. */
} jpeg_fuzz_bound_t;

/*
 * Output buffer is fixed-size and statically allocated. We probe a few
 * very small dimension caps via ra8_jpeg_sw_get_dimensions() before
 * decoding, but the decoder itself enforces buffer sizing.
 */
enum : uint32_t {
  k_fuzz_jpeg_max_pixels = 256U * 256U,                 /**< Fuzz JPEG maximum pixels. */
  k_fuzz_jpeg_out_bytes  = k_fuzz_jpeg_max_pixels * 3U, /**< Fuzz JPEG out bytes.      */
};

static uint8_t s_out_buf[k_fuzz_jpeg_out_bytes];

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size == 0U || size > (1U << k_fuzz_input_cap_log2)) {
    return 0;
  }
  uint16_t  w     = 0U;
  uint16_t  h     = 0U;
  ra8_err_t e_dim = ra8_jpeg_sw_get_dimensions(data, (uint32_t)size, &w, &h);
  (void)e_dim;
  /* Bound the dimensions so a malicious SOF0 cannot make the decoder
   * try to fill a multi-gigabyte output buffer. The decoder will return
   * k_ra8_err_invalid_size in that case. */
  if (w == 0U || h == 0U || ((uint32_t)w * (uint32_t)h) > k_fuzz_jpeg_max_pixels) {
    return 0;
  }
  uint16_t  out_w = 0U;
  uint16_t  out_h = 0U;
  ra8_err_t e =
    ra8_jpeg_sw_decode(data, (uint32_t)size, s_out_buf, sizeof s_out_buf, &out_w, &out_h);
  (void)e;
  return 0;
}
