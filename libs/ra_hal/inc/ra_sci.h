/**
 * @file ra_sci.h
 * @brief Full-featured Serial Communications Interface driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * full build-out of the SCI peripheral. Replaces the
 * polling-only ``ra_uart`` stub from with a driver that
 * ticks every box on the 14-checkbox template:
 *
 * - Init / Deinit with a full configuration struct.
 * - Polling TX / RX.
 * - Interrupt-driven TX / RX with ring buffers.
 * - Error-status handling (overrun, framing, parity).
 * - Runtime baud-rate reconfigure.
 * - Power-mode enter / exit.
 * - Register-coverage full across the current ``r_sci_regs_t``
 * window.
 *
 * DMA TX / RX land via ``ra_sci_write_dma`` and
 * ``ra_sci_read_dma``, which programme the ra_dma substrate for a
 * byte-stream transfer between a host buffer and the SCI TDR/RDR
 * data registers. ELC trigger routing (one DMAC element per TXI /
 * RXI event) is handled downstream once the NSC layer
 * can annotate the trigger table safely; uses the
 * ``k_ra_elc_event_none`` software-start path which is what the
 * host-side ra_sim_dma loop simulates.
 *
 * ## Register layout
 *
 * The driver targets the **SCI_B** variant of the SCI peripheral
 * (HUM Ch 38 "Serial Communications Interface", p 2174 onwards) --
 * 32-bit registers throughout, with CCR0..CCR4 / FCR / CSR / CFCLR
 * replacing the legacy 8-bit SMR / SCR / SSR / SCMR. See
 * ``ra8d2_sci_regs.h`` for the full layout.
 *
 * ## Threading
 *
 * Not thread-safe. Configuration calls run from single-threaded
 * init context. IRQ callbacks fire from handler mode and must
 * not take any ra_sci locks.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_dma.h"
#include "ra_err.h"

/* =============================================================================
 * Types
 * =============================================================================
 */

/**
 * @enum ra_sci_parity_t
 * @brief Parity mode for ``ra_sci_cfg_t::parity``.
 */
typedef enum : uint8_t {
  k_ra_sci_parity_none = 0U,
  k_ra_sci_parity_even = 1U,
  k_ra_sci_parity_odd  = 2U,
} ra_sci_parity_t;

/**
 * @enum ra_sci_stop_bits_t
 * @brief Stop-bit count.
 */
typedef enum : uint8_t {
  k_ra_sci_stop_1 = 0U,
  k_ra_sci_stop_2 = 1U,
} ra_sci_stop_bits_t;

/**
 * @enum ra_sci_data_bits_t
 * @brief Data-bit count.
 */
typedef enum : uint8_t {
  k_ra_sci_data_7 = 7U,
  k_ra_sci_data_8 = 8U,
} ra_sci_data_bits_t;

/**
 * @struct ra_sci_cfg_t
 * @brief Configuration descriptor for ``ra_sci_init``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra_sci_init`` in
 * ``libs/ra_hal/src/ra_sci.c``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint32_t           baud;      /**< Target baud rate in bps. */
  ra_sci_data_bits_t data_bits; /**< 7 or 8 data bits. */
  ra_sci_parity_t    parity;    /**< Parity mode. */
  ra_sci_stop_bits_t stop_bits; /**< 1 or 2 stop bits. */
  uint32_t           pclk_hz;   /**< PCLKB frequency in Hz (used
                                      for BRR calculation). */
} ra_sci_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @enum ra_sci_err_mask_t
 * @brief Bit mask of SCI error flags.
 */
typedef enum : uint8_t {
  k_ra_sci_err_none    = 0x00U,
  k_ra_sci_err_overrun = 0x01U, /**< ORER set. */
  k_ra_sci_err_framing = 0x02U, /**< FER set. */
  k_ra_sci_err_parity  = 0x04U, /**< PER set. */
} ra_sci_err_mask_t;

/**
 * @typedef ra_sci_rx_fn_t
 * @brief RX interrupt callback signature.
 *
 * @param[in] ctx Caller-supplied context.
 * @param[in] byte Received byte.
 */
typedef void (*ra_sci_rx_fn_t)(void* ctx, uint8_t byte);

