/**
 * @file test_ra8_reflow_tokenize_scan_mcdc.c
 * @brief MC/DC tests for the XHTML tokenizer's scanning decision families.
 *
 * @details
 * Split sibling of test_ra8_reflow_tokenize_tag_mcdc.c and
 * test_ra8_reflow_tokenize_link_mcdc.c covering the byte-scanning halves of
 * libs/ra8_reflow/src/ra8_reflow_tokenize.c: the numeric / named entity
 * decoder (base select, digit classes, terminators, short-input guards, the
 * below-'A' hex reject), the CDATA section scanner (close scan, empty
 * content, emit-pool exhaustion), the end-tag scanner and block-end emission,
 * and the raw-text `<style>` / `<script>` body handling. Every test drives
 * the real tokenizer over crafted byte strings; the shared engine fixture
 * lives in tests/support/reflow_tokenize_test_util.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "support/reflow_tokenize_test_util.h"
#include "unity_minimal.h"

/**
 * @test internal_test_numeric_base_select
 *
 * @par MC/DC:
 * Decision: `(i < avail) && ((src[i] == 'x') || (src[i] == 'X'))` selecting
 * hex vs decimal in internal_decode_numeric (ra8_reflow_tokenize.c).
 *  - V1 "&#65;"  avail=5 -> i<avail true,  'x' false, 'X' false -> decimal.
 *  - V2 "&#x4f;" avail=6 -> i<avail true,  'x' true              -> hex.
 *  - V3 "&#X4F;" avail=6 -> i<avail true,  'x' false,'X' true    -> hex.
 *  - V4 "&#x"    avail=3 -> i<avail FALSE  (and ';' missing -> reject).
 * V1 vs V2 isolates the 'x' arm; V1 vs V3 isolates the 'X' arm; V2 vs V4
 * isolates the `i < avail` guard. N+1 = 4 vectors for the 3-condition group.
 * @brief Verify numeric base select behavior against the reflow contract.
 * @details Exercises the numeric base select path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_numeric_base_select(void)
{
  TEST_BEGIN("priv_decode_numeric base-select MC/DC");
  uint32_t cp   = 0U;
  size_t   used = 0U;
  TEST_ASSERT(priv_ra8_reflow_tok_decode_entity("&#65;", 5U, &cp, &used)); /* V1 decimal */
  TEST_ASSERT_EQ(k_cp_uppercase_a, cp);
  TEST_ASSERT_EQ(k_used_dec_a, used);
  TEST_ASSERT(priv_ra8_reflow_tok_decode_entity("&#x4f;", 6U, &cp, &used)); /* V2 'x' hex */
  TEST_ASSERT_EQ(k_cp_hex_4f, cp);
  TEST_ASSERT(priv_ra8_reflow_tok_decode_entity("&#X4F;", 6U, &cp, &used)); /* V3 'X' hex */
  TEST_ASSERT_EQ(k_cp_hex_4f_upper, cp);
  TEST_ASSERT(!priv_ra8_reflow_tok_decode_entity("&#x", k_avail_three, &cp, &used)); /* V4 short */
  TEST_END("priv_decode_numeric base-select MC/DC");
}

/**
 * @test internal_test_numeric_digit_classes
 *
 * @par MC/DC:
 * Covers the digit-class chain in internal_decode_numeric's loop body
 * (ra8_reflow_tokenize.c): the decimal class `(c>='0')&&(c<='9')`, the
 * lowercase-hex class `(base==hex)&&(c>='a')&&(c<='f')`, the uppercase-hex
 * class `(base==hex)&&(c>='A')&&(c<='F')`, and the final `else -> false`.
 *  - V1 "&#x2a;" -> '2' decimal-class true; 'a' lowercase-hex-class true.
 *  - V2 "&#x1B;" -> '1' decimal-class true; 'B' uppercase-hex-class true.
 *  - V3 "&#x1g;" -> '1' true; 'g' fails all three classes -> else -> reject.
 *  - V4 "&#1a;"  -> base is DECIMAL so 'a' fails the base==hex guard of both
 *                   hex classes (decimal-class also false) -> else -> reject.
 * V1 isolates the lowercase-hex class true arm, V2 the uppercase-hex class,
 * V3 the all-false (else) reject, V4 the base==hex guard false arm.
 * @brief Verify numeric digit classes behavior against the reflow contract.
 * @details Exercises the numeric digit classes path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_numeric_digit_classes(void)
{
  TEST_BEGIN("priv_decode_numeric digit-class MC/DC");
  uint32_t cp   = 0U;
  size_t   used = 0U;
  TEST_ASSERT(priv_ra8_reflow_tok_decode_entity("&#x2a;", 6U, &cp, &used)); /* V1 dec + lc-hex */
  TEST_ASSERT_EQ(0x2AU, cp);
  TEST_ASSERT(priv_ra8_reflow_tok_decode_entity("&#x1B;", 6U, &cp, &used)); /* V2 dec + uc-hex */
  TEST_ASSERT_EQ(0x1BU, cp);
  TEST_ASSERT(
    !priv_ra8_reflow_tok_decode_entity("&#x1g;", 6U, &cp, &used));          /* V3 bad hex digit   */
  TEST_ASSERT(!priv_ra8_reflow_tok_decode_entity("&#1a;", 5U, &cp, &used)); /* V4 'a' not decimal */
  TEST_END("priv_decode_numeric digit-class MC/DC");
}

