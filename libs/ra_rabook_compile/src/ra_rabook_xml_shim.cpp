/**
 * @file ra_rabook_xml_shim.cpp
 * @brief C++ -> C shim: tinyxml2 XHTML parser -> ra_rabook DOM builder (#149).
 *
 * @details
 * Wraps tinyxml2 to parse an XHTML chapter file and build the
 * @ref ra_rabook_ctx_t DOM that @ref ra_rabook_compile.h serialises.  The
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
 * @par NASA Rule 1 (no recursion): RA_NO_RECURSION documented below.
 * @par NASA Rule 3 deviation: tinyxml2 XMLDocument heap (bounded, local).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / EPUB Compiler] {World: NS}
 *
 * @since Version 0.1.0
 */

#include "ra_rabook_xml_shim.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ra_attributes.h"
#include "ra_check.h"
#include "ra_log.h"
#include "ra_rabook_compile.h"
#include "tinyxml2.h"

/* -------------------------------------------------------------------------- */
/* Private constants */
/* -------------------------------------------------------------------------- */

/**
 * @enum ra_xhtml_limits_t
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
} ra_xhtml_limits_t;

static const char* const s_tag = "ra_rabook_xml_shim";

/* -------------------------------------------------------------------------- */
/* Private helpers */
/* -------------------------------------------------------------------------- */

/**
 * @brief Find the `<body>` element, or fall back to the XML root element.
 */
