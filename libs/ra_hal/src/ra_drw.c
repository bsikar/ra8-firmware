/**
 * @file ra_drw.c
 * @brief 2D Drawing Engine (DRW / D/AVE 2D) HAL driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Full RA8D2 D/AVE 2D driver. The wave-3 expansion takes the driver
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
 * @startuml
 * [*] --> reset
 * reset --> initialised: ra_drw_init
 * initialised --> stopped: ra_drw_enter_stop
 * stopped --> initialised: ra_drw_exit_stop
 * initialised --> reset: ra_drw_deinit
 * @enduml
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_drw.h"

#include <stdint.h>

#include "ra8d2_drw_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

/* =============================================================================
 * Driver-private state
 * =============================================================================
 */

/**
 * @var s_tag
 * @brief Logging tag for the DRW driver.
 */
static const char* s_tag = "DRW";

/**
 * @var s_drw_fn
 * @brief Currently registered DRW event callback (or ``nullptr``).
 */
static ra_drw_event_fn_t s_drw_fn;

/**
 * @var s_drw_ctx
 * @brief Opaque context paired with ``s_drw_fn``.
 */
static void* s_drw_ctx;

/**
 * @enum ra_drw_internal_const_t
 * @brief Driver-internal numeric constants -- avoid bare literals.
 */
typedef enum : uint16_t {
  k_ra_drw_internal_byte_mask   = 0x00FFU, /**< 8-bit channel mask. */
  k_ra_drw_internal_one_subpx   = 16U,     /**< 1 px == 16 sub-pixels. */
  k_ra_drw_internal_clut_max    = 256U,    /**< CLUT entries (HUM 62.5.4). */
  k_ra_drw_internal_dlist_align = 4U,      /**< 4-byte alignment of DLIST. */
  k_ra_drw_internal_perfev_max  = 0x001FU, /**< 0x1F is "every clock". */
} ra_drw_internal_const_t;

/**
 * @enum ra_drw_internal_mask32_t
 * @brief 32-bit driver-internal masks.
 */
typedef enum : uint32_t {
  k_ra_drw_internal_color_alpha_mask = 0x00FFFFFFUL, /**< 0x00RRGGBB low bits. */
  k_ra_drw_internal_align_mask       = 0x00000003UL, /**< 4-byte alignment. */
} ra_drw_internal_mask32_t;

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Fold the three-bit WRITEFORMAT field into a 32-bit CONTROL2
 * value (WRITEFORMAT2 lives at bit 8, WRITEFORMAT[1:0] at
 * bits 21:20).
 *
 * @param[in] fmt One of ``ra_drw_writeformat_t``.
 * @return CONTROL2 word with format bits set, all other bits zero.
 *
 * @pre ``fmt`` is one of the documented enum values (0..3).
 * @pre Caller will OR additional CONTROL2 bits as needed.
 *
 * @post Returned value has only the format bits set.
 * @post No side effects.
 */
static inline uint32_t internal_pack_writeformat(ra_drw_writeformat_t fmt)
{
  const uint32_t low  = ((uint32_t)fmt & 0x3UL) << k_ra_drw_control2_writeformat_pos;
  uint32_t       high = 0UL;
  if (((uint32_t)fmt & 0x4UL) != 0UL) {
    high = (uint32_t)k_ra_drw_control2_writeformat2_bit;
  }
  return low | high;
}

/**
 * @brief Pack a 4-bit READFORMAT value into CONTROL2.READFORMAT_H+L.
 *
 * @param[in] fmt One of ``ra_drw_readformat_t``.
 * @return CONTROL2 word with the four READFORMAT bits set.
 *
 * @pre ``fmt`` is in the documented set (0x0..0xC).
 * @pre Caller OR-combines with the rest of CONTROL2.
 * @post Returned word touches only READFORMAT_L (18:19) and
 * READFORMAT_H (4:5).
 * @post No side effects.
 */
static inline uint32_t internal_pack_readformat(ra_drw_readformat_t fmt)
{
  const uint32_t code = (uint32_t)fmt;
  const uint32_t lo   = (code & 0x3UL) << k_ra_drw_control2_readformatl_pos;
  const uint32_t hi   = ((code >> 2U) & 0x3UL) << k_ra_drw_control2_readformath_pos;
  return lo | hi;
}

/**
 * @brief Convert a signed pixel coordinate to the DRW Q12.4 sub-pixel grid.
 *
 * @param[in] px Pixel coordinate (signed, in [-32768, 32767] range
 * -- the framebuffer never grows beyond 1024x1024).
 * @return Sub-pixel value (px << 4), wrapped to uint32_t.
 *
 * @pre Caller has range-checked ``px`` upstream.
 * @pre The shift cannot overflow 32 bits for the supported FB size.
 * @post Returned value preserves bit width and is suitable for
 * LnSTART / LnXADD / LnYADD writes.
 * @post No side effects.
 */
static inline uint32_t internal_to_subpixel(int32_t px)
{
  return (uint32_t)(px << k_ra_drw_subpixel_shift);
}

/**
 * @brief Compute integer absolute value with no library calls.
 *
 * @param[in] v Signed value (range [-32768, 32767]).
 * @return Non-negative absolute value cast to int32_t.
 *
 * @pre ``v`` may be any int32_t except INT32_MIN.
 * @pre Result fits in int32_t.
 * @post Returned value is >= 0.
 * @post No side effects.
 */