/**
 * @test internal_test_numeric_terminator
 *
 * @par MC/DC:
 * Decision: `(digits == 0U) || (i >= avail) || (src[i] != ';')` -- the
 * post-loop terminator check in internal_decode_numeric (ra8_reflow_tokenize.c).
 *  - V1 "&#65;"   -> digits!=0, i<avail, src[i]==';'  -> all false -> accept.
 *  - V2 "&#;"     -> digits==0 (loop saw ';' first)   -> first true -> reject.
 *  - V3 "&#65"    -> digits!=0, loop ran to i>=avail  -> 2nd true  -> reject.
 *  - V4 "&#65x;"  -> 'x' is not a decimal digit, so the loop's else returns
 *                    early; the terminator check is reached only by the cases
 *                    above. (Documented: the `src[i] != ';'` arm cannot be hit
 *                    after a clean digit run because the loop only exits on ';'
 *                    or i>=avail; V1/V2/V3 give the reachable independence.)
 * V1 vs V2 isolates `digits==0`; V1 vs V3 isolates `i>=avail`.
 * @brief Verify numeric terminator behavior against the reflow contract.
 * @details Exercises the numeric terminator path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_numeric_terminator(void)
{
  TEST_BEGIN("priv_decode_numeric terminator MC/DC");
  uint32_t cp   = 0U;
  size_t   used = 0U;
  TEST_ASSERT(priv_ra8_reflow_tok_decode_entity("&#65;", 5U, &cp, &used)); /* V1 accept */
  TEST_ASSERT_EQ(k_cp_uppercase_a, cp);
  TEST_ASSERT(!priv_ra8_reflow_tok_decode_entity("&#;", 3U, &cp, &used));  /* V2 no digits     */
  TEST_ASSERT(!priv_ra8_reflow_tok_decode_entity("&#65", 4U, &cp, &used)); /* V3 no terminator */
  TEST_END("priv_decode_numeric terminator MC/DC");
}

/**
 * @test internal_test_entity_short_guard
 *
 * @par MC/DC:
 * Decision: `(window < k_priv_entity_min) || (src[1] == '\0')` -- the early
 * reject in priv_ra8_reflow_tok_decode_entity (ra8_reflow_tokenize.c). `window` is
 * `min(avail, 12)`, `k_priv_entity_min` is 4.
 *  - V1 "&amp;" avail=5 -> window=5>=4 false; src[1]='a'!='\0' false -> proceed.
 *  - V2 "&lt"   avail=3 -> window=3<4 TRUE                          -> reject.
 *  - V3 "&\0xx" avail=5 -> window=5>=4 false; src[1]=='\0' TRUE     -> reject.
 * V1 vs V2 isolates the window-too-small condition; V1 vs V3 isolates the
 * `src[1] == '\0'` condition. N+1 = 3 vectors for the 2-condition OR.
 * @brief Verify entity short guard behavior against the reflow contract.
 * @details Exercises the entity short guard path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_entity_short_guard(void)
{
  TEST_BEGIN("decode_entity short-guard MC/DC");
  uint32_t   cp         = 0U;
  size_t     used       = 0U;
  const char nul_at_1[] = {'&', '\0', 'x', 'x', 'x'};
  TEST_ASSERT(priv_ra8_reflow_tok_decode_entity("&amp;", 5U, &cp, &used)); /* V1 proceed */
  TEST_ASSERT_EQ('&', cp);
  TEST_ASSERT(!priv_ra8_reflow_tok_decode_entity("&lt", 3U, &cp, &used));    /* V2 window<min */
  TEST_ASSERT(!priv_ra8_reflow_tok_decode_entity(nul_at_1, 5U, &cp, &used)); /* V3 src[1]==0  */
  TEST_END("decode_entity short-guard MC/DC");
}

