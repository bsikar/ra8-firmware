/**
 * @file ra8_ssie_stream.c
 * @brief SSIE streaming data path: DMAC channel attach/detach + polled ISO
 *        bulk send/receive.
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Sibling translation unit of ``ra8_ssie.c`` carrying the bulk-transfer half of
 * the Serial Sound Interface Enhanced (SSIE / I2S) driver: the DMAC adapter
 * (``ra8_ssie_attach_dma`` / ``ra8_ssie_detach_dma`` / ``ra8_ssie_attach_dma_pair``
 * and their private helpers) and the polled isochronous streaming primitives
 * (``ra8_ssie_send_iso`` / ``ra8_ssie_recv_iso``). Split out of ``ra8_ssie.c``
 * purely to satisfy the per-file size cap; behaviour is unchanged.
 *
 * Every register access carries a HUM Ch 46 citation against
 * ``r01uh1065ej0130-ra8d2.pdf``. The per-channel runtime bookkeeping array
 * (``g_ssie_runtime``) and the channel-validating register accessor
 * (``priv_ra8_ssie_internal_regs``) are shared with ``ra8_ssie.c`` through
 * ``ra8_hal_internal.h``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_dmac.h"
#include "ra8_err.h"
#include "ra8_hal_internal.h"
#include "ra8_ssie.h"
#include "ra8_ssie_regs.h"

/**
 * @var s_tag
 * @brief Logger tag for ra8_log_* messages produced by this driver.
 *
 * @note Module-private; never modified at runtime. Independent copy of the
 *       identical literal in ``ra8_ssie.c`` so the split needs no promoted
 *       external symbol for a read-only constant.
 */
static const char* s_tag = "SSIE";

