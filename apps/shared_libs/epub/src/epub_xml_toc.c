/**
 * @file epub_xml_toc.c
 * @brief Bounded NCX and EPUB 3 nav table-of-contents consumers.
 * @ingroup grp_ereader
 *
 * @details Replays an already-validated navigation document through the
 * caller-owned pull-reader workspace, proves the selected subtree fits the
 * fixed-capacity TOC table before a single entry is written, and only then
 * records each entry's nesting depth, title, and href. The container and OPF
 * passes that name the navigation document live in `epub_xml_shim.c`; the
 * helpers both sides share are declared in `epub_xml_consumer_internal.h`.
 *
 * [Ring 4 / EPUB] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#include <stddef.h>
#include <stdint.h>

#include "epub_xml_consumer_internal.h"
#include "epub_xml_shim_internal.h"
#include "epub_xml_toc_internal.h"
#include "ra8_attributes.h"
#include "xml.h"

RA8_INTERNAL static xml_span_t
internal_ancestor_name(const xml_reader_t* reader, uint16_t depth, uint16_t levels)
{
  uint16_t frame = 0U;
  return priv_epub_xml_ancestor_frame(depth, levels, &frame)
           ? priv_epub_xml_frame_name(reader, frame)
           : (xml_span_t){};
}

RA8_INTERNAL static uint8_t internal_ancestor_depth(const uint8_t*      source,
                                                    size_t              source_len,
                                                    const xml_reader_t* reader,
                                                    uint16_t            depth,
                                                    const char*         local)
{
  uint8_t count = 0U;
  for (uint16_t i = 0U; i < depth; ++i) {
    if (xml_span_local_equal(source, source_len, priv_epub_xml_frame_name(reader, i), local)) {
      ++count;
    }
  }
  return count;
}

RA8_INTERNAL static uint16_t internal_toc_reserve(const uint8_t*     source,
                                                  size_t             source_len,
                                                  const xml_event_t* event,
                                                  xml_reader_t*      reader,
                                                  epub_book_t*       book,
                                                  const char*        local)
{
  uint16_t marker = 0U;
  if (book->toc_count < (uint16_t)k_epub_max_toc) {
    const uint16_t slot = book->toc_count;
    book->toc_count++;
    book->toc[slot] = (epub_toc_entry_t){};
    book->toc[slot].depth =
      internal_ancestor_depth(source, source_len, reader, event->depth, local);
    marker = (uint16_t)(slot + 1U);
  }
  if (event->self_closing == 0U) {
    reader->workspace->frames[event->depth].consumer = marker;
  }
  return marker;
}

RA8_INTERNAL static uint16_t internal_toc_marker(const xml_reader_t* reader, uint16_t depth)
{
  uint16_t level = depth;
  while (level > 0U) {
    --level;
    const uint16_t marker = reader->workspace->frames[level].consumer;
    if (marker != 0U) {
      return marker;
    }
  }
  return 0U;
}

RA8_INTERNAL static ra8_err_t internal_toc_capacity(const uint8_t*        source,
                                                    size_t                length,
                                                    epub_xml_workspace_t* workspace,
                                                    const xml_event_t*    selected,
                                                    const char*           entry_local,
                                                    bool                  require_list)
{
  xml_reader_t reader  = {};
  ra8_err_t    err     = priv_epub_xml_reader(&reader, source, length, workspace);
  bool         active  = false;
  bool         enabled = !require_list;
  uint16_t     count   = 0U;
  while (err == k_ra8_ok) {
    xml_event_t event = {};
    err               = xml_reader_next(&reader, &event);
    // mcdc-deactivated: internal_toc_capacity pull-loop status gate; the public entry point validated this exact byte range with xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which xml never reads back -- so xml_reader_next() cannot fail here and only the end-of-document condition varies.
    if ((err != k_ra8_ok) || (event.kind == (uint8_t)k_xml_event_none)) {
      break;
    }
    if (!active && (event.kind == (uint8_t)k_xml_event_start) &&
        (event.markup.offset == selected->markup.offset)) {
      active = true;
    } else if (active && (event.kind == (uint8_t)k_xml_event_start) && require_list &&
               priv_epub_xml_direct_child(event.depth, selected->depth) &&
               xml_span_local_equal(source, length, event.name, "ol")) {
      enabled = true;
    } else if (active && enabled && (event.kind == (uint8_t)k_xml_event_start) &&
               xml_span_local_equal(source, length, event.name, entry_local)) {
      if (count >= (uint16_t)k_epub_max_toc) {
        return k_ra8_err_no_mem;
      }
      ++count;
    } else if (active && (event.kind == (uint8_t)k_xml_event_end) &&
               (event.depth == selected->depth)) {
      return k_ra8_ok;
    } else {
      /* Any other event is outside the counted entries and changes nothing. */
    }
  }
  return err;
}

