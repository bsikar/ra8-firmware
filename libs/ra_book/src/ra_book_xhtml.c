/**
 * @file ra_book_xhtml.c
 * @brief Serialize a compiled-book chapter DOM back to XHTML for the renderer.
 *
 * @details
 * `ra_reflow_layout_chapter()` consumes XHTML. This bridge walks a chapter's
 * pre-parsed `ra_book` DOM and re-emits well-formed XHTML so a compiled book
 * renders through the existing, MC/DC-tested reflow engine without modifying
 * it. The walk is iterative (an explicit open/close stack, NASA Rule 1) and
 * bounded (NASA Rule 2): nothing recurses, the stack and iteration count are
 * capped. Serialization is lossless -- every element tag, attribute and text
 * run is preserved; void elements self-close and text/attribute values are
 * entity-escaped.
 *
 * A future optimization can feed the DOM straight into the tokenizer and skip
 * this XHTML round-trip; until then the bridge keeps the renderer untouched.
 *
 * @since Version 1.0.0
 */
#include <string.h>

#include "ra_book.h"
#include "ra_check.h"

/** @brief Log tag for serializer diagnostics. */
static const char* const s_tag_xhtml = "ra_book_xhtml";

/**
 * @enum ra_book_xhtml_bound_t
 * @brief Bounds for the iterative, recursion-free DOM walk.
 * @since Version 1.0.0
 */
typedef enum : uint32_t {
  k_ra_book_xhtml_stack  = 512U, /**< Max pending open/close walk entries.       */
  k_ra_book_xhtml_iter_x = 4U,   /**< Iteration-guard multiplier over node_count. */
} ra_book_xhtml_bound_t;

/**
 * @struct ra_book_walk_entry_t
 * @brief One deferred unit of work on the serializer stack.
 * @details `is_close` distinguishes a pending close tag (carrying the tag-name
 *          string offset) from a node to open (carrying the node index).
 */
typedef struct {
  bool     is_close; /**< true: emit `</name>`; false: open node `value`.       */
  uint32_t value;    /**< Close: tag-name string offset. Open: node index.      */
} ra_book_walk_entry_t;

/** @brief Implementation of `ra_book_emit()` -- bounded byte-span append. */
static bool ra_book_emit(char* out, size_t cap, size_t* pos, const char* src, size_t len)
{
  if (*pos + len > cap) {
    return false;
  }
  memcpy(out + *pos, src, len);
  *pos += len;
  return true;
}

/** @brief Implementation of `ra_book_emit_cstr()` -- append a NUL-terminated string. */
static bool ra_book_emit_cstr(char* out, size_t cap, size_t* pos, const char* str)
{
  return ra_book_emit(out, cap, pos, str, strlen(str));
}

/**
 * @brief Implementation of `ra_book_emit_escaped()` -- append entity-escaped text.
 * @details Escapes `&`, `<`, `>` always and `"` when @p in_attr is true.
 */
static bool ra_book_emit_escaped(char* out, size_t cap, size_t* pos, const char* str, bool in_attr)
{
  for (const char* p = str; *p != '\0'; ++p) {
    bool ok = true;
    switch (*p) {
      case '&':
        ok = ra_book_emit_cstr(out, cap, pos, "&amp;");
        break;
      case '<':
        ok = ra_book_emit_cstr(out, cap, pos, "&lt;");
        break;
      case '>':
        ok = ra_book_emit_cstr(out, cap, pos, "&gt;");
        break;
      case '"':
        ok =
          in_attr ? ra_book_emit_cstr(out, cap, pos, "&quot;") : ra_book_emit(out, cap, pos, p, 1U);
        break;
      default:
        ok = ra_book_emit(out, cap, pos, p, 1U);
        break;
    }
    if (!ok) {
      return false;
    }
  }
  return true;
}

/** @brief Implementation of `ra_book_is_void()` -- HTML void (self-closing) element test. */
static bool ra_book_is_void(const char* name)
{
  static const char* const k_void[] = {
    "br",
    "hr",
    "img",
    "meta",
    "link",
    "input",
    "area",
    "base",
    "col",
    "embed",
    "param",
    "source",
    "track",
    "wbr",
  };
  for (size_t i = 0U; i < (sizeof(k_void) / sizeof(k_void[0])); ++i) {
    if (strcmp(name, k_void[i]) == 0) {
      return true;
    }
  }
  return false;
}

