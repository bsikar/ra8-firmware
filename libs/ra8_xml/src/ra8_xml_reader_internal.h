/**
 * @file ra8_xml_reader_internal.h
 * @brief Private contracts for the bounded XML reader implementation.
 * @details Declares the file-local lexical and event helpers of `ra8_xml.c` so
 * their contracts remain authoritative without inflating the implementation
 * unit. The entity/UTF-8 decoding half of the reader lives in
 * `ra8_xml_decode.c` and documents its helpers at their definitions.
 * [Ring 3 / LIB] {World: NS}
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_xml.h"

/**
 * @brief Test whether one byte is XML spacing.
 * @details Recognises only space, tab, carriage return, and line feed.
 * @param[in] c Byte to classify.
 * @return True exactly for supported XML spacing.
 * @retval true @p c is XML spacing.
 * @retval false @p c is not XML spacing.
 * @pre @p c is an unsigned source byte.
 * @pre Locale-independent ASCII classification is required.
 * @post No memory is modified.
 * @post The result depends only on @p c.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_space(uint8_t c);

/**
 * @brief Test whether one byte is an ASCII letter.
 * @details Performs locale-independent XML-name classification.
 * @param[in] c Byte to classify.
 * @return True for `A-Z` or `a-z`.
 * @retval true @p c is an ASCII letter.
 * @retval false @p c is outside both letter ranges.
 * @pre @p c is an unsigned source byte.
 * @pre Locale-dependent ctype state is not consulted.
 * @post No memory is modified.
 * @post The result depends only on @p c.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_ascii_letter(uint8_t c);

/**
 * @brief Test a supported ASCII NCName start byte.
 * @details Accepts an ASCII letter or underscore.
 * @param[in] c Byte to classify.
 * @return True exactly for a supported component start.
 * @retval true @p c can begin an NCName component.
 * @retval false @p c cannot begin one.
 * @pre @p c is an unsigned source byte.
 * @pre The caller applies the documented ASCII QName subset.
 * @post No memory is modified.
 * @post The result depends only on @p c.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_name_start(uint8_t c);

/**
 * @brief Test a supported later ASCII NCName byte.
 * @details Adds digits, dot, and dash to the start-byte set.
 * @param[in] c Byte to classify.
 * @return True exactly for a supported continuation.
 * @retval true @p c can continue an NCName component.
 * @retval false @p c cannot continue one.
 * @pre @p c is an unsigned source byte.
 * @pre The caller handles namespace colons separately.
 * @post No memory is modified.
 * @post The result depends only on @p c.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_name_continue(uint8_t c);

/**
 * @brief Parse one attribute at a bounded tag cursor.
 * @details Validates QName, quoting, decoded value, and cursor progression.
 * @param[in] source Immutable XML source.
 * @param[in] source_len Exact readable source extent.
 * @param[in] end Offset of the closing tag delimiter.
 * @param[in,out] cursor Attribute cursor.
 * @param[out] out Parsed source-aliasing attribute spans.
 * @return Repository error code.
 * @retval k_ra8_ok Attribute parsed and decoded value validated.
 * @retval k_ra8_err_validation_failed Syntax or value was invalid.
 * @pre @p end and cursor position lie within @p source_len.
 * @pre Outputs are writable and source remains immutable.
 * @post Success advances cursor once and fills @p out.
 * @post Failure leaves source unchanged; cursor/output may be partial.
 * @note Caller handles duplicate-name detection separately.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_attr_parse(const uint8_t*         source,
                                                  size_t                 source_len,
                                                  uint32_t               end,
                                                  ra8_xml_attr_cursor_t* cursor,
                                                  ra8_xml_attribute_t*   out);

/**
 * @brief Detect a duplicate attribute name.
 * @details Replays prior source-order attributes and compares raw QName bytes.
 * @param[in] source Immutable XML source.
 * @param[in] source_len Exact readable source extent.
 * @param[in] event Start event being validated.
 * @param[in] current Newly parsed attribute.
 * @param[in] prior_count Number of prior attributes to compare.
 * @return True for a duplicate or inconsistent prior traversal.
 * @retval true Duplicate or malformed prior attribute detected.
 * @retval false Current name is unique among prior attributes.
 * @pre Event/current spans lie within @p source_len.
 * @pre @p prior_count does not exceed the event's parsed prefix.
 * @post No argument memory is modified.
 * @post The result is deterministic for the source/event.
 * @note Fail-closed on replay inconsistency.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_attr_duplicate(const uint8_t*             source,
                                                 size_t                     source_len,
                                                 const ra8_xml_event_t*     event,
                                                 const ra8_xml_attribute_t* current,
                                                 uint16_t                   prior_count);

/**
 * @brief Locate a markup closing byte while respecting quotes.
 * @details Ignores greater-than bytes inside single or double quoted values.
 * @param[in] source Immutable XML source.
 * @param[in] length Exact readable source extent.
 * @param[in] start Offset of the opening less-than byte.
 * @param[out] out_end Offset of the closing greater-than byte.
 * @return Repository error code.
 * @retval k_ra8_ok Closing byte located.
 * @retval k_ra8_err_validation_failed Markup was unterminated.
 * @pre @p source spans @p length bytes and @p start is in range.
 * @pre @p out_end is writable and does not overlap source.
 * @post Success sets @p out_end within the source.
 * @post Failure leaves source unchanged and output unspecified.
 * @note Lexical validation occurs in later helpers.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_markup_end(const uint8_t* source, size_t length, size_t start, size_t* out_end);

/**
 * @brief Count and validate one start tag's attributes.
 * @details Enforces unique names, quoted decoded values, and exact tag tail.
 * @param[in] source Immutable XML source.
 * @param[in] source_len Exact readable source extent.
 * @param[in,out] event Start event whose count is populated.
 * @return Repository error code.
 * @retval k_ra8_ok Attributes validated and counted.
 * @retval k_ra8_err_validation_failed Tag attributes were malformed.
 * @pre Event name/markup spans lie within @p source_len.
 * @pre @p event is writable and source remains immutable.
 * @post Success sets the exact source-order attribute count.
 * @post Failure leaves source unchanged; event count is unspecified.
 * @note Attribute count is bounded by UINT16_MAX.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_attributes(const uint8_t* source, size_t source_len, ra8_xml_event_t* event);

/**
 * @brief Parse one start element and update nesting.
 * @details Validates QName/attributes/root/depth before pushing a live frame.
 * @param[in,out] reader Active pull reader positioned at a start tag.
 * @param[in] end Offset of its closing delimiter.
 * @param[out] event Start event to populate.
 * @return Repository error code.
 * @retval k_ra8_ok Event emitted and reader advanced.
 * @retval k_ra8_err_validation_failed Tag, root, or depth was invalid.
 * @pre @p end is within the reader source and was quote-aware scanned.
 * @pre Reader workspace is exclusive and writable.
 * @post Success advances position and updates bounded nesting.
 * @post Failure leaves source unchanged; reader progress may be partial.
 * @note Self-closing root elements close immediately.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_start(ra8_xml_reader_t* reader, size_t end, ra8_xml_event_t* event);

/**
 * @brief Parse one close element and update nesting.
 * @details Allows trailing XML spacing and requires an exact open-name match.
 * @param[in,out] reader Active pull reader positioned at a close tag.
 * @param[in] end Offset of its closing delimiter.
 * @param[out] event End event to populate.
 * @return Repository error code.
 * @retval k_ra8_ok Event emitted and top frame popped.
 * @retval k_ra8_err_validation_failed Close syntax or nesting mismatched.
 * @pre @p end is within the reader source.
 * @pre Reader workspace contains every live open frame.
 * @post Success advances position and reduces nesting by one.
 * @post Failure leaves source unchanged; reader progress may be partial.
 * @note Closing the root marks the document body complete.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_end(ra8_xml_reader_t* reader, size_t end, ra8_xml_event_t* event);

/**
 * @brief Locate a literal terminator in a bounded source.
 * @details Performs a forward byte-exact search without sentinel reads.
 * @param[in] source Immutable source bytes.
 * @param[in] length Exact readable extent.
 * @param[in] start First candidate byte.
 * @param[in] terminator NUL-terminated ASCII terminator.
 * @param[out] out_start First terminator byte.
 * @return Repository error code.
 * @retval k_ra8_ok Terminator found.
 * @retval k_ra8_err_validation_failed Terminator absent.
 * @pre @p start does not exceed @p length.
 * @pre Output and literal are valid and do not overlap source mutation.
 * @post Success returns an in-range offset.
 * @post Failure leaves source and output storage unchanged.
 * @note Pure with respect to source bytes.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_terminator(const uint8_t* source,
                                                  size_t         length,
                                                  size_t         start,
                                                  const char*    terminator,
                                                  size_t*        out_start);

/**
 * @brief Validate and skip one XML comment.
 * @details Rejects internal double dash and a payload ending in dash.
 * @param[in,out] reader Active reader positioned at `<!--`.
 * @return Repository error code.
 * @retval k_ra8_ok Comment validated and skipped.
 * @retval k_ra8_err_validation_failed Comment text or terminator was invalid.
 * @pre Reader source/position describe a complete bounded document.
 * @pre Comment bytes begin at the current position.
 * @post Success advances after `-->` without emitting an event.
 * @post Failure leaves source unchanged; reader progress is unspecified.
 * @note Comment contents are not retained.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_comment(ra8_xml_reader_t* reader);

/**
 * @brief Test for the reserved case-insensitive PI target XML.
 * @details Compares exactly three bytes with ASCII case folding.
 * @param[in] source Immutable source bytes.
 * @param[in] start First target byte.
 * @param[in] end One-past-last target byte.
 * @return True exactly for any ASCII case spelling of XML.
 * @retval true Reserved target matched.
 * @retval false Length or bytes differed.
 * @pre `[start,end)` lies in readable source.
 * @pre @p start does not exceed @p end.
 * @post No memory is modified.
 * @post The result depends only on the bounded bytes.
 * @note Exact lowercase handling is decided by the PI consumer.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_xml_target(const uint8_t* source, size_t start, size_t end);

/**
 * @brief Validate an XML declaration encoding name.
 * @details Accepts exactly `UTF-8` or `utf-8` and no aliases.
 * @param[in] source Immutable XML source.
 * @param[in] value Source-relative declared encoding span.
 * @return True only for the supported encoding spellings.
 * @retval true Encoding is supported.
 * @retval false Encoding is absent from the supported set.
 * @pre @p value lies within readable source.
 * @pre Declaration attribute syntax was already validated.
 * @post No memory is modified.
 * @post The result depends only on the bounded span.
 * @note The reader always interprets source bytes as UTF-8.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_encoding(const uint8_t* source, ra8_xml_span_t value);

/**
 * @brief Validate one ordered XML-declaration pseudo-attribute.
 * @details Enforces version first, then optional encoding and standalone.
 * @param[in] source Immutable XML source.
 * @param[in] source_len Exact readable source extent.
 * @param[in] attribute Parsed pseudo-attribute.
 * @param[in] ordinal Zero-based declaration attribute index.
 * @param[in,out] saw_encoding Encoding-presence state.
 * @param[in,out] saw_standalone Standalone-presence state.
 * @return True only for a supported name/value/order combination.
 * @retval true Attribute was accepted and state updated.
 * @retval false Attribute was invalid or out of order.
 * @pre Attribute spans lie within @p source_len.
 * @pre State flags belong to this declaration pass.
 * @post Success advances only the matching presence flag.
 * @post Source bytes are unchanged on every result.
 * @note Version is restricted to 1.0.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_declaration_attr(const uint8_t*             source,
                                                   size_t                     source_len,
                                                   const ra8_xml_attribute_t* attribute,
                                                   uint16_t                   ordinal,
                                                   bool*                      saw_encoding,
                                                   bool*                      saw_standalone);

/**
 * @brief Validate and skip the leading XML declaration.
 * @details Enforces byte-zero/BOM-zero placement and ordered declaration grammar.
 * @param[in,out] reader Active reader at the lowercase `xml` target.
 * @param[in] target_end One-past-last target byte.
 * @param[in] term First byte of `?>`.
 * @return Repository error code.
 * @retval k_ra8_ok Declaration accepted and skipped.
 * @retval k_ra8_err_validation_failed Placement or grammar was invalid.
 * @pre Target/terminator offsets lie within the reader source.
 * @pre Reader declaration/root state reflects all prior bytes.
 * @post Success marks the declaration seen and advances position.
 * @post Failure leaves source unchanged; reader scratch may be partial.
 * @note Only XML 1.0 with absent or exact UTF-8 encoding is accepted.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_declaration(ra8_xml_reader_t* reader, size_t target_end, size_t term);

/**
 * @brief Validate and skip a PI or leading XML declaration.
 * @details QName-validates the target and reserves case-insensitive XML.
 * @param[in,out] reader Active reader positioned at `<?`.
 * @return Repository error code.
 * @retval k_ra8_ok Instruction/declaration validated and skipped.
 * @retval k_ra8_err_validation_failed Target, data, or placement was invalid.
 * @pre Reader source/position describe a complete bounded document.
 * @pre PI opener begins at the current position.
 * @post Success advances after `?>` without emitting an event.
 * @post Failure leaves source unchanged; reader scratch may be partial.
 * @note Non-XML processing instructions may appear wherever XML permits.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_pi(ra8_xml_reader_t* reader);

/**
 * @brief Validate and emit one CDATA payload.
 * @details CDATA is accepted only within a live element and aliases source.
 * @param[in,out] reader Active reader positioned at `<![CDATA[`.
 * @param[out] event CDATA event to populate.
 * @return Repository error code.
 * @retval k_ra8_ok Payload validated and emitted.
 * @retval k_ra8_err_validation_failed Placement, bytes, or terminator was invalid.
 * @pre Reader source/position describe a complete bounded document.
 * @pre @p event is writable and does not overlap reader/source.
 * @post Success advances after `]]>` and returns a bounded payload span.
 * @post Failure leaves source unchanged; reader/event may be partial.
 * @note Consumers may choose to ignore CDATA events.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cdata(ra8_xml_reader_t* reader, ra8_xml_event_t* event);

/**
 * @brief Dispatch comment, CDATA, DOCTYPE, or processing instruction.
 * @details Fails closed for unsupported declarations and malformed openers.
 * @param[in,out] reader Active reader positioned at special markup.
 * @param[out] event Optional semantic event storage.
 * @param[out] out_emitted Whether @p event contains CDATA.
 * @return Repository error code.
 * @retval k_ra8_ok Supported markup validated and consumed.
 * @retval k_ra8_err_validation_failed Unsupported or malformed markup.
 * @pre Reader source/position describe a complete bounded document.
 * @pre Outputs are writable and do not overlap reader/source.
 * @post Success advances reader and accurately reports event emission.
 * @post Failure leaves source unchanged; output/reader scratch may be partial.
 * @note DOCTYPE handling never fetches an external resource.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_special(ra8_xml_reader_t* reader, ra8_xml_event_t* event, bool* out_emitted);

/**
 * @brief Validate and emit one text run.
 * @details Validates UTF-8/entities, rejects raw `]]>`, and enforces outer S.
 * @param[in,out] reader Active reader positioned at text.
 * @param[out] event Text event to populate.
 * @return Repository error code.
 * @retval k_ra8_ok Text span validated and emitted.
 * @retval k_ra8_err_validation_failed Text bytes or document placement was invalid.
 * @pre Reader source/position describe a complete bounded document.
 * @pre @p event is writable and does not overlap reader/source.
 * @post Success advances to markup/EOF and returns a bounded span.
 * @post Failure leaves source unchanged; reader/event may be partial.
 * @note Inter-element whitespace is preserved as text events.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_text(ra8_xml_reader_t* reader, ra8_xml_event_t* event);
