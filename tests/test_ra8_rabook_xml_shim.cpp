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
 *  - Deep nesting at the bounded reader element-depth cap: the deepest
 *    accepted document compiles with every node emitted -- the DFS
 *    frame stack is never overrun -- and one level deeper is rejected by the
 *    parser rather than silently truncated.
 *
 * @par MC/DC decisions exercised (ra8_rabook_xml_shim.cpp):
 *   - @c doc.Parse(...) != XML_SUCCESS -- single condition.
 *       T: internal_test_malformed_xml (unclosed element). F: every well-formed fixture.
 *   - @c root == nullptr in internal_find_body -- single condition. F: every fixture has
 *       a root element. T is unreachable through the public entry point (a
 *       document with no root makes doc.Parse fail first), so it is a defensive
 *       guard with no independent-influence vector (documented, not testable).
 *   - @c std::strcmp(e->Name(), "body") == 0 in internal_find_body -- single condition.
 *       T: internal_test_html_wrapper_body_fallback (<body> found under <html>).
 *       F: the same scan steps past <head/> before matching <body>.
 *   - @c elem != nullptr in internal_emit_node -- single condition.
 *       T: any element fixture. F: text/CDATA fixtures fall through to ToText().
 *   - @c text->CData() in internal_emit_node -- single condition.
 *       T: internal_test_cdata_skipped (CDATA dropped). F: any real text node.
 *   - @c val != nullptr && val[0] != '\0' in internal_emit_node -- COMPOUND AND, but the
 *       first condition is structurally always true: it is reached only after
 *       a non-empty text event was produced by the validated pull reader. So
 *       @c val != nullptr cannot be observed false on any reachable input and has
 *       no independent-influence vector (defensive guard, same class as the
 *       mcdc-deactivated NULL guards in ra8_epub_xml_shim.cpp). The second
 *       condition @c val[0] != '\0' is the live one and BOTH its arms are covered:
 *         - true  -> text emitted: internal_test_simple_p_with_text ("Hello").
 *         - false -> text skipped: internal_test_empty_text_skipped (<p></p>, val[0]=='\0').
 *   - @c node != nullptr && top < k_xhtml_max_stack in internal_push_frame -- COMPOUND
 *       AND. The first condition is live (a null FirstChild/NextSibling stops a
 *       branch): T covered by any fixture with children, F by leaf/last-sibling
 *       nodes (internal_test_nested_siblings_preorder exercises both). The second is the
 *       NASA Rule 2 safety valve and cannot go false: the frame high-water mark
 *       is the node-depth of the deepest node below @c <body> (depth + 1, not
 *       2 * depth -- a sibling step pops one frame and pushes one), and the
 *       reader rejects nesting beyond @c k_ra8_xml_max_element_depth.
 *       internal_test_deep_at_reader_cap pins that worst
 *       case and internal_test_deep_beyond_reader_cap pins the rejection one level
 *       deeper, so the deactivation rests on a measurement rather than on the
 *       false cap-of-100 claim it carried before #625.
 *   - @c frame.prev_sib_idx == k_ra8_book_nil in internal_walk_body_subtree -- single
 *       condition. T (first child) and F (later sibling) both in
 *       internal_test_nested_siblings_preorder.
 *   - @c elem != nullptr && new_idx != k_ra8_book_nil in internal_walk_body_subtree --
 *       COMPOUND AND. elem!=nullptr varies (element vs text node) and
 *       new_idx!=nil varies (emitted vs skipped); internal_test_nested_siblings_preorder
 *       supplies element-with-children (both true -> push children) and text
 *       leaves (elem false -> no push), and internal_test_cdata_skipped supplies a skipped
 *       node (new_idx nil -> no push).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <cassert>
#include <cstddef>
#include <cstdint>
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

/* The deep-nesting fixtures derive from the production reader contract. */
#include "ra8_xml.h"

/* -------------------------------------------------------------------------- */
/* Static test arenas */
/* -------------------------------------------------------------------------- */

