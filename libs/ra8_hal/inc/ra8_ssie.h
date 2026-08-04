/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_ssie.h
 * @brief Serial Sound Interface Enhanced (SSIE / I2S audio) driver
 * @ingroup grp_hal_audio
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Full-coverage RA8D2 SSIE driver. Each chip has two SSIE channels:
 * SSIE0 is full-duplex (SSITXD0 + SSIRXD0) and SSIE1 is half-duplex
 * (shared SSIDATA1 line). Both channels share the same register map
 * and are addressed by ``channel = 0|1``.
 *
 * Coverage of HUM Ch 46 "Serial Sound Interface Enhanced (SSIE)"
 * pages 3051-3121:
 *
 *  - Lifecycle: ``ra8_ssie_init`` / ``ra8_ssie_deinit`` follow the
 *    procedure in HUM Ch 46.6.1 "Initial Settings" p 3097.
 *  - All four formats: I2S, TDM (4/6/8 ch), Monaural, Right/Left
 *    justified through SDTA/PDTA polarity selectors.
 *  - All eight system word lengths (8/16/24/32/48/64/128/256 b)
 *    and all seven legal data word lengths (8/16/18/20/22/24/32 b).
 *  - All thirteen controller-mode bit clock dividers (CKDV 0x0..0xC).
 *  - Controller + peripheral mode with audio clock source selection (CKS).
 *  - Mute control with frame-boundary effect (HUM Ch 46.5.4 p 3107).
 *  - Polarity controls (BCKP, LRCKP, SPDP) and DEL short/long frame.
 *  - LRCONT (LR-clock continuation) and BCKASTP (idle BCK auto-stop)
 *    for power-friendly controller operation.
 *  - Byte swap (BSW) and AUDIO_MCK enable (AUCKE).
 *  - FIFO threshold programming via SSISCR (TDES, RDFS).
 *  - Single-sample read/write + buffered TX/RX through the FIFO.
 *  - Full IRQ surface: TX-empty (TXI / TIE+TDE), RX-full
 *    (RXI / RIE+RDF), idle (IIRQ), and four error sources
 *    (TUIRQ, TOIRQ, RUIRQ, ROIRQ).
 *  - DMA pump: TX + RX channels via ``ra8_dmac`` with FIFO half-full
 *    watermarks driving the request signals.
 *  - Power transition (MSTPC7/MSTPC8 enter/exit).
 *  - Error recovery (start_recovery) and IRQ dispatch with W1C
 *    flag clearing.
 *
 * Every register access in the matching ``.c`` carries a HUM Ch 46
 * citation. FSP ``r_ssi.c`` was used as reference for the SSICR /
 * SSIFCR / SSISCR shift macros and the start/stop sequences only
 * -- no code copied.
 *
 * @par State Machine
 * @dot
 * digraph ra8_ssie_states {
 *   bgcolor="transparent";
 *   rankdir=LR;
 *   node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
 *         fillcolor="#e8eef7", color="#5a7ca6"];
 *   edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];
 *
 *   __start [shape=circle, width=0.18, label="", fillcolor="#5a7ca6", color="#5a7ca6"];
 *
 *   Closed [label="Closed"];
 *   Idle [label="Idle"];
 *   Tx [label="Tx"];
 *   Rx [label="Rx"];
 *   TxRx [label="TxRx"];
 *   Error [label="Error"];
 *   Stopped [label="Stopped"];
 *
 *   __start -> Closed;
 *   Closed -> Idle [label="init() (MSTP+SSIRST)"];
 *   Idle -> Tx [label="start(TX)"];
 *   Idle -> Rx [label="start(RX)"];
 *   Idle -> TxRx [label="start(TX|RX)"];
 *   Tx -> Idle [label="stop()"];
 *   Rx -> Idle [label="stop()"];
 *   TxRx -> Idle [label="stop()"];
 *   Tx -> Error [label="TUIRQ"];
 *   Rx -> Error [label="ROIRQ"];
 *   Error -> Idle [label="start_recovery()"];
 *   Idle -> Stopped [label="enter_stop()"];
 *   Stopped -> Idle [label="exit_stop()"];
 *   Idle -> Closed [label="deinit()"];
 * }
 * @enddot
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_ssie_role_t
 * @brief Controller / peripheral selection (drives SSICR.MST).
 *
 * @details See HUM Ch 46.2.1 "MST" bit, p 3057.
 */
typedef enum : uint8_t {
  k_ra8_ssie_role_peripheral = 0U, /**< Bit clock + LR clock are inputs.  */
  k_ra8_ssie_role_controller = 1U, /**< Bit clock + LR clock are outputs. */
} ra8_ssie_role_t;

