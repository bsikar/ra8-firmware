/**
 * @file test_reflow_css_select_mcdc.c
 * @brief MC/DC tests for the content-CSS selector / cascade / font-face arms.
 *
 * @details
 * Split sibling of test_reflow_css_parse_mcdc.c covering the match-side
 * decision families of reflow_css: selector-part parsing (combinator
 * drops, empty names), ancestor constraints (tag / class / id arms,
 * null-identity arms), cascade resolution (order tie-breaks, specificity
 * overrides), class-list token scanning, rule-family extraction, the
 * `@font-face` accept guards (family/src presence, quote stripping, weight
 * and style keywords), face matching (regular fallback, length mismatches,
 * bold/italic arms), and the face-src output guards. Every test reaches the
 * target decision through a public entry point with crafted CSS / element
 * input; the shared sheet fixture lives in
 * tests/inc/reflow_css_test_util.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "reflow_css_test_util.h"
#include "unity_minimal.h"

/**
 * @test internal_test_sel_part_combinator_drop
 * @brief A child / sibling combinator selector is unsupported -> dropped.
 *
 * @par MC/DC:
 * Decision (internal_parse_selector part loop):
 *   `((s[i] != '.') && (s[i] != '#')) || !internal_parse_sel_part(...)` (2-cond OR):
 *  - "p.note" -> at the '.' both `s[i]!='.'`/`s[i]!='#'` are false AND the part
 *    parses (intern OK) -> whole disjunction false -> rule kept (control).
 *  - "p>span" -> at '>' both `s[i]!='.'` and `s[i]!='#'` are true -> cond1 true
 *    -> return false -> rule dropped (isolates the combinator condition).
 *  - "p.a.b"  -> the second '.b' is a `.` (cond1 false) but internal_parse_sel_part
 *    fails on the duplicate class -> `!part` true -> rule dropped (isolates the
 *    intern/dup condition).
 * @details Exercises the sel part combinator drop path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_sel_part_combinator_drop(void)
{
  TEST_BEGIN("css selector part loop OR (combinator / dup-class drop)");
  load("p.note { color: red; }"   /* kept                      */
       "p>span { color: blue; }"  /* '>' combinator -> dropped */
       "p.a.b { color: navy; }"); /* second class -> dropped   */
  TEST_ASSERT_EQ(1, s_sheet.rule_count);
  TEST_ASSERT_EQ(k_reflow_tag_p, s_sheet.rules[0].sel_tag);
  TEST_ASSERT(s_sheet.rules[0].class_len > 0U);
  TEST_ASSERT_EQ(k_css_color_red, s_sheet.rules[0].decl.color);
  TEST_END("css selector part loop OR (combinator / dup-class drop)");
}

/**
 * @test internal_test_fontface_quote_strip
 * @brief @font-face family quotes (both `'` and `"`) are stripped; bare too.
 *
 * @par MC/DC:
 * Decision (internal_strip_quotes):
 *   `(*len>=2) && ((s[0]=='"') || (s[0]=='\'')) && (s[*len-1]==s[0])` (AND of
 * three, with an inner OR). Vectors:
 *  - "\"Body\"" -> len>=2 AND (double-quote) AND closing matches -> stripped to
 *    "Body" (the AND-true / OR-cond1 arm).
 *  - 'Body'     -> the single-quote OR-cond2 arm; stripped to "Body".
 *  - Body       -> s[0]=='B' -> the quote OR is false -> not stripped (still a
 *    valid family name, kept verbatim).
 * Matching on the resolved family proves which bytes were interned.
 * @details Exercises the fontface quote strip path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_fontface_quote_strip(void)
{
  TEST_BEGIN("css @font-face quote strip (\" / ' / bare)");
  load("@font-face{font-family:\"Body\";src:url(a.ttf)}"
       "@font-face{font-family:'Body';src:url(b.ttf)}"
       "@font-face{font-family:Body;src:url(c.ttf)}");
  TEST_ASSERT_EQ(3, s_sheet.face_count);
  /* All three faces resolve to the 4-byte family "Body" (quotes removed). */
  for (uint16_t i = 0U; i < s_sheet.face_count; ++i) {
    TEST_ASSERT_EQ(4, s_sheet.faces[i].family_len);
    TEST_ASSERT(css_test_bytes_equal(&s_sheet.names[s_sheet.faces[i].family_off], "Body", 4U));
  }
  TEST_END("css @font-face quote strip (\" / ' / bare)");
}

