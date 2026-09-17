/**
 * @file book_xhtml.c
 * @brief Serialize a compiled-book chapter DOM back to XHTML for the renderer.
 *
 * @details
 * `reflow_layout_chapter()` consumes XHTML. This bridge walks a chapter's
 * pre-parsed `book` DOM and re-emits well-formed XHTML so a compiled book
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
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 *
 */
#include <string.h>

#include "book.h"
#include "book_internal.h"
#include "book_paged.h"
#include "ra8_attributes.h"
#include "ra8_check.h"

/** @brief Log tag for serializer diagnostics. */
static const char* const s_tag_xhtml = "book_xhtml";

/**
 * @struct book_walk_entry_t
 * @brief One deferred unit of work on the serializer stack.
 * @details `is_close` distinguishes a pending close tag (carrying the tag-name
 *          string offset) from a node to open (carrying the node index).
 */
typedef struct {
  bool     is_close; /**< true: emit `</name>`; false: open node `value`.  */
  uint32_t value;    /**< Close: tag-name string offset. Open: node index. */
} book_walk_entry_t;

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
RA8_INTERNAL static bool
internal_emit(char* out, size_t cap, size_t* pos, const char* src, size_t len)
{
  if (len > (cap - *pos)) {
    return false;
  }
  (void)memcpy(&out[*pos], src, len);
  *pos += len;
  return true;
}

