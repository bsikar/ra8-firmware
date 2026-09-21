/**
 * @file ra8_ssie.c
 * @brief Serial Sound Interface Enhanced (SSIE / I2S) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Full-coverage implementation of HUM Ch 46 "Serial Sound Interface
 * Enhanced (SSIE)" pages 3051-3121. Every documented register field
 * is reachable, every operating mode (I2S, left/right justified,
 * monaural, TDM 4/6/8) is selectable through the ``ra8_ssie_format_t``
 * enum, every IRQ source (TXI/RXI/IDLE/error) is dispatched through
 * a unified callback, and DMAC channels are pumped through a thin
 * adapter over ``ra8_dmac``.
 *
 * Every register access carries a HUM Ch 46 citation against
 * ``r01uh1065ej0130-ra8d2.pdf``. FSP ``r_ssi.c`` was used as a
 * reference for the start/stop sequence and SSIFCR poll loop only;
 * no source code was copied from the FSP tree.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_ssie.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_dmac.h"
#include "ra8_err.h"
#include "ra8_hal_internal.h"
#include "ra8_hw_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"
#include "ra8_ssie_regs.h"

/**
 * @var s_tag
 * @brief Logger tag for ra8_log_* messages produced by this driver.
 *
 * @note Module-private; never modified at runtime.
 */
static const char* s_tag = "SSIE";

/**
 * @var s_ssie_mstp_table
 * @brief Channel-index -> MSTP id lookup.
 *
 * @details
 * SSIE0 sits on MSTPC8 and SSIE1 on MSTPC7 per HUM Ch 11.2.8
 * "MSTPCRC : Module Stop Control Register C", p 446. The two
 * enum values were already declared in ``ra8_mstp_regs.h``.
 */
static const ra8_mstp_t s_ssie_mstp_table[k_ra8_ssie_channel_count] = {
  k_ra8_mstp_ssie0,
  k_ra8_mstp_ssie1,
};

/**
 * @var g_ssie_runtime
 * @brief Per-channel runtime state. Initialized to zero / unused.
 *
 * @details
 * Single definition for the ra8_ssie split; ``ra8_ssie_stream.c`` shares this
 * object via the ``extern`` declaration in ``ra8_hal_internal.h``. External
 * linkage (no ``static``) is required so both translation units bind the same
 * storage. The ``s_ssie_`` prefix keeps it clang-tidy-clean and link-unique.
 */
ra8_ssie_runtime_t g_ssie_runtime[k_ra8_ssie_channel_count] = {
  {.tx_dma_channel = k_ra8_ssie_dma_ch_unused,
   .rx_dma_channel = k_ra8_ssie_dma_ch_unused,
   .initialized    = false,
   .dma_attached   = false},
  {.tx_dma_channel = k_ra8_ssie_dma_ch_unused,
   .rx_dma_channel = k_ra8_ssie_dma_ch_unused,
   .initialized    = false,
   .dma_attached   = false},
};

/**
 * @var s_ssie_fn
 * @brief Globally registered SSIE event callback (one slot, shared).
 *
 * @note Modified only via ``ra8_ssie_attach_handler``.
 */
static ra8_ssie_event_fn_t s_ssie_fn;

/**
 * @var s_ssie_ctx
 * @brief Opaque context delivered to ``s_ssie_fn`` on dispatch.
 *
 * @note Modified only via ``ra8_ssie_attach_handler``.
 */
static void* s_ssie_ctx;

/** @brief Implementation of `priv_ra8_ssie_internal_regs()` -- bounds-check + accessor. */
volatile r_ssie_regs_t* priv_ra8_ssie_internal_regs(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra8_ssie_channel_count) {
    return nullptr;
  }
  return ra8_ssie(channel);
}

