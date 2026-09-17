/**
 * @file emu_cpu1.c
 * @brief Second-core (cpu1) engine implementation (see emu_cpu1.h)
 *
 * @details
 * The CPU_CTRL release watcher, the second Unicorn engine bring-up with the
 * shared-SRAM backing, and the interleaved stepping -- moved verbatim out of
 * the ra8_emulator main translation unit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "emu_cpu1.h"

#include <stdio.h>
#include <string.h>

#include "emu_console.h"
#include "emu_elf.h"
#include "emu_engine.h"
#include "emu_host_io_internal.h"
#include "emu_memmap.h"
#include "emu_mmio.h"

/**
 * @enum dual_core_addr_t
 * @brief CPU_CTRL registers + activation bit for the second core (cpu1).
 *
 * @details
 * The RA8D2 boots cpu0 (Cortex-M85); the application releases cpu1
 * (Cortex-M33) by writing CPU1INITVTOR (its vector table) then
 * CPU1ACTCSR.ACTREQ. ra8_emulator emulates cpu1 in a second Unicorn engine
 * that shares the on-chip SRAM with cpu0 (host-backed), so cpu1_pingpong's
 * cross-core IPC over shared SRAM actually runs. Inert unless the firmware
 * carries a cpu1 image and asserts ACTREQ.
 */
typedef enum : uint64_t {
  k_cpu1_initvtor_addr = 0x4000F044UL, /**< CPU_CTRL.CPU1INITVTOR (32-bit). */
  k_cpu1_actcsr_addr   = 0x4000F064UL, /**< CPU_CTRL.CPU1ACTCSR  (16-bit).  */
  k_cpu1_mram_base     = 0x020C0000UL, /**< MRAM_CPU1: cpu1 image base.     */
  k_cpu1_mram_end      = 0x02100000UL, /**< MRAM_CPU1 end (256 KiB).        */
} dual_core_addr_t;

typedef enum : uint32_t {
  k_cpu1_actcsr_actreq    = 1U << 0U, /**< CPU1ACTCSR.ACTREQ -> release cpu1.   */
  k_cpu1_actcsr_key       = 0xA5U,    /**< KEY[15:8] required to honor a write. */
  k_cpu1_actcsr_key_shift = 8U,       /**< KEY byte position (bits [15:8]).     */
  k_cpu1_actcsr_key_mask  = 0xFFU,    /**< KEY byte mask after the shift.       */
  k_cpu1_chunk_insns      = 100000U,  /**< cpu1 instructions per interleave.    */
} dual_core_bits_t;

static uc_engine* s_cpu1_uc;          /**< 2nd engine for cpu1 (NULL if N/A). */
static bool       s_cpu1_active;      /**< cpu1 released and stepping.        */
static bool       s_cpu1_release_req; /**< CPU1ACTCSR.ACTREQ observed.        */
static uint32_t   s_cpu1_initvtor;    /**< Captured CPU1INITVTOR value.       */
static uint32_t   s_cpu1_pc;          /**< cpu1 run PC across interleaves.    */

static uc_engine* s_cpu1_uc;          /**< 2nd engine for cpu1 (NULL if N/A). */
static bool       s_cpu1_active;      /**< cpu1 released and stepping.        */
static bool       s_cpu1_release_req; /**< CPU1ACTCSR.ACTREQ observed.        */
static uint32_t   s_cpu1_initvtor;    /**< Captured CPU1INITVTOR value.       */
static uint32_t   s_cpu1_pc;          /**< cpu1 run PC across interleaves.    */

/** @brief Implementation of `emu_cpu1_notify_mmio_write()` -- release capture.
 */
void emu_cpu1_notify_mmio_write(uint64_t mmio_abs, uint64_t value)
{
  if (mmio_abs == (uint64_t)k_cpu1_initvtor_addr) {
    s_cpu1_initvtor = (uint32_t)value;
  } else if (mmio_abs == (uint64_t)k_cpu1_actcsr_addr) {
    /* HUM Ch 2.9.1.9 "CPU1ACTCSR" p 130 -- a write is honored only with
     * KEY=0xA5 in bits [15:8]; the firmware writes 0xA501 (KEY | ACTREQ).
     * Model the key gate so a keyless ACTREQ does not release cpu1. */
    const uint32_t key =
      ((uint32_t)value >> (uint32_t)k_cpu1_actcsr_key_shift) & (uint32_t)k_cpu1_actcsr_key_mask;
    if ((key == (uint32_t)k_cpu1_actcsr_key) &&
        (((uint32_t)value & (uint32_t)k_cpu1_actcsr_actreq) != 0U)) {
      s_cpu1_release_req = true;
    }
  }
}

