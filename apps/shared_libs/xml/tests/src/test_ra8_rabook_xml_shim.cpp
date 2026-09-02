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
 *       mcdc-deactivated NULL guards in epub_xml_shim.cpp). The second
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
 *       reader rejects nesting beyond @c k_xml_max_element_depth.
 *       internal_test_deep_at_reader_cap pins that worst
 *       case and internal_test_deep_beyond_reader_cap pins the rejection one level
 *       deeper, so the deactivation rests on a measurement rather than on the
 *       false cap-of-100 claim it carried before #625.
 *   - @c frame.prev_sib_idx == k_book_nil in internal_walk_body_subtree -- single
 *       condition. T (first child) and F (later sibling) both in
 *       internal_test_nested_siblings_preorder.
 *   - @c elem != nullptr && new_idx != k_book_nil in internal_walk_body_subtree --
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

#include "ra8_rabook_xml_shim_test_fixture_internal.h"

namespace {
struct main_test_owner;
using test_fixture_t = ra8_rabook_xml_shim_fixture_detail::test_fixture<main_test_owner>;
struct test_suite : test_fixture_t {

  /* -------------------------------------------------------------------------- */
  /* Fixtures */
  /* -------------------------------------------------------------------------- */

  static constexpr const char* s_xhtml_empty_body = "<?xml version=\"1.0\"?><body></body>";

  static constexpr const char* s_xhtml_simple =
    "<?xml version=\"1.0\"?>"
    "<!DOCTYPE body PUBLIC '-//W3C//DTD XHTML 1.1//EN' 'xhtml11.dtd'>"
    "<body><p>Hello</p></body>";

  /* Nested: body > div > p > "A", body > p > "B" */
  static constexpr const char* s_xhtml_nested = "<?xml version=\"1.0\"?>"
                                                "<body>"
                                                "<div><p>A</p></div>"
                                                "<p>B</p>"
                                                "</body>";

  /* HTML wrapper: should fall through to <body> */
  static constexpr const char* s_xhtml_html_wrapper = "<?xml version=\"1.0\"?>"
                                                      "<html><head/><body><p>Hi</p></body></html>";

  /* Contains two XML comments: both should be silently skipped. */
  static constexpr const char* s_xhtml_with_comment = "<?xml version=\"1.0\"?>"
                                                      "<body>"
                                                      "<!-- first ignored -->"
                                                      "<!-- second ignored -->"
                                                      "<p>Visible</p>"
                                                      "</body>";

  /* A real CDATA section: the production event policy drops it rather than
 * emitting a text node. The CDATA sits
 * before a real <p> so the visible element still lands as body's second walked
 * child. If CDATA were wrongly emitted, node_count would be 4 instead of 3. */
  static constexpr const char* s_xhtml_with_cdata = "<?xml version=\"1.0\"?>"
                                                    "<body>"
                                                    "<![CDATA[raw & < > not emitted]]>"
                                                    "<p>After</p>"
                                                    "</body>";

  static constexpr const char* s_xhtml_malformed = "<body><unclosed>";

  /* Empty text (whitespace-only inside an element gets skipped when trimmed) */
  static constexpr const char* s_xhtml_empty_text = "<?xml version=\"1.0\"?><body><p></p></body>";

  /* -------------------------------------------------------------------------- */
  /* Individual tests */
  /* -------------------------------------------------------------------------- */

