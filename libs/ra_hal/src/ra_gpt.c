/**
 * @file ra_gpt.c
 * @brief General PWM Timer (GPT) driver implementation
 *
 * @details
 * Provides a minimal "free-running 32-bit timer" interface on top of
 * the GPT register block. A full PWM / compare-match driver will
 * land once the motor-control layer needs it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_gpt.h"

#include <stdint.h>

#include "ra8d2_gpt_regs.h"
#include "ra_check.h"
#include "ra_dma.h"
#include "ra_dmac.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "GPT";

/**
 * @var s_gpt_mstp_table
 * @brief Channel-index -> MSTP id lookup. GPT4..GPT9 share a single
 *        bit (MSTPE27); the other channels each have their own.
 *        Sized by ``k_ra_gpt_channel_count`` from ``ra8d2_gpt_regs.h``.
 *        HUM Ch 11.2.10 "MSTPCRE", p 449..450.
 */
static const ra_mstp_t s_gpt_mstp_table[k_ra_gpt_channel_count] = {
  k_ra_mstp_gpt0,
  k_ra_mstp_gpt1,
  k_ra_mstp_gpt2,
  k_ra_mstp_gpt3,
  k_ra_mstp_gpt4_9,
  k_ra_mstp_gpt4_9,
  k_ra_mstp_gpt4_9,
  k_ra_mstp_gpt4_9,
  k_ra_mstp_gpt4_9,
  k_ra_mstp_gpt4_9,
  k_ra_mstp_gpt10,
  k_ra_mstp_gpt11,
  k_ra_mstp_gpt12,
  k_ra_mstp_gpt13,
};

/**
 * @enum ra_gtwp_t
 * @brief GTWP write-protect key.
 */
typedef enum : uint32_t {
  k_ra_gtwp_key_unlock = 0xA500U, /**< Password in upper byte, WP=0. */
  k_ra_gtwp_key_lock   = 0xA501U, /**< Password in upper byte, WP=1. */
} ra_gtwp_t;

/**
 * @enum ra_gpt_bits_t
 * @brief GTCR / GTSTR / GTSTP bit positions.
 */
typedef enum : uint32_t {
  k_ra_gpt_gtcr_cst_set    = 0x00000001UL, /**< GTCR.CST start.       */
  k_ra_gpt_gtcr_md_shift   = 16U,          /**< GTCR.MD bit0.        */
  k_ra_gpt_gtcr_tpcs_shift = 24U,          /**< GTCR.TPCS bit0.      */
  k_ra_gpt_gtstr_start     = 0x00000001UL, /**< GTSTR.CSTRT0 write.  */
  k_ra_gpt_gtstp_stop      = 0x00000001UL, /**< GTSTP.CSTOP0 write.  */
  k_ra_gpt_gtst_mask       = 0x00000033UL, /**< OVF|UDF|CCRA|CCRB.   */
} ra_gpt_bits_t;

/**
 * @struct ra_gpt_state_t
 * @brief Per-channel runtime state (callback, configured flag).
 */
typedef struct {
  ra_gpt_event_fn_t fn;         /**< Registered callback.   */
  void*             ctx;        /**< Callback context.      */
  bool              configured; /**< True after ra_gpt_init. */
} ra_gpt_state_t;

static ra_gpt_state_t s_gpt_state[k_ra_gpt_channel_count];

static uint32_t internal_gtcr(ra_gpt_mode_t mode, ra_gpt_prescaler_t ps)
{
  return ((uint32_t)mode << (uint32_t)k_ra_gpt_gtcr_md_shift) |
         ((uint32_t)ps << (uint32_t)k_ra_gpt_gtcr_tpcs_shift);
}

ra_err_t ra_gpt_start_free_run(uint8_t channel, uint32_t period)
{
  volatile r_gpt_channel_regs_t* reg = ra_gpt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  if (channel >= (uint8_t)k_ra_gpt_channel_count) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 11.2.10 "MSTPCRE : Module Stop Control Register E", p 449 */
  const ra_err_t mst_err = ra_mstp_enable(s_gpt_mstp_table[channel]);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "gpt_start: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  reg->GTWP  = (uint32_t)k_ra_gtwp_key_unlock;
  reg->GTSTP = 1UL;          /* Stop if running. */
  reg->GTCR  = 0x00000001UL; /* Saw-wave PWM mode. */
  reg->GTPR  = period;
  reg->GTCNT = 0U;
  reg->GTSTR = 1UL; /* Start. */
  reg->GTWP  = (uint32_t)k_ra_gtwp_key_lock;

  ra_log_info_val(s_tag, "start channel", (uint32_t)channel);
  return k_ra_ok;
}

ra_err_t ra_gpt_stop(uint8_t channel)
{
  volatile r_gpt_channel_regs_t* reg = ra_gpt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  reg->GTWP  = (uint32_t)k_ra_gtwp_key_unlock;
  reg->GTSTP = 1UL;
  reg->GTWP  = (uint32_t)k_ra_gtwp_key_lock;
  return k_ra_ok;
}

