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

/**
 * @brief Prove depth cap and cap-minus-one are deterministic.
 * @par MC/DC:
 * The valid maximum-depth document is the all-false control; the extra level
 * independently selects the depth rejection while every close tag exercises
 * the matching-stack control. Decisions:
 * - libs/ra8_xml/src/ra8_xml.c@internal_start
 * - libs/ra8_xml/src/ra8_xml.c@internal_end
 * - libs/ra8_xml/src/ra8_xml.c@ra8_xml_reader_next
 * - libs/ra8_xml/src/ra8_xml.c@ra8_xml_validate
 */
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

/**
 * @brief Prove two readers do not share mutable parser state.
 * @par MC/DC:
 * Alternating valid start/self-closing/text events supplies the successful
 * controls for attribute and span guards before the hostile vectors vary them.
 * Decisions:
 * - libs/ra8_xml/src/ra8_xml.c@ra8_xml_span_equal
 * - libs/ra8_xml/src/ra8_xml.c@internal_attributes
 */
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

/**
 * @brief Exercise every public reader and attribute pointer guard.
 * @par MC/DC:
 * Each required pointer is varied alone against the same fully valid control;
 * zero length varies the independent size guard, and a forged markup span
 * varies the attribute provenance guard. Decisions:
 * - libs/ra8_xml/src/ra8_xml.c@ra8_xml_reader_init
 * - libs/ra8_xml/src/ra8_xml.c@ra8_xml_reader_next
 * - libs/ra8_xml/src/ra8_xml.c@ra8_xml_attr_begin
 * - libs/ra8_xml/src/ra8_xml.c@ra8_xml_attr_next
 * - libs/ra8_xml/src/ra8_xml.c@internal_span_valid
 */
RA8_INTERNAL static void internal_public_reader_guards(void)
{
  static const uint8_t source[]  = "<a x='1'/>";
  ra8_xml_workspace_t  workspace = {};
  ra8_xml_reader_t     reader    = {};
  ra8_xml_event_t      event     = {};
  assert(ra8_xml_reader_init(nullptr, source, sizeof(source) - 1U, &workspace) ==
         k_ra8_err_null_ptr);
  assert(ra8_xml_reader_init(&reader, nullptr, sizeof(source) - 1U, &workspace) ==
         k_ra8_err_null_ptr);
  assert(ra8_xml_reader_init(&reader, source, sizeof(source) - 1U, nullptr) == k_ra8_err_null_ptr);
  assert(ra8_xml_reader_init(&reader, source, 0U, &workspace) == k_ra8_err_invalid_size);
  assert(ra8_xml_reader_init(&reader, source, sizeof(source) - 1U, &workspace) == k_ra8_ok);
  assert(ra8_xml_reader_next(nullptr, &event) == k_ra8_err_null_ptr);
  assert(ra8_xml_reader_next(&reader, nullptr) == k_ra8_err_null_ptr);
  assert(ra8_xml_reader_next(&reader, &event) == k_ra8_ok);

  ra8_xml_attr_cursor_t cursor = {.position = 7U, .emitted = 8U};
  ra8_xml_attr_begin(nullptr, &cursor);
  assert((cursor.position == 7U) && (cursor.emitted == 8U));
  ra8_xml_attr_begin(&event, nullptr);
  ra8_xml_attr_begin(&event, &cursor);
  ra8_xml_attribute_t attribute = {};
  bool                present   = false;
  assert(ra8_xml_attr_next(nullptr, sizeof(source) - 1U, &event, &cursor, &attribute, &present) ==
         k_ra8_err_null_ptr);
  assert(ra8_xml_attr_next(source, sizeof(source) - 1U, nullptr, &cursor, &attribute, &present) ==
         k_ra8_err_null_ptr);
  assert(ra8_xml_attr_next(source, sizeof(source) - 1U, &event, nullptr, &attribute, &present) ==
         k_ra8_err_null_ptr);
  assert(ra8_xml_attr_next(source, sizeof(source) - 1U, &event, &cursor, nullptr, &present) ==
         k_ra8_err_null_ptr);
  assert(ra8_xml_attr_next(source, sizeof(source) - 1U, &event, &cursor, &attribute, nullptr) ==
         k_ra8_err_null_ptr);
  assert(ra8_xml_attr_next(source, sizeof(source) - 1U, &event, &cursor, &attribute, &present) ==
           k_ra8_ok &&
         present);
  ra8_xml_event_t forged = event;
  forged.markup.offset   = UINT32_MAX;
  ra8_xml_attr_begin(&forged, &cursor);
  assert(ra8_xml_attr_next(source, sizeof(source) - 1U, &forged, &cursor, &attribute, &present) ==
         k_ra8_err_validation_failed);
}