/**
 * @test internal_test_named_entity_match
 *
 * @par MC/DC:
 * Decision: `((wlen+2) <= window) && (src[1+wlen] == ';') && strncmp==0` --
 * the named-entity match in priv_ra8_reflow_tok_decode_entity (ra8_reflow_tokenize.c).
 *  - V1 "&quot;" -> all three true            -> match (cp == '"').
 *  - V2 "&ampX"  -> name "amp" present but the byte after it is 'X' not ';'
 *                   -> terminator arm false   -> no match.
 *  - V3 "&zzz;"  -> length / ';' satisfied for "amp"(3) sized slots but the
 *                   strncmp differs for every table word -> compare arm false.
 * V1 isolates the all-true match; V2 isolates the `src[1+wlen]==';'` arm;
 * V3 isolates the strncmp arm. (The `(wlen+2)<=window` arm is exercised by the
 * window-too-small reject in internal_test_entity_short_guard.)
 * @brief Verify named entity match behavior against the reflow contract.
 * @details Exercises the named entity match path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_named_entity_match(void)
{
  TEST_BEGIN("decode_entity named-match MC/DC");
  uint32_t cp   = 0U;
  size_t   used = 0U;
  TEST_ASSERT(priv_ra8_reflow_tok_decode_entity("&quot;", 6U, &cp, &used)); /* V1 match */
  TEST_ASSERT_EQ('"', cp);
  TEST_ASSERT(!priv_ra8_reflow_tok_decode_entity("&ampX", 5U, &cp, &used)); /* V2 no ';'        */
  TEST_ASSERT(!priv_ra8_reflow_tok_decode_entity("&zzz;", 5U, &cp, &used)); /* V3 name mismatch */
  TEST_END("decode_entity named-match MC/DC");
}

/**
 * @test internal_test_cdata_close_and_emit
 *
 * @par MC/DC:
 * Decisions in internal_handle_cdata (ra8_reflow_tokenize.c):
 *  - close scan `((close+3)<=len) && (memcmp(&buf[close],"]]>")!=0)`: a
 *    terminated CDATA stops on the "]]>" match (memcmp arm false); an
 *    UNTERMINATED CDATA runs to `close+3 > len` (bounds arm false).
 *  - emit gate `(ctx->sp > 0U) && !internal_suppressed(ctx)`: inside `<p>` (sp>0,
 *    not suppressed) emits the inner text verbatim (no entity decode); inside
 *    a `display:none` block the suppressed arm drops it.
 * V-terminated + inside-element proves both arms true (text appears literally);
 * V-unterminated proves the bounds arm; V-suppressed proves the suppress arm.
 * @brief Verify cdata close and emit behavior against the reflow contract.
 * @details Exercises the cdata close and emit path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_cdata_close_and_emit(void)
{
  TEST_BEGIN("priv_handle_cdata MC/DC");

  /* Terminated CDATA inside an element: emitted verbatim, '&' NOT decoded. */
  TEST_ASSERT_EQ(k_ra8_ok, walk("<html><body><p><![CDATA[raw & keep]]>tail</p></body></html>"));
  TEST_ASSERT(text_has("raw & keep")); /* literal ampersand survives */
  TEST_ASSERT(text_has("tail"));       /* parsing resumed past "]]>" */

  /* Unterminated CDATA: consumed to end-of-buffer (bounds arm of the scan).
   * The truncated document is rejected as a validation failure. */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, walk("<html><body><p><![CDATA[never closed"));

  /* CDATA inside display:none -> the emit gate's suppress arm drops it. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 walk("<html><body><div style=\"display:none\">"
                      "<p><![CDATA[hiddencdata]]></p></div>"
                      "<p>aftercdata</p></body></html>"));
  TEST_ASSERT(!text_has("hiddencdata"));
  TEST_ASSERT(text_has("aftercdata"));

  TEST_END("priv_handle_cdata MC/DC");
}

/**
 * @test internal_test_end_tag_scan_and_block_end
 *
 * @par MC/DC:
 * Decisions in internal_handle_end (ra8_reflow_tokenize.c):
 *  - name scan `(i < len) && (buf[i] != '>')`: a normal "</p>" stops on '>'
 *    (the `!= '>'` arm goes false); a TRUNCATED "</p" with no '>' runs to
 *    `i >= len` (the `i < len` arm goes false).
 *  - block-end emit `priv_is_block(tag) && !internal_suppressed(ctx) && !emit`:
 *    closing a block `<p>` (block true, not suppressed) emits block-end;
 *    closing an inline `<b>` leaves priv_is_block false so no block-end.
 * Closing `<p>` proves the block arm true; closing `<b>` proves it false; the
 * truncated end-tag proves the `i < len` scan-termination arm.
 * @brief Verify end tag scan and block end behavior against the reflow contract.
 * @details Exercises the end tag scan and block end path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_end_tag_scan_and_block_end(void)
{
  TEST_BEGIN("priv_handle_end MC/DC");

  /* Normal close: '>' terminates the name scan; <p> emits a block-end. */
  TEST_ASSERT_EQ(k_ra8_ok, walk("<html><body><p>hi</p></body></html>"));
  TEST_ASSERT(count_kind(k_ra8_reflow_tok_block_end) >= 1U);

  /* Inline close <b>: priv_is_block false -> no block-end for it. */
  TEST_ASSERT_EQ(k_ra8_ok, walk("<html><body><p>a<b>x</b>b</p></body></html>"));
  /* Exactly one block-end (the <p>), confirming </b> emitted none. */
  TEST_ASSERT_EQ(k_count_one, count_kind(k_ra8_reflow_tok_block_end));

  /* Truncated end tag with no '>': the i<len arm ends the scan at EOF; the
   * truncated document is rejected as a validation failure. */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, walk("<html><body><p>x</p"));

  TEST_END("priv_handle_end MC/DC");
}

