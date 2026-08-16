/**
 * @file ra8_epub_xml_shim.c
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

#include "ra8_attributes.h"
#include "ra8_epub_xml_consumer_internal.h"
#include "ra8_epub_xml_shim_internal.h"
#include "ra8_xml.h"

/** @brief Consumer markers stored in live reader frames. */
typedef enum : uint16_t {
  k_priv_mark_none             = 0U, /**< Frame carries no metadata role.       */
  k_priv_mark_title            = 1U, /**< Frame is a Dublin Core title.         */
  k_priv_mark_creator          = 2U, /**< Frame is a Dublin Core creator.       */
  k_priv_mark_language         = 3U, /**< Frame is a Dublin Core language.      */
  k_priv_mark_identifier       = 4U, /**< Frame is a fallback identifier.       */
  k_priv_mark_identifier_match = 5U, /**< Frame matches the package identifier. */
} priv_marker_t;

/* see header for the documented contract. */
RA8_INTERNAL static void internal_copy(const uint8_t* source,
                                       size_t         source_len,
                                       ra8_xml_span_t span,
                                       char*          destination,
                                       size_t         capacity)
{
  size_t ignored = 0U;
  (void)ra8_xml_decode_prefix(source, source_len, span, destination, capacity, &ignored);
}

/* see header for the documented contract. */
RA8_INTERNAL static priv_attr_t internal_attr(const uint8_t*         source,
                                              size_t                 source_len,
                                              const ra8_xml_event_t* event,
                                              const char*            name)
{
  ra8_xml_attr_cursor_t cursor = {};
  ra8_xml_attr_begin(event, &cursor);
  for (uint16_t i = 0U; i < event->attribute_count; ++i) {
    ra8_xml_attribute_t attribute = {};
    bool                present   = false;
    // mcdc-deactivated: internal_attr attribute re-walk gate; internal_attributes fixed event->attribute_count by successfully parsing exactly that many attributes over these immutable bytes, so the bounded re-parse cannot fail and ra8_xml_attr_next() only clears `present` once `emitted` reaches that same count, which this loop never exceeds -- both conditions are constant-false.
    if ((ra8_xml_attr_next(source, source_len, event, &cursor, &attribute, &present) != k_ra8_ok) ||
        !present) {
      break;
    }
    if (ra8_xml_span_equal(source, source_len, attribute.name, name)) {
      return (priv_attr_t){attribute.value, true};
    }
  }
  return (priv_attr_t){};
}

/* see header for the documented contract. */
RA8_INTERNAL static bool internal_attr_contains(const uint8_t* source,
                                                size_t         source_len,
                                                priv_attr_t    attribute,
                                                const char*    needle)
{
  if (!attribute.present) {
    return false;
  }
  char   value[k_ra8_epub_max_path_len] = {};
  size_t length                         = 0U;
  if (ra8_xml_decode(source, source_len, attribute.span, value, sizeof(value), &length) !=
      k_ra8_ok) {
    return false;
  }
  return strstr(value, needle) != nullptr;
}

/* see header for the documented contract. */
RA8_INTERNAL static ra8_xml_span_t internal_frame_name(const ra8_xml_reader_t* reader,
                                                       uint16_t                frame)
{
  const ra8_xml_frame_t* value = &reader->workspace->frames[frame];
  return (ra8_xml_span_t){value->name_offset, value->name_length};
}

/* see header for the documented contract. */
RA8_INTERNAL static bool internal_direct_child(uint16_t child, uint16_t parent)
{
  if (parent == UINT16_MAX) {
    return false;
  }
  ++parent;
  return child == parent;
}

/* see header for the documented contract. */
RA8_INTERNAL static bool
internal_ancestor_frame(uint16_t depth, uint16_t levels, uint16_t* out_frame)
{
  while (levels > 0U) {
    if (depth == 0U) {
      return false;
    }
    --depth;
    --levels;
  }
  *out_frame = depth;
  return true;
}

/* see header for the documented contract. */
RA8_INTERNAL static ra8_xml_span_t
internal_ancestor_name(const ra8_xml_reader_t* reader, uint16_t depth, uint16_t levels)
{
  uint16_t frame = 0U;
  return internal_ancestor_frame(depth, levels, &frame) ? internal_frame_name(reader, frame)
                                                        : (ra8_xml_span_t){};
}

/* see header for the documented contract. */
RA8_INTERNAL static uint16_t
internal_ancestor_marker(const ra8_xml_reader_t* reader, uint16_t depth, uint16_t levels)
{
  uint16_t frame = 0U;
  return internal_ancestor_frame(depth, levels, &frame) ? reader->workspace->frames[frame].consumer
                                                        : 0U;
}

/* see header for the documented contract. */
RA8_INTERNAL static ra8_err_t internal_reader(ra8_xml_reader_t*         reader,
                                              const uint8_t*            source,
                                              size_t                    length,
                                              ra8_epub_xml_workspace_t* workspace)
{
  return ra8_xml_reader_init(reader, source, length, &workspace->reader);
}

