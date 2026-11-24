/**
 * @file inc/zoom_popup.h
 * @brief Tap-driven figure-zoom overlay controller over ::zoom_view_t.
 *
 * @details
 * Wraps the strip-composited ::zoom_view_t viewport with an e-reader popup
 * chrome bar (top-left Close touch target + status text) and discrete tap-zone
 * hit-testing modeled on ``mg_reader``:
 *   - top-left bar chip closes the popup;
 *   - top / bottom / left / right edge bands pan by 7/8 viewport step;
 *   - centre tap cycles magnification along `[1x .. scale_max]` anchored on the
 *     tapped pixel, and flags the status bar dirty so the scale badge updates.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "zoom.h"

/**
 * @enum zoom_popup_metric_t
 * @brief Default chrome and tap-zone dimensions in panel pixels.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_zoom_popup_bar_h_default     = 48U,  /**< Top chrome bar height (matches mg_reader).   */
  k_zoom_popup_close_w_default   = 112U, /**< Top-left Close chip width.                   */
  k_zoom_popup_edge_band_default = 176U, /**< Pan edge band thickness (matches mg_reader). */
} zoom_popup_metric_t;

/**
 * @enum zoom_popup_zone_t
 * @brief Discrete touchscreen tap zones recognized by ::zoom_popup_zone_hit.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_zoom_popup_zone_none      = 0U, /**< Outside active zones (e.g. non-close header). */
  k_zoom_popup_zone_pan_up    = 1U, /**< Top viewport band: pan upward.                */
  k_zoom_popup_zone_pan_down  = 2U, /**< Bottom viewport band: pan downward.           */
  k_zoom_popup_zone_pan_left  = 3U, /**< Left viewport band: pan left.                 */
  k_zoom_popup_zone_pan_right = 4U, /**< Right viewport band: pan right.               */
  k_zoom_popup_zone_zoom      = 5U, /**< Viewport centre: cycle scale ladder.          */
  k_zoom_popup_zone_close     = 6U, /**< Top-left header chip: dismiss popup.          */
} zoom_popup_zone_t;

/**
 * @enum zoom_popup_action_t
 * @brief High-level outcome returned by ::zoom_popup_tap.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_zoom_popup_act_none    = 0U, /**< No state change (dead band or clamped edge). */
  k_zoom_popup_act_redrawn = 1U, /**< View panned or zoomed; render + present due. */
  k_zoom_popup_act_closed  = 2U, /**< Popup dismissed; caller should restore page. */
} zoom_popup_action_t;

/**
 * @struct zoom_popup_cfg_t
 * @brief Opening configuration for ::zoom_popup_open.
 *
 * @details Zero-valued layout overrides (`bar_h`, `close_w`, `edge_band`), `scale`,
 *          `scale_max`, and `settle_ms` expand to module defaults clamped to the
 *          panel geometry so small unit-test framebuffers preserve a valid centre
 *          tap zone while 1024x600 panels match ``mg_reader`` dimensions exactly.
 *
 * @since 0.1.0
 */
typedef struct {
  zoom_source_t             src;       /**< Bound figure source (mem/book/tiles).    */
  zoom_scratch_t            scratch;   /**< Caller-owned strip composite buffers.    */
  uint16_t                  fb_w;      /**< Panel width in pixels (> 0).             */
  uint16_t                  fb_h;      /**< Panel height in pixels (>= 24 or > bar). */
  uint16_t                  bar_h;     /**< Chrome bar height (0 => default).        */
  uint16_t                  close_w;   /**< Close chip width (0 => default).         */
  uint16_t                  edge_band; /**< Edge pan band (0 => clamped default).    */
  uint8_t                   scale;     /**< Opening zoom (0 => k_zoom_scale_min).    */
  uint8_t                   scale_max; /**< Ceiling zoom (0 => k_zoom_scale_max).    */
  zoom_policy_t             policy;    /**< Responsive A2/GC16 or always-quality.    */
  uint16_t                  settle_ms; /**< Settle window (0 => default 350 ms).     */
  int32_t                   focus_x;   /**< Opening focus X in source pixels.        */
  int32_t                   focus_y;   /**< Opening focus Y in source pixels.        */
  const ra8_gfx_tone_map_t* tone;      /**< Optional panel tone LUT (NULL = flat).   */
} zoom_popup_cfg_t;

/**
 * @struct zoom_popup_t
 * @brief Live figure-zoom overlay controller.
 *
 * @details Embedded ::zoom_view_t owns viewport bounds `[0, bar_h) .. [fb_w, fb_h)`
 *          and full pan/zoom/settle math. Caller reads via API helpers rather than
 *          mutating fields directly.
 *
 * @since 0.1.0
 */
typedef struct {
  zoom_view_t view;         /**< Embedded zoomed viewport (`dst.y == bar_h`). */
  uint16_t    fb_w;         /**< Panel width in pixels.                       */
  uint16_t    fb_h;         /**< Panel height in pixels.                      */
  uint16_t    bar_h;        /**< Resolved top chrome bar height.              */
  uint16_t    close_w;      /**< Resolved Close touch-zone width.             */
  uint16_t    edge_band;    /**< Resolved pan edge-band thickness.            */
  bool        chrome_dirty; /**< True when top bar text/chip needs flush.     */
  bool        active;       /**< True while the popup is presented.           */
} zoom_popup_t;

