/**
 * @file epub_xml_shim.c
 * @brief Strict bounded container/OPF/NCX/nav XML consumers.
 * @ingroup grp_ereader
 *
 * @details Replays validated immutable XML through a caller-owned pull-reader
 * workspace and commits EPUB metadata, spine, and TOC fields only after every
 * syntax, shape, and fixed-capacity preflight succeeds.
 *
 * [Ring 4 / EPUB] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "epub_xml_consumer_internal.h"
#include "epub_xml_opf_internal.h"
#include "epub_xml_shim_internal.h"
#include "ra8_attributes.h"
#include "xml.h"

/** @brief Consumer markers stored in live reader frames. */
typedef enum : uint16_t {
  k_priv_mark_none             = 0U, /**< Frame carries no metadata role.       */
  k_priv_mark_title            = 1U, /**< Frame is a Dublin Core title.         */
  k_priv_mark_creator          = 2U, /**< Frame is a Dublin Core creator.       */
  k_priv_mark_language         = 3U, /**< Frame is a Dublin Core language.      */
  k_priv_mark_identifier       = 4U, /**< Frame is a fallback identifier.       */
  k_priv_mark_identifier_match = 5U, /**< Frame matches the package identifier. */
} priv_marker_t;

void priv_epub_xml_copy(const uint8_t* source,
                        size_t         source_len,
                        xml_span_t     span,
                        char*          destination,
                        size_t         capacity)
{
  size_t ignored = 0U;
  (void)xml_decode_prefix(source, source_len, span, destination, capacity, &ignored);
}

priv_attr_t priv_epub_xml_attr(const uint8_t*     source,
                               size_t             source_len,
                               const xml_event_t* event,
                               const char*        name)
{
  xml_attr_cursor_t cursor = {};
  xml_attr_begin(event, &cursor);
  for (uint16_t i = 0U; i < event->attribute_count; ++i) {
    xml_attribute_t attribute = {};
    bool            present   = false;
    // mcdc-deactivated: priv_epub_xml_attr attribute re-walk gate; internal_attributes fixed event->attribute_count by successfully parsing exactly that many attributes over these immutable bytes, so the bounded re-parse cannot fail and xml_attr_next() only clears `present` once `emitted` reaches that same count, which this loop never exceeds -- both conditions are constant-false.
    if ((xml_attr_next(source, source_len, event, &cursor, &attribute, &present) != k_ra8_ok) ||
        !present) {
      break;
    }
    if (xml_span_equal(source, source_len, attribute.name, name)) {
      return (priv_attr_t){attribute.value, true};
    }
  }
  return (priv_attr_t){};
}

bool priv_epub_xml_attr_contains(const uint8_t* source,
                                 size_t         source_len,
                                 priv_attr_t    attribute,
                                 const char*    needle)
{
  if (!attribute.present) {
    return false;
  }
  char   value[k_epub_max_path_len] = {};
  size_t length                     = 0U;
  if (xml_decode(source, source_len, attribute.span, value, sizeof(value), &length) != k_ra8_ok) {
    return false;
  }
  return strstr(value, needle) != nullptr;
}

xml_span_t priv_epub_xml_frame_name(const xml_reader_t* reader, uint16_t frame)
{
  const xml_frame_t* value = &reader->workspace->frames[frame];
  return (xml_span_t){value->name_offset, value->name_length};
}

bool priv_epub_xml_direct_child(uint16_t child, uint16_t parent)
{
  const uint16_t parent_depth = parent;
  if (parent_depth == UINT16_MAX) {
    return false;
  }
  return child == (uint16_t)(parent_depth + 1U);
}

bool priv_epub_xml_ancestor_frame(uint16_t depth, uint16_t levels, uint16_t* out_frame)
{
  uint16_t frame     = depth;
  uint16_t remaining = levels;
  while (remaining > 0U) {
    if (frame == 0U) {
      return false;
    }
    --frame;
    --remaining;
  }
  *out_frame = frame;
  return true;
}

uint16_t priv_epub_xml_ancestor_marker(const xml_reader_t* reader, uint16_t depth, uint16_t levels)
{
  uint16_t frame = 0U;
  return priv_epub_xml_ancestor_frame(depth, levels, &frame)
           ? reader->workspace->frames[frame].consumer
           : 0U;
}

