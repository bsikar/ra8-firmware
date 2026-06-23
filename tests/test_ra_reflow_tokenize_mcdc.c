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
  k_body_color      = 0xFFFFFFU, /**< Body colour for the layout engine.    */
  k_link_color      = 0x3060FFU, /**< Link colour for the layout engine.    */
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
  TEST_ASSERT(!ra_reflow_tok_decode_entity("&#x1g;", 6U, &cp, &used)); /* V3 bad hex digit */
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
  TEST_ASSERT(!ra_reflow_tok_decode_entity("&#;", 3U, &cp, &used));  /* V2 no digits */
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
  TEST_ASSERT(!ra_reflow_tok_decode_entity(nul_at_1, 5U, &cp, &used)); /* V3 src[1]==0 */
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
  TEST_ASSERT(!ra_reflow_tok_decode_entity("&ampX", 5U, &cp, &used)); /* V2 no ';' */
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
  (void)fprintf(stderr, "[OK ] test_ra_reflow_tokenize_mcdc.c\n");
  return 0;
}
