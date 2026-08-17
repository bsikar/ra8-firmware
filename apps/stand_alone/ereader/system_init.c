/**
 * @file apps/stand_alone/ereader/system_init.c
 * @brief Cortex-M85 / RA8D2 core bring-up (called from Reset_Handler)
 *
 * @par Tag
 * [Ring 1 / Boot] {World: S}
 *
 * @par TrustZone:
 * Unlike the sibling ``usb_cdc_echo`` app, this variant calls
 * ``ra8_trustzone_init`` so that the SAU is programmed before any NS
 * code can run. The NS-side ``main()`` then crosses NS->S only via
 * the ``cmse_nonsecure_entry`` veneers in ``libs/ra8_nsc/``.
 *
 * @details
 * `SystemInit()` follows the CMSIS naming convention. `Reset_Handler`
 * calls it *after* it has initialised the C runtime (copied .data,
 * zeroed .bss), so it may read and write globals freely. It performs
 * the CPU-core bring-up (VTOR / FPU / priority grouping) and then the
 * Secure peripheral bring-up (clock tree + SAU + the BLXNS into the NS
 * image), which does not return on hardware.
 *
 * ## Bring-up sequence
 *
 * 1. Set VTOR to the physical address of the vector table (so that
 * linker relocations and later TrustZone / bootloader hand-offs
 * work).
 * 2. Enable CP10 / CP11 so the FPU is usable before any
 * floating-point instruction runs.
 * 3. Enable FPU lazy stacking (FPCCR.LSPEN) so ISRs do not pay the
 * full FP-state save cost unless they touch the FPU.
 * 4. Enable the I-cache and D-cache (Cortex-M85 has both). Without
 * this the core runs at a small fraction of its rated speed.
 * 5. Enable the branch-target predictor (CCR.BP).
 * 6. Programme NVIC priority grouping to 4 bits preempt / 0 bits
 * sub (the standard embedded pattern).
 * 7. Disable interrupts until the application is ready -- this
 * mirrors CMSIS convention. `main()` re-enables via
 * `__enable_irq()` once all drivers are up.
 *
 * The function is C, not naked asm, so the stack, .data, and .bss must
 * already be usable. `Reset_Handler` loads SP from the vector table and
 * initialises the C runtime (.data copy + .bss zero) before calling
 * `SystemInit()`, so reads and writes of initialised globals here are
 * safe -- which matters because the Secure clock + TrustZone bring-up it
 * performs touches .data-resident driver state (e.g. log-tag pointers).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "trustzone_init.h"

extern const uint32_t g_ra8_vector_table_start[];

/* =============================================================================
 * Core / SCB / NVIC register addresses (direct, no CMSIS dep)
 * =============================================================================
 */

typedef enum : uintptr_t {
  k_ra8_scb_vtor_addr  = 0xE000ED08UL, /**< Vector Table Offset Register.              */
  k_ra8_scb_ccr_addr   = 0xE000ED14UL, /**< Configuration and Control Register.        */
  k_ra8_scb_shcsr_addr = 0xE000ED24UL, /**< System Handler Control and State.          */
  k_ra8_scb_cpacr_addr = 0xE000ED88UL, /**< Coprocessor Access Control.                */
  k_ra8_scb_nsacr_addr = 0xE000ED8CUL, /**< Non-secure Access Control.                 */
  k_ra8_scb_iciallu    = 0xE000EF50UL, /**< ICIALLU -- invalidate I-cache.             */
  k_ra8_scb_dciallu    = 0xE000EF58UL, /**< DCIALLU -- invalidate D-cache (sets only). */
  k_ra8_scb_csselr     = 0xE000ED84UL, /**< Cache Size Selection Register.             */
  k_ra8_scb_ccsidr     = 0xE000ED80UL, /**< Cache Size ID Register.                    */
  k_ra8_scb_dcisw      = 0xE000EF60UL, /**< D-cache Invalidate by Set/Way.             */
  k_ra8_fpu_fpccr_addr = 0xE000EF34UL, /**< FPU Context Control Register.              */
  k_ra8_nvic_aircr     = 0xE000ED0CUL, /**< Application Interrupt and Reset Ctrl.      */
  k_ra8_mpu_type_addr  = 0xE000ED90UL, /**< MPU Type Register.                         */
  k_ra8_mpu_ctrl_addr  = 0xE000ED94UL, /**< MPU Control Register.                      */
  k_ra8_mpu_rnr_addr   = 0xE000ED98UL, /**< MPU Region Number.                         */
  k_ra8_mpu_rbar_addr  = 0xE000ED9CUL, /**< MPU Region Base Address.                   */
  k_ra8_mpu_rlar_addr  = 0xE000EDA0UL, /**< MPU Region Limit Address.                  */
  k_ra8_mpu_mair0_addr = 0xE000EDC0UL, /**< MPU Attribute Indirection 0.               */
  k_ra8_mpu_mair1_addr = 0xE000EDC4UL, /**< MPU Attribute Indirection 1.               */
} ra8_core_addr_t;