namespace {

static ra8_rabook_xml_workspace_t s_xml_workspace = {};

/** @brief Provide the file-local parse chapter test helper. @details Implements the parse chapter fixture operation used only by this focused test executable. @param[in] bytes Fixture argument governed by the exercised interface contract. @param[in] length Fixture argument governed by the exercised interface contract. @param[in,out] ctx Fixture argument governed by the exercised interface contract. @param[in] href Fixture argument governed by the exercised interface contract. @param[in] title Fixture argument governed by the exercised interface contract. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_parse_chapter(const uint8_t*    bytes,
                                                     size_t            length,
                                                     ra8_rabook_ctx_t* ctx,
                                                     const char*       href,
                                                     const char*       title)
{
  return ra8_rabook_xml_parse_chapter(bytes, length, ctx, href, title, &s_xml_workspace);
}

/** @brief Call the production parser with an explicitly chosen workspace. @details The object-like macro below rewrites every later call site to the five-argument wrapper, so the workspace guard needs an entry point declared before it that still forwards the caller's own pointer. @param[in] bytes Immutable XHTML bytes. @param[in] length Readable extent of the source. @param[in,out] ctx Builder receiving one chapter. @param[in] href Chapter identity string. @param[in] title Chapter title string. @param[in,out] workspace Caller workspace, or NULL for the workspace guard. @return RA8 status from the production parser. @retval k_ra8_ok The chapter was appended. @retval k_ra8_err_null_ptr A required pointer was NULL. @pre Fixed-capacity fixture storage required by this operation is available. @pre Non-NULL arguments follow the production interface contract. @post The production result is returned unchanged. @post No file-local fixture state is modified by the wrapper. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_parse_chapter_ws(const uint8_t*              bytes,
                                                        size_t                      length,
                                                        ra8_rabook_ctx_t*           ctx,
                                                        const char*                 href,
                                                        const char*                 title,
                                                        ra8_rabook_xml_workspace_t* workspace)
{
  return ra8_rabook_xml_parse_chapter(bytes, length, ctx, href, title, workspace);
}

/** @brief Route legacy call sites through the test-owned XML workspace wrapper. */
#define ra8_rabook_xml_parse_chapter internal_parse_chapter

typedef enum : uint16_t {
  k_chapter_cap = 8U,    /**< Maximum chapters retained by the fixture context.  */
  k_node_cap    = 512U,  /**< Roomy: the sibling-scan test walks 257 children.   */
  k_attr_cap    = 64U,   /**< Maximum attributes retained across emitted nodes.  */
  k_style_cap   = 4U,    /**< Maximum stylesheet records in the fixture context. */
  k_image_cap   = 4U,    /**< Maximum image records in the fixture context.      */
  k_string_cap  = 4096U, /**< Bytes reserved for emitted strings.                */
  k_imgpool_cap = 64U,   /**< Bytes reserved for fixture image payloads.         */
  k_out_cap     = 8192U, /**< Bytes reserved for compiled chapter output.        */
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
 * @details The worst-case document at the reader depth cap emits nearly twice
 *          the shared @ref k_node_cap, so those tests get their own table.
 */
typedef enum : uint16_t {
  k_deep_node_cap = 1024U, /**< Covers the 996-node worst case with headroom. */
} deep_cap_t;

ra8_book_node_t s_deep_nodes[k_deep_node_cap];

uint32_t s_total = 0U;
uint32_t s_pass  = 0U;

/** @brief Provide the file-local check test helper. @details Implements the check fixture operation used only by this focused test executable. @param[in] cond Fixture argument governed by the exercised interface contract. @param[in] name Fixture argument governed by the exercised interface contract. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_check(bool cond, const char* name)
{
  (void)name;
  ++s_total;
  if (cond) {
    ++s_pass;
  }
}

/* Prepare a fresh builder context over the static arenas. */
/** @brief Prepare the fixture's make ctx state. @details Implements the make ctx fixture operation used only by this focused test executable. @return The value computed by the fixture helper. @retval value The computed fixture value for the supplied inputs. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static ra8_rabook_ctx_t internal_make_ctx()
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
RA8_INTERNAL static const char* internal_pool_str(const ra8_rabook_ctx_t& ctx, uint32_t off)
{
  if (off == k_ra8_book_nil || off >= ctx.string_size) {
    return "<NIL>";
  }
  return ctx.buf.string_pool + off;
}

/** @brief Compare all logical builder state that a failed parse must restore. @details Implements the same ctx state fixture operation used only by this focused test executable. @param[in] lhs Fixture argument governed by the exercised interface contract. @param[in] rhs Fixture argument governed by the exercised interface contract. @return Whether the named fixture condition holds. @retval true The named fixture condition holds. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static bool internal_same_ctx_state(const ra8_rabook_ctx_t& lhs,
                                                 const ra8_rabook_ctx_t& rhs)
{
  return (lhs.chapter_count == rhs.chapter_count) && (lhs.node_count == rhs.node_count) &&
         (lhs.attr_count == rhs.attr_count) && (lhs.stylesheet_count == rhs.stylesheet_count) &&
         (lhs.image_count == rhs.image_count) && (lhs.string_size == rhs.string_size) &&
         (lhs.image_pool_size == rhs.image_pool_size) && (lhs.title_off == rhs.title_off) &&
         (lhs.author_off == rhs.author_off) && (lhs.language_off == rhs.language_off) &&
         (lhs.identifier_off == rhs.identifier_off) &&
         (lhs.cover_image_index == rhs.cover_image_index) && (lhs.flags == rhs.flags) &&
         (lhs.image_pool_mode == rhs.image_pool_mode) && (lhs.failed == rhs.failed);
}

/* -------------------------------------------------------------------------- */
/* Fixtures */
/* -------------------------------------------------------------------------- */

constexpr const char* s_xhtml_empty_body = "<?xml version=\"1.0\"?><body></body>";

constexpr const char* s_xhtml_simple =
  "<?xml version=\"1.0\"?>"
  "<!DOCTYPE body PUBLIC '-//W3C//DTD XHTML 1.1//EN' 'xhtml11.dtd'>"
  "<body><p>Hello</p></body>";

/* Nested: body > div > p > "A", body > p > "B" */
constexpr const char* s_xhtml_nested = "<?xml version=\"1.0\"?>"
                                       "<body>"
                                       "<div><p>A</p></div>"
                                       "<p>B</p>"
                                       "</body>";

/* HTML wrapper: should fall through to <body> */
constexpr const char* s_xhtml_html_wrapper = "<?xml version=\"1.0\"?>"
                                             "<html><head/><body><p>Hi</p></body></html>";

/* Contains two XML comments: both should be silently skipped. */
constexpr const char* s_xhtml_with_comment = "<?xml version=\"1.0\"?>"
                                             "<body>"
                                             "<!-- first ignored -->"
                                             "<!-- second ignored -->"
                                             "<p>Visible</p>"
                                             "</body>";

/* A real CDATA section: the production event policy drops it rather than
 * emitting a text node. The CDATA sits
 * before a real <p> so the visible element still lands as body's second walked
 * child. If CDATA were wrongly emitted, node_count would be 4 instead of 3. */
constexpr const char* s_xhtml_with_cdata = "<?xml version=\"1.0\"?>"
                                           "<body>"
                                           "<![CDATA[raw & < > not emitted]]>"
                                           "<p>After</p>"
                                           "</body>";

constexpr const char* s_xhtml_malformed = "<body><unclosed>";

/* Empty text (whitespace-only inside an element gets skipped when trimmed) */
constexpr const char* s_xhtml_empty_text = "<?xml version=\"1.0\"?><body><p></p></body>";

/* -------------------------------------------------------------------------- */
/* Individual tests */
/* -------------------------------------------------------------------------- */

/** @brief Verify null xhtml bytes behavior. @details Executes the null xhtml bytes scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_null_xhtml_bytes()
{
  ra8_rabook_ctx_t ctx = internal_make_ctx();
  const ra8_err_t  err =
    ra8_rabook_xml_parse_chapter(nullptr, k_test_dummy_len, &ctx, "ch.xhtml", "T");
  internal_check(err == k_ra8_err_null_ptr, "null xhtml_bytes -> null_ptr");
}

/** @brief Verify null ctx behavior. @details Executes the null ctx scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_null_ctx()
{
  const uint8_t          dummy = 0U;
  const ra8_rabook_ctx_t ctx   = internal_make_ctx();
  const ra8_err_t        err   = ra8_rabook_xml_parse_chapter(&dummy, 1U, nullptr, "ch.xhtml", "T");
  (void)ctx;
  internal_check(err == k_ra8_err_null_ptr, "null ctx -> null_ptr");
}

/** @brief Verify null href behavior. @details Executes the null href scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_null_href()
{
  ra8_rabook_ctx_t ctx   = internal_make_ctx();
  const uint8_t    dummy = 0U;
  const ra8_err_t  err   = ra8_rabook_xml_parse_chapter(&dummy, 1U, &ctx, nullptr, "T");
  internal_check(err == k_ra8_err_null_ptr, "null chapter_href -> null_ptr");
}

/** @brief Verify null title behavior. @details Executes the null title scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_null_title()
{
  ra8_rabook_ctx_t ctx   = internal_make_ctx();
  const uint8_t    dummy = 0U;
  const ra8_err_t  err   = ra8_rabook_xml_parse_chapter(&dummy, 1U, &ctx, "ch.xhtml", nullptr);
  internal_check(err == k_ra8_err_null_ptr, "null chapter_title -> null_ptr");
}

/** @brief Verify malformed xml behavior. @details Executes the malformed xml scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_malformed_xml()
{
  ra8_rabook_ctx_t       ctx    = internal_make_ctx();
  const ra8_rabook_ctx_t before = ctx;
  const ra8_err_t        err =
    ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(s_xhtml_malformed),
                                 std::strlen(s_xhtml_malformed),
                                 &ctx,
                                 "ch.xhtml",
                                 "T");
  internal_check(err != k_ra8_ok, "malformed XHTML returns error");
  internal_check(internal_same_ctx_state(ctx, before), "malformed XHTML preserves builder state");
}

/** @brief Verify empty body behavior. @details Executes the empty body scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_empty_body()
{
  ra8_rabook_ctx_t ctx = internal_make_ctx();
  const ra8_err_t  err =
    ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(s_xhtml_empty_body),
                                 std::strlen(s_xhtml_empty_body),
                                 &ctx,
                                 "empty.xhtml",
                                 "Empty");

  internal_check(err == k_ra8_ok, "empty body: parse ok");
  /* One chapter, one node (the body root), no children. */
  internal_check(ctx.chapter_count == 1U, "empty body: chapter_count == 1");
  internal_check(ctx.node_count == 1U, "empty body: node_count == 1");
  internal_check(std::strcmp(internal_pool_str(ctx, ctx.buf.nodes[0].name_off), "body") == 0,
                 "empty body: root tag is 'body'");
  internal_check(ctx.buf.nodes[0].first_child == k_ra8_book_nil,
                 "empty body: root has no children");
}

/** @brief Verify simple p with text behavior. @details Executes the simple p with text scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_simple_p_with_text()
{
  ra8_rabook_ctx_t ctx = internal_make_ctx();
  const ra8_err_t  err =
    ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(s_xhtml_simple),
                                 std::strlen(s_xhtml_simple),
                                 &ctx,
                                 "simple.xhtml",
                                 "Simple");

  internal_check(err == k_ra8_ok, "simple: parse ok");
  internal_check(ctx.chapter_count == 1U, "simple: chapter_count == 1");
  /*
   * Expected DOM (pre-order):
   *   node[0] = body    (chapter root)
   *   node[1] = p       (first_child of body)
   *   node[2] = "Hello" (first_child of p)
   */
  internal_check(ctx.node_count == 3U, "simple: node_count == 3");

