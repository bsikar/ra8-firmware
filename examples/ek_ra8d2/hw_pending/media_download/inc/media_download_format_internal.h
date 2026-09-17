/**
 * @file media_download_format_internal.h
 * @brief Private source-image to RBKC formatter for the media app.
 * @details Declares the caller-workspace contract that lets the RA8 normalize
 *          one fetched image into a reader-native RABOOK1 book and wrap it as
 *          the chunked RBKC `.rabook` artifact saved by the application.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_rabook_comic.h"
#include "ra8_rabook_container.h"

/**
 * @struct media_download_format_config_t
 * @brief Immutable policy for one fetched-image conversion.
 * @invariant String pointers are NUL-terminated within the comic string cap.
 * @invariant `chunk_bytes` is nonzero and fits container input storage.
 * @since 0.1.0
 */
typedef struct {
  const char*                 page_id;        /**< Stable page identifier. */
  ra8_rabook_comic_metadata_t metadata;       /**< Book header metadata.   */
  uint32_t                    chunk_bytes;    /**< RBKC chunk geometry.    */
  uint16_t                    max_image_edge; /**< Output edge clamp.      */
  uint8_t                     pixel_format;   /**< Gray4 or gray8 output.  */
} media_download_format_config_t;

/**
 * @struct media_download_format_workspace_t
 * @brief Complete caller-owned storage for image-to-RBKC conversion.
 * @invariant Image, flat, and packed spans are pairwise non-overlapping.
 * @invariant Comic and container workspaces satisfy their public contracts.
 * @since 0.1.0
 */
typedef struct {
  ra8_rabook_comic_workspace_t     comic;      /**< Builder and raster arenas.  */
  ra8_rabook_container_workspace_t container;  /**< Chunk-compressor workspace. */
  uint8_t*                         image;      /**< Normalized image spool.     */
  uint8_t*                         flat;       /**< Flat RABOOK1 spool.         */
  uint8_t*                         packed;     /**< Final RBKC destination.     */
  uint32_t                         image_cap;  /**< Image-spool capacity.       */
  uint32_t                         flat_cap;   /**< Flat-spool capacity.        */
  uint32_t                         packed_cap; /**< RBKC capacity.              */
} media_download_format_workspace_t;

/**
 * @struct media_download_memory_t
 * @brief Mutable bounds for one formatter-owned in-memory spool.
 * @invariant `length <= capacity`.
 * @invariant A nonzero capacity has a writable @c bytes backing span.
 * @since 0.1.0
 */
typedef struct {
  uint8_t* bytes;    /**< Writable backing bytes.            */
  uint32_t capacity; /**< Maximum accessible byte extent.    */
  uint32_t length;   /**< Highest initialized byte plus one. */
} media_download_memory_t;

/**
 * @brief Write one exact span at an absolute private-spool offset.
 * @param[in,out] memory Destination spool.
 * @param[in] offset Absolute destination offset.
 * @param[in] source Complete source span.
 * @param[in] requested Exact byte count.
 * @param[out] written Accepted byte count.
 * @return Memory-write status.
 * @retval k_ra8_ok The complete span was copied.
 * @retval k_ra8_err_null_ptr A required pointer was NULL.
 * @retval k_ra8_err_invalid_size The requested range exceeded capacity.
 * @pre Non-NULL pointers span their declared extents.
 * @pre Source and destination ranges do not overlap.
 * @post Success reports @p requested and extends length monotonically.
 * @post Failure reports zero when @p written is available and preserves length.
 * @note Private formatter seam exposed only to same-module tests.
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t
priv_media_download_memory_write_at(media_download_memory_t* memory,
                                    uint64_t                 offset,
                                    const uint8_t*           source,
                                    uint32_t                 requested,
                                    uint32_t*                written);

/**
 * @brief Read one exact initialized range from a private spool.
 * @param[in] context Bound ::media_download_memory_t source.
 * @param[in] offset Absolute source offset.
 * @param[out] destination Writable destination span.
 * @param[in] requested Exact byte count.
 * @param[out] out_read Copied byte count.
 * @return Memory-read status.
 * @retval k_ra8_ok The complete range was copied.
 * @retval k_ra8_err_null_ptr A required pointer was NULL.
 * @retval k_ra8_err_invalid_size The requested range exceeded initialized bytes.
 * @pre Context and destination remain live for the call.
 * @pre Destination does not overlap the selected source range.
 * @post Success reports @p requested and initializes the destination range.
 * @post Failure reports zero when @p out_read is available and preserves source state.
 * @note Private formatter seam exposed only to same-module tests.
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t priv_media_download_memory_read(void*     context,
                                                                 uint32_t  offset,
                                                                 uint8_t*  destination,
                                                                 uint32_t  requested,
                                                                 uint32_t* out_read);

/**
 * @brief Format one encoded source image as a reader-native `.rabook`.
 * @param[in] source Complete encoded JPEG, PNG, GIF, BMP, or WebP bytes.
 * @param[in] source_size Exact encoded byte count.
 * @param[in] config Page, book, raster, and chunk policy.
 * @param[in,out] workspace Exclusive caller-owned conversion storage.
 * @param[out] packed_size Exact RBKC artifact byte count.
 * @return Conversion status.
 * @retval k_ra8_ok A complete RBKC artifact occupies `workspace->packed`.
 * @retval k_ra8_err_null_ptr A required pointer or workspace span is NULL.
 * @retval k_ra8_err_invalid_size A capacity, source, or callback extent failed.
 * @return Raster, builder, compressor, and validation statuses propagate.
 * @pre Workspace spans are live, writable, and mutually non-overlapping.
 * @pre @p source is disjoint from every mutable workspace span.
 * @post Success initializes exactly @p packed_size bytes of packed storage.
 * @post Failure sets @p packed_size to zero and publishes no external object.
 * @note The caller must strictly validate before transactionally publishing.
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t
priv_media_download_format_rabook(const uint8_t*                        source,
                                  size_t                                source_size,
                                  const media_download_format_config_t* config,
                                  media_download_format_workspace_t*    workspace,
                                  uint64_t*                             packed_size);
