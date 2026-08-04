/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_sci.h
 * @brief Full-featured Serial Communications Interface driver
 * @ingroup grp_hal_comms
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * full build-out of the SCI peripheral. Replaces the
 * polling-only ``ra8_uart`` stub from with a driver that
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
 * DMA TX / RX land via ``ra8_sci_write_dma`` and
 * ``ra8_sci_read_dma``, which programme the ra8_dma substrate for a
 * byte-stream transfer between a host buffer and the SCI TDR/RDR
 * data registers. ELC trigger routing (one DMAC element per TXI /
 * RXI event) is handled downstream once the NSC layer
 * can annotate the trigger table safely; uses the
 * ``k_ra8_elc_event_none`` software-start path which is what the
 * host-side ra8_fake_dma loop simulates.
 *
 * ## Register layout
 *
 * The driver targets the **SCI_B** variant of the SCI peripheral
 * (HUM Ch 38 "Serial Communications Interface", p 2174 onwards) --
 * 32-bit registers throughout, with CCR0..CCR4 / FCR / CSR / CFCLR
 * replacing the legacy 8-bit SMR / SCR / SSR / SCMR. See
 * ``ra8_sci_regs.h`` for the full layout.
 *
 * ## Threading
 *
 * Not thread-safe. Configuration calls run from single-threaded
 * init context. IRQ callbacks fire from handler mode and must
 * not take any ra8_sci locks.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_dma.h"
#include "ra8_err.h"

/* =============================================================================
 * Types
 * =============================================================================
 */

/**
 * @enum ra8_sci_parity_t
 * @brief Parity mode for ``ra8_sci_cfg_t::parity``.
 */
typedef enum : uint8_t {
  k_ra8_sci_parity_none = 0U, /**< RA8 SCI parity none. */
  k_ra8_sci_parity_even = 1U, /**< RA8 SCI parity even. */
  k_ra8_sci_parity_odd  = 2U, /**< RA8 SCI parity odd.  */
} ra8_sci_parity_t;

/**
 * @enum ra8_sci_stop_bits_t
 * @brief Stop-bit count.
 */
typedef enum : uint8_t {
  k_ra8_sci_stop_1 = 0U, /**< RA8 SCI stop 1. */
  k_ra8_sci_stop_2 = 1U, /**< RA8 SCI stop 2. */
} ra8_sci_stop_bits_t;

/**
 * @enum ra8_sci_data_bits_t
 * @brief Data-bit count.
 */
typedef enum : uint8_t {
  k_ra8_sci_data_7 = 7U, /**< RA8 SCI data 7. */
  k_ra8_sci_data_8 = 8U, /**< RA8 SCI data 8. */
} ra8_sci_data_bits_t;

/**
 * @struct ra8_sci_cfg_t
 * @brief Configuration descriptor for ``ra8_sci_init``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra8_sci_init`` in
 * ``libs/ra8_hal/src/ra8_sci.c``.
 */
typedef struct {
  uint32_t            baud;      /**< Target baud rate in bps. */
  ra8_sci_data_bits_t data_bits; /**< 7 or 8 data bits.        */
  ra8_sci_parity_t    parity;    /**< Parity mode.             */
  ra8_sci_stop_bits_t stop_bits; /**< 1 or 2 stop bits.        */
  uint32_t            pclk_hz;   /**< PCLKB frequency in Hz (used
                                      for BRR calculation). */
} ra8_sci_cfg_t;

/**
 * @enum ra8_sci_err_mask_t
 * @brief Bit mask of SCI error flags.
 */
typedef enum : uint8_t {
  k_ra8_sci_err_none    = 0x00U, /**< RA8 SCI error none. */
  k_ra8_sci_err_overrun = 0x01U, /**< ORER set.           */
  k_ra8_sci_err_framing = 0x02U, /**< FER set.            */
  k_ra8_sci_err_parity  = 0x04U, /**< PER set.            */
} ra8_sci_err_mask_t;

/**
 * @typedef ra8_sci_rx_fn_t
 * @brief RX interrupt callback signature.
 *
 * @param[in] ctx Caller-supplied context.
 * @param[in] byte Received byte.
 */
typedef void (*ra8_sci_rx_fn_t)(void* ctx, uint8_t byte);

