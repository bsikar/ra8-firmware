/**
 * @file test_ra8_rabook_xml_shim.cpp
 * @brief MC/DC + DOM-structure tests for ra8_rabook_xml_shim (#149).
 *
 * @details
 * Drives @ref ra8_rabook_xml_parse_chapter with synthetic XHTML fixtures to
 * verify:
 *
 *  - Null-pointer validation (4 guards).
 *  - Malformed XML rejection.
 *  - Empty @c <body> produces a single element node (chapter root).
 *  - Simple @c <body><p>Hello</p></body> -- one element child, one text child.
 *  - Nested siblings @c <body><div><p>A</p></div><p>B</p></body> -- proves the
 *    iterative DFS visits nodes in the correct pre-order: body, div, p, "A",
 *    p2, "B".
 *  - Comment nodes are silently skipped.
 *  - @c <![CDATA[...]]> sections are silently skipped (B's fix: CDATA is dropped,
 *    never emitted as a text node).
 *  - @c <html><body>...</body></html> wrapper: body fallback search works.
 *  - Deep nesting at the tinyxml2 element-depth cap (#625): the deepest
 *    document tinyxml2 accepts compiles with every node emitted -- the DFS
 *    frame stack is never overrun -- and one level deeper is rejected by the
 *    parser rather than silently truncated.
 *
 * @par MC/DC decisions exercised (ra8_rabook_xml_shim.cpp):
 *   - @c doc.Parse(...) != XML_SUCCESS -- single condition.
 *       T: test_malformed_xml (unclosed element). F: every well-formed fixture.
 *   - @c root == nullptr in internal_find_body -- single condition. F: every fixture has
 *       a root element. T is unreachable through the public entry point (a
 *       document with no root makes doc.Parse fail first), so it is a defensive
 *       guard with no independent-influence vector (documented, not testable).
 *   - @c std::strcmp(e->Name(), "body") == 0 in internal_find_body -- single condition.
 *       T: test_html_wrapper_body_fallback (<body> found under <html>).
 *       F: the same scan steps past <head/> before matching <body>.
 *   - @c elem != nullptr in internal_emit_node -- single condition.
 *       T: any element fixture. F: text/CDATA fixtures fall through to ToText().
 *   - @c text->CData() in internal_emit_node -- single condition.
 *       T: test_cdata_skipped (CDATA dropped). F: any real text node.
 *   - @c val != nullptr && val[0] != '\0' in internal_emit_node -- COMPOUND AND, but the
 *       first condition is structurally always true: it is reached only after
 *       @c node->ToText() returned non-null, and tinyxml2's XMLText::Value()
 *       never returns nullptr (it returns the empty string ""). So
 *       @c val != nullptr cannot be observed false on any reachable input and has
 *       no independent-influence vector (defensive guard, same class as the
 *       mcdc-deactivated NULL guards in ra8_epub_xml_shim.cpp). The second
 *       condition @c val[0] != '\0' is the live one and BOTH its arms are covered:
 *         - true  -> text emitted: test_simple_p_with_text ("Hello").
 *         - false -> text skipped: test_empty_text_skipped (<p></p>, val[0]=='\0').
 *   - @c node != nullptr && top < k_xhtml_max_stack in internal_push_frame -- COMPOUND
 *       AND. The first condition is live (a null FirstChild/NextSibling stops a
 *       branch): T covered by any fixture with children, F by leaf/last-sibling
 *       nodes (test_nested_siblings_preorder exercises both). The second is the
 *       NASA Rule 2 safety valve and cannot go false: the frame high-water mark
 *       is the node-depth of the deepest node below @c <body> (depth + 1, not
 *       2 * depth -- a sibling step pops one frame and pushes one), and tinyxml2
 *       aborts at @c TINYXML2_MAX_ELEMENT_DEPTH (500), which caps top at 498
 *       against a capacity of 512. test_deep_at_tinyxml2_cap pins that worst
 *       case and test_deep_beyond_tinyxml2_cap pins the rejection one level
 *       deeper, so the deactivation rests on a measurement rather than on the
 *       false cap-of-100 claim it carried before #625.
 *   - @c frame.prev_sib_idx == k_ra8_book_nil in internal_walk_body_subtree -- single
 *       condition. T (first child) and F (later sibling) both in
 *       test_nested_siblings_preorder.
 *   - @c elem != nullptr && new_idx != k_ra8_book_nil in internal_walk_body_subtree --
 *       COMPOUND AND. elem!=nullptr varies (element vs text node) and
 *       new_idx!=nil varies (emitted vs skipped); test_nested_siblings_preorder
 *       supplies element-with-children (both true -> push children) and text
 *       leaves (elem false -> no push), and test_cdata_skipped supplies a skipped
 *       node (new_idx nil -> no push).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include "ra8_attributes.h"
#include "ra8_book.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_rabook_compile.h"
#include "ra8_rabook_xml_shim.h"
}

/* Third-party, header-only use: the deep-nesting fixtures below are derived from
 * TINYXML2_MAX_ELEMENT_DEPTH so they track the vendored snapshot instead of
 * hard-coding a cap that can silently drift (#625). */