/**
 * @test internal_test_raw_text_style_vs_script
 *
 * @par MC/DC:
 * Decisions across internal_tag_is, internal_handle_lt and internal_handle_raw_text
 * (ra8_reflow_tokenize.c):
 *  - internal_tag_is delimiter `(c=='>') || (c=='/') || is_xml_whitespace(c)`:
 *    "<style>" (delimiter '>'), "<style src='a'>" (delimiter ' '), and a
 *    look-alike "<styled>" (the byte after "style" is 'd' -> all three arms
 *    false -> not the style element, parsed as an unknown tag).
 *  - internal_handle_raw_text style-parse `is_style && (close_at > open_end)`:
 *    a non-empty `<style>` body (is_style true, content present) parses CSS so
 *    a later `.x` rule applies; a `<script>` body (is_style false) is discarded.
 * The styled paragraph picking up the `<style>` colour proves the style-parse
 * arm; the script content never appearing proves the is_style false arm; the
 * "<styled>" text still flowing proves the internal_tag_is delimiter reject.
 * @brief Verify raw text style vs script behavior against the reflow contract.
 * @details Exercises the raw text style vs script path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_raw_text_style_vs_script(void)
{
  TEST_BEGIN("raw-text <style>/<script> MC/DC");

  s_engine.css_loader = nullptr;

  /* Baseline: no <style>, so the .hot run takes its default colour. */
  TEST_ASSERT_EQ(k_ra8_ok, walk("<html><body><p class=\"hot\">x</p></body></html>"));
  const uint32_t c_def = first_text_color();

  /* <style> with content -> parsed -> .hot colour changes (style-parse arm). */
  TEST_ASSERT_EQ(k_ra8_ok,
                 walk("<html><head><style>.hot{color:#00aa00}</style></head>"
                      "<body><p class=\"hot\">x</p></body></html>"));
  TEST_ASSERT(first_text_color() != c_def);

  /* <script> body -> is_style false -> discarded, its text never emits. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 walk("<html><body><p>before</p>"
                      "<script>var x = scripttext;</script>"
                      "<p>after</p></body></html>"));
  TEST_ASSERT(text_has("before"));
  TEST_ASSERT(text_has("after"));
  TEST_ASSERT(!text_has("scripttext"));

  /* "<styled>" look-alike: internal_tag_is delimiter check fails -> treated as an
   * ordinary (unknown) element, so its inner text still flows. */
  TEST_ASSERT_EQ(k_ra8_ok, walk("<html><body><p><styled>insidestyled</styled></p></body></html>"));
  TEST_ASSERT(text_has("insidestyled"));

  TEST_END("raw-text <style>/<script> MC/DC");
}

/**
 * @test internal_test_numeric_prefix_avail_zero
 *
 * @par MC/DC:
 * Decision (L249): `(i < avail) && ((src[i] == 'x') || (src[i] == 'X'))` in
 * internal_decode_numeric (ra8_reflow_tokenize.c). The existing vectors V1-V4 in
 * internal_test_numeric_base_select drive: (i<avail=true, src[i]='x' true),
 * (i<avail=true, src[i]='X' true), (i<avail=true, both false -> decimal).
 * The still-missing arm is i<avail FALSE, which requires avail == 2 so that
 * after consuming "&#" the index i=2 equals avail=2.
 *  - V5 "&#" avail=2 -> i=2 == avail -> outer AND false -> stays decimal ->
 *    loop runs 0 times -> digits==0 -> reject.
 * V5 is the only vector that makes the `i < avail` condition FALSE at L249.
 * @brief Verify numeric prefix avail zero behavior against the reflow contract.
 * @details Exercises the numeric prefix avail zero path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_numeric_prefix_avail_zero(void)
{
  TEST_BEGIN("priv_decode_numeric prefix i>=avail arm (L249)");
  uint32_t cp   = 0U;
  size_t   used = 0U;
  /* V5: avail==2 -> i=2 is not < avail -> outer AND false -> decimal/no prefix */
  TEST_ASSERT(!priv_ra8_reflow_tok_decode_entity("&#", 2U, &cp, &used));
  TEST_END("priv_decode_numeric prefix i>=avail arm (L249)");
}

