/**
 * @file examples/ereader/power.h
 * @brief Sleep / wake / brown-out helpers for the ereader app.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Three-piece power-management surface for the ereader skeleton:
 *
 *  - ``ereader_power_init()`` -- programme ICU button IRQ + LVD
 *    threshold + LPM defaults so the rest of the app doesn't have
 *    to know about register layouts.
 *  - ``ereader_power_sleep_until_button()`` -- gate the page-turn
 *    loop on a button press by dropping the CPU into sleep mode and
 *    letting the SW1/SW2 ICU IRQs wake it via WFI.
 *  - ``ereader_power_check_battery()`` -- non-blocking poll the
 *    LVD status flag set by the LVD ISR and return whether the host
 *    should warn the user about low battery.
 *
 * The implementation is deliberately tiny -- the full LPM /
 * deep-standby / RAM-retention surface in ``ra_lpm`` is not exercised
 * here because the EK-RA8D2 dev kit doesn't have a battery; the
 * surface still compiles against the chip's LVD / LPM blocks so the
 * future custom-board firmware can layer on top without rewriting
 * the application.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"

/**
 * @enum ereader_power_irq_t
 * @brief ICU IRQ-line numbers wired to SW1 / SW2 on the EK-RA8D2.
 *
 * @details
 * The EK-RA8D2 v1 board manual is still pending (see
 * docs/reference/EK-RA8D2-board-manual-PLACEHOLDER.md), so these
 * pin choices follow the FSP reference EK-RA8D2 BSP file
 * ``board/ra8d2_ek/board.c`` until the real manual lands. Update
 * the cite here when the manual is committed.
 */
typedef enum : uint8_t {
  k_ereader_power_irq_sw1  = 11U,   /**< SW1 -> IRQ11.                    */
  k_ereader_power_irq_sw2  = 12U,   /**< SW2 -> IRQ12.                    */
  k_ereader_power_irq_none = 0xFFU, /**< Sentinel: not woken by SW1/SW2.  */
} ereader_power_irq_t;

/**
 * @brief Bring up the ICU button channels, the LVD threshold and the
 *        LPM bits the ereader app uses for sleep entry.
 *
 * @details
 * Wraps ``ra_icu_init`` + ``ra_icu_configure_irq_pin`` for SW1/SW2,
 * ``ra_lvd_channel_init`` for PVD2 (the chip-side battery/Vcc
 * monitor), and ``ra_lpm_init`` with a sleep-friendly default config
 * (IO retention enabled so the GLCDC pins do not float during sleep
 * and re-glitch the panel on wake).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Power surface is armed.
 * @retval k_ra_err_hw_init_failed Any of the above sub-inits failed.
 *
 * @pre  ``ra_cgc_init`` has been called.
 * @pre  IOPORT clocks are up.
 * @post On success ICU IRQs for SW1/SW2 are armed and LVD will fire
 *       its callback when the rail crosses the configured threshold.
 *
 * @note Not thread-safe; runs once during single-threaded init.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ereader_power_init(void);

/**
 * @brief Drop the CPU into sleep mode until the next button press.
 *
 * @details
 * Calls ``ra_lpm_enter_sleep(k_ra_sleep_mode_sleep)`` which programs
 * STBYCR / SCR.SLEEPDEEP=0 and executes WFI. The SW1/SW2 IRQs are
 * left enabled in the NVIC so they pull the core out of WFI; on
 * return the function clears the IRQ status flag and returns the
 * raw IRQ number that woke us so the caller can route the event into
 * its message queue.
 *
 * @param[out] out_woken_irq IRQ number that woke the core (0xFF if
 *                            woken by something other than SW1/SW2).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Returned from sleep cleanly.
 * @retval k_ra_err_null_ptr       ``out_woken_irq`` is NULL.
 * @retval k_ra_err_invalid_state  ``ereader_power_init`` not called.
 *
 * @pre  ``ereader_power_init`` succeeded.
 * @post On success, ``*out_woken_irq`` is one of
 *       ``k_ereader_power_irq_sw1`` / ``k_ereader_power_irq_sw2``
 *       / 0xFF.
 *
 * @note Not thread-safe; called from the reader thread between
 *       page renders.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ereader_power_sleep_until_button(uint8_t* out_woken_irq);

/**
 * @brief Non-blocking query for low-battery status.
 *
 * @details
 * The LVD ISR sets a one-shot sticky flag when the rail trips the
 * configured threshold. This call returns the flag (true == battery
 * is low) and clears it.
 *
 * @return ``true`` if the LVD has tripped since the last poll;
 *         ``false`` otherwise.
 *
 * @note Thread-safe (single 8-bit flag, atomic on Cortex-M85).
 *
 * @since 0.1.0
 */
bool ereader_power_check_battery(void);

#ifdef __cplusplus
}
#endif