#include "tinyxml2.h"

/* -------------------------------------------------------------------------- */
/* Static test arenas */
/* -------------------------------------------------------------------------- */

namespace {

typedef enum : uint16_t {
  k_chapter_cap = 8U,
  k_node_cap    = 512U, /**< Roomy: the sibling-scan test walks 257 children. */
  k_attr_cap    = 64U,
  k_style_cap   = 4U,
  k_image_cap   = 4U,
  k_string_cap  = 4096U,
  k_imgpool_cap = 64U,
  k_out_cap     = 8192U,
} test_cap_t;

typedef enum : uint8_t {
  k_test_dummy_len = 10U, /**< Arbitrary nonzero length for the null-arg guards. */
  k_nested_b_idx   = 5U,  /**< Pre-order index of the "B" text node (last node). */
} test_idx_t;

ra8_book_chapter_t    s_chapters[k_chapter_cap];
ra8_book_node_t       s_nodes[k_node_cap];
ra8_book_attr_t       s_attrs[k_attr_cap];
ra8_book_stylesheet_t s_styles[k_style_cap];
ra8_book_image_t      s_images[k_image_cap];
char                  s_strpool[k_string_cap];
uint8_t               s_imgpool[k_imgpool_cap];
uint8_t               s_out[k_out_cap];

/**
 * @brief Node arena for the deep-nesting fixtures (#625).
 * @details The worst-case document at the tinyxml2 depth cap emits
 *          2 * (TINYXML2_MAX_ELEMENT_DEPTH - 2) == 996 nodes, nearly twice the
 *          shared @ref k_node_cap, so those tests get their own table.
 */
typedef enum : uint16_t {
  k_deep_node_cap = 1024U, /**< Covers the 996-node worst case with headroom. */
} deep_cap_t;

ra8_book_node_t s_deep_nodes[k_deep_node_cap];

uint32_t s_total = 0U;
uint32_t s_pass  = 0U;

void check(bool cond, const char* name)
{
  ++s_total;
  if (cond) {
    ++s_pass;
    std::printf("[PASS] %s\n", name);
  } else {
    std::printf("[FAIL] %s\n", name);
  }
}

/* Prepare a fresh builder context over the static arenas. */
ra8_rabook_ctx_t make_ctx()
{
  const ra8_rabook_buffers_t bufs = {
    .chapters       = s_chapters,
    .nodes          = s_nodes,
    .attrs          = s_attrs,
    .stylesheets    = s_styles,
    .images         = s_images,
    .string_pool    = s_strpool,
    .image_pool     = s_imgpool,
    .out            = s_out,
    .chapter_cap    = k_chapter_cap,
    .node_cap       = k_node_cap,
    .attr_cap       = k_attr_cap,
    .stylesheet_cap = k_style_cap,
    .image_cap      = k_image_cap,
    .string_cap     = k_string_cap,
    .image_pool_cap = k_imgpool_cap,
    .out_cap        = k_out_cap,
  };
  ra8_rabook_ctx_t ctx = {};
  (void)ra8_rabook_compile_init(&ctx, &bufs);
  return ctx;
}

/* Return the string-pool content at offset @p off. */
const char* pool_str(const ra8_rabook_ctx_t& ctx, uint32_t off)
{
  if (off == k_ra8_book_nil || off >= ctx.string_size) {
    return "<NIL>";
  }
  return ctx.buf.string_pool + off;
}

/* -------------------------------------------------------------------------- */
/* Fixtures */
/* -------------------------------------------------------------------------- */

constexpr const char* k_xhtml_empty_body = "<?xml version=\"1.0\"?><body></body>";

constexpr const char* k_xhtml_simple = "<?xml version=\"1.0\"?><body><p>Hello</p></body>";

/* Nested: body > div > p > "A", body > p > "B" */
constexpr const char* k_xhtml_nested = "<?xml version=\"1.0\"?>"
                                       "<body>"
                                       "<div><p>A</p></div>"
                                       "<p>B</p>"
                                       "</body>";

/* HTML wrapper: should fall through to <body> */
constexpr const char* k_xhtml_html_wrapper = "<?xml version=\"1.0\"?>"
                                             "<html><head/><body><p>Hi</p></body></html>";

/* Contains two XML comments: both should be silently skipped. */
constexpr const char* k_xhtml_with_comment = "<?xml version=\"1.0\"?>"
                                             "<body>"
                                             "<!-- first ignored -->"
                                             "<!-- second ignored -->"
                                             "<p>Visible</p>"
                                             "</body>";

/* A real CDATA section: tinyxml2 models it as an XMLText with CData()==true, so
 * internal_emit_node must DROP it (not emit a text node) per B's fix. The CDATA sits
 * before a real <p> so the visible element still lands as body's second walked
 * child. If CDATA were wrongly emitted, node_count would be 4 instead of 3. */
constexpr const char* k_xhtml_with_cdata = "<?xml version=\"1.0\"?>"
                                           "<body>"
                                           "<![CDATA[raw & < > not emitted]]>"
                                           "<p>After</p>"
                                           "</body>";

constexpr const char* k_xhtml_malformed = "<body><unclosed>";

/* Empty text (whitespace-only inside an element gets skipped when trimmed) */
constexpr const char* k_xhtml_empty_text = "<?xml version=\"1.0\"?><body><p></p></body>";

/* -------------------------------------------------------------------------- */
/* Individual tests */
/* -------------------------------------------------------------------------- */

void test_null_xhtml_bytes()
{
  ra8_rabook_ctx_t ctx = make_ctx();
  const ra8_err_t  err =
    ra8_rabook_xml_parse_chapter(nullptr, k_test_dummy_len, &ctx, "ch.xhtml", "T");
  check(err == k_ra8_err_null_ptr, "null xhtml_bytes -> null_ptr");
}

void test_null_ctx()
{
  const uint8_t          dummy = 0U;
  const ra8_rabook_ctx_t ctx   = make_ctx();
  const ra8_err_t        err   = ra8_rabook_xml_parse_chapter(&dummy, 1U, nullptr, "ch.xhtml", "T");
  (void)ctx;
  check(err == k_ra8_err_null_ptr, "null ctx -> null_ptr");
}

void test_null_href()
{
  ra8_rabook_ctx_t ctx   = make_ctx();
  const uint8_t    dummy = 0U;
  const ra8_err_t  err   = ra8_rabook_xml_parse_chapter(&dummy, 1U, &ctx, nullptr, "T");
  check(err == k_ra8_err_null_ptr, "null chapter_href -> null_ptr");
}

void test_null_title()
{
  ra8_rabook_ctx_t ctx   = make_ctx();
  const uint8_t    dummy = 0U;
  const ra8_err_t  err   = ra8_rabook_xml_parse_chapter(&dummy, 1U, &ctx, "ch.xhtml", nullptr);
  check(err == k_ra8_err_null_ptr, "null chapter_title -> null_ptr");
}

void test_malformed_xml()
{
  ra8_rabook_ctx_t ctx = make_ctx();
  const ra8_err_t  err =
    ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(k_xhtml_malformed),
                                 std::strlen(k_xhtml_malformed),
                                 &ctx,
                                 "ch.xhtml",
                                 "T");
  /* tinyxml2 fails to parse unclosed elements -> error returned */
  check(err != k_ra8_ok, "malformed XHTML returns error");
}