  /* body is an element */
  internal_check(ctx.buf.nodes[0].kind == (uint8_t)k_ra8_book_node_element,
                 "simple: node[0] is element");
  internal_check(std::strcmp(internal_pool_str(ctx, ctx.buf.nodes[0].name_off), "body") == 0,
                 "simple: node[0] tag is 'body'");
  internal_check(ctx.buf.nodes[0].first_child == 1U, "simple: body.first_child == 1");

  /* p is an element */
  internal_check(ctx.buf.nodes[1].kind == (uint8_t)k_ra8_book_node_element,
                 "simple: node[1] is element");
  internal_check(std::strcmp(internal_pool_str(ctx, ctx.buf.nodes[1].name_off), "p") == 0,
                 "simple: node[1] tag is 'p'");
  internal_check(ctx.buf.nodes[1].first_child == 2U, "simple: p.first_child == 2");
  internal_check(ctx.buf.nodes[1].next_sibling == k_ra8_book_nil, "simple: p.next_sibling == nil");

  /* "Hello" is a text node */
  internal_check(ctx.buf.nodes[2].kind == (uint8_t)k_ra8_book_node_text, "simple: node[2] is text");
  internal_check(std::strcmp(internal_pool_str(ctx, ctx.buf.nodes[2].text_off), "Hello") == 0,
                 "simple: node[2] text is 'Hello'");
  internal_check(ctx.buf.nodes[2].next_sibling == k_ra8_book_nil,
                 "simple: text.next_sibling == nil");
}