RA_INTERNAL
static const tinyxml2::XMLElement* s_find_body(const tinyxml2::XMLDocument& doc)
{
  const tinyxml2::XMLElement* root = doc.RootElement();
  if (root == nullptr) {
    return nullptr;
  }

  uint16_t tries = 0U;
  RA_BOUNDED_LOOP(k_xhtml_max_siblings)
  for (const tinyxml2::XMLElement* e = root->FirstChildElement();
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
 */
RA_INTERNAL
static uint16_t
s_collect_attrs(const tinyxml2::XMLElement* elem, ra_rabook_ctx_t* ctx, ra_book_attr_t* out)
{
  uint16_t count = 0U;
  RA_BOUNDED_LOOP(k_xhtml_max_attrs)
  for (const tinyxml2::XMLAttribute* a = elem->FirstAttribute();
       a != nullptr && count < (uint16_t)k_xhtml_max_attrs;
       a = a->Next(), ++count) {
    out[count].name_off  = ra_rabook_intern(ctx, a->Name());
    out[count].value_off = ra_rabook_intern(ctx, a->Value());
  }
  return count;
}

/* -------------------------------------------------------------------------- */
/* DFS frame */
/* -------------------------------------------------------------------------- */

/**
 * @struct ra_xhtml_frame_t
 * @brief One entry on the iterative DFS explicit call-stack.
 * @details @p prev_sib_idx is the ra_rabook index of the node's immediately
 *          preceding sibling (already added), or @ref k_ra_book_nil for the
 *          first child of a parent.
 * @since Version 0.1.0
 */
typedef struct {
  const tinyxml2::XMLNode* node;         /**< XML node to process next.              */
  uint32_t                 parent_idx;   /**< ra_rabook index of this node's parent. */
  uint32_t                 prev_sib_idx; /**< ra_rabook index of preceding sibling.  */
} ra_xhtml_frame_t;

/* -------------------------------------------------------------------------- */
/* Public API */
/* -------------------------------------------------------------------------- */

extern "C" {

RA_NO_RECURSION
ra_err_t ra_rabook_xml_parse_chapter(const uint8_t*   xhtml_bytes,
                                     size_t           xhtml_len,
                                     ra_rabook_ctx_t* ctx,
                                     const char*      chapter_href,
                                     const char*      chapter_title)
{
  RA_CHECK_NULL_PTR(xhtml_bytes, s_tag, "xhtml_bytes");
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  RA_CHECK_NULL_PTR(chapter_href, s_tag, "chapter_href");
  RA_CHECK_NULL_PTR(chapter_title, s_tag, "chapter_title");

  tinyxml2::XMLDocument doc;
  if (doc.Parse(reinterpret_cast<const char*>(xhtml_bytes), xhtml_len) != tinyxml2::XML_SUCCESS) {
    ra_log_error(s_tag, "tinyxml2 parse failed");
    return k_ra_err_no_mem;
  }

  const tinyxml2::XMLElement* body = s_find_body(doc);
  if (body == nullptr) {
    ra_log_error(s_tag, "no root element in XHTML");
    return k_ra_err_no_mem;
  }

  /* Add the <body> element as the chapter root. */
  ra_book_attr_t body_attrs[k_xhtml_max_attrs] = {};
  uint16_t       body_ac                       = s_collect_attrs(body, ctx, body_attrs);
  uint32_t       chapter_root =
    ra_rabook_add_element(ctx, ra_rabook_intern(ctx, body->Name()), body_attrs, body_ac);

  /* Iterative DFS -- pre-order, left-to-right.
   * Invariant: each popped frame is the next node to visit.
   * next_sibling is pushed before first_child so first_child is popped first,
   * giving the correct pre-order: node, node's subtree, node's sibling.
   */
  ra_xhtml_frame_t stack[k_xhtml_max_stack] = {};
  uint16_t         top                      = 0U;

  if (body->FirstChild() != nullptr && top < (uint16_t)k_xhtml_max_stack) {
    stack[top++] = {body->FirstChild(), chapter_root, k_ra_book_nil};
  }

  RA_BOUNDED_LOOP(k_xhtml_max_stack)
  while (top > 0U) {
    ra_xhtml_frame_t frame = stack[--top];

    const tinyxml2::XMLElement* elem    = frame.node->ToElement();
    const tinyxml2::XMLText*    text    = frame.node->ToText();
    uint32_t                    new_idx = k_ra_book_nil;

    if (elem != nullptr) {
      ra_book_attr_t attrs[k_xhtml_max_attrs] = {};
      uint16_t       ac                       = s_collect_attrs(elem, ctx, attrs);
      new_idx = ra_rabook_add_element(ctx, ra_rabook_intern(ctx, elem->Name()), attrs, ac);
    } else if (text != nullptr) {
      const char* val = text->Value();
      if (val != nullptr && val[0] != '\0') {
        new_idx = ra_rabook_add_text(ctx, ra_rabook_intern(ctx, val));
      }
    }
    /* else: comment / PI / CDATA -- skip (no children to push) */

    if (new_idx != k_ra_book_nil) {
      /* Link into the parent's child list or the preceding sibling chain. */
      if (frame.prev_sib_idx == k_ra_book_nil) {
        (void)ra_rabook_link_child(ctx, frame.parent_idx, new_idx);
      } else {
        (void)ra_rabook_link_sibling(ctx, frame.prev_sib_idx, new_idx);
      }
    }

    /* The effective "previous sibling" for the next sibling frame. */
    uint32_t sib_prev = (new_idx != k_ra_book_nil) ? new_idx : frame.prev_sib_idx;

    /* Push next_sibling BEFORE first_child (LIFO: first_child popped first). */
    const tinyxml2::XMLNode* next_sib = frame.node->NextSibling();
    if (next_sib != nullptr && top < (uint16_t)k_xhtml_max_stack) {
      stack[top++] = {next_sib, frame.parent_idx, sib_prev};
    }

    if (elem != nullptr && new_idx != k_ra_book_nil) {
      const tinyxml2::XMLNode* first_child = elem->FirstChild();
      if (first_child != nullptr && top < (uint16_t)k_xhtml_max_stack) {
        stack[top++] = {first_child, new_idx, k_ra_book_nil};
      }
    }
  }

  uint32_t title_off   = ra_rabook_intern(ctx, chapter_title);
  uint32_t href_off    = ra_rabook_intern(ctx, chapter_href);
  uint32_t chapter_idx = ra_rabook_add_chapter(ctx, title_off, href_off, chapter_root);
  return (chapter_idx != k_ra_book_nil) ? k_ra_ok : k_ra_err_no_mem;
}

} /* extern "C" */