void test_empty_body()
{
  ra8_rabook_ctx_t ctx = make_ctx();
  const ra8_err_t  err =
    ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(k_xhtml_empty_body),
                                 std::strlen(k_xhtml_empty_body),
                                 &ctx,
                                 "empty.xhtml",
                                 "Empty");

  check(err == k_ra8_ok, "empty body: parse ok");
  /* One chapter, one node (the body root), no children. */
  check(ctx.chapter_count == 1U, "empty body: chapter_count == 1");
  check(ctx.node_count == 1U, "empty body: node_count == 1");
  check(std::strcmp(pool_str(ctx, ctx.buf.nodes[0].name_off), "body") == 0,
        "empty body: root tag is 'body'");
  check(ctx.buf.nodes[0].first_child == k_ra8_book_nil, "empty body: root has no children");
}

void test_simple_p_with_text()
{
  ra8_rabook_ctx_t ctx = make_ctx();
  const ra8_err_t  err =
    ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(k_xhtml_simple),
                                 std::strlen(k_xhtml_simple),
                                 &ctx,
                                 "simple.xhtml",
                                 "Simple");

  check(err == k_ra8_ok, "simple: parse ok");
  check(ctx.chapter_count == 1U, "simple: chapter_count == 1");
  /*
   * Expected DOM (pre-order):
   *   node[0] = body    (chapter root)
   *   node[1] = p       (first_child of body)
   *   node[2] = "Hello" (first_child of p)
   */
  check(ctx.node_count == 3U, "simple: node_count == 3");

  /* body is an element */
  check(ctx.buf.nodes[0].kind == (uint8_t)k_ra8_book_node_element, "simple: node[0] is element");
  check(std::strcmp(pool_str(ctx, ctx.buf.nodes[0].name_off), "body") == 0,
        "simple: node[0] tag is 'body'");
  check(ctx.buf.nodes[0].first_child == 1U, "simple: body.first_child == 1");

  /* p is an element */
  check(ctx.buf.nodes[1].kind == (uint8_t)k_ra8_book_node_element, "simple: node[1] is element");
  check(std::strcmp(pool_str(ctx, ctx.buf.nodes[1].name_off), "p") == 0,
        "simple: node[1] tag is 'p'");
  check(ctx.buf.nodes[1].first_child == 2U, "simple: p.first_child == 2");
  check(ctx.buf.nodes[1].next_sibling == k_ra8_book_nil, "simple: p.next_sibling == nil");

  /* "Hello" is a text node */
  check(ctx.buf.nodes[2].kind == (uint8_t)k_ra8_book_node_text, "simple: node[2] is text");
  check(std::strcmp(pool_str(ctx, ctx.buf.nodes[2].text_off), "Hello") == 0,
        "simple: node[2] text is 'Hello'");
  check(ctx.buf.nodes[2].next_sibling == k_ra8_book_nil, "simple: text.next_sibling == nil");
}

