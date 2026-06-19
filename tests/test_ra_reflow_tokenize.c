/**
 * @file test_ra_reflow_tokenize.c
 * @brief Unit tests for libs/ra_reflow/src/ra_reflow_tokenize.c
 *
 * @details
 * Drives the no-heap XHTML tokenizer's exposed helpers
 * (`ra_reflow_tok_is_xml_whitespace`, `ra_reflow_tok_classify`,
 * `ra_reflow_tok_decode_entity`, `ra_reflow_tok_utf8_encode`) and the
 * end-to-end `priv_reflow_xml_walk` over inputs that exercise every
 * markup handler. The tokenizer replaced the tinyxml2 DOM shim; its
 * output was verified byte-for-byte equivalent to that shim across a
 * corpus before the swap (see issue #82).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra_reflow.h"
#include "ra_reflow_tokenize_internal.h"
#include "unity_minimal.h"

/* Forward decl of the tokenizer entry point (defined in the production TU). */
ra_err_t priv_reflow_xml_walk(ra_reflow_t* engine, const uint8_t* xhtml_buf, size_t xhtml_len);

static ra_reflow_t s_engine; /* large; keep off the stack */

static ra_err_t walk(const char* xhtml)
{
  s_engine.token_count    = 0U;
  s_engine.text_pool_used = 0U;
  return priv_reflow_xml_walk(&s_engine, (const uint8_t*)xhtml, strlen(xhtml));
}

/**
 * @test test_is_xml_whitespace
 *
 * @par MC/DC:
 * Decision: the 6-term OR in
 * libs/ra_reflow/src/ra_reflow_tokenize.c@ra_reflow_tok_is_xml_whitespace.
 *  - Each whitespace byte (space/tab/nl/cr/ff/vtab) drives exactly one
 *    term true with the rest false -> decision true (independence per
 *    term). A non-whitespace byte leaves all terms false -> decision
 *    false. N+1 vectors for the 6-condition OR.
 */
static void test_is_xml_whitespace(void)
{
  TEST_BEGIN("ra_reflow_tok_is_xml_whitespace");
  const char ws[] = {' ', '\t', '\n', '\r', '\f', '\v'};
  for (size_t i = 0U; i < sizeof(ws); ++i) {
    TEST_ASSERT(ra_reflow_tok_is_xml_whitespace(ws[i]));
  }
  TEST_ASSERT(!ra_reflow_tok_is_xml_whitespace('a'));
  TEST_ASSERT(!ra_reflow_tok_is_xml_whitespace('\0'));
  TEST_END("ra_reflow_tok_is_xml_whitespace");
}

/**
 * @test test_classify
 *
 * @par MC/DC:
 * Decisions in the local-name lowercase copy
 * libs/ra_reflow/src/ra_reflow_tokenize.c@priv_local_lower.
 *  - loop guard `(k < len) && (n + 1 < cap)`: V-both-true copies a char;
 *    V-len-exhausted and V-cap-exhausted each terminate (long names are
 *    truncated, exercised via the >15-char input).
 *  - upper-case fold `(c >= 'A') && (c <= 'Z')`: V-both-true folds
 *    "STRONG"; V-false leaves "p" unchanged; the namespace prefix path is
 *    driven by "html:p".
 */
static void test_classify(void)
{
  TEST_BEGIN("ra_reflow_tok_classify");
  TEST_ASSERT_EQ(k_ra_reflow_tag_p, ra_reflow_tok_classify("p", 1U));
  TEST_ASSERT_EQ(k_ra_reflow_tag_p, ra_reflow_tok_classify("P", 1U));
  TEST_ASSERT_EQ(k_ra_reflow_tag_strong, ra_reflow_tok_classify("STRONG", 6U));
  TEST_ASSERT_EQ(k_ra_reflow_tag_p, ra_reflow_tok_classify("html:p", 6U));
  TEST_ASSERT_EQ(k_ra_reflow_tag_br, ra_reflow_tok_classify("br", 2U));
  TEST_ASSERT_EQ(k_ra_reflow_tag_unknown, ra_reflow_tok_classify("div", 3U));
  TEST_ASSERT_EQ(k_ra_reflow_tag_unknown,
                 ra_reflow_tok_classify("averylongtagnamethatistruncated", 31U));
  TEST_ASSERT_EQ(k_ra_reflow_tag_unknown, ra_reflow_tok_classify(nullptr, 0U));
  TEST_END("ra_reflow_tok_classify");
}

/**
 * @test test_decode_entity
 *
 * @par MC/DC:
 * Decisions in
 * libs/ra_reflow/src/ra_reflow_tokenize.c@ra_reflow_tok_decode_entity and
 * libs/ra_reflow/src/ra_reflow_tokenize.c@priv_decode_numeric.
 *  - decode_entity guard `(window < 4) || (src[1] == 0)`: V-T short input
 *    rejects; V-F normal input proceeds.
 *  - named match `((wlen+2) <= window) && (src[1+wlen] == ';') &&
 *    strncmp==0`: V-all-true matches "&amp;"; each false arm (window too
 *    small, missing ';', name mismatch) rejects.
 *  - numeric base select `(src[i]=='x') || (src[i]=='X')`: hex vs dec.
 *  - numeric digit class chain and terminator `(digits==0) || (i>=avail)
 *    || (src[i]!=';')`: valid "&#65;"/"&#x41;" accept; bad digit / no ';'
 *    reject.
 */
