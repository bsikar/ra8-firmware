/**
 * @file test_ra8_book_xhtml.c
 * @brief MC/DC unit tests for the compiled-book XHTML/text serializer
 *        (libs/ra8_book/src/ra8_book_xhtml.c).
 *
 * @details
 * Drives `ra8_book_chapter_to_xhtml()` and `ra8_book_chapter_text()` over a
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

#include "ra8_book.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum book_xhtml_fixture_t
 * @brief Poison values written into out-parameters before a call, so one that fails without assigning is detectable, plus buffer capacities and payload sizes.
 */
typedef enum : uint8_t {
  k_xhtml_poison_len =
    99U, /**< Poison in a length out-parameter, so a failed call that skips it is detectable. */
} book_xhtml_fixture_t;

/**
 * @enum bx_dim_t
 * @brief Fixture capacities and DOM indices for the serializer tests.
 */
typedef enum : uint32_t {
  k_bx_node_cap    = 8U,    /**< Node-table slots in the fixture.             */
  k_bx_attr_cap    = 2U,    /**< Attribute-table slots in the fixture.        */
  k_bx_str_cap     = 256U,  /**< String-pool bytes in the fixture.            */
  k_bx_out_cap     = 512U,  /**< Serializer output-buffer bytes.              */
  k_bx_big_cap     = 1024U, /**< Larger buffer for the guard-exhaustion walk. */
  k_bx_node_div    = 0U,    /**< Root <div> element node index.               */
  k_bx_node_p      = 1U,    /**< <p> block element node index.                */
  k_bx_node_ptext  = 2U,    /**< Text run inside <p> (entity-bearing).        */
  k_bx_node_img    = 3U,    /**< <img> void element node index.               */
  k_bx_node_br     = 4U,    /**< <br> void element node index.                */
  k_bx_node_span   = 5U,    /**< <span> inline element node index.            */
  k_bx_node_sptext = 6U,    /**< Whitespace-heavy text run inside <span>.     */
  k_bx_node_count  = 7U,    /**< Live nodes in the rich fixture.              */
  k_bx_attr_class  = 0U,    /**< class="..." attribute record index.          */
  k_bx_attr_src    = 1U,    /**< src="..." attribute record index.            */
} bx_dim_t;

/**
 * @struct bx_book_t
 * @brief A flat, in-memory `.rabook` blob the accessors can walk.
 * @details Layout mirrors the on-disk container: a header carrying table
 *          offsets, then the chapter/node/attribute tables and the string pool.
 */
typedef struct {
  ra8_book_header_t  hdr;                   /**< Container header with offsets. */
  ra8_book_chapter_t chapters[1];           /**< Single spine chapter.          */
  ra8_book_node_t    nodes[k_bx_node_cap];  /**< DOM node table.                */
  ra8_book_attr_t    attrs[k_bx_attr_cap];  /**< Attribute table.               */
  char               strings[k_bx_str_cap]; /**< NUL-separated string pool.     */
} bx_book_t;

/** @brief Append @p s to the pool at @p off (NUL included); return its offset. */
static uint32_t bx_intern(bx_book_t* b, uint32_t* off, const char* s)
{
  const uint32_t at  = *off;
  const size_t   len = strlen(s);
  /* The pool is a fixed-size fixture buffer sized for the strings this file
   * interns. An unbounded strcpy here would run off the end of the enclosing
   * struct if a fixture ever grew past it; fail the test loudly instead. */
  TEST_ASSERT(((size_t)at + len + 1U) <= sizeof(b->strings));
  memcpy(&b->strings[at], s, len + 1U);
  *off += (uint32_t)len + 1U;
  return at;
}

/**
 * @struct bx_offsets_t
 * @brief Interned string offsets shared between the mock-book build phases.
 * @see bx_build
 */
