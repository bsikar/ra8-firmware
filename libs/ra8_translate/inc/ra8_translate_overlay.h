/**
 * @file ra8_translate_overlay.h
 * @brief C23 ABI for TRN1 manga/webtoon translation overlay rendering and English
 * text fitting over `ra8_widget_paint_t`.
 * @since 0.1.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_widget.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Wire and stack-bounds constants for overlay typesetting and rendering. */
typedef enum : uint32_t {
  k_ra8_translate_overlay_max_lines      = 64U,       /**< Max fitted lines per bubble.     */
  k_ra8_translate_overlay_max_line_bytes = 512U,      /**< Bounded stack line buffer limit. */
  k_ra8_translate_overlay_default_fg     = 0x000000U, /**< Default black ink (0xRRGGBB).    */
  k_ra8_translate_overlay_default_bg     = 0xFFFFFFU, /**< Default white mask (0xRRGGBB).   */
  k_ra8_translate_overlay_default_border = 0x000000U, /**< Default black border (0xRRGGBB). */
} ra8_translate_overlay_const_t;

/** @brief Fit diagnostic bitmask flags written into `ra8_translate_overlay_fit_t::fit_flags`. */
typedef enum : uint8_t {
  k_ra8_translate_overlay_fit_none         = 0U,      /**< Clean word-wrapped fit.      */
  k_ra8_translate_overlay_fit_hard_wrapped = 1U << 0, /**< Word split mid-token to fit. */
  k_ra8_translate_overlay_fit_clipped      = 1U << 1, /**< Overflowed height at min sz. */
} ra8_translate_overlay_fit_flag_t;

static_assert(sizeof(ra8_translate_overlay_fit_flag_t) == 1U,
              "ra8_translate_overlay_fit_flag_t width");

/**
 * @brief Visual style parameters controlling bubble fill, border, padding, and typography.
 * @since 0.1.0
 */
typedef struct {
  uint32_t                 fg_color;     /**< Text foreground RGB24 (`0xRRGGBB`).          */
  uint32_t                 bg_color;     /**< Mask fill RGB24 (`0xFFFFFF` for white mask). */
  uint32_t                 border_color; /**< Optional outline RGB24 when `border_w > 0`.  */
  int16_t                  pad;          /**< Inner safe-box padding in pixels (`>= 0`).   */
  int16_t                  border_w;     /**< Outline stroke thickness in pixels (`>= 0`). */
  int16_t                  line_gap;     /**< Extra pixel gap between lines (`>= 0`).      */
  ra8_widget_align_t       align;        /**< Horizontal alignment (`center` typical).     */
  ra8_widget_text_face_t   face;         /**< Font family (`sans` or `serif`).             */
  ra8_widget_text_weight_t weight;       /**< Font weight (`regular` or `bold`).           */
  uint8_t                  flags;        /**< Reserved style option flags; must be `0`.    */
  uint8_t                  reserved[2];  /**< Reserved; must be `{0, 0}`.                  */
} ra8_translate_overlay_style_t;

static_assert(sizeof(ra8_translate_overlay_style_t) == 24U, "ra8_translate_overlay_style_t size");
static_assert(alignof(ra8_translate_overlay_style_t) == 4U, "ra8_translate_overlay_style_t align");
static_assert(offsetof(ra8_translate_overlay_style_t, fg_color) == 0U, "style fg_color off");
static_assert(offsetof(ra8_translate_overlay_style_t, bg_color) == 4U, "style bg_color off");
static_assert(offsetof(ra8_translate_overlay_style_t, border_color) == 8U,
              "style border_color off");
static_assert(offsetof(ra8_translate_overlay_style_t, pad) == 12U, "style pad off");
static_assert(offsetof(ra8_translate_overlay_style_t, border_w) == 14U, "style border_w off");
static_assert(offsetof(ra8_translate_overlay_style_t, line_gap) == 16U, "style line_gap off");
static_assert(offsetof(ra8_translate_overlay_style_t, align) == 18U, "style align off");
static_assert(offsetof(ra8_translate_overlay_style_t, face) == 19U, "style face off");
static_assert(offsetof(ra8_translate_overlay_style_t, weight) == 20U, "style weight off");
static_assert(offsetof(ra8_translate_overlay_style_t, flags) == 21U, "style flags off");
static_assert(offsetof(ra8_translate_overlay_style_t, reserved) == 22U, "style reserved off");

/**
 * @brief Standalone bubble geometry descriptor matching `TRN1` bubble mask fields.
 * @since 0.1.0
 */
