/**
 * @file ra8_rabook_comic.h
 * @brief Streaming image-sequence builder for reader-native RABOOK comics.
 * @ingroup grp_ereader
 *
 * @details Converts one encoded page at a time into a canonical RABOOK1 book.
 * JPEG, PNG, GIF, BMP, and WebP pages pass through the shared caller-arena
 * raster normalizer. Normalized image bytes are written to an external spool,
 * while only bounded chapter, DOM, image, and string tables remain resident.
 * The completed flat book is emitted through a caller append callback and can
 * then be wrapped by ::ra8_rabook_container_write into the RBKC `.rabook`
 * artifact. No filesystem, transport, heap, or global workspace is owned here.
 *
 * [Ring 3 / RABOOK Compiler] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_rabook_compile.h"
#include "ra8_rabook_raster.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Bounded string dimensions accepted by the comic facade. */
typedef enum : uint16_t {
  k_ra8_rabook_comic_string_max = 512U, /**< Bytes including the terminating NUL. */
} ra8_rabook_comic_limit_t;

/**
 * @brief Write normalized image bytes at one logical spool offset.
 * @param[in,out] ctx Caller-owned spool context.
 * @param[in] offset Logical image-pool byte offset.
 * @param[in] source Source bytes to write.
 * @param[in] requested Exact requested byte count.
 * @param[out] out_written Actual bytes accepted.
 * @return Backend status; callback-specific failures propagate unchanged.
 * @pre Pointers are non-NULL and @p source spans @p requested readable bytes.
 * @pre The destination supports bounded writes at arbitrary uint32 offsets.
 * @post The comic builder rejects a successful short write.
 * @post No byte before @p offset is modified by a conforming callback.
 * @note Not thread-safe unless the callback context is synchronized.
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_rabook_comic_write_at_fn)(void*          ctx,
                                                  uint32_t       offset,
                                                  const uint8_t* source,
                                                  uint32_t       requested,
                                                  uint32_t*      out_written);

/**
 * @struct ra8_rabook_comic_metadata_t
 * @brief Header metadata interned when the comic is finalized.
 * @invariant Every member is NUL-terminated within
 *            ::k_ra8_rabook_comic_string_max bytes.
 * @since 0.1.0
 */
typedef struct {
  const char* title;      /**< Human-readable book title.       */
  const char* author;     /**< Human-readable author or source. */
  const char* language;   /**< BCP-47 language spelling.        */
  const char* identifier; /**< Stable book identifier.          */
} ra8_rabook_comic_metadata_t;

/**
 * @struct ra8_rabook_comic_workspace_t
 * @brief Caller-owned arenas retained for one streaming comic build.
 * @details Builder output and image-pool arenas may each be a one-byte dummy:
 *          this facade reserves external images and uses
 *          ::ra8_rabook_finalize_stream rather than resident finalization.
 * @invariant Every builder arena satisfies ::ra8_rabook_buffers_t.
 * @invariant Raster and normalized storage are pairwise non-overlapping.
 * @invariant Stream scratch is non-NULL and has nonzero capacity.
 * @since 0.1.0
 */
typedef struct {
  ra8_rabook_buffers_t          builder;            /**< Resident table/string arenas. */
  ra8_rabook_raster_workspace_t raster;             /**< Codec and scaling arenas.     */
  uint8_t*                      normalized;         /**< One normalized page payload.  */
  size_t                        normalized_cap;     /**< Writable normalized bytes.    */
  uint8_t*                      stream_scratch;     /**< Finalizer transfer scratch.   */
  uint32_t                      stream_scratch_cap; /**< Finalizer scratch bytes.      */
} ra8_rabook_comic_workspace_t;

/**
 * @struct ra8_rabook_comic_t
 * @brief Mutable state for one image-sequence-to-RABOOK build.
 * @details Initialize with ::ra8_rabook_comic_init and treat every field as
 *          private until ::ra8_rabook_comic_finish returns.
 * @invariant `page_count == builder.chapter_count == builder.image_count`.
 * @invariant `active` is false after the first terminal failure or finish.
 * @since 0.1.0
 */
typedef struct {
  ra8_rabook_ctx_t              builder;            /**< Canonical table builder.       */
  ra8_rabook_raster_workspace_t raster;             /**< Retained raster workspace.     */
  ra8_rabook_image_read_fn      image_read;         /**< Repeatable image-spool reader. */
  ra8_rabook_comic_write_at_fn  image_write;        /**< Image-spool random writer.     */
  void*                         image_ctx;          /**< Shared image-spool context.    */
  uint8_t*                      normalized;         /**< One normalized page payload.   */
  uint8_t*                      stream_scratch;     /**< Finalizer transfer scratch.    */
  size_t                        normalized_cap;     /**< Normalized output capacity.    */
  uint32_t                      stream_scratch_cap; /**< Finalizer scratch capacity.    */
  uint32_t                      page_count;         /**< Successfully appended pages.   */
  uint16_t                      max_image_edge;     /**< Optional output edge clamp.    */
  uint8_t                       pixel_format;       /**< Gray4 or gray8 output depth.   */
  bool                          active;             /**< Build accepts more operations. */
} ra8_rabook_comic_t;