/* see header for the documented contract. */
RA8_INTERNAL static ra8_err_t internal_find(const uint8_t*            source,
                                            size_t                    length,
                                            ra8_epub_xml_workspace_t* workspace,
                                            const char*               local,
                                            bool                      use_parent,
                                            uint32_t                  parent_offset,
                                            uint16_t                  parent_depth,
                                            ra8_xml_event_t*          out)
{
  ra8_xml_reader_t reader        = {};
  ra8_err_t        err           = internal_reader(&reader, source, length, workspace);
  bool             parent_active = false;
  while (err == k_ra8_ok) {
    ra8_xml_event_t event = {};
    err                   = ra8_xml_reader_next(&reader, &event);
    if ((err != k_ra8_ok) || (event.kind == (uint8_t)k_ra8_xml_event_none)) {
      break;
    }
    if (use_parent && (event.kind == (uint8_t)k_ra8_xml_event_start) &&
        (event.depth == parent_depth) && (event.markup.offset == parent_offset)) {
      parent_active = true;
      continue;
    }
    if ((event.kind == (uint8_t)k_ra8_xml_event_start) &&
        ra8_xml_span_local_equal(source, length, event.name, local)) {
      if (!use_parent || (parent_active && internal_direct_child(event.depth, parent_depth))) {
        *out = event;
        return k_ra8_ok;
      }
    } else if (use_parent && parent_active && (event.kind == (uint8_t)k_ra8_xml_event_end) &&
               (event.depth == parent_depth)) {
      return k_ra8_err_validation_failed;
    }
  }
  return (err == k_ra8_ok) ? k_ra8_err_validation_failed : err;
}

