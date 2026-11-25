/**
 * @file inc/mg_translate.h
 * @brief Manga reader translation state: per-title toggle, bubble inspection,
 *        queued/translating chapter markers, and missing/stale fallback.
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * Bridges ``libs/ra8_translate`` into ``mg_reader`` with zero dynamic memory
 * allocation:
 *   - Per-title Original vs. Translated viewing preference table.
 *   - Fail-closed `TRN1` sidecar open + key-match verification (falling back
 *     to the unmasked original page on missing, corrupt, or stale sidecars).
 *   - Viewport-mapped bubble overlay rendering via `ra8_widget_paint_t`.
 *   - Panel-to-page bubble tap hit-testing exposing source UTF-8 + OCR bps.
 *   - Live chapter translation marker queries (`queued` / `translating`).
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
#include "ra8_translate.h"
#include "ra8_translate_job.h"

/**
 * @enum mg_trn_const_t
 * @brief Fixed bounds, chrome geometry, and palette colors for translation UI.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_mg_trn_max_titles     = 16U,       /**< Tracked per-title mode slots.      */
  k_mg_trn_max_chapters   = 8U,        /**< Attached chapter descriptors cap.  */
  k_mg_trn_damage_cap     = 64U,       /**< Overlay damage rectangles cap.     */
  k_mg_trn_src_cap        = 160U,      /**< Saved original UTF-8 text cap.     */
  k_mg_trn_preview_cap    = 96U,       /**< Printable popup text preview cap.  */
  k_mg_trn_label_cap      = 32U,       /**< Status pill label buffer length.   */
  k_mg_trn_pill_h         = 32U,       /**< Status-bar pill height, px.        */
  k_mg_trn_pill_y         = 8U,        /**< Status-bar pill top offset, px.    */
  k_mg_trn_pill_pad       = 12U,       /**< Right edge inset for chrome, px.   */
  k_mg_trn_pill_gap       = 8U,        /**< Horizontal gap between pills, px.  */
  k_mg_trn_mode_pill_w    = 112U,      /**< Original/Translated pill width.    */
  k_mg_trn_chap_pill_w    = 168U,      /**< Chapter queue status pill width.   */
  k_mg_trn_min_chrome_w   = 520U,      /**< Minimum panel width showing pills. */
  k_mg_trn_popup_x        = 12U,       /**< Bubble popup left inset, px.       */
  k_mg_trn_popup_bottom   = 14U,       /**< Bubble popup bottom margin, px.    */
  k_mg_trn_popup_h        = 60U,       /**< Bubble popup card height, px.      */
  k_mg_trn_popup_pad      = 10U,       /**< Bubble popup interior padding, px. */
  k_mg_trn_popup_line_gap = 22U,       /**< Vertical step to popup body text.  */
  k_mg_trn_popup_right_m  = 136U,      /**< Reserved right width for minimap.  */
  k_mg_trn_col_border     = 0xFFFFFFU, /**< Crisp white pill/card border.      */
  k_mg_trn_col_orig_bg    = 0x303038U, /**< Original-mode pill fill.           */
  k_mg_trn_col_trn_bg     = 0xF4F4F4U, /**< Translated-mode pill fill.         */
  k_mg_trn_col_trn_fg     = 0x101010U, /**< Dark ink for light pill fill.      */
  k_mg_trn_col_warn_bg    = 0x9C2C2CU, /**< Missing/stale fallback pill fill.  */
  k_mg_trn_col_busy_bg    = 0xC68A18U, /**< Queued/translating badge fill.     */
  k_mg_trn_col_ready_bg   = 0x286440U, /**< Ready chapter badge fill.          */
  k_mg_trn_col_popup_bg   = 0x18181CU, /**< Original-text popup card fill.     */
  k_mg_trn_col_popup_fg   = 0xFFFFFFU, /**< Original-text popup title ink.     */
  k_mg_trn_col_popup_sub  = 0xD8D8E0U, /**< Original-text popup body ink.      */
} mg_trn_const_t;

/**
 * @enum mg_trn_sidecar_state_t
 * @brief Validation verdict for the active chapter's `TRN1` translation sidecar.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_mg_trn_sidecar_none    = 0U, /**< No sidecar buffer supplied.          */
  k_mg_trn_sidecar_corrupt = 1U, /**< Buffer failed `TRN1` open/CRC check. */
  k_mg_trn_sidecar_stale   = 2U, /**< Key/hash/model mismatch or page OOB. */
  k_mg_trn_sidecar_ready   = 3U, /**< Valid `TRN1` sidecar matching page.  */
} mg_trn_sidecar_state_t;

