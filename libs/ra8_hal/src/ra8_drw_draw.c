/**
 * @file ra8_drw_draw.c
 * @brief 2D Drawing Engine (DRW / D/AVE 2D) geometry primitives
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Geometry-primitive half of the RA8D2 D/AVE 2D driver, split out of
 * ra8_drw.c to keep each translation unit under the file-size cap. This
 * TU owns the drawing primitives (fill / textured-blit / line /
 * triangle), display-list submission and the performance counters.
 * The surface-setup, lifecycle, IRQ and cache helpers remain in
 * ra8_drw.c. This TU keeps its own read-only copy of the ``s_tag``
 * logging tag and reuses the shared limiter helper
 * ``internal_program_rect_limiters`` declared in ra8_drw_internal.h.
 *
 * Every register access carries a HUM Ch 62 citation immediately
 * above it so ``cite_check.py`` can validate provenance.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_drw.h"
#include "ra8_drw_internal.h"
#include "ra8_drw_regs.h"
#include "ra8_err.h"

/* =============================================================================
 * Driver-private state
 * =============================================================================
 */

/**
 * @var s_tag
 * @brief Logging tag for the DRW driver.
 *
 * @details
 * TU-local read-only copy. @c ra8_drw.c keeps its own identical copy so
 * both TUs log under the same @c "DRW" tag without sharing linkage.
 *
 * @note Read-only after definition; do not reassign.
 * @warning Direct modification breaks log correlation.
 * @since 0.1.0
 */
static const char* s_tag = "DRW";

