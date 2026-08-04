/**
 * @file ra8_rabook_xml_shim.cpp
 * @brief C++ -> C shim: tinyxml2 XHTML parser -> ra8_rabook DOM builder (#149).
 *
 * @details
 * Wraps tinyxml2 to parse an XHTML chapter file and build the
 * @ref ra8_rabook_ctx_t DOM that @ref ra8_rabook_compile.h serialises.  The
 * iterative DFS walk mirrors `DomBuilder.add_element()` in the desktop tool
 * `tools/epub_compile/epub_compile.py`:
 *
 *  - Element nodes: tag name + source-order attributes.
 *  - Text nodes: verbatim character data (entities resolved by tinyxml2).
 *  - Comments / PIs / CDATA: silently skipped.
 *
 * @par DFS pre-order (correct left-to-right, depth-first):
 * When a node N is popped, its next_sibling is pushed (before first_child).
 * Because first_child is pushed last it is popped first, giving: N, N's
 * subtree, N's next_sibling, N's next_sibling's subtree -- exactly the
 * pre-order the desktop tool's recursive implementation produces.
 *
 * @par NASA Rule 1 (no recursion): RA8_NO_RECURSION documented below.
 * @par NASA Rule 3 deviation: tinyxml2 XMLDocument heap (bounded, local).
 *
 *
 * [Ring 4 / EPUB Compiler] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 */

#include "ra8_rabook_xml_shim.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_log.h"
#include "ra8_rabook_compile.h"
#include "tinyxml2.h"

/* -------------------------------------------------------------------------- */
/* Private constants */
/* -------------------------------------------------------------------------- */

/**
 * @enum ra8_xhtml_limits_t
 * @brief Static bounds for the iterative DFS to satisfy NASA Rule 2.
 * @details The DFS stack holds at most one next_sibling frame plus one
 *          first_child frame per depth level, so the worst-case stack depth
 *          is 2 * max_tree_depth.  @ref k_xhtml_max_stack = 512 handles trees
 *          up to ~256 deep with any branching factor.
 * @since Version 0.1.0
 */
typedef enum : uint16_t {
  k_xhtml_max_stack    = 512U, /**< Total frame-stack capacity (entries).    */
  k_xhtml_max_attrs    = 32U,  /**< Max attributes collected per element.    */
  k_xhtml_max_siblings = 256U, /**< Limit for the root-level sibling search. */
} ra8_xhtml_limits_t;

static const char* const s_tag = "ra8_rabook_xml_shim";

/* -------------------------------------------------------------------------- */
/* Private helpers (file-local: anonymous namespace, not `static`) */
/* -------------------------------------------------------------------------- */