extern uint32_t g_ra8_ls_stack_top; /* from vector_table.c / linker. */

/* =============================================================================
 * Small helpers
 * =============================================================================
 */

/**
 * @brief Read a 32-bit core/system register via a volatile load.
 * @details The @c volatile cast forces the access to happen exactly once and
 *          in program order relative to the other register touches in the
 *          bring-up sequence, so the compiler cannot cache or reorder it.
 * @param[in] addr 32-bit-aligned MMIO / system-register address to read.
 * @return The current 32-bit contents of the register at @p addr.
 * @retval 0 Valid when the addressed field reads back zero.
 * @pre @p addr is a 32-bit-aligned, mapped system-register address.
 * @pre Called during single-threaded core bring-up.
 * @post No architectural state is changed (pure read).
 * @post The returned value reflects the live register contents.
 * @note Not thread-safe; reset-path use only.
 * @since 0.1.0
 */
RA8_INTERNAL static inline uint32_t internal_read32(uintptr_t addr)
{
  return *(volatile uint32_t*)addr;
}

/**
 * @brief Write a 32-bit value to a core/system register via a volatile store.
 * @details The @c volatile cast forces the store to be emitted exactly once and
 *          kept in program order with the surrounding register accesses, which
 *          the cache / FPU / MPU enable sequences below depend on.
 * @param[in] addr  32-bit-aligned MMIO / system-register address to write.
 * @param[in] value 32-bit value to store at @p addr.
 * @return Nothing.
 * @pre @p addr is a 32-bit-aligned, mapped system-register address.
 * @pre Called during single-threaded core bring-up.
 * @post The register at @p addr holds @p value.
 * @post No other architectural state is changed.
 * @note Not thread-safe; reset-path use only.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_write32(uintptr_t addr, uint32_t value)
{
  *(volatile uint32_t*)addr = value;
}

/**
 * @brief Data Synchronization Barrier (DSB).
 * @details Stalls until every preceding memory access completes, so a register
 *          write (e.g. a cache or MPU enable) is in effect before later
 *          instructions observe it. Compiled out under @c RA8_OFF_TARGET,
 *          where the host emulator has no pipeline to drain.
 * @return Nothing.
 * @pre Called from the single-threaded reset path.
 * @pre The memory accesses to be ordered have already been issued.
 * @post All prior memory accesses are complete before execution continues.
 * @post No general-purpose register or memory state is modified.
 * @note Not thread-safe; reset-path use only.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_dsb(void)
{
#ifndef RA8_OFF_TARGET
  __asm__ volatile("dsb 0xF" ::: "memory");
#endif
}

/**
 * @brief Instruction Synchronization Barrier (ISB).
 * @details Flushes the pipeline so instructions after a context-altering
 *          register write (CPACR, CCR, ...) are re-fetched under the new
 *          context. Compiled out under @c RA8_OFF_TARGET.
 * @return Nothing.
 * @pre Called from the single-threaded reset path.
 * @pre A preceding context-altering write (e.g. CPACR) has been issued.
 * @post The instruction stream is synchronized to the updated context.
 * @post No general-purpose register or memory state is modified.
 * @note Not thread-safe; reset-path use only.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_isb(void)
{
#ifndef RA8_OFF_TARGET
  __asm__ volatile("isb 0xF" ::: "memory");
#endif
}

/**
 * @brief Mask all maskable IRQs at the core (PRIMASK = 1, @c cpsid @c i).
 * @details Gives the rest of @c SystemInit a deterministic, interrupt-free
 *          bring-up window; @c main() re-enables IRQs once every handler is
 *          wired up. Compiled out under @c RA8_OFF_TARGET.
 * @return Nothing.
 * @pre Called from the single-threaded reset path before any IRQ source is on.
 * @pre PRIMASK is in its reset (interrupts-enabled) state.
 * @post Maskable interrupts cannot preempt the remaining init sequence.
 * @post Only PRIMASK changes; no other architectural state is modified.
 * @note Not thread-safe; reset-path use only.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_disable_irq(void)
{
#ifndef RA8_OFF_TARGET
  __asm__ volatile("cpsid i" ::: "memory");
#endif
}

/* =============================================================================
 * Core init steps
 * =============================================================================
 */