/* =============================================================================
 * Drawing primitives
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_drw_fill_rect(const ra8_drw_rect_t* rect)
{
  RA8_CHECK_NULL_PTR(rect, s_tag, "rect must not be nullptr");
  if ((uint16_t)rect->width_px < k_ra8_drw_min_dim_px ||
      (uint16_t)rect->height_px < k_ra8_drw_min_dim_px) {
    return k_ra8_err_invalid_arg;
  }
  if ((uint16_t)rect->width_px > k_ra8_drw_max_width_px ||
      (uint16_t)rect->height_px > k_ra8_drw_max_height_px) {
    return k_ra8_err_invalid_arg;
  }
  if (ra8_drw_internal_rect_off_surface(rect)) {
    return k_ra8_err_invalid_arg;
  }

  /* COLOR1 goes through the shadowed writer (write-only register). */
  ra8_drw_internal_color1_write(rect->color_argb8888);

  internal_program_rect_bbox(rect);

  /* HUM Ch 62.2.1 "CONTROL: Geometry Control Register", p 3689 */
  /* No spatial limiter: the bounding box scan (HUM Ch 62.6.2 p 3716) already
   * covers exactly the rectangle, so every enable bit stays clear. Driving
   * the quad-box limiters here is what produced the half-sized rect on
   * silicon -- the limiters were fed absolute pixel coordinates while the
   * hardware wants the decision value at the bounding box's top-left corner,
   * and their coverage output then landed as alpha 0x01 rather than 0xFF. */
  *ra8_drw_reg32(k_ra8_drw_off_control) = 0UL;

  /* HUM Ch 62.2.31 "ORIGIN: Framebuffer Base Address Register", p 3705 */
  /* ORIGIN both POSITIONS the bounding box and TRIGGERS the render, so it is
   * written last and points at the rectangle's own top-left pixel rather
   * than the framebuffer base. Without the trigger the engine never
   * rasterizes (silicon-verified: the demo framebuffer stayed zero-filled). */
  *ra8_drw_reg32(k_ra8_drw_off_origin) = ra8_drw_internal_rect_origin(rect);

  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_drw_blit_textured_rect(const ra8_drw_rect_t* rect)
{
  RA8_CHECK_NULL_PTR(rect, s_tag, "rect must not be nullptr");
  if (ra8_drw_internal_rect_below_min((uint16_t)k_ra8_drw_min_dim_px,
                                      (uint16_t)rect->width_px,
                                      (uint16_t)rect->height_px)) {
    return k_ra8_err_invalid_arg;
  }
  if (ra8_drw_internal_rect_above_max((uint16_t)k_ra8_drw_max_width_px,
                                      (uint16_t)k_ra8_drw_max_height_px,
                                      (uint16_t)rect->width_px,
                                      (uint16_t)rect->height_px)) {
    return k_ra8_err_invalid_arg;
  }
  if (ra8_drw_internal_rect_off_surface(rect)) {
    return k_ra8_err_invalid_arg;
  }

  internal_program_rect_bbox(rect);

  /* HUM Ch 62.2.18 "LUSTART: U Limiter Start Value Register" p 3701 */
  *ra8_drw_reg32(k_ra8_drw_off_lustart) = 0UL;
  /* HUM Ch 62.2.18 "LUSTART: U Limiter Start Value Register" p 3701 */
  *ra8_drw_reg32(k_ra8_drw_off_luxadd) = (uint32_t)k_ra8_drw_subpixel_unit;
  /* HUM Ch 62.2.18 "LUSTART: U Limiter Start Value Register" p 3701 */
  *ra8_drw_reg32(k_ra8_drw_off_luyadd) = 0UL;

  /* HUM Ch 62.2.20 "LVSTARTI: V Limiter Start Integer Part", p 3702 */
  *ra8_drw_reg32(k_ra8_drw_off_lvstarti) = 0UL;
  /* HUM Ch 62.2.21 "LVSTARTF: V Limiter Start Fractional Part", p 3702 */
  *ra8_drw_reg32(k_ra8_drw_off_lvstartf) = 0UL;
  /* HUM Ch 62.2.22 "LVXADDI: V Limiter X-Axis Increment Integer", p 3702 */
  *ra8_drw_reg32(k_ra8_drw_off_lvxaddi) = 0UL;
  /* HUM Ch 62.2.23 "LVYADDI: V Limiter Y-Axis Increment Integer", p 3702 */
  *ra8_drw_reg32(k_ra8_drw_off_lvyaddi) = (uint32_t)k_ra8_drw_subpixel_unit;
  /* HUM Ch 62.2.24 "LVYXADDF: V Limiter Increment Fractional", p 3703 */
  *ra8_drw_reg32(k_ra8_drw_off_lvyxaddf) = 0UL;

  /* HUM Ch 62.2.4 "CACHECTL: Cache Control Register", p 3694 */
  /* Pulse the texture cache flush so this blit cannot consume texels a
 * previous primitive left cached. The framebuffer cache is left exactly as
 * ::ra8_drw_init configured it -- a primitive must not silently switch on a
 * cache the caller asked to keep off, because the rendered pixels then sit
 * in the DRW cache instead of memory until something flushes them. */
  *ra8_drw_reg32(k_ra8_drw_off_cachectl) = k_ra8_drw_cachectl_cflushtx;

  /* HUM Ch 62.2.1 "CONTROL: Geometry Control Register", p 3689 */
  /* No spatial limiter -- the bounding box scan is the rectangle. */
  *ra8_drw_reg32(k_ra8_drw_off_control) = 0UL;

  /* HUM Ch 62.2.31 "ORIGIN: Framebuffer Base Address Register", p 3705 */
  /* ORIGIN positions the bounding box AND triggers the render. */
  *ra8_drw_reg32(k_ra8_drw_off_origin) = ra8_drw_internal_rect_origin(rect);
  return k_ra8_ok;
}