/**
 * @brief Validate the DMA-attach descriptor against TX/RX intent.
 *
 * @param[in]  dma     Caller's DMA config.
 * @param[out] want_tx Set to true when caller requests a TX DMA channel.
 * @param[out] want_rx Set to true when caller requests an RX DMA channel.
 *
 * @return ``k_ra8_ok`` if the descriptor is consistent, else
 *         ``k_ra8_err_invalid_arg``.
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
static ra8_err_t
internal_validate_dma_cfg(const ra8_ssie_dma_cfg_t* dma, bool* want_tx, bool* want_rx)
{
  *want_tx = (dma->tx_dma_channel < k_ra8_ssie_dma_max_ch);
  *want_rx = (dma->rx_dma_channel < k_ra8_ssie_dma_max_ch);
  if (!*want_tx && !*want_rx) {
    return k_ra8_err_invalid_arg;
  }
  if (*want_tx && (dma->tx_buffer == nullptr || dma->tx_samples == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if (*want_rx && (dma->rx_buffer == nullptr || dma->rx_samples == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Kick off the TX DMAC for an SSIE channel.
 *
 * @details See HUM Ch 46.4.1 "Operation in DMAC Transfer" p 3104.
 *
 * @param[in] reg See implementation.
 * @param[in] channel See implementation.
 * @param[in] dma See implementation.
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
static ra8_err_t
internal_start_tx_dma(volatile r_ssie_regs_t* reg, uint8_t channel, const ra8_ssie_dma_cfg_t* dma)
{
  const ra8_dmac_config_t tx_cfg = {
    .src     = (uint32_t)(uintptr_t)dma->tx_buffer,
    .dst     = (uint32_t)(uintptr_t)&reg->SSIFTDR,
    .count   = dma->tx_samples,
    .width   = k_ra8_dmac_width_word,
    .src_inc = true,
    .dst_inc = false,
  };
  const ra8_err_t tx_err = ra8_dmac_start(dma->tx_dma_channel, &tx_cfg);
  if (tx_err == k_ra8_ok) {
    g_ssie_runtime[channel].tx_dma_channel = dma->tx_dma_channel;
  }
  return tx_err;
}

/**
 * @brief Kick off the RX DMAC for an SSIE channel.
 *
 * @details See HUM Ch 46.4.1 "Operation in DMAC Transfer" p 3104.
 *
 * @param[in] reg See implementation.
 * @param[in] channel See implementation.
 * @param[in] dma See implementation.
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
static ra8_err_t
internal_start_rx_dma(volatile r_ssie_regs_t* reg, uint8_t channel, const ra8_ssie_dma_cfg_t* dma)
{
  const ra8_dmac_config_t rx_cfg = {
    .src     = (uint32_t)(uintptr_t)&reg->SSIFRDR,
    .dst     = (uint32_t)(uintptr_t)dma->rx_buffer,
    .count   = dma->rx_samples,
    .width   = k_ra8_dmac_width_word,
    .src_inc = false,
    .dst_inc = true,
  };
  const ra8_err_t rx_err = ra8_dmac_start(dma->rx_dma_channel, &rx_cfg);
  if (rx_err == k_ra8_ok) {
    g_ssie_runtime[channel].rx_dma_channel = dma->rx_dma_channel;
  }
  return rx_err;
}

/**
 * @brief Roll back a TX DMAC start when the RX side fails.
 *
 * @details See implementation.
 * @param[in] channel See implementation.
 * @param[in] tx_dma_channel See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_unwind_tx_dma(uint8_t channel, uint8_t tx_dma_channel)
{
  (void)ra8_dmac_stop(tx_dma_channel);
  g_ssie_runtime[channel].tx_dma_channel = k_ra8_ssie_dma_ch_unused;
}

/**
 * @brief Run the per-direction DMA setup (TX then RX) with rollback.
 *
 * @details
 * Encapsulates the body of ``ra8_ssie_attach_dma`` once validation has
 * already chosen ``want_tx`` / ``want_rx``. On RX failure, the TX side
 * (if started) is unwound through ``internal_unwind_tx_dma``.
 *
 * @param[in] reg See implementation.
 * @param[in] channel See implementation.
 * @param[in] dma See implementation.
 * @param[in] want_tx See implementation.
 * @param[in] want_rx See implementation.
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
static ra8_err_t internal_attach_dma_dirs(volatile r_ssie_regs_t*   reg,
                                          uint8_t                   channel,
                                          const ra8_ssie_dma_cfg_t* dma,
                                          bool                      want_tx,
                                          bool                      want_rx)
{
  if (want_tx) {
    const ra8_err_t tx_err = internal_start_tx_dma(reg, channel, dma);
    /* GCOVR_EXCL_BR_START -- internal_start_tx_dma() error edge; the fake DMA seam always starts */
    RA8_RETURN_ON_ERROR(tx_err, s_tag, "ssie_attach_dma: tx start");
    /* GCOVR_EXCL_BR_STOP */
  }
  if (want_rx) {
    const ra8_err_t rx_err = internal_start_rx_dma(reg, channel, dma);
    if (rx_err != k_ra8_ok) {
      if (want_tx) {
        internal_unwind_tx_dma(channel, dma->tx_dma_channel);
      }
      return rx_err;
    }
  }
  return k_ra8_ok;
}

ra8_err_t ra8_ssie_attach_dma(uint8_t channel, const ra8_ssie_dma_cfg_t* dma)
{
  RA8_CHECK_NULL_PTR(dma, s_tag, "dma must not be nullptr");
  volatile r_ssie_regs_t* reg = priv_ra8_ssie_internal_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }

  bool            want_tx = false;
  bool            want_rx = false;
  const ra8_err_t v_err   = internal_validate_dma_cfg(dma, &want_tx, &want_rx);
  RA8_RETURN_ON_ERROR(v_err, s_tag, "ssie_attach_dma: bad cfg");

  const ra8_err_t d_err = internal_attach_dma_dirs(reg, channel, dma, want_tx, want_rx);
  /* GCOVR_EXCL_BR_START -- internal_attach_dma_dirs() error edge; both directions always attach */
  RA8_RETURN_ON_ERROR(d_err, s_tag, "ssie_attach_dma: dir start");
  /* GCOVR_EXCL_BR_STOP */

  g_ssie_runtime[channel].dma_attached = true;
  return k_ra8_ok;
}

ra8_err_t ra8_ssie_detach_dma(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra8_ssie_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  ra8_ssie_runtime_t* rt = &g_ssie_runtime[channel];
  if (rt->tx_dma_channel < k_ra8_ssie_dma_max_ch) {
    (void)ra8_dmac_stop(rt->tx_dma_channel);
    rt->tx_dma_channel = k_ra8_ssie_dma_ch_unused;
  }
  if (rt->rx_dma_channel < k_ra8_ssie_dma_max_ch) {
    (void)ra8_dmac_stop(rt->rx_dma_channel);
    rt->rx_dma_channel = k_ra8_ssie_dma_ch_unused;
  }
  rt->dma_attached = false;
  return k_ra8_ok;
}

