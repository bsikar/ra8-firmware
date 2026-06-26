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
 * @since Version 0.1.0
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "ra_book.h"
#include "ra_book_paged.h"
#include "ra_check.h"

/** @brief Log tag for serializer diagnostics. */
static const char* const s_tag_xhtml = "ra_book_xhtml";

/**
 * @enum ra_book_xhtml_bound_t
 * @brief Bounds for the iterative, recursion-free DOM walk.
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_ra_book_xhtml_stack  = 512U, /**< Max pending open/close walk entries.        */
  k_ra_book_xhtml_iter_x = 4U,   /**< Iteration-guard multiplier over node_count. */
} ra_book_xhtml_bound_t;

/**
 * @struct ra_book_walk_entry_t
 * @brief One deferred unit of work on the serializer stack.
 * @details `is_close` distinguishes a pending close tag (carrying the tag-name
 *          string offset) from a node to open (carrying the node index).
 */
typedef struct {
  bool     is_close; /**< true: emit `</name>`; false: open node `value`.  */
  uint32_t value;    /**< Close: tag-name string offset. Open: node index. */
} ra_book_walk_entry_t;

/**
 * @brief Append a raw byte span to the output buffer.
 *
 * @details
 * Copies exactly @p len bytes from @p src into @p out starting at @p *pos,
 * advancing @p *pos by @p len on success.  If the copy would exceed @p cap
 * the buffer is left unchanged and the function returns false so callers can
 * chain with @c && and abort the entire serialisation on first overflow.
 *
 * @param[out]   out  Destination character buffer receiving the bytes.
 * @param[in]    cap  Total capacity of @p out in bytes.
 * @param[in,out] pos Current write offset into @p out; advanced by @p len
 *                    when the append succeeds.
 * @param[in]    src  Source byte span to copy (need not be NUL-terminated).
 * @param[in]    len  Number of bytes to copy from @p src.
 *
 * @return bool Append result.
 * @retval true  Bytes copied successfully; @p *pos advanced by @p len.
 * @retval false Output buffer would overflow; buffer and @p *pos unchanged.
 *
 * @pre  @p out is a valid, writable buffer of at least @p cap bytes.
 * @pre  @p pos is non-null and @p *pos <= @p cap on entry.
 * @post On success @p *pos equals the previous value plus @p len.
 * @post On failure @p *pos and @p out contents are unchanged.
 *
 * @note Not thread-safe; callers must provide external synchronisation.
 *
 * @since Version 0.1.0
 */
static bool ra_book_emit(char* out, size_t cap, size_t* pos, const char* src, size_t len)
{
  if (*pos + len > cap) {
    return false;
  }
  memcpy(out + *pos, src, len);
  *pos += len;
  return true;
}

/**
 * @brief Append a NUL-terminated string to the output buffer.
 *
 * @details
 * Determines the length of @p str with @c strlen and delegates to
 * @c ra_book_emit.  Provides a convenient wrapper so callers do not need to
 * compute string lengths manually when emitting tag names, attribute names, or
 * literal XML punctuation.
 *
 * @param[out]    out  Destination character buffer.
 * @param[in]     cap  Total capacity of @p out in bytes.
 * @param[in,out] pos  Current write offset; advanced by the string length on
 *                     success.
 * @param[in]     str  NUL-terminated source string to append.
 *
 * @return bool Append result.
 * @retval true  String appended; @p *pos advanced by @c strlen(str).
 * @retval false Output buffer would overflow; buffer and @p *pos unchanged.
 *
 * @pre  @p out is a valid, writable buffer of at least @p cap bytes.
 * @pre  @p str is a valid, NUL-terminated C string.
 * @post On success @p *pos equals the previous value plus @c strlen(str).
 * @post On failure @p *pos and @p out contents are unchanged.
 *
 * @note Not thread-safe; callers must provide external synchronisation.
 *
 * @since Version 0.1.0
 */
static bool ra_book_emit_cstr(char* out, size_t cap, size_t* pos, const char* str)
{
  return ra_book_emit(out, cap, pos, str, strlen(str));
}

