/**
 * @file ra8_xml.h
 * @brief Bounded, caller-owned, no-heap XML pull reader.
 * @ingroup grp_ereader
 *
 * @details The reader validates and scans immutable XML bytes without building
 * a DOM. Element nesting lives in an explicit caller-owned workspace. Event
 * and attribute spans alias the input and remain valid for its lifetime. The
 * supported markup-name grammar is an explicit ASCII QName subset: one or two
 * NCName components separated by at most one interior colon, each beginning
 * with `[A-Za-z_]` and continuing with `[A-Za-z0-9_.-]`. Text and attribute
 * values accept canonical UTF-8 XML 1.0 characters and the five predefined or
 * numeric character references. One leading UTF-8 BOM and a beginning-only
 * XML 1.0 declaration with absent or exact `UTF-8`/`utf-8` encoding are
 * supported. Before the root, a bare, SYSTEM, or PUBLIC external-only DOCTYPE
 * is lexically validated and ignored; no external resource is fetched and no
 * entity is expanded. Internal subsets, entity declarations, and every other
 * declaration fail closed. Comments, processing instructions, and in-element
 * CDATA are validated without becoming consumer-owned storage.
 *
 * [Ring 3 / LIB] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Fixed parser geometry. */
typedef enum : uint16_t {
  k_ra8_xml_max_element_depth = 499U, /**< Accepted element levels, including root. */
  k_ra8_xml_workspace_frames  = 512U, /**< Physical frame capacity with headroom.   */
} ra8_xml_limits_t;

/** @brief Immutable byte span expressed relative to the source. */
typedef struct {
  uint32_t offset; /**< First byte offset. */
  uint32_t length; /**< Byte count.        */
} ra8_xml_span_t;

/** @brief One open-element identity retained for close-tag validation. */
typedef struct {
  uint32_t name_offset; /**< Element-name offset in the source.       */
  uint16_t name_length; /**< Element-name byte count.                 */
  uint16_t consumer;    /**< Consumer-owned while this frame is live. */
} ra8_xml_frame_t;

/** @brief Exactly bounded caller-owned nesting storage. */
typedef struct {
  ra8_xml_frame_t frames[k_ra8_xml_workspace_frames]; /**< Open-element stack. */
} ra8_xml_workspace_t;

/** @brief Pull-event kind. */
typedef enum : uint8_t {
  k_ra8_xml_event_none  = 0U, /**< No event / end of source. */
  k_ra8_xml_event_start = 1U, /**< Element start.            */
  k_ra8_xml_event_end   = 2U, /**< Element end.              */
  k_ra8_xml_event_text  = 3U, /**< Character-data run.       */
  k_ra8_xml_event_cdata = 4U, /**< CDATA payload.            */
} ra8_xml_event_kind_t;

/** @brief One XML pull event. */
typedef struct {
  ra8_xml_span_t markup;          /**< Complete markup span, or text payload. */
  ra8_xml_span_t name;            /**< Element name for start/end.            */
  uint16_t       depth;           /**< Root is depth zero.                    */
  uint16_t       attribute_count; /**< Source-order attributes on start.      */
  uint8_t        kind;            /**< ::ra8_xml_event_kind_t.                */
  uint8_t        self_closing;    /**< One for an empty-element start.        */
} ra8_xml_event_t;

/** @brief Source-order attribute view. */
typedef struct {
  ra8_xml_span_t name;  /**< Attribute name.                    */
  ra8_xml_span_t value; /**< Quoted value excluding delimiters. */
} ra8_xml_attribute_t;

/** @brief Mutable cursor for one start event's attributes. */
typedef struct {
  uint32_t position; /**< Next scan position.          */
  uint16_t emitted;  /**< Attributes already returned. */
} ra8_xml_attr_cursor_t;

