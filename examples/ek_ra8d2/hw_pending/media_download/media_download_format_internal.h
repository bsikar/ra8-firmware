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
