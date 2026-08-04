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
 *
 * @since 0.1.0
 */

#include "emu_cpu1.h"

#include <stdio.h>
#include <string.h>

#include "emu_console.h"
#include "emu_elf.h"
#include "emu_engine.h"
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

/** @brief Implementation of `emu_cpu1_notify_mmio_write()` -- release capture. */
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
 *
 * Dual-core only: cpu1's image is embedded as raw bytes (.cpu1_image, not a
 * cpu0 symbol), so detect it by a PT_LOAD segment whose physical address lands
 * in the MRAM_CPU1 aperture.
 */
static bool cpu1_image_present(const uint8_t* elf)
{
  uint32_t phoff = 0U;
  (void)memcpy(&phoff, elf + (uint32_t)k_elf_e_phoff_off, 4);
  const uint16_t phentsize = (uint16_t)(elf[42] | (elf[43] << 8));
  const uint16_t phnum     = (uint16_t)(elf[44] | (elf[45] << 8));
  for (uint16_t i = 0U; i < phnum; i++) {
    const uint8_t* ph = elf + phoff + ((size_t)(uint32_t)i * phentsize);
    uint32_t       p_type;
    uint32_t       p_paddr;
    (void)memcpy(&p_type, ph + 0, 4);
    (void)memcpy(&p_paddr, ph + (uint32_t)k_elf_ph_paddr_off, 4);
    if ((p_type == 1U) && (p_paddr >= (uint32_t)k_cpu1_mram_base) &&
        (p_paddr < (uint32_t)k_cpu1_mram_end)) {
      return true;
    }
  }
  return false;
}

/** @brief Mirror cpu0's memory map into @p c1 (SRAM host-backed and shared). */
static bool cpu1_map_regions(uc_engine* c1)
{
  uint32_t            region_n = 0U;
  const mem_region_t* regions  = emu_memmap_regions(&region_n);
  for (size_t i = 0U; i < (size_t)region_n; i++) {
    uc_err mr = UC_ERR_OK;
    if (regions[i].base == (uint64_t)k_sram_base) {
      mr = uc_mem_map_ptr(c1,
                          regions[i].base,
                          (size_t)regions[i].size,
                          UC_PROT_ALL,
                          emu_memmap_sram_buf());
    } else if (regions[i].base == (uint64_t)k_ns_sram2_base) {
      /* SRAM2 Non-secure alias: the SAME physical bytes as 0x22100000, so back
       * it with the shared SRAM host buffer at that offset (mirroring cpu0's
       * map). cpu1 stamps its bring-up markers through this alias
       * (cpu1_pingpong_ipc's cpu1_main writes 0x321002xx); a private mapping
       * here would hide them from cpu0 and the --dump-sym probe. */
      uint8_t* const ns_host =
        emu_memmap_sram_buf() +
        ((size_t)k_ns_sram2_base - (size_t)k_ns_alias_bit - (size_t)k_sram_base);
      mr = uc_mem_map_ptr(c1, regions[i].base, (size_t)regions[i].size, UC_PROT_ALL, ns_host);
    } else {
      mr = uc_mem_map(c1, regions[i].base, (size_t)regions[i].size, UC_PROT_ALL);
    }
    if (mr != UC_ERR_OK) {
      return false;
    }
  }
  return true;
}

/** @brief Map the shared peripheral window and its NS bit[28] alias into @p c1. */
static bool cpu1_map_periph(uc_engine* c1)
{
  /* Map the Renesas peripheral space through the SAME board_periph models cpu0
   * uses -- the peripherals are shared hardware, so the M33's MMIO (GPIO LED
   * toggles, SCI, timers, ...) reaches the models and the board view instead of
   * faulting on unmapped space. cpu0 maps this region separately via uc_mmio_map
   * (it is absent from k_regions); cpu1 needs the identical mapping. */
  if (uc_mmio_map(c1,
                  (uint64_t)k_periph_base,
                  (size_t)k_periph_size,
                  mmio_read,
                  nullptr,
                  mmio_write,
                  nullptr) != UC_ERR_OK) {
    return false;
  }
  /* The M33 runs as a permanent non-secure bus controller (SECEXT off): the
   * peripheral channels it owns -- e.g. the IPC unit it pokes to wake the M85 --
   * are NS-attributed, so its accesses route through the bit[28] non-secure
   * alias (0x5xxxxxxx) rather than the Secure 0x4xxxxxxx the M85 uses. Map that
   * alias to the SAME models with the SAME hooks: the hooks rebuild the absolute
   * address as k_periph_base + window-relative offset, so a 0x50020000 access
   * (offset 0x20000) dispatches to 0x40020000 identically to a Secure one. */
  return uc_mmio_map(c1,
                     (uint64_t)k_periph_base | (uint64_t)k_ns_alias_bit,
                     (size_t)k_periph_size,
                     mmio_read,
                     nullptr,
                     mmio_write,
                     nullptr) == UC_ERR_OK;
}

/**
 * @brief Create the second emulator engine for cpu1 (Cortex-M33), if present.
 *
 * @details
 * Only dual-core firmware (one exporting ``cpu1_reset_handler``) gets a cpu1
 * engine, so single-core apps pay nothing. The engine maps the same regions as
 * cpu0 but binds the on-chip SRAM to the shared host buffer (::s_sram_buf) so
 * cross-core IPC over shared SRAM is coherent; the full ELF (which carries
 * cpu1's image at MRAM_CPU1) is loaded so cpu1 can boot from its own vector
 * table when released. The engine is left idle -- the run loop boots it on the
 * CPU1ACTCSR release.
 *
 * @param[in] elf     The firmware image (cpu0 + cpu1).
 * @param[in] elf_len Length of @p elf, bytes.
 *
 * @return The cpu1 engine, or NULL if this is not a dual-core image (or setup
 *         failed -- cpu0 then runs alone, exactly as before).
 * @retval NULL Not a dual-core image / engine setup failed.
 *
 * @pre ::s_sram_buf is allocated (cpu0 SRAM is host-backed).
 * @pre @p elf is a valid ELF still resident (called before it is freed).
 * @post On success a Cortex-M33 engine mirrors cpu0's map with shared SRAM.
 * @post No cpu1 instruction has executed yet (idle until released).
 * @note cpu1's PPB / peripheral writes stay private to its engine.
 * @since 0.1.0
 */
static uc_engine* cpu1_engine_init(const uint8_t* elf, long elf_len)
{
  if (!cpu1_image_present(elf)) {
    return nullptr; /* single-core firmware -- no second engine. */
  }
  uc_engine* c1 = nullptr;
  if (uc_open(UC_ARCH_ARM, (uc_mode)(UC_MODE_THUMB | UC_MODE_MCLASS), &c1) != UC_ERR_OK) {
    return nullptr;
  }
  (void)uc_ctl_set_cpu_model(c1, UC_CPU_ARM_CORTEX_M33);
  if (!cpu1_map_regions(c1) || !cpu1_map_periph(c1) || (load_elf(c1, elf, elf_len) != 0)) {
    (void)uc_close(c1);
    return nullptr;
  }
  (void)fprintf(stderr, "  cpu1 engine   : Cortex-M33, shared SRAM + peripherals (dual-core)\n");
  return c1;
}

/** @brief Implementation of `emu_cpu1_init()` -- detect + build the engine. */
void emu_cpu1_init(const uint8_t* elf, long elf_len)
{
  s_cpu1_uc = cpu1_engine_init(elf, elf_len);
}

/** @brief Implementation of `emu_cpu1_step()` -- release boot + one interleave. */
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
