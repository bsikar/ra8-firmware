/**
 * @file test_ra8_xml.c
 * @brief Strict no-heap XML reader boundaries and ownership tests.
 *
 * @details Brackets workspace depth/capacity and exercises strict QName,
 * UTF-8, entity, declaration, DOCTYPE, BOM, span, and immutability contracts.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_xml.h"

/** @brief Build a document with exactly @p levels element levels. */
RA8_INTERNAL static size_t
internal_deep_document(uint8_t* destination, size_t capacity, uint16_t levels)
{
  size_t used = 0U;
  for (uint16_t i = 0U; i < levels; ++i) {
    assert((used + 3U) <= capacity);
    (void)memcpy(&destination[used], "<x>", 3U);
    used += 3U;
  }
  for (uint16_t i = 0U; i < levels; ++i) {
    assert((used + 4U) <= capacity);
    (void)memcpy(&destination[used], "</x>", 4U);
    used += 4U;
  }
  return used;
}

/** @brief Prove depth cap and cap-minus-one are deterministic. */
RA8_INTERNAL static void internal_depth_bound(void)
{
  static uint8_t      source[4096];
  ra8_xml_workspace_t workspace = {};
  size_t length = internal_deep_document(source, sizeof(source), k_ra8_xml_max_element_depth);
  assert(ra8_xml_validate(source, length, &workspace) == k_ra8_ok);
  length =
    internal_deep_document(source, sizeof(source), (uint16_t)(k_ra8_xml_max_element_depth + 1U));
  assert(ra8_xml_validate(source, length, &workspace) == k_ra8_err_validation_failed);
}

/** @brief Prove two readers do not share mutable parser state. */
RA8_INTERNAL static void internal_independent_readers(void)
{
  static const uint8_t left[]          = "<a><b x='1'/>left</a>";
  static const uint8_t right[]         = "<r><s/>right</r>";
  ra8_xml_workspace_t  left_workspace  = {};
  ra8_xml_workspace_t  right_workspace = {};
  ra8_xml_reader_t     left_reader     = {};
  ra8_xml_reader_t     right_reader    = {};
  ra8_xml_event_t      event           = {};
  assert(ra8_xml_reader_init(&left_reader, left, sizeof(left) - 1U, &left_workspace) == k_ra8_ok);
  assert(ra8_xml_reader_init(&right_reader, right, sizeof(right) - 1U, &right_workspace) ==
         k_ra8_ok);
  assert(ra8_xml_reader_next(&left_reader, &event) == k_ra8_ok);
  assert(ra8_xml_span_equal(left, sizeof(left) - 1U, event.name, "a"));
  assert(ra8_xml_reader_next(&right_reader, &event) == k_ra8_ok);
  assert(ra8_xml_span_equal(right, sizeof(right) - 1U, event.name, "r"));
  assert(ra8_xml_reader_next(&left_reader, &event) == k_ra8_ok);
  assert(ra8_xml_span_equal(left, sizeof(left) - 1U, event.name, "b"));
  assert(ra8_xml_reader_next(&right_reader, &event) == k_ra8_ok);
  assert(ra8_xml_span_equal(right, sizeof(right) - 1U, event.name, "s"));
  assert(left_workspace.frames[0].name_offset == 1U);
  assert(right_workspace.frames[0].name_offset == 1U);
}

