/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_gpt.c
 * @brief General PWM Timer (GPT) driver implementation
 *
 * @details
 * Provides a minimal "free-running 32-bit timer" interface on top of
 * the GPT register block. A full PWM / compare-match driver will
 * land once the motor-control layer needs it.
 */

#include "ra8_gpt.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_dma.h"
#include "ra8_dmac.h"
#include "ra8_err.h"
#include "ra8_gpt_capture.h"
#include "ra8_gpt_regs.h"
#include "ra8_log.h"
#include "ra8_mstp.h"

static const char* s_tag = "GPT";

/**
 * @var s_gpt_mstp_table
 * @brief Channel-index -> MSTP id lookup. GPT4..GPT9 share a single
 * bit (MSTPE27); the other channels each have their own.
 * Sized by ``k_ra8_gpt_channel_count`` from ``ra8_gpt_regs.h``.
 * HUM Ch 11.2.10 "MSTPCRE", p 449..450.
 */
static const ra8_mstp_t s_gpt_mstp_table[k_ra8_gpt_channel_count] = {
  k_ra8_mstp_gpt0,
  k_ra8_mstp_gpt1,
  k_ra8_mstp_gpt2,
  k_ra8_mstp_gpt3,
  k_ra8_mstp_gpt4_9,
  k_ra8_mstp_gpt4_9,
  k_ra8_mstp_gpt4_9,
  k_ra8_mstp_gpt4_9,
  k_ra8_mstp_gpt4_9,
  k_ra8_mstp_gpt4_9,
  k_ra8_mstp_gpt10,
  k_ra8_mstp_gpt11,
  k_ra8_mstp_gpt12,
  k_ra8_mstp_gpt13,
};

/**
 * @enum ra8_gtwp_t
 * @brief GTWP write-protect key.
 */
typedef enum : uint32_t {
  k_ra8_gtwp_key_unlock = 0xA500U, /**< Password in upper byte, WP=0. */
  k_ra8_gtwp_key_lock   = 0xA501U, /**< Password in upper byte, WP=1. */
} ra8_gtwp_t;

/**
 * @enum ra8_gptclkcr_bits_t
 * @brief GTCLKCR block-level clock-control bits.
 *
 * @details
 * GTCLKCR is the GPT-bank-wide clock-domain register at 0x40323F10
 * (per ``k_ra8_gpt_gtclk_addr`` in ra8_gpt_regs.h). HUM Ch 22.2.47
 * "GTCLKCR : General PWM Timer Clock Control Register", p 974, plus
 * HUM Ch 22.10.1 "Module-Stop Function Setting", p 1146, require
 * GTCLKCR to be programmed BEFORE MSTPCRE.MSTPE31 (or any other GPT
 * MSTP bit) is cleared. Without that step the per-channel bus side
 * of the GPT block sits in an undefined clock-domain-crossing
 * state and writes to GTWP / GTCR / etc. silently drop. Symptom on
 * this chip: every J-Link or firmware read of 0x40322000 onward
 * returns 0 even after MSTPCR is cleared.
 *
 * BPEN = 1 ties GTCLK to PCLKA synchronously, which matches the
 * project's CGC tree (no separate GPTCLK source via the CGC's
 * ``GPTCKCR``).
 */
typedef enum : uint32_t {
  k_ra8_gptclkcr_bpen = 0x00000001UL, /**< BPEN: PCLKA <-> GTCLK sync. */
} ra8_gptclkcr_bits_t;

/**
 * @brief Block-level GPT clock-control init (one-shot, MSTP-stopped).
 *
 * @details
 * Programs GTCLKCR.BPEN=1 while every GPT channel's MSTP bit in
 * MSTPCRE is still asserted (the chip-default reset state). The HUM
 * Ch 22.10.1 p 1146 explicitly states "Set GTCLKCR register before
 * releasing the module-stop state" and Ch 22.2.47 p 974 adds "Set
 * first of initial setting after resetting. If MSTPCRE.MSTPE31 bit
 * is 0, changing this register is prohibited". This helper must
 * therefore run before the first ``ra8_mstp_enable(k_ra8_mstp_gptN)``
 * call. A static guard makes it idempotent: subsequent
 * ``ra8_gpt_init`` calls on additional channels skip the write.
 *
 * @pre At least one GPT channel's MSTP bit is still asserted (any
 *      channel works -- the register is bank-wide, one instance).
 * @pre The chip is running secure-mode or the firmware accesses
 *      0x40323F10 via the secure alias.
 *
 * @post GTCLKCR.BPEN == 1; the GPT bus interface is now in a
 *       defined clock-domain state and subsequent MSTP releases
 *       leave per-channel registers writable.
 * @post Subsequent calls become no-ops via the static guard.
 *
 * @note Not thread-safe; intended to run from single-threaded
 *       boot context before any GPT channel is enabled.
 * @since 0.1.0
 */
static void internal_gpt_clock_block_init(void)
{
  static bool s_gpt_clock_inited = false;
  if (s_gpt_clock_inited) {
    return;
  }
  /* Enable GTCLK source for the GPT block. */
  /* HUM Ch 22.2.47 "GTCLKCR : Clock Control Register" p 975 */
  /* HUM Ch 22.10.1 "Clock and Pin Setup" p 1146 */
  volatile uint32_t* gtclkcr = (volatile uint32_t*)k_ra8_gpt_gtclk_addr;
  *gtclkcr                   = (uint32_t)k_ra8_gptclkcr_bpen;
  s_gpt_clock_inited         = true;
}

