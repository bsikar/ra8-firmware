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
#include "ra8_xml_internal.h"

/**
 * @brief The open and close element bytes appended by the depth fixture.
 * @details Byte arrays rather than C strings: they are concatenated into one
 *          length-delimited document buffer, which carries no terminator.
 * @note Read-only fixture constants.
 * @since 0.1.0
 */
static const uint8_t s_xml_open[]  = {'<', 'x', '>'};
static const uint8_t s_xml_close[] = {'<', '/', 'x', '>'};

/** @brief Build a document with exactly @p levels element levels. */
RA8_INTERNAL static size_t
internal_deep_document(uint8_t* destination, size_t capacity, uint16_t levels)
{
  size_t used = 0U;
  for (uint16_t i = 0U; i < levels; ++i) {
    assert((used + 3U) <= capacity);
    (void)memcpy(&destination[used], s_xml_open, sizeof(s_xml_open));
    used += 3U;
  }
  for (uint16_t i = 0U; i < levels; ++i) {
    assert((used + 4U) <= capacity);
    (void)memcpy(&destination[used], s_xml_close, sizeof(s_xml_close));
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

/**
 * @brief Complete the DOCTYPE lexer's spacing, literal, and terminator vectors.
 * @par MC/DC:
 * Tab/CR/LF spacing bytes independently select the three remaining operands of
 * the XML S classifier; a source that ends inside the declaration varies the
 * range operand of every bounded scan; a double-quoted, a bare, and an absent
 * literal independently vary the three quote operands; a truncated tail varies
 * the six-byte keyword-lookahead bound; an unseparated name varies the
 * separator operand; and a second declaration varies the already-seen operand.
 * Decisions:
 * - libs/ra8_xml/src/ra8_xml_doctype.c@internal_space
 * - libs/ra8_xml/src/ra8_xml_doctype.c@internal_skip_space
 * - libs/ra8_xml/src/ra8_xml_doctype.c@internal_literal
 * - libs/ra8_xml/src/ra8_xml_doctype.c@internal_external_id
 * - libs/ra8_xml/src/ra8_xml_doctype.c@priv_ra8_xml_doctype
 */
RA8_INTERNAL static void internal_doctype_lexer_vectors(void)
{
  static const uint8_t tab_spacing[]   = "<!DOCTYPE\ta\t\r\n><a/>";
  static const uint8_t double_quoted[] = "<!DOCTYPE a SYSTEM \"x\"><a/>";
  static const uint8_t twice[]         = "<!DOCTYPE a><!DOCTYPE b><a/>";
  static const uint8_t prefix_only[]   = "<!DOCTYPE";
  static const uint8_t open_system[]   = "<!DOCTYPE a SYSTEM ";
  static const uint8_t bare_literal[]  = "<!DOCTYPE a SYSTEM x>";
  static const uint8_t short_tail[]    = "<!DOCTYPE a xy";
  static const uint8_t space_tail[]    = "<!DOCTYPE a ";
  static const uint8_t unseparated[]   = "<!DOCTYPE a'sys'>";
  static const uint8_t trailing_byte[] = "<!DOCTYPE a SYSTEM 'x'y>";
  ra8_xml_workspace_t  workspace       = {};
  assert(ra8_xml_validate(tab_spacing, sizeof(tab_spacing) - 1U, &workspace) == k_ra8_ok);
  assert(ra8_xml_validate(double_quoted, sizeof(double_quoted) - 1U, &workspace) == k_ra8_ok);
  internal_reject(twice, sizeof(twice) - 1U);
  internal_reject(prefix_only, sizeof(prefix_only) - 1U);
  internal_reject(open_system, sizeof(open_system) - 1U);
  internal_reject(bare_literal, sizeof(bare_literal) - 1U);
  internal_reject(short_tail, sizeof(short_tail) - 1U);
  internal_reject(space_tail, sizeof(space_tail) - 1U);
  internal_reject(unseparated, sizeof(unseparated) - 1U);
  internal_reject(trailing_byte, sizeof(trailing_byte) - 1U);
}

/**
 * @brief Drive the DOCTYPE prefix guard through its documented private seam.
 * @par MC/DC:
 * `ra8_xml_reader_next()` only dispatches to the lexer once it has already
 * proved the nine prefix bytes are present and equal, so the length and
 * mismatch operands of the entry guard are unreachable from the pull loop. The
 * private seam is called directly with a truncated and with a mistyped prefix,
 * each varying exactly one of those operands against the accepted control.
 * Decisions: libs/ra8_xml/src/ra8_xml_doctype.c@priv_ra8_xml_doctype
 */
RA8_INTERNAL static void internal_doctype_direct_seam(void)
{
  static const uint8_t truncated[] = "<!DOC";
  static const uint8_t mistyped[]  = "<!DOCTYPX a>";
  ra8_xml_workspace_t  workspace   = {};
  ra8_xml_reader_t     reader      = {};
  assert(ra8_xml_reader_init(&reader, truncated, sizeof(truncated) - 1U, &workspace) == k_ra8_ok);
  assert(priv_ra8_xml_doctype(&reader) == k_ra8_err_validation_failed);
  assert(ra8_xml_reader_init(&reader, mistyped, sizeof(mistyped) - 1U, &workspace) == k_ra8_ok);
  assert(priv_ra8_xml_doctype(&reader) == k_ra8_err_validation_failed);
}

/**
 * @brief Complete the name, UTF-8 lead, and numeric-entity operand vectors.
 * @par MC/DC:
 * A byte above 'z' varies the lower-case upper bound of the ASCII letter test;
 * an empty tag varies the start-past-end operand of the QName scan; a colon at
 * the markup terminator varies its lookahead bound; a 0xF5 lead varies the
 * four-byte UTF-8 upper bound; a sign byte varies the decimal-digit lower
 * bound and reaches both hex tables with base ten; 'a'..'f', 'g', and '/'
 * independently vary the two hex-range operands; an upper-case radix marker
 * varies the second radix operand; and an unterminated numeric reference
 * varies the scan bound and the terminator operand.
 * Decisions:
 * - libs/ra8_xml/src/ra8_xml.c@internal_ascii_letter
 * - libs/ra8_xml/src/ra8_xml.c@priv_ra8_xml_qname
 * - libs/ra8_xml/src/ra8_xml.c@internal_utf8_next
 * - libs/ra8_xml/src/ra8_xml.c@internal_digit
 * - libs/ra8_xml/src/ra8_xml.c@internal_entity
 */
RA8_INTERNAL static void internal_xml_lexer_vectors(void)
{
  static const uint8_t hex_letter[]  = "<a>&#xab;</a>";
  static const uint8_t hex_upper_x[] = "<a>&#X41;</a>";
  static const uint8_t high_letter[] = "<a{/>";
  static const uint8_t empty_tag[]   = "<>";
  static const uint8_t tail_colon[]  = "<a:>";
  static const uint8_t bad_four[]    = {'<', 'a', '>', 0xF5U, '<', '/', 'a', '>'};
  static const uint8_t sign_entity[] = "<a>&#+1;</a>";
  static const uint8_t hex_above_f[] = "<a>&#xg;</a>";
  static const uint8_t hex_below_a[] = "<a>&#x/;</a>";
  static const uint8_t open_entity[] = "<a>&#41</a>";
  ra8_xml_workspace_t  workspace     = {};
  assert(ra8_xml_validate(hex_letter, sizeof(hex_letter) - 1U, &workspace) == k_ra8_ok);
  assert(ra8_xml_validate(hex_upper_x, sizeof(hex_upper_x) - 1U, &workspace) == k_ra8_ok);
  internal_reject(high_letter, sizeof(high_letter) - 1U);
  internal_reject(empty_tag, sizeof(empty_tag) - 1U);
  internal_reject(tail_colon, sizeof(tail_colon) - 1U);
  internal_reject(bad_four, sizeof(bad_four));
  internal_reject(sign_entity, sizeof(sign_entity) - 1U);
  internal_reject(hex_above_f, sizeof(hex_above_f) - 1U);
  internal_reject(hex_below_a, sizeof(hex_below_a) - 1U);
  internal_reject(open_entity, sizeof(open_entity) - 1U);
}

/**
 * @brief Complete the attribute scanner's bound and delimiter vectors.
 * @par MC/DC:
 * A name that ends at the markup terminator varies the range operand of the
 * post-name space scan and the range operand of the equals test; a second name
 * varies the equals byte itself; an equals at the terminator varies the range
 * operand of the pre-value space scan and of the quote test while a spaced
 * equals varies that same scan's spacing operand; an unquoted value
 * varies both quote bytes; a spaced solidus varies the trailing-space scan in
 * both directions; and a spaced element name varies the trailing-trim scan.
 * Decisions:
 * - libs/ra8_xml/src/ra8_xml.c@internal_attr_parse
 * - libs/ra8_xml/src/ra8_xml.c@internal_attributes
 * - libs/ra8_xml/src/ra8_xml.c@internal_start
 */
RA8_INTERNAL static void internal_xml_attribute_vectors(void)
{
  static const uint8_t slash_space[]   = "<a / >";
  static const uint8_t space_tag[]     = "<a ></a>";
  static const uint8_t spaced_equals[] = "<a x = '1'/>";
  static const uint8_t name_at_end[]   = "<a x></a>";
  static const uint8_t no_equals[]     = "<a x y='1'></a>";
  static const uint8_t equals_at_end[] = "<a x=></a>";
  static const uint8_t unquoted[]      = "<a x=1></a>";
  static const uint8_t slash_junk[]    = "<a /x>";
  ra8_xml_workspace_t  workspace       = {};
  assert(ra8_xml_validate(slash_space, sizeof(slash_space) - 1U, &workspace) == k_ra8_ok);
  assert(ra8_xml_validate(space_tag, sizeof(space_tag) - 1U, &workspace) == k_ra8_ok);
  assert(ra8_xml_validate(spaced_equals, sizeof(spaced_equals) - 1U, &workspace) == k_ra8_ok);
  internal_reject(name_at_end, sizeof(name_at_end) - 1U);
  internal_reject(no_equals, sizeof(no_equals) - 1U);
  internal_reject(equals_at_end, sizeof(equals_at_end) - 1U);
  internal_reject(unquoted, sizeof(unquoted) - 1U);
  internal_reject(slash_junk, sizeof(slash_junk) - 1U);
}

/**
 * @brief Vary each caller-supplied attribute event and cursor guard alone.
 * @par MC/DC:
 * The attribute cursor and event are caller-owned, so the public entry point
 * must reject any provenance a caller can present. A forged name span varies
 * the second operand of the provenance guard, a zero-length markup span varies
 * the third, and a cursor parked on the markup terminator varies the range
 * operand of the leading-space scan -- each against the same accepted control.
 * Decisions:
 * - libs/ra8_xml/src/ra8_xml.c@ra8_xml_attr_next
 * - libs/ra8_xml/src/ra8_xml.c@internal_attr_parse
 */
RA8_INTERNAL static void internal_xml_attr_guard_vectors(void)
{
  static const uint8_t source[]  = "<a x='12'/>";
  ra8_xml_workspace_t  workspace = {};
  ra8_xml_reader_t     reader    = {};
  ra8_xml_event_t      event     = {};
  ra8_xml_attribute_t  attribute = {};
  bool                 present   = false;
  assert(ra8_xml_reader_init(&reader, source, sizeof(source) - 1U, &workspace) == k_ra8_ok);
  assert(ra8_xml_reader_next(&reader, &event) == k_ra8_ok);

  ra8_xml_attr_cursor_t cursor      = {};
  ra8_xml_event_t       forged_name = event;
  forged_name.name.offset           = UINT32_MAX;
  ra8_xml_attr_begin(&forged_name, &cursor);
  assert(
    ra8_xml_attr_next(source, sizeof(source) - 1U, &forged_name, &cursor, &attribute, &present) ==
    k_ra8_err_validation_failed);

  ra8_xml_event_t empty_markup = event;
  empty_markup.markup.length   = 0U;
  ra8_xml_attr_begin(&empty_markup, &cursor);
  assert(
    ra8_xml_attr_next(source, sizeof(source) - 1U, &empty_markup, &cursor, &attribute, &present) ==
    k_ra8_err_validation_failed);

  ra8_xml_attr_cursor_t parked = {.position = event.markup.offset + event.markup.length - 1U,
                                  .emitted  = 0U};
  assert(ra8_xml_attr_next(source, sizeof(source) - 1U, &event, &parked, &attribute, &present) ==
         k_ra8_err_validation_failed);
}

/**
 * @brief Complete the element, comment, text, and dispatch operand vectors.
 * @par MC/DC:
 * A second root varies the already-closed operand of the start guard; a close
 * tag with trailing junk varies the trailing-space scan and the exact-end
 * operand of the end guard; an unmatched close varies its empty-stack operand;
 * a longer close name varies the length operand of the name comparison; an
 * empty comment varies the adjacency operand of the trailing-hyphen guard;
 * bracket runs vary the second and third operands of the CDATA-close scan; and
 * a two-byte source varies the length operand of every markup-prefix probe.
 * Decisions:
 * - libs/ra8_xml/src/ra8_xml.c@internal_start
 * - libs/ra8_xml/src/ra8_xml.c@internal_end
 * - libs/ra8_xml/src/ra8_xml.c@internal_comment
 * - libs/ra8_xml/src/ra8_xml.c@internal_text
 * - libs/ra8_xml/src/ra8_xml.c@internal_special
 */
RA8_INTERNAL static void internal_xml_structure_vectors(void)
{
  static const uint8_t empty_comment[] = "<a><!----></a>";
  static const uint8_t bracket_text[]  = "<a>]x]]yy</a>";
  static const uint8_t second_root[]   = "<a/><b/>";
  static const uint8_t close_junk[]    = "<a></a x>";
  static const uint8_t orphan_close[]  = "</a>";
  static const uint8_t long_close[]    = "<a></ab>";
  static const uint8_t short_bang[]    = "<!";
  ra8_xml_workspace_t  workspace       = {};
  assert(ra8_xml_validate(empty_comment, sizeof(empty_comment) - 1U, &workspace) == k_ra8_ok);
  assert(ra8_xml_validate(bracket_text, sizeof(bracket_text) - 1U, &workspace) == k_ra8_ok);
  internal_reject(second_root, sizeof(second_root) - 1U);
  internal_reject(close_junk, sizeof(close_junk) - 1U);
  internal_reject(orphan_close, sizeof(orphan_close) - 1U);
  internal_reject(long_close, sizeof(long_close) - 1U);
  internal_reject(short_bang, sizeof(short_bang) - 1U);
}

/**
 * @brief Complete the processing-instruction and declaration operand vectors.
 * @par MC/DC:
 * Three-letter targets that differ in exactly one position independently vary
 * the second, third, and fourth operands of the `xml` target test; a
 * standalone `no` varies the second accepted value; a trailing space varies
 * the range operand of the inner space scan; an encoding-first declaration
 * varies the version-name operand; a declaration whose encoding follows
 * standalone varies the already-seen-standalone operands of both attribute
 * arms; a declaration at a non-initial offset varies the BOM operand of the
 * placement test; an empty declaration varies the target-versus-terminator
 * operand; a malformed trailing attribute varies the attribute-status operand;
 * an unterminated value varies the range operand of the value scan; and a
 * quoted byte after a PI target varies both operands of the separator test.
 * Decisions:
 * - libs/ra8_xml/src/ra8_xml.c@internal_xml_target
 * - libs/ra8_xml/src/ra8_xml.c@internal_declaration_attr
 * - libs/ra8_xml/src/ra8_xml.c@internal_declaration
 * - libs/ra8_xml/src/ra8_xml.c@internal_attr_parse
 * - libs/ra8_xml/src/ra8_xml.c@internal_pi
 */
RA8_INTERNAL static void internal_xml_pi_vectors(void)
{
  static const uint8_t alt_targets[]    = "<?yml a?><?xnl a?><?xmz a?><a/>";
  static const uint8_t standalone_no[]  = "<?xml version='1.0' standalone='no'?><a/>";
  static const uint8_t trailing_space[] = "<?xml version='1.0' ?><a/>";
  static const uint8_t no_version[]     = "<?xml encoding='UTF-8'?><a/>";
  static const uint8_t late_encoding[] =
    "<?xml version='1.0' standalone='yes' encoding='UTF-8'?><a/>";
  static const uint8_t offset_decl[] = "   <?xml version='1.0'?><a/>";
  static const uint8_t empty_decl[]  = "<?xml?><a/>";
  static const uint8_t attr_fault[]  = "<?xml version='1.0' x?><a/>";
  static const uint8_t open_value[]  = "<?xml version='1.0?><a/>";
  static const uint8_t pi_punct[]    = "<?ab'c'?><a/>";
  ra8_xml_workspace_t  workspace     = {};
  assert(ra8_xml_validate(alt_targets, sizeof(alt_targets) - 1U, &workspace) == k_ra8_ok);
  assert(ra8_xml_validate(standalone_no, sizeof(standalone_no) - 1U, &workspace) == k_ra8_ok);
  assert(ra8_xml_validate(trailing_space, sizeof(trailing_space) - 1U, &workspace) == k_ra8_ok);
  internal_reject(no_version, sizeof(no_version) - 1U);
  internal_reject(late_encoding, sizeof(late_encoding) - 1U);
  internal_reject(offset_decl, sizeof(offset_decl) - 1U);
  internal_reject(empty_decl, sizeof(empty_decl) - 1U);
  internal_reject(attr_fault, sizeof(attr_fault) - 1U);
  internal_reject(open_value, sizeof(open_value) - 1U);
  internal_reject(pi_punct, sizeof(pi_punct) - 1U);
}

/** @brief Assert one short byte sequence initialises without skipping a BOM. */
RA8_INTERNAL static void internal_expect_no_bom(const uint8_t* source, size_t length)
{
  ra8_xml_workspace_t workspace = {};
  ra8_xml_reader_t    reader    = {};
  assert(ra8_xml_reader_init(&reader, source, length, &workspace) == k_ra8_ok);
  assert(reader.position == 0U);
}

/**
 * @brief Complete the public span, decode-capacity, and BOM operand vectors.
 * @par MC/DC:
 * A span whose length overruns the source varies the remaining-length operand
 * of the span validator; a zero destination capacity varies the capacity
 * operand of the decoder preflight; a forged left operand varies the first
 * status operand of the decoded comparison; a length above UINT32_MAX varies
 * the upper size bound of reader init; and two-byte, wrong-second-byte, and
 * wrong-third-byte sources independently vary the three remaining operands of
 * the BOM probe.
 * Decisions:
 * - libs/ra8_xml/src/ra8_xml.c@internal_span_valid
 * - libs/ra8_xml/src/ra8_xml.c@internal_decode
 * - libs/ra8_xml/src/ra8_xml.c@ra8_xml_decoded_equal
 * - libs/ra8_xml/src/ra8_xml.c@ra8_xml_reader_init
 */
RA8_INTERNAL static void internal_xml_public_boundary_vectors(void)
{
  static const uint8_t values[]      = "A&amp;A&amp;B";
  static const uint8_t short_bom[]   = {0xEFU, 0xBBU};
  static const uint8_t wrong_two[]   = {0xEFU, 0x41U, 0x42U};
  static const uint8_t wrong_three[] = {0xEFU, 0xBBU, 0x41U};
  const ra8_xml_span_t left          = {0U, 6U};
  const ra8_xml_span_t right         = {6U, 6U};
  const ra8_xml_span_t overrun       = {0U, (uint32_t)sizeof(values)};
  const ra8_xml_span_t forged        = {UINT32_MAX, 8U};
  char                 decoded[3]    = {};
  size_t               length        = 0U;
  ra8_xml_workspace_t  workspace     = {};
  ra8_xml_reader_t     reader        = {};
  assert(!ra8_xml_span_equal(values, sizeof(values) - 1U, overrun, "A&"));
  assert(ra8_xml_decode(values, sizeof(values) - 1U, left, decoded, 0U, &length) ==
         k_ra8_err_no_mem);
  assert(!ra8_xml_decoded_equal(values, sizeof(values) - 1U, forged, right));
  assert(ra8_xml_reader_init(&reader, values, (size_t)UINT32_MAX + 1U, &workspace) ==
         k_ra8_err_invalid_size);
  internal_expect_no_bom(short_bom, sizeof(short_bom));
  internal_expect_no_bom(wrong_two, sizeof(wrong_two));
  internal_expect_no_bom(wrong_three, sizeof(wrong_three));
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
  internal_doctype_lexer_vectors();
  internal_doctype_direct_seam();
  internal_xml_lexer_vectors();
  internal_xml_attribute_vectors();
  internal_xml_attr_guard_vectors();
  internal_xml_structure_vectors();
  internal_xml_pi_vectors();
  internal_xml_public_boundary_vectors();
  return 0;
}