/**
 * @test internal_test_fontface_weight_style_keywords
 * @brief @font-face weight (6-OR) and style (2-OR) descriptor keywords.
 *
 * @par MC/DC:
 * internal_is_bold_kw: `bold||bolder||600||700||800||900` and
 * internal_is_italic_kw: `italic||oblique`, reached via @font-face descriptors:
 *  - a face with `font-weight:900`     -> weight_bold=1 (bold OR cond6).
 *  - a face with `font-weight:bolder`  -> weight_bold=1 (bold OR cond2).
 *  - a face with `font-style:oblique`  -> style_italic=1 (italic OR cond2).
 *  - a face with `font-weight:normal`  -> weight_bold=0 (bold OR all-false).
 * ra8_css_match_face then proves each face's recorded emphasis selects it.
 * @details Exercises the fontface weight style keywords path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_fontface_weight_style_keywords(void)
{
  TEST_BEGIN("css @font-face weight/style keyword ORs");
  load("@font-face{font-family:F;font-weight:900;src:url(w9.ttf)}"
       "@font-face{font-family:G;font-weight:bolder;src:url(wb.ttf)}"
       "@font-face{font-family:H;font-style:oblique;src:url(ho.ttf)}"
       "@font-face{font-family:N;font-weight:normal;src:url(nn.ttf)}");
  TEST_ASSERT_EQ(4, s_sheet.face_count);
  TEST_ASSERT_EQ(1, s_sheet.faces[0].weight_bold);  /* 900                */
  TEST_ASSERT_EQ(1, s_sheet.faces[1].weight_bold);  /* bolder             */
  TEST_ASSERT_EQ(1, s_sheet.faces[2].style_italic); /* oblique            */
  TEST_ASSERT_EQ(0, s_sheet.faces[3].weight_bold);  /* normal -> not bold */
  /* The bold faces are pickable as bold; the normal face is the regular weight. */
  TEST_ASSERT_EQ(0, ra8_css_match_face(&s_sheet, "F", 1U, true, false));
  TEST_ASSERT_EQ(2, ra8_css_match_face(&s_sheet, "H", 1U, false, true));
  TEST_ASSERT_EQ(3, ra8_css_match_face(&s_sheet, "N", 1U, false, false));
  TEST_END("css @font-face weight/style keyword ORs");
}

/**
 * @test internal_test_fontface_accept_guard
 * @brief A @font-face is kept only when BOTH family and src are present.
 *
 * @par MC/DC:
 * Decision (internal_parse_fontface): `(family_len != 0) && (src_len != 0)`
 * (2-cond AND; N+1 = 3):
 *  - family + src present     -> both true  -> face kept (control).
 *  - family present, no src   -> cond2 false -> face dropped.
 *  - src present, no family    -> cond1 false -> face dropped.
 * Only the complete declaration survives in the face table.
 * @details Exercises the fontface accept guard path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_fontface_accept_guard(void)
{
  TEST_BEGIN("css @font-face accept guard (family && src)");
  load("@font-face{font-family:Keep;src:url(k.ttf)}" /* both -> kept      */
       "@font-face{font-family:NoSrc}"               /* no src -> drop    */
       "@font-face{src:url(nofam.ttf)}");            /* no family -> drop */
  TEST_ASSERT_EQ(1, s_sheet.face_count);
  TEST_ASSERT_EQ(0, ra8_css_match_face(&s_sheet, "Keep", 4U, false, false));
  TEST_ASSERT_EQ(k_ra8_css_no_face, ra8_css_match_face(&s_sheet, "NoSrc", 5U, false, false));
  TEST_END("css @font-face accept guard (family && src)");
}

/**
 * @test internal_test_ancestor_constraint_arms
 * @brief Descendant ancestor parts match by tag, by class, and by id.
 *
 * @par MC/DC:
 * internal_anc_matches has three constraint arms, each a 2-condition decision:
 *   tag  : `(anc->tag != unknown) && (el->tag != anc->tag)`
 *   class: `(anc->class_len != 0) && (class-list lacks the name)`
 *   id   : `(anc->id_len != 0) && (id mismatch)`
 * Three rules, each constraining a different ancestor facet, are matched against
 * an ancestor that satisfies it (true outcome) and one that does not (false).
 * `blockquote` is used as the block ancestor tag (the tokenizer classifies it;
 * `div` is not a recognised tag and would drop the rule):
 *  - `blockquote p` : blockquote ancestor -> tag arm passes; h1 -> tag fails.
 *  - `.box p`       : ancestor .box -> class arm passes; ancestor .other fails.
 *  - `#main p`      : ancestor #main -> id arm passes; ancestor #side -> fails.
 * @details Exercises the ancestor constraint arms path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_ancestor_constraint_arms(void)
{
  TEST_BEGIN("css descendant ancestor tag/class/id arms");
  const ra8_css_style_t   inh  = {};
  const ra8_css_element_t p    = elem(k_reflow_tag_p, nullptr, nullptr);
  const uint8_t           bset = (uint8_t)k_ra8_css_set_bold;

  /* Tag-ancestor `blockquote p` -- blockquote matches (bold), h1 does not. */
  load("blockquote p { font-weight: bold; }");
  const ra8_css_element_t a_bq[1] = {elem(k_reflow_tag_blockquote, nullptr, nullptr)};
  const ra8_css_element_t a_h1[1] = {elem(k_reflow_tag_h1, nullptr, nullptr)};
  ra8_css_style_t         t_ok    = ra8_css_cascade_ctx(&s_sheet, &p, inh, no_inline(), a_bq, 1U);
  ra8_css_style_t         t_no    = ra8_css_cascade_ctx(&s_sheet, &p, inh, no_inline(), a_h1, 1U);
  TEST_ASSERT((t_ok.set & bset) != 0U);
  TEST_ASSERT((t_no.set & bset) == 0U);

  /* Class-ancestor `.box p` -- .box matches, .other does not. */
  load(".box p { font-weight: bold; }");
  const ra8_css_element_t a_box[1]   = {elem(k_reflow_tag_blockquote, nullptr, "box")};
  const ra8_css_element_t a_other[1] = {elem(k_reflow_tag_blockquote, nullptr, "other")};
  ra8_css_style_t         c_ok = ra8_css_cascade_ctx(&s_sheet, &p, inh, no_inline(), a_box, 1U);
  ra8_css_style_t         c_no = ra8_css_cascade_ctx(&s_sheet, &p, inh, no_inline(), a_other, 1U);
  TEST_ASSERT((c_ok.set & bset) != 0U);
  TEST_ASSERT((c_no.set & bset) == 0U);

  /* Id-ancestor `blockquote#main p` -- #main matches, #side does not. */
  load("blockquote#main p { font-weight: bold; }");
  const ra8_css_element_t a_main[1] = {elem(k_reflow_tag_blockquote, "main", nullptr)};
  const ra8_css_element_t a_side[1] = {elem(k_reflow_tag_blockquote, "side", nullptr)};
  ra8_css_style_t         i_ok = ra8_css_cascade_ctx(&s_sheet, &p, inh, no_inline(), a_main, 1U);
  ra8_css_style_t         i_no = ra8_css_cascade_ctx(&s_sheet, &p, inh, no_inline(), a_side, 1U);
  TEST_ASSERT((i_ok.set & bset) != 0U);
  TEST_ASSERT((i_no.set & bset) == 0U);
  TEST_END("css descendant ancestor tag/class/id arms");
}