/**
 * @enum ra8_ssie_format_t
 * @brief Audio frame format (drives SSIOFR.OMOD[1:0] and SSICR.FRM).
 *
 * @details
 * The ``omod`` value goes straight to SSIOFR.OMOD[1:0] (HUM Ch 46.2.7
 * "OMOD" bits, p 3091). The driver maps the ``format_t`` enum to the
 * exact OMOD encoding from the chapter table:
 *
 * | Enum                          | OMOD | FRM  | Words/frame |
 * |-------------------------------|-----:|-----:|------------:|
 * | k_ra8_ssie_format_i2s          |  00b |  00b | 2 (L,R)     |
 * | k_ra8_ssie_format_left_just    |  00b |  00b | 2, SDTA=0   |
 * | k_ra8_ssie_format_right_just   |  00b |  00b | 2, PDTA=1   |
 * | k_ra8_ssie_format_monaural     |  10b |  00b | 1           |
 * | k_ra8_ssie_format_tdm_4        |  01b |  01b | 4           |
 * | k_ra8_ssie_format_tdm_6        |  01b |  10b | 6           |
 * | k_ra8_ssie_format_tdm_8        |  01b |  11b | 8           |
 */
typedef enum : uint8_t {
  k_ra8_ssie_format_i2s        = 0U, /**< I2S, 2 channels per frame.   */
  k_ra8_ssie_format_left_just  = 1U, /**< Left-justified, 2 channels.  */
  k_ra8_ssie_format_right_just = 2U, /**< Right-justified, 2 channels. */
  k_ra8_ssie_format_monaural   = 3U, /**< Monaural single-channel.     */
  k_ra8_ssie_format_tdm_4      = 4U, /**< TDM 4-slot.                  */
  k_ra8_ssie_format_tdm_6      = 5U, /**< TDM 6-slot.                  */
  k_ra8_ssie_format_tdm_8      = 6U, /**< TDM 8-slot.                  */
  /**
   * Backwards alias kept for unit tests that index OMOD directly.
   * Same encoding as ``k_ra8_ssie_format_tdm_4`` (OMOD=01b, FRM=01b).
   */
  k_ra8_ssie_format_tdm = 4U,
} ra8_ssie_format_t;

/**
 * @enum ra8_ssie_data_word_t
 * @brief Data word length (drives SSICR.DWL[2:0]).
 *
 * @details
 * "Number of significant bits per channel" (HUM Ch 46.2.1 DWL,
 * p 3057). The encoding ``111b`` is "setting prohibited" and not
 * exposed.
 */
typedef enum : uint8_t {
  k_ra8_ssie_dwl_8  = 0U, /**< 8-bit data word.  */
  k_ra8_ssie_dwl_16 = 1U, /**< 16-bit data word. */
  k_ra8_ssie_dwl_18 = 2U, /**< 18-bit data word. */
  k_ra8_ssie_dwl_20 = 3U, /**< 20-bit data word. */
  k_ra8_ssie_dwl_22 = 4U, /**< 22-bit data word. */
  k_ra8_ssie_dwl_24 = 5U, /**< 24-bit data word. */
  k_ra8_ssie_dwl_32 = 6U, /**< 32-bit data word. */
} ra8_ssie_data_word_t;

/**
 * @enum ra8_ssie_system_word_t
 * @brief System word length (drives SSICR.SWL[2:0]).
 *
 * @details
 * "Number of bits per channel" including padding (HUM Ch 46.2.1
 * SWL, p 3057). Must be at least the data word length.
 */
typedef enum : uint8_t {
  k_ra8_ssie_swl_8   = 0U, /**< 8-bit system word.   */
  k_ra8_ssie_swl_16  = 1U, /**< 16-bit system word.  */
  k_ra8_ssie_swl_24  = 2U, /**< 24-bit system word.  */
  k_ra8_ssie_swl_32  = 3U, /**< 32-bit system word.  */
  k_ra8_ssie_swl_48  = 4U, /**< 48-bit system word.  */
  k_ra8_ssie_swl_64  = 5U, /**< 64-bit system word.  */
  k_ra8_ssie_swl_128 = 6U, /**< 128-bit system word. */
  k_ra8_ssie_swl_256 = 7U, /**< 256-bit system word. */
} ra8_ssie_system_word_t;

/**
 * @enum ra8_ssie_bclk_div_t
 * @brief Bit-clock divider (drives SSICR.CKDV[3:0]).
 *
 * @details
 * Controller-mode only. Divides AUDIO_MCK to produce SSIBCKn. See
 * HUM Ch 46.2.1 CKDV table, p 3056. Encodings 0xD..0xF are
 * "setting prohibited" and intentionally not exposed.
 */