void test_nested_siblings_preorder()
{
  ra8_rabook_ctx_t ctx = make_ctx();
  const ra8_err_t  err =
    ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(k_xhtml_nested),
                                 std::strlen(k_xhtml_nested),
                                 &ctx,
                                 "nested.xhtml",
                                 "Nested");

  check(err == k_ra8_ok, "nested: parse ok");
  /*
   * Expected pre-order:
   *   node[0] = body  (root)
   *   node[1] = div   (body.first_child)
   *   node[2] = p     (div.first_child)
   *   node[3] = "A"   (p.first_child)
   *   node[4] = p2    (div.next_sibling)
   *   node[5] = "B"   (p2.first_child)
   */
  check(ctx.node_count == 6U, "nested: node_count == 6");

  /* body */
  check(ctx.buf.nodes[0].first_child == 1U, "nested: body.first_child == div (1)");

  /* div */
  check(std::strcmp(pool_str(ctx, ctx.buf.nodes[1].name_off), "div") == 0,
        "nested: node[1] is div");
  check(ctx.buf.nodes[1].first_child == 2U, "nested: div.first_child == p (2)");
  check(ctx.buf.nodes[1].next_sibling == 4U, "nested: div.next_sibling == p2 (4)");

  /* p under div */
  check(std::strcmp(pool_str(ctx, ctx.buf.nodes[2].name_off), "p") == 0, "nested: node[2] is p");
  check(ctx.buf.nodes[2].first_child == 3U, "nested: p.first_child == 'A' (3)");
  check(ctx.buf.nodes[2].next_sibling == k_ra8_book_nil, "nested: p.next_sibling == nil");

  /* "A" */
  check(ctx.buf.nodes[3].kind == (uint8_t)k_ra8_book_node_text, "nested: node[3] is text");
  check(std::strcmp(pool_str(ctx, ctx.buf.nodes[3].text_off), "A") == 0,
        "nested: node[3] text is 'A'");

  /* p2 (sibling of div) */
  check(std::strcmp(pool_str(ctx, ctx.buf.nodes[4].name_off), "p") == 0, "nested: node[4] is p2");
  check(ctx.buf.nodes[4].first_child == k_nested_b_idx, "nested: p2.first_child == 'B' (5)");
  check(ctx.buf.nodes[4].next_sibling == k_ra8_book_nil, "nested: p2.next_sibling == nil");

  /* "B" */
  check(std::strcmp(pool_str(ctx, ctx.buf.nodes[k_nested_b_idx].text_off), "B") == 0,
        "nested: node[5] text is 'B'");
}

void test_html_wrapper_body_fallback()
{
  ra8_rabook_ctx_t ctx = make_ctx();
  const ra8_err_t  err =
    ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(k_xhtml_html_wrapper),
                                 std::strlen(k_xhtml_html_wrapper),
                                 &ctx,
                                 "wrapped.xhtml",
                                 "Wrapped");

  check(err == k_ra8_ok, "html-wrapper: parse ok");
  /* The body fallback finds <body> inside <html>; root tag should be 'body'. */
  check(ctx.node_count >= 1U, "html-wrapper: at least one node");
  check(std::strcmp(pool_str(ctx, ctx.buf.nodes[0].name_off), "body") == 0,
        "html-wrapper: root tag is 'body'");
}