/**
 * @test internal_test_resolve_order_tiebreak
 * @brief Two equal-specificity rules: the later one wins the order tie-break.
 *
 * @par MC/DC:
 * Decision (internal_resolve winner): `(!have) || (rank > best_rank) ||
 *   ((rank == best_rank) && (order >= best_order))` -- this targets the third
 * disjunct's inner `(rank == best_rank) && (order >= best_order)`:
 *  - first `p { color: red }` -> !have true (seeds best with red, order 0).
 *  - second `p { color: blue }` -> have set, rank == best_rank (both type-only),
 *    order(1) >= best_order(0) -> inner AND true -> blue overrides red.
 * The result is blue: the later same-specificity rule wins, exercising the
 * `rank == best_rank` true arm and the `order >= best_order` true arm together.
 * @details Exercises the resolve order tiebreak path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_resolve_order_tiebreak(void)
{
  TEST_BEGIN("css resolve order tie-break (rank==best && order>=best)");
  load("p { color: red; } p { color: blue; }");
  ra8_css_element_t p = elem(k_reflow_tag_p, nullptr, nullptr);
  ra8_css_style_t   r = ra8_css_cascade(&s_sheet, &p, no_inline(), no_inline());
  TEST_ASSERT((r.set & (uint8_t)k_ra8_css_set_color) != 0U);
  TEST_ASSERT_EQ(k_css_color_blue, r.color); /* later same-spec wins */
  TEST_END("css resolve order tie-break (rank==best && order>=best)");
}

/**
 * @test internal_test_match_face_regular_fallback
 * @brief A bold-only family with no regular face yields no regular fallback.
 *
 * @par MC/DC:
 * Decision (ra8_css_match_face fallback record):
 *   `(weight_bold == 0) && (style_italic == 0) && (fallback < 0)` (3-cond AND),
 * driven against two families:
 *  (A) "Reg" has a single non-bold non-italic face:
 *      - asking (bold, italic) finds no exact match, but the regular face sets
 *        the fallback (all three true: weight 0, style 0, fallback still <0) ->
 *        returns the regular face index.
 *  (B) "BoldOnly" has a single bold face:
 *      - asking (italic-only) finds no exact match AND the face is bold so
 *        `weight_bold == 0` is false (cond1) -> no fallback recorded -> no-face.
 *      This isolates cond1 against (A)'s all-true control.
 * @details Exercises the match face regular fallback path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_match_face_regular_fallback(void)
{
  TEST_BEGIN("css match_face regular fallback (3-AND record)");
  load("@font-face{font-family:Reg;src:url(r.ttf)}"
       "@font-face{font-family:BoldOnly;font-weight:bold;src:url(b.ttf)}");
  /* (A): bold+italic request on a regular-only family -> regular fallback (0). */
  TEST_ASSERT_EQ(0, ra8_css_match_face(&s_sheet, "Reg", 3U, true, true));
  /* (B): italic request on a bold-only family -> no regular face -> no-face. */
  TEST_ASSERT_EQ(k_ra8_css_no_face, ra8_css_match_face(&s_sheet, "BoldOnly", 8U, false, true));
  TEST_END("css match_face regular fallback (3-AND record)");
}