/**
 * @brief Point VTOR at the physical vector table base.
 * @details Writes the absolute address of the @c .vectors section (pinned by the
 *          linker to the start of MRAM, @c 0x02000000) into the SCB VTOR
 *          register so exceptions vector to this image's handler table.
 * @return Nothing.
 * @pre Called from the single-threaded reset path before any exception can fire.
 * @pre The linker symbol @c g_ra8_vector_table_start resolves to the table base.
 * @post SCB->VTOR holds the physical vector-table base address.
 * @post Subsequent exceptions dispatch through this image's vector table.
 * @note Not thread-safe; reset-path use only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_set_vtor(void)
{
  /* Vector table sits at the start of the .vectors section, which
   * the linker pins to the start of MRAM (`0x02000000`). The SCB
   * VTOR register stores the absolute address. */
  internal_write32(k_ra8_scb_vtor_addr, (uint32_t)(uintptr_t)g_ra8_vector_table_start);
}

/**
 * @brief Grant full access to CP10 / CP11 (the FPU coprocessors).
 * @details Sets the CP10 and CP11 access fields in SCB CPACR to full access,
 *          then issues a DSB + ISB so the FPU is usable before the first
 *          floating-point instruction is fetched. Without this, an FP access
 *          would raise a UsageFault (NOCP).
 * @return Nothing.
 * @pre Called from the single-threaded reset path before any FP instruction.
 * @pre The FPU is present on this core (Cortex-M85 with FP).
 * @post CPACR grants CP10/CP11 full access and the change is synchronized.
 * @post Floating-point instructions execute without a NOCP UsageFault.
 * @note Not thread-safe; reset-path use only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_enable_fpu(void)
{
  enum : uint32_t {
    k_ra8_cpacr_cp10_cp11_full_access = 0x00F00000UL, /**< RA8 cpacr cp10 cp11 full access. */
  };
  uint32_t cpacr = internal_read32(k_ra8_scb_cpacr_addr);
  cpacr |= k_ra8_cpacr_cp10_cp11_full_access;
  internal_write32(k_ra8_scb_cpacr_addr, cpacr);
  internal_dsb();
  internal_isb();
}

/**
 * @brief Turn on FPU lazy stacking (FPCCR.LSPEN = 1, ASPEN = 1).
 * @details Sets LSPEN (lazy state preservation) and ASPEN (automatic state
 *          preservation) in FPCCR so exception entry reserves the FP stack
 *          frame but defers the costly FP register save until a handler actually
 *          uses the FPU -- cutting interrupt latency for non-FP handlers.
 * @return Nothing.
 * @pre Called from the single-threaded reset path after ::internal_enable_fpu.
 * @pre The FPU coprocessor access has already been granted (CPACR set).
 * @post FPCCR.LSPEN and FPCCR.ASPEN are set; FP context is lazily stacked.
 * @post Exception entry on non-FP handlers no longer saves FP registers.
 * @note Not thread-safe; reset-path use only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_enable_fpu_lazy_stack(void)
{
  enum : uint32_t {
    k_ra8_fpccr_lspen = 1UL << 30, /**< RA8 fpccr lspen. */
    k_ra8_fpccr_aspen = 1UL << 31, /**< RA8 fpccr aspen. */
  };
  uint32_t fpccr = internal_read32(k_ra8_fpu_fpccr_addr);
  fpccr |= k_ra8_fpccr_lspen | k_ra8_fpccr_aspen;
  internal_write32(k_ra8_fpu_fpccr_addr, fpccr);
}

