/**
 * @file ra8_viewer_reader_internal.h
 * @brief Cross-translation-unit state for the bounded JOF/comic viewer.
 * @details Shares only caller-bound reader state and explicitly attributed
 * RA8_PRIV engine seams between dispatch, JOF, and comic translation units.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_comic.h"
#include "ra8_decomp_limits.h"
#include "ra8_err.h"
#include "ra8_jof.h"
#include "ra8_longstrip.h"
#include "ra8_tile_cache.h"
#include "ra8_viewer_reader.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Stable constants used by requirements and bind. */
typedef enum : uint32_t {
  k_viewer_jof_cells      = 1U,          /**< One decoded band resident.       */
  k_viewer_jof_buckets    = 64U,         /**< Power-of-two cache bucket count. */
  k_viewer_layout_version = 0x56570002U, /**< Workspace layout ABI guard.      */
  k_viewer_background     = 0x00FFFFFFU, /**< White graphics clear colour.     */
  k_viewer_white_byte     = 0xFFU,       /**< Byte fill for white RGB565.      */
} ra8_viewer_budget_t;

/** @brief Fixed bounded slices for the bare comic engine. */
typedef enum : uint32_t {
  k_viewer_comic_page_cap    = 8192U,              /**< Maximum indexed pages.     */
  k_viewer_comic_name_bytes  = 512U * 1024U,       /**< Member-name arena bytes.   */
  k_viewer_comic_page_bytes  = 4U * 1024U * 1024U, /**< Encoded-page resident cap. */
  k_viewer_comic_arena_bytes = 8U * 1024U * 1024U, /**< stb decode arena capacity. */
} ra8_viewer_comic_budget_t;

/** @brief Raw host descriptor behind the reusable positional-read callback. */
typedef struct {
  uint64_t size;    /**< Document length in bytes.    */
  int      fd;      /**< Owned descriptor while open. */
  bool     is_open; /**< Descriptor ownership flag.   */
} viewer_file_ctx_t;

/** @brief Caller-bound JOF engine and one-band cache backing. */
typedef struct {
  ra8_longstrip_decode_ctx_t dctx;        /**< JOF decoder callback state.     */
  ra8_tile_cache_t           cache;       /**< One-band keyed cache.           */
  ra8_longstrip_t            strip;       /**< Scroll/composite engine.        */
  uint8_t*                   cells;       /**< One decoded-band buffer.        */
  ra8_keycache_cell_t*       meta;        /**< Single cache metadata entry.    */
  ra8_tile_key_t*            keys;        /**< Single cache key.               */
  ra8_tile_dims_t*           dims;        /**< Single cache dimensions entry.  */
  int32_t*                   buckets;     /**< Hash bucket heads.              */
  uint8_t*                   scratch;     /**< Compressed-band staging.        */
  size_t                     cell_cap;    /**< Bound decoded-band bytes.       */
  size_t                     scratch_cap; /**< Bound compressed staging bytes. */
  uint32_t                   viewport_h;  /**< Page height on the strip.       */
  int32_t                    x_off;       /**< Horizontal target centring.     */
} viewer_jof_t;

/** @brief Bare comic engine and every caller-bound backing slice. */
typedef struct {
  ra8_comic_t       archive;   /**< Open CBZ/CBR/CBT facade.         */
  ra8_comic_page_t* pages;     /**< Resident sorted page index.      */
  char*             names;     /**< Resident member-name arena.      */
  uint8_t*          page;      /**< Resident encoded-image bytes.    */
  uint8_t*          arena;     /**< stb caller-owned decode arena.   */
  size_t            page_cap;  /**< Encoded-image capacity in bytes. */
  size_t            arena_cap; /**< Decode-arena capacity in bytes.  */
} viewer_comic_t;

/** @brief Bound reader state; every buffer is borrowed from one workspace. */
struct ra8_viewer_reader {
  viewer_file_ctx_t   file;     /**< Raw source descriptor.              */
  bool                is_bound; /**< Successful workspace bind flag.     */
  bool                is_open;  /**< Successful engine-open flag.        */
  uint32_t            engine;   /**< ::ra8_viewer_reader_engine_t value. */
  ra8_decomp_limits_t limits;   /**< Shared untrusted-input policy.      */
  uint16_t*           fb;       /**< Fixed headless framebuffer.         */
  uint32_t*           tile_wpx; /**< Native width per viewport tile.     */
  uint32_t*           tile_hpx; /**< Native height per viewport tile.    */
  uint32_t            tile_cap; /**< Bound dimension entries.            */
  uint32_t            tile_n;   /**< Populated dimension entries.        */
  uint16_t*           rt565;    /**< Current composite target.           */
  uint32_t            rt_w;     /**< Current target width.               */
  uint32_t            rt_h;     /**< Current target height.              */
  viewer_jof_t        jof;      /**< JOF engine and caller-bound cache.  */
  viewer_comic_t      comic;    /**< Comic engine and caller slices.     */
};