/**
 * @test internal_test_rule_matches_null_arms
 * @brief ra8_css_rule_matches rejects each NULL pointer arm independently.
 *
 * @par MC/DC:
 * Decision: `(rule == NULL) || (el == NULL) || (sheet == NULL)` (3-cond OR;
 * N+1 = 4):
 *  - all non-NULL              -> false (control: a real match is computed).
 *  - rule == NULL              -> cond1 true  -> false return.
 *  - el == NULL                -> cond2 true  -> false return.
 *  - sheet == NULL             -> cond3 true  -> false return.
 * @details Exercises the rule matches null arms path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_rule_matches_null_arms(void)
{
  TEST_BEGIN("css rule_matches null guard 3-OR");
  load("p { color: red; }");
  const ra8_css_rule_t* r  = &s_sheet.rules[0];
  ra8_css_element_t     el = elem(k_reflow_tag_p, nullptr, nullptr);
  TEST_ASSERT(ra8_css_rule_matches(r, &el, &s_sheet));        /* control true */
  TEST_ASSERT(!ra8_css_rule_matches(nullptr, &el, &s_sheet)); /* cond1        */
  TEST_ASSERT(!ra8_css_rule_matches(r, nullptr, &s_sheet));   /* cond2        */
  TEST_ASSERT(!ra8_css_rule_matches(r, &el, nullptr));        /* cond3        */
  TEST_END("css rule_matches null guard 3-OR");
}

/**
 * @test internal_test_sel_part_empty_name
 * @brief A `.`/`#` with no following name (nlen == 0) drops the rule.
 *
 * @par MC/DC:
 * Decision (internal_parse_sel_part): `(nlen == 0U) || !internal_intern_name(...)`
 * (2-cond OR). The existing suite drives the `!intern` arm (over-long / dup);
 * this isolates the `nlen == 0` arm:
 *  - control `p { ... }` -> a bare type, no `.`/`#` part -> kept.
 *  - `em# { ... }` -> after the type `em`, the `#` part has no name byte before
 *    the selector ends -> nlen == 0 (cond1 true) -> internal_parse_sel_part returns
 *    false -> the rule is dropped.
 * Only the control survives in the rule table.
 * @details Exercises the sel part empty name path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_sel_part_empty_name(void)
{
  TEST_BEGIN("css selector empty .#-part (nlen == 0) drop");
  load("p { color: red; }"      /* control kept             */
       "em# { color: blue; }"); /* '#' with no name -> drop */
  TEST_ASSERT_EQ(k_css_rule_one, s_sheet.rule_count);
  TEST_ASSERT_EQ(k_reflow_tag_p, s_sheet.rules[0].sel_tag);
  TEST_ASSERT_EQ(k_css_color_red, s_sheet.rules[0].decl.color);
  TEST_END("css selector empty .#-part (nlen == 0) drop");
}

/**
 * @test internal_test_match_face_length_mismatch
 * @brief A family query whose length differs from a face is rejected.
 *
 * @par MC/DC:
 * Decision (internal_ci_eq_span via internal_family_eq): `(a == NULL) || (b == NULL) ||
 *   (alen != blen)` (3-cond OR). The pointer arms are unreachable through
 * ra8_css_match_face (it pre-checks family != NULL and the pooled name is never
 * NULL), so this isolates the reachable `alen != blen` condition:
 *  - querying "Body" (len 4) against the "Body" face -> alen == blen -> the span
 *    compare runs and the face matches (cond3 false: control).
 *  - querying "Bod" (len 3) against the same face -> alen(4) != blen(3) -> cond3
 *    true -> no family match -> no-face.
 * @details Exercises the match face length mismatch path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_match_face_length_mismatch(void)
{
  TEST_BEGIN("css match_face family length mismatch (alen != blen)");
  load("@font-face{font-family:Body;src:url(b.ttf)}");
  /* Exact length -> the family compare runs and matches. */
  TEST_ASSERT_EQ(0, ra8_css_match_face(&s_sheet, "Body", 4U, false, false));
  /* Shorter query -> alen != blen short-circuits the compare -> no match. */
  TEST_ASSERT_EQ(k_ra8_css_no_face, ra8_css_match_face(&s_sheet, "Bod", 3U, false, false));
  TEST_END("css match_face family length mismatch (alen != blen)");
}

/**
 * @test internal_test_fontface_bold_kw_full_or
 * @brief Each remaining @font-face bold keyword isolates one OR condition.
 *
 * @par MC/DC:
 * Decision identity:
 * `apps/shared_libs/reflow/src/reflow_css_rules.c@internal_is_bold_kw`.
 * Decision (internal_is_bold_kw): `bold || bolder || 600 || 700 || 800 || 900`
 * (6-cond OR). The existing suite drives cond6 (900), cond2 (bolder) and the
 * all-false arm (normal). This isolates the four still-uncovered conditions, one
 * keyword each (the rest false), and a `font-style:italic` face also isolates
 * internal_is_italic_kw cond1 (italic) against the existing oblique cond2:
 *  - `font-weight:bold`   -> weight_bold = 1 (bold OR cond1).
 *  - `font-weight:600`    -> weight_bold = 1 (bold OR cond3).
 *  - `font-weight:700`    -> weight_bold = 1 (bold OR cond4).
 *  - `font-weight:800`    -> weight_bold = 1 (bold OR cond5).
 *  - `font-style:italic`  -> style_italic = 1 (italic OR cond1).
 * @details Exercises the fontface bold kw full or path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_fontface_bold_kw_full_or(void)
{
  TEST_BEGIN("css @font-face bold-kw OR conds 1/3/4/5 + italic cond1");
  load("@font-face{font-family:A;font-weight:bold;src:url(a.ttf)}"
       "@font-face{font-family:B;font-weight:600;src:url(b.ttf)}"
       "@font-face{font-family:C;font-weight:700;src:url(c.ttf)}"
       "@font-face{font-family:D;font-weight:800;src:url(d.ttf)}"
       "@font-face{font-family:E;font-style:italic;src:url(e.ttf)}");
  const uint16_t face_five = 5U;
  TEST_ASSERT_EQ(face_five, s_sheet.face_count);
  TEST_ASSERT_EQ(1, s_sheet.faces[0].weight_bold);  /* bold   */
  TEST_ASSERT_EQ(1, s_sheet.faces[1].weight_bold);  /* 600    */
  TEST_ASSERT_EQ(1, s_sheet.faces[2].weight_bold);  /* 700    */
  TEST_ASSERT_EQ(1, s_sheet.faces[3].weight_bold);  /* 800    */
  TEST_ASSERT_EQ(1, s_sheet.faces[4].style_italic); /* italic */
  TEST_END("css @font-face bold-kw OR conds 1/3/4/5 + italic cond1");
}