/**
 * @brief Translate a public format enum to OMOD + FRM register fields.
 *
 * @param[in]  format    Public format enum.
 * @param[out] out_omod  Receives SSIOFR.OMOD encoding (0..2).
 * @param[out] out_frm   Receives SSICR.FRM encoding (0..3).
 * @param[out] out_pdta  Receives PDTA polarity (right justify uses 1).
 * @param[out] out_sdta  Receives SDTA polarity (left justify uses 0).
 *
 * @return true on a recognised enum value, false otherwise.
 *
 * @details See HUM Ch 46.2.1 FRM table p 3057 and HUM Ch 46.2.7
 * OMOD table p 3091.
 *
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_format_decode(ra8_ssie_format_t format,
                                   uint8_t*          out_omod,
                                   uint8_t*          out_frm,
                                   uint8_t*          out_pdta,
                                   uint8_t*          out_sdta)
{
  /* Defaults map to I2S (OMOD=00, FRM=00, PDTA=0, SDTA=0). */
  *out_omod = k_ra8_ssie_omod_i2s;
  *out_frm  = k_ra8_ssie_frm_default;
  *out_pdta = 0U;
  *out_sdta = 0U;

  switch (format) {
    case k_ra8_ssie_format_i2s:
    case k_ra8_ssie_format_left_just:
      /* I2S and left-justified both map to OMOD=I2S with SDTA=0
       * (already default). Shown in HUM Ch 46.3.1 "Stereo Format"
       * Figures, p 3094-3096. */
      return true;
    case k_ra8_ssie_format_right_just:
      /* Right-justified maps to OMOD=I2S with PDTA=1 (right-justify
       * placement) per HUM Ch 46.2.1 "PDTA" p 3056. */
      *out_pdta = 1U;
      return true;
    case k_ra8_ssie_format_monaural:
      *out_omod = k_ra8_ssie_omod_monaural;
      *out_frm  = k_ra8_ssie_frm_default;
      return true;
    case k_ra8_ssie_format_tdm_4:
      *out_omod = k_ra8_ssie_omod_tdm;
      *out_frm  = k_ra8_ssie_frm_alt1;
      return true;
    case k_ra8_ssie_format_tdm_6:
      *out_omod = k_ra8_ssie_omod_tdm;
      *out_frm  = k_ra8_ssie_frm_alt2;
      return true;
    case k_ra8_ssie_format_tdm_8:
      *out_omod = k_ra8_ssie_omod_tdm;
      *out_frm  = k_ra8_ssie_frm_alt3;
      return true;
    default:
      return false;
  }
}

/**
 * @brief Decode SSISR/SSIFSR into a ``ra8_ssie_event_t`` bitmap.
 *
 * @param[in] ssisr  Raw SSISR snapshot (HUM Ch 46.2.2 p 3066).
 * @param[in] ssifsr Raw SSIFSR snapshot (HUM Ch 46.2.4 p 3083).
 * @return Bitmap of ``ra8_ssie_event_t`` flags.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t internal_decode_event(uint32_t ssisr, uint32_t ssifsr)
{
  uint8_t events = k_ra8_ssie_evt_none;
  if ((ssisr & k_ra8_ssie_mask_iirq) != 0U) {
    events |= k_ra8_ssie_evt_idle;
  }
  if ((ssisr & k_ra8_ssie_mask_tuirq) != 0U) {
    events |= k_ra8_ssie_evt_tx_under;
  }
  if ((ssisr & k_ra8_ssie_mask_toirq) != 0U) {
    events |= k_ra8_ssie_evt_tx_over;
  }
  if ((ssisr & k_ra8_ssie_mask_ruirq) != 0U) {
    events |= k_ra8_ssie_evt_rx_under;
  }
  if ((ssisr & k_ra8_ssie_mask_roirq) != 0U) {
    events |= k_ra8_ssie_evt_rx_over;
  }
  if ((ssifsr & k_ra8_ssie_mask_tde) != 0U) {
    events |= k_ra8_ssie_evt_tx_empty;
  }
  if ((ssifsr & k_ra8_ssie_mask_rdf) != 0U) {
    events |= k_ra8_ssie_evt_rx_full;
  }
  return events;
}

/**
 * @brief Build the SSICR value from a config descriptor.
 *
 * @details
 * Packs role / divider / data + system word lengths into a 32-bit
 * register image. TEN/REN are intentionally left clear so the
 * caller can prime the TX FIFO before starting communication
 * (HUM Ch 46.6.2 "Transmission" p 3098).
 *
 * @param[in] cfg See implementation.
 * @param[in] frm See implementation.
 * @param[in] pdta See implementation.
 * @param[in] sdta See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t
internal_build_ssicr(const ra8_ssie_cfg_t* cfg, uint8_t frm, uint8_t pdta, uint8_t sdta)
{
  uint32_t ssicr = 0U;
  /* HUM Ch 46.2.1 "SSICR : Control Register" p 3057 */
  if (cfg->role == k_ra8_ssie_role_controller) {
    ssicr |= k_ra8_ssie_mask_mst;
  }
  /* HUM Ch 46.2.1 "SSICR : Control Register" p 3056 */
  ssicr |= ((uint32_t)cfg->bclk_div << k_ra8_ssie_bit_ckdv0) & k_ra8_ssie_mask_ckdv;
  /* HUM Ch 46.2.1 "SSICR : Control Register" p 3057 */
  ssicr |= ((uint32_t)cfg->system_word << k_ra8_ssie_bit_swl0) & k_ra8_ssie_mask_swl;
  /* HUM Ch 46.2.1 "SSICR : Control Register" p 3057 */
  ssicr |= ((uint32_t)cfg->data_word << k_ra8_ssie_bit_dwl0) & k_ra8_ssie_mask_dwl;
  /* HUM Ch 46.2.1 "SSICR : Control Register" p 3057 */
  ssicr |= ((uint32_t)frm << k_ra8_ssie_bit_frm0) & k_ra8_ssie_mask_frm;
  /* HUM Ch 46.2.1 "SSICR : Control Register" p 3056 */
  if (cfg->long_frame) {
    ssicr |= k_ra8_ssie_mask_del;
  }
  /* HUM Ch 46.2.1 "SSICR : Control Register" p 3056 */
  if (pdta != 0U) {
    ssicr |= k_ra8_ssie_mask_pdta;
  }
  /* HUM Ch 46.2.1 "SSICR : Control Register" p 3056 */
  if (sdta != 0U) {
    ssicr |= k_ra8_ssie_mask_sdta;
  }
  /* HUM Ch 46.2.1 "SSICR : Control Register" p 3057 */
  if (cfg->spdp_high) {
    ssicr |= k_ra8_ssie_mask_spdp;
  }
  /* HUM Ch 46.2.1 "SSICR : Control Register" p 3057 */
  if (cfg->lrckp_low) {
    ssicr |= k_ra8_ssie_mask_lrckp;
  }
  /* HUM Ch 46.2.1 "SSICR : Control Register" p 3057 */
  if (cfg->bckp_rising) {
    ssicr |= k_ra8_ssie_mask_bckp;
  }
  /* HUM Ch 46.2.1 "SSICR : Control Register" p 3058 */
  if (cfg->use_gpt_clk) {
    ssicr |= k_ra8_ssie_mask_cks;
  }
  return ssicr;
}

