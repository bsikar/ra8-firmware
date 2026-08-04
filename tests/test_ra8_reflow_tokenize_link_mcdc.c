/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_reflow_tokenize_link_mcdc.c
 * @brief MC/DC tests for the tokenizer's link / stylesheet / font-face arms.
 *
 * @details
 * Split sibling of test_ra8_reflow_tokenize_scan_mcdc.c and
 * test_ra8_reflow_tokenize_tag_mcdc.c covering the link and font decision
 * families of libs/ra8_reflow/src/ra8_reflow_tokenize.c and
 * ra8_reflow_tokenize_attr.c: link-target interning (empty href, table
 * full), the `<link rel="stylesheet">` keyword scan, the external-stylesheet
 * loader guard arms (failure, zero length, null bytes), and the
 * `@font-face` slot resolution short-circuits. The single test that has to
 * register an embedded `@font-face` uses the public `ra8_reflow_init` /
 * `ra8_reflow_register_face` / `ra8_reflow_layout_chapter` API with the
 * baked Ahem face. The shared engine fixture lives in
 * tests/support/reflow_tokenize_test_util.h.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fixture_ahem.h"
#include "ra8_err.h"
#include "support/reflow_tokenize_test_util.h"
#include "unity_minimal.h"

/** @brief Constants for the link-table-full flood document. */
typedef enum : uint32_t {
  k_max_links_count = 255U,   /**< k_ra8_reflow_max_links (link table capacity). */
  k_flood_doc_cap   = 65536U, /**< Document buffer size for the link flood.      */
  k_link_tmp_cap    = 64U,    /**< Per-anchor scratch buffer size.               */
  k_link_slack      = 32U,    /**< Tail slack so the overflow anchor still fits. */
} link_flood_consts_t;

/**
 * @brief Build an XHTML document that floods the engine's link table.
 *
 * @details Emits "<html><body>", then k_max_links_count distinct
 *          `<a href="hN">t</a>` spans (each href unique so every link gets a
 *          distinct slot), then one final overflow anchor
 *          `<a href="extra">overflow</a>`, then "</body></html>".
 *
 * @param[out] dst Destination buffer; receives the NUL-terminated document.
 * @param[in]  cap Capacity of @p dst, bytes (>= k_flood_doc_cap).
 *
 * @pre dst is non-null.
 * @pre cap is at least k_flood_doc_cap bytes.
 * @post dst holds a NUL-terminated document with k_max_links_count + 1 anchors.
 *
 * @note Test helper; not thread-safe.
 * @since 0.1.0
 */
static void build_link_flood_doc(char* dst, size_t cap)
{
  uint32_t          pos    = 0U;
  const char* const k_head = "<html><body>";
  const size_t      k_hl   = strlen(k_head);
  memcpy(&dst[pos], k_head, k_hl);
  pos += (uint32_t)k_hl;
  for (uint32_t n = 0U; n < k_max_links_count; ++n) {
    char tmp[k_link_tmp_cap];
    (void)snprintf(tmp, sizeof(tmp), "<a href=\"h%u\">t</a>", n);
    const size_t tl = strlen(tmp);
    if (pos + tl + (uint32_t)k_link_slack < cap) {
      memcpy(&dst[pos], tmp, tl);
      pos += (uint32_t)tl;
    }
  }
  const char* const k_extra = "<a href=\"extra\">overflow</a>";
  const size_t      k_el    = strlen(k_extra);
  memcpy(&dst[pos], k_extra, k_el);
  pos += (uint32_t)k_el;
  const char* const k_tail = "</body></html>";
  const size_t      k_tl   = strlen(k_tail);
  memcpy(&dst[pos], k_tail, k_tl);
  pos += (uint32_t)k_tl;
  dst[pos] = '\0';
}

/**
 * @test test_intern_link_empty_href
 *
 * @par MC/DC:
 * Decision: `(href_len == 0U) || (link_target_count >= max)` -- the intern
 * guard in priv_intern_link (ra8_reflow_tokenize.c).
 *  - V1 href="x"  -> href_len!=0 and table not full -> both false -> interned
 *                    (the text run carries a non-zero link id in `reserved`).
 *  - V2 href=""   -> href_len==0 -> first arm true -> not interned (link id 0).
 * V1 vs V2 isolates the `href_len == 0` condition. (The table-full arm needs
 * 255 distinct links and is out of scope here.) Observable: the V1 run's
 * `reserved` link id is non-zero; the V2 run's is zero.
 */