/**
 * @brief Append a NUL-terminated string to the output buffer.
 *
 * @details
 * Determines the length of @p str with @c strlen and delegates to
 * @c internal_emit.  Provides a convenient wrapper so callers do not need to
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
RA8_INTERNAL static bool internal_emit_cstr(char* out, size_t cap, size_t* pos, const char* str)
{
  return internal_emit(out, cap, pos, str, strlen(str));
}

/**
 * @brief Append entity-escaped text to the output buffer.
 *
 * @details
 * Iterates over every character in @p str and replaces XML-special characters
 * with their entity references before writing them to the output buffer via
 * @c internal_emit or @c internal_emit_cstr.  The substitution table is:
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
RA8_INTERNAL static bool
internal_emit_escaped(char* out, size_t cap, size_t* pos, const char* str, bool in_attr)
{
  for (const char* p = str; *p != '\0'; ++p) {
    bool ok = true;
    switch (*p) {
      case '&':
        ok = internal_emit_cstr(out, cap, pos, "&amp;");
        break;
      case '<':
        ok = internal_emit_cstr(out, cap, pos, "&lt;");
        break;
      case '>':
        ok = internal_emit_cstr(out, cap, pos, "&gt;");
        break;
      case '"':
        ok = in_attr ? internal_emit_cstr(out, cap, pos, "&quot;")
                     : internal_emit(out, cap, pos, p, 1U);
        break;
      default:
        ok = internal_emit(out, cap, pos, p, 1U);
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
RA8_INTERNAL static bool internal_is_void(const char* name)
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
 * in the flat attribute array returned by @c book_attrs.  For each attribute
 * it emits a single space, the attribute name, @c ="  (with entity-escaped value),
 * and a closing @c " using @c internal_emit and @c internal_emit_escaped.  The
 * function is called while the opening tag is still open (before the @c > or
 * @c />) so the caller must emit the tag terminator after this returns.
 *
 * @param[in]     base  Pointer to the start of the compiled book blob; used to
 *                      resolve attribute name and value string offsets via
 *                      @c book_string and @c book_attrs.
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
RA8_INTERNAL static bool
internal_emit_attrs(const void* base, const book_node_t* node, char* out, size_t cap, size_t* pos)
{
  const book_attr_t* attrs = book_attrs(base);
  for (uint16_t i = 0U; i < node->attr_count; ++i) {
    const book_attr_t* a = &attrs[node->first_attr + i];
    bool ok = internal_emit(out, cap, pos, " ", 1U) &&
              internal_emit_cstr(out, cap, pos, book_string(base, a->name_off)) &&
              internal_emit(out, cap, pos, "=\"", 2U) &&
              internal_emit_escaped(out, cap, pos, book_string(base, a->value_off), true) &&
              internal_emit(out, cap, pos, "\"", 1U);
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
 * the tag name, all attributes via @c internal_emit_attrs, then either @c />
 * for void elements (using @c internal_is_void) or @c > for non-void elements.
 * For non-void elements two entries are pushed onto the caller-supplied
 * @p stack before returning: first a close entry (so the closing tag is
 * emitted after all descendants), then a child-list entry (so the first child
 * is processed next by the outer loop in @c internal_walk_to_xhtml).  Stack
 * indices grow upward; the current depth is held in @p *sp.
 *
 * @param[in]     base   Pointer to the compiled book blob; used to resolve
 *                       string offsets via @c book_string.
 * @param[in]     node   Element node to open; must have kind == element.
 * @param[out]    out    Destination character buffer.
 * @param[in]     cap    Total capacity of @p out in bytes.
 * @param[in,out] pos    Current write offset; advanced for each byte emitted.
 * @param[in,out] stack  Caller-managed walk stack array of length
 *                       @c k_book_xhtml_stack.
 * @param[in,out] sp     Stack pointer (count of live entries); incremented by
 *                       up to 2 for non-void elements.
 *
 * @return bool Operation result.
 * @retval true  Open tag emitted; child and close entries pushed for non-void.
 * @retval false Output buffer overflowed or stack is full.
 *
 * @pre  @p base points to a valid, fully initialised compiled book blob.
 * @pre  @p node is a non-null pointer to an element node; @p *sp < @c k_book_xhtml_stack.
 * @post On success the open tag bytes are in @p out and @p *sp is incremented
 *       by 2 for non-void elements (0 for void elements).
 * @post On failure @p *pos and @p *sp may reflect partially completed writes;
 *       the caller must abort the walk.
 *
 * @note Not thread-safe; callers must provide external synchronisation.
 *
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool internal_open_element(const void*        base,
                                               const book_node_t* node,
                                               char*              out,
                                               size_t             cap,
                                               size_t*            pos,
                                               book_walk_entry_t* stack,
                                               uint32_t*          sp)
{
  const char* name = book_string(base, node->name_off);
  if (!internal_emit(out, cap, pos, "<", 1U) || !internal_emit_cstr(out, cap, pos, name) ||
      !internal_emit_attrs(base, node, out, cap, pos)) {
    return false;
  }
  if (internal_is_void(name)) {
    return internal_emit(out, cap, pos, "/>", 2U);
  }
  if (!internal_emit(out, cap, pos, ">", 1U) || ((*sp + 2U) > k_book_xhtml_stack)) {
    return false;
  }
  stack[*sp] = (book_walk_entry_t){true, node->name_off}; /* close after children */
  *sp += 1U;
  stack[*sp] = (book_walk_entry_t){false, node->first_child}; /* children first */
  *sp += 1U;
  return true;
}

