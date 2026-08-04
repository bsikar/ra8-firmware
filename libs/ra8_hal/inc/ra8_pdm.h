/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_pdm.h
 * @brief Pulse Density Modulation Interface (PDM-IF) capture driver
 * @ingroup grp_hal_audio
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Polling-mode capture driver for the RA8D2 PDM-IF block (HUM Ch 49,
 * p 3190-3256). A PDM MEMS microphone streams a 1-bit pulse-density
 * bitstream on PDMDATn clocked by PDMCLKn; the peripheral's hardware
 * filter chain (sinc decimator -> compensation FIR -> high-pass ->
 * half-band decimation) converts it to signed PCM presented, one sample
 * per FIFO slot, in the channel data-read register (PDDRR).
 *
 * The driver is the mechanism; the filter coefficients and decimation
 * ratio are microphone-and-rate policy supplied by the caller via
 * ::ra8_pdm_channel_cfg_t (see the EK-RA8D2 ``pdm_mic_demo`` for the
 * SPH0690 coefficient set). Sample delivery is polled -- no interrupt or
 * DTC plumbing -- which keeps the demo path self-contained.
 *
 * Bring-up order (HUM Ch 49.4.1 "Start Flow"):
 *   1. ::ra8_pdm_init            -- module-stop release.
 *   2. ::ra8_pdm_configure       -- mode + filter + coefficients.
 *   3. ::ra8_pdm_start           -- activate channel filtering.
 *   4. wait mic wake-up + filter settling (caller delay).
 *   5. ::ra8_pdm_read_enable     -- clear status, enable + prime FIFO.
 *   6. ::ra8_pdm_read (repeated) -- drain PCM samples.
 *   7. ::ra8_pdm_stop            -- halt the channel.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_pdm_channel_t
 * @brief PDM-IF channel selectors.
 *
 * @details
 * The RA8D2 exposes three PDM channels. The EK-RA8D2 SPH0690 MEMS
 * microphones (MIC1/MIC2) are wired to PDMCLK2 (P812) / PDMDAT2 (P502),
 * i.e. channel 2.
 */
typedef enum : uint8_t {
  k_ra8_pdm_ch0      = 0U, /**< Channel 0.                     */
  k_ra8_pdm_ch1      = 1U, /**< Channel 1.                     */
  k_ra8_pdm_ch2      = 2U, /**< Channel 2 (EK-RA8D2 MEMS mic). */
  k_ra8_pdm_ch_count = 3U, /**< Number of channels (bound).    */
} ra8_pdm_channel_t;

/**
 * @enum ra8_pdm_coeff_count_t
 * @brief Filter-coefficient array lengths (HUM Ch 49.2.21-49.2.35).
 *
 * @details
 * Fixed by the PDM-IF hardware filter topology; used to size the
 * coefficient arrays in ::ra8_pdm_channel_cfg_t.
 */
typedef enum : uint8_t {
  k_ra8_pdm_hpf_h_count  = 2U,  /**< High-pass filter h(0..1) coefficients.       */
  k_ra8_pdm_comp_h_count = 11U, /**< Compensation filter h(0..10) coefficients.   */
  k_ra8_pdm_lpf_h1_count = 20U, /**< Low-pass (half-band) h1(0..19) coefficients. */
} ra8_pdm_coeff_count_t;

/**
 * @struct ra8_pdm_channel_cfg_t
 * @brief One PDM channel's mode + filter configuration.
 *
 * @details
 * Maps directly onto the channel's PDMDSR/PDSFCR/coefficient/PDDBCR
 * registers. The sinc order, decimation ratio and clip range must be a
 * combination allowed by HUM Table 49.7; the FIR coefficients are the
 * microphone/vendor filter set.
 *
 * @invariant ``sinc_order`` is in 1..4.
 * @invariant ``clock_div``/``sinc_dec``/``sinc_range`` form a valid
 *            HUM Table 49.7 row.
 *
 * @see ra8_pdm_configure
 */