RA8_INTERNAL static void internal_ncx_event(priv_ncx_ctx_t* ctx, const xml_event_t* event)
{
  if (!ctx->active && (event->kind == (uint8_t)k_xml_event_start) &&
      (event->markup.offset == ctx->selected.markup.offset)) {
    ctx->active = true;
  } else if (ctx->active && (event->kind == (uint8_t)k_xml_event_start) &&
             xml_span_local_equal(ctx->source, ctx->source_len, event->name, "navPoint")) {
    (void)
      internal_toc_reserve(ctx->source, ctx->source_len, event, ctx->reader, ctx->book, "navPoint");
  } else if (ctx->active && (event->kind == (uint8_t)k_xml_event_start) &&
             xml_span_local_equal(ctx->source, ctx->source_len, event->name, "content")) {
    const uint16_t    marker = internal_toc_marker(ctx->reader, event->depth);
    const priv_attr_t src    = priv_epub_xml_attr(ctx->source, ctx->source_len, event, "src");
    if ((marker != 0U) && src.present) {
      priv_epub_xml_copy(ctx->source,
                         ctx->source_len,
                         src.span,
                         ctx->book->toc[marker - 1U].href,
                         k_epub_max_path_len);
    }
  } else if ((event->kind == (uint8_t)k_xml_event_text) && (event->depth >= 3U) &&
             xml_span_local_equal(ctx->source,
                                  ctx->source_len,
                                  internal_ancestor_name(ctx->reader, event->depth, 1U),
                                  "text") &&
             xml_span_local_equal(ctx->source,
                                  ctx->source_len,
                                  internal_ancestor_name(ctx->reader, event->depth, 2U),
                                  "navLabel")) {
    uint16_t       nav_label = 0U;
    const bool     found     = priv_epub_xml_ancestor_frame(event->depth, 2U, &nav_label);
    const uint16_t marker    = found ? internal_toc_marker(ctx->reader, nav_label) : 0U;
    if ((marker != 0U) && (ctx->book->toc[marker - 1U].title[0] == '\0')) {
      priv_epub_xml_copy(ctx->source,
                         ctx->source_len,
                         event->markup,
                         ctx->book->toc[marker - 1U].title,
                         k_epub_meta_len);
    }
  } else if ((event->kind == (uint8_t)k_xml_event_end) && ctx->active &&
             (event->depth == ctx->selected.depth)) {
    ctx->active = false;
  } else {
    /* Any other event carries nothing the navMap walker consumes. */
  }
}