/**
 * @brief Append entity-escaped text to the output buffer.
 *
 * @details
 * Iterates over every character in @p str and replaces XML-special characters
 * with their entity references before writing them to the output buffer via
 * @c ra_book_emit or @c ra_book_emit_cstr.  The substitution table is:
 *   - @c & becomes @c &amp;
 *   - @c < becomes @c &lt;
 *   - @c > becomes @c &gt;
 *   - @c " becomes @c &quot; only when @p in_attr is @c true; otherwise
 *     the literal @c " is emitted unchanged.
 * All other characters are forwarded as-is.  The function stops on the first
 * overflow, leaving @p *pos at the last successfully written position.
 *
 * @param[out]    out      Destination character buffer.
 * @param[in]     cap      Total capacity of @p out in bytes.
 * @param[in,out] pos      Current write offset; advanced for each character
 *                         (or its entity expansion) successfully appended.
 * @param[in]     str      NUL-terminated source text to escape and append.
 * @param[in]     in_attr  When @c true the context is an XML attribute value
 *                         and double-quotes are entity-escaped; when @c false
 *                         the context is element content and @c " is literal.
 *
 * @return bool Escape-and-append result.
 * @retval true  All characters written successfully.
 * @retval false Output buffer overflowed; partial output may have been written.
 *
 * @pre  @p out is a valid, writable buffer of at least @p cap bytes.
 * @pre  @p str is a valid, NUL-terminated C string.
 * @post On success every character of @p str has been entity-escaped and
 *       appended; @p *pos reflects the total bytes written.
 * @post On failure the write stops at the overflowing character; @p *pos
 *       reflects the number of bytes written before the overflow.
 *
 * @note Not thread-safe; callers must provide external synchronisation.
 *
 * @since Version 0.1.0
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

/**
 * @brief Test whether an element name is an HTML void element.
 *
 * @details
 * Performs a linear search through a fixed table of HTML void element names
 * (elements that must self-close and may not have child nodes in XHTML):
 * @c br, @c hr, @c img, @c meta, @c link, @c input, @c area, @c base,
 * @c col, @c embed, @c param, @c source, @c track, and @c wbr.  The
 * comparison is case-sensitive because compiled book DOM names are already
 * lower-cased during parsing.
 *
 * @param[in] name  NUL-terminated element name to test (e.g. @c "br").
 *
 * @return bool Query result.
 * @retval true  @p name matches a known void element; the serialiser must
 *               emit @c /> instead of a separate close tag.
 * @retval false @p name is not a void element; a close tag is required.
 *
 * @pre  @p name is a valid, NUL-terminated C string.
 * @pre  @p name contains only lower-case ASCII letters (DOM invariant).
 * @post The void-element table is not modified.
 * @post The return value is purely a function of @p name with no side effects.
 *
 * @note Not thread-safe; callers must provide external synchronisation.
 *
 * @since Version 0.1.0
 */
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

