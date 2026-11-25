/**
 * @file ra8_translate_detector.h
 * @brief C23 ABI for DB-style detector map thresholding, connected components,
 * bubble grouping, furigana suppression, and manga/webtoon reading order.
 * @ingroup grp_translate
 *
 * @details
 * Converts int8 quantized or float detector activation maps into ordered
 * speech bubbles and per-line crop boxes (`ra8_translate_det_line_t`) using
 * bounded caller-provided scratch memory and zero heap allocation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Wire and heuristic defaults for detector map post-processing. */
typedef enum : uint32_t {
  k_ra8_translate_det_max_map_dim  = 2048U,  /**< Max detector map width/height.    */
  k_ra8_translate_det_max_comps    = 4096U,  /**< Max candidate line components.    */
  k_ra8_translate_det_max_bps      = 10000U, /**< 100.00% score scale upper bound.  */
  k_ra8_translate_det_def_txt_bps  = 3000U,  /**< Default text threshold (0.30).    */
  k_ra8_translate_det_def_bub_bps  = 4000U,  /**< Default bubble threshold (0.40).  */
  k_ra8_translate_det_def_gap_pct  = 150U,   /**< Default line grouping gap (1.5x). */
  k_ra8_translate_det_def_furi_pct = 55U,    /**< Default max furigana ratio (55%). */
  k_ra8_translate_det_def_fgap_pct = 120U,   /**< Default max furigana gap (120%).  */
} ra8_translate_det_const_t;

/** @brief Page-level reading order policy for grouped speech bubbles. */
typedef enum : uint8_t {
  k_ra8_translate_det_order_manga_rtl   = 0U, /**< Manga: top-to-bottom, right-to-left.   */
  k_ra8_translate_det_order_webtoon_ttb = 1U, /**< Webtoon: top-to-bottom, left-to-right. */
} ra8_translate_det_order_t;

static_assert(sizeof(ra8_translate_det_order_t) == 1U, "ra8_translate_det_order_t width");

/** @brief Bitmask flags controlling detector post-processing heuristics. */
typedef enum : uint8_t {
  k_ra8_translate_det_flag_none          = 0U,       /**< Default 8-connected, keep all.  */
  k_ra8_translate_det_flag_drop_furigana = 1U << 0U, /**< Suppress adjacent ruby strips.  */
  k_ra8_translate_det_flag_4_connected   = 1U << 1U, /**< Use 4-neighborhood CC labeling. */
} ra8_translate_det_flag_t;

static_assert(sizeof(ra8_translate_det_flag_t) == 1U, "ra8_translate_det_flag_t width");

/**
 * @brief Page-coordinate text line bounding box associated with a bubble.
 * @since 0.1.0
 */
typedef struct {
  uint16_t x;                /**< Left origin pixel in page coordinates.  */
  uint16_t y;                /**< Top origin pixel in page coordinates.   */
  uint16_t w;                /**< Box width in page pixels (> 0).         */
  uint16_t h;                /**< Box height in page pixels (> 0).        */
  uint16_t bubble_index;     /**< Parent index in `out_bubbles`.          */
  uint16_t confidence_bps;   /**< Mean activation probability [0, 10000]. */
  uint8_t  text_orientation; /**< Matches `ra8_translate_orientation_t`.  */
  uint8_t  flags;            /**< Reserved line flags (0).                */
  uint16_t order_in_bubble;  /**< 0-based reading index inside bubble.    */
} ra8_translate_det_line_t;