typedef struct {
  uint16_t x;                /**< Left pixel coordinate in page space.           */
  uint16_t y;                /**< Top pixel coordinate in page space.            */
  uint16_t w;                /**< Bubble bounding width in pixels (`> 0`).       */
  uint16_t h;                /**< Bubble bounding height in pixels (`> 0`).      */
  uint8_t  shape;            /**< ::ra8_translate_shape_t discriminant (`0..3`). */
  uint8_t  text_orientation; /**< ::ra8_translate_orientation_t (`0..2`).        */
  uint16_t corner_radius;    /**< Rounded-rect corner radius (`0` otherwise).    */
  uint32_t reserved;         /**< Reserved; must be `0`.                         */
} ra8_translate_overlay_bubble_spec_t;

static_assert(sizeof(ra8_translate_overlay_bubble_spec_t) == 16U,
              "ra8_translate_overlay_bubble_spec_t size");
static_assert(alignof(ra8_translate_overlay_bubble_spec_t) == 4U,
              "ra8_translate_overlay_bubble_spec_t align");
static_assert(offsetof(ra8_translate_overlay_bubble_spec_t, x) == 0U, "spec x off");
static_assert(offsetof(ra8_translate_overlay_bubble_spec_t, y) == 2U, "spec y off");
static_assert(offsetof(ra8_translate_overlay_bubble_spec_t, w) == 4U, "spec w off");
static_assert(offsetof(ra8_translate_overlay_bubble_spec_t, h) == 6U, "spec h off");
static_assert(offsetof(ra8_translate_overlay_bubble_spec_t, shape) == 8U, "spec shape off");
static_assert(offsetof(ra8_translate_overlay_bubble_spec_t, text_orientation) == 9U,
              "spec text_orientation off");
static_assert(offsetof(ra8_translate_overlay_bubble_spec_t, corner_radius) == 10U,
              "spec corner_radius off");
static_assert(offsetof(ra8_translate_overlay_bubble_spec_t, reserved) == 12U, "spec reserved off");

/**
 * @brief Output of the largest-font English fitting and wrapping search.
 * @since 0.1.0
 */
typedef struct {
  uint16_t font_size_px;   /**< Fitted glyph cell line height in pixels (`0` if empty).    */
  uint16_t line_count;     /**< Number of cumulative line-end offsets written.             */
  uint16_t max_line_width; /**< Pixel width of widest wrapped line.                        */
  uint16_t total_height;   /**< Vertical pixel extent including line gaps.                 */
  uint8_t  size_enum;      /**< Selected ::ra8_widget_text_size_t or `0` for compact 8x16. */
  uint8_t  fit_flags;      /**< Bitmask of ::ra8_translate_overlay_fit_flag_t values.      */
  uint8_t  reserved[2];    /**< Zero padding.                                              */
} ra8_translate_overlay_fit_t;

static_assert(sizeof(ra8_translate_overlay_fit_t) == 12U, "ra8_translate_overlay_fit_t size");
static_assert(alignof(ra8_translate_overlay_fit_t) == 2U, "ra8_translate_overlay_fit_t align");
static_assert(offsetof(ra8_translate_overlay_fit_t, font_size_px) == 0U, "fit font_size_px off");
static_assert(offsetof(ra8_translate_overlay_fit_t, line_count) == 2U, "fit line_count off");
static_assert(offsetof(ra8_translate_overlay_fit_t, max_line_width) == 4U,
              "fit max_line_width off");
static_assert(offsetof(ra8_translate_overlay_fit_t, total_height) == 6U, "fit total_height off");
static_assert(offsetof(ra8_translate_overlay_fit_t, size_enum) == 8U, "fit size_enum off");
static_assert(offsetof(ra8_translate_overlay_fit_t, fit_flags) == 9U, "fit fit_flags off");
static_assert(offsetof(ra8_translate_overlay_fit_t, reserved) == 10U, "fit reserved off");