/**
 * @brief Bounded poll on SSIFCR until SSIRST clears.
 *
 * @return ``k_ra8_ok`` if SSIRST self-cleared, otherwise
 *         ``k_ra8_err_hw_timeout``.
 *
 * @details See HUM Ch 46.2.3 "SSIRST" description, p 3077-3078.
 * In fake/host mode the bit clears immediately after we
 * write 0; in production a few PCLK cycles are required.
 *
 * @param[in] reg See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_wait_ssirst_clear(volatile r_ssie_regs_t* reg)
{
  /* HUM Ch 46.2.3 "SSIFCR : FIFO Control Register" p 3077 */
  return ra8_hw_wait_flag_clear32(&reg->SSIFCR,
                                  k_ra8_ssie_mask_ssirst,
                                  (uint32_t)k_ra8_ssie_reset_poll_max);
}

/**
 * @brief Bounded poll on SSIFCR until both FIFO reset bits clear.
 *
 * @details See implementation.
 * @param[in] reg See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_wait_fifo_reset_clear(volatile r_ssie_regs_t* reg)
{
  /* HUM Ch 46.2.3 "SSIFCR : FIFO Control Register" p 3077 */
  return ra8_hw_wait_flag_clear32(&reg->SSIFCR,
                                  k_ra8_ssie_mask_rfrst_tfrst,
                                  (uint32_t)k_ra8_ssie_reset_poll_max);
}

