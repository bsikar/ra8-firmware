/**
 * @file examples/ek_ra8d2/hw_pending/da16600_probe/src/da16600_probe_steps.h
 * @brief Shared contract between the DA16600 probe main ladder and its
 *        physical-layer diagnostic rungs.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The DA16600 bring-up probe is split across two translation units to stay
 * under the file-size gate. ``main.c`` owns boot, the AT transport, and the
 * Wi-Fi / BLE / TCP ladder rungs; the wire / U15-expander / edge-sampler
 * diagnostic rungs live in ``da16600_probe_steps.c``. This header is the
 * single seam between them: it exposes the compile-time tunables enum, the
 * console helpers the diagnostics call back into, the one shared mutable PCLKA
 * cache, and the prototypes ``main()`` needs to drive the diagnostic rungs.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra_err.h"
#include "ra_port_constants.h"

/* =============================================================================
 * Tunables (shared)
 * =============================================================================
 */

/**
 * @enum da16600_probe_const_t
 * @brief Compile-time settings for the bring-up ladder.
 */
typedef enum : uint32_t {
  k_probe_decimal_base    = 10U,     /**< Radix for integer-to-ASCII.          */
  k_probe_u16_max_digits  = 5U,      /**< Max decimal digits in a uint16_t.    */
  k_probe_u16_str_len     = 6U,      /**< Digits + NUL for a uint16_t string.  */
  k_probe_hex_str_len     = 5U,      /**< 4 hex digits + NUL.                  */
  k_probe_hex_nibble      = 4U,      /**< Bits per hex digit.                  */
  k_probe_hex_mask        = 0xFU,    /**< One hex digit.                       */
  k_probe_at_timeout_ms   = 2000U,   /**< Default AT command timeout.          */
  k_probe_baud            = 115200U, /**< Console + DA16600 UART baud.         */
  k_probe_baud_alt1       = 230400U, /**< Sweep: DA16600 debug-console rate.   */
  k_probe_baud_alt2       = 9600U,   /**< Sweep: conservative fallback.        */
  k_probe_baud_alt3       = 57600U,  /**< Sweep: legacy default.               */
  k_probe_baud_alt4       = 921600U, /**< Sweep: high-rate option.             */
  k_probe_da16600_sci_ch  = 2U,      /**< Pmod1 UART = SCI2 (TXD2/RXD2).       */
  k_probe_at_line_buf_len = 256U,    /**< AT-line accumulator length.          */
  k_probe_boot_settle_ms  = 2500U,   /**< DA16600 cold-boot settle window.     */
  k_probe_sweep_window_ms = 500U,    /**< RX listen window per baud sweep.     */
  k_probe_reset_pulse_ms  = 200U,    /**< RESET low-pulse width.               */
  k_probe_reset_boot_ms   = 2000U,   /**< Post-reset boot wait (DA16600 cold). */
  k_probe_banner_wait_ms  = 6000U,   /**< Boot-banner listen window.           */
  k_probe_u15_iic_ch      = 1U,      /**< U15 expander IIC_B channel.          */
  k_probe_u15_addr        = 0x43U,   /**< U15 PI4IOE5V6408 7-bit address.      */
  k_probe_u15_reg_iodir   = 0x03U,   /**< U15 direction (1=output).            */
  k_probe_u15_reg_out     = 0x05U,   /**< U15 output-latch register.           */
  k_probe_u15_reg_hiz     = 0x07U,   /**< U15 output Hi-Z (1=Hi-Z).            */
  k_probe_u15_reg_input   = 0x0FU,   /**< U15 input-level register.            */
  k_probe_u15_default     = 0xF2U,   /**< Project-default latch (BSP value).   */
  k_probe_loop_pattern    = 0xA5U,   /**< Loopback test byte.                  */
  k_probe_tx_test_bytes   = 24U,     /**< 0x00 frames sent during pin sampling.*/
  k_probe_tx_samples      = 400U,    /**< PIDR samples per transmitted frame.  */
  k_probe_txd2_pin_bit    = 1U,      /**< P801 bit within PORT8 PIDR.          */
  k_probe_rxd2_pin_bit    = 2U,      /**< P802 bit within PORT8 PIDR.          */
  k_probe_ard_rx_pin_bit  = 8U,      /**< P808 (Arduino D0/RXD7) PIDR bit.     */
  k_probe_ard_tx_pin_bit  = 9U,      /**< P809 (Arduino D1/TXD7) PIDR bit.     */
  k_probe_sci7_ch         = 7U,      /**< Arduino D0/D1 UART = SCI7.           */
  k_probe_echo_port       = 7U,      /**< RFC 862 TCP echo port.               */
  k_probe_echo_buf_len    = 128U,    /**< Echo payload buffer.                 */
  k_probe_recv_window_ms  = 10000U,  /**< Per-pass TCP recv wait.              */
  k_probe_edge_window_ms  = 8000U,   /**< Edge-sampler listen window.          */
  k_probe_u15_sweep_base  = 0xF8U,   /**< Focused sweep base (bits 3-7 high).  */
  k_probe_u15_sweep_n     = 8U,      /**< All combos of latch bits 0-2.        */
  k_probe_u15_full_n      = 256U,    /**< Exhaustive latch state space.        */
  k_probe_u15_settle_ms   = 150U,    /**< Mux/level settle after a latch write.*/
  k_probe_quick_at_ms     = 400U,    /**< Short AT window inside the sweep.    */
  k_probe_baud_div        = 100U,    /**< Sweep label divisor (fits uint16).   */
  k_probe_ascii_del       = 0x7FU,   /**< First non-printable upper ASCII.     */
  k_probe_pass_period_ms  = 5000U,   /**< Idle heartbeat between ladder runs.  */
  k_probe_p602_pin_bit    = 2U,      /**< Pmod2 RXD0 (P602) bit in PORT6 PIDR.  */
  k_probe_p603_pin_bit    = 3U,      /**< Pmod2 TXD0 (P603) bit in PORT6 PIDR.  */
  k_probe_u15_all_high    = 0xFFU,   /**< Drive-test latch: all outputs HIGH.   */
  k_probe_u15_all_low     = 0x00U,   /**< Drive-test latch: all outputs LOW.    */
  k_probe_wake_pulse_ms   = 20U,     /**< Hold time per wake pulse level.       */
  k_probe_wake_cycles     = 3U,      /**< Wake L->H->L pulses to emit.          */
  k_probe_wake_settle_ms  = 200U,    /**< Settle after wake before AT.          */
} da16600_probe_const_t;

