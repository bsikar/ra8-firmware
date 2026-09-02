/**
 * @file book_stream_internal.h
 * @brief Private wire geometry for strict RABOOK1 stream validation.
 *
 * @details
 * Centralizes fixed wire offsets, bit geometry, and the bounded caller-owned
 * validation state used only by the stream validator implementation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 */
#pragma once

#include "book_stream.h"
#include "ra8_attributes.h"

/** @brief Wire offsets inside the fixed 100-byte RABOOK1 header. */
typedef enum : uint8_t {
  k_stream_hdr_version       = 8U,  /**< Format-version field byte offset. */
  k_stream_hdr_total         = 12U, /**< Total-size field byte offset.     */
  k_stream_hdr_flags         = 16U, /**< Feature-flags field byte offset.  */
  k_stream_hdr_title         = 20U, /**< Title string-offset field.        */
  k_stream_hdr_author        = 24U, /**< Author string-offset field.       */
  k_stream_hdr_language      = 28U, /**< Language string-offset field.     */
  k_stream_hdr_identifier    = 32U, /**< Identifier string-offset field.   */
  k_stream_hdr_cover         = 36U, /**< Cover-image index field.          */
  k_stream_hdr_chapter_count = 40U, /**< Chapter-table count field.        */
  k_stream_hdr_chapter_off   = 44U, /**< Chapter-table byte offset.        */
  k_stream_hdr_node_count    = 48U, /**< DOM-node table count field.       */
  k_stream_hdr_node_off      = 52U, /**< DOM-node table byte offset.       */
  k_stream_hdr_attr_count    = 56U, /**< Attribute-table count field.      */
  k_stream_hdr_attr_off      = 60U, /**< Attribute-table byte offset.      */
  k_stream_hdr_style_count   = 64U, /**< Stylesheet-table count field.     */
  k_stream_hdr_style_off     = 68U, /**< Stylesheet-table byte offset.     */
  k_stream_hdr_image_count   = 72U, /**< Image-table count field.          */
  k_stream_hdr_image_off     = 76U, /**< Image-table byte offset.          */
  k_stream_hdr_string_off    = 80U, /**< Interned-string pool offset.      */
  k_stream_hdr_string_size   = 84U, /**< Interned-string pool size.        */
  k_stream_hdr_pool_off      = 88U, /**< Image-payload pool offset.        */
  k_stream_hdr_pool_size     = 92U, /**< Image-payload pool size.          */
  k_stream_hdr_crc           = 96U, /**< Body CRC-32 field byte offset.    */
} stream_header_off_t;

/** @brief Wire offsets inside a 24-byte DOM node record. */
typedef enum : uint8_t {
  k_stream_node_kind         = 0U,  /**< Node-kind byte offset.         */
  k_stream_node_reserved     = 1U,  /**< Reserved-zero byte offset.     */
  k_stream_node_attr_count   = 2U,  /**< Attribute-count field offset.  */
  k_stream_node_name         = 4U,  /**< Element-name string offset.    */
  k_stream_node_text         = 8U,  /**< Text string-offset field.      */
  k_stream_node_first_attr   = 12U, /**< First-attribute index field.   */
  k_stream_node_first_child  = 16U, /**< First-child node index field.  */
  k_stream_node_next_sibling = 20U, /**< Next-sibling node index field. */
} stream_node_off_t;

/** @brief Wire offsets inside a 24-byte image descriptor. */
typedef enum : uint8_t {
  k_stream_image_id        = 0U,  /**< Image-id string offset.        */
  k_stream_image_width     = 4U,  /**< Raster-width field offset.     */
  k_stream_image_height    = 6U,  /**< Raster-height field offset.    */
  k_stream_image_format    = 8U,  /**< Image-kind byte offset.        */
  k_stream_image_pixfmt    = 9U,  /**< Raster pixel-format offset.    */
  k_stream_image_reserved  = 10U, /**< Reserved-zero field offset.    */
  k_stream_image_data_off  = 12U, /**< Payload-relative offset field. */
  k_stream_image_data_size = 16U, /**< Stored payload-size field.     */
  k_stream_image_raw_size  = 20U, /**< Decoded payload-size field.    */
} stream_image_off_t;

/** @brief Bit geometry shared by wire decoding and the node ownership map. */
typedef enum : uint8_t {
  k_stream_bits_per_byte = 8U,  /**< Bits represented by one byte.    */
  k_stream_mark_round    = 7U,  /**< Ceiling-division numerator bias. */
  k_stream_le_shift_3    = 24U, /**< Shift of byte three in a uint32. */
} stream_bit_t;