/**
 * @brief Prove entity decoding, attributes, malformed rejection, and immutability.
 * @par MC/DC:
 * Named, decimal, and hexadecimal entities provide the success controls;
 * unknown/control entities, mismatched tags, and duplicate attributes vary
 * the respective rejection predicates. Decisions:
 * - libs/ra8_xml/src/ra8_xml.c@internal_digit
 * - libs/ra8_xml/src/ra8_xml.c@internal_entity
 * - libs/ra8_xml/src/ra8_xml.c@internal_decode
 * - libs/ra8_xml/src/ra8_xml.c@ra8_xml_decode
 * - libs/ra8_xml/src/ra8_xml.c@internal_attr_parse
 * - libs/ra8_xml/src/ra8_xml.c@internal_attr_duplicate
 * - libs/ra8_xml/src/ra8_xml.c@internal_attributes
 * - libs/ra8_xml/src/ra8_xml.c@internal_text
 */
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

/**
 * @brief Prove numeric entities compare equal to their UTF-8 bytes.
 * @par MC/DC:
 * Equal raw/entity values are the all-false control for size and byte mismatch
 * predicates; the public span matrix separately varies invalid and unequal
 * operands. Decisions: libs/ra8_xml/src/ra8_xml.c@ra8_xml_decoded_equal
 */
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

/**
 * @brief Prove strict names, characters, comments, declarations, and placement.
 * @par MC/DC:
 * Valid QName/close/comment controls are paired with invalid start, colon,
 * comment, declaration-placement, character, and UTF-8 vectors. Decisions:
 * - libs/ra8_xml/src/ra8_xml.c@internal_space
 * - libs/ra8_xml/src/ra8_xml.c@internal_ascii_letter
 * - libs/ra8_xml/src/ra8_xml.c@internal_name_start
 * - libs/ra8_xml/src/ra8_xml.c@internal_name_continue
 * - libs/ra8_xml/src/ra8_xml.c@priv_ra8_xml_qname
 * - libs/ra8_xml/src/ra8_xml.c@internal_xml_char
 * - libs/ra8_xml/src/ra8_xml.c@internal_utf8_next
 * - libs/ra8_xml/src/ra8_xml.c@internal_end
 * - libs/ra8_xml/src/ra8_xml.c@internal_comment
 * - libs/ra8_xml/src/ra8_xml.c@internal_xml_target
 * - libs/ra8_xml/src/ra8_xml.c@internal_special
 * - libs/ra8_xml/src/ra8_xml.c@internal_text
 */
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

/**
 * @brief Exercise every accepted QName class, whitespace byte, entity, and UTF-8 width.
 * @par MC/DC:
 * The valid documents independently select upper/lower/underscore name starts,
 * all continuation classes, all four XML whitespace bytes, every predefined
 * entity, and canonical two/three/four-byte UTF-8. The malformed streams vary
 * the overlong, surrogate, out-of-range, and truncated UTF-8 guards. Decisions:
 * - libs/ra8_xml/src/ra8_xml.c@internal_space
 * - libs/ra8_xml/src/ra8_xml.c@internal_ascii_letter
 * - libs/ra8_xml/src/ra8_xml.c@internal_name_start
 * - libs/ra8_xml/src/ra8_xml.c@internal_name_continue
 * - libs/ra8_xml/src/ra8_xml.c@priv_ra8_xml_qname
 * - libs/ra8_xml/src/ra8_xml.c@internal_xml_char
 * - libs/ra8_xml/src/ra8_xml.c@internal_utf8_next
 * - libs/ra8_xml/src/ra8_xml.c@internal_entity
 */
