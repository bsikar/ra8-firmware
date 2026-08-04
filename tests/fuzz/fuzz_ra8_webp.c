/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file fuzz_ra8_webp.c
 * @brief libFuzzer harness for the WebP decoder (ra8_webp over libwebp).
 *
 * @details
 * Longstrip corpora are increasingly WebP, and a WebP inside a downloaded book is
 * fully attacker-controlled initial-access content. This harness feeds the raw
 * fuzz input to the exact firmware decode path -- ra8_webp_get_info() then
 * ra8_webp_decode_rgba() over the vendored libwebp (VP8 + VP8L) -- backed by the
 * same heap-free ra8_webp bump arena the firmware binds. An ASan / UBSan
 * diagnostic here is a real out-of-bounds read or integer overflow in the WebP
 * decode path. The declared dimensions are bounded before decoding (the facade
 * also enforces its per-axis cap) so a malicious header cannot demand a
 * multi-gigapixel buffer; every random input is expected to fail the decode
 * without crashing.
 *
 * Build via tests/fuzz/CMakeLists.txt with -DRA8_FUZZ=ON.
 * Run for a longer fuzz session via scripts/checks/run_fuzz.sh.
 */

#include <stddef.h>
#include <stdint.h>

#include "fuzz_entry.h"
#include "ra8_err.h"
#include "ra8_webp.h"       /* ra8_webp_get_info, ra8_webp_decode_rgba */
#include "ra8_webp_arena.h" /* ra8_webp_arena_t                        */

/*
 * Output scratch is a fixed, statically allocated RGBA8888 buffer, and the
 * decoder's transient scratch is a fixed bump arena -- the same zero-heap
 * strategy the firmware uses. The pixel cap keeps a valid-but-huge header from
 * exhausting the arena on every input (which would only slow the fuzzer); the
 * facade + decoder enforce the real output sizing.
 */
enum : uint32_t {
  k_fuzz_webp_max_dim       = 512U,                /**< Max decoded width/height probed. */
  k_fuzz_webp_bpp           = 4U,                  /**< RGBA8888 bytes per pixel.        */
  k_fuzz_webp_scratch_bytes = 16U * 1024U * 1024U, /**< Arena backing store, bytes.      */
  k_fuzz_webp_max_input     = 1U << 20,            /**< Cap fuzz input at 1 MiB.         */
};

alignas(16) static uint8_t s_scratch[k_fuzz_webp_scratch_bytes];
alignas(16) static uint8_t s_out[k_fuzz_webp_max_dim * k_fuzz_webp_max_dim * k_fuzz_webp_bpp];

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size == 0U || size > (size_t)k_fuzz_webp_max_input) {
    return 0;
  }

  /* Bound the declared dimensions before decoding so a malicious width/height
   * cannot make the decoder try to fill a multi-gigabyte buffer. */
  uint32_t w = 0U;
  uint32_t h = 0U;
  if (ra8_webp_get_info(data, size, &w, &h) != k_ra8_ok) {
    return 0;
  }
  if (w > k_fuzz_webp_max_dim || h > k_fuzz_webp_max_dim) {
    return 0;
  }

  ra8_webp_arena_t arena  = {.base = s_scratch, .cap = sizeof s_scratch, .offset = 0U, .live = 0U};
  const size_t     stride = (size_t)w * (size_t)k_fuzz_webp_bpp;
  (void)ra8_webp_decode_rgba(data, size, &arena, s_out, stride, sizeof s_out, &w, &h);
  return 0;
}