/** @brief Verify nested siblings preorder behavior. @details Executes the nested siblings preorder scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_nested_siblings_preorder()
{
  ra8_rabook_ctx_t ctx = internal_make_ctx();
  const ra8_err_t  err =
    ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(s_xhtml_nested),
                                 std::strlen(s_xhtml_nested),
                                 &ctx,
                                 "nested.xhtml",
                                 "Nested");

  internal_check(err == k_ra8_ok, "nested: parse ok");
  /*
   * Expected pre-order:
   *   node[0] = body  (root)
   *   node[1] = div   (body.first_child)
   *   node[2] = p     (div.first_child)
   *   node[3] = "A"   (p.first_child)
   *   node[4] = p2    (div.next_sibling)
   *   node[5] = "B"   (p2.first_child)
   */
  internal_check(ctx.node_count == 6U, "nested: node_count == 6");

  /* body */
  internal_check(ctx.buf.nodes[0].first_child == 1U, "nested: body.first_child == div (1)");

  /* div */
  internal_check(std::strcmp(internal_pool_str(ctx, ctx.buf.nodes[1].name_off), "div") == 0,
                 "nested: node[1] is div");
  internal_check(ctx.buf.nodes[1].first_child == 2U, "nested: div.first_child == p (2)");
  internal_check(ctx.buf.nodes[1].next_sibling == 4U, "nested: div.next_sibling == p2 (4)");

  /* p under div */
  internal_check(std::strcmp(internal_pool_str(ctx, ctx.buf.nodes[2].name_off), "p") == 0,
                 "nested: node[2] is p");
  internal_check(ctx.buf.nodes[2].first_child == 3U, "nested: p.first_child == 'A' (3)");
  internal_check(ctx.buf.nodes[2].next_sibling == k_ra8_book_nil, "nested: p.next_sibling == nil");

  /* "A" */
  internal_check(ctx.buf.nodes[3].kind == (uint8_t)k_ra8_book_node_text, "nested: node[3] is text");
  internal_check(std::strcmp(internal_pool_str(ctx, ctx.buf.nodes[3].text_off), "A") == 0,
                 "nested: node[3] text is 'A'");

  /* p2 (sibling of div) */
  internal_check(std::strcmp(internal_pool_str(ctx, ctx.buf.nodes[4].name_off), "p") == 0,
                 "nested: node[4] is p2");
  internal_check(ctx.buf.nodes[4].first_child == k_nested_b_idx,
                 "nested: p2.first_child == 'B' (5)");
  internal_check(ctx.buf.nodes[4].next_sibling == k_ra8_book_nil, "nested: p2.next_sibling == nil");

  /* "B" */
  internal_check(std::strcmp(internal_pool_str(ctx, ctx.buf.nodes[k_nested_b_idx].text_off), "B") ==
                   0,
                 "nested: node[5] text is 'B'");
}

/** @brief Verify html wrapper body fallback behavior. @details Executes the html wrapper body fallback scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_html_wrapper_body_fallback()
{
  ra8_rabook_ctx_t ctx = internal_make_ctx();
  const ra8_err_t  err =
    ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(s_xhtml_html_wrapper),
                                 std::strlen(s_xhtml_html_wrapper),
                                 &ctx,
                                 "wrapped.xhtml",
                                 "Wrapped");

  internal_check(err == k_ra8_ok, "html-wrapper: parse ok");
  /* The body fallback finds <body> inside <html>; root tag should be 'body'. */
  internal_check(ctx.node_count >= 1U, "html-wrapper: at least one node");
  internal_check(std::strcmp(internal_pool_str(ctx, ctx.buf.nodes[0].name_off), "body") == 0,
                 "html-wrapper: root tag is 'body'");
}

/**
 * @test internal_test_comment_skipped
 * @brief Two XML comments are dropped; only <p>Visible</p> survives.
 *
 * @par MC/DC:
 * Exercises comment-event filtering before builder mutation. Two
 * comments + one element + its text -> exactly 3 emitted nodes. @details Executes the comment skipped scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_comment_skipped()
{
  ra8_rabook_ctx_t ctx = internal_make_ctx();
  const ra8_err_t  err =
    ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(s_xhtml_with_comment),
                                 std::strlen(s_xhtml_with_comment),
                                 &ctx,
                                 "comment.xhtml",
                                 "Comment");

  internal_check(err == k_ra8_ok, "comment: parse ok");
  /*
   * Expected: body (root), p (child), "Visible" (text).
   * Both comments are skipped entirely -> 3 nodes.
   */
  internal_check(ctx.node_count == 3U, "comment: 3 nodes (two comments skipped)");
  internal_check(std::strcmp(internal_pool_str(ctx, ctx.buf.nodes[2].text_off), "Visible") == 0,
                 "comment: text node is 'Visible'");
}