ra8_err_t priv_epub_xml_reader(xml_reader_t*         reader,
                               const uint8_t*        source,
                               size_t                length,
                               epub_xml_workspace_t* workspace)
{
  return xml_reader_init(reader, source, length, &workspace->reader);
}

ra8_err_t priv_epub_xml_find(const uint8_t*        source,
                             size_t                length,
                             epub_xml_workspace_t* workspace,
                             const char*           local,
                             bool                  use_parent,
                             uint32_t              parent_offset,
                             uint16_t              parent_depth,
                             xml_event_t*          out)
{
  xml_reader_t reader        = {};
  ra8_err_t    err           = priv_epub_xml_reader(&reader, source, length, workspace);
  bool         parent_active = false;
  while (err == k_ra8_ok) {
    xml_event_t event = {};
    err               = xml_reader_next(&reader, &event);
    if ((err != k_ra8_ok) || (event.kind == (uint8_t)k_xml_event_none)) {
      break;
    }
    if (use_parent && (event.kind == (uint8_t)k_xml_event_start) && (event.depth == parent_depth) &&
        (event.markup.offset == parent_offset)) {
      parent_active = true;
      continue;
    }
    if ((event.kind == (uint8_t)k_xml_event_start) &&
        xml_span_local_equal(source, length, event.name, local)) {
      if (!use_parent || (parent_active && priv_epub_xml_direct_child(event.depth, parent_depth))) {
        *out = event;
        return k_ra8_ok;
      }
    } else if (use_parent && parent_active && (event.kind == (uint8_t)k_xml_event_end) &&
               (event.depth == parent_depth)) {
      return k_ra8_err_validation_failed;
    } else {
      /* This event does not affect the requested element search. */
    }
  }
  return (err == k_ra8_ok) ? k_ra8_err_validation_failed : err;
}