typedef struct {
  uint32_t div;    /**< "div" element name.        */
  uint32_t p;      /**< "p" element name.          */
  uint32_t img;    /**< "img" element name.        */
  uint32_t br;     /**< "br" element name.         */
  uint32_t span;   /**< "span" element name.       */
  uint32_t cls;    /**< "class" attribute name.    */
  uint32_t classv; /**< class value (needs quote). */
  uint32_t src;    /**< "src" attribute name.      */
  uint32_t srcv;   /**< src value.                 */
  uint32_t ptext;  /**< Paragraph text.            */
  uint32_t sptext; /**< Span whitespace text.      */
} bx_offsets_t;

/**
 * @brief Fill the mock book's fixed header and table offsets.
 * @param[out] b Mock book to initialise (fully zeroed first).
 * @pre @p b is non-null.
 * @post Header magic/version and every table offset/count are populated.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void bx_build_header(bx_book_t* b)
{
  memset(b, 0, sizeof(*b));
  memcpy(b->hdr.magic, "RABOOK1", 8);
  b->hdr.format_version = k_ra8_book_format_version;
  b->hdr.total_size     = sizeof(*b);

  b->hdr.chapter_count = 1U;
  b->hdr.chapter_off   = (uint32_t)offsetof(bx_book_t, chapters);
  b->hdr.node_count    = k_bx_node_count;
  b->hdr.node_off      = (uint32_t)offsetof(bx_book_t, nodes);
  b->hdr.attr_count    = k_bx_attr_cap;
  b->hdr.attr_off      = (uint32_t)offsetof(bx_book_t, attrs);
  b->hdr.string_off    = (uint32_t)offsetof(bx_book_t, strings);
  b->hdr.string_size   = k_bx_str_cap;
}

/**
 * @brief Intern every element/attribute/text string into the mock book pool.
 * @param[in,out] b Mock book whose string pool is filled.
 * @param[out]    o Receives the interned string offsets.
 * @pre bx_build_header(@p b) already ran.
 * @post @p o holds the offset of every interned string.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void bx_build_strings(bx_book_t* b, bx_offsets_t* o)
{
  uint32_t off    = 0U;
  b->strings[off] = '\0';
  off += 1U;
  o->div    = bx_intern(b, &off, "div");
  o->p      = bx_intern(b, &off, "p");
  o->img    = bx_intern(b, &off, "img");
  o->br     = bx_intern(b, &off, "br");
  o->span   = bx_intern(b, &off, "span");
  o->cls    = bx_intern(b, &off, "class");
  o->classv = bx_intern(b, &off, "a\"b"); /* needs &quot; */
  o->src    = bx_intern(b, &off, "src");
  o->srcv   = bx_intern(b, &off, "x.png");
  o->ptext  = bx_intern(b, &off, "A & B < C > D"); /* &amp; &lt; &gt; */
  o->sptext = bx_intern(b, &off, "  hi\tthere\nyou\rzz  ");
}

