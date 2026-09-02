/**
 * @file epub_xml_consumer_internal.h
 * @brief Private contracts for streamed EPUB XML consumers.
 * @details Declares the helpers every consumer unit shares and their
 * caller-owned workspace rules; definitions defer here so generated
 * documentation has one authoritative block. Each unit's own file-local
 * helpers are declared beside it, in `epub_xml_opf_internal.h` and
 * `epub_xml_toc_internal.h`, so neither unit sees a `static` prototype
 * it does not define.
 * [Ring 4 / EPUB] {World: NS}
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "epub_xml_shim_internal.h"
#include "ra8_attributes.h"
#include "xml.h"

/** @brief Optional attribute span. */
typedef struct {
  xml_span_t span;    /**< Value span.        */
  bool       present; /**< Attribute existed. */
} priv_attr_t;

/**
 * @brief Copy decoded text into a bounded EPUB field.
 * @details Truncates only at a complete entity/codepoint, matching legacy fields.
 * @param[in] source Immutable XML source.
 * @param[in] source_len Exact source extent.
 * @param[in] span Encoded source-relative span.
 * @param[out] destination Bounded NUL-terminated output.
 * @param[in] capacity Writable capacity including NUL.
 * @pre @p span lies within @p source_len validated bytes.
 * @pre Destination does not overlap source and has nonzero capacity.
 * @post Destination is NUL-terminated with a maximal complete prefix.
 * @post Source bytes remain unchanged.
 * @note Decode cannot fail after document validation.
 * @since 0.1.0
 */
RA8_PRIV void priv_epub_xml_copy(const uint8_t* source,
                                 size_t         source_len,
                                 xml_span_t     span,
                                 char*          destination,
                                 size_t         capacity);

/**
 * @brief Find an exact-name attribute on one start event.
 * @details Traverses source order and returns an optional source-aliasing span.
 * @param[in] source Immutable XML source.
 * @param[in] source_len Exact source extent.
 * @param[in] event Valid start event.
 * @param[in] name NUL-terminated exact QName.
 * @return Optional attribute span.
 * @retval present Attribute matched.
 * @pre Event spans lie within @p source_len.
 * @pre Source and @p name remain readable for the call.
 * @post No input memory is modified.
 * @post Absent result carries an empty span.
 * @note Fail-closed if attribute replay is inconsistent.
 * @since 0.1.0
 */
RA8_PRIV priv_attr_t priv_epub_xml_attr(const uint8_t*     source,
                                        size_t             source_len,
                                        const xml_event_t* event,
                                        const char*        name);

/**
 * @brief Test whether a decoded attribute contains an ASCII literal.
 * @details Decodes into the fixed EPUB path bound before substring search.
 * @param[in] source Immutable XML source.
 * @param[in] source_len Exact source extent.
 * @param[in] attribute Optional encoded attribute.
 * @param[in] needle NUL-terminated search literal.
 * @return True only when the complete decoded value contains @p needle.
 * @retval false Attribute is absent, oversized, invalid, or does not contain it.
 * @pre Present span lies within @p source_len.
 * @pre @p needle remains readable for the call.
 * @post No input memory is modified.
 * @post Temporary decoded bytes do not escape the call.
 * @note Search semantics intentionally preserve the legacy substring policy.
 * @since 0.1.0
 */
RA8_PRIV bool priv_epub_xml_attr_contains(const uint8_t* source,
                                          size_t         source_len,
                                          priv_attr_t    attribute,
                                          const char*    needle);

/**
 * @brief Return one live reader-frame name span.
 * @details Reconstructs the source-relative span retained for close validation.
 * @param[in] reader Active reader.
 * @param[in] frame Live frame index.
 * @return Source-relative element-name span.
 * @retval span Coordinates stored in the selected frame.
 * @pre @p reader and its workspace are valid.
 * @pre @p frame is below the live stack size.
 * @post Reader/workspace bytes are unchanged.
 * @post Returned span continues to alias the reader source.
 * @note Pure with respect to parser state.
 * @since 0.1.0
 */
RA8_PRIV xml_span_t priv_epub_xml_frame_name(const xml_reader_t* reader, uint16_t frame);

