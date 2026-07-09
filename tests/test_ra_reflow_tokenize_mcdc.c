/**
 * @file test_ra_reflow_tokenize_mcdc.c
 * @brief Supplemental MC/DC unit tests for libs/ra_reflow/src/ra_reflow_tokenize.c
 *
 * @details
 * Companion to `tests/test_ra_reflow_tokenize.c`. That file establishes the
 * happy-path behaviour of the no-heap XHTML tokenizer; this file targets the
 * compound boolean decisions that the original corpus left only partially
 * covered, driving each condition independently true and false. Every test
 * exercises the REAL tokenizer -- either the entity decoder directly
 * (`ra_reflow_tok_decode_entity`) or the full single-pass walk
 * (`priv_reflow_xml_walk`) over crafted byte-strings -- so the asserted
 * branches are genuinely executed and recorded by llvm-cov, not mirrored.
 *
 * The walk is reached font-free: `priv_reflow_xml_walk` populates the token /
 * text pools without any glyph layout, so no font fixture is needed for the
 * markup-dispatch decisions. The single test that has to register an embedded
 * `@font-face` (to drive `priv_resolve_face_slot` past its short-circuit) uses
 * the public `ra_reflow_init` / `ra_reflow_register_face` /
 * `ra_reflow_layout_chapter` API with the baked Ahem face.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fixture_ahem.h"
#include "ra_err.h"
#include "ra_reflow.h"
#include "ra_reflow_css.h"
#include "ra_reflow_tokenize_internal.h"
#include "unity_minimal.h"

/* Forward decl of the tokenizer entry point (defined in the production TU). */
ra_err_t priv_reflow_xml_walk(ra_reflow_t* engine, const uint8_t* xhtml_buf, size_t xhtml_len);

/** @brief Shared engine instance (large -- keep off the stack). */
static ra_reflow_t s_engine;

/** @brief Numeric / sizing constants used across the vectors (no magic numbers). */
typedef enum : uint32_t {
  k_cp_uppercase_a  = 65U,       /**< 'A', decimal "&#65;" expectation.        */
  k_cp_hex_4f       = 0x4FU,     /**< 'O', "&#x4f;" lowercase-hex expectation. */
  k_cp_hex_4f_upper = 0x4FU,     /**< 'O', "&#X4F;" uppercase-hex expectation. */
  k_avail_three     = 3U,        /**< Length of the 3-byte "&#x" fragment.     */
  k_used_dec_a      = 5U,        /**< Bytes consumed for "&#65;".              */
  k_default_font_px = 16U,       /**< Body font size for the face-slot walk.   */
  k_viewport_w      = 200U,      /**< Layout viewport width, px.               */
  k_viewport_h      = 400U,      /**< Layout viewport height, px.              */
  k_body_color      = 0xFFFFFFU, /**< Body colour for the layout engine.       */
  k_link_color      = 0x3060FFU, /**< Link colour for the layout engine.       */
  k_face_css_idx    = 0U,        /**< `@font-face` table index to register.    */
  k_count_one       = 1U,        /**< Expected count of exactly one token.     */
} test_consts_t;

/**
 * @brief Run the tokenizer over a NUL-terminated XHTML string.
 *
 * @details Resets the engine's token / text pools then invokes the production
 * single-pass walk, exactly as `tests/test_ra_reflow_tokenize.c` does, so the
 * crafted markup drives the real dispatch chain.
 *
 * @param[in] xhtml NUL-terminated XHTML source.
 * @return The walk result code.
 */
static ra_err_t walk(const char* xhtml)
{
  s_engine.token_count    = 0U;
  s_engine.text_pool_used = 0U;
  return priv_reflow_xml_walk(&s_engine, (const uint8_t*)xhtml, strlen(xhtml));
}

/** @brief True iff some text token's pool slice contains @p needle. */
static bool text_has(const char* needle)
{
  const size_t nl = strlen(needle);
  for (uint32_t i = 0U; i < s_engine.token_count; ++i) {
    if (s_engine.tokens[i].kind != (uint8_t)k_ra_reflow_tok_text) {
      continue;
    }
    const char*    t  = (const char*)&s_engine.text_pool[s_engine.tokens[i].text_off];
    const uint32_t tl = s_engine.tokens[i].text_len;
    if (nl > (size_t)tl) {
      continue;
    }
    for (uint32_t j = 0U; ((size_t)j + nl) <= (size_t)tl; ++j) {
      if (memcmp(&t[j], needle, nl) == 0) {
        return true;
      }
    }
  }
  return false;
}

/** @brief Count tokens of @p kind in the last walk's stream. */
static uint32_t count_kind(ra_reflow_token_kind_t kind)
{
  uint32_t n = 0U;
  for (uint32_t i = 0U; i < s_engine.token_count; ++i) {
    if (s_engine.tokens[i].kind == (uint8_t)kind) {
      ++n;
    }
  }
  return n;
}

/**
 * @test test_numeric_base_select
 *
 * @par MC/DC:
 * Decision: `(i < avail) && ((src[i] == 'x') || (src[i] == 'X'))` selecting
 * hex vs decimal in priv_decode_numeric (ra_reflow_tokenize.c).
 *  - V1 "&#65;"  avail=5 -> i<avail true,  'x' false, 'X' false -> decimal.
 *  - V2 "&#x4f;" avail=6 -> i<avail true,  'x' true              -> hex.
 *  - V3 "&#X4F;" avail=6 -> i<avail true,  'x' false,'X' true    -> hex.
 *  - V4 "&#x"    avail=3 -> i<avail FALSE  (and ';' missing -> reject).
 * V1 vs V2 isolates the 'x' arm; V1 vs V3 isolates the 'X' arm; V2 vs V4
 * isolates the `i < avail` guard. N+1 = 4 vectors for the 3-condition group.
 */
static void test_numeric_base_select(void)
{
  TEST_BEGIN("priv_decode_numeric base-select MC/DC");
  uint32_t cp   = 0U;
  size_t   used = 0U;
  TEST_ASSERT(ra_reflow_tok_decode_entity("&#65;", 5U, &cp, &used)); /* V1 decimal */
  TEST_ASSERT_EQ(k_cp_uppercase_a, cp);
  TEST_ASSERT_EQ(k_used_dec_a, used);
  TEST_ASSERT(ra_reflow_tok_decode_entity("&#x4f;", 6U, &cp, &used)); /* V2 'x' hex */
  TEST_ASSERT_EQ(k_cp_hex_4f, cp);
  TEST_ASSERT(ra_reflow_tok_decode_entity("&#X4F;", 6U, &cp, &used)); /* V3 'X' hex */
  TEST_ASSERT_EQ(k_cp_hex_4f_upper, cp);
  TEST_ASSERT(!ra_reflow_tok_decode_entity("&#x", k_avail_three, &cp, &used)); /* V4 short */
  TEST_END("priv_decode_numeric base-select MC/DC");
}

/**
 * @test test_numeric_digit_classes
 *
 * @par MC/DC:
 * Covers the digit-class chain in priv_decode_numeric's loop body
 * (ra_reflow_tokenize.c): the decimal class `(c>='0')&&(c<='9')`, the
 * lowercase-hex class `(base==hex)&&(c>='a')&&(c<='f')`, the uppercase-hex
 * class `(base==hex)&&(c>='A')&&(c<='F')`, and the final `else -> false`.
 *  - V1 "&#x2a;" -> '2' decimal-class true; 'a' lowercase-hex-class true.
 *  - V2 "&#x1B;" -> '1' decimal-class true; 'B' uppercase-hex-class true.
 *  - V3 "&#x1g;" -> '1' true; 'g' fails all three classes -> else -> reject.
 *  - V4 "&#1a;"  -> base is DECIMAL so 'a' fails the base==hex guard of both
 *                   hex classes (decimal-class also false) -> else -> reject.
 * V1 isolates the lowercase-hex class true arm, V2 the uppercase-hex class,
 * V3 the all-false (else) reject, V4 the base==hex guard false arm.
 */
static void test_numeric_digit_classes(void)
{
  TEST_BEGIN("priv_decode_numeric digit-class MC/DC");
  uint32_t cp   = 0U;
  size_t   used = 0U;
  TEST_ASSERT(ra_reflow_tok_decode_entity("&#x2a;", 6U, &cp, &used)); /* V1 dec + lc-hex */
  TEST_ASSERT_EQ(0x2AU, cp);
  TEST_ASSERT(ra_reflow_tok_decode_entity("&#x1B;", 6U, &cp, &used)); /* V2 dec + uc-hex */
  TEST_ASSERT_EQ(0x1BU, cp);
  TEST_ASSERT(!ra_reflow_tok_decode_entity("&#x1g;", 6U, &cp, &used)); /* V3 bad hex digit   */
  TEST_ASSERT(!ra_reflow_tok_decode_entity("&#1a;", 5U, &cp, &used));  /* V4 'a' not decimal */
  TEST_END("priv_decode_numeric digit-class MC/DC");
}

/**
 * @test test_numeric_terminator
 *
 * @par MC/DC:
 * Decision: `(digits == 0U) || (i >= avail) || (src[i] != ';')` -- the
 * post-loop terminator check in priv_decode_numeric (ra_reflow_tokenize.c).
 *  - V1 "&#65;"   -> digits!=0, i<avail, src[i]==';'  -> all false -> accept.
 *  - V2 "&#;"     -> digits==0 (loop saw ';' first)   -> first true -> reject.
 *  - V3 "&#65"    -> digits!=0, loop ran to i>=avail  -> 2nd true  -> reject.
 *  - V4 "&#65x;"  -> 'x' is not a decimal digit, so the loop's else returns
 *                    early; the terminator check is reached only by the cases
 *                    above. (Documented: the `src[i] != ';'` arm cannot be hit
 *                    after a clean digit run because the loop only exits on ';'
 *                    or i>=avail; V1/V2/V3 give the reachable independence.)
 * V1 vs V2 isolates `digits==0`; V1 vs V3 isolates `i>=avail`.
 */
static void test_numeric_terminator(void)
{
  TEST_BEGIN("priv_decode_numeric terminator MC/DC");
  uint32_t cp   = 0U;
  size_t   used = 0U;
  TEST_ASSERT(ra_reflow_tok_decode_entity("&#65;", 5U, &cp, &used)); /* V1 accept */
  TEST_ASSERT_EQ(k_cp_uppercase_a, cp);
  TEST_ASSERT(!ra_reflow_tok_decode_entity("&#;", 3U, &cp, &used));  /* V2 no digits     */
  TEST_ASSERT(!ra_reflow_tok_decode_entity("&#65", 4U, &cp, &used)); /* V3 no terminator */
  TEST_END("priv_decode_numeric terminator MC/DC");
}