ra8_err_t priv_epub_xml_parse_container(const uint8_t*           xml_bytes,
                                        size_t                   xml_len,
                                        epub_container_result_t* out,
                                        epub_xml_workspace_t*    workspace)
{
  if ((xml_bytes == nullptr) || (out == nullptr) || (workspace == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (xml_len == 0U) {
    return k_ra8_err_invalid_size;
  }
  ra8_err_t   err       = xml_validate(xml_bytes, xml_len, &workspace->reader);
  xml_event_t rootfiles = {};
  if (err == k_ra8_ok) {
    err = priv_epub_xml_find(xml_bytes, xml_len, workspace, "rootfiles", false, 0U, 0U, &rootfiles);
  }
  xml_event_t rootfile = {};
  if (err == k_ra8_ok) {
    err = priv_epub_xml_find(xml_bytes,
                             xml_len,
                             workspace,
                             "rootfile",
                             true,
                             rootfiles.markup.offset,
                             rootfiles.depth,
                             &rootfile);
  } else if (err == k_ra8_err_validation_failed) {
    err = priv_epub_xml_find(xml_bytes, xml_len, workspace, "rootfile", false, 0U, 0U, &rootfile);
  } else {
    /* Preserve the parser error for the common return below. */
  }
  if (err != k_ra8_ok) {
    return err;
  }
  const priv_attr_t path = priv_epub_xml_attr(xml_bytes, xml_len, &rootfile, "full-path");
  if (!path.present || (path.span.length == 0U)) {
    return k_ra8_err_validation_failed;
  }
  out->opf_path[0] = '\0';
  priv_epub_xml_copy(xml_bytes, xml_len, path.span, out->opf_path, sizeof(out->opf_path));
  return k_ra8_ok;
}

RA8_INTERNAL static bool
internal_font_type(const uint8_t* source, size_t source_len, priv_attr_t media)
{
  static const char* const types[] = {"application/font-sfnt",
                                      "application/vnd.ms-opentype",
                                      "font/ttf",
                                      "font/otf",
                                      "application/x-font-ttf"};
  for (size_t i = 0U; i < (sizeof(types) / sizeof(types[0])); ++i) {
    if (media.present && xml_span_equal(source, source_len, media.span, types[i])) {
      return true;
    }
  }
  return false;
}

RA8_INTERNAL static void internal_manifest_item(const uint8_t*     source,
                                                size_t             source_len,
                                                const xml_event_t* event,
                                                epub_book_t*       book)
{
  const priv_attr_t id         = priv_epub_xml_attr(source, source_len, event, "id");
  const priv_attr_t href       = priv_epub_xml_attr(source, source_len, event, "href");
  const priv_attr_t media      = priv_epub_xml_attr(source, source_len, event, "media-type");
  const priv_attr_t properties = priv_epub_xml_attr(source, source_len, event, "properties");
  if (book->manifest_count < (uint16_t)k_epub_max_manifest) {
    epub_manifest_item_t* item = &book->manifest[book->manifest_count];
    ++book->manifest_count;
    if (id.present) {
      priv_epub_xml_copy(source, source_len, id.span, item->id, sizeof(item->id));
    }
    if (href.present) {
      priv_epub_xml_copy(source, source_len, href.span, item->href, sizeof(item->href));
    }
    if (media.present) {
      priv_epub_xml_copy(source,
                         source_len,
                         media.span,
                         item->media_type,
                         sizeof(item->media_type));
    }
  }
  if (href.present && internal_font_type(source, source_len, media) &&
      (book->embedded_font_count < (uint16_t)k_epub_max_fonts)) {
    priv_epub_xml_copy(source,
                       source_len,
                       href.span,
                       book->embedded_font_paths[book->embedded_font_count++],
                       k_epub_max_path_len);
  }
  if (href.present && priv_epub_xml_attr_contains(source, source_len, properties, "cover-image")) {
    priv_epub_xml_copy(source, source_len, href.span, book->cover_path, sizeof(book->cover_path));
  }
  if (href.present && priv_epub_xml_attr_contains(source, source_len, properties, "nav")) {
    priv_epub_xml_copy(source, source_len, href.span, book->toc_path, sizeof(book->toc_path));
    book->toc_kind = (uint8_t)k_epub_toc_nav;
  }
}

RA8_INTERNAL static void internal_metadata_text(const uint8_t*      source,
                                                size_t              source_len,
                                                const xml_event_t*  event,
                                                const xml_reader_t* reader,
                                                epub_book_t*        book)
{
  if (event->depth == 0U) {
    return;
  }
  const uint16_t marker = priv_epub_xml_ancestor_marker(reader, event->depth, 1U);
  if (marker == (uint16_t)k_priv_mark_title) {
    priv_epub_xml_copy(source, source_len, event->markup, book->title, sizeof(book->title));
  } else if (marker == (uint16_t)k_priv_mark_creator) {
    priv_epub_xml_copy(source, source_len, event->markup, book->author, sizeof(book->author));
  } else if (marker == (uint16_t)k_priv_mark_language) {
    priv_epub_xml_copy(source, source_len, event->markup, book->language, sizeof(book->language));
  } else if ((marker == (uint16_t)k_priv_mark_identifier_match) ||
             ((marker == (uint16_t)k_priv_mark_identifier) && (book->identifier[0] == '\0'))) {
    priv_epub_xml_copy(source,
                       source_len,
                       event->markup,
                       book->identifier,
                       sizeof(book->identifier));
  } else {
    /* Text outside recognized metadata elements is ignored. */
  }
}

RA8_INTERNAL static void internal_mark_metadata(const uint8_t*     source,
                                                size_t             source_len,
                                                const xml_event_t* event,
                                                xml_reader_t*      reader,
                                                xml_span_t         unique_id)
{
  uint16_t marker = (uint16_t)k_priv_mark_none;
  if (xml_span_local_equal(source, source_len, event->name, "title")) {
    marker = (uint16_t)k_priv_mark_title;
  } else if (xml_span_local_equal(source, source_len, event->name, "creator")) {
    marker = (uint16_t)k_priv_mark_creator;
  } else if (xml_span_local_equal(source, source_len, event->name, "language")) {
    marker = (uint16_t)k_priv_mark_language;
  } else if (xml_span_local_equal(source, source_len, event->name, "identifier")) {
    marker               = (uint16_t)k_priv_mark_identifier;
    const priv_attr_t id = priv_epub_xml_attr(source, source_len, event, "id");
    if (id.present && (unique_id.length > 0U) &&
        xml_decoded_equal(source, source_len, id.span, unique_id)) {
      marker = (uint16_t)k_priv_mark_identifier_match;
    }
  } else {
    /* Unrecognized metadata elements retain the no-marker value. */
  }
  reader->workspace->frames[event->depth].consumer = marker;
}

RA8_INTERNAL static ra8_err_t
internal_opf_status(ra8_err_t err, uint16_t manifest_depth, uint16_t spine_depth)
{
  // mcdc-deactivated: internal_opf_status package-shape gate; it runs only after internal_opf_shape accepted the same bytes using the identical depth-1 <manifest>/<spine> predicates that assign manifest_depth and spine_depth here, so neither can still be UINT16_MAX, and the public entry point validated this exact byte range with xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which xml never reads back -- so xml_reader_next() cannot fail here -- all three conditions are constant.
  return ((err == k_ra8_ok) && ((manifest_depth == UINT16_MAX) || (spine_depth == UINT16_MAX)))
           ? k_ra8_err_validation_failed
           : err;
}

/**
 * @brief Record one direct metadata child of the OPF metadata element.
 * @details Marks the Dublin Core element, then captures the EPUB 2 legacy
 * cover pointer carried by `<meta name="cover" content="..."/>`.
 * @param[in] source Complete OPF byte range being parsed.
 * @param[in] length Readable bytes at @p source.
 * @param[in] event Start event for the metadata child.
 * @param[in,out] reader Reader whose frame stack names the enclosing element.
 * @param[in] unique_id Span naming the package unique-identifier attribute.
 * @param[in,out] book Book whose XML workspace receives the recorded values.
 * @return Nothing.
 * @pre @p event is a start event whose parent is the metadata element.
 * @pre @p book owns a live XML workspace.
 * @post Recognised Dublin Core elements are marked in the workspace.
 * @post A legacy cover meta records its content span; nothing else is touched.
 * @note Extracted from ::internal_opf_first so both stay within the
 * 60-line function cap; the vectors that cover it are unchanged.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_opf_metadata_child(const uint8_t*     source,
                                                     size_t             length,
                                                     const xml_event_t* event,
                                                     xml_reader_t*      reader,
                                                     xml_span_t         unique_id,
                                                     epub_book_t*       book)
{
  internal_mark_metadata(source, length, event, reader, unique_id);
  if (!xml_span_local_equal(source, length, event->name, "meta")) {
    return;
  }
  const priv_attr_t name    = priv_epub_xml_attr(source, length, event, "name");
  const priv_attr_t content = priv_epub_xml_attr(source, length, event, "content");
  if (name.present && content.present && xml_span_equal(source, length, name.span, "cover")) {
    book->xml_workspace.legacy_cover_id = content.span;
  }
}

/**
 * @brief Classify one OPF start-tag event during the first parse pass.
 * @details Records the package unique-identifier attribute at depth 0,
 * remembers where `<metadata>`, `<manifest>`, and `<spine>` begin, and
 * forwards a direct metadata or manifest-item child to its own handler.
 * @param[in] source Complete OPF byte range being parsed.
 * @param[in] length Readable bytes at @p source.
 * @param[in] event Start-tag event being classified.
 * @param[in,out] reader Reader whose frame stack names enclosing elements.
 * @param[in,out] book Book whose XML workspace receives recorded values.
 * @param[in,out] metadata_depth Depth of `<metadata>`, or UINT16_MAX.
 * @param[in,out] manifest_depth Depth of `<manifest>`, or UINT16_MAX.
 * @param[in,out] spine_depth Depth of `<spine>`, or UINT16_MAX.
 * @param[in,out] unique_id Span naming the package unique-identifier.
 * @param[out] out_spine_toc Spine `toc` attribute span, set once seen.
 * @return Nothing.
 * @pre @p event->kind is `k_xml_event_start`.
 * @pre @p book owns a live XML workspace.
 * @post At most one tracked depth, the unique-id span, the spine toc
 * span, or a forwarded child handler's effects is updated.
 * @post A start tag matching no arm leaves every tracked depth, the
 * unique-id span, and @p book byte-for-byte as they were.
 * @note Extracted from ::internal_opf_first so both stay within the
 * 60-line function cap; the vectors that cover it are unchanged.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_opf_first_event(const uint8_t*     source,
                                                  size_t             length,
                                                  const xml_event_t* event,
                                                  xml_reader_t*      reader,
                                                  epub_book_t*       book,
                                                  uint16_t*          metadata_depth,
                                                  uint16_t*          manifest_depth,
                                                  uint16_t*          spine_depth,
                                                  xml_span_t*        unique_id,
                                                  xml_span_t*        out_spine_toc)
{
  if (event->depth == 0U) {
    const priv_attr_t uid = priv_epub_xml_attr(source, length, event, "unique-identifier");
    *unique_id            = uid.present ? uid.span : (xml_span_t){};
  } else if ((event->depth == 1U) &&
             xml_span_local_equal(source, length, event->name, "metadata")) {
    *metadata_depth = event->depth;
  } else if ((event->depth == 1U) &&
             xml_span_local_equal(source, length, event->name, "manifest")) {
    *manifest_depth = event->depth;
  } else if ((event->depth == 1U) && xml_span_local_equal(source, length, event->name, "spine")) {
    *spine_depth          = event->depth;
    const priv_attr_t toc = priv_epub_xml_attr(source, length, event, "toc");
    *out_spine_toc        = toc.present ? toc.span : (xml_span_t){};
  } else if (priv_epub_xml_direct_child(event->depth, *metadata_depth) &&
             xml_span_local_equal(source,
                                  length,
                                  priv_epub_xml_frame_name(reader, *metadata_depth),
                                  "metadata")) {
    internal_opf_metadata_child(source, length, event, reader, *unique_id, book);
  } else if (priv_epub_xml_direct_child(event->depth, *manifest_depth) &&
             xml_span_local_equal(source,
                                  length,
                                  priv_epub_xml_frame_name(reader, *manifest_depth),
                                  "manifest") &&
             xml_span_local_equal(source, length, event->name, "item")) {
    internal_manifest_item(source, length, event, book);
  } else {
    /* Elements outside the OPF sections consumed here are ignored. */
  }
}