/**
 * @test internal_test_face_apply_reject_arms
 * @brief An empty-quoted family and a url-less src each leave their slot unset.
 *
 * @par MC/DC:
 * Two guards in internal_face_apply:
 *  (A) family: `(n > 0U) && internal_intern_name(...)` -- a `font-family:""` value
 *      strips to length 0, so `n > 0` is false (cond1 false): no family interned.
 *  (B) src: `internal_extract_url(...) && internal_intern_name(...)` -- a `src:none`
 *      value has no `url(`, so internal_extract_url returns false (cond1 false):
 *      no src interned.
 * With neither family nor src set, internal_parse_fontface's accept guard rejects
 * the block, so the face table stays empty (the surviving control face proves
 * the table itself works).
 * @details Exercises the face apply reject arms path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_face_apply_reject_arms(void)
{
  TEST_BEGIN("css face_apply empty-family / no-url-src rejects");
  load("@font-face{font-family:Good;src:url(g.ttf)}"  /* control kept         */
       "@font-face{font-family:\"\";src:none}"        /* empty fam + no url() */
       "@font-face{font-family:NoUrl;src:none}");     /* fam ok, src no url() */
  TEST_ASSERT_EQ(k_css_face_one, s_sheet.face_count); /* only control         */
  TEST_ASSERT_EQ(0, ra8_css_match_face(&s_sheet, "Good", 4U, false, false));
  TEST_ASSERT_EQ(k_ra8_css_no_face, ra8_css_match_face(&s_sheet, "NoUrl", 5U, false, false));
  TEST_END("css face_apply empty-family / no-url-src rejects");
}

/**
 * @test internal_test_rule_family_empty
 * @brief A rule `font-family:""` interns nothing (the n == 0 guard arm).
 *
 * @par MC/DC:
 * Decision (internal_family_cb): `(n > 0U) && internal_intern_name(...)` (2-cond AND):
 *  - `p { font-family: Serif }` -> n>0 AND intern OK -> true (family set bit on).
 *  - `h1 { font-family: "" }`   -> the empty value strips to length 0, so
 *    `n > 0` is false (cond1 false): the family set bit stays clear.
 * @details Exercises the rule family empty path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_rule_family_empty(void)
{
  TEST_BEGIN("css rule font-family empty (n == 0) guard");
  const uint8_t famset = (uint8_t)k_ra8_css_set_family;
  load("p { font-family: Serif; }"   /* family interned           */
       "h1 { font-family: \"\"; }"); /* empty -> nothing interned */
  ra8_css_element_t pe = elem(k_reflow_tag_p, nullptr, nullptr);
  ra8_css_element_t he = elem(k_reflow_tag_h1, nullptr, nullptr);
  ra8_css_style_t   ps = ra8_css_cascade(&s_sheet, &pe, no_inline(), no_inline());
  ra8_css_style_t   hs = ra8_css_cascade(&s_sheet, &he, no_inline(), no_inline());
  TEST_ASSERT((ps.set & famset) != 0U); /* control set the family */
  TEST_ASSERT((hs.set & famset) == 0U); /* empty family not set   */
  TEST_END("css rule font-family empty (n == 0) guard");
}

