/**
 * @file test_ra8_imgdec_mux_carve.c
 * @brief Host tests for one scratch budget over a set of backends (RA8FW-308).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_arena.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_imgdec.h"
#include "ra8_imgdec_backend.h"
#include "ra8_imgdec_mux.h"
#include "ra8_imgdec_scratch.h"
#include "unity_minimal.h"

/** @brief Fixture sizes (no magic numbers). */
enum : uint32_t {
  k_pool_bytes   = 2048, /**< Arena backing region every case carves from. */
  k_small_budget = 128,  /**< The quieter backend\'s published budget.     */
  k_big_budget   = 512,  /**< The hungrier backend\'s published budget.    */
  k_strong_align = 32,   /**< Stronger than the contract\'s block align.   */
  k_fake_dim     = 4096, /**< dim_max every fake backend advertises.       */
  k_fill_byte    = 0x5A, /**< Non-zero pre-fill, so a carve is visible.    */
};

/**
 * @struct fake_backend_t
 * @brief A backend that publishes a scratch budget and nothing else of note.
 */
typedef struct {
  uint32_t formats; /**< Formats it claims to open.        */
  uint32_t pixels;  /**< Pixel layouts it claims to write. */
  uint32_t scratch; /**< Published scratch budget.         */
  uint32_t align;   /**< Published scratch alignment.      */
} fake_backend_t;

RA8_INTERNAL static ra8_err_t internal_fake_caps(void* ctx, ra8_imgdec_caps_t* out) {
  const fake_backend_t* const be = (const fake_backend_t*)ctx;
  out->formats                   = be->formats;
  out->pixels                    = be->pixels;
  out->scratch_bytes             = be->scratch;
  out->scratch_align             = be->align;
  out->dim_max                   = (uint32_t)k_fake_dim;
  out->streams                   = false;
  return k_ra8_ok;
}

RA8_INTERNAL static ra8_err_t internal_fake_decode(void*                   ctx,
                                                   const ra8_imgdec_req_t* req,
                                                   ra8_imgdec_image_t*     out) {
  (void)ctx;
  (void)req;
  (void)out;
  return k_ra8_err_not_supported; /* no case here reaches a decode */
}

/** @brief One shared vtable; the fakes differ only in their context. */
static const ra8_imgdec_iface_t s_fake_iface = {
    .get_caps = internal_fake_caps,
    .decode   = internal_fake_decode,
};

alignas(64) static uint8_t g_pool[k_pool_bytes];

/**
 * @brief Fresh arena over the shared pool, pre-filled non-zero.
 *
 * @param[out] arena Arena to initialise.
 *
 * @return None.
 */
RA8_INTERNAL static void internal_fresh_arena(ra8_arena_t* arena) {
  (void)memset(g_pool, k_fill_byte, sizeof(g_pool));
  *arena = (ra8_arena_t){0};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_init(arena, g_pool, sizeof(g_pool)));
}

/**
 * @brief Build a mux holding the given fakes, in order.
 *
 * @param[out]    mux  Mux to initialise and fill.
 * @param[in,out] a    First member, or NULL.
 * @param[in,out] b    Second member, or NULL.
 *
 * @return None.
 */
RA8_INTERNAL static void internal_build(ra8_imgdec_mux_t* mux,
                                        fake_backend_t*   a,
                                        fake_backend_t*   b) {
  *mux = (ra8_imgdec_mux_t){};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_init(mux));
  if (a != nullptr) {
    const ra8_imgdec_t dec = {.iface = &s_fake_iface, .ctx = a};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_add(mux, &dec));
  }
  if (b != nullptr) {
    const ra8_imgdec_t dec = {.iface = &s_fake_iface, .ctx = b};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_add(mux, &dec));
  }
}

RA8_INTERNAL static void internal_test_budget_is_the_peak(void) {
  TEST_BEGIN("the set\'s budget is the peak member, not the sum and not the first");

  fake_backend_t small = {.formats = (uint32_t)k_ra8_imgdec_format_png,
                          .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgba8888,
                          .scratch = (uint32_t)k_small_budget};
  fake_backend_t big   = {.formats = (uint32_t)k_ra8_imgdec_format_webp,
                          .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgba8888,
                          .scratch = (uint32_t)k_big_budget};

  /* Quiet member first, so a first-member answer would be visibly wrong. */
  ra8_imgdec_mux_t mux = {};
  internal_build(&mux, &small, &big);

  uint32_t bytes = 0U;
  uint32_t align = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_scratch_budget(&mux, &bytes, &align));
  TEST_ASSERT(bytes == (uint32_t)k_big_budget);
  TEST_ASSERT(align == (uint32_t)k_ra8_imgdec_scratch_align);

  /* Order must not change the answer: only one member decodes at a time. */
  internal_build(&mux, &big, &small);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_scratch_budget(&mux, &bytes, &align));
  TEST_ASSERT(bytes == (uint32_t)k_big_budget);

  TEST_END("the set\'s budget is the peak member, not the sum and not the first");
}