typedef struct {
  uint8_t  sinc_order;                     /**< Sinc filter order 1..4 (PDMDSR.SFMD).            */
  uint8_t  clock_div;                      /**< PDM_CLKn divider code (PDSFCR.CKDIV[3:0]).       */
  uint8_t  sinc_dec;                       /**< Sinc decimation ratio - 1 (PDSFCR.SINCDEC[7:0]). */
  uint8_t  sinc_range;                     /**< Sinc output clip range (PDSFCR.SINCRNG[4:0]).    */
  uint8_t  data_shift;                     /**< Data-buffer input shift (PDMDSR.DBIS[3:0]).      */
  uint8_t  edge;                           /**< 0: rise-edge ch n; 1: fall-edge ch n-1 (INPSEL). */
  uint8_t  hpf_shift;                      /**< High-pass input shift (PDMDSR.HFIS[1:0]).        */
  uint8_t  cf_shift;                       /**< Compensation input shift (PDMDSR.CFIS[1:0]).     */
  uint8_t  lpf_shift;                      /**< Low-pass input shift (PDMDSR.LFIS[1:0]).         */
  uint8_t  rx_threshold;                   /**< Data reception threshold code (PDDBCR.DATRITHR). */
  uint16_t hpf_s0;                         /**< High-pass filter coefficient s(0).               */
  uint16_t hpf_k1;                         /**< High-pass filter coefficient k(1).               */
  uint16_t hpf_h[k_ra8_pdm_hpf_h_count];   /**< High-pass filter h(0..1).                        */
  uint16_t comp_h[k_ra8_pdm_comp_h_count]; /**< Compensation filter h(0..10).                    */
  uint16_t lpf_h0;                         /**< Low-pass filter coefficient h0.                  */
  uint16_t lpf_h1[k_ra8_pdm_lpf_h1_count]; /**< Low-pass filter h1(0..19).                       */
} ra8_pdm_channel_cfg_t;