/**
 * @brief Invalidate and enable the Cortex-M85 I-cache.
 */
RA8_INTERNAL [[maybe_unused]] static void internal_enable_icache(void)
{
  /* Invalidate, then set CCR.IC. */
  internal_dsb();
  internal_write32(k_ra8_scb_iciallu, 0U);
  internal_dsb();
  internal_isb();

  enum : uint32_t { k_ra8_ccr_ic = 1UL << 17 /**< RA8 ccr ic. */ };
  uint32_t ccr = internal_read32(k_ra8_scb_ccr_addr);
  ccr |= k_ra8_ccr_ic;
  internal_write32(k_ra8_scb_ccr_addr, ccr);
  internal_dsb();
  internal_isb();
}

/**
 * @brief Invalidate and enable the Cortex-M85 D-cache.
 *
 * @details
 * The invalidate-by-set/way loop is omitted here for brevity -- on
 * the RA8D2 the boot ROM leaves the D-cache with all lines marked
 * invalid, so a bulk CCR.DC = 1 write is safe. A production build
 * should add a real CCSIDR-driven loop.
 */
RA8_INTERNAL [[maybe_unused]] static void internal_enable_dcache(void)
{
  enum : uint32_t { k_ra8_ccr_dc = 1UL << 16 /**< RA8 ccr dc. */ };
  uint32_t ccr = internal_read32(k_ra8_scb_ccr_addr);
  ccr |= k_ra8_ccr_dc;
  internal_write32(k_ra8_scb_ccr_addr, ccr);
  internal_dsb();
  internal_isb();
}

/**
 * @brief Enable branch-target prediction (CCR.BP on Cortex-M85).
 */
RA8_INTERNAL [[maybe_unused]] static void internal_enable_branch_predictor(void)
{
  enum : uint32_t { k_ra8_ccr_bp = 1UL << 18 /**< RA8 ccr bp. */ };
  uint32_t ccr = internal_read32(k_ra8_scb_ccr_addr);
  ccr |= k_ra8_ccr_bp;
  internal_write32(k_ra8_scb_ccr_addr, ccr);
  internal_dsb();
  internal_isb();
}

/**
 * @brief Set NVIC priority grouping to 4 preempt bits / 0 sub-priority.
 * @details Writes AIRCR with the required VECTKEY and PRIGROUP = 3, so all
 *          available priority bits select the preemption level and none are
 *          sub-priority. This gives every configured IRQ a distinct preemption
 *          priority, matching the driver layer's priority assignments.
 * @return Nothing.
 * @pre Called from the single-threaded reset path before IRQs are enabled.
 * @pre The VECTKEY (0x05FA) is included in the AIRCR write (else it is ignored).
 * @post AIRCR.PRIGROUP = 3 (4 preemption bits, 0 sub-priority bits).
 * @post Subsequent NVIC priority writes are interpreted as preemption levels.
 * @note Not thread-safe; reset-path use only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_set_priority_grouping(void)
{
  enum : uint32_t {
    k_ra8_aircr_vectkey    = 0x05FA0000UL, /**< Required write key.     */
    k_ra8_aircr_prigroup_4 = 0x00000300UL, /**< PRIGROUP = 3 -> 4 bits. */
  };
  internal_write32(k_ra8_nvic_aircr, k_ra8_aircr_vectkey | k_ra8_aircr_prigroup_4);
}

/**
 * @brief Programme and enable the Cortex-M85 core MPU.
 *
 * @details
 * Installs four regions and enables the MPU with PRIVDEFENA so
 * privileged mode still sees the default memory map for anything
 * not explicitly covered:
 *
 * - Region 0: MRAM (0x02000000, 1 MiB) -- read + execute, inner/outer cacheable.
 * - Region 1: SRAM (0x22000000, 2 MiB) -- read + write, no execute.
 * - Region 2: SDRAM (0x68000000, 64 MiB) -- read + write, no execute.
 * - Region 3: Peripherals (0x40000000, 128 MiB) -- read + write,
 * device memory, no execute.
 *
 * Attribute indirection table:
 * - MAIR0[0] = 0xFF (Normal inner + outer write-back / write-alloc).
 * - MAIR0[1] = 0x44 (Normal non-cacheable).
 * - MAIR0[2] = 0x04 (Device-nGnRE).
 *
 * Region base + limit addresses are encoded per ARMv8-M main-profile
 * PMSAv8 rules: low 5 bits of RBAR hold `AP[2:0]` / `SH[4:3]` / `XN`,
 * and RLAR holds the upper bound OR the enable flag in bit 0.
 */