/**
 * @test internal_test_cdata_skipped
 * @brief A <![CDATA[...]]> section is dropped, never emitted as a text node.
 *
 * @par MC/DC:
 * Covers the TRUE arm of @c if (text->CData()) in internal_emit_node (B's fix): the
 * CDATA node returns k_ra8_book_nil and is not added. The fixture is
 * @c <body><![CDATA[...]]><p>After</p></body>; if the CDATA were wrongly emitted
 * the node_count would be 4. Asserting node_count == 3 (body, p, "After") and
 * that no text node carries the CDATA content proves the skip. Complements the
 * FALSE arm (real text -> emitted) covered by internal_test_simple_p_with_text. @details Executes the cdata skipped scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_cdata_skipped()
{
  ra8_rabook_ctx_t ctx = internal_make_ctx();
  const ra8_err_t  err =
    ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(s_xhtml_with_cdata),
                                 std::strlen(s_xhtml_with_cdata),
                                 &ctx,
                                 "cdata.xhtml",
                                 "Cdata");

  internal_check(err == k_ra8_ok, "cdata: parse ok");
  /*
   * Expected pre-order: body (root), p (body's 2nd walked child; the CDATA is
   * the 1st child but is skipped), "After" (p's text). The CDATA never lands.
   */
  internal_check(ctx.node_count == 3U, "cdata: 3 nodes (CDATA skipped, not emitted)");
  internal_check(ctx.buf.nodes[0].first_child == 1U,
                 "cdata: body.first_child is the <p> (CDATA dropped)");
  internal_check(std::strcmp(internal_pool_str(ctx, ctx.buf.nodes[1].name_off), "p") == 0,
                 "cdata: node[1] is 'p'");
  internal_check(ctx.buf.nodes[2].kind == (uint8_t)k_ra8_book_node_text, "cdata: node[2] is text");
  internal_check(std::strcmp(internal_pool_str(ctx, ctx.buf.nodes[2].text_off), "After") == 0,
                 "cdata: surviving text is 'After'");
  /* No emitted text node carries the CDATA payload. */
  bool cdata_leaked = false;
  for (uint32_t i = 0U; i < ctx.node_count; ++i) {
    if (ctx.buf.nodes[i].kind == (uint8_t)k_ra8_book_node_text) {
      if (std::strstr(internal_pool_str(ctx, ctx.buf.nodes[i].text_off), "not emitted") !=
          nullptr) {
        cdata_leaked = true;
      }
    }
  }
  internal_check(!cdata_leaked, "cdata: payload never emitted as a text node");
}

/**
 * @test internal_test_empty_text_skipped
 * @brief An empty <p></p> adds the element but no text node.
 *
 * @par MC/DC:
 * Covers the FALSE arm of the live @c val[0] != '\0' condition in the compound
 * @c val != nullptr && val[0] != '\0' decision (internal_emit_node): @c <p></p> yields
 * an empty Value() string, so @c val[0] == '\0' and no text node is added.
 * The TRUE arm (non-empty text emitted) is covered by internal_test_simple_p_with_text. @details Executes the empty text skipped scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_empty_text_skipped()
{
  ra8_rabook_ctx_t ctx = internal_make_ctx();
  const ra8_err_t  err =
    ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(s_xhtml_empty_text),
                                 std::strlen(s_xhtml_empty_text),
                                 &ctx,
                                 "empty_text.xhtml",
                                 "EmptyText");

  internal_check(err == k_ra8_ok, "empty-text: parse ok");
  /* <p></p> -> body (root) + p (child); empty text node not added. */
  internal_check(ctx.node_count == 2U, "empty-text: 2 nodes (empty text skipped)");
}

/* -------------------------------------------------------------------------- */
/* Edge fixtures for the loop-bound / overflow MC/DC legs */
/* -------------------------------------------------------------------------- */

typedef enum : uint16_t {
  k_sib_at_max    = 256U, /**< Largest supported direct-child set.           */
  k_sib_over_max  = 257U, /**< Root children beyond the documented bound.    */
  k_attr_over_max = 33U,  /**< Attributes on one element beyond the bound.   */
  k_ovf_node_cap  = 3U,   /**< Tiny node cap: the fourth element cannot fit. */
} edge_dim_t;

