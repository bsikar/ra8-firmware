/**
 * @file inc/mg_reader.h
 * @brief Tap-driven pan/zoom viewer over a tiled manga page (ereader_manga).
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * The presentation model for the ereader_manga demo: a viewport onto a page
 * far larger than the 1024x600 panel, backed by a JOF tile atlas
 * (``ra8_tileatlas``) paged through a small fixed-budget tile cache
 * (``ra8_tile_cache``). The reader owns no hardware -- it draws into a
 * caller-supplied RGB565 framebuffer through ``ra8_gfx`` and reads decoded
 * tiles through the cache -- so the identical render runs on the cross-built
 * firmware, under board_sim, and in the host unit test.
 *
 * Navigation is discrete tap-zones (board_sim's GT911 model has no gestures):
 *   - the four screen-edge bands pan the viewport one step in that direction;
 *   - a tap anywhere in the centre toggles the zoom between 1:1 (pan a page
 *     bigger than the screen) and fit-page (the whole page decimated to fit).
 *
 * A tile is fetched, blitted, and released one at a time, so a viewport that
 * spans more tiles than the cache has cells forces LRU eviction every frame --
 * the streaming-larger-than-RAM property the tile cache exists for.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_tile_cache.h"
#include "ra8_tileatlas.h"

/**
 * @enum mg_layout_t
 * @brief Fixed chrome + navigation geometry, in panel/page pixels.
 *
 * @details The status band and minimap are panel-pixel metrics; the pan step is
 *          measured in page pixels (how far one edge tap slides the viewport);
 *          the edge band is the panel-pixel thickness of each pan tap-zone.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_mg_statusbar_h = 48U,  /**< Top status-bar band height, panel px.  */
  k_mg_pan_step    = 256U, /**< Viewport slide per edge tap, page px.  */
  k_mg_edge_band   = 176U, /**< Edge pan tap-zone thickness, panel px. */
  k_mg_minimap_w   = 104U, /**< Minimap overlay width, panel px.       */
  k_mg_minimap_h   = 140U, /**< Minimap overlay height, panel px.      */
  k_mg_minimap_pad = 14U,  /**< Minimap inset from the panel edge, px. */
  k_mg_minimap_in  = 2U,   /**< Minimap interior inset, px.            */
} mg_layout_t;

/**
 * @enum mg_zoom_t
 * @brief The two zoom states cycled by a centre tap.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_mg_zoom_full = 0U, /**< 1:1 -- one page pixel per panel pixel; panned. */
  k_mg_zoom_fit  = 1U, /**< Fit-page -- whole page decimated to the panel. */
} mg_zoom_t;

/**
 * @enum mg_zone_t
 * @brief The tap-zone a panel coordinate falls in (::mg_zone_hit result).
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_mg_zone_none      = 0U, /**< Outside every actionable zone.    */
  k_mg_zone_pan_up    = 1U, /**< Top edge band: slide viewport up. */
  k_mg_zone_pan_down  = 2U, /**< Bottom edge band: slide down.     */
  k_mg_zone_pan_left  = 3U, /**< Left edge band: slide left.       */
  k_mg_zone_pan_right = 4U, /**< Right edge band: slide right.     */
  k_mg_zone_zoom      = 5U, /**< Centre: toggle 1:1 <-> fit-page.  */
} mg_zone_t;

/**
 * @struct mg_tile_src_t
 * @brief Decode-on-miss context bound into the tile cache (DIP seam).
 *
 * @details Holds everything ::mg_tile_decode needs to page one tile out of the
 *          produced JOF atlas: the parsed geometry, the atlas positioned-read
 *          seam, and a scratch staging buffer for the deflate tile codec. The
 *          pointed-at objects must out-live the cache.
 *
 * @since 0.1.0
 */
typedef struct {
  const ra8_tileatlas_info_t* info;        /**< Parsed atlas geometry.         */
  ra8_tileatlas_pread_fn      pread;       /**< Atlas positioned-read seam.    */
  void*                       pread_ctx;   /**< Context for @c pread.          */
  uint8_t*                    scratch;     /**< Deflate stored-stream staging. */
  uint32_t                    scratch_cap; /**< Capacity of @c scratch.        */
} mg_tile_src_t;

/**
 * @struct mg_reader_cfg_t
 * @brief Everything ::mg_reader_init needs to bind a viewer to a page.
 *
 * @since 0.1.0
 */
typedef struct {
  uint16_t*                   fb;       /**< RGB565 framebuffer (panel). */
  int32_t                     fb_w;     /**< Framebuffer width, pixels.  */
  int32_t                     fb_h;     /**< Framebuffer height, pixels. */
  ra8_tile_cache_t*           cache;    /**< Tile cache over the atlas.  */
  const ra8_tileatlas_info_t* info;     /**< Parsed atlas geometry.      */
  uint32_t                    image_id; /**< Tile-cache key image id.    */
} mg_reader_cfg_t;