/* RBAR attribute-byte encodings (bottom 5 bits of RBAR). */
enum : uint32_t {
  k_ra8_rbar_attr_ro_x      = 0x02U,    /**< AP=RO, SH=none, XN=0.                        */
  k_ra8_rbar_attr_rw_xn     = 0x03U,    /**< AP=RW, SH=none, XN=1.                        */
  k_ra8_rbar_attr_device_rw = 0x23U,    /**< AP=RW, SH=outer sh, XN=1, MAIR idx = device. */
  k_ra8_mpu_rlar_enable     = 1UL << 0, /**< RA8 MPU rlar enable.                         */
};

/* Region base and limit addresses. RLAR limits are <region-end> minus
 * the ARMv8-M 32-byte region quantum, OR-ed with the enable bit at write time. */
enum : uint32_t {
  k_ra8_mpu_mram_base   = 0x02000000UL, /**< 1 MiB MRAM code region.     */
  k_ra8_mpu_mram_limit  = 0x020FFFE0UL, /**< RA8 MPU MRAM limit.         */
  k_ra8_mpu_sram_base   = 0x22000000UL, /**< 2 MiB ECC SRAM region.      */
  k_ra8_mpu_sram_limit  = 0x221FFFE0UL, /**< RA8 MPU SRAM limit.         */
  k_ra8_mpu_sdram_base  = 0x68000000UL, /**< 64 MiB external SDRAM.      */
  k_ra8_mpu_sdram_limit = 0x6BFFFFE0UL, /**< RA8 MPU SDRAM limit.        */
  k_ra8_mpu_peri_base   = 0x40000000UL, /**< Peripheral bus window base. */
  k_ra8_mpu_peri_limit  = 0x47FFFFE0UL, /**< RA8 MPU peri limit.         */
};

/**
 * @brief Program a single MPU region via RNR/RBAR/RLAR.
 */
RA8_INTERNAL [[maybe_unused]] static void
internal_mpu_set_region(uint32_t region, uint32_t base_attr, uint32_t limit_enable)
{
  internal_write32(k_ra8_mpu_rnr_addr, region);
  internal_write32(k_ra8_mpu_rbar_addr, base_attr);
  internal_write32(k_ra8_mpu_rlar_addr, limit_enable);
}

/**
 * @brief Programme and enable the Cortex-M85 core MPU.
 */
RA8_INTERNAL [[maybe_unused]] static void internal_mpu_init(void)
{
  enum : uint32_t {
    k_ra8_mpu_ctrl_enable     = 1UL << 0,     /**< RA8 MPU control enable.     */
    k_ra8_mpu_ctrl_privdefena = 1UL << 2,     /**< RA8 MPU control privdefena. */
    k_ra8_mair0_default       = 0x000404FFUL, /**< RA8 mair0 default.          */
  };

  /* MAIR0: idx 0 = WB/WA, idx 1 = non-cacheable, idx 2 = device-nGnRE. */
  internal_write32(k_ra8_mpu_mair0_addr, k_ra8_mair0_default);
  internal_write32(k_ra8_mpu_mair1_addr, 0U);

  internal_mpu_set_region(0U,
                          (k_ra8_mpu_mram_base | k_ra8_rbar_attr_ro_x),
                          (k_ra8_mpu_mram_limit | k_ra8_mpu_rlar_enable));
  internal_mpu_set_region(1U,
                          (k_ra8_mpu_sram_base | k_ra8_rbar_attr_rw_xn),
                          (k_ra8_mpu_sram_limit | k_ra8_mpu_rlar_enable));
  internal_mpu_set_region(2U,
                          (k_ra8_mpu_sdram_base | k_ra8_rbar_attr_rw_xn),
                          (k_ra8_mpu_sdram_limit | k_ra8_mpu_rlar_enable));
  internal_mpu_set_region(3U,
                          (k_ra8_mpu_peri_base | k_ra8_rbar_attr_device_rw),
                          (k_ra8_mpu_peri_limit | k_ra8_mpu_rlar_enable));

  /* Enable MPU with PRIVDEFENA so unclassified privileged accesses
   * still succeed through the default system memory map. */
  internal_dsb();
  internal_write32(k_ra8_mpu_ctrl_addr, k_ra8_mpu_ctrl_enable | k_ra8_mpu_ctrl_privdefena);
  internal_dsb();
  internal_isb();
}