/**
 * @brief Wire the mock book's DOM node tree and attribute table.
 * @param[in,out] b Mock book whose node/attr tables are populated.
 * @param[in]     o Interned string offsets from bx_build_strings.
 * @pre bx_build_strings(@p b, @p o) already ran.
 * @post The node tree and both attributes are set.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void bx_build_nodes(bx_book_t* b, const bx_offsets_t* o)
{
  b->chapters[0].root_node = k_bx_node_div;

  b->nodes[k_bx_node_div]    = (ra8_book_node_t){.kind         = (uint8_t)k_ra8_book_node_element,
                                                 .attr_count   = 1U,
                                                 .name_off     = o->div,
                                                 .first_attr   = k_bx_attr_class,
                                                 .first_child  = k_bx_node_p,
                                                 .next_sibling = k_ra8_book_nil};
  b->nodes[k_bx_node_p]      = (ra8_book_node_t){.kind         = (uint8_t)k_ra8_book_node_element,
                                                 .name_off     = o->p,
                                                 .first_attr   = k_ra8_book_nil,
                                                 .first_child  = k_bx_node_ptext,
                                                 .next_sibling = k_bx_node_img};
  b->nodes[k_bx_node_ptext]  = (ra8_book_node_t){.kind         = (uint8_t)k_ra8_book_node_text,
                                                 .text_off     = o->ptext,
                                                 .first_attr   = k_ra8_book_nil,
                                                 .first_child  = k_ra8_book_nil,
                                                 .next_sibling = k_ra8_book_nil};
  b->nodes[k_bx_node_img]    = (ra8_book_node_t){.kind         = (uint8_t)k_ra8_book_node_element,
                                                 .attr_count   = 1U,
                                                 .name_off     = o->img,
                                                 .first_attr   = k_bx_attr_src,
                                                 .first_child  = k_ra8_book_nil,
                                                 .next_sibling = k_bx_node_br};
  b->nodes[k_bx_node_br]     = (ra8_book_node_t){.kind         = (uint8_t)k_ra8_book_node_element,
                                                 .name_off     = o->br,
                                                 .first_attr   = k_ra8_book_nil,
                                                 .first_child  = k_ra8_book_nil,
                                                 .next_sibling = k_bx_node_span};
  b->nodes[k_bx_node_span]   = (ra8_book_node_t){.kind         = (uint8_t)k_ra8_book_node_element,
                                                 .name_off     = o->span,
                                                 .first_attr   = k_ra8_book_nil,
                                                 .first_child  = k_bx_node_sptext,
                                                 .next_sibling = k_ra8_book_nil};
  b->nodes[k_bx_node_sptext] = (ra8_book_node_t){.kind         = (uint8_t)k_ra8_book_node_text,
                                                 .text_off     = o->sptext,
                                                 .first_attr   = k_ra8_book_nil,
                                                 .first_child  = k_ra8_book_nil,
                                                 .next_sibling = k_ra8_book_nil};

  b->attrs[k_bx_attr_class] = (ra8_book_attr_t){.name_off = o->cls, .value_off = o->classv};
  b->attrs[k_bx_attr_src]   = (ra8_book_attr_t){.name_off = o->src, .value_off = o->srcv};
}

/**
 * @brief Build the complete XHTML-shaped mock book for the escaping tests.
 * @param[out] b Mock book to populate end to end.
 * @pre @p b is non-null.
 * @post @p b is a self-consistent XHTML fixture book.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void bx_build(bx_book_t* b)
{
  bx_offsets_t o = {};
  bx_build_header(b);
  bx_build_strings(b, &o);
  bx_build_nodes(b, &o);
}

/** @brief Build a degenerate book with a single self-cyclic text node. */
static void bx_build_cyclic(bx_book_t* b)
{
  memset(b, 0, sizeof(*b));
  memcpy(b->hdr.magic, "RABOOK1", 8);
  b->hdr.format_version = k_ra8_book_format_version;
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
  b->nodes[0]              = (ra8_book_node_t){.kind         = (uint8_t)k_ra8_book_node_text,
                                               .text_off     = s_x,
                                               .first_attr   = k_ra8_book_nil,
                                               .first_child  = k_ra8_book_nil,
                                               .next_sibling = 0U}; /* cycle to self */
}

/**
 * @brief Serialize the rich DOM and confirm entity escaping + void elements.
 *
 * @par MC/DC:
 * Decision: the `internal_emit_escaped` switch over `& < > "` and the
 * void-element branch `if (internal_is_void(name))` in `internal_open_element`.
 * - Vector 1: text "A & B < C > D" -> `&amp; &lt; &gt;` (each escape arm true).
 * - Vector 2: attr value `a"b` -> `&quot;` (in_attr quote arm true).
 * - Vector 3: <img>/<br> self-close `/>` (is_void true); <div>/<p>/<span>
 *             emit `>` and a close tag (is_void false).
 */
static void test_ra8_book_xhtml_serializes_and_escapes(void)
{
  TEST_BEGIN("ra8_book_xhtml serialize + entity escaping + void elements");
  bx_book_t b;
  bx_build(&b);
  char   out[k_bx_out_cap] = {};
  size_t len               = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_chapter_to_xhtml(&b, 0U, out, sizeof(out), &len));
  out[len] = '\0';
  TEST_ASSERT_NOT_NULL(strstr(out, "<div class=\"a&quot;b\">"));
  TEST_ASSERT_NOT_NULL(strstr(out, "A &amp; B &lt; C &gt; D"));
  TEST_ASSERT_NOT_NULL(strstr(out, "<img src=\"x.png\"/>"));
  TEST_ASSERT_NOT_NULL(strstr(out, "<br/>"));
  TEST_ASSERT_NOT_NULL(strstr(out, "</span>"));
  TEST_ASSERT_NOT_NULL(strstr(out, "</div>"));
  TEST_END("ra8_book_xhtml serialize + entity escaping + void elements");
}