/* =============================================================================
 * Shared mutable state
 * =============================================================================
 */

/**
 * @var s_da16600_probe_pclka_hz
 * @brief Cached PCLKA rate (Hz) for late SCI bring-up.
 *
 * @details Written once by the boot path in ``main.c`` and read by the SCI
 * re-init paths in both translation units (baud sweep, Arduino UART hunt).
 *
 * @note Single-writer at boot; read-only thereafter, so no synchronization
 *       is required in this single-threaded bare-metal app.
 * @warning Do not modify after boot; the SCI baud generators depend on it.
 * @since 0.1.0
 */
extern uint32_t s_da16600_probe_pclka_hz;

/* =============================================================================
 * Console helpers (defined in main.c, called from the diagnostic rungs)
 * =============================================================================
 */

/**
 * @brief Write a NUL-terminated string to the VCOM console.
 *
 * @details Thin wrapper over ::ra_board_uart_console_write.
 *
 * @param[in] s NUL-terminated ASCII string.
 *
 * @pre Console is initialized.
 * @pre @p s is non-NULL and NUL-terminated.
 * @post The bytes have been queued on SCI8.
 * @post No other state changes.
 * @note Blocking polled write.
 * @since 0.1.0
 */
void probe_log(const char* s);

/**
 * @brief Log a 16-bit value as 4 uppercase hex digits.
 *
 * @details Used to surface ::ra_err_t codes for remote diagnosis.
 *
 * @param[in] v Value to print.
 *
 * @pre Console is initialized.
 * @pre None beyond console readiness.
 * @post Four hex digits have been written to the console.
 * @post No other state changes.
 * @note Blocking polled write.
 * @since 0.1.0
 */
void probe_log_hex16(uint16_t v);

/**
 * @brief Decimal-format @p value into @p dst (libc-free, NUL-terminated).
 *
 * @details Worst case "65535" + NUL = 6 bytes.
 *
 * @param[out] dst   Caller buffer (must hold at least 6 bytes).
 * @param[in]  value Value to format.
 *
 * @pre @p dst points to at least 6 writable bytes.
 * @pre None beyond the buffer contract.
 * @post @p dst holds the NUL-terminated decimal string.
 * @post No other state changes.
 * @note Pure function apart from the output buffer.
 * @since 0.1.0
 */
void probe_format_u16(char* dst, uint16_t value);

/* =============================================================================
 * Diagnostic rungs (defined in da16600_probe_steps.c, driven from main.c)
 * =============================================================================
 */

/**
 * @brief Rung 0: physical-layer survey of the five Pmod1 P80x lines.
 *
 * @details Must run BEFORE any SCI routing so the GPIO claims succeed.
 *
 * @pre Console is initialized; no P80x pin is claimed yet.
 * @pre ::ra_time_init has been called.
 * @post Five survey lines have been printed.
 * @post The pins remain claimed as GPIO inputs (re-routed later).
 * @note Diagnostic only; results are human-read over VCOM.
 * @since 0.1.0
 */
void probe_rung_wire(void);

/**
 * @brief Try one baud: send ``AT`` (CRLF) raw and hex-dump the reply.
 *
 * @param[in] baud UART bit-rate to test.
 *
 * @pre SCI2 pins are routed; console is initialized.
 * @pre ::ra_time_init has been called.
 * @post One sweep line has been printed.
 * @post SCI2 is left configured at @p baud.
 * @note Diagnostic only.
 * @since 0.1.0
 */