RA8_INTERNAL static void internal_character_classes(void)
{
  static const uint8_t names[]     = "<_A:b-c.d1\tq='1'\nr='2'\rs='3' />";
  static const uint8_t entities[]  = "<a>&amp;&lt;&gt;&quot;&apos;&#65;&#x42;</a>";
  static const uint8_t utf8[]      = "<a>\xC3\xA9\xE2\x82\xAC\xF0\x90\x80\x80</a>";
  static const uint8_t overlong3[] = {'<', 'a', '>', 0xE0U, 0x80U, 0x80U, '<', '/', 'a', '>'};
  static const uint8_t overlong4[] =
    {'<', 'a', '>', 0xF0U, 0x80U, 0x80U, 0x80U, '<', '/', 'a', '>'};
  static const uint8_t surrogate[] = {'<', 'a', '>', 0xEDU, 0xA0U, 0x80U, '<', '/', 'a', '>'};
  static const uint8_t too_high[] = {'<', 'a', '>', 0xF4U, 0x90U, 0x80U, 0x80U, '<', '/', 'a', '>'};
  static const uint8_t truncated[]     = {'<', 'a', '>', 0xE2U, 0x82U};
  static const uint8_t empty_numeric[] = "<a>&#x;</a>";
  static const uint8_t bad_numeric[]   = "<a>&#xG;</a>";
  static const uint8_t high_numeric[]  = "<a>&#x110000;</a>";
  ra8_xml_workspace_t  workspace       = {};
  assert(ra8_xml_validate(names, sizeof(names) - 1U, &workspace) == k_ra8_ok);
  assert(ra8_xml_validate(entities, sizeof(entities) - 1U, &workspace) == k_ra8_ok);
  assert(ra8_xml_validate(utf8, sizeof(utf8) - 1U, &workspace) == k_ra8_ok);
  internal_reject(overlong3, sizeof(overlong3));
  internal_reject(overlong4, sizeof(overlong4));
  internal_reject(surrogate, sizeof(surrogate));
  internal_reject(too_high, sizeof(too_high));
  internal_reject(truncated, sizeof(truncated));
  internal_reject(empty_numeric, sizeof(empty_numeric) - 1U);
  internal_reject(bad_numeric, sizeof(bad_numeric) - 1U);
  internal_reject(high_numeric, sizeof(high_numeric) - 1U);
}

/**
 * @brief Prove declaration, external DOCTYPE, PI, and BOM boundaries.
 * @par MC/DC:
 * Bare/SYSTEM/PUBLIC declarations, both accepted encodings, standalone, PI,
 * and leading BOM form success controls; version/encoding/placement/internal
 * subset/entity/BOM faults vary the fail-closed predicates. Decisions:
 * - libs/ra8_xml/src/ra8_xml.c@internal_markup_end
 * - libs/ra8_xml/src/ra8_xml.c@internal_xml_target
 * - libs/ra8_xml/src/ra8_xml.c@internal_encoding
 * - libs/ra8_xml/src/ra8_xml.c@internal_declaration_attr
 * - libs/ra8_xml/src/ra8_xml.c@internal_declaration
 * - libs/ra8_xml/src/ra8_xml.c@internal_pi
 * - libs/ra8_xml/src/ra8_xml.c@internal_special
 * - libs/ra8_xml/src/ra8_xml.c@ra8_xml_reader_init
 * - libs/ra8_xml/src/ra8_xml_doctype.c@internal_space
 * - libs/ra8_xml/src/ra8_xml_doctype.c@internal_skip_space
 * - libs/ra8_xml/src/ra8_xml_doctype.c@internal_pubid_byte
 * - libs/ra8_xml/src/ra8_xml_doctype.c@internal_literal
 * - libs/ra8_xml/src/ra8_xml_doctype.c@internal_keyword
 * - libs/ra8_xml/src/ra8_xml_doctype.c@internal_external_id
 * - libs/ra8_xml/src/ra8_xml_doctype.c@priv_ra8_xml_doctype
 */
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

/** @brief Prove malformed declaration and external-identifier forms fail closed. */
RA8_INTERNAL static void internal_declaration_faults(void)
{
  static const uint8_t duplicate_decl[] = "<?xml version='1.0'?><?xml version='1.0'?><a/>";
  static const uint8_t unknown_attr[]   = "<?xml version='1.0' other='x'?><a/>";
  static const uint8_t duplicate_encoding[] =
    "<?xml version='1.0' encoding='UTF-8' encoding='UTF-8'?><a/>";
  static const uint8_t bad_standalone[] = "<?xml version='1.0' standalone='maybe'?><a/>";
  static const uint8_t bad_public[]     = "<!DOCTYPE a PUBLIC '[' 'sys'><a/>";
  static const uint8_t missing_system[] = "<!DOCTYPE a SYSTEM><a/>";
  static const uint8_t missing_public[] = "<!DOCTYPE a PUBLIC 'id'><a/>";
  static const uint8_t bad_keyword[]    = "<!DOCTYPE a SYSTEMX 'sys'><a/>";
  static const uint8_t open_literal[]   = "<!DOCTYPE a SYSTEM 'sys><a/>";
  internal_reject(duplicate_decl, sizeof(duplicate_decl) - 1U);
  internal_reject(unknown_attr, sizeof(unknown_attr) - 1U);
  internal_reject(duplicate_encoding, sizeof(duplicate_encoding) - 1U);
  internal_reject(bad_standalone, sizeof(bad_standalone) - 1U);
  internal_reject(bad_public, sizeof(bad_public) - 1U);
  internal_reject(missing_system, sizeof(missing_system) - 1U);
  internal_reject(missing_public, sizeof(missing_public) - 1U);
  internal_reject(bad_keyword, sizeof(bad_keyword) - 1U);
  internal_reject(open_literal, sizeof(open_literal) - 1U);
}

