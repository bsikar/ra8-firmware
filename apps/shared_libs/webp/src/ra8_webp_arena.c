/**
 * @file ra8_webp_arena.c
 * @brief The libwebp-shaped face of the shared decoder scratch contract.
 *
 * @details
 * See ra8_webp_arena.h for the rationale. This file used to carry its own copy
 * of the bump arithmetic, byte for byte the same as the stb_image arena next
 * door apart from `live` being narrower. RA8FW-308 says there should be one copy,
 * so the policy now lives in `libs/ra8_imgdec/inc/ra8_imgdec_scratch.h` and
 * these hooks forward to it.
 *
 * What stays is the part libwebp forces. `WebPSafeMalloc` and friends take no
 * context argument, so the bound arena has to sit in a file-static slot on
 * this side of the seam; ra8_webp_arena_bind() fills it and
 * ra8_webp_arena_unbind() empties it, fencing the hooks outside a decode.
 *
 *
 * [Ring 4 / WebP] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_webp_arena.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_imgdec_scratch.h"

/** Currently-bound arena, or nullptr when no decode is in flight. */
static ra8_webp_arena_t* s_arena = nullptr;

void ra8_webp_arena_bind(ra8_webp_arena_t* arena)
{
  s_arena = arena;
  if (arena == nullptr) {
    return;
  }
  /* A bind is a re-init: the caller owns `base`/`cap` and expects the arena to
   * come back empty. An unusable descriptor (no store, or no room in it) is
   * still emptied, so the hooks refuse it later the same way they always did
   * rather than drawing from whatever the last decode left behind. */
  if (ra8_imgdec_scratch_init(arena, arena->base, arena->cap) != k_ra8_ok) {
    ra8_imgdec_scratch_reset(arena);
  }
}

void ra8_webp_arena_unbind(void)
{
  s_arena = nullptr;
}

void* ra8_webp_arena_malloc(size_t n)
{
  return ra8_imgdec_scratch_alloc(s_arena, n);
}

void* ra8_webp_arena_calloc(size_t nmemb, size_t size)
{
  return ra8_imgdec_scratch_calloc(s_arena, nmemb, size);
}

void ra8_webp_arena_free(void* p)
{
  ra8_imgdec_scratch_free(s_arena, p);
}