/**
 * @enum mg_trn_job_marker_t
 * @brief Background chapter translation state exposed to reader chrome.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_mg_trn_job_none        = 0U, /**< Chapter neither queued nor active.    */
  k_mg_trn_job_queued      = 1U, /**< Waiting in chapter translation queue. */
  k_mg_trn_job_translating = 2U, /**< Actively running pipeline right now.  */
  k_mg_trn_job_paused      = 3U, /**< Active job yielded or battery-paused. */
} mg_trn_job_marker_t;

/**
 * @enum mg_trn_bar_hit_t
 * @brief Actionable hit region inside the top status bar.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_mg_trn_bar_none         = 0U, /**< Non-chrome status bar area.        */
  k_mg_trn_bar_toggle_mode  = 1U, /**< Original / Translated toggle pill. */
  k_mg_trn_bar_next_chapter = 2U, /**< Chapter / queue marker pill.       */
} mg_trn_bar_hit_t;

/**
 * @struct mg_trn_chapter_t
 * @brief One reader chapter descriptor plus optional sidecar and job fallback.
 * @since 0.1.0
 */
typedef struct {
  uint32_t                    chapter_order; /**< Ordinal inside title (`1..N`).     */
  ra8_translate_sidecar_key_t expected_key;  /**< Expected chapter hash & versions.  */
  const uint8_t*              sidecar_buf;   /**< Optional `TRN1` sidecar bytes.     */
  size_t                      sidecar_len;   /**< Length of @c sidecar_buf in bytes. */
  uint16_t                    page_index;    /**< Active page inside @c sidecar_buf. */
  mg_trn_job_marker_t         static_marker; /**< Fallback marker if no job bound.   */
} mg_trn_chapter_t;

/**
 * @struct mg_trn_title_pref_t
 * @brief One per-title Original/Translated mode slot.
 * @since 0.1.0
 */
typedef struct {
  uint32_t title_id;        /**< Non-zero title identifier.          */
  bool     show_translated; /**< True for translated overlay mode.   */
  bool     occupied;        /**< True when slot holds a bound title. */
} mg_trn_title_pref_t;

/**
 * @struct mg_translate_popup_t
 * @brief Original-text + OCR-confidence state for a tapped speech bubble.
 * @since 0.1.0
 */
typedef struct {
  bool     open;                          /**< True when popup is visible.  */
  uint32_t bubble_index;                  /**< Selected global `TRN1` idx.  */
  uint16_t reading_order;                 /**< Page reading order (0-base). */
  uint16_t ocr_confidence_bps;            /**< OCR confidence `0..10000`.   */
  char     source_utf8[k_mg_trn_src_cap]; /**< NUL-terminated UTF-8 source. */
  uint32_t source_len;                    /**< Raw source byte count.       */
} mg_translate_popup_t;

/**
 * @struct mg_trn_view_t
 * @brief Decoupled viewport mapping handed from `mg_reader` during draw/tap.
 * @since 0.1.0
 */
typedef struct {
  int32_t fb_w;       /**< Panel framebuffer width, px.   */
  int32_t fb_h;       /**< Panel framebuffer height, px.  */
  int32_t content_y0; /**< Top Y below status bar, px.    */
  int32_t view_x;     /**< Left page X in 1:1 source px.  */
  int32_t view_y;     /**< Top page Y in 1:1 source px.   */
  int32_t scale;      /**< Integer decimation factor >=1. */
  int32_t ox;         /**< Panel centering X offset, px.  */
  int32_t oy;         /**< Panel centering Y offset, px.  */
} mg_trn_view_t;

/**
 * @struct mg_translate_ctx_t
 * @brief Reader-level translation controller state attached to `mg_reader_t`.
 * @since 0.1.0
 */
typedef struct {
  uint32_t               title_id;                        /**< Active title id.      */
  size_t                 chapter_count;                   /**< Bound chapter count.  */
  size_t                 active_chapter;                  /**< Selected chap index.  */
  mg_trn_chapter_t       chapters[k_mg_trn_max_chapters]; /**< Chapter descriptors.  */
  mg_trn_title_pref_t    prefs[k_mg_trn_max_titles];      /**< Per-title toggles.    */
  size_t                 lru_next;                        /**< LRU eviction cursor.  */
  const uint8_t*         job_work;                        /**< Optional `TRJW` buf.  */
  size_t                 job_work_bytes;                  /**< Byte length of buf.   */
  mg_trn_sidecar_state_t active_sidecar_state;            /**< Cached sidecar state. */
  mg_translate_popup_t   popup;                           /**< Active bubble popup.  */
  /** @brief Active speech-bubble mask fill and font style configuration. */
  ra8_translate_overlay_style_t overlay_style;
  /** @brief Page-space damage rectangles emitted by the last overlay render. */
  ra8_ui_rect_t last_damage[k_mg_trn_damage_cap];
  /** @brief Number of valid entries in @c last_damage. */
  size_t last_damage_count;
} mg_translate_ctx_t;