/** @brief Build a builder context whose node table is capped at @p node_cap. @details Implements the make ctx capped fixture operation used only by this focused test executable. @param[in] node_cap Fixture argument governed by the exercised interface contract. @return The value computed by the fixture helper. @retval value The computed fixture value for the supplied inputs. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static ra8_rabook_ctx_t internal_make_ctx_capped(uint32_t node_cap)
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
 * @test internal_test_find_body_many_siblings
 * @brief A root with more direct children than the documented bound is rejected.
 *
 * @par MC/DC:
 * Drives the second-condition-false leg of `e != nullptr && tries <
 * k_xhtml_max_siblings` in internal_find_body: a `<root>` with 257 non-`<body>`
 * children crosses the bounded selection workspace. The parser returns a
 * validation error instead of changing policy and silently selecting the root. @details Executes the find body many siblings scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_find_body_many_siblings()
{
  ra8_rabook_ctx_t ctx    = internal_make_ctx();
  const auto       before = ctx;
  std::string      xml    = "<?xml version=\"1.0\"?><root>";
  for (uint32_t i = 0U; i < (uint32_t)k_sib_over_max; ++i) {
    xml += "<c/>";
  }
  xml += "</root>";
  const ra8_err_t err = ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(xml.c_str()),
                                                     xml.size(),
                                                     &ctx,
                                                     "wide.xhtml",
                                                     "Wide");
  internal_check(err == k_ra8_err_validation_failed, "many-siblings: cap plus one is rejected");
  internal_check(internal_same_ctx_state(ctx, before), "many-siblings: failure is atomic");
}

/** @test internal_test_find_body_sibling_cap accepts the documented bound exactly. @brief Verify find body sibling cap behavior. @details Executes the find body sibling cap scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_find_body_sibling_cap()
{
  ra8_rabook_ctx_t ctx = internal_make_ctx();
  std::string      xml = "<?xml version=\"1.0\"?><root>";
  for (uint32_t i = 0U; i < (uint32_t)k_sib_at_max; ++i) {
    xml += "<c/>";
  }
  xml += "</root>";
  const ra8_err_t err = ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(xml.c_str()),
                                                     xml.size(),
                                                     &ctx,
                                                     "wide.xhtml",
                                                     "Wide");
  internal_check(err == k_ra8_ok, "many-siblings: exact cap accepted");
  internal_check(ctx.node_count == (uint32_t)k_sib_at_max + 1U, "many-siblings: all nodes emitted");
}

/**
 * @test internal_test_collect_attrs_overflow
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
 * by the attribute-bearing fixture. The reader's documented attribute cap makes a
 * 33-attribute element is a reachable input. @details Executes the collect attrs overflow scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_collect_attrs_overflow()
{
  ra8_rabook_ctx_t ctx    = internal_make_ctx();
  const auto       before = ctx;
  std::string      xml    = "<?xml version=\"1.0\"?><body><p";
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
  internal_check(err == k_ra8_err_no_mem, "collect-attrs: overflow latches builder failure");
  internal_check(internal_same_ctx_state(ctx, before), "collect-attrs: failure is atomic");
}

/**
 * @test internal_test_walk_builder_overflow
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
 * fixture. A book that overruns the builder node budget is a reachable input. @details Executes the walk builder overflow scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_walk_builder_overflow()
{
  ra8_rabook_ctx_t      ctx    = internal_make_ctx_capped((uint32_t)k_ovf_node_cap);
  const auto            before = ctx;
  constexpr const char* k_deep =
    "<?xml version=\"1.0\"?><body><div><p><span>x</span></p></div></body>";
  const ra8_err_t err = ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(k_deep),
                                                     std::strlen(k_deep),
                                                     &ctx,
                                                     "deep.xhtml",
                                                     "Deep");
  /* body(0), div(1), p(2) fill the 3-node table; <span> fails to add (nil),
   * so the walk takes the new_idx == nil leg and the compile reports no_mem. */
  internal_check(err == k_ra8_err_no_mem, "walk-overflow: element add-nil leg (node cap hit)");
  internal_check(internal_same_ctx_state(ctx, before), "walk-overflow: failure is atomic");
}

/* -------------------------------------------------------------------------- */
/* Arena-starvation and lifecycle edges of the public entry point. */
/* -------------------------------------------------------------------------- */

typedef enum : uint16_t {
  k_attr_string_cap   = 2U, /**< Pool holding the empty sentinel plus one byte. */
  k_text_node_cap     = 2U, /**< Root and one element; no room for a text node. */
  k_no_chapter_cap    = 0U, /**< Chapter table that cannot hold one entry.      */
  k_zero_length       = 0U, /**< Zero-length source for the length guard.       */
  k_self_closed_nodes = 1U, /**< Nodes emitted by a self-closing selected root. */
} edge_limit_t;

/** @brief Build a builder context with chosen node, string, and chapter caps. @details Initialises against the shared arenas and then narrows only the three capacities under test, so a starved vector can fail in exactly one table; the empty-string sentinel interned by init still fits every narrowed pool. @param[in] node_cap DOM node-table capacity. @param[in] string_cap String-pool byte capacity. @param[in] chapter_cap Chapter-table capacity. @return The initialised builder context. @retval value A context bound to the shared fixture arenas. @pre Fixed-capacity fixture storage required by this operation is available. @pre Each requested capacity still admits the already-interned sentinel. @post The context carries the requested capacities and zero live rows. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static ra8_rabook_ctx_t
internal_make_ctx_limits(uint32_t node_cap, uint32_t string_cap, uint32_t chapter_cap)
{
  ra8_rabook_ctx_t ctx = internal_make_ctx();
  ctx.buf.node_cap     = node_cap;
  ctx.buf.string_cap   = string_cap;
  ctx.buf.chapter_cap  = chapter_cap;
  return ctx;
}

/* A self-closing <body> selected under a wrapper: nothing after it is emitted. */
constexpr const char* s_xhtml_self_closing_body = "<?xml version=\"1.0\"?>"
                                                  "<html><body/><p>after</p></html>";

/* One attribute on the root: its name is the first string the parser interns. */
constexpr const char* s_xhtml_attr = "<?xml version=\"1.0\"?><body a=\"v\"/>";