/**
 * @struct mg_reader_t
 * @brief Viewer state: the page geometry plus the current viewport + zoom.
 *
 * @invariant `view_x` in `[0, page_w - visible_w]` and likewise for `view_y`,
 *            with the viewport clamped inside the page on every mutation.
 *
 * @since 0.1.0
 */
typedef struct {
  uint16_t*         fb;         /**< RGB565 panel framebuffer.              */
  int32_t           fb_w;       /**< Framebuffer width, pixels.             */
  int32_t           fb_h;       /**< Framebuffer height, pixels.            */
  ra8_tile_cache_t* cache;      /**< Tile cache paging the atlas.           */
  uint32_t          image_id;   /**< Tile-cache key image id.               */
  uint16_t          page_w;     /**< Full page width, pixels.               */
  uint16_t          page_h;     /**< Full page height, pixels.              */
  uint16_t          tile_w;     /**< Atlas tile width, pixels.              */
  uint16_t          tile_h;     /**< Atlas tile height, pixels.             */
  uint16_t          tile_cols;  /**< Atlas tile columns.                    */
  uint16_t          tile_rows;  /**< Atlas tile rows.                       */
  int32_t           view_x;     /**< Viewport left in page px (1:1 anchor). */
  int32_t           view_y;     /**< Viewport top in page px (1:1 anchor).  */
  uint8_t           zoom;       /**< Current ::mg_zoom_t.                   */
  uint8_t           fit_factor; /**< Decimation factor for fit-page (>= 1). */
} mg_reader_t;

/**
 * @brief Bind a viewer to a produced atlas and reset it to 1:1 top-left.
 *
 * @param[out] r   Reader state to populate.
 * @param[in]  cfg Framebuffer + cache + parsed atlas geometry.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Reader ready; viewport at (0,0), zoom 1:1.
 * @retval k_ra8_err_null_ptr     @p r, @p cfg, or a required @p cfg field NULL.
 * @retval k_ra8_err_invalid_size @p cfg geometry is zero / smaller than the fb.
 *
 * @pre @p cfg->info came from a successful ``ra8_tileatlas_parse``.
 * @pre @p cfg->fb covers `fb_w * fb_h` RGB565 pixels.
 * @post On success the viewport is clamped inside the page.
 * @post On any error @p r is left unbound.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mg_reader_init(mg_reader_t* r, const mg_reader_cfg_t* cfg);

/**
 * @brief Classify a panel coordinate into its tap-zone.
 *
 * @details Pure hit-test with no side effects: the centre rectangle (inside all
 *          four edge bands, below the status bar) is the zoom zone; each edge
 *          band is its pan zone; the status bar and any gap map to
 *          ::k_mg_zone_none.
 *
 * @param[in] r Bound reader (for the framebuffer extent).
 * @param[in] x Tap X, panel pixels.
 * @param[in] y Tap Y, panel pixels.
 *
 * @return mg_zone_t The zone @p (x,y) falls in.
 * @retval k_mg_zone_none  Status bar / outside the content area.
 * @retval k_mg_zone_pan_* An edge band.
 * @retval k_mg_zone_zoom  The centre.
 *
 * @pre @p r was populated by ::mg_reader_init.
 * @pre @p x and @p y are panel coordinates.
 * @post No state is mutated.
 * @post The result is one ::mg_zone_t member.
 *
 * @note Thread-safe (pure over its inputs).
 * @since 0.1.0
 */
[[nodiscard]] mg_zone_t mg_zone_hit(const mg_reader_t* r, int32_t x, int32_t y);