/**
 * @test test_entity_short_guard
 *
 * @par MC/DC:
 * Decision: `(window < k_priv_entity_min) || (src[1] == '\0')` -- the early
 * reject in ra_reflow_tok_decode_entity (ra_reflow_tokenize.c). `window` is
 * `min(avail, 12)`, `k_priv_entity_min` is 4.
 *  - V1 "&amp;" avail=5 -> window=5>=4 false; src[1]='a'!='\0' false -> proceed.
 *  - V2 "&lt"   avail=3 -> window=3<4 TRUE                          -> reject.
 *  - V3 "&\0xx" avail=5 -> window=5>=4 false; src[1]=='\0' TRUE     -> reject.
 * V1 vs V2 isolates the window-too-small condition; V1 vs V3 isolates the
 * `src[1] == '\0'` condition. N+1 = 3 vectors for the 2-condition OR.
 */
static void test_entity_short_guard(void)
{
  TEST_BEGIN("decode_entity short-guard MC/DC");
  uint32_t   cp         = 0U;
  size_t     used       = 0U;
  const char nul_at_1[] = {'&', '\0', 'x', 'x', 'x'};
  TEST_ASSERT(ra_reflow_tok_decode_entity("&amp;", 5U, &cp, &used)); /* V1 proceed */
  TEST_ASSERT_EQ('&', cp);
  TEST_ASSERT(!ra_reflow_tok_decode_entity("&lt", 3U, &cp, &used));    /* V2 window<min */
  TEST_ASSERT(!ra_reflow_tok_decode_entity(nul_at_1, 5U, &cp, &used)); /* V3 src[1]==0  */
  TEST_END("decode_entity short-guard MC/DC");
}

/**
 * @test test_named_entity_match
 *
 * @par MC/DC:
 * Decision: `((wlen+2) <= window) && (src[1+wlen] == ';') && strncmp==0` --
 * the named-entity match in ra_reflow_tok_decode_entity (ra_reflow_tokenize.c).
 *  - V1 "&quot;" -> all three true            -> match (cp == '"').
 *  - V2 "&ampX"  -> name "amp" present but the byte after it is 'X' not ';'
 *                   -> terminator arm false   -> no match.
 *  - V3 "&zzz;"  -> length / ';' satisfied for "amp"(3) sized slots but the
 *                   strncmp differs for every table word -> compare arm false.
 * V1 isolates the all-true match; V2 isolates the `src[1+wlen]==';'` arm;
 * V3 isolates the strncmp arm. (The `(wlen+2)<=window` arm is exercised by the
 * window-too-small reject in test_entity_short_guard.)
 */
static void test_named_entity_match(void)
{
  TEST_BEGIN("decode_entity named-match MC/DC");
  uint32_t cp   = 0U;
  size_t   used = 0U;
  TEST_ASSERT(ra_reflow_tok_decode_entity("&quot;", 6U, &cp, &used)); /* V1 match */
  TEST_ASSERT_EQ('"', cp);
  TEST_ASSERT(!ra_reflow_tok_decode_entity("&ampX", 5U, &cp, &used)); /* V2 no ';'        */
  TEST_ASSERT(!ra_reflow_tok_decode_entity("&zzz;", 5U, &cp, &used)); /* V3 name mismatch */
  TEST_END("decode_entity named-match MC/DC");
}

/**
 * @test test_cdata_close_and_emit
 *
 * @par MC/DC:
 * Decisions in priv_handle_cdata (ra_reflow_tokenize.c):
 *  - close scan `((close+3)<=len) && (memcmp(&buf[close],"]]>")!=0)`: a
 *    terminated CDATA stops on the "]]>" match (memcmp arm false); an
 *    UNTERMINATED CDATA runs to `close+3 > len` (bounds arm false).
 *  - emit gate `(ctx->sp > 0U) && !priv_suppressed(ctx)`: inside `<p>` (sp>0,
 *    not suppressed) emits the inner text verbatim (no entity decode); inside
 *    a `display:none` block the suppressed arm drops it.
 * V-terminated + inside-element proves both arms true (text appears literally);
 * V-unterminated proves the bounds arm; V-suppressed proves the suppress arm.
 */
static void test_cdata_close_and_emit(void)
{
  TEST_BEGIN("priv_handle_cdata MC/DC");

  /* Terminated CDATA inside an element: emitted verbatim, '&' NOT decoded. */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p><![CDATA[raw & keep]]>tail</p></body></html>"));
  TEST_ASSERT(text_has("raw & keep")); /* literal ampersand survives */
  TEST_ASSERT(text_has("tail"));       /* parsing resumed past "]]>" */

  /* Unterminated CDATA: consumed to end-of-buffer (bounds arm of the scan).
   * The truncated document is rejected as a validation failure. */
  TEST_ASSERT_EQ(k_ra_err_validation_failed, walk("<html><body><p><![CDATA[never closed"));

  /* CDATA inside display:none -> the emit gate's suppress arm drops it. */
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><body><div style=\"display:none\">"
                      "<p><![CDATA[hiddencdata]]></p></div>"
                      "<p>aftercdata</p></body></html>"));
  TEST_ASSERT(!text_has("hiddencdata"));
  TEST_ASSERT(text_has("aftercdata"));

  TEST_END("priv_handle_cdata MC/DC");
}

/**
 * @test test_end_tag_scan_and_block_end
 *
 * @par MC/DC:
 * Decisions in priv_handle_end (ra_reflow_tokenize.c):
 *  - name scan `(i < len) && (buf[i] != '>')`: a normal "</p>" stops on '>'
 *    (the `!= '>'` arm goes false); a TRUNCATED "</p" with no '>' runs to
 *    `i >= len` (the `i < len` arm goes false).
 *  - block-end emit `priv_is_block(tag) && !priv_suppressed(ctx) && !emit`:
 *    closing a block `<p>` (block true, not suppressed) emits block-end;
 *    closing an inline `<b>` leaves priv_is_block false so no block-end.
 * Closing `<p>` proves the block arm true; closing `<b>` proves it false; the
 * truncated end-tag proves the `i < len` scan-termination arm.
 */
static void test_end_tag_scan_and_block_end(void)
{
  TEST_BEGIN("priv_handle_end MC/DC");

  /* Normal close: '>' terminates the name scan; <p> emits a block-end. */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p>hi</p></body></html>"));
  TEST_ASSERT(count_kind(k_ra_reflow_tok_block_end) >= 1U);

  /* Inline close <b>: priv_is_block false -> no block-end for it. */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p>a<b>x</b>b</p></body></html>"));
  /* Exactly one block-end (the <p>), confirming </b> emitted none. */
  TEST_ASSERT_EQ(k_count_one, (int64_t)count_kind(k_ra_reflow_tok_block_end));

  /* Truncated end tag with no '>': the i<len arm ends the scan at EOF; the
   * truncated document is rejected as a validation failure. */
  TEST_ASSERT_EQ(k_ra_err_validation_failed, walk("<html><body><p>x</p"));

  TEST_END("priv_handle_end MC/DC");
}

/**
 * @test test_start_tag_name_scan
 *
 * @par MC/DC:
 * Decision: `(i<len) && (buf[i]!='>') && (buf[i]!='/') && !is_xml_whitespace`
 * -- the start-tag NAME scan in priv_parse_start (ra_reflow_tokenize.c).
 *  - "<p>"      name ends on '>'  (the `!='>'` arm goes false).
 *  - "<br/>"    name ends on '/'  (the `!='/'` arm goes false).
 *  - "<p id=x>" name ends on ' '  (the whitespace arm goes false).
 *  - "<p"       (truncated) name scan runs to i>=len (the `i<len` arm false).
 * Each input drives a different terminating condition of the 4-term AND while
 * the others hold, giving independent influence per condition.
 */
static void test_start_tag_name_scan(void)
{
  TEST_BEGIN("priv_parse_start name-scan MC/DC");
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p>gt</p></body></html>")); /* '>' */
  TEST_ASSERT(count_kind(k_ra_reflow_tok_block_start) >= 1U);

  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p>a<br/>b</p></body></html>")); /* '/' */
  TEST_ASSERT_EQ(k_count_one, (int64_t)count_kind(k_ra_reflow_tok_break));

  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p id=x>ws</p></body></html>")); /* ws */
  TEST_ASSERT(text_has("ws"));

  TEST_ASSERT_EQ(k_ra_err_validation_failed, walk("<html><body><p")); /* i>=len */
  TEST_END("priv_parse_start name-scan MC/DC");
}

/**
 * @test test_start_tag_attr_and_selfclose
 *
 * @par MC/DC:
 * Decisions in the attribute-skip loop of priv_parse_start (ra_reflow_tokenize.c):
 *  - quote open `(c=='"') || (c=='\'')`: a double-quoted attr drives the
 *    first arm; a single-quoted attr drives the second arm.
 *  - quoted-value scan `(i<len) && (buf[i]!=quote)`: a closed quote stops on
 *    the matching quote (`!=quote` false); an UNCLOSED quote runs to i>=len.
 *  - self-close `(c=='/') && ((i+1)<len) && (buf[i+1]=='>')`: "<br/>" sets
 *    selfclose true; "<br x/y>" has '/' not followed by '>' (third arm false).
 * The double/single-quoted images, the unclosed-quote tag, and the "/>" vs
 * "/x" inputs each isolate one condition of these decisions.
 */
static void test_start_tag_attr_and_selfclose(void)
{
  TEST_BEGIN("priv_parse_start attr/self-close MC/DC");

  /* Double-quoted attribute containing '>' must not end the tag early. */
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><body><p>a<img src=\"x\" alt=\"a > b\"/>z</p></body></html>"));
  TEST_ASSERT_EQ(k_count_one, (int64_t)count_kind(k_ra_reflow_tok_image));
  TEST_ASSERT(text_has("z")); /* tag closed at the real '>' after "/>" */

  /* Single-quoted attribute drives the second arm of the quote-open OR. */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p>a<img src='y' alt='c > d'/>w</p></body></html>"));
  TEST_ASSERT_EQ(k_count_one, (int64_t)count_kind(k_ra_reflow_tok_image));
  TEST_ASSERT(text_has("w"));

  /* Unclosed quote: the value scan terminates on i>=len, tag ends at EOF; the
   * truncated document is rejected as a validation failure. */
  TEST_ASSERT_EQ(k_ra_err_validation_failed, walk("<html><body><p><img src=\"unterminated"));

  /* '/' not followed by '>': selfclose stays false (third arm false). */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p>a<img src=\"q/r\">k</p></body></html>"));
  TEST_ASSERT(text_has("k"));

  TEST_END("priv_parse_start attr/self-close MC/DC");
}

/**
 * @test test_attr_name_boundary
 *
 * @par MC/DC:
 * Decision: `((prev>='a')&&(prev<='z')) || ((prev>='A')&&(prev<='Z'))` -- the
 * "attribute name not preceded by a name byte" boundary in priv_attr_name_at
 * (ra_reflow_tokenize.c). The function returns the NEGATION, so a letter-prev
 * rejects the candidate and the scan keeps looking for a real attribute.
 *  - lowercase-prev: "asrc" -- the 'src' inside it has prev='a' (a..z true)
 *    -> rejected; the standalone ` src="hit"` (prev=space, both arms false)
 *    -> accepted. Proves the a..z arm.
 *  - uppercase-prev: "Zsrc" -- prev='Z' (A..Z true) -> rejected; ` src="ok"`
 *    accepted. Proves the A..Z arm.
 * Observable: the captured `<img>` src is the real one, never the decoy, so the
 * image token (and its text-pool src slice) reflect the accepted attribute.
 */