static_assert(sizeof(ra8_translate_det_line_t) == 16U, "ra8_translate_det_line_t size");
static_assert(alignof(ra8_translate_det_line_t) == 2U, "ra8_translate_det_line_t align");
static_assert(offsetof(ra8_translate_det_line_t, x) == 0U, "det line x off");
static_assert(offsetof(ra8_translate_det_line_t, y) == 2U, "det line y off");
static_assert(offsetof(ra8_translate_det_line_t, w) == 4U, "det line w off");
static_assert(offsetof(ra8_translate_det_line_t, h) == 6U, "det line h off");
static_assert(offsetof(ra8_translate_det_line_t, bubble_index) == 8U, "det line bubble off");
static_assert(offsetof(ra8_translate_det_line_t, confidence_bps) == 10U, "det line conf off");
static_assert(offsetof(ra8_translate_det_line_t, text_orientation) == 12U, "det line orient off");
static_assert(offsetof(ra8_translate_det_line_t, flags) == 13U, "det line flags off");
static_assert(offsetof(ra8_translate_det_line_t, order_in_bubble) == 14U, "det line order off");

/**
 * @brief Grouped speech or narration bubble in page reading order.
 * @since 0.1.0
 */
typedef struct {
  uint16_t x;                /**< Bubble left origin in page pixels.     */
  uint16_t y;                /**< Bubble top origin in page pixels.      */
  uint16_t w;                /**< Bubble width in page pixels (> 0).     */
  uint16_t h;                /**< Bubble height in page pixels (> 0).    */
  uint16_t reading_order;    /**< 0-based page reading order index.      */
  uint16_t confidence_bps;   /**< Mean line activation score [0, 10000]. */
  uint16_t first_line;       /**< Start index in sorted `out_lines`.     */
  uint16_t line_count;       /**< Number of constituent lines (> 0).     */
  uint8_t  shape;            /**< Matches `ra8_translate_shape_t`.       */
  uint8_t  text_orientation; /**< Matches `ra8_translate_orientation_t`. */
  uint8_t  flags;            /**< Bit 0 set if backed by `bubble_map`.   */
  uint8_t  reserved;         /**< Zero padding.                          */
} ra8_translate_det_bubble_t;

static_assert(sizeof(ra8_translate_det_bubble_t) == 20U, "ra8_translate_det_bubble_t size");
static_assert(alignof(ra8_translate_det_bubble_t) == 2U, "ra8_translate_det_bubble_t align");
static_assert(offsetof(ra8_translate_det_bubble_t, x) == 0U, "det bubble x off");
static_assert(offsetof(ra8_translate_det_bubble_t, y) == 2U, "det bubble y off");
static_assert(offsetof(ra8_translate_det_bubble_t, w) == 4U, "det bubble w off");
static_assert(offsetof(ra8_translate_det_bubble_t, h) == 6U, "det bubble h off");
static_assert(offsetof(ra8_translate_det_bubble_t, reading_order) == 8U, "det bubble order off");
static_assert(offsetof(ra8_translate_det_bubble_t, confidence_bps) == 10U, "det bubble conf off");
static_assert(offsetof(ra8_translate_det_bubble_t, first_line) == 12U, "det bubble first off");
static_assert(offsetof(ra8_translate_det_bubble_t, line_count) == 14U, "det bubble count off");
static_assert(offsetof(ra8_translate_det_bubble_t, shape) == 16U, "det bubble shape off");
static_assert(offsetof(ra8_translate_det_bubble_t, text_orientation) == 17U,
              "det bubble orient off");
static_assert(offsetof(ra8_translate_det_bubble_t, flags) == 18U, "det bubble flags off");
static_assert(offsetof(ra8_translate_det_bubble_t, reserved) == 19U, "det bubble reserved off");

/**
 * @brief Threshold, padding, grouping, furigana-filter, and ordering settings.
 * @since 0.1.0
 */