/**
 * @typedef ra8_sci_tx_fn_t
 * @brief TX-empty interrupt callback signature.
 *
 * @param[in] ctx Caller-supplied context.
 * @param[out] byte Next byte to transmit.
 * @return ``true`` if ``*byte`` is valid; ``false`` to disable
 * TIE (no more data to send).
 */
typedef bool (*ra8_sci_tx_fn_t)(void* ctx, uint8_t* byte);

/**
 * @enum ra8_sci_dir_t
 * @brief Direction selector for ``ra8_sci_abort``.
 *
 * @details Mirrors FSP's `uart_dir_t` (TX=1, RX=2, BOTH=3) so callers
 * can use one bitmask to abort either direction or both at once.
 */
typedef enum : uint8_t {
  k_ra8_sci_dir_tx   = 0x01U, /**< Cancel an in-flight TX. */
  k_ra8_sci_dir_rx   = 0x02U, /**< Cancel an in-flight RX. */
  k_ra8_sci_dir_both = 0x03U, /**< Cancel both directions. */
} ra8_sci_dir_t;

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise an SCI channel using the descriptor.
 *
 * @details
 * Ungates the channel through ``ra8_mstp_enable``, programs CCR1 /
 * CCR2 (BRR + MDDR) / CCR3 (mode + framing) / CCR4 / FCR registers
 * from ``cfg``, then enables CCR0.TE + CCR0.RE. Errors before the
 * final enable step leave the channel gated.
 *
 * @param[in] channel SCI channel number (0..9).
 * @param[in] cfg Configuration descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Channel ready.
 * @retval k_ra8_err_null_ptr ``cfg`` was NULL.
 * @retval k_ra8_err_invalid_arg ``channel`` > 9 or ``cfg`` has
 * bad field values.
 * @retval k_ra8_err_hw_init_failed ``ra8_mstp_enable`` failed.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_mstp_init`` has been called.
 * @post On success, the channel is clocked, configured, and
 * ready to TX / RX.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_init(uint8_t channel, const ra8_sci_cfg_t* cfg);

/**
 * @brief Tear down a channel -- disable TX/RX, release MSTP.
 *
 * @param[in] channel SCI channel number.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Channel released.
 * @retval k_ra8_err_invalid_arg ``channel`` > 9.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Caller previously called ``ra8_sci_init(channel)``.
 *
 * @post TX/RX disabled; MSTP reference for the channel decremented.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_deinit(uint8_t channel);

/* =============================================================================
 * Polling TX / RX
 * =============================================================================
 */

