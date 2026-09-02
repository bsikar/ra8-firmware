/**
 * @file test_ra8_rabook_xml_shim_limits.cpp
 * @brief Resource-limit and deep-nesting tests for the rabook XML shim.
 *
 * @details
 * Keeps bounded sibling/attribute traversal, arena exhaustion, public-entry
 * lifecycle, and reader-depth boundary responsibilities separate from the DOM
 * shape and event-policy cases in test_ra8_rabook_xml_shim.cpp.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_rabook_xml_shim_test_fixture_internal.h"

/* The deep-nesting fixtures derive from the production reader contract. */
#include "xml.h"

namespace {
struct limit_test_owner;
using test_fixture_t = ra8_rabook_xml_shim_fixture_detail::test_fixture<limit_test_owner>;
struct limit_test_suite : test_fixture_t {

  static constexpr const char* s_xhtml_empty_body = "<?xml version=\"1.0\"?><body></body>";

  static constexpr const char* s_xhtml_simple =
    "<?xml version=\"1.0\"?>"
    "<!DOCTYPE body PUBLIC '-//W3C//DTD XHTML 1.1//EN' 'xhtml11.dtd'>"
    "<body><p>Hello</p></body>";

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
    (void)rabook_compile_init(&ctx, &bufs);
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
    const ra8_err_t err = internal_parse_chapter(reinterpret_cast<const uint8_t*>(xml.c_str()),
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
    const ra8_err_t err = internal_parse_chapter(reinterpret_cast<const uint8_t*>(xml.c_str()),
                                                 xml.size(),
                                                 &ctx,
                                                 "wide.xhtml",
                                                 "Wide");
    internal_check(err == k_ra8_ok, "many-siblings: exact cap accepted");
    internal_check(ctx.node_count == (uint32_t)k_sib_at_max + 1U,
                   "many-siblings: all nodes emitted");
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
    const ra8_err_t err = internal_parse_chapter(reinterpret_cast<const uint8_t*>(xml.c_str()),
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
 * k_book_nil` in internal_walk_body_subtree: with the node table capped at 3, the
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
    const ra8_err_t err = internal_parse_chapter(reinterpret_cast<const uint8_t*>(k_deep),
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

  typedef enum : uint8_t {
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
  static constexpr const char* s_xhtml_self_closing_body = "<?xml version=\"1.0\"?>"
                                                           "<html><body/><p>after</p></html>";

  /* One attribute on the root: its name is the first string the parser interns. */
  static constexpr const char* s_xhtml_attr = "<?xml version=\"1.0\"?><body a=\"v\"/>";

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
      internal_parse_chapter(&dummy, (size_t)k_zero_length, &ctx, "ch.xhtml", "T");
    internal_check(empty == k_ra8_err_invalid_size, "zero length -> invalid_size");
    internal_check(internal_same_ctx_state(ctx, before), "entry guards preserve builder state");
  }

  /** @brief Parse one document into a deliberately starved builder. @details Every vector must report the exhaustion rather than emit a partial chapter, and must restore the complete logical builder state it started from. @param[in] ctx Builder whose arenas were narrowed for this vector. @param[in] doc NUL-terminated XHTML source. @param[in] label Vector name used in the assertion record. @pre Fixed-capacity fixture storage required by this operation is available. @pre The document is well formed, so only an arena can fail. @post The parse returns the no-memory status. @post The builder is restored to its entry state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
  RA8_INTERNAL static void
  internal_expect_starved(ra8_rabook_ctx_t ctx, const char* doc, const char* label)
  {
    const auto      before = ctx;
    const ra8_err_t err    = internal_parse_chapter(reinterpret_cast<const uint8_t*>(doc),
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
      internal_parse_chapter(reinterpret_cast<const uint8_t*>(s_xhtml_self_closing_body),
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
    internal_check(ctx.buf.nodes[0].first_child == k_book_nil,
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
    (void)rabook_compile_init(&ctx, &bufs);
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
    const int         depth = (int)k_xml_max_element_depth - 1;
    const std::string doc   = internal_make_deep_doc(depth);
    ra8_rabook_ctx_t  ctx   = internal_make_ctx_deep();
    const ra8_err_t   err   = internal_parse_chapter(reinterpret_cast<const uint8_t*>(doc.c_str()),
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
    const int         depth = (int)k_xml_max_element_depth;
    const std::string doc   = internal_make_deep_doc(depth);
    ra8_rabook_ctx_t  ctx   = internal_make_ctx_deep();
    const ra8_err_t   err   = internal_parse_chapter(reinterpret_cast<const uint8_t*>(doc.c_str()),
                                                     doc.size(),
                                                     &ctx,
                                                     "deeper.xhtml",
                                                     "Deeper");

    internal_check(err == k_ra8_err_validation_failed,
                   "deep-beyond-cap: parser rejects the document");
    internal_check(ctx.node_count == 0U,
                   "deep-beyond-cap: nothing emitted (rejected, not truncated)");
  }
};
} // namespace

/** @copydoc ra8_rabook_xml_shim_limits_tests_run */
RA8_TEST_HELPER bool ra8_rabook_xml_shim_limits_tests_run()
{
  limit_test_suite::internal_test_find_body_sibling_cap();
  limit_test_suite::internal_test_find_body_many_siblings();
  limit_test_suite::internal_test_collect_attrs_overflow();
  limit_test_suite::internal_test_walk_builder_overflow();
  limit_test_suite::internal_test_deep_at_reader_cap();
  limit_test_suite::internal_test_deep_beyond_reader_cap();
  limit_test_suite::internal_test_entry_guards();
  limit_test_suite::internal_test_starved_arenas();
  limit_test_suite::internal_test_self_closing_body_ends_subtree();

  return limit_test_suite::s_pass == limit_test_suite::s_total;
}
