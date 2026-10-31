/**
 * @file examples/ek_ra8d2/hw_pending/secure_boot_ns_hil/src/system_init.c
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
 * `SystemInit()` follows the CMSIS naming convention and runs out of
 * reset right *after* `Reset_Handler` copies .data and zeroes .bss
 * (see `Reset_Handler()`). Its responsibilities are strictly CPU-core
 * level -- anything peripheral-bus-side belongs in
 * `ra8_infrastructure_init()` called from `main()` after the C
 * runtime is live.
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
 * The function is C, not naked asm, so stack, `.data` and `.bss` must
 * already be usable. `Reset_Handler` loads SP from the vector table before
 * calling `SystemInit()`, and now copies `.data` + zeroes `.bss` BEFORE the
 * `SystemInit()` call, so any global state this function establishes
 * survives into `main()`. This ordering is load-bearing: `ra8_cgc_init()`
 * (called below) publishes the settled clock rates into a `.data`-resident
 * cache in `ra8_cgc`; if the `.data` copy ran after `SystemInit()` it would
 * clobber that cache back to the MOCO boot default and the SCI8 console
 * would then read a stale ~8 MHz PCLKA and refuse to initialise.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_boot_entry.h"
#include "ra8_cache.h"
#include "ra8_mpu.h"
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
  k_ra8_fpu_fpccr_addr = 0xE000EF34UL, /**< FPU Context Control Register.              */
  k_ra8_nvic_aircr     = 0xE000ED0CUL, /**< Application Interrupt and Reset Ctrl.      */
} ra8_core_addr_t;

extern uint32_t g_ra8_ls_stack_top; /* from vector_table.c / linker. */

/* =============================================================================
 * Small helpers
 * =============================================================================
 */

static inline uint32_t internal_read32(uintptr_t addr)
{
  return *(volatile uint32_t*)addr;
}

static inline void internal_write32(uintptr_t addr, uint32_t value)
{
  *(volatile uint32_t*)addr = value;
}

static inline void internal_dsb(void)
{
#ifndef RA8_OFF_TARGET
  __asm__ volatile("dsb 0xF" ::: "memory");
#endif
}

static inline void internal_isb(void)
{
#ifndef RA8_OFF_TARGET
  __asm__ volatile("isb 0xF" ::: "memory");
#endif
}

static inline void internal_disable_irq(void)
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
 */
static void internal_set_vtor(void)
{
  /* Vector table sits at the start of the .vectors section, which
   * the linker pins to the start of MRAM (`0x02000000`). The SCB
   * VTOR register stores the absolute address. */
  internal_write32(k_ra8_scb_vtor_addr, (uint32_t)(uintptr_t)g_ra8_vector_table_start);
}

/**
 * @brief Grant full access to CP10 / CP11 (the FPU coprocessors).
 */
static void internal_enable_fpu(void)
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
 */
static void internal_enable_fpu_lazy_stack(void)
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
 * @brief Enable branch-target prediction (CCR.BP on Cortex-M85).
 */
static void internal_enable_branch_predictor(void)
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
 */
static void internal_set_priority_grouping(void)
{
  enum : uint32_t {
    k_ra8_aircr_vectkey    = 0x05FA0000UL, /**< Required write key.     */
    k_ra8_aircr_prigroup_4 = 0x00000300UL, /**< PRIGROUP = 3 -> 4 bits. */
  };
  internal_write32(k_ra8_nvic_aircr, k_ra8_aircr_vectkey | k_ra8_aircr_prigroup_4);
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
 * @pre Global interrupts are masked (ra8_boot_disable_irq ran first).
 * @post The CPU is parked with interrupts masked; no NS code runs.
 * @post No global state is touched.
 *
 * @note Not thread-safe; single-threaded boot only.
 * @since 0.1.0
 */
[[noreturn, gnu::noinline]] static void internal_halt_secure_clock_fault(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/* =============================================================================
 * Public entry point
 * =============================================================================
 *
 * Called from `Reset_Handler` after the .data copy and .bss zero, which
 * the Secure clock bring-up and the TrustZone hand-off below rely on.
 */

void SystemInit(void)
{
  /* Mask global IRQs so the application gets a deterministic init
   * window. main() must call ``ra8_isr_globals_enable`` once every
   * driver / handler is wired up before any IRQ source can fire. */
  internal_disable_irq();

  internal_set_vtor();
  internal_enable_fpu();
  internal_enable_fpu_lazy_stack();
  /* Cache enable and MPU init stay unsequenced here -- they HardFault on
   * first reset while the MPU regions are unconfigured. The cache half runs
   * through ra8_cache_enable, whose D-cache enable carries the full
   * CCSIDR-driven invalidate-by-set/way loop the inline version only sketched;
   * the MPU half runs through ra8_mpu_apply_boot_map, the canonical 5-region
   * boot attribute map (MRAM RO+X cacheable, SRAM/SDRAM RW/XN cacheable,
   * peripherals Device-nGnRE, plus the shared M85<->M33 SRAM bank as its own
   * Normal non-cacheable region so the mailbox stays coherent with the
   * D-cache). Re-enable both once the boot image validates on the HIL rig. */
  (void)ra8_cache_enable;
  (void)ra8_mpu_apply_boot_map;
  (void)internal_enable_branch_predictor;
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