/** @brief Prove entity decoding, attributes, malformed rejection, and immutability. */
RA8_INTERNAL static void internal_entities_and_faults(void)
{
  static const uint8_t source[]                 = "<r a='A&amp;&#x42;&#67;' b=\"q\"> x &lt; y </r>";
  uint8_t              snapshot[sizeof(source)] = {};
  (void)memcpy(snapshot, source, sizeof(source));
  ra8_xml_workspace_t workspace = {};
  assert(ra8_xml_validate(source, sizeof(source) - 1U, &workspace) == k_ra8_ok);
  assert(memcmp(snapshot, source, sizeof(source)) == 0);
  ra8_xml_reader_t reader = {};
  ra8_xml_event_t  event  = {};
  assert(ra8_xml_reader_init(&reader, source, sizeof(source) - 1U, &workspace) == k_ra8_ok);
  assert(ra8_xml_reader_next(&reader, &event) == k_ra8_ok);
  assert(event.attribute_count == 2U);
  ra8_xml_attr_cursor_t cursor    = {};
  ra8_xml_attribute_t   attribute = {};
  bool                  present   = false;
  ra8_xml_attr_begin(&event, &cursor);
  assert(ra8_xml_attr_next(source, sizeof(source) - 1U, &event, &cursor, &attribute, &present) ==
         k_ra8_ok);
  assert(present && ra8_xml_span_equal(source, sizeof(source) - 1U, attribute.name, "a"));
  char   decoded[8]  = {};
  size_t decoded_len = 0U;
  assert(ra8_xml_decode(source,
                        sizeof(source) - 1U,
                        attribute.value,
                        decoded,
                        sizeof(decoded),
                        &decoded_len) == k_ra8_ok);
  assert((decoded_len == 4U) && (strcmp(decoded, "A&BC") == 0));
  static const uint8_t mismatch[]   = "<a><b></a></b>";
  static const uint8_t bad_entity[] = "<a>&unknown;</a>";
  static const uint8_t duplicate[]  = "<a x='1' x='2'/>";
  assert(ra8_xml_validate(mismatch, sizeof(mismatch) - 1U, &workspace) ==
         k_ra8_err_validation_failed);
  assert(ra8_xml_validate(bad_entity, sizeof(bad_entity) - 1U, &workspace) ==
         k_ra8_err_validation_failed);
  assert(ra8_xml_validate(duplicate, sizeof(duplicate) - 1U, &workspace) ==
         k_ra8_err_validation_failed);
}

/** @brief Prove numeric entities compare equal to their UTF-8 bytes. */
RA8_INTERNAL static void internal_entity_comparison(void)
{
  static const uint8_t source[]  = "<r a='\xC3\xA9' b='&#xE9;'/>";
  ra8_xml_workspace_t  workspace = {};
  ra8_xml_reader_t     reader    = {};
  ra8_xml_event_t      event     = {};
  assert(ra8_xml_reader_init(&reader, source, sizeof(source) - 1U, &workspace) == k_ra8_ok);
  assert(ra8_xml_reader_next(&reader, &event) == k_ra8_ok);
  ra8_xml_attr_cursor_t cursor  = {};
  ra8_xml_attribute_t   left    = {};
  ra8_xml_attribute_t   right   = {};
  bool                  present = false;
  ra8_xml_attr_begin(&event, &cursor);
  assert(ra8_xml_attr_next(source, sizeof(source) - 1U, &event, &cursor, &left, &present) ==
           k_ra8_ok &&
         present);
  assert(ra8_xml_attr_next(source, sizeof(source) - 1U, &event, &cursor, &right, &present) ==
           k_ra8_ok &&
         present);
  assert(ra8_xml_decoded_equal(source, sizeof(source) - 1U, left.value, right.value));
}

/** @brief Assert one static byte document is rejected as malformed XML. */
RA8_INTERNAL static void internal_reject(const uint8_t* source, size_t length)
{
  ra8_xml_workspace_t workspace = {};
  assert(ra8_xml_validate(source, length, &workspace) == k_ra8_err_validation_failed);
}