/**
 * @brief Append all attributes of a DOM node to the output buffer.
 *
 * @details
 * Iterates over the @c attr_count attribute slots starting at @p node->first_attr
 * in the flat attribute array returned by @c ra_book_attrs.  For each attribute
 * it emits a single space, the attribute name, @c ="  (with entity-escaped value),
 * and a closing @c " using @c ra_book_emit and @c ra_book_emit_escaped.  The
 * function is called while the opening tag is still open (before the @c > or
 * @c />) so the caller must emit the tag terminator after this returns.
 *
 * @param[in]     base  Pointer to the start of the compiled book blob; used to
 *                      resolve attribute name and value string offsets via
 *                      @c ra_book_string and @c ra_book_attrs.
 * @param[in]     node  DOM node whose attributes are to be serialised.
 * @param[out]    out   Destination character buffer.
 * @param[in]     cap   Total capacity of @p out in bytes.
 * @param[in,out] pos   Current write offset; advanced for each byte emitted.
 *
 * @return bool Serialisation result.
 * @retval true  All attributes emitted successfully.
 * @retval false Output buffer overflowed; partial output may have been written.
 *
 * @pre  @p base points to a valid, fully initialised compiled book blob.
 * @pre  @p node is a non-null pointer to an element node within the blob.
 * @post On success @p *pos reflects all attribute bytes written.
 * @post On failure the write stops at the overflowing attribute byte; @p *pos
 *       reflects the bytes written before the overflow.
 *
 * @note Not thread-safe; callers must provide external synchronisation.
 *
 * @since Version 0.1.0
 */
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
 * @brief Emit an element open tag and schedule its close and children on the
 *        walk stack.
 *
 * @details
 * Serialises the opening half of a DOM element node into @p out: emits @c <,
 * the tag name, all attributes via @c ra_book_emit_attrs, then either @c />
 * for void elements (using @c ra_book_is_void) or @c > for non-void elements.
 * For non-void elements two entries are pushed onto the caller-supplied
 * @p stack before returning: first a close entry (so the closing tag is
 * emitted after all descendants), then a child-list entry (so the first child
 * is processed next by the outer loop in @c ra_book_walk_to_xhtml).  Stack
 * indices grow upward; the current depth is held in @p *sp.
 *
 * @param[in]     base   Pointer to the compiled book blob; used to resolve
 *                       string offsets via @c ra_book_string.
 * @param[in]     node   Element node to open; must have kind == element.
 * @param[out]    out    Destination character buffer.
 * @param[in]     cap    Total capacity of @p out in bytes.
 * @param[in,out] pos    Current write offset; advanced for each byte emitted.
 * @param[in,out] stack  Caller-managed walk stack array of length
 *                       @c k_ra_book_xhtml_stack.
 * @param[in,out] sp     Stack pointer (count of live entries); incremented by
 *                       up to 2 for non-void elements.
 *
 * @return bool Operation result.
 * @retval true  Open tag emitted; child and close entries pushed for non-void.
 * @retval false Output buffer overflowed or stack is full.
 *
 * @pre  @p base points to a valid, fully initialised compiled book blob.
 * @pre  @p node is a non-null pointer to an element node; @p *sp < @c k_ra_book_xhtml_stack.
 * @post On success the open tag bytes are in @p out and @p *sp is incremented
 *       by 2 for non-void elements (0 for void elements).
 * @post On failure @p *pos and @p *sp may reflect partially completed writes;
 *       the caller must abort the walk.
 *
 * @note Not thread-safe; callers must provide external synchronisation.
 *
 * @since Version 0.1.0
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
 * @brief Iterative, bounded DOM walk that serialises a subtree to XHTML.
 *
 * @details
 * Performs a depth-first serialisation of the subtree rooted at @p root
 * without recursion.  A @c ra_book_walk_entry_t stack (maximum depth
 * @c k_ra_book_xhtml_stack) holds deferred work: each entry is either a
 * node-to-open or a pending close tag.  The main loop pops one entry per
 * iteration, emitting the appropriate text.  An iteration guard capped at
 * @c node_count * @c k_ra_book_xhtml_iter_x + @c k_ra_book_xhtml_stack
 * prevents any unbounded execution even if the DOM contains cycles.
 * Element nodes delegate opening (and child/close scheduling) to
 * @c ra_book_open_element; text nodes are emitted via @c ra_book_emit_escaped.
 *
 * @param[in]     base        Pointer to the compiled book blob.
 * @param[in]     root        Node index of the subtree root to serialise.
 * @param[in]     node_count  Total number of nodes in the blob; used to
 *                            compute the iteration guard bound.
 * @param[out]    out         Destination character buffer for the XHTML output.
 * @param[in]     cap         Total capacity of @p out in bytes.
 * @param[in,out] pos         Current write offset; advanced as bytes are emitted.
 *
 * @return bool Walk result.
 * @retval true  Entire subtree serialised without overflow or guard expiry.
 * @retval false Output buffer overflowed, stack exhausted, or iteration guard
 *               triggered (possible cycle in DOM).
 *
 * @pre  @p base points to a valid, fully initialised compiled book blob.
 * @pre  @p root is a valid node index within the blob (less than @p node_count).
 * @post On success @p *pos reflects all XHTML bytes written for the subtree.
 * @post On failure @p *pos reflects bytes written before the error condition.
 *
 * @note Not thread-safe; callers must provide external synchronisation.
 *
 * @since Version 0.1.0
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

/**
 * @brief Test whether an element name is a block-level HTML element.
 *
 * @details
 * Performs a linear search through a fixed table of block-level element names
 * used to decide where to insert paragraph-break newlines during plain-text
 * extraction.  The checked names are: @c p, @c h1, @c h2, @c h3, @c h4,
 * @c h5, @c h6, @c li, @c ul, @c ol, @c div, @c br, @c hr, @c section,
 * @c tr, @c pre, @c header, @c blockquote, @c article, @c aside, @c footer,
 * @c figure, and @c figcaption.  The comparison is case-sensitive because
 * compiled book DOM names are lower-cased during parsing.
 *
 * @param[in] name  NUL-terminated element name to test (e.g. @c "p").
 *
 * @return bool Query result.
 * @retval true  @p name is a block-level element; the text walker should emit
 *               a line break before descending into this element.
 * @retval false @p name is an inline element; no break is inserted.
 *
 * @pre  @p name is a valid, NUL-terminated C string.
 * @pre  @p name contains only lower-case ASCII letters (DOM invariant).
 * @post The block-element table is not modified.
 * @post The return value is purely a function of @p name with no side effects.
 *
 * @note Not thread-safe; callers must provide external synchronisation.
 *
 * @since Version 0.1.0
 */
