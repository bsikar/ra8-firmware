/**
 * @file ra_sci.c
 * @brief Full-featured SCI driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Wave 3.1 replacement for the Wave 0 ``uart.c`` stub. See
 * ``ra_sci.h`` for the public API contract.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_sci.h"

#include <stdint.h>

#include "ra8d2_mstp_regs.h"
#include "ra8d2_sci_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_hw_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "SCI";

/* =============================================================================
 * Local constants
 * =============================================================================
 */

/**
 * @enum ra_sci_limits_inner_t
 * @brief File-local bounds + magic register values.
 */
typedef enum : uint8_t {
  k_ra_sci_channel_max_index = 9U,    /**< SCI0..SCI9.                  */
  k_ra_sci_channel_count_val = 10U,   /**< Total channels tracked.      */
  k_ra_sci_scmr_default      = 0xF2U, /**< SCMR reset value per HUM.    */
} ra_sci_limits_inner_t;

/**
 * @enum ra_sci_smr_bit_t
 * @brief SMR bit positions (HUM Ch 38.2 "SMR Serial Mode Register", p 2174).
 */
typedef enum : uint8_t {
  k_ra_sci_smr_bit_stop = 3U, /**< STOP bit = 1 means 2 stop bits. */
  k_ra_sci_smr_bit_pm   = 4U, /**< PM  = 1 means odd parity.       */
  k_ra_sci_smr_bit_pe   = 5U, /**< PE  = 1 means parity enabled.   */
  k_ra_sci_smr_bit_chr  = 6U, /**< CHR = 1 means 7-bit data.       */
} ra_sci_smr_bit_t;

/**
 * @enum ra_sci_brr_const_t
 * @brief Constants used in the BRR calculation (HUM Ch 38.2).
 */
typedef enum : uint32_t {
  k_ra_sci_brr_base = 64U, /**< 64x base-clock divider.          */
} ra_sci_brr_const_t;

/* =============================================================================
 * Per-channel state
 * =============================================================================
 */

/**
 * @struct ra_sci_state_t
 * @brief Per-channel dispatch state.
 */
typedef struct {
  ra_sci_rx_fn_t rx_fn;       /**< Attached RX handler, NULL if none. */
  void*          rx_ctx;      /**< RX handler context.                */
  ra_sci_tx_fn_t tx_fn;       /**< Attached TX handler, NULL if none. */
  void*          tx_ctx;      /**< TX handler context.                */
  bool           initialised; /**< True after ra_sci_init.       */
} ra_sci_state_t;

/**
 * @var s_state
 * @brief Per-channel allocation + dispatch table.
 */
static ra_sci_state_t s_state[k_ra_sci_channel_count_val];

/**
 * @var s_mstp_table
 * @brief Channel-index -> MSTP id lookup.
 */
static const ra_mstp_t s_mstp_table[k_ra_sci_channel_count_val] = {
  k_ra_mstp_sci0,
  k_ra_mstp_sci1,
  k_ra_mstp_sci2,
  k_ra_mstp_sci3,
  k_ra_mstp_sci4,
  k_ra_mstp_sci5,
  k_ra_mstp_sci6,
  k_ra_mstp_sci7,
  k_ra_mstp_sci8,
  k_ra_mstp_sci9,
};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Validate ``channel`` and return the register pointer.
 */
static volatile r_sci_regs_t* internal_reg(uint8_t channel)
{
  if (channel > (uint8_t)k_ra_sci_channel_max_index) {
    return nullptr;
  }
  return ra_sci(channel);
}

/**
 * @brief Compute BRR from the requested baud and PCLKB.
 *
 * @details
 * HUM Ch 38 gives the formula:
 *
 * @f[
 *    BRR = \frac{PCLKB}{64 \cdot B} - 1
 * @f]
 *
 * for the default CKS = 0, ABCSE = 0, BGDM = 0 path.
 */