/** @test internal_test_entry_guards @brief The workspace pointer and the source length are each rejected alone. @details Every other required pointer stays valid in both vectors, so the observed status names the one argument under test: a missing workspace is a null-pointer error and a zero-length source is a size error, never a parse error. @pre Fixed-capacity fixture storage required by this operation is available. @pre The source pointer addresses at least one readable byte. @post Each vector returns its own canonical status. @post The builder is left exactly as it was constructed. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_entry_guards()
{
  ra8_rabook_ctx_t ctx    = internal_make_ctx();
  const auto       before = ctx;
  const ra8_err_t  no_ws =
    internal_parse_chapter_ws(reinterpret_cast<const uint8_t*>(s_xhtml_empty_body),
                              std::strlen(s_xhtml_empty_body),
                              &ctx,
                              "ch.xhtml",
                              "T",
                              nullptr);
  internal_check(no_ws == k_ra8_err_null_ptr, "null workspace -> null_ptr");
  const uint8_t   dummy = static_cast<uint8_t>('<');
  const ra8_err_t empty =
    ra8_rabook_xml_parse_chapter(&dummy, (size_t)k_zero_length, &ctx, "ch.xhtml", "T");
  internal_check(empty == k_ra8_err_invalid_size, "zero length -> invalid_size");
  internal_check(internal_same_ctx_state(ctx, before), "entry guards preserve builder state");
}

/** @brief Parse one document into a deliberately starved builder. @details Every vector must report the exhaustion rather than emit a partial chapter, and must restore the complete logical builder state it started from. @param[in] ctx Builder whose arenas were narrowed for this vector. @param[in] doc NUL-terminated XHTML source. @param[in] label Vector name used in the assertion record. @pre Fixed-capacity fixture storage required by this operation is available. @pre The document is well formed, so only an arena can fail. @post The parse returns the no-memory status. @post The builder is restored to its entry state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void
internal_expect_starved(ra8_rabook_ctx_t ctx, const char* doc, const char* label)
{
  const auto      before = ctx;
  const ra8_err_t err    = ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(doc),
                                                        std::strlen(doc),
                                                        &ctx,
                                                        "starved.xhtml",
                                                        "Starved");
  internal_check(err == k_ra8_err_no_mem, label);
  internal_check(internal_same_ctx_state(ctx, before), label);
}

/** @test internal_test_starved_arenas @brief Each starved builder arena stops the chapter with no-memory. @details The string pool holds only the empty sentinel, so an attribute name exhausts it and the following value intern observes the latched failure; the node table holds the body and its element child exactly, so the first text run is the allocation that fails; the chapter table cannot hold a row, so a completely emitted DOM is still rejected. Each vector must undo every node and string it appended. @pre Fixed-capacity fixture storage required by this operation is available. @pre Every document is well formed and every other arena is sufficient. @post Every vector returns the no-memory status. @post Every failure is atomic: the builder is restored to its entry state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_starved_arenas()
{
  internal_expect_starved(
    internal_make_ctx_limits(k_node_cap, (uint32_t)k_attr_string_cap, k_chapter_cap),
    s_xhtml_attr,
    "attr-pool: exhausted string pool -> no_mem");
  internal_expect_starved(
    internal_make_ctx_limits((uint32_t)k_text_node_cap, k_string_cap, k_chapter_cap),
    s_xhtml_simple,
    "text-overflow: text node cannot be added -> no_mem");
  internal_expect_starved(
    internal_make_ctx_limits(k_node_cap, k_string_cap, (uint32_t)k_no_chapter_cap),
    s_xhtml_empty_body,
    "chapter-full: no chapter row -> no_mem");
}

/** @test internal_test_self_closing_body_ends_subtree @brief A self-closing selected root closes the subtree immediately. @details `<body/>` under `<html>` is selected, emitted, and finished by the same event, so the sibling `<p>` that follows it is outside the chapter. If the emitter stayed active the sibling and its text would land in the chapter, so the exact node count is what distinguishes the two behaviours. @pre Fixed-capacity fixture storage required by this operation is available. @pre The document has a body sibling that would otherwise be emitted. @post The parse succeeds with exactly the self-closing body emitted. @post The chapter root is that body element. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_self_closing_body_ends_subtree()
{
  ra8_rabook_ctx_t ctx = internal_make_ctx();
  const ra8_err_t  err =
    ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(s_xhtml_self_closing_body),
                                 std::strlen(s_xhtml_self_closing_body),
                                 &ctx,
                                 "selfclose.xhtml",
                                 "SelfClose");
  internal_check(err == k_ra8_ok, "self-closing body: parse ok");
  internal_check(ctx.node_count == (uint32_t)k_self_closed_nodes,
                 "self-closing body: only the body is emitted");
  internal_check(ctx.chapter_count == 1U, "self-closing body: one chapter");
  internal_check(std::strcmp(internal_pool_str(ctx, ctx.buf.nodes[0].name_off), "body") == 0,
                 "self-closing body: root tag is 'body'");
  internal_check(ctx.buf.nodes[0].first_child == k_ra8_book_nil,
                 "self-closing body: root has no children");
}

/* -------------------------------------------------------------------------- */
/* Deep nesting: the bounded XML reader element-depth cap. */
/* -------------------------------------------------------------------------- */