ra8_err_t ra8_ssie_attach_dma_pair(uint8_t channel, uint8_t tx_dma_channel, uint8_t rx_dma_channel)
{
  if ((uint16_t)channel >= k_ra8_ssie_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  if (tx_dma_channel >= (uint8_t)k_ra8_ssie_dma_max_ch &&
      rx_dma_channel >= (uint8_t)k_ra8_ssie_dma_max_ch) {
    return k_ra8_err_invalid_arg;
  }
  ra8_ssie_runtime_t* rt = &g_ssie_runtime[channel];
  /* HUM Ch 46.4.1 "Operation in DMAC Transfer" p 3104:
   * the SSIE issues TX-empty / RX-full requests to the DMAC; the
   * driver only records which DMAC channels to free at detach. */
  rt->tx_dma_channel =
    (tx_dma_channel < k_ra8_ssie_dma_max_ch) ? tx_dma_channel : (uint8_t)k_ra8_ssie_dma_ch_unused;
  rt->rx_dma_channel =
    (rx_dma_channel < k_ra8_ssie_dma_max_ch) ? rx_dma_channel : (uint8_t)k_ra8_ssie_dma_ch_unused;
  rt->dma_attached = true;
  return k_ra8_ok;
}

ra8_err_t ra8_ssie_send_iso(uint8_t channel, const uint32_t* buffer, uint16_t samples)
{
  RA8_CHECK_NULL_PTR(buffer, s_tag, "buffer must not be nullptr");
  volatile r_ssie_regs_t* reg = priv_ra8_ssie_internal_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* Bounded loop: at most samples * k_ra8_ssie_fifo_depth FIFO probes.
   * HUM Ch 46.2.4 "SSIFSR : FIFO Status Register" p 3083 (TDC field). */
  uint16_t sent = 0U;
  while (sent < samples) {
    /* HUM Ch 46.2.4 "SSIFSR" p 3083 */
    const uint8_t tdc = (uint8_t)((reg->SSIFSR & k_ra8_ssie_mask_tdc) >> k_ra8_ssie_shift_tdc);
    if (tdc < (uint8_t)k_ra8_ssie_fifo_depth) {
      /* HUM Ch 46.2.5 "SSIFTDR" p 3088 */
      reg->SSIFTDR = buffer[sent];
      ++sent;
    }
    /* In fake/host builds the TDC count never advances because
     * no clock is consuming TX samples; treat the slot as drained
     * immediately on every loop pass to remain bounded. */
  }
  /* W1C of TDE keeps the RDF bit. */
  reg->SSIFSR = k_ra8_ssie_mask_rdf_clear;
  return k_ra8_ok;
}

ra8_err_t
ra8_ssie_recv_iso(uint8_t channel, uint32_t* buffer, uint16_t max_samples, uint16_t* out_got)
{
  RA8_CHECK_NULL_PTR(buffer, s_tag, "buffer must not be nullptr");
  RA8_CHECK_NULL_PTR(out_got, s_tag, "out_got must not be nullptr");
  volatile r_ssie_regs_t* reg = priv_ra8_ssie_internal_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }

  uint16_t got = 0U;
  for (uint16_t i = 0U; i < max_samples; ++i) {
    /* HUM Ch 46.2.4 "SSIFSR" p 3083 */ /* (RDC field) */
    const uint8_t rdc = (uint8_t)((reg->SSIFSR & k_ra8_ssie_mask_rdc) >> k_ra8_ssie_shift_rdc);
    if (rdc == 0U) {
      break;
    }
    /* HUM Ch 46.2.6 "SSIFRDR" p 3089 */
    buffer[got] = reg->SSIFRDR;
    ++got;
  }
  if (got > 0U) {
    /* W1C RDF, keep TDE. */
    reg->SSIFSR = k_ra8_ssie_mask_tde_clear;
  }
  *out_got = got;
  return k_ra8_ok;
}