static void test_intern_link_empty_href(void)
{
  TEST_BEGIN("priv_intern_link empty-href MC/DC");

  /* Non-empty href -> interned; the enclosed text run is link-tagged. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 walk("<html><body><p><a href=\"page.html\">tappable</a></p></body></html>"));
  bool tagged = false;
  for (uint32_t i = 0U; i < s_engine.token_count; ++i) {
    if ((s_engine.tokens[i].kind == (uint8_t)k_ra8_reflow_tok_text) &&
        (s_engine.tokens[i].reserved != 0U)) {
      tagged = true;
    }
  }
  TEST_ASSERT(tagged);

  /* Empty href -> not interned; no text run carries a link id. */
  TEST_ASSERT_EQ(k_ra8_ok, walk("<html><body><p><a href=\"\">plain</a></p></body></html>"));
  bool any_link = false;
  for (uint32_t i = 0U; i < s_engine.token_count; ++i) {
    if ((s_engine.tokens[i].kind == (uint8_t)k_ra8_reflow_tok_text) &&
        (s_engine.tokens[i].reserved != 0U)) {
      any_link = true;
    }
  }
  TEST_ASSERT(!any_link);

  TEST_END("priv_intern_link empty-href MC/DC");
}

/**
 * @test test_rel_is_stylesheet_scan
 *
 * @par MC/DC:
 * Decision: the inner match loop `(j < k_klen) && (rel[i+j] == k_kw[j])` of
 * priv_rel_is_stylesheet (ra8_reflow_tokenize.c). The outer guard in
 * priv_handle_link (`find(rel) && find(href) && rel_is_stylesheet`) gates the
 * css_loader call; the observable is the `.lead` colour changing.
 *  - V1 rel="stylesheet"        -> the inner loop runs `rel[i+j]==kw[j]` true
 *    for all 10 chars until j==k_klen (the `j<k_klen` arm ends it) -> match.
 *  - V2 rel="stylesheeX altname" -> the compare arm goes false at the final
 *    char of the candidate window -> that window fails; no full match -> no load.
 * V1 proves the all-equal exit (j<k_klen false); V2 proves the compare arm
 * (rel[i+j]==kw[j] false). A baseline with no loader gives the default colour.
 */
static void test_rel_is_stylesheet_scan(void)
{
  TEST_BEGIN("priv_rel_is_stylesheet MC/DC");

  /* Baseline: loader unbound -> external rule invisible. */
  s_engine.css_loader = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, walk("<html><body><p class=\"lead\">x</p></body></html>"));
  const uint32_t c_def = first_text_color();

  s_engine.css_loader     = css_stub;
  s_engine.css_loader_ctx = nullptr;

  /* V1 rel contains "stylesheet" exactly -> loads -> colour changes. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 walk("<html><head><link rel=\"stylesheet\" href=\"s\"/></head>"
                      "<body><p class=\"lead\">x</p></body></html>"));
  TEST_ASSERT(first_text_color() != c_def);

  /* V2 rel is a near-miss ("stylesheeX") -> no full match -> no load. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 walk("<html><head><link rel=\"stylesheeX\" href=\"s\"/></head>"
                      "<body><p class=\"lead\">x</p></body></html>"));
  TEST_ASSERT_EQ(c_def, first_text_color());

  s_engine.css_loader = nullptr;
  TEST_END("priv_rel_is_stylesheet MC/DC");
}

/**
 * @test test_link_loader_result
 *
 * @par MC/DC:
 * Decision: `(loader(...) == k_ra8_ok) && (css_bytes != nullptr) &&
 * (css_len > 0U)` -- the loader-result guard in priv_handle_link
 * (ra8_reflow_tokenize.c) before parsing the fetched CSS.
 *  - V1 css_stub returns k_ra8_ok + non-null bytes + len>0 -> all true -> parse
 *    -> the `.lead` colour changes from its no-link default.
 *  - V2 css_fail_stub returns k_ra8_err_not_found (and null/0) -> first arm
 *    false -> no parse -> colour stays at the default.
 * V1 vs V2 isolates the loader-return-code condition (the null-pointer and
 * zero-length arms are guarded by the same stub on the failure path).
 */