static bool ra_book_is_block(const char* name)
{
  static const char* const k_block[] = {
    "p",      "h1",         "h2",      "h3",    "h4",     "h5",      "h6",         "li",
    "ul",     "ol",         "div",     "br",    "hr",     "section", "tr",         "pre",
    "header", "blockquote", "article", "aside", "footer", "figure",  "figcaption",
  };
  for (size_t i = 0U; i < (sizeof(k_block) / sizeof(k_block[0])); ++i) {
    if (strcmp(name, k_block[i]) == 0) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Append a whitespace-collapsed text run to the output buffer.
 *
 * @details
 * Iterates over @p str character by character applying the HTML
 * whitespace-collapsing rule: any run of space (@c ' '), tab (@c '\t'),
 * carriage return (@c '\r'), or newline (@c '\n') characters is folded into at
 * most a single ASCII space.  Non-whitespace characters are passed through
 * unchanged via @c ra_book_emit.  The @p at_break flag carries inter-call
 * state: when @c true any leading whitespace in the current fragment is
 * silently dropped, preventing pretty-print indentation between inline elements
 * from leaking into the rendered prose.  @p at_break is set to @c true after
 * a whitespace run is emitted and set to @c false after any non-whitespace
 * character is emitted.
 *
 * @param[out]    out       Destination character buffer.
 * @param[in]     cap       Total capacity of @p out in bytes.
 * @param[in,out] pos       Current write offset; advanced for each byte written.
 * @param[in]     str       NUL-terminated text fragment to collapse and append.
 * @param[in,out] at_break  On entry: @c true if the previous output ended with
 *                          a break or collapsed space that suppresses leading
 *                          whitespace.  On exit: updated to reflect the trailing
 *                          state of this fragment.
 *
 * @return bool Append result.
 * @retval true  Text run appended successfully (or was entirely suppressed
 *               whitespace).
 * @retval false Output buffer overflowed; partial output may have been written.
 *
 * @pre  @p out is a valid, writable buffer of at least @p cap bytes.
 * @pre  @p str is a valid, NUL-terminated C string.
 * @post On success every character in @p str has been processed and
 *       @p *pos reflects the bytes appended to @p out.
 * @post @p *at_break accurately reflects the trailing whitespace state after
 *       the call, regardless of success or failure.
 *
 * @note Not thread-safe; callers must provide external synchronisation.
 *
 * @since Version 0.1.0
 */
static bool ra_book_emit_text(char* out, size_t cap, size_t* pos, const char* str, bool* at_break)
{
  for (const char* p = str; *p != '\0'; ++p) {
    const char c  = *p;
    const bool ws = (c == ' ') || (c == '\t') || (c == '\n') || (c == '\r');
    if (!ws) {
      if (!ra_book_emit(out, cap, pos, &c, 1U)) {
        return false;
      }
      *at_break = false;
      continue;
    }
    if (*at_break) {
      continue;
    }
    if (!ra_book_emit(out, cap, pos, " ", 1U)) {
      return false;
    }
    *at_break = true;
  }
  return true;
}

/**
 * @brief Append a paragraph break, collapsing consecutive block-level breaks.
 *
 * @details
 * Trims any trailing space characters already written to @p out (by
 * decrementing @p *pos while the last byte is @c ' '), then sets @p *at_break
 * to @c true to suppress leading whitespace in the next text fragment.  If the
 * last byte already in @p out is @c '\n' no additional newline is written,
 * which collapses runs of breaks produced by consecutive or nested block-level
 * elements (e.g. a @c p inside a @c div) into a single blank line.  Otherwise
 * a single @c '\n' is appended via @c ra_book_emit.
 *
 * @param[out]    out       Destination character buffer.
 * @param[in]     cap       Total capacity of @p out in bytes.
 * @param[in,out] pos       Current write offset; may be decremented to trim
 *                          trailing spaces, then advanced by 1 if a newline is
 *                          emitted.
 * @param[in,out] at_break  Set to @c true on entry to suppress subsequent
 *                          leading whitespace; value on entry is ignored.
 *
 * @return bool Append result.
 * @retval true  Break emitted (or collapsed into an existing newline).
 * @retval false Output buffer overflowed while writing the newline byte.
 *
 * @pre  @p out is a valid, writable buffer of at least @p cap bytes.
 * @pre  @p pos is non-null and @p *pos <= @p cap on entry.
 * @post @p *at_break is @c true on return.
 * @post Trailing space characters in @p out before the current @p *pos have
 *       been removed; at most one @c '\n' is appended.
 *
 * @note Not thread-safe; callers must provide external synchronisation.
 *
 * @since Version 0.1.0
 */
static bool ra_book_emit_break(char* out, size_t cap, size_t* pos, bool* at_break)
{
  while ((*pos > 0U) && (out[*pos - 1U] == ' ')) {
    (*pos)--;
  }
  *at_break = true;
  if ((*pos > 0U) && (out[*pos - 1U] == '\n')) {
    return true;
  }
  return ra_book_emit(out, cap, pos, "\n", 1U);
}

/**
 * @brief Bounded pre-order walk that extracts plain text from a DOM subtree.
 *
 * @details
 * Traverses the subtree rooted at @p root in pre-order using an iterative
 * stack (maximum depth @c k_ra_book_xhtml_stack) without recursion.  An
 * iteration guard capped at @c node_count * @c k_ra_book_xhtml_iter_x +
 * @c k_ra_book_xhtml_stack prevents unbounded execution.  For each node:
 *   - Text nodes are handed to @c ra_book_emit_text for whitespace collapsing.
 *   - Element nodes that are block-level (tested via @c ra_book_is_block)
 *     trigger @c ra_book_emit_break to insert a paragraph separator before
 *     descending into the element's children.
 *   - All other element nodes descend immediately with no separator.
 * The @c at_break flag threads through all text and break calls so that
 * leading whitespace at the start of each block is suppressed.
 *
 * @param[in]     base        Pointer to the compiled book blob.
 * @param[in]     root        Node index of the subtree root to extract text from.
 * @param[in]     node_count  Total number of nodes in the blob; used to compute
 *                            the iteration guard bound.
 * @param[out]    out         Destination character buffer for the plain text.
 * @param[in]     cap         Total capacity of @p out in bytes.
 * @param[in,out] pos         Current write offset; advanced as bytes are emitted.
 *
 * @return bool Walk result.
 * @retval true  Entire subtree processed; plain text in @p out at @p *pos bytes.
 * @retval false Output buffer overflowed, stack exhausted, or iteration guard
 *               triggered (possible cycle in DOM).
 *
 * @pre  @p base points to a valid, fully initialised compiled book blob.
 * @pre  @p root is a valid node index within the blob (less than @p node_count).
 * @post On success @p *pos equals the number of plain-text bytes written.
 * @post On failure @p *pos reflects bytes written before the error condition.
 *
 * @note Not thread-safe; callers must provide external synchronisation.
 *
 * @since Version 0.1.0
 */
static bool ra_book_walk_text(const void* base,
                              uint32_t    root,
                              uint32_t    node_count,
                              char*       out,
                              size_t      cap,
                              size_t*     pos)
{
  const ra_book_node_t* nodes = ra_book_nodes(base);
  uint32_t              stack[k_ra_book_xhtml_stack];
  uint32_t              sp       = 0U;
  bool                  ok       = true;
  bool                  at_break = true;
  stack[sp++]                    = root;

  const uint32_t max_iter = (node_count * k_ra_book_xhtml_iter_x) + k_ra_book_xhtml_stack;
  uint32_t       guard    = 0U;
  while (sp > 0U && ok && guard < max_iter) {
    ++guard;
    const uint32_t n = stack[--sp];
    if (n == k_ra_book_nil) {
      continue;
    }
    const ra_book_node_t* node = &nodes[n];
    if (sp >= k_ra_book_xhtml_stack) {
      return false;
    }
    stack[sp++] = node->next_sibling; /* sibling chain after this subtree */
    if (node->kind == (uint8_t)k_ra_book_node_text) {
      ok = ra_book_emit_text(out, cap, pos, ra_book_string(base, node->text_off), &at_break);
      continue;
    }
    if (ra_book_is_block(ra_book_string(base, node->name_off))) {
      ok = ra_book_emit_break(out, cap, pos, &at_break);
    }
    if (ok && (sp < k_ra_book_xhtml_stack)) {
      stack[sp++] = node->first_child; /* descend, pre-order */
    }
  }
  return ok && (guard < max_iter);
}

ra_err_t
ra_book_chapter_text(const void* base, uint32_t chapter_idx, char* out, size_t cap, size_t* out_len)
{
  RA_CHECK_NULL_PTR(base, s_tag_xhtml, "text: null base");
  RA_CHECK_NULL_PTR(out, s_tag_xhtml, "text: null out");
  RA_CHECK_NULL_PTR(out_len, s_tag_xhtml, "text: null out_len");

  const ra_book_header_t* hdr = ra_book_header(base);
  if (chapter_idx >= hdr->chapter_count) {
    return k_ra_err_invalid_arg;
  }
  const uint32_t root = ra_book_chapters(base)[chapter_idx].root_node;
  size_t         pos  = 0U;
  if (!ra_book_walk_text(base, root, hdr->node_count, out, cap, &pos)) {
    return k_ra_err_invalid_size;
  }
  *out_len = pos;
  return k_ra_ok;
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

/* ===========================================================================
 * Paged plain-text extraction (#163): same output as ra_book_chapter_text but
 * the DOM is read frame-by-frame through an ra_book_src_t / ra_vmem cache, so a
 * book that exceeds the resident budget is walked with a bounded working set.
 * The resident path above is untouched; these helpers reuse the shared,
 * source-agnostic emit_text / emit_break / is_block leaf helpers on staging
 * buffers, so paged output is byte-for-byte identical to the resident walk.
 * =========================================================================== */

/**
 * @enum ra_book_paged_walk_bound_t
 * @brief Staging sizes and loop bounds for the paged text walk (no magic numbers).
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_ra_book_paged_strbuf = 256U, /**< Staging chunk for a paged text run.       */
  k_ra_book_paged_tagbuf = 64U,  /**< Staging for one (short) element tag name. */
  k_ra_book_paged_pad    = 2U,   /**< Loop-bound headroom over the exact span.  */
} ra_book_paged_walk_bound_t;

/**
 * @brief Read one DOM node record out of a paged/resident book source by index.
 *
 * @details Translates the node index to its blob byte offset
 *          (`node_off + idx * sizeof(ra_book_node_t)`) and copies the 24-byte
 *          record out via ::ra_book_src_read, which bounds-checks the range and
 *          faults the frame in for a paged source.
 *
 * @param[in]  src Bound book source.
 * @param[in]  idx Node-table index to read.
 * @param[out] out Receives the node record.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Node copied into @p out.
 * @retval k_ra_err_out_of_range @p idx lies outside the node table.
 * @retval k_ra_err_*            A ::ra_book_src_read fault (returned verbatim).
 *
 * @pre  @p src is bound and @p out is writable.
 * @pre  @p idx is intended to index a valid node (verified by the read bounds).
 * @post On success @p out holds the node at @p idx.
 * @post On any non-ok return @p out content is unspecified.
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra_err_t ra_book_paged_node(const ra_book_src_t* src, uint32_t idx, ra_book_node_t* out)
{
  const uint32_t off = src->hdr.node_off + (idx * (uint32_t)sizeof(ra_book_node_t));
  return ra_book_src_read(src, off, out, (uint32_t)sizeof(ra_book_node_t));
}

/**
 * @brief Copy a short NUL-terminated string (a tag name) out into a buffer.
 *
 * @details Reads up to `cap - 1` bytes at @p abs_off and NUL-terminates at the
 *          first embedded NUL or at the buffer end. Used for element tag names,
 *          which are short; a name longer than the buffer is truncated, which at
 *          worst makes ::ra_book_is_block treat it as inline (no break) -- never
 *          a correctness issue for real markup.
 *
 * @param[in]  src     Bound book source.
 * @param[in]  abs_off Absolute blob offset of the string start.
 * @param[out] buf     Destination buffer (>= @p cap bytes).
 * @param[in]  cap     Capacity of @p buf in bytes (>= 1).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               String (possibly truncated) copied + terminated.
 * @retval k_ra_err_out_of_range @p abs_off is at or past the blob end.
 * @retval k_ra_err_*            A ::ra_book_src_read fault (returned verbatim).
 *
 * @pre  @p src is bound and @p buf has room for @p cap bytes.
 * @pre  @p cap is at least one (for the NUL terminator).
 * @post On success @p buf is a NUL-terminated string.
 * @post On any non-ok return @p buf content is unspecified.
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra_err_t
ra_book_paged_str_short(const ra_book_src_t* src, uint32_t abs_off, char* buf, uint32_t cap)
{
  if (abs_off >= src->size) {
    return k_ra_err_out_of_range;
  }
  const uint32_t remain = src->size - abs_off;
  const uint32_t want   = cap - 1U;
  const uint32_t chunk  = (remain < want) ? remain : want;
  const ra_err_t re     = ra_book_src_read(src, abs_off, buf, chunk);
  if (re != k_ra_ok) {
    return re;
  }
  uint32_t nlen = 0U;
  while ((nlen < chunk) && (buf[nlen] != '\0')) {
    ++nlen;
  }
  buf[nlen] = '\0';
  return k_ra_ok;
}

/**
 * @brief Emit one text run from a paged source, collapsing whitespace.
 *
 * @details Streams the NUL-terminated run at @p abs_off through a staging buffer
 *          in `k_ra_book_paged_strbuf`-sized chunks, calling the shared
 *          ::ra_book_emit_text on each chunk. Chunking keeps the resident
 *          footprint bounded for arbitrarily long runs; the `at_break` flag
 *          threads across chunks so whitespace collapsing is identical to the
 *          resident single-pass walk.
 *
 * @param[in]     src      Bound book source.
 * @param[in]     abs_off  Absolute blob offset of the NUL-terminated run.
 * @param[out]    out      Destination text buffer.
 * @param[in]     cap      Capacity of @p out in bytes.
 * @param[in,out] pos      Current write offset; advanced as text is emitted.
 * @param[in,out] at_break Whitespace-collapse carry flag threaded across chunks.
 * @param[out]    io_err   Set to the fault code on a read fault; untouched otherwise.
 *
 * @return bool Emit result.
 * @retval true  The whole run was emitted; @p *io_err is unchanged.
 * @retval false Output buffer overflowed (@p io_err untouched), or a page fault
 *               occurred (@p *io_err set to the fault code).
 *
 * @pre  @p src is bound; @p out has room for @p cap bytes.
 * @pre  @p pos and @p at_break are non-null and valid.
 * @post On true with `*io_err == k_ra_ok` the run is fully emitted.
 * @post On a fault `*io_err` holds the fault code and false is returned.
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static bool ra_book_paged_emit_run(const ra_book_src_t* src,
                                   uint32_t             abs_off,
                                   char*                out,
                                   size_t               cap,
                                   size_t*              pos,
                                   bool*                at_break,
                                   ra_err_t*            io_err)
{
  char           buf[k_ra_book_paged_strbuf];
  uint32_t       off = abs_off;
  const uint32_t max_iter =
    (src->size / (uint32_t)(k_ra_book_paged_strbuf - 1U)) + (uint32_t)k_ra_book_paged_pad;
  for (uint32_t it = 0U; it < max_iter; ++it) {
    if (off >= src->size) {
      return true; /* Ran to blob end without a NUL (degenerate); treat as done. */
    }
    const uint32_t remain = src->size - off;
    const uint32_t want   = (uint32_t)(k_ra_book_paged_strbuf - 1U);
    const uint32_t chunk  = (remain < want) ? remain : want;
    const ra_err_t re     = ra_book_src_read(src, off, buf, chunk);
    if (re != k_ra_ok) {
      *io_err = re;
      return false;
    }
    uint32_t nlen = 0U;
    while ((nlen < chunk) && (buf[nlen] != '\0')) {
      ++nlen;
    }
    buf[nlen] = '\0';
    if (!ra_book_emit_text(out, cap, pos, buf, at_break)) {
      return false; /* Output overflow. */
    }
    if (nlen < chunk) {
      return true; /* Hit the real NUL terminator. */
    }
    off += nlen; /* nlen == chunk: advance and read the next chunk. */
  }
  return true;
}