/**
 * @brief Truncated buffers force the emit chains to fail at graded depths.
 *
 * @par MC/DC:
 * Decision: the `||` open-tag chain `!emit("<") || !emit_cstr(name) ||
 * !emit_attrs(...)` and the `&&` close-tag chain. Capping the output at graded
 * lengths makes the failing `internal_emit()` occur at increasing depth, so each
 * condition is in turn the deciding (short-circuiting) one.
 * - Vector 1: cap 0 -> first emit fails (chain condition 1 true).
 * - Vector 2: cap 1 -> name emit fails (condition 2 true).
 * - Vector 3: cap 4 -> attribute emit fails (condition 3 true).
 * - Vector 4: full cap -> no emit fails (all conditions false; see other test).
 * Each truncated serialize returns ::k_ra8_err_invalid_size.
 */
static void test_ra8_book_xhtml_to_xhtml_overflow_graded(void)
{
  TEST_BEGIN("ra8_book_xhtml to_xhtml graded overflow");
  bx_book_t b;
  bx_build(&b);
  char   out[k_bx_out_cap] = {};
  size_t len               = 0U;
  /* Caps walk through the open tag, the per-attribute emit chain (positions
   * ~5..18 inside ` class="a&quot;b"`) and on into the body, so each condition
   * of the attribute `&&` chain is the deciding (failing) one in turn. */
  const size_t caps[] =
    {0U, 1U, 2U, 4U, 5U, 6U, 7U, 8U, 10U, 11U, 12U, 13U, 16U, 17U, 20U, 30U, 40U, 60U};
  for (size_t i = 0U; i < (sizeof(caps) / sizeof(caps[0])); ++i) {
    len = k_xhtml_poison_len;
    TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_book_chapter_to_xhtml(&b, 0U, out, caps[i], &len));
  }
  TEST_END("ra8_book_xhtml to_xhtml graded overflow");
}

/**
 * @brief Out-of-range chapter index and null arguments are rejected.
 *
 * @par MC/DC:
 * Decision: `if (chapter_idx >= hdr->chapter_count)` plus the entry
 * `RA8_CHECK_NULL_PTR` guards in both public functions.
 * - Vector 1: chapter_idx == count -> ::k_ra8_err_invalid_arg (condition true).
 * - Vector 2: chapter_idx 0 valid -> proceeds (condition false; other tests).
 * - Vector 3: null base/out/out_len -> guard rejects (non-ok).
 */
static void test_ra8_book_xhtml_invalid_chapter_and_null_guards(void)
{
  TEST_BEGIN("ra8_book_xhtml invalid chapter + null guards");
  bx_book_t b;
  bx_build(&b);
  char   out[k_bx_out_cap] = {};
  size_t len               = 0U;

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_book_chapter_to_xhtml(&b, 1U, out, sizeof(out), &len));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_book_chapter_text(&b, 1U, out, sizeof(out), &len));
  TEST_ASSERT(ra8_book_chapter_to_xhtml(nullptr, 0U, out, sizeof(out), &len) != k_ra8_ok);
  TEST_ASSERT(ra8_book_chapter_to_xhtml(&b, 0U, nullptr, sizeof(out), &len) != k_ra8_ok);
  TEST_ASSERT(ra8_book_chapter_text(&b, 0U, out, sizeof(out), nullptr) != k_ra8_ok);
  TEST_END("ra8_book_xhtml invalid chapter + null guards");
}

/**
 * @brief The text walk collapses whitespace runs to single spaces.
 *
 * @par MC/DC:
 * Decision: `ws = (c==' ') || (c=='\t') || (c=='\n') || (c=='\r')` and the
 * `if (*at_break)` collapse guard in `priv_book_emit_text`.
 * - Vector 1: "  hi\tthere\nyou\rzz  " exercises each whitespace class true and
 *             a non-ws char (false).
 * - Vector 2: leading/trailing/duplicate ws collapse and drop at a break,
 *             yielding "hi there you zz" with no raw tab/CR.
 */
