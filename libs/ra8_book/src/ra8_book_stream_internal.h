/**
 * @file ra8_book_stream_internal.h
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

#include "ra8_book_stream.h"

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

/** @brief Immutable validation state shared by the bounded table passes. */
typedef struct {
  ra8_book_stream_read_fn read;        /**< Exact source callback.       */
  void*                   read_ctx;    /**< Callback context.            */
  uint64_t                source_size; /**< Exact flat-source byte size. */
  uint8_t*                scratch;     /**< Caller transfer buffer.      */
  uint32_t                scratch_cap; /**< Transfer-buffer capacity.    */
  ra8_book_header_t       hdr;         /**< Decoded host-order header.   */
} stream_validate_t;
