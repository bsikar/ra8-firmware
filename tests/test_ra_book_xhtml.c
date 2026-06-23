/**
 * @file test_ra_book_xhtml.c
 * @brief MC/DC unit tests for the compiled-book XHTML/text serializer
 *        (libs/ra_book/src/ra_book_xhtml.c).
 *
 * @details
 * Drives `ra_book_chapter_to_xhtml()` and `ra_book_chapter_text()` over a
 * hand-built `.rabook` DOM fixture so every compound decision in the iterative
 * walk is exercised both ways: the emit-chain `&&`/`||` short circuits (varied
 * by capping the output buffer at graded lengths), the whitespace-class test in
 * the text collapser, the paragraph-break trimming, and the recursion-free
 * walk's iteration guard (driven to exhaustion with a self-cyclic node).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_book.h"
#include "ra_err.h"
#include "unity_minimal.h"

/**
 * @enum bx_dim_t
 * @brief Fixture capacities and DOM indices for the serializer tests.
 */
typedef enum : uint32_t {
  k_bx_node_cap    = 8U,    /**< Node-table slots in the fixture.               */
  k_bx_attr_cap    = 2U,    /**< Attribute-table slots in the fixture.          */
  k_bx_str_cap     = 256U,  /**< String-pool bytes in the fixture.              */
  k_bx_out_cap     = 512U,  /**< Serializer output-buffer bytes.                */
  k_bx_big_cap     = 1024U, /**< Larger buffer for the guard-exhaustion walk.   */
  k_bx_node_div    = 0U,    /**< Root <div> element node index.                 */
  k_bx_node_p      = 1U,    /**< <p> block element node index.                  */
  k_bx_node_ptext  = 2U,    /**< Text run inside <p> (entity-bearing).          */
  k_bx_node_img    = 3U,    /**< <img> void element node index.                 */
  k_bx_node_br     = 4U,    /**< <br> void element node index.                  */
  k_bx_node_span   = 5U,    /**< <span> inline element node index.              */
  k_bx_node_sptext = 6U,    /**< Whitespace-heavy text run inside <span>.       */
  k_bx_node_count  = 7U,    /**< Live nodes in the rich fixture.                */
  k_bx_attr_class  = 0U,    /**< class="..." attribute record index.            */
  k_bx_attr_src    = 1U,    /**< src="..." attribute record index.              */
} bx_dim_t;

/**
 * @struct bx_book_t
 * @brief A flat, in-memory `.rabook` blob the accessors can walk.
 * @details Layout mirrors the on-disk container: a header carrying table
 *          offsets, then the chapter/node/attribute tables and the string pool.
 */
typedef struct {
  ra_book_header_t  hdr;                   /**< Container header with offsets. */
  ra_book_chapter_t chapters[1];           /**< Single spine chapter.          */
  ra_book_node_t    nodes[k_bx_node_cap];  /**< DOM node table.                */
  ra_book_attr_t    attrs[k_bx_attr_cap];  /**< Attribute table.               */
  char              strings[k_bx_str_cap]; /**< NUL-separated string pool.     */
} bx_book_t;

/** @brief Append @p s to the pool at @p off (NUL included); return its offset. */
static uint32_t bx_intern(bx_book_t* b, uint32_t* off, const char* s)
{
  const uint32_t at = *off;
  strcpy(&b->strings[at], s);
  *off += (uint32_t)strlen(s) + 1U;
  return at;
}

/**
 * @brief Build the rich fixture: div > (p > text, img, br, span > wstext).
 * @details Covers element + text + void + block + inline nodes, an attribute
 *          whose value needs `"` escaping, and a text run carrying `& < >`.
 */