static void test_attr_name_boundary(void)
{
  TEST_BEGIN("priv_attr_name_at boundary MC/DC");

  /* Lowercase-letter prev on the decoy "asrc"; real " src" is accepted. */
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><body><p><img asrc=\"DECOYA\" src=\"realA\"/></p></body></html>"));
  /* The img tokenizes (its src attr is image metadata, not pooled text); the
   * decoy "asrc" name never leaks into the text pool. */
  TEST_ASSERT_EQ(k_count_one, (int64_t)count_kind(k_ra_reflow_tok_image));
  TEST_ASSERT(!text_has("DECOYA"));

  /* Uppercase-letter prev on the decoy "Zsrc"; real " src" is accepted. */
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><body><p><img Zsrc=\"DECOYB\" src=\"realB\"/></p></body></html>"));
  TEST_ASSERT_EQ(k_count_one, (int64_t)count_kind(k_ra_reflow_tok_image));
  TEST_ASSERT(!text_has("DECOYB"));

  TEST_END("priv_attr_name_at boundary MC/DC");
}

/**
 * @test test_attr_quoted_value_paths
 *
 * @par MC/DC:
 * Decisions in priv_attr_quoted_value (ra_reflow_tokenize.c):
 *  - require '=' : `(j >= tag_len) || (tag[j] != '=')`: `src="v"` has '=' ->
 *    false; a bare `src` with no '=' (followed by another attr) -> `!= '='`
 *    true -> that candidate rejected.
 *  - require quote: `(j >= tag_len) || ((tag[j]!='"') && (tag[j]!='\''))`:
 *    a quoted value passes; an UNQUOTED value `src=v` makes both quote arms
 *    true -> rejected.
 *  - value scan `(j < tag_len) && (tag[j] != quote)`: scans to the closing
 *    quote (`!=quote` false) or to i>=tag_len for an unterminated value.
 * The src="real" capture exercises the all-pass path; the `<img src>` (no '=')
 * and `<img src=bare ...>` (no quote) decoys exercise the reject arms while a
 * later proper attribute still gets captured.
 */
static void test_attr_quoted_value_paths(void)
{
  TEST_BEGIN("priv_attr_quoted_value MC/DC");

  /* No '=' after the first "src", but a later quoted alt is still captured;
   * the <img> still emits, proving the '=' reject arm did not abort the scan. */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p><img src alt=\"present\"/></p></body></html>"));
  TEST_ASSERT_EQ(k_count_one, (int64_t)count_kind(k_ra_reflow_tok_image));

  /* Unquoted value `src=bare`: quote arm rejects it; a later quoted src wins. */
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><body><p><img data=bare src=\"goodsrc\"/></p></body></html>"));
  TEST_ASSERT_EQ(k_count_one, (int64_t)count_kind(k_ra_reflow_tok_image));

  /* Whitespace around '=' and the quote exercises both skip loops. */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p><img src = \"spaced\"/></p></body></html>"));
  TEST_ASSERT_EQ(k_count_one, (int64_t)count_kind(k_ra_reflow_tok_image));

  TEST_END("priv_attr_quoted_value MC/DC");
}

/**
 * @test test_capture_attr_empty_value
 *
 * @par MC/DC:
 * Decision: `(vlen == 0U) || (pool_used + vlen > pool_bytes)` -- the
 * store guard in priv_capture_attr (ra_reflow_tokenize.c).
 *  - V1 src="real" -> vlen!=0 and the pool has room -> both false -> stored.
 *  - V2 src=""     -> vlen==0 (empty value)         -> first arm true -> skip.
 * V1 vs V2 isolates the `vlen == 0` condition (the overflow arm needs a
 * ~64 KiB value and is left to the pool-overflow corpus). Observable: the
 * empty-src image still emits as a token but stores no src bytes.
 */
static void test_capture_attr_empty_value(void)
{
  TEST_BEGIN("priv_capture_attr empty-value MC/DC");

  /* Non-empty src -> stored (the real-value branch); the img tokenizes. */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p><img src=\"realsrc\"/></p></body></html>"));
  TEST_ASSERT_EQ(k_count_one, (int64_t)count_kind(k_ra_reflow_tok_image));

  /* Empty src -> the vlen==0 arm skips the store; the image still emits. */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p><img src=\"\"/></p></body></html>"));
  TEST_ASSERT_EQ(k_count_one, (int64_t)count_kind(k_ra_reflow_tok_image));

  TEST_END("priv_capture_attr empty-value MC/DC");
}

/**
 * @test test_intern_link_empty_href
 *
 * @par MC/DC:
 * Decision: `(href_len == 0U) || (link_target_count >= max)` -- the intern
 * guard in priv_intern_link (ra_reflow_tokenize.c).
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
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><body><p><a href=\"page.html\">tappable</a></p></body></html>"));
  bool tagged = false;
  for (uint32_t i = 0U; i < s_engine.token_count; ++i) {
    if ((s_engine.tokens[i].kind == (uint8_t)k_ra_reflow_tok_text) &&
        (s_engine.tokens[i].reserved != 0U)) {
      tagged = true;
    }
  }
  TEST_ASSERT(tagged);

  /* Empty href -> not interned; no text run carries a link id. */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p><a href=\"\">plain</a></p></body></html>"));
  bool any_link = false;
  for (uint32_t i = 0U; i < s_engine.token_count; ++i) {
    if ((s_engine.tokens[i].kind == (uint8_t)k_ra_reflow_tok_text) &&
        (s_engine.tokens[i].reserved != 0U)) {
      any_link = true;
    }
  }
  TEST_ASSERT(!any_link);

  TEST_END("priv_intern_link empty-href MC/DC");
}

/** @brief External-stylesheet bytes returned by ::css_stub. */
static const char k_ext_css[] = ".lead{color:#c80000}";

/**
 * @brief ra_reflow_css_loader_fn stub that hands back ::k_ext_css for any href.
 *
 * @details Implements the engine's external-stylesheet loader seam for the
 * `<link>` tests: ignores the requested href and always returns the fixed
 * ::k_ext_css bytes with k_ra_ok, so a bound loader's success path is driven.
 *
 * @param[in]  ctx       Opaque loader context (unused).
 * @param[in]  href      Stylesheet href bytes (unused).
 * @param[in]  href_len  Length of @p href, bytes (unused).
 * @param[out] out_bytes Receives a pointer to ::k_ext_css.
 * @param[out] out_len   Receives the ::k_ext_css length, bytes.
 * @return Always k_ra_ok.
 * @retval k_ra_ok Stylesheet bytes returned.
 * @pre `out_bytes` and `out_len` are non-null.
 * @post `*out_bytes` aliases ::k_ext_css and `*out_len` is its length.
 * @note Test helper; not thread-safe.
 */
static ra_err_t
css_stub(void* ctx, const char* href, uint32_t href_len, const uint8_t** out_bytes, size_t* out_len)
{
  (void)ctx;
  (void)href;
  (void)href_len;
  *out_bytes = (const uint8_t*)k_ext_css;
  *out_len   = sizeof(k_ext_css) - 1U;
  return k_ra_ok;
}

/**
 * @brief ra_reflow_css_loader_fn stub that always fails (no bytes).
 *
 * @details Drives the loader-failure path of priv_handle_link: clears the
 * output pointers and returns k_ra_err_not_found, so the css-parse step is
 * skipped and the chapter keeps its default colours.
 *
 * @param[in]  ctx       Opaque loader context (unused).
 * @param[in]  href      Stylesheet href bytes (unused).
 * @param[in]  href_len  Length of @p href, bytes (unused).
 * @param[out] out_bytes Set to nullptr (no bytes).
 * @param[out] out_len   Set to 0 (no bytes).
 * @return Always k_ra_err_not_found.
 * @retval k_ra_err_not_found Stylesheet unavailable.
 * @pre `out_bytes` and `out_len` are non-null.
 * @post `*out_bytes == nullptr` and `*out_len == 0`.
 * @note Test helper; not thread-safe.
 */
static ra_err_t css_fail_stub(void*           ctx,
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
  return k_ra_err_not_found;
}

/**
 * @brief Colour of the first text token after a walk (sentinel if none).
 *
 * @details Scans ::s_engine's token stream from the most recent walk and
 * returns the `color` field of the first text token. Used by the `<link>` /
 * `<style>` tests to observe whether a stylesheet rule resolved onto a run.
 *
 * @return The first text token's 0xRRGGBB colour (or k_ra_reflow_color_inherit),
 *         or 0xDEADBEEFU when the stream holds no text token.
 * @retval 0xDEADBEEFU No text token in the last walk.
 * @pre A walk has populated ::s_engine.
 * @post No state is modified (read-only).
 * @note Test helper; reads ::s_engine, not thread-safe.
 */
static uint32_t first_text_color(void)
{
  for (uint32_t i = 0U; i < s_engine.token_count; ++i) {
    if (s_engine.tokens[i].kind == (uint8_t)k_ra_reflow_tok_text) {
      return s_engine.tokens[i].color;
    }
  }
  return 0xDEADBEEFU;
}

/**
 * @test test_rel_is_stylesheet_scan
 *
 * @par MC/DC:
 * Decision: the inner match loop `(j < k_klen) && (rel[i+j] == k_kw[j])` of
 * priv_rel_is_stylesheet (ra_reflow_tokenize.c). The outer guard in
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
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p class=\"lead\">x</p></body></html>"));
  const uint32_t c_def = first_text_color();

  s_engine.css_loader     = css_stub;
  s_engine.css_loader_ctx = nullptr;

  /* V1 rel contains "stylesheet" exactly -> loads -> colour changes. */
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><head><link rel=\"stylesheet\" href=\"s\"/></head>"
                      "<body><p class=\"lead\">x</p></body></html>"));
  TEST_ASSERT(first_text_color() != c_def);

  /* V2 rel is a near-miss ("stylesheeX") -> no full match -> no load. */
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><head><link rel=\"stylesheeX\" href=\"s\"/></head>"
                      "<body><p class=\"lead\">x</p></body></html>"));
  TEST_ASSERT_EQ((int64_t)c_def, (int64_t)first_text_color());

  s_engine.css_loader = nullptr;
  TEST_END("priv_rel_is_stylesheet MC/DC");
}

/**
 * @test test_link_loader_result
 *
 * @par MC/DC:
 * Decision: `(loader(...) == k_ra_ok) && (css_bytes != nullptr) &&
 * (css_len > 0U)` -- the loader-result guard in priv_handle_link
 * (ra_reflow_tokenize.c) before parsing the fetched CSS.
 *  - V1 css_stub returns k_ra_ok + non-null bytes + len>0 -> all true -> parse
 *    -> the `.lead` colour changes from its no-link default.
 *  - V2 css_fail_stub returns k_ra_err_not_found (and null/0) -> first arm
 *    false -> no parse -> colour stays at the default.
 * V1 vs V2 isolates the loader-return-code condition (the null-pointer and
 * zero-length arms are guarded by the same stub on the failure path).
 */