RA8_INTERNAL static ra8_err_t internal_opf_first(const uint8_t* source,
                                                 size_t         length,
                                                 epub_book_t*   book,
                                                 xml_span_t*    out_spine_toc)
{
  xml_reader_t reader         = {};
  ra8_err_t    err            = priv_epub_xml_reader(&reader, source, length, &book->xml_workspace);
  uint16_t     metadata_depth = UINT16_MAX;
  uint16_t     manifest_depth = UINT16_MAX;
  uint16_t     spine_depth    = UINT16_MAX;
  xml_span_t   unique_id      = {};
  while (err == k_ra8_ok) {
    xml_event_t event = {};
    err               = xml_reader_next(&reader, &event);
    // mcdc-deactivated: internal_opf_first pull-loop status gate; the public entry point validated this exact byte range with xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which xml never reads back -- so xml_reader_next() cannot fail here and only the end-of-document condition varies.
    if ((err != k_ra8_ok) || (event.kind == (uint8_t)k_xml_event_none)) {
      break;
    }
    if (event.kind == (uint8_t)k_xml_event_text) {
      internal_metadata_text(source, length, &event, &reader, book);
      continue;
    }
    if (event.kind != (uint8_t)k_xml_event_start) {
      continue;
    }
    internal_opf_first_event(source,
                             length,
                             &event,
                             &reader,
                             book,
                             &metadata_depth,
                             &manifest_depth,
                             &spine_depth,
                             &unique_id,
                             out_spine_toc);
    (void)spine_depth;
  }
  return internal_opf_status(err, manifest_depth, spine_depth);
}