namespace {

using tinyxml2::XMLAttribute;
using tinyxml2::XMLDocument;
using tinyxml2::XMLElement;
using tinyxml2::XMLNode;
using tinyxml2::XMLText;

/**
 * @struct ra8_xhtml_frame_t
 * @brief One entry on the iterative DFS explicit call-stack.
 * @details @p prev_sib_idx is the ra8_rabook index of the node's immediately
 *          preceding sibling (already added), or @ref k_ra8_book_nil for the
 *          first child of a parent.
 * @since Version 0.1.0
 */
typedef struct {
  const XMLNode* node;         /**< XML node to process next.               */
  uint32_t       parent_idx;   /**< ra8_rabook index of this node's parent. */
  uint32_t       prev_sib_idx; /**< ra8_rabook index of preceding sibling.  */
} ra8_xhtml_frame_t;

/**
 * @brief Find the `<body>` element, or fall back to the XML root element.
 * @details Searches the document element's direct children (bounded by
 *          @ref k_xhtml_max_siblings) for a tag named `body`; on a miss the root
 *          element itself is returned so a body-less fragment still has a root.
 * @param[in] doc Parsed XHTML document to search.
 * @return The `<body>` element, the root element on a miss, or nullptr when the
 *         document has no root element at all.
 * @retval nullptr The document parsed but has no root element.
 * @pre @p doc has been parsed (successfully or not) by tinyxml2.
 * @pre The document tree is not mutated concurrently.
 * @post @p doc is not modified (read-only search).
 * @post The returned pointer (if non-null) is owned by @p doc.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
const XMLElement* s_find_body(const XMLDocument& doc)
{
  const XMLElement* root = doc.RootElement();
  if (root == nullptr) {
    return nullptr;
  }

  uint16_t tries = 0U;
  for (const XMLElement* e = root->FirstChildElement();
       e != nullptr && tries < (uint16_t)k_xhtml_max_siblings;
       e = e->NextSiblingElement(), ++tries) {
    if (std::strcmp(e->Name(), "body") == 0) {
      return e;
    }
  }
  return root;
}

/**
 * @brief Collect element attributes into @p out; return the count written.
 * @details Interns each attribute name/value in source order, up to
 *          @ref k_xhtml_max_attrs records.  An element carrying more than
 *          @ref k_xhtml_max_attrs attributes is rejected rather than silently
 *          truncated: the builder's sticky `failed` flag is latched so
 *          @ref ra8_rabook_finalize surfaces the overflow.
 * @param[in]     elem Element whose attributes are collected (non-NULL).
 * @param[in,out] ctx  Builder used to intern the attribute strings (non-NULL).
 * @param[out]    out  Destination array of at least @ref k_xhtml_max_attrs records.
 * @return The number of attribute records written (0..@ref k_xhtml_max_attrs).
 * @pre @p elem, @p ctx and @p out are non-NULL (caller-guaranteed).
 * @pre @p out holds at least @ref k_xhtml_max_attrs records.
 * @post The returned count never exceeds @ref k_xhtml_max_attrs.
 * @post On attribute overflow `ctx->failed` is latched true.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
uint16_t s_collect_attrs(const XMLElement* elem, ra8_rabook_ctx_t* ctx, ra8_book_attr_t* out)
{
  uint16_t            count = 0U;
  const XMLAttribute* a     = elem->FirstAttribute();
  for (; a != nullptr && count < (uint16_t)k_xhtml_max_attrs; a = a->Next(), ++count) {
    out[count].name_off  = ra8_rabook_intern(ctx, a->Name());
    out[count].value_off = ra8_rabook_intern(ctx, a->Value());
  }
  if (a != nullptr) {
    /* More than k_xhtml_max_attrs attributes on one element: refuse to silently
     * drop the overflow -- latch the builder failure so finalize reports it. */
    ra8_log_error(s_tag, "element exceeds k_xhtml_max_attrs; refusing to truncate");
    ctx->failed = true;
  }
  return count;
}

/**
 * @brief Emit one XML node as a ra8_rabook element / text node (or skip it).
 * @details Element nodes become @ref ra8_rabook_add_element calls (tag + attrs);
 *          non-CDATA text nodes with non-empty content become
 *          @ref ra8_rabook_add_text calls.  Comments, processing instructions,
 *          CDATA sections and empty text produce no node, mirroring the desktop
 *          `DomBuilder` (CDATA is dropped, not emitted as text).
 * @param[in,out] ctx  Builder the node is appended to (non-NULL).
 * @param[in]     node XML node to emit (non-NULL).
 * @return The new ra8_rabook node index, or @ref k_ra8_book_nil when the node is
 *         skipped or the builder overflowed.
 * @retval k_ra8_book_nil Node skipped (comment/PI/CDATA/empty) or builder full.
 * @pre @p ctx and @p node are non-NULL (caller-guaranteed).
 * @pre @p ctx was initialised by @ref ra8_rabook_compile_init.
 * @post At most one node is appended to the builder.
 * @post @p node is not modified (read-only emit).
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
uint32_t s_emit_node(ra8_rabook_ctx_t* ctx, const XMLNode* node)
{
  const XMLElement* elem = node->ToElement();
  if (elem != nullptr) {
    ra8_book_attr_t attrs[k_xhtml_max_attrs] = {};
    const uint16_t  ac                       = s_collect_attrs(elem, ctx, attrs);
    return ra8_rabook_add_element(ctx, ra8_rabook_intern(ctx, elem->Name()), attrs, ac);
  }

  const XMLText* text = node->ToText();
  if (text != nullptr) {
    /* tinyxml2 models CDATA as an XMLText with CData()==true; skip it like a
     * comment/PI to match the desktop DomBuilder (CDATA is dropped). */
    if (text->CData()) {
      return k_ra8_book_nil;
    }
    const char* val = text->Value();
    // mcdc-deactivated: DO-178C 6.4.4.3 -- both conditions are invariantly true on any parsed text node, so the decision has no false outcome to pair against. tinyxml2's XMLText::Value() never returns nullptr (it returns "" at worst), so `val != nullptr` cannot flip; and a text node reached here always carries at least one character -- an empty run produces no XMLText, and PEDANTIC_WHITESPACE preserves even whitespace-only runs -- so `val[0] != '\0'` cannot flip either. Both are defensive guards against a future tinyxml2 contract change.
    if (val != nullptr && val[0] != '\0') {
      return ra8_rabook_add_text(ctx, ra8_rabook_intern(ctx, val));
    }
  }
  return k_ra8_book_nil;
}