static uint8_t internal_brr(uint32_t pclk_hz, uint32_t baud)
{
  if ((baud == 0U) || (pclk_hz == 0U)) {
    return 0U;
  }
  const uint32_t divisor = (uint32_t)k_ra_sci_brr_base * baud;
  const uint32_t n       = (pclk_hz / divisor);
  if (n == 0U) {
    return 0U;
  }
  return (uint8_t)(n - 1U);
}

/**
 * @brief Compute the SMR value from a config descriptor.
 *
 * @details
 * Bit map (from HUM Ch 38.2 "SMR"):
 *   [1:0] CKS  -- clock source select (always PCLKB in this driver)
 *   [2]   MP   -- multi-processor (0 = disabled)
 *   [3]   STOP -- 0 = 1 stop bit, 1 = 2 stop bits
 *   [4]   PM   -- 0 = even, 1 = odd
 *   [5]   PE   -- 0 = no parity, 1 = parity
 *   [6]   CHR  -- 0 = 8-bit, 1 = 7-bit
 *   [7]   CM   -- 0 = async, 1 = clock-sync
 */
static uint8_t internal_smr(const ra_sci_cfg_t* cfg)
{
  uint8_t smr = 0U;
  if (cfg->stop_bits == k_ra_sci_stop_2) {
    smr |= (uint8_t)(1U << (uint8_t)k_ra_sci_smr_bit_stop);
  }
  if (cfg->parity != k_ra_sci_parity_none) {
    smr |= (uint8_t)(1U << (uint8_t)k_ra_sci_smr_bit_pe);
    if (cfg->parity == k_ra_sci_parity_odd) {
      smr |= (uint8_t)(1U << (uint8_t)k_ra_sci_smr_bit_pm);
    }
  }
  if (cfg->data_bits == k_ra_sci_data_7) {
    smr |= (uint8_t)(1U << (uint8_t)k_ra_sci_smr_bit_chr);
  }
  return smr;
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra_err_t ra_sci_init(uint8_t channel, const ra_sci_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "sci_init: cfg");
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B", p 445 */
  const ra_err_t mst_err = ra_mstp_enable(s_mstp_table[channel]);
  if (mst_err != k_ra_ok) {
    ra_log_error_val(s_tag, "sci_init: mstp enable failed", (uint32_t)mst_err);
    return k_ra_err_hw_init_failed;
  }

  /* HUM Ch 38.2 "SCR : Serial Control Register", p 2174 -- disable TX/RX
   * before reprogramming the other control registers. */
  reg->SCR = 0U;

  /* HUM Ch 38.2 "SMR : Serial Mode Register", p 2174 */
  reg->SMR = internal_smr(cfg);

  /* HUM Ch 38.2 "SCMR : Smart Card Mode Register", p 2174 -- factory
   * default per HUM Table 38.x (SINV = 0, SMIF = 0, SDIR = 0). */
  reg->SCMR = (uint8_t)k_ra_sci_scmr_default;

  /* HUM Ch 38.2 "BRR : Bit Rate Register", p 2174 */
  reg->BRR = internal_brr(cfg->pclk_hz, cfg->baud);

  /* HUM Ch 38.2 "SEMR : Serial Extended Mode Register", p 2174 -- async,
   * base-clock = PCLKB, no noise-cancel. */
  reg->SEMR = 0U;

  /* HUM Ch 38.2 "SNFR : Noise Filter Setting Register", p 2174 */
  reg->SNFR = 0U;

  /* HUM Ch 38.2 "SCR : Serial Control Register", p 2174 -- enable TE + RE. */
  reg->SCR = (uint8_t)((1U << (uint8_t)k_ra_scr_bit_te) | (1U << (uint8_t)k_ra_scr_bit_re));

  s_state[channel].initialised = true;
  ra_log_info_val(s_tag, "sci_init channel", (uint32_t)channel);
  return k_ra_ok;
}