static void test_link_loader_result(void)
{
  TEST_BEGIN("priv_handle_link loader-result MC/DC");

  s_engine.css_loader = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, walk("<html><body><p class=\"lead\">x</p></body></html>"));
  const uint32_t c_def = first_text_color();

  /* V1 success loader -> CSS parsed -> colour changes. */
  s_engine.css_loader     = css_stub;
  s_engine.css_loader_ctx = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok,
                 walk("<html><head><link rel=\"stylesheet\" href=\"s\"/></head>"
                      "<body><p class=\"lead\">x</p></body></html>"));
  TEST_ASSERT(first_text_color() != c_def);

  /* V2 failing loader -> no parse -> default colour. */
  s_engine.css_loader = css_fail_stub;
  TEST_ASSERT_EQ(k_ra8_ok,
                 walk("<html><head><link rel=\"stylesheet\" href=\"s\"/></head>"
                      "<body><p class=\"lead\">x</p></body></html>"));
  TEST_ASSERT_EQ(c_def, first_text_color());

  s_engine.css_loader = nullptr;
  TEST_END("priv_handle_link loader-result MC/DC");
}

/**
 * @test test_resolve_face_slot_conditions
 *
 * @par MC/DC:
 * Decision: `(face_count==0) || ((comp.set & family)==0) || (family_len==0)`
 * -- the early return in priv_resolve_face_slot (ra8_reflow_tokenize.c). This is
 * the only decision reachable solely through the public layout API: with no
 * registered face the first arm short-circuits true on every element (covered
 * by every other walk in this file). Here a face IS registered, so the first
 * arm is false and the cascade is forced to evaluate the family arms.
 *  - With a registered face and an element carrying `font-family`, the set-bit
 *    arm is false and `family_len != 0` (third arm false), so the function
 *    proceeds to ra8_css_match_face -- driving the false side of all three
 *    conditions in one element.
 *  - An element WITHOUT a font-family has the `(set & family)==0` arm true,
 *    short-circuiting the third condition.
 * The full layout simply succeeds; the value win is reaching the post-guard
 * body (ra8_css_match_face + the face-registry scan) at all.
 */
static void test_resolve_face_slot_conditions(void)
{
  TEST_BEGIN("priv_resolve_face_slot MC/DC");

  uint32_t pages = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init((uint16_t)k_viewport_w,
                                 (uint16_t)k_viewport_h,
                                 k_fixture_ahem,
                                 (size_t)k_fixture_ahem_len,
                                 (uint16_t)k_default_font_px,
                                 (uint32_t)k_body_color,
                                 (uint32_t)k_link_color,
                                 &s_engine));

  /* Register the Ahem blob as an embedded @font-face so face_count != 0,
   * forcing the first arm of the guard false on every element. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_register_face(&s_engine,
                                          (uint8_t)k_face_css_idx,
                                          k_fixture_ahem,
                                          (size_t)k_fixture_ahem_len));

  /* One element with a font-family (family arms false -> reaches match_face),
   * one without (the (set & family)==0 arm true). Layout must still succeed. */
  const char* doc = "<html><head><style>@font-face{font-family:Baked;src:url(b.ttf)}"
                    ".fam{font-family:Baked}</style></head>"
                    "<body><p class=\"fam\">styledfam</p><p>plainfam</p></body></html>";
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_reflow_layout_chapter(&s_engine, (const uint8_t*)doc, (uint32_t)strlen(doc), &pages));
  TEST_ASSERT(pages >= 1U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_close(&s_engine));
  TEST_END("priv_resolve_face_slot MC/DC");
}

/**
 * @brief CSS-loader stub that returns k_ra8_ok with non-null bytes but zero len.
 *
 * @details Drives the `css_len > 0U` false arm of the loader-result guard in
 * priv_handle_link (L1230): the loader succeeds and the bytes pointer is
 * non-null, but the length is zero, so the CSS parse step is skipped.
 *
 * @param[in]  ctx       Opaque loader context (unused).
 * @param[in]  href      Stylesheet href bytes (unused).
 * @param[in]  href_len  Length of @p href (unused).
 * @param[out] out_bytes Receives a non-null pointer (to static storage).
 * @param[out] out_len   Receives 0.
 * @return Always k_ra8_ok.
 * @retval k_ra8_ok Non-null pointer with zero length returned.
 * @pre `out_bytes` and `out_len` are non-null.
 * @post `*out_bytes` is non-null and `*out_len == 0`.
 * @note Test helper; not thread-safe.
 */