void probe_sweep_baud(uint32_t baud);

/**
 * @brief Read back U15's output latch and physical input levels.
 *
 * @pre The SW4 override ran (IIC_B1 is initialized, U15 ACKs).
 * @pre Console is up.
 * @post One status line with both register values has been printed.
 * @post No U15 state is modified.
 * @note Diagnostic only.
 * @since 0.1.0
 */
void probe_rung_u15_readback(void);

/**
 * @brief Empirically decide whether U15's input register tracks its outputs.
 *
 * @pre ::probe_rung_u15_readback has run; IIC_B1 is up.
 * @pre Console is initialized.
 * @post U15 output latch restored to the project default (0xF2).
 * @post Two probe lines have been printed.
 * @note Diagnostic only.
 * @since 0.1.0
 */
void probe_u15_drive_test(void);

/**
 * @brief Hard-reset the DA16600 via Pmod1.8 (P402) and dump its banner.
 *
 * @pre SCI2 is initialized at the listen baud; console is up.
 * @pre P402 is unclaimed.
 * @post One reset cycle has been issued; the dump line is printed.
 * @post P402 is held a driven output-high (reset de-asserted).
 * @note Diagnostic only.
 * @since 0.1.0
 */
void probe_rung_reset_banner(void);

/**
 * @brief Replay the boot-window RX capture on the console.
 *
 * @pre Console is initialized.
 * @pre ::probe_rung_reset_banner has run once.
 * @post One replay line has been printed.
 * @post The boot-log capture is unmodified.
 * @note Diagnostic only.
 * @since 0.1.0
 */
void probe_replay_bootlog(void);

/**
 * @brief Sweep all 8 combinations of U15 latch bits 0-2, AT-probing each.
 *
 * @pre SCI2 + console are initialized; U15 is configured.
 * @pre ::ra_time_init has been called.
 * @post Eight result lines have been printed.
 * @post The latch is left at the last RX-traffic value, or the default.
 * @note Diagnostic only.
 * @since 0.1.0
 */
void probe_rung_u15_sweep(void);

/**
 * @brief Exhaustive sweep: every U15 latch value 0x00..0xFF, AT each.
 *
 * @pre SCI2 + console are initialized; U15 is configured.
 * @pre ::ra_time_init has been called.
 * @post A summary line has been printed.
 * @post The latch holds the winner, or the project default.
 * @note Runs ~2.5 minutes; diagnostic only.
 * @since 0.1.0
 */
void probe_rung_u15_full_sweep(void);

/**
 * @brief Self-test SCI2 with zero external dependencies.
 *
 * @pre SCI2 is initialized at the working baud; console is up.
 * @pre P801 is routed to TXD2 (peripheral mode).
 * @post Two verdict lines have been printed.
 * @post SCI2 is restored to normal (non-loopback) mode.
 * @note Diagnostic only.
 * @since 0.1.0
 */
void probe_rung_sci2_self(void);

/**
 * @brief GPIO logic-analyzer: watch P801+P802 raw across a module reset.
 *
 * @pre Console is up; ::ra_time_init has been called.
 * @pre The Pmod1 pins are claimable.
 * @post One report line has been printed.
 * @post P801/P802 are released (caller re-routes them to SCI2).
 * @note Diagnostic only.
 * @since 0.1.0
 */
void probe_rung_edge_sampler(void);

/**
 * @brief GPIO logic-analyzer for Pmod2 (J25): watch P602/P603 across a reset.
 *
 * @pre Console is up; ::ra_time_init has been called.
 * @pre PORT6 P602/P603/P604 and P410 are claimable.
 * @post One report line has been printed.
 * @post P602/P603 are released; P604 stays driven LOW; P410 driven HIGH.
 * @note Diagnostic only.
 * @since 0.1.0
 */
void probe_rung_pmod2_edge(void);

/**
 * @brief Alt-UART hunt: probe SCI7 on the Arduino D0/D1 header pins.
 *
 * @pre Console is up; P808/P809 are claimable.
 * @pre ::ra_time_init has been called.
 * @post Two verdict lines have been printed.
 * @post SCI7 stays configured (harmless).
 * @note Diagnostic only.
 * @since 0.1.0
 */
void probe_rung_arduino_uart(void);

/**
 * @brief Pulse the DA16600 host-wake lines (Pmod1 GPIO_A/GPIO_B) Low->High->Low.
 *
 * @pre Console + ::ra_time_init are up; P412/P413 are claimable.
 * @post Both lines have seen ::k_probe_wake_cycles L->H->L pulses, left LOW.
 * @post Both pins are released for re-claim on the next pass.
 * @note Diagnostic / bring-up; harmless if the pins are not the wake line.
 * @since 0.1.0
 */
void probe_wake_pulse(void);
