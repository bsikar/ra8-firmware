/**
 * @file ra8_rabook_xml_shim.c
 * @brief Strict streamed XHTML-to-RABOOK consumer.
 * @ingroup grp_ereader
 *
 * @details Validates immutable XHTML before streaming a selected body/root
 * subtree into the bounded RABOOK builder, restoring the complete logical
 * builder checkpoint on every failure.
 *
 * [Ring 4 / EPUB Compiler] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "ra8_rabook_xml_shim.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#ifdef UNIT_TEST
#include "ra8_rabook_xml_shim_test_internal.h"
#endif

/** @brief Selected subtree root coordinates. */
typedef struct {
  uint32_t offset; /**< Start-markup offset. */
  uint16_t depth;  /**< Absolute XML depth.  */
} priv_selection_t;

/**
 * @brief Intern one decoded immutable span through the existing string pool.
 * @details Measures and decodes the complete span before committing pool size.
 * @param[in,out] ctx Active bounded RABOOK builder.
 * @param[in] source Immutable XML source containing @p span.
 * @param[in] source_len Exact readable source extent.
 * @param[in] span Source-relative encoded text span.
 * @return String-pool offset, or ::k_book_nil on failure.
 * @retval k_book_nil Decode or string-pool capacity failed.
 * @pre @p ctx and @p source are non-NULL.
 * @pre @p span lies within @p source_len validated bytes.
 * @post Success returns a live interned string offset.
 * @post Failure sets `ctx->failed`; source bytes remain unchanged.
 * @note Duplicate decoded strings reuse their existing offset.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_intern_span(ra8_rabook_ctx_t* ctx,
                                                  const uint8_t*    source,
                                                  size_t            source_len,
                                                  xml_span_t        span)
{
  if (ctx->failed) {
    return (uint32_t)k_book_nil;
  }
  size_t decoded = 0U;
  if (xml_decoded_size(source, source_len, span, &decoded) != k_ra8_ok) {
    ctx->failed = true;
    return (uint32_t)k_book_nil;
  }
  if ((decoded + 1U) > (size_t)(ctx->buf.string_cap - ctx->string_size)) {
    ctx->failed = true;
    return (uint32_t)k_book_nil;
  }
  char* const scratch = &ctx->buf.string_pool[ctx->string_size];
  size_t      written = 0U;
  if (xml_decode(source,
                 source_len,
                 span,
                 scratch,
                 (size_t)(ctx->buf.string_cap - ctx->string_size),
                 &written) != k_ra8_ok) {
    ctx->failed = true;
    return (uint32_t)k_book_nil;
  }
  uint32_t scan = 0U;
  while (scan < ctx->string_size) {
    const char*  candidate = &ctx->buf.string_pool[scan];
    const size_t length    = strlen(candidate);
    if (length == written) {
      size_t equal_bytes = 0U;
      while ((equal_bytes < written) && (candidate[equal_bytes] == scratch[equal_bytes])) {
        equal_bytes++;
      }
      if (equal_bytes == written) {
        return scan;
      }
    }
    scan += (uint32_t)length + 1U;
  }
  const uint32_t result = ctx->string_size;
  ctx->string_size += (uint32_t)written + 1U;
  return result;
}

/**
 * @brief Collect and intern one event's attributes.
 * @details Preserves source order in caller workspace for element emission.
 * @param[in] source Immutable XML source.
 * @param[in] source_len Exact readable source extent.
 * @param[in] event Valid start event aliasing @p source.
 * @param[in,out] ctx Active bounded RABOOK builder.
 * @param[in,out] workspace Exclusive attribute scratch.
 * @return Repository error code.
 * @retval k_ra8_ok Every attribute was interned.
 * @retval k_ra8_err_no_mem Attribute or string capacity was exhausted.
 * @retval k_ra8_err_validation_failed Attribute traversal was inconsistent.
 * @pre The complete document was validated before this pass.
 * @pre @p workspace is not shared by another operation.
 * @post Success fills exactly `event->attribute_count` scratch entries.
 * @post Failure leaves source unchanged and marks builder failure when capacity failed.
 * @note Workspace bytes are scratch on every result.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_attributes(const uint8_t*              source,
                                                  size_t                      source_len,
                                                  const xml_event_t*          event,
                                                  ra8_rabook_ctx_t*           ctx,
                                                  ra8_rabook_xml_workspace_t* workspace)
{
  if (event->attribute_count > (uint16_t)k_ra8_rabook_xml_max_attributes) {
    ctx->failed = true;
    return k_ra8_err_no_mem;
  }
  xml_attr_cursor_t cursor = {};
  xml_attr_begin(event, &cursor);
  for (uint16_t i = 0U; i < event->attribute_count; ++i) {
    xml_attribute_t attribute = {};
    bool            has_value = false;
    const ra8_err_t attr_err =
      xml_attr_next(source, source_len, event, &cursor, &attribute, &has_value);
    if (attr_err != k_ra8_ok) {
      return k_ra8_err_validation_failed;
    }
    if (!has_value) {
      return k_ra8_err_validation_failed;
    }
    workspace->attributes[i].name_off =
      internal_intern_span(ctx, source, source_len, attribute.name);
    workspace->attributes[i].value_off =
      internal_intern_span(ctx, source, source_len, attribute.value);
    if (ctx->failed) {
      return k_ra8_err_no_mem;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Link a new node beneath its selected-subtree parent.
 * @details Appends after the remembered last child or establishes the first child.
 * @param[in,out] ctx Active bounded RABOOK builder.
 * @param[in,out] workspace Selected-subtree linkage state.
 * @param[in] parent_level Workspace level holding the parent.
 * @param[in] next_sibling Live node to append.
 * @return Repository error code.
 * @retval k_ra8_ok Node linked and last-child state advanced.
 * @retval k_ra8_err_no_mem Underlying bounded link operation failed.
 * @pre @p parent_level indexes live workspace entries.
 * @pre @p next_sibling and its parent are live builder nodes.
 * @post Success records @p next_sibling as the parent's last child.
 * @post Failure leaves the workspace last-child entry unchanged.
 * @note Builder mutation follows the underlying link operation's contract.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_link(ra8_rabook_ctx_t*           ctx,
                                            ra8_rabook_xml_workspace_t* workspace,
                                            uint16_t                    parent_level,
                                            uint32_t                    next_sibling)
{
  const uint32_t  previous_node = workspace->last_children[parent_level];
  const ra8_err_t err =
    (previous_node == (uint32_t)k_book_nil)
      ? ra8_rabook_link_child(ctx, workspace->element_nodes[parent_level], next_sibling)
      : ra8_rabook_link_sibling(ctx, previous_node, next_sibling);
  if (err == k_ra8_ok) {
    workspace->last_children[parent_level] = next_sibling;
  }
  return err;
}

/**
 * @brief Emit one element event and link it into preorder.
 * @details Interns the name/attributes, creates the node, then links its parent.
 * @param[in] source Immutable XML source.
 * @param[in] source_len Exact readable source extent.
 * @param[in] event Valid element-start event.
 * @param[in] selected_depth Absolute depth of the emitted subtree root.
 * @param[in,out] ctx Active bounded builder.
 * @param[in,out] workspace Exclusive emission scratch.
 * @param[out] out_node Newly created node on success.
 * @return Repository error code.
 * @retval k_ra8_ok Element emitted.
 * @retval k_ra8_err_no_mem Any bounded builder capacity was exhausted.
 * @retval k_ra8_err_validation_failed Attribute traversal failed.
 * @pre @p event lies within the selected validated subtree.
 * @pre Workspace level capacity covers `event->depth - selected_depth`.
 * @post Success stores the live node in @p out_node and workspace.
 * @post Failure never changes source bytes; caller restores the builder checkpoint.
 * @note Intermediate builder bytes are transactional at the public caller.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_element(const uint8_t*              source,
                                               size_t                      source_len,
                                               const xml_event_t*          event,
                                               uint16_t                    selected_depth,
                                               ra8_rabook_ctx_t*           ctx,
                                               ra8_rabook_xml_workspace_t* workspace,
                                               uint32_t*                   out_node)
{
  const ra8_err_t attr_err = internal_attributes(source, source_len, event, ctx, workspace);
  if (attr_err != k_ra8_ok) {
    return attr_err;
  }
  const uint32_t name = internal_intern_span(ctx, source, source_len, event->name);
  const uint32_t node =
    ra8_rabook_add_element(ctx, name, workspace->attributes, event->attribute_count);
  if (node == (uint32_t)k_book_nil) {
    return k_ra8_err_no_mem;
  }
  const uint16_t level = (uint16_t)(event->depth - selected_depth);
  if (level > 0U) {
    if (internal_link(ctx, workspace, (uint16_t)(level - 1U), node) != k_ra8_ok) {
      return k_ra8_err_no_mem;
    }
  }
  workspace->element_nodes[level] = node;
  workspace->last_children[level] = (uint32_t)k_book_nil;
  *out_node                       = node;
  return k_ra8_ok;
}

/**
 * @brief Emit one decoded text run beneath the current element.
 * @details Empty decoded runs are ignored; nonempty runs become linked text nodes.
 * @param[in] source Immutable XML source.
 * @param[in] source_len Exact readable source extent.
 * @param[in] event Valid text event.
 * @param[in] selected_depth Absolute depth of the emitted subtree root.
 * @param[in,out] ctx Active bounded builder.
 * @param[in,out] workspace Exclusive linkage scratch.
 * @return Repository error code.
 * @retval k_ra8_ok Text was empty or emitted and linked.
 * @retval k_ra8_err_no_mem String, node, or link capacity was exhausted.
 * @retval k_ra8_err_validation_failed Text decoding failed.
 * @pre @p event has a live parent inside the selected subtree.
 * @pre The complete source was validated before this pass.
 * @post Success preserves source order beneath the current parent.
 * @post Failure never changes source bytes; caller restores the builder checkpoint.
 * @note Workspace mutation is scratch until public-call success.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_text(const uint8_t*              source,
                                            size_t                      source_len,
                                            const xml_event_t*          event,
                                            uint16_t                    selected_depth,
                                            ra8_rabook_ctx_t*           ctx,
                                            ra8_rabook_xml_workspace_t* workspace)
{
  size_t decoded = 0U;
  if (xml_decoded_size(source, source_len, event->markup, &decoded) != k_ra8_ok) {
    return k_ra8_err_validation_failed;
  }
  if (decoded == 0U) {
    return k_ra8_ok;
  }
  const uint32_t text = internal_intern_span(ctx, source, source_len, event->markup);
  const uint32_t node = ra8_rabook_add_text(ctx, text);
  if (node == (uint32_t)k_book_nil) {
    return k_ra8_err_no_mem;
  }
  const uint16_t parent_level = (uint16_t)(event->depth - selected_depth - 1U);
  return (internal_link(ctx, workspace, parent_level, node) == k_ra8_ok) ? k_ra8_ok
                                                                         : k_ra8_err_no_mem;
}

/**
 * @brief Apply one reader event to the direct-body selection search.
 * @details Latches the document root on first sight, then watches direct
 *          children of the root for a `body` element, enforcing the bounded
 *          sibling-search limit.
 * @param[in] source Immutable validated XML source.
 * @param[in] length Exact readable source extent.
 * @param[in] event One decoded reader event.
 * @param[in,out] root_depth Root element depth; latched on first start event.
 * @param[in,out] saw_root Whether the root has been latched yet.
 * @param[in,out] direct_children Direct-child count seen since the root.
 * @param[out] out Selected start offset and absolute depth.
 * @param[out] out_done Set true once a `body` element has been selected.
 * @return Selection status.
 * @retval k_ra8_ok The event was applied; @p out_done reports finality.
 * @retval k_ra8_err_validation_failed The bounded sibling-search limit was exceeded.
 * @pre @p event came from the same reader driving this search.
 * @pre @p out_done was initialized false by the caller before the first call.
 * @post On a `body` match, @p out selects the body and `*out_done` is true.
 * @post Otherwise @p out holds the root fallback selection.
 * @note Not thread-safe; caller owns the reader state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_select_event(const uint8_t*     source,
                                                    size_t             length,
                                                    const xml_event_t* event,
                                                    uint16_t*          root_depth,
                                                    bool*              saw_root,
                                                    uint16_t*          direct_children,
                                                    priv_selection_t*  out,
                                                    bool*              out_done)
{
  if (!*saw_root) {
    if (event->kind == (uint8_t)k_xml_event_start) {
      *saw_root   = true;
      *root_depth = event->depth;
      *out        = (priv_selection_t){event->markup.offset, event->depth};
    }
    return k_ra8_ok;
  }
  if (event->kind != (uint8_t)k_xml_event_start) {
    return k_ra8_ok;
  }
  if (event->depth != (uint16_t)(*root_depth + 1U)) {
    return k_ra8_ok;
  }
  if (*direct_children >= (uint16_t)k_ra8_rabook_xml_body_siblings) {
    return k_ra8_err_validation_failed;
  }
  if (xml_span_equal(source, length, event->name, "body")) {
    *out      = (priv_selection_t){event->markup.offset, event->depth};
    *out_done = true;
    return k_ra8_ok;
  }
  ++*direct_children;
  return k_ra8_ok;
}

/**
 * @brief Find a direct body or fall back to the document root.
 * @details Rejects a root with more than the bounded direct-child search
 *          limit; delegates the per-event decision to
 *          ::internal_select_event.
 * @param[in] source Immutable validated XML source.
 * @param[in] length Exact readable source extent.
 * @param[in,out] xml Exclusive reader workspace.
 * @param[out] out Selected start offset and absolute depth.
 * @return Repository error code.
 * @retval k_ra8_ok Selection completed.
 * @retval k_ra8_err_validation_failed Reader or bounded search failed.
 * @pre @p source already passed ::xml_validate.
 * @pre @p xml and @p out are writable and non-overlapping.
 * @post Success selects the first direct body or the document root.
 * @post Failure leaves source bytes unchanged; @p out is unspecified.
 * @note Workspace bytes are scratch on return.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_select(const uint8_t* source, size_t length, xml_workspace_t* xml, priv_selection_t* out)
{
  xml_reader_t reader          = {};
  ra8_err_t    err             = xml_reader_init(&reader, source, length, xml);
  uint16_t     root_depth      = 0U;
  uint16_t     direct_children = 0U;
  bool         saw_root        = false;
  bool         done            = false;
  while ((err == k_ra8_ok) && !done) {
    xml_event_t event = {};
    err               = xml_reader_next(&reader, &event);
    if (err == k_ra8_ok) {
      if (event.kind == (uint8_t)k_xml_event_none) {
        break;
      }
      err = internal_select_event(source,
                                  length,
                                  &event,
                                  &root_depth,
                                  &saw_root,
                                  &direct_children,
                                  out,
                                  &done);
    }
  }
  return err;
}

/**
 * @brief Handle one start event for the in-progress subtree emission.
 * @details Emits the element, records the root node when the started
 * element is the subtree root, and closes a self-closing root immediately.
 * @param[in] source Immutable validated XML source.
 * @param[in] length Exact readable source extent.
 * @param[in] selection Selected subtree coordinates.
 * @param[in] event One decoded start-kind reader event.
 * @param[in,out] ctx Active bounded builder.
 * @param[in,out] workspace Exclusive reader/emission scratch.
 * @param[in,out] active Whether the selected subtree is currently open.
 * @param[out] out_root Emitted subtree root node; set when the root starts.
 * @return Repository error code.
 * @retval k_ra8_ok The element was emitted.
 * @retval k_ra8_err_no_mem Bounded builder capacity failed.
 * @pre @p event->kind is ::k_xml_event_start.
 * @pre @p selection came from ::internal_select on this same source.
 * @post Success may advance @p out_root and close a self-closing root.
 * @post @p out_root is written whenever the event starts the selected root, even
 *       when the element emission itself then fails.
 * @note Not thread-safe; caller owns the reader state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_emit_start(const uint8_t*              source,
                                                  size_t                      length,
                                                  const priv_selection_t*     selection,
                                                  const xml_event_t*          event,
                                                  ra8_rabook_ctx_t*           ctx,
                                                  ra8_rabook_xml_workspace_t* workspace,
                                                  bool*                       active,
                                                  uint32_t*                   out_root)
{
  uint32_t        node = (uint32_t)k_book_nil;
  const ra8_err_t err =
    internal_element(source, length, event, selection->depth, ctx, workspace, &node);
  if (event->markup.offset == selection->offset) {
    *out_root = node;
  }
  if (err != k_ra8_ok) {
    return err;
  }
  if (event->self_closing == 0U) {
    return err;
  }
  if (event->markup.offset != selection->offset) {
    return err;
  }
  *active = false;
  return err;
}

/**
 * @brief Apply one reader event to the in-progress subtree emission.
 * @details While inactive, skips events until the selected subtree's start
 *          event arrives. While active, delegates a start event to
 *          ::internal_emit_start, emits text events, and closes the active
 *          span on the matching end event.
 * @param[in] source Immutable validated XML source.
 * @param[in] length Exact readable source extent.
 * @param[in] selection Selected subtree coordinates.
 * @param[in] event One decoded reader event.
 * @param[in,out] ctx Active bounded builder.
 * @param[in,out] workspace Exclusive reader/emission scratch.
 * @param[in,out] active Whether the selected subtree is currently open.
 * @param[out] out_root Emitted subtree root node; set when the root starts.
 * @return Repository error code.
 * @retval k_ra8_ok The event was applied.
 * @retval k_ra8_err_no_mem Bounded builder capacity failed.
 * @pre @p selection came from ::internal_select on this source.
 * @pre @p event came from the same reader driving this emission.
 * @post Success may advance @p out_root, mutate the builder, or update
 *       @p active per the event's kind.
 * @post @p active becomes false on the end event whose depth matches the selection.
 * @note Not thread-safe; caller owns the reader state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_emit_event(const uint8_t*              source,
                                                  size_t                      length,
                                                  const priv_selection_t*     selection,
                                                  const xml_event_t*          event,
                                                  ra8_rabook_ctx_t*           ctx,
                                                  ra8_rabook_xml_workspace_t* workspace,
                                                  bool*                       active,
                                                  uint32_t*                   out_root)
{
  if (!*active) {
    if (event->kind != (uint8_t)k_xml_event_start) {
      return k_ra8_ok;
    }
    if (event->markup.offset != selection->offset) {
      return k_ra8_ok;
    }
    *active = true;
  }
  if (event->kind == (uint8_t)k_xml_event_start) {
    return internal_emit_start(source, length, selection, event, ctx, workspace, active, out_root);
  }
  if (event->kind == (uint8_t)k_xml_event_text) {
    return internal_text(source, length, event, selection->depth, ctx, workspace);
  }
  if (event->kind == (uint8_t)k_xml_event_end) {
    if (event->depth == selection->depth) {
      *active = false;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Stream the selected subtree into the builder.
 * @details Emits start and text events until the selected element closes;
 *          delegates the per-event decision to ::internal_emit_event.
 * @param[in] source Immutable validated XML source.
 * @param[in] length Exact readable source extent.
 * @param[in] selection Selected subtree coordinates.
 * @param[in,out] ctx Active bounded builder.
 * @param[in,out] workspace Exclusive reader/emission scratch.
 * @param[out] out_root Emitted subtree root node.
 * @return Repository error code.
 * @retval k_ra8_ok Selected subtree consumed.
 * @retval k_ra8_err_no_mem Bounded builder capacity failed.
 * @retval k_ra8_err_validation_failed Repeated reader traversal failed.
 * @pre @p selection came from ::internal_select on this source.
 * @pre The complete source was validated and workspace is exclusive.
 * @post Success returns the emitted root and canonical preorder links.
 * @post Failure leaves source unchanged; caller restores the builder checkpoint.
 * @note Workspace contents are scratch on every result.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_emit(const uint8_t*              source,
                                            size_t                      length,
                                            const priv_selection_t*     selection,
                                            ra8_rabook_ctx_t*           ctx,
                                            ra8_rabook_xml_workspace_t* workspace,
                                            uint32_t*                   out_root)
{
  xml_reader_t reader = {};
  ra8_err_t    err    = xml_reader_init(&reader, source, length, &workspace->xml);
  bool         active = false;
  while (err == k_ra8_ok) {
    xml_event_t event = {};
    err               = xml_reader_next(&reader, &event);
    if (err == k_ra8_ok) {
      if (event.kind == (uint8_t)k_xml_event_none) {
        break;
      }
      err =
        internal_emit_event(source, length, selection, &event, ctx, workspace, &active, out_root);
    }
  }
  return err;
}

#ifdef UNIT_TEST
RA8_TEST_HELPER
ra8_err_t priv_ra8_rabook_xml_select_unvalidated_test(const uint8_t*              source,
                                                      size_t                      source_len,
                                                      ra8_rabook_xml_workspace_t* workspace)
{
  priv_selection_t selection = {};
  return internal_select(source, source_len, &workspace->xml, &selection);
}

RA8_TEST_HELPER ra8_err_t
priv_ra8_rabook_xml_emit_unvalidated_test(const uint8_t*              source,
                                          size_t                      source_len,
                                          ra8_rabook_ctx_t*           ctx,
                                          ra8_rabook_xml_workspace_t* workspace)
{
  const priv_selection_t selection = {.offset = 0U, .depth = 0U};
  uint32_t               root      = (uint32_t)k_book_nil;
  return internal_emit(source, source_len, &selection, ctx, workspace, &root);
}
#endif

/**
 * @brief Validate the public chapter-parse entry point's arguments.
 * @details Rejects any null pointer argument and a zero-length source before
 *          any parsing work begins.
 * @param[in] xhtml_bytes Candidate XHTML source bytes.
 * @param[in] xhtml_len Candidate source length in bytes.
 * @param[in] ctx Candidate active builder.
 * @param[in] chapter_href Candidate spine href string.
 * @param[in] chapter_title Candidate TOC title string.
 * @param[in] workspace Candidate reader/emission scratch.
 * @return Argument validation status.
 * @retval k_ra8_ok Every argument is present and @p xhtml_len is nonzero.
 * @retval k_ra8_err_null_ptr A required pointer argument is NULL.
 * @retval k_ra8_err_invalid_size @p xhtml_len is zero.
 * @pre None; every argument is treated as untrusted.
 * @pre @p ctx and @p workspace need not be initialized; only addresses are read.
 * @post No argument or builder state is modified.
 * @post Success proves every later dereference in the parse path has a non-NULL base.
 * @note Not thread-safe with respect to concurrent callers sharing @p ctx.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_parse_chapter_validate_args(const uint8_t*                    xhtml_bytes,
                                     size_t                            xhtml_len,
                                     const ra8_rabook_ctx_t*           ctx,
                                     const char*                       chapter_href,
                                     const char*                       chapter_title,
                                     const ra8_rabook_xml_workspace_t* workspace)
{
  if (xhtml_bytes == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (ctx == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (chapter_href == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (chapter_title == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (workspace == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (xhtml_len == 0U) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_rabook_xml_parse_chapter(const uint8_t*              xhtml_bytes,
                                       size_t                      xhtml_len,
                                       ra8_rabook_ctx_t*           ctx,
                                       const char*                 chapter_href,
                                       const char*                 chapter_title,
                                       ra8_rabook_xml_workspace_t* workspace)
{
  const ra8_err_t arg_err = internal_parse_chapter_validate_args(xhtml_bytes,
                                                                 xhtml_len,
                                                                 ctx,
                                                                 chapter_href,
                                                                 chapter_title,
                                                                 workspace);
  if (arg_err != k_ra8_ok) {
    return arg_err;
  }
  const ra8_rabook_ctx_t checkpoint = *ctx;
  ra8_err_t              err        = xml_validate(xhtml_bytes, xhtml_len, &workspace->xml);
  priv_selection_t       selection  = {};
  if (err == k_ra8_ok) {
    err = internal_select(xhtml_bytes, xhtml_len, &workspace->xml, &selection);
  }
  uint32_t root = (uint32_t)k_book_nil;
  if (err == k_ra8_ok) {
    err = internal_emit(xhtml_bytes, xhtml_len, &selection, ctx, workspace, &root);
  }
  if (err != k_ra8_ok) {
    *ctx = checkpoint;
    return (err == k_ra8_err_validation_failed) ? err : k_ra8_err_no_mem;
  }
  if (ctx->failed) {
    *ctx = checkpoint;
    return k_ra8_err_no_mem;
  }
  if (root == (uint32_t)k_book_nil) {
    *ctx = checkpoint;
    return k_ra8_err_no_mem;
  }
  const uint32_t title   = ra8_rabook_intern(ctx, chapter_title);
  const uint32_t href    = ra8_rabook_intern(ctx, chapter_href);
  const uint32_t chapter = ra8_rabook_add_chapter(ctx, title, href, root);
  if (chapter == (uint32_t)k_book_nil) {
    *ctx = checkpoint;
    return k_ra8_err_no_mem;
  }
  return k_ra8_ok;
}