/**
 * @enum ra8_gpt_bits_t
 * @brief GTCR / GTSTR / GTSTP / GTST bit positions.
 *
 * @details
 * Per HUM Ch 22.2.5 "GTCR : General PWM Timer Control Register",
 * p 904..906: CST = bit 0, MD = bits 16..19, TPCS = bits 23..26.
 * GTST flag layout from HUM Ch 22.2.16 "GTST : General PWM Timer
 * Status Register", p 962..964: TCFA = bit 0, TCFB = bit 1,
 * TCFPO = bit 6, TCFPU = bit 7.
 */
typedef enum : uint32_t {
  k_ra8_gpt_gtcr_cst_set    = 0x00000001UL, /**< GTCR.CST start.        */
  k_ra8_gpt_gtcr_md_shift   = 16U,          /**< GTCR.MD bit0.          */
  k_ra8_gpt_gtcr_tpcs_shift = 23U,          /**< GTCR.TPCS bit0.        */
  k_ra8_gpt_gtstr_start     = 0x00000001UL, /**< GTSTR.CSTRT0 write.    */
  k_ra8_gpt_gtstp_stop      = 0x00000001UL, /**< GTSTP.CSTOP0 write.    */
  k_ra8_gpt_gtst_mask       = 0x000000C3UL, /**< TCFPU|TCFPO|TCFB|TCFA. */
} ra8_gpt_bits_t;

/**
 * @enum ra8_gpt_gtior_bits_t
 * @brief GTIOR field positions and masks.
 *
 * @details
 * From the RA8D2 device CMSIS header for R_GPT0_GTIOR (mirrored
 * from HUM Ch 22.2.13 "GTIOR : General PWM Timer I/O Control
 * Register", p 942..946):
 * - GTIOA[4:0]  bits  0..4
 * - OADFLT      bit   6
 * - OAE         bit   8
 * - OADF[1:0]   bits  9..10
 * - GTIOB[4:0]  bits 16..20
 * - OBDFLT      bit  22
 * - OBE         bit  24
 * - OBDF[1:0]   bits 25..26
 *
 * The two GTIO sub-field encodings used by the polarity helper come
 * from HUM Table 22.18 (GTIOA[4:0] / GTIOB[4:0] settings):
 * - 0x9 -> "low at compare match, high at cycle end"
 *          == active-high PWM
 * - 0x6 -> "high at compare match, low at cycle end"
 *          == active-low  PWM
 */
typedef enum : uint32_t {
  k_ra8_gpt_gtioa_mask       = 0x0000001FUL, /**< RA8 GPT gtioa mask.       */
  k_ra8_gpt_gtioa_shift      = 0U,           /**< RA8 GPT gtioa shift.      */
  k_ra8_gpt_oadflt_mask      = 0x00000040UL, /**< RA8 GPT oadflt mask.      */
  k_ra8_gpt_oadflt_shift     = 6U,           /**< RA8 GPT oadflt shift.     */
  k_ra8_gpt_oae_mask         = 0x00000100UL, /**< RA8 GPT oae mask.         */
  k_ra8_gpt_oadf_mask        = 0x00000600UL, /**< RA8 GPT oadf mask.        */
  k_ra8_gpt_oadf_shift       = 9U,           /**< RA8 GPT oadf shift.       */
  k_ra8_gpt_gtiob_mask       = 0x001F0000UL, /**< RA8 GPT gtiob mask.       */
  k_ra8_gpt_gtiob_shift      = 16U,          /**< RA8 GPT gtiob shift.      */
  k_ra8_gpt_obdflt_mask      = 0x00400000UL, /**< RA8 GPT obdflt mask.      */
  k_ra8_gpt_obdflt_shift     = 22U,          /**< RA8 GPT obdflt shift.     */
  k_ra8_gpt_obe_mask         = 0x01000000UL, /**< RA8 GPT obe mask.         */
  k_ra8_gpt_obdf_mask        = 0x06000000UL, /**< RA8 GPT obdf mask.        */
  k_ra8_gpt_obdf_shift       = 25U,          /**< RA8 GPT obdf shift.       */
  k_ra8_gpt_gtio_active_high = 0x9UL,        /**< RA8 GPT gtio active high. */
  k_ra8_gpt_gtio_active_low  = 0x6UL,        /**< RA8 GPT gtio active low.  */
} ra8_gpt_gtior_bits_t;

/**
 * @enum ra8_gpt_buffer_bits_t
 * @brief GTBER bits and GTDTCR bits used by the runtime PWM API.
 *
 * @details
 * GTBER layout from RA8D2 CMSIS R_GPT0_GTBER (HUM Ch 22.2.17,
 * p 932..935): CCRA at bits 16..17, CCRB at bits 18..19. Setting
 * the lower bit of each pair to 1 selects single-buffer (GTCCRC ->
 * GTCCRA / GTCCRE -> GTCCRB) reload at cycle end.
 *
 * GTDTCR.TDE is bit 0 (HUM Ch 22.2.27 "GTDTCR", p 941..942).
 *
 * The ``compare_c`` / ``compare_e`` indices are GTCCR[2] and
 * GTCCR[3] respectively per HUM Ch 22.2.20.
 */