/**
 * @brief Initialize one bounded streaming comic builder.
 * @param[out] comic Caller-owned builder state.
 * @param[in] workspace Complete caller-owned arenas and scratch.
 * @param[in] image_read Repeatable reader for the normalized image spool.
 * @param[in] image_write Random writer for the normalized image spool.
 * @param[in,out] image_ctx Context shared by the two spool callbacks.
 * @param[in] max_image_edge Optional longer-edge clamp; zero preserves size.
 * @param[in] pixel_format ::k_ra8_book_pixfmt_gray4 or gray8.
 * @return Initialization status.
 * @retval k_ra8_ok The empty comic is ready for pages.
 * @retval k_ra8_err_null_ptr A required pointer, callback, or arena is NULL.
 * @retval k_ra8_err_invalid_arg The requested pixel format is unsupported.
 * @retval k_ra8_err_invalid_size A required scratch capacity is zero.
 * @return Builder initialization failures propagate unchanged.
 * @pre Workspace spans are disjoint and exclusively owned for this build.
 * @pre Callback context remains live until finish returns.
 * @post Success sets `active` and records zero pages.
 * @post Failure leaves @p comic zeroed and inactive.
 * @note Not thread-safe because raster codec arena bindings are global.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rabook_comic_init(ra8_rabook_comic_t*                 comic,
                                              const ra8_rabook_comic_workspace_t* workspace,
                                              ra8_rabook_image_read_fn            image_read,
                                              ra8_rabook_comic_write_at_fn        image_write,
                                              void*                               image_ctx,
                                              uint16_t                            max_image_edge,
                                              uint8_t                             pixel_format);

/**
 * @brief Normalize and append one source image as one comic chapter.
 * @param[in,out] comic Active comic build.
 * @param[in] page_id Stable NUL-terminated page href/title.
 * @param[in] source Complete encoded JPEG, PNG, GIF, BMP, or WebP bytes.
 * @param[in] source_size Readable source byte count.
 * @return Page conversion and append status.
 * @retval k_ra8_ok One image, DOM chapter, and spool span were appended.
 * @retval k_ra8_err_invalid_state The build is inactive.
 * @retval k_ra8_err_invalid_arg An input is empty or invalid.
 * @retval k_ra8_err_invalid_size @p page_id lacks a bounded terminator.
 * @retval k_ra8_err_no_mem A builder, codec, or normalized arena is too small.
 * @return Raster, spool, or builder errors otherwise propagate unchanged.
 * @pre @p source remains readable and disjoint from comic scratch for the call.
 * @pre Page identifiers are unique within one comic.
 * @post Success increments page, chapter, and image counts exactly once.
 * @post Failure makes the build inactive; the caller discards its private spools.
 * @note The first successfully appended page becomes the cover at finish.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rabook_comic_add_page(ra8_rabook_comic_t* comic,
                                                  const char*         page_id,
                                                  const uint8_t*      source,
                                                  size_t              source_size);

/**
 * @brief Finalize the completed comic as one canonical flat RABOOK1 stream.
 * @param[in,out] comic Active comic containing at least one page.
 * @param[in] metadata Complete bounded header metadata.
 * @param[in] write Exact append callback for the flat output.
 * @param[in,out] write_ctx Context passed to @p write.
 * @param[out] out_len Exact flat RABOOK1 byte length.
 * @return Metadata, finalization, or callback status.
 * @retval k_ra8_ok A complete canonical flat book was appended.
 * @retval k_ra8_err_invalid_state The build is inactive.
 * @retval k_ra8_err_empty No page was appended.
 * @retval k_ra8_err_invalid_size A metadata string lacks a bounded terminator.
 * @return Builder and callback failures otherwise propagate unchanged.
 * @pre The image spool exposes every normalized byte written by add-page calls.
 * @pre @p write appends to a private destination that may be discarded on error.
 * @post The build is inactive on every return.
 * @post Success sets @p out_len to the exact emitted byte count.
 * @note Wrap the flat result with ::ra8_rabook_container_write for `.rabook`.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rabook_comic_finish(ra8_rabook_comic_t*                comic,
                                                const ra8_rabook_comic_metadata_t* metadata,
                                                ra8_rabook_write_fn                write,
                                                void*                              write_ctx,
                                                uint32_t*                          out_len);

#ifdef __cplusplus
} /* extern "C" */
#endif