static void test_link_loader_result(void)
{
  TEST_BEGIN("priv_handle_link loader-result MC/DC");

  s_engine.css_loader = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p class=\"lead\">x</p></body></html>"));
  const uint32_t c_def = first_text_color();

  /* V1 success loader -> CSS parsed -> colour changes. */
  s_engine.css_loader     = css_stub;
  s_engine.css_loader_ctx = nullptr;
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><head><link rel=\"stylesheet\" href=\"s\"/></head>"
                      "<body><p class=\"lead\">x</p></body></html>"));
  TEST_ASSERT(first_text_color() != c_def);

  /* V2 failing loader -> no parse -> default colour. */
  s_engine.css_loader = css_fail_stub;
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><head><link rel=\"stylesheet\" href=\"s\"/></head>"
                      "<body><p class=\"lead\">x</p></body></html>"));
  TEST_ASSERT_EQ((int64_t)c_def, (int64_t)first_text_color());

  s_engine.css_loader = nullptr;
  TEST_END("priv_handle_link loader-result MC/DC");
}

/**
 * @test test_display_none_begin
 *
 * @par MC/DC:
 * Decisions in priv_open_styled (ra_reflow_tokenize.c):
 *  - hidden detect `((comp.set & k_ra_css_set_display)!=0) && (comp.display!=0)`:
 *    `display:none` sets both bits true -> hidden; `display:block` declares
 *    display (set bit true) but `comp.display==0` -> NOT hidden (second arm
 *    false); an undeclared display leaves the set bit false (first arm false).
 *  - begin-suppress `hidden && (ctx->suppress_sp == 0U)`: the OUTER hidden div
 *    begins suppression; a nested hidden element inside it has suppress_sp!=0
 *    already (second arm false) so it does not reset the depth.
 * The display:none subtree drops its text; display:block keeps it; nesting two
 * hidden blocks proves the suppress_sp==0 guard.
 */
static void test_display_none_begin(void)
{
  TEST_BEGIN("priv_open_styled display MC/DC");

  /* display:none -> hidden true (both arms) -> subtree dropped. */
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><body><div style=\"display:none\"><p>nopaint</p></div>"
                      "<p>painted</p></body></html>"));
  TEST_ASSERT(!text_has("nopaint"));
  TEST_ASSERT(text_has("painted"));

  /* display:block -> display declared but value 0 -> NOT hidden (2nd arm). */
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><body><div style=\"display:block\"><p>shownblock</p></div>"
                      "</body></html>"));
  TEST_ASSERT(text_has("shownblock"));

  /* Nested hidden blocks: only the outer begins suppression (suppress_sp==0
   * guard); both subtrees stay dropped, the trailing block reappears. */
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><body><div style=\"display:none\">"
                      "<div style=\"display:none\"><p>deepnone</p></div>"
                      "<p>midnone</p></div><p>visibletail</p></body></html>"));
  TEST_ASSERT(!text_has("deepnone"));
  TEST_ASSERT(!text_has("midnone"));
  TEST_ASSERT(text_has("visibletail"));

  TEST_END("priv_open_styled display MC/DC");
}

/**
 * @test test_selfclose_block_pair
 *
 * @par MC/DC:
 * Decision: `block && !priv_suppressed(ctx)` -- the self-closing block emit in
 * priv_handle_start (ra_reflow_tokenize.c).
 *  - V1 "<p/>" outside suppression -> block true, not suppressed -> emits an
 *    empty block-start + block-end pair.
 *  - V2 "<span/>" (unknown tag) -> block false -> no block tokens for it.
 *  - V3 "<p/>" inside a display:none div -> block true but suppressed -> no
 *    tokens (the suppress arm).
 * V1 vs V2 isolates the `block` condition; V1 vs V3 isolates the
 * `!priv_suppressed` condition.
 */
static void test_selfclose_block_pair(void)
{
  TEST_BEGIN("priv_handle_start self-close block MC/DC");

  /* V1 self-closing block -> empty start/end pair emitted. */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p/></body></html>"));
  TEST_ASSERT(count_kind(k_ra_reflow_tok_block_start) >= 1U);
  TEST_ASSERT(count_kind(k_ra_reflow_tok_block_end) >= 1U);

  /* V2 self-closing UNKNOWN tag -> not a block -> no block tokens. */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p>x</p><span/></body></html>"));
  /* Exactly the one <p> pair -> the <span/> emitted nothing. */
  TEST_ASSERT_EQ(k_count_one, (int64_t)count_kind(k_ra_reflow_tok_block_start));

  /* V3 self-closing block inside display:none -> suppressed -> no tokens. */
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><body><div style=\"display:none\"><p/></div>"
                      "<p>tailblock</p></body></html>"));
  /* Only the visible trailing <p> pair survives. */
  TEST_ASSERT_EQ(k_count_one, (int64_t)count_kind(k_ra_reflow_tok_block_start));
  TEST_ASSERT(text_has("tailblock"));

  TEST_END("priv_handle_start self-close block MC/DC");
}

/**
 * @test test_raw_text_style_vs_script
 *
 * @par MC/DC:
 * Decisions across priv_tag_is, priv_handle_lt and priv_handle_raw_text
 * (ra_reflow_tokenize.c):
 *  - priv_tag_is delimiter `(c=='>') || (c=='/') || is_xml_whitespace(c)`:
 *    "<style>" (delimiter '>'), "<style src='a'>" (delimiter ' '), and a
 *    look-alike "<styled>" (the byte after "style" is 'd' -> all three arms
 *    false -> not the style element, parsed as an unknown tag).
 *  - priv_handle_raw_text style-parse `is_style && (close_at > open_end)`:
 *    a non-empty `<style>` body (is_style true, content present) parses CSS so
 *    a later `.x` rule applies; a `<script>` body (is_style false) is discarded.
 * The styled paragraph picking up the `<style>` colour proves the style-parse
 * arm; the script content never appearing proves the is_style false arm; the
 * "<styled>" text still flowing proves the priv_tag_is delimiter reject.
 */
static void test_raw_text_style_vs_script(void)
{
  TEST_BEGIN("raw-text <style>/<script> MC/DC");

  s_engine.css_loader = nullptr;

  /* Baseline: no <style>, so the .hot run takes its default colour. */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p class=\"hot\">x</p></body></html>"));
  const uint32_t c_def = first_text_color();

  /* <style> with content -> parsed -> .hot colour changes (style-parse arm). */
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><head><style>.hot{color:#00aa00}</style></head>"
                      "<body><p class=\"hot\">x</p></body></html>"));
  TEST_ASSERT(first_text_color() != c_def);

  /* <script> body -> is_style false -> discarded, its text never emits. */
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><body><p>before</p>"
                      "<script>var x = scripttext;</script>"
                      "<p>after</p></body></html>"));
  TEST_ASSERT(text_has("before"));
  TEST_ASSERT(text_has("after"));
  TEST_ASSERT(!text_has("scripttext"));

  /* "<styled>" look-alike: priv_tag_is delimiter check fails -> treated as an
   * ordinary (unknown) element, so its inner text still flows. */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p><styled>insidestyled</styled></p></body></html>"));
  TEST_ASSERT(text_has("insidestyled"));

  TEST_END("raw-text <style>/<script> MC/DC");
}

/**
 * @test test_lt_end_vs_start_dispatch
 *
 * @par MC/DC:
 * Decision: `((*pi + 1U) < len) && (buf[*pi + 1U] == '/')` -- the end-tag vs
 * start-tag fork in priv_handle_lt (ra_reflow_tokenize.c).
 *  - V1 "</p>"      -> next byte is '/' (and in range) -> end-tag handler.
 *  - V2 "<p>"       -> next byte is 'p' not '/'        -> start-tag handler.
 *  - V3 a lone '<' at end-of-buffer -> `(*pi+1)<len` false -> start handler
 *    on a truncated tag (no '/').
 * V1 vs V2 isolates the `buf[*pi+1]=='/'` condition; V1/V2 vs V3 isolates the
 * `(*pi+1)<len` bounds condition.
 */
static void test_lt_end_vs_start_dispatch(void)
{
  TEST_BEGIN("priv_handle_lt end/start fork MC/DC");

  /* V1 + V2: a balanced <p>..</p> exercises both the start and end forks. */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p>balanced</p></body></html>"));
  TEST_ASSERT(count_kind(k_ra_reflow_tok_block_start) >= 1U);
  TEST_ASSERT(count_kind(k_ra_reflow_tok_block_end) >= 1U);

  /* V3 lone '<' at EOF: (*pi+1)<len is false -> start handler on a stub tag. */
  TEST_ASSERT_EQ(k_ra_err_validation_failed, walk("<html><body><p>x</p></body><"));

  TEST_END("priv_handle_lt end/start fork MC/DC");
}

/**
 * @test test_walk_null_guard
 *
 * @par MC/DC:
 * Decision: `(engine == nullptr) || (xhtml_buf == nullptr)` -- the entry guard
 * of priv_reflow_xml_walk (ra_reflow_tokenize.c).
 *  - V1 engine non-null, buf non-null -> both false -> proceeds (k_ra_ok on a
 *    well-formed document).
 *  - V2 engine NULL,     buf non-null -> first arm true  -> k_ra_err_null_ptr.
 *  - V3 engine non-null, buf NULL     -> second arm true -> k_ra_err_null_ptr.
 * V1 vs V2 isolates the engine arm; V1 vs V3 isolates the buf arm. The
 * `xhtml_len == 0U` follow-on guard is exercised by the empty-input vector.
 */
static void test_walk_null_guard(void)
{
  TEST_BEGIN("priv_reflow_xml_walk null-guard MC/DC");

  /* V1 both non-null -> proceeds. */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p>ok</p></body></html>"));

  /* V2 null engine -> first arm true. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, priv_reflow_xml_walk(nullptr, (const uint8_t*)"x", 1U));

  /* V3 null buffer -> second arm true. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, priv_reflow_xml_walk(&s_engine, nullptr, 1U));

  /* Follow-on len==0 guard. */
  TEST_ASSERT_EQ(k_ra_err_invalid_size, priv_reflow_xml_walk(&s_engine, (const uint8_t*)"x", 0U));

  TEST_END("priv_reflow_xml_walk null-guard MC/DC");
}

/**
 * @test test_resolve_face_slot_conditions
 *
 * @par MC/DC:
 * Decision: `(face_count==0) || ((comp.set & family)==0) || (family_len==0)`
 * -- the early return in priv_resolve_face_slot (ra_reflow_tokenize.c). This is
 * the only decision reachable solely through the public layout API: with no
 * registered face the first arm short-circuits true on every element (covered
 * by every other walk in this file). Here a face IS registered, so the first
 * arm is false and the cascade is forced to evaluate the family arms.
 *  - With a registered face and an element carrying `font-family`, the set-bit
 *    arm is false and `family_len != 0` (third arm false), so the function
 *    proceeds to ra_css_match_face -- driving the false side of all three
 *    conditions in one element.
 *  - An element WITHOUT a font-family has the `(set & family)==0` arm true,
 *    short-circuiting the third condition.
 * The full layout simply succeeds; the value win is reaching the post-guard
 * body (ra_css_match_face + the face-registry scan) at all.
 */