/**
 * @brief Reset a translation controller to clean defaults (Original mode).
 * @param[out] ctx Controller storage to initialize.
 * @return ::k_ra8_ok or ::k_ra8_err_null_ptr.
 * @pre @p ctx is non-NULL.
 * @post All title toggles, active chapter state, and popup state are cleared.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mg_translate_init(mg_translate_ctx_t* ctx);

/**
 * @brief Bind or detach an initialized `ra8_translate_job` scheduler workspace.
 * @param[in,out] ctx Translation controller.
 * @param[in] work Initialized `TRJW` workspace buffer (NULL detaches).
 * @param[in] work_bytes Byte length of @p work (`0` when @p work is NULL).
 * @return ::k_ra8_ok on valid binding, or explicit error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
mg_translate_bind_job(mg_translate_ctx_t* ctx, const uint8_t* work, size_t work_bytes);

/**
 * @brief Open a title and its chapter list while preserving per-title toggle.
 * @param[in,out] ctx Translation controller.
 * @param[in] title_id Non-zero title identifier.
 * @param[in] chapters Array of chapter descriptors (`1..k_mg_trn_max_chapters`).
 * @param[in] chapter_count Number of entries in @p chapters.
 * @param[in] initial_chapter Zero-based initially active chapter index.
 * @return ::k_ra8_ok or explicit validation error (`k_ra8_err_invalid_arg`, etc.).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mg_translate_open_title(mg_translate_ctx_t*     ctx,
                                                uint32_t                title_id,
                                                const mg_trn_chapter_t* chapters,
                                                size_t                  chapter_count,
                                                size_t                  initial_chapter);

/**
 * @brief Switch active chapter within the currently open title and re-validate.
 * @param[in,out] ctx Translation controller.
 * @param[in] chapter_index Target chapter index in `[0, chapter_count)`.
 * @return ::k_ra8_ok or explicit error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mg_translate_select_chapter(mg_translate_ctx_t* ctx, size_t chapter_index);

/**
 * @brief Advance cleanly to the next bound chapter (wraps modulo count).
 * @param[in,out] ctx Translation controller.
 * @return True if active chapter changed; false if <= 1 chapter or NULL.
 * @since 0.1.0
 */
[[nodiscard]] bool mg_translate_next_chapter(mg_translate_ctx_t* ctx);

/**
 * @brief Query per-title Original/Translated toggle state.
 * @param[in] ctx Translation controller.
 * @param[in] title_id Title identifier to inspect.
 * @return True if Translated mode is stored for @p title_id; false otherwise.
 * @since 0.1.0
 */
[[nodiscard]] bool mg_translate_get_title_mode(const mg_translate_ctx_t* ctx, uint32_t title_id);

/**
 * @brief Explicitly set per-title Original or Translated viewing preference.
 * @param[in,out] ctx Translation controller.
 * @param[in] title_id Non-zero title identifier.
 * @param[in] show_translated True for translated overlays, false for original.
 * @return ::k_ra8_ok or explicit validation error.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
mg_translate_set_title_mode(mg_translate_ctx_t* ctx, uint32_t title_id, bool show_translated);

/**
 * @brief Toggle Original <-> Translated mode for the currently open title.
 * @param[in,out] ctx Translation controller.
 * @return True if toggle succeeded; false if no title is open or NULL.
 * @since 0.1.0
 */
[[nodiscard]] bool mg_translate_toggle_mode(mg_translate_ctx_t* ctx);

/**
 * @brief Return cached `TRN1` sidecar validation state for the active chapter.
 * @param[in] ctx Translation controller.
 * @return One ::mg_trn_sidecar_state_t value.
 * @since 0.1.0
 */
[[nodiscard]] mg_trn_sidecar_state_t mg_translate_sidecar_state(const mg_translate_ctx_t* ctx);

/**
 * @brief True when Translated mode was requested but fell back to Original.
 * @param[in] ctx Translation controller.
 * @return True if active title wants Translated and sidecar is missing/stale.
 * @since 0.1.0
 */
[[nodiscard]] bool mg_translate_is_fallback_active(const mg_translate_ctx_t* ctx);

