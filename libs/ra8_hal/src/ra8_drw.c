/**
 * @file ra8_drw.c
 * @brief 2D Drawing Engine (DRW / D/AVE 2D) HAL driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Full RA8D2 D/AVE 2D driver. the expansion takes the driver
 * from the original solid-fill MVP up to the complete HUM Ch 62
 * register surface: textured blits, CLUT loads, RLE-encoded source,
 * per-pixel + global alpha blending, anti-aliased line / triangle,
 * display-list mode, every IRQ source, performance counters and
 * cache-flush hooks.
 *
 * Every register access carries a HUM Ch 62 citation immediately
 * above it so ``cite_check.py`` can validate provenance.
 *
 * @par State Machine
 * @dot
 * digraph ra8_drw_states {
 *   bgcolor="transparent";
 *   rankdir=LR;
 *   node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
 *         fillcolor="#e8eef7", color="#5a7ca6"];
 *   edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];
 *
 *   __start [shape=circle, width=0.18, label="", fillcolor="#5a7ca6", color="#5a7ca6"];
 *
 *   reset [label="reset"];
 *   initialized [label="initialized"];
 *   stopped [label="stopped"];
 *
 *   __start -> reset;
 *   reset -> initialized [label="ra8_drw_init"];
 *   initialized -> stopped [label="ra8_drw_enter_stop"];
 *   stopped -> initialized [label="ra8_drw_exit_stop"];
 *   initialized -> reset [label="ra8_drw_deinit"];
 * }
 * @enddot
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_drw.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_drw_internal.h"
#include "ra8_drw_regs.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_lpm.h"
#include "ra8_mstp.h"

/**
 * @brief Pure rect-below-min predicate -- see header for full contract.
 * @details Promoted helper so the line-776 OR can be driven under MC/DC.
 * @param[in] min_dim Minimum permitted dimension (1 px).
 * @param[in] width   Width in pixels.
 * @param[in] height  Height in pixels.
 * @return Boolean reject predicate.
 * @retval true  Dimension below min.
 * @retval false Both dimensions in range.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool ra8_drw_internal_rect_below_min(uint16_t min_dim, uint16_t width, uint16_t height)
{
  return (width < min_dim) || (height < min_dim);
}

/**
 * @brief Pure rect-above-max predicate -- see header for full contract.
 * @details Promoted helper so the line-780 OR can be driven under MC/DC.
 * @param[in] max_w  Maximum permitted width.
 * @param[in] max_h  Maximum permitted height.
 * @param[in] width  Width in pixels.
 * @param[in] height Height in pixels.
 * @return Boolean reject predicate.
 * @retval true  Dimension above max.
 * @retval false Both within bounds.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool ra8_drw_internal_rect_above_max(uint16_t max_w,
                                     uint16_t max_h,
                                     uint16_t width,
                                     uint16_t height)
{
  return (width > max_w) || (height > max_h);
}

/* =============================================================================
 * Driver-private state
 * =============================================================================
 */

/**
 * @var s_tag
 * @brief Logging tag for the DRW driver.
 *
 * @details
 * TU-local read-only copy. @c ra8_drw_draw.c keeps its own identical copy
 * so both TUs log under the same @c "DRW" tag without sharing linkage.
 *
 * @note Read-only after definition; do not reassign.
 * @warning Direct modification breaks log correlation.
 * @since 0.1.0
 */
static const char* s_tag = "DRW";

/**
 * @var s_drw_fn
 * @brief Currently registered DRW event callback (or ``nullptr``).
 */
static ra8_drw_event_fn_t s_drw_fn;

/**
 * @var s_drw_ctx
 * @brief Opaque context paired with ``s_drw_fn``.
 */
static void* s_drw_ctx;

/**
 * @var s_drw_origin
 * @brief Cached ORIGIN (framebuffer base) for the per-primitive render trigger.
 *
 * @details
 * HUM Ch 62.2.31: writing ORIGIN triggers the start of rendering, so every
 * geometry primitive must re-write it as its final step (the CONTROL write
 * only selects the geometry mode). Cached at ::ra8_drw_init so the draw TU
 * can issue the trigger without re-deriving the framebuffer address.
 *
 * @note Written by ::ra8_drw_init, cleared by ::ra8_drw_deinit.
 * @warning Do not modify outside init/deinit -- primitives only read it.
 * @since 0.1.0
 */