/**
 * @brief Decode one little-endian 16-bit field from unaligned bytes.
 * @details Combines two octets explicitly so behavior is independent of host
 *          byte order and does not require an aligned integer load.
 * @param[in] p Readable two-byte wire field.
 * @return Host-order unsigned field value.
 * @retval UINT16_C(0) Both wire bytes are zero.
 * @retval UINT16_MAX Both wire bytes are 0xff.
 * @pre @p p addresses at least two readable bytes.
 * @pre The source bytes remain stable for the duration of the two reads.
 * @post No memory or external state is modified.
 * @post The result is exactly `p[0] | (p[1] << 8)`.
 * @note Pure and thread-safe for immutable input.
 * @since Version 0.1.0
 */
RA8_INTERNAL static inline uint16_t internal_book_stream_le16(const uint8_t* p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << k_stream_bits_per_byte));
}

/**
 * @brief Decode one little-endian 32-bit field from unaligned bytes.
 * @details Combines four octets explicitly so behavior is independent of host
 *          byte order and does not require an aligned integer load.
 * @param[in] p Readable four-byte wire field.
 * @return Host-order unsigned field value.
 * @retval UINT32_C(0) All four wire bytes are zero.
 * @retval UINT32_MAX All four wire bytes are 0xff.
 * @pre @p p addresses at least four readable bytes.
 * @pre The source bytes remain stable for the duration of the four reads.
 * @post No memory or external state is modified.
 * @post The result is the exact canonical little-endian decoding.
 * @note Pure and thread-safe for immutable input.
 * @since Version 0.1.0
 */
RA8_INTERNAL static inline uint32_t internal_book_stream_le32(const uint8_t* p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << k_stream_bits_per_byte) |
         ((uint32_t)p[2] << (2U * k_stream_bits_per_byte)) |
         ((uint32_t)p[3] << k_stream_le_shift_3);
}

/** @brief Immutable validation state shared by the bounded table passes. */
typedef struct {
  book_stream_read_fn read;        /**< Exact source callback.       */
  void*               read_ctx;    /**< Callback context.            */
  uint64_t            source_size; /**< Exact flat-source byte size. */
  uint8_t*            scratch;     /**< Caller transfer buffer.      */
  uint32_t            scratch_cap; /**< Transfer-buffer capacity.    */
  book_header_t       hdr;         /**< Decoded host-order header.   */
} stream_validate_t;