static void bx_build(bx_book_t* b)
{
  memset(b, 0, sizeof(*b));
  memcpy(b->hdr.magic, "RABOOK1", 8);
  b->hdr.format_version = k_ra_book_format_version;
  b->hdr.total_size     = sizeof(*b);

  b->hdr.chapter_count = 1U;
  b->hdr.chapter_off   = (uint32_t)offsetof(bx_book_t, chapters);
  b->hdr.node_count    = k_bx_node_count;
  b->hdr.node_off      = (uint32_t)offsetof(bx_book_t, nodes);
  b->hdr.attr_count    = k_bx_attr_cap;
  b->hdr.attr_off      = (uint32_t)offsetof(bx_book_t, attrs);
  b->hdr.string_off    = (uint32_t)offsetof(bx_book_t, strings);
  b->hdr.string_size   = k_bx_str_cap;

  uint32_t off    = 0U;
  b->strings[off] = '\0';
  off += 1U;
  const uint32_t s_div    = bx_intern(b, &off, "div");
  const uint32_t s_p      = bx_intern(b, &off, "p");
  const uint32_t s_img    = bx_intern(b, &off, "img");
  const uint32_t s_br     = bx_intern(b, &off, "br");
  const uint32_t s_span   = bx_intern(b, &off, "span");
  const uint32_t s_class  = bx_intern(b, &off, "class");
  const uint32_t s_classv = bx_intern(b, &off, "a\"b"); /* needs &quot; */
  const uint32_t s_src    = bx_intern(b, &off, "src");
  const uint32_t s_srcv   = bx_intern(b, &off, "x.png");
  const uint32_t s_ptext  = bx_intern(b, &off, "A & B < C > D"); /* &amp; &lt; &gt; */
  const uint32_t s_sptext = bx_intern(b, &off, "  hi\tthere\nyou\rzz  ");

  b->chapters[0].root_node = k_bx_node_div;

  b->nodes[k_bx_node_div]    = (ra_book_node_t){.kind         = (uint8_t)k_ra_book_node_element,
                                                .attr_count   = 1U,
                                                .name_off     = s_div,
                                                .first_attr   = k_bx_attr_class,
                                                .first_child  = k_bx_node_p,
                                                .next_sibling = k_ra_book_nil};
  b->nodes[k_bx_node_p]      = (ra_book_node_t){.kind         = (uint8_t)k_ra_book_node_element,
                                                .name_off     = s_p,
                                                .first_attr   = k_ra_book_nil,
                                                .first_child  = k_bx_node_ptext,
                                                .next_sibling = k_bx_node_img};
  b->nodes[k_bx_node_ptext]  = (ra_book_node_t){.kind         = (uint8_t)k_ra_book_node_text,
                                                .text_off     = s_ptext,
                                                .first_attr   = k_ra_book_nil,
                                                .first_child  = k_ra_book_nil,
                                                .next_sibling = k_ra_book_nil};
  b->nodes[k_bx_node_img]    = (ra_book_node_t){.kind         = (uint8_t)k_ra_book_node_element,
                                                .attr_count   = 1U,
                                                .name_off     = s_img,
                                                .first_attr   = k_bx_attr_src,
                                                .first_child  = k_ra_book_nil,
                                                .next_sibling = k_bx_node_br};
  b->nodes[k_bx_node_br]     = (ra_book_node_t){.kind         = (uint8_t)k_ra_book_node_element,
                                                .name_off     = s_br,
                                                .first_attr   = k_ra_book_nil,
                                                .first_child  = k_ra_book_nil,
                                                .next_sibling = k_bx_node_span};
  b->nodes[k_bx_node_span]   = (ra_book_node_t){.kind         = (uint8_t)k_ra_book_node_element,
                                                .name_off     = s_span,
                                                .first_attr   = k_ra_book_nil,
                                                .first_child  = k_bx_node_sptext,
                                                .next_sibling = k_ra_book_nil};
  b->nodes[k_bx_node_sptext] = (ra_book_node_t){.kind         = (uint8_t)k_ra_book_node_text,
                                                .text_off     = s_sptext,
                                                .first_attr   = k_ra_book_nil,
                                                .first_child  = k_ra_book_nil,
                                                .next_sibling = k_ra_book_nil};

  b->attrs[k_bx_attr_class] = (ra_book_attr_t){.name_off = s_class, .value_off = s_classv};
  b->attrs[k_bx_attr_src]   = (ra_book_attr_t){.name_off = s_src, .value_off = s_srcv};
}