/**
 * @test internal_test_class_list_multi_token
 * @brief A space-separated multi-class ancestor list is scanned token by token.
 *
 * @par MC/DC:
 * Loop (internal_class_list_has): the inner whitespace skip
 * `while ((i < list_len) && priv_is_ws(list[i]))` plus the token scan, reached
 * via a descendant `.box p` rule whose ancestor carries several classes:
 *  - ancestor class " lead box " -> the leading space exercises the ws-skip
 *    (is_ws true), the "lead" token mismatches, the inter-token space skips, and
 *    "box" matches; the trailing space drives the `i < list_len`-false exit.
 *  - the same rule against an ancestor class "lead only" (no "box") -> the scan
 *    walks both tokens and returns no match (the rule does not apply).
 * @details Exercises the class list multi token path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_class_list_multi_token(void)
{
  TEST_BEGIN("css class-list multi-token whitespace scan");
  const uint8_t           bset = (uint8_t)k_ra8_css_set_bold;
  const ra8_css_style_t   inh  = {};
  const ra8_css_element_t p    = elem(k_reflow_tag_p, nullptr, nullptr);
  load(".box p { font-weight: bold; }");
  /* Leading + inter-token + trailing whitespace around the matching "box". */
  const ra8_css_element_t a_has[1] = {elem(k_reflow_tag_blockquote, nullptr, " lead box ")};
  const ra8_css_element_t a_no[1]  = {elem(k_reflow_tag_blockquote, nullptr, "lead only")};
  ra8_css_style_t         hit      = ra8_css_cascade_ctx(&s_sheet, &p, inh, no_inline(), a_has, 1U);
  ra8_css_style_t         miss     = ra8_css_cascade_ctx(&s_sheet, &p, inh, no_inline(), a_no, 1U);
  TEST_ASSERT((hit.set & bset) != 0U);  /* "box" found among the tokens */
  TEST_ASSERT((miss.set & bset) == 0U); /* no "box" token -> no match   */
  TEST_END("css class-list multi-token whitespace scan");
}

/**
 * @test internal_test_anc_null_class_id_and_idlen
 * @brief Ancestor class/id constraints reject NULL attrs and id-length mismatch.
 *
 * @par MC/DC:
 * Decision owner:
 * apps/shared_libs/reflow/src/reflow_css_cascade.c@internal_anc_matches
 * internal_anc_matches has two remaining compound arms:
 *   class: `(el->class_str == NULL) || !class_list_has(...)`.
 *   id initialization: `(el->id != NULL) && (el->id_len == anc->id_len)`.
 *   id byte loop: `(equal && (i < anc->id_len))`.
 * This test isolates the NULL and length arms; companion
 * internal_test_ancestor_constraint_arms supplies exact `main` and same-length
 * mismatch `side` vectors:
 *  - `.box p` vs an ancestor with NO class (class_str == NULL) -> class cond1
 *    true -> no match.
 *  - `blockquote#main p` vs an ancestor with NO id (id == NULL) -> id cond1 true
 *    -> no match.
 *  - `blockquote#main p` vs an ancestor id "mains" (len 5 != 4) -> id cond2
 *    false -> no match.
 *  - id "main" -> initialization `(T,T)`, loop `(T,T)` while bytes match, and
 *    exact-bound exit `(T,F)` -> match.
 *  - id "side" -> initialization `(T,T)` then a byte mismatch makes the next
 *    loop guard `(F,T)` -> no match.
 * These are the N+1 independence vectors for both new id decisions.
 * @details Exercises the anc null class id and idlen path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_anc_null_class_id_and_idlen(void)
{
  TEST_BEGIN("css anc NULL-class / NULL-id / id-len-mismatch arms");
  const uint8_t           bset = (uint8_t)k_ra8_css_set_bold;
  const ra8_css_style_t   inh  = {};
  const ra8_css_element_t p    = elem(k_reflow_tag_p, nullptr, nullptr);

  /* Class constraint vs an ancestor with NO class attribute. */
  load(".box p { font-weight: bold; }");
  const ra8_css_element_t a_noc[1] = {elem(k_reflow_tag_blockquote, nullptr, nullptr)};
  ra8_css_style_t         noc      = ra8_css_cascade_ctx(&s_sheet, &p, inh, no_inline(), a_noc, 1U);
  TEST_ASSERT((noc.set & bset) == 0U); /* class_str == NULL -> no match */

  /* Id constraint vs an ancestor with NO id, and vs a wrong-length id. */
  load("blockquote#main p { font-weight: bold; }");
  const ra8_css_element_t a_noid[1] = {elem(k_reflow_tag_blockquote, nullptr, nullptr)};
  const ra8_css_element_t a_len[1]  = {elem(k_reflow_tag_blockquote, "mains", nullptr)};
  ra8_css_style_t         noid = ra8_css_cascade_ctx(&s_sheet, &p, inh, no_inline(), a_noid, 1U);
  ra8_css_style_t         len  = ra8_css_cascade_ctx(&s_sheet, &p, inh, no_inline(), a_len, 1U);
  TEST_ASSERT((noid.set & bset) == 0U); /* id == NULL -> no match    */
  TEST_ASSERT((len.set & bset) == 0U);  /* id_len 5 != 4 -> no match */
  TEST_END("css anc NULL-class / NULL-id / id-len-mismatch arms");
}

/**
 * @test internal_test_resolve_specificity_override
 * @brief A higher-specificity rule overrides a lower one (rank > best_rank).
 *
 * @par MC/DC:
 * Decision (internal_resolve winner): `(!have) || (rank > best_rank) ||
 *   ((rank == best_rank) && (order >= best_order))`. The existing suite drives
 * the `!have` seed and the equal-rank order tie. This isolates the
 * `rank > best_rank` disjunct:
 *  - `p { color: red }` (rank = type only) matches and seeds best.
 *  - `p.note { color: blue }` (rank = type + class) also matches; its rank is
 *    strictly greater, so `rank > best_rank` true -> blue overrides red even
 *    though it is interned at a higher source order.
 * The result is blue, proving specificity (not source order) chose the winner.
 * @details Exercises the resolve specificity override path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_resolve_specificity_override(void)
{
  TEST_BEGIN("css resolve specificity override (rank > best_rank)");
  load("p { color: red; } p.note { color: blue; }");
  ra8_css_element_t e = elem(k_reflow_tag_p, nullptr, "note");
  ra8_css_style_t   r = ra8_css_cascade(&s_sheet, &e, no_inline(), no_inline());
  TEST_ASSERT((r.set & (uint8_t)k_ra8_css_set_color) != 0U);
  TEST_ASSERT_EQ(k_css_color_blue, r.color); /* higher specificity wins */
  TEST_END("css resolve specificity override (rank > best_rank)");
}