/**
 * @brief Program L1..L4 START/XADD/YADD for a stroked line.
 *
 * @details
 * HUM Ch 62.2.10-62.2.12 (LnSTART/LnXADD/LnYADD pp 3698-3699). The
 * line is encoded as four perpendicular limiters per HUM Ch 62.4.4
 * "Lines" p 3725.
 *
 * @param[in] line Validated line descriptor.
 *
 * @pre ``line`` is non-null.
 * @post Limiter registers reflect the line stroke envelope.
 *
 * @note Internal helper, not thread-safe.
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_program_line_limiters(const ra8_drw_line_t* line)
{
  const int32_t dx  = line->x1 - line->x0;
  const int32_t dy  = line->y1 - line->y0;
  const int32_t adx = internal_iabs(dx);
  const int32_t ady = internal_iabs(dy);

  /* HUM Ch 62.2.10 "LnSTART: Limiter n Start Value Register", p 3698 */
  *ra8_drw_reg32(k_ra8_drw_off_l1start) = internal_to_subpixel(line->x0);
  *ra8_drw_reg32(k_ra8_drw_off_l2start) = internal_to_subpixel(line->x0);
  *ra8_drw_reg32(k_ra8_drw_off_l3start) = internal_to_subpixel(line->y0);
  *ra8_drw_reg32(k_ra8_drw_off_l4start) = internal_to_subpixel(line->y0);

  /* HUM Ch 62.2.11 "LnXADD: Limiter n X-Axis Increment Register", p 3698 */
  *ra8_drw_reg32(k_ra8_drw_off_l1xadd) = internal_to_subpixel(adx);
  *ra8_drw_reg32(k_ra8_drw_off_l2xadd) = internal_to_subpixel(-adx);
  *ra8_drw_reg32(k_ra8_drw_off_l3xadd) = internal_to_subpixel(ady);
  *ra8_drw_reg32(k_ra8_drw_off_l4xadd) = internal_to_subpixel(-ady);

  /* HUM Ch 62.2.12 "LnYADD: Limiter n Y-Axis Increment Register", p 3699 */
  *ra8_drw_reg32(k_ra8_drw_off_l1yadd) = internal_to_subpixel(ady);
  *ra8_drw_reg32(k_ra8_drw_off_l2yadd) = internal_to_subpixel(-ady);
  *ra8_drw_reg32(k_ra8_drw_off_l3yadd) = internal_to_subpixel(adx);
  *ra8_drw_reg32(k_ra8_drw_off_l4yadd) = internal_to_subpixel(-adx);

  /* HUM Ch 62.2.13 "L1BAND/L2BAND: Limiter Band Width Register", p 3699.
   * Band width = stroke width in sub-pixels for L1 and L2. */
  const uint32_t band = (uint32_t)line->width_px * (uint32_t)k_ra8_drw_subpixel_unit;
  *ra8_drw_reg32(k_ra8_drw_off_l1band) = band;
  *ra8_drw_reg32(k_ra8_drw_off_l2band) = band;
}