/** @brief Build a degenerate book with a single self-cyclic text node. */
static void bx_build_cyclic(bx_book_t* b)
{
  memset(b, 0, sizeof(*b));
  memcpy(b->hdr.magic, "RABOOK1", 8);
  b->hdr.format_version = k_ra_book_format_version;
  b->hdr.total_size     = sizeof(*b);
  b->hdr.chapter_count  = 1U;
  b->hdr.chapter_off    = (uint32_t)offsetof(bx_book_t, chapters);
  b->hdr.node_count     = 1U;
  b->hdr.node_off       = (uint32_t)offsetof(bx_book_t, nodes);
  b->hdr.string_off     = (uint32_t)offsetof(bx_book_t, strings);
  b->hdr.string_size    = k_bx_str_cap;

  uint32_t off    = 0U;
  b->strings[off] = '\0';
  off += 1U;
  const uint32_t s_x = bx_intern(b, &off, "x");

  b->chapters[0].root_node = 0U;
  b->nodes[0]              = (ra_book_node_t){.kind         = (uint8_t)k_ra_book_node_text,
                                              .text_off     = s_x,
                                              .first_attr   = k_ra_book_nil,
                                              .first_child  = k_ra_book_nil,
                                              .next_sibling = 0U}; /* cycle to self */
}

/**
 * @brief Serialize the rich DOM and confirm entity escaping + void elements.
 *
 * @par MC/DC:
 * Decision: the `ra_book_emit_escaped` switch over `& < > "` and the
 * void-element branch `if (ra_book_is_void(name))` in `ra_book_open_element`.
 * - Vector 1: text "A & B < C > D" -> `&amp; &lt; &gt;` (each escape arm true).
 * - Vector 2: attr value `a"b` -> `&quot;` (in_attr quote arm true).
 * - Vector 3: <img>/<br> self-close `/>` (is_void true); <div>/<p>/<span>
 *             emit `>` and a close tag (is_void false).
 */
static void test_ra_book_xhtml_serializes_and_escapes(void)
{
  TEST_BEGIN("ra_book_xhtml serialize + entity escaping + void elements");
  bx_book_t b;
  bx_build(&b);
  char   out[k_bx_out_cap] = {};
  size_t len               = 0U;

  TEST_ASSERT_EQ(k_ra_ok, ra_book_chapter_to_xhtml(&b, 0U, out, sizeof(out), &len));
  out[len] = '\0';
  TEST_ASSERT_NOT_NULL(strstr(out, "<div class=\"a&quot;b\">"));
  TEST_ASSERT_NOT_NULL(strstr(out, "A &amp; B &lt; C &gt; D"));
  TEST_ASSERT_NOT_NULL(strstr(out, "<img src=\"x.png\"/>"));
  TEST_ASSERT_NOT_NULL(strstr(out, "<br/>"));
  TEST_ASSERT_NOT_NULL(strstr(out, "</span>"));
  TEST_ASSERT_NOT_NULL(strstr(out, "</div>"));
  TEST_END("ra_book_xhtml serialize + entity escaping + void elements");
}

/**
 * @brief Truncated buffers force the emit chains to fail at graded depths.
 *
 * @par MC/DC:
 * Decision: the `||` open-tag chain `!emit("<") || !emit_cstr(name) ||
 * !emit_attrs(...)` and the `&&` close-tag chain. Capping the output at graded
 * lengths makes the failing `ra_book_emit()` occur at increasing depth, so each
 * condition is in turn the deciding (short-circuiting) one.
 * - Vector 1: cap 0 -> first emit fails (chain condition 1 true).
 * - Vector 2: cap 1 -> name emit fails (condition 2 true).
 * - Vector 3: cap 4 -> attribute emit fails (condition 3 true).
 * - Vector 4: full cap -> no emit fails (all conditions false; see other test).
 * Each truncated serialize returns ::k_ra_err_invalid_size.
 */