typedef enum : uint8_t {
  k_ra8_ssie_bclk_div_1   = 0x0U, /**< AUDIO_MCK / 1.   */
  k_ra8_ssie_bclk_div_2   = 0x1U, /**< AUDIO_MCK / 2.   */
  k_ra8_ssie_bclk_div_4   = 0x2U, /**< AUDIO_MCK / 4.   */
  k_ra8_ssie_bclk_div_8   = 0x3U, /**< AUDIO_MCK / 8.   */
  k_ra8_ssie_bclk_div_16  = 0x4U, /**< AUDIO_MCK / 16.  */
  k_ra8_ssie_bclk_div_32  = 0x5U, /**< AUDIO_MCK / 32.  */
  k_ra8_ssie_bclk_div_64  = 0x6U, /**< AUDIO_MCK / 64.  */
  k_ra8_ssie_bclk_div_128 = 0x7U, /**< AUDIO_MCK / 128. */
  k_ra8_ssie_bclk_div_6   = 0x8U, /**< AUDIO_MCK / 6.   */
  k_ra8_ssie_bclk_div_12  = 0x9U, /**< AUDIO_MCK / 12.  */
  k_ra8_ssie_bclk_div_24  = 0xAU, /**< AUDIO_MCK / 24.  */
  k_ra8_ssie_bclk_div_48  = 0xBU, /**< AUDIO_MCK / 48.  */
  k_ra8_ssie_bclk_div_96  = 0xCU, /**< AUDIO_MCK / 96.  */
} ra8_ssie_bclk_div_t;

/**
 * @enum ra8_ssie_dir_t
 * @brief Communication direction selector for ``ra8_ssie_start``.
 */
typedef enum : uint8_t {
  k_ra8_ssie_dir_rx    = 1U, /**< Reception only (REN).    */
  k_ra8_ssie_dir_tx    = 2U, /**< Transmission only (TEN). */
  k_ra8_ssie_dir_tx_rx = 3U, /**< Full duplex (TEN | REN). */
} ra8_ssie_dir_t;

/**
 * @enum ra8_ssie_event_t
 * @brief Decoded SSIE callback event bit-flags.
 *
 * @details
 * Event bits delivered to the registered callback via
 * ``ra8_ssie_dispatch``. Multiple bits may be set on a single
 * dispatch. Maps to SSISR / SSIFSR flags via
 * ``internal_decode_event`` in ``ra8_ssie.c``.
 */
typedef enum : uint8_t {
  k_ra8_ssie_evt_none     = 0x00U, /**< No bits set.           */
  k_ra8_ssie_evt_idle     = 0x01U, /**< IIRQ asserted.         */
  k_ra8_ssie_evt_tx_empty = 0x02U, /**< TDE asserted (TIE on). */
  k_ra8_ssie_evt_rx_full  = 0x04U, /**< RDF asserted (RIE on). */
  k_ra8_ssie_evt_tx_under = 0x08U, /**< TUIRQ.                 */
  k_ra8_ssie_evt_tx_over  = 0x10U, /**< TOIRQ.                 */
  k_ra8_ssie_evt_rx_under = 0x20U, /**< RUIRQ.                 */
  k_ra8_ssie_evt_rx_over  = 0x40U, /**< ROIRQ.                 */
  k_ra8_ssie_evt_error    = 0x78U, /**< Aggregate error mask.  */
} ra8_ssie_event_t;

/**
 * @struct ra8_ssie_cfg_t
 * @brief Configuration descriptor for ``ra8_ssie_init``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read by ``ra8_ssie_init`` in
 * ``libs/ra8_hal/src/ra8_ssie.c``.
 */
typedef struct {
  ra8_ssie_role_t        role;          /**< Controller or peripheral.         */
  ra8_ssie_format_t      format;        /**< I2S / TDM / monaural.             */
  ra8_ssie_data_word_t   data_word;     /**< Significant bits per channel.     */
  ra8_ssie_system_word_t system_word;   /**< Total bits per channel.           */
  ra8_ssie_bclk_div_t    bclk_div;      /**< CKDV divider (controller only).   */
  bool                   use_gpt_clk;   /**< true: GTIOC2A; false: AUDIO_CLK.  */
  bool                   long_frame;    /**< true: SSICR.DEL = 0 (long).       */
  bool                   bckp_rising;   /**< true: SSICR.BCKP = 1.             */
  bool                   lrckp_low;     /**< true: SSICR.LRCKP = 1.            */
  bool                   spdp_high;     /**< true: SSICR.SPDP = 1.             */
  bool                   byte_swap;     /**< SSIFCR.BSW.                       */
  bool                   lr_continue;   /**< SSIOFR.LRCONT (controller only).  */
  bool                   bck_idle_stop; /**< SSIOFR.BCKASTP (controller only). */
  bool                   enable_aucke;  /**< SSIFCR.AUCKE (controller only).   */
  uint8_t                tx_threshold;  /**< SSISCR.TDES[4:0] watermark.       */
  uint8_t                rx_threshold;  /**< SSISCR.RDFS[4:0] watermark.       */
} ra8_ssie_cfg_t;

/**
 * @struct ra8_ssie_status_t
 * @brief Snapshot of FIFO levels + error flags returned by
 *        ``ra8_ssie_get_status``.
 */