/**
 * @test test_comment_skipped
 * @brief Two XML comments are dropped; only <p>Visible</p> survives.
 *
 * @par MC/DC:
 * Exercises the @c text != nullptr / @c text->CData() path in internal_emit_node for
 * comment nodes: tinyxml2 models a comment as an XMLComment, so @c ToText()
 * returns null and the node is skipped without ever reaching add_text. Two
 * comments + one element + its text -> exactly 3 emitted nodes.
 */
void test_comment_skipped()
{
  ra8_rabook_ctx_t ctx = make_ctx();
  const ra8_err_t  err =
    ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(k_xhtml_with_comment),
                                 std::strlen(k_xhtml_with_comment),
                                 &ctx,
                                 "comment.xhtml",
                                 "Comment");

  check(err == k_ra8_ok, "comment: parse ok");
  /*
   * Expected: body (root), p (child), "Visible" (text).
   * Both comments are skipped entirely -> 3 nodes.
   */
  check(ctx.node_count == 3U, "comment: 3 nodes (two comments skipped)");
  check(std::strcmp(pool_str(ctx, ctx.buf.nodes[2].text_off), "Visible") == 0,
        "comment: text node is 'Visible'");
}

/**
 * @test test_cdata_skipped
 * @brief A <![CDATA[...]]> section is dropped, never emitted as a text node.
 *
 * @par MC/DC:
 * Covers the TRUE arm of @c if (text->CData()) in internal_emit_node (B's fix): the
 * CDATA node returns k_ra8_book_nil and is not added. The fixture is
 * @c <body><![CDATA[...]]><p>After</p></body>; if the CDATA were wrongly emitted
 * the node_count would be 4. Asserting node_count == 3 (body, p, "After") and
 * that no text node carries the CDATA content proves the skip. Complements the
 * FALSE arm (real text -> emitted) covered by test_simple_p_with_text.
 */
void test_cdata_skipped()
{
  ra8_rabook_ctx_t ctx = make_ctx();
  const ra8_err_t  err =
    ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(k_xhtml_with_cdata),
                                 std::strlen(k_xhtml_with_cdata),
                                 &ctx,
                                 "cdata.xhtml",
                                 "Cdata");

  check(err == k_ra8_ok, "cdata: parse ok");
  /*
   * Expected pre-order: body (root), p (body's 2nd walked child; the CDATA is
   * the 1st child but is skipped), "After" (p's text). The CDATA never lands.
   */
  check(ctx.node_count == 3U, "cdata: 3 nodes (CDATA skipped, not emitted)");
  check(ctx.buf.nodes[0].first_child == 1U, "cdata: body.first_child is the <p> (CDATA dropped)");
  check(std::strcmp(pool_str(ctx, ctx.buf.nodes[1].name_off), "p") == 0, "cdata: node[1] is 'p'");
  check(ctx.buf.nodes[2].kind == (uint8_t)k_ra8_book_node_text, "cdata: node[2] is text");
  check(std::strcmp(pool_str(ctx, ctx.buf.nodes[2].text_off), "After") == 0,
        "cdata: surviving text is 'After'");
  /* No emitted text node carries the CDATA payload. */
  bool cdata_leaked = false;
  for (uint32_t i = 0U; i < ctx.node_count; ++i) {
    if (ctx.buf.nodes[i].kind == (uint8_t)k_ra8_book_node_text) {
      if (std::strstr(pool_str(ctx, ctx.buf.nodes[i].text_off), "not emitted") != nullptr) {
        cdata_leaked = true;
      }
    }
  }
  check(!cdata_leaked, "cdata: payload never emitted as a text node");
}

/**
 * @test test_empty_text_skipped
 * @brief An empty <p></p> adds the element but no text node.
 *
 * @par MC/DC:
 * Covers the FALSE arm of the live @c val[0] != '\0' condition in the compound
 * @c val != nullptr && val[0] != '\0' decision (internal_emit_node): @c <p></p> yields
 * an empty Value() string, so @c val[0] == '\0' and no text node is added.
 * The TRUE arm (non-empty text emitted) is covered by test_simple_p_with_text.
 */