/**
 * @typedef ra_sci_tx_fn_t
 * @brief TX-empty interrupt callback signature.
 *
 * @param[in] ctx Caller-supplied context.
 * @param[out] byte Next byte to transmit.
 * @return ``true`` if ``*byte`` is valid; ``false`` to disable
 * TIE (no more data to send).
 */
typedef bool (*ra_sci_tx_fn_t)(void* ctx, uint8_t* byte);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise an SCI channel using the descriptor.
 *
 * @details
 * Ungates the channel through ``ra_mstp_enable``, programs CCR1 /
 * CCR2 (BRR + MDDR) / CCR3 (mode + framing) / CCR4 / FCR registers
 * from ``cfg``, then enables CCR0.TE + CCR0.RE. Errors before the
 * final enable step leave the channel gated.
 *
 * @param[in] channel SCI channel number (0..9).
 * @param[in] cfg Configuration descriptor.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Channel ready.
 * @retval k_ra_err_null_ptr ``cfg`` was NULL.
 * @retval k_ra_err_invalid_arg ``channel`` > 9 or ``cfg`` has
 * bad field values.
 * @retval k_ra_err_hw_init_failed ``ra_mstp_enable`` failed.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra_mstp_init`` has been called.
 * @post On success, the channel is clocked, configured, and
 * ready to TX / RX.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sci_init(uint8_t channel, const ra_sci_cfg_t* cfg);

/**
 * @brief Tear down a channel -- disable TX/RX, release MSTP.
 *
 * @param[in] channel SCI channel number.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Channel released.
 * @retval k_ra_err_invalid_arg ``channel`` > 9.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Caller previously called ``ra_sci_init(channel)``.
 *
 * @post TX/RX disabled; MSTP reference for the channel decremented.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sci_deinit(uint8_t channel);

/* =============================================================================
 * Polling TX / RX
 * =============================================================================
 */

/**
 * @brief Poll-send one byte (blocking, bounded by ra_hw_err spin budget).
 *
 * @param[in] channel SCI channel number.
 * @param[in] byte Byte to transmit.
 * @return ``k_ra_ok`` / ``k_ra_err_hw_timeout`` / ``k_ra_err_invalid_arg``.
 *
 * @pre Channel previously initialised.
 * @post On success, the byte has been handed to the TX register.
 *
 * @note Thread safety: not thread-safe with respect to IRQ TX on
 * the same channel.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sci_putc_polling(uint8_t channel, uint8_t byte);

/**
 * @brief Poll-receive one byte (blocking, bounded spin).
 *
 * @param[in] channel SCI channel number.
 * @param[out] out_byte Received byte on success.
 * @return ``k_ra_ok`` / ``k_ra_err_hw_timeout`` / ``k_ra_err_null_ptr``.
 *
 * @pre ``out_byte`` non-NULL.
 * @pre Channel previously initialised.
 * @post On success, one byte was drained from RDR.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sci_getc_polling(uint8_t channel, uint8_t* out_byte);

/**
 * @brief Send ``len`` bytes by polling (convenience wrapper).
 *
 * @param[in] channel SCI channel number.
 * @param[in] data Byte buffer.
 * @param[in] len Number of bytes.
 * @return ``k_ra_ok`` or the first error the inner putc returned.
 *
 * @pre ``data`` non-NULL unless ``len == 0``.
 * @post On success, every byte has been handed to TDR.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sci_write_polling(uint8_t channel, const uint8_t* data, uint32_t len);

/* =============================================================================
 * Interrupt TX / RX
 * =============================================================================
 */

/**
 * @brief Install the RX interrupt callback + context.
 *
 * @param[in] channel SCI channel.
 * @param[in] fn Callback fired on RDRF interrupt. Must not
 * be NULL to enable; pass NULL to detach.
 * @param[in] ctx Context passed to the callback.
 * @return ``k_ra_ok`` / ``k_ra_err_invalid_arg``.
 *
 * @pre Channel previously initialised.
 * @post On success, SCR.RIE is set (if ``fn`` non-NULL).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sci_attach_rx_handler(uint8_t channel, ra_sci_rx_fn_t fn, void* ctx);

/**
 * @brief Install the TX interrupt callback + context.
 *
 * @param[in] channel SCI channel.
 * @param[in] fn Callback fired on TDRE interrupt. Return
 * true with the next byte or false to disable.
 * @param[in] ctx Context passed to the callback.
 * @return ``k_ra_ok`` / ``k_ra_err_invalid_arg``.
 *
 * @pre Channel previously initialised.
 * @post On success, SCR.TIE is set (if ``fn`` non-NULL).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sci_attach_tx_handler(uint8_t channel, ra_sci_tx_fn_t fn, void* ctx);

/* =============================================================================
 * Error status
 * =============================================================================
 */