/** @brief Prove strict names, characters, comments, declarations, and placement. */
RA8_INTERNAL static void internal_hostile_syntax(void)
{
  static const uint8_t close_space[]     = "<a></a >";
  static const uint8_t bad_start[]       = "<1a/>";
  static const uint8_t bad_dash_start[]  = "<-a/>";
  static const uint8_t bad_lead_colon[]  = "<:a/>";
  static const uint8_t bad_tail_colon[]  = "<a:/>";
  static const uint8_t bad_colon[]       = "<a:b:c/>";
  static const uint8_t good_colon[]      = "<a:b c:d='1'/>";
  static const uint8_t bad_entity[]      = "<a>&#1;</a>";
  static const uint8_t bad_zero[]        = "<a>&#0;</a>";
  static const uint8_t bad_cdata_close[] = "<a>]]></a>";
  static const uint8_t bad_comment[]     = "<a><!--x--y--></a>";
  static const uint8_t bad_comment_end[] = "<a><!--x---></a>";
  static const uint8_t bad_declaration[] = "<!--x--><?xml version='1.0'?><a/>";
  static const uint8_t control[]         = {'<', 'a', '>', 1U, '<', '/', 'a', '>'};
  static const uint8_t nul[]             = {'<', 'a', '>', 0U, '<', '/', 'a', '>'};
  static const uint8_t invalid_utf8[]    = {'<', 'a', '>', 0xC0U, 0x80U, '<', '/', 'a', '>'};
  static const uint8_t invalid_attr[] =
    {'<', 'a', ' ', 'x', '=', '\'', 0xC0U, 0x80U, '\'', '/', '>'};
  ra8_xml_workspace_t workspace = {};
  assert(ra8_xml_validate(close_space, sizeof(close_space) - 1U, &workspace) == k_ra8_ok);
  assert(ra8_xml_validate(good_colon, sizeof(good_colon) - 1U, &workspace) == k_ra8_ok);
  internal_reject(bad_start, sizeof(bad_start) - 1U);
  internal_reject(bad_dash_start, sizeof(bad_dash_start) - 1U);
  internal_reject(bad_lead_colon, sizeof(bad_lead_colon) - 1U);
  internal_reject(bad_tail_colon, sizeof(bad_tail_colon) - 1U);
  internal_reject(bad_colon, sizeof(bad_colon) - 1U);
  internal_reject(bad_entity, sizeof(bad_entity) - 1U);
  internal_reject(bad_zero, sizeof(bad_zero) - 1U);
  internal_reject(bad_cdata_close, sizeof(bad_cdata_close) - 1U);
  internal_reject(bad_comment, sizeof(bad_comment) - 1U);
  internal_reject(bad_comment_end, sizeof(bad_comment_end) - 1U);
  internal_reject(bad_declaration, sizeof(bad_declaration) - 1U);
  internal_reject(control, sizeof(control));
  internal_reject(nul, sizeof(nul));
  internal_reject(invalid_utf8, sizeof(invalid_utf8));
  internal_reject(invalid_attr, sizeof(invalid_attr));
}