/**
 * @brief Test whether a depth is exactly one level below another.
 * @details Guards UINT16_MAX before incrementing the parent depth.
 * @param[in] child Candidate child depth.
 * @param[in] parent Candidate parent depth.
 * @return True only for an exact direct-child relationship.
 * @retval false Parent is sentinel or depths are not adjacent.
 * @pre Depth values use the XML reader coordinate system.
 * @pre UINT16_MAX denotes no active parent.
 * @post No memory is modified.
 * @post Arithmetic does not wrap.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_PRIV bool priv_epub_xml_direct_child(uint16_t child, uint16_t parent);

/**
 * @brief Resolve a bounded ancestor frame index.
 * @details Subtracts one level at a time to avoid narrowing or underflow.
 * @param[in] depth Current event depth.
 * @param[in] levels Ancestor distance.
 * @param[out] out_frame Resolved frame index.
 * @return True when the ancestor exists.
 * @retval false @p levels exceeds @p depth.
 * @pre @p out_frame is writable.
 * @pre Depth and levels are reader-bounded values.
 * @post Success sets @p out_frame exactly.
 * @post Failure leaves @p out_frame unchanged.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_PRIV bool priv_epub_xml_ancestor_frame(uint16_t depth, uint16_t levels, uint16_t* out_frame);

/**
 * @brief Return a checked ancestor consumer marker.
 * @details Converts an invalid ancestor request to zero.
 * @param[in] reader Active reader.
 * @param[in] depth Current event depth.
 * @param[in] levels Ancestor distance.
 * @return Stored consumer marker or zero.
 * @retval 0 Ancestor does not exist or carries no marker.
 * @pre Reader workspace contains live frames for @p depth.
 * @pre Marker zero remains the unassigned sentinel.
 * @post Reader/workspace bytes are unchanged.
 * @post Return is bounded to uint16_t.
 * @note Pure with respect to parser state.
 * @since 0.1.0
 */
RA8_PRIV uint16_t priv_epub_xml_ancestor_marker(const xml_reader_t* reader,
                                                uint16_t            depth,
                                                uint16_t            levels);

/**
 * @brief Initialise another validated EPUB pull pass.
 * @details Binds the shared reader stack within the caller-owned EPUB workspace.
 * @param[out] reader Reader state to initialise.
 * @param[in] source Immutable complete XML source.
 * @param[in] length Exact source extent.
 * @param[in,out] workspace Exclusive EPUB XML scratch.
 * @return Repository error code.
 * @retval k_ra8_ok Reader initialised.
 * @retval k_ra8_err_invalid_size Source extent is invalid.
 * @pre All pointers are non-NULL.
 * @pre Workspace is not shared with a live pass.
 * @post Success positions reader before its first event.
 * @post Source bytes remain unchanged.
 * @note Caller already validated the same document.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_epub_xml_reader(xml_reader_t*         reader,
                                        const uint8_t*        source,
                                        size_t                length,
                                        epub_xml_workspace_t* workspace);

/**
 * @brief Find the first local-name element under an optional direct parent.
 * @details Replays the validated source without retaining a DOM.
 * @param[in] source Immutable XML source.
 * @param[in] length Exact source extent.
 * @param[in,out] workspace Exclusive reader scratch.
 * @param[in] local NUL-terminated local-name literal.
 * @param[in] use_parent Require the parent coordinates when true.
 * @param[in] parent_offset Parent start-markup offset.
 * @param[in] parent_depth Parent absolute depth.
 * @param[out] out Matching start event.
 * @return Repository error code.
 * @retval k_ra8_ok Matching element found.
 * @retval k_ra8_err_validation_failed Match absent or replay failed.
 * @pre Source passed complete validation.
 * @pre Output/workspace are writable and source stays live.
 * @post Success fills a bounded source-aliasing event.
 * @post Failure leaves source unchanged and output unspecified.
 * @note Workspace is scratch on return.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_epub_xml_find(const uint8_t*        source,
                                      size_t                length,
                                      epub_xml_workspace_t* workspace,
                                      const char*           local,
                                      bool                  use_parent,
                                      uint32_t              parent_offset,
                                      uint16_t              parent_depth,
                                      xml_event_t*          out);