/**
 * @brief Bounded pre-order text walk over a paged book source.
 *
 * @details The paged counterpart of ra_book_walk_text(): identical iterative,
 *          recursion-free pre-order traversal and identical output, but each
 *          node is read with ::ra_book_paged_node and each string is streamed
 *          out on demand (::ra_book_paged_emit_run for text, a staged tag-name
 *          copy for ::ra_book_is_block). At most one cache frame is pinned at a
 *          time, so the resident working set stays bounded.
 *
 * @param[in]     src        Bound (paged) book source.
 * @param[in]     root       Node index of the subtree root.
 * @param[in]     node_count Total node count, for the iteration guard.
 * @param[out]    out        Destination plain-text buffer.
 * @param[in]     cap        Capacity of @p out in bytes.
 * @param[in,out] pos        Current write offset; advanced as text is emitted.
 * @param[out]    io_err     Set to the fault code on a read fault; k_ra_ok otherwise.
 *
 * @return bool Walk result.
 * @retval true  Subtree fully serialised (check @p io_err for a clean run).
 * @retval false Overflow, stack/guard exhaustion, or a read fault (see @p io_err).
 *
 * @pre  @p src is a paged source bound by ra_book_src_paged().
 * @pre  @p root is a node index within the blob.
 * @post On true with `*io_err == k_ra_ok`, @p out holds the chapter text.
 * @post On a fault `*io_err` carries the fault code.
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static bool ra_book_walk_text_paged(const ra_book_src_t* src,
                                    uint32_t             root,
                                    uint32_t             node_count,
                                    char*                out,
                                    size_t               cap,
                                    size_t*              pos,
                                    ra_err_t*            io_err)
{
  uint32_t stack[k_ra_book_xhtml_stack];
  uint32_t sp       = 0U;
  bool     ok       = true;
  bool     at_break = true;
  stack[sp++]       = root;

  const uint32_t max_iter = (node_count * k_ra_book_xhtml_iter_x) + k_ra_book_xhtml_stack;
  uint32_t       guard    = 0U;
  while ((sp > 0U) && ok && (guard < max_iter)) {
    ++guard;
    const uint32_t n = stack[--sp];
    if (n == k_ra_book_nil) {
      continue;
    }
    ra_book_node_t node = {};
    const ra_err_t ne   = ra_book_paged_node(src, n, &node);
    if (ne != k_ra_ok) {
      *io_err = ne;
      return false;
    }
    if (sp >= k_ra_book_xhtml_stack) {
      return false;
    }
    stack[sp++] = node.next_sibling; /* sibling chain after this subtree */
    if (node.kind == (uint8_t)k_ra_book_node_text) {
      ok = ra_book_paged_emit_run(src,
                                  src->hdr.string_off + node.text_off,
                                  out,
                                  cap,
                                  pos,
                                  &at_break,
                                  io_err);
      continue;
    }
    char           tag[k_ra_book_paged_tagbuf] = {};
    const ra_err_t te = ra_book_paged_str_short(src,
                                                src->hdr.string_off + node.name_off,
                                                tag,
                                                (uint32_t)k_ra_book_paged_tagbuf);
    if (te != k_ra_ok) {
      *io_err = te;
      return false;
    }
    if (ra_book_is_block(tag)) {
      ok = ra_book_emit_break(out, cap, pos, &at_break);
    }
    if (ok && (sp < k_ra_book_xhtml_stack)) {
      stack[sp++] = node.first_child; /* descend, pre-order */
    }
  }
  return ok && (guard < max_iter);
}