/**
 * @brief Poll-send one byte (blocking, bounded by ra8_hw_err spin budget).
 *
 * @param[in] channel SCI channel number.
 * @param[in] byte Byte to transmit.
 * @return ``k_ra8_ok`` / ``k_ra8_err_hw_timeout`` / ``k_ra8_err_invalid_arg``.
 *
 * @pre Channel previously initialized.
 * @post On success, the byte has been handed to the TX register.
 *
 * @note Thread safety: not thread-safe with respect to IRQ TX on
 * the same channel.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_putc_polling(uint8_t channel, uint8_t byte);

/**
 * @brief Poll-receive one byte (blocking, bounded spin).
 *
 * @param[in] channel SCI channel number.
 * @param[out] out_byte Received byte on success.
 * @return ``k_ra8_ok`` / ``k_ra8_err_hw_timeout`` / ``k_ra8_err_null_ptr``.
 *
 * @pre ``out_byte`` non-NULL.
 * @pre Channel previously initialized.
 * @post On success, one byte was drained from RDR.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_getc_polling(uint8_t channel, uint8_t* out_byte);

/**
 * @brief Send ``len`` bytes by polling (convenience wrapper).
 *
 * @param[in] channel SCI channel number.
 * @param[in] data Byte buffer.
 * @param[in] len Number of bytes.
 * @return ``k_ra8_ok`` or the first error the inner putc returned.
 *
 * @pre ``data`` non-NULL unless ``len == 0``.
 * @post On success, every byte has been handed to TDR.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_write_polling(uint8_t channel, const uint8_t* data, uint32_t len);

/**
 * @brief Block until the channel's transmit shift register is empty.
 *
 * @details
 * Spins on CSR.TEND (HUM Ch 38.2.17 "CSR : Common Status Register",
 * p 2225). TEND asserts only after both TDR and the shift register are
 * drained, so this call guarantees that every byte previously handed to
 * the SCI has finished clocking out on the wire.
 *
 * The intended use is **before any panic / sleep / WFI sequence** that
 * would gate the SCI clock and silently lose in-flight bytes:
 *
 * @code
 * (void)ra8_sci_write_polling(k_demo_sci_channel, panic_msg, len);
 * (void)ra8_sci_flush(k_demo_sci_channel);  // wait for the wire
 * panic_halt();                            // safe to WFI now
 * @endcode
 *
 * The wait is bounded by ``ra8_hw_wait_flag_set32`` (medium budget --
 * roughly 65k tight-loop iterations), so the call is guaranteed to
 * return in finite time even if TXD is wedged. On the host
 * (`RA8_OFF_TARGET`) the fake does not model the shift-register
 * drain, so this routine returns ``k_ra8_ok`` without polling.
 *
 * @param[in] channel SCI channel (0..9).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok TEND observed high (or fake stub).
 * @retval k_ra8_err_invalid_arg ``channel`` > 9.
 * @retval k_ra8_err_hw_timeout Spin budget elapsed without TEND.
 *
 * @pre Channel previously initialized via ``ra8_sci_init``.
 * @post On success, the SCI transmit shift register is empty -- safe to
 *       drop CCR0.TE, gate the MSTP clock, or execute WFI.
 *
 * @note Thread safety: not thread-safe with respect to ISR-driven TX on
 *       the same channel (the ISR may refill TDR mid-poll). Drain or
 *       abort the async TX first, then flush.
 *
 * @see ra8_sci_write_polling
 * @see ra8_sci_abort
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_flush(uint8_t channel);

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
 * @return ``k_ra8_ok`` / ``k_ra8_err_invalid_arg``.
 *
 * @pre Channel previously initialized.
 * @post On success, SCR.RIE is set (if ``fn`` non-NULL).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_attach_rx_handler(uint8_t channel, ra8_sci_rx_fn_t fn, void* ctx);

/**
 * @brief Install the TX interrupt callback + context.
 *
 * @param[in] channel SCI channel.
 * @param[in] fn Callback fired on TDRE interrupt. Return
 * true with the next byte or false to disable.
 * @param[in] ctx Context passed to the callback.
 * @return ``k_ra8_ok`` / ``k_ra8_err_invalid_arg``.
 *
 * @pre Channel previously initialized.
 * @post On success, SCR.TIE is set (if ``fn`` non-NULL).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_attach_tx_handler(uint8_t channel, ra8_sci_tx_fn_t fn, void* ctx);

/* =============================================================================
 * Error status
 * =============================================================================
 */

/**
 * @brief Read the SSR error bits (ORER, FER, PER).
 *
 * @param[in] channel SCI channel.
 * @param[out] out_mask OR of ``k_ra8_sci_err_*`` values.
 * @return ``k_ra8_ok`` / ``k_ra8_err_null_ptr`` / ``k_ra8_err_invalid_arg``.
 *
 * @pre ``out_mask`` non-NULL.
 * @post No hardware state is modified.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_get_errors(uint8_t channel, uint8_t* out_mask);

/**
 * @brief Clear the SSR error flags via write-zero.
 *
 * @param[in] channel SCI channel.
 * @return ``k_ra8_ok`` / ``k_ra8_err_invalid_arg``.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post ORER, FER, PER read back as 0.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_clear_errors(uint8_t channel);

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
 * @return ``k_ra8_ok`` / ``k_ra8_err_invalid_arg``.
 *
 * @pre Channel initialized.
 * @pre IRQs masked or single-threaded context.
 * @post BRR reflects the new divider.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_set_baud(uint8_t channel, uint32_t baud, uint32_t pclk_hz);

/* =============================================================================
 * Power transition
 * =============================================================================
 */

/**
 * @brief Put the channel into MSTP-gated stop state.
 *
 * @param[in] channel SCI channel.
 * @return ``k_ra8_ok`` / ``k_ra8_err_invalid_arg``.
 *
 * @pre IRQs masked or single-threaded context.
 * @post Channel is MSTP-gated.
 *
 * @warning Callers lose every register setting; pair with
 * ``ra8_sci_exit_stop`` + re-init if reconfiguration is
 * needed.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_enter_stop(uint8_t channel);

/**
 * @brief Exit MSTP-gated stop state; the channel must be re-init'd.
 *
 * @param[in] channel SCI channel.
 * @return ``k_ra8_ok`` / ``k_ra8_err_invalid_arg``.
 *
 * @pre Channel is currently MSTP-gated.
 * @post MSTP bit is cleared; caller must call ``ra8_sci_init`` to
 * restore registers.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_exit_stop(uint8_t channel);

/* =============================================================================
 * Async byte-stream TX / RX (FSP r_sci_b_uart Read/Write parity)
 * =============================================================================
 */