/**
 * @brief Compute the inscribed safe text rectangle inside a bubble mask contour.
 *
 * @details Calling context: `task-safe-reentrant`. Computes a strictly inscribed
 * box respecting shape contours (rounded corners, inscribed ellipse chords, cloud
 * lobes), outline thickness, and inner padding.
 *
 * @param[in] bubble Bubble bounding box and shape metadata.
 * @param[in] style Visual style controlling padding and border width.
 * @param[out] out_rect Non-null output rectangle in page coordinates.
 * @return ::k_ra8_ok on success, or explicit error status leaving @p out_rect untouched.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_overlay_safe_rect(const ra8_translate_overlay_bubble_spec_t* bubble,
                                          const ra8_translate_overlay_style_t*       style,
                                          ra8_ui_rect_t*                             out_rect);

/**
 * @brief Find the largest font size and line-break offsets fitting English text
 * inside a translated speech bubble.
 *
 * @details Calling context: `task-safe-reentrant`. Queries glyph metrics through
 * @p paint without dynamic allocation, prefers clean word wrapping, falls back
 * smoothly for narrow vertical-Japanese bubbles, and writes cumulative `TRN1`
 * byte end offsets into @p out_line_breaks.
 *
 * @param[in] paint `ra8_widget` Paint backend providing font measurement callbacks.
 * @param[in] bubble Target bubble bounding box and contour descriptor.
 * @param[in] style Alignment, padding, face, and weight selection.
 * @param[in] english_utf8 UTF-8 English translation (NULL allowed if @p english_len == 0).
 * @param[in] english_len Byte length of @p english_utf8.
 * @param[out] out_line_breaks Caller array receiving cumulative `u16` line end offsets.
 * @param[in] line_break_cap Element capacity of @p out_line_breaks.
 * @param[out] out_fit Non-null summary receiving fitted pixel height, line count, and flags.
 * @return ::k_ra8_ok on success, or explicit error status leaving outputs untouched.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_overlay_fit_text(const ra8_widget_paint_t*                  paint,
                                         const ra8_translate_overlay_bubble_spec_t* bubble,
                                         const ra8_translate_overlay_style_t*       style,
                                         const uint8_t*                             english_utf8,
                                         size_t                                     english_len,
                                         uint16_t*                                  out_line_breaks,
                                         size_t                                     line_break_cap,
                                         ra8_translate_overlay_fit_t*               out_fit);

/**
 * @brief Paint a single bubble's white mask and centered pre-fitted English lines.
 *
 * @details Calling context: `task-safe-reentrant`. Validates all arguments before
 * invoking any @p paint callback, fills the bubble contour (`rect`, `ellipse`,
 * `rounded_rect`, or `cloud`), typesets each wrapped line centered inside the
 * inscribed safe rect, and reports the bubble's tight damage rectangle.
 *
 * @param[in] paint `ra8_widget` Paint backend (`fill_rect` + text callbacks).
 * @param[in] bubble Bubble bounding geometry and shape contour.
 * @param[in] style Foreground, mask background, border, alignment, and font style.
 * @param[in] font_size_px Fitted line height stored in `TRN1` (`> 0` when text present).
 * @param[in] english_utf8 UTF-8 English string (`NULL` allowed if @p english_len == 0).
 * @param[in] english_len Byte length of @p english_utf8.
 * @param[in] line_breaks Pre-fitted cumulative byte end offsets (`NULL` iff `line_count == 0`).
 * @param[in] line_count Number of entries in @p line_breaks.
 * @param[out] out_damage Non-null rectangle receiving this bubble's tight damage box.
 * @return ::k_ra8_ok on success, or explicit error status with @p paint untouched.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_overlay_render_bubble(const ra8_widget_paint_t*                  paint,
                                              const ra8_translate_overlay_bubble_spec_t* bubble,
                                              const ra8_translate_overlay_style_t*       style,
                                              uint16_t        font_size_px,
                                              const uint8_t*  english_utf8,
                                              size_t          english_len,
                                              const uint16_t* line_breaks,
                                              size_t          line_count,
                                              ra8_ui_rect_t*  out_damage);

/**
 * @brief Render every bubble overlay for one page of a validated `TRN1` sidecar
 * and emit minimal bubble damage rectangles for partial e-ink panel refresh.
 *
 * @details Calling context: `task-safe-reentrant`. Validates `TRN1` integrity,
 * @p page_index, @p style, @p paint capabilities, and @p out_damage_rects capacity
 * upfront before issuing any draw callback, paints bubbles in `reading_order`,
 * and deduplicates strictly contained damage rectangles.
 *
 * @param[in] paint `ra8_widget` Paint backend targeting e-ink or host framebuffer.
 * @param[in] trn1_buf Serialized `TRN1` per-chapter translation sidecar bytes.
 * @param[in] trn1_len Byte length of @p trn1_buf.
 * @param[in] page_index Zero-based page index inside the sidecar.
 * @param[in] style Overlay visual and typography configuration.
 * @param[out] out_damage_rects Caller array receiving tight non-contained damage rects.
 * @param[in] damage_cap Element capacity of @p out_damage_rects.
 * @param[out] out_damage_count Non-null storage receiving emitted damage rect count.
 * @return ::k_ra8_ok on success, or explicit error status leaving framebuffer untouched.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_overlay_render_page(const ra8_widget_paint_t*            paint,
                                            const uint8_t*                       trn1_buf,
                                            size_t                               trn1_len,
                                            uint16_t                             page_index,
                                            const ra8_translate_overlay_style_t* style,
                                            ra8_ui_rect_t*                       out_damage_rects,
                                            size_t                               damage_cap,
                                            size_t*                              out_damage_count);

#ifdef __cplusplus
}
#endif
