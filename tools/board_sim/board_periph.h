/**
 * @file board_periph.h
 * @brief Register-accurate peripheral-model framework for the board emulator
 *
 * @details
 * A registry that maps RA8D2 peripheral-register address ranges to per-block
 * read/write handlers backed by real state, dispatched from board_sim's MMIO
 * callbacks. It SUPERSEDES the sparse reflect-then-settle fallback for the
 * blocks modelled here (the fallback still answers every UNmodelled address),
 * so a non-display example produces real peripheral data instead of faked
 * ready-bit handshakes.
 *
 * The first blocks modelled are GPIO/PORT (the board LEDs become observable),
 * the AGT and GPT timers (counters that advance on their configured clock and
 * raise compare-match / overflow / underflow events), the ICU/NVIC (a
 * peripheral event linked through IELSR pends the matching NVIC IRQ, taken as a
 * real Cortex-M exception by the engine's exception layer), and the SCI_B
 * UART (TDR writes are captured to a host console sink, RDR reads return a
 * host-supplied byte stream, and TXI / TEI / RXI route through the same ICU
 * event path so interrupt-driven serial works as well as polled). The block
 * table is the extension point: I2C / SPI / USB slot in as new entries later.
 *
 * Design: this module owns no Unicorn engine of its own and takes no AppKit
 * dependency. main.c passes the engine in where the model must read or write
 * emulated memory / pend an NVIC line, so board_periph stays plain C and the
 * exception delivery stays in the one place that already models it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>
#include <unicorn/unicorn.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Board user-LED identity, mirrored from the EK-RA8D2 BSP.
 *
 * @details
 * Pin assignments per libs/ra_board_ek_ra8d2 (EK-RA8D2 v1 UM Table 24, p 31):
 * LED1 BLUE = P600, LED2 GREEN = P303, LED3 RED = PA07. All three are
 * active-high. board_periph traces these specific port/pin output latches so
 * the run summary / --trace can report each LED transition.
 */
typedef enum : uint8_t {
  k_board_led1      = 0U, /**< LED1, BLUE,  P600 (port 6, pin 0).  */
  k_board_led2      = 1U, /**< LED2, GREEN, P303 (port 3, pin 3).  */
  k_board_led3      = 2U, /**< LED3, RED,   PA07 (port 10, pin 7). */
  k_board_led_count = 3U,
} board_led_id_t;

/**
 * @brief One-time reset of all peripheral-model state.
 *
 * @details
 * Clears every modelled block (PORT latches/direction, AGT/GPT counters and
 * status, ICU event-link table and NVIC pend records) and the observability
 * counters. Call once after the memory map is created and before the run loop.
 *
 * @param[in] trace When true, each LED / GPIO transition and each taken IRQ is
 *                   logged to stderr as it happens (the --trace flag).
 * @return Nothing.
 * @post All counters read zero and every block is in its reset state.
 * @since 0.1.0
 */
void board_periph_init(bool trace);

/**
 * @brief Wire a host sink that receives every byte the firmware transmits.
 *
 * @details
 * The SCI_B model calls @p sink once per byte written to a channel's TDR (the
 * transmit-data register, used by both the polled and interrupt TX paths and by
 * FIFO mode, which also writes TDR). main.c installs a sink that prints each
 * byte to stdout with a clear @c [uart] prefix so a console example's output is
 * captured and greppable. When no sink is installed, transmitted bytes are
 * still counted for the end-of-run summary but not echoed.
 *
 * @param[in] sink Callback invoked as @c sink(channel, byte) per TX byte, or
 *                 NULL to detach. The model owns no copy of @p byte.
 * @return Nothing.
 * @post Subsequent TDR writes are delivered to @p sink.
 * @since 0.1.0
 */
void board_periph_sci_set_tx_sink(void (*sink)(uint8_t channel, uint8_t byte));

/**
 * @brief Queue host->firmware bytes for a channel's receive path.
 *
 * @details
 * Appends @p len bytes to the channel's RX queue. The SCI_B model asserts
 * CSR.RDRF (and the FIFO RX data flags) while the queue is non-empty, returns
 * queued bytes from reads of RDR oldest-first, and -- if the firmware armed
 * RXI (CCR0.RIE) and routed the channel's RXI event through the ICU -- pends the
 * RXI interrupt so interrupt-driven receive also runs. main.c feeds this from
 * @c --input and/or stdin so a console example sees real input. Bytes beyond
 * the per-channel queue capacity are dropped (reported on @p --trace).
 *
 * @param[in] channel SCI channel index (0..9). Out-of-range is ignored.
 * @param[in] data    Source bytes (copied into the queue); ignored if NULL.
 * @param[in] len     Number of bytes to queue.
 * @return Nothing.
 * @post Up to the queue's free space of @p data is readable via RDR and RDRF
 *       reflects availability.
 * @since 0.1.0
 */
void board_periph_sci_feed_rx(uint8_t channel, const uint8_t* data, uint32_t len);