ra_err_t ra_sci_deinit(uint8_t channel)
{
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 38.2 "SCR : Serial Control Register", p 2174 */
  reg->SCR                     = 0U;
  s_state[channel].rx_fn       = nullptr;
  s_state[channel].rx_ctx      = nullptr;
  s_state[channel].tx_fn       = nullptr;
  s_state[channel].tx_ctx      = nullptr;
  s_state[channel].initialised = false;
  return ra_mstp_disable(s_mstp_table[channel]);
}

/* ---- Polling TX / RX -------------------------------------------------- */

ra_err_t ra_sci_putc_polling(uint8_t channel, uint8_t byte)
{
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  const ra_err_t werr = ra_hw_wait_flag_set8(&reg->SSR,
                                             (uint8_t)(1U << (uint8_t)k_ra_ssr_bit_tdre),
                                             (uint32_t)k_ra_hw_budget_medium);
  if (werr != k_ra_ok) {
    return werr;
  }
  /* HUM Ch 38.2 "TDR : Transmit Data Register", p 2174 */
  reg->TDR = byte;
  return k_ra_ok;
}

ra_err_t ra_sci_getc_polling(uint8_t channel, uint8_t* out_byte)
{
  RA_CHECK_NULL_PTR(out_byte, s_tag, "getc: out_byte");
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  const ra_err_t werr = ra_hw_wait_flag_set8(&reg->SSR,
                                             (uint8_t)(1U << (uint8_t)k_ra_ssr_bit_rdrf),
                                             (uint32_t)k_ra_hw_budget_medium);
  if (werr != k_ra_ok) {
    return werr;
  }
  /* HUM Ch 38.2 "RDR : Receive Data Register", p 2174 */
  *out_byte = reg->RDR;
  return k_ra_ok;
}

ra_err_t ra_sci_write_polling(uint8_t channel, const uint8_t* data, uint32_t len)
{
  if ((data == nullptr) && (len != 0U)) {
    return k_ra_err_null_ptr;
  }
  for (uint32_t i = 0U; i < len; ++i) {
    const ra_err_t err = ra_sci_putc_polling(channel, data[i]);
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}

/* ---- Interrupt handler attach ---------------------------------------- */

ra_err_t ra_sci_attach_rx_handler(uint8_t channel, ra_sci_rx_fn_t fn, void* ctx)
{
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  s_state[channel].rx_fn  = fn;
  s_state[channel].rx_ctx = ctx;
  /* HUM Ch 38.2 "SCR : Serial Control Register", p 2174 -- toggle RIE. */
  if (fn != nullptr) {
    reg->SCR = (uint8_t)(reg->SCR | (uint8_t)(1U << (uint8_t)k_ra_scr_bit_rie));
  } else {
    reg->SCR = (uint8_t)(reg->SCR & (uint8_t)~(1U << (uint8_t)k_ra_scr_bit_rie));
  }
  return k_ra_ok;
}

ra_err_t ra_sci_attach_tx_handler(uint8_t channel, ra_sci_tx_fn_t fn, void* ctx)
{
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  s_state[channel].tx_fn  = fn;
  s_state[channel].tx_ctx = ctx;
  /* HUM Ch 38.2 "SCR : Serial Control Register", p 2174 -- toggle TIE. */
  if (fn != nullptr) {
    reg->SCR = (uint8_t)(reg->SCR | (uint8_t)(1U << (uint8_t)k_ra_scr_bit_tie));
  } else {
    reg->SCR = (uint8_t)(reg->SCR & (uint8_t)~(1U << (uint8_t)k_ra_scr_bit_tie));
  }
  return k_ra_ok;
}

/* ---- Error status ----------------------------------------------------- */

ra_err_t ra_sci_get_errors(uint8_t channel, uint8_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "get_errors: out");
  volatile const r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  uint8_t       mask = (uint8_t)k_ra_sci_err_none;
  const uint8_t ssr  = reg->SSR;
  if ((ssr & (uint8_t)(1U << (uint8_t)k_ra_ssr_bit_orer)) != 0U) {
    mask |= (uint8_t)k_ra_sci_err_overrun;
  }
  if ((ssr & (uint8_t)(1U << (uint8_t)k_ra_ssr_bit_fer)) != 0U) {
    mask |= (uint8_t)k_ra_sci_err_framing;
  }
  if ((ssr & (uint8_t)(1U << (uint8_t)k_ra_ssr_bit_per)) != 0U) {
    mask |= (uint8_t)k_ra_sci_err_parity;
  }
  *out_mask = mask;
  return k_ra_ok;
}