/**
 * @brief True if @p elf carries a PT_LOAD segment in the cpu1 MRAM window.
 * @details Dual-core only: cpu1's image is embedded as raw bytes (.cpu1_image,
 * not a cpu0 symbol), so detect it by a PT_LOAD segment whose physical address
 * lands in the MRAM_CPU1 aperture.
 * @param[in] segment Bounds-checked load-segment descriptor.
 * @param[in,out] opaque Boolean presence accumulator.
 * @return Whether the load-segment walk should continue.
 * @retval true The segment is outside the cpu1 MRAM window.
 * @retval false A cpu1 segment was found and the walk may stop.
 * @pre @p segment is non-null.
 * @pre @p opaque points to a writable bool.
 * @post The accumulator is true exactly when this segment belongs to cpu1.
 * @post No source or emulator state changes.
 * @note Pure apart from the caller-owned accumulator.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_cpu1_segment(const elf_exec_segment_t* segment, void* opaque)
{
  bool* const present = (bool*)opaque;
  *present =
    (segment->paddr >= (uint32_t)k_cpu1_mram_base) && (segment->paddr < (uint32_t)k_cpu1_mram_end);
  return !*present;
}

/**
 * @brief Detect a PT_LOAD segment in the cpu1 MRAM window.
 * @details Walks load headers without reading or retaining segment payloads.
 * @param[in] elf Open primary ELF source.
 * @return Whether a cpu1 MRAM load segment exists.
 * @retval true At least one load segment targets the cpu1 aperture.
 * @retval false No usable load segment targets the cpu1 aperture.
 * @pre @p elf is non-null.
 * @pre @p elf remains open during the walk.
 * @post The source descriptor and cursor are unchanged.
 * @post No emulator state changes.
 * @note Uses one caller-local accumulator.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_cpu1_image_present(const emu_elf_source_t* elf)
{
  bool present = false;
  (void)elf_foreach_load_segment(elf, internal_cpu1_segment, &present);
  return present;
}

/**
 * @brief Create the second emulator engine for cpu1 (Cortex-M33), if present.
 *
 * @details
 * Only dual-core firmware (one exporting ``cpu1_reset_handler``) gets a cpu1
 * engine, so single-core apps pay nothing. The engine maps the same regions as
 * cpu0 but binds the on-chip SRAM to the same authoritative descriptor so
 * cross-core IPC over shared SRAM is coherent; the full ELF (which carries
 * cpu1's image at MRAM_CPU1) is loaded so cpu1 can boot from its own vector
 * table when released. The engine is left idle -- the run loop boots it on the
 * CPU1ACTCSR release.
 *
 * @param[in] elf The open firmware image (cpu0 + cpu1).
 * @param[in,out] memory The authoritative backing already attached to CPU0.
 *
 * @return The cpu1 engine, or NULL if this is not a dual-core image (or setup
 *         failed -- cpu0 then runs alone, exactly as before).
 * @retval NULL Not a dual-core image / engine setup failed.
 *
 * @pre @p memory is open and already attached to CPU0.
 * @pre @p elf is a valid open source retained through CPU1 loading.
 * @post On success a Cortex-M33 engine mirrors descriptor-backed shared memory.
 * @post No cpu1 instruction has executed yet (idle until released).
 * @note cpu1's PPB / peripheral writes stay private to its engine.
 * @since 0.1.0
 */
RA8_INTERNAL static uc_engine* internal_cpu1_engine_init(const emu_elf_source_t* elf,
                                                         emu_memmap_workspace_t* memory)
{
  if (!internal_cpu1_image_present(elf)) {
    return nullptr; /* single-core firmware -- no second engine. */
  }
  uc_engine* c1 = nullptr;
  if (uc_open(UC_ARCH_ARM, (uc_mode)(UC_MODE_THUMB | UC_MODE_MCLASS), &c1) != UC_ERR_OK) {
    return nullptr;
  }
  (void)uc_ctl_set_cpu_model(c1, UC_CPU_ARM_CORTEX_M33);
  if ((emu_memmap_attach(memory, c1).status != k_emu_memmap_ok) || (load_elf(c1, elf) != 0)) {
    (void)emu_memmap_detach(memory, c1);
    (void)uc_close(c1);
    return nullptr;
  }
  (void)priv_emu_io_errf("  cpu1 engine   : Cortex-M33, shared SRAM + peripherals (dual-core)\n");
  return c1;
}

/** @brief Implementation of `emu_cpu1_init()` -- detect + build the engine. */
void emu_cpu1_init(const emu_elf_source_t* elf, emu_memmap_workspace_t* memory)
{
  s_cpu1_uc = internal_cpu1_engine_init(elf, memory);
}

void emu_cpu1_close(emu_memmap_workspace_t* memory)
{
  if (s_cpu1_uc != nullptr) {
    (void)emu_memmap_detach(memory, s_cpu1_uc);
    (void)uc_close(s_cpu1_uc);
    s_cpu1_uc = nullptr;
  }
  s_cpu1_active      = false;
  s_cpu1_release_req = false;
}

/** @brief Implementation of `emu_cpu1_step()` -- release boot + one interleave.
 */
void emu_cpu1_step(void)
{
  if (s_cpu1_uc == nullptr) {
    return;
  }
  if (s_cpu1_release_req && !s_cpu1_active) {
    s_cpu1_release_req     = false;
    const uint32_t cpu1_sp = rd32(s_cpu1_uc, (uint64_t)s_cpu1_initvtor);
    s_cpu1_pc              = rd32(s_cpu1_uc, (uint64_t)s_cpu1_initvtor + 4U);
    (void)uc_reg_write(s_cpu1_uc, UC_ARM_REG_SP, &cpu1_sp);
    s_cpu1_active = true;
  }
  if (s_cpu1_active) {
    const uc_err e1 =
      uc_emu_start(s_cpu1_uc, (uint64_t)s_cpu1_pc | 1U, 0, 0, (size_t)k_cpu1_chunk_insns);
    (void)uc_reg_read(s_cpu1_uc, UC_ARM_REG_PC, &s_cpu1_pc);
    if (e1 != UC_ERR_OK) {
      s_cpu1_active = false; /* cpu1 hit a fault -- halt it, cpu0 continues */
    }
  }
}