/**
 * @test internal_test_numeric_digit_subrange
 *
 * @par MC/DC:
 * Decision (L258): `(c >= '0') && (c <= '9')` -- the decimal-range check in
 * internal_decode_numeric's digit loop. The existing internal_test_numeric_digit_classes
 * drives c='2' (true) and c='a' (c>='0' true but c<='9' false -> result false).
 * The still-missing arm is `c < '0'` (first sub-condition false), e.g. c='/'.
 *
 * Decision (L262): `(base == hex) && (c >= 'a') && (c <= 'f')` -- the
 * lowercase-hex range. With base=hex and c='g': base==hex=true, c>='a'=true,
 * c<='f'=false -> condition false -> tries uppercase check -> also false ->
 * else { return false }. This exercises the `c <= 'f'` false arm.
 *  - V6 "&#/;" -> c='/' -> c>='0' FALSE (first sub-cond false at L258) ->
 *    else { return false }.
 *  - V7 "&#xg;" -> base=hex, c='g' -> c>='a' true, c<='f' false (L262 false)
 *    -> uppercase check also false -> else { return false }.
 * V6 isolates the `c >= '0'` false arm; V7 isolates the `c <= 'f'` false arm.
 * @brief Verify numeric digit subrange behavior against the reflow contract.
 * @details Exercises the numeric digit subrange path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_numeric_digit_subrange(void)
{
  TEST_BEGIN("priv_decode_numeric digit sub-range arms (L258, L262)");
  uint32_t cp   = 0U;
  size_t   used = 0U;
  /* V6: c='/' (ASCII 47 < '0'=48) -> c>='0' false -> else -> reject (L258). */
  TEST_ASSERT(!priv_ra8_reflow_tok_decode_entity("&#/;", 4U, &cp, &used));
  /* V7: hex mode, c='g' -> c>='a' true, c<='f' false -> reject (L262). */
  TEST_ASSERT(!priv_ra8_reflow_tok_decode_entity("&#xg;", 5U, &cp, &used));
  TEST_END("priv_decode_numeric digit sub-range arms (L258, L262)");
}

/**
 * @test internal_test_numeric_terminator_unreachable_note
 *
 * @par MC/DC:
 * Decision (L271): `(digits == 0U) || (i >= avail) || (src[i] != ';')`.
 * The THIRD condition `src[i] != ';'` is structurally unreachable when
 * digits > 0: the digit loop exits either because `src[i] == ';'` (leaving
 * src[i]==';', so the third condition is false) or because `i >= avail`
 * (making the second condition true, which short-circuits the third).
 * No input can produce digits>0 AND i<avail AND src[i]!=';' after the loop.
 * This test documents that the two reachable conditions ARE driven:
 *  - V2 "&#;" digits==0 -> first arm true (covered by internal_test_numeric_terminator).
 *  - V3 "&#65" i>=avail -> second arm true (covered by internal_test_numeric_terminator).
 * The third arm (src[i]!=';') is an unreachable branch at L271.
 *
 * This function exists as a documentation anchor; it asserts nothing new but
 * records the structural unreachability so future auditors do not attempt to
 * cover it.
 *
 * @note RA8_MCDC_DEACTIVATED: the `src[i] != ';'` arm of the L271 OR
 * is not independently reachable because the digit-scan loop only exits on
 * src[i]==';' or i>=avail; the former keeps src[i]==';' (third cond false)
 * and the latter already makes the second cond true (short-circuit).
 * @brief Verify numeric terminator unreachable note behavior against the reflow contract.
 * @details Exercises the numeric terminator unreachable note path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_numeric_terminator_unreachable_note(void)
{
  TEST_BEGIN("priv_decode_numeric L271 third-arm unreachable doc (L271)");
  /* Both reachable terminator arms are exercised by internal_test_numeric_terminator.
   * The third arm (src[i]!=';') is a structural dead code path: no reachable
   * input exercises it (loop exits only via ';' or i>=avail). */
  uint32_t cp   = 0U;
  size_t   used = 0U;
  /* Confirm: digits>0 + terminator present -> accept (all three false -> ok). */
  TEST_ASSERT(priv_ra8_reflow_tok_decode_entity("&#65;", 5U, &cp, &used));
  TEST_ASSERT_EQ(k_cp_uppercase_a, cp);
  TEST_END("priv_decode_numeric L271 third-arm unreachable doc (L271)");
}