ra_err_t ra_sci_clear_errors(uint8_t channel)
{
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 38.2 "SSR : Serial Status Register", p 2174 -- ORER/FER/PER
   * are write-zero-to-clear. */
  const uint8_t clr_mask =
    (uint8_t)~((1U << (uint8_t)k_ra_ssr_bit_orer) | (1U << (uint8_t)k_ra_ssr_bit_fer) |
               (1U << (uint8_t)k_ra_ssr_bit_per));
  reg->SSR = (uint8_t)(reg->SSR & clr_mask);
  return k_ra_ok;
}

/* ---- Runtime reconfigure --------------------------------------------- */

ra_err_t ra_sci_set_baud(uint8_t channel, uint32_t baud, uint32_t pclk_hz)
{
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if (baud == 0U) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 38.2 "BRR : Bit Rate Register", p 2174 -- writes take effect
   * on the next bit cell, no full-restart required. */
  reg->BRR = internal_brr(pclk_hz, baud);
  return k_ra_ok;
}

/* ---- Power transition ------------------------------------------------- */

ra_err_t ra_sci_enter_stop(uint8_t channel)
{
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 38.2 "SCR : Serial Control Register", p 2174 */
  reg->SCR = 0U;
  return ra_mstp_disable(s_mstp_table[channel]);
}

ra_err_t ra_sci_exit_stop(uint8_t channel)
{
  if (channel > (uint8_t)k_ra_sci_channel_max_index) {
    return k_ra_err_invalid_arg;
  }
  return ra_mstp_enable(s_mstp_table[channel]);
}

/* ---- ISR dispatch ----------------------------------------------------- */

void ra_sci_dispatch_txi(uint8_t channel)
{
  if (channel > (uint8_t)k_ra_sci_channel_max_index) {
    return;
  }
  volatile r_sci_regs_t* reg = ra_sci(channel);
  if (reg == nullptr) { /* GCOVR_EXCL_BR_LINE -- bounds already validated */
    return;
  }
  const ra_sci_tx_fn_t cb  = s_state[channel].tx_fn;
  void* const          ctx = s_state[channel].tx_ctx;
  if (cb == nullptr) {
    reg->SCR = (uint8_t)(reg->SCR & (uint8_t)~(1U << (uint8_t)k_ra_scr_bit_tie));
    return;
  }
  uint8_t byte = 0U;
  if (cb(ctx, &byte)) {
    reg->TDR = byte;
  } else {
    reg->SCR = (uint8_t)(reg->SCR & (uint8_t)~(1U << (uint8_t)k_ra_scr_bit_tie));
  }
}

void ra_sci_dispatch_rxi(uint8_t channel)
{
  if (channel > (uint8_t)k_ra_sci_channel_max_index) {
    return;
  }
  volatile r_sci_regs_t* reg = ra_sci(channel);
  if (reg == nullptr) { /* GCOVR_EXCL_BR_LINE -- bounds already validated */
    return;
  }
  const ra_sci_rx_fn_t cb  = s_state[channel].rx_fn;
  void* const          ctx = s_state[channel].rx_ctx;
  const uint8_t        b   = reg->RDR;
  if (cb != nullptr) {
    cb(ctx, b);
  }
}

void ra_sci_dispatch_eri(uint8_t channel)
{
  if (channel > (uint8_t)k_ra_sci_channel_max_index) {
    return;
  }
  (void)ra_sci_clear_errors(channel);
}