RA8_INTERNAL static void internal_test_silent_align_does_not_weaken(void) {
  TEST_BEGIN("a member reporting align 0 has no preference, it does not weaken one");

  fake_backend_t loud  = {.formats = (uint32_t)k_ra8_imgdec_format_png,
                          .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgba8888,
                          .scratch = (uint32_t)k_small_budget,
                          .align   = (uint32_t)k_ra8_imgdec_scratch_align};
  fake_backend_t quiet = {.formats = (uint32_t)k_ra8_imgdec_format_webp,
                          .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgba8888,
                          .scratch = (uint32_t)k_small_budget,
                          .align   = 0U};

  ra8_imgdec_mux_t mux = {};
  internal_build(&mux, &loud, &quiet);

  uint32_t bytes = 0U;
  uint32_t align = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_scratch_budget(&mux, &bytes, &align));
  TEST_ASSERT(align == (uint32_t)k_ra8_imgdec_scratch_align);

  TEST_END("a member reporting align 0 has no preference, it does not weaken one");
}

RA8_INTERNAL static void internal_test_carve_funds_the_hungriest(void) {
  TEST_BEGIN("one carve funds whichever member the router picks");

  fake_backend_t small = {.formats = (uint32_t)k_ra8_imgdec_format_png,
                          .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgba8888,
                          .scratch = (uint32_t)k_small_budget};
  fake_backend_t big   = {.formats = (uint32_t)k_ra8_imgdec_format_webp,
                          .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgba8888,
                          .scratch = (uint32_t)k_big_budget};

  ra8_imgdec_mux_t mux = {};
  internal_build(&mux, &small, &big);

  ra8_arena_t arena = {};
  internal_fresh_arena(&arena);

  ra8_imgdec_scratch_t scratch = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_carve(&mux, &arena, &scratch));
  TEST_ASSERT_NOT_NULL(scratch.base);
  TEST_ASSERT(scratch.cap == (size_t)k_big_budget);
  TEST_ASSERT(scratch.live == 0U);

  /* The hungriest member\'s whole budget really is available from it. */
  void* const block = ra8_imgdec_scratch_alloc(&scratch, (size_t)k_big_budget);
  TEST_ASSERT_NOT_NULL(block);

  TEST_END("one carve funds whichever member the router picks");
}

RA8_INTERNAL static void internal_test_scratchless_set_carves_nothing(void) {
  TEST_BEGIN("a set that needs no scratch carves nothing and keeps the arena whole");

  fake_backend_t free_a = {.formats = (uint32_t)k_ra8_imgdec_format_png,
                           .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgba8888,
                           .scratch = 0U};
  fake_backend_t free_b = {.formats = (uint32_t)k_ra8_imgdec_format_jpeg,
                           .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgb888,
                           .scratch = 0U};

  ra8_imgdec_mux_t mux = {};
  internal_build(&mux, &free_a, &free_b);

  uint32_t bytes = 0xFFFFU;
  uint32_t align = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_scratch_budget(&mux, &bytes, &align));
  TEST_ASSERT(bytes == 0U);

  ra8_arena_t arena = {};
  internal_fresh_arena(&arena);
  uint32_t before = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_remaining(&arena, &before));

  ra8_imgdec_scratch_t scratch = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_carve(&mux, &arena, &scratch));
  TEST_ASSERT_NULL(scratch.base);
  TEST_ASSERT(scratch.cap == 0U);

  uint32_t after = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_remaining(&arena, &after));
  TEST_ASSERT(after == before);

  TEST_END("a set that needs no scratch carves nothing and keeps the arena whole");
}

RA8_INTERNAL static void internal_test_strong_align_is_refused(void) {
  TEST_BEGIN("a member wanting more alignment than the contract gives is refused");

  fake_backend_t picky = {.formats = (uint32_t)k_ra8_imgdec_format_webp,
                          .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgba8888,
                          .scratch = (uint32_t)k_small_budget,
                          .align   = (uint32_t)k_strong_align};

  ra8_imgdec_mux_t mux = {};
  internal_build(&mux, &picky, nullptr);

  uint32_t bytes = 0U;
  uint32_t align = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_scratch_budget(&mux, &bytes, &align));
  TEST_ASSERT(align == (uint32_t)k_strong_align);

  ra8_arena_t arena = {};
  internal_fresh_arena(&arena);
  uint32_t before = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_remaining(&arena, &before));

  ra8_imgdec_scratch_t scratch = {};
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_imgdec_mux_carve(&mux, &arena, &scratch));
  TEST_ASSERT_NULL(scratch.base);

  uint32_t after = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_remaining(&arena, &after));
  TEST_ASSERT(after == before);

  TEST_END("a member wanting more alignment than the contract gives is refused");
}