/**
 * @brief Read, decode, and validate the canonical stream header and layout.
 * @details Reads the fixed wire header, decodes every little-endian field, and
 *          proves that all following tables and pools form one gap-free span.
 * @param[in,out] ctx Validation state whose header receives decoded fields.
 * @return Header read and layout-validation status.
 * @retval k_ra8_ok The header and complete source layout are canonical.
 * @retval k_ra8_err_invalid_arg A magic, version, or feature invariant fails.
 * @retval k_ra8_err_invalid_size A source, scratch, table, or pool extent fails.
 * @pre @p ctx and its exact-read callback are valid.
 * @pre ctx->source_size is at least the fixed wire-header length.
 * @post Success leaves ctx->hdr fully decoded and layout-validated.
 * @post Failure does not read outside the advertised source extent.
 * @note Private translation-unit seam; performs no allocation.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_book_stream_read_validate_header(stream_validate_t* ctx);

/**
 * @brief Require a referenced offset to name an interned-string boundary.
 * @details Accepts the empty-string sentinel at zero; every other offset must
 *          be in range and immediately preceded by a NUL terminator.
 * @param[in] ctx Validation state with canonical string-pool geometry.
 * @param[in] off Offset relative to the string pool.
 * @return String-boundary validation status.
 * @retval k_ra8_ok @p off names the sentinel or a valid string boundary.
 * @retval k_ra8_err_invalid_arg The offset is outside the pool or mid-string.
 * @pre @p ctx and its exact-read callback are valid.
 * @pre The string-pool envelope lies within the validated source layout.
 * @post No state is modified.
 * @post Success permits reads beginning at the referenced offset.
 * @note Private translation-unit seam; performs at most one byte read.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_book_stream_string_ref(const stream_validate_t* ctx, uint32_t off);

/**
 * @brief Require a string reference to name a non-empty interned string.
 * @details Proves the offset is a string boundary and rejects a NUL first byte.
 * @param[in] ctx Validation state with canonical string-pool geometry.
 * @param[in] off Offset relative to the string pool.
 * @return Non-empty string-boundary validation status.
 * @retval k_ra8_ok The reference begins with a non-NUL byte.
 * @retval k_ra8_err_invalid_arg The reference is invalid or names an empty string.
 * @pre @p ctx and its exact-read callback are valid.
 * @pre The string-pool envelope lies within the validated source layout.
 * @post No state is modified.
 * @post Success proves at least one non-NUL byte follows the boundary.
 * @note Private translation-unit seam; performs at most two byte reads.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_book_stream_nonempty_string_ref(const stream_validate_t* ctx, uint32_t off);

/**
 * @brief Read one exact, bounded source span.
 * @details Private test seam for the overflow-safe source-range guard used by
 * every streamed validation pass.
 * @param[in] ctx Validation source.
 * @param[in] off Source byte offset.
 * @param[out] dst Destination buffer.
 * @param[in] len Exact byte count.
 * @return Callback status or invalid-size for an out-of-range span.
 * @retval k_ra8_ok The callback supplied the requested bytes.
 * @retval k_ra8_err_invalid_size The offset or length exceeds the source.
 * @pre All pointers are non-NULL and @p dst holds @p len bytes.
 * @pre The callback obeys the exact-read contract.
 * @post Rejected spans do not invoke the callback.
 * @post Success fills exactly @p len bytes.
 * @note Private MC/DC seam; performs no allocation.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_book_stream_read(const stream_validate_t* ctx,
                                         uint64_t                 off,
                                         uint8_t*                 dst,
                                         uint32_t                 len);

/**
 * @brief Validate the string pool's leading and trailing NUL sentinels.
 * @details Private test seam for the read-status and two boundary-byte
 * conditions used before any string reference is accepted.
 * @param[in] ctx Validation state with canonical string-pool geometry.
 * @return String-envelope validation status.
 * @retval k_ra8_ok Both sentinel bytes are NUL.
 * @retval k_ra8_err_invalid_size The pool is empty or unreadable.
 * @retval k_ra8_err_invalid_arg One sentinel byte is non-NUL.
 * @pre @p ctx and its read callback are valid.
 * @pre The advertised string-pool extent lies within the source.
 * @post No source or validation state is modified.
 * @post Success establishes both string-pool sentinels.
 * @note Private MC/DC seam; performs at most two reads.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_book_stream_validate_string_envelope(const stream_validate_t* ctx);

/**
 * @brief Validate metadata string references and the optional cover index.
 * @details Private test seam for the nil-cover and image-count bounds policy.
 * @param[in] ctx Validation state with a checked string envelope.
 * @return Metadata validation status.
 * @retval k_ra8_ok All references are valid.
 * @retval k_ra8_err_invalid_arg A string or cover reference is invalid.
 * @pre Header string offsets and image count are decoded.
 * @pre The string pool has valid boundary sentinels.
 * @post No state is modified.
 * @post Success proves every metadata reference safe.
 * @note Private MC/DC seam; uses only caller-owned state.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_book_stream_validate_metadata(const stream_validate_t* ctx);

/**
 * @brief Validate one element node and its canonical attribute span.
 * @details Private test seam for first-attribute and remaining-count guards.
 * @param[in] ctx Validation state with canonical strings and attributes.
 * @param[in] rec One complete element-node wire record.
 * @param[in,out] attr_cursor Next unowned attribute index.
 * @return Element validation status.
 * @retval k_ra8_ok The element and attribute span are canonical.
 * @retval k_ra8_err_invalid_arg One element invariant is invalid.
 * @pre All pointers are valid for their documented extents.
 * @pre @p attr_cursor does not exceed the attribute count.
 * @post Success consumes exactly the element's attribute span.
 * @post Failure does not advance beyond the advertised count.
 * @note Private MC/DC seam; mutates only @p attr_cursor.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_book_stream_validate_element(const stream_validate_t* ctx,
                                                     const uint8_t*           rec,
                                                     uint32_t*                attr_cursor);

/**
 * @brief Validate one text node's element-only fields and string reference.
 * @details Private test seam for the four-condition text-node invariant.
 * @param[in] ctx Validation state with a checked string envelope.
 * @param[in] rec One complete text-node wire record.
 * @return Text-node validation status.
 * @retval k_ra8_ok All element-only fields are empty or nil.
 * @retval k_ra8_err_invalid_arg One field or string reference is invalid.
 * @pre Both pointers address their documented extents.
 * @pre @p rec has the text node kind.
 * @post No state is modified.
 * @post Success proves the text node owns no children or attributes.
 * @note Private MC/DC seam; uses only caller-owned state.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_book_stream_validate_text(const stream_validate_t* ctx, const uint8_t* rec);

/**
 * @brief Validate every stylesheet source and optional chapter scope.
 * @details Private test seam for read status, nil scope, and chapter bounds.
 * @param[in] ctx Validation state with canonical stylesheet geometry.
 * @return Stylesheet-table validation status.
 * @retval k_ra8_ok Every source and optional scope is valid.
 * @retval k_ra8_err_invalid_arg One source or scope is invalid.
 * @pre @p ctx and its callback are valid.
 * @pre Header layout bounds every stylesheet record.
 * @post No state is modified.
 * @post Success proves each stylesheet reference safe.
 * @note Private MC/DC seam; iteration is stylesheet-count bounded.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_book_stream_validate_styles(const stream_validate_t* ctx);
