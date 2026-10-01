/**
 * @file ra8_img_arena.c
 * @brief stb_image's allocator hooks, forwarded to the shared scratch (RA8FW-308).
 *
 * @details
 * See ra8_img_arena.h for the rationale. The bump arithmetic that used to live
 * here is now ::ra8_imgdec_scratch_t, written once for every decoder shim in
 * the tree; what stays is the one thing the shared contract cannot take: the
 * file-static "currently bound arena" slot, forced by `STBI_MALLOC` and
 * friends being macros with no context parameter.
 *
 *
 * [Ring 4 / Reflow] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_img_arena.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_imgdec_scratch.h"

/**
 * @enum ra8_img_arena_consts_t
 * @brief Local knobs (no magic numbers).
 */
typedef enum : uint32_t {
  k_ra8_img_zero_request_bytes = 1U, /**< What a zero-byte request reserves. */
} ra8_img_arena_consts_t;

/** Currently-bound arena, or nullptr when no decode is in flight. */
static ra8_img_arena_t* s_arena = nullptr;

void ra8_img_arena_bind(ra8_img_arena_t* arena)
{
  s_arena = arena;
  if (arena != nullptr) {
    if (ra8_imgdec_scratch_init(arena, arena->base, arena->cap) != k_ra8_ok) {
      /* An unusable descriptor is still emptied, so a stray hook call cannot
       * hand out a block from whatever the fields happened to hold. */
      ra8_imgdec_scratch_reset(arena);
    }
  }
}

void ra8_img_arena_unbind(void)
{
  s_arena = nullptr;
}

void* ra8_img_arena_malloc(size_t n)
{
  /* stb_image, unlike libwebp, does not promise a non-zero request. Reserve a
   * byte for one rather than refuse it: a zero-byte block must still be
   * non-NULL (stb reads NULL as out-of-memory and fails the decode), and
   * reserving something is what stops two zero-byte blocks aliasing the next
   * real allocation the way this shim's own arithmetic used to. */
  const size_t want = (n == 0U) ? (size_t)k_ra8_img_zero_request_bytes : n;
  return ra8_imgdec_scratch_alloc(s_arena, want);
}

void ra8_img_arena_free(void* p)
{
  ra8_imgdec_scratch_free(s_arena, p);
}

void* ra8_img_arena_realloc_sized(void* p, size_t oldsz, size_t newsz)
{
  if (newsz == 0U) {
    return ra8_img_arena_malloc(0U);
  }
  return ra8_imgdec_scratch_realloc(s_arena, p, oldsz, newsz);
}