static void test_ra_book_xhtml_to_xhtml_overflow_graded(void)
{
  TEST_BEGIN("ra_book_xhtml to_xhtml graded overflow");
  bx_book_t b;
  bx_build(&b);
  char         out[k_bx_out_cap] = {};
  size_t       len               = 0U;
  const size_t caps[]            = {0U, 1U, 2U, 4U, 6U, 10U, 20U, 40U};
  for (size_t i = 0U; i < (sizeof(caps) / sizeof(caps[0])); ++i) {
    len = 99U;
    TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_book_chapter_to_xhtml(&b, 0U, out, caps[i], &len));
  }
  TEST_END("ra_book_xhtml to_xhtml graded overflow");
}

/**
 * @brief Out-of-range chapter index and null arguments are rejected.
 *
 * @par MC/DC:
 * Decision: `if (chapter_idx >= hdr->chapter_count)` plus the entry
 * `RA_CHECK_NULL_PTR` guards in both public functions.
 * - Vector 1: chapter_idx == count -> ::k_ra_err_invalid_arg (condition true).
 * - Vector 2: chapter_idx 0 valid -> proceeds (condition false; other tests).
 * - Vector 3: null base/out/out_len -> guard rejects (non-ok).
 */
static void test_ra_book_xhtml_invalid_chapter_and_null_guards(void)
{
  TEST_BEGIN("ra_book_xhtml invalid chapter + null guards");
  bx_book_t b;
  bx_build(&b);
  char   out[k_bx_out_cap] = {};
  size_t len               = 0U;

  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_book_chapter_to_xhtml(&b, 1U, out, sizeof(out), &len));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_book_chapter_text(&b, 1U, out, sizeof(out), &len));
  TEST_ASSERT(ra_book_chapter_to_xhtml(nullptr, 0U, out, sizeof(out), &len) != k_ra_ok);
  TEST_ASSERT(ra_book_chapter_to_xhtml(&b, 0U, nullptr, sizeof(out), &len) != k_ra_ok);
  TEST_ASSERT(ra_book_chapter_text(&b, 0U, out, sizeof(out), nullptr) != k_ra_ok);
  TEST_END("ra_book_xhtml invalid chapter + null guards");
}

/**
 * @brief The text walk collapses whitespace runs to single spaces.
 *
 * @par MC/DC:
 * Decision: `ws = (c==' ') || (c=='\t') || (c=='\n') || (c=='\r')` and the
 * `if (*at_break)` collapse guard in `ra_book_emit_text`.
 * - Vector 1: "  hi\tthere\nyou\rzz  " exercises each whitespace class true and
 *             a non-ws char (false).
 * - Vector 2: leading/trailing/duplicate ws collapse and drop at a break,
 *             yielding "hi there you zz" with no raw tab/CR.
 */
static void test_ra_book_xhtml_text_collapses_whitespace(void)
{
  TEST_BEGIN("ra_book_xhtml chapter_text whitespace collapse");
  bx_book_t b;
  bx_build(&b);
  char   out[k_bx_out_cap] = {};
  size_t len               = 0U;

  TEST_ASSERT_EQ(k_ra_ok, ra_book_chapter_text(&b, 0U, out, sizeof(out), &len));
  out[len] = '\0';
  TEST_ASSERT_NOT_NULL(strstr(out, "hi there you zz"));
  TEST_ASSERT_NULL(strchr(out, '\t'));
  TEST_ASSERT_NULL(strchr(out, '\r'));
  TEST_END("ra_book_xhtml chapter_text whitespace collapse");
}