static void test_decode_entity(void)
{
  TEST_BEGIN("ra_reflow_tok_decode_entity");
  uint32_t cp   = 0U;
  size_t   used = 0U;
  TEST_ASSERT(ra_reflow_tok_decode_entity("&amp;", 5U, &cp, &used));
  TEST_ASSERT_EQ('&', cp);
  TEST_ASSERT_EQ(5, used);
  TEST_ASSERT(ra_reflow_tok_decode_entity("&lt;x", 5U, &cp, &used));
  TEST_ASSERT_EQ('<', cp);
  TEST_ASSERT_EQ(4, used);
  TEST_ASSERT(ra_reflow_tok_decode_entity("&#65;", 5U, &cp, &used));
  TEST_ASSERT_EQ(65, cp);
  TEST_ASSERT(ra_reflow_tok_decode_entity("&#x41;", 6U, &cp, &used));
  TEST_ASSERT_EQ(0x41, cp);
  TEST_ASSERT_EQ(6, used);
  TEST_ASSERT(!ra_reflow_tok_decode_entity("&zz;", 4U, &cp, &used));  /* unknown name */
  TEST_ASSERT(!ra_reflow_tok_decode_entity("&amp", 4U, &cp, &used));  /* no terminator */
  TEST_ASSERT(!ra_reflow_tok_decode_entity("&#;", 3U, &cp, &used));   /* no digits / short */
  TEST_ASSERT(!ra_reflow_tok_decode_entity("&#9z;", 5U, &cp, &used)); /* bad digit */
  TEST_END("ra_reflow_tok_decode_entity");
}

/**
 * @test test_utf8_encode
 *
 * @par MC/DC:
 * The encoder branches on single range comparisons (no compound boolean):
 * 1/2/3/4-byte forms selected by `cp < 0x80 / 0x800 / 0x10000`. Vectors
 * cover all four width tiers and the out-of-range clamp.
 */
static void test_utf8_encode(void)
{
  TEST_BEGIN("ra_reflow_tok_utf8_encode");
  uint8_t b[4];
  TEST_ASSERT_EQ(1, ra_reflow_tok_utf8_encode(0x41U, b));
  TEST_ASSERT_EQ(0x41, b[0]);
  TEST_ASSERT_EQ(2, ra_reflow_tok_utf8_encode(0xE9U, b)); /* e-acute */
  TEST_ASSERT_EQ(0xC3, b[0]);
  TEST_ASSERT_EQ(0xA9, b[1]);
  TEST_ASSERT_EQ(3, ra_reflow_tok_utf8_encode(0x20ACU, b));   /* euro */
  TEST_ASSERT_EQ(4, ra_reflow_tok_utf8_encode(0x1F600U, b));  /* emoji */
  TEST_ASSERT_EQ(4, ra_reflow_tok_utf8_encode(0xFFFFFFU, b)); /* clamped */
  TEST_END("ra_reflow_tok_utf8_encode");
}

/**
 * @test test_walk_end_to_end
 *
 * @par MC/DC:
 * Drives the markup dispatch chain:
 * libs/ra_reflow/src/ra_reflow_tokenize.c@priv_handle_lt,
 * libs/ra_reflow/src/ra_reflow_tokenize.c@priv_parse_start,
 * libs/ra_reflow/src/ra_reflow_tokenize.c@priv_handle_start,
 * libs/ra_reflow/src/ra_reflow_tokenize.c@priv_handle_end,
 * libs/ra_reflow/src/ra_reflow_tokenize.c@priv_handle_cdata,
 * libs/ra_reflow/src/ra_reflow_tokenize.c@priv_reflow_xml_walk.
 *  - priv_handle_lt prefix tests select comment / CDATA / PI / decl /
 *    end-tag / start-tag arms (each input below hits a distinct arm).
 *  - priv_parse_start name-scan and quoted-attr / self-close compounds:
 *    `<img .../>`, `<br/>`, and `alt="a > b"` exercise both arms.
 *  - priv_handle_start block vs void vs inline vs unknown; priv_handle_end
 *    block-end emission and stack restore; priv_reflow_xml_walk arg-null,
 *    text-run, and unbalanced-stack validation.
 */