/**
 * @brief Open a figure-zoom overlay onto a bound source.
 *
 * @param[out] p   Popup controller state to initialise.
 * @param[in]  cfg Opening configuration; zero defaults expand automatically.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Popup active; render + full-panel quality present due.
 * @retval k_ra8_err_null_ptr    @p p, @p cfg, source read, or scratch pointer is NULL.
 * @retval k_ra8_err_invalid_arg Bad panel dimensions (`bar_h >= fb_h`) or bad scale.
 * @retval k_ra8_err_no_mem      Scratch buffers too small for @p cfg->fb_w.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t zoom_popup_open(zoom_popup_t* p, const zoom_popup_cfg_t* cfg);

/**
 * @brief Dismiss an open figure popup and clear any pending refresh state.
 *
 * @param[in,out] p Popup controller.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Popup is now closed (`p->active == false`).
 * @retval k_ra8_err_null_ptr @p p is NULL.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t zoom_popup_close(zoom_popup_t* p);

/**
 * @brief Query whether the figure-zoom popup is currently active.
 *
 * @param[in] p Popup controller (NULL returns false).
 * @return true while open; false if NULL or closed.
 *
 * @since 0.1.0
 */
bool zoom_popup_active(const zoom_popup_t* p);

/**
 * @brief Map panel coordinates `(x, y)` to a popup interaction zone.
 *
 * @param[in] p Active popup controller.
 * @param[in] x Touch column in panel pixels.
 * @param[in] y Touch row in panel pixels.
 *
 * @return Classified ::zoom_popup_zone_t (returns `k_zoom_popup_zone_none` if
 *         @p p is NULL, inactive, out of bounds, or on non-button chrome).
 *
 * @since 0.1.0
 */
zoom_popup_zone_t zoom_popup_zone_hit(const zoom_popup_t* p, int32_t x, int32_t y);

/**
 * @brief Process one touchscreen tap on the active popup.
 *
 * @param[in,out] p          Active popup controller.
 * @param[in]     x          Tap column in panel pixels.
 * @param[in]     y          Tap row in panel pixels.
 * @param[in]     now_ms     Current monotonic millisecond timestamp.
 * @param[out]    out_action Receives ::zoom_popup_action_t describing what happened.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok            Tap processed cleanly (`*out_action` populated).
 * @retval k_ra8_err_null_ptr  @p p or @p out_action is NULL.
 * @retval k_ra8_err_invalid_state @p p is not currently open.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t zoom_popup_tap(zoom_popup_t*        p,
                                       int32_t              x,
                                       int32_t              y,
                                       uint32_t             now_ms,
                                       zoom_popup_action_t* out_action);

/**
 * @brief Advance the idle settle timer and report whether a quality repaint is due.
 *
 * @param[in,out] p      Popup controller (NULL returns false).
 * @param[in]     now_ms Current monotonic millisecond timestamp.
 * @return true if settle expired and a quality render/present should run.
 *
 * @since 0.1.0
 */
bool zoom_popup_tick(zoom_popup_t* p, uint32_t now_ms);

/**
 * @brief Obtain the panel dirty rectangle and waveform class, clearing pending flags.
 *
 * @details Reports the entire `[0, 0, fb_w, fb_h]` rectangle whenever the chrome
 *          status bar also changed (on open or zoom scale cycle), and otherwise
 *          reports `view.dst` (`[0, bar_h, fb_w, fb_h - bar_h]`) so fast A2 pans
 *          do not needlessly flash the static chrome bar.
 *
 * @param[in,out] p   Active popup controller.
 * @param[out]    out Receives flush rectangle, waveform, and `present` flag.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Flush plan written to @p out.
 * @retval k_ra8_err_null_ptr      @p p or @p out is NULL.
 * @retval k_ra8_err_invalid_state @p p is closed.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t zoom_popup_present(zoom_popup_t* p, zoom_present_t* out);

/**
 * @brief Composite both the figure viewport and top chrome bar via `ra8_gfx`.
 *
 * @param[in,out] p Active popup controller.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Both chrome bar and figure rendered.
 * @retval k_ra8_err_null_ptr      @p p is NULL.
 * @retval k_ra8_err_invalid_state @p p is closed.
 * @retval k_ra8_err_*             Propagated from source reader or `ra8_gfx`.
 *
 * @pre ::ra8_gfx_init bound a framebuffer of at least `fb_w x fb_h` pixels.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t zoom_popup_render(zoom_popup_t* p);

/**
 * @brief Format the popup's ASCII status label (`"FIG WxH Sx"`).
 *
 * @param[in]  p       Active popup controller.
 * @param[out] buf     Destination byte buffer.
 * @param[in]  buf_cap Capacity of @p buf in bytes (must include NUL terminator).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                NUL-terminated ASCII label written.
 * @retval k_ra8_err_null_ptr      @p p or @p buf is NULL.
 * @retval k_ra8_err_invalid_state @p p is closed.
 * @retval k_ra8_err_no_mem        @p buf_cap is too small to hold the label.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t zoom_popup_status(const zoom_popup_t* p, char* buf, size_t buf_cap);

#ifdef __cplusplus
}
#endif