/**
 * @brief Read the SSR error bits (ORER, FER, PER).
 *
 * @param[in] channel SCI channel.
 * @param[out] out_mask OR of ``k_ra_sci_err_*`` values.
 * @return ``k_ra_ok`` / ``k_ra_err_null_ptr`` / ``k_ra_err_invalid_arg``.
 *
 * @pre ``out_mask`` non-NULL.
 * @post No hardware state is modified.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sci_get_errors(uint8_t channel, uint8_t* out_mask);

/**
 * @brief Clear the SSR error flags via write-zero.
 *
 * @param[in] channel SCI channel.
 * @return ``k_ra_ok`` / ``k_ra_err_invalid_arg``.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post ORER, FER, PER read back as 0.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sci_clear_errors(uint8_t channel);

/* =============================================================================
 * Runtime reconfigure
 * =============================================================================
 */

/**
 * @brief Change the baud rate without tearing down the channel.
 *
 * @param[in] channel SCI channel.
 * @param[in] baud New target baud rate in bps.
 * @param[in] pclk_hz Current PCLKB frequency in Hz.
 * @return ``k_ra_ok`` / ``k_ra_err_invalid_arg``.
 *
 * @pre Channel initialised.
 * @pre IRQs masked or single-threaded context.
 * @post BRR reflects the new divider.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sci_set_baud(uint8_t channel, uint32_t baud, uint32_t pclk_hz);

/* =============================================================================
 * Power transition
 * =============================================================================
 */

/**
 * @brief Put the channel into MSTP-gated stop state.
 *
 * @param[in] channel SCI channel.
 * @return ``k_ra_ok`` / ``k_ra_err_invalid_arg``.
 *
 * @pre IRQs masked or single-threaded context.
 * @post Channel is MSTP-gated.
 *
 * @warning Callers lose every register setting; pair with
 * ``ra_sci_exit_stop`` + re-init if reconfiguration is
 * needed.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sci_enter_stop(uint8_t channel);

/**
 * @brief Exit MSTP-gated stop state; the channel must be re-init'd.
 *
 * @param[in] channel SCI channel.
 * @return ``k_ra_ok`` / ``k_ra_err_invalid_arg``.
 *
 * @pre Channel is currently MSTP-gated.
 * @post MSTP bit is cleared; caller must call ``ra_sci_init`` to
 * restore registers.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sci_exit_stop(uint8_t channel);

/* =============================================================================
 * DMA TX / RX
 * =============================================================================
 */