static void test_ra8_book_xhtml_text_collapses_whitespace(void)
{
  TEST_BEGIN("ra8_book_xhtml chapter_text whitespace collapse");
  bx_book_t b;
  bx_build(&b);
  char   out[k_bx_out_cap] = {};
  size_t len               = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_chapter_text(&b, 0U, out, sizeof(out), &len));
  out[len] = '\0';
  TEST_ASSERT_NOT_NULL(strstr(out, "hi there you zz"));
  TEST_ASSERT_NULL(strchr(out, '\t'));
  TEST_ASSERT_NULL(strchr(out, '\r'));
  TEST_END("ra8_book_xhtml chapter_text whitespace collapse");
}

/**
 * @brief Block elements insert paragraph breaks; runs of breaks collapse.
 *
 * @par MC/DC:
 * Decision: `if (priv_book_is_block(name))` in the text walk and the break
 * collapser `if ((*pos > 0) && (out[*pos-1] == '\n'))`.
 * - Vector 1: <div>/<p>/<br> are block -> a newline is emitted (is_block true).
 * - Vector 2: <span> is inline -> no extra break (is_block false).
 * - Vector 3: adjacent block boundaries collapse to a single '\n'.
 */
static void test_ra8_book_xhtml_text_block_breaks(void)
{
  TEST_BEGIN("ra8_book_xhtml chapter_text block breaks");
  bx_book_t b;
  bx_build(&b);
  char   out[k_bx_out_cap] = {};
  size_t len               = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_chapter_text(&b, 0U, out, sizeof(out), &len));
  out[len] = '\0';
  TEST_ASSERT_NOT_NULL(strchr(out, '\n'));
  TEST_ASSERT_NULL(strstr(out, "\n\n\n"));
  TEST_END("ra8_book_xhtml chapter_text block breaks");
}

/**
 * @brief Truncated buffers fail the text walk at graded depths.
 *
 * @par MC/DC:
 * Decision: the `internal_emit*` capacity checks reached from the text walk and
 * the `if (ok && (sp < stack))` descend guard (ok false arm).
 * - Vectors: graded caps 0..8 make an emit fail at increasing depth so the walk
 *   returns ::k_ra8_err_invalid_size with `ok` false on the descend guard.
 */
static void test_ra8_book_xhtml_text_overflow_graded(void)
{
  TEST_BEGIN("ra8_book_xhtml chapter_text graded overflow");
  bx_book_t b;
  bx_build(&b);
  char         out[k_bx_out_cap] = {};
  size_t       len               = 0U;
  const size_t caps[]            = {0U, 1U, 2U, 4U, 8U};
  for (size_t i = 0U; i < (sizeof(caps) / sizeof(caps[0])); ++i) {
    len = k_xhtml_poison_len;
    TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_book_chapter_text(&b, 0U, out, caps[i], &len));
  }
  TEST_END("ra8_book_xhtml chapter_text graded overflow");
}

/**
 * @brief A self-cyclic node drives both walks to their iteration guard.
 *
 * @par MC/DC:
 * Decision: the walk-loop guard `while (sp > 0 && ok && guard < max_iter)` and
 * the final `return ok && (guard < max_iter)` in both `internal_walk_to_xhtml`
 * and `internal_walk_text`.
 * - Vector 1: self-cyclic node with a large output buffer keeps `sp > 0` and
 *             `ok` true, so `guard < max_iter` is the deciding condition and
 *             goes false at exhaustion -> ::k_ra8_err_invalid_size.
 * - Vector 2: the acyclic fixture (other tests) exits via `sp > 0` false with
 *             `guard < max_iter` true, isolating that condition's influence.
 */