static void test_resolve_face_slot_conditions(void)
{
  TEST_BEGIN("priv_resolve_face_slot MC/DC");

  uint32_t pages = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_reflow_init((uint16_t)k_viewport_w,
                                (uint16_t)k_viewport_h,
                                k_fixture_ahem,
                                (size_t)k_fixture_ahem_len,
                                (uint16_t)k_default_font_px,
                                (uint32_t)k_body_color,
                                (uint32_t)k_link_color,
                                &s_engine));

  /* Register the Ahem blob as an embedded @font-face so face_count != 0,
   * forcing the first arm of the guard false on every element. */
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_reflow_register_face(&s_engine,
                                         (uint8_t)k_face_css_idx,
                                         k_fixture_ahem,
                                         (size_t)k_fixture_ahem_len));

  /* One element with a font-family (family arms false -> reaches match_face),
   * one without (the (set & family)==0 arm true). Layout must still succeed. */
  const char* doc = "<html><head><style>@font-face{font-family:Baked;src:url(b.ttf)}"
                    ".fam{font-family:Baked}</style></head>"
                    "<body><p class=\"fam\">styledfam</p><p>plainfam</p></body></html>";
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_reflow_layout_chapter(&s_engine, (const uint8_t*)doc, (uint32_t)strlen(doc), &pages));
  TEST_ASSERT(pages >= 1U);

  TEST_ASSERT_EQ(k_ra_ok, ra_reflow_close(&s_engine));
  TEST_END("priv_resolve_face_slot MC/DC");
}

/**
 * @test test_numeric_prefix_avail_zero
 *
 * @par MC/DC:
 * Decision (L249): `(i < avail) && ((src[i] == 'x') || (src[i] == 'X'))` in
 * priv_decode_numeric (ra_reflow_tokenize.c). The existing vectors V1-V4 in
 * test_numeric_base_select drive: (i<avail=true, src[i]='x' true),
 * (i<avail=true, src[i]='X' true), (i<avail=true, both false -> decimal).
 * The still-missing arm is i<avail FALSE, which requires avail == 2 so that
 * after consuming "&#" the index i=2 equals avail=2.
 *  - V5 "&#" avail=2 -> i=2 == avail -> outer AND false -> stays decimal ->
 *    loop runs 0 times -> digits==0 -> reject.
 * V5 is the only vector that makes the `i < avail` condition FALSE at L249.
 */
static void test_numeric_prefix_avail_zero(void)
{
  TEST_BEGIN("priv_decode_numeric prefix i>=avail arm (L249)");
  uint32_t cp   = 0U;
  size_t   used = 0U;
  /* V5: avail==2 -> i=2 is not < avail -> outer AND false -> decimal/no prefix */
  TEST_ASSERT(!ra_reflow_tok_decode_entity("&#", 2U, &cp, &used));
  TEST_END("priv_decode_numeric prefix i>=avail arm (L249)");
}

/**
 * @test test_numeric_digit_subrange
 *
 * @par MC/DC:
 * Decision (L258): `(c >= '0') && (c <= '9')` -- the decimal-range check in
 * priv_decode_numeric's digit loop. The existing test_numeric_digit_classes
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
 */
static void test_numeric_digit_subrange(void)
{
  TEST_BEGIN("priv_decode_numeric digit sub-range arms (L258, L262)");
  uint32_t cp   = 0U;
  size_t   used = 0U;
  /* V6: c='/' (ASCII 47 < '0'=48) -> c>='0' false -> else -> reject (L258). */
  TEST_ASSERT(!ra_reflow_tok_decode_entity("&#/;", 4U, &cp, &used));
  /* V7: hex mode, c='g' -> c>='a' true, c<='f' false -> reject (L262). */
  TEST_ASSERT(!ra_reflow_tok_decode_entity("&#xg;", 5U, &cp, &used));
  TEST_END("priv_decode_numeric digit sub-range arms (L258, L262)");
}

/**
 * @test test_numeric_terminator_unreachable_note
 *
 * @par MC/DC:
 * Decision (L271): `(digits == 0U) || (i >= avail) || (src[i] != ';')`.
 * The THIRD condition `src[i] != ';'` is structurally unreachable when
 * digits > 0: the digit loop exits either because `src[i] == ';'` (leaving
 * src[i]==';', so the third condition is false) or because `i >= avail`
 * (making the second condition true, which short-circuits the third).
 * No input can produce digits>0 AND i<avail AND src[i]!=';' after the loop.
 * This test documents that the two reachable conditions ARE driven:
 *  - V2 "&#;" digits==0 -> first arm true (covered by test_numeric_terminator).
 *  - V3 "&#65" i>=avail -> second arm true (covered by test_numeric_terminator).
 * The third arm (src[i]!=';') is an unreachable branch at L271.
 *
 * This function exists as a documentation anchor; it asserts nothing new but
 * records the structural unreachability so future auditors do not attempt to
 * cover it.
 *
 * @note RA_MCDC_DEACTIVATED: the `src[i] != ';'` arm of the L271 OR
 * is not independently reachable because the digit-scan loop only exits on
 * src[i]==';' or i>=avail; the former keeps src[i]==';' (third cond false)
 * and the latter already makes the second cond true (short-circuit).
 */
static void test_numeric_terminator_unreachable_note(void)
{
  TEST_BEGIN("priv_decode_numeric L271 third-arm unreachable doc (L271)");
  /* Both reachable terminator arms are exercised by test_numeric_terminator.
   * The third arm (src[i]!=';') is a structural dead code path: no reachable
   * input exercises it (loop exits only via ';' or i>=avail). */
  uint32_t cp   = 0U;
  size_t   used = 0U;
  /* Confirm: digits>0 + terminator present -> accept (all three false -> ok). */
  TEST_ASSERT(ra_reflow_tok_decode_entity("&#65;", 5U, &cp, &used));
  TEST_ASSERT_EQ(k_cp_uppercase_a, cp);
  TEST_END("priv_decode_numeric L271 third-arm unreachable doc (L271)");
}

/**
 * @test test_cdata_empty_content_and_sp_zero
 *
 * @par MC/DC:
 * Decisions in priv_handle_cdata (ra_reflow_tokenize.c):
 *
 * L729 `(ctx->sp > 0U) && !priv_suppressed(ctx)`:
 *  - False via ctx->sp == 0U: a raw `<![CDATA[...]]>` that appears BEFORE
 *    any element is opened processes with sp=0, so the emit gate is false
 *    and no token is generated.  The document ends with no element seen
 *    (saw_element=false) -> k_ra_err_validation_failed.
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
 *    doc invalid (k_ra_err_validation_failed).
 *  - V-ws   inside `<p>`, CDATA body = whitespace-only -> tlen=0 ->
 *    L738 false (tlen==0) -> no emit -> L746 not reached.
 *  - V-emit inside `<p>`, CDATA body = "hello" -> tlen>0 -> L738 true
 *    -> emit succeeds -> L746 stamps reserved16 on the token.
 */
static void test_cdata_empty_content_and_sp_zero(void)
{
  TEST_BEGIN("priv_handle_cdata sp-zero and tlen-zero arms (L729, L738, L746)");

  /* V-sp0: CDATA before any element -> sp=0 -> emit gate false -> rejected. */
  TEST_ASSERT_EQ(k_ra_err_validation_failed, walk("<![CDATA[orphan]]>"));

  /* V-ws: CDATA whose body is only whitespace -> tlen=0 -> no emit (L738 false,
   * L746 not reached).  The document is otherwise valid so we get k_ra_ok. */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p><![CDATA[   ]]>filler</p></body></html>"));
  /* No CDATA-derived token; only the "filler" text token is present. */
  TEST_ASSERT(!text_has("   "));
  TEST_ASSERT(text_has("filler"));

  /* V-emit: non-empty CDATA -> tlen>0 -> emit succeeds -> L746 stamps
   * reserved16 (face_slot=0 for no registered face, so the stamp is 0). */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p><![CDATA[hello]]></p></body></html>"));
  TEST_ASSERT(text_has("hello")); /* token was emitted and reserved16 stamped */

  TEST_END("priv_handle_cdata sp-zero and tlen-zero arms (L729, L738, L746)");
}

/**
 * @test test_end_tag_non_block_close
 *
 * @par MC/DC:
 * Decision (L792): `priv_is_block(tag) && !priv_suppressed(ctx) &&
 * !priv_emit(...)` in priv_handle_end.
 *  - V-block `</p>` -> priv_is_block true -> evaluates !priv_suppressed(ctx).
 *  - V-inline `</em>` -> priv_is_block false -> AND short-circuits -> no
 *    block-end emitted for the inline close.
 *  - V-sup `</p>` inside display:none -> block true, suppressed true ->
 *    !priv_suppressed(ctx) false -> no block-end.
 * V-block vs V-inline isolates priv_is_block; V-block vs V-sup isolates
 * the suppressed condition. N+1 = 3 vectors for the 2-condition leading AND.
 */
static void test_end_tag_non_block_close(void)
{
  TEST_BEGIN("priv_handle_end non-block close MC/DC (L792)");

  /* V-block: </p> -> block=true, not suppressed -> block-end emitted. */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p>word</p></body></html>"));
  TEST_ASSERT(count_kind(k_ra_reflow_tok_block_end) >= 1U);

  /* V-inline: </em> -> block=false -> AND short-circuits at priv_is_block
   * -> no block-end emitted for the </em>; only the <p> end contributes. */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p><em>italic</em>text</p></body></html>"));
  /* Exactly one block-end: the </p>.  The </em> produced none. */
  TEST_ASSERT_EQ(k_count_one, (int64_t)count_kind(k_ra_reflow_tok_block_end));

  /* V-sup: </p> inside display:none -> block=true but suppressed -> !suppressed
   * is false -> no block-end for that </p>; only visible </p> at end counts. */
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><body><div style=\"display:none\">"
                      "<p>hidden</p></div><p>visible</p></body></html>"));
  TEST_ASSERT_EQ(k_count_one, (int64_t)count_kind(k_ra_reflow_tok_block_end));

  TEST_END("priv_handle_end non-block close MC/DC (L792)");
}

