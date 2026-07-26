/**
 * @file sim_cpu1.h
 * @brief Second-core (cpu1, Cortex-M33) engine: release watch, boot, stepping
 *
 * @details
 * The RA8D2 boots cpu0 (Cortex-M85); the application releases cpu1
 * (Cortex-M33) by writing CPU1INITVTOR (its vector table) then
 * CPU1ACTCSR.ACTREQ. board_sim emulates cpu1 in a second Unicorn engine that
 * shares the on-chip SRAM with cpu0 (host-backed), so cross-core IPC over
 * shared SRAM actually runs. The MMIO model notifies this module of every
 * peripheral write so the release sequence is captured; the run loop boots
 * and steps the second engine between cpu0 chunks. Inert unless the firmware
 * carries a cpu1 image and asserts ACTREQ.
 *
 * Split out of the board_sim main translation unit; behaviour unchanged.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
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
 * @brief Watch a peripheral MMIO write for the cpu1 release sequence.
 *
 * @details Captures CPU1INITVTOR (cpu1's vector-table base) and, on a
 * correctly-keyed CPU1ACTCSR write (HUM Ch 2.9.1.9 "CPU1ACTCSR" p 130:
 * KEY=0xA5 in bits [15:8] plus ACTREQ), latches the release request the run
 * loop consumes. Called by the MMIO write model for every peripheral write;
 * anything but the two CPU_CTRL words is ignored.
 *
 * @param[in] mmio_abs Absolute peripheral address being written.
 * @param[in] value    Value being written.
 * @return Nothing.
 * @pre The MMIO write model dispatches every peripheral write here.
 * @pre None otherwise.
 * @post The vector base / release latch reflect the observed write.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @see sim_cpu1_step()  Consumes the release latch.
 * @since 0.1.0
 */
RA8_PRIV void sim_cpu1_notify_mmio_write(uint64_t mmio_abs, uint64_t value);

/**
 * @brief Create the second emulator engine for cpu1, if the image carries one.
 *
 * @details
 * Only dual-core firmware (a PT_LOAD segment in the MRAM_CPU1 window) gets a
 * cpu1 engine, so single-core apps pay nothing. The engine maps the same
 * regions as cpu0 but binds the on-chip SRAM (and its NS alias) to the shared
 * host buffer so cross-core IPC over shared SRAM is coherent, maps the
 * peripheral windows through the same MMIO model, and loads the full ELF so
 * cpu1 can boot from its own vector table when released. The engine is left
 * idle -- sim_cpu1_step() boots it on the CPU1ACTCSR release.
 *
 * @param[in] elf     The firmware image (cpu0 + cpu1).
 * @param[in] elf_len Length of @p elf, bytes.
 * @return Nothing (the engine handle stays module-private).
 * @pre sim_memmap_init() succeeded (the shared SRAM backing exists).
 * @pre @p elf is a valid ELF still resident (called before it is freed).
 * @post On a dual-core image a Cortex-M33 engine mirrors cpu0's map with
 *       shared SRAM; otherwise no engine exists and stepping is a no-op.
 * @post No cpu1 instruction has executed yet (idle until released).
 * @note cpu1's PPB / peripheral writes stay private to its engine.
 * @since 0.1.0
 */
RA8_PRIV void sim_cpu1_init(const uint8_t* elf, long elf_len);

/**
 * @brief Boot cpu1 on a pending release, then step it one interleave chunk.
 *
 * @details The run loop calls this once per outer chunk. On the latched
 * CPU1ACTCSR release it reads cpu1's SP/PC from the captured CPU1INITVTOR and
 * marks the core active; while active it runs one bounded instruction chunk.
 * cpu1 is a tight poll loop (no interrupts), so a plain stepped run suffices;
 * a cpu1 fault just halts cpu1 (cpu0 keeps running). A no-op without a cpu1
 * engine.
 *
 * @return Nothing.
 * @pre sim_cpu1_init() ran (engine present or absent as detected).
 * @pre The run loop is between cpu0 chunks.
 * @post cpu1 advanced up to one interleave chunk (or stayed idle/halted).
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV void sim_cpu1_step(void);

#ifdef __cplusplus
}
#endif
