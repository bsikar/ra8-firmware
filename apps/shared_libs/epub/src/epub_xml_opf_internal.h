/**
 * @file epub_xml_opf_internal.h
 * @brief Private contracts for the EPUB container and OPF consumers.
 * @details Declares the file-local helpers of `epub_xml_shim.c`; the
 * helpers it shares with the table-of-contents pass stay in
 * `epub_xml_consumer_internal.h`, which this header includes.
 * [Ring 4 / EPUB] {World: NS}
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "epub_xml_consumer_internal.h"
#include "ra8_attributes.h"
#include "xml.h"

/**
 * @brief Recognise one supported embedded-font media type.
 * @details Performs exact byte comparisons against the fixed supported set.
 * @param[in] source Immutable XML source.
 * @param[in] source_len Exact source extent.
 * @param[in] media Optional encoded media-type attribute.
 * @return True only for a supported exact media type.
 * @retval false Attribute is absent, out of range, or unsupported.
 * @pre Present span lies within @p source_len.
 * @pre Source remains readable for the call.
 * @post No input memory is modified.
 * @post Result is deterministic for the span.
 * @note No MIME parameter parsing is performed.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_font_type(const uint8_t* source, size_t source_len, priv_attr_t media);

/**
 * @brief Collect one manifest item and its derived resource roles.
 * @details Updates bounded manifest/font/cover/nav fields using decoded prefixes.
 * @param[in] source Immutable XML source.
 * @param[in] source_len Exact source extent.
 * @param[in] event Valid manifest item start.
 * @param[in,out] book EPUB semantic output.
 * @pre The document and event were fully validated.
 * @pre @p book is writable and owns its workspace.
 * @post Resource fields reflect this item within fixed capacities.
 * @post Source bytes remain unchanged.
 * @note Overflow beyond legacy manifest/font caps is ignored deterministically.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_manifest_item(const uint8_t*     source,
                                                size_t             source_len,
                                                const xml_event_t* event,
                                                epub_book_t*       book);

/**
 * @brief Record metadata text based on its live parent marker.
 * @details Copies the first applicable title/creator/language/identifier field.
 * @param[in] source Immutable XML source.
 * @param[in] source_len Exact source extent.
 * @param[in] event Valid text event.
 * @param[in] reader Reader whose live frames carry markers.
 * @param[in,out] book EPUB semantic output.
 * @pre Event/frames come from the same validated source.
 * @pre @p book is writable and distinct from source.
 * @post A matching metadata field receives a decoded bounded prefix.
 * @post Nonmatching events leave semantic fields unchanged.
 * @note Source bytes are never modified.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_metadata_text(const uint8_t*      source,
                                                size_t              source_len,
                                                const xml_event_t*  event,
                                                const xml_reader_t* reader,
                                                epub_book_t*        book);

/**
 * @brief Mark a metadata child frame for later text consumption.
 * @details Distinguishes the package unique identifier from fallback identifiers.
 * @param[in] source Immutable XML source.
 * @param[in] source_len Exact source extent.
 * @param[in] event Valid metadata-child start event.
 * @param[in,out] reader Reader whose frame receives the marker.
 * @param[in] unique_id Encoded package identifier reference.
 * @pre Event/frame spans lie within the same validated source.
 * @pre Event is non-self-closing and its frame is live.
 * @post The event frame carries exactly one metadata marker.
 * @post Source bytes remain unchanged.
 * @note Marker zero means no metadata role.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_mark_metadata(const uint8_t*     source,
                                                size_t             source_len,
                                                const xml_event_t* event,
                                                xml_reader_t*      reader,
                                                xml_span_t         unique_id);

/**
 * @brief Preserve parser status while enforcing required OPF sections.
 * @details Converts a successful pass missing manifest/spine to validation failure.
 * @param[in] err Existing parser status.
 * @param[in] manifest_depth Manifest depth or UINT16_MAX sentinel.
 * @param[in] spine_depth Spine depth or UINT16_MAX sentinel.
 * @return Original or derived repository error.
 * @retval k_ra8_err_validation_failed Required section was absent.
 * @pre Sentinel semantics are shared by the OPF pass.
 * @pre @p err is a repository error code.
 * @post Non-success input status is preserved exactly.
 * @post No memory is modified.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_opf_status(ra8_err_t err, uint16_t manifest_depth, uint16_t spine_depth);

/**
 * @brief Run the OPF metadata/manifest/cover/font/TOC-source pass.
 * @details Consumes only the already-validated and capacity-preflighted document.
 * @param[in] source Immutable OPF source.
 * @param[in] length Exact source extent.
 * @param[in,out] book EPUB semantic output and reader workspace.
 * @param[out] out_spine_toc Encoded spine TOC identifier span.
 * @return Repository error code.
 * @retval k_ra8_ok Required sections consumed.
 * @retval k_ra8_err_validation_failed Required shape replay failed.
 * @pre ::internal_opf_shape succeeded on this source/book workspace.
 * @pre Output storage is writable and source remains live.
 * @post Success updates bounded semantic fields and output span.
 * @post Source bytes remain unchanged on every result.
 * @note Public caller owns failure atomicity through preflight.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_opf_first(const uint8_t* source,
                                                 size_t         length,
                                                 epub_book_t*   book,
                                                 xml_span_t*    out_spine_toc);

/**
 * @brief Collect exact unprefixed spine itemrefs in source order.
 * @details Stores bounded encoded idref spans in the caller-owned workspace.
 * @param[in] source Immutable OPF source.
 * @param[in] length Exact source extent.
 * @param[in,out] book EPUB book and scratch workspace.
 * @return Repository error code.
 * @retval k_ra8_ok Complete spine collected.
 * @retval k_ra8_err_no_mem Chapter-reference capacity was exceeded.
 * @pre Source passed XML and OPF-shape validation.
 * @pre Book workspace is exclusive and writable.
 * @post Success records exact reference count and spans.
 * @post Failure leaves source unchanged; workspace scratch may be partial.
 * @note Semantic chapter fields are not changed here.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_collect_spine(const uint8_t* source, size_t length, epub_book_t* book);

/**
 * @brief Resolve one manifest identifier across the uncapped source manifest.
 * @details Compares decoded IDs and returns the matching encoded href span.
 * @param[in] source Immutable OPF source.
 * @param[in] length Exact source extent.
 * @param[in,out] workspace Exclusive reader scratch.
 * @param[in] wanted Encoded identifier span.
 * @param[out] out_href Matching encoded href span.
 * @return Repository error code.
 * @retval k_ra8_ok Matching item found.
 * @retval k_ra8_err_no_data No item matched.
 * @pre Source passed complete validation and spans share its coordinates.
 * @pre Output/workspace are writable and source remains live.
 * @post Success fills a bounded source-aliasing href span.
 * @post Failure leaves source unchanged and output unspecified.
 * @note Workspace is scratch on return.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_manifest_lookup(const uint8_t*        source,
                                                       size_t                length,
                                                       epub_xml_workspace_t* workspace,
                                                       xml_span_t            wanted,
                                                       xml_span_t*           out_href);

/**
 * @brief Validate OPF structure and spine capacity before semantic mutation.
 * @details Requires direct manifest/spine sections and counts resolving itemrefs.
 * @param[in] source Immutable OPF source.
 * @param[in] length Exact source extent.
 * @param[in,out] workspace Exclusive reader scratch.
 * @return Repository error code.
 * @retval k_ra8_ok Shape and fixed chapter capacity fit.
 * @retval k_ra8_err_no_mem Spine reference capacity was exceeded.
 * @pre Source already passed complete XML validation.
 * @pre Workspace is exclusive and writable.
 * @post Success proves later semantic passes cannot hit chapter capacity.
 * @post Source remains unchanged; workspace is scratch.
 * @note This pass provides OPF failure atomicity.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_opf_shape(const uint8_t* source, size_t length, epub_xml_workspace_t* workspace);