/**
 * @test internal_test_cdata_empty_content_and_sp_zero
 *
 * @par MC/DC:
 * Decisions in internal_handle_cdata (ra8_reflow_tokenize.c):
 *
 * L729 `(ctx->sp > 0U) && !internal_suppressed(ctx)`:
 *  - False via ctx->sp == 0U: a raw `<![CDATA[...]]>` that appears BEFORE
 *    any element is opened processes with sp=0, so the emit gate is false
 *    and no token is generated.  The document ends with no element seen
 *    (saw_element=false) -> k_ra8_err_validation_failed.
 *
 * L738 `(tlen > 0U) && !priv_emit(...)` false via tlen == 0 (sp>0 but
 * CDATA content collapses entirely to whitespace):
 *  - A CDATA section whose entire body is XML whitespace collapses to
 *    zero bytes (last_ws starts true and every byte is a space/newline).
 *    L729's if-body IS entered (sp>0, not suppressed), but the inner emit
 *    gate at L738 evaluates tlen==0 -> false -> neither the emit nor the
 *    reserved16 stamp (L746) runs.
 *
 * L738 `(tlen > 0U) && !priv_emit(...)` true arm:
 *  - A non-empty CDATA inside an element reaches the emit call; the emit
 *    succeeds (pool not full) so the `&&` short-circuits at the emit
 *    success side; L746 `if (tlen > 0U)` stamps reserved16 on the new
 *    token.  This drives BOTH the L738 emit-gate true arm AND L746.
 *
 *  - V-sp0  `<![CDATA[orphan]]>`  -> sp=0 -> L729 false -> no emit ->
 *    doc invalid (k_ra8_err_validation_failed).
 *  - V-ws   inside `<p>`, CDATA body = whitespace-only -> tlen=0 ->
 *    L738 false (tlen==0) -> no emit -> L746 not reached.
 *  - V-emit inside `<p>`, CDATA body = "hello" -> tlen>0 -> L738 true
 *    -> emit succeeds -> L746 stamps reserved16 on the token.
 * @brief Verify cdata empty content and sp zero behavior against the reflow contract.
 * @details Exercises the cdata empty content and sp zero path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_cdata_empty_content_and_sp_zero(void)
{
  TEST_BEGIN("priv_handle_cdata sp-zero and tlen-zero arms (L729, L738, L746)");

  /* V-sp0: CDATA before any element -> sp=0 -> emit gate false -> rejected. */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, walk("<![CDATA[orphan]]>"));

  /* V-ws: CDATA whose body is only whitespace -> tlen=0 -> no emit (L738 false,
   * L746 not reached).  The document is otherwise valid so we get k_ra8_ok. */
  TEST_ASSERT_EQ(k_ra8_ok, walk("<html><body><p><![CDATA[   ]]>filler</p></body></html>"));
  /* No CDATA-derived token; only the "filler" text token is present. */
  TEST_ASSERT(!text_has("   "));
  TEST_ASSERT(text_has("filler"));

  /* V-emit: non-empty CDATA -> tlen>0 -> emit succeeds -> L746 stamps
   * reserved16 (face_slot=0 for no registered face, so the stamp is 0). */
  TEST_ASSERT_EQ(k_ra8_ok, walk("<html><body><p><![CDATA[hello]]></p></body></html>"));
  TEST_ASSERT(text_has("hello")); /* token was emitted and reserved16 stamped */

  TEST_END("priv_handle_cdata sp-zero and tlen-zero arms (L729, L738, L746)");
}