typedef enum : uint32_t {
  k_ra8_gpt_gtber_ccra_single = 0x00010000UL, /**< RA8 GPT gtber ccra single. */
  k_ra8_gpt_gtber_ccrb_single = 0x00040000UL, /**< RA8 GPT gtber ccrb single. */
  k_ra8_gpt_gtdtcr_tde        = 0x00000001UL, /**< RA8 GPT gtdtcr tde.        */
  k_ra8_gpt_gtcr_cst_mask     = 0x00000001UL, /**< RA8 GPT gtcr cst mask.     */
} ra8_gpt_buffer_bits_t;

/**
 * @enum ra8_gpt_ccr_idx_t
 * @brief Index positions inside GTCCR[6] used by the buffer-based
 * duty cycle setter.
 *
 * @details
 * Mirrors FSP ``GPT_PRV_GTCCRA..F`` from
 * ``fsp/ra/fsp/src/r_gpt/r_gpt.c``. The shadow buffer that feeds
 * GTCCRA on the next reload is GTCCRC; the shadow buffer for
 * GTCCRB is GTCCRE.
 */
typedef enum : uint8_t {
  k_ra8_gpt_ccr_idx_a = 0U, /**< RA8 GPT ccr index a. */
  k_ra8_gpt_ccr_idx_b = 1U, /**< RA8 GPT ccr index b. */
  k_ra8_gpt_ccr_idx_c = 2U, /**< RA8 GPT ccr index c. */
  k_ra8_gpt_ccr_idx_e = 3U, /**< RA8 GPT ccr index e. */
} ra8_gpt_ccr_idx_t;

/**
 * @struct ra8_gpt_state_t
 * @brief Per-channel runtime state (callback, configured flag).
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] period_counts See header declaration for direction and constraints.
 * @return ``ra8_err_t`` error code (or void if the signature returns void).
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 *
 * @param[in] out_counts See header declaration for direction and constraints.
 * @param[in] count See header declaration for direction and constraints.
 * @param[in] on_complete See header declaration for direction and constraints.
 * @param[in] ctx See header declaration for direction and constraints.
 * @param[in] out_dma_channel See header declaration for direction and constraints.
 *
 * @param[in] periods See header declaration for direction and constraints.
 *
 * @param[in] cfg See header declaration for direction and constraints.
 */
typedef struct {
  ra8_gpt_event_fn_t fn;         /**< Registered callback.     */
  void*              ctx;        /**< Callback context.        */
  bool               configured; /**< True after ra8_gpt_init. */
} ra8_gpt_state_t;

static ra8_gpt_state_t s_gpt_state[k_ra8_gpt_channel_count];

/* internal_gtcr -- see header for full description. */
static uint32_t internal_gtcr(ra8_gpt_mode_t mode, ra8_gpt_prescaler_t ps)
{
  return ((uint32_t)mode << k_ra8_gpt_gtcr_md_shift) | ((uint32_t)ps << k_ra8_gpt_gtcr_tpcs_shift);
}

