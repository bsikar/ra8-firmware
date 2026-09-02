/**
 * @file fuzz_ra8_stb_image.c
 * @brief libFuzzer harness for stbi_load_from_memory() (stb_image decoder).
 *
 * @details
 * The e-reader decodes EPUB cover art and inline figures (JPEG / PNG / GIF /
 * BMP) through stb_image, driven from ra8_img_decode_blit() in
 * apps/shared_libs/reflow/src/reflow_image.c. Those bytes are fully
 * attacker-controlled: an image inside a book file. This harness feeds the
 * raw fuzz input to the same stbi_load_from_memory() entry, backed by the same
 * heap-free ra8_img_arena bump allocator the firmware binds, so an ASan / UBSan
 * diagnostic here is a real out-of-bounds read or integer overflow in the
 * decode path. The declared dimensions are bounded before decoding (stb also
 * enforces STBI_MAX_DIMENSIONS) so a malicious header cannot demand a
 * multi-gigapixel buffer; every random input is expected to fail the decode
 * without crashing.
 *
 * Build via tests/fuzz/CMakeLists.txt with -DRA8_FUZZ=ON.
 * Run for a longer fuzz session via scripts/checks/run_fuzz.sh.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

/** @brief STBI NO STDIO. */
#define STBI_NO_STDIO
#include "fuzz_entry.h"
#include "ra8_img_arena.h" /* ra8_img_arena_t, ra8_img_arena_bind/_unbind  */
#include "stb_image.h"     /* stbi_info_from_memory, stbi_load_from_memory */

/**
 * @enum stb_image_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_stb_image_u_20 = 20, /**< Log2 of the fuzz input-size cap (1 MiB). */
} stb_image_uint8_const_t;

/*
 * Output scratch is a fixed, statically allocated bump arena bound around each
 * decode -- the same zero-heap strategy the firmware uses. The pixel cap keeps
 * a valid-but-huge header from exhausting the arena on every input (which would
 * only slow the fuzzer); the decoder itself enforces the real output sizing.
 */
enum : uint32_t {
  k_fuzz_img_max_pixels    = 512U * 512U,         /**< Max decoded pixels probed.  */
  k_fuzz_img_scratch_bytes = 16U * 1024U * 1024U, /**< Arena backing store, bytes. */
  k_fuzz_img_req_rgb       = 3U,                  /**< Force 3-channel RGB output. */
};

alignas(16) static uint8_t s_scratch[k_fuzz_img_scratch_bytes];

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size == 0U || size > (1U << k_stb_image_u_20)) {
    return 0;
  }
  /* Bound the declared dimensions before decoding so a malicious width/height
   * cannot make the decoder try to fill a multi-gigabyte buffer. */
  int x    = 0;
  int y    = 0;
  int comp = 0;
  if (stbi_info_from_memory((const stbi_uc*)data, (int)size, &x, &y, &comp) == 0) {
    return 0;
  }
  if (x <= 0 || y <= 0 || (uint64_t)(uint32_t)x * (uint32_t)y > k_fuzz_img_max_pixels) {
    return 0;
  }

  ra8_img_arena_t arena = {.base = s_scratch, .cap = sizeof s_scratch, .offset = 0U, .live = 0U};
  ra8_img_arena_bind(&arena);
  int      sx = 0;
  int      sy = 0;
  int      sc = 0;
  stbi_uc* pixels =
    stbi_load_from_memory((const stbi_uc*)data, (int)size, &sx, &sy, &sc, (int)k_fuzz_img_req_rgb);
  if (pixels != nullptr) {
    stbi_image_free(pixels);
  }
  ra8_img_arena_unbind();
  return 0;
}
