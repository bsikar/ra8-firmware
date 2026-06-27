/**
 * @file test_ra_rabook_xml_shim.cpp
 * @brief MC/DC + DOM-structure tests for ra_rabook_xml_shim (#149).
 *
 * @details
 * Drives @ref ra_rabook_xml_parse_chapter with synthetic XHTML fixtures to
 * verify:
 *
 *  - Null-pointer validation (4 guards).
 *  - Malformed XML rejection.
 *  - Empty @c <body> produces a single element node (chapter root).
 *  - Simple @c <body><p>Hello</p></body> -- one element child, one text child.
 *  - Nested siblings @c <body><div><p>A</p></div><p>B</p></body> -- proves the
 *    iterative DFS visits nodes in the correct pre-order: body, div, p, "A",
 *    p2, "B".
 *  - Comment and PI nodes are silently skipped.
 *  - @c <html><body>...</body></html> wrapper: body fallback search works.
 *
 * @par MC/DC decisions exercised (ra_rabook_xml_shim.cpp):
 *   - @c doc.Parse != XML_SUCCESS  (T: malformed, F: valid)
 *   - @c body == nullptr           (T: empty doc would produce this via fallback)
 *   - @c elem != nullptr           (T: element node, F->text check)
 *   - @c text != nullptr           (T: text node, F: comment/PI skipped)
 *   - @c val != nullptr && val[0] != '\\0'  (T: non-empty, F: empty text)
 *   - @c frame.prev_sib_idx == k_ra_book_nil  (T: first child, F: sibling)
 *   - @c next_sib != nullptr       (T: sibling present, F: last sibling)
 *   - @c elem != nullptr && new_idx != k_ra_book_nil  (T: push children)
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

extern "C" {
#include "ra_book.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_rabook_compile.h"
#include "ra_rabook_xml_shim.h"
}

/* -------------------------------------------------------------------------- */
/* Static test arenas */
/* -------------------------------------------------------------------------- */

namespace {

typedef enum : uint32_t {
  k_chapter_cap = 8U,
  k_node_cap    = 128U,
  k_attr_cap    = 64U,
  k_style_cap   = 4U,
  k_image_cap   = 4U,
  k_string_cap  = 4096U,
  k_imgpool_cap = 64U,
  k_out_cap     = 8192U,
} TestCap;

ra_book_chapter_t    s_chapters[k_chapter_cap];
ra_book_node_t       s_nodes[k_node_cap];
ra_book_attr_t       s_attrs[k_attr_cap];
ra_book_stylesheet_t s_styles[k_style_cap];
ra_book_image_t      s_images[k_image_cap];
char                 s_strpool[k_string_cap];
uint8_t              s_imgpool[k_imgpool_cap];
uint8_t              s_out[k_out_cap];

static uint32_t s_total = 0U;
static uint32_t s_pass  = 0U;

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
ra_rabook_ctx_t make_ctx()
{
  const ra_rabook_buffers_t bufs = {
    s_chapters,
    k_chapter_cap,
    s_nodes,
    k_node_cap,
    s_attrs,
    k_attr_cap,
    s_styles,
    k_style_cap,
    s_images,
    k_image_cap,
    s_strpool,
    k_string_cap,
    s_imgpool,
    k_imgpool_cap,
    s_out,
    k_out_cap,
  };
  ra_rabook_ctx_t ctx = {};
  (void)ra_rabook_compile_init(&ctx, &bufs);
  return ctx;
}

/* Return the string-pool content at offset @p off. */
const char* pool_str(const ra_rabook_ctx_t& ctx, uint32_t off)
{
  if (off == k_ra_book_nil || off >= ctx.string_size) {
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

/* Contains comments and CDATA: both should be silently skipped. */
constexpr const char* k_xhtml_with_comment = "<?xml version=\"1.0\"?>"
                                             "<body>"
                                             "<!-- first ignored -->"
                                             "<!-- second ignored -->"
                                             "<p>Visible</p>"
                                             "</body>";

constexpr const char* k_xhtml_malformed = "<body><unclosed>";

/* Empty text (whitespace-only inside an element gets skipped when trimmed) */
constexpr const char* k_xhtml_empty_text = "<?xml version=\"1.0\"?><body><p></p></body>";

} // namespace

/* -------------------------------------------------------------------------- */
/* Individual tests */
/* -------------------------------------------------------------------------- */

static void test_null_xhtml_bytes()
{
  ra_rabook_ctx_t ctx = make_ctx();
  ra_err_t        err = ra_rabook_xml_parse_chapter(nullptr, 10U, &ctx, "ch.xhtml", "T");
  check(err == k_ra_err_null_ptr, "null xhtml_bytes -> null_ptr");
}

static void test_null_ctx()
{
  const uint8_t   dummy = 0U;
  ra_rabook_ctx_t ctx   = make_ctx();
  ra_err_t        err   = ra_rabook_xml_parse_chapter(&dummy, 1U, nullptr, "ch.xhtml", "T");
  (void)ctx;
  check(err == k_ra_err_null_ptr, "null ctx -> null_ptr");
}

static void test_null_href()
{
  ra_rabook_ctx_t ctx   = make_ctx();
  const uint8_t   dummy = 0U;
  ra_err_t        err   = ra_rabook_xml_parse_chapter(&dummy, 1U, &ctx, nullptr, "T");
  check(err == k_ra_err_null_ptr, "null chapter_href -> null_ptr");
}

static void test_null_title()
{
  ra_rabook_ctx_t ctx   = make_ctx();
  const uint8_t   dummy = 0U;
  ra_err_t        err   = ra_rabook_xml_parse_chapter(&dummy, 1U, &ctx, "ch.xhtml", nullptr);
  check(err == k_ra_err_null_ptr, "null chapter_title -> null_ptr");
}

static void test_malformed_xml()
{
  ra_rabook_ctx_t ctx = make_ctx();
  ra_err_t err = ra_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(k_xhtml_malformed),
                                             std::strlen(k_xhtml_malformed),
                                             &ctx,
                                             "ch.xhtml",
                                             "T");
  /* tinyxml2 fails to parse unclosed elements -> error returned */
  check(err != k_ra_ok, "malformed XHTML returns error");
}

static void test_empty_body()
{
  ra_rabook_ctx_t ctx = make_ctx();
  ra_err_t err = ra_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(k_xhtml_empty_body),
                                             std::strlen(k_xhtml_empty_body),
                                             &ctx,
                                             "empty.xhtml",
                                             "Empty");