/**
 * @brief Pack SSIOFR (audio format register) value from cfg + decoded OMOD.
 *
 * @details
 * Honours the HUM Ch 46.2.7 Note 2 p 3091 constraint that BCKASTP and
 * LRCONT must not be set together: LRCONT wins, BCKASTP is silently
 * dropped when both are requested.
 *
 * @param[in] cfg See implementation.
 * @param[in] omod See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_build_ssiofr(const ra8_ssie_cfg_t* cfg, uint8_t omod)
{
  uint32_t ssiofr = (uint32_t)omod & k_ra8_ssie_mask_omod;
  if (cfg->lr_continue && cfg->role == k_ra8_ssie_role_controller) {
    ssiofr |= k_ra8_ssie_mask_lrcont;
  }
  if (cfg->bck_idle_stop && cfg->role == k_ra8_ssie_role_controller && !cfg->lr_continue) {
    ssiofr |= k_ra8_ssie_mask_bckastp;
  }
  return ssiofr;
}

/**
 * @brief Pack SSIFCR (FIFO control register) value from cfg.
 *
 * @details See HUM Ch 46.2.3 "SSIFCR : FIFO Control Register" p 3077.
 *
 * @param[in] cfg See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_build_ssifcr(const ra8_ssie_cfg_t* cfg)
{
  uint32_t ssifcr = 0U;
  if (cfg->byte_swap) {
    ssifcr |= k_ra8_ssie_mask_bsw;
  }
  if (cfg->enable_aucke && cfg->role == k_ra8_ssie_role_controller) {
    ssifcr |= k_ra8_ssie_mask_aucke;
  }
  return ssifcr;
}

/**
 * @brief Pack SSISCR (status control register) value from TX/RX thresholds.
 *
 * @details See HUM Ch 46.2.8 "SSISCR : Status Control Register" p 3094.
 *
 * @param[in] tx_threshold See implementation.
 * @param[in] rx_threshold See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_build_ssiscr(uint8_t tx_threshold, uint8_t rx_threshold)
{
  const uint32_t scr_tdes =
    ((uint32_t)tx_threshold << k_ra8_ssie_shift_tdes) & k_ra8_ssie_mask_tdes;
  const uint32_t scr_rdfs =
    ((uint32_t)rx_threshold << k_ra8_ssie_shift_rdfs) & k_ra8_ssie_mask_rdfs;
  return scr_tdes | scr_rdfs;
}

/**
 * @brief Pulse RFRST/TFRST high then low and wait for both bits to clear.
 *
 * @details
 * HUM Ch 46.2.3 "SSIFCR : FIFO Control Register" p 3077. ``ssifcr_base``
 * is the desired post-reset SSIFCR image (RFRST/TFRST clear).
 *
 * @param[in] reg See implementation.
 * @param[in] ssifcr_base See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_pulse_fifo_reset(volatile r_ssie_regs_t* reg, uint32_t ssifcr_base)
{
  reg->SSIFCR = ssifcr_base | k_ra8_ssie_mask_rfrst_tfrst;
  reg->SSIFCR = ssifcr_base;
  return internal_wait_fifo_reset_clear(reg);
}

/**
 * @brief Apply per-direction IRQ-enable bits to in-progress SSICR/SSIFCR.
 *
 * @details
 * Sets TUIEN+TIE for TX-enabled directions and ROIEN+RIE for RX-enabled
 * directions per HUM Ch 46.2.1 p 3057 and HUM Ch 46.2.3 p 3077.
 *
 * @param[in] desired_ren_ten See implementation.
 * @param[in] ssicr See implementation.
 * @param[in] ssifcr See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_apply_dir_irq_bits(uint32_t desired_ren_ten, uint32_t* ssicr, uint32_t* ssifcr)
{
  if ((desired_ren_ten & k_ra8_ssie_mask_ten) != 0U) {
    *ssicr |= k_ra8_ssie_mask_tuien;
    *ssifcr |= k_ra8_ssie_mask_tie;
  }
  if ((desired_ren_ten & k_ra8_ssie_mask_ren) != 0U) {
    *ssicr |= k_ra8_ssie_mask_roien;
    *ssifcr |= k_ra8_ssie_mask_rie;
  }
}

/**
 * @brief Format-decode wrapper that validates ranges before delegation.
 *
 * @param[in]  cfg     Caller's SSIE config.
 * @param[out] out_omod SSIOFR.OMOD encoding (0..2).
 * @param[out] out_frm  SSICR.FRM encoding (0..3).
 * @param[out] out_pdta Right-justify polarity bit.
 * @param[out] out_sdta Left-justify polarity bit.
 *
 * @return ``k_ra8_ok`` on success, ``k_ra8_err_invalid_arg`` if thresholds
 *         are out of range or the format enum is unrecognised.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_init_cfg(const ra8_ssie_cfg_t* cfg,
                                            uint8_t*              out_omod,
                                            uint8_t*              out_frm,
                                            uint8_t*              out_pdta,
                                            uint8_t*              out_sdta)
{
  if (cfg->tx_threshold > k_ra8_ssie_thresh_max || cfg->rx_threshold > k_ra8_ssie_thresh_max) {
    return k_ra8_err_invalid_arg;
  }
  *out_omod = 0U;
  *out_frm  = 0U;
  *out_pdta = 0U;
  *out_sdta = 0U;
  if (!internal_format_decode(cfg->format, out_omod, out_frm, out_pdta, out_sdta)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Drive the soft-reset sequence required before SSICR writes.
 *
 * @details
 * Pulses SSIRST high then low, then polls SSIFCR until SSIRST clears.
 * HUM Ch 46.2.3 "SSIFCR : FIFO Control Register" p 3077.
 *
 * @param[in] reg See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_pulse_ssi_reset(volatile r_ssie_regs_t* reg)
{
  reg->SSIFCR = k_ra8_ssie_mask_ssirst;
  reg->SSIFCR = 0U;
  return internal_wait_ssirst_clear(reg);
}

/**
 * @brief Power up the SSIE block: enable MSTP then drive SSIRST low.
 *
 * @details
 * Combines the two HUM-required pre-config steps so ``ra8_ssie_init``
 * stays under the per-function statement budget. HUM Ch 11.2.8 p 446
 * for MSTPCRC, HUM Ch 46.2.3 p 3077 for the reset pulse.
 *
 * @param[in] channel See implementation.
 * @param[in] reg See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_ssie_power_up(uint8_t channel, volatile r_ssie_regs_t* reg)
{
  const ra8_err_t mst_err = ra8_mstp_enable(s_ssie_mstp_table[channel]);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "ssie_init: mstp enable");
  const ra8_err_t rst_err = internal_pulse_ssi_reset(reg);
  RA8_RETURN_ON_ERROR(rst_err, s_tag, "ssie_init: SSIRST stuck");
  return k_ra8_ok;
}

/**
 * @brief Stamp SSICR/SSIOFR/SSISCR/SSIFCR for a freshly reset SSIE block.
 *
 * @details See implementation.
 * @param[in] reg See implementation.
 * @param[in] cfg See implementation.
 * @param[in] frm See implementation.
 * @param[in] pdta See implementation.
 * @param[in] sdta See implementation.
 * @param[in] omod See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_apply_init_regs(volatile r_ssie_regs_t* reg,
                                     const ra8_ssie_cfg_t*   cfg,
                                     uint8_t                 frm,
                                     uint8_t                 pdta,
                                     uint8_t                 sdta,
                                     uint8_t                 omod)
{
  reg->SSICR  = internal_build_ssicr(cfg, frm, pdta, sdta);
  reg->SSIOFR = internal_build_ssiofr(cfg, omod);
  reg->SSISCR = internal_build_ssiscr(cfg->tx_threshold, cfg->rx_threshold);
  reg->SSIFCR = internal_build_ssifcr(cfg);
}

ra8_err_t ra8_ssie_init(uint8_t channel, const ra8_ssie_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  volatile r_ssie_regs_t* reg = priv_ra8_ssie_internal_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }

  uint8_t         omod  = 0U;
  uint8_t         frm   = 0U;
  uint8_t         pdta  = 0U;
  uint8_t         sdta  = 0U;
  const ra8_err_t v_err = internal_validate_init_cfg(cfg, &omod, &frm, &pdta, &sdta);
  RA8_RETURN_ON_ERROR(v_err, s_tag, "ssie_init: bad cfg");

  /* HUM Ch 11.2.8 "MSTPCRC" p 446 */
  /* HUM Ch 46.2.3 "SSIRST" p 3077 */
  const ra8_err_t pwr_err = internal_ssie_power_up(channel, reg);
  RA8_RETURN_ON_ERROR(pwr_err, s_tag, "ssie_init: power up");

  internal_apply_init_regs(reg, cfg, frm, pdta, sdta, omod);

  g_ssie_runtime[channel].initialized  = true;
  g_ssie_runtime[channel].dma_attached = false;
  ra8_log_info_val(s_tag, "ssie_init channel", (uint32_t)channel);
  return k_ra8_ok;
}