/**
 * @test internal_test_match_face_fallback_conditions
 * @brief Isolate the italic-face (cond2) and second-regular (cond3) arms.
 *
 * @par MC/DC:
 * Decision (ra8_css_match_face fallback record):
 *   `(weight_bold == 0) && (style_italic == 0) && (fallback < 0)` (3-cond AND).
 * The existing suite drives the all-true record and the `weight_bold == 0`-false
 * (bold-only) arm. This isolates the other two:
 *  (A) `style_italic == 0` false -- a family with a single italic-only face:
 *      asking (bold, not-italic) finds no exact match, and the face's
 *      style_italic = 1 makes cond2 false -> no fallback recorded -> no-face.
 *  (B) `fallback < 0` false -- a family with TWO regular faces: the first sets
 *      the fallback, the second sees `fallback < 0` false (cond3 false) and is
 *      skipped; asking (bold) returns the FIRST regular face.
 * @details Exercises the match face fallback conditions path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_match_face_fallback_conditions(void)
{
  TEST_BEGIN("css match_face fallback conds 2 (italic) / 3 (2nd regular)");
  /* (A) italic-only family: cond2 (style_italic == 0) false -> no fallback. */
  load("@font-face{font-family:Ital;font-style:italic;src:url(i.ttf)}");
  TEST_ASSERT_EQ(k_ra8_css_no_face, ra8_css_match_face(&s_sheet, "Ital", 4U, true, false));
  /* (B) two regular faces: the second hits cond3 (fallback < 0) false. */
  load("@font-face{font-family:Two;src:url(r1.ttf)}"
       "@font-face{font-family:Two;src:url(r2.ttf)}");
  TEST_ASSERT_EQ(k_css_face_two, s_sheet.face_count);
  TEST_ASSERT_EQ(0, ra8_css_match_face(&s_sheet, "Two", 3U, true, false));
  TEST_END("css match_face fallback conds 2 (italic) / 3 (2nd regular)");
}

/**
 * @test internal_test_face_src_guard_arms
 * @brief ra8_css_face_src validates each guard condition independently.
 *
 * @par MC/DC:
 * Decision: `(sheet == NULL) || (out_src == NULL) || (out_len == NULL) ||
 *   (idx >= face_count)` (4-cond OR; N+1 = 5):
 *  - all valid               -> false (control: the href is returned).
 *  - sheet == NULL           -> cond1 true  -> false.
 *  - out_src == NULL         -> cond2 true  -> false.
 *  - out_len == NULL         -> cond3 true  -> false.
 *  - idx >= face_count       -> cond4 true  -> false.
 * @details Exercises the face src guard arms path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_face_src_guard_arms(void)
{
  TEST_BEGIN("css face_src guard 4-OR");
  load("@font-face{font-family:Body;src:url(body.ttf)}");
  const char*    src     = nullptr;
  uint16_t       slen    = 0U;
  const uint16_t bad_idx = 99U;
  TEST_ASSERT(ra8_css_face_src(&s_sheet, 0U, &src, &slen)); /* control true */
  TEST_ASSERT(slen > 0U);
  TEST_ASSERT(!ra8_css_face_src(nullptr, 0U, &src, &slen));       /* cond1 */
  TEST_ASSERT(!ra8_css_face_src(&s_sheet, 0U, nullptr, &slen));   /* cond2 */
  TEST_ASSERT(!ra8_css_face_src(&s_sheet, 0U, &src, nullptr));    /* cond3 */
  TEST_ASSERT(!ra8_css_face_src(&s_sheet, bad_idx, &src, &slen)); /* cond4 */
  TEST_END("css face_src guard 4-OR");
}

/**
 * @test internal_test_fontface_style_normal_mcdc
 *
 * @par MC/DC:
 * Decision: `internal_is_italic_kw()` =
 *   `priv_reflow_css_ci_eq(val,vlen,"italic") || priv_reflow_css_ci_eq(val,vlen,"oblique")`
 * (2 conditions, OR; apps/shared_libs/reflow/src/reflow_css_rules.c, @font-face
 * font-style). Existing vectors cover the italic (C1 true) and oblique (C1 false,
 * C2 true) arms. N+1 completion:
 *  - "font-style: normal" -> C1 false AND C2 false -> the face is committed with
 *    style_italic == 0. This both-false vector provides the independence pairs for
 *    each condition against the italic / oblique true vectors.
 * @brief Verify fontface style normal mcdc behavior against the reflow contract.
 * @details Exercises the fontface style normal mcdc path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_fontface_style_normal_mcdc(void)
{
  TEST_BEGIN("css @font-face MC/DC: font-style normal (is_italic_kw both-false)");
  load("@font-face{font-family:Reg;src:url(r.ttf);font-style:normal}");
  TEST_ASSERT_EQ(1, s_sheet.face_count);
  /* A normal-style face still matches a non-italic query. */
  TEST_ASSERT_EQ(0, ra8_css_match_face(&s_sheet, "Reg", 3U, false, false));
  TEST_END("css @font-face MC/DC: font-style normal (is_italic_kw both-false)");
}