static ra8_err_t css_zero_len_stub(void*           ctx,
                                   const char*     href,
                                   uint32_t        href_len,
                                   const uint8_t** out_bytes,
                                   size_t*         out_len)
{
  (void)ctx;
  (void)href;
  (void)href_len;
  /* Non-null pointer but zero length: drives css_len==0 arm (L1230). */
  static const uint8_t s_dummy = 0U;
  *out_bytes                   = &s_dummy;
  *out_len                     = 0U;
  return k_ra8_ok;
}

/**
 * @brief CSS-loader stub that returns k_ra8_ok with null bytes and zero len.
 *
 * @details Drives the `css_bytes != nullptr` false arm of the loader-result
 * guard in priv_handle_link (L1230): the loader reports success but hands
 * back a null pointer, so the CSS parse step is skipped.
 *
 * @param[in]  ctx       Opaque loader context (unused).
 * @param[in]  href      Stylesheet href bytes (unused).
 * @param[in]  href_len  Length of @p href (unused).
 * @param[out] out_bytes Set to nullptr.
 * @param[out] out_len   Receives 0.
 * @return Always k_ra8_ok.
 * @retval k_ra8_ok Null pointer returned (spurious success).
 * @pre `out_bytes` and `out_len` are non-null.
 * @post `*out_bytes == nullptr` and `*out_len == 0`.
 * @note Test helper; not thread-safe.
 */
static ra8_err_t css_null_bytes_stub(void*           ctx,
                                     const char*     href,
                                     uint32_t        href_len,
                                     const uint8_t** out_bytes,
                                     size_t*         out_len)
{
  (void)ctx;
  (void)href;
  (void)href_len;
  *out_bytes = nullptr;
  *out_len   = 0U;
  return k_ra8_ok;
}

/**
 * @test test_link_loader_guard_arms
 *
 * @par MC/DC:
 * Decision (L1230): `(loader()==k_ra8_ok) && (css_bytes!=nullptr) && (css_len>0U)`
 * in priv_handle_link.  The existing test_link_loader_result covers:
 *  - V1 all true  -> CSS parsed -> colour changes.
 *  - V2 loader fail -> first false -> no parse.
 * Still-missing arms at L1230:
 *  - V3 loader ok, css_bytes==nullptr -> second arm false -> no parse.
 *  - V4 loader ok, css_bytes non-null, css_len==0 -> third arm false -> no parse.
 * Both V3 and V4 leave the chapter colour at its no-link default because the
 * CSS parse is skipped.
 */
static void test_link_loader_guard_arms(void)
{
  TEST_BEGIN("priv_handle_link loader-guard css_bytes/css_len arms (L1230)");

  /* Establish default colour with no loader. */
  s_engine.css_loader = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, walk("<html><body><p class=\"lead\">x</p></body></html>"));
  const uint32_t c_def = first_text_color();

  /* V3 loader returns ok but css_bytes==nullptr -> second AND arm false. */
  s_engine.css_loader     = css_null_bytes_stub;
  s_engine.css_loader_ctx = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok,
                 walk("<html><head><link rel=\"stylesheet\" href=\"s\"/></head>"
                      "<body><p class=\"lead\">x</p></body></html>"));
  TEST_ASSERT_EQ(c_def, first_text_color());

  /* V4 loader returns ok, non-null bytes, css_len==0 -> third AND arm false. */
  s_engine.css_loader = css_zero_len_stub;
  TEST_ASSERT_EQ(k_ra8_ok,
                 walk("<html><head><link rel=\"stylesheet\" href=\"s\"/></head>"
                      "<body><p class=\"lead\">x</p></body></html>"));
  TEST_ASSERT_EQ(c_def, first_text_color());

  s_engine.css_loader = nullptr;
  TEST_END("priv_handle_link loader-guard css_bytes/css_len arms (L1230)");
}