  /** @brief Verify null xhtml bytes behavior. @details Executes the null xhtml bytes scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
  RA8_INTERNAL static void internal_test_null_xhtml_bytes()
  {
    ra8_rabook_ctx_t ctx = internal_make_ctx();
    const ra8_err_t  err = internal_parse_chapter(nullptr, k_test_dummy_len, &ctx, "ch.xhtml", "T");
    internal_check(err == k_ra8_err_null_ptr, "null xhtml_bytes -> null_ptr");
  }

  /** @brief Verify null ctx behavior. @details Executes the null ctx scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
  RA8_INTERNAL static void internal_test_null_ctx()
  {
    const uint8_t          dummy = 0U;
    const ra8_rabook_ctx_t ctx   = internal_make_ctx();
    const ra8_err_t        err   = internal_parse_chapter(&dummy, 1U, nullptr, "ch.xhtml", "T");
    (void)ctx;
    internal_check(err == k_ra8_err_null_ptr, "null ctx -> null_ptr");
  }

  /** @brief Verify null href behavior. @details Executes the null href scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
  RA8_INTERNAL static void internal_test_null_href()
  {
    ra8_rabook_ctx_t ctx   = internal_make_ctx();
    const uint8_t    dummy = 0U;
    const ra8_err_t  err   = internal_parse_chapter(&dummy, 1U, &ctx, nullptr, "T");
    internal_check(err == k_ra8_err_null_ptr, "null chapter_href -> null_ptr");
  }

  /** @brief Verify null title behavior. @details Executes the null title scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
  RA8_INTERNAL static void internal_test_null_title()
  {
    ra8_rabook_ctx_t ctx   = internal_make_ctx();
    const uint8_t    dummy = 0U;
    const ra8_err_t  err   = internal_parse_chapter(&dummy, 1U, &ctx, "ch.xhtml", nullptr);
    internal_check(err == k_ra8_err_null_ptr, "null chapter_title -> null_ptr");
  }

  /** @brief Verify malformed xml behavior. @details Executes the malformed xml scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
  RA8_INTERNAL static void internal_test_malformed_xml()
  {
    ra8_rabook_ctx_t       ctx    = internal_make_ctx();
    const ra8_rabook_ctx_t before = ctx;
    const ra8_err_t        err =
      internal_parse_chapter(reinterpret_cast<const uint8_t*>(s_xhtml_malformed),
                             std::strlen(s_xhtml_malformed),
                             &ctx,
                             "ch.xhtml",
                             "T");
    internal_check(err != k_ra8_ok, "malformed XHTML returns error");
    internal_check(internal_same_ctx_state(ctx, before), "malformed XHTML preserves builder state");
  }

  /**
 * @test internal_test_post_validation_reader_faults
 * @brief Both defensive second-pass reader failures propagate unchanged.
 * @details Public parsing validates immutable input first, so these arms need
 *          the narrow test-only seams to inject a malformed second-pass source.
 * @pre Static builder and XML workspaces are writable.
 * @post Selection and emission each report validation failure.
 * @post Emission consumes exactly the selected root before its later reader fault.
 * @note No production caller can bypass the first-pass validator.
 * @since 0.1.0
 */
  RA8_INTERNAL static void internal_test_post_validation_reader_faults()
  {
    static constexpr uint8_t faulting[] = "<body><";
    internal_check(priv_ra8_rabook_xml_select_unvalidated_test(faulting,
                                                               sizeof(faulting) - 1U,
                                                               &s_xml_workspace) ==
                     k_ra8_err_validation_failed,
                   "selection reader fault propagates");
    ra8_rabook_ctx_t ctx = internal_make_ctx();
    internal_check(priv_ra8_rabook_xml_emit_unvalidated_test(faulting,
                                                             sizeof(faulting) - 1U,
                                                             &ctx,
                                                             &s_xml_workspace) ==
                     k_ra8_err_validation_failed,
                   "emission reader fault propagates");
    internal_check(ctx.node_count == 1U, "emission reaches the post-root reader fault");
  }