/**
 * @brief Block elements insert paragraph breaks; runs of breaks collapse.
 *
 * @par MC/DC:
 * Decision: `if (ra_book_is_block(name))` in the text walk and the break
 * collapser `if ((*pos > 0) && (out[*pos-1] == '\n'))`.
 * - Vector 1: <div>/<p>/<br> are block -> a newline is emitted (is_block true).
 * - Vector 2: <span> is inline -> no extra break (is_block false).
 * - Vector 3: adjacent block boundaries collapse to a single '\n'.
 */
static void test_ra_book_xhtml_text_block_breaks(void)
{
  TEST_BEGIN("ra_book_xhtml chapter_text block breaks");
  bx_book_t b;
  bx_build(&b);
  char   out[k_bx_out_cap] = {};
  size_t len               = 0U;

  TEST_ASSERT_EQ(k_ra_ok, ra_book_chapter_text(&b, 0U, out, sizeof(out), &len));
  out[len] = '\0';
  TEST_ASSERT_NOT_NULL(strchr(out, '\n'));
  TEST_ASSERT_NULL(strstr(out, "\n\n\n"));
  TEST_END("ra_book_xhtml chapter_text block breaks");
}

/**
 * @brief Truncated buffers fail the text walk at graded depths.
 *
 * @par MC/DC:
 * Decision: the `ra_book_emit*` capacity checks reached from the text walk and
 * the `if (ok && (sp < stack))` descend guard (ok false arm).
 * - Vectors: graded caps 0..8 make an emit fail at increasing depth so the walk
 *   returns ::k_ra_err_invalid_size with `ok` false on the descend guard.
 */
static void test_ra_book_xhtml_text_overflow_graded(void)
{
  TEST_BEGIN("ra_book_xhtml chapter_text graded overflow");
  bx_book_t b;
  bx_build(&b);
  char         out[k_bx_out_cap] = {};
  size_t       len               = 0U;
  const size_t caps[]            = {0U, 1U, 2U, 4U, 8U};
  for (size_t i = 0U; i < (sizeof(caps) / sizeof(caps[0])); ++i) {
    len = 99U;
    TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_book_chapter_text(&b, 0U, out, caps[i], &len));
  }
  TEST_END("ra_book_xhtml chapter_text graded overflow");
}

/**
 * @brief A self-cyclic node drives both walks to their iteration guard.
 *
 * @par MC/DC:
 * Decision: the walk-loop guard `while (sp > 0 && ok && guard < max_iter)` and
 * the final `return ok && (guard < max_iter)` in both `ra_book_walk_to_xhtml`
 * and `ra_book_walk_text`.
 * - Vector 1: self-cyclic node with a large output buffer keeps `sp > 0` and
 *             `ok` true, so `guard < max_iter` is the deciding condition and
 *             goes false at exhaustion -> ::k_ra_err_invalid_size.
 * - Vector 2: the acyclic fixture (other tests) exits via `sp > 0` false with
 *             `guard < max_iter` true, isolating that condition's influence.
 */
static void test_ra_book_xhtml_walk_guard_exhaustion(void)
{
  TEST_BEGIN("ra_book_xhtml walk iteration-guard exhaustion");
  bx_book_t b;
  bx_build_cyclic(&b);
  char   out[k_bx_big_cap] = {};
  size_t len               = 0U;

  TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_book_chapter_to_xhtml(&b, 0U, out, sizeof(out), &len));
  TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_book_chapter_text(&b, 0U, out, sizeof(out), &len));
  TEST_END("ra_book_xhtml walk iteration-guard exhaustion");
}

int main(void)
{
  test_ra_book_xhtml_serializes_and_escapes();
  test_ra_book_xhtml_to_xhtml_overflow_graded();
  test_ra_book_xhtml_invalid_chapter_and_null_guards();
  test_ra_book_xhtml_text_collapses_whitespace();
  test_ra_book_xhtml_text_block_breaks();
  test_ra_book_xhtml_text_overflow_graded();
  test_ra_book_xhtml_walk_guard_exhaustion();
  return 0;
}
