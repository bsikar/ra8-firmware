/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_fake_irq.h
 * @brief Host-test interrupt-injection shim
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * Unit tests that exercise an ISR-path need a way to simulate an
 * interrupt firing without actually running on the Cortex-M85.
 * ``ra8_fake_irq`` is the minimal helper that lets a test:
 *
 * 1. Build up the ``ra8_isr`` dispatch table as usual (register
 * the handlers through ``ra8_isr_register``).
 * 2. Call ``ra8_fake_irq_fire(event)`` to look up the registered
 * slot for ``event`` and invoke ``ra8_isr_dispatch(slot)``
 * directly.
 *
 * That's the entire API. There is no scheduler, no priority, no
 * preemption -- the helper just finds the slot and calls the
 * handler synchronously. This matches what the Cortex-M85 NVIC
 * + vector-table trampoline do at runtime, minus the actual
 * exception entry / exit sequence.
 *
 * ## When to use
 *
 * - Tests for IRQ-driven SCI / SPI / IIC drivers that want to
 * "drive" an RX-ready interrupt after staging some bytes in
 * the fake MMIO.
 * - GPT compare-match tests that want to simulate an edge event.
 * - Any driver that registers a completion callback via
 * ``ra8_isr`` and needs to verify the callback runs.
 *
 * The stub lives here so that + drivers can call
 * it the moment they grow interrupt paths; today only a handful
 * of trivial use-cases exercise it.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_elc_regs.h"
#include "ra8_err.h"

/**
 * @brief Look up the slot for ``event`` and invoke its handler.
 *
 * @details
 * Calls ``ra8_isr_lookup_slot(event, &slot)`` to find the slot
 * allocated to ``event`` by ``ra8_isr_register``, then calls
 * ``ra8_isr_dispatch(slot)``. If the event was not registered,
 * returns ``k_ra8_err_not_found`` without invoking anything.
 *
 * @param[in] event ELC event number.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Handler invoked.
 * @retval k_ra8_err_not_found ``event`` had no registered slot.
 *
 * @pre ``ra8_isr_init`` and ``ra8_isr_register`` have been called.
 * @pre The test is running under ``RA8_OFF_TARGET``.
 *
 * @post On success, the caller's registered handler ran exactly
 * once.
 *
 * @note Thread safety: test code only, single-threaded.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fake_irq_fire(ra8_elc_event_t event);

#ifdef __cplusplus
}
#endif