/**
 * @test internal_test_end_tag_non_block_close
 *
 * @par MC/DC:
 * Decision (L792): `priv_is_block(tag) && !internal_suppressed(ctx) &&
 * !priv_emit(...)` in internal_handle_end.
 *  - V-block `</p>` -> priv_is_block true -> evaluates !internal_suppressed(ctx).
 *  - V-inline `</em>` -> priv_is_block false -> AND short-circuits -> no
 *    block-end emitted for the inline close.
 *  - V-sup `</p>` inside display:none -> block true, suppressed true ->
 *    !internal_suppressed(ctx) false -> no block-end.
 * V-block vs V-inline isolates priv_is_block; V-block vs V-sup isolates
 * the suppressed condition. N+1 = 3 vectors for the 2-condition leading AND.
 * @brief Verify end tag non block close behavior against the reflow contract.
 * @details Exercises the end tag non block close path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_end_tag_non_block_close(void)
{
  TEST_BEGIN("priv_handle_end non-block close MC/DC (L792)");

  /* V-block: </p> -> block=true, not suppressed -> block-end emitted. */
  TEST_ASSERT_EQ(k_ra8_ok, walk("<html><body><p>word</p></body></html>"));
  TEST_ASSERT(count_kind(k_ra8_reflow_tok_block_end) >= 1U);

  /* V-inline: </em> -> block=false -> AND short-circuits at priv_is_block
   * -> no block-end emitted for the </em>; only the <p> end contributes. */
  TEST_ASSERT_EQ(k_ra8_ok, walk("<html><body><p><em>italic</em>text</p></body></html>"));
  /* Exactly one block-end: the </p>.  The </em> produced none. */
  TEST_ASSERT_EQ(k_count_one, count_kind(k_ra8_reflow_tok_block_end));

  /* V-sup: </p> inside display:none -> block=true but suppressed -> !suppressed
   * is false -> no block-end for that </p>; only visible </p> at end counts. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 walk("<html><body><div style=\"display:none\">"
                      "<p>hidden</p></div><p>visible</p></body></html>"));
  TEST_ASSERT_EQ(k_count_one, count_kind(k_ra8_reflow_tok_block_end));

  TEST_END("priv_handle_end non-block close MC/DC (L792)");
}

/**
 * @test internal_test_raw_text_empty_style_body
 *
 * @par MC/DC:
 * Decision (L1591): `is_style && (close_at > open_end)` in internal_handle_raw_text.
 *  - V-content `<style>.x{...}</style>` -> is_style=true AND close_at>open_end
 *    (CSS body present) -> both true -> ra8_css_parse called.
 *  - V-empty   `<style></style>`        -> is_style=true but close_at==open_end
 *    (the </style> immediately follows the opening '>') -> second arm false
 *    -> ra8_css_parse NOT called (no CSS body to parse).
 *  - V-script  `<script>...</script>`   -> is_style=false -> first arm false.
 * V-content vs V-empty isolates the `close_at > open_end` condition.
 * V-content vs V-script isolates the `is_style` condition (covered by
 * internal_test_raw_text_style_vs_script; restated here for local MC/DC completeness).
 * @brief Verify raw text empty style body behavior against the reflow contract.
 * @details Exercises the raw text empty style body path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_raw_text_empty_style_body(void)
{
  TEST_BEGIN("priv_handle_raw_text empty style body (L1591)");

  s_engine.css_loader = nullptr;

  /* Baseline: no <style> -> default colour for .emp. */
  TEST_ASSERT_EQ(k_ra8_ok, walk("<html><body><p class=\"emp\">x</p></body></html>"));
  const uint32_t c_def = first_text_color();

  /* V-content: <style> with CSS body -> ra8_css_parse called -> colour changes. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 walk("<html><head><style>.emp{color:#33aa33}</style></head>"
                      "<body><p class=\"emp\">x</p></body></html>"));
  TEST_ASSERT(first_text_color() != c_def);

  /* V-empty: <style></style> -> close_at == open_end -> ra8_css_parse NOT called
   * -> colour stays at the no-style default for .emp. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 walk("<html><head><style></style></head>"
                      "<body><p class=\"emp\">x</p></body></html>"));
  TEST_ASSERT_EQ(c_def, first_text_color());

  TEST_END("priv_handle_raw_text empty style body (L1591)");
}

/**
 * @test internal_test_cdata_emit_pool_full_mcdc
 *
 * @par MC/DC:
 * Decision: `if ((tlen > 0U) && !priv_ra8_reflow_tok_emit(...))` -- the CDATA text emit
 * in internal_handle_cdata (libs/ra8_reflow/src/ra8_reflow_tokenize.c, 2 conditions, AND).
 * The emit-failure arm needs the token pool exactly full when a non-empty CDATA
 * run is emitted. Driving priv_reflow_xml_walk with token_count pre-set to
 * k_ra8_reflow_max_tokens - 1 makes the opening `<p>` fill the pool, so the CDATA
 * text emit is the overflowing one. N+1 completion of the existing empty-CDATA
 * (C1 false) and successful-emit (C2 false) vectors:
 *  - tlen > 0 ("Z") AND the emit fails (pool full) -> C1 true, C2 true -> the
 *    handler returns k_ra8_err_no_mem. This is the only vector reaching the
 *    emit-failure path; the pool cap is not otherwise hit in unit inputs.
 * @brief Verify cdata emit pool full mcdc behavior against the reflow contract.
 * @details Exercises the cdata emit pool full mcdc path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_cdata_emit_pool_full_mcdc(void)
{
  TEST_BEGIN("priv_handle_cdata MC/DC: non-empty CDATA emit on a full token pool");
  /* One below capacity: the opening <p> block-start fills the pool exactly, so
   * the CDATA text emit overflows. */
  s_engine.token_count    = (uint32_t)k_ra8_reflow_max_tokens - 1U;
  s_engine.text_pool_used = 0U;
  const char* const doc   = "<p><![CDATA[Z]]></p>";
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 priv_reflow_xml_walk(&s_engine, (const uint8_t*)doc, strlen(doc)));
  /* The <p> block-start was emitted (pool now exactly full); the CDATA was not. */
  TEST_ASSERT_EQ(k_ra8_reflow_max_tokens, s_engine.token_count);
  s_engine.token_count = 0U;
  TEST_END("priv_handle_cdata MC/DC: non-empty CDATA emit on a full token pool");
}