typedef struct {
  uint32_t ssisr;    /**< Raw SSISR value.                     */
  uint32_t ssifsr;   /**< Raw SSIFSR value.                    */
  uint8_t  rx_count; /**< RDC[5:0] -- entries currently in RX. */
  uint8_t  tx_count; /**< TDC[5:0] -- entries currently in TX. */
  bool     rx_full;  /**< RDF flag: RX above SSISCR.RDFS.      */
  bool     tx_empty; /**< TDE flag: TX below SSISCR.TDES.      */
  bool     idle;     /**< IIRQ: SSIE in idle state.            */
  bool     error;    /**< Any of ROIRQ/RUIRQ/TOIRQ/TUIRQ set.  */
  uint8_t  events;   /**< Decoded ra8_ssie_event_t bitmap.     */
} ra8_ssie_status_t;

/**
 * @struct ra8_ssie_dma_cfg_t
 * @brief DMA pump descriptor for ``ra8_ssie_attach_dma``.
 */
typedef struct {
  uint8_t   tx_dma_channel; /**< DMAC channel to drain TX (>=8 = unused). */
  uint8_t   rx_dma_channel; /**< DMAC channel to feed RX (>=8 = unused).  */
  uint32_t* tx_buffer;      /**< Source buffer for TX (32-bit samples).   */
  uint32_t* rx_buffer;      /**< Destination buffer for RX.               */
  uint16_t  tx_samples;     /**< TX sample count (must be > 0 if tx).     */
  uint16_t  rx_samples;     /**< RX sample count (must be > 0 if rx).     */
} ra8_ssie_dma_cfg_t;

/**
 * @typedef ra8_ssie_event_fn_t
 * @brief SSIE event callback fired by ``ra8_ssie_dispatch``.
 *
 * @param[in] ctx     Caller context registered via
 *                    ``ra8_ssie_attach_handler``.
 * @param[in] channel Channel that fired (0 or 1).
 * @param[in] events  Decoded ``ra8_ssie_event_t`` bitmap.
 * @param[in] ssisr   Snapshot of SSISR at IRQ entry.
 */
typedef void (*ra8_ssie_event_fn_t)(void* ctx, uint8_t channel, uint8_t events, uint32_t ssisr);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise an SSIE channel for I2S / TDM / monaural transfer.
 *
 * @details
 * Algorithm (HUM Ch 46.6.1 "Initial Settings", p 3097):
 *  1. Ungate MSTPC8 (SSIE0) or MSTPC7 (SSIE1) via ``ra8_mstp``.
 *  2. Software-reset the channel (SSIFCR.SSIRST = 1, then 0).
 *  3. Programme SSICR with role / clock divider / word lengths +
 *     polarity (BCKP, LRCKP, SPDP) and DEL (short/long frame).
 *  4. Programme SSIOFR with the audio format (OMOD), LRCONT and
 *     BCKASTP.
 *  5. Programme SSISCR with the FIFO watermarks.
 *  6. Programme SSIFCR with AUCKE + BSW.
 *  7. Leave TEN / REN / interrupts cleared so the caller can prime
 *     the FIFOs and call ``ra8_ssie_start``.
 *
 * @param[in] channel Channel index (0 = SSIE0, 1 = SSIE1).
 * @param[in] cfg     Non-NULL configuration descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Channel initialized.
 * @retval k_ra8_err_null_ptr     ``cfg`` was nullptr.
 * @retval k_ra8_err_invalid_arg  ``channel`` out of range.
 * @retval k_ra8_err_hw_timeout   SSIRST did not self-clear within
 *                               ``k_ra8_ssie_reset_poll_max`` reads.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_mstp_init()`` has run.
 *
 * @post SSICR holds the requested format with TEN/REN clear.
 * @post SSIE channel is ungated (MSTP bit cleared).
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_ssie_deinit
 * @see ra8_ssie_start
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ssie_init(uint8_t channel, const ra8_ssie_cfg_t* cfg);

/**
 * @brief Tear down an SSIE channel and gate its clock.
 *
 * @param[in] channel Channel index.
 * @return ``ra8_err_t`` error code.
 *
 * @pre IRQs masked or single-threaded.
 * @pre ``ra8_ssie_init`` previously succeeded for ``channel``.
 *
 * @post SSICR.TEN = 0 and SSICR.REN = 0.
 * @post MSTP bit set (peripheral gated).
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_ssie_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ssie_deinit(uint8_t channel);

/* =============================================================================
 * Run control
 * =============================================================================
 */