/** @brief Pull-reader state; initialise before each pass. */
typedef struct {
  const uint8_t*       source;           /**< Immutable source bytes.        */
  size_t               source_len;       /**< Source byte count.             */
  size_t               position;         /**< Next scan position.            */
  ra8_xml_workspace_t* workspace;        /**< Caller-owned stack.            */
  uint16_t             stack_size;       /**< Open element count.            */
  uint8_t              root_count;       /**< Completed/started roots.       */
  uint8_t              root_closed;      /**< One after root close.          */
  uint8_t              declaration_seen; /**< One after XML declaration.     */
  uint8_t              doctype_seen;     /**< One after a supported DOCTYPE. */
  uint8_t              finished;         /**< End validation complete.       */
} ra8_xml_reader_t;

/**
 * @brief Initialise a pull pass over immutable bytes.
 * @param[out] reader Reader state to initialise.
 * @param[in] source Immutable complete XML byte sequence.
 * @param[in] source_len Exact readable extent of @p source in bytes.
 * @param[in,out] workspace Exclusive caller-owned element-stack storage.
 * @retval k_ra8_ok Reader ready.
 * @retval k_ra8_err_null_ptr A required pointer is NULL.
 * @retval k_ra8_err_invalid_size @p source_len is zero or exceeds UINT32_MAX.
 * @pre @p source remains readable and immutable for the complete pull pass.
 * @pre @p source_len is the source's true readable extent, not a sentinel length.
 * @pre @p workspace is not shared with another live reader.
 * @post On success @p reader owns no source bytes and starts before the first event.
 * @post On failure @p source and @p workspace are unchanged.
 * @note A leading UTF-8 BOM is consumed only when it begins at byte zero.
 * @note Thread-safe for distinct readers, sources, and workspaces.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xml_reader_init(ra8_xml_reader_t*    reader,
                                            const uint8_t*       source,
                                            size_t               source_len,
                                            ra8_xml_workspace_t* workspace);

/**
 * @brief Return the next semantic event and validate syntax incrementally.
 * @param[in,out] reader Active pull pass.
 * @param[out] out_event Next source-aliasing event.
 * @retval k_ra8_ok Event returned, or `kind==none` at validated EOF.
 * @retval k_ra8_err_null_ptr @p reader or @p out_event is NULL.
 * @retval k_ra8_err_validation_failed Malformed XML or depth overflow.
 * @pre @p reader was initialised successfully and its source remains live.
 * @post On success with a non-none event, all spans stay within `reader->source_len`.
 * @post On success with a none event, exactly one root closed and no frame is live.
 * @post The immutable source is never modified.
 * @note After a validation failure, discard the event and reinitialise the pass;
 *       reader/workspace progress is not failure-atomic.
 * @note Thread-safe for distinct reader/workspace pairs.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xml_reader_next(ra8_xml_reader_t* reader, ra8_xml_event_t* out_event);

/**
 * @brief Validate a complete document before any consumer mutation.
 * @param[in] source Immutable complete XML byte sequence.
 * @param[in] source_len Exact readable extent of @p source.
 * @param[in,out] workspace Exclusive caller-owned validation stack.
 * @retval k_ra8_ok Document is well formed within the depth bound.
 * @retval k_ra8_err_null_ptr A required pointer is NULL.
 * @retval k_ra8_err_invalid_size @p source_len is zero or exceeds UINT32_MAX.
 * @retval k_ra8_err_validation_failed Document is malformed.
 * @pre @p source spans exactly @p source_len readable bytes.
 * @pre @p workspace is not shared by another live reader.
 * @post @p source is byte-for-byte unchanged on success and failure.
 * @post Success permits a consumer to begin a fresh pass using the same workspace.
 * @note Workspace contents are scratch and unspecified after either result.
 * @note Thread-safe for distinct sources and workspaces.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_xml_validate(const uint8_t* source, size_t source_len, ra8_xml_workspace_t* workspace);

/**
 * @brief Initialise source-order attribute iteration for a start event.
 * @details Derives the first attribute position from the event's bounded name span.
 * @param[in] event Valid start event returned by the reader.
 * @param[out] cursor Attribute cursor to initialise.
 * @pre @p event aliases the same live source later passed to ::ra8_xml_attr_next.
 * @pre @p cursor does not overlap @p event.
 * @post With non-NULL arguments, @p cursor addresses the first attribute.
 * @post If either argument is NULL, no memory is modified.
 * @note Thread-safe for distinct cursors.
 * @since 0.1.0
 */
