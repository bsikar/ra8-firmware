/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_face_select.c
 * @brief #109 (items 2-3): per-run embedded `@font-face` selection in ra8_reflow.
 *
 * @details
 * The #109 foundation loads + binds a single embedded face; #142 parses the
 * `@font-face` table + resolves `font-family`. This test covers the remainder:
 * a text run whose cascaded `font-family` + bold/italic matches a registered
 * `@font-face` is laid out with that face, a different weight selects a
 * *different* registered face, and an unmatched (or absent) family falls back to
 * the engine's default. It also asserts the 4b cache-bypass invariant (a
 * multi-face book is never serialized/loaded) and `ra8_reflow_register_face`'s
 * validation + cap.
 *
 * Only one TTF (the baked Ahem fixture) is in tree, so -- as the issue's plan
 * anticipates -- distinctness is proven through the per-run face *routing* (the
 * face index packed into each glyph's `style`), not distinct render bitmaps: the
 * same blob is registered under two `(family, weight)` keys and the bold vs
 * regular runs must resolve to *distinct* registry slots.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fixture_ahem.h"
#include "ra8_err.h"
#include "ra8_reflow.h"
#include "ra8_reflow_cache.h"
#include "unity_minimal.h"

/** @brief Fill byte for the deliberately invalid face blob. */
typedef enum : uint8_t {
  k_face_fill_garbage = 0xA5U, /**< Matches no face signature. */
} face_fill_t;

/** @brief Local sizing constants (no magic numbers). */
typedef enum : uint32_t {
  k_tfs_viewport_w = 600U,      /**< Layout viewport width, px.       */
  k_tfs_viewport_h = 800U,      /**< Layout viewport height, px.      */
  k_tfs_font_px    = 16U,       /**< Body font size, px.              */
  k_tfs_body_color = 0x000000U, /**< Black body text.                 */
  k_tfs_link_color = 0x0000FFU, /**< Blue links.                      */
  k_tfs_blob_cap   = 65536U,    /**< Cache serialize scratch, bytes.  */
  k_tfs_garbage    = 64U,       /**< Crafted invalid-blob length.     */
  k_tfs_two_faces  = 2U,        /**< `@font-face` rules in k_chapter. */
} tfs_const_t;

/** @brief Reflow engine under test (file-scope: large struct). */
static ra8_reflow_t s_engine;
/** @brief Cache-serialize scratch buffer. */
static uint8_t s_blob[k_tfs_blob_cap];

/**
 * @brief A chapter declaring two `@font-face` rules for family "Book" (regular +
 *        bold) plus runs marked by distinct code points: 'R' = Book regular,
 *        'B' = Book bold, 'U' = unmatched family, 'N' = no font-family.
 */
static const char* const k_chapter =
  "<html><head><style>"
  "@font-face{font-family:Book;font-weight:normal;src:url(reg.ttf)}"
  "@font-face{font-family:Book;font-weight:bold;src:url(bold.ttf)}"
  ".bk{font-family:Book}"
  ".ot{font-family:Other}"
  "</style></head><body>"
  "<p class=\"bk\">R<b>B</b></p>"
  "<p class=\"ot\">U</p>"
  "<p>N</p>"
  "</body></html>";

/** @brief Init the engine with the baked Ahem face as the default. */
static void tfs_init(void)
{
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init((uint16_t)k_tfs_viewport_w,
                                 (uint16_t)k_tfs_viewport_h,
                                 k_fixture_ahem,
                                 (size_t)k_fixture_ahem_len,
                                 (uint16_t)k_tfs_font_px,
                                 (uint32_t)k_tfs_body_color,
                                 (uint32_t)k_tfs_link_color,
                                 &s_engine));
}

/** @brief Lay out ::k_chapter into the engine. */
static void tfs_layout(void)
{
  uint32_t pages = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_reflow_layout_chapter(&s_engine, (const uint8_t*)k_chapter, strlen(k_chapter), &pages));
}

/**
 * @brief Face slot (glyph `style` high nibble) of the first glyph with code
 *        point @p cp, or -1 if absent.
 */