/**
 * @brief Enable transmission, reception, or full duplex.
 *
 * @details
 * Implements HUM Ch 46.6.2 "Transmission" (p 3098) and 46.6.3
 * "Reception" (p 3099). Resets both FIFOs (TFRST/RFRST), enables
 * the matching interrupt mask in SSIFCR (TIE/RIE), enables the
 * matching error masks in SSICR (TUIEN/ROIEN), then sets TEN/REN.
 *
 * @param[in] channel Channel index.
 * @param[in] dir     ``k_ra8_ssie_dir_tx``, ``..._rx`` or ``..._tx_rx``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Direction enabled.
 * @retval k_ra8_err_invalid_arg Channel or direction out of range.
 * @retval k_ra8_err_busy        SSIE is not idle and direction would
 *                              change (must call ``ra8_ssie_stop`` first).
 *
 * @pre Channel previously initialized.
 * @pre IRQs masked while modifying SSICR.
 *
 * @post Requested REN/TEN bits asserted.
 * @post Matching IRQ enable bits asserted.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_ssie_stop
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ssie_start(uint8_t channel, ra8_ssie_dir_t dir);

/**
 * @brief Disable transmission and reception.
 *
 * @details
 * Implements HUM Ch 46.6.4 "Idle Operation" p 3100. Clears
 * REN/TEN/error-IEN, leaves IIEN set so the caller is notified
 * when SSIE finishes the in-flight frame.
 *
 * @param[in] channel Channel index.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Stop requested; IIEN now armed.
 * @retval k_ra8_err_invalid_arg Channel out of range.
 *
 * @pre Channel previously initialized.
 * @pre IRQs masked while modifying SSICR.
 *
 * @post REN/TEN/error-IEN bits cleared.
 * @post IIEN bit set so the next IIRQ fires the callback.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ssie_stop(uint8_t channel);

/**
 * @brief Force the channel back into a known idle state.
 *
 * @details
 * Issues a software reset (SSIFCR.SSIRST), waits for it to
 * self-clear, then clears every error flag in SSISR. Used by the
 * IRQ dispatcher when a TUIRQ/ROIRQ pair is observed.
 *
 * @param[in] channel Channel index.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Channel reset and flags cleared.
 * @retval k_ra8_err_invalid_arg Channel out of range.
 * @retval k_ra8_err_hw_timeout  SSIRST did not self-clear in bound.
 *
 * @pre Channel previously initialized.
 * @pre Caller has consumed any pending RX data (FIFO is reset).
 *
 * @post FIFOs are empty and all SSISR error flags cleared.
 * @post SSICR retains its mode bits (only TEN/REN are touched).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ssie_start_recovery(uint8_t channel);

/**
 * @brief Enable or disable mute on the next frame boundary.
 *
 * @details Toggles SSICR.MUEN per HUM Ch 46.2.1 (p 3056). Data is
 * still written to the TX line but its bits are forced to zero.
 *
 * @param[in] channel Channel index.
 * @param[in] enable  true to mute, false to un-mute.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              MUEN updated.
 * @retval k_ra8_err_invalid_arg Channel out of range.
 *
 * @pre Channel previously initialized.
 * @pre IRQs masked.
 *
 * @post SSICR.MUEN reflects ``enable``.
 * @post No other SSICR field touched.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ssie_mute(uint8_t channel, bool enable);

/**
 * @brief Update the FIFO trigger watermarks at runtime.
 *
 * @param[in] channel       Channel index.
 * @param[in] tx_threshold  TDES[4:0]: TX-empty IRQ when TDC <= value.
 * @param[in] rx_threshold  RDFS[4:0]: RX-full IRQ when RDC > value.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              SSISCR updated.
 * @retval k_ra8_err_invalid_arg Channel out of range or threshold > 0x1F.
 *
 * @pre Channel previously initialized.
 * @pre Channel currently idle (SSISR.IIRQ = 1).
 *
 * @post SSISCR.TDES = ``tx_threshold``, SSISCR.RDFS = ``rx_threshold``.
 * @post Other SSISCR bits zero (reserved).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_ssie_set_thresholds(uint8_t channel, uint8_t tx_threshold, uint8_t rx_threshold);

/**
 * @brief Programme SSITDMR / SSIRDMR FIFO thresholds in one call.
 *
 * @details
 * Convenience wrapper that drives ``ra8_ssie_set_thresholds`` with the
 * exact pair of TX / RX FIFO watermarks the audio pipeline wants. The
 * SSIE only carries ``SSISCR.TDES`` / ``SSISCR.RDFS`` (HUM Ch 46.2.8
 * p 3094) -- the names ``SSITDMR`` / ``SSIRDMR`` are aliases the
 * application layer uses for clarity.
 *
 * @param[in] channel       Channel index (0 = SSIE0, 1 = SSIE1).
 * @param[in] tx_threshold  TDES[4:0]: TX-empty IRQ when TDC <= value.
 * @param[in] rx_threshold  RDFS[4:0]: RX-full IRQ when RDC > value.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Thresholds programmed.
 * @retval k_ra8_err_invalid_arg Channel out of range or threshold > 0x1F.
 *
 * @pre Channel previously initialized.
 * @pre Channel currently idle.
 *
 * @post SSISCR reflects the requested thresholds.
 * @post Other SSISCR fields are zero (reserved).
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_ssie_set_thresholds
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_ssie_set_fifo_threshold(uint8_t channel, uint8_t tx_threshold, uint8_t rx_threshold);

/**
 * @brief Pair an SSIE channel's TX + RX FIFOs to DMAC channels.
 *
 * @details
 * Thin variant of ``ra8_ssie_attach_dma`` that takes only the DMAC
 * channel ids (no buffers / counts). Useful for callers who programme
 * the DMAC descriptors themselves and only need the SSIE side to
 * record which DMAC channels to free during ``ra8_ssie_detach_dma``.
 * Pass ``>= 8`` to skip a direction.
 *
 * @param[in] channel        SSIE channel index 0..1.
 * @param[in] tx_dma_channel DMAC channel id pumping TX (or >= 8).
 * @param[in] rx_dma_channel DMAC channel id pumping RX (or >= 8).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Channel binding recorded.
 * @retval k_ra8_err_invalid_arg Channel out of range OR both DMA ids >= 8.
 *
 * @pre Channel previously initialized.
 * @pre At least one of ``tx_dma_channel`` / ``rx_dma_channel`` < 8.
 *
 * @post Internal DMA bookkeeping records the TX / RX DMAC ids.
 * @post No DMAC start was issued (compare ``ra8_ssie_attach_dma``).
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_ssie_attach_dma
 * @see ra8_ssie_detach_dma
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_ssie_attach_dma_pair(uint8_t channel, uint8_t tx_dma_channel, uint8_t rx_dma_channel);

/**
 * @brief Block-mode isochronous send. Drains the buffer fully.
 *
 * @details
 * Loops until every requested sample has been pushed into SSIFTDR.
 * Internally relies on the FIFO depth read-back to throttle, so the
 * routine is bounded by ``samples * k_ra8_ssie_fifo_depth`` SSIFSR reads
 * worst-case (HUM Ch 46.2.4 p 3083). The non-DMA path used by the iso
 * pacing layer.
 *
 * @param[in] channel Channel index 0..1.
 * @param[in] buffer  Source samples.
 * @param[in] samples Number of 32-bit samples to send.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              All ``samples`` queued.
 * @retval k_ra8_err_null_ptr    ``buffer`` was NULL.
 * @retval k_ra8_err_invalid_arg Channel out of range.
 *
 * @pre Channel previously initialized + started in TX mode.
 * @pre ``buffer`` is non-NULL and points to ``samples`` words.
 *
 * @post All ``samples`` words have been pushed into SSIFTDR.
 * @post Remaining FIFO occupancy is implementation-defined.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_ssie_recv_iso
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_ssie_send_iso(uint8_t channel, const uint32_t* buffer, uint16_t samples);

/**
 * @brief Block-mode isochronous receive. Drains up to ``max_samples``.
 *
 * @details
 * Loops until either the destination buffer is full or the SSIE RX
 * FIFO drains. Returns the actual sample count via ``out_got``.
 *
 * @param[in]  channel     Channel index 0..1.
 * @param[out] buffer      Destination buffer.
 * @param[in]  max_samples Capacity of ``buffer`` in 32-bit samples.
 * @param[out] out_got     Receives the number of samples consumed.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Up to ``max_samples`` words consumed.
 * @retval k_ra8_err_null_ptr    ``buffer`` or ``out_got`` was NULL.
 * @retval k_ra8_err_invalid_arg Channel out of range.
 *
 * @pre Channel previously initialized + started in RX mode.
 * @pre ``buffer`` and ``out_got`` are non-NULL.
 *
 * @post ``*out_got <= max_samples``.
 * @post Receive FIFO drained to either empty or ``max_samples`` cap.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_ssie_send_iso
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_ssie_recv_iso(uint8_t channel, uint32_t* buffer, uint16_t max_samples, uint16_t* out_got);

/* =============================================================================
 * Data path
 * =============================================================================
 */