/**
 * @brief True when Translated mode is selected AND active sidecar is valid.
 * @param[in] ctx Translation controller.
 * @return True when translation overlays should render and hit-test bubbles.
 * @since 0.1.0
 */
[[nodiscard]] bool mg_translate_overlays_active(const mg_translate_ctx_t* ctx);

/**
 * @brief Query whether chapter @p chapter_index is queued, translating, or idle.
 * @param[in] ctx Translation controller.
 * @param[in] chapter_index Zero-based index inside @c ctx->chapters.
 * @return Live scheduler chapter marker when bound, else chapter static marker.
 * @since 0.1.0
 */
[[nodiscard]] mg_trn_job_marker_t mg_translate_query_chapter_marker(const mg_translate_ctx_t* ctx,
                                                                    size_t chapter_index);

/**
 * @brief Format right-hand mode pill label ("ORIG", "TRN", or "ORIG!").
 * @param[in] ctx Translation controller.
 * @param[out] buf Destination character buffer.
 * @param[in] cap Buffer capacity in bytes (>= 1).
 * @return ::k_ra8_ok or explicit error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
mg_translate_format_mode_pill(const mg_translate_ctx_t* ctx, char* buf, uint32_t cap);

/**
 * @brief Format chapter + job marker pill label (e.g. "CH2:QUEUED").
 * @param[in] ctx Translation controller.
 * @param[out] buf Destination character buffer.
 * @param[in] cap Buffer capacity in bytes (>= 1).
 * @return ::k_ra8_ok or explicit error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
mg_translate_format_chapter_pill(const mg_translate_ctx_t* ctx, char* buf, uint32_t cap);

/**
 * @brief Format popup header showing confidence percent and reading index.
 * @param[in] popup Valid bubble popup descriptor.
 * @param[out] buf Destination character buffer.
 * @param[in] cap Buffer capacity in bytes (>= 1).
 * @return ::k_ra8_ok or explicit error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
mg_translate_format_popup_header(const mg_translate_popup_t* popup, char* buf, uint32_t cap);

/**
 * @brief Format printable 7-bit ASCII preview of popup UTF-8 original text.
 * @param[in] popup Valid bubble popup descriptor.
 * @param[out] buf Destination character buffer.
 * @param[in] cap Buffer capacity in bytes (>= 1).
 * @return ::k_ra8_ok or explicit error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
mg_translate_format_popup_body(const mg_translate_popup_t* popup, char* buf, uint32_t cap);

/**
 * @brief Classify a status-bar tap coordinate into a translation chrome action.
 * @param[in] ctx Translation controller.
 * @param[in] fb_w Panel width in pixels.
 * @param[in] statusbar_h Status bar height in pixels.
 * @param[in] x Tap X in panel pixels.
 * @param[in] y Tap Y in panel pixels.
 * @return Hit status-bar control, or ::k_mg_trn_bar_none.
 * @since 0.1.0
 */
[[nodiscard]] mg_trn_bar_hit_t mg_translate_hit_bar(const mg_translate_ctx_t* ctx,
                                                    int32_t                   fb_w,
                                                    int32_t                   statusbar_h,
                                                    int32_t                   x,
                                                    int32_t                   y);

/**
 * @brief Hit-test a content-area panel tap against visible `TRN1` bubbles.
 * @param[in,out] ctx Translation controller.
 * @param[in] view Active viewport geometry.
 * @param[in] x Tap X in panel pixels.
 * @param[in] y Tap Y in panel pixels.
 * @return True if a bubble popup was opened, switched, or dismissed.
 * @since 0.1.0
 */
[[nodiscard]] bool
mg_translate_tap_page(mg_translate_ctx_t* ctx, const mg_trn_view_t* view, int32_t x, int32_t y);

/**
 * @brief Paint active page's `TRN1` bubble overlays mapped to @p view.
 * @param[in,out] ctx Translation controller.
 * @param[in] view Active viewport geometry.
 * @return ::k_ra8_ok (including no-op fallback when disabled, missing, or stale).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mg_translate_render_overlays(mg_translate_ctx_t*  ctx,
                                                     const mg_trn_view_t* view);

/**
 * @brief Paint status-bar mode/chapter pills and active bubble popup card.
 * @param[in] ctx Translation controller.
 * @param[in] fb_w Panel width in pixels.
 * @param[in] fb_h Panel height in pixels.
 * @return ::k_ra8_ok or error from graphics backend.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
mg_translate_render_chrome(const mg_translate_ctx_t* ctx, int32_t fb_w, int32_t fb_h);

#ifdef __cplusplus
}
#endif