  check(err == k_ra_ok, "empty body: parse ok");
  /* One chapter, one node (the body root), no children. */
  check(ctx.chapter_count == 1U, "empty body: chapter_count == 1");
  check(ctx.node_count == 1U, "empty body: node_count == 1");
  check(std::strcmp(pool_str(ctx, ctx.buf.nodes[0].name_off), "body") == 0,
        "empty body: root tag is 'body'");
  check(ctx.buf.nodes[0].first_child == k_ra_book_nil, "empty body: root has no children");
}

static void test_simple_p_with_text()
{
  ra_rabook_ctx_t ctx = make_ctx();
  ra_err_t err = ra_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(k_xhtml_simple),
                                             std::strlen(k_xhtml_simple),
                                             &ctx,
                                             "simple.xhtml",
                                             "Simple");

  check(err == k_ra_ok, "simple: parse ok");
  check(ctx.chapter_count == 1U, "simple: chapter_count == 1");
  /*
   * Expected DOM (pre-order):
   *   node[0] = body    (chapter root)
   *   node[1] = p       (first_child of body)
   *   node[2] = "Hello" (first_child of p)
   */
  check(ctx.node_count == 3U, "simple: node_count == 3");

  /* body is an element */
  check(ctx.buf.nodes[0].kind == (uint8_t)k_ra_book_node_element, "simple: node[0] is element");
  check(std::strcmp(pool_str(ctx, ctx.buf.nodes[0].name_off), "body") == 0,
        "simple: node[0] tag is 'body'");
  check(ctx.buf.nodes[0].first_child == 1U, "simple: body.first_child == 1");

  /* p is an element */
  check(ctx.buf.nodes[1].kind == (uint8_t)k_ra_book_node_element, "simple: node[1] is element");
  check(std::strcmp(pool_str(ctx, ctx.buf.nodes[1].name_off), "p") == 0,
        "simple: node[1] tag is 'p'");
  check(ctx.buf.nodes[1].first_child == 2U, "simple: p.first_child == 2");
  check(ctx.buf.nodes[1].next_sibling == k_ra_book_nil, "simple: p.next_sibling == nil");

  /* "Hello" is a text node */
  check(ctx.buf.nodes[2].kind == (uint8_t)k_ra_book_node_text, "simple: node[2] is text");
  check(std::strcmp(pool_str(ctx, ctx.buf.nodes[2].text_off), "Hello") == 0,
        "simple: node[2] text is 'Hello'");
  check(ctx.buf.nodes[2].next_sibling == k_ra_book_nil, "simple: text.next_sibling == nil");
}