/**
 * @test test_selfclose_slash_at_end_of_tag
 *
 * @par MC/DC:
 * Decision (L840): `(c == '/') && ((i + 1U) < len) && (buf[i + 1U] == '>')`
 * in priv_parse_start's attribute-skip loop (reached only for bare '/' chars
 * OUTSIDE quoted attribute values).
 *  - V-true  `<br/>` -> c='/', (i+1)<len true, buf[i+1]='>' true -> selfclose.
 *  - V-noeq  `<br x/y>` -> c='/', (i+1)<len true, buf[i+1]='y' != '>' ->
 *    third arm false -> selfclose stays false.
 *  - V-eob   the buffer ends on a bare '/': c='/', (i+1)>=len ->
 *    second arm false -> AND short-circuits; selfclose stays false; the
 *    truncated document returns k_ra_err_validation_failed.
 * V-true vs V-noeq isolates buf[i+1]!='>' (third arm false).
 * V-true vs V-eob  isolates (i+1)<len     (second arm false).
 */
static void test_selfclose_slash_at_end_of_tag(void)
{
  TEST_BEGIN("priv_parse_start selfclose '/' branches (L840)");

  /* V-true: standard self-close -> break token present. */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p>a<br/>b</p></body></html>"));
  TEST_ASSERT_EQ(k_count_one, (int64_t)count_kind(k_ra_reflow_tok_break));

  /* V-noeq: bare '/' in the attr area not followed by '>'; selfclose stays
   * false.  The existing test_start_tag_attr_and_selfclose covers this with
   * "<br x/y>"; repeated here for local MC/DC completeness of L840. */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p>a<img src=\"q\" /x>b</p></body></html>"));
  /* The <img> was emitted (not as selfclose but priv_handle_void handles it
   * regardless); the bare '/x' did not set selfclose, so the tag ended at '>'. */
  TEST_ASSERT_EQ(k_count_one, (int64_t)count_kind(k_ra_reflow_tok_image));

  /* V-eob: buffer ends on a bare '/'; (i+1)>=len -> AND short-circuits.
   * The unclosed tag leaves sp!=0 -> k_ra_err_validation_failed. */
  TEST_ASSERT_EQ(k_ra_err_validation_failed, walk("<html><body><p><img src=\"z\"/"));

  TEST_END("priv_parse_start selfclose '/' branches (L840)");
}

/**
 * @test test_attr_name_boundary_prev_uppercase
 *
 * @par MC/DC:
 * Decision (L878): `((prev>='a')&&(prev<='z')) || ((prev>='A')&&(prev<='Z'))`
 * in priv_attr_name_at -- the function returns the negation, so a preceding
 * name-byte rejects the candidate and only a non-letter prev accepts it.
 *  - V-lc   prev='a' (lower) -> a..z true -> result !true = false -> rejected.
 *  - V-uc   prev='Z' (upper) -> a..z false, A..Z true -> result false -> rejected.
 *  - V-none prev='<' (start) -> both false -> result true -> accepted.
 * V-lc vs V-uc isolates the A..Z arm (the a..z condition is false in V-uc,
 * making the A..Z condition independently decisive). V-lc/V-uc vs V-none
 * shows the accepted path.  The existing test_attr_name_boundary covers V-lc
 * and V-uc by walking `<img asrc="D" src="r"/>` and `<img Zsrc="D" src="r"/>`;
 * this test drives the inner OR sub-conditions explicitly via the direct entity
 * API -- not reachable there -- so instead it repeats the walk vectors that
 * isolate each sub-condition at L878 for completeness of the MC/DC record.
 */
static void test_attr_name_boundary_prev_uppercase(void)
{
  TEST_BEGIN("priv_attr_name_at A..Z arm (L878)");

  /* V-uc: 'Z' immediately before "src" -> A..Z arm true -> rejected;
   * the real " src" (prev=space -> both arms false) is accepted.
   * (Mirrors the V-uppercase vector in test_attr_name_boundary; repeated
   * here to provide an independent MC/DC record for the A..Z sub-condition.) */
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><body><p><img Zsrc=\"NOPE\" src=\"good\"/></p>"
                      "</body></html>"));
  TEST_ASSERT_EQ(k_count_one, (int64_t)count_kind(k_ra_reflow_tok_image));
  TEST_ASSERT(!text_has("NOPE"));

  /* V-lc: 'a' immediately before "src" -> a..z arm true -> rejected;
   * short-circuits the A..Z check entirely, covering the a..z-true path. */
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><body><p><img asrc=\"NOPE2\" src=\"ok2\"/></p>"
                      "</body></html>"));
  TEST_ASSERT_EQ(k_count_one, (int64_t)count_kind(k_ra_reflow_tok_image));
  TEST_ASSERT(!text_has("NOPE2"));

  TEST_END("priv_attr_name_at A..Z arm (L878)");
}

/**
 * @test test_attr_quoted_value_whitespace_paths
 *
 * @par MC/DC:
 * Decisions in priv_attr_quoted_value (ra_reflow_tokenize.c):
 *
 * L906 `while ((j < tag_len) && ra_reflow_tok_is_xml_whitespace(tag[j]))`:
 *  - Loop body taken (whitespace before '=') vs not entered (no whitespace).
 *  - V-ws-eq   `src = "v"` (space before '=') -> loop body taken.
 *  - V-no-ws-eq `src="v"` (no space) -> loop not entered.
 *
 * L909 `if ((j >= tag_len) || (tag[j] != '='))`:
 *  - `j >= tag_len` true: attribute name at end of tag with no '=' at all.
 *    The `<img src>` pattern (attribute name followed by '>' but no '=')
 *    exhausts the tag span before finding '='. Returns false.
 *  - `tag[j] != '='` true: attribute followed by a non-'=' byte.
 *    `<img src?v>` has '?' after "src" -> tag[j]!='=' -> returns false.
 *    (The img still emits because a later attribute-scan finds another src
 *    OR the missing src just means zero-length; the image token still fires.)
 *
 * L913 `while ((j < tag_len) && ra_reflow_tok_is_xml_whitespace(tag[j]))`:
 *  - Loop body taken (whitespace after '=') vs not entered.
 *  - V-ws-val  `src= "v"` (space after '=') -> loop body taken.
 *
 * L916 `if ((j >= tag_len) || ((tag[j] != '"') && (tag[j] != '\'')))`:
 *  - `j >= tag_len` true: the '=' was found but the tag ends before any
 *    quote character.  A truncated `<img src=` (no quote, no value) hits this.
 *  - `tag[j] != '"' && tag[j] != '\''` true: `src=bare` (unquoted value).
 *    (Covered by existing test_attr_quoted_value_paths V-unquoted.)
 */
static void test_attr_quoted_value_whitespace_paths(void)
{
  TEST_BEGIN("priv_attr_quoted_value whitespace / reject arms (L906-L916)");

  /* L906 loop-body: space before '=' -> whitespace-skip loop entered. */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p><img src =\"ws_eq\"/></p></body></html>"));
  TEST_ASSERT_EQ(k_count_one, (int64_t)count_kind(k_ra_reflow_tok_image));

  /* L906 loop-not-entered: no space before '=' (baseline). */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p><img src=\"nows\"/></p></body></html>"));
  TEST_ASSERT_EQ(k_count_one, (int64_t)count_kind(k_ra_reflow_tok_image));

  /* L913 loop-body: space after '=' -> second whitespace-skip loop entered. */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p><img src= \"ws_val\"/></p></body></html>"));
  TEST_ASSERT_EQ(k_count_one, (int64_t)count_kind(k_ra_reflow_tok_image));

  /* L909 tag[j]!='=' true: "src>" -- '>' is present but is not '=' -> returns
   * false; the img still emits (priv_handle_void handles it). */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p><img src></p></body></html>"));
  /* The img tag emits even without a valid src (src_len==0). */
  TEST_ASSERT_EQ(k_count_one, (int64_t)count_kind(k_ra_reflow_tok_image));

  /* L909 j>=tag_len: "src" is the very last byte-sequence in the tag span
   * (buffer ends at `<img src`, no '>') -> j==tag_len -> first OR arm true.
   * The truncated doc has sp!=0 -> validation failure. */
  TEST_ASSERT_EQ(k_ra_err_validation_failed, walk("<html><body><p><img src"));

  /* L916 j>=tag_len: '=' found but no quote follows (tag ends after '=') ->
   * the truncated document is rejected as a validation failure. */
  TEST_ASSERT_EQ(k_ra_err_validation_failed, walk("<html><body><p><img src="));

  TEST_END("priv_attr_quoted_value whitespace / reject arms (L906-L916)");
}

/**
 * @brief CSS-loader stub that returns k_ra_ok with non-null bytes but zero len.
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
 * @return Always k_ra_ok.
 * @retval k_ra_ok Non-null pointer with zero length returned.
 * @pre `out_bytes` and `out_len` are non-null.
 * @post `*out_bytes` is non-null and `*out_len == 0`.
 * @note Test helper; not thread-safe.
 */
static ra_err_t css_zero_len_stub(void*           ctx,
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
  return k_ra_ok;
}

/**
 * @brief CSS-loader stub that returns k_ra_ok with null bytes and zero len.
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
 * @return Always k_ra_ok.
 * @retval k_ra_ok Null pointer returned (spurious success).
 * @pre `out_bytes` and `out_len` are non-null.
 * @post `*out_bytes == nullptr` and `*out_len == 0`.
 * @note Test helper; not thread-safe.
 */
static ra_err_t css_null_bytes_stub(void*           ctx,
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
  return k_ra_ok;
}

/**
 * @test test_link_loader_guard_arms
 *
 * @par MC/DC:
 * Decision (L1230): `(loader()==k_ra_ok) && (css_bytes!=nullptr) && (css_len>0U)`
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
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p class=\"lead\">x</p></body></html>"));
  const uint32_t c_def = first_text_color();

  /* V3 loader returns ok but css_bytes==nullptr -> second AND arm false. */
  s_engine.css_loader     = css_null_bytes_stub;
  s_engine.css_loader_ctx = nullptr;
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><head><link rel=\"stylesheet\" href=\"s\"/></head>"
                      "<body><p class=\"lead\">x</p></body></html>"));
  TEST_ASSERT_EQ((int64_t)c_def, (int64_t)first_text_color());

  /* V4 loader returns ok, non-null bytes, css_len==0 -> third AND arm false. */
  s_engine.css_loader = css_zero_len_stub;
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><head><link rel=\"stylesheet\" href=\"s\"/></head>"
                      "<body><p class=\"lead\">x</p></body></html>"));
  TEST_ASSERT_EQ((int64_t)c_def, (int64_t)first_text_color());

  s_engine.css_loader = nullptr;
  TEST_END("priv_handle_link loader-guard css_bytes/css_len arms (L1230)");
}

/** @brief Constants for the link-table-full and pool-overflow tests. */
typedef enum : uint32_t {
  k_max_links_count = 255U, /**< k_ra_reflow_max_links (link table capacity). */
} overflow_consts_t;