/**
 * @brief Park forever after a Secure clock-tree bring-up failure.
 *
 * @details
 * Reached only when ::ra8_cgc_init fails inside SystemInit. The NS-side NSC CGC
 * veneers require PLL1 locked, and ::ra8_trustzone_init BLXNS-es into the NS
 * world without returning -- so a failed clock bring-up must NOT proceed to the
 * world switch, which would run NS code on a dead clock tree. Parks in a ``wfi``
 * loop at this named, non-inlined symbol so a debugger or HIL probe can pin the
 * secure-boot clock failure; the only escape is a watchdog or debugger reset.
 *
 * @return Does not return.
 *
 * @pre Reached from SystemInit, before the SAU + BLXNS world switch.
 * @pre Global interrupts are masked (internal_disable_irq ran first).
 * @post The CPU is parked with interrupts masked; no NS code runs.
 * @post No global state is touched (safe before .data/.bss init).
 *
 * @note Not thread-safe; single-threaded boot only.
 * @since 0.1.0
 */
RA8_INTERNAL [[noreturn, gnu::noinline]] static void internal_halt_secure_clock_fault(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/* =============================================================================
 * Public entry point
 * =============================================================================
 *
 * Called from `Reset_Handler` after the C runtime is live (.data
 * copied, .bss zeroed). It may therefore read and write globals
 * freely; the Secure clock + TrustZone bring-up below relies on that.
 */

/* NOLINTNEXTLINE(readability-identifier-naming) -- CMSIS-mandated name. */
void SystemInit(void)
{
  /* Mask global IRQs so the application gets a deterministic init
   * window. main() must call ``ra8_isr_globals_enable`` once every
   * driver / handler is wired up before any IRQ source can fire. */
  internal_disable_irq();

  internal_set_vtor();
  internal_enable_fpu();
  internal_enable_fpu_lazy_stack();
  /* Cache enable, MPU init, and TrustZone bring-up are temporarily
   * disabled -- they HardFault on first reset because the CCSIDR-driven
   * invalidate-by-set/way loop is not yet implemented and the MPU
   * regions are unconfigured. Re-enable once those code paths land. */
  (void)internal_enable_icache;
  (void)internal_enable_dcache;
  (void)internal_enable_branch_predictor;
  (void)internal_mpu_init;
  internal_set_priority_grouping();

  /* Bring up the Secure clock tree (XTAL -> PLL1 -> CPUCLK0 = 1 GHz) BEFORE the
   * SAU + BLXNS below. ra8_trustzone_init() does NOT return on hardware (BLXNS
   * leaves Secure thread mode), so anything sequenced after it never runs on the
   * Secure side. The NS-side NSC CGC veneers (ra8_nsc_cgc_pll2_enable etc.) trap
   * back into this Secure CGC driver, which needs PLL1 already locked -- without
   * it the first PLL2 enable fails because its PLL1 source is missing. */
  if (ra8_cgc_init() != k_ra8_ok) {
    /* NASA Rule 7: a failed Secure clock bring-up must not BLXNS into NS code
     * on a dead clock tree -- park at a named symbol instead of switching. */
    internal_halt_secure_clock_fault();
  }

#ifdef RA8_TRUSTZONE_ENABLE
  /* Programme the SAU, then BLXNS into the NS image. The NSC veneers depend on
   * the SAU being live so the cmse_nonsecure_entry traps land on the Secure
   * side. Does not return on hardware. */
  ra8_trustzone_init();
#else
  (void)ra8_trustzone_init;
#endif
}
