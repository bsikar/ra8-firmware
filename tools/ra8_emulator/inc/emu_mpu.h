/**
 * @file emu_mpu.h
 * @brief Armv8-M MPU enforcement model for ra8_emulator
 *
 * @details
 * The firmware programs the MPU via ra8_mpu (RNR -> RBAR -> RLAR per region,
 * then CTRL). ra8_emulator watches those writes (no banked per-region storage in
 * the flat-RAM PPB, so each region is captured at its RLAR write) and, while
 * CTRL.ENABLE is set, faults a privileged store into any read-only region
 * with a synthesised MemManage -- the part Unicorn's core does not model.
 * Inert unless the firmware enables the MPU.
 *
 * Split out of the ra8_emulator main translation unit; behaviour unchanged.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>
#include <unicorn/unicorn.h>

#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Arm the MPU register watchers (RLAR capture + CTRL edge hooks).
 *
 * @return Nothing.
 * @pre @p uc is initialised with the PPB mapped as RAM.
 * @pre Called once during single-threaded setup.
 * @post The MPU_RLAR and MPU_CTRL write hooks are installed.
 * @note Apps that never program the MPU pay nothing beyond the two hooks.
 * @since 0.1.0
  * @details Arm the mpu register watchers (rlar capture + ctrl edge hooks); this step is contained within the emu MPU model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @post Ownership of caller-supplied storage is unchanged.
 */
void emu_mpu_install(uc_engine* uc);

/**
 * @brief Whether an RO-region write violation is latched.
 *
 * @return true while a trapped store awaits its synthesised MemManage.
 * @retval false No MPU violation is pending.
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
  * @details Whether an ro-region write violation is latched; this step is contained within the emu MPU model and uses bounded caller or module-owned storage.
 * @post Ownership of caller-supplied storage is unchanged.
 */
bool emu_mpu_fault_pending(void);

/**
 * @brief Clear the latched MPU violation.
 *
 * @details The run loop clears the latch right before synthesising the
 * MemManage; the warm-reboot path clears it so a rebooted image starts clean.
 *
 * @return Nothing.
 * @pre A violation was latched (or the call is a harmless reset).
 * @pre None otherwise.
 * @post No MPU violation is pending.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
  * @post Ownership of caller-supplied storage is unchanged.
 */
void emu_mpu_clear_fault(void);

/**
 * @brief Synthesise a MemManage (#4) fault for a trapped RO-region write.
 *
 * @details
 * Called by the run loop after the RO-write hook latched a violation. Latches
 * CFSR.MMFSR.DACCVIOL + MMARVALID and MMFAR (so a fault handler -- and the
 * HIL alive probe -- see the architectural status), forces PC back to the
 * faulting store so the exception entry stacks *that* address (a recovering
 * handler skips exactly one store), and vectors into the application's
 * MemManage_Handler. If no handler is installed the violation is dropped (no
 * escalation modelled).
 *
 * @param[in,out] uc        Unicorn engine.
 * @param[in]     vtor_base Fallback vector base if VTOR reads as 0.
 * @return Nothing.
 * @pre A violation is latched (PC / address captured at the trapped store).
 * @pre The PPB CFSR / MMFAR words and the vector table are mapped as RAM.
 * @post On a valid vector, the core is in the MemManage handler with the
 *       basic frame stacked (stacked PC == the faulting store).
 * @note Faithful to the recovering-handler contract of mpu_partition_simple.
 * @since 0.1.0
  * @post Ownership of caller-supplied storage is unchanged.
 */
void mpu_synth_memmanage(uc_engine* uc, uint32_t vtor_base);

#ifdef __cplusplus
}
#endif