[[nodiscard]] ra8_err_t ra8_drw_draw_line(const ra8_drw_line_t* line)
{
  RA8_CHECK_NULL_PTR(line, s_tag, "line must not be nullptr");
  if (line->width_px == 0U || (uint16_t)line->width_px > k_ra8_drw_max_width_px) {
    return k_ra8_err_invalid_arg;
  }

  /* COLOR1 goes through the shadowed writer (write-only register). */
  ra8_drw_internal_color1_write(line->color_argb8888);

  /* Compute the bounding box of the stroke for SIZE: a simple
   * over-estimate using axis-aligned span + width. The DRW only uses
   * SIZE as an enumeration upper bound, not for clipping the stroke
   * itself. HUM Ch 62.2.29 p 3704. */
  const int32_t  min_x  = (line->x0 < line->x1) ? line->x0 : line->x1;
  const int32_t  max_x  = (line->x0 > line->x1) ? line->x0 : line->x1;
  const int32_t  min_y  = (line->y0 < line->y1) ? line->y0 : line->y1;
  const int32_t  max_y  = (line->y0 > line->y1) ? line->y0 : line->y1;
  const uint32_t span_x = (uint32_t)(max_x - min_x) + (uint32_t)line->width_px;
  const uint32_t span_y = (uint32_t)(max_y - min_y) + (uint32_t)line->width_px;

  /* HUM Ch 62.2.29 "SIZE: Bounding Box Dimension Register", p 3704 */
  *ra8_drw_reg32(k_ra8_drw_off_size) = (span_y << k_ra8_drw_size_height_pos) | span_x;

  internal_program_line_limiters(line);

  /* HUM Ch 62.2.4 "CACHECTL: Cache Control Register", p 3694 */
  *ra8_drw_reg32(k_ra8_drw_off_cachectl) =
    (k_ra8_drw_cachectl_all_en | k_ra8_drw_cachectl_cflushfx);

  /* HUM Ch 62.2.1 "CONTROL: Geometry Control Register", p 3689 */
  *ra8_drw_reg32(k_ra8_drw_off_control) = k_ra8_drw_control_line_quad;

  /* HUM Ch 62.2.31 "ORIGIN: Framebuffer Base Address Register", p 3705 */
  /* ORIGIN write = render trigger (see ra8_drw_fill_rect). */
  *ra8_drw_reg32(k_ra8_drw_off_origin) = ra8_drw_internal_origin();
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_drw_draw_triangle(const ra8_drw_triangle_t* tri)
{
  RA8_CHECK_NULL_PTR(tri, s_tag, "tri must not be nullptr");

  /* COLOR1 goes through the shadowed writer (write-only register). */
  ra8_drw_internal_color1_write(tri->color_argb8888);

  /* Bounding box for SIZE -- HUM Ch 62.2.29 p 3704. */
  int32_t min_x = (tri->x0 < tri->x1) ? tri->x0 : tri->x1;
  if (tri->x2 < (int16_t)min_x) {
    min_x = tri->x2;
  }
  int32_t max_x = (tri->x0 > tri->x1) ? tri->x0 : tri->x1;
  if (tri->x2 > (int16_t)max_x) {
    max_x = tri->x2;
  }
  int32_t min_y = (tri->y0 < tri->y1) ? tri->y0 : tri->y1;
  if (tri->y2 < (int16_t)min_y) {
    min_y = tri->y2;
  }
  int32_t max_y = (tri->y0 > tri->y1) ? tri->y0 : tri->y1;
  if (tri->y2 > (int16_t)max_y) {
    max_y = tri->y2;
  }
  const uint32_t span_x = (uint32_t)(max_x - min_x);
  const uint32_t span_y = (uint32_t)(max_y - min_y);

  *ra8_drw_reg32(k_ra8_drw_off_size) = (span_y << k_ra8_drw_size_height_pos) | span_x;

  /* HUM Ch 62.4.5 "Triangles" p 3726: three half-plane limiters,
 * one per edge. The HAL programmes the simplest "axis-aligned
 * triangle" encoding -- the application can override LnSTART /
 * LnXADD / LnYADD via display lists for arbitrary geometry. */

  /* HUM Ch 62.2.10 "LnSTART: Limiter n Start Value Register", p 3698 */
  *ra8_drw_reg32(k_ra8_drw_off_l1start) = internal_to_subpixel(tri->x0);
  *ra8_drw_reg32(k_ra8_drw_off_l2start) = internal_to_subpixel(tri->x1);
  *ra8_drw_reg32(k_ra8_drw_off_l3start) = internal_to_subpixel(tri->x2);

  /* HUM Ch 62.2.11 "LnXADD: Limiter n X-Axis Increment", p 3698 */
  *ra8_drw_reg32(k_ra8_drw_off_l1xadd) = internal_to_subpixel((int32_t)tri->y1 - (int32_t)tri->y0);
  *ra8_drw_reg32(k_ra8_drw_off_l2xadd) = internal_to_subpixel((int32_t)tri->y2 - (int32_t)tri->y1);
  *ra8_drw_reg32(k_ra8_drw_off_l3xadd) = internal_to_subpixel((int32_t)tri->y0 - (int32_t)tri->y2);

  /* HUM Ch 62.2.12 "LnYADD: Limiter n Y-Axis Increment", p 3699 */
  *ra8_drw_reg32(k_ra8_drw_off_l1yadd) = internal_to_subpixel((int32_t)tri->x0 - (int32_t)tri->x1);
  *ra8_drw_reg32(k_ra8_drw_off_l2yadd) = internal_to_subpixel((int32_t)tri->x1 - (int32_t)tri->x2);
  *ra8_drw_reg32(k_ra8_drw_off_l3yadd) = internal_to_subpixel((int32_t)tri->x2 - (int32_t)tri->x0);

  /* HUM Ch 62.2.4 "CACHECTL: Cache Control Register", p 3694 */
  *ra8_drw_reg32(k_ra8_drw_off_cachectl) =
    (k_ra8_drw_cachectl_all_en | k_ra8_drw_cachectl_cflushfx);

  /* HUM Ch 62.2.1 "CONTROL: Geometry Control Register", p 3689 */
  *ra8_drw_reg32(k_ra8_drw_off_control) = k_ra8_drw_control_triangle;

  /* HUM Ch 62.2.31 "ORIGIN: Framebuffer Base Address Register", p 3705 */
  /* ORIGIN write = render trigger (see ra8_drw_fill_rect). */
  *ra8_drw_reg32(k_ra8_drw_off_origin) = ra8_drw_internal_origin();
  return k_ra8_ok;
}

/* =============================================================================
 * Display list mode
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_drw_run_dlist(const uint32_t* dlist_addr)
{
  if (dlist_addr == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (((uintptr_t)dlist_addr & (uintptr_t)k_ra8_drw_internal_align_mask) != 0U) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 62.2.4 "CACHECTL: Cache Control Register", p 3694 */
  /* Flush both caches so the dlist words written by the CPU are
 * visible to the DRW bus initiator. */
  const uint32_t cur_cc                  = *ra8_drw_reg32(k_ra8_drw_off_cachectl);
  *ra8_drw_reg32(k_ra8_drw_off_cachectl) = cur_cc | k_ra8_drw_cachectl_all_flush;

  /* HUM Ch 62.2.32 "DLISTSTART: Display List Start Address Register",
 * p 3705. Writing a new value triggers execution. */
  *ra8_drw_reg32(k_ra8_drw_off_dliststart) = (uint32_t)(uintptr_t)dlist_addr;
  return k_ra8_ok;
}