ra8_err_t priv_epub_xml_parse_ncx(const uint8_t* xml_bytes, size_t xml_len, epub_book_t* book)
{
  if ((xml_bytes == nullptr) || (book == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (xml_len == 0U) {
    return k_ra8_err_invalid_size;
  }
  ra8_err_t   err      = xml_validate(xml_bytes, xml_len, &book->xml_workspace.reader);
  xml_event_t selected = {};
  if (err == k_ra8_ok) {
    err = priv_epub_xml_find(xml_bytes,
                             xml_len,
                             &book->xml_workspace,
                             "navMap",
                             false,
                             0U,
                             0U,
                             &selected);
  }
  if (err == k_ra8_ok) {
    err =
      internal_toc_capacity(xml_bytes, xml_len, &book->xml_workspace, &selected, "navPoint", false);
  }
  if (err != k_ra8_ok) {
    return err;
  }
  book->toc_count     = 0U;
  xml_reader_t reader = {};
  err                 = priv_epub_xml_reader(&reader, xml_bytes, xml_len, &book->xml_workspace);
  priv_ncx_ctx_t ctx  = {.source     = xml_bytes,
                         .source_len = xml_len,
                         .book       = book,
                         .reader     = &reader,
                         .selected   = selected};
  while (err == k_ra8_ok) {
    xml_event_t event = {};
    err               = xml_reader_next(&reader, &event);
    // mcdc-deactivated: priv_epub_xml_parse_ncx pull-loop status gate; the public entry point validated this exact byte range with xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which xml never reads back -- so xml_reader_next() cannot fail here and only the end-of-document condition varies.
    if ((err != k_ra8_ok) || (event.kind == (uint8_t)k_xml_event_none)) {
      break;
    }
    internal_ncx_event(&ctx, &event);
  }
  return err;
}

RA8_INTERNAL static ra8_err_t internal_select_nav(const uint8_t*        source,
                                                  size_t                length,
                                                  epub_xml_workspace_t* workspace,
                                                  xml_event_t*          out)
{
  xml_reader_t reader   = {};
  ra8_err_t    err      = priv_epub_xml_reader(&reader, source, length, workspace);
  xml_event_t  fallback = {};
  while (err == k_ra8_ok) {
    xml_event_t event = {};
    err               = xml_reader_next(&reader, &event);
    // mcdc-deactivated: internal_select_nav pull-loop status gate; the public entry point validated this exact byte range with xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which xml never reads back -- so xml_reader_next() cannot fail here and only the end-of-document condition varies.
    if ((err != k_ra8_ok) || (event.kind == (uint8_t)k_xml_event_none)) {
      break;
    }
    if ((event.kind == (uint8_t)k_xml_event_start) && (event.depth > 0U) &&
        xml_span_local_equal(source, length, event.name, "nav")) {
      if (fallback.kind == (uint8_t)k_xml_event_none) {
        fallback = event;
      }
      if (priv_epub_xml_attr_contains(source,
                                      length,
                                      priv_epub_xml_attr(source, length, &event, "epub:type"),
                                      "toc")) {
        *out = event;
        return k_ra8_ok;
      }
    }
  }
  // mcdc-deactivated: internal_select_nav fallback gate; the public entry point validated this exact byte range with xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which xml never reads back -- so xml_reader_next() cannot fail here, so the loop can only exit at end-of-document with `err` equal to k_ra8_ok and the status condition is constant-true.
  if ((err == k_ra8_ok) && (fallback.kind != (uint8_t)k_xml_event_none)) {
    *out = fallback;
    return k_ra8_ok;
  }
  return (err == k_ra8_ok) ? k_ra8_err_validation_failed : err;
}

RA8_INTERNAL static ra8_err_t internal_nav_has_list(const uint8_t*        source,
                                                    size_t                length,
                                                    epub_xml_workspace_t* workspace,
                                                    const xml_event_t*    selected)
{
  xml_reader_t reader = {};
  ra8_err_t    err    = priv_epub_xml_reader(&reader, source, length, workspace);
  bool         active = false;
  while (err == k_ra8_ok) {
    xml_event_t event = {};
    err               = xml_reader_next(&reader, &event);
    // mcdc-deactivated: internal_nav_has_list pull-loop status gate; the public entry point validated this exact byte range with xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which xml never reads back -- so xml_reader_next() cannot fail here and only the end-of-document condition varies.
    if ((err != k_ra8_ok) || (event.kind == (uint8_t)k_xml_event_none)) {
      break;
    }
    if (!active && (event.kind == (uint8_t)k_xml_event_start) &&
        (event.markup.offset == selected->markup.offset)) {
      active = true;
    } else if (active && (event.kind == (uint8_t)k_xml_event_start) &&
               priv_epub_xml_direct_child(event.depth, selected->depth) &&
               xml_span_local_equal(source, length, event.name, "ol")) {
      return k_ra8_ok;
    } else if (active && (event.kind == (uint8_t)k_xml_event_end) &&
               (event.depth == selected->depth)) {
      return k_ra8_err_validation_failed;
    } else {
      /* Any other event neither opens the list nor ends the selected nav. */
    }
  }
  return (err == k_ra8_ok) ? k_ra8_err_validation_failed : err;
}

/**
 * @brief Dispatch one start-tag event inside an active nav-list subtree.
 * @details Detects the `<ol>` that begins the selected list, reserves a
 * TOC slot for each `<li>`, and -- for the first `<a>` or `<span>` label
 * inside a reserved item -- records its marker, copies an `<a>` href, and
 * resets the title so the following text event can fill it in.
 * @param[in,out] ctx Active nav-walk context (source, reader, book, flags).
 * @param[in] event Start-tag event being classified.
 * @return Nothing.
 * @pre @p ctx->active is true; the caller only reaches this dispatch once
 * the selected nav's subtree has been entered.
 * @pre @p event->kind is `k_xml_event_start`.
 * @post At most one of `ctx->saw_ol`, a reserved TOC slot's consumer
 * marker, or a TOC entry's href/title is updated.
 * @post A start tag matching no arm, or one whose enclosing item never
 * reserved a slot, leaves @p ctx and the book unchanged.
 * @note Extracted from ::internal_nav_event so both stay within the
 * 60-line function cap and the nesting-depth cap; the vectors that cover
 * it are unchanged.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_nav_event_start(priv_nav_ctx_t* ctx, const xml_event_t* event)
{
  if (priv_epub_xml_direct_child(event->depth, ctx->selected.depth) &&
      xml_span_local_equal(ctx->source, ctx->source_len, event->name, "ol")) {
    ctx->saw_ol = true;
    return;
  }
  if (ctx->saw_ol && xml_span_local_equal(ctx->source, ctx->source_len, event->name, "li")) {
    (void)internal_toc_reserve(ctx->source, ctx->source_len, event, ctx->reader, ctx->book, "li");
    return;
  }
  if (!(ctx->saw_ol && (xml_span_local_equal(ctx->source, ctx->source_len, event->name, "a") ||
                        xml_span_local_equal(ctx->source, ctx->source_len, event->name, "span")))) {
    return;
  }
  const uint16_t marker = internal_toc_marker(ctx->reader, event->depth);
  if ((marker == 0U) || (event->self_closing != 0U)) {
    return;
  }
  ctx->reader->workspace->frames[event->depth].consumer = marker;
  if (!xml_span_local_equal(ctx->source, ctx->source_len, event->name, "a")) {
    return;
  }
  const priv_attr_t href = priv_epub_xml_attr(ctx->source, ctx->source_len, event, "href");
  if (href.present) {
    priv_epub_xml_copy(ctx->source,
                       ctx->source_len,
                       href.span,
                       ctx->book->toc[marker - 1U].href,
                       k_epub_max_path_len);
  }
  ctx->book->toc[marker - 1U].title[0] = '\0';
}

RA8_INTERNAL static void internal_nav_event(priv_nav_ctx_t* ctx, const xml_event_t* event)
{
  if (!ctx->active && (event->kind == (uint8_t)k_xml_event_start) &&
      (event->markup.offset == ctx->selected.markup.offset)) {
    ctx->active = true;
    return;
  }
  if (!ctx->active) {
    return;
  }
  if (event->kind == (uint8_t)k_xml_event_start) {
    internal_nav_event_start(ctx, event);
    // mcdc-deactivated: internal_nav_event text-depth gate; this arm is reached only while ctx->active, which spans the subtree of a <nav> that internal_select_nav selected at depth >= 1, so every event inside it sits at depth > 0 and the depth condition is constant-true.
  } else if ((event->kind == (uint8_t)k_xml_event_text) && (event->depth > 0U)) {
    const xml_span_t parent = internal_ancestor_name(ctx->reader, event->depth, 1U);
    const uint16_t   marker = priv_epub_xml_ancestor_marker(ctx->reader, event->depth, 1U);
    if ((xml_span_local_equal(ctx->source, ctx->source_len, parent, "a") ||
         xml_span_local_equal(ctx->source, ctx->source_len, parent, "span")) &&
        (marker != 0U) && (ctx->book->toc[marker - 1U].title[0] == '\0')) {
      priv_epub_xml_copy(ctx->source,
                         ctx->source_len,
                         event->markup,
                         ctx->book->toc[marker - 1U].title,
                         k_epub_meta_len);
    }
  } else if ((event->kind == (uint8_t)k_xml_event_end) && (event->depth == ctx->selected.depth)) {
    ctx->active = false;
  } else {
    /* Any other event carries nothing the nav walker consumes. */
  }
}

