/**
 * @file ra8_epub_xml_toc_internal.h
 * @brief Private contracts for the EPUB NCX and nav table-of-contents consumers.
 * @details Declares the walk state and the file-local helpers of
 * `ra8_epub_xml_toc.c`; the helpers both it and the container/OPF pass call
 * stay in `ra8_epub_xml_consumer_internal.h`, which this header includes.
 * [Ring 4 / EPUB] {World: NS}
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_epub_xml_consumer_internal.h"
#include "ra8_xml.h"

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