/**
 * @brief Push one 32-bit sample into the transmit FIFO.
 *
 * @details
 * Performs a single 32-bit write to SSIFTDR (HUM Ch 46.2.5 p 3088).
 * Samples wider than the configured DWL are truncated by the FIFO
 * shift logic. Data alignment within the 32-bit word is governed
 * by SSICR.PDTA / SSICR.SDTA.
 *
 * @param[in] channel Channel index (0 = SSIE0, 1 = SSIE1).
 * @param[in] sample  Sample bits.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Sample queued.
 * @retval k_ra8_err_invalid_arg  ``channel`` out of range.
 *
 * @pre Channel was previously initialized.
 * @pre Caller has (or accepts) FIFO space (TDE = 1).
 *
 * @post One write to SSIFTDR has been issued.
 * @post No other registers are modified.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_ssie_read_sample
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ssie_write_sample(uint8_t channel, uint32_t sample);

/**
 * @brief Pop one 32-bit sample from the receive FIFO.
 *
 * @param[in]  channel Channel index.
 * @param[out] out     Receives the sample bits.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Sample read.
 * @retval k_ra8_err_null_ptr     ``out`` was nullptr.
 * @retval k_ra8_err_invalid_arg  ``channel`` out of range.
 *
 * @pre Channel previously initialized, ``out`` non-NULL.
 * @pre Caller has confirmed RDF or non-zero RDC.
 *
 * @post ``*out`` holds the dequeued sample.
 * @post SSIFRDR read pointer advanced by one stage.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_ssie_write_sample
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ssie_read_sample(uint8_t channel, uint32_t* out);

/**
 * @brief Drain a host buffer into the TX FIFO until either runs out.
 *
 * @details
 * Loops while ``samples > 0`` and ``SSIFSR.TDC < depth``. Returns
 * the number of samples actually written via ``out_written``.
 *
 * @param[in]  channel     Channel index.
 * @param[in]  buffer      Source samples.
 * @param[in]  samples     Source-buffer length in samples.
 * @param[out] out_written Receives the number of samples written.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              At least zero samples written.
 * @retval k_ra8_err_null_ptr    ``buffer`` or ``out_written`` was NULL.
 * @retval k_ra8_err_invalid_arg Channel out of range.
 *
 * @pre Channel previously initialized.
 * @pre ``buffer`` and ``out_written`` non-NULL.
 *
 * @post ``*out_written <= samples``.
 * @post SSIFSR.TDE cleared if any sample was written.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ssie_write_buffer(uint8_t         channel,
                                              const uint32_t* buffer,
                                              uint16_t        samples,
                                              uint16_t*       out_written);

/**
 * @brief Empty the RX FIFO into a host buffer until either runs out.
 *
 * @param[in]  channel  Channel index.
 * @param[out] buffer   Destination samples.
 * @param[in]  samples  Destination-buffer length in samples.
 * @param[out] out_read Receives the number of samples consumed.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              At least zero samples consumed.
 * @retval k_ra8_err_null_ptr    ``buffer`` or ``out_read`` was NULL.
 * @retval k_ra8_err_invalid_arg Channel out of range.
 *
 * @pre Channel previously initialized.
 * @pre ``buffer`` and ``out_read`` non-NULL.
 *
 * @post ``*out_read <= samples``.
 * @post SSIFSR.RDF cleared if any sample was read.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_ssie_read_buffer(uint8_t channel, uint32_t* buffer, uint16_t samples, uint16_t* out_read);

/* =============================================================================
 * DMA
 * =============================================================================
 */