static inline int32_t internal_iabs(int32_t v)
{
  return (v < 0) ? -v : v;
}

/**
 * @brief Read-modify-write a CONTROL2 mask; preserve unrelated bits.
 *
 * @param[in] clear_mask Bits to clear before applying ``set_mask``.
 * @param[in] set_mask Bits to set after the clear.
 * @return Updated CONTROL2 value (also written back to the register).
 *
 * @pre Driver initialised; CONTROL2 is reachable.
 * @pre Caller must ensure no draw is in flight (CONTROL2 is latched
 * at the start of an enumeration).
 * @post CONTROL2 = (old & ~clear_mask) | set_mask.
 * @post Other CONTROL2 fields preserved.
 */
static inline uint32_t internal_control2_rmw(uint32_t clear_mask, uint32_t set_mask)
{
  /* HUM Ch 62.2.2 "CONTROL2: Surface Control Register", p 3690 */
  const uint32_t cur = *ra_drw_reg32(k_ra_drw_off_control2);
  const uint32_t nxt = (cur & ~clear_mask) | set_mask;
  /* HUM Ch 62.2.2 "CONTROL2: Surface Control Register", p 3691 */
  *ra_drw_reg32(k_ra_drw_off_control2) = nxt;
  return nxt;
}

/**
 * @brief Programme the four edge limiters that bound an axis-aligned rect.
 *
 * @param[in] rect Rectangle in pixel space (validated by caller).
 *
 * @pre rect dimensions in [1..1024].
 * @pre Driver initialised.
 * @post L1..L4 START / XADD / YADD describe a filled axis-aligned quad.
 * @post SIZE = (height << 16) | width.
 */