/**
 * @brief Push one DFS frame onto @p stack if @p node exists and capacity remains.
 * @details A no-op when @p node is nullptr or the stack is full; the full-stack
 *          case is the bounded-loop safety valve (NASA Rule 2) and cannot occur
 *          for documents tinyxml2 accepts (its nesting cap is below the stack).
 * @param[in,out] stack        DFS frame stack of @ref k_xhtml_max_stack entries.
 * @param[in]     top          Current frame count (next free slot index).
 * @param[in]     node         Node to push, or nullptr to skip.
 * @param[in]     parent_idx   ra8_rabook index of the node's parent.
 * @param[in]     prev_sib_idx ra8_rabook index of the preceding sibling, or nil.
 * @return The new frame count: @p top + 1 on a push, otherwise @p top unchanged.
 * @pre @p stack holds @ref k_xhtml_max_stack frames and @p top <= that capacity.
 * @pre @p parent_idx / @p prev_sib_idx are valid ra8_rabook indices or nil.
 * @post The returned count never exceeds @ref k_xhtml_max_stack.
 * @post At most one frame is written, at index @p top.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
uint16_t s_push_frame(ra8_xhtml_frame_t* stack,
                      uint16_t           top,
                      const XMLNode*     node,
                      uint32_t           parent_idx,
                      uint32_t           prev_sib_idx)
{
  // mcdc-deactivated: DO-178C 6.4.4.3 -- C2 (top < k_xhtml_max_stack) is invariantly true: this DFS never pushes a nil frame (see the node != nullptr guard), so the stack depth is bounded by 2 * tree_depth, and tinyxml2 rejects any document deeper than its element-depth cap (100) before the walk begins. 100 * 2 < k_xhtml_max_stack (512), so no document tinyxml2 accepts can fill the stack -- C2 is the NASA Rule 2 safety valve and cannot flip on any reachable input. C1 (node != nullptr) is the live condition and both its arms are covered.
  if (node != nullptr && top < (uint16_t)k_xhtml_max_stack) {
    stack[top] = {node, parent_idx, prev_sib_idx};
    return (uint16_t)(top + 1U);
  }
  return top;
}

/**
 * @brief Walk @p body's subtree in DFS pre-order, building the chapter DOM.
 * @details Iterative (NASA Rule 1: no recursion) pre-order walk over an explicit
 *          frame stack.  Each popped node is emitted via @ref s_emit_node, linked
 *          into its parent / preceding-sibling chain, then its next_sibling is
 *          pushed before its first_child so first_child is popped first (correct
 *          left-to-right pre-order).
 * @param[in,out] ctx          Builder the nodes are appended to (non-NULL).
 * @param[in]     body         Chapter root element whose children are walked.
 * @param[in]     chapter_root ra8_rabook index of @p body's already-added node.
 * @pre @p ctx and @p body are non-NULL (caller-guaranteed).
 * @pre @p chapter_root is the index returned for @p body.
 * @post Every emitted node is linked under @p chapter_root in pre-order.
 * @post The DFS stack never exceeds @ref k_xhtml_max_stack frames.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
void s_walk_body_subtree(ra8_rabook_ctx_t* ctx, const XMLElement* body, uint32_t chapter_root)
{
  ra8_xhtml_frame_t stack[k_xhtml_max_stack] = {};
  uint16_t          top                      = 0U;
  top = s_push_frame(stack, top, body->FirstChild(), chapter_root, k_ra8_book_nil);

  while (top > 0U) {
    const ra8_xhtml_frame_t frame   = stack[--top];
    const uint32_t          new_idx = s_emit_node(ctx, frame.node);

    if (new_idx != k_ra8_book_nil) {
      /* Link into the parent's child list or the preceding sibling chain. */
      if (frame.prev_sib_idx == k_ra8_book_nil) {
        (void)ra8_rabook_link_child(ctx, frame.parent_idx, new_idx);
      } else {
        (void)ra8_rabook_link_sibling(ctx, frame.prev_sib_idx, new_idx);
      }
    }

    /* The effective "previous sibling" for the next sibling frame. */
    const uint32_t sib_prev = (new_idx != k_ra8_book_nil) ? new_idx : frame.prev_sib_idx;

    /* Push next_sibling BEFORE first_child (LIFO: first_child popped first). */
    top = s_push_frame(stack, top, frame.node->NextSibling(), frame.parent_idx, sib_prev);

    const XMLElement* elem = frame.node->ToElement();
    if (elem != nullptr && new_idx != k_ra8_book_nil) {
      top = s_push_frame(stack, top, elem->FirstChild(), new_idx, k_ra8_book_nil);
    }
  }
}