ra8_err_t ra8_ssie_deinit(uint8_t channel)
{
  volatile r_ssie_regs_t* reg = priv_ra8_ssie_internal_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 46.2.1 "SSICR : Control Register" p 3056 */
  reg->SSICR = reg->SSICR & ~k_ra8_ssie_mask_ren_ten;
  /* HUM Ch 46.2.3 "SSIFCR : FIFO Control Register" p 3077 */
  reg->SSIFCR = reg->SSIFCR & ~k_ra8_ssie_mask_aucke;

  if (g_ssie_runtime[channel].dma_attached) {
    (void)ra8_ssie_detach_dma(channel);
  }
  g_ssie_runtime[channel].initialized = false;
  return ra8_mstp_disable(s_ssie_mstp_table[channel]);
}

ra8_err_t ra8_ssie_start(uint8_t channel, ra8_ssie_dir_t dir)
{
  if (dir != k_ra8_ssie_dir_rx && dir != k_ra8_ssie_dir_tx && dir != k_ra8_ssie_dir_tx_rx) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_ssie_regs_t* reg = priv_ra8_ssie_internal_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 46.6.2 "Transmission" p 3098 / 46.6.3 "Reception" p 3099:
   * the SSIE must be idle before REN/TEN may be set. */
  /* HUM Ch 46.2.1 "SSICR : Control Register" p 3056 */
  uint32_t       ssicr           = reg->SSICR;
  const uint32_t current_ren_ten = ssicr & k_ra8_ssie_mask_ren_ten;
  const uint32_t desired_ren_ten = (uint32_t)dir;
  if (desired_ren_ten == (current_ren_ten & desired_ren_ten)) {
    return k_ra8_ok; /* Already enabled, nothing to do. */
  }
  if (current_ren_ten != 0U) {
    return k_ra8_err_busy;
  }
  /* HUM Ch 46.2.2 "SSISR : Status Register" p 3066 */
  if ((reg->SSISR & k_ra8_ssie_mask_iirq) == 0U) {
    return k_ra8_err_busy;
  }

  /* HUM Ch 46.2.3 "SSIFCR : FIFO Control Register" p 3077 */
  uint32_t        ssifcr  = reg->SSIFCR;
  const ra8_err_t rst_err = internal_pulse_fifo_reset(reg, ssifcr);
  RA8_RETURN_ON_ERROR(rst_err, s_tag, "ssie_start: FIFO reset stuck");

  /* HUM Ch 46.2.1 "SSICR" p 3057 */ /* / HUM Ch 46.2.3 "SSIFCR" p 3077 */
  internal_apply_dir_irq_bits(desired_ren_ten, &ssicr, &ssifcr);

  reg->SSIFCR = ssifcr;
  /* HUM Ch 46.2.1 "SSICR : Control Register" p 3056 */
  reg->SSICR = ssicr | desired_ren_ten;
  return k_ra8_ok;
}

