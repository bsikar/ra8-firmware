/**
 * @file ra8_nsc_dualcore.h
 * @brief NSC veneers for dual-core Cortex-M33 hold-page and M85 sleep handoff
 * @ingroup grp_security
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * Exposes the TrustZone Non-Secure Callable gateways required when a
 * TrustZone-split e-reader product (CPU0 Non-Secure ThreadX UI) hands off
 * idle page-hold supervision to the secondary Cortex-M33 core (CPU1) and
 * parks CPU0 in clock-gated Sleep until an IPC0 wake or button edge fires.
 *
 * All CPU_CTRL (`CPU1INITVTOR` / `CPU1WAITCR` / `CPU1ACTCSR`), SYSC/LPM
 * (`HOCOCR` / `LPSCR` / `PRCR`), CPSCU (`IPCSAR` / `SRAMSABAR3` / `BUSSAR`),
 * and ICU IELSR/NVIC IPC0 IRQ setup registers are Secure-only on RA8D2; this
 * header is the TrustZone-safe boundary for Non-Secure application callers.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_nsc_veneer.h"

/**
 * @brief NSC veneer: configure dual-core SRAM3/IPC security and release M33.
 *
 * @details
 * Validates ``cpu1_mram_base`` (`0x020C0000`) and ``cpu1_stack_top`` (`0x221A0000`),
 * opens the Secure CPSCU partition for SRAM3 (`0x22180000..0x2219FFFF`),
 * configures IPC channels `0` (CPU1->CPU0) and `2` (CPU0->CPU1) as Non-Secure
 * in `IPCSAR` (`0x00050000`), binds the Secure `IPC0` IRQ0 wake handler,
 * assigns `LED1` (P600) and `SW1` (P009) pins to Non-Secure in `PMSAR`, and
 * releases CPU1 at the embedded `.cpu1_image` entry and initial stack top.
 *
 * @param[in] cpu1_mram_base Expected MRAM_CPU1 base (`0x020C0000`).
 * @param[in] cpu1_stack_top Expected SRAM_CPU1 stack top (`0x221A0000`).
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok               M33 image released and running.
 * @retval k_ra8_err_invalid_arg  Entry or stack top outside fixed CPU1 bounds.
 * @retval k_ra8_err_not_supported Target flash at ``cpu1_mram_base`` lacks a valid vector table.
 * @retval k_ra8_err_hw_timeout   `CPU1ACTCSR.ACT` handshake timed out.
 *
 * @pre Caller executes in Non-Secure Thread mode after Secure boot setup.
 * @pre Shared mailbox in SRAM3 has been initialised by caller before call.
 * @post SRAM3 is marked Non-Secure (`SRAMSABAR3 = 0x00180000`).
 * @post `IPC0` RX IRQ0 handler is armed to record M33 wake events.
 * @post CPU1 is executing from `ORIGIN(MRAM_CPU1)`.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. Scalar address validation only.
 *
 * @note Not thread-safe; call once during NS system initialisation.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_dualcore_start_m33(uintptr_t cpu1_mram_base,
                                                                  uintptr_t cpu1_stack_top);

/**
 * @brief NSC veneer: clock-gate M85 / stop HOCO and enter Sleep until wake.
 *
 * @details
 * Stops the HOCO oscillator via ``ra8_lpm_set_clock_stop(k_ra8_lpm_clock_hoco, true)``
 * and executes ``ra8_lpm_enter_sleep(k_ra8_sleep_mode_sleep)`` up to
 * ``max_wake_iters`` iterations until either the Secure `IPC0` IRQ0 wake flag
 * latches or the bounded poll cap is reached. Restores HOCO running before
 * returning to the Non-Secure caller.
 *
 * Callers running ThreadX in Non-Secure world should pause SysTick across this
 * call so 1 ms tick interrupts do not immediately break M85 out of Sleep WFI
 * while CPU1 is holding the page.
 *
 * @param[in]  max_wake_iters Maximum Sleep-WFI rounds (must be non-zero).
 * @param[out] out_woken      Non-Secure output set to 1 if IPC0 wake fired,
 *                            0 otherwise.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok              Sleep/wake cycle completed and HOCO restored.
 * @retval k_ra8_err_null_ptr    ``out_woken`` was NULL.
 * @retval k_ra8_err_invalid_arg ``max_wake_iters`` was 0 or ``out_woken`` outside NS.
 *
 * @pre ::ra8_nsc_dualcore_start_m33 succeeded.
 * @pre ``out_woken`` points to writable Non-Secure memory.
 * @post HOCO stop bit is restored to running (`false`) on every exit path.
 * @post `*out_woken` reflects whether IPC0 wake fired during sleep.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. ``out_woken`` is range-checked.
 *
 * @note Not thread-safe; single coordinator thread only.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_dualcore_park_m85(uint32_t  max_wake_iters,
                                                                 uint32_t* out_woken);

/**
 * @brief NSC veneer: poke CPU1 via IPC1 channel 0 IRQ0 after ack/update.
 *
 * @details
 * Issues a data barrier and sends ``ra8_ipc_send_event(2U, k_ra8_ipc_irq_event_0)``
 * so CPU1 observes a clean handoff/ack notification from CPU0.
 *
 * @return ::ra8_err_t outcome from ::ra8_ipc_send_event.
 * @retval k_ra8_ok Event asserted on IPC1 channel 0.
 *
 * @pre TrustZone substrate up.
 * @post Peer STA.IRQ0 asserted on IPC channel 2.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. Argument-less call.
 *
 * @note Thread-safe across distinct IPC channels.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_dualcore_notify_cpu1(void);

#ifdef __cplusplus
}
#endif