/**
 * @brief Release the PDM-IF module stop and reset its common bank.
 *
 * @details
 * Clears the PDM-IF bit in MSTPCRC so the block is clocked, then zeroes
 * the common start/stop/status-clear triggers to a known state. Must run
 * before any channel configuration.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Module clocked and common bank reset.
 * @retval k_ra8_err_hw_timeout    Module-stop release did not settle.
 *
 * @pre PDMIFCLK (MOCO, 8 MHz) is running.
 * @pre Single-threaded init context or IRQs masked.
 * @post The PDM-IF block is clocked out of module stop.
 * @post Common start/stop triggers read back clear.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdm_init(void);

/**
 * @brief Put the PDM-IF block back into module stop.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Module stopped.
 * @retval k_ra8_err_hw_timeout    Module-stop entry did not settle.
 *
 * @pre ::ra8_pdm_init previously succeeded.
 * @pre No channel is mid-capture.
 * @post The PDM-IF block is unclocked.
 * @post Register contents are undefined until re-init.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdm_deinit(void);

/**
 * @brief Program a channel's mode, decimation and filter coefficients.
 *
 * @details
 * Implements the configuration portion of HUM Ch 49.4.1 "Start Flow":
 * writes PDMDSR (mode/shifts/edge), PDSFCR (clock divider, decimation,
 * clip range), the high-pass/compensation/low-pass coefficient banks and
 * PDDBCR (reception threshold). Does not start the channel.
 *
 * @param[in] ch  Channel index (0..2).
 * @param[in] cfg Channel configuration (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Channel configured.
 * @retval k_ra8_err_invalid_arg  ``ch`` out of range.
 * @retval k_ra8_err_null_ptr     ``cfg`` was NULL.
 *
 * @pre ::ra8_pdm_init succeeded.
 * @pre ``cfg`` describes a HUM Table 49.7 combination.
 * @post The channel's filter registers hold ``cfg``.
 * @post The channel remains stopped (no data produced yet).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdm_configure(uint8_t ch, const ra8_pdm_channel_cfg_t* cfg);

/**
 * @brief Activate a configured channel's filter (start trigger).
 *
 * @details
 * Sets the channel's common start-trigger bit (PDCSTRTR.STRTRGn), which
 * begins clocking PDMCLKn and running the filter chain. The caller must
 * then wait the microphone wake-up plus filter settling time before
 * enabling data read.
 *
 * @param[in] ch Channel index (0..2).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Start trigger issued.
 * @retval k_ra8_err_invalid_arg  ``ch`` out of range.
 *
 * @pre ::ra8_pdm_configure succeeded for ``ch``.
 * @pre PDMCLKn / PDMDATn pins are routed to the PDM function.
 * @post The channel filter is running.
 * @post PDMCLKn is toggling at the configured rate.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdm_start(uint8_t ch);

/**
 * @brief Clear status, enable data read and prime the receive FIFO.
 *
 * @details
 * Clears the channel status flags, sets PDDRCR.DATRE and performs the
 * mandatory ``FIFO_DEPTH - 2`` priming reads of PDDRR (discarded) that
 * HUM Ch 49 "Normal Processing Flow" requires before valid samples flow.
 * Call after the microphone wake-up delay.
 *
 * @param[in] ch Channel index (0..2).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Data read enabled and FIFO primed.
 * @retval k_ra8_err_invalid_arg  ``ch`` out of range.
 *
 * @pre ::ra8_pdm_start succeeded and the settling delay elapsed.
 * @pre The channel filter reports running.
 * @post Subsequent ::ra8_pdm_read calls return live PCM samples.
 * @post The channel status flags are cleared.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdm_read_enable(uint8_t ch);

/**
 * @brief Drain up to @p max PCM samples from a channel's FIFO.
 *
 * @details
 * Reads PDDSR for the current FIFO fill count, then pops that many (capped
 * at @p max) 20-bit samples from PDDRR, sign-extending each to a full
 * ``int32_t``. Non-blocking: returns 0 in ``*out_count`` if the FIFO is
 * empty.
 *
 * @param[in]  ch        Channel index (0..2).
 * @param[out] out       Destination sample buffer (non-NULL).
 * @param[in]  max       Capacity of @p out in samples (>0).
 * @param[out] out_count Number of samples written (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Zero or more samples returned.
 * @retval k_ra8_err_invalid_arg  ``ch`` out of range or ``max`` was 0.
 * @retval k_ra8_err_null_ptr     ``out`` or ``out_count`` was NULL.
 *
 * @pre ::ra8_pdm_read_enable succeeded for ``ch``.
 * @pre ``out`` has room for ``max`` ``int32_t`` samples.
 * @post ``*out_count`` is in ``[0, max]``.
 * @post Each written sample is sign-extended from 20 bits.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdm_read(uint8_t ch, int32_t* out, uint32_t max, uint32_t* out_count);

/**
 * @brief Stop a channel and wait for its filter to halt.
 *
 * @details
 * Disables data read (PDDRCR), issues the channel stop trigger
 * (PDCSTPTR.STPTRGn) per HUM Ch 49.4.2 "Stop Flow" and polls PDCSR.STATEn
 * until the channel reports stopped or a bounded retry budget expires.
 *
 * @param[in] ch Channel index (0..2).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Channel stopped.
 * @retval k_ra8_err_invalid_arg  ``ch`` out of range.
 * @retval k_ra8_err_hw_timeout   Channel did not report stopped.
 *
 * @pre ::ra8_pdm_start previously succeeded for ``ch``.
 * @pre Single-threaded context.
 * @post The channel filter is halted.
 * @post No further samples are produced until re-start.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdm_stop(uint8_t ch);

#ifdef __cplusplus
}
#endif