ra8_err_t ra8_gpt_start_free_run(uint8_t channel, uint32_t period)
{
  volatile r_gpt_channel_regs_t* reg = ra8_gpt(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  if (channel >= (uint8_t)k_ra8_gpt_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 22.10.1 "Module-Stop Function Setting" p 1146: GTCLKCR
   * must be programmed BEFORE MSTPCR is cleared, or per-channel
   * writes silently drop. */
  internal_gpt_clock_block_init();
  /* HUM Ch 11.2.10 "MSTPCRE : Module Stop Control Register E", p 449 */
  const ra8_err_t mst_err = ra8_mstp_enable(s_gpt_mstp_table[channel]);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "gpt_start: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  reg->GTWP  = k_ra8_gtwp_key_unlock;
  reg->GTSTP = 1UL;          /* Stop if running.   */
  reg->GTCR  = 0x00000001UL; /* Saw-wave PWM mode. */
  reg->GTPR  = period;
  reg->GTCNT = 0U;
  reg->GTSTR = 1UL; /* Start. */
  reg->GTWP  = k_ra8_gtwp_key_lock;

  ra8_log_info_val(s_tag, "start channel", (uint32_t)channel);
  return k_ra8_ok;
}

ra8_err_t ra8_gpt_stop(uint8_t channel)
{
  volatile r_gpt_channel_regs_t* reg = ra8_gpt(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  reg->GTWP  = k_ra8_gtwp_key_unlock;
  reg->GTSTP = 1UL;
  reg->GTWP  = k_ra8_gtwp_key_lock;
  return k_ra8_ok;
}

ra8_err_t ra8_gpt_read(uint8_t channel, uint32_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  volatile r_gpt_channel_regs_t* reg = ra8_gpt(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  *out = reg->GTCNT;
  return k_ra8_ok;
}

/* =============================================================================
 * full build-out
 * =============================================================================
 */

ra8_err_t ra8_gpt_init(uint8_t channel, const ra8_gpt_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  volatile r_gpt_channel_regs_t* reg = ra8_gpt(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  /* HUM Ch 22.10.1 "Module-Stop Function Setting" p 1146: GTCLKCR
   * must be programmed BEFORE MSTPCR is cleared, or per-channel
   * writes silently drop. */
  internal_gpt_clock_block_init();
  /* HUM Ch 11.2.10 "MSTPCRE : Module Stop Control Register E", p 449 */
  const ra8_err_t mst_err = ra8_mstp_enable(s_gpt_mstp_table[channel]);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "gpt_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  reg->GTWP  = k_ra8_gtwp_key_unlock;
  reg->GTSTP = k_ra8_gpt_gtstp_stop;
  reg->GTCR  = internal_gtcr(cfg->mode, cfg->prescaler);
  reg->GTPR  = cfg->period;
  reg->GTPBR = cfg->period;
  /* GTCCRA / GTCCRB are PWM compare values for GTIOCnA / GTIOCnB
     outputs in saw-wave PWM mode. */
  /* HUM Ch 22.2.20 "GTCCRA..F : General PWM Timer Compare Capture Register" p 938 */
  reg->GTCCR[0] = cfg->duty_a;
  reg->GTCCR[1] = cfg->duty_b;
  reg->GTCNT    = 0U;
  if (cfg->auto_start) {
    reg->GTCR |= k_ra8_gpt_gtcr_cst_set;
    reg->GTSTR = k_ra8_gpt_gtstr_start;
  }
  reg->GTWP = k_ra8_gtwp_key_lock;

  s_gpt_state[channel].configured = true;
  ra8_log_info_val(s_tag, "init channel", (uint32_t)channel);
  return k_ra8_ok;
}

ra8_err_t ra8_gpt_deinit(uint8_t channel)
{
  volatile r_gpt_channel_regs_t* reg = ra8_gpt(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  reg->GTWP  = k_ra8_gtwp_key_unlock;
  reg->GTSTP = k_ra8_gpt_gtstp_stop;
  reg->GTCR  = 0U;
  reg->GTWP  = k_ra8_gtwp_key_lock;

  s_gpt_state[channel].fn         = nullptr;
  s_gpt_state[channel].ctx        = nullptr;
  s_gpt_state[channel].configured = false;
  (void)ra8_mstp_disable(s_gpt_mstp_table[channel]);
  return k_ra8_ok;
}

ra8_err_t ra8_gpt_set_period(uint8_t channel, uint32_t period)
{
  volatile r_gpt_channel_regs_t* reg = ra8_gpt(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  reg->GTWP  = k_ra8_gtwp_key_unlock;
  reg->GTPR  = period;
  reg->GTPBR = period;
  reg->GTWP  = k_ra8_gtwp_key_lock;
  return k_ra8_ok;
}

ra8_err_t ra8_gpt_set_duty(uint8_t channel, ra8_gpt_ccr_sel_t which, uint32_t value)
{
  volatile r_gpt_channel_regs_t* reg = ra8_gpt(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  if ((uint8_t)which > k_ra8_gpt_ccr_b) {
    return k_ra8_err_invalid_arg;
  }

  reg->GTWP                  = k_ra8_gtwp_key_unlock;
  reg->GTCCR[(uint8_t)which] = value;
  reg->GTWP                  = k_ra8_gtwp_key_lock;
  return k_ra8_ok;
}

ra8_err_t ra8_gpt_get_status(uint8_t channel, uint32_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  volatile r_gpt_channel_regs_t* reg = ra8_gpt(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  *out_mask = reg->GTST & k_ra8_gpt_gtst_mask;
  return k_ra8_ok;
}

ra8_err_t ra8_gpt_clear_status(uint8_t channel, uint32_t mask)
{
  volatile r_gpt_channel_regs_t* reg = ra8_gpt(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  /* GTST bits are cleared by writing the current value with target bits zero. */
  const uint32_t current = reg->GTST;
  reg->GTST              = current & ~(mask & k_ra8_gpt_gtst_mask);
  return k_ra8_ok;
}

ra8_err_t ra8_gpt_attach_handler(uint8_t channel, ra8_gpt_event_fn_t fn, void* ctx)
{
  if (channel >= (uint8_t)k_ra8_gpt_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  s_gpt_state[channel].fn  = fn;
  s_gpt_state[channel].ctx = ctx;
  return k_ra8_ok;
}

ra8_err_t ra8_gpt_enter_stop(uint8_t channel)
{
  if (channel >= (uint8_t)k_ra8_gpt_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_gpt_channel_regs_t* reg = ra8_gpt(channel);
  if (reg != nullptr) {
    reg->GTWP  = k_ra8_gtwp_key_unlock;
    reg->GTSTP = k_ra8_gpt_gtstp_stop;
    reg->GTWP  = k_ra8_gtwp_key_lock;
  }
  return ra8_mstp_disable(s_gpt_mstp_table[channel]);
}

ra8_err_t ra8_gpt_exit_stop(uint8_t channel)
{
  if (channel >= (uint8_t)k_ra8_gpt_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  return ra8_mstp_enable(s_gpt_mstp_table[channel]);
}

/* ---- DMA TX / RX ----------------------------------------- */

ra8_err_t ra8_gpt_write_dma(uint8_t               channel,
                            const uint32_t*       periods,
                            uint16_t              count,
                            ra8_dma_complete_fn_t on_complete,
                            void*                 ctx,
                            uint8_t*              out_dma_channel)
{
  RA8_CHECK_NULL_PTR(periods, s_tag, "gpt_write_dma: periods");
  RA8_CHECK_NULL_PTR(out_dma_channel, s_tag, "gpt_write_dma: out_dma_channel");
  if ((channel >= (uint8_t)k_ra8_gpt_channel_count) || (count == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_gpt_channel_regs_t* reg = ra8_gpt(channel);
  if (reg == nullptr) {           /* GCOVR_EXCL_BR_LINE */
    return k_ra8_err_invalid_arg; /* GCOVR_EXCL_LINE    */
  }
  /* Word-wide DMA writes stream period values into GTPR;
     dst_inc=false so every element lands at the same MMIO address. */
  /* HUM Ch 22.2.21 "GTPR : General PWM Timer Cycle Setting Register" p 938 */
  ra8_dma_request_t req = {};
  req.src_addr          = (uintptr_t)periods;
  req.dst_addr          = (uintptr_t)&reg->GTPR;
  req.count             = count;
  req.width             = k_ra8_dmac_width_word;
  req.src_inc           = true;
  req.dst_inc           = false;
  req.trigger           = (ra8_elc_event_t)0;
  req.on_complete       = on_complete;
  req.ctx               = ctx;
  return ra8_dma_request(&req, out_dma_channel);
}

/* out_counts is written by the DMAC engine. */
ra8_err_t ra8_gpt_read_dma(
  uint8_t channel,
  uint32_t*
    out_counts, // NOLINT(readability-non-const-parameter) -- written by the DMAC engine via dst_addr, never through the pointer.
  uint16_t              count,
  ra8_dma_complete_fn_t on_complete,
  void*                 ctx,
  uint8_t*              out_dma_channel)
{
  RA8_CHECK_NULL_PTR(out_counts, s_tag, "gpt_read_dma: out_counts");
  RA8_CHECK_NULL_PTR(out_dma_channel, s_tag, "gpt_read_dma: out_dma_channel");
  if ((channel >= (uint8_t)k_ra8_gpt_channel_count) || (count == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_gpt_channel_regs_t* reg = ra8_gpt(channel);
  if (reg == nullptr) {           /* GCOVR_EXCL_BR_LINE */
    return k_ra8_err_invalid_arg; /* GCOVR_EXCL_LINE    */
  }
  /* Word-wide DMA reads stream GTCNT snapshots into out_counts[]. */
  /* HUM Ch 22.2.19 "GTCNT : General PWM Timer Counter" p 938 */
  ra8_dma_request_t req = {};
  req.src_addr          = (uintptr_t)&reg->GTCNT;
  req.dst_addr          = (uintptr_t)out_counts;
  req.count             = count;
  req.width             = k_ra8_dmac_width_word;
  req.src_inc           = false;
  req.dst_inc           = true;
  req.trigger           = (ra8_elc_event_t)0;
  req.on_complete       = on_complete;
  req.ctx               = ctx;
  return ra8_dma_request(&req, out_dma_channel);
}

/* =============================================================================
 * Input capture & external event counting
 * =============================================================================
 */

ra8_err_t ra8_gpt_capture_configure(uint8_t channel, ra8_gpt_ccr_sel_t which, uint32_t source_mask)
{
  volatile r_gpt_channel_regs_t* reg = ra8_gpt(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  if (((uint8_t)which > (uint8_t)k_ra8_gpt_ccr_b) ||
      ((source_mask & ~(uint32_t)k_ra8_gpt_cap_src_valid_mask) != 0U)) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 22.2.1 "GTWP : General PWM Timer Write Protection Register" p 883-884 */
  reg->GTWP = k_ra8_gtwp_key_unlock;
  if (which == k_ra8_gpt_ccr_a) {
    /* HUM Ch 22.2.10 "GTICASR : General PWM Timer Input Capture Source Select Register A" p 906-908
     */
    reg->GTICASR = source_mask;
  } else {
    /* HUM Ch 22.2.11 "GTICBSR : General PWM Timer Input Capture Source Select Register B" p 909-912
     */
    reg->GTICBSR = source_mask;
  }
  /* HUM Ch 22.2.1 "GTWP : General PWM Timer Write Protection Register" p 883-884 */
  reg->GTWP = k_ra8_gtwp_key_lock;
  return k_ra8_ok;
}

ra8_err_t ra8_gpt_capture_read(uint8_t channel, ra8_gpt_ccr_sel_t which, uint32_t* out_value)
{
  RA8_CHECK_NULL_PTR(out_value, s_tag, "out_value must not be nullptr");
  volatile r_gpt_channel_regs_t* reg = ra8_gpt(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  if ((uint8_t)which > (uint8_t)k_ra8_gpt_ccr_b) {
    return k_ra8_err_invalid_arg;
  }

  /* GTCCRA (index A) and GTCCRB (index B) hold the GTCNT value latched on the
     last capture edge while GTICASR / GTICBSR is armed. */
  /* HUM Ch 22.2.20 "GTCCRk : General PWM Timer Compare Capture Register k (k = A to F)" p 938 */
  *out_value = reg->GTCCR[(uint8_t)which];
  return k_ra8_ok;
}

ra8_err_t ra8_gpt_event_count_configure(uint8_t channel, uint32_t up_source, uint32_t down_source)
{
  volatile r_gpt_channel_regs_t* reg = ra8_gpt(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  if (((up_source & ~(uint32_t)k_ra8_gpt_cnt_src_valid_mask) != 0U) ||
      ((down_source & ~(uint32_t)k_ra8_gpt_cnt_src_valid_mask) != 0U)) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 22.2.1 "GTWP : General PWM Timer Write Protection Register" p 883-884 */
  reg->GTWP = k_ra8_gtwp_key_unlock;
  /* HUM Ch 22.2.8 "GTUPSR : General PWM Timer Up Count Source Select Register" p 898-901 */
  reg->GTUPSR = up_source;
  /* HUM Ch 22.2.9 "GTDNSR : General PWM Timer Down Count Source Select Register" p 902-905 */
  reg->GTDNSR = down_source;
  /* HUM Ch 22.2.1 "GTWP : General PWM Timer Write Protection Register" p 883-884 */
  reg->GTWP = k_ra8_gtwp_key_lock;
  return k_ra8_ok;
}

/* =============================================================================
 * Sweep 3 Task 2 -- runtime PWM duty / period / counter / dead-time / 3-phase
 * Mirrors FSP r_gpt + r_gpt_three_phase.
 * =============================================================================
 */

ra8_err_t ra8_gpt_period_set(uint8_t channel, uint32_t period_counts)
{
  volatile r_gpt_channel_regs_t* reg = ra8_gpt(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  reg->GTWP  = k_ra8_gtwp_key_unlock;
  reg->GTPBR = period_counts;
  /* Per FSP R_GPT_PeriodSet: if the counter is currently stopped,
     the new period is also written into the live register so the
     next start has the correct GTPR. */
  if ((reg->GTCR & k_ra8_gpt_gtcr_cst_mask) == 0U) {
    reg->GTPR  = period_counts;
    reg->GTCNT = 0U;
  }
  reg->GTWP = k_ra8_gtwp_key_lock;
  return k_ra8_ok;
}

ra8_err_t ra8_gpt_duty_cycle_set(uint8_t channel, ra8_gpt_pwm_pin_t pin, uint32_t compare_counts)
{
  if (pin > k_ra8_gpt_pin_b) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_gpt_channel_regs_t* reg = ra8_gpt(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  /* HUM Ch 22.2.20 "GTCCRA..F" p 938 */ /* + Ch 22.2.17 "GTBER" p 932:
     write compare into GTCCRC (shadow for A) or GTCCRE (shadow for
     B), then assert the matching GTBER buffer-enable bit so the
     next cycle-end reloads GTCCRA / GTCCRB. */
  reg->GTWP = k_ra8_gtwp_key_unlock;
  if (pin == k_ra8_gpt_pin_a) {
    reg->GTCCR[k_ra8_gpt_ccr_idx_c] = compare_counts;
    reg->GTBER |= k_ra8_gpt_gtber_ccra_single;
  } else {
    reg->GTCCR[k_ra8_gpt_ccr_idx_e] = compare_counts;
    reg->GTBER |= k_ra8_gpt_gtber_ccrb_single;
  }
  reg->GTWP = k_ra8_gtwp_key_lock;
  return k_ra8_ok;
}

ra8_err_t ra8_gpt_counter_set(uint8_t channel, uint32_t value)
{
  volatile r_gpt_channel_regs_t* reg = ra8_gpt(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  /* FSP R_GPT_CounterSet rejects writes while the timer is running. */
  if ((reg->GTCR & k_ra8_gpt_gtcr_cst_mask) != 0U) {
    return k_ra8_err_invalid_state;
  }

  reg->GTWP  = k_ra8_gtwp_key_unlock;
  reg->GTCNT = value;
  reg->GTWP  = k_ra8_gtwp_key_lock;
  return k_ra8_ok;
}

/**
 * @brief Build the GTIO[A/B] sub-field encoding for the requested polarity.
 * @param[in] polarity Active-high or active-low.
 * @return 5-bit GTIO field value (0x9 active-high, 0x6 active-low).
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static uint32_t internal_gtio_pattern(ra8_gpt_pwm_polarity_t polarity)
{
  if (polarity == k_ra8_gpt_pol_active_low) {
    return k_ra8_gpt_gtio_active_low;
  }
  return k_ra8_gpt_gtio_active_high;
}

ra8_err_t
ra8_gpt_pwm_pin_configure(uint8_t channel, ra8_gpt_pwm_pin_t pin, const ra8_gpt_pwm_pin_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  if (pin > k_ra8_gpt_pin_b) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_gpt_channel_regs_t* reg = ra8_gpt(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  const uint32_t pattern = internal_gtio_pattern(cfg->polarity);
  reg->GTWP              = k_ra8_gtwp_key_unlock;
  uint32_t gtior         = reg->GTIOR;

  if (pin == k_ra8_gpt_pin_a) {
    /* Clear and re-pack the A-side fields only -- preserve B-side. */
    gtior &=
      ~(k_ra8_gpt_gtioa_mask | k_ra8_gpt_oadflt_mask | k_ra8_gpt_oae_mask | k_ra8_gpt_oadf_mask);
    gtior |= (pattern & k_ra8_gpt_gtioa_mask) << k_ra8_gpt_gtioa_shift;
    gtior |= ((uint32_t)cfg->stop_level << k_ra8_gpt_oadflt_shift) & k_ra8_gpt_oadflt_mask;
    if (cfg->output_enable) {
      gtior |= k_ra8_gpt_oae_mask;
    }
    gtior |= ((uint32_t)cfg->disable_on_fault << k_ra8_gpt_oadf_shift) & k_ra8_gpt_oadf_mask;
  } else {
    gtior &=
      ~(k_ra8_gpt_gtiob_mask | k_ra8_gpt_obdflt_mask | k_ra8_gpt_obe_mask | k_ra8_gpt_obdf_mask);
    gtior |= (pattern & (k_ra8_gpt_gtiob_mask >> k_ra8_gpt_gtiob_shift)) << k_ra8_gpt_gtiob_shift;
    gtior |= ((uint32_t)cfg->stop_level << k_ra8_gpt_obdflt_shift) & k_ra8_gpt_obdflt_mask;
    if (cfg->output_enable) {
      gtior |= k_ra8_gpt_obe_mask;
    }
    gtior |= ((uint32_t)cfg->disable_on_fault << k_ra8_gpt_obdf_shift) & k_ra8_gpt_obdf_mask;
  }
  reg->GTIOR = gtior;
  reg->GTWP  = k_ra8_gtwp_key_lock;
  return k_ra8_ok;
}

ra8_err_t ra8_gpt_dead_time_set(uint8_t channel, uint32_t rising_dt, uint32_t falling_dt)
{
  volatile r_gpt_channel_regs_t* reg = ra8_gpt(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  reg->GTWP   = k_ra8_gtwp_key_unlock;
  reg->GTDVU  = rising_dt;
  reg->GTDVD  = falling_dt;
  reg->GTDTCR = ((rising_dt != 0U) || (falling_dt != 0U)) ? k_ra8_gpt_gtdtcr_tde : 0U;
  reg->GTWP   = k_ra8_gtwp_key_lock;
  return k_ra8_ok;
}

/* ---- Three-phase synchronized PWM ----------------------------------- */

/**
 * @struct ra8_gpt_three_phase_state_t
 * @brief Driver-internal three-phase tracking state.
 *
 * @details
 * Holds the U/V/W channel ids recorded at ``ra8_gpt_three_phase_open``
 * time so that ``set_duty`` and ``close`` know which channels to
 * operate on without re-reading caller config.
 */
typedef struct {
  uint8_t  channels[k_ra8_gpt_three_phase_count]; /**< Channel ids.        */
  uint32_t channel_mask;                          /**< OR of (1<<ch) bits. */
  bool     open;                                  /**< Open flag.          */
} ra8_gpt_three_phase_state_t;

static ra8_gpt_three_phase_state_t s_three_phase;

/**
 * @brief Bring up the three GPT submodules and build the start mask.
 * @param[in] cfg Three-phase configuration descriptor.
 * @param[out] out_mask Combined channel-bit mask on success.
 * @return ``k_ra8_ok`` or the first failing channel's error code.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 *
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 */
static ra8_err_t internal_three_phase_init_subs(const ra8_gpt_three_phase_cfg_t* cfg,
                                                uint32_t*                        out_mask)
{
  const uint32_t initial_duties[k_ra8_gpt_three_phase_count] = {
    cfg->initial_duty_u,
    cfg->initial_duty_v,
    cfg->initial_duty_w,
  };
  uint32_t mask = 0U;
  for (uint8_t i = 0U; i < k_ra8_gpt_three_phase_count; ++i) {
    const ra8_gpt_cfg_t per_ch_cfg = {
      .mode       = cfg->mode,
      .prescaler  = cfg->prescaler,
      .period     = cfg->period_counts,
      .duty_a     = initial_duties[i],
      .duty_b     = initial_duties[i],
      .auto_start = false,
    };
    const ra8_err_t err = ra8_gpt_init(cfg->channels[i], &per_ch_cfg);
    if (err != k_ra8_ok) {                      /* GCOVR_EXCL_BR_LINE */
      for (uint8_t j = 0U; j < i; ++j) {        /* GCOVR_EXCL_LINE    */
        (void)ra8_gpt_deinit(cfg->channels[j]); /* GCOVR_EXCL_LINE    */
      } /* GCOVR_EXCL_LINE */
      return err; /* GCOVR_EXCL_LINE */
    }
    mask |= (uint32_t)(1UL << cfg->channels[i]);
    s_three_phase.channels[i] = cfg->channels[i];
  }
  *out_mask = mask;
  return k_ra8_ok;
}

ra8_err_t ra8_gpt_three_phase_open(const ra8_gpt_three_phase_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "three_phase cfg must not be nullptr");
  if (s_three_phase.open) {
    return k_ra8_err_invalid_state;
  }
  for (uint8_t i = 0U; i < k_ra8_gpt_three_phase_count; ++i) {
    if (cfg->channels[i] >= (uint8_t)k_ra8_gpt_channel_count) {
      return k_ra8_err_invalid_arg;
    }
  }

  uint32_t        mask = 0U;
  const ra8_err_t err  = internal_three_phase_init_subs(cfg, &mask);
  if (err != k_ra8_ok) { /* GCOVR_EXCL_BR_LINE */
    return err;          /* GCOVR_EXCL_LINE    */
  }

  /* Synchronous start: a single GTSTR write to the U-channel slot
     with all three CSTRTn bits set kicks all three counters on the
     same PCLKD edge. HUM Ch 22.2.2 "GTSTR" p 886. */
  volatile r_gpt_channel_regs_t* u_reg = ra8_gpt(cfg->channels[0]);
  if (u_reg != nullptr) {
    u_reg->GTWP  = k_ra8_gtwp_key_unlock;
    u_reg->GTSTR = mask;
    u_reg->GTWP  = k_ra8_gtwp_key_lock;
  }

  s_three_phase.channel_mask = mask;
  s_three_phase.open         = true;
  ra8_log_info_val(s_tag, "three_phase open mask", mask);
  return k_ra8_ok;
}

ra8_err_t ra8_gpt_three_phase_set_duty(uint32_t u_duty, uint32_t v_duty, uint32_t w_duty)
{
  if (!s_three_phase.open) {
    return k_ra8_err_invalid_state;
  }
  const uint32_t duties[k_ra8_gpt_three_phase_count] = {u_duty, v_duty, w_duty};

  /* Range check against the shared GTPR (read once from U). */
  volatile r_gpt_channel_regs_t* u_reg = ra8_gpt(s_three_phase.channels[0]);
  if (u_reg == nullptr) {           /* GCOVR_EXCL_BR_LINE */
    return k_ra8_err_invalid_state; /* GCOVR_EXCL_LINE    */
  }
  const uint32_t period = u_reg->GTPR;
  for (uint8_t i = 0U; i < k_ra8_gpt_three_phase_count; ++i) {
    if (duties[i] > period) {
      return k_ra8_err_invalid_arg;
    }
  }

  for (uint8_t i = 0U; i < k_ra8_gpt_three_phase_count; ++i) {
    volatile r_gpt_channel_regs_t* reg = ra8_gpt(s_three_phase.channels[i]);
    if (reg == nullptr) {             /* GCOVR_EXCL_BR_LINE */
      return k_ra8_err_invalid_state; /* GCOVR_EXCL_LINE    */
    }
    reg->GTWP                       = k_ra8_gtwp_key_unlock;
    reg->GTCCR[k_ra8_gpt_ccr_idx_c] = duties[i];
    reg->GTCCR[k_ra8_gpt_ccr_idx_e] = duties[i];
    reg->GTBER |= k_ra8_gpt_gtber_ccra_single | k_ra8_gpt_gtber_ccrb_single;
    reg->GTWP = k_ra8_gtwp_key_lock;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_gpt_three_phase_close(void)
{
  if (!s_three_phase.open) {
    return k_ra8_err_invalid_state;
  }
  /* Synchronous stop: single GTSTP write covers all three channels. */
  volatile r_gpt_channel_regs_t* u_reg = ra8_gpt(s_three_phase.channels[0]);
  if (u_reg != nullptr) {
    u_reg->GTWP  = k_ra8_gtwp_key_unlock;
    u_reg->GTSTP = s_three_phase.channel_mask;
    u_reg->GTWP  = k_ra8_gtwp_key_lock;
  }
  for (uint8_t i = 0U; i < k_ra8_gpt_three_phase_count; ++i) {
    (void)ra8_gpt_deinit(s_three_phase.channels[i]);
  }
  s_three_phase.channel_mask = 0U;
  s_three_phase.open         = false;
  return k_ra8_ok;
}

/* ---- ISR dispatch ---------------------------------------------------- */

/* internal_dispatch -- see header for full description. */
static void internal_dispatch(uint8_t channel, uint32_t status_mask)
{
  if (channel >= (uint8_t)k_ra8_gpt_channel_count) {
    return;
  }
  volatile r_gpt_channel_regs_t* reg = ra8_gpt(channel);
  if (reg != nullptr) {
    const uint32_t current = reg->GTST;
    reg->GTST              = current & ~status_mask;
  }
  const ra8_gpt_event_fn_t fn  = s_gpt_state[channel].fn;
  void* const              ctx = s_gpt_state[channel].ctx;
  if (fn != nullptr) {
    fn(ctx, status_mask);
  }
}

RA8_ISR_SAFE
void ra8_gpt_dispatch_ovf(uint8_t channel)
{
  internal_dispatch(channel, k_ra8_gpt_status_overflow);
}

RA8_ISR_SAFE
void ra8_gpt_dispatch_und(uint8_t channel)
{
  internal_dispatch(channel, k_ra8_gpt_status_underflow);
}

RA8_ISR_SAFE
void ra8_gpt_dispatch_ccra(uint8_t channel)
{
  internal_dispatch(channel, k_ra8_gpt_status_ccra);
}

RA8_ISR_SAFE
void ra8_gpt_dispatch_ccrb(uint8_t channel)
{
  internal_dispatch(channel, k_ra8_gpt_status_ccrb);
}