/**
 * @brief Attach DMAC channels to the SSIE TX and/or RX FIFOs.
 *
 * @details
 * Programmes a TX DMAC channel (source = caller buffer, dest =
 * SSIFTDR, no dest increment) and an RX DMAC channel (source =
 * SSIFRDR, dest = caller buffer, no source increment) per HUM
 * Ch 46.4.1 "Operation in DMAC Transfer" p 3104. After this call
 * the SSIE TIE/RIE bits are still required to advance the DMA
 * (the FIFO watermark is the request signal).
 *
 * Setting a channel ID >= 8 (the DMAC has only 8 channels) skips
 * that direction.
 *
 * @param[in] channel  Channel index (0 = SSIE0, 1 = SSIE1).
 * @param[in] dma      Non-NULL DMA configuration.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              DMA channels programmed.
 * @retval k_ra8_err_null_ptr    ``dma`` was NULL.
 * @retval k_ra8_err_invalid_arg Channel out of range or DMA samples = 0.
 *
 * @pre Channel previously initialized.
 * @pre DMA channels not currently in use.
 *
 * @post DMAC sources/destinations and counts loaded.
 * @post SSIE FIFO watermarks unchanged.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_ssie_detach_dma
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ssie_attach_dma(uint8_t channel, const ra8_ssie_dma_cfg_t* dma);

/**
 * @brief Tear down DMAC channels attached to the SSIE FIFOs.
 *
 * @param[in] channel Channel index.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              All attached DMA channels stopped.
 * @retval k_ra8_err_invalid_arg Channel out of range.
 *
 * @pre ``ra8_ssie_attach_dma`` previously succeeded for ``channel``.
 * @pre TIE/RIE bits in SSIFCR cleared by caller before call.
 *
 * @post Both DMAC channels stopped.
 * @post Internal DMA bookkeeping cleared.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ssie_detach_dma(uint8_t channel);

/* =============================================================================
 * Status
 * =============================================================================
 */