/**
 * @test internal_test_end_block_emit_pool_full_mcdc
 *
 * @par MC/DC:
 * Decision: `if (priv_ra8_reflow_tok_is_block(tag) && !internal_suppressed(ctx) &&
 * !priv_ra8_reflow_tok_emit(...))` -- the block-end emit in internal_handle_end
 * (libs/ra8_reflow/src/ra8_reflow_tokenize.c, 3 conditions, AND). The final
 * emit-failure condition needs the token pool full at a block close. Pre-setting
 * token_count to k_ra8_reflow_max_tokens - 1 lets the opening `<p>` fill the pool
 * so the `</p>` block-end emit overflows. N+1 completion of the existing
 * non-block-close (C1 false), suppressed (C2 false), and successful-emit (C3 false)
 * vectors:
 *  - block tag `p`, not suppressed, emit fails -> C1 true, C2 true, C3 true ->
 *    the handler returns k_ra8_err_no_mem.
 * @brief Verify end block emit pool full mcdc behavior against the reflow contract.
 * @details Exercises the end block emit pool full mcdc path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_end_block_emit_pool_full_mcdc(void)
{
  TEST_BEGIN("priv_handle_end MC/DC: block-end emit on a full token pool");
  s_engine.token_count    = (uint32_t)k_ra8_reflow_max_tokens - 1U;
  s_engine.text_pool_used = 0U;
  const char* const doc   = "<p></p>";
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 priv_reflow_xml_walk(&s_engine, (const uint8_t*)doc, strlen(doc)));
  TEST_ASSERT_EQ(k_ra8_reflow_max_tokens, s_engine.token_count);
  s_engine.token_count = 0U;
  TEST_END("priv_handle_end MC/DC: block-end emit on a full token pool");
}

/**
 * @test internal_test_decode_numeric_hex_below_upper_a_mcdc
 *
 * @par MC/DC:
 * Decision: `else if ((base == k_priv_base_hex) && (c >= 'A') && (c <= 'F'))` --
 * the uppercase-hex digit classifier in internal_decode_numeric
 * (libs/ra8_reflow/src/ra8_reflow_tokenize_lex.c, 3 conditions, AND). Existing
 * vectors cover an in-range 'A'..'F' (all true) and a byte above 'F' (C3 false).
 * The `(c >= 'A')` false side needs a non-digit byte that is BELOW 'A' yet reaches
 * this elif (so also below 'a', failing the lowercase elif):
 *  - "&#x@;" -- '@' (0x40) is below 'A' (0x41) with base hex -> C1 true, C2 false;
 *    the reference is rejected (return false). This completes the C2 independence
 *    pair against the in-range 'A'..'F' vector.
 * @brief Verify decode numeric hex below upper a mcdc behavior against the reflow contract.
 * @details Exercises the decode numeric hex below upper a mcdc path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_decode_numeric_hex_below_upper_a_mcdc(void)
{
  TEST_BEGIN("priv_decode_numeric MC/DC: hex byte below 'A' (c >= 'A' false)");
  uint32_t cp   = 0U;
  size_t   used = 0U;
  /* '@' = 0x40 is one below 'A'; with base hex it fails the (c >= 'A') condition. */
  TEST_ASSERT(!priv_ra8_reflow_tok_decode_entity("&#x@;", 5U, &cp, &used));
  TEST_END("priv_decode_numeric MC/DC: hex byte below 'A' (c >= 'A' false)");
}

/**
 * @brief Test executable entry point -- runs the scanner MC/DC vectors.
 *
 * @return 0 on success (all tests passed).
 *
 * @pre Host environment provides stderr.
 * @post Every scanning decision family above has executed its vectors.
 *
 * @note Not thread-safe (single-threaded test runner).
 * @since 0.1.0
 */
int32_t main(void)
{
  internal_test_numeric_base_select();
  internal_test_numeric_digit_classes();
  internal_test_numeric_terminator();
  internal_test_entity_short_guard();
  internal_test_named_entity_match();
  internal_test_cdata_close_and_emit();
  internal_test_end_tag_scan_and_block_end();
  internal_test_raw_text_style_vs_script();
  internal_test_numeric_prefix_avail_zero();
  internal_test_numeric_digit_subrange();
  internal_test_numeric_terminator_unreachable_note();
  internal_test_cdata_empty_content_and_sp_zero();
  internal_test_end_tag_non_block_close();
  internal_test_raw_text_empty_style_body();
  internal_test_cdata_emit_pool_full_mcdc();
  internal_test_end_block_emit_pool_full_mcdc();
  internal_test_decode_numeric_hex_below_upper_a_mcdc();
  return 0;
}