RA8_INTERNAL static ra8_err_t
internal_collect_spine(const uint8_t* source, size_t length, epub_book_t* book)
{
  xml_reader_t reader      = {};
  ra8_err_t    err         = priv_epub_xml_reader(&reader, source, length, &book->xml_workspace);
  uint16_t     spine_depth = UINT16_MAX;
  book->xml_workspace.reference_count = 0U;
  while (err == k_ra8_ok) {
    xml_event_t event = {};
    err               = xml_reader_next(&reader, &event);
    // mcdc-deactivated: internal_collect_spine pull-loop status gate; the public entry point validated this exact byte range with xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which xml never reads back -- so xml_reader_next() cannot fail here and only the end-of-document condition varies.
    if ((err != k_ra8_ok) || (event.kind == (uint8_t)k_xml_event_none)) {
      break;
    }
    if (event.kind != (uint8_t)k_xml_event_start) {
      continue;
    }
    if ((event.depth == 1U) && xml_span_local_equal(source, length, event.name, "spine")) {
      spine_depth = event.depth;
    } else if (priv_epub_xml_direct_child(event.depth, spine_depth) &&
               xml_span_local_equal(source,
                                    length,
                                    priv_epub_xml_frame_name(&reader, spine_depth),
                                    "spine") &&
               xml_span_equal(source, length, event.name, "itemref")) {
      const priv_attr_t idref = priv_epub_xml_attr(source, length, &event, "idref");
      if (!idref.present) {
        continue;
      }
      if (book->xml_workspace.reference_count >= (uint16_t)k_epub_max_chapters) {
        return k_ra8_err_no_mem;
      }
      book->xml_workspace.references[book->xml_workspace.reference_count] = idref.span;
      ++book->xml_workspace.reference_count;
    } else {
      /* Other start elements do not contribute spine references. */
    }
  }
  return err;
}