/**
 * @brief Snapshot SSISR + SSIFSR into a status struct.
 *
 * @param[in]  channel Channel index.
 * @param[out] out     Non-NULL status struct.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Snapshot taken.
 * @retval k_ra8_err_null_ptr     ``out`` was nullptr.
 * @retval k_ra8_err_invalid_arg  ``channel`` out of range.
 *
 * @pre Channel previously initialized, ``out`` non-NULL.
 * @pre IRQs may be enabled (read-only).
 *
 * @post ``*out`` reflects a coherent SSISR/SSIFSR snapshot.
 * @post No registers are modified.
 *
 * @note Thread safety: safe to call from IRQ context.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ssie_get_status(uint8_t channel, ra8_ssie_status_t* out);

/**
 * @brief Clear sticky error flags in SSISR.
 *
 * @details
 * Performs a write-1-to-clear on the bits in ``mask`` that overlap
 * SSISR.{ROIRQ, RUIRQ, TOIRQ, TUIRQ}. Other bits are ignored. See
 * HUM Ch 46.2.2, p 3066-3071 for the per-flag clear protocol.
 *
 * @param[in] channel Channel index.
 * @param[in] mask    Bitmask of flags to clear (use
 *                    ``k_ra8_ssie_mask_err_all`` to clear them all).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Mask written.
 * @retval k_ra8_err_invalid_arg  ``channel`` out of range.
 *
 * @pre Channel initialized.
 * @pre Caller already read SSISR (W1C requires "read 1 then write 0").
 *
 * @post Flags in ``mask`` are cleared.
 * @post Other SSISR bits unchanged.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ssie_clear_status(uint8_t channel, uint32_t mask);

/* =============================================================================
 * Interrupt path
 * =============================================================================
 */

/**
 * @brief Attach a shared event callback for both SSIE channels.
 *
 * @param[in] fn  Callback invoked from ``ra8_ssie_dispatch``.
 * @param[in] ctx Opaque context forwarded to the callback.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always succeeds (handler may be nullptr to detach).
 *
 * @pre None.
 * @pre Storage for the function pointer pair is process-global.
 *
 * @post Subsequent ``ra8_ssie_dispatch`` calls invoke ``fn``.
 * @post Previous handler is replaced.
 *
 * @note Thread safety: not thread-safe (writes two static pointers).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ssie_attach_handler(ra8_ssie_event_fn_t fn, void* ctx);

/**
 * @brief ISR helper: fire the registered event handler.
 *
 * @details
 * Snapshots SSISR for ``channel`` (HUM Ch 51 "Serial Sound Interface
 * Enhanced (SSIE)", p 2731), then invokes the attached event handler.
 * Out-of-range channel indices and unattached slots are silently
 * dropped so spurious IRQs are harmless.
 *
 * @param[in] channel Channel that fired.
 *
 * @pre Channel index in range (out-of-range silently dropped).
 * @pre A handler was attached via ``ra8_ssie_attach_handler``.
 *
 * @post The registered handler ran with the SSISR snapshot.
 * @post No SSISR flags are auto-cleared (caller's responsibility).
 *
 * @note Thread safety: callable from IRQ context.
 * @since 0.1.0
 */
void ra8_ssie_dispatch(uint8_t channel);

/**
 * @brief Update the IRQ enable mask without rewriting SSICR fully.
 *
 * @details
 * Performs read-modify-write on SSICR.{IIEN, ROIEN, RUIEN, TOIEN,
 * TUIEN}. Pass ``k_ra8_ssie_mask_irq_all`` in ``mask`` to enable
 * every IRQ source.
 *
 * @param[in] channel Channel index.
 * @param[in] mask    Mask of bits to set / clear.
 * @param[in] enable  true to set bits, false to clear.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              SSICR updated.
 * @retval k_ra8_err_invalid_arg Channel out of range.
 *
 * @pre Channel initialized.
 * @pre Caller has registered an event handler via
 *      ``ra8_ssie_attach_handler``.
 *
 * @post Requested bits set or cleared in SSICR.
 * @post Other SSICR fields untouched.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ssie_set_irq_enable(uint8_t channel, uint32_t mask, bool enable);

/* =============================================================================
 * Power transition
 * =============================================================================
 */

/**
 * @brief Gate the channel's MSTP bit (preserve register state).
 *
 * @param[in] channel Channel index.
 * @return ``ra8_err_t`` error code.
 *
 * @pre Channel was initialized.
 * @pre IRQs masked.
 *
 * @post MSTP bit set, peripheral clock removed.
 * @post SSICR / SSIOFR contents persist (registers retain values).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ssie_enter_stop(uint8_t channel);

/**
 * @brief Re-ungate the channel's MSTP bit.
 *
 * @param[in] channel Channel index.
 * @return ``ra8_err_t`` error code.
 *
 * @pre Channel was previously gated via ``ra8_ssie_enter_stop``.
 * @pre IRQs masked.
 *
 * @post MSTP bit cleared, peripheral clock restored.
 * @post No SSICR / SSIOFR fields reprogrammed by this call.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ssie_exit_stop(uint8_t channel);

#ifdef __cplusplus
}
#endif