/**
 * @brief Arm an interrupt-driven TX of ``len`` bytes from ``data``.
 *
 * @details
 * Mirrors FSP `R_SCI_B_UART_Write` (r_sci_b_uart.c): installs an
 * internal byte-counting state machine on the channel, sets CCR0.TIE,
 * and returns immediately. Each subsequent TXI then shifts one byte
 * out of TDR and decrements the remaining count; when the count reaches
 * zero, TIE is cleared automatically. If a user TX callback was
 * previously attached via ``ra8_sci_attach_tx_handler``, it is still
 * invoked once per byte (with ``*byte`` already populated from
 * ``data[i]``) so existing flow-control hooks keep working.
 *
 * @param[in] channel SCI channel (0..9).
 * @param[in] data Source buffer; must stay live until the
 * transfer drains or is aborted.
 * @param[in] len Number of bytes to send. Zero is a no-op.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Transfer armed.
 * @retval k_ra8_err_null_ptr ``data`` is NULL with ``len`` > 0.
 * @retval k_ra8_err_invalid_arg ``channel`` > 9 or channel not
 * initialized.
 * @retval k_ra8_err_busy A previous async TX is still draining.
 *
 * @pre Channel previously initialized via ``ra8_sci_init``.
 * @pre ``data`` non-NULL when ``len`` > 0.
 * @post On success, CCR0.TIE = 1 and the per-channel TX state holds
 * ``data`` / ``len``.
 *
 * @note Thread safety: not thread-safe; ISR contention is the caller's
 * responsibility.
 * @see ra8_sci_abort
 * @see ra8_sci_dispatch_txi
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_write(uint8_t channel, const uint8_t* data, uint32_t len);

/**
 * @brief Arm an interrupt-driven RX of ``len`` bytes into ``buf``.
 *
 * @details
 * Mirrors FSP `R_SCI_B_UART_Read` (r_sci_b_uart.c): installs an
 * internal byte-counting RX state machine, sets CCR0.RIE, and returns
 * immediately. Each subsequent RXI copies the byte from RDR into
 * ``buf[index++]`` and decrements ``remaining``. When ``remaining``
 * reaches zero, RIE is cleared automatically. A previously-attached
 * user RX callback is still invoked per byte.
 *
 * @param[in] channel SCI channel (0..9).
 * @param[out] buf Destination buffer; must stay live until the
 * transfer drains or ``ra8_sci_read_stop`` is called.
 * @param[in] len Number of bytes to receive. Zero is a no-op.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Transfer armed.
 * @retval k_ra8_err_null_ptr ``buf`` is NULL with ``len`` > 0.
 * @retval k_ra8_err_invalid_arg ``channel`` > 9 or channel not
 * initialized.
 * @retval k_ra8_err_busy A previous async RX is still draining.
 *
 * @pre Channel previously initialized via ``ra8_sci_init``.
 * @pre ``buf`` non-NULL when ``len`` > 0.
 * @post On success, CCR0.RIE = 1 and the per-channel RX state holds
 * ``buf`` / ``len`` / ``index = 0``.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_sci_read_stop
 * @see ra8_sci_abort
 * @see ra8_sci_dispatch_rxi
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_read(uint8_t channel, uint8_t* buf, uint32_t len);

/**
 * @brief Cancel an in-flight async TX or RX.
 *
 * @details
 * Mirrors FSP `R_SCI_B_UART_Abort` (r_sci_b_uart.c). When
 * ``direction`` includes ``k_ra8_sci_dir_tx``, CCR0.TIE / CCR0.TEIE are
 * cleared and the per-channel TX state is zeroed. When ``direction``
 * includes ``k_ra8_sci_dir_rx``, CCR0.RIE is cleared and the per-channel
 * RX state is zeroed. ``k_ra8_sci_dir_both`` aborts both. CCR0.TE / RE
 * are left alone -- callers may still poll or arm a fresh transfer.
 *
 * @param[in] channel SCI channel (0..9).
 * @param[in] direction TX, RX, or both.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Direction(s) aborted.
 * @retval k_ra8_err_invalid_arg ``channel`` > 9 or ``direction`` is not
 * one of the three defined values.
 *
 * @pre Channel previously initialized.
 * @post The selected direction(s) have ``IE`` bits cleared and the
 * matching per-channel byte counter is zero.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_sci_write
 * @see ra8_sci_read
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_abort(uint8_t channel, ra8_sci_dir_t direction);

/**
 * @brief Stop an in-flight RX and report how many bytes were not
 * yet consumed.
 *
 * @details
 * Mirrors FSP `R_SCI_B_UART_ReadStop` (r_sci_b_uart.c). Clears
 * the per-channel RX byte counter, then writes the value that was
 * just cleared into ``*remaining`` so the caller knows how much of
 * the original ``len`` is still pending. Disarms RIE.
 *
 * @param[in] channel SCI channel (0..9).
 * @param[out] remaining Bytes still pending at stop time. Set to 0
 * if no RX was active.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok RX state cleared, ``*remaining`` updated.
 * @retval k_ra8_err_null_ptr ``remaining`` is NULL.
 * @retval k_ra8_err_invalid_arg ``channel`` > 9.
 *
 * @pre ``remaining`` is non-NULL.
 * @pre Channel previously initialized.
 * @post CCR0.RIE = 0 and the per-channel RX state is zeroed.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_sci_read
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_read_stop(uint8_t channel, uint32_t* remaining);

/**
 * @brief Pure-math conversion from a target baud rate to BRR + clock
 * divider settings.
 *
 * @details
 * Mirrors FSP `R_SCI_B_UART_BaudCalculate` (r_sci_b_uart.c) for
 * the simple non-modulated 16x base-clock path. Implements the
 * formula from HUM Ch 38.2.7 "CCR2 : Common Control Register 2",
 * p 2189 Table 38.7:
 *
 *   N = TCLK / (64 * 2^(2n - 1) * B) - 1
 *
 * with n = ``*clk_div_out`` chosen as the smallest CKS divider for
 * which BRR fits in 8 bits. ``*brr_out`` receives the 8-bit BRR.
 * Pure math only -- this routine does not touch any hardware
 * register. Use ``ra8_sci_set_baud`` to apply the result.
 *
 * @param[in] baud Target baud rate in bps; must be > 0.
 * @param[in] pclk_hz PCLKB frequency in Hz; must be > 0.
 * @param[out] brr_out Computed BRR value (0..255).
 * @param[out] clk_div_out Computed CKS divider exponent (0..3).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok BRR computed within hardware reach.
 * @retval k_ra8_err_null_ptr ``brr_out`` or ``clk_div_out`` is NULL.
 * @retval k_ra8_err_invalid_arg ``baud`` or ``pclk_hz`` is zero, or
 * the requested baud cannot be reached
 * with any CKS divider (BRR > 255).
 *
 * @pre ``brr_out`` and ``clk_div_out`` are non-NULL.
 * @post On success, the pair (``*brr_out``, ``*clk_div_out``) yields
 * an effective baud whose error vs. ``baud`` is < 5 percent for
 * standard 9.6k/19.2k/57.6k/115.2k targets at typical PCLKB rates.
 *
 * @note Thread safety: pure function; safe to call concurrently.
 * @see ra8_sci_set_baud
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_sci_baud_calculate(uint32_t baud, uint32_t pclk_hz, uint16_t* brr_out, uint8_t* clk_div_out);

/**
 * @brief Suspend reception by clearing CCR0.RE.
 *
 * @details
 * Mirrors FSP `R_SCI_B_UART_ReceiveSuspend` (r_sci_b_uart.c)
 * but with a real implementation: FSP returns
 * ``FSP_ERR_UNSUPPORTED`` for SCI_B because the hardware does not
 * have a dedicated "RX-suspend" bit. We approximate it by dropping
 * CCR0.RE -- the receive shift register stops sampling RXD and
 * RXI is silenced. ``ra8_sci_receive_resume`` re-asserts RE.
 *
 * @param[in] channel SCI channel (0..9).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok RX paused (CCR0.RE = 0).
 * @retval k_ra8_err_invalid_arg ``channel`` > 9.
 *
 * @pre Channel previously initialized.
 * @post CCR0.RE = 0; bytes already in RDR remain readable.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_sci_receive_resume
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_receive_suspend(uint8_t channel);

/**
 * @brief Resume reception by setting CCR0.RE.
 *
 * @param[in] channel SCI channel (0..9).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok RX re-armed (CCR0.RE = 1).
 * @retval k_ra8_err_invalid_arg ``channel`` > 9.
 *
 * @pre Channel previously initialized.
 * @post CCR0.RE = 1.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_sci_receive_suspend
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_receive_resume(uint8_t channel);

/* =============================================================================
 * DMA TX / RX
 * =============================================================================
 */

