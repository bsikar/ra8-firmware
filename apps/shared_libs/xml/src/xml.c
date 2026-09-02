/**
 * @file xml.c
 * @brief Bounded no-heap XML pull-reader implementation.
 * @ingroup grp_ereader
 *
 * @details Implements strict UTF-8/XML 1.0 lexical validation, bounded entity
 * decoding, attribute traversal, and source-aliasing pull events using only
 * caller-owned stack storage.
 *
 * [Ring 3 / LIB] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "xml.h"

#include <limits.h>
#include <string.h>

#include "ra8_attributes.h"
#include "xml_internal.h"
#include "xml_reader_internal.h"

RA8_INTERNAL static bool internal_space(uint8_t c)
{
  return (c == (uint8_t)' ') || (c == (uint8_t)'\t') || (c == (uint8_t)'\n') ||
         (c == (uint8_t)'\r');
}

RA8_INTERNAL static bool internal_ascii_letter(uint8_t c)
{
  return ((c >= (uint8_t)'A') && (c <= (uint8_t)'Z')) ||
         ((c >= (uint8_t)'a') && (c <= (uint8_t)'z'));
}

RA8_INTERNAL static bool internal_name_start(uint8_t c)
{
  return internal_ascii_letter(c) || (c == (uint8_t)'_');
}

RA8_INTERNAL static bool internal_name_continue(uint8_t c)
{
  return internal_name_start(c) || ((c >= (uint8_t)'0') && (c <= (uint8_t)'9')) ||
         (c == (uint8_t)'.') || (c == (uint8_t)'-');
}

ra8_err_t priv_xml_qname(const uint8_t* source, size_t end, size_t start, size_t* out_end)
{
  if ((start >= end) || !internal_name_start(source[start])) {
    return k_ra8_err_validation_failed;
  }
  size_t cursor    = start + 1U;
  bool   saw_colon = false;
  while (cursor < end) {
    const uint8_t byte = source[cursor];
    if (internal_name_continue(byte)) {
      ++cursor;
    } else if ((byte == (uint8_t)':') && !saw_colon && ((cursor + 1U) < end) &&
               internal_name_start(source[cursor + 1U])) {
      saw_colon = true;
      cursor += 2U;
    } else {
      break;
    }
  }
  *out_end = cursor;
  return k_ra8_ok;
}

void xml_attr_begin(const xml_event_t* event, xml_attr_cursor_t* cursor)
{
  if ((event != nullptr) && (cursor != nullptr)) {
    cursor->position = event->name.offset + event->name.length;
    cursor->emitted  = 0U;
  }
}

/**
 * @brief Parse the equals sign, quote, and value of one XML attribute.
 * @details Skips allowed XML whitespace, requires a quoted value, rejects a
 *          literal less-than sign, and advances the caller past the closing
 *          quote.
 * @param[in] source Complete immutable XML source.
 * @param[in] end Exclusive end of the current start-tag payload.
 * @param[in,out] position Cursor just past the attribute name.
 * @param[out] out Attribute whose value span receives the parsed range.
 * @return Attribute-value parse status.
 * @retval k_ra8_ok A quoted value was parsed and @p position advanced.
 * @retval k_ra8_err_validation_failed The separator or value is malformed.
 * @pre @p source, @p position, and @p out are non-NULL.
 * @pre `*position <= end` and @p source is readable through @p end.
 * @post Success initializes `out->value` and advances @p position.
 * @post Failure does not advance the caller-owned @p position.
 * @note Pure except for the documented output fields.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_attr_value(const uint8_t* source, uint32_t end, uint32_t* position, xml_attribute_t* out)
{
  uint32_t cursor = *position;
  while ((cursor < end) && internal_space(source[cursor])) {
    ++cursor;
  }
  if ((cursor >= end) || (source[cursor] != (uint8_t)'=')) {
    return k_ra8_err_validation_failed;
  }
  ++cursor;
  while ((cursor < end) && internal_space(source[cursor])) {
    ++cursor;
  }
  if ((cursor >= end) || ((source[cursor] != (uint8_t)'\'') && (source[cursor] != (uint8_t)'"'))) {
    return k_ra8_err_validation_failed;
  }
  const uint8_t quote = source[cursor];
  ++cursor;
  const uint32_t value_start = cursor;
  while ((cursor < end) && (source[cursor] != quote)) {
    if (source[cursor] == (uint8_t)'<') {
      return k_ra8_err_validation_failed;
    }
    ++cursor;
  }
  if (cursor >= end) {
    return k_ra8_err_validation_failed;
  }
  out->value = (xml_span_t){value_start, cursor - value_start};
  *position  = cursor + 1U;
  return k_ra8_ok;
}

RA8_INTERNAL static ra8_err_t internal_attr_parse(const uint8_t*     source,
                                                  size_t             source_len,
                                                  uint32_t           end,
                                                  xml_attr_cursor_t* cursor,
                                                  xml_attribute_t*   out)
{
  uint32_t position = cursor->position;
  while ((position < end) && internal_space(source[position])) {
    ++position;
  }
  const uint32_t name_start = position;
  size_t         name_end   = 0U;
  if (priv_xml_qname(source, end, name_start, &name_end) != k_ra8_ok) {
    return k_ra8_err_validation_failed;
  }
  position      = (uint32_t)name_end;
  out->name     = (xml_span_t){name_start, position - name_start};
  ra8_err_t err = internal_attr_value(source, end, &position, out);
  if (err == k_ra8_ok) {
    cursor->position = position;
    ++cursor->emitted;
    size_t decoded = 0U;
    err            = xml_decoded_size(source, source_len, out->value, &decoded);
  }
  return err;
}

RA8_INTERNAL static bool internal_attr_duplicate(const uint8_t*         source,
                                                 size_t                 source_len,
                                                 const xml_event_t*     event,
                                                 const xml_attribute_t* current,
                                                 uint16_t               prior_count)
{
  xml_attr_cursor_t prior = {};
  xml_attr_begin(event, &prior);
  const uint32_t end = event->markup.offset + event->markup.length - 1U;
  for (uint16_t i = 0U; i < prior_count; ++i) {
    xml_attribute_t attribute = {};
    if (internal_attr_parse(source, source_len, end, &prior, &attribute) != k_ra8_ok) {
      return true;
    }
    if ((attribute.name.length == current->name.length) && (memcmp(&source[attribute.name.offset],
                                                                   &source[current->name.offset],
                                                                   current->name.length) == 0)) {
      return true;
    }
  }
  return false;
}

ra8_err_t xml_attr_next(const uint8_t*     source,
                        size_t             source_len,
                        const xml_event_t* event,
                        xml_attr_cursor_t* cursor,
                        xml_attribute_t*   out_attribute,
                        bool*              out_has_value)
{
  if ((source == nullptr) || (event == nullptr) || (cursor == nullptr) ||
      (out_attribute == nullptr) || (out_has_value == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (cursor->emitted >= event->attribute_count) {
    *out_has_value = false;
    return k_ra8_ok;
  }
  if (!priv_xml_span_valid(source_len, event->markup) ||
      !priv_xml_span_valid(source_len, event->name) || (event->markup.length == 0U)) {
    return k_ra8_err_validation_failed;
  }
  const uint32_t  end = event->markup.offset + event->markup.length - 1U;
  const ra8_err_t err = internal_attr_parse(source, source_len, end, cursor, out_attribute);
  *out_has_value      = err == k_ra8_ok;
  return err;
}

RA8_INTERNAL static ra8_err_t
internal_markup_end(const uint8_t* source, size_t length, size_t start, size_t* out_end)
{
  uint8_t quote = 0U;
  for (size_t i = start + 1U; i < length; ++i) {
    const uint8_t byte = source[i];
    if (quote != 0U) {
      if (byte == quote) {
        quote = 0U;
      }
      continue;
    }
    if ((byte == (uint8_t)'\'') || (byte == (uint8_t)'"')) {
      quote = byte;
      continue;
    }
    if (byte == (uint8_t)'>') {
      *out_end = i;
      return k_ra8_ok;
    }
  }
  return k_ra8_err_validation_failed;
}

RA8_INTERNAL static ra8_err_t
internal_attributes(const uint8_t* source, size_t source_len, xml_event_t* event)
{
  xml_attr_cursor_t cursor = {};
  xml_attr_begin(event, &cursor);
  const uint32_t end   = event->markup.offset + event->markup.length - 1U;
  uint16_t       count = 0U;
  for (;;) {
    uint32_t position = cursor.position;
    while ((position < end) && internal_space(source[position])) {
      ++position;
    }
    if ((position >= end) || (source[position] == (uint8_t)'/')) {
      cursor.position = position;
      break;
    }
    xml_attribute_t attribute = {};
    const ra8_err_t err       = internal_attr_parse(source, source_len, end, &cursor, &attribute);
    // mcdc-deactivated: internal_attributes attribute-count saturation guard; it exists to stop `count` wrapping the uint16_t event->attribute_count, and flipping it needs 65535 mutually distinct attributes on ONE element while internal_attr_duplicate re-parses every prior attribute per new one -- a quadratic ~2.1e9-parse input that no bounded unit test can drive.
    if ((err != k_ra8_ok) || (count == UINT16_MAX) ||
        internal_attr_duplicate(source, source_len, event, &attribute, count)) {
      return k_ra8_err_validation_failed;
    }
    ++count;
  }
  if (event->self_closing != 0U) {
    // mcdc-deactivated: internal_attributes self-closing slash recheck; the attribute loop above breaks only on `position >= end` or `source[position] == '/'`, and self_closing was set from a non-space '/' that lies inside the still-unscanned range, so the loop necessarily stopped on that slash -- both conditions are constant-false here.
    if ((cursor.position >= end) || (source[cursor.position] != (uint8_t)'/')) {
      return k_ra8_err_validation_failed;
    }
    ++cursor.position;
  }
  while ((cursor.position < end) && internal_space(source[cursor.position])) {
    ++cursor.position;
  }
  if (cursor.position != end) {
    return k_ra8_err_validation_failed;
  }
  event->attribute_count = count;
  return k_ra8_ok;
}

RA8_INTERNAL static ra8_err_t internal_start(xml_reader_t* reader, size_t end, xml_event_t* event)
{
  size_t       cursor     = reader->position + 1U;
  const size_t name_start = cursor;
  if ((priv_xml_qname(reader->source, end, name_start, &cursor) != k_ra8_ok) ||
      (reader->root_closed != 0U) ||
      ((size_t)reader->stack_size >= (size_t)k_xml_max_element_depth)) {
    return k_ra8_err_validation_failed;
  }
  event->kind   = (uint8_t)k_xml_event_start;
  event->depth  = reader->stack_size;
  event->name   = (xml_span_t){(uint32_t)name_start, (uint32_t)(cursor - name_start)};
  event->markup = (xml_span_t){(uint32_t)reader->position, (uint32_t)(end - reader->position + 1U)};
  size_t tail   = end;
  while ((tail > cursor) && internal_space(reader->source[tail - 1U])) {
    --tail;
  }
  event->self_closing = ((tail > cursor) && (reader->source[tail - 1U] == (uint8_t)'/')) ? 1U : 0U;
  const ra8_err_t attr_err = internal_attributes(reader->source, reader->source_len, event);
  if (attr_err != k_ra8_ok) {
    return attr_err;
  }
  if (reader->stack_size == 0U) {
    if (reader->root_count != 0U) {
      return k_ra8_err_validation_failed;
    }
    reader->root_count = 1U;
  }
  if (event->self_closing != 0U) {
    if (reader->stack_size == 0U) {
      reader->root_closed = 1U;
    }
  } else {
    xml_frame_t* frame = &reader->workspace->frames[reader->stack_size];
    reader->stack_size += 1U;
    *frame = (xml_frame_t){event->name.offset, (uint16_t)event->name.length, 0U};
  }
  reader->position = end + 1U;
  return k_ra8_ok;
}

RA8_INTERNAL static ra8_err_t internal_end(xml_reader_t* reader, size_t end, xml_event_t* event)
{
  size_t       cursor     = reader->position + 2U;
  const size_t name_start = cursor;
  if (priv_xml_qname(reader->source, end, name_start, &cursor) != k_ra8_ok) {
    return k_ra8_err_validation_failed;
  }
  const size_t name_end = cursor;
  while ((cursor < end) && internal_space(reader->source[cursor])) {
    ++cursor;
  }
  if ((cursor != end) || (reader->stack_size == 0U)) {
    return k_ra8_err_validation_failed;
  }
  const xml_frame_t* frame       = &reader->workspace->frames[reader->stack_size - 1U];
  const size_t       name_length = name_end - name_start;
  if ((name_length != frame->name_length) ||
      (memcmp(&reader->source[name_start], &reader->source[frame->name_offset], name_length) !=
       0)) {
    return k_ra8_err_validation_failed;
  }
  --reader->stack_size;
  event->kind   = (uint8_t)k_xml_event_end;
  event->depth  = reader->stack_size;
  event->name   = (xml_span_t){(uint32_t)name_start, (uint32_t)name_length};
  event->markup = (xml_span_t){(uint32_t)reader->position, (uint32_t)(end - reader->position + 1U)};
  if (reader->stack_size == 0U) {
    reader->root_closed = 1U;
  }
  reader->position = end + 1U;
  return k_ra8_ok;
}

RA8_INTERNAL static ra8_err_t internal_terminator(const uint8_t* source,
                                                  size_t         length,
                                                  size_t         start,
                                                  const char*    terminator,
                                                  size_t*        out_start)
{
  const size_t term_len = strlen(terminator);
  for (size_t i = start; (i + term_len) <= length; ++i) {
    if (priv_xml_bytes_equal(source, i, terminator, term_len)) {
      *out_start = i;
      return k_ra8_ok;
    }
  }
  return k_ra8_err_validation_failed;
}

RA8_INTERNAL static ra8_err_t internal_comment(xml_reader_t* reader)
{
  const size_t content = reader->position + 4U;
  size_t       term    = 0U;
  ra8_err_t    err = internal_terminator(reader->source, reader->source_len, content, "-->", &term);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((term > content) && (reader->source[term - 1U] == (uint8_t)'-')) {
    return k_ra8_err_validation_failed;
  }
  for (size_t i = content; (i + 1U) < term; ++i) {
    if ((reader->source[i] == (uint8_t)'-') && (reader->source[i + 1U] == (uint8_t)'-')) {
      return k_ra8_err_validation_failed;
    }
  }
  err = priv_xml_raw(reader->source, content, term);
  if (err == k_ra8_ok) {
    reader->position = term + 3U;
  }
  return err;
}

RA8_INTERNAL static bool internal_xml_target(const uint8_t* source, size_t start, size_t end)
{
  return ((end - start) == 3U) && ((source[start] | 0x20U) == (uint8_t)'x') &&
         ((source[start + 1U] | 0x20U) == (uint8_t)'m') &&
         ((source[start + 2U] | 0x20U) == (uint8_t)'l');
}

RA8_INTERNAL static bool internal_encoding(const uint8_t* source, xml_span_t value)
{
  return (value.length == k_priv_xml_encoding_bytes) &&
         (priv_xml_bytes_equal(source, value.offset, "UTF-8", k_priv_xml_encoding_bytes) ||
          priv_xml_bytes_equal(source, value.offset, "utf-8", k_priv_xml_encoding_bytes));
}

RA8_INTERNAL static bool internal_declaration_attr(const uint8_t*         source,
                                                   size_t                 source_len,
                                                   const xml_attribute_t* attribute,
                                                   uint16_t               ordinal,
                                                   bool*                  saw_encoding,
                                                   bool*                  saw_standalone)
{
  if (ordinal == 0U) {
    return xml_span_equal(source, source_len, attribute->name, "version") &&
           xml_span_equal(source, source_len, attribute->value, "1.0");
  }
  if (!*saw_encoding && !*saw_standalone &&
      xml_span_equal(source, source_len, attribute->name, "encoding")) {
    *saw_encoding = true;
    return internal_encoding(source, attribute->value);
  }
  if (!*saw_standalone && xml_span_equal(source, source_len, attribute->name, "standalone")) {
    *saw_standalone = true;
    return xml_span_equal(source, source_len, attribute->value, "yes") ||
           xml_span_equal(source, source_len, attribute->value, "no");
  }
  return false;
}

RA8_INTERNAL static ra8_err_t
internal_declaration(xml_reader_t* reader, size_t target_end, size_t term)
{
  const bool initial = (reader->position == 0U) ||
                       ((reader->position == 3U) && (reader->source[0] == k_priv_utf8_bom_first));
  // mcdc-deactivated: internal_declaration placement gate; `initial` is only true at offset 0 or at offset 3 behind a BOM, and both declaration_seen and root_count are set by passes that leave reader->position beyond those offsets, so conditions 2 and 3 cannot be true while condition 1 is false. Condition 5 is likewise constant: internal_pi has already rejected `(target_end < term) && !internal_space(source[target_end])`, so reaching here with `target_end < term` implies the byte IS spacing. Only the `target_end >= term` condition varies.
  if (!initial || (reader->declaration_seen != 0U) || (reader->root_count != 0U) ||
      (target_end >= term) || !internal_space(reader->source[target_end])) {
    return k_ra8_err_validation_failed;
  }
  xml_attr_cursor_t cursor         = {.position = (uint32_t)target_end};
  bool              saw_encoding   = false;
  bool              saw_standalone = false;
  uint16_t          count          = 0U;
  while (cursor.position < term) {
    while ((cursor.position < term) && internal_space(reader->source[cursor.position])) {
      ++cursor.position;
    }
    if (cursor.position == term) {
      break;
    }
    xml_attribute_t attribute = {};
    const ra8_err_t err =
      internal_attr_parse(reader->source, reader->source_len, (uint32_t)term, &cursor, &attribute);
    if ((err != k_ra8_ok) || !internal_declaration_attr(reader->source,
                                                        reader->source_len,
                                                        &attribute,
                                                        count,
                                                        &saw_encoding,
                                                        &saw_standalone)) {
      return k_ra8_err_validation_failed;
    }
    ++count;
  }
  if (count == 0U) {
    return k_ra8_err_validation_failed;
  }
  reader->declaration_seen = 1U;
  reader->position         = term + 2U;
  return k_ra8_ok;
}

RA8_INTERNAL static ra8_err_t internal_pi(xml_reader_t* reader)
{
  const size_t target_start = reader->position + 2U;
  size_t       term         = 0U;
  ra8_err_t    err =
    internal_terminator(reader->source, reader->source_len, target_start, "?>", &term);
  if (err != k_ra8_ok) {
    return err;
  }
  size_t target_end = 0U;
  err               = priv_xml_qname(reader->source, term, target_start, &target_end);
  if ((err != k_ra8_ok) || ((target_end < term) && !internal_space(reader->source[target_end]))) {
    return k_ra8_err_validation_failed;
  }
  if (internal_xml_target(reader->source, target_start, target_end)) {
    if (!priv_xml_bytes_equal(reader->source, target_start, "xml", 3U)) {
      return k_ra8_err_validation_failed;
    }
    return internal_declaration(reader, target_end, term);
  }
  err = priv_xml_raw(reader->source, target_end, term);
  if (err == k_ra8_ok) {
    reader->position = term + 2U;
  }
  return err;
}

RA8_INTERNAL static ra8_err_t internal_cdata(xml_reader_t* reader, xml_event_t* event)
{
  if (reader->stack_size == 0U) {
    return k_ra8_err_validation_failed;
  }
  const size_t content = reader->position + k_priv_xml_cdata_open_bytes;
  size_t       term    = 0U;
  ra8_err_t    err = internal_terminator(reader->source, reader->source_len, content, "]]>", &term);
  if (err != k_ra8_ok) {
    return err;
  }
  err = priv_xml_raw(reader->source, content, term);
  if (err != k_ra8_ok) {
    return err;
  }
  event->kind      = (uint8_t)k_xml_event_cdata;
  event->depth     = reader->stack_size;
  event->markup    = (xml_span_t){(uint32_t)content, (uint32_t)(term - content)};
  reader->position = term + 3U;
  return k_ra8_ok;
}

RA8_INTERNAL static ra8_err_t
internal_special(xml_reader_t* reader, xml_event_t* event, bool* out_emitted)
{
  const size_t pos = reader->position;
  *out_emitted     = false;
  if (((pos + 4U) <= reader->source_len) && priv_xml_bytes_equal(reader->source, pos, "<!--", 4U)) {
    return internal_comment(reader);
  }
  if (((pos + k_priv_xml_cdata_open_bytes) <= reader->source_len) &&
      priv_xml_bytes_equal(reader->source, pos, "<![CDATA[", k_priv_xml_cdata_open_bytes)) {
    const ra8_err_t err = internal_cdata(reader, event);
    *out_emitted        = err == k_ra8_ok;
    return err;
  }
  if (((pos + k_priv_xml_doctype_open_bytes) <= reader->source_len) &&
      priv_xml_bytes_equal(reader->source, pos, "<!DOCTYPE", k_priv_xml_doctype_open_bytes)) {
    return priv_xml_doctype(reader);
  }
  // mcdc-deactivated: internal_special processing-instruction probe; xml_reader_next rejects `(position + 1U) >= source_len` before dispatching here, so `(pos + 2U) <= source_len` holds on every reachable call and only the '?' byte test varies.
  if (((pos + 2U) <= reader->source_len) && (reader->source[pos + 1U] == (uint8_t)'?')) {
    return internal_pi(reader);
  }
  return k_ra8_err_validation_failed;
}

RA8_INTERNAL static ra8_err_t internal_text(xml_reader_t* reader, xml_event_t* event)
{
  const size_t start = reader->position;
  while ((reader->position < reader->source_len) &&
         (reader->source[reader->position] != (uint8_t)'<')) {
    ++reader->position;
  }
  const xml_span_t span    = {(uint32_t)start, (uint32_t)(reader->position - start)};
  size_t           decoded = 0U;
  const ra8_err_t  err     = xml_decoded_size(reader->source, reader->source_len, span, &decoded);
  if (err != k_ra8_ok) {
    return err;
  }
  for (size_t i = start; (i + 2U) < reader->position; ++i) {
    if ((reader->source[i] == (uint8_t)']') && (reader->source[i + 1U] == (uint8_t)']') &&
        (reader->source[i + 2U] == (uint8_t)'>')) {
      return k_ra8_err_validation_failed;
    }
  }
  if (reader->stack_size == 0U) {
    for (size_t i = start; i < reader->position; ++i) {
      if (!internal_space(reader->source[i])) {
        return k_ra8_err_validation_failed;
      }
    }
  }
  event->kind   = (uint8_t)k_xml_event_text;
  event->depth  = reader->stack_size;
  event->markup = span;
  return k_ra8_ok;
}

ra8_err_t xml_reader_init(xml_reader_t*    reader,
                          const uint8_t*   source,
                          size_t           source_len,
                          xml_workspace_t* workspace)
{
  if ((reader == nullptr) || (source == nullptr) || (workspace == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((source_len == 0U) || (source_len > UINT32_MAX)) {
    return k_ra8_err_invalid_size;
  }
  *reader = (xml_reader_t){.source = source, .source_len = source_len, .workspace = workspace};
  if ((source_len >= 3U) && (source[0] == k_priv_utf8_bom_first) &&
      (source[1] == k_priv_utf8_bom_second) && (source[2] == k_priv_utf8_bom_third)) {
    reader->position = 3U;
  }
  return k_ra8_ok;
}

ra8_err_t xml_reader_next(xml_reader_t* reader, xml_event_t* out_event)
{
  if ((reader == nullptr) || (out_event == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out_event = (xml_event_t){};
  while (reader->position < reader->source_len) {
    if (reader->source[reader->position] != (uint8_t)'<') {
      return internal_text(reader, out_event);
    }
    if ((reader->position + 1U) >= reader->source_len) {
      return k_ra8_err_validation_failed;
    }
    const uint8_t next = reader->source[reader->position + 1U];
    if ((next == (uint8_t)'!') || (next == (uint8_t)'?')) {
      bool            emitted = false;
      const ra8_err_t err     = internal_special(reader, out_event, &emitted);
      if ((err != k_ra8_ok) || emitted) {
        return err;
      }
      continue;
    }
    size_t          end = 0U;
    const ra8_err_t scan_err =
      internal_markup_end(reader->source, reader->source_len, reader->position, &end);
    if (scan_err != k_ra8_ok) {
      return scan_err;
    }
    if (next == (uint8_t)'/') {
      return internal_end(reader, end, out_event);
    }
    return internal_start(reader, end, out_event);
  }
  // mcdc-deactivated: xml_reader_next end-of-document completeness gate; root_closed is set by the only two ways the element stack can drain back to zero after a root was started (a self-closing root in internal_start, a matching end tag in internal_end), so `stack_size == 0 && root_count == 1 && root_closed == 0` is unreachable and the third condition cannot flip independently.
  if ((reader->stack_size != 0U) || (reader->root_count != 1U) || (reader->root_closed == 0U)) {
    return k_ra8_err_validation_failed;
  }
  reader->finished = 1U;
  return k_ra8_ok;
}

ra8_err_t xml_validate(const uint8_t* source, size_t source_len, xml_workspace_t* workspace)
{
  xml_reader_t reader = {};
  ra8_err_t    err    = xml_reader_init(&reader, source, source_len, workspace);
  while (err == k_ra8_ok) {
    xml_event_t event = {};
    err               = xml_reader_next(&reader, &event);
    if ((err != k_ra8_ok) || (event.kind == (uint8_t)k_xml_event_none)) {
      break;
    }
  }
  return err;
}