static uint32_t s_drw_origin;

/**
 * @var s_drw_control2
 * @brief Software shadow of the write-only CONTROL2 register.
 *
 * @details
 * Every CONTROL2 bit is W-only (HUM Ch 62.2.2, R/W column "W"), so a
 * hardware read-modify-write returns undefined data on silicon. All
 * CONTROL2 updates go through ::internal_control2_rmw, which modifies
 * this shadow and writes the full value out.
 *
 * @note Seeded by ::ra8_drw_init with the packed WRITEFORMAT bits.
 * @warning Never read CONTROL2 from hardware; use this shadow.
 * @since 0.1.0
 */
static uint32_t s_drw_control2;

/**
 * @var s_drw_color1
 * @brief Software shadow of the write-only COLOR1 register.
 *
 * @details
 * COLOR1 is W-only like the rest of the DRW register file, so the
 * alpha-preserving update in ::ra8_drw_set_blend cannot read it back from
 * hardware. Every COLOR1 write goes through
 * ::ra8_drw_internal_color1_write, which keeps this shadow coherent.
 *
 * @note Updated on every COLOR1 write (gradient, blend, fill).
 * @warning Never read COLOR1 from hardware; use this shadow.
 * @since 0.1.0
 */
static uint32_t s_drw_color1;

/** @brief Implementation of `ra8_drw_internal_origin()` -- shadow read. */
uint32_t ra8_drw_internal_origin(void)
{
  return s_drw_origin;
}

/** @brief Implementation of `ra8_drw_internal_color1_write()` -- MMIO + shadow. */
void ra8_drw_internal_color1_write(uint32_t argb8888)
{
  /* HUM Ch 62.2.7 "COLOR1: Base Color Register", p 3697 */
  *ra8_drw_reg32(k_ra8_drw_off_color1) = argb8888;
  s_drw_color1                         = argb8888;
}

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Fold the three-bit WRITEFORMAT field into a 32-bit CONTROL2
 * value (WRITEFORMAT2 lives at bit 8, WRITEFORMAT[1:0] at
 * bits 21:20).
 *
 * @param[in] fmt One of ``ra8_drw_writeformat_t``.
 * @return CONTROL2 word with format bits set, all other bits zero.
 *
 * @pre ``fmt`` is one of the documented enum values (0..3).
 * @pre Caller will OR additional CONTROL2 bits as needed.
 *
 * @post Returned value has only the format bits set.
 * @post No side effects.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline uint32_t internal_pack_writeformat(ra8_drw_writeformat_t fmt)
{
  const uint32_t low  = ((uint32_t)fmt & 0x3U) << k_ra8_drw_control2_writeformat_pos;
  uint32_t       high = 0UL;
  if (((uint32_t)fmt & 0x4UL) != 0UL) {
    high = k_ra8_drw_control2_writeformat2_bit;
  }
  return low | high;
}

/**
 * @brief Pack a 4-bit READFORMAT value into CONTROL2.READFORMAT_H+L.
 *
 * @param[in] fmt One of ``ra8_drw_readformat_t``.
 * @return CONTROL2 word with the four READFORMAT bits set.
 *
 * @pre ``fmt`` is in the documented set (0x0..0xC).
 * @pre Caller OR-combines with the rest of CONTROL2.
 * @post Returned word touches only READFORMAT_L (18:19) and
 * READFORMAT_H (4:5).
 * @post No side effects.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline uint32_t internal_pack_readformat(ra8_drw_readformat_t fmt)
{
  const uint32_t code = (uint32_t)fmt;
  const uint32_t lo   = (code & 0x3U) << k_ra8_drw_control2_readformatl_pos;
  const uint32_t hi   = ((code >> 2U) & 0x3U) << k_ra8_drw_control2_readformath_pos;
  return lo | hi;
}

/**
 * @brief Read-modify-write a CONTROL2 mask; preserve unrelated bits.
 *
 * @param[in] clear_mask Bits to clear before applying ``set_mask``.
 * @param[in] set_mask Bits to set after the clear.
 * @return Updated CONTROL2 value (also written back to the register).
 *
 * @pre Driver initialized; CONTROL2 is reachable.
 * @pre Caller must ensure no draw is in flight (CONTROL2 is latched
 * at the start of an enumeration).
 * @post CONTROL2 = (old & ~clear_mask) | set_mask.
 * @post Other CONTROL2 fields preserved.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline uint32_t internal_control2_rmw(uint32_t clear_mask, uint32_t set_mask)
{
  /* CONTROL2 is write-only on silicon (HUM Ch 62.2.2, R/W column "W"), so the
   * read-modify-write MUST source the software shadow, never the register. */
  const uint32_t nxt = (s_drw_control2 & ~clear_mask) | set_mask;
  /* HUM Ch 62.2.2 "CONTROL2: Surface Control Register", p 3691 */
  *ra8_drw_reg32(k_ra8_drw_off_control2) = nxt;
  s_drw_control2                         = nxt;
  return nxt;
}