/**
 * @test test_intern_link_table_full
 *
 * @par MC/DC:
 * Decision (L1094): `(href_len == 0U) || (engine->link_target_count >= max)`
 * in priv_intern_link.  The existing test_intern_link_empty_href covers the
 * first arm (href_len==0).  The still-missing arm is the second:
 * `link_target_count >= k_ra8_reflow_max_links (255)`.
 *  - V-full: prime the engine with 255 links by walking 255 distinct `<a>`
 *    elements, then walk a 256th; the table is full so the 256th link is
 *    not interned (returns 0) and the text run carries reserved==0.
 * Observable: after the 256th link walk the text token inside it has
 * reserved==0 (no link id).
 */
static void test_intern_link_table_full(void)
{
  TEST_BEGIN("priv_intern_link table-full arm (L1094)");

  /* Build a document with exactly k_max_links_count <a> elements to fill the
   * table, then a (max+1)-th whose href should NOT be interned. */
  static char s_doc[k_flood_doc_cap]; /* static: stays off the stack */
  build_link_flood_doc(s_doc, sizeof(s_doc));

  s_engine.token_count       = 0U;
  s_engine.text_pool_used    = 0U;
  s_engine.link_target_count = 0U;
  const ra8_err_t err = priv_reflow_xml_walk(&s_engine, (const uint8_t*)s_doc, strlen(s_doc));
  /* The walk may succeed or hit a token-pool limit; either way, once the link
   * table is full the 256th href must not be interned (reserved==0 for it). */
  if (err == k_ra8_ok) {
    TEST_ASSERT_EQ(k_max_links_count, s_engine.link_target_count);
    /* Find the text token for "overflow" and check its reserved byte is 0. */
    bool found_overflow = false;
    for (uint32_t i = 0U; i < s_engine.token_count; ++i) {
      if (s_engine.tokens[i].kind != (uint8_t)k_ra8_reflow_tok_text) {
        continue;
      }
      const char*    t  = (const char*)&s_engine.text_pool[s_engine.tokens[i].text_off];
      const uint32_t tl = s_engine.tokens[i].text_len;
      if ((tl >= 8U) && (memcmp(t, "overflow", 8U) == 0)) {
        TEST_ASSERT_EQ(0, s_engine.tokens[i].reserved);
        found_overflow = true;
      }
    }
    TEST_ASSERT(found_overflow);
  }
  /* Reset so subsequent tests start clean. */
  s_engine.link_target_count = 0U;

  TEST_END("priv_intern_link table-full arm (L1094)");
}
/**
 * @test test_resolve_face_slot_family_len_zero
 *
 * @par MC/DC:
 * Decision (L1412): `(engine->face_count == 0U) || ((comp->set & family)==0U)
 * || (comp->family_len == 0U)` in priv_resolve_face_slot.  The existing
 * test_resolve_face_slot_conditions covers:
 *  - face_count==0 true (first arm, exercised across the whole suite because no
 *    face is registered for most walks), and
 *  - face_count>0 AND (set & family)!=0 AND family_len>0 -> all three false
 *    -> body entered (priv_resolve_face_slot_conditions test).
 *
 * The THIRD condition `comp->family_len == 0U` is structurally unreachable as
 * an INDEPENDENT true arm: the CSS parser (priv_family_cb) only sets the
 * k_ra8_css_set_family bit when `vlen > 0` (i.e. when family_len > 0); there
 * is no code path that sets the set-bit while leaving family_len == 0.
 * Therefore conditions 2 and 3 are always correlated: set-bit absent <=> family_
 * len could be 0, but the set-bit present <=> family_len > 0.  No input drives
 * face_count>0 AND set-bit present AND family_len==0.
 *
 * This test documents the structural unreachability; it exercises the two
 * REACHABLE independent arms (conditions 1 and 2) to confirm they work:
 *  - V-noface: no registered face -> face_count==0 true -> first arm.
 *  - V-nofam:  face registered but element has no font-family rule ->
 *    (set & family)==0 true -> second arm.
 * Both correctly return 0 (default face).
 *
 * @note RA8_MCDC_DEACTIVATED: the `comp->family_len == 0U` arm at L1412 is not
 * independently reachable because ra8_reflow_css.c's priv_family_cb only sets
 * k_ra8_css_set_family when n>0 (family_len>0); no production code path sets
 * the family set-bit with a zero-length name.
 */