ra8_err_t ra8_ssie_stop(uint8_t channel)
{
  volatile r_ssie_regs_t* reg = priv_ra8_ssie_internal_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 46.6.4 "Halt of Communication" p 3100 */
  /* HUM Ch 46.2.1 "SSICR : Control Register" p 3056 */
  uint32_t ssicr = reg->SSICR;
  ssicr &= ~k_ra8_ssie_mask_ren_ten;
  ssicr &= ~k_ra8_ssie_mask_err_ien;
  ssicr |= k_ra8_ssie_mask_iien;
  reg->SSICR = ssicr;

  /* HUM Ch 46.2.3 "SSIFCR : FIFO Control Register" p 3077 */
  uint32_t ssifcr = reg->SSIFCR;
  ssifcr &= ~k_ra8_ssie_mask_rie_tie;
  reg->SSIFCR = ssifcr;
  return k_ra8_ok;
}

ra8_err_t ra8_ssie_start_recovery(uint8_t channel)
{
  volatile r_ssie_regs_t* reg = priv_ra8_ssie_internal_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 46.2.3 "SSIFCR : FIFO Control Register" p 3077 */
  const uint32_t ssifcr_save = reg->SSIFCR & ~k_ra8_ssie_mask_ssirst;
  reg->SSIFCR                = ssifcr_save | k_ra8_ssie_mask_ssirst;
  reg->SSIFCR                = ssifcr_save;
  /* HUM Ch 46.2.3 "SSIFCR : FIFO Control Register" p 3077 */
  const ra8_err_t rst_err = internal_wait_ssirst_clear(reg);
  RA8_RETURN_ON_ERROR(rst_err, s_tag, "ssie_recover: SSIRST stuck");

  /* HUM Ch 46.2.2 "SSISR : Status Register" p 3066-3072 */
  const uint32_t ssisr = reg->SSISR;
  reg->SSISR           = ssisr & ~k_ra8_ssie_mask_err_all;
  return k_ra8_ok;
}