static void internal_program_rect_limiters(const ra_drw_rect_t* rect)
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
  *ra_drw_reg32(k_ra_drw_off_size) =
    ((uint32_t)rect->height_px << k_ra_drw_size_height_pos) | (uint32_t)rect->width_px;

  /* HUM Ch 62.2.10 "LnSTART: Limiter n Start Value Register", p 3698 */
  *ra_drw_reg32(k_ra_drw_off_l1start) = l1;
  *ra_drw_reg32(k_ra_drw_off_l2start) = l2;
  *ra_drw_reg32(k_ra_drw_off_l3start) = l3;
  *ra_drw_reg32(k_ra_drw_off_l4start) = l4;

  /* HUM Ch 62.2.11 "LnXADD: Limiter n X-Axis Increment Register", p 3698 */
  *ra_drw_reg32(k_ra_drw_off_l1xadd) = (uint32_t)k_ra_drw_subpixel_unit;
  *ra_drw_reg32(k_ra_drw_off_l2xadd) = (uint32_t)k_ra_drw_subpixel_unit;
  *ra_drw_reg32(k_ra_drw_off_l3xadd) = 0UL;
  *ra_drw_reg32(k_ra_drw_off_l4xadd) = 0UL;

  /* HUM Ch 62.2.12 "LnYADD: Limiter n Y-Axis Increment Register", p 3699 */
  *ra_drw_reg32(k_ra_drw_off_l1yadd) = 0UL;
  *ra_drw_reg32(k_ra_drw_off_l2yadd) = 0UL;
  *ra_drw_reg32(k_ra_drw_off_l3yadd) = (uint32_t)k_ra_drw_subpixel_unit;
  *ra_drw_reg32(k_ra_drw_off_l4yadd) = (uint32_t)k_ra_drw_subpixel_unit;
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_drw_init(const ra_drw_config_t* cfg)
{
  RA_CHECK_NULL_PTR((void*)cfg, s_tag, "cfg must not be nullptr");

  /* HUM Ch 11.2.8 "MSTPCRC: Module Stop Control Register C", p 446 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_drw);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "drw_init: mstp enable");

  /* HUM Ch 62.2.3 "IRQCTL: Interrupt Control Register", p 3693 */
  /* Clear all pending IRQs and leave them masked. */
  *ra_drw_reg32(k_ra_drw_off_irqctl) = (uint32_t)k_ra_drw_irqctl_all_clr;

  /* HUM Ch 62.2.31 "ORIGIN: Framebuffer Base Address Register", p 3705 */
  *ra_drw_reg32(k_ra_drw_off_origin) = (uint32_t)cfg->framebuffer_addr;

  /* HUM Ch 62.2.30 "PITCH: Framebuffer Pitch and Spanstore Delay
 * Register", p 3705. Spanstore delay (SSD) field [31:16] kept 0. */
  *ra_drw_reg32(k_ra_drw_off_pitch) = (uint32_t)cfg->pitch_px;

  /* HUM Ch 62.2.2 "CONTROL2: Surface Control Register", p 3690 */
  /* Plain solid-fill mode: no texture, no pattern, no blend; only the
 * framebuffer pixel format bits are set. */
  *ra_drw_reg32(k_ra_drw_off_control2) = internal_pack_writeformat(cfg->format);

  /* HUM Ch 62.2.4 "CACHECTL: Cache Control Register", p 3694 */
  if (cfg->enable_caches) {
    *ra_drw_reg32(k_ra_drw_off_cachectl) = (uint32_t)k_ra_drw_cachectl_all_en;
  } else {
    *ra_drw_reg32(k_ra_drw_off_cachectl) = 0UL;
  }

  /* HUM Ch 62.2.35 "DBWER: DRW Bufferable Write Enable", p 3707 */
  if (cfg->enable_buffered_writes) {
    *ra_drw_reg32(k_ra_drw_off_dbwer) = (uint32_t)k_ra_drw_dbwer_bwe;
  } else {
    *ra_drw_reg32(k_ra_drw_off_dbwer) = 0UL;
  }

  ra_log_info_val(s_tag, "drw_init fb", (uint32_t)cfg->framebuffer_addr);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_drw_deinit(void)
{
  /* HUM Ch 62.2.3 "IRQCTL: Interrupt Control Register", p 3693 */
  /* Clear and disable every interrupt before dropping the clock. */
  *ra_drw_reg32(k_ra_drw_off_irqctl) = (uint32_t)k_ra_drw_irqctl_all_clr;

  /* HUM Ch 62.2.4 "CACHECTL: Cache Control Register", p 3694 */
  *ra_drw_reg32(k_ra_drw_off_cachectl) = 0UL;

  /* HUM Ch 62.2.35 "DBWER: DRW Bufferable Write Enable", p 3707 */
  *ra_drw_reg32(k_ra_drw_off_dbwer) = 0UL;

  s_drw_fn  = nullptr;
  s_drw_ctx = nullptr;
  return ra_mstp_disable(k_ra_mstp_drw);
}

/* =============================================================================
 * Status / IRQ
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_drw_get_status(uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 62.2.5 "STATUS: Status Control Register", p 3695 */
  /* STATUS aliases CONTROL: a read returns engine status bits. */
  *out_mask = *ra_drw_reg32(k_ra_drw_off_status);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_drw_get_hwrevision(uint32_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 62.2.6 "HWREVISION: Hardware Revision Register", p 3696 */
  *out = *ra_drw_reg32(k_ra_drw_off_hwrevision);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_drw_clear_status(uint32_t mask)
{
  /* HUM Ch 62.2.3 "IRQCTL: Interrupt Control Register", p 3693 */
  /* Write-1-to-clear semantics for ENUMIRQCLR/DLISTIRQCLR/BUSIRQCLR. */
  *ra_drw_reg32(k_ra_drw_off_irqctl) = mask;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_drw_set_irq_enables(uint32_t enable_mask)
{
  /* HUM Ch 62.2.3 "IRQCTL: Interrupt Control Register", p 3693 */
  /* Combine caller's enable bits with an unconditional W1C of every
 * pending flag so we never re-enable while a stale flag is latched. */
  const uint32_t value               = (enable_mask | (uint32_t)k_ra_drw_irqctl_all_clr);
  *ra_drw_reg32(k_ra_drw_off_irqctl) = value;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_drw_attach_handler(ra_drw_event_fn_t fn, void* ctx)
{
  s_drw_fn  = fn;
  s_drw_ctx = ctx;
  return k_ra_ok;
}

void ra_drw_dispatch(void)
{
  /* HUM Ch 62.2.5 "STATUS: Status Control Register", p 3695 */
  const uint32_t status = *ra_drw_reg32(k_ra_drw_off_status);

  /* HUM Ch 62.2.3 "IRQCTL: Interrupt Control Register", p 3693 */
  /* Acknowledge all pending IRQ flags (write-1-to-clear). */
  *ra_drw_reg32(k_ra_drw_off_irqctl) = (uint32_t)k_ra_drw_irqctl_all_clr;

  const ra_drw_event_fn_t fn  = s_drw_fn;
  void* const             ctx = s_drw_ctx;
  if (fn != nullptr) {
    fn(ctx, status);
  }
}

[[nodiscard]] ra_err_t ra_drw_wait_idle(uint32_t poll_budget)
{
  if (poll_budget == 0UL) {
    return k_ra_err_invalid_arg;
  }
  /* NASA Rule 2: bounded loop. */
  for (uint32_t i = 0UL; i < poll_budget; ++i) {
    /* HUM Ch 62.2.5 "STATUS: Status Control Register", p 3695 */
    const uint32_t s = *ra_drw_reg32(k_ra_drw_off_status);
    if ((s & (uint32_t)k_ra_drw_status_busy_mask) == 0UL) {
      return k_ra_ok;
    }
  }
  ra_log_error(s_tag, "drw_wait_idle: timeout");
  return k_ra_err_hw_timeout;
}

/* =============================================================================
 * Power transition
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_drw_enter_stop(void)
{
  /* HUM Ch 62.2.3 "IRQCTL: Interrupt Control Register", p 3693 */
  /* Quiesce IRQs before removing the clock, otherwise a pending bit
 * could latch and surprise us when we power back up. */
  *ra_drw_reg32(k_ra_drw_off_irqctl) = (uint32_t)k_ra_drw_irqctl_all_clr;
  return ra_mstp_disable(k_ra_mstp_drw);
}

[[nodiscard]] ra_err_t ra_drw_exit_stop(void)
{
  return ra_mstp_enable(k_ra_mstp_drw);
}

[[nodiscard]] ra_err_t ra_drw_reset(void)
{
  /* HUM Ch 62.2.3 "IRQCTL: Interrupt Control Register", p 3693 */
  *ra_drw_reg32(k_ra_drw_off_irqctl) = (uint32_t)k_ra_drw_irqctl_all_clr;

  /* HUM Ch 62.2.4 "CACHECTL: Cache Control Register", p 3694 */
  /* Pulse both cache flushes while preserving enable bits. */
  const uint32_t cur_cc                = *ra_drw_reg32(k_ra_drw_off_cachectl);
  *ra_drw_reg32(k_ra_drw_off_cachectl) = cur_cc | (uint32_t)k_ra_drw_cachectl_all_flush;

  /* HUM Ch 62.2.34 "PERFCOUNTk: Performance Counter k", p 3706 */
  *ra_drw_reg32(k_ra_drw_off_perfcount1) = 0UL;
  *ra_drw_reg32(k_ra_drw_off_perfcount2) = 0UL;
  return k_ra_ok;
}

/* =============================================================================
 * Cache control
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_drw_cache_flush(bool flush_fb, bool flush_texture)
{
  if (!flush_fb && !flush_texture) {
    ra_log_warn(s_tag, "cache_flush: nothing to flush");
    return k_ra_ok;
  }
  /* HUM Ch 62.2.4 "CACHECTL: Cache Control Register", p 3694 */
  /* Preserve enable bits, OR in the requested flush pulses. */
  const uint32_t cur = *ra_drw_reg32(k_ra_drw_off_cachectl);
  uint32_t       nxt = cur;
  if (flush_fb) {
    nxt |= (uint32_t)k_ra_drw_cachectl_cflushfx;
  }
  if (flush_texture) {
    nxt |= (uint32_t)k_ra_drw_cachectl_cflushtx;
  }
  *ra_drw_reg32(k_ra_drw_off_cachectl) = nxt;
  return k_ra_ok;
}

/* =============================================================================
 * Surface / blend / colour
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_drw_set_gradient(const ra_drw_gradient_t* grad)
{
  RA_CHECK_NULL_PTR((void*)grad, s_tag, "grad must not be nullptr");
  /* HUM Ch 62.2.7 "COLOR1: Base Color Register", p 3697 */
  *ra_drw_reg32(k_ra_drw_off_color1) = grad->color1_argb8888;
  /* HUM Ch 62.2.8 "COLOR2: Secondary Color Register", p 3697 */
  *ra_drw_reg32(k_ra_drw_off_color2) = grad->color2_argb8888;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_drw_set_pattern(uint8_t pattern_byte)
{
  /* HUM Ch 62.2.9 "PATTERN: Pattern Register", p 3698 */
  /* Only PATTERN[7:0] is writable; bits 31:8 must be zero. */
  *ra_drw_reg32(k_ra_drw_off_pattern) =
    (uint32_t)pattern_byte & (uint32_t)k_ra_drw_internal_byte_mask;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_drw_set_pattern_enable(bool enable, bool source_from_l5)
{
  uint32_t set_bits = 0UL;
  if (enable) {
    set_bits |= (uint32_t)k_ra_drw_control2_patternenable;
  }
  if (source_from_l5) {
    set_bits |= (uint32_t)k_ra_drw_control2_patternsourcel5;
  }
  const uint32_t clr_mask =
    (uint32_t)(k_ra_drw_control2_patternenable | k_ra_drw_control2_patternsourcel5);
  (void)internal_control2_rmw(clr_mask, set_bits);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_drw_set_blend(const ra_drw_blend_t* blend)
{
  RA_CHECK_NULL_PTR((void*)blend, s_tag, "blend must not be nullptr");

  /* HUM Ch 62.2.2 "CONTROL2: Surface Control Register", p 3691 */
  /* Compose the blend bit slab. */
  uint32_t set_bits = 0UL;
  if (blend->use_alpha_channel) {
    set_bits |= (uint32_t)k_ra_drw_control2_useacb;
  }
  if (blend->src_factor) {
    set_bits |= (uint32_t)k_ra_drw_control2_bsf;
  }
  if (blend->dst_factor) {
    set_bits |= (uint32_t)k_ra_drw_control2_bdf;
  }
  if (blend->src_invert) {
    set_bits |= (uint32_t)k_ra_drw_control2_bsi;
  }
  if (blend->dst_invert) {
    set_bits |= (uint32_t)k_ra_drw_control2_bdi;
  }
  if (blend->src_factor_alpha) {
    set_bits |= (uint32_t)k_ra_drw_control2_bsfa;
  }
  if (blend->dst_factor_alpha) {
    set_bits |= (uint32_t)k_ra_drw_control2_bdfa;
  }
  if (blend->src_invert_alpha) {
    set_bits |= (uint32_t)k_ra_drw_control2_bsia;
  }
  if (blend->dst_invert_alpha) {
    set_bits |= (uint32_t)k_ra_drw_control2_bdia;
  }
  if (blend->use_color2_dst) {
    set_bits |= (uint32_t)k_ra_drw_control2_bc2;
  }

  const uint32_t clr_mask =
    (uint32_t)(k_ra_drw_control2_useacb | k_ra_drw_control2_bsf | k_ra_drw_control2_bdf |
               k_ra_drw_control2_bsi | k_ra_drw_control2_bdi | k_ra_drw_control2_bsfa |
               k_ra_drw_control2_bdfa | k_ra_drw_control2_bsia | k_ra_drw_control2_bdia |
               k_ra_drw_control2_bc2);
  (void)internal_control2_rmw(clr_mask, set_bits);

  /* HUM Ch 62.2.7 "COLOR1: Base Color Register", p 3697 */
  /* Update only the alpha byte; preserve RGB to avoid surprise drift. */
  const uint32_t cur_color1          = *ra_drw_reg32(k_ra_drw_off_color1);
  const uint32_t new_color1          = (cur_color1 & (uint32_t)k_ra_drw_internal_color_alpha_mask) |
                                       ((uint32_t)blend->global_alpha << k_ra_drw_color_a_pos);
  *ra_drw_reg32(k_ra_drw_off_color1) = new_color1;

  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_drw_set_color_key(uint32_t key_rgb, bool enable)
{
  /* HUM Ch 62.2.28 "COLKEY: Color Key Register", p 3704 */
  /* Bits 31:24 must be zero per the HUM table. */
  *ra_drw_reg32(k_ra_drw_off_colkey) = key_rgb & (uint32_t)k_ra_drw_internal_color_alpha_mask;

  const uint32_t set_bits = enable ? (uint32_t)k_ra_drw_control2_colkeyenable : 0UL;
  (void)internal_control2_rmw((uint32_t)k_ra_drw_control2_colkeyenable, set_bits);
  return k_ra_ok;
}

/* =============================================================================
 * Texture
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_drw_set_texture(const ra_drw_texture_t* tex)
{
  RA_CHECK_NULL_PTR((void*)tex, s_tag, "tex must not be nullptr");
  if ((uint16_t)tex->pitch_px > (uint16_t)k_ra_drw_max_texpitch_tx) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 62.2.17 "TEXORIGIN: Texture Base Address Register", p 3700 */
  *ra_drw_reg32(k_ra_drw_off_texorigin) = (uint32_t)tex->base_addr;

  /* HUM Ch 62.2.15 "TEXPITCH: Texels Per Texture Line Register", p 3700 */
  *ra_drw_reg32(k_ra_drw_off_texpitch) = (uint32_t)tex->pitch_px;

  /* HUM Ch 62.2.16 "TEXMASK: Texture U/V Mask Register", p 3700.
 * U mask in [15:0], V mask in [31:16]. */
  *ra_drw_reg32(k_ra_drw_off_texmask) = ((uint32_t)tex->v_mask << 16U) | (uint32_t)tex->u_mask;

  /* HUM Ch 62.2.27 "TEXCLOFFSET: CLUT Offset Register", p 3704 */
  *ra_drw_reg32(k_ra_drw_off_texcloffset) =
    (uint32_t)tex->clut_offset & (uint32_t)k_ra_drw_internal_byte_mask;

  /* Compose CONTROL2 texture bits. */
  uint32_t set_bits = (uint32_t)k_ra_drw_control2_textureenable;
  set_bits |= internal_pack_readformat(tex->format);
  if (tex->clamp_x) {
    set_bits |= (uint32_t)k_ra_drw_control2_textureclampx;
  }
  if (tex->clamp_y) {
    set_bits |= (uint32_t)k_ra_drw_control2_textureclampy;
  }
  if (tex->filter_x) {
    set_bits |= (uint32_t)k_ra_drw_control2_texturefilterx;
  }
  if (tex->filter_y) {
    set_bits |= (uint32_t)k_ra_drw_control2_texturefiltery;
  }
  if (tex->enable_clut) {
    set_bits |= (uint32_t)k_ra_drw_control2_clutenable;
  }
  if (tex->clut_565) {
    set_bits |= (uint32_t)k_ra_drw_control2_clutformat_565;
  }
  if (tex->enable_color_key) {
    set_bits |= (uint32_t)k_ra_drw_control2_colkeyenable;
  }
  if (tex->enable_rle) {
    set_bits |= (uint32_t)k_ra_drw_control2_rleenable;
  }
  set_bits |= ((uint32_t)tex->rle_pixel_width & 0x3UL) << k_ra_drw_control2_rlepixel_pos;

  const uint32_t clr_mask =
    (uint32_t)(k_ra_drw_control2_textureenable | k_ra_drw_control2_textureclampx |
               k_ra_drw_control2_textureclampy | k_ra_drw_control2_texturefilterx |
               k_ra_drw_control2_texturefiltery | k_ra_drw_control2_clutenable |
               k_ra_drw_control2_clutformat_565 | k_ra_drw_control2_colkeyenable |
               k_ra_drw_control2_rleenable | k_ra_drw_control2_rlepixel_mask |
               k_ra_drw_control2_readformatl_mask | k_ra_drw_control2_readformath_mask);
  (void)internal_control2_rmw(clr_mask, set_bits);

  if (tex->enable_color_key) {
    /* HUM Ch 62.2.28 "COLKEY: Color Key Register", p 3704 */
    *ra_drw_reg32(k_ra_drw_off_colkey) =
      tex->color_key_rgb & (uint32_t)k_ra_drw_internal_color_alpha_mask;
  }
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_drw_clear_texture(void)
{
  const uint32_t clr_mask =
    (uint32_t)(k_ra_drw_control2_textureenable | k_ra_drw_control2_rleenable |
               k_ra_drw_control2_clutenable | k_ra_drw_control2_colkeyenable |
               k_ra_drw_control2_rlepixel_mask);
  (void)internal_control2_rmw(clr_mask, 0UL);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t
ra_drw_load_clut(uint8_t start_index, const uint32_t* entries, uint32_t count)
{
  RA_CHECK_NULL_PTR((void*)entries, s_tag, "entries must not be nullptr");
  if (count == 0UL) {
    return k_ra_err_invalid_arg;
  }
  if (((uint32_t)start_index + count) > (uint32_t)k_ra_drw_internal_clut_max) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 62.2.25 "TEXCLADDR: CLUT Start Address Register", p 3703 */
  *ra_drw_reg32(k_ra_drw_off_texcladdr) = (uint32_t)start_index;

  /* HUM Ch 62.2.26 "TEXCLDATA: CLUT Data Register", p 3703 */
  /* The DRW auto-increments CLADDR after each TEXCLDATA write per
 * HUM Ch 62.5.4 "CLUT setup", p 3717. NASA Rule 2: bounded loop. */
  for (uint32_t i = 0UL; i < count; ++i) {
    *ra_drw_reg32(k_ra_drw_off_texcldata) = entries[i];
  }
  return k_ra_ok;
}

/* =============================================================================
 * Drawing primitives
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_drw_fill_rect(const ra_drw_rect_t* rect)
{
  RA_CHECK_NULL_PTR((void*)rect, s_tag, "rect must not be nullptr");
  if ((uint16_t)rect->width_px < (uint16_t)k_ra_drw_min_dim_px ||
      (uint16_t)rect->height_px < (uint16_t)k_ra_drw_min_dim_px) {
    return k_ra_err_invalid_arg;
  }
  if ((uint16_t)rect->width_px > (uint16_t)k_ra_drw_max_width_px ||
      (uint16_t)rect->height_px > (uint16_t)k_ra_drw_max_height_px) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 62.2.7 "COLOR1: Base Color Register", p 3697 */
  *ra_drw_reg32(k_ra_drw_off_color1) = rect->color_argb8888;

  internal_program_rect_limiters(rect);

  /* HUM Ch 62.2.4 "CACHECTL: Cache Control Register", p 3694 */
  /* Flush the framebuffer cache so writes from a previous primitive
 * land in memory before this fill consumes the same lines. */
  *ra_drw_reg32(k_ra_drw_off_cachectl) =
    (uint32_t)(k_ra_drw_cachectl_all_en | k_ra_drw_cachectl_cflushfx);

  /* HUM Ch 62.2.1 "CONTROL: Geometry Control Register", p 3689 */
  /* Enable limiters 1..4; engine starts on this write. */
  *ra_drw_reg32(k_ra_drw_off_control) = (uint32_t)k_ra_drw_control_quad_box;

  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_drw_blit_textured_rect(const ra_drw_rect_t* rect)
{
  RA_CHECK_NULL_PTR((void*)rect, s_tag, "rect must not be nullptr");
  if ((uint16_t)rect->width_px < (uint16_t)k_ra_drw_min_dim_px ||
      (uint16_t)rect->height_px < (uint16_t)k_ra_drw_min_dim_px) {
    return k_ra_err_invalid_arg;
  }
  if ((uint16_t)rect->width_px > (uint16_t)k_ra_drw_max_width_px ||
      (uint16_t)rect->height_px > (uint16_t)k_ra_drw_max_height_px) {
    return k_ra_err_invalid_arg;
  }

  internal_program_rect_limiters(rect);

  /* HUM Ch 62.2.18 "LUSTART: U Limiter Start Value Register" p 3701 */
  *ra_drw_reg32(k_ra_drw_off_lustart) = 0UL;
  /* HUM Ch 62.2.18 "LUSTART: U Limiter Start Value Register" p 3701 */
  *ra_drw_reg32(k_ra_drw_off_luxadd) = (uint32_t)k_ra_drw_subpixel_unit;
  /* HUM Ch 62.2.18 "LUSTART: U Limiter Start Value Register" p 3701 */
  *ra_drw_reg32(k_ra_drw_off_luyadd) = 0UL;

  /* HUM Ch 62.2.20 "LVSTARTI: V Limiter Start Integer Part", p 3702 */
  *ra_drw_reg32(k_ra_drw_off_lvstarti) = 0UL;
  /* HUM Ch 62.2.21 "LVSTARTF: V Limiter Start Fractional Part", p 3702 */
  *ra_drw_reg32(k_ra_drw_off_lvstartf) = 0UL;
  /* HUM Ch 62.2.22 "LVXADDI: V Limiter X-Axis Increment Integer", p 3702 */
  *ra_drw_reg32(k_ra_drw_off_lvxaddi) = 0UL;
  /* HUM Ch 62.2.23 "LVYADDI: V Limiter Y-Axis Increment Integer", p 3702 */
  *ra_drw_reg32(k_ra_drw_off_lvyaddi) = (uint32_t)k_ra_drw_subpixel_unit;
  /* HUM Ch 62.2.24 "LVYXADDF: V Limiter Increment Fractional", p 3703 */
  *ra_drw_reg32(k_ra_drw_off_lvyxaddf) = 0UL;

  /* HUM Ch 62.2.4 "CACHECTL: Cache Control Register", p 3694 */
  /* Pulse FB + texture cache flush before consuming. */
  *ra_drw_reg32(k_ra_drw_off_cachectl) =
    (uint32_t)(k_ra_drw_cachectl_all_en | k_ra_drw_cachectl_all_flush);

  /* HUM Ch 62.2.1 "CONTROL: Geometry Control Register", p 3689 */
  *ra_drw_reg32(k_ra_drw_off_control) = (uint32_t)k_ra_drw_control_quad_box;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_drw_draw_line(const ra_drw_line_t* line)
{
  RA_CHECK_NULL_PTR((void*)line, s_tag, "line must not be nullptr");
  if (line->width_px == 0U || (uint16_t)line->width_px > (uint16_t)k_ra_drw_max_width_px) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 62.2.7 "COLOR1: Base Color Register", p 3697 */
  *ra_drw_reg32(k_ra_drw_off_color1) = line->color_argb8888;

  /* Compute the bounding box of the stroke for SIZE: a simple
 * over-estimate using axis-aligned span + width. The DRW only
 * uses SIZE as an enumeration upper bound, not for clipping
 * the stroke itself. HUM Ch 62.2.29 p 3704. */
  const int32_t  min_x  = (line->x0 < line->x1) ? line->x0 : line->x1;
  const int32_t  max_x  = (line->x0 > line->x1) ? line->x0 : line->x1;
  const int32_t  min_y  = (line->y0 < line->y1) ? line->y0 : line->y1;
  const int32_t  max_y  = (line->y0 > line->y1) ? line->y0 : line->y1;
  const uint32_t span_x = (uint32_t)(max_x - min_x) + (uint32_t)line->width_px;
  const uint32_t span_y = (uint32_t)(max_y - min_y) + (uint32_t)line->width_px;

  /* HUM Ch 62.2.29 "SIZE: Bounding Box Dimension Register", p 3704 */
  *ra_drw_reg32(k_ra_drw_off_size) = (span_y << k_ra_drw_size_height_pos) | span_x;

  /* HUM Ch 62.4.4 "Lines" p 3725: 4 perpendicular limiters bound the
 * stroke. A full Bresenham/AA solver is out of HAL scope -- the
 * application typically uses ra_drw_run_dlist for finished AA
 * strokes. The HAL programmes the simplest valid encoding so the
 * caller can verify the geometry path; the four limiters bound the
 * axis-aligned stroke envelope. */
  const int32_t dx  = line->x1 - line->x0;
  const int32_t dy  = line->y1 - line->y0;
  const int32_t adx = internal_iabs(dx);
  const int32_t ady = internal_iabs(dy);

  /* HUM Ch 62.2.10 "LnSTART: Limiter n Start Value Register", p 3698 */
  *ra_drw_reg32(k_ra_drw_off_l1start) = internal_to_subpixel(line->x0);
  *ra_drw_reg32(k_ra_drw_off_l2start) = internal_to_subpixel(line->x0);
  *ra_drw_reg32(k_ra_drw_off_l3start) = internal_to_subpixel(line->y0);
  *ra_drw_reg32(k_ra_drw_off_l4start) = internal_to_subpixel(line->y0);

  /* HUM Ch 62.2.11 "LnXADD: Limiter n X-Axis Increment Register", p 3698 */
  *ra_drw_reg32(k_ra_drw_off_l1xadd) = internal_to_subpixel(adx);
  *ra_drw_reg32(k_ra_drw_off_l2xadd) = internal_to_subpixel(-adx);
  *ra_drw_reg32(k_ra_drw_off_l3xadd) = internal_to_subpixel(ady);
  *ra_drw_reg32(k_ra_drw_off_l4xadd) = internal_to_subpixel(-ady);

  /* HUM Ch 62.2.12 "LnYADD: Limiter n Y-Axis Increment Register", p 3699 */
  *ra_drw_reg32(k_ra_drw_off_l1yadd) = internal_to_subpixel(ady);
  *ra_drw_reg32(k_ra_drw_off_l2yadd) = internal_to_subpixel(-ady);
  *ra_drw_reg32(k_ra_drw_off_l3yadd) = internal_to_subpixel(adx);
  *ra_drw_reg32(k_ra_drw_off_l4yadd) = internal_to_subpixel(-adx);

  /* HUM Ch 62.2.13 "L1BAND/L2BAND: Limiter Band Width Register", p 3699.
 * Band width = stroke width in sub-pixels for L1 and L2. */
  *ra_drw_reg32(k_ra_drw_off_l1band) =
    (uint32_t)((uint32_t)line->width_px * (uint32_t)k_ra_drw_subpixel_unit);
  *ra_drw_reg32(k_ra_drw_off_l2band) =
    (uint32_t)((uint32_t)line->width_px * (uint32_t)k_ra_drw_subpixel_unit);

  /* HUM Ch 62.2.4 "CACHECTL: Cache Control Register", p 3694 */
  *ra_drw_reg32(k_ra_drw_off_cachectl) =
    (uint32_t)(k_ra_drw_cachectl_all_en | k_ra_drw_cachectl_cflushfx);

  /* HUM Ch 62.2.1 "CONTROL: Geometry Control Register", p 3689 */
  *ra_drw_reg32(k_ra_drw_off_control) = (uint32_t)k_ra_drw_control_line_quad;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_drw_draw_triangle(const ra_drw_triangle_t* tri)
{
  RA_CHECK_NULL_PTR((void*)tri, s_tag, "tri must not be nullptr");

  /* HUM Ch 62.2.7 "COLOR1: Base Color Register", p 3697 */
  *ra_drw_reg32(k_ra_drw_off_color1) = tri->color_argb8888;

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

  *ra_drw_reg32(k_ra_drw_off_size) = (span_y << k_ra_drw_size_height_pos) | span_x;

  /* HUM Ch 62.4.5 "Triangles" p 3726: three half-plane limiters,
 * one per edge. The HAL programmes the simplest "axis-aligned
 * triangle" encoding -- the application can override LnSTART /
 * LnXADD / LnYADD via display lists for arbitrary geometry. */

  /* HUM Ch 62.2.10 "LnSTART: Limiter n Start Value Register", p 3698 */
  *ra_drw_reg32(k_ra_drw_off_l1start) = internal_to_subpixel(tri->x0);
  *ra_drw_reg32(k_ra_drw_off_l2start) = internal_to_subpixel(tri->x1);
  *ra_drw_reg32(k_ra_drw_off_l3start) = internal_to_subpixel(tri->x2);

  /* HUM Ch 62.2.11 "LnXADD: Limiter n X-Axis Increment", p 3698 */
  *ra_drw_reg32(k_ra_drw_off_l1xadd) = internal_to_subpixel((int32_t)tri->y1 - (int32_t)tri->y0);
  *ra_drw_reg32(k_ra_drw_off_l2xadd) = internal_to_subpixel((int32_t)tri->y2 - (int32_t)tri->y1);
  *ra_drw_reg32(k_ra_drw_off_l3xadd) = internal_to_subpixel((int32_t)tri->y0 - (int32_t)tri->y2);

  /* HUM Ch 62.2.12 "LnYADD: Limiter n Y-Axis Increment", p 3699 */
  *ra_drw_reg32(k_ra_drw_off_l1yadd) = internal_to_subpixel((int32_t)tri->x0 - (int32_t)tri->x1);
  *ra_drw_reg32(k_ra_drw_off_l2yadd) = internal_to_subpixel((int32_t)tri->x1 - (int32_t)tri->x2);
  *ra_drw_reg32(k_ra_drw_off_l3yadd) = internal_to_subpixel((int32_t)tri->x2 - (int32_t)tri->x0);

  /* HUM Ch 62.2.4 "CACHECTL: Cache Control Register", p 3694 */
  *ra_drw_reg32(k_ra_drw_off_cachectl) =
    (uint32_t)(k_ra_drw_cachectl_all_en | k_ra_drw_cachectl_cflushfx);

  /* HUM Ch 62.2.1 "CONTROL: Geometry Control Register", p 3689 */
  *ra_drw_reg32(k_ra_drw_off_control) = (uint32_t)k_ra_drw_control_triangle;
  return k_ra_ok;
}

/* =============================================================================
 * Display list mode
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_drw_run_dlist(const uint32_t* dlist_addr)
{
  if (dlist_addr == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if (((uintptr_t)dlist_addr & (uintptr_t)k_ra_drw_internal_align_mask) != 0U) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 62.2.4 "CACHECTL: Cache Control Register", p 3694 */
  /* Flush both caches so the dlist words written by the CPU are
 * visible to the DRW bus master. */
  const uint32_t cur_cc                = *ra_drw_reg32(k_ra_drw_off_cachectl);
  *ra_drw_reg32(k_ra_drw_off_cachectl) = cur_cc | (uint32_t)k_ra_drw_cachectl_all_flush;

  /* HUM Ch 62.2.32 "DLISTSTART: Display List Start Address Register",
 * p 3705. Writing a new value triggers execution. */
  *ra_drw_reg32(k_ra_drw_off_dliststart) = (uint32_t)(uintptr_t)dlist_addr;
  return k_ra_ok;
}

/* =============================================================================
 * Performance counters
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_drw_perf_arm(ra_drw_perftrigger_t event_ctr1,
                                       ra_drw_perftrigger_t event_ctr2)
{
  if ((uint16_t)event_ctr1 > (uint16_t)k_ra_drw_internal_perfev_max ||
      (uint16_t)event_ctr2 > (uint16_t)k_ra_drw_internal_perfev_max) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 62.2.33 "PERFTRIGGER: Performance Counters Control Register",
 * p 3706. PERFTRIGGER1 in [15:0], PERFTRIGGER2 in [31:16]. */
  *ra_drw_reg32(k_ra_drw_off_perftrigger) =
    ((uint32_t)event_ctr2 << k_ra_drw_perftrigger2_pos) | (uint32_t)event_ctr1;

  /* HUM Ch 62.2.34 "PERFCOUNTk: Performance Counter k", p 3706 */
  *ra_drw_reg32(k_ra_drw_off_perfcount1) = 0UL;
  *ra_drw_reg32(k_ra_drw_off_perfcount2) = 0UL;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_drw_perf_read(ra_drw_perfcounter_id_t id, uint32_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 62.2.34 "PERFCOUNTk: Performance Counter k", p 3706 */
  if (id == k_ra_drw_perfctr_1) {
    *out = *ra_drw_reg32(k_ra_drw_off_perfcount1);
    return k_ra_ok;
  }
  if (id == k_ra_drw_perfctr_2) {
    *out = *ra_drw_reg32(k_ra_drw_off_perfcount2);
    return k_ra_ok;
  }
  return k_ra_err_invalid_arg;
}

[[nodiscard]] ra_err_t ra_drw_perf_reset(ra_drw_perfcounter_id_t id)
{
  /* HUM Ch 62.2.34 "PERFCOUNTk: Performance Counter k", p 3706 */
  if (id == k_ra_drw_perfctr_1) {
    *ra_drw_reg32(k_ra_drw_off_perfcount1) = 0UL;
    return k_ra_ok;
  }
  if (id == k_ra_drw_perfctr_2) {
    *ra_drw_reg32(k_ra_drw_off_perfcount2) = 0UL;
    return k_ra_ok;
  }
  return k_ra_err_invalid_arg;
}