typedef struct {
  uint16_t                  map_width;              /**< Activation map width (> 0).      */
  uint16_t                  map_height;             /**< Activation map height (> 0).     */
  uint16_t                  row_stride;             /**< Row stride in elems (0 = tight). */
  uint16_t                  elem_stride;            /**< Pixel elem step (0/1 = tight).   */
  uint16_t                  page_width;             /**< Target page width (> 0).         */
  uint16_t                  page_height;            /**< Target page height (> 0).        */
  uint16_t                  text_thresh_bps;        /**< Text cutoff [1, 10000].          */
  uint16_t                  bubble_thresh_bps;      /**< Bubble cutoff [1, 10000].        */
  uint16_t                  min_comp_area;          /**< Min component map pixels (>= 1). */
  uint16_t                  box_pad_pct;            /**< DB unclip pad [0, 100] %.        */
  uint16_t                  group_gap_pct;          /**< Line merge gap [0, 1000] %.      */
  uint16_t                  furigana_max_ratio_pct; /**< Max minor ratio [1, 99] %.       */
  uint16_t                  furigana_max_gap_pct;   /**< Max adjacent gap [0, 500] %.     */
  ra8_translate_det_order_t reading_order;          /**< Manga RTL vs webtoon TTB.        */
  uint8_t                   flags;                  /**< `ra8_translate_det_flag_t` mask. */
  uint16_t                  max_components;         /**< Scratch comp cap (0 = line_cap). */
  uint8_t                   reserved[2];            /**< Reserved; must be zero.          */
} ra8_translate_det_cfg_t;

static_assert(sizeof(ra8_translate_det_cfg_t) == 32U, "ra8_translate_det_cfg_t size");
static_assert(alignof(ra8_translate_det_cfg_t) == 2U, "ra8_translate_det_cfg_t align");
static_assert(offsetof(ra8_translate_det_cfg_t, map_width) == 0U, "det cfg map_w off");
static_assert(offsetof(ra8_translate_det_cfg_t, page_width) == 8U, "det cfg page_w off");
static_assert(offsetof(ra8_translate_det_cfg_t, text_thresh_bps) == 12U, "det cfg txt_t off");
static_assert(offsetof(ra8_translate_det_cfg_t, reading_order) == 26U, "det cfg order off");
static_assert(offsetof(ra8_translate_det_cfg_t, max_components) == 28U, "det cfg max_comps off");
static_assert(offsetof(ra8_translate_det_cfg_t, reserved) == 30U, "det cfg reserved off");

/**
 * @brief Summary counts produced by detector post-processing.
 * @since 0.1.0
 */
typedef struct {
  uint32_t raw_components;   /**< Connected components passing `min_comp_area`. */
  uint32_t dropped_furigana; /**< Candidate boxes suppressed as furigana.       */
  uint32_t line_count;       /**< Surviving lines written to `out_lines`.       */
  uint32_t bubble_count;     /**< Ordered bubbles written to `out_bubbles`.     */
} ra8_translate_det_summary_t;

static_assert(sizeof(ra8_translate_det_summary_t) == 16U, "ra8_translate_det_summary_t size");
static_assert(alignof(ra8_translate_det_summary_t) == 4U, "ra8_translate_det_summary_t align");
static_assert(offsetof(ra8_translate_det_summary_t, raw_components) == 0U, "det sum raw off");
static_assert(offsetof(ra8_translate_det_summary_t, dropped_furigana) == 4U, "det sum furi off");
static_assert(offsetof(ra8_translate_det_summary_t, line_count) == 8U, "det sum line off");
static_assert(offsetof(ra8_translate_det_summary_t, bubble_count) == 12U, "det sum bub off");

/**
 * @brief Compute conservative scratch workspace bytes required for detector post-processing.
 *
 * @details Calling context: `task-safe-reentrant`. Computes aligned row-labeling
 * and component/bubble scratch sizes without heap allocation.
 *
 * @param[in] map_width Activation map width (> 0, <= ::k_ra8_translate_det_max_map_dim).
 * @param[in] max_components Capacity for candidate components (> 0, <= max_comps).
 * @param[out] out_work_bytes Non-null storage receiving required byte count.
 * @return ::k_ra8_ok on success, or explicit error status with @p out_work_bytes untouched.
 * @since 0.1.0
 */
ra8_err_t
ra8_translate_det_work_bytes(uint16_t map_width, uint16_t max_components, size_t* out_work_bytes);