/**
 * @brief Apply a tap: pan or toggle zoom, and report whether state changed.
 *
 * @details Maps @p (x,y) through ::mg_zone_hit, then mutates the viewport (an
 *          edge zone, clamped inside the page) or the zoom (the centre zone).
 *          A pan already at the page edge, or a tap on no zone, leaves the
 *          state unchanged and returns false so the caller can skip the redraw.
 *
 * @param[in,out] r Bound reader.
 * @param[in]     x Tap X, panel pixels.
 * @param[in]     y Tap Y, panel pixels.
 *
 * @return bool Whether the viewport or zoom changed.
 * @retval true  State changed; the caller should re-render.
 * @retval false Tap hit nothing actionable (no redraw needed).
 *
 * @pre @p r was populated by ::mg_reader_init.
 * @pre @p x and @p y are panel coordinates.
 * @post On true the viewport/zoom reflects the tap, clamped inside the page.
 * @post On false @p r is byte-for-byte unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] bool mg_reader_tap(mg_reader_t* r, int32_t x, int32_t y);

/**
 * @brief Toggle the zoom between 1:1 and fit-page (SW-button entry point).
 *
 * @param[in,out] r Bound reader.
 *
 * @return bool Whether the zoom was toggled.
 * @retval true  The zoom state flipped (the normal case).
 * @retval false @p r was NULL (nothing toggled).
 *
 * @pre @p r was populated by ::mg_reader_init.
 * @pre None.
 * @post On true @p r->zoom is the other ::mg_zoom_t member.
 * @post The viewport is re-clamped for the new zoom.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] bool mg_reader_toggle_zoom(mg_reader_t* r);

/**
 * @brief Render the current viewport + chrome into the framebuffer.
 *
 * @details Clears the framebuffer, pages every tile overlapping the viewport
 *          through the cache (get/blit/put one at a time so a small cache
 *          evicts across the frame), packs gray8 to RGB565 with nearest-
 *          neighbour decimation at fit-page zoom, then draws the status bar and
 *          the minimap overlay.
 *
 * @param[in,out] r Bound reader.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           The framebuffer holds the current view.
 * @retval k_ra8_err_null_ptr @p r or its framebuffer was NULL.
 * @retval k_ra8_err_*        Propagated from the tile cache / gfx.
 *
 * @pre @p r was populated by ::mg_reader_init and ``ra8_gfx`` is bound to
 *      @p r->fb.
 * @pre The tile cache can decode every covered tile.
 * @post On success @p r->fb holds the rendered screen.
 * @post Every tile pinned during the frame was released.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mg_reader_render(mg_reader_t* r);

/**
 * @brief Format the status-bar string ("MANGA  1:1  x=.. y=..").
 *
 * @param[in]  r   Bound reader.
 * @param[out] buf Destination, NUL-terminated on return.
 * @param[in]  cap Capacity of @p buf (>= 1).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               The status text was written.
 * @retval k_ra8_err_null_ptr     @p r or @p buf was NULL.
 * @retval k_ra8_err_invalid_size @p cap was 0.
 *
 * @pre @p r was populated by ::mg_reader_init.
 * @pre @p buf holds @p cap writable bytes.
 * @post @p buf is NUL-terminated within @p cap.
 * @post No reader state is mutated.
 *
 * @note Thread-safe for distinct buffers.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mg_reader_status(const mg_reader_t* r, char* buf, uint32_t cap);

/**
 * @brief FNV-1a-32 over a byte buffer (framebuffer render hash for the banner).
 *
 * @param[in] buf Bytes to hash.
 * @param[in] len Byte count.
 *
 * @return uint32_t The FNV-1a-32 digest.
 * @retval 2166136261 The offset basis, when @p len is 0 (empty digest).
 *
 * @pre @p buf holds @p len readable bytes (or @p len is 0).
 * @pre None.
 * @post No state is mutated.
 * @post The digest is deterministic across host / board_sim / silicon.
 *
 * @note Thread-safe (pure).
 * @since 0.1.0
 */
[[nodiscard]] uint32_t mg_fnv1a(const void* buf, uint32_t len);

/**
 * @brief Tile-cache decode-on-miss callback: page one atlas tile into a cell.
 *
 * @details Matches ::ra8_tile_decode_fn. Reads @p key->tile_x / tile_y and
 *          reads that tile out of the atlas through
 *          ``ra8_tileatlas_read_tile`` (raw copy or deflate inflate) into
 *          @p cell, reporting the tile's clamped dimensions.
 *
 * @param[in]  ctx        A ::mg_tile_src_t* bound at cache init.
 * @param[in]  key        The tile to decode.
 * @param[out] cell       Destination cell (`cell_bytes` writable).
 * @param[in]  cell_bytes Cell capacity in bytes.
 * @param[out] out_w      Decoded tile width, pixels.
 * @param[out] out_h      Decoded tile height, pixels.
 *
 * @return ra8_err_t k_ra8_ok on success, or the read-tile error verbatim.
 * @retval k_ra8_ok           Tile decoded into @p cell.
 * @retval k_ra8_err_null_ptr A required pointer was NULL.
 * @retval k_ra8_err_*        Propagated from ``ra8_tileatlas_read_tile``.
 *
 * @pre @p ctx points at a live ::mg_tile_src_t.
 * @pre @p cell holds @p cell_bytes writable bytes.
 * @post On success `(*out_w) * (*out_h)` gray8 bytes are valid in @p cell.
 * @post On any error @p cell content is unspecified.
 *
 * @note Not thread-safe (shares the read-tile scratch).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mg_tile_decode(void*                 ctx,
                                       const ra8_tile_key_t* key,
                                       uint8_t*              cell,
                                       uint32_t              cell_bytes,
                                       uint16_t*             out_w,
                                       uint16_t*             out_h);

#ifdef __cplusplus
}
#endif