ra8_err_t priv_epub_xml_parse_nav(const uint8_t* xml_bytes, size_t xml_len, epub_book_t* book)
{
  if ((xml_bytes == nullptr) || (book == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (xml_len == 0U) {
    return k_ra8_err_invalid_size;
  }
  ra8_err_t   err      = xml_validate(xml_bytes, xml_len, &book->xml_workspace.reader);
  xml_event_t selected = {};
  if (err == k_ra8_ok) {
    err = internal_select_nav(xml_bytes, xml_len, &book->xml_workspace, &selected);
  }
  if (err == k_ra8_ok) {
    err = internal_nav_has_list(xml_bytes, xml_len, &book->xml_workspace, &selected);
  }
  if (err == k_ra8_ok) {
    err = internal_toc_capacity(xml_bytes, xml_len, &book->xml_workspace, &selected, "li", true);
  }
  if (err != k_ra8_ok) {
    return err;
  }
  book->toc_count     = 0U;
  xml_reader_t reader = {};
  err                 = priv_epub_xml_reader(&reader, xml_bytes, xml_len, &book->xml_workspace);
  priv_nav_ctx_t ctx  = {.source     = xml_bytes,
                         .source_len = xml_len,
                         .book       = book,
                         .reader     = &reader,
                         .selected   = selected};
  while (err == k_ra8_ok) {
    xml_event_t event = {};
    err               = xml_reader_next(&reader, &event);
    // mcdc-deactivated: priv_epub_xml_parse_nav pull-loop status gate; the public entry point validated this exact byte range with xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which xml never reads back -- so xml_reader_next() cannot fail here and only the end-of-document condition varies.
    if ((err != k_ra8_ok) || (event.kind == (uint8_t)k_xml_event_none)) {
      break;
    }
    internal_nav_event(&ctx, &event);
  }
  // mcdc-deactivated: priv_epub_xml_parse_nav completion gate; this point is reached only after internal_nav_has_list accepted the same bytes with the identical direct-child <ol> predicate that sets ctx.saw_ol in internal_nav_event, so saw_ol is always true, and the public entry point validated this exact byte range with xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which xml never reads back -- so xml_reader_next() cannot fail here.
  if ((err != k_ra8_ok) || !ctx.saw_ol) {
    return (err == k_ra8_ok) ? k_ra8_err_validation_failed : err;
  }
  return k_ra8_ok;
}