void test_empty_text_skipped()
{
  ra8_rabook_ctx_t ctx = make_ctx();
  const ra8_err_t  err =
    ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(k_xhtml_empty_text),
                                 std::strlen(k_xhtml_empty_text),
                                 &ctx,
                                 "empty_text.xhtml",
                                 "EmptyText");

  check(err == k_ra8_ok, "empty-text: parse ok");
  /* <p></p> -> body (root) + p (child); empty text node not added. */
  check(ctx.node_count == 2U, "empty-text: 2 nodes (empty text skipped)");
}

/* -------------------------------------------------------------------------- */
/* Edge fixtures for the loop-bound / overflow MC/DC legs */
/* -------------------------------------------------------------------------- */

typedef enum : uint16_t {
  k_sib_over_max  = 257U, /**< Root children > k_xhtml_max_siblings (256).    */
  k_attr_over_max = 33U,  /**< Attributes on one element > k_xhtml_max_attrs. */
  k_ovf_node_cap  = 3U,   /**< Tiny node cap: the 4th element fails to add.   */
} edge_dim_t;

/** @brief Build a builder context whose node table is capped at @p node_cap. */
ra8_rabook_ctx_t make_ctx_capped(uint32_t node_cap)
{
  const ra8_rabook_buffers_t bufs = {
    .chapters       = s_chapters,
    .nodes          = s_nodes,
    .attrs          = s_attrs,
    .stylesheets    = s_styles,
    .images         = s_images,
    .string_pool    = s_strpool,
    .image_pool     = s_imgpool,
    .out            = s_out,
    .chapter_cap    = k_chapter_cap,
    .node_cap       = node_cap,
    .attr_cap       = k_attr_cap,
    .stylesheet_cap = k_style_cap,
    .image_cap      = k_image_cap,
    .string_cap     = k_string_cap,
    .image_pool_cap = k_imgpool_cap,
    .out_cap        = k_out_cap,
  };
  ra8_rabook_ctx_t ctx = {};
  (void)ra8_rabook_compile_init(&ctx, &bufs);
  return ctx;
}

/**
 * @test test_find_body_many_siblings
 * @brief A root with more direct children than the sibling-scan bound exercises
 *        the loop-bound leg of internal_find_body's compound condition.
 *
 * @par MC/DC:
 * Drives the second-condition-false leg of `e != nullptr && tries <
 * k_xhtml_max_siblings` in internal_find_body: a `<root>` with 257 non-`<body>`
 * children makes `tries` reach 256, so the second condition is false while the
 * first is still true (C1 true, C2 false) and the scan gives up (returning the
 * root). The (true, true) scanning control and the (false, true) exhausted-scan
 * leg are supplied by the small fixtures; N+1 vectors complete. tinyxml2 does not
 * cap sibling breadth, so this flat 257-wide document is a reachable input.
 */
void test_find_body_many_siblings()
{
  ra8_rabook_ctx_t ctx = make_ctx();
  std::string      xml = "<?xml version=\"1.0\"?><root>";
  for (uint32_t i = 0U; i < (uint32_t)k_sib_over_max; ++i) {
    xml += "<c/>";
  }
  xml += "</root>";
  const ra8_err_t err = ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(xml.c_str()),
                                                     xml.size(),
                                                     &ctx,
                                                     "wide.xhtml",
                                                     "Wide");
  /* No <body> among 257 children -> internal_find_body gives up on the scan bound and
   * returns the root; the walk then serialises it without error. */
  check(err == k_ra8_ok, "many-siblings: parse ok (scan-bound leg)");
  check(ctx.chapter_count == 1U, "many-siblings: one chapter");
}

/**
 * @test test_collect_attrs_overflow
 * @brief An element carrying more attributes than the collect bound exercises the
 *        loop-bound leg of internal_collect_attrs and latches the builder failure.
 *
 * @par MC/DC:
 * Drives the second-condition-false leg of `a != nullptr && count <
 * k_xhtml_max_attrs` in internal_collect_attrs: a `<p>` with 33 attributes makes `count`
 * reach 32, so the second condition is false while more attributes remain (C1
 * true, C2 false); the helper then latches ctx->failed so finalize surfaces the
 * overflow. The (true, true) collecting control (an element with a few
 * attributes) and the (false, true) no-more-attributes leg are supplied here and
 * by the attribute-bearing fixture. tinyxml2 does not cap attribute count, so a
 * 33-attribute element is a reachable input.
 */