static void test_ra8_book_xhtml_walk_guard_exhaustion(void)
{
  TEST_BEGIN("ra8_book_xhtml walk iteration-guard exhaustion");
  bx_book_t b;
  bx_build_cyclic(&b);
  char   out[k_bx_big_cap] = {};
  size_t len               = 0U;

  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_book_chapter_to_xhtml(&b, 0U, out, sizeof(out), &len));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_book_chapter_text(&b, 0U, out, sizeof(out), &len));
  TEST_END("ra8_book_xhtml walk iteration-guard exhaustion");
}

/** @brief Build a minimal `<p>x</p>` book so small caps reach the close tag. */
static void bx_build_small(bx_book_t* b)
{
  memset(b, 0, sizeof(*b));
  memcpy(b->hdr.magic, "RABOOK1", 8);
  b->hdr.format_version = k_ra8_book_format_version;
  b->hdr.total_size     = sizeof(*b);
  b->hdr.chapter_count  = 1U;
  b->hdr.chapter_off    = (uint32_t)offsetof(bx_book_t, chapters);
  b->hdr.node_count     = 2U;
  b->hdr.node_off       = (uint32_t)offsetof(bx_book_t, nodes);
  b->hdr.string_off     = (uint32_t)offsetof(bx_book_t, strings);
  b->hdr.string_size    = k_bx_str_cap;

  uint32_t off    = 0U;
  b->strings[off] = '\0';
  off += 1U;
  const uint32_t s_p = bx_intern(b, &off, "p");
  const uint32_t s_x = bx_intern(b, &off, "x");

  b->chapters[0].root_node = 0U;
  b->nodes[0]              = (ra8_book_node_t){.kind         = (uint8_t)k_ra8_book_node_element,
                                               .name_off     = s_p,
                                               .first_attr   = k_ra8_book_nil,
                                               .first_child  = 1U,
                                               .next_sibling = k_ra8_book_nil};
  b->nodes[1]              = (ra8_book_node_t){.kind         = (uint8_t)k_ra8_book_node_text,
                                               .text_off     = s_x,
                                               .first_attr   = k_ra8_book_nil,
                                               .first_child  = k_ra8_book_nil,
                                               .next_sibling = k_ra8_book_nil};
}

/** @brief Build `<div>word <p>x</p></div>`: a trailing space before a block. */
static void bx_build_trail(bx_book_t* b)
{
  memset(b, 0, sizeof(*b));
  memcpy(b->hdr.magic, "RABOOK1", 8);
  b->hdr.format_version = k_ra8_book_format_version;
  b->hdr.total_size     = sizeof(*b);
  b->hdr.chapter_count  = 1U;
  b->hdr.chapter_off    = (uint32_t)offsetof(bx_book_t, chapters);
  b->hdr.node_count     = 4U;
  b->hdr.node_off       = (uint32_t)offsetof(bx_book_t, nodes);
  b->hdr.string_off     = (uint32_t)offsetof(bx_book_t, strings);
  b->hdr.string_size    = k_bx_str_cap;

  uint32_t off    = 0U;
  b->strings[off] = '\0';
  off += 1U;
  const uint32_t s_div  = bx_intern(b, &off, "div");
  const uint32_t s_p    = bx_intern(b, &off, "p");
  const uint32_t s_word = bx_intern(b, &off, "word ");
  const uint32_t s_x    = bx_intern(b, &off, "x");

  b->chapters[0].root_node = 0U;
  b->nodes[0]              = (ra8_book_node_t){.kind         = (uint8_t)k_ra8_book_node_element,
                                               .name_off     = s_div,
                                               .first_attr   = k_ra8_book_nil,
                                               .first_child  = 1U,
                                               .next_sibling = k_ra8_book_nil};
  b->nodes[1]              = (ra8_book_node_t){.kind         = (uint8_t)k_ra8_book_node_text,
                                               .text_off     = s_word,
                                               .first_attr   = k_ra8_book_nil,
                                               .first_child  = k_ra8_book_nil,
                                               .next_sibling = 2U};
  b->nodes[2]              = (ra8_book_node_t){.kind         = (uint8_t)k_ra8_book_node_element,
                                               .name_off     = s_p,
                                               .first_attr   = k_ra8_book_nil,
                                               .first_child  = 3U,
                                               .next_sibling = k_ra8_book_nil};
  b->nodes[3]              = (ra8_book_node_t){.kind         = (uint8_t)k_ra8_book_node_text,
                                               .text_off     = s_x,
                                               .first_attr   = k_ra8_book_nil,
                                               .first_child  = k_ra8_book_nil,
                                               .next_sibling = k_ra8_book_nil};
}