RA8_INTERNAL static ra8_err_t internal_manifest_lookup(const uint8_t*        source,
                                                       size_t                length,
                                                       epub_xml_workspace_t* workspace,
                                                       xml_span_t            wanted,
                                                       xml_span_t*           out_href)
{
  xml_reader_t reader         = {};
  ra8_err_t    err            = priv_epub_xml_reader(&reader, source, length, workspace);
  uint16_t     manifest_depth = UINT16_MAX;
  while (err == k_ra8_ok) {
    xml_event_t event = {};
    err               = xml_reader_next(&reader, &event);
    // mcdc-deactivated: internal_manifest_lookup pull-loop status gate; the public entry point validated this exact byte range with xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which xml never reads back -- so xml_reader_next() cannot fail here and only the end-of-document condition varies.
    if ((err != k_ra8_ok) || (event.kind == (uint8_t)k_xml_event_none)) {
      break;
    }
    if (event.kind != (uint8_t)k_xml_event_start) {
      continue;
    }
    if ((event.depth == 1U) && xml_span_local_equal(source, length, event.name, "manifest")) {
      manifest_depth = event.depth;
    } else if (priv_epub_xml_direct_child(event.depth, manifest_depth) &&
               xml_span_local_equal(source,
                                    length,
                                    priv_epub_xml_frame_name(&reader, manifest_depth),
                                    "manifest")) {
      const priv_attr_t id   = priv_epub_xml_attr(source, length, &event, "id");
      const priv_attr_t href = priv_epub_xml_attr(source, length, &event, "href");
      if (id.present && href.present && xml_decoded_equal(source, length, id.span, wanted)) {
        *out_href = href.span;
        return k_ra8_ok;
      }
    } else {
      /* Other start elements cannot match this manifest lookup. */
    }
  }
  return (err == k_ra8_ok) ? k_ra8_err_no_data : err;
}

