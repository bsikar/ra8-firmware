/**
 * @file ra8_epub_xml_consumer_internal.h
 * @brief Private contracts for streamed EPUB XML consumers.
 * @details Declares file-local helpers and their caller-owned workspace rules;
 * definitions defer here so generated documentation has one authoritative block.
 * [Ring 4 / EPUB] {World: NS}
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_epub_xml_shim_internal.h"
#include "ra8_xml.h"

/** @brief Optional attribute span. */
typedef struct {
  ra8_xml_span_t span;    /**< Value span.        */
  bool           present; /**< Attribute existed. */
} priv_attr_t;

/** @brief Mutable state for one validated EPUB navigation pass. */
typedef struct {
  const uint8_t*    source;     /**< Immutable navigation document.          */
  size_t            source_len; /**< Navigation-document byte count.         */
  ra8_epub_book_t*  book;       /**< TOC output being populated.             */
  ra8_xml_reader_t* reader;     /**< Reader whose live frames carry markers. */
  ra8_xml_event_t   selected;   /**< Selected navigation element.            */
  bool              active;     /**< Selected subtree has begun.             */
  bool              saw_ol;     /**< Selected navigation contains its list.  */
} priv_nav_ctx_t;

/** @brief Mutable state for one validated NCX navigation-map pass. */
typedef struct {
  const uint8_t*    source;     /**< Immutable NCX document.                 */
  size_t            source_len; /**< NCX-document byte count.                */
  ra8_epub_book_t*  book;       /**< TOC output being populated.             */
  ra8_xml_reader_t* reader;     /**< Reader whose live frames carry markers. */
  ra8_xml_event_t   selected;   /**< Selected navigation-map element.        */
  bool              active;     /**< Selected subtree has begun.             */
} priv_ncx_ctx_t;

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
RA8_INTERNAL static void internal_copy(const uint8_t* source,
                                       size_t         source_len,
                                       ra8_xml_span_t span,
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
RA8_INTERNAL static priv_attr_t internal_attr(const uint8_t*         source,
                                              size_t                 source_len,
                                              const ra8_xml_event_t* event,
                                              const char*            name);

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
RA8_INTERNAL static bool internal_attr_contains(const uint8_t* source,
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
RA8_INTERNAL static ra8_xml_span_t internal_frame_name(const ra8_xml_reader_t* reader,
                                                       uint16_t                frame);

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
RA8_INTERNAL static bool internal_direct_child(uint16_t child, uint16_t parent);

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
RA8_INTERNAL static bool
internal_ancestor_frame(uint16_t depth, uint16_t levels, uint16_t* out_frame);

/**
 * @brief Return a checked ancestor name span.
 * @details Converts an invalid ancestor request to an empty span.
 * @param[in] reader Active reader.
 * @param[in] depth Current event depth.
 * @param[in] levels Ancestor distance.
 * @return Ancestor name or an empty span.
 * @retval span Source-relative name when the ancestor exists.
 * @pre Reader workspace contains live frames for @p depth.
 * @pre Source remains live and immutable.
 * @post Reader/workspace bytes are unchanged.
 * @post Returned nonempty span aliases source.
 * @note Pure with respect to parser state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_xml_span_t
internal_ancestor_name(const ra8_xml_reader_t* reader, uint16_t depth, uint16_t levels);

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
RA8_INTERNAL static uint16_t
internal_ancestor_marker(const ra8_xml_reader_t* reader, uint16_t depth, uint16_t levels);

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
RA8_INTERNAL static ra8_err_t internal_reader(ra8_xml_reader_t*         reader,
                                              const uint8_t*            source,
                                              size_t                    length,
                                              ra8_epub_xml_workspace_t* workspace);

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
RA8_INTERNAL static ra8_err_t internal_find(const uint8_t*            source,
                                            size_t                    length,
                                            ra8_epub_xml_workspace_t* workspace,
                                            const char*               local,
                                            bool                      use_parent,
                                            uint32_t                  parent_offset,
                                            uint16_t                  parent_depth,
                                            ra8_xml_event_t*          out);

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
RA8_INTERNAL static void internal_manifest_item(const uint8_t*         source,
                                                size_t                 source_len,
                                                const ra8_xml_event_t* event,
                                                ra8_epub_book_t*       book);

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
RA8_INTERNAL static void internal_metadata_text(const uint8_t*          source,
                                                size_t                  source_len,
                                                const ra8_xml_event_t*  event,
                                                const ra8_xml_reader_t* reader,
                                                ra8_epub_book_t*        book);

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
RA8_INTERNAL static void internal_mark_metadata(const uint8_t*         source,
                                                size_t                 source_len,
                                                const ra8_xml_event_t* event,
                                                ra8_xml_reader_t*      reader,
                                                ra8_xml_span_t         unique_id);

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
RA8_INTERNAL static ra8_err_t internal_opf_first(const uint8_t*   source,
                                                 size_t           length,
                                                 ra8_epub_book_t* book,
                                                 ra8_xml_span_t*  out_spine_toc);

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
internal_collect_spine(const uint8_t* source, size_t length, ra8_epub_book_t* book);

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
RA8_INTERNAL static ra8_err_t internal_manifest_lookup(const uint8_t*            source,
                                                       size_t                    length,
                                                       ra8_epub_xml_workspace_t* workspace,
                                                       ra8_xml_span_t            wanted,
                                                       ra8_xml_span_t*           out_href);

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
internal_opf_shape(const uint8_t* source, size_t length, ra8_epub_xml_workspace_t* workspace);

/**
 * @brief Count local-name ancestors of a TOC entry type.
 * @details Uses live reader frames to derive flattened TOC depth.
 * @param[in] source Immutable navigation source.
 * @param[in] source_len Exact source extent.
 * @param[in] reader Active reader.
 * @param[in] depth Current event depth.
 * @param[in] local NUL-terminated ancestor local name.
 * @return Bounded matching ancestor count.
 * @retval count Count narrowed only within accepted reader depth.
 * @pre Reader frames belong to @p source and are live through @p depth.
 * @pre @p local remains readable for the call.
 * @post No input memory is modified.
 * @post Result preserves legacy uint8_t TOC depth semantics.
 * @note Accepted parser depth bounds make the narrowing deterministic.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t internal_ancestor_depth(const uint8_t*          source,
                                                    size_t                  source_len,
                                                    const ra8_xml_reader_t* reader,
                                                    uint16_t                depth,
                                                    const char*             local);

/**
 * @brief Reserve one preflight-proven TOC slot and mark its frame.
 * @details Clears the slot, derives depth, and retains a one-based marker.
 * @param[in] source Immutable navigation source.
 * @param[in] source_len Exact source extent.
 * @param[in] event Valid TOC-entry start event.
 * @param[in,out] reader Active reader whose frame receives the marker.
 * @param[in,out] book TOC output.
 * @param[in] local Entry local-name literal.
 * @return One-based TOC marker, or zero when no capacity remains.
 * @retval 0 No slot was reserved.
 * @pre ::internal_toc_capacity succeeded for this selected subtree.
 * @pre Event/frame/source coordinates are consistent.
 * @post Success increments TOC count and initializes exactly one slot.
 * @post Non-self-closing entries retain the marker on their live frame.
 * @note Capacity failure is defensive after mandatory preflight.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t internal_toc_reserve(const uint8_t*         source,
                                                  size_t                 source_len,
                                                  const ra8_xml_event_t* event,
                                                  ra8_xml_reader_t*      reader,
                                                  ra8_epub_book_t*       book,
                                                  const char*            local);

/**
 * @brief Find the nearest marked ancestor frame.
 * @details Walks toward the root and returns the first one-based TOC marker.
 * @param[in] reader Active navigation reader.
 * @param[in] depth Current event depth.
 * @return Nearest marker or zero.
 * @retval 0 No marked ancestor exists.
 * @pre Reader workspace has live frames below @p depth.
 * @pre Consumer marker zero remains the unassigned sentinel.
 * @post Reader/workspace bytes are unchanged.
 * @post Return is a valid one-based TOC slot when nonzero.
 * @note Pure with respect to parser state.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t internal_toc_marker(const ra8_xml_reader_t* reader, uint16_t depth);

/**
 * @brief Prove a selected TOC subtree fits before changing the book.
 * @details Counts entry starts, optionally only after a direct ordered list.
 * @param[in] source Immutable navigation source.
 * @param[in] length Exact source extent.
 * @param[in,out] workspace Exclusive reader scratch.
 * @param[in] selected Selected nav/navMap start event.
 * @param[in] entry_local NUL-terminated entry local name.
 * @param[in] require_list Require a direct `ol` before counting entries.
 * @return Repository error code.
 * @retval k_ra8_ok Entry count fits the fixed TOC capacity.
 * @retval k_ra8_err_no_mem One entry beyond capacity was observed.
 * @pre Source is fully validated and selection aliases it.
 * @pre Workspace is exclusive; strings remain readable.
 * @post Success proves emission cannot overflow TOC slots.
 * @post Failure leaves all semantic book fields untouched.
 * @note Workspace bytes are scratch on return.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_toc_capacity(const uint8_t*            source,
                                                    size_t                    length,
                                                    ra8_epub_xml_workspace_t* workspace,
                                                    const ra8_xml_event_t*    selected,
                                                    const char*               entry_local,
                                                    bool                      require_list);

/**
 * @brief Consume one event from the selected NCX navigation map.
 * @details Maintains frame markers and copies title/content into reserved entries.
 * @param[in,out] ctx Active NCX consumer state.
 * @param[in] event Next validated pull event.
 * @pre TOC capacity was preflighted and @p ctx owns the reader/book.
 * @pre Event aliases the same immutable source.
 * @post Matching events update at most one reserved TOC entry.
 * @post Source bytes remain unchanged.
 * @note This emission pass has no remaining error path after preflight.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_ncx_event(priv_ncx_ctx_t* ctx, const ra8_xml_event_t* event);

/**
 * @brief Select typed TOC nav, falling back to the first nav descendant.
 * @details Prefers an `epub:type` containing `toc` in document order.
 * @param[in] source Immutable navigation source.
 * @param[in] length Exact source extent.
 * @param[in,out] workspace Exclusive reader scratch.
 * @param[out] out Selected nav start event.
 * @return Repository error code.
 * @retval k_ra8_ok A nav was selected.
 * @retval k_ra8_err_validation_failed No nav exists or replay failed.
 * @pre Source passed complete XML validation.
 * @pre Output/workspace are writable and source remains live.
 * @post Success fills a bounded source-aliasing event.
 * @post Failure leaves source unchanged and output unspecified.
 * @note Substring token policy preserves legacy EPUB behavior.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_select_nav(const uint8_t*            source,
                                                  size_t                    length,
                                                  ra8_epub_xml_workspace_t* workspace,
                                                  ra8_xml_event_t*          out);

/**
 * @brief Require a direct ordered-list child before TOC mutation.
 * @details Replays only the selected nav region and fails before emission.
 * @param[in] source Immutable navigation source.
 * @param[in] length Exact source extent.
 * @param[in,out] workspace Exclusive reader scratch.
 * @param[in] selected Selected nav start event.
 * @return Repository error code.
 * @retval k_ra8_ok Direct ordered-list child exists.
 * @retval k_ra8_err_validation_failed List is absent or replay failed.
 * @pre Source is fully validated and selection aliases it.
 * @pre Workspace is exclusive and writable.
 * @post Semantic TOC fields remain unchanged.
 * @post Source bytes remain unchanged.
 * @note Workspace bytes are scratch on return.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_nav_has_list(const uint8_t*            source,
                                                    size_t                    length,
                                                    ra8_epub_xml_workspace_t* workspace,
                                                    const ra8_xml_event_t*    selected);

/**
 * @brief Consume one event from the selected EPUB navigation subtree.
 * @details Maintains list/entry markers and copies anchor/span labels and hrefs.
 * @param[in,out] ctx Active navigation consumer state.
 * @param[in] event Next validated pull event.
 * @pre List existence and TOC capacity were preflighted.
 * @pre Event aliases the same immutable source owned by @p ctx.
 * @post Matching events update at most one reserved TOC entry.
 * @post Source bytes remain unchanged.
 * @note This emission pass has no remaining error path after preflight.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_nav_event(priv_nav_ctx_t* ctx, const ra8_xml_event_t* event);