void internal_program_rect_limiters(const ra8_drw_rect_t* rect)
{
  const int32_t  x0 = (int32_t)rect->x;
  const int32_t  y0 = (int32_t)rect->y;
  const int32_t  x1 = x0 + (int32_t)rect->width_px;
  const int32_t  y1 = y0 + (int32_t)rect->height_px;
  const uint32_t l1 = internal_to_subpixel(x0);
  const uint32_t l2 = internal_to_subpixel(x1);
  const uint32_t l3 = internal_to_subpixel(y0);
  const uint32_t l4 = internal_to_subpixel(y1);

  /* HUM Ch 62.2.29 "SIZE: Bounding Box Dimension Register", p 3704 */
  *ra8_drw_reg32(k_ra8_drw_off_size) =
    ((uint32_t)rect->height_px << k_ra8_drw_size_height_pos) | (uint32_t)rect->width_px;

  /* HUM Ch 62.2.10 "LnSTART: Limiter n Start Value Register", p 3698 */
  *ra8_drw_reg32(k_ra8_drw_off_l1start) = l1;
  *ra8_drw_reg32(k_ra8_drw_off_l2start) = l2;
  *ra8_drw_reg32(k_ra8_drw_off_l3start) = l3;
  *ra8_drw_reg32(k_ra8_drw_off_l4start) = l4;

  /* HUM Ch 62.2.11 "LnXADD: Limiter n X-Axis Increment Register", p 3698 */
  *ra8_drw_reg32(k_ra8_drw_off_l1xadd) = (uint32_t)k_ra8_drw_subpixel_unit;
  *ra8_drw_reg32(k_ra8_drw_off_l2xadd) = (uint32_t)k_ra8_drw_subpixel_unit;
  *ra8_drw_reg32(k_ra8_drw_off_l3xadd) = 0UL;
  *ra8_drw_reg32(k_ra8_drw_off_l4xadd) = 0UL;

  /* HUM Ch 62.2.12 "LnYADD: Limiter n Y-Axis Increment Register", p 3699 */
  *ra8_drw_reg32(k_ra8_drw_off_l1yadd) = 0UL;
  *ra8_drw_reg32(k_ra8_drw_off_l2yadd) = 0UL;
  *ra8_drw_reg32(k_ra8_drw_off_l3yadd) = (uint32_t)k_ra8_drw_subpixel_unit;
  *ra8_drw_reg32(k_ra8_drw_off_l4yadd) = (uint32_t)k_ra8_drw_subpixel_unit;
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_drw_init(const ra8_drw_config_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");

  /* The DRW lives in the GRAPHICS POWER DOMAIN, which is gated OFF out of
   * reset (PDCTRGD = 0x81: PDPGSF = 1, PDDE = 1 -- HUM Ch 11.2.14 p 452;
   * domain contents in HUM Ch 11.5.1 Table 11.7 p 480). Cancelling
   * module-stop is NOT sufficient: with the domain dark every DRW register
   * reads 0 and every write is lost, which is exactly why the engine never
   * rasterised a pixel on silicon (#247) while board_sim -- modelling no
   * power domain -- rendered happily. Power the domain first.
   * Bench evidence: HWREVISION reads 0x00000000 before this call and
   * 0x0FBE0107 after it. */
  const ra8_err_t pd_err = ra8_lpm_graphics_power_on((uint32_t)k_ra8_lpm_pd_timeout_default);
  RA8_RETURN_ON_ERROR(pd_err, s_tag, "drw_init: graphics power"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 11.2.8 "MSTPCRC: Module Stop Control Register C", p 446 */
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_drw);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "drw_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 62.2.3 "IRQCTL: Interrupt Control Register", p 3693 */
  /* Clear all pending IRQs and leave them masked. */
  *ra8_drw_reg32(k_ra8_drw_off_irqctl) = k_ra8_drw_irqctl_all_clr;

  /* HUM Ch 62.2.31 "ORIGIN: Framebuffer Base Address Register", p 3705 */
  /* Cache the framebuffer base: writing ORIGIN is the render TRIGGER, so
   * every primitive re-writes it as its final step (see ra8_drw_draw.c).
   * This init write fires a trigger too, but the limiters/SIZE are still
   * zero so no pixel is produced. */
  s_drw_origin                         = (uint32_t)cfg->framebuffer_addr;
  *ra8_drw_reg32(k_ra8_drw_off_origin) = s_drw_origin;

  /* HUM Ch 62.2.30 "PITCH: Framebuffer Pitch and Spanstore Delay
 * Register", p 3705. Spanstore delay (SSD) field [31:16] kept 0. */
  *ra8_drw_reg32(k_ra8_drw_off_pitch) = (uint32_t)cfg->pitch_px;

  /* HUM Ch 62.2.2 "CONTROL2: Surface Control Register", p 3690 */
  /* Plain solid-fill mode: no texture, no pattern, no blend; only the
 * framebuffer pixel format bits are set. Seed the write-only shadow. */
  s_drw_control2                         = internal_pack_writeformat(cfg->format);
  *ra8_drw_reg32(k_ra8_drw_off_control2) = s_drw_control2;
  s_drw_color1                           = 0U;

  /* HUM Ch 62.2.4 "CACHECTL: Cache Control Register", p 3694 */
  if (cfg->enable_caches) {
    *ra8_drw_reg32(k_ra8_drw_off_cachectl) = k_ra8_drw_cachectl_all_en;
  } else {
    *ra8_drw_reg32(k_ra8_drw_off_cachectl) = 0UL;
  }

  /* HUM Ch 62.2.35 "DBWER: DRW Bufferable Write Enable", p 3707 */
  if (cfg->enable_buffered_writes) {
    *ra8_drw_reg32(k_ra8_drw_off_dbwer) = k_ra8_drw_dbwer_bwe;
  } else {
    *ra8_drw_reg32(k_ra8_drw_off_dbwer) = 0UL;
  }

  ra8_log_info_val(s_tag, "drw_init fb", (uint32_t)cfg->framebuffer_addr);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_drw_deinit(void)
{
  /* HUM Ch 62.2.3 "IRQCTL: Interrupt Control Register", p 3693 */
  /* Clear and disable every interrupt before dropping the clock. */
  *ra8_drw_reg32(k_ra8_drw_off_irqctl) = k_ra8_drw_irqctl_all_clr;

  /* HUM Ch 62.2.4 "CACHECTL: Cache Control Register", p 3694 */
  *ra8_drw_reg32(k_ra8_drw_off_cachectl) = 0UL;

  /* HUM Ch 62.2.35 "DBWER: DRW Bufferable Write Enable", p 3707 */
  *ra8_drw_reg32(k_ra8_drw_off_dbwer) = 0UL;

  s_drw_fn       = nullptr;
  s_drw_ctx      = nullptr;
  s_drw_origin   = 0U;
  s_drw_control2 = 0U;
  s_drw_color1   = 0U;
  return ra8_mstp_disable(k_ra8_mstp_drw);
}

/* =============================================================================
 * Status / IRQ
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_drw_get_status(uint32_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 62.2.5 "STATUS: Status Control Register", p 3695 */
  /* STATUS aliases CONTROL: a read returns engine status bits. */
  *out_mask = *ra8_drw_reg32(k_ra8_drw_off_status);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_drw_get_hwrevision(uint32_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 62.2.6 "HWREVISION: Hardware Revision Register", p 3696 */
  *out = *ra8_drw_reg32(k_ra8_drw_off_hwrevision);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_drw_clear_status(uint32_t mask)
{
  /* HUM Ch 62.2.3 "IRQCTL: Interrupt Control Register", p 3693 */
  /* Write-1-to-clear semantics for ENUMIRQCLR/DLISTIRQCLR/BUSIRQCLR. */
  *ra8_drw_reg32(k_ra8_drw_off_irqctl) = mask;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_drw_set_irq_enables(uint32_t enable_mask)
{
  /* HUM Ch 62.2.3 "IRQCTL: Interrupt Control Register", p 3693 */
  /* Combine caller's enable bits with an unconditional W1C of every
 * pending flag so we never re-enable while a stale flag is latched. */
  const uint32_t value                 = (enable_mask | k_ra8_drw_irqctl_all_clr);
  *ra8_drw_reg32(k_ra8_drw_off_irqctl) = value;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_drw_attach_handler(ra8_drw_event_fn_t fn, void* ctx)
{
  s_drw_fn  = fn;
  s_drw_ctx = ctx;
  return k_ra8_ok;
}

RA8_ISR_SAFE
void ra8_drw_dispatch(void)
{
  /* HUM Ch 62.2.5 "STATUS: Status Control Register", p 3695 */
  const uint32_t status = *ra8_drw_reg32(k_ra8_drw_off_status);

  /* HUM Ch 62.2.3 "IRQCTL: Interrupt Control Register", p 3693 */
  /* Acknowledge all pending IRQ flags (write-1-to-clear). */
  *ra8_drw_reg32(k_ra8_drw_off_irqctl) = k_ra8_drw_irqctl_all_clr;

  const ra8_drw_event_fn_t fn  = s_drw_fn;
  void* const              ctx = s_drw_ctx;
  if (fn != nullptr) {
    fn(ctx, status);
  }
}

[[nodiscard]] ra8_err_t ra8_drw_wait_idle(uint32_t poll_budget)
{
  if (poll_budget == 0UL) {
    return k_ra8_err_invalid_arg;
  }
  /* NASA Rule 2: bounded loop. */
  for (uint32_t i = 0UL; i < poll_budget; ++i) { /* GCOVR_EXCL_BR_LINE */
    /* HUM Ch 62.2.5 "STATUS: Status Control Register", p 3695 */
    const uint32_t s = *ra8_drw_reg32(k_ra8_drw_off_status);
    if ((s & k_ra8_drw_status_busy_mask) == 0UL) { /* GCOVR_EXCL_BR_LINE */
      return k_ra8_ok;
    }
  }
  ra8_log_error(s_tag, "drw_wait_idle: timeout");
  return k_ra8_err_hw_timeout;
}

/* =============================================================================
 * Power transition
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_drw_enter_stop(void)
{
  /* HUM Ch 62.2.3 "IRQCTL: Interrupt Control Register", p 3693 */
  /* Quiesce IRQs before removing the clock, otherwise a pending bit
 * could latch and surprise us when we power back up. */
  *ra8_drw_reg32(k_ra8_drw_off_irqctl) = k_ra8_drw_irqctl_all_clr;
  return ra8_mstp_disable(k_ra8_mstp_drw);
}

[[nodiscard]] ra8_err_t ra8_drw_exit_stop(void)
{
  return ra8_mstp_enable(k_ra8_mstp_drw);
}

[[nodiscard]] ra8_err_t ra8_drw_reset(void)
{
  /* HUM Ch 62.2.3 "IRQCTL: Interrupt Control Register", p 3693 */
  *ra8_drw_reg32(k_ra8_drw_off_irqctl) = k_ra8_drw_irqctl_all_clr;

  /* HUM Ch 62.2.4 "CACHECTL: Cache Control Register", p 3694 */
  /* Pulse both cache flushes while preserving enable bits. */
  const uint32_t cur_cc                  = *ra8_drw_reg32(k_ra8_drw_off_cachectl);
  *ra8_drw_reg32(k_ra8_drw_off_cachectl) = cur_cc | k_ra8_drw_cachectl_all_flush;

  /* HUM Ch 62.2.34 "PERFCOUNTk: Performance Counter k", p 3706 */
  *ra8_drw_reg32(k_ra8_drw_off_perfcount1) = 0UL;
  *ra8_drw_reg32(k_ra8_drw_off_perfcount2) = 0UL;
  return k_ra8_ok;
}

/* =============================================================================
 * Cache control
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_drw_cache_flush(bool flush_fb, bool flush_texture)
{
  if (!flush_fb && !flush_texture) {
    ra8_log_warn(s_tag, "cache_flush: nothing to flush");
    return k_ra8_ok;
  }
  /* HUM Ch 62.2.4 "CACHECTL: Cache Control Register", p 3694 */
  /* Preserve enable bits, OR in the requested flush pulses. */
  const uint32_t cur = *ra8_drw_reg32(k_ra8_drw_off_cachectl);
  uint32_t       nxt = cur;
  if (flush_fb) {
    nxt |= k_ra8_drw_cachectl_cflushfx;
  }
  if (flush_texture) {
    nxt |= k_ra8_drw_cachectl_cflushtx;
  }
  *ra8_drw_reg32(k_ra8_drw_off_cachectl) = nxt;
  return k_ra8_ok;
}

/* =============================================================================
 * Surface / blend / colour
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_drw_set_gradient(const ra8_drw_gradient_t* grad)
{
  RA8_CHECK_NULL_PTR(grad, s_tag, "grad must not be nullptr");
  /* COLOR1 goes through the shadowed writer (write-only register). */
  ra8_drw_internal_color1_write(grad->color1_argb8888);
  /* HUM Ch 62.2.8 "COLOR2: Secondary Color Register", p 3697 */
  *ra8_drw_reg32(k_ra8_drw_off_color2) = grad->color2_argb8888;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_drw_set_pattern(uint8_t pattern_byte)
{
  /* HUM Ch 62.2.9 "PATTERN: Pattern Register", p 3698 */
  /* Only PATTERN[7:0] is writable; bits 31:8 must be zero. */
  *ra8_drw_reg32(k_ra8_drw_off_pattern) =
    (uint32_t)pattern_byte & (uint32_t)k_ra8_drw_internal_byte_mask;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_drw_set_pattern_enable(bool enable, bool source_from_l5)
{
  uint32_t set_bits = 0UL;
  if (enable) {
    set_bits |= k_ra8_drw_control2_patternenable;
  }
  if (source_from_l5) {
    set_bits |= k_ra8_drw_control2_patternsourcel5;
  }
  const uint32_t clr_mask = (k_ra8_drw_control2_patternenable | k_ra8_drw_control2_patternsourcel5);
  (void)internal_control2_rmw(clr_mask, set_bits);
  return k_ra8_ok;
}

/**
 * @brief Pack ``ra8_drw_blend_t`` into a CONTROL2 set-bit slab.
 *
 * @details
 * HUM Ch 62.2.2 "CONTROL2: Surface Control Register" p 3691. Each
 * field of ``blend`` maps to a single CONTROL2 bit; this helper just
 * collects them so ``ra8_drw_set_blend`` stays under the
 * function-size threshold.
 *
 * @param[in] blend Caller-supplied blend descriptor.
 * @return CONTROL2 set-bit slab.
 *
 * @pre ``blend`` is non-null.
 * @post No side effects.
 *
 * @note Internal helper, not thread-safe.
 *
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_pack_blend_bits(const ra8_drw_blend_t* blend)
{
  uint32_t set_bits = 0UL;
  if (blend->use_alpha_channel) {
    set_bits |= k_ra8_drw_control2_useacb;
  }
  if (blend->src_factor) {
    set_bits |= k_ra8_drw_control2_bsf;
  }
  if (blend->dst_factor) {
    set_bits |= k_ra8_drw_control2_bdf;
  }
  if (blend->src_invert) {
    set_bits |= k_ra8_drw_control2_bsi;
  }
  if (blend->dst_invert) {
    set_bits |= k_ra8_drw_control2_bdi;
  }
  if (blend->src_factor_alpha) {
    set_bits |= k_ra8_drw_control2_bsfa;
  }
  if (blend->dst_factor_alpha) {
    set_bits |= k_ra8_drw_control2_bdfa;
  }
  if (blend->src_invert_alpha) {
    set_bits |= k_ra8_drw_control2_bsia;
  }
  if (blend->dst_invert_alpha) {
    set_bits |= k_ra8_drw_control2_bdia;
  }
  if (blend->use_color2_dst) {
    set_bits |= k_ra8_drw_control2_bc2;
  }
  return set_bits;
}

[[nodiscard]] ra8_err_t ra8_drw_set_blend(const ra8_drw_blend_t* blend)
{
  RA8_CHECK_NULL_PTR(blend, s_tag, "blend must not be nullptr");

  /* HUM Ch 62.2.2 "CONTROL2: Surface Control Register", p 3691 */
  const uint32_t set_bits = internal_pack_blend_bits(blend);
  const uint32_t clr_mask =
    (k_ra8_drw_control2_useacb | k_ra8_drw_control2_bsf | k_ra8_drw_control2_bdf |
     k_ra8_drw_control2_bsi | k_ra8_drw_control2_bdi | k_ra8_drw_control2_bsfa |
     k_ra8_drw_control2_bdfa | k_ra8_drw_control2_bsia | k_ra8_drw_control2_bdia |
     k_ra8_drw_control2_bc2);
  (void)internal_control2_rmw(clr_mask, set_bits);

  /* Update only the alpha byte; preserve RGB to avoid surprise drift.
   * COLOR1 is write-only on silicon, so the current value comes from the
   * software shadow, and the write goes through the shadowed writer. */
  const uint32_t new_color1 = (s_drw_color1 & k_ra8_drw_internal_color_alpha_mask) |
                              ((uint32_t)blend->global_alpha << k_ra8_drw_color_a_pos);
  ra8_drw_internal_color1_write(new_color1);

  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_drw_set_color_key(uint32_t key_rgb, bool enable)
{
  /* HUM Ch 62.2.28 "COLKEY: Color Key Register", p 3704 */
  /* Bits 31:24 must be zero per the HUM table. */
  *ra8_drw_reg32(k_ra8_drw_off_colkey) = key_rgb & k_ra8_drw_internal_color_alpha_mask;

  uint32_t set_bits = 0UL;
  if (enable) {
    set_bits = k_ra8_drw_control2_colkeyenable;
  }
  (void)internal_control2_rmw(k_ra8_drw_control2_colkeyenable, set_bits);
  return k_ra8_ok;
}

/* =============================================================================
 * Texture
 * =============================================================================
 */

/**
 * @brief Pack ``ra8_drw_texture_t`` into a CONTROL2 set-bit slab.
 *
 * @details
 * HUM Ch 62.2.2 "CONTROL2: Surface Control Register" p 3691, including
 * format, clamp/filter, CLUT, color-key and RLE bits.
 *
 * @param[in] tex Caller-supplied texture descriptor.
 * @return CONTROL2 set-bit slab.
 *
 * @pre ``tex`` is non-null.
 * @post No side effects.
 *
 * @note Internal helper, not thread-safe.
 *
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_pack_texture_bits(const ra8_drw_texture_t* tex)
{
  uint32_t set_bits = k_ra8_drw_control2_textureenable;
  set_bits |= internal_pack_readformat(tex->format);
  if (tex->clamp_x) {
    set_bits |= k_ra8_drw_control2_textureclampx;
  }
  if (tex->clamp_y) {
    set_bits |= k_ra8_drw_control2_textureclampy;
  }
  if (tex->filter_x) {
    set_bits |= k_ra8_drw_control2_texturefilterx;
  }
  if (tex->filter_y) {
    set_bits |= k_ra8_drw_control2_texturefiltery;
  }
  if (tex->enable_clut) {
    set_bits |= k_ra8_drw_control2_clutenable;
  }
  if (tex->clut_565) {
    set_bits |= k_ra8_drw_control2_clutformat_565;
  }
  if (tex->enable_color_key) {
    set_bits |= k_ra8_drw_control2_colkeyenable;
  }
  if (tex->enable_rle) {
    set_bits |= k_ra8_drw_control2_rleenable;
  }
  set_bits |= ((uint32_t)tex->rle_pixel_width & 0x3U) << k_ra8_drw_control2_rlepixel_pos;
  return set_bits;
}

[[nodiscard]] ra8_err_t ra8_drw_set_texture(const ra8_drw_texture_t* tex)
{
  RA8_CHECK_NULL_PTR(tex, s_tag, "tex must not be nullptr");
  if ((uint16_t)tex->pitch_px > k_ra8_drw_max_texpitch_tx) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 62.2.17 "TEXORIGIN: Texture Base Address Register", p 3700 */
  *ra8_drw_reg32(k_ra8_drw_off_texorigin) = (uint32_t)tex->base_addr;

  /* HUM Ch 62.2.15 "TEXPITCH: Texels Per Texture Line Register", p 3700 */
  *ra8_drw_reg32(k_ra8_drw_off_texpitch) = (uint32_t)tex->pitch_px;

  /* HUM Ch 62.2.16 "TEXMASK: Texture U/V Mask Register", p 3700.
 * U mask in [15:0], V mask in [31:16]. */
  *ra8_drw_reg32(k_ra8_drw_off_texmask) = ((uint32_t)tex->v_mask << 16U) | (uint32_t)tex->u_mask;

  /* HUM Ch 62.2.27 "TEXCLOFFSET: CLUT Offset Register", p 3704 */
  *ra8_drw_reg32(k_ra8_drw_off_texcloffset) =
    (uint32_t)tex->clut_offset & (uint32_t)k_ra8_drw_internal_byte_mask;

  const uint32_t set_bits = internal_pack_texture_bits(tex);
  const uint32_t clr_mask =
    (k_ra8_drw_control2_textureenable | k_ra8_drw_control2_textureclampx |
     k_ra8_drw_control2_textureclampy | k_ra8_drw_control2_texturefilterx |
     k_ra8_drw_control2_texturefiltery | k_ra8_drw_control2_clutenable |
     k_ra8_drw_control2_clutformat_565 | k_ra8_drw_control2_colkeyenable |
     k_ra8_drw_control2_rleenable | k_ra8_drw_control2_rlepixel_mask |
     k_ra8_drw_control2_readformatl_mask | k_ra8_drw_control2_readformath_mask);
  (void)internal_control2_rmw(clr_mask, set_bits);

  if (tex->enable_color_key) {
    /* HUM Ch 62.2.28 "COLKEY: Color Key Register", p 3704 */
    *ra8_drw_reg32(k_ra8_drw_off_colkey) = tex->color_key_rgb & k_ra8_drw_internal_color_alpha_mask;
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_drw_clear_texture(void)
{
  const uint32_t clr_mask = (k_ra8_drw_control2_textureenable | k_ra8_drw_control2_rleenable |
                             k_ra8_drw_control2_clutenable | k_ra8_drw_control2_colkeyenable |
                             k_ra8_drw_control2_rlepixel_mask);
  (void)internal_control2_rmw(clr_mask, 0UL);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t
ra8_drw_load_clut(uint8_t start_index, const uint32_t* entries, uint32_t count)
{
  RA8_CHECK_NULL_PTR(entries, s_tag, "entries must not be nullptr");
  if (count == 0UL) {
    return k_ra8_err_invalid_arg;
  }
  if (((uint32_t)start_index + count) > (uint32_t)k_ra8_drw_internal_clut_max) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 62.2.25 "TEXCLADDR: CLUT Start Address Register", p 3703 */
  *ra8_drw_reg32(k_ra8_drw_off_texcladdr) = (uint32_t)start_index;

  /* HUM Ch 62.2.26 "TEXCLDATA: CLUT Data Register", p 3703 */
  /* The DRW auto-increments CLADDR after each TEXCLDATA write per
 * HUM Ch 62.5.4 "CLUT setup", p 3717. NASA Rule 2: bounded loop. */
  for (uint32_t i = 0UL; i < count; ++i) {
    *ra8_drw_reg32(k_ra8_drw_off_texcldata) = entries[i];
  }
  return k_ra8_ok;
}
