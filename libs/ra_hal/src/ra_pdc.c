/**
 * @file ra_pdc.c
 * @brief Parallel Data Capture (PDC) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Hand-written PDC driver mirroring FSP's ``r_pdc``. PDC is the
 * parallel-bus camera bridge -- it accepts a parallel pixel
 * stream from a CMOS sensor and DMA-fills a contiguous
 * framebuffer through its 22-stage 32-bit FIFO (PCDR). Distinct
 * from CEU (which has its own Y/Cb/Cr separator + scaler).
 *
 * The driver only programmes the PDC peripheral itself -- the
 * downstream DMAC/DTC channel that drains PCDR is owned by the
 * application. ``ra_pdc_capture_start`` arms PCCR1.PCE, the
 * IRQ dispatcher snapshots PCSR + invokes the callback, and
 * ``ra_pdc_capture_stop`` clears PCE for an early abort.
 *
 * Because PDC is not exposed in the RA8D2 ``ra_mstp_t`` enum
 * (the canonical FSP r_pdc IP block has no MSTPCRA..E slot
 * documented for this MCU), this driver does not gate on
 * ``ra_mstp_enable``; the block runs from its CGC-managed
 * peripheral clock and is implicitly idle while PCCR1.PCE = 0.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_pdc.h"

#include <stdint.h>

#include "ra8d2_pdc_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "PDC";

/** @brief Registered frame-end / error callback. */
static ra_pdc_event_fn_t s_pdc_fn;
/** @brief Opaque context handed back to ::s_pdc_fn. */
static void* s_pdc_ctx;
/** @brief Destination buffer for the active capture. */
static uint8_t* s_pdc_buf;

/**
 * @brief Build PCCR0 from a configuration descriptor.
 *
 * @param[in] cfg Non-NULL configuration descriptor.
 * @return Packed PCCR0 control word.
 */
static inline uint32_t internal_ra_pdc_build_pccr0(const ra_pdc_config_t* cfg)
{
  uint32_t v = 0U;

  if (cfg->pcko_output_enable) {
    v |= k_ra_pdc_mask_pccr0_pcke;
    v |= k_ra_pdc_mask_pccr0_pckoe;
  }
  if (cfg->vsync_polarity == k_ra_pdc_pol_active_low) {
    v |= k_ra_pdc_mask_pccr0_vps;
  }
  if (cfg->hsync_polarity == k_ra_pdc_pol_active_low) {
    v |= k_ra_pdc_mask_pccr0_hps;
  }
  if (cfg->endian == k_ra_pdc_endian_big) {
    v |= k_ra_pdc_mask_pccr0_eds;
  }
  v |= ((uint32_t)cfg->clock_div << (uint32_t)k_ra_pdc_shift_pckdiv) & k_ra_pdc_mask_pccr0_pckdiv;

  if (cfg->enable_frame_end_irq) {
    v |= k_ra_pdc_mask_pccr0_feie;
  }
  if (cfg->enable_error_irq) {
    v |= k_ra_pdc_mask_pccr0_ovie;
    v |= k_ra_pdc_mask_pccr0_udrie;
    v |= k_ra_pdc_mask_pccr0_verie;
    v |= k_ra_pdc_mask_pccr0_herie;
  }
  return v;
}

/**
 * @brief Validate that ``start + size`` fits the 12-bit window.
 */
static inline bool internal_ra_pdc_window_ok(uint16_t start, uint16_t size)
{
  if (size == 0U) {
    return false;
  }
  const uint32_t end = (uint32_t)start + (uint32_t)size;
  return end <= (uint32_t)k_ra_pdc_max_position + 1U;
}

[[nodiscard]] ra_err_t ra_pdc_open(const ra_pdc_config_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");

  if (!internal_ra_pdc_window_ok(cfg->x_capture_start_byte, cfg->x_capture_bytes) ||
      !internal_ra_pdc_window_ok(cfg->y_capture_start_pixel, cfg->y_capture_pixels)) {
    ra_log_error(s_tag, "pdc_open: capture window out of range");
    return k_ra_err_invalid_arg;
  }

  volatile r_pdc_regs_t* reg = ra_pdc();

  /* Programme PCCR0 first so subsequent PCE assert sees the
   * intended polarity / endian / IE configuration. */
  reg->PCCR0 = internal_ra_pdc_build_pccr0(cfg);

  reg->VCR =
    ((uint32_t)cfg->y_capture_start_pixel & k_ra_pdc_mask_vcr_vst) |
    (((uint32_t)cfg->y_capture_pixels << (uint32_t)k_ra_pdc_shift_size) & k_ra_pdc_mask_vcr_vsz);
  reg->HCR =
    ((uint32_t)cfg->x_capture_start_byte & k_ra_pdc_mask_hcr_hst) |
    (((uint32_t)cfg->x_capture_bytes << (uint32_t)k_ra_pdc_shift_size) & k_ra_pdc_mask_hcr_hsz);

  /* PCE stays clear; capture is armed by ra_pdc_capture_start. */
  reg->PCCR1 = 0U;
  /* Clear sticky status so the first capture sees a clean slate. */
  reg->PCSR = k_ra_pdc_mask_pcsr_w1c;

  s_pdc_buf = nullptr;
  ra_log_info(s_tag, "pdc_open");
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_pdc_close(void)
{
  volatile r_pdc_regs_t* reg = ra_pdc();
  reg->PCCR1                 = 0U;
  reg->PCCR0                 = 0U;
  s_pdc_fn                   = nullptr;
  s_pdc_ctx                  = nullptr;
  s_pdc_buf                  = nullptr;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_pdc_capture_start(uint8_t* buf)
{
  RA_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");

  volatile r_pdc_regs_t* reg = ra_pdc();
  s_pdc_buf                  = buf;
  reg->PCSR                  = k_ra_pdc_mask_pcsr_w1c;
  reg->PCCR1                 = k_ra_pdc_mask_pccr1_pce;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_pdc_capture_stop(void)
{
  ra_pdc()->PCCR1 = 0U;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_pdc_get_status(uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  *out_mask = ra_pdc()->PCSR;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_pdc_clear_status(uint32_t mask)
{
  ra_pdc()->PCSR = mask & k_ra_pdc_mask_pcsr_w1c;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_pdc_attach_handler(ra_pdc_event_fn_t fn, void* ctx)
{
  s_pdc_fn  = fn;
  s_pdc_ctx = ctx;
  return k_ra_ok;
}

void ra_pdc_dispatch(void)
{
  volatile r_pdc_regs_t*  reg  = ra_pdc();
  const uint32_t          mask = reg->PCSR;
  const ra_pdc_event_fn_t fn   = s_pdc_fn;
  void* const             ctx  = s_pdc_ctx;
  uint8_t* const          buf  = s_pdc_buf;
  reg->PCSR                    = mask & k_ra_pdc_mask_pcsr_w1c;
  if (fn != nullptr) {
    fn(ctx, mask, buf);
  }
}