static int32_t tfs_face_of(int32_t cp)
{
  for (uint32_t i = 0U; i < s_engine.glyph_count; ++i) {
    if (s_engine.glyphs[i].cp == cp) {
      return (int32_t)(((uint32_t)s_engine.glyphs[i].style >> (uint32_t)k_ra8_reflow_face_shift) &
                       (uint32_t)k_ra8_reflow_face_mask);
    }
  }
  return -1;
}

/** @brief Register the baked Ahem blob once per parsed `@font-face` entry. */
static void tfs_register_all_faces(void)
{
  for (uint16_t ci = 0U; ci < s_engine.css.face_count; ++ci) {
    TEST_ASSERT_EQ(
      k_ra8_ok,
      ra8_reflow_register_face(&s_engine, (uint8_t)ci, k_fixture_ahem, (size_t)k_fixture_ahem_len));
  }
}

/**
 * @test Per-run embedded face selection routes each run to the right face.
 *
 * @par MC/DC:
 * Decision: the `priv_resolve_face_slot` guard (libs/ra8_reflow/src/
 * ra8_reflow_tokenize.c) `if (face_count==0 || (set & family)==0 ||
 * family_len==0)` -> default face (0); observed as each run's glyph face index.
 * - Vector 1 (all conditions false): faces registered + a run with font-family
 *     Book -> face != 0 (the resolve path). Control.
 * - Vector 2 (face_count==0 true): NO faces registered, same family -> face 0.
 *     Varies face_count only.
 * - Vector 3 ((set & family)==0 true): faces registered, the 'N' run carries no
 *     font-family -> face 0. Varies the family-present condition only.
 * Vectors 1+2 prove face_count independently flips the outcome; 1+3 prove the
 * family-present condition does. (The third guard term, family_len==0 with the
 * family flag set, is unreachable from the cascade, which only sets the flag with
 * a real slice.) The regular vs bold runs additionally prove the (weight,style)
 * match routes to DISTINCT registry slots, and the 'U' run proves an unmatched
 * family falls back to the default rather than faulting.
 */
static void test_face_select_routing(void)
{
  TEST_BEGIN("face_select: per-run routing + mcdc");
  tfs_init();

  /* Vector 2 (face_count==0): no faces registered -> every run is the default. */
  tfs_layout();
  TEST_ASSERT_EQ(k_tfs_two_faces, s_engine.css.face_count);
  TEST_ASSERT_EQ(0, tfs_face_of('R'));
  TEST_ASSERT_EQ(0, tfs_face_of('B'));
  TEST_ASSERT_EQ(0, tfs_face_of('U'));
  TEST_ASSERT_EQ(0, tfs_face_of('N'));

  /* Register Ahem for both @font-face entries, then re-flow. */
  tfs_register_all_faces();
  TEST_ASSERT_EQ(k_tfs_two_faces, s_engine.face_count);
  tfs_layout();

  const int32_t fr = tfs_face_of('R'); /* Book regular                              */
  const int32_t fb = tfs_face_of('B'); /* Book bold                                 */
  const int32_t fu = tfs_face_of('U'); /* unmatched family                          */
  const int32_t fn = tfs_face_of('N'); /* no font-family                            */
  TEST_ASSERT(fr > 0);                 /* Vector 1: regular run resolves a face     */
  TEST_ASSERT(fb > 0);                 /* bold run resolves a face                  */
  TEST_ASSERT(fr != fb);               /* bold selects a face DISTINCT from regular */
  TEST_ASSERT_EQ(0, fu);               /* unmatched family -> default (no-match)    */
  TEST_ASSERT_EQ(0, fn);               /* Vector 3: no font-family -> default       */

  (void)ra8_reflow_close(&s_engine);
  TEST_END("face_select: per-run routing + mcdc");
}

/**
 * @test While an embedded face is registered the pagination cache is bypassed
 *       (the 4b invariant), so a multi-face book is never serialized or
 *       mis-served under a different face set.
 *
 * @par MC/DC:
 * (no compound decision is varied by this case -- it flips the single-condition
 * `if (engine->face_count != 0U)` bypass guard in priv_cache_precheck: with zero
 * faces the serialize proceeds, and once a face is registered both cache entry
 * points return invalid_state. The content-null compound guard on that path is
 * held at its content-non-null short-circuit leg.)
 */