/**
 * @brief Kick off a DMA-backed TX transfer.
 *
 * @details
 * Programmes the ra8_dma substrate to copy ``len`` bytes from
 * ``data[]`` into the channel's TDR register as byte elements
 * (src_inc=true, dst_inc=false). The caller-supplied completion
 * callback fires from DMAC ISR context on transfer-end. The
 * allocated DMAC channel is returned in ``*out_dma_channel`` so
 * the caller can release it via ``ra8_dma_release`` once the
 * transfer is done.
 *
 * Uses ``k_ra8_elc_event_none`` (software-start). Real hardware
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
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Transfer armed.
 * @retval k_ra8_err_null_ptr ``data`` or ``out_dma_channel`` NULL.
 * @retval k_ra8_err_invalid_arg ``channel`` > 9 or ``len`` zero.
 * @retval k_ra8_err_no_mem All DMAC channels in use.
 * @retval k_ra8_err_hw_error Underlying ``ra8_dma_request`` failed.
 *
 * @pre Channel previously initialized via ``ra8_sci_init``.
 * @pre ``ra8_dma_init`` has been called.
 * @pre ``out_dma_channel`` is non-NULL.
 *
 * @post On success, the DMAC channel is programmed and armed.
 * @post ``*out_dma_channel`` holds a valid DMAC channel index.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_sci_read_dma
 * @see ra8_dma_release
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_write_dma(uint8_t               channel,
                                          const uint8_t*        data,
                                          uint16_t              len,
                                          ra8_dma_complete_fn_t on_complete,
                                          void*                 ctx,
                                          uint8_t*              out_dma_channel);

/**
 * @brief Kick off a DMA-backed RX transfer.
 *
 * @details
 * Programmes the ra8_dma substrate to copy ``len`` bytes from the
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
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Transfer armed.
 * @retval k_ra8_err_null_ptr ``out_buf`` or ``out_dma_channel`` NULL.
 * @retval k_ra8_err_invalid_arg ``channel`` > 9 or ``len`` zero.
 * @retval k_ra8_err_no_mem All DMAC channels in use.
 * @retval k_ra8_err_hw_error Underlying ``ra8_dma_request`` failed.
 *
 * @pre Channel previously initialized via ``ra8_sci_init``.
 * @pre ``ra8_dma_init`` has been called.
 * @pre ``out_buf`` and ``out_dma_channel`` are non-NULL.
 *
 * @post On success, the DMAC channel is programmed and armed.
 * @post ``*out_dma_channel`` holds a valid DMAC channel index.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_sci_write_dma
 * @see ra8_dma_release
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_read_dma(uint8_t               channel,
                                         uint8_t*              out_buf,
                                         uint16_t              len,
                                         ra8_dma_complete_fn_t on_complete,
                                         void*                 ctx,
                                         uint8_t*              out_dma_channel);

/* =============================================================================
 * ISR entry points (called from ra8_sci_irq.c)
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
 *
 * @details See implementation for details.
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
void ra8_sci_dispatch_txi(uint8_t channel);

/**
 * @brief RXI dispatch -- hand a received byte to the RX callback.
 *
 * @param[in] channel SCI channel whose RXI fired.
 *
 * @pre Called from ISR context.
 * @post If an RX callback is attached, it has been invoked with
 * the byte read from RDR.
 * @since 0.1.0
 *
 * @details See implementation for details.
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
void ra8_sci_dispatch_rxi(uint8_t channel);

/**
 * @brief ERI dispatch -- clear SSR error flags, invoke optional
 * error callback (none in reserved for 3.1b).
 *
 * @param[in] channel SCI channel whose ERI fired.
 *
 * @pre Called from ISR context.
 * @post SSR error bits are cleared.
 * @since 0.1.0
 *
 * @details See implementation for details.
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
void ra8_sci_dispatch_eri(uint8_t channel);

#ifdef __cplusplus
}
#endif