RA8_INTERNAL static ra8_err_t
internal_opf_shape(const uint8_t* source, size_t length, epub_xml_workspace_t* workspace)
{
  xml_reader_t reader       = {};
  ra8_err_t    err          = priv_epub_xml_reader(&reader, source, length, workspace);
  bool         saw_manifest = false;
  bool         saw_spine    = false;
  uint16_t     spine_depth  = UINT16_MAX;
  uint16_t     references   = 0U;
  while (err == k_ra8_ok) {
    xml_event_t event = {};
    err               = xml_reader_next(&reader, &event);
    // mcdc-deactivated: internal_opf_shape pull-loop status gate; the public entry point validated this exact byte range with xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which xml never reads back -- so xml_reader_next() cannot fail here and only the end-of-document condition varies.
    if ((err != k_ra8_ok) || (event.kind == (uint8_t)k_xml_event_none)) {
      break;
    }
    if (event.kind != (uint8_t)k_xml_event_start) {
      continue;
    }
    if ((event.depth == 1U) && xml_span_local_equal(source, length, event.name, "manifest")) {
      saw_manifest = true;
    } else if ((event.depth == 1U) && xml_span_local_equal(source, length, event.name, "spine")) {
      saw_spine   = true;
      spine_depth = event.depth;
    } else if (priv_epub_xml_direct_child(event.depth, spine_depth) &&
               xml_span_equal(source, length, event.name, "itemref") &&
               priv_epub_xml_attr(source, length, &event, "idref").present) {
      if (references >= (uint16_t)k_epub_max_chapters) {
        return k_ra8_err_no_mem;
      }
      ++references;
    } else {
      /* Other start elements do not affect the OPF shape. */
    }
  }
  // mcdc-deactivated: internal_opf_shape completeness gate; the loop above exits only by end-of-document or by returning k_ra8_err_no_mem outright, and the public entry point validated this exact byte range with xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which xml never reads back -- so xml_reader_next() cannot fail here, so `err` is always k_ra8_ok here and only the two shape flags vary.
  if ((err == k_ra8_ok) && (!saw_manifest || !saw_spine)) {
    return k_ra8_err_validation_failed;
  }
  return err;
}