/**
 * @brief Post-process quantized `int8_t` detector output maps into ordered bubbles and lines.
 *
 * @details Calling context: `task-safe-reentrant`. Performs zero heap allocation,
 * extracts connected components via streaming row union-find, drops furigana strips
 * next to larger text columns when enabled, groups lines using optional @p bubble_map
 * contours plus spatial proximity, orders bubbles for manga or webtoon pages, and
 * leaves caller outputs untouched on any error.
 *
 * @param[in] text_map Quantized text activation map (`int8_t`).
 * @param[in] text_elem_count Readable elements in @p text_map.
 * @param[in] text_scale Positive finite affine scale for @p text_map.
 * @param[in] text_zp Zero point for @p text_map.
 * @param[in] bubble_map Optional quantized bubble region map (`NULL` if none).
 * @param[in] bubble_elem_count Elements in @p bubble_map (must be `0` if `NULL`).
 * @param[in] bubble_scale Positive finite affine scale when @p bubble_map != `NULL`.
 * @param[in] bubble_zp Zero point for @p bubble_map.
 * @param[in] cfg Validated detector geometry and heuristic configuration.
 * @param[in,out] work_buf Non-null caller scratch buffer (`work_cap` bytes).
 * @param[in] work_cap Byte capacity of @p work_buf.
 * @param[out] out_lines Caller array receiving ordered text lines.
 * @param[in] line_cap Element capacity of @p out_lines.
 * @param[out] out_bubbles Caller array receiving reading-order bubbles.
 * @param[in] bubble_cap Element capacity of @p out_bubbles.
 * @param[out] out_summary Non-null summary counts output.
 * @return ::k_ra8_ok on success, or explicit error status with outputs untouched.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_det_post_i8(const int8_t*                  text_map,
                                    size_t                         text_elem_count,
                                    float                          text_scale,
                                    int8_t                         text_zp,
                                    const int8_t*                  bubble_map,
                                    size_t                         bubble_elem_count,
                                    float                          bubble_scale,
                                    int8_t                         bubble_zp,
                                    const ra8_translate_det_cfg_t* cfg,
                                    uint8_t*                       work_buf,
                                    size_t                         work_cap,
                                    ra8_translate_det_line_t*      out_lines,
                                    size_t                         line_cap,
                                    ra8_translate_det_bubble_t*    out_bubbles,
                                    size_t                         bubble_cap,
                                    ra8_translate_det_summary_t*   out_summary);

/**
 * @brief Post-process `float` detector output maps into ordered bubbles and lines.
 *
 * @details Calling context: `task-safe-reentrant`. Host reference equivalent of
 * ::ra8_translate_det_post_i8 operating directly on finite `[0, 1]` probability maps.
 *
 * @param[in] text_map Floating-point text probability map.
 * @param[in] text_elem_count Readable elements in @p text_map.
 * @param[in] bubble_map Optional floating-point bubble probability map (`NULL` if none).
 * @param[in] bubble_elem_count Elements in @p bubble_map (must be `0` if `NULL`).
 * @param[in] cfg Validated detector geometry and heuristic configuration.
 * @param[in,out] work_buf Non-null caller scratch buffer (`work_cap` bytes).
 * @param[in] work_cap Byte capacity of @p work_buf.
 * @param[out] out_lines Caller array receiving ordered text lines.
 * @param[in] line_cap Element capacity of @p out_lines.
 * @param[out] out_bubbles Caller array receiving reading-order bubbles.
 * @param[in] bubble_cap Element capacity of @p out_bubbles.
 * @param[out] out_summary Non-null summary counts output.
 * @return ::k_ra8_ok on success, or explicit error status with outputs untouched.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_det_post_f32(const float*                   text_map,
                                     size_t                         text_elem_count,
                                     const float*                   bubble_map,
                                     size_t                         bubble_elem_count,
                                     const ra8_translate_det_cfg_t* cfg,
                                     uint8_t*                       work_buf,
                                     size_t                         work_cap,
                                     ra8_translate_det_line_t*      out_lines,
                                     size_t                         line_cap,
                                     ra8_translate_det_bubble_t*    out_bubbles,
                                     size_t                         bubble_cap,
                                     ra8_translate_det_summary_t*   out_summary);

#ifdef __cplusplus
}
#endif