static void test_cache_bypass_with_faces(void)
{
  TEST_BEGIN("face_select: cache bypassed when faces registered");
  tfs_init();
  tfs_layout();

  /* Single-face (empty registry): the cache serializes normally. */
  size_t n = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_cache_serialize(&s_engine,
                                            (const uint8_t*)k_chapter,
                                            strlen(k_chapter),
                                            s_blob,
                                            sizeof(s_blob),
                                            &n));
  TEST_ASSERT(n > 0U);

  /* Register a face -> both cache entry points now bypass (invalid_state). */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_reflow_register_face(&s_engine, 0U, k_fixture_ahem, (size_t)k_fixture_ahem_len));
  size_t n2 = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_reflow_cache_serialize(&s_engine,
                                            (const uint8_t*)k_chapter,
                                            strlen(k_chapter),
                                            s_blob,
                                            sizeof(s_blob),
                                            &n2));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_state,
    ra8_reflow_cache_load(&s_engine, (const uint8_t*)k_chapter, strlen(k_chapter), s_blob, n));

  (void)ra8_reflow_close(&s_engine);
  TEST_END("face_select: cache bypassed when faces registered");
}

/**
 * @test `ra8_reflow_register_face` validates its inputs, rejects an invalid blob
 *       without disturbing the registry, and enforces the face cap.
 *
 * @par MC/DC:
 * Decision: `if (engine == nullptr || blob == nullptr)` (2 conditions, OR,
 * `ra8_reflow_register_face` in libs/ra8_reflow/src/ra8_reflow_layout_driver.c).
 * - V1: engine=NULL, blob=valid  -> null_ptr (first condition true).
 * - V2: engine=valid, blob=NULL  -> null_ptr (second condition true).
 * - V3: engine=valid, blob=valid (the invalid_size / cap calls) -> proceeds past
 *       the guard (both conditions false: control).
 * V3+V1 prove `engine == nullptr` independently drives the outcome; V3+V2 prove
 * the same for `blob == nullptr`. N+1 = 3 vectors. The same function's blob-
 * validation OR `offset < 0 || stbtt_InitFont(...) == 0` is driven here at its
 * `offset < 0` leg (the garbage blob) and both-false leg (the valid faces); its
 * remaining InitFont-only leg is discharged by the source-text-equivalent
 * decision (DO-178C 6.4.4.3) in `ra8_reflow_bind_font` (test_bind_measure_and_mcdc).
 */
static void test_register_validation(void)
{
  TEST_BEGIN("face_select: register_face validation + cap");
  tfs_init();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_reflow_register_face(nullptr, 0U, k_fixture_ahem, (size_t)k_fixture_ahem_len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_reflow_register_face(&s_engine, 0U, nullptr, (size_t)k_fixture_ahem_len));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_reflow_register_face(&s_engine,
                                          0U,
                                          k_fixture_ahem,
                                          (size_t)k_ra8_reflow_min_font_bytes - 1U));

  /* Garbage (no sfnt tag) is rejected; the registry stays empty. */
  static uint8_t s_garbage[k_tfs_garbage];
  memset(s_garbage, k_face_fill_garbage, sizeof(s_garbage));
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_reflow_register_face(&s_engine, 0U, s_garbage, sizeof(s_garbage)));
  TEST_ASSERT_EQ(0, s_engine.face_count);

  /* Fill to the cap; the next register is rejected (no_mem). */
  for (uint8_t k = 0U; k < (uint8_t)k_ra8_reflow_max_faces; ++k) {
    TEST_ASSERT_EQ(
      k_ra8_ok,
      ra8_reflow_register_face(&s_engine, k, k_fixture_ahem, (size_t)k_fixture_ahem_len));
  }
  TEST_ASSERT_EQ(k_ra8_reflow_max_faces, s_engine.face_count);
  TEST_ASSERT_EQ(
    k_ra8_err_no_mem,
    ra8_reflow_register_face(&s_engine, 0U, k_fixture_ahem, (size_t)k_fixture_ahem_len));

  (void)ra8_reflow_close(&s_engine);
  TEST_END("face_select: register_face validation + cap");
}

int main(void)
{
  test_face_select_routing();
  test_cache_bypass_with_faces();
  test_register_validation();
  return 0;
}