/** @brief Build a builder context over the roomy deep-nesting node arena. @details Implements the make ctx deep fixture operation used only by this focused test executable. @return The value computed by the fixture helper. @retval value The computed fixture value for the supplied inputs. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static ra8_rabook_ctx_t internal_make_ctx_deep()
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
 *          single frame. The innermost element is self-closing so the fixture
 *          reaches the exact element-depth boundary without adding text.
 * @param[in] depth Number of nested `<d>` levels below `<body>` (>= 1).
 * @return The XHTML document text.
 * @pre @p depth is at least 1.
 * @post The result contains 2 * @p depth elements including `<body>`.
 */
RA8_INTERNAL static std::string internal_make_deep_doc(int depth)
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
 * @test internal_test_deep_at_reader_cap
 * @brief The deepest accepted document compiles with nothing truncated.
 *
 * @par MC/DC:
 * This is the non-vacuity witness for the `top < k_xhtml_max_stack`
 * deactivation in internal_push_frame, which claims the condition cannot go false on
 * any reachable input. It drives the walk to its provable worst case -- a frame
 * high-water mark below the fixed frame capacity -- and checks the emitted
 * node count exactly. A dropped frame loses an
 * entire subtree, so an exact count is what distinguishes "the guard never
 * fired" from "the guard fired and the chapter was silently truncated". Before
 * #625 the deactivation cited a cap of 100 and a 2 * depth bound; under those
 * numbers with the real cap this document would have demanded 1000 frames and
 * overrun the stack, so this case is precisely the one the false rationale
 * hid. @details Executes the deep at reader cap scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_deep_at_reader_cap()
{
  /* The reader rejects input beyond its public element-depth contract. */
  const int         depth = (int)k_ra8_xml_max_element_depth - 1;
  const std::string doc   = internal_make_deep_doc(depth);
  ra8_rabook_ctx_t  ctx   = internal_make_ctx_deep();
  const ra8_err_t err = ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(doc.c_str()),
                                                     doc.size(),
                                                     &ctx,
                                                     "deep.xhtml",
                                                     "Deep");

  internal_check(err == k_ra8_ok, "deep-at-cap: deepest accepted document compiles");
  internal_check(!ctx.failed, "deep-at-cap: builder latched no failure");
  /* <body> + depth <d> + (depth - 1) <s/> == 2 * depth nodes. */
  internal_check(ctx.node_count == (uint32_t)(2 * depth),
                 "deep-at-cap: every node emitted (no silent truncation)");
}

/**
 * @test internal_test_deep_beyond_reader_cap
 * @brief One level past the cap is rejected by the parser, never truncated.
 *
 * @par MC/DC:
 * The other side of the cap, and the reason the internal_push_frame safety valve stays
 * unreachable: the reader refuses the document before the walk starts, so the shim returns an
 * error rather than emitting a partial chapter. Together with
 * internal_test_deep_at_reader_cap this brackets the true cap from both sides, which is
 * the evidence the deactivation rationale now cites in place of the false
 * cap-of-100 claim. @details Executes the deep beyond reader cap scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_deep_beyond_reader_cap()
{
  const int         depth = (int)k_ra8_xml_max_element_depth;
  const std::string doc   = internal_make_deep_doc(depth);
  ra8_rabook_ctx_t  ctx   = internal_make_ctx_deep();
  const ra8_err_t err = ra8_rabook_xml_parse_chapter(reinterpret_cast<const uint8_t*>(doc.c_str()),
                                                     doc.size(),
                                                     &ctx,
                                                     "deeper.xhtml",
                                                     "Deeper");

  internal_check(err == k_ra8_err_validation_failed,
                 "deep-beyond-cap: parser rejects the document");
  internal_check(ctx.node_count == 0U,
                 "deep-beyond-cap: nothing emitted (rejected, not truncated)");
}

/* -------------------------------------------------------------------------- */
/* Log sink redirect (avoid ITM hardware access on host) */
/* -------------------------------------------------------------------------- */

/** @brief Provide the file-local log sink test helper. @details Implements the log sink fixture operation used only by this focused test executable. @param[in,out] arg0 Fixture argument governed by the exercised interface contract. @param[in] byte Fixture argument governed by the exercised interface contract. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_log_sink(void* /*ctx*/, uint8_t byte)
{
  (void)byte;
}

} // namespace

/* -------------------------------------------------------------------------- */
/* main */
/* -------------------------------------------------------------------------- */

/** @brief Run the focused test cases in this executable. @details Invokes each isolated case once and returns the accumulated assertion status. @return Process status from the accumulated assertions. @retval 0 Every focused assertion passed. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
int main()
{
  ra8_log_set_byte_sink(internal_log_sink, nullptr);

  internal_test_null_xhtml_bytes();
  internal_test_null_ctx();
  internal_test_null_href();
  internal_test_null_title();
  internal_test_malformed_xml();
  internal_test_empty_body();
  internal_test_simple_p_with_text();
  internal_test_nested_siblings_preorder();
  internal_test_html_wrapper_body_fallback();
  internal_test_comment_skipped();
  internal_test_cdata_skipped();
  internal_test_empty_text_skipped();
  internal_test_find_body_sibling_cap();
  internal_test_find_body_many_siblings();
  internal_test_collect_attrs_overflow();
  internal_test_walk_builder_overflow();
  internal_test_deep_at_reader_cap();
  internal_test_deep_beyond_reader_cap();
  internal_test_entry_guards();
  internal_test_starved_arenas();
  internal_test_self_closing_body_ends_subtree();

  return (s_pass == s_total) ? 0 : 1;
}