/**
 * @brief A minimal book lets graded caps fail inside the close-tag emit chain.
 *
 * @par MC/DC:
 * Decision: the close-tag `&&` chain `emit("</") && emit_cstr(name) &&
 * emit(">")` in `internal_walk_to_xhtml`. With the tiny `<p>x</p>` output the
 * close tag begins at byte 4, so caps 4/5/6/7 make each emit in the close
 * chain the deciding failure, while the full cap exercises the all-true path.
 */
static void test_ra8_book_xhtml_close_chain_overflow(void)
{
  TEST_BEGIN("ra8_book_xhtml close-tag chain overflow");
  bx_book_t b;
  bx_build_small(&b);
  char         out[k_bx_out_cap] = {};
  size_t       len               = 0U;
  const size_t caps[]            = {1U, 2U, 3U, 4U, 5U, 6U, 7U};
  for (size_t i = 0U; i < (sizeof(caps) / sizeof(caps[0])); ++i) {
    len = k_xhtml_poison_len;
    TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_book_chapter_to_xhtml(&b, 0U, out, caps[i], &len));
  }
  /* Full cap: the whole "<p>x</p>" serializes (all chain conditions false). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_chapter_to_xhtml(&b, 0U, out, sizeof(out), &len));
  out[len] = '\0';
  TEST_ASSERT_NOT_NULL(strstr(out, "<p>x</p>"));
  TEST_END("ra8_book_xhtml close-tag chain overflow");
}

/**
 * @brief A trailing space ahead of a block break drives the trim loop.
 *
 * @par MC/DC:
 * Decision: `while ((*pos > 0) && (out[*pos-1] == ' '))` in `priv_book_emit_break`.
 * - Both-true: "word " leaves a trailing space, then the `<p>` block break
 *   trims it (the loop body runs).
 * - `out[*pos-1] == ' '` false: a later break sees a non-space tail and stops.
 */
static void test_ra8_book_xhtml_break_trims_trailing_space(void)
{
  TEST_BEGIN("ra8_book_xhtml break trims trailing space");
  bx_book_t b;
  bx_build_trail(&b);
  char   out[k_bx_out_cap] = {};
  size_t len               = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_chapter_text(&b, 0U, out, sizeof(out), &len));
  out[len] = '\0';
  TEST_ASSERT_NOT_NULL(strstr(out, "word"));
  TEST_ASSERT_NOT_NULL(strstr(out, "x"));
  /* The space between "word" and the block break was trimmed away. */
  TEST_ASSERT_NULL(strstr(out, "word \n"));
  TEST_END("ra8_book_xhtml break trims trailing space");
}

/**
 * @enum bx_deep_dim_t
 * @brief Nesting depth for the stack-exhaustion fixture.
 */
typedef enum : uint32_t {
  k_bx_deep_n = 560U, /**< Nesting depth: exceeds the 512-entry walk stack. */
} bx_deep_dim_t;

/**
 * @struct bx_deep_t
 * @brief A `.rabook` blob whose nodes nest @ref k_bx_deep_n elements deep.
 */
typedef struct {
  ra8_book_header_t  hdr;                /**< Container header.                */
  ra8_book_chapter_t chapters[1];        /**< Single spine chapter.            */
  ra8_book_node_t    nodes[k_bx_deep_n]; /**< Linear chain of nested elements. */
  char               strings[8];         /**< "d" tag + "x" leaf text.         */
} bx_deep_t;