/**
 * @brief Resolve chapter, cover, and TOC hrefs from cross-referenced ids.
 * @details Looks up each spine reference ::internal_collect_spine already
 * gathered against the manifest to fill in the ordered chapter path
 * table, then resolves the legacy cover id and the spine `toc=`
 * attribute the same way.
 * @param[in] xml_bytes Complete OPF byte range.
 * @param[in] xml_len Readable bytes at @p xml_bytes.
 * @param[in] spine_toc Spine `toc` attribute span, or empty if absent.
 * @param[in,out] book Book whose chapter/cover/toc fields are filled in.
 * @return Canonical status.
 * @retval k_ra8_ok Every manifest lookup that mattered succeeded or was
 * legitimately absent.
 * @retval other The first manifest-lookup failure that is not "no data".
 * @pre @p book's spine references were already collected.
 * @pre @p book's xml_workspace names a live reader over @p xml_bytes.
 * @post @p book->chapter_paths holds one entry per resolved spine
 * reference and @p book->chapter_count is their count.
 * @post @p book->cover_path and @p book->toc_path are filled when their
 * source ids resolve; @p book->toc_kind may flip to the NCX kind.
 * @note Extracted from ::priv_epub_xml_parse_opf so both stay within
 * the 60-line function cap; the vectors that cover it are unchanged.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_opf_resolve_refs(const uint8_t* xml_bytes,
                                                        size_t         xml_len,
                                                        xml_span_t     spine_toc,
                                                        epub_book_t*   book)
{
  ra8_err_t err       = k_ra8_ok;
  book->chapter_count = 0U;
  // mcdc-deactivated: internal_opf_resolve_refs spine-reference loop status gate; `err` enters at k_ra8_ok -- priv_epub_xml_parse_opf calls this helper only from its `if (err == k_ra8_ok)` arm and the helper re-initialises it -- and the spine-reference loop's `err = found` arm is its only writer, which needs internal_manifest_lookup to return something other than k_ra8_ok or k_ra8_err_no_data. Its only such returns are propagated xml_reader_init()/xml_reader_next() failures over the exact byte range priv_epub_xml_parse_opf already accepted with xml_validate(), which runs that same bounded pull reader as a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which xml never reads back -- and xml_reader_init() rejects only a null pointer or a zero/oversized length, none of which can hold here because that same xml_validate() call accepted this (xml_bytes, xml_len) pair through that identical init. The status condition is therefore constant-true and only the reference-count bound varies.
  for (uint16_t i = 0U; (err == k_ra8_ok) && (i < book->xml_workspace.reference_count); ++i) {
    xml_span_t      href  = {};
    const ra8_err_t found = internal_manifest_lookup(xml_bytes,
                                                     xml_len,
                                                     &book->xml_workspace,
                                                     book->xml_workspace.references[i],
                                                     &href);
    if (found == k_ra8_ok) {
      const uint16_t chapter = book->chapter_count;
      priv_epub_xml_copy(xml_bytes,
                         xml_len,
                         href,
                         book->chapter_paths[chapter],
                         k_epub_max_path_len);
      ++book->chapter_count;
    } else if (found != k_ra8_err_no_data) {
      err = found;
    } else {
      /* A missing optional manifest reference is skipped. */
    }
  }
  // mcdc-deactivated: internal_opf_resolve_refs legacy-cover resolution gate; `err` enters at k_ra8_ok -- priv_epub_xml_parse_opf calls this helper only from its `if (err == k_ra8_ok)` arm and the helper re-initialises it -- and the spine-reference loop's `err = found` arm is its only writer, which needs internal_manifest_lookup to return something other than k_ra8_ok or k_ra8_err_no_data. Its only such returns are propagated xml_reader_init()/xml_reader_next() failures over the exact byte range priv_epub_xml_parse_opf already accepted with xml_validate(), which runs that same bounded pull reader as a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which xml never reads back -- and xml_reader_init() rejects only a null pointer or a zero/oversized length, none of which can hold here because that same xml_validate() call accepted this (xml_bytes, xml_len) pair through that identical init. The status condition is therefore constant-true and only the empty-cover-path and legacy-cover-id-present conditions vary.
  if ((err == k_ra8_ok) && (book->cover_path[0] == '\0') &&
      (book->xml_workspace.legacy_cover_id.length > 0U)) {
    xml_span_t href = {};
    if (internal_manifest_lookup(xml_bytes,
                                 xml_len,
                                 &book->xml_workspace,
                                 book->xml_workspace.legacy_cover_id,
                                 &href) == k_ra8_ok) {
      priv_epub_xml_copy(xml_bytes, xml_len, href, book->cover_path, sizeof(book->cover_path));
    }
  }
  // mcdc-deactivated: internal_opf_resolve_refs spine `toc` fallback gate; `err` enters at k_ra8_ok -- priv_epub_xml_parse_opf calls this helper only from its `if (err == k_ra8_ok)` arm and the helper re-initialises it -- and the spine-reference loop's `err = found` arm is its only writer, which needs internal_manifest_lookup to return something other than k_ra8_ok or k_ra8_err_no_data. Its only such returns are propagated xml_reader_init()/xml_reader_next() failures over the exact byte range priv_epub_xml_parse_opf already accepted with xml_validate(), which runs that same bounded pull reader as a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which xml never reads back -- and xml_reader_init() rejects only a null pointer or a zero/oversized length, none of which can hold here because that same xml_validate() call accepted this (xml_bytes, xml_len) pair through that identical init. The status condition is therefore constant-true and only the non-nav TOC kind and the present spine `toc` attribute vary.
  if ((err == k_ra8_ok) && (book->toc_kind != (uint8_t)k_epub_toc_nav) && (spine_toc.length > 0U)) {
    xml_span_t href = {};
    if (internal_manifest_lookup(xml_bytes, xml_len, &book->xml_workspace, spine_toc, &href) ==
        k_ra8_ok) {
      priv_epub_xml_copy(xml_bytes, xml_len, href, book->toc_path, sizeof(book->toc_path));
      book->toc_kind = (uint8_t)k_epub_toc_ncx;
    }
  }
  return err;
}

ra8_err_t priv_epub_xml_parse_opf(const uint8_t* xml_bytes, size_t xml_len, epub_book_t* book)
{
  if ((xml_bytes == nullptr) || (book == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (xml_len == 0U) {
    return k_ra8_err_invalid_size;
  }
  ra8_err_t  err       = xml_validate(xml_bytes, xml_len, &book->xml_workspace.reader);
  xml_span_t spine_toc = {};
  if (err == k_ra8_ok) {
    err = internal_opf_shape(xml_bytes, xml_len, &book->xml_workspace);
  }
  if (err == k_ra8_ok) {
    err = internal_opf_first(xml_bytes, xml_len, book, &spine_toc);
  }
  if (err == k_ra8_ok) {
    err = internal_collect_spine(xml_bytes, xml_len, book);
  }
  if (err == k_ra8_ok) {
    err = internal_opf_resolve_refs(xml_bytes, xml_len, spine_toc, book);
  }
  return err;
}