void test_collect_attrs_overflow()
{
  ra8_rabook_ctx_t ctx = make_ctx();
  std::string      xml = "<?xml version=\"1.0\"?><body><p";
  for (uint32_t i = 0U; i < (uint32_t)k_attr_over_max; ++i) {
    xml += " a" + std::to_string(i) + "=\"v\"";
  }
  xml += ">x</p></body>";
  const ra8_err_t err = ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(xml.c_str()),
                                                     xml.size(),
                                                     &ctx,
                                                     "attrs.xhtml",
                                                     "Attrs");
  /* The overflow latches the sticky builder failure, so finalize (via
   * add_chapter) reports it out of the parse as no_mem. */
  check(err == k_ra8_err_no_mem, "collect-attrs: overflow latches builder failure");
}

/**
 * @test test_walk_builder_overflow
 * @brief An element whose add fails (node table full) exercises the emit-failure
 *        leg of the subtree walk's link/descend condition.
 *
 * @par MC/DC:
 * Drives the second-condition-false leg of `elem != nullptr && new_idx !=
 * k_ra8_book_nil` in internal_walk_body_subtree: with the node table capped at 3, the
 * nested `<span>` in `<body><div><p><span>x</span></p></div></body>` is the
 * fourth element and ra8_rabook_add_element returns nil for it, so the second
 * condition is false while the node is still an element (C1 true, C2 false) and
 * its children are not descended into. The (true, true) element-emitted control
 * and the (false, true) text-node leg are supplied by the nested-siblings
 * fixture. A book that overruns the builder node budget is a reachable input.
 */
void test_walk_builder_overflow()
{
  ra8_rabook_ctx_t      ctx = make_ctx_capped((uint32_t)k_ovf_node_cap);
  constexpr const char* k_deep =
    "<?xml version=\"1.0\"?><body><div><p><span>x</span></p></div></body>";
  const ra8_err_t err = ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(k_deep),
                                                     std::strlen(k_deep),
                                                     &ctx,
                                                     "deep.xhtml",
                                                     "Deep");
  /* body(0), div(1), p(2) fill the 3-node table; <span> fails to add (nil),
   * so the walk takes the new_idx == nil leg and the compile reports no_mem. */
  check(err == k_ra8_err_no_mem, "walk-overflow: element add-nil leg (node cap hit)");
}

/* -------------------------------------------------------------------------- */
/* Deep nesting: the tinyxml2 element-depth cap (#625) */
/* -------------------------------------------------------------------------- */

/** @brief Build a builder context over the roomy deep-nesting node arena. */
ra8_rabook_ctx_t make_ctx_deep()
{
  const ra8_rabook_buffers_t bufs = {
    .chapters       = s_chapters,
    .nodes          = s_deep_nodes,
    .attrs          = s_attrs,
    .stylesheets    = s_styles,
    .images         = s_images,
    .string_pool    = s_strpool,
    .image_pool     = s_imgpool,
    .out            = s_out,
    .chapter_cap    = k_chapter_cap,
    .node_cap       = k_deep_node_cap,
    .attr_cap       = k_attr_cap,
    .stylesheet_cap = k_style_cap,
    .image_cap      = k_image_cap,
    .string_cap     = k_string_cap,
    .image_pool_cap = k_imgpool_cap,
    .out_cap        = k_out_cap,
  };
  ra8_rabook_ctx_t ctx = {};
  (void)ra8_rabook_compile_init(&ctx, &bufs);
  return ctx;
}

/**
 * @brief Build the deep chapter shape that maximises the DFS frame stack.
 * @details `<body>` is the ROOT element (absolute depth 1 -- the shallowest
 *          placement, which leaves the most room below it), with @p depth levels
 *          of `<d>` beneath it and an `<s/>` sibling after every `</d>`.  The
 *          sibling at each level is what drives the stack: the walk pushes a
 *          level's next_sibling before descending into its first_child, so each
 *          descent leaves one frame parked and the high-water mark reaches
 *          exactly @p depth.  Without those siblings the same nesting peaks at a
 *          single frame.  The innermost element is self-closing because tinyxml2
 *          only charges depth for elements it descends INTO, so a self-closing
 *          leaf reaches one level deeper than a leaf with content.
 * @param[in] depth Number of nested `<d>` levels below `<body>` (>= 1).
 * @return The XHTML document text.
 * @pre @p depth is at least 1.
 * @post The result contains 2 * @p depth elements including `<body>`.
 */
std::string make_deep_doc(int depth)
{
  std::string s = "<?xml version=\"1.0\"?><body>";
  for (int i = 0; i < depth - 1; ++i) {
    s += "<d>";
  }
  s += "<d/>";
  for (int i = 0; i < depth - 1; ++i) {
    s += "</d><s/>";
  }
  s += "</body>";
  return s;
}