/**
 * @brief Kick off a DMA-backed TX transfer.
 *
 * @details
 * Programmes the ra_dma substrate to copy ``len`` bytes from
 * ``data[]`` into the channel's TDR register as byte elements
 * (src_inc=true, dst_inc=false). The caller-supplied completion
 * callback fires from DMAC ISR context on transfer-end. The
 * allocated DMAC channel is returned in ``*out_dma_channel`` so
 * the caller can release it via ``ra_dma_release`` once the
 * transfer is done.
 *
 * Uses ``k_ra_elc_event_none`` (software-start). Real hardware
 * one-element-per-TXI routing is a task alongside the
 * TrustZone retrofit.
 *
 * @param[in] channel SCI channel 0..9.
 * @param[in] data Source byte buffer. Must stay
 * live until ``on_complete`` fires.
 * @param[in] len Number of bytes to transfer; must
 * be non-zero.
 * @param[in] on_complete Completion callback. May be NULL.
 * @param[in] ctx Context passed to ``on_complete``.
 * @param[out] out_dma_channel Allocated DMAC channel on success.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Transfer armed.
 * @retval k_ra_err_null_ptr ``data`` or ``out_dma_channel`` NULL.
 * @retval k_ra_err_invalid_arg ``channel`` > 9 or ``len`` zero.
 * @retval k_ra_err_no_mem All DMAC channels in use.
 * @retval k_ra_err_hw_error Underlying ``ra_dma_request`` failed.
 *
 * @pre Channel previously initialised via ``ra_sci_init``.
 * @pre ``ra_dma_init`` has been called.
 * @pre ``out_dma_channel`` is non-NULL.
 *
 * @post On success, the DMAC channel is programmed and armed.
 * @post ``*out_dma_channel`` holds a valid DMAC channel index.
 *
 * @note Thread safety: not thread-safe.
 * @see ra_sci_read_dma
 * @see ra_dma_release
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sci_write_dma(uint8_t              channel,
                                        const uint8_t*       data,
                                        uint16_t             len,
                                        ra_dma_complete_fn_t on_complete,
                                        void*                ctx,
                                        uint8_t*             out_dma_channel);

/**
 * @brief Kick off a DMA-backed RX transfer.
 *
 * @details
 * Programmes the ra_dma substrate to copy ``len`` bytes from the
 * channel's RDR register into ``out_buf[]`` as byte elements
 * (src_inc=false, dst_inc=true). Completion callback fires from
 * DMAC ISR context on transfer-end.
 *
 * @param[in] channel SCI channel 0..9.
 * @param[out] out_buf Destination byte buffer. Must stay
 * live until ``on_complete`` fires.
 * @param[in] len Number of bytes; must be non-zero.
 * @param[in] on_complete Completion callback. May be NULL.
 * @param[in] ctx Context passed to ``on_complete``.
 * @param[out] out_dma_channel Allocated DMAC channel on success.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Transfer armed.
 * @retval k_ra_err_null_ptr ``out_buf`` or ``out_dma_channel`` NULL.
 * @retval k_ra_err_invalid_arg ``channel`` > 9 or ``len`` zero.
 * @retval k_ra_err_no_mem All DMAC channels in use.
 * @retval k_ra_err_hw_error Underlying ``ra_dma_request`` failed.
 *
 * @pre Channel previously initialised via ``ra_sci_init``.
 * @pre ``ra_dma_init`` has been called.
 * @pre ``out_buf`` and ``out_dma_channel`` are non-NULL.
 *
 * @post On success, the DMAC channel is programmed and armed.
 * @post ``*out_dma_channel`` holds a valid DMAC channel index.
 *
 * @note Thread safety: not thread-safe.
 * @see ra_sci_write_dma
 * @see ra_dma_release
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sci_read_dma(uint8_t              channel,
                                       uint8_t*             out_buf,
                                       uint16_t             len,
                                       ra_dma_complete_fn_t on_complete,
                                       void*                ctx,
                                       uint8_t*             out_dma_channel);

/* =============================================================================
 * ISR entry points (called from ra_sci_irq.c)
 * =============================================================================
 */

/**
 * @brief TXI dispatch -- advance the TX callback.
 *
 * @param[in] channel SCI channel whose TXI fired.
 *
 * @pre Called from ISR context (or from test helper).
 * @post If the attached TX callback returns true, the next byte
 * has been written to TDR. Otherwise TIE is cleared.
 * @since 0.1.0
 */
void ra_sci_dispatch_txi(uint8_t channel);

/**
 * @brief RXI dispatch -- hand a received byte to the RX callback.
 *
 * @param[in] channel SCI channel whose RXI fired.
 *
 * @pre Called from ISR context.
 * @post If an RX callback is attached, it has been invoked with
 * the byte read from RDR.
 * @since 0.1.0
 */
void ra_sci_dispatch_rxi(uint8_t channel);

/**
 * @brief ERI dispatch -- clear SSR error flags, invoke optional
 * error callback (none in reserved for 3.1b).
 *
 * @param[in] channel SCI channel whose ERI fired.
 *
 * @pre Called from ISR context.
 * @post SSR error bits are cleared.
 * @since 0.1.0
 */
void ra_sci_dispatch_eri(uint8_t channel);

#ifdef __cplusplus
}
#endif