static void test_walk_end_to_end(void)
{
  TEST_BEGIN("priv_reflow_xml_walk end-to-end");

  /* Null / empty argument guards. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, priv_reflow_xml_walk(nullptr, (const uint8_t*)"x", 1U));
  TEST_ASSERT_EQ(k_ra_err_invalid_size, priv_reflow_xml_walk(&s_engine, (const uint8_t*)"x", 0U));

  /* No root element -> validation failed. */
  TEST_ASSERT_EQ(k_ra_err_validation_failed, walk("<!-- only a comment -->"));

  /* Block + inline + entity. */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p>Hi <b>bold</b> &amp; end</p></body></html>"));
  /* Tokens: block_start(p), text"Hi ", text"bold"(bold), text" & end" */
  TEST_ASSERT_EQ(k_ra_reflow_tok_block_start, s_engine.tokens[0].kind);
  TEST_ASSERT_EQ(k_ra_reflow_tag_p, s_engine.tokens[0].tag);
  bool saw_bold_text = false;
  bool saw_block_end = false;
  for (uint32_t i = 0U; i < s_engine.token_count; ++i) {
    if ((s_engine.tokens[i].kind == k_ra_reflow_tok_text) &&
        ((s_engine.tokens[i].style & (uint8_t)k_ra_reflow_style_bold) != 0U)) {
      saw_bold_text = true;
    }
    if (s_engine.tokens[i].kind == k_ra_reflow_tok_block_end) {
      saw_block_end = true;
    }
  }
  TEST_ASSERT(saw_bold_text);
  TEST_ASSERT(saw_block_end);

  /* Void + self-close + quoted attribute containing '>'. */
  TEST_ASSERT_EQ(
    k_ra_ok,
    walk("<html><body><p>a<br/>b<img src=\"x\" alt=\"a > b\"/></p><hr/></body></html>"));
  bool saw_break = false;
  bool saw_image = false;
  bool saw_rule  = false;
  for (uint32_t i = 0U; i < s_engine.token_count; ++i) {
    saw_break = saw_break || (s_engine.tokens[i].kind == k_ra_reflow_tok_break);
    saw_image = saw_image || (s_engine.tokens[i].kind == k_ra_reflow_tok_image);
    saw_rule  = saw_rule || (s_engine.tokens[i].kind == k_ra_reflow_tok_rule);
  }
  TEST_ASSERT(saw_break);
  TEST_ASSERT(saw_image);
  TEST_ASSERT(saw_rule);

  /* CDATA inner text is emitted verbatim (no entity decode). */
  TEST_ASSERT_EQ(k_ra_ok, walk("<html><body><p><![CDATA[raw & <x>]]></p></body></html>"));
  bool saw_amp_literal = false;
  for (uint32_t i = 0U; i < s_engine.token_count; ++i) {
    if (s_engine.tokens[i].kind == k_ra_reflow_tok_text) {
      const uint8_t* t = &s_engine.text_pool[s_engine.tokens[i].text_off];
      if (memchr(t, '&', s_engine.tokens[i].text_len) != nullptr) {
        saw_amp_literal = true;
      }
    }
  }
  TEST_ASSERT(saw_amp_literal);

  /* Unbalanced (unclosed) -> validation failed. */
  TEST_ASSERT_EQ(k_ra_err_validation_failed, walk("<html><body><p>oops"));
  /* Stray end tag -> validation failed. */
  TEST_ASSERT_EQ(k_ra_err_validation_failed, walk("</p>"));

  TEST_END("priv_reflow_xml_walk end-to-end");
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

/**
 * @test test_display_none_suppressed
 * @brief `display:none` (#140) drops the element's whole subtree from the stream.
 */
static void test_display_none_suppressed(void)
{
  TEST_BEGIN("display:none suppresses subtree");
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><body><p>visibleone</p>"
                      "<div style=\"display:none\"><p>hiddentext</p><img src=\"x\"/></div>"
                      "<p>visibletwo</p></body></html>"));
  TEST_ASSERT(text_has("visibleone"));  /* before the hidden div */
  TEST_ASSERT(text_has("visibletwo"));  /* after the hidden div  */
  TEST_ASSERT(!text_has("hiddentext")); /* inside -> suppressed   */
  bool saw_img = false;
  for (uint32_t i = 0U; i < s_engine.token_count; ++i) {
    saw_img = saw_img || (s_engine.tokens[i].kind == (uint8_t)k_ra_reflow_tok_image);
  }
  TEST_ASSERT(!saw_img); /* hidden <img> dropped too */

  /* Suppression clears: a visible element after the hidden one still emits. */
  TEST_ASSERT_EQ(k_ra_ok,
                 walk("<html><body><p style=\"display:none\">gone</p>"
                      "<p>stays</p></body></html>"));
  TEST_ASSERT(!text_has("gone"));
  TEST_ASSERT(text_has("stays"));
  TEST_END("display:none suppresses subtree");
}

int32_t main(void)
{
  test_is_xml_whitespace();
  test_classify();
  test_decode_entity();
  test_utf8_encode();
  test_walk_end_to_end();
  test_display_none_suppressed();
  (void)fprintf(stderr, "[OK ] test_ra_reflow_tokenize.c\n");
  return 0;
}