/** @brief Prove declaration, external DOCTYPE, PI, and BOM boundaries. */
RA8_INTERNAL static void internal_declarations(void)
{
  static const uint8_t declaration[] =
    "<?xml version='1.0' encoding='UTF-8' standalone='yes'?><a/>";
  static const uint8_t lower_encoding[] = "<?xml version='1.0' encoding='utf-8'?><a/>";
  static const uint8_t doctype[]        = "<!DOCTYPE html><html/>";
  static const uint8_t system_id[]      = "<!DOCTYPE html SYSTEM 'urn:x>y'><html/>";
  static const uint8_t public_id[] =
    "<!DOCTYPE html PUBLIC '-//W3C//DTD XHTML 1.1//EN' 'xhtml11.dtd'><html/>";
  static const uint8_t valid_pi[] = "<?build target?><a><?inside?></a><?after?>";
  static const uint8_t bom[] = "\xEF\xBB\xBF<?xml version='1.0' encoding='UTF-8'?><!DOCTYPE a><a/>";
  static const uint8_t upper_xml[]      = "<?XML version='1.0'?><a/>";
  static const uint8_t bad_version[]    = "<?xml version='1.1'?><a/>";
  static const uint8_t bad_encoding[]   = "<?xml version='1.0' encoding='ISO-8859-1'?><a/>";
  static const uint8_t mixed_encoding[] = "<?xml version='1.0' encoding='Utf-8'?><a/>";
  static const uint8_t bad_pi[]         = "<?1build?><a/>";
  static const uint8_t internal_set[]   = "<!DOCTYPE a [<!ENTITY x 'x'>]><a/>";
  static const uint8_t entity_decl[]    = "<!ENTITY x 'x'><a/>";
  static const uint8_t late_doctype[]   = "<a/><!DOCTYPE a>";
  static const uint8_t duplicate_bom[]  = "\xEF\xBB\xBF\xEF\xBB\xBF<a/>";
  static const uint8_t misplaced_bom[]  = " \xEF\xBB\xBF<a/>";
  static const uint8_t feff_text[]      = "<a>\xEF\xBB\xBF</a>";
  ra8_xml_workspace_t  workspace        = {};
  assert(ra8_xml_validate(declaration, sizeof(declaration) - 1U, &workspace) == k_ra8_ok);
  assert(ra8_xml_validate(lower_encoding, sizeof(lower_encoding) - 1U, &workspace) == k_ra8_ok);
  assert(ra8_xml_validate(doctype, sizeof(doctype) - 1U, &workspace) == k_ra8_ok);
  assert(ra8_xml_validate(system_id, sizeof(system_id) - 1U, &workspace) == k_ra8_ok);
  assert(ra8_xml_validate(public_id, sizeof(public_id) - 1U, &workspace) == k_ra8_ok);
  assert(ra8_xml_validate(valid_pi, sizeof(valid_pi) - 1U, &workspace) == k_ra8_ok);
  assert(ra8_xml_validate(bom, sizeof(bom) - 1U, &workspace) == k_ra8_ok);
  assert(ra8_xml_validate(feff_text, sizeof(feff_text) - 1U, &workspace) == k_ra8_ok);
  internal_reject(upper_xml, sizeof(upper_xml) - 1U);
  internal_reject(bad_version, sizeof(bad_version) - 1U);
  internal_reject(bad_encoding, sizeof(bad_encoding) - 1U);
  internal_reject(mixed_encoding, sizeof(mixed_encoding) - 1U);
  internal_reject(bad_pi, sizeof(bad_pi) - 1U);
  internal_reject(internal_set, sizeof(internal_set) - 1U);
  internal_reject(entity_decl, sizeof(entity_decl) - 1U);
  internal_reject(late_doctype, sizeof(late_doctype) - 1U);
  internal_reject(duplicate_bom, sizeof(duplicate_bom) - 1U);
  internal_reject(misplaced_bom, sizeof(misplaced_bom) - 1U);
}

/** @brief Prove public spans are bounded and prefix decode is codepoint-safe. */
RA8_INTERNAL static void internal_span_bounds_and_prefix(void)
{
  static const uint8_t source[]  = "<r a='123&amp;456'/>";
  ra8_xml_workspace_t  workspace = {};
  ra8_xml_reader_t     reader    = {};
  ra8_xml_event_t      event     = {};
  assert(ra8_xml_reader_init(&reader, source, sizeof(source) - 1U, &workspace) == k_ra8_ok);
  assert(ra8_xml_reader_next(&reader, &event) == k_ra8_ok);
  ra8_xml_attr_cursor_t cursor    = {};
  ra8_xml_attribute_t   attribute = {};
  bool                  present   = false;
  ra8_xml_attr_begin(&event, &cursor);
  assert(ra8_xml_attr_next(source, sizeof(source) - 1U, &event, &cursor, &attribute, &present) ==
           k_ra8_ok &&
         present);
  char   decoded[5] = {};
  size_t length     = 0U;
  assert(ra8_xml_decode_prefix(source,
                               sizeof(source) - 1U,
                               attribute.value,
                               decoded,
                               sizeof(decoded),
                               &length) == k_ra8_ok);
  assert((length == 4U) && (strcmp(decoded, "123&") == 0));
  const ra8_xml_span_t forged = {UINT32_MAX, 8U};
  assert(!ra8_xml_span_equal(source, sizeof(source) - 1U, forged, "x"));
  assert(ra8_xml_decoded_size(source, sizeof(source) - 1U, forged, &length) ==
         k_ra8_err_validation_failed);
}

int main(void)
{
  internal_depth_bound();
  internal_independent_readers();
  internal_entities_and_faults();
  internal_entity_comparison();
  internal_hostile_syntax();
  internal_declarations();
  internal_span_bounds_and_prefix();
  return 0;
}