/**
 * @brief Reject any NULL argument to @ref ra8_rabook_xml_parse_chapter.
 * @details Centralises the entry-point null guards so the public function stays
 *          flat; each guard names its argument in the log line.
 * @param[in] xhtml_bytes   Raw XHTML byte pointer to validate.
 * @param[in] ctx           Builder context pointer to validate.
 * @param[in] chapter_href  Chapter href pointer to validate.
 * @param[in] chapter_title Chapter title pointer to validate.
 * @return Error code.
 * @retval k_ra8_ok           Every argument is non-NULL.
 * @retval k_ra8_err_null_ptr One of the arguments is NULL.
 * @pre The pointers, if non-NULL, address valid objects for the call.
 * @pre Called once at the top of the parse entry point.
 * @post No argument is modified (read-only validation).
 * @post On k_ra8_ok all four arguments are guaranteed non-NULL.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
ra8_err_t s_check_parse_args(const uint8_t*          xhtml_bytes,
                             const ra8_rabook_ctx_t* ctx,
                             const char*             chapter_href,
                             const char*             chapter_title)
{
  RA8_CHECK_NULL_PTR(xhtml_bytes, s_tag, "xhtml_bytes");
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  RA8_CHECK_NULL_PTR(chapter_href, s_tag, "chapter_href");
  RA8_CHECK_NULL_PTR(chapter_title, s_tag, "chapter_title");
  return k_ra8_ok;
}

} /* namespace */

/* -------------------------------------------------------------------------- */
/* Public API */
/* -------------------------------------------------------------------------- */

extern "C" {

RA8_NO_RECURSION
ra8_err_t ra8_rabook_xml_parse_chapter(const uint8_t*    xhtml_bytes,
                                       size_t            xhtml_len,
                                       ra8_rabook_ctx_t* ctx,
                                       const char*       chapter_href,
                                       const char*       chapter_title)
{
  const ra8_err_t arg_err = s_check_parse_args(xhtml_bytes, ctx, chapter_href, chapter_title);
  if (arg_err != k_ra8_ok) {
    return arg_err;
  }

  /* PEDANTIC_WHITESPACE (#151): keep inter-element whitespace text nodes so the
   * chapter DOM is byte-identical to the desktop reference DomBuilder, which
   * preserves every text run. This retains significant inline whitespace such as
   * the space between adjacent <abbr> elements ("<abbr>x</abbr> <abbr>y</abbr>"),
   * which the default PRESERVE_WHITESPACE mode drops -- merging the words. Entity
   * processing stays on (processEntities=true). Only chapter content opts in;
   * ra8_epub keeps the default mode for OPF / container / TOC parsing. */
  tinyxml2::XMLDocument doc(true, tinyxml2::PEDANTIC_WHITESPACE);
  if (doc.Parse(reinterpret_cast<const char*>(xhtml_bytes), xhtml_len) != tinyxml2::XML_SUCCESS) {
    ra8_log_error(s_tag, "tinyxml2 parse failed");
    return k_ra8_err_no_mem;
  }

  const tinyxml2::XMLElement* body = s_find_body(doc);
  if (body == nullptr) {
    ra8_log_error(s_tag, "no root element in XHTML");
    return k_ra8_err_no_mem;
  }

  /* Add the <body> element as the chapter root, then walk its subtree. */
  ra8_book_attr_t body_attrs[k_xhtml_max_attrs] = {};
  const uint16_t  body_ac                       = s_collect_attrs(body, ctx, body_attrs);
  const uint32_t  chapter_root =
    ra8_rabook_add_element(ctx, ra8_rabook_intern(ctx, body->Name()), body_attrs, body_ac);

  s_walk_body_subtree(ctx, body, chapter_root);

  const uint32_t title_off   = ra8_rabook_intern(ctx, chapter_title);
  const uint32_t href_off    = ra8_rabook_intern(ctx, chapter_href);
  const uint32_t chapter_idx = ra8_rabook_add_chapter(ctx, title_off, href_off, chapter_root);
  return (chapter_idx != k_ra8_book_nil) ? k_ra8_ok : k_ra8_err_no_mem;
}

} /* extern "C" */