/**
 * @test test_deep_at_tinyxml2_cap
 * @brief The deepest document tinyxml2 accepts compiles with nothing truncated.
 *
 * @par MC/DC:
 * This is the non-vacuity witness for the `top < k_xhtml_max_stack`
 * deactivation in internal_push_frame, which claims the condition cannot go false on
 * any reachable input. It drives the walk to its provable worst case -- a frame
 * high-water mark of TINYXML2_MAX_ELEMENT_DEPTH - 2 == 498 against a capacity of
 * 512 -- and checks the emitted node count exactly. A dropped frame loses an
 * entire subtree, so an exact count is what distinguishes "the guard never
 * fired" from "the guard fired and the chapter was silently truncated". Before
 * #625 the deactivation cited a cap of 100 and a 2 * depth bound; under those
 * numbers with the real cap this document would have demanded 1000 frames and
 * overrun the stack, so this case is precisely the one the false rationale
 * hid.
 */
void test_deep_at_tinyxml2_cap()
{
  /* tinyxml2 aborts once tracked nesting reaches TINYXML2_MAX_ELEMENT_DEPTH; the
   * document node costs one level and <body> another, leaving CAP - 2 below. */
  const int         depth = TINYXML2_MAX_ELEMENT_DEPTH - 2;
  const std::string doc   = make_deep_doc(depth);
  ra8_rabook_ctx_t  ctx   = make_ctx_deep();
  const ra8_err_t err = ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(doc.c_str()),
                                                     doc.size(),
                                                     &ctx,
                                                     "deep.xhtml",
                                                     "Deep");

  check(err == k_ra8_ok, "deep-at-cap: deepest accepted document compiles");
  check(!ctx.failed, "deep-at-cap: builder latched no failure");
  /* <body> + depth <d> + (depth - 1) <s/> == 2 * depth nodes. */
  check(ctx.node_count == (uint32_t)(2 * depth),
        "deep-at-cap: every node emitted (no silent truncation)");
}

/**
 * @test test_deep_beyond_tinyxml2_cap
 * @brief One level past the cap is rejected by the parser, never truncated.
 *
 * @par MC/DC:
 * The other side of the cap, and the reason the internal_push_frame safety valve stays
 * unreachable: tinyxml2 refuses the document outright with
 * XML_ELEMENT_DEPTH_EXCEEDED, so the walk never starts and the shim returns an
 * error rather than emitting a partial chapter. Together with
 * test_deep_at_tinyxml2_cap this brackets the true cap from both sides, which is
 * the evidence the deactivation rationale now cites in place of the false
 * cap-of-100 claim.
 */
void test_deep_beyond_tinyxml2_cap()
{
  const int         depth = TINYXML2_MAX_ELEMENT_DEPTH - 1;
  const std::string doc   = make_deep_doc(depth);
  ra8_rabook_ctx_t  ctx   = make_ctx_deep();
  const ra8_err_t err = ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(doc.c_str()),
                                                     doc.size(),
                                                     &ctx,
                                                     "deeper.xhtml",
                                                     "Deeper");

  check(err == k_ra8_err_no_mem, "deep-beyond-cap: parser rejects the document");
  check(ctx.node_count == 0U, "deep-beyond-cap: nothing emitted (rejected, not truncated)");
}

/* -------------------------------------------------------------------------- */
/* Log sink redirect (avoid ITM hardware access on host) */
/* -------------------------------------------------------------------------- */

RA8_INTERNAL static void internal_log_sink(void* /*ctx*/, uint8_t byte)
{
  (void)std::fputc(static_cast<int>(byte), stderr);
}

} // namespace

/* -------------------------------------------------------------------------- */
/* main */
/* -------------------------------------------------------------------------- */

int main()
{
  ra8_log_set_byte_sink(internal_log_sink, nullptr);
  std::printf("=== test_ra8_rabook_xml_shim ===\n");

  test_null_xhtml_bytes();
  test_null_ctx();
  test_null_href();
  test_null_title();
  test_malformed_xml();
  test_empty_body();
  test_simple_p_with_text();
  test_nested_siblings_preorder();
  test_html_wrapper_body_fallback();
  test_comment_skipped();
  test_cdata_skipped();
  test_empty_text_skipped();
  test_find_body_many_siblings();
  test_collect_attrs_overflow();
  test_walk_builder_overflow();
  test_deep_at_tinyxml2_cap();
  test_deep_beyond_tinyxml2_cap();

  std::printf("\n%s: %u/%u passed\n",
              (s_pass == s_total) ? "[PASS] ra8_rabook_xml_shim" : "[FAIL] ra8_rabook_xml_shim",
              s_pass,
              s_total);
  return (s_pass == s_total) ? 0 : 1;
}