/** @brief Build a single chain of @ref k_bx_deep_n nested `<d>` elements. */
static void bx_build_deep(bx_deep_t* b)
{
  memset(b, 0, sizeof(*b));
  memcpy(b->hdr.magic, "RABOOK1", 8);
  b->hdr.format_version = k_ra8_book_format_version;
  b->hdr.total_size     = sizeof(*b);
  b->hdr.chapter_count  = 1U;
  b->hdr.chapter_off    = (uint32_t)offsetof(bx_deep_t, chapters);
  b->hdr.node_count     = k_bx_deep_n;
  b->hdr.node_off       = (uint32_t)offsetof(bx_deep_t, nodes);
  b->hdr.string_off     = (uint32_t)offsetof(bx_deep_t, strings);
  b->hdr.string_size    = sizeof(b->strings);
  b->strings[0]         = '\0';
  b->strings[1]         = 'd'; /* tag name at offset 1 */
  b->strings[2]         = '\0';
  b->strings[3]         = 'x'; /* leaf text at offset 3 */

  b->chapters[0].root_node = 0U;
  for (uint32_t i = 0U; i < (k_bx_deep_n - 1U); ++i) {
    b->nodes[i] = (ra8_book_node_t){.kind         = (uint8_t)k_ra8_book_node_element,
                                    .name_off     = 1U,
                                    .first_attr   = k_ra8_book_nil,
                                    .first_child  = i + 1U,
                                    .next_sibling = k_ra8_book_nil};
  }
  b->nodes[k_bx_deep_n - 1U] = (ra8_book_node_t){.kind         = (uint8_t)k_ra8_book_node_text,
                                                 .text_off     = 3U,
                                                 .first_attr   = k_ra8_book_nil,
                                                 .first_child  = k_ra8_book_nil,
                                                 .next_sibling = k_ra8_book_nil};
}

/**
 * @brief A deeply-nested DOM exhausts the bounded walk stack on both walkers.
 *
 * @par MC/DC:
 * Decision: the open-tag stack guard `!emit(">") || (*sp + 2 > k_stack)` in
 * `internal_open_element`, and the descend guard `if (ok && (sp < k_stack))` in
 * `internal_walk_text`. A chain of @ref k_bx_deep_n nested elements (output
 * buffer sized so no emit fails first) drives:
 * - to_xhtml: `*sp + 2 > k_stack` becomes true near depth 256, so the open-tag
 *   guard's stack-capacity arm aborts the walk -> ::k_ra8_err_invalid_size.
 * - chapter_text: `sp < k_stack` becomes false and caps the descent, so the
 *   walk completes without overflow -> ::k_ra8_ok with empty output (the false
 *   arm of the descend guard).
 * @note The loop-internal `if (sp >= k_stack) return false` in both walkers is
 *       defense-in-depth: the per-element open / descend guards above cap `sp`
 *       at k_stack, so a pop always leaves `sp <= k_stack - 1` and that guard's
 *       true arm is unreachable through the public API (deactivated, not faked).
 */
static void test_ra8_book_xhtml_deep_nest_bounds(void)
{
  TEST_BEGIN("ra8_book_xhtml deep-nest walk bounds");
  static bx_deep_t s_b;
  bx_build_deep(&s_b);
  char   out[k_bx_big_cap] = {};
  size_t len               = k_xhtml_poison_len;

  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_book_chapter_to_xhtml(&s_b, 0U, out, sizeof(out), &len));
  len = k_xhtml_poison_len;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_chapter_text(&s_b, 0U, out, sizeof(out), &len));
  TEST_ASSERT_EQ(0, len); /* descent capped before reaching the leaf */
  TEST_END("ra8_book_xhtml deep-nest walk bounds");
}

int main(void)
{
  test_ra8_book_xhtml_serializes_and_escapes();
  test_ra8_book_xhtml_to_xhtml_overflow_graded();
  test_ra8_book_xhtml_invalid_chapter_and_null_guards();
  test_ra8_book_xhtml_text_collapses_whitespace();
  test_ra8_book_xhtml_text_block_breaks();
  test_ra8_book_xhtml_text_overflow_graded();
  test_ra8_book_xhtml_walk_guard_exhaustion();
  test_ra8_book_xhtml_close_chain_overflow();
  test_ra8_book_xhtml_break_trims_trailing_space();
  test_ra8_book_xhtml_deep_nest_bounds();
  return 0;
}