  /** @brief Verify empty body behavior. @details Executes the empty body scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
  RA8_INTERNAL static void internal_test_empty_body()
  {
    ra8_rabook_ctx_t ctx = internal_make_ctx();
    const ra8_err_t  err =
      internal_parse_chapter(reinterpret_cast<const uint8_t*>(s_xhtml_empty_body),
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
    internal_check(ctx.buf.nodes[0].first_child == k_book_nil, "empty body: root has no children");
  }

  /** @brief Verify simple p with text behavior. @details Executes the simple p with text scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
  RA8_INTERNAL static void internal_test_simple_p_with_text()
  {
    ra8_rabook_ctx_t ctx = internal_make_ctx();
    const ra8_err_t  err = internal_parse_chapter(reinterpret_cast<const uint8_t*>(s_xhtml_simple),
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
    internal_check(ctx.buf.nodes[0].kind == (uint8_t)k_book_node_element,
                   "simple: node[0] is element");
    internal_check(std::strcmp(internal_pool_str(ctx, ctx.buf.nodes[0].name_off), "body") == 0,
                   "simple: node[0] tag is 'body'");
    internal_check(ctx.buf.nodes[0].first_child == 1U, "simple: body.first_child == 1");

    /* p is an element */
    internal_check(ctx.buf.nodes[1].kind == (uint8_t)k_book_node_element,
                   "simple: node[1] is element");
    internal_check(std::strcmp(internal_pool_str(ctx, ctx.buf.nodes[1].name_off), "p") == 0,
                   "simple: node[1] tag is 'p'");
    internal_check(ctx.buf.nodes[1].first_child == 2U, "simple: p.first_child == 2");
    internal_check(ctx.buf.nodes[1].next_sibling == k_book_nil, "simple: p.next_sibling == nil");

    /* "Hello" is a text node */
    internal_check(ctx.buf.nodes[2].kind == (uint8_t)k_book_node_text, "simple: node[2] is text");
    internal_check(std::strcmp(internal_pool_str(ctx, ctx.buf.nodes[2].text_off), "Hello") == 0,
                   "simple: node[2] text is 'Hello'");
    internal_check(ctx.buf.nodes[2].next_sibling == k_book_nil, "simple: text.next_sibling == nil");
  }

  /** @brief Verify nested siblings preorder behavior. @details Executes the nested siblings preorder scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
  RA8_INTERNAL static void internal_test_nested_siblings_preorder()
  {
    ra8_rabook_ctx_t ctx = internal_make_ctx();
    const ra8_err_t  err = internal_parse_chapter(reinterpret_cast<const uint8_t*>(s_xhtml_nested),
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
    internal_check(ctx.buf.nodes[2].next_sibling == k_book_nil, "nested: p.next_sibling == nil");

    /* "A" */
    internal_check(ctx.buf.nodes[3].kind == (uint8_t)k_book_node_text, "nested: node[3] is text");
    internal_check(std::strcmp(internal_pool_str(ctx, ctx.buf.nodes[3].text_off), "A") == 0,
                   "nested: node[3] text is 'A'");

    /* p2 (sibling of div) */
    internal_check(std::strcmp(internal_pool_str(ctx, ctx.buf.nodes[4].name_off), "p") == 0,
                   "nested: node[4] is p2");
    internal_check(ctx.buf.nodes[4].first_child == k_nested_b_idx,
                   "nested: p2.first_child == 'B' (5)");
    internal_check(ctx.buf.nodes[4].next_sibling == k_book_nil, "nested: p2.next_sibling == nil");