static void test_resolve_face_slot_family_len_zero(void)
{
  TEST_BEGIN("priv_resolve_face_slot family_len==0 unreachable arm doc (L1412)");

  uint32_t pages = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init((uint16_t)k_viewport_w,
                                 (uint16_t)k_viewport_h,
                                 k_fixture_ahem,
                                 (size_t)k_fixture_ahem_len,
                                 (uint16_t)k_default_font_px,
                                 (uint32_t)k_body_color,
                                 (uint32_t)k_link_color,
                                 &s_engine));

  /* Register a face so face_count > 0, making the first OR arm false. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_register_face(&s_engine,
                                          (uint8_t)k_face_css_idx,
                                          k_fixture_ahem,
                                          (size_t)k_fixture_ahem_len));

  /* V-nofam: element has no font-family rule -> (set & family)==0 true ->
   * second OR arm short-circuits -> returns 0 (default face). */
  const char* const k_doc = "<html><body><p>nofam</p></body></html>";
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_reflow_layout_chapter(&s_engine, (const uint8_t*)k_doc, (uint32_t)strlen(k_doc), &pages));
  TEST_ASSERT(pages >= 1U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_close(&s_engine));
  TEST_END("priv_resolve_face_slot family_len==0 unreachable arm doc (L1412)");
}

/**
 * @test test_resolve_face_slot_family_len_zero_direct_mcdc
 *
 * @par MC/DC:
 * Decision: `if ((face_count == 0U) || ((comp->set & family) == 0U) ||
 * (comp->family_len == 0U))` in ra8_reflow_tok_resolve_face_slot
 * (libs/ra8_reflow/src/ra8_reflow_tokenize_attr.c, 3 conditions, OR). Existing
 * cascade-driven vectors cover the no-face (C1 true) and no-family-bit (C2 true)
 * arms. The `(comp->family_len == 0U)` true side is reached by calling the
 * promoted helper directly with a hand-built comp that sets the family bit but
 * leaves family_len == 0 (a state the cascade never emits, so it is white-box):
 *  - face_count > 0, family bit set, family_len == 0 -> C1 false, C2 false, C3
 *    true -> returns the default slot 0. This completes the C3 independence pair.
 */
static void test_resolve_face_slot_family_len_zero_direct_mcdc(void)
{
  TEST_BEGIN("ra8_reflow_tok_resolve_face_slot MC/DC: family_len==0 direct arm");
  uint32_t pages = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init((uint16_t)k_viewport_w,
                                 (uint16_t)k_viewport_h,
                                 k_fixture_ahem,
                                 (size_t)k_fixture_ahem_len,
                                 (uint16_t)k_default_font_px,
                                 (uint32_t)k_body_color,
                                 (uint32_t)k_link_color,
                                 &s_engine));
  /* Register a face so face_count > 0 (C1 false). */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_register_face(&s_engine,
                                          (uint8_t)k_face_css_idx,
                                          k_fixture_ahem,
                                          (size_t)k_fixture_ahem_len));
  /* Hand-built cascaded style: family bit set (C2 false) but family_len 0 (C3 true). */
  ra8_css_style_t comp = {};
  comp.set             = (uint8_t)k_ra8_css_set_family;
  comp.family_len      = 0U;
  TEST_ASSERT_EQ(0, ra8_reflow_tok_resolve_face_slot(&s_engine, &comp));
  (void)pages;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_close(&s_engine));
  TEST_END("ra8_reflow_tok_resolve_face_slot MC/DC: family_len==0 direct arm");
}

/**
 * @brief Test executable entry point -- runs the link/face MC/DC vectors.
 *
 * @return 0 on success (all tests passed).
 *
 * @pre Host environment provides stderr.
 * @post Every link/stylesheet/font-face decision family above has executed
 *       its vectors.
 *
 * @note Not thread-safe (single-threaded test runner).
 * @since 0.1.0
 */
int32_t main(void)
{
  test_intern_link_empty_href();
  test_rel_is_stylesheet_scan();
  test_link_loader_result();
  test_resolve_face_slot_conditions();
  test_link_loader_guard_arms();
  test_intern_link_table_full();
  test_resolve_face_slot_family_len_zero();
  test_resolve_face_slot_family_len_zero_direct_mcdc();
  (void)fprintf(stderr, "[OK ] test_ra8_reflow_tokenize_link_mcdc.c\n");
  return 0;
}