/** @brief Implementation of `ra_book_emit_attrs()` -- append an element's attributes. */
static bool
ra_book_emit_attrs(const void* base, const ra_book_node_t* node, char* out, size_t cap, size_t* pos)
{
  const ra_book_attr_t* attrs = ra_book_attrs(base);
  for (uint16_t i = 0U; i < node->attr_count; ++i) {
    const ra_book_attr_t* a = &attrs[node->first_attr + i];
    bool ok = ra_book_emit(out, cap, pos, " ", 1U) &&
              ra_book_emit_cstr(out, cap, pos, ra_book_string(base, a->name_off)) &&
              ra_book_emit(out, cap, pos, "=\"", 2U) &&
              ra_book_emit_escaped(out, cap, pos, ra_book_string(base, a->value_off), true) &&
              ra_book_emit(out, cap, pos, "\"", 1U);
    if (!ok) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Implementation of `ra_book_open_element()` -- emit an element open tag
 *        and schedule its close + children on the walk stack.
 * @return false on output overflow or stack exhaustion.
 */
static bool ra_book_open_element(const void*           base,
                                 const ra_book_node_t* node,
                                 char*                 out,
                                 size_t                cap,
                                 size_t*               pos,
                                 ra_book_walk_entry_t* stack,
                                 uint32_t*             sp)
{
  const char* name = ra_book_string(base, node->name_off);
  if (!ra_book_emit(out, cap, pos, "<", 1U) || !ra_book_emit_cstr(out, cap, pos, name) ||
      !ra_book_emit_attrs(base, node, out, cap, pos)) {
    return false;
  }
  if (ra_book_is_void(name)) {
    return ra_book_emit(out, cap, pos, "/>", 2U);
  }
  if (!ra_book_emit(out, cap, pos, ">", 1U) || (*sp + 2U > k_ra_book_xhtml_stack)) {
    return false;
  }
  stack[(*sp)++] = (ra_book_walk_entry_t){true, node->name_off};     /* close after children */
  stack[(*sp)++] = (ra_book_walk_entry_t){false, node->first_child}; /* children first       */
  return true;
}

/**
 * @brief Implementation of `ra_book_walk_to_xhtml()` -- iterative, bounded DOM
 *        walk that serializes the subtree rooted at @p root into @p out.
 * @return true on success; false on output overflow or stack/iteration exhaustion.
 */
static bool ra_book_walk_to_xhtml(const void* base,
                                  uint32_t    root,
                                  uint32_t    node_count,
                                  char*       out,
                                  size_t      cap,
                                  size_t*     pos)
{
  const ra_book_node_t* nodes = ra_book_nodes(base);
  ra_book_walk_entry_t  stack[k_ra_book_xhtml_stack];
  uint32_t              sp = 0U;
  bool                  ok = true;
  stack[sp++]              = (ra_book_walk_entry_t){false, root};

  const uint32_t max_iter = (node_count * k_ra_book_xhtml_iter_x) + k_ra_book_xhtml_stack;
  uint32_t       guard    = 0U;
  while (sp > 0U && ok && guard < max_iter) {
    ++guard;
    ra_book_walk_entry_t e = stack[--sp];
    if (e.is_close) {
      ok = ra_book_emit(out, cap, pos, "</", 2U) &&
           ra_book_emit_cstr(out, cap, pos, ra_book_string(base, e.value)) &&
           ra_book_emit(out, cap, pos, ">", 1U);
      continue;
    }
    if (e.value == k_ra_book_nil) {
      continue;
    }
    const ra_book_node_t* node = &nodes[e.value];
    if (sp >= k_ra_book_xhtml_stack) {
      return false;
    }
    /* Continue this level's sibling chain after the whole subtree + close. */
    stack[sp++] = (ra_book_walk_entry_t){false, node->next_sibling};
    if (node->kind == (uint8_t)k_ra_book_node_text) {
      ok = ra_book_emit_escaped(out, cap, pos, ra_book_string(base, node->text_off), false);
    } else {
      ok = ra_book_open_element(base, node, out, cap, pos, stack, &sp);
    }
  }
  return ok && (guard < max_iter);
}

ra_err_t ra_book_chapter_to_xhtml(const void* base,
                                  uint32_t    chapter_idx,
                                  char*       out,
                                  size_t      cap,
                                  size_t*     out_len)
{
  RA_CHECK_NULL_PTR(base, s_tag_xhtml, "to_xhtml: null base");
  RA_CHECK_NULL_PTR(out, s_tag_xhtml, "to_xhtml: null out");
  RA_CHECK_NULL_PTR(out_len, s_tag_xhtml, "to_xhtml: null out_len");

  const ra_book_header_t* hdr = ra_book_header(base);
  if (chapter_idx >= hdr->chapter_count) {
    return k_ra_err_invalid_arg;
  }

  const uint32_t root = ra_book_chapters(base)[chapter_idx].root_node;
  size_t         pos  = 0U;
  if (!ra_book_walk_to_xhtml(base, root, hdr->node_count, out, cap, &pos)) {
    return k_ra_err_invalid_size;
  }
  *out_len = pos;
  return k_ra_ok;
}
