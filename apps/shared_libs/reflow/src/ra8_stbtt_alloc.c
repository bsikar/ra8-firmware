/**
 * @file ra8_stbtt_alloc.c
 * @brief stb_truetype's allocator hooks, forwarded to the shared scratch (RA8FW-308).
 *
 * @details
 * See ra8_stbtt_alloc.h for the rationale. The bump arithmetic that used to
 * live here is now ::ra8_imgdec_scratch_t, written once for every decoder shim
 * in the tree; what stays is the one thing the shared contract cannot take:
 * the backing store itself, and the file-static slot it is reached through,
 * forced by `STBTT_malloc` / `STBTT_free` being macros with no context
 * parameter.
 *
 * The capacity is sized from the measured worst case: the densest
 * printable-ASCII glyph ('@') of the bundled Literata face at the library's
 * maximum font size (`k_reflow_max_font_px` = 96 px) accumulates ~32 KiB of
 * stb scratch within a single rasterisation. The arena is provisioned at 3x
 * that so a heavier face or a larger glyph still fits with margin.
 *
 *
 * [Ring 4 / Reflow] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_stbtt_alloc.h"

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_imgdec_scratch.h"

/**
 * @enum ra8_stbtt_alloc_consts_t
 * @brief Arena sizing knobs (no magic numbers).
 */
typedef enum : uint32_t {
  k_ra8_stbtt_align              = 16U,         /**< Allocation alignment, bytes.       */
  k_ra8_stbtt_arena_bytes        = 96U * 1024U, /**< 3x the 32 KiB worst case.          */
  k_ra8_stbtt_zero_request_bytes = 1U,          /**< What a zero-byte request reserves. */
} ra8_stbtt_alloc_consts_t;

/** The shim's documented 16-byte guarantee is the shared contract's, not a
 * second rule; if the contract ever rounds to something else the header here
 * would be lying, so pin them together. */
static_assert((uint32_t)k_ra8_stbtt_align == (uint32_t)k_ra8_imgdec_scratch_align,
              "stb_truetype scratch alignment must match the shared contract");

/** Backing store for all stb_truetype scratch (NASA P10 Rule 3). Aligned
 * so every 16-byte-rounded offset yields a 16-byte-aligned pointer. */
alignas(k_ra8_stbtt_align) static uint8_t s_arena[k_ra8_stbtt_arena_bytes];

/** The arena, composed over @ref s_arena at load time rather than through
 * ra8_imgdec_scratch_init(): this shim has no bind point to run an init from
 * (the hooks are macros invoked by SOUP), and the field values below are
 * exactly what init writes. Composing statically keeps both hooks free of a
 * lazy-init branch whose failure arm could never be reached, let alone
 * tested. */
static ra8_imgdec_scratch_t s_scratch = {
    .base       = s_arena,
    .cap        = (size_t)k_ra8_stbtt_arena_bytes,
    .offset     = 0U,
    .live       = 0U,
    .high_water = 0U,
};

void* ra8_stbtt_malloc(size_t n)
{
  /* A zero-byte request reserves a slot rather than being refused: it must
   * still answer with an address distinct from the next block, and reserving
   * something is what stops two zero-byte blocks aliasing the next real
   * allocation the way this shim's own arithmetic used to. */
  const size_t want = (n == 0U) ? (size_t)k_ra8_stbtt_zero_request_bytes : n;
  return ra8_imgdec_scratch_alloc(&s_scratch, want);
}

void ra8_stbtt_free(void* p)
{
  ra8_imgdec_scratch_free(&s_scratch, p);
}

size_t ra8_stbtt_alloc_high_water(void)
{
  return ra8_imgdec_scratch_high_water(&s_scratch);
}