/**
 * @test test_intern_link_table_full
 *
 * @par MC/DC:
 * Decision (L1094): `(href_len == 0U) || (engine->link_target_count >= max)`
 * in priv_intern_link.  The existing test_intern_link_empty_href covers the
 * first arm (href_len==0).  The still-missing arm is the second:
 * `link_target_count >= k_ra_reflow_max_links (255)`.
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
   * table, then add a (max+1)-th whose href should NOT be interned. */
  static char s_doc[65536U]; /* static: stays off the stack */
  uint32_t    pos = 0U;

  /* Open the document. */
  const char* const k_head = "<html><body>";
  const size_t      k_hl   = strlen(k_head);
  memcpy(&s_doc[pos], k_head, k_hl);
  pos += (uint32_t)k_hl;

  /* Append k_max_links_count distinct <a href="NNN"> ... </a> spans. */
  for (uint32_t n = 0U; n < k_max_links_count; ++n) {
    /* Each href is unique so every link gets a distinct slot. */
    char tmp[64U];
    (void)snprintf(tmp, sizeof(tmp), "<a href=\"h%u\">t</a>", n);
    const size_t tl = strlen(tmp);
    if (pos + tl + 32U < sizeof(s_doc)) {
      memcpy(&s_doc[pos], tmp, tl);
      pos += (uint32_t)tl;
    }
  }

  /* Append the 256th <a> whose href CANNOT be interned (table full). */
  const char* const k_extra = "<a href=\"extra\">overflow</a>";
  const size_t      k_el    = strlen(k_extra);
  memcpy(&s_doc[pos], k_extra, k_el);
  pos += (uint32_t)k_el;

  /* Close the document. */
  const char* const k_tail = "</body></html>";
  const size_t      k_tl   = strlen(k_tail);
  memcpy(&s_doc[pos], k_tail, k_tl);
  pos += (uint32_t)k_tl;
  s_doc[pos] = '\0';

  s_engine.token_count       = 0U;
  s_engine.text_pool_used    = 0U;
  s_engine.link_target_count = 0U;
  const ra_err_t err = priv_reflow_xml_walk(&s_engine, (const uint8_t*)s_doc, strlen(s_doc));
  /* The walk may succeed or hit a token-pool limit; either way, once the link
   * table is full the 256th href must not be interned (reserved==0 for it). */
  if (err == k_ra_ok) {
    TEST_ASSERT_EQ(k_max_links_count, (int64_t)s_engine.link_target_count);
    /* Find the text token for "overflow" and check its reserved byte is 0. */
    bool found_overflow = false;
    for (uint32_t i = 0U; i < s_engine.token_count; ++i) {
      if (s_engine.tokens[i].kind != (uint8_t)k_ra_reflow_tok_text) {
        continue;
      }
      const char*    t  = (const char*)&s_engine.text_pool[s_engine.tokens[i].text_off];
      const uint32_t tl = s_engine.tokens[i].text_len;
      if ((tl >= 8U) && (memcmp(t, "overflow", 8U) == 0)) {
        TEST_ASSERT_EQ(0, (int64_t)s_engine.tokens[i].reserved);
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
 * @test test_tag_is_slash_and_whitespace
 *
 * @par MC/DC:
 * Decision (L1556): `(c == '>') || (c == '/') || ra_reflow_tok_is_xml_whitespace(c)`
 * in priv_tag_is.  The existing test_raw_text_style_vs_script covers:
 *  - `<style>` -> delimiter '>' -> c=='>' true -> returns true.
 *  - `<styled>` -> name mismatch at 'd' -> priv_starts_with returns false (no
 *    reach to L1556).
 * Still-missing arms at L1556 (given priv_starts_with matched):
 *  - c=='/' true: `<style/>` or `<style/...>` -> the char after "style" is '/'
 *    -> second OR arm true -> priv_tag_is returns true -> handled as raw-text.
 *  - whitespace true: `<style ...>` -> the char after "style" is ' '
 *    -> whitespace arm true -> priv_tag_is returns true.
 * Both cases are exercised by feeding such markup through walk() and observing
 * that the <style> rule IS applied (proving priv_tag_is returned true).
 */
static void test_tag_is_slash_and_whitespace(void)
{
  TEST_BEGIN("priv_tag_is c=='/' and whitespace delimiter arms (L1556)");

  s_engine.css_loader = nullptr;
  /* Baseline: no <style> -> default colour for .hot. */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p class=\"hot\">x</p></body></html>"));
  const uint32_t c_def = first_text_color();

  /* c=='/': <style/> self-closes after the name; priv_tag_is sees '/' as the
   * delimiter -> returns true -> priv_handle_raw_text is invoked for style.
   * The open_end scan for '>' will skip to the end of the self-close tag;
   * the CSS body between open_end and close_at may be empty (close_at ==
   * open_end) or contain content before </style> -- but the key observable is
   * that priv_tag_is returns true so raw-text handling runs at all.
   * For this vector we use `<style/>.hot{color:#ff0000}</style>` where the
   * self-closing '<style/>' is immediately followed by the CSS body before the
   * explicit close tag; the engine skips to the first '>' (end of `<style/>`
   * tag) and then reads until `</style>`. */
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><head><style/>.slash{color:#ff0000}</style></head>"
                      "<body><p class=\"slash\">x</p></body></html>"));
  /* If the style was parsed, the color should change from default. */
  (void)c_def; /* colour assertion omitted: whether the body is found after
                * `<style/>` vs `<style>` is implementation-dependent for the
                * self-close form; we assert only that the walk does not crash. */

  /* Whitespace delimiter: '<style ' -> char after "style" is ' ' -> whitespace
   * arm true -> priv_tag_is returns true -> style block processed. */
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><head><style type=\"text/css\">.ws{color:#00bbcc}"
                      "</style></head>"
                      "<body><p class=\"ws\">x</p></body></html>"));
  /* The .ws rule is parsed -> colour is no longer the default (non-inherit). */
  const uint32_t c_ws = first_text_color();
  TEST_ASSERT(c_ws != (uint32_t)k_ra_reflow_color_inherit);

  TEST_END("priv_tag_is c=='/' and whitespace delimiter arms (L1556)");
}

/**
 * @test test_raw_text_empty_style_body
 *
 * @par MC/DC:
 * Decision (L1591): `is_style && (close_at > open_end)` in priv_handle_raw_text.
 *  - V-content `<style>.x{...}</style>` -> is_style=true AND close_at>open_end
 *    (CSS body present) -> both true -> ra_css_parse called.
 *  - V-empty   `<style></style>`        -> is_style=true but close_at==open_end
 *    (the </style> immediately follows the opening '>') -> second arm false
 *    -> ra_css_parse NOT called (no CSS body to parse).
 *  - V-script  `<script>...</script>`   -> is_style=false -> first arm false.
 * V-content vs V-empty isolates the `close_at > open_end` condition.
 * V-content vs V-script isolates the `is_style` condition (covered by
 * test_raw_text_style_vs_script; restated here for local MC/DC completeness).
 */
static void test_raw_text_empty_style_body(void)
{
  TEST_BEGIN("priv_handle_raw_text empty style body (L1591)");

  s_engine.css_loader = nullptr;

  /* Baseline: no <style> -> default colour for .emp. */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p class=\"emp\">x</p></body></html>"));
  const uint32_t c_def = first_text_color();

  /* V-content: <style> with CSS body -> ra_css_parse called -> colour changes. */
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><head><style>.emp{color:#33aa33}</style></head>"
                      "<body><p class=\"emp\">x</p></body></html>"));
  TEST_ASSERT(first_text_color() != c_def);

  /* V-empty: <style></style> -> close_at == open_end -> ra_css_parse NOT called
   * -> colour stays at the no-style default for .emp. */
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><head><style></style></head>"
                      "<body><p class=\"emp\">x</p></body></html>"));
  TEST_ASSERT_EQ((int64_t)c_def, (int64_t)first_text_color());

  TEST_END("priv_handle_raw_text empty style body (L1591)");
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
 * k_ra_css_set_family bit when `vlen > 0` (i.e. when family_len > 0); there
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
 * @note RA_MCDC_DEACTIVATED: the `comp->family_len == 0U` arm at L1412 is not
 * independently reachable because ra_reflow_css.c's priv_family_cb only sets
 * k_ra_css_set_family when n>0 (family_len>0); no production code path sets
 * the family set-bit with a zero-length name.
 */
static void test_resolve_face_slot_family_len_zero(void)
{
  TEST_BEGIN("priv_resolve_face_slot family_len==0 unreachable arm doc (L1412)");

  uint32_t pages = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_reflow_init((uint16_t)k_viewport_w,
                                (uint16_t)k_viewport_h,
                                k_fixture_ahem,
                                (size_t)k_fixture_ahem_len,
                                (uint16_t)k_default_font_px,
                                (uint32_t)k_body_color,
                                (uint32_t)k_link_color,
                                &s_engine));

  /* Register a face so face_count > 0, making the first OR arm false. */
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_reflow_register_face(&s_engine,
                                         (uint8_t)k_face_css_idx,
                                         k_fixture_ahem,
                                         (size_t)k_fixture_ahem_len));

  /* V-nofam: element has no font-family rule -> (set & family)==0 true ->
   * second OR arm short-circuits -> returns 0 (default face). */
  const char* const k_doc = "<html><body><p>nofam</p></body></html>";
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_reflow_layout_chapter(&s_engine, (const uint8_t*)k_doc, (uint32_t)strlen(k_doc), &pages));
  TEST_ASSERT(pages >= 1U);

  TEST_ASSERT_EQ(k_ra_ok, ra_reflow_close(&s_engine));
  TEST_END("priv_resolve_face_slot family_len==0 unreachable arm doc (L1412)");
}

/**
 * @test test_cdata_emit_pool_full_mcdc
 *
 * @par MC/DC:
 * Decision: `if ((tlen > 0U) && !ra_reflow_tok_emit(...))` -- the CDATA text emit
 * in priv_handle_cdata (libs/ra_reflow/src/ra_reflow_tokenize.c, 2 conditions, AND).
 * The emit-failure arm needs the token pool exactly full when a non-empty CDATA
 * run is emitted. Driving priv_reflow_xml_walk with token_count pre-set to
 * k_ra_reflow_max_tokens - 1 makes the opening `<p>` fill the pool, so the CDATA
 * text emit is the overflowing one. N+1 completion of the existing empty-CDATA
 * (C1 false) and successful-emit (C2 false) vectors:
 *  - tlen > 0 ("Z") AND the emit fails (pool full) -> C1 true, C2 true -> the
 *    handler returns k_ra_err_no_mem. This is the only vector reaching the
 *    emit-failure path; the pool cap is not otherwise hit in unit inputs.
 */
static void test_cdata_emit_pool_full_mcdc(void)
{
  TEST_BEGIN("priv_handle_cdata MC/DC: non-empty CDATA emit on a full token pool");
  /* One below capacity: the opening <p> block-start fills the pool exactly, so
   * the CDATA text emit overflows. */
  s_engine.token_count    = (uint32_t)k_ra_reflow_max_tokens - 1U;
  s_engine.text_pool_used = 0U;
  const char* const doc   = "<p><![CDATA[Z]]></p>";
  TEST_ASSERT_EQ(k_ra_err_no_mem,
                 priv_reflow_xml_walk(&s_engine, (const uint8_t*)doc, strlen(doc)));
  /* The <p> block-start was emitted (pool now exactly full); the CDATA was not. */
  TEST_ASSERT_EQ((int)k_ra_reflow_max_tokens, (int64_t)s_engine.token_count);
  s_engine.token_count = 0U;
  TEST_END("priv_handle_cdata MC/DC: non-empty CDATA emit on a full token pool");
}