/**
 * @brief The SCI channel the EK-RA8D2 console (J-Link OB VCOM) uses.
 *
 * @details
 * PD02 TXD / PD03 RXD route to SCI8 on the EK-RA8D2 v1, surfaced as the board's
 * debug-console UART (mirrored from libs/ra_board_ek_ra8d2). main.c feeds
 * @c --input / stdin to this channel by default.
 *
 * @return The console SCI channel index (8 on the EK-RA8D2).
 * @since 0.1.0
 */
uint8_t board_periph_sci_console_channel(void);

/**
 * @brief Dispatch an MMIO read to the owning block, if any.
 *
 * @details
 * Looks up @p addr in the block table; on a hit the block's read handler
 * returns the register value and @p *handled is set true. On a miss @p *handled
 * is false and the caller falls back to the sparse model.
 *
 * @param[in,out] uc      Unicorn engine (handlers may read emulated memory).
 * @param[in]     addr    Absolute peripheral address being read.
 * @param[in]     size    Access width in bytes (1/2/4).
 * @param[out]    handled True iff a modelled block answered the read.
 * @return The register value when @p *handled is true, else 0.
 * @since 0.1.0
 */
uint64_t board_periph_read(uc_engine* uc, uint64_t addr, unsigned size, bool* handled);

/**
 * @brief Dispatch an MMIO write to the owning block, if any.
 *
 * @param[in,out] uc      Unicorn engine (handlers may read emulated memory).
 * @param[in]     addr    Absolute peripheral address being written.
 * @param[in]     size    Access width in bytes (1/2/4).
 * @param[in]     value   Value being written.
 * @param[out]    handled True iff a modelled block consumed the write.
 * @return Nothing.
 * @since 0.1.0
 */
void board_periph_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value, bool* handled);

/**
 * @brief Advance every modelled timer by one emulation chunk and raise events.
 *
 * @details
 * Called once per run-loop chunk (the same cadence as one SysTick period). Each
 * running AGT / GPT counter steps by its per-chunk increment; a wrap past the
 * period sets the block's status flag (overflow / underflow / compare-match)
 * and, if that event is linked through the ICU with its NVIC line enabled,
 * records a pending IRQ for the engine to take. Stopped timers do not advance.
 * The SCI_B model is also serviced here: with TX always drained in the model,
 * an enabled TXI / TEI re-pends each tick so an interrupt-driven transmitter
 * keeps streaming, and an enabled RXI pends while queued RX bytes remain.
 *
 * @param[in,out] uc Unicorn engine (the ICU reads IELSR / NVIC ISER from PPB).
 * @return Nothing.
 * @since 0.1.0
 */
void board_periph_tick(uc_engine* uc);

/**
 * @brief Set or clear a NVIC line's enable in the model's set-enable shadow.
 *
 * @details
 * The Cortex-M NVIC ISER / ICER registers are set-enable / clear-enable: a
 * written 1 sets (ISER) or clears (ICER) that interrupt line and a written 0 is
 * ignored, so several independent stores accumulate. board_sim maps the PPB as
 * plain RAM, where a raw @c "1 << bit" store to ISER would instead overwrite the
 * whole word and drop every other enabled line. main.c decodes ISER / ICER
 * writes and calls this so the ICU model sees the correct accumulated enable
 * state -- essential once firmware enables more than one line at once (SCI
 * RXI + TXI + TEI, and the USB controller lines in Phase 3).
 *
 * @param[in] irq    NVIC line number (0-based).
 * @param[in] enable true to set the line's enable, false to clear it.
 * @return Nothing.
 * @since 0.1.0
 */
void board_periph_nvic_set_enable(uint32_t irq, bool enable);

/**
 * @brief Pop the next pending, enabled NVIC IRQ number the ICU has queued.
 *
 * @details
 * The software half of "the ICU asserts a line and the NVIC latches it". The
 * run loop calls this at an instruction boundary; the returned IRQ number is
 * vectored in by the engine's exception layer as a real Cortex-M exception
 * (vector 16 + IRQn from VTOR). Priority and PRIMASK are handled by that layer,
 * so this only reports a line that is event-linked and NVIC-enabled.
 *
 * @param[out] out_irq Receives the IRQ number (0-based, NVIC line) on success.
 * @return true if a pending IRQ was popped into @p out_irq.
 * @since 0.1.0
 */
bool board_periph_next_irq(uint32_t* out_irq);

/**
 * @brief Record that NVIC IRQ @p irq was actually taken (for the summary).
 *
 * @param[in] irq IRQ number that the engine just vectored in.
 * @return Nothing.
 * @since 0.1.0
 */
void board_periph_note_irq_taken(uint32_t irq);

/**
 * @brief Print the peripheral-model section of the end-of-run summary.
 *
 * @details
 * Reports the final driven level of each board LED and its transition count,
 * each modelled timer's final counter / event totals, the per-IRQ taken count,
 * and each active SCI channel's transmitted / received byte totals -- the
 * observability the epic asks for (GPIO/LED transitions + per-IRQ interrupt
 * counts + captured serial), beyond the generic MMIO table main.c already
 * prints.
 *
 * @param[in,out] uc Unicorn engine (read for any final register state).
 * @return Nothing.
 * @since 0.1.0
 */
void board_periph_report(uc_engine* uc);

#ifdef __cplusplus
}
#endif