/**
 * @test internal_test_class_list_trailing_ws_and_samelen_mcdc
 *
 * @par MC/DC:
 * Decision owner:
 * apps/shared_libs/reflow/src/reflow_css_cascade.c@internal_class_list_has
 * Decisions in internal_class_list_has:
 *   - the inner whitespace skip `while ((i<list_len) && priv_reflow_css_is_ws(list[i]))`.
 *   - the byte loop `for (...; equal && (j < nlen); ...)`.
 * A NON-matching class list drives the false arms that matching lists do not:
 *  - ancestor class "lead only " (trailing space, no "box") -> after the last token
 *    the skip loop consumes the trailing space until i == list_len, taking the
 *    `(i < list_len)` false side (C1 pair for the ws skip).
 *  - ancestor class "cat" (length 3 == "box", different bytes) initializes
 *    `equal` true, enters `(T,T)`, then the mismatch yields `(F,T)`.
 * Companion internal_test_class_list_multi_token supplies exact token "box",
 * whose loop stays `(T,T)` and exits at the exact bound with `(T,F)`; its
 * different-length "lead" token supplies `(F,T)` at entry. Together these are
 * the byte-loop's N+1 independence vectors.
 * Both leave the `.box p` rule unmatched (bold bit clear), the observable proof.
 * @brief Verify class list trailing ws and samelen mcdc behavior against the reflow contract.
 * @details Exercises the class list trailing ws and samelen mcdc path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_class_list_trailing_ws_and_samelen_mcdc(void)
{
  TEST_BEGIN("css class-list MC/DC: trailing-ws end + same-length mismatch");
  const uint8_t           bset = (uint8_t)k_ra8_css_set_bold;
  const ra8_css_style_t   inh  = {};
  const ra8_css_element_t p    = elem(k_reflow_tag_p, nullptr, nullptr);
  load(".box p { font-weight: bold; }");
  /* Non-matching list with TRAILING whitespace -> ws-skip reaches list_len. */
  const ra8_css_element_t a_trail[1] = {elem(k_reflow_tag_blockquote, nullptr, "lead only ")};
  /* Non-matching token the SAME length as "box" (3) -> tlen==nlen, memcmp!=0. */
  const ra8_css_element_t a_samelen[1] = {elem(k_reflow_tag_blockquote, nullptr, "cat")};
  const ra8_css_style_t   miss_trail =
    ra8_css_cascade_ctx(&s_sheet, &p, inh, no_inline(), a_trail, 1U);
  const ra8_css_style_t miss_samelen =
    ra8_css_cascade_ctx(&s_sheet, &p, inh, no_inline(), a_samelen, 1U);
  TEST_ASSERT((miss_trail.set & bset) == 0U);   /* "box" absent -> rule unmatched   */
  TEST_ASSERT((miss_samelen.set & bset) == 0U); /* "cat" != "box" -> rule unmatched */
  TEST_END("css class-list MC/DC: trailing-ws end + same-length mismatch");
}

/**
 * @brief Test executable entry point -- runs the selector/face MC/DC vectors.
 *
 * @details Walks a function-local, fixed-order roster of every selector case
 * so adding a case remains a one-line edit. Cases run from top to bottom.
 *
 * @return 0 on success (all tests passed).
 *
 * @pre Host environment provides stderr.
 * @post Every match-side decision family above has executed its vectors.
 *
 * @note Not thread-safe (single-threaded test runner).
 * @since 0.1.0
 */
int main(void)
{
  static void (*const s_test_roster[])(void) = {
    internal_test_sel_part_combinator_drop,
    internal_test_fontface_quote_strip,
    internal_test_fontface_weight_style_keywords,
    internal_test_fontface_accept_guard,
    internal_test_ancestor_constraint_arms,
    internal_test_resolve_order_tiebreak,
    internal_test_match_face_regular_fallback,
    internal_test_rule_matches_null_arms,
    internal_test_sel_part_empty_name,
    internal_test_match_face_length_mismatch,
    internal_test_fontface_bold_kw_full_or,
    internal_test_face_apply_reject_arms,
    internal_test_rule_family_empty,
    internal_test_class_list_multi_token,
    internal_test_anc_null_class_id_and_idlen,
    internal_test_resolve_specificity_override,
    internal_test_match_face_fallback_conditions,
    internal_test_face_src_guard_arms,
    internal_test_fontface_style_normal_mcdc,
    internal_test_class_list_trailing_ws_and_samelen_mcdc,
  };
  const size_t roster_len = sizeof s_test_roster / sizeof s_test_roster[0];
  for (size_t i = 0U; i < roster_len; ++i) {
    s_test_roster[i]();
  }
  return 0;
}
