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
 * raise compare-match / overflow / underflow events), and the ICU/NVIC (a
 * peripheral event linked through IELSR pends the matching NVIC IRQ, taken as a
 * real Cortex-M exception by the engine's exception layer). The block table is
 * the extension point: UART / I2C / SPI / USB slot in as new entries later.
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
 *
 * @param[in,out] uc Unicorn engine (the ICU reads IELSR / NVIC ISER from PPB).
 * @return Nothing.
 * @since 0.1.0
 */
void board_periph_tick(uc_engine* uc);

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
 * each modelled timer's final counter / event totals, and the per-IRQ taken
 * count -- the observability the epic asks for (GPIO/LED transitions + per-IRQ
 * interrupt counts), beyond the generic MMIO table main.c already prints.
 *
 * @param[in,out] uc Unicorn engine (read for any final register state).
 * @return Nothing.
 * @since 0.1.0
 */
void board_periph_report(uc_engine* uc);

#ifdef __cplusplus
}
#endif