void ra8_xml_attr_begin(const ra8_xml_event_t* event, ra8_xml_attr_cursor_t* cursor);

/**
 * @brief Return the next source-order attribute.
 * @param[in] source Immutable source that produced @p event.
 * @param[in] source_len Exact readable extent of @p source.
 * @param[in] event Start event whose attributes are being traversed.
 * @param[in,out] cursor Cursor initialised by ::ra8_xml_attr_begin.
 * @param[out] out_attribute Next source-aliasing name/value spans.
 * @param[out] out_has_value False after the final attribute.
 * @retval k_ra8_ok An attribute or the clean end of the sequence was reported.
 * @retval k_ra8_err_null_ptr A required pointer is NULL.
 * @retval k_ra8_err_validation_failed An event/span/cursor is stale or malformed.
 * @pre @p source spans exactly @p source_len readable bytes and remains live.
 * @pre @p event came from that same source and @p cursor belongs to @p event.
 * @post On success with `*out_has_value`, spans are within @p source_len and the
 *       cursor advances once; on clean end, @p out_attribute is unchanged.
 * @post @p source and @p event are never modified.
 * @note On failure, discard @p cursor and @p out_attribute; progress may be partial.
 * @note Thread-safe for distinct cursors.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xml_attr_next(const uint8_t*         source,
                                          size_t                 source_len,
                                          const ra8_xml_event_t* event,
                                          ra8_xml_attr_cursor_t* cursor,
                                          ra8_xml_attribute_t*   out_attribute,
                                          bool*                  out_has_value);

/**
 * @brief Compare a bounded span with an exact ASCII literal.
 * @param[in] source Immutable source containing @p span.
 * @param[in] source_len Exact readable extent of @p source.
 * @param[in] span Candidate source-relative byte span.
 * @param[in] literal NUL-terminated ASCII literal.
 * @return True only for equal byte length and contents.
 * @retval false A pointer is NULL, @p span is out of range, or bytes differ.
 * @pre @p source spans @p source_len bytes when non-NULL.
 * @post No argument memory is modified.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_xml_span_equal(const uint8_t* source,
                                      size_t         source_len,
                                      ra8_xml_span_t span,
                                      const char*    literal);

/**
 * @brief Compare the namespace-local tail of a bounded span with an ASCII literal.
 * @param[in] source Immutable source containing @p span.
 * @param[in] source_len Exact readable extent of @p source.
 * @param[in] span Candidate QName span.
 * @param[in] literal NUL-terminated local-name literal.
 * @return True only when the bytes after the optional colon equal @p literal.
 * @retval false A pointer is NULL, @p span is out of range, or bytes differ.
 * @pre @p source spans @p source_len bytes when non-NULL.
 * @post No argument memory is modified.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_xml_span_local_equal(const uint8_t* source,
                                            size_t         source_len,
                                            ra8_xml_span_t span,
                                            const char*    literal);

/**
 * @brief Compare two entity-decoded spans from the same immutable source.
 * @param[in] source Immutable source containing both spans.
 * @param[in] source_len Exact readable extent of @p source.
 * @param[in] left First source-relative span.
 * @param[in] right Second source-relative span.
 * @return True only when both spans are valid and decode to identical UTF-8 bytes.
 * @retval false @p source is NULL, a span/entity is invalid, or values differ.
 * @pre @p source spans @p source_len bytes when non-NULL.
 * @post No argument memory is modified.
 * @note Pure, allocation-free, and thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_xml_decoded_equal(const uint8_t* source,
                                         size_t         source_len,
                                         ra8_xml_span_t left,
                                         ra8_xml_span_t right);

/**
 * @brief Entity-decode a source span into a bounded NUL-terminated buffer.
 * @param[in] source Immutable source containing @p span.
 * @param[in] source_len Exact readable extent of @p source.
 * @param[in] span Source-relative encoded text span.
 * @param[out] destination Decoded UTF-8 destination.
 * @param[in] capacity Writable destination capacity including the NUL.
 * @param[out] out_length Decoded bytes excluding the NUL.
 * @retval k_ra8_ok Decoded completely.
 * @retval k_ra8_err_null_ptr A required pointer is NULL.
 * @retval k_ra8_err_no_mem Destination too small.
 * @retval k_ra8_err_validation_failed @p span, UTF-8, or an entity is invalid.
 * @pre @p source spans @p source_len readable bytes.
 * @pre @p destination spans @p capacity writable bytes and does not overlap source.
 * @post On success @p destination is NUL-terminated and `*out_length < capacity`.
 * @post @p source is unchanged on every result.
 * @note On failure @p destination may contain an unterminated prefix and
 *       @p out_length is unspecified; the operation is not output-atomic.
 * @note Thread-safe for distinct buffers.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xml_decode(const uint8_t* source,
                                       size_t         source_len,
                                       ra8_xml_span_t span,
                                       char*          destination,
                                       size_t         capacity,
                                       size_t*        out_length);

/**
 * @brief Measure the entity-decoded byte count of a bounded span.
 * @param[in] source Immutable source containing @p span.
 * @param[in] source_len Exact readable extent of @p source.
 * @param[in] span Source-relative encoded text span.
 * @param[out] out_length Exact decoded UTF-8 byte count excluding any NUL.
 * @retval k_ra8_ok Complete span measured.
 * @retval k_ra8_err_null_ptr @p source or @p out_length is NULL.
 * @retval k_ra8_err_validation_failed @p span, UTF-8, or an entity is invalid.
 * @pre @p source spans @p source_len readable bytes.
 * @post On success @p out_length is exact; on failure it is unspecified.
 * @post @p source is unchanged.
 * @note Pure, allocation-free, and thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xml_decoded_size(const uint8_t* source,
                                             size_t         source_len,
                                             ra8_xml_span_t span,
                                             size_t*        out_length);

/**
 * @brief Decode the longest complete prefix that fits the destination.
 * @details The prefix never splits an entity or UTF-8 sequence, and the entire
 *          encoded span is still validated after output clips.
 * @param[in] source Immutable source containing @p span.
 * @param[in] source_len Exact readable extent of @p source.
 * @param[in] span Source-relative encoded text span.
 * @param[out] destination Decoded UTF-8 prefix destination.
 * @param[in] capacity Writable destination capacity including the NUL.
 * @param[out] out_length Decoded bytes excluding the NUL.
 * @retval k_ra8_ok Complete validation and maximal prefix decode succeeded.
 * @retval k_ra8_err_null_ptr A required pointer is NULL.
 * @retval k_ra8_err_no_mem @p capacity is zero.
 * @retval k_ra8_err_validation_failed @p span, UTF-8, or an entity is invalid.
 * @pre @p source spans @p source_len readable bytes.
 * @pre @p destination spans @p capacity writable bytes and does not overlap source.
 * @post On success @p destination is NUL-terminated, `*out_length < capacity`,
 *       and no partial decoded codepoint/entity is present.
 * @post @p source is unchanged on every result.
 * @note On validation failure @p destination may contain a prefix and
 *       @p out_length is unspecified; the operation is not output-atomic.
 * @note Thread-safe for distinct buffers.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_xml_decode_prefix(const uint8_t* source,
                                              size_t         source_len,
                                              ra8_xml_span_t span,
                                              char*          destination,
                                              size_t         capacity,
                                              size_t*        out_length);

#ifdef __cplusplus
} /* extern "C" */
#endif