/**
 * @brief Iterative, bounded DOM walk that serialises a subtree to XHTML.
 *
 * @details
 * Performs a depth-first serialisation of the subtree rooted at @p root
 * without recursion.  A @c book_walk_entry_t stack (maximum depth
 * @c k_book_xhtml_stack) holds deferred work: each entry is either a
 * node-to-open or a pending close tag.  The main loop pops one entry per
 * iteration, emitting the appropriate text.  An iteration guard capped at
 * @c node_count * @c k_book_xhtml_iter_x + @c k_book_xhtml_stack
 * prevents any unbounded execution even if the DOM contains cycles.
 * Element nodes delegate opening (and child/close scheduling) to
 * @c internal_open_element; text nodes are emitted via @c internal_emit_escaped.
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
RA8_INTERNAL static bool internal_walk_to_xhtml(const void* base,
                                                uint32_t    root,
                                                uint32_t    node_count,
                                                char*       out,
                                                size_t      cap,
                                                size_t*     pos)
{
  const book_node_t* nodes = book_nodes(base);
  /* Explicit DFS stack (~4 KiB) kept in module-static storage so this frame
   * stays within the stack-usage budget; the walk is iterative (no recursion)
   * and single-threaded, so the shared buffer never overlaps. */
  static book_walk_entry_t s_xhtml_stack[k_book_xhtml_stack];
  book_walk_entry_t*       stack = s_xhtml_stack;
  uint32_t                 sp    = 0U;
  bool                     ok    = true;
  stack[sp]                      = (book_walk_entry_t){false, root};
  sp += 1U;

  const uint32_t max_iter = (node_count * k_book_xhtml_iter_x) + k_book_xhtml_stack;
  uint32_t       guard    = 0U;
  while ((sp > 0U) && ok && (guard < max_iter)) {
    ++guard;
    sp -= 1U;
    book_walk_entry_t e = stack[sp];
    if (e.is_close) {
      ok = internal_emit(out, cap, pos, "</", 2U) &&
           internal_emit_cstr(out, cap, pos, book_string(base, e.value)) &&
           internal_emit(out, cap, pos, ">", 1U);
      continue;
    }
    if (e.value == k_book_nil) {
      continue;
    }
    const book_node_t* node = &nodes[e.value];
    if (sp >= k_book_xhtml_stack) {
      return false;
    }
    /* Continue this level's sibling chain after the whole subtree + close. */
    stack[sp] = (book_walk_entry_t){false, node->next_sibling};
    sp += 1U;
    if (node->kind == (uint8_t)k_book_node_text) {
      ok = internal_emit_escaped(out, cap, pos, book_string(base, node->text_off), false);
    } else {
      ok = internal_open_element(base, node, out, cap, pos, stack, &sp);
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
RA8_PRIV bool priv_book_is_block(const char* name)
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
 * whitespace-collapsing rule: any run of space (`' '`), tab (`'\t'`),
 * carriage return (`'\r'`), or newline (`'\n'`) characters is folded into at
 * most a single ASCII space.  Non-whitespace characters are passed through
 * unchanged via @c internal_emit.  The @p at_break flag carries inter-call
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
RA8_PRIV bool
priv_book_emit_text(char* out, size_t cap, size_t* pos, const char* str, bool* at_break)
{
  for (const char* p = str; *p != '\0'; ++p) {
    const char c  = *p;
    const bool ws = (c == ' ') || (c == '\t') || (c == '\n') || (c == '\r');
    if (!ws) {
      if (!internal_emit(out, cap, pos, &c, 1U)) {
        return false;
      }
      *at_break = false;
      continue;
    }
    if (*at_break) {
      continue;
    }
    if (!internal_emit(out, cap, pos, " ", 1U)) {
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
 * decrementing @p *pos while the last byte is `' '`), then sets @p *at_break
 * to @c true to suppress leading whitespace in the next text fragment.  If the
 * last byte already in @p out is `'\n'` no additional newline is written,
 * which collapses runs of breaks produced by consecutive or nested block-level
 * elements (e.g. a @c p inside a @c div) into a single blank line.  Otherwise
 * a single `'\n'` is appended via @c internal_emit.
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
 *       been removed; at most one `'\n'` is appended.
 *
 * @note Not thread-safe; callers must provide external synchronisation.
 *
 * @since Version 0.1.0
 */
RA8_PRIV bool priv_book_emit_break(char* out, size_t cap, size_t* pos, bool* at_break)
{
  while ((*pos > 0U) && (out[*pos - 1U] == ' ')) {
    (*pos)--;
  }
  *at_break = true;
  if ((*pos > 0U) && (out[*pos - 1U] == '\n')) {
    return true;
  }
  return internal_emit(out, cap, pos, "\n", 1U);
}

/**
 * @brief Bounded pre-order walk that extracts plain text from a DOM subtree.
 *
 * @details
 * Traverses the subtree rooted at @p root in pre-order using an iterative
 * stack (maximum depth @c k_book_xhtml_stack) without recursion.  An
 * iteration guard capped at @c node_count * @c k_book_xhtml_iter_x +
 * @c k_book_xhtml_stack prevents unbounded execution.  For each node:
 *   - Text nodes are handed to @c priv_book_emit_text for whitespace collapsing.
 *   - Element nodes that are block-level (tested via @c priv_book_is_block)
 *     trigger @c priv_book_emit_break to insert a paragraph separator before
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
RA8_INTERNAL static bool internal_walk_text(const void* base,
                                            uint32_t    root,
                                            uint32_t    node_count,
                                            char*       out,
                                            size_t      cap,
                                            size_t*     pos)
{
  const book_node_t* nodes = book_nodes(base);
  /* Explicit DFS stack (2 KiB) kept in module-static storage so this frame
   * stays within the stack-usage budget; iterative (no recursion) and
   * single-threaded, so the shared buffer never overlaps. */
  static uint32_t s_text_stack[k_book_xhtml_stack];
  uint32_t*       stack    = s_text_stack;
  uint32_t        sp       = 0U;
  bool            ok       = true;
  bool            at_break = true;
  stack[sp]                = root;
  sp += 1U;

  const uint32_t max_iter = (node_count * k_book_xhtml_iter_x) + k_book_xhtml_stack;
  uint32_t       guard    = 0U;
  while ((sp > 0U) && ok && (guard < max_iter)) {
    ++guard;
    sp -= 1U;
    const uint32_t n = stack[sp];
    if (n == k_book_nil) {
      continue;
    }
    const book_node_t* node = &nodes[n];
    if (sp >= k_book_xhtml_stack) {
      return false;
    }
    stack[sp] = node->next_sibling; /* sibling chain after this subtree */
    sp += 1U;
    if (node->kind == (uint8_t)k_book_node_text) {
      ok = priv_book_emit_text(out, cap, pos, book_string(base, node->text_off), &at_break);
      continue;
    }
    if (priv_book_is_block(book_string(base, node->name_off))) {
      ok = priv_book_emit_break(out, cap, pos, &at_break);
    }
    if (ok && (sp < k_book_xhtml_stack)) {
      stack[sp] = node->first_child; /* descend, pre-order */
      sp += 1U;
    }
  }
  return ok && (guard < max_iter);
}

ra8_err_t
book_chapter_text(const void* base, uint32_t chapter_idx, char* out, size_t cap, size_t* out_len)
{
  RA8_CHECK_NULL_PTR(base, s_tag_xhtml, "text: null base");
  RA8_CHECK_NULL_PTR(out, s_tag_xhtml, "text: null out");
  RA8_CHECK_NULL_PTR(out_len, s_tag_xhtml, "text: null out_len");

  const book_header_t* hdr = book_header(base);
  if (chapter_idx >= hdr->chapter_count) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t root = book_chapters(base)[chapter_idx].root_node;
  size_t         pos  = 0U;
  if (!internal_walk_text(base, root, hdr->node_count, out, cap, &pos)) {
    return k_ra8_err_invalid_size;
  }
  *out_len = pos;
  return k_ra8_ok;
}

ra8_err_t book_chapter_to_xhtml(const void* base,
                                uint32_t    chapter_idx,
                                char*       out,
                                size_t      cap,
                                size_t*     out_len)
{
  RA8_CHECK_NULL_PTR(base, s_tag_xhtml, "to_xhtml: null base");
  RA8_CHECK_NULL_PTR(out, s_tag_xhtml, "to_xhtml: null out");
  RA8_CHECK_NULL_PTR(out_len, s_tag_xhtml, "to_xhtml: null out_len");

  const book_header_t* hdr = book_header(base);
  if (chapter_idx >= hdr->chapter_count) {
    return k_ra8_err_invalid_arg;
  }

  const uint32_t root = book_chapters(base)[chapter_idx].root_node;
  size_t         pos  = 0U;
  if (!internal_walk_to_xhtml(base, root, hdr->node_count, out, cap, &pos)) {
    return k_ra8_err_invalid_size;
  }
  *out_len = pos;
  return k_ra8_ok;
}