ra8_err_t ra8_ssie_mute(uint8_t channel, bool enable)
{
  volatile r_ssie_regs_t* reg = priv_ra8_ssie_internal_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 46.2.1 "SSICR : Control Register" p 3056 */
  uint32_t ssicr = reg->SSICR;
  if (enable) {
    ssicr |= k_ra8_ssie_mask_muen;
  } else {
    ssicr &= ~k_ra8_ssie_mask_muen;
  }
  reg->SSICR = ssicr;
  return k_ra8_ok;
}

ra8_err_t ra8_ssie_set_thresholds(uint8_t channel, uint8_t tx_threshold, uint8_t rx_threshold)
{
  volatile r_ssie_regs_t* reg = priv_ra8_ssie_internal_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (tx_threshold > k_ra8_ssie_thresh_max || rx_threshold > k_ra8_ssie_thresh_max) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 46.2.8 "SSISCR : Status Control Register" p 3094 */
  reg->SSISCR = internal_build_ssiscr(tx_threshold, rx_threshold);
  return k_ra8_ok;
}

ra8_err_t ra8_ssie_write_sample(uint8_t channel, uint32_t sample)
{
  volatile r_ssie_regs_t* reg = priv_ra8_ssie_internal_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 46.2.5 "SSIFTDR : Transmit FIFO Data Register" p 3088 */
  reg->SSIFTDR = sample;
  return k_ra8_ok;
}

ra8_err_t ra8_ssie_read_sample(uint8_t channel, uint32_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  volatile const r_ssie_regs_t* reg = priv_ra8_ssie_internal_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 46.2.6 "SSIFRDR : Receive FIFO Data Register" p 3089 */
  *out = reg->SSIFRDR;
  return k_ra8_ok;
}

ra8_err_t ra8_ssie_write_buffer(uint8_t         channel,
                                const uint32_t* buffer,
                                uint16_t        samples,
                                uint16_t*       out_written)
{
  RA8_CHECK_NULL_PTR(buffer, s_tag, "buffer must not be nullptr");
  RA8_CHECK_NULL_PTR(out_written, s_tag, "out_written must not be nullptr");
  volatile r_ssie_regs_t* reg = priv_ra8_ssie_internal_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }

  uint16_t written = 0U;
  for (uint16_t i = 0U; i < samples; i++) {
    /* HUM Ch 46.2.4 "SSIFSR : FIFO Status Register" p 3083 */
    const uint8_t tdc = (uint8_t)((reg->SSIFSR & k_ra8_ssie_mask_tdc) >> k_ra8_ssie_shift_tdc);
    if (tdc >= (uint8_t)k_ra8_ssie_fifo_depth) {
      break;
    }
    /* HUM Ch 46.2.5 "SSIFTDR : Transmit FIFO Data Register" p 3088 */
    reg->SSIFTDR = buffer[i];
    written++;
  }
  if (written > 0U) {
    /* HUM Ch 46.2.4 "SSIFSR : FIFO Status Register" p 3083 */
    const uint32_t fsr = reg->SSIFSR;
    (void)fsr;
    /* W1C of TDE keeps the RDF bit (mask_rdf_clear keeps RDF=1). */
    reg->SSIFSR = k_ra8_ssie_mask_rdf_clear;
  }
  *out_written = written;
  return k_ra8_ok;
}

ra8_err_t
ra8_ssie_read_buffer(uint8_t channel, uint32_t* buffer, uint16_t samples, uint16_t* out_read)
{
  RA8_CHECK_NULL_PTR(buffer, s_tag, "buffer must not be nullptr");
  RA8_CHECK_NULL_PTR(out_read, s_tag, "out_read must not be nullptr");
  volatile r_ssie_regs_t* reg = priv_ra8_ssie_internal_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }

  uint16_t read = 0U;
  for (uint16_t i = 0U; i < samples; i++) {
    /* HUM Ch 46.2.4 "SSIFSR : FIFO Status Register" p 3083 */
    const uint8_t rdc = (uint8_t)((reg->SSIFSR & k_ra8_ssie_mask_rdc) >> k_ra8_ssie_shift_rdc);
    if (rdc == 0U) {
      break;
    }
    /* HUM Ch 46.2.6 "SSIFRDR : Receive FIFO Data Register" p 3089 */
    buffer[i] = reg->SSIFRDR;
    read++;
  }
  if (read > 0U) {
    /* HUM Ch 46.2.4 "SSIFSR : FIFO Status Register" p 3083 */
    const uint32_t fsr = reg->SSIFSR;
    (void)fsr;
    /* W1C of RDF keeps the TDE bit (mask_tde_clear keeps TDE=1). */
    reg->SSIFSR = k_ra8_ssie_mask_tde_clear;
  }
  *out_read = read;
  return k_ra8_ok;
}