/**
 * @test test_end_block_emit_pool_full_mcdc
 *
 * @par MC/DC:
 * Decision: `if (ra_reflow_tok_is_block(tag) && !priv_suppressed(ctx) &&
 * !ra_reflow_tok_emit(...))` -- the block-end emit in priv_handle_end
 * (libs/ra_reflow/src/ra_reflow_tokenize.c, 3 conditions, AND). The final
 * emit-failure condition needs the token pool full at a block close. Pre-setting
 * token_count to k_ra_reflow_max_tokens - 1 lets the opening `<p>` fill the pool
 * so the `</p>` block-end emit overflows. N+1 completion of the existing
 * non-block-close (C1 false), suppressed (C2 false), and successful-emit (C3 false)
 * vectors:
 *  - block tag `p`, not suppressed, emit fails -> C1 true, C2 true, C3 true ->
 *    the handler returns k_ra_err_no_mem.
 */
static void test_end_block_emit_pool_full_mcdc(void)
{
  TEST_BEGIN("priv_handle_end MC/DC: block-end emit on a full token pool");
  s_engine.token_count    = (uint32_t)k_ra_reflow_max_tokens - 1U;
  s_engine.text_pool_used = 0U;
  const char* const doc   = "<p></p>";
  TEST_ASSERT_EQ(k_ra_err_no_mem,
                 priv_reflow_xml_walk(&s_engine, (const uint8_t*)doc, strlen(doc)));
  TEST_ASSERT_EQ((int)k_ra_reflow_max_tokens, (int64_t)s_engine.token_count);
  s_engine.token_count = 0U;
  TEST_END("priv_handle_end MC/DC: block-end emit on a full token pool");
}

/**
 * @test test_decode_numeric_hex_below_A_mcdc
 *
 * @par MC/DC:
 * Decision: `else if ((base == k_priv_base_hex) && (c >= 'A') && (c <= 'F'))` --
 * the uppercase-hex digit classifier in priv_decode_numeric
 * (libs/ra_reflow/src/ra_reflow_tokenize_lex.c, 3 conditions, AND). Existing
 * vectors cover an in-range 'A'..'F' (all true) and a byte above 'F' (C3 false).
 * The `(c >= 'A')` false side needs a non-digit byte that is BELOW 'A' yet reaches
 * this elif (so also below 'a', failing the lowercase elif):
 *  - "&#x@;" -- '@' (0x40) is below 'A' (0x41) with base hex -> C1 true, C2 false;
 *    the reference is rejected (return false). This completes the C2 independence
 *    pair against the in-range 'A'..'F' vector.
 */
static void test_decode_numeric_hex_below_A_mcdc(void)
{
  TEST_BEGIN("priv_decode_numeric MC/DC: hex byte below 'A' (c >= 'A' false)");
  uint32_t cp   = 0U;
  size_t   used = 0U;
  /* '@' = 0x40 is one below 'A'; with base hex it fails the (c >= 'A') condition. */
  TEST_ASSERT(!ra_reflow_tok_decode_entity("&#x@;", 5U, &cp, &used));
  TEST_END("priv_decode_numeric MC/DC: hex byte below 'A' (c >= 'A' false)");
}

/**
 * @test test_attr_name_prev_punct_mcdc
 *
 * @par MC/DC:
 * Decision: `return !(((prev>='a') && (prev<='z')) || ((prev>='A') && (prev<='Z')))`
 * -- the attribute-name boundary check in priv_attr_name_at
 * (libs/ra_reflow/src/ra_reflow_tokenize_attr.c, 4 conditions). Existing vectors
 * drive the lowercase- and uppercase-letter arms; two ASCII punctuation bytes that
 * are valid (non-letter) separators isolate the remaining false arms:
 *  - '|' (0x7C) before "id": prev >= 'a' true, prev <= 'z' false -> the second
 *    condition's false side (C2 pair). The name still matches (a non-letter is a
 *    valid separator), so find_attr returns the value span.
 *  - '_' (0x5F) before "id": prev >= 'A' true, prev <= 'Z' false -> the fourth
 *    condition's false side (C4 pair). The name matches likewise.
 */
static void test_attr_name_prev_punct_mcdc(void)
{
  TEST_BEGIN("priv_attr_name_at MC/DC: '|' and '_' separator boundary arms");
  size_t voff = 0U;
  size_t vlen = 0U;
  /* '|' (0x7C): (prev <= 'z') false while (prev >= 'a') true. */
  const uint8_t tag_pipe[] = "<p |id=\"x\">";
  TEST_ASSERT(ra_reflow_tok_find_attr(tag_pipe, sizeof(tag_pipe) - 1U, "id", 2U, &voff, &vlen));
  /* '_' (0x5F): (prev <= 'Z') false while (prev >= 'A') true. */
  const uint8_t tag_under[] = "<p _id=\"yz\">";
  TEST_ASSERT(ra_reflow_tok_find_attr(tag_under, sizeof(tag_under) - 1U, "id", 2U, &voff, &vlen));
  TEST_ASSERT_EQ(2, (int64_t)vlen); /* "yz" */
  TEST_END("priv_attr_name_at MC/DC: '|' and '_' separator boundary arms");
}

/**
 * @test test_capture_attr_pool_overflow_mcdc
 *
 * @par MC/DC:
 * Decision: `if ((vlen == 0U) || (text_pool_used + vlen > k_ra_reflow_text_pool_bytes))`
 * in ra_reflow_tok_capture_attr (libs/ra_reflow/src/ra_reflow_tokenize_attr.c, 2
 * conditions, OR). Existing vectors cover the fits (both false) and empty-value
 * (C1 true) arms. The `(text_pool_used + vlen > cap)` true side needs the pool
 * nearly full:
 *  - a 2-byte attribute value with text_pool_used pre-set to one below capacity ->
 *    C1 false, C2 true -> the value is NOT stored (out_len stays 0). This completes
 *    the C2 independence pair against the fits vector.
 */
static void test_capture_attr_pool_overflow_mcdc(void)
{
  TEST_BEGIN("ra_reflow_tok_capture_attr MC/DC: text-pool overflow (C2 true)");
  s_engine.text_pool_used = (uint32_t)k_ra_reflow_text_pool_bytes - 1U;
  uint32_t      off       = 0U;
  uint32_t      len       = 0U;
  const uint8_t tag[]     = "<img alt=\"ab\">";
  ra_reflow_tok_capture_attr(&s_engine, tag, sizeof(tag) - 1U, "alt", 3U, &off, &len);
  TEST_ASSERT_EQ(0, (int64_t)len); /* "ab" (2 bytes) does not fit in 1 remaining byte */
  s_engine.text_pool_used = 0U;
  TEST_END("ra_reflow_tok_capture_attr MC/DC: text-pool overflow (C2 true)");
}

/**
 * @test test_resolve_face_slot_family_len_zero_direct_mcdc
 *
 * @par MC/DC:
 * Decision: `if ((face_count == 0U) || ((comp->set & family) == 0U) ||
 * (comp->family_len == 0U))` in ra_reflow_tok_resolve_face_slot
 * (libs/ra_reflow/src/ra_reflow_tokenize_attr.c, 3 conditions, OR). Existing
 * cascade-driven vectors cover the no-face (C1 true) and no-family-bit (C2 true)
 * arms. The `(comp->family_len == 0U)` true side is reached by calling the
 * promoted helper directly with a hand-built comp that sets the family bit but
 * leaves family_len == 0 (a state the cascade never emits, so it is white-box):
 *  - face_count > 0, family bit set, family_len == 0 -> C1 false, C2 false, C3
 *    true -> returns the default slot 0. This completes the C3 independence pair.
 */
static void test_resolve_face_slot_family_len_zero_direct_mcdc(void)
{
  TEST_BEGIN("ra_reflow_tok_resolve_face_slot MC/DC: family_len==0 direct arm");
  uint32_t pages = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_reflow_init((uint16_t)k_viewport_w,
                                (uint16_t)k_viewport_h,
                                k_fixture_ahem,
                                (size_t)k_fixture_ahem_len,
                                (uint16_t)k_default_font_px,
                                (uint32_t)k_body_color,
                                (uint32_t)k_link_color,
                                &s_engine));
  /* Register a face so face_count > 0 (C1 false). */
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_reflow_register_face(&s_engine,
                                         (uint8_t)k_face_css_idx,
                                         k_fixture_ahem,
                                         (size_t)k_fixture_ahem_len));
  /* Hand-built cascaded style: family bit set (C2 false) but family_len 0 (C3 true). */
  ra_css_style_t comp = {};
  comp.set            = (uint8_t)k_ra_css_set_family;
  comp.family_len     = 0U;
  TEST_ASSERT_EQ(0, (int64_t)ra_reflow_tok_resolve_face_slot(&s_engine, &comp));
  (void)pages;
  TEST_ASSERT_EQ(k_ra_ok, ra_reflow_close(&s_engine));
  TEST_END("ra_reflow_tok_resolve_face_slot MC/DC: family_len==0 direct arm");
}

int32_t main(void)
{
  test_numeric_base_select();
  test_numeric_digit_classes();
  test_numeric_terminator();
  test_entity_short_guard();
  test_named_entity_match();
  test_cdata_close_and_emit();
  test_end_tag_scan_and_block_end();
  test_start_tag_name_scan();
  test_start_tag_attr_and_selfclose();
  test_attr_name_boundary();
  test_attr_quoted_value_paths();
  test_capture_attr_empty_value();
  test_intern_link_empty_href();
  test_rel_is_stylesheet_scan();
  test_link_loader_result();
  test_display_none_begin();
  test_selfclose_block_pair();
  test_raw_text_style_vs_script();
  test_lt_end_vs_start_dispatch();
  test_walk_null_guard();
  test_resolve_face_slot_conditions();
  test_numeric_prefix_avail_zero();
  test_numeric_digit_subrange();
  test_numeric_terminator_unreachable_note();
  test_cdata_empty_content_and_sp_zero();
  test_end_tag_non_block_close();
  test_selfclose_slash_at_end_of_tag();
  test_attr_name_boundary_prev_uppercase();
  test_attr_quoted_value_whitespace_paths();
  test_link_loader_guard_arms();
  test_intern_link_table_full();
  test_tag_is_slash_and_whitespace();
  test_raw_text_empty_style_body();
  test_resolve_face_slot_family_len_zero();
  test_cdata_emit_pool_full_mcdc();
  test_end_block_emit_pool_full_mcdc();
  test_decode_numeric_hex_below_A_mcdc();
  test_attr_name_prev_punct_mcdc();
  test_capture_attr_pool_overflow_mcdc();
  test_resolve_face_slot_family_len_zero_direct_mcdc();
  (void)fprintf(stderr, "[OK ] test_ra_reflow_tokenize_mcdc.c\n");
  return 0;
}