    /* "B" */
    internal_check(
      std::strcmp(internal_pool_str(ctx, ctx.buf.nodes[k_nested_b_idx].text_off), "B") == 0,
      "nested: node[5] text is 'B'");
  }

  /**
 * @brief Verify html wrapper body fallback behavior.
 * @par MC/DC:
 * Decision: `while ((err == k_ra8_ok) && !done)`
 * (2 conditions, apps/shared_libs/rabook_compile/src/ra8_rabook_xml_shim.c@internal_select)
 * `err` is deactivated (see the `mcdc-deactivated` comment on the loop: every
 * failing `err` breaks out before the condition is re-evaluated, so it reads
 * true on every reachable check); only `!done` varies.
 * - Vector 1: first evaluation -- done=false -> true -> loop enters and reads
 *   the `<head/>` event, which matches neither body nor sets done.
 * - Vector 2: second evaluation -- done=true (the `<body>` event just matched)
 *   -> false -> loop exits.
 * Vectors 1+2 flip the outcome varying done alone -- N+1 = 2 vectors for the
 * one live condition.
 * @details Executes the html wrapper body fallback scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
  RA8_INTERNAL static void internal_test_html_wrapper_body_fallback()
  {
    ra8_rabook_ctx_t ctx = internal_make_ctx();
    const ra8_err_t  err =
      internal_parse_chapter(reinterpret_cast<const uint8_t*>(s_xhtml_html_wrapper),
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
      internal_parse_chapter(reinterpret_cast<const uint8_t*>(s_xhtml_with_comment),
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
 * CDATA node returns k_book_nil and is not added. The fixture is
 * @c <body><![CDATA[...]]><p>After</p></body>; if the CDATA were wrongly emitted
 * the node_count would be 4. Asserting node_count == 3 (body, p, "After") and
 * that no text node carries the CDATA content proves the skip. Complements the
 * FALSE arm (real text -> emitted) covered by internal_test_simple_p_with_text. @details Executes the cdata skipped scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
  RA8_INTERNAL static void internal_test_cdata_skipped()
  {
    ra8_rabook_ctx_t ctx = internal_make_ctx();
    const ra8_err_t  err =
      internal_parse_chapter(reinterpret_cast<const uint8_t*>(s_xhtml_with_cdata),
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
    internal_check(ctx.buf.nodes[2].kind == (uint8_t)k_book_node_text, "cdata: node[2] is text");
    internal_check(std::strcmp(internal_pool_str(ctx, ctx.buf.nodes[2].text_off), "After") == 0,
                   "cdata: surviving text is 'After'");
    /* No emitted text node carries the CDATA payload. */
    bool cdata_leaked = false;
    for (uint32_t i = 0U; i < ctx.node_count; ++i) {
      if (ctx.buf.nodes[i].kind == (uint8_t)k_book_node_text) {
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
      internal_parse_chapter(reinterpret_cast<const uint8_t*>(s_xhtml_empty_text),
                             std::strlen(s_xhtml_empty_text),
                             &ctx,
                             "empty_text.xhtml",
                             "EmptyText");

    internal_check(err == k_ra8_ok, "empty-text: parse ok");
    /* <p></p> -> body (root) + p (child); empty text node not added. */
    internal_check(ctx.node_count == 2U, "empty-text: 2 nodes (empty text skipped)");
  }

  /* -------------------------------------------------------------------------- */
  /* Log sink redirect (avoid ITM hardware access on host) */
  /* -------------------------------------------------------------------------- */

  /** @brief Provide the file-local log sink test helper. @details Implements the log sink fixture operation used only by this focused test executable. @param[in,out] arg0 Fixture argument governed by the exercised interface contract. @param[in] byte Fixture argument governed by the exercised interface contract. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
  RA8_INTERNAL static void internal_log_sink(void* /*ctx*/, uint8_t byte)
  {
    (void)byte;
  }
};
} // namespace

/* -------------------------------------------------------------------------- */
/* main */
/* -------------------------------------------------------------------------- */

/** @brief Run the focused test cases in this executable. @details Invokes each isolated case once and returns the accumulated assertion status. @return Process status from the accumulated assertions. @retval 0 Every focused assertion passed. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
int main()
{
  ra8_log_set_byte_sink(test_suite::internal_log_sink, nullptr);

  test_suite::internal_test_null_xhtml_bytes();
  test_suite::internal_test_null_ctx();
  test_suite::internal_test_null_href();
  test_suite::internal_test_null_title();
  test_suite::internal_test_malformed_xml();
  test_suite::internal_test_post_validation_reader_faults();
  test_suite::internal_test_empty_body();
  test_suite::internal_test_simple_p_with_text();
  test_suite::internal_test_nested_siblings_preorder();
  test_suite::internal_test_html_wrapper_body_fallback();
  test_suite::internal_test_comment_skipped();
  test_suite::internal_test_cdata_skipped();
  test_suite::internal_test_empty_text_skipped();
  test_suite::internal_check(ra8_rabook_xml_shim_limits_tests_run(),
                             "resource-limit and deep-nesting suite");

  return (test_suite::s_pass == test_suite::s_total) ? 0 : 1;
}