ra_err_t ra_gpt_read(uint8_t channel, uint32_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  volatile r_gpt_channel_regs_t* reg = ra_gpt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  *out = reg->GTCNT;
  return k_ra_ok;
}

/* =============================================================================
 * Wave 3.5 -- full build-out
 * =============================================================================
 */

ra_err_t ra_gpt_init(uint8_t channel, const ra_gpt_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR((void*)cfg, s_tag, "cfg must not be nullptr");
  volatile r_gpt_channel_regs_t* reg = ra_gpt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  /* HUM Ch 11.2.10 "MSTPCRE : Module Stop Control Register E", p 449 */
  const ra_err_t mst_err = ra_mstp_enable(s_gpt_mstp_table[channel]);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "gpt_init: mstp enable");

  reg->GTWP  = (uint32_t)k_ra_gtwp_key_unlock;
  reg->GTSTP = (uint32_t)k_ra_gpt_gtstp_stop;
  reg->GTCR  = internal_gtcr(cfg->mode, cfg->prescaler);
  reg->GTPR  = cfg->period;
  reg->GTPBR = cfg->period;
  /* HUM Ch 22.2.12 "GTCCRA..F : General PWM Timer Compare Capture Register",
     p 968 */
  reg->GTCCR[0] = cfg->duty_a;
  reg->GTCCR[1] = cfg->duty_b;
  reg->GTCNT    = 0U;
  if (cfg->auto_start) {
    reg->GTCR |= (uint32_t)k_ra_gpt_gtcr_cst_set;
    reg->GTSTR = (uint32_t)k_ra_gpt_gtstr_start;
  }
  reg->GTWP = (uint32_t)k_ra_gtwp_key_lock;

  s_gpt_state[channel].configured = true;
  ra_log_info_val(s_tag, "init channel", (uint32_t)channel);
  return k_ra_ok;
}

ra_err_t ra_gpt_deinit(uint8_t channel)
{
  volatile r_gpt_channel_regs_t* reg = ra_gpt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  reg->GTWP  = (uint32_t)k_ra_gtwp_key_unlock;
  reg->GTSTP = (uint32_t)k_ra_gpt_gtstp_stop;
  reg->GTCR  = 0U;
  reg->GTWP  = (uint32_t)k_ra_gtwp_key_lock;

  s_gpt_state[channel].fn         = nullptr;
  s_gpt_state[channel].ctx        = nullptr;
  s_gpt_state[channel].configured = false;
  (void)ra_mstp_disable(s_gpt_mstp_table[channel]);
  return k_ra_ok;
}

ra_err_t ra_gpt_set_period(uint8_t channel, uint32_t period)
{
  volatile r_gpt_channel_regs_t* reg = ra_gpt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  reg->GTWP  = (uint32_t)k_ra_gtwp_key_unlock;
  reg->GTPR  = period;
  reg->GTPBR = period;
  reg->GTWP  = (uint32_t)k_ra_gtwp_key_lock;
  return k_ra_ok;
}

ra_err_t ra_gpt_set_duty(uint8_t channel, ra_gpt_ccr_sel_t which, uint32_t value)
{
  volatile r_gpt_channel_regs_t* reg = ra_gpt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  if ((uint8_t)which > (uint8_t)k_ra_gpt_ccr_b) {
    return k_ra_err_invalid_arg;
  }

  reg->GTWP                  = (uint32_t)k_ra_gtwp_key_unlock;
  reg->GTCCR[(uint8_t)which] = value;
  reg->GTWP                  = (uint32_t)k_ra_gtwp_key_lock;
  return k_ra_ok;
}

ra_err_t ra_gpt_get_status(uint8_t channel, uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  volatile r_gpt_channel_regs_t* reg = ra_gpt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  *out_mask = reg->GTST & (uint32_t)k_ra_gpt_gtst_mask;
  return k_ra_ok;
}

ra_err_t ra_gpt_clear_status(uint8_t channel, uint32_t mask)
{
  volatile r_gpt_channel_regs_t* reg = ra_gpt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  /* GTST bits are cleared by writing the current value with target bits zero. */
  const uint32_t current = reg->GTST;
  reg->GTST              = current & ~(mask & (uint32_t)k_ra_gpt_gtst_mask);
  return k_ra_ok;
}

ra_err_t ra_gpt_attach_handler(uint8_t channel, ra_gpt_event_fn_t fn, void* ctx)
{
  if (channel >= (uint8_t)k_ra_gpt_channel_count) {
    return k_ra_err_invalid_arg;
  }
  s_gpt_state[channel].fn  = fn;
  s_gpt_state[channel].ctx = ctx;
  return k_ra_ok;
}