ra8_err_t priv_ra8_epub_xml_parse_container(const uint8_t*               xml_bytes,
                                            size_t                       xml_len,
                                            ra8_epub_container_result_t* out,
                                            ra8_epub_xml_workspace_t*    workspace)
{
  if ((xml_bytes == nullptr) || (out == nullptr) || (workspace == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (xml_len == 0U) {
    return k_ra8_err_invalid_size;
  }
  ra8_err_t       err       = ra8_xml_validate(xml_bytes, xml_len, &workspace->reader);
  ra8_xml_event_t rootfiles = {};
  if (err == k_ra8_ok) {
    err = internal_find(xml_bytes, xml_len, workspace, "rootfiles", false, 0U, 0U, &rootfiles);
  }
  ra8_xml_event_t rootfile = {};
  if (err == k_ra8_ok) {
    err = internal_find(xml_bytes,
                        xml_len,
                        workspace,
                        "rootfile",
                        true,
                        rootfiles.markup.offset,
                        rootfiles.depth,
                        &rootfile);
  } else if (err == k_ra8_err_validation_failed) {
    err = internal_find(xml_bytes, xml_len, workspace, "rootfile", false, 0U, 0U, &rootfile);
  }
  if (err != k_ra8_ok) {
    return err;
  }
  const priv_attr_t path = internal_attr(xml_bytes, xml_len, &rootfile, "full-path");
  if (!path.present || (path.span.length == 0U)) {
    return k_ra8_err_validation_failed;
  }
  out->opf_path[0] = '\0';
  internal_copy(xml_bytes, xml_len, path.span, out->opf_path, sizeof(out->opf_path));
  return k_ra8_ok;
}

/* see header for the documented contract. */
RA8_INTERNAL static bool
internal_font_type(const uint8_t* source, size_t source_len, priv_attr_t media)
{
  static const char* const types[] = {"application/font-sfnt",
                                      "application/vnd.ms-opentype",
                                      "font/ttf",
                                      "font/otf",
                                      "application/x-font-ttf"};
  for (size_t i = 0U; i < (sizeof(types) / sizeof(types[0])); ++i) {
    if (media.present && ra8_xml_span_equal(source, source_len, media.span, types[i])) {
      return true;
    }
  }
  return false;
}

/* see header for the documented contract. */
RA8_INTERNAL static void internal_manifest_item(const uint8_t*         source,
                                                size_t                 source_len,
                                                const ra8_xml_event_t* event,
                                                ra8_epub_book_t*       book)
{
  const priv_attr_t id         = internal_attr(source, source_len, event, "id");
  const priv_attr_t href       = internal_attr(source, source_len, event, "href");
  const priv_attr_t media      = internal_attr(source, source_len, event, "media-type");
  const priv_attr_t properties = internal_attr(source, source_len, event, "properties");
  if (book->manifest_count < (uint16_t)k_ra8_epub_max_manifest) {
    ra8_epub_manifest_item_t* item = &book->manifest[book->manifest_count++];
    if (id.present) {
      internal_copy(source, source_len, id.span, item->id, sizeof(item->id));
    }
    if (href.present) {
      internal_copy(source, source_len, href.span, item->href, sizeof(item->href));
    }
    if (media.present) {
      internal_copy(source, source_len, media.span, item->media_type, sizeof(item->media_type));
    }
  }
  if (href.present && internal_font_type(source, source_len, media) &&
      (book->embedded_font_count < (uint16_t)k_ra8_epub_max_fonts)) {
    internal_copy(source,
                  source_len,
                  href.span,
                  book->embedded_font_paths[book->embedded_font_count++],
                  k_ra8_epub_max_path_len);
  }
  if (href.present && internal_attr_contains(source, source_len, properties, "cover-image")) {
    internal_copy(source, source_len, href.span, book->cover_path, sizeof(book->cover_path));
  }
  if (href.present && internal_attr_contains(source, source_len, properties, "nav")) {
    internal_copy(source, source_len, href.span, book->toc_path, sizeof(book->toc_path));
    book->toc_kind = (uint8_t)k_ra8_epub_toc_nav;
  }
}

/* see header for the documented contract. */
RA8_INTERNAL static void internal_metadata_text(const uint8_t*          source,
                                                size_t                  source_len,
                                                const ra8_xml_event_t*  event,
                                                const ra8_xml_reader_t* reader,
                                                ra8_epub_book_t*        book)
{
  if (event->depth == 0U) {
    return;
  }
  const uint16_t marker = internal_ancestor_marker(reader, event->depth, 1U);
  if (marker == (uint16_t)k_priv_mark_title) {
    internal_copy(source, source_len, event->markup, book->title, sizeof(book->title));
  } else if (marker == (uint16_t)k_priv_mark_creator) {
    internal_copy(source, source_len, event->markup, book->author, sizeof(book->author));
  } else if (marker == (uint16_t)k_priv_mark_language) {
    internal_copy(source, source_len, event->markup, book->language, sizeof(book->language));
  } else if ((marker == (uint16_t)k_priv_mark_identifier_match) ||
             ((marker == (uint16_t)k_priv_mark_identifier) && (book->identifier[0] == '\0'))) {
    internal_copy(source, source_len, event->markup, book->identifier, sizeof(book->identifier));
  }
}

/* see header for the documented contract. */
RA8_INTERNAL static void internal_mark_metadata(const uint8_t*         source,
                                                size_t                 source_len,
                                                const ra8_xml_event_t* event,
                                                ra8_xml_reader_t*      reader,
                                                ra8_xml_span_t         unique_id)
{
  uint16_t marker = (uint16_t)k_priv_mark_none;
  if (ra8_xml_span_local_equal(source, source_len, event->name, "title")) {
    marker = (uint16_t)k_priv_mark_title;
  } else if (ra8_xml_span_local_equal(source, source_len, event->name, "creator")) {
    marker = (uint16_t)k_priv_mark_creator;
  } else if (ra8_xml_span_local_equal(source, source_len, event->name, "language")) {
    marker = (uint16_t)k_priv_mark_language;
  } else if (ra8_xml_span_local_equal(source, source_len, event->name, "identifier")) {
    marker               = (uint16_t)k_priv_mark_identifier;
    const priv_attr_t id = internal_attr(source, source_len, event, "id");
    if (id.present && (unique_id.length > 0U) &&
        ra8_xml_decoded_equal(source, source_len, id.span, unique_id)) {
      marker = (uint16_t)k_priv_mark_identifier_match;
    }
  }
  reader->workspace->frames[event->depth].consumer = marker;
}

/* see header for the documented contract. */
RA8_INTERNAL static ra8_err_t
internal_opf_status(ra8_err_t err, uint16_t manifest_depth, uint16_t spine_depth)
{
  // mcdc-deactivated: internal_opf_status package-shape gate; it runs only after internal_opf_shape accepted the same bytes using the identical depth-1 <manifest>/<spine> predicates that assign manifest_depth and spine_depth here, so neither can still be UINT16_MAX, and the public entry point validated this exact byte range with ra8_xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which ra8_xml never reads back -- so ra8_xml_reader_next() cannot fail here -- all three conditions are constant.
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
RA8_INTERNAL static void internal_opf_metadata_child(const uint8_t*         source,
                                                     size_t                 length,
                                                     const ra8_xml_event_t* event,
                                                     ra8_xml_reader_t*      reader,
                                                     ra8_xml_span_t         unique_id,
                                                     ra8_epub_book_t*       book)
{
  internal_mark_metadata(source, length, event, reader, unique_id);
  if (!ra8_xml_span_local_equal(source, length, event->name, "meta")) {
    return;
  }
  const priv_attr_t name    = internal_attr(source, length, event, "name");
  const priv_attr_t content = internal_attr(source, length, event, "content");
  if (name.present && content.present && ra8_xml_span_equal(source, length, name.span, "cover")) {
    book->xml_workspace.legacy_cover_id = content.span;
  }
}

/* see header for the documented contract. */
RA8_INTERNAL static ra8_err_t internal_opf_first(const uint8_t*   source,
                                                 size_t           length,
                                                 ra8_epub_book_t* book,
                                                 ra8_xml_span_t*  out_spine_toc)
{
  ra8_xml_reader_t reader         = {};
  ra8_err_t        err            = internal_reader(&reader, source, length, &book->xml_workspace);
  uint16_t         metadata_depth = UINT16_MAX;
  uint16_t         manifest_depth = UINT16_MAX;
  uint16_t         spine_depth    = UINT16_MAX;
  ra8_xml_span_t   unique_id      = {};
  while (err == k_ra8_ok) {
    ra8_xml_event_t event = {};
    err                   = ra8_xml_reader_next(&reader, &event);
    // mcdc-deactivated: internal_opf_first pull-loop status gate; the public entry point validated this exact byte range with ra8_xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which ra8_xml never reads back -- so ra8_xml_reader_next() cannot fail here and only the end-of-document condition varies.
    if ((err != k_ra8_ok) || (event.kind == (uint8_t)k_ra8_xml_event_none)) {
      break;
    }
    if (event.kind == (uint8_t)k_ra8_xml_event_text) {
      internal_metadata_text(source, length, &event, &reader, book);
      continue;
    }
    if (event.kind != (uint8_t)k_ra8_xml_event_start) {
      continue;
    }
    if (event.depth == 0U) {
      const priv_attr_t uid = internal_attr(source, length, &event, "unique-identifier");
      unique_id             = uid.present ? uid.span : (ra8_xml_span_t){};
    } else if ((event.depth == 1U) &&
               ra8_xml_span_local_equal(source, length, event.name, "metadata")) {
      metadata_depth = event.depth;
    } else if ((event.depth == 1U) &&
               ra8_xml_span_local_equal(source, length, event.name, "manifest")) {
      manifest_depth = event.depth;
    } else if ((event.depth == 1U) &&
               ra8_xml_span_local_equal(source, length, event.name, "spine")) {
      spine_depth           = event.depth;
      const priv_attr_t toc = internal_attr(source, length, &event, "toc");
      *out_spine_toc        = toc.present ? toc.span : (ra8_xml_span_t){};
    } else if (internal_direct_child(event.depth, metadata_depth) &&
               ra8_xml_span_local_equal(source,
                                        length,
                                        internal_frame_name(&reader, metadata_depth),
                                        "metadata")) {
      internal_opf_metadata_child(source, length, &event, &reader, unique_id, book);
    } else if (internal_direct_child(event.depth, manifest_depth) &&
               ra8_xml_span_local_equal(source,
                                        length,
                                        internal_frame_name(&reader, manifest_depth),
                                        "manifest") &&
               ra8_xml_span_local_equal(source, length, event.name, "item")) {
      internal_manifest_item(source, length, &event, book);
    }
    (void)spine_depth;
  }
  return internal_opf_status(err, manifest_depth, spine_depth);
}

/* see header for the documented contract. */
RA8_INTERNAL static ra8_err_t
internal_collect_spine(const uint8_t* source, size_t length, ra8_epub_book_t* book)
{
  ra8_xml_reader_t reader      = {};
  ra8_err_t        err         = internal_reader(&reader, source, length, &book->xml_workspace);
  uint16_t         spine_depth = UINT16_MAX;
  book->xml_workspace.reference_count = 0U;
  while (err == k_ra8_ok) {
    ra8_xml_event_t event = {};
    err                   = ra8_xml_reader_next(&reader, &event);
    // mcdc-deactivated: internal_collect_spine pull-loop status gate; the public entry point validated this exact byte range with ra8_xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which ra8_xml never reads back -- so ra8_xml_reader_next() cannot fail here and only the end-of-document condition varies.
    if ((err != k_ra8_ok) || (event.kind == (uint8_t)k_ra8_xml_event_none)) {
      break;
    }
    if (event.kind != (uint8_t)k_ra8_xml_event_start) {
      continue;
    }
    if ((event.depth == 1U) && ra8_xml_span_local_equal(source, length, event.name, "spine")) {
      spine_depth = event.depth;
    } else if (internal_direct_child(event.depth, spine_depth) &&
               ra8_xml_span_local_equal(source,
                                        length,
                                        internal_frame_name(&reader, spine_depth),
                                        "spine") &&
               ra8_xml_span_equal(source, length, event.name, "itemref")) {
      const priv_attr_t idref = internal_attr(source, length, &event, "idref");
      if (!idref.present) {
        continue;
      }
      if (book->xml_workspace.reference_count >= (uint16_t)k_ra8_epub_max_chapters) {
        return k_ra8_err_no_mem;
      }
      book->xml_workspace.references[book->xml_workspace.reference_count++] = idref.span;
    }
  }
  return err;
}

/* see header for the documented contract. */
RA8_INTERNAL static ra8_err_t internal_manifest_lookup(const uint8_t*            source,
                                                       size_t                    length,
                                                       ra8_epub_xml_workspace_t* workspace,
                                                       ra8_xml_span_t            wanted,
                                                       ra8_xml_span_t*           out_href)
{
  ra8_xml_reader_t reader         = {};
  ra8_err_t        err            = internal_reader(&reader, source, length, workspace);
  uint16_t         manifest_depth = UINT16_MAX;
  while (err == k_ra8_ok) {
    ra8_xml_event_t event = {};
    err                   = ra8_xml_reader_next(&reader, &event);
    // mcdc-deactivated: internal_manifest_lookup pull-loop status gate; the public entry point validated this exact byte range with ra8_xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which ra8_xml never reads back -- so ra8_xml_reader_next() cannot fail here and only the end-of-document condition varies.
    if ((err != k_ra8_ok) || (event.kind == (uint8_t)k_ra8_xml_event_none)) {
      break;
    }
    if (event.kind != (uint8_t)k_ra8_xml_event_start) {
      continue;
    }
    if ((event.depth == 1U) && ra8_xml_span_local_equal(source, length, event.name, "manifest")) {
      manifest_depth = event.depth;
    } else if (internal_direct_child(event.depth, manifest_depth) &&
               ra8_xml_span_local_equal(source,
                                        length,
                                        internal_frame_name(&reader, manifest_depth),
                                        "manifest")) {
      const priv_attr_t id   = internal_attr(source, length, &event, "id");
      const priv_attr_t href = internal_attr(source, length, &event, "href");
      if (id.present && href.present && ra8_xml_decoded_equal(source, length, id.span, wanted)) {
        *out_href = href.span;
        return k_ra8_ok;
      }
    }
  }
  return (err == k_ra8_ok) ? k_ra8_err_no_data : err;
}

/* see header for the documented contract. */
RA8_INTERNAL static ra8_err_t
internal_opf_shape(const uint8_t* source, size_t length, ra8_epub_xml_workspace_t* workspace)
{
  ra8_xml_reader_t reader       = {};
  ra8_err_t        err          = internal_reader(&reader, source, length, workspace);
  bool             saw_manifest = false;
  bool             saw_spine    = false;
  uint16_t         spine_depth  = UINT16_MAX;
  uint16_t         references   = 0U;
  while (err == k_ra8_ok) {
    ra8_xml_event_t event = {};
    err                   = ra8_xml_reader_next(&reader, &event);
    // mcdc-deactivated: internal_opf_shape pull-loop status gate; the public entry point validated this exact byte range with ra8_xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which ra8_xml never reads back -- so ra8_xml_reader_next() cannot fail here and only the end-of-document condition varies.
    if ((err != k_ra8_ok) || (event.kind == (uint8_t)k_ra8_xml_event_none)) {
      break;
    }
    if (event.kind != (uint8_t)k_ra8_xml_event_start) {
      continue;
    }
    if ((event.depth == 1U) && ra8_xml_span_local_equal(source, length, event.name, "manifest")) {
      saw_manifest = true;
    } else if ((event.depth == 1U) &&
               ra8_xml_span_local_equal(source, length, event.name, "spine")) {
      saw_spine   = true;
      spine_depth = event.depth;
    } else if (internal_direct_child(event.depth, spine_depth) &&
               ra8_xml_span_equal(source, length, event.name, "itemref") &&
               internal_attr(source, length, &event, "idref").present) {
      if (references >= (uint16_t)k_ra8_epub_max_chapters) {
        return k_ra8_err_no_mem;
      }
      ++references;
    }
  }
  // mcdc-deactivated: internal_opf_shape completeness gate; the loop above exits only by end-of-document or by returning k_ra8_err_no_mem outright, and the public entry point validated this exact byte range with ra8_xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which ra8_xml never reads back -- so ra8_xml_reader_next() cannot fail here, so `err` is always k_ra8_ok here and only the two shape flags vary.
  if ((err == k_ra8_ok) && (!saw_manifest || !saw_spine)) {
    return k_ra8_err_validation_failed;
  }
  return err;
}

ra8_err_t
priv_ra8_epub_xml_parse_opf(const uint8_t* xml_bytes, size_t xml_len, ra8_epub_book_t* book)
{
  if ((xml_bytes == nullptr) || (book == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (xml_len == 0U) {
    return k_ra8_err_invalid_size;
  }
  ra8_err_t      err       = ra8_xml_validate(xml_bytes, xml_len, &book->xml_workspace.reader);
  ra8_xml_span_t spine_toc = {};
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
    book->chapter_count = 0U;
  }
  for (uint16_t i = 0U; (err == k_ra8_ok) && (i < book->xml_workspace.reference_count); ++i) {
    ra8_xml_span_t  href  = {};
    const ra8_err_t found = internal_manifest_lookup(xml_bytes,
                                                     xml_len,
                                                     &book->xml_workspace,
                                                     book->xml_workspace.references[i],
                                                     &href);
    if (found == k_ra8_ok) {
      internal_copy(xml_bytes,
                    xml_len,
                    href,
                    book->chapter_paths[book->chapter_count++],
                    k_ra8_epub_max_path_len);
    } else if (found != k_ra8_err_no_data) {
      err = found;
    }
  }
  if ((err == k_ra8_ok) && (book->cover_path[0] == '\0') &&
      (book->xml_workspace.legacy_cover_id.length > 0U)) {
    ra8_xml_span_t href = {};
    if (internal_manifest_lookup(xml_bytes,
                                 xml_len,
                                 &book->xml_workspace,
                                 book->xml_workspace.legacy_cover_id,
                                 &href) == k_ra8_ok) {
      internal_copy(xml_bytes, xml_len, href, book->cover_path, sizeof(book->cover_path));
    }
  }
  if ((err == k_ra8_ok) && (book->toc_kind != (uint8_t)k_ra8_epub_toc_nav) &&
      (spine_toc.length > 0U)) {
    ra8_xml_span_t href = {};
    if (internal_manifest_lookup(xml_bytes, xml_len, &book->xml_workspace, spine_toc, &href) ==
        k_ra8_ok) {
      internal_copy(xml_bytes, xml_len, href, book->toc_path, sizeof(book->toc_path));
      book->toc_kind = (uint8_t)k_ra8_epub_toc_ncx;
    }
  }
  return err;
}

/* see header for the documented contract. */
RA8_INTERNAL static uint8_t internal_ancestor_depth(const uint8_t*          source,
                                                    size_t                  source_len,
                                                    const ra8_xml_reader_t* reader,
                                                    uint16_t                depth,
                                                    const char*             local)
{
  uint8_t count = 0U;
  for (uint16_t i = 0U; i < depth; ++i) {
    if (ra8_xml_span_local_equal(source, source_len, internal_frame_name(reader, i), local)) {
      ++count;
    }
  }
  return count;
}

/* see header for the documented contract. */
RA8_INTERNAL static uint16_t internal_toc_reserve(const uint8_t*         source,
                                                  size_t                 source_len,
                                                  const ra8_xml_event_t* event,
                                                  ra8_xml_reader_t*      reader,
                                                  ra8_epub_book_t*       book,
                                                  const char*            local)
{
  uint16_t marker = 0U;
  if (book->toc_count < (uint16_t)k_ra8_epub_max_toc) {
    const uint16_t slot = book->toc_count++;
    book->toc[slot]     = (ra8_epub_toc_entry_t){};
    book->toc[slot].depth =
      internal_ancestor_depth(source, source_len, reader, event->depth, local);
    marker = (uint16_t)(slot + 1U);
  }
  if (event->self_closing == 0U) {
    reader->workspace->frames[event->depth].consumer = marker;
  }
  return marker;
}

/* see header for the documented contract. */
RA8_INTERNAL static uint16_t internal_toc_marker(const ra8_xml_reader_t* reader, uint16_t depth)
{
  while (depth > 0U) {
    --depth;
    const uint16_t marker = reader->workspace->frames[depth].consumer;
    if (marker != 0U) {
      return marker;
    }
  }
  return 0U;
}

/* see header for the documented contract. */
RA8_INTERNAL static ra8_err_t internal_toc_capacity(const uint8_t*            source,
                                                    size_t                    length,
                                                    ra8_epub_xml_workspace_t* workspace,
                                                    const ra8_xml_event_t*    selected,
                                                    const char*               entry_local,
                                                    bool                      require_list)
{
  ra8_xml_reader_t reader  = {};
  ra8_err_t        err     = internal_reader(&reader, source, length, workspace);
  bool             active  = false;
  bool             enabled = !require_list;
  uint16_t         count   = 0U;
  while (err == k_ra8_ok) {
    ra8_xml_event_t event = {};
    err                   = ra8_xml_reader_next(&reader, &event);
    // mcdc-deactivated: internal_toc_capacity pull-loop status gate; the public entry point validated this exact byte range with ra8_xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which ra8_xml never reads back -- so ra8_xml_reader_next() cannot fail here and only the end-of-document condition varies.
    if ((err != k_ra8_ok) || (event.kind == (uint8_t)k_ra8_xml_event_none)) {
      break;
    }
    if (!active && (event.kind == (uint8_t)k_ra8_xml_event_start) &&
        (event.markup.offset == selected->markup.offset)) {
      active = true;
    } else if (active && (event.kind == (uint8_t)k_ra8_xml_event_start) && require_list &&
               internal_direct_child(event.depth, selected->depth) &&
               ra8_xml_span_local_equal(source, length, event.name, "ol")) {
      enabled = true;
    } else if (active && enabled && (event.kind == (uint8_t)k_ra8_xml_event_start) &&
               ra8_xml_span_local_equal(source, length, event.name, entry_local)) {
      if (count >= (uint16_t)k_ra8_epub_max_toc) {
        return k_ra8_err_no_mem;
      }
      ++count;
    } else if (active && (event.kind == (uint8_t)k_ra8_xml_event_end) &&
               (event.depth == selected->depth)) {
      return k_ra8_ok;
    }
  }
  return err;
}

/* see header for the documented contract. */
RA8_INTERNAL static void internal_ncx_event(priv_ncx_ctx_t* ctx, const ra8_xml_event_t* event)
{
  if (!ctx->active && (event->kind == (uint8_t)k_ra8_xml_event_start) &&
      (event->markup.offset == ctx->selected.markup.offset)) {
    ctx->active = true;
  } else if (ctx->active && (event->kind == (uint8_t)k_ra8_xml_event_start) &&
             ra8_xml_span_local_equal(ctx->source, ctx->source_len, event->name, "navPoint")) {
    (void)
      internal_toc_reserve(ctx->source, ctx->source_len, event, ctx->reader, ctx->book, "navPoint");
  } else if (ctx->active && (event->kind == (uint8_t)k_ra8_xml_event_start) &&
             ra8_xml_span_local_equal(ctx->source, ctx->source_len, event->name, "content")) {
    const uint16_t    marker = internal_toc_marker(ctx->reader, event->depth);
    const priv_attr_t src    = internal_attr(ctx->source, ctx->source_len, event, "src");
    if ((marker != 0U) && src.present) {
      internal_copy(ctx->source,
                    ctx->source_len,
                    src.span,
                    ctx->book->toc[marker - 1U].href,
                    k_ra8_epub_max_path_len);
    }
  } else if ((event->kind == (uint8_t)k_ra8_xml_event_text) && (event->depth >= 3U) &&
             ra8_xml_span_local_equal(ctx->source,
                                      ctx->source_len,
                                      internal_ancestor_name(ctx->reader, event->depth, 1U),
                                      "text") &&
             ra8_xml_span_local_equal(ctx->source,
                                      ctx->source_len,
                                      internal_ancestor_name(ctx->reader, event->depth, 2U),
                                      "navLabel")) {
    uint16_t       nav_label = 0U;
    const bool     found     = internal_ancestor_frame(event->depth, 2U, &nav_label);
    const uint16_t marker    = found ? internal_toc_marker(ctx->reader, nav_label) : 0U;
    if ((marker != 0U) && (ctx->book->toc[marker - 1U].title[0] == '\0')) {
      internal_copy(ctx->source,
                    ctx->source_len,
                    event->markup,
                    ctx->book->toc[marker - 1U].title,
                    k_ra8_epub_meta_len);
    }
  } else if ((event->kind == (uint8_t)k_ra8_xml_event_end) && ctx->active &&
             (event->depth == ctx->selected.depth)) {
    ctx->active = false;
  }
}

ra8_err_t
priv_ra8_epub_xml_parse_ncx(const uint8_t* xml_bytes, size_t xml_len, ra8_epub_book_t* book)
{
  if ((xml_bytes == nullptr) || (book == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (xml_len == 0U) {
    return k_ra8_err_invalid_size;
  }
  ra8_err_t       err      = ra8_xml_validate(xml_bytes, xml_len, &book->xml_workspace.reader);
  ra8_xml_event_t selected = {};
  if (err == k_ra8_ok) {
    err =
      internal_find(xml_bytes, xml_len, &book->xml_workspace, "navMap", false, 0U, 0U, &selected);
  }
  if (err == k_ra8_ok) {
    err =
      internal_toc_capacity(xml_bytes, xml_len, &book->xml_workspace, &selected, "navPoint", false);
  }
  if (err != k_ra8_ok) {
    return err;
  }
  book->toc_count         = 0U;
  ra8_xml_reader_t reader = {};
  err                     = internal_reader(&reader, xml_bytes, xml_len, &book->xml_workspace);
  priv_ncx_ctx_t ctx      = {.source     = xml_bytes,
                             .source_len = xml_len,
                             .book       = book,
                             .reader     = &reader,
                             .selected   = selected};
  while (err == k_ra8_ok) {
    ra8_xml_event_t event = {};
    err                   = ra8_xml_reader_next(&reader, &event);
    // mcdc-deactivated: priv_ra8_epub_xml_parse_ncx pull-loop status gate; the public entry point validated this exact byte range with ra8_xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which ra8_xml never reads back -- so ra8_xml_reader_next() cannot fail here and only the end-of-document condition varies.
    if ((err != k_ra8_ok) || (event.kind == (uint8_t)k_ra8_xml_event_none)) {
      break;
    }
    internal_ncx_event(&ctx, &event);
  }
  return err;
}

/* see header for the documented contract. */
RA8_INTERNAL static ra8_err_t internal_select_nav(const uint8_t*            source,
                                                  size_t                    length,
                                                  ra8_epub_xml_workspace_t* workspace,
                                                  ra8_xml_event_t*          out)
{
  ra8_xml_reader_t reader   = {};
  ra8_err_t        err      = internal_reader(&reader, source, length, workspace);
  ra8_xml_event_t  fallback = {};
  while (err == k_ra8_ok) {
    ra8_xml_event_t event = {};
    err                   = ra8_xml_reader_next(&reader, &event);
    // mcdc-deactivated: internal_select_nav pull-loop status gate; the public entry point validated this exact byte range with ra8_xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which ra8_xml never reads back -- so ra8_xml_reader_next() cannot fail here and only the end-of-document condition varies.
    if ((err != k_ra8_ok) || (event.kind == (uint8_t)k_ra8_xml_event_none)) {
      break;
    }
    if ((event.kind == (uint8_t)k_ra8_xml_event_start) && (event.depth > 0U) &&
        ra8_xml_span_local_equal(source, length, event.name, "nav")) {
      if (fallback.kind == (uint8_t)k_ra8_xml_event_none) {
        fallback = event;
      }
      if (internal_attr_contains(source,
                                 length,
                                 internal_attr(source, length, &event, "epub:type"),
                                 "toc")) {
        *out = event;
        return k_ra8_ok;
      }
    }
  }
  // mcdc-deactivated: internal_select_nav fallback gate; the public entry point validated this exact byte range with ra8_xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which ra8_xml never reads back -- so ra8_xml_reader_next() cannot fail here, so the loop can only exit at end-of-document with `err` equal to k_ra8_ok and the status condition is constant-true.
  if ((err == k_ra8_ok) && (fallback.kind != (uint8_t)k_ra8_xml_event_none)) {
    *out = fallback;
    return k_ra8_ok;
  }
  return (err == k_ra8_ok) ? k_ra8_err_validation_failed : err;
}

/* see header for the documented contract. */
RA8_INTERNAL static ra8_err_t internal_nav_has_list(const uint8_t*            source,
                                                    size_t                    length,
                                                    ra8_epub_xml_workspace_t* workspace,
                                                    const ra8_xml_event_t*    selected)
{
  ra8_xml_reader_t reader = {};
  ra8_err_t        err    = internal_reader(&reader, source, length, workspace);
  bool             active = false;
  while (err == k_ra8_ok) {
    ra8_xml_event_t event = {};
    err                   = ra8_xml_reader_next(&reader, &event);
    // mcdc-deactivated: internal_nav_has_list pull-loop status gate; the public entry point validated this exact byte range with ra8_xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which ra8_xml never reads back -- so ra8_xml_reader_next() cannot fail here and only the end-of-document condition varies.
    if ((err != k_ra8_ok) || (event.kind == (uint8_t)k_ra8_xml_event_none)) {
      break;
    }
    if (!active && (event.kind == (uint8_t)k_ra8_xml_event_start) &&
        (event.markup.offset == selected->markup.offset)) {
      active = true;
    } else if (active && (event.kind == (uint8_t)k_ra8_xml_event_start) &&
               internal_direct_child(event.depth, selected->depth) &&
               ra8_xml_span_local_equal(source, length, event.name, "ol")) {
      return k_ra8_ok;
    } else if (active && (event.kind == (uint8_t)k_ra8_xml_event_end) &&
               (event.depth == selected->depth)) {
      return k_ra8_err_validation_failed;
    }
  }
  return (err == k_ra8_ok) ? k_ra8_err_validation_failed : err;
}

/* see header for the documented contract. */
RA8_INTERNAL static void internal_nav_event(priv_nav_ctx_t* ctx, const ra8_xml_event_t* event)
{
  if (!ctx->active && (event->kind == (uint8_t)k_ra8_xml_event_start) &&
      (event->markup.offset == ctx->selected.markup.offset)) {
    ctx->active = true;
    return;
  }
  if (!ctx->active) {
    return;
  }
  if (event->kind == (uint8_t)k_ra8_xml_event_start) {
    if (internal_direct_child(event->depth, ctx->selected.depth) &&
        ra8_xml_span_local_equal(ctx->source, ctx->source_len, event->name, "ol")) {
      ctx->saw_ol = true;
    } else if (ctx->saw_ol &&
               ra8_xml_span_local_equal(ctx->source, ctx->source_len, event->name, "li")) {
      (void)internal_toc_reserve(ctx->source, ctx->source_len, event, ctx->reader, ctx->book, "li");
    } else if (ctx->saw_ol &&
               (ra8_xml_span_local_equal(ctx->source, ctx->source_len, event->name, "a") ||
                ra8_xml_span_local_equal(ctx->source, ctx->source_len, event->name, "span"))) {
      const uint16_t marker = internal_toc_marker(ctx->reader, event->depth);
      if ((marker != 0U) && (event->self_closing == 0U)) {
        ctx->reader->workspace->frames[event->depth].consumer = marker;
        if (ra8_xml_span_local_equal(ctx->source, ctx->source_len, event->name, "a")) {
          const priv_attr_t href = internal_attr(ctx->source, ctx->source_len, event, "href");
          if (href.present) {
            internal_copy(ctx->source,
                          ctx->source_len,
                          href.span,
                          ctx->book->toc[marker - 1U].href,
                          k_ra8_epub_max_path_len);
          }
          ctx->book->toc[marker - 1U].title[0] = '\0';
        }
      }
    }
    // mcdc-deactivated: internal_nav_event text-depth gate; this arm is reached only while ctx->active, which spans the subtree of a <nav> that internal_select_nav selected at depth >= 1, so every event inside it sits at depth > 0 and the depth condition is constant-true.
  } else if ((event->kind == (uint8_t)k_ra8_xml_event_text) && (event->depth > 0U)) {
    const ra8_xml_span_t parent = internal_ancestor_name(ctx->reader, event->depth, 1U);
    const uint16_t       marker = internal_ancestor_marker(ctx->reader, event->depth, 1U);
    if ((ra8_xml_span_local_equal(ctx->source, ctx->source_len, parent, "a") ||
         ra8_xml_span_local_equal(ctx->source, ctx->source_len, parent, "span")) &&
        (marker != 0U) && (ctx->book->toc[marker - 1U].title[0] == '\0')) {
      internal_copy(ctx->source,
                    ctx->source_len,
                    event->markup,
                    ctx->book->toc[marker - 1U].title,
                    k_ra8_epub_meta_len);
    }
  } else if ((event->kind == (uint8_t)k_ra8_xml_event_end) &&
             (event->depth == ctx->selected.depth)) {
    ctx->active = false;
  }
}

ra8_err_t
priv_ra8_epub_xml_parse_nav(const uint8_t* xml_bytes, size_t xml_len, ra8_epub_book_t* book)
{
  if ((xml_bytes == nullptr) || (book == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (xml_len == 0U) {
    return k_ra8_err_invalid_size;
  }
  ra8_err_t       err      = ra8_xml_validate(xml_bytes, xml_len, &book->xml_workspace.reader);
  ra8_xml_event_t selected = {};
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
  book->toc_count         = 0U;
  ra8_xml_reader_t reader = {};
  err                     = internal_reader(&reader, xml_bytes, xml_len, &book->xml_workspace);
  priv_nav_ctx_t ctx      = {.source     = xml_bytes,
                             .source_len = xml_len,
                             .book       = book,
                             .reader     = &reader,
                             .selected   = selected};
  while (err == k_ra8_ok) {
    ra8_xml_event_t event = {};
    err                   = ra8_xml_reader_next(&reader, &event);
    // mcdc-deactivated: priv_ra8_epub_xml_parse_nav pull-loop status gate; the public entry point validated this exact byte range with ra8_xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which ra8_xml never reads back -- so ra8_xml_reader_next() cannot fail here and only the end-of-document condition varies.
    if ((err != k_ra8_ok) || (event.kind == (uint8_t)k_ra8_xml_event_none)) {
      break;
    }
    internal_nav_event(&ctx, &event);
  }
  // mcdc-deactivated: priv_ra8_epub_xml_parse_nav completion gate; this point is reached only after internal_nav_has_list accepted the same bytes with the identical direct-child <ol> predicate that sets ctx.saw_ol in internal_nav_event, so saw_ol is always true, and the public entry point validated this exact byte range with ra8_xml_validate() before this re-parse and the bounded pull reader is a pure function of (source, source_len, workspace) -- the shim only ever writes the frame `consumer` field, which ra8_xml never reads back -- so ra8_xml_reader_next() cannot fail here.
  if ((err != k_ra8_ok) || !ctx.saw_ol) {
    return (err == k_ra8_ok) ? k_ra8_err_validation_failed : err;
  }
  return k_ra8_ok;
}