RA8_INTERNAL static void internal_test_arena_too_small(void) {
  TEST_BEGIN("a peak the arena cannot fund is no_mem, and spends nothing");

  fake_backend_t huge = {.formats = (uint32_t)k_ra8_imgdec_format_webp,
                         .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgba8888,
                         .scratch = (uint32_t)k_pool_bytes * 2U};

  ra8_imgdec_mux_t mux = {};
  internal_build(&mux, &huge, nullptr);

  ra8_arena_t arena = {};
  internal_fresh_arena(&arena);
  uint32_t before = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_remaining(&arena, &before));

  ra8_imgdec_scratch_t scratch = {};
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_imgdec_mux_carve(&mux, &arena, &scratch));
  TEST_ASSERT_NULL(scratch.base);

  uint32_t after = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_remaining(&arena, &after));
  TEST_ASSERT(after == before);

  TEST_END("a peak the arena cannot fund is no_mem, and spends nothing");
}

RA8_INTERNAL static void internal_test_guards(void) {
  TEST_BEGIN("an empty or absent set is refused before any arena is touched");

  fake_backend_t one = {.formats = (uint32_t)k_ra8_imgdec_format_png,
                        .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgba8888,
                        .scratch = (uint32_t)k_small_budget};

  uint32_t bytes = 0xFFFFU;
  uint32_t align = 0xFFFFU;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_mux_scratch_budget(nullptr, &bytes, &align));
  TEST_ASSERT(bytes == 0U);
  TEST_ASSERT(align == 0U);

  ra8_imgdec_mux_t mux = {};
  internal_build(&mux, &one, nullptr);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_mux_scratch_budget(&mux, nullptr, &align));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_mux_scratch_budget(&mux, &bytes, nullptr));

  /* An initialised but empty set is uninitialised, same as every other query. */
  ra8_imgdec_mux_t empty = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_init(&empty));
  bytes = 0xFFFFU;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_imgdec_mux_scratch_budget(&empty, &bytes, &align));
  TEST_ASSERT(bytes == 0U);

  ra8_arena_t arena = {};
  internal_fresh_arena(&arena);
  ra8_imgdec_scratch_t scratch = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_mux_carve(&mux, nullptr, &scratch));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_mux_carve(&mux, &arena, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_mux_carve(nullptr, &arena, &scratch));
  TEST_ASSERT_NULL(scratch.base);
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_imgdec_mux_carve(&empty, &arena, &scratch));
  TEST_ASSERT_NULL(scratch.base);

  uint32_t after = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_remaining(&arena, &after));
  TEST_ASSERT(after == (uint32_t)k_pool_bytes);

  TEST_END("an empty or absent set is refused before any arena is touched");
}

RA8_INTERNAL static void internal_test_carve_is_binding_time(void) {
  TEST_BEGIN("the carved scratch rewinds between decodes instead of re-carving");

  fake_backend_t big = {.formats = (uint32_t)k_ra8_imgdec_format_webp,
                        .pixels  = (uint32_t)k_ra8_imgdec_pixel_rgba8888,
                        .scratch = (uint32_t)k_big_budget};

  ra8_imgdec_mux_t mux = {};
  internal_build(&mux, &big, nullptr);

  ra8_arena_t arena = {};
  internal_fresh_arena(&arena);

  ra8_imgdec_scratch_t scratch = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_mux_carve(&mux, &arena, &scratch));
  uint32_t after_carve = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_remaining(&arena, &after_carve));

  /* Three decodes\' worth of draw and release, the arena untouched throughout. */
  for (uint32_t i = 0U; i < 3U; ++i) {
    void* const block = ra8_imgdec_scratch_alloc(&scratch, (size_t)k_big_budget);
    TEST_ASSERT_NOT_NULL(block);
    ra8_imgdec_scratch_free(&scratch, block);
    TEST_ASSERT(scratch.live == 0U);
    TEST_ASSERT(scratch.offset == 0U);

    uint32_t now = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_remaining(&arena, &now));
    TEST_ASSERT(now == after_carve);
  }

  TEST_END("the carved scratch rewinds between decodes instead of re-carving");
}

int main(void) {
  internal_test_budget_is_the_peak();
  internal_test_silent_align_does_not_weaken();
  internal_test_carve_funds_the_hungriest();
  internal_test_scratchless_set_carves_nothing();
  internal_test_strong_align_is_refused();
  internal_test_arena_too_small();
  internal_test_guards();
  internal_test_carve_is_binding_time();
  return 0;
}