ra_err_t ra_book_chapter_text_src(const ra_book_src_t* src,
                                  uint32_t             chapter_idx,
                                  char*                out,
                                  size_t               cap,
                                  size_t*              out_len)
{
  RA_CHECK_NULL_PTR(src, s_tag_xhtml, "text_src: null src");
  RA_CHECK_NULL_PTR(out, s_tag_xhtml, "text_src: null out");
  RA_CHECK_NULL_PTR(out_len, s_tag_xhtml, "text_src: null out_len");

  if (chapter_idx >= src->hdr.chapter_count) {
    return k_ra_err_invalid_arg;
  }
  /* Resident source: delegate to the untouched, byte-identical fast path. */
  if (src->base != nullptr) {
    return ra_book_chapter_text(src->base, chapter_idx, out, cap, out_len);
  }

  /* Paged source: read the chapter record, then walk the DOM frame-by-frame. */
  ra_book_chapter_t chap = {};
  const uint32_t coff = src->hdr.chapter_off + (chapter_idx * (uint32_t)sizeof(ra_book_chapter_t));
  const ra_err_t ce   = ra_book_src_read(src, coff, &chap, (uint32_t)sizeof(ra_book_chapter_t));
  if (ce != k_ra_ok) {
    return ce;
  }
  size_t   pos    = 0U;
  ra_err_t io_err = k_ra_ok;
  if (!ra_book_walk_text_paged(src, chap.root_node, src->hdr.node_count, out, cap, &pos, &io_err)) {
    return (io_err != k_ra_ok) ? io_err : k_ra_err_invalid_size;
  }
  *out_len = pos;
  return k_ra_ok;
}