/**
 * @enum ra8_drw_dlist_span_t
 * @brief Word spans of the display-list entries the builder emits.
 *
 * @details
 * A single-register entry is a tag word plus one value word; a special word
 * (wait / terminate) is one word. A solid-fill primitive programs COLOR1,
 * SIZE, CONTROL and ORIGIN and appends one wait word.
 *
 * @invariant ``k_ra8_drw_dlist_fill_words`` == fill regs * entry words + 1.
 * @see ra8_drw_dlist_add_fill
 */
typedef enum : uint8_t {
  k_ra8_drw_dlist_entry_words   = 2U, /**< 1-register entry: tag + value. */
  k_ra8_drw_dlist_special_words = 1U, /**< Wait or terminate word.        */
  k_ra8_drw_dlist_fill_regs     = 4U, /**< COLOR1, SIZE, CONTROL, ORIGIN. */
  k_ra8_drw_dlist_fill_words    = 9U, /**< 4 * 2 + 1 wait word.           */
} ra8_drw_dlist_span_t;

/**
 * @brief Append a single-register display-list entry to the builder buffer.
 *
 * @details
 * Writes a one-index tag (register index OR ::k_ra8_drw_dlr_tag_one_index)
 * followed by the value word. The caller (::ra8_drw_dlist_add_fill) reserves
 * the whole primitive's span up front, so this writer never bounds-checks --
 * it is a plain memory append to the caller buffer, never MMIO.
 *
 * @param[in,out] dl  Bound builder with at least two free words.
 * @param[in]     idx Register index (::ra8_drw_dlr_index_t).
 * @param[in]     val Value word to program.
 *
 * @pre ``dl`` and ``dl->buf`` are non-null.
 * @pre ``dl->count + 2 <= dl->cap_words`` (caller-reserved).
 * @post Two words were appended and ``dl->count`` grew by two.
 *
 * @note Not thread-safe; caller-buffer writes only.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_dlist_put_reg(ra8_drw_dlist_t* dl, ra8_drw_dlr_index_t idx, uint32_t val)
{
  dl->buf[dl->count]      = (uint32_t)k_ra8_drw_dlr_tag_one_index | (uint32_t)idx;
  dl->buf[dl->count + 1U] = val;
  dl->count += (uint32_t)k_ra8_drw_dlist_entry_words;
}

/**
 * @brief Append a display-list special word (wait or terminate).
 *
 * @details
 * Encodes ::k_ra8_drw_dlr_eol_byte with @p arg in byte 1. The caller reserves
 * the word up front, so this writer never bounds-checks.
 *
 * @param[in,out] dl  Bound builder with at least one free word.
 * @param[in]     arg End-of-list argument (wait / terminate).
 *
 * @pre ``dl`` and ``dl->buf`` are non-null.
 * @pre ``dl->count + 1 <= dl->cap_words`` (caller-reserved).
 * @post One word was appended and ``dl->count`` grew by one.
 *
 * @note Not thread-safe; caller-buffer writes only.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_dlist_put_special(ra8_drw_dlist_t* dl, uint32_t arg)
{
  dl->buf[dl->count] =
    (uint32_t)k_ra8_drw_dlr_eol_byte | (arg << (uint32_t)k_ra8_drw_dlr_eol_arg_pos);
  dl->count += (uint32_t)k_ra8_drw_dlist_special_words;
}

[[nodiscard]] ra8_err_t ra8_drw_dlist_begin(ra8_drw_dlist_t* dl, uint32_t* buf, uint32_t cap_words)
{
  RA8_CHECK_NULL_PTR(dl, s_tag, "dl must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  if (((uintptr_t)buf & (uintptr_t)k_ra8_drw_internal_align_mask) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (cap_words == 0U) {
    return k_ra8_err_invalid_arg;
  }
  dl->buf        = buf;
  dl->cap_words  = cap_words;
  dl->count      = 0U;
  dl->overflow   = false;
  dl->terminated = false;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_drw_dlist_add_fill(ra8_drw_dlist_t* dl, const ra8_drw_rect_t* rect)
{
  RA8_CHECK_NULL_PTR(dl, s_tag, "dl must not be nullptr");
  RA8_CHECK_NULL_PTR(rect, s_tag, "rect must not be nullptr");
  if (ra8_drw_internal_rect_below_min((uint16_t)k_ra8_drw_min_dim_px,
                                      (uint16_t)rect->width_px,
                                      (uint16_t)rect->height_px)) {
    return k_ra8_err_invalid_arg;
  }
  if (ra8_drw_internal_rect_above_max((uint16_t)k_ra8_drw_max_width_px,
                                      (uint16_t)k_ra8_drw_max_height_px,
                                      (uint16_t)rect->width_px,
                                      (uint16_t)rect->height_px)) {
    return k_ra8_err_invalid_arg;
  }
  if (ra8_drw_internal_rect_off_surface(rect)) {
    return k_ra8_err_invalid_arg;
  }
  /* Reject up front so no partial primitive is left in the buffer. */
  if ((dl->count + (uint32_t)k_ra8_drw_dlist_fill_words) > dl->cap_words) {
    dl->overflow = true;
    return k_ra8_err_no_mem;
  }

  /* Same geometry as the register-mode ra8_drw_fill_rect: COLOR1 carries the
   * ARGB, SIZE the bounding box, CONTROL clears the spatial limiters so the
   * box scan is the rectangle, and ORIGIN (the render trigger) anchors it at
   * the rectangle's top-left pixel. CONTROL2 / PITCH / cache stay as
   * ra8_drw_init programmed them. */
  internal_dlist_put_reg(dl, k_ra8_drw_dlr_idx_color1, rect->color_argb8888);
  internal_dlist_put_reg(dl,
                         k_ra8_drw_dlr_idx_size,
                         ((uint32_t)rect->height_px << k_ra8_drw_size_height_pos) |
                           (uint32_t)rect->width_px);
  internal_dlist_put_reg(dl, k_ra8_drw_dlr_idx_control, 0UL);
  internal_dlist_put_reg(dl, k_ra8_drw_dlr_idx_origin, ra8_drw_internal_rect_origin(rect));
  /* Wait for the pipeline and framebuffer cache to drain so this primitive
   * fully lands before the next one (or the CPU read) begins. */
  internal_dlist_put_special(dl, (uint32_t)k_ra8_drw_dlr_arg_wait);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_drw_dlist_end(ra8_drw_dlist_t* dl)
{
  RA8_CHECK_NULL_PTR(dl, s_tag, "dl must not be nullptr");
  if (dl->overflow) {
    return k_ra8_err_invalid_arg;
  }
  if ((dl->count + (uint32_t)k_ra8_drw_dlist_special_words) > dl->cap_words) {
    dl->overflow = true;
    return k_ra8_err_no_mem;
  }
  internal_dlist_put_special(dl, (uint32_t)k_ra8_drw_dlr_arg_terminate);
  dl->terminated = true;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_drw_dlist_run(const ra8_drw_dlist_t* dl)
{
  RA8_CHECK_NULL_PTR(dl, s_tag, "dl must not be nullptr");
  if (dl->overflow || !dl->terminated || (dl->count == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  return ra8_drw_run_dlist(dl->buf);
}

/* =============================================================================
 * Performance counters
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_drw_perf_arm(ra8_drw_perftrigger_t event_ctr1,
                                         ra8_drw_perftrigger_t event_ctr2)
{
  if ((uint16_t)event_ctr1 > k_ra8_drw_internal_perfev_max ||
      (uint16_t)event_ctr2 > k_ra8_drw_internal_perfev_max) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 62.2.33 "PERFTRIGGER: Performance Counters Control Register",
 * p 3706. PERFTRIGGER1 in [15:0], PERFTRIGGER2 in [31:16]. */
  *ra8_drw_reg32(k_ra8_drw_off_perftrigger) =
    ((uint32_t)event_ctr2 << k_ra8_drw_perftrigger2_pos) | (uint32_t)event_ctr1;

  /* HUM Ch 62.2.34 "PERFCOUNTk: Performance Counter k", p 3706 */
  *ra8_drw_reg32(k_ra8_drw_off_perfcount1) = 0UL;
  *ra8_drw_reg32(k_ra8_drw_off_perfcount2) = 0UL;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_drw_perf_read(ra8_drw_perfcounter_id_t id, uint32_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  ra8_err_t result = k_ra8_err_invalid_arg;
  /* HUM Ch 62.2.34 "PERFCOUNTk: Performance Counter k", p 3706 */
  if (id == k_ra8_drw_perfctr_1) {
    *out   = *ra8_drw_reg32(k_ra8_drw_off_perfcount1);
    result = k_ra8_ok;
  } else if (id == k_ra8_drw_perfctr_2) {
    *out   = *ra8_drw_reg32(k_ra8_drw_off_perfcount2);
    result = k_ra8_ok;
  } else {
    result = k_ra8_err_invalid_arg;
  }
  return result;
}

[[nodiscard]] ra8_err_t ra8_drw_perf_reset(ra8_drw_perfcounter_id_t id)
{
  ra8_err_t result = k_ra8_err_invalid_arg;
  /* HUM Ch 62.2.34 "PERFCOUNTk: Performance Counter k", p 3706 */
  if (id == k_ra8_drw_perfctr_1) {
    *ra8_drw_reg32(k_ra8_drw_off_perfcount1) = 0UL;
    result                                   = k_ra8_ok;
  } else if (id == k_ra8_drw_perfctr_2) {
    *ra8_drw_reg32(k_ra8_drw_off_perfcount2) = 0UL;
    result                                   = k_ra8_ok;
  } else {
    result = k_ra8_err_invalid_arg;
  }
  return result;
}