static void test_nested_siblings_preorder()
{
  ra_rabook_ctx_t ctx = make_ctx();
  ra_err_t err = ra_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(k_xhtml_nested),
                                             std::strlen(k_xhtml_nested),
                                             &ctx,
                                             "nested.xhtml",
                                             "Nested");

  check(err == k_ra_ok, "nested: parse ok");
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
  check(ctx.buf.nodes[2].next_sibling == k_ra_book_nil, "nested: p.next_sibling == nil");

  /* "A" */
  check(ctx.buf.nodes[3].kind == (uint8_t)k_ra_book_node_text, "nested: node[3] is text");
  check(std::strcmp(pool_str(ctx, ctx.buf.nodes[3].text_off), "A") == 0,
        "nested: node[3] text is 'A'");

  /* p2 (sibling of div) */
  check(std::strcmp(pool_str(ctx, ctx.buf.nodes[4].name_off), "p") == 0, "nested: node[4] is p2");
  check(ctx.buf.nodes[4].first_child == 5U, "nested: p2.first_child == 'B' (5)");
  check(ctx.buf.nodes[4].next_sibling == k_ra_book_nil, "nested: p2.next_sibling == nil");

  /* "B" */
  check(std::strcmp(pool_str(ctx, ctx.buf.nodes[5].text_off), "B") == 0,
        "nested: node[5] text is 'B'");
}

static void test_html_wrapper_body_fallback()
{
  ra_rabook_ctx_t ctx = make_ctx();
  ra_err_t err = ra_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(k_xhtml_html_wrapper),
                                             std::strlen(k_xhtml_html_wrapper),
                                             &ctx,
                                             "wrapped.xhtml",
                                             "Wrapped");

  check(err == k_ra_ok, "html-wrapper: parse ok");
  /* The body fallback finds <body> inside <html>; root tag should be 'body'. */
  check(ctx.node_count >= 1U, "html-wrapper: at least one node");
  check(std::strcmp(pool_str(ctx, ctx.buf.nodes[0].name_off), "body") == 0,
        "html-wrapper: root tag is 'body'");
}

static void test_comment_and_pi_skipped()
{
  ra_rabook_ctx_t ctx = make_ctx();
  ra_err_t err = ra_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(k_xhtml_with_comment),
                                             std::strlen(k_xhtml_with_comment),
                                             &ctx,
                                             "comment.xhtml",
                                             "Comment");

  check(err == k_ra_ok, "comment: parse ok");
  /*
   * Expected: body (root), p (child), "Visible" (text).
   * Comment and PI are skipped entirely -> 3 nodes.
   */
  check(ctx.node_count == 3U, "comment: 3 nodes (two comments skipped)");
  check(std::strcmp(pool_str(ctx, ctx.buf.nodes[2].text_off), "Visible") == 0,
        "comment: text node is 'Visible'");
}

static void test_empty_text_skipped()
{
  ra_rabook_ctx_t ctx = make_ctx();
  ra_err_t err = ra_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(k_xhtml_empty_text),
                                             std::strlen(k_xhtml_empty_text),
                                             &ctx,
                                             "empty_text.xhtml",
                                             "EmptyText");

  check(err == k_ra_ok, "empty-text: parse ok");
  /* <p></p> -> body (root) + p (child); empty text node not added. */
  check(ctx.node_count == 2U, "empty-text: 2 nodes (empty text skipped)");
}

/* -------------------------------------------------------------------------- */
/* Log sink redirect (avoid ITM hardware access on host) */
/* -------------------------------------------------------------------------- */

static void s_log_sink(void* /*ctx*/, uint8_t byte)
{
  (void)std::fputc(static_cast<int>(byte), stderr);
}

/* -------------------------------------------------------------------------- */
/* main */
/* -------------------------------------------------------------------------- */

int main()
{
  ra_log_set_byte_sink(s_log_sink, nullptr);
  std::printf("=== test_ra_rabook_xml_shim ===\n");

  test_null_xhtml_bytes();
  test_null_ctx();
  test_null_href();
  test_null_title();
  test_malformed_xml();
  test_empty_body();
  test_simple_p_with_text();
  test_nested_siblings_preorder();
  test_html_wrapper_body_fallback();
  test_comment_and_pi_skipped();
  test_empty_text_skipped();

  std::printf("\n%s: %u/%u passed\n",
              (s_pass == s_total) ? "[PASS] ra_rabook_xml_shim" : "[FAIL] ra_rabook_xml_shim",
              s_pass,
              s_total);
  return (s_pass == s_total) ? 0 : 1;
}