/** @brief JOF positional-reader seam over ::viewer_file_ctx_t. */
RA8_PRIV [[nodiscard]] ra8_err_t
priv_viewer_pread(void* ctx, uint64_t offset, uint8_t* buffer, size_t length, size_t* out_read);

/** @brief Open and wire the JOF engine over a bound reader. */
RA8_PRIV [[nodiscard]] ra8_err_t priv_viewer_open_jof(ra8_viewer_reader_t* reader);

/**
 * @brief Adapt exact descriptor reads to the comic size-returning seam.
 * @details Delegates to the reader's positional I/O contract and converts a
 * successful exact read to its byte count, while every failure becomes zero.
 * @param[in] ctx Bound ::viewer_file_ctx_t.
 * @param[in] offset Absolute source offset.
 * @param[out] buffer Destination for exactly @p length bytes.
 * @param[in] length Requested byte count.
 * @return Exact bytes read or zero on any descriptor/read failure.
 * @retval 0 The positional read failed or could not complete exactly.
 * @pre @p ctx is non-NULL and names an open viewer descriptor.
 * @pre @p buffer spans @p length writable bytes when length is non-zero.
 * @post Success initializes exactly @p length destination bytes.
 * @post Failure publishes no partial byte count to the comic facade.
 * @note Not thread-safe; shares descriptor state with the reader.
 * @since 0.1.0
 */
RA8_PRIV size_t priv_viewer_comic_read(void* ctx, uint64_t offset, void* buffer, size_t length);

/** @brief Open a bare CBZ/CBR/CBT engine over a bound reader. */
RA8_PRIV [[nodiscard]] ra8_err_t priv_viewer_open_comic(ra8_viewer_reader_t* reader);

/** @brief Render one JOF viewport into the fixed framebuffer. */
RA8_PRIV [[nodiscard]] ra8_err_t priv_viewer_render_jof(ra8_viewer_reader_t* reader, uint32_t page);

/** @brief Render one comic page into the fixed framebuffer. */
RA8_PRIV [[nodiscard]] ra8_err_t priv_viewer_render_comic(ra8_viewer_reader_t* reader,
                                                          uint32_t             page);

/** @brief Render one JOF viewport into caller-owned RGB565 output. */
RA8_PRIV [[nodiscard]] ra8_err_t priv_viewer_tile_jof(ra8_viewer_reader_t* reader,
                                                      uint32_t             index,
                                                      uint16_t*            pixels,
                                                      size_t               pixel_bytes,
                                                      uint32_t*            width,
                                                      uint32_t*            height);

/** @brief Render one comic page into caller-owned RGB565 storage. */
RA8_PRIV [[nodiscard]] ra8_err_t priv_viewer_tile_comic(ra8_viewer_reader_t* reader,
                                                        uint32_t             index,
                                                        uint16_t*            pixels,
                                                        size_t               pixel_bytes,
                                                        uint32_t*            width,
                                                        uint32_t*            height);

/** @brief Probe every comic page into the bound dimension arrays. */
RA8_PRIV [[nodiscard]] ra8_err_t priv_viewer_size_comic_tiles(ra8_viewer_reader_t* reader);

/**
 * @brief Populate native tile dimensions after the strip opens.
 * @details Clips the last viewport to the remaining canvas rows.
 * @param[in,out] reader Open JOF reader.
 * @param[in] count Number of dimension entries to populate.
 * @pre @p reader has parsed JOF geometry.
 * @pre @p count does not exceed the bound tile capacity.
 * @post Every width equals the strip width.
 * @post Every height is non-zero and at most the viewport height.
 * @note Not thread-safe; mutates reader dimension arrays.
 * @since 0.1.0
 */
RA8_PRIV void priv_viewer_size_jof_tiles(ra8_viewer_reader_t* reader, uint32_t count);

#ifdef __cplusplus
}
#endif