ra_err_t ra_gpt_enter_stop(uint8_t channel)
{
  if (channel >= (uint8_t)k_ra_gpt_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_gpt_channel_regs_t* reg = ra_gpt(channel);
  if (reg != nullptr) {
    reg->GTWP  = (uint32_t)k_ra_gtwp_key_unlock;
    reg->GTSTP = (uint32_t)k_ra_gpt_gtstp_stop;
    reg->GTWP  = (uint32_t)k_ra_gtwp_key_lock;
  }
  return ra_mstp_disable(s_gpt_mstp_table[channel]);
}

ra_err_t ra_gpt_exit_stop(uint8_t channel)
{
  if (channel >= (uint8_t)k_ra_gpt_channel_count) {
    return k_ra_err_invalid_arg;
  }
  return ra_mstp_enable(s_gpt_mstp_table[channel]);
}

/* ---- DMA TX / RX (Wave 3.7b) ----------------------------------------- */

ra_err_t ra_gpt_write_dma(uint8_t              channel,
                          const uint32_t*      periods,
                          uint16_t             count,
                          ra_dma_complete_fn_t on_complete,
                          void*                ctx,
                          uint8_t*             out_dma_channel)
{
  RA_CHECK_NULL_PTR((void*)periods, s_tag, "gpt_write_dma: periods");
  RA_CHECK_NULL_PTR(out_dma_channel, s_tag, "gpt_write_dma: out_dma_channel");
  if ((channel >= (uint8_t)k_ra_gpt_channel_count) || (count == 0U)) {
    return k_ra_err_invalid_arg;
  }
  volatile r_gpt_channel_regs_t* reg = ra_gpt(channel);
  if (reg == nullptr) {          /* GCOVR_EXCL_BR_LINE */
    return k_ra_err_invalid_arg; /* GCOVR_EXCL_LINE */
  }
  /* HUM Ch 22.2 "GTPR : General PWM Timer Cycle Setting Register", p 878 */
  /* Word-wide DMA writes stream period values into GTPR; dst_inc=false
   * so every element lands at the same MMIO address. */
  ra_dma_request_t req = {};
  req.src_addr         = (uintptr_t)periods;
  req.dst_addr         = (uintptr_t)&reg->GTPR;
  req.count            = count;
  req.width            = k_ra_dmac_width_word;
  req.src_inc          = true;
  req.dst_inc          = false;
  req.trigger          = (ra_elc_event_t)0;
  req.on_complete      = on_complete;
  req.ctx              = ctx;
  return ra_dma_request(&req, out_dma_channel);
}

/* out_counts is written by the DMAC engine. */
ra_err_t ra_gpt_read_dma(uint8_t              channel,
                         uint32_t*            out_counts, // NOLINT(readability-non-const-parameter)
                         uint16_t             count,
                         ra_dma_complete_fn_t on_complete,
                         void*                ctx,
                         uint8_t*             out_dma_channel)
{
  RA_CHECK_NULL_PTR(out_counts, s_tag, "gpt_read_dma: out_counts");
  RA_CHECK_NULL_PTR(out_dma_channel, s_tag, "gpt_read_dma: out_dma_channel");
  if ((channel >= (uint8_t)k_ra_gpt_channel_count) || (count == 0U)) {
    return k_ra_err_invalid_arg;
  }
  volatile r_gpt_channel_regs_t* reg = ra_gpt(channel);
  if (reg == nullptr) {          /* GCOVR_EXCL_BR_LINE */
    return k_ra_err_invalid_arg; /* GCOVR_EXCL_LINE */
  }
  /* HUM Ch 22.2 "GTCNT : General PWM Timer Counter", p 878 */
  /* Word-wide DMA reads stream GTCNT snapshots into out_counts[]. */
  ra_dma_request_t req = {};
  req.src_addr         = (uintptr_t)&reg->GTCNT;
  req.dst_addr         = (uintptr_t)out_counts;
  req.count            = count;
  req.width            = k_ra_dmac_width_word;
  req.src_inc          = false;
  req.dst_inc          = true;
  req.trigger          = (ra_elc_event_t)0;
  req.on_complete      = on_complete;
  req.ctx              = ctx;
  return ra_dma_request(&req, out_dma_channel);
}

/* ---- ISR dispatch ---------------------------------------------------- */

static void internal_dispatch(uint8_t channel, uint32_t status_mask)
{
  if (channel >= (uint8_t)k_ra_gpt_channel_count) {
    return;
  }
  volatile r_gpt_channel_regs_t* reg = ra_gpt(channel);
  if (reg != nullptr) {
    const uint32_t current = reg->GTST;
    reg->GTST              = current & ~status_mask;
  }
  const ra_gpt_event_fn_t fn  = s_gpt_state[channel].fn;
  void* const             ctx = s_gpt_state[channel].ctx;
  if (fn != nullptr) {
    fn(ctx, status_mask);
  }
}

void ra_gpt_dispatch_ovf(uint8_t channel)
{
  internal_dispatch(channel, (uint32_t)k_ra_gpt_status_overflow);
}

void ra_gpt_dispatch_und(uint8_t channel)
{
  internal_dispatch(channel, (uint32_t)k_ra_gpt_status_underflow);
}

void ra_gpt_dispatch_ccra(uint8_t channel)
{
  internal_dispatch(channel, (uint32_t)k_ra_gpt_status_ccra);
}

void ra_gpt_dispatch_ccrb(uint8_t channel)
{
  internal_dispatch(channel, (uint32_t)k_ra_gpt_status_ccrb);
}