/**
 * @brief Prove public spans are bounded and prefix decode is codepoint-safe.
 * @par MC/DC:
 * Valid/raw/entity spans supply all-false controls; forged bounds, exact-fit,
 * clipped-prefix, unequal-size, unequal-byte, and null operands vary each
 * public span/decode predicate. Decisions:
 * - libs/ra8_xml/src/ra8_xml.c@internal_span_valid
 * - libs/ra8_xml/src/ra8_xml.c@internal_decode
 * - libs/ra8_xml/src/ra8_xml.c@ra8_xml_decode_prefix
 * - libs/ra8_xml/src/ra8_xml.c@ra8_xml_decoded_size
 * - libs/ra8_xml/src/ra8_xml.c@ra8_xml_span_equal
 * - libs/ra8_xml/src/ra8_xml.c@ra8_xml_span_local_equal
 * - libs/ra8_xml/src/ra8_xml.c@ra8_xml_decoded_equal
 */
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

/** @brief Exercise public decode, span, local-name, and comparison guard matrices. */
RA8_INTERNAL static void internal_public_span_guards(void)
{
  static const uint8_t names[]    = "ns:a";
  static const uint8_t values[]   = "A&amp;A&amp;B";
  const ra8_xml_span_t name       = {0U, 4U};
  const ra8_xml_span_t left       = {0U, 6U};
  const ra8_xml_span_t right      = {6U, 6U};
  const ra8_xml_span_t different  = {12U, 1U};
  const ra8_xml_span_t forged     = {UINT32_MAX, 8U};
  char                 decoded[3] = {};
  size_t               length     = 0U;
  assert(ra8_xml_decode(values, sizeof(values) - 1U, left, decoded, sizeof(decoded), &length) ==
           k_ra8_ok &&
         (length == 2U) && (strcmp(decoded, "A&") == 0));
  assert(ra8_xml_decode(values, sizeof(values) - 1U, left, decoded, 2U, &length) ==
         k_ra8_err_no_mem);
  assert(ra8_xml_decode(nullptr, sizeof(values) - 1U, left, decoded, sizeof(decoded), &length) ==
         k_ra8_err_null_ptr);
  assert(ra8_xml_decode(values, sizeof(values) - 1U, left, nullptr, sizeof(decoded), &length) ==
         k_ra8_err_null_ptr);
  assert(ra8_xml_decode(values, sizeof(values) - 1U, left, decoded, sizeof(decoded), nullptr) ==
         k_ra8_err_null_ptr);
  assert(
    ra8_xml_decode_prefix(nullptr, sizeof(values) - 1U, left, decoded, sizeof(decoded), &length) ==
    k_ra8_err_null_ptr);
  assert(
    ra8_xml_decode_prefix(values, sizeof(values) - 1U, left, nullptr, sizeof(decoded), &length) ==
    k_ra8_err_null_ptr);
  assert(
    ra8_xml_decode_prefix(values, sizeof(values) - 1U, left, decoded, sizeof(decoded), nullptr) ==
    k_ra8_err_null_ptr);
  assert(ra8_xml_decoded_size(nullptr, sizeof(values) - 1U, left, &length) == k_ra8_err_null_ptr);
  assert(ra8_xml_decoded_size(values, sizeof(values) - 1U, left, nullptr) == k_ra8_err_null_ptr);
  assert(ra8_xml_span_equal(names, sizeof(names) - 1U, name, "ns:a"));
  assert(!ra8_xml_span_equal(nullptr, sizeof(names) - 1U, name, "ns:a"));
  assert(!ra8_xml_span_equal(names, sizeof(names) - 1U, name, nullptr));
  assert(!ra8_xml_span_equal(names, sizeof(names) - 1U, forged, "a"));
  assert(ra8_xml_span_local_equal(names, sizeof(names) - 1U, name, "a"));
  assert(!ra8_xml_span_local_equal(nullptr, sizeof(names) - 1U, name, "a"));
  assert(!ra8_xml_span_local_equal(names, sizeof(names) - 1U, forged, "a"));
  assert(ra8_xml_decoded_equal(values, sizeof(values) - 1U, left, right));
  assert(!ra8_xml_decoded_equal(values, sizeof(values) - 1U, left, different));
  assert(!ra8_xml_decoded_equal(values, sizeof(values) - 1U, left, forged));
  assert(!ra8_xml_decoded_equal(nullptr, sizeof(values) - 1U, left, right));
}

int main(void)
{
  internal_depth_bound();
  internal_independent_readers();
  internal_public_reader_guards();
  internal_entities_and_faults();
  internal_entity_comparison();
  internal_hostile_syntax();
  internal_character_classes();
  internal_declarations();
  internal_declaration_faults();
  internal_span_bounds_and_prefix();
  internal_public_span_guards();
  return 0;
}