ra8_err_t ra8_ssie_get_status(uint8_t channel, ra8_ssie_status_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  volatile const r_ssie_regs_t* reg = priv_ra8_ssie_internal_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 46.2.2 "SSISR : Status Register" p 3066 */
  const uint32_t ssisr = reg->SSISR;
  /* HUM Ch 46.2.4 "SSIFSR : FIFO Status Register" p 3083 */
  const uint32_t ssifsr = reg->SSIFSR;

  out->ssisr    = ssisr;
  out->ssifsr   = ssifsr;
  out->rx_count = (uint8_t)((ssifsr & k_ra8_ssie_mask_rdc) >> k_ra8_ssie_shift_rdc);
  out->tx_count = (uint8_t)((ssifsr & k_ra8_ssie_mask_tdc) >> k_ra8_ssie_shift_tdc);
  out->rx_full  = ((ssifsr & k_ra8_ssie_mask_rdf) != 0U);
  out->tx_empty = ((ssifsr & k_ra8_ssie_mask_tde) != 0U);
  out->idle     = ((ssisr & k_ra8_ssie_mask_iirq) != 0U);
  out->error    = ((ssisr & k_ra8_ssie_mask_err_all) != 0U);
  out->events   = internal_decode_event(ssisr, ssifsr);
  return k_ra8_ok;
}

ra8_err_t ra8_ssie_clear_status(uint8_t channel, uint32_t mask)
{
  volatile r_ssie_regs_t* reg = priv_ra8_ssie_internal_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 46.2.2 "SSISR : Status Register" p 3069-3072 */
  const uint32_t writeable = mask & k_ra8_ssie_mask_err_all;
  const uint32_t current   = reg->SSISR;
  reg->SSISR               = current & ~writeable;
  return k_ra8_ok;
}

ra8_err_t ra8_ssie_set_irq_enable(uint8_t channel, uint32_t mask, bool enable)
{
  volatile r_ssie_regs_t* reg = priv_ra8_ssie_internal_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 46.2.1 "SSICR : Control Register" p 3057 */
  const uint32_t apply = mask & k_ra8_ssie_mask_irq_all;
  uint32_t       ssicr = reg->SSICR;
  if (enable) {
    ssicr |= apply;
  } else {
    ssicr &= ~apply;
  }
  reg->SSICR = ssicr;
  return k_ra8_ok;
}

ra8_err_t ra8_ssie_attach_handler(ra8_ssie_event_fn_t fn, void* ctx)
{
  s_ssie_fn  = fn;
  s_ssie_ctx = ctx;
  return k_ra8_ok;
}

RA8_ISR_SAFE
void ra8_ssie_dispatch(uint8_t channel)
{
  volatile const r_ssie_regs_t* reg = priv_ra8_ssie_internal_regs(channel);
  if (reg == nullptr) {
    return;
  }
  /* HUM Ch 46.2.2 "SSISR : Status Register" p 3066 */
  const uint32_t ssisr = reg->SSISR;
  /* HUM Ch 46.2.4 "SSIFSR : FIFO Status Register" p 3083 */
  const uint32_t            ssifsr = reg->SSIFSR;
  const uint8_t             events = internal_decode_event(ssisr, ssifsr);
  const ra8_ssie_event_fn_t fn     = s_ssie_fn;
  void* const               ctx    = s_ssie_ctx;
  if (fn != nullptr) {
    fn(ctx, channel, events, ssisr);
  }
}

ra8_err_t ra8_ssie_set_fifo_threshold(uint8_t channel, uint8_t tx_threshold, uint8_t rx_threshold)
{
  /* HUM Ch 46.2.8 "SSISCR : Status Control Register" p 3094.
   * Convenience wrapper named after the SSITDMR / SSIRDMR aliases used
   * by the audio pipeline; same effect as ra8_ssie_set_thresholds. */
  return ra8_ssie_set_thresholds(channel, tx_threshold, rx_threshold);
}

ra8_err_t ra8_ssie_enter_stop(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra8_ssie_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  return ra8_mstp_disable(s_ssie_mstp_table[channel]);
}

ra8_err_t ra8_ssie_exit_stop(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra8_ssie_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  return ra8_mstp_enable(s_ssie_mstp_table[channel]);
}
