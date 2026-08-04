/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Brighton Sikarskie */
/**
 * @file examples/ek_ra8d2/hw_pending/blink_m33_hal/blink_m33_hal.h
 * @brief One-iteration LED1 blink step for the HAL-based M33 example
 *
 * @details
 * The single visible action of `blink_m33_hal`'s CPU1 loop, factored into one
 * bounded, side-effect-scoped inline step so it is exercised by BOTH the
 * freestanding M33 firmware (`cpu1_main.c` calls it once per pass) and the
 * host app-level test (`tests/test_app_blink_m33_hal.c` calls it against the
 * `ra8_fake_mmap` MMIO backing). The tested logic is therefore the SAME code the
 * firmware runs, not a re-implementation that could drift from it.
 *
 * LED1 (BLUE) is `k_ra8_pin_led1` = P600 = PORT6 pin 0; the step drives it through
 * the CPU1-safe HAL primitive `ra8_pcntr_set_output()` (issue #580) -- no raw
 * MMIO, no `ra8_hal` object linked.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_pcntr.h"
#include "ra8_port_constants.h"

/**
 * @brief Advance the blink by one step: toggle the level and drive LED1 to it.
 *
 * @details
 * Computes the next level (the logical inverse of @p level) and holds LED1
 * (`k_ra8_pin_led1`, P600 = PORT6 pin 0) an output at that level via
 * `ra8_pcntr_set_output()`, then returns the new level so the caller can carry it
 * into the next pass. Exactly one PCNTR1 read-modify-write happens per call; the
 * port/pin come from `k_ra8_pin_led1` so they are compile-time valid and the
 * primitive's `[[nodiscard]]` result is intentionally discarded.
 *
 * @param[in] level The current LED level (`k_ra8_level_low` / `k_ra8_level_high`).
 *
 * @return The new level LED1 was driven to (the inverse of @p level).
 * @retval k_ra8_level_high @p level was `k_ra8_level_low`; LED1 is now driven high.
 * @retval k_ra8_level_low  @p level was `k_ra8_level_high`; LED1 is now driven low.
 *
 * @pre The IOPORT module clock is on (always-on after reset on the RA8D2).
 * @pre LED1 (P600) powers up routed to PORT (no PFS setup is needed).
 * @post PORT6 pin 0 is configured as an output.
 * @post PORT6 pin 0's output latch equals the returned level.
 *
 * @note Not thread-safe / not ISR-safe: the underlying PCNTR1 read-modify-write
 *       is not atomic. Single-threaded blink-loop use only.
 * @since 0.1.0
 */
static inline ra8_level_t blink_m33_hal_step(ra8_level_t level)
{
  const ra8_level_t next = (level == k_ra8_level_high) ? k_ra8_level_low : k_ra8_level_high;
  (void)ra8_pcntr_set_output(RA8_PIN_PORT(k_ra8_pin_led1), RA8_PIN_PIN(k_ra8_pin_led1), next);
  return next;
}

#ifdef __cplusplus
}
#endif
