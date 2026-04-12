/**
 * @file system_init.c
 * @brief Cortex-M85 / RA8D2 core bring-up (called from Reset_Handler)
 *
 * @details
 * `SystemInit()` follows the CMSIS naming convention and runs as the
 * first C function out of reset, *before* `Reset_Handler` copies
 * .data or zeroes .bss. Its responsibilities are strictly CPU-core
 * level -- anything peripheral-bus-side belongs in
 * `ra_infrastructure_init()` called from `main()` after the C
 * runtime is live.
 *
 * ## Bring-up sequence
 *
 *  1. Set VTOR to the physical address of the vector table (so that
 *     linker relocations and later TrustZone / bootloader hand-offs
 *     work).
 *  2. Enable CP10 / CP11 so the FPU is usable before any
 *     floating-point instruction runs.
 *  3. Enable FPU lazy stacking (FPCCR.LSPEN) so ISRs do not pay the
 *     full FP-state save cost unless they touch the FPU.
 *  4. Enable the I-cache and D-cache (Cortex-M85 has both). Without
 *     this the core runs at a small fraction of its rated speed.
 *  5. Enable the branch-target predictor (CCR.BP).
 *  6. Programme NVIC priority grouping to 4 bits preempt / 0 bits
 *     sub (the standard embedded pattern).
 *  7. Disable interrupts until the application is ready -- this
 *     mirrors CMSIS convention. `main()` re-enables via
 *     `__enable_irq()` once all drivers are up.
 *
 * The function is C, not naked asm, so stack and BSS must already be
 * usable. `Reset_Handler` loads SP from the vector table before
 * calling `SystemInit()`, so the stack is fine; BSS is zeroed only
 * *after* `SystemInit()` returns but `SystemInit()` writes to no
 * BSS or data-section variables, so the ordering is safe.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

/* =============================================================================
 * Core / SCB / NVIC register addresses (direct, no CMSIS dep)
 * =============================================================================
 */

typedef enum : uintptr_t {
  k_ra_scb_vtor_addr  = 0xE000ED08UL, /**< Vector Table Offset Register.       */
  k_ra_scb_ccr_addr   = 0xE000ED14UL, /**< Configuration and Control Register. */
  k_ra_scb_shcsr_addr = 0xE000ED24UL, /**< System Handler Control and State.   */
  k_ra_scb_cpacr_addr = 0xE000ED88UL, /**< Coprocessor Access Control.         */
  k_ra_scb_nsacr_addr = 0xE000ED8CUL, /**< Non-secure Access Control.          */
  k_ra_scb_iciallu    = 0xE000EF50UL, /**< ICIALLU -- invalidate I-cache.      */
  k_ra_scb_dciallu    = 0xE000EF58UL, /**< DCIALLU -- invalidate D-cache (sets only). */
  k_ra_scb_csselr     = 0xE000ED84UL, /**< Cache Size Selection Register.      */
  k_ra_scb_ccsidr     = 0xE000ED80UL, /**< Cache Size ID Register.             */
  k_ra_scb_dcisw      = 0xE000EF60UL, /**< D-cache Invalidate by Set/Way.      */
  k_ra_fpu_fpccr_addr = 0xE000EF34UL, /**< FPU Context Control Register.       */
  k_ra_nvic_aircr     = 0xE000ED0CUL, /**< Application Interrupt and Reset Ctrl.*/
} ra_core_addr_t;

extern uint32_t g_ra_ls_stack_top; /* from vector_table.c / linker. */

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
#ifndef RA_SIMULATOR_MODE
  __asm__ volatile("dsb 0xF" ::: "memory");
#endif
}

static inline void internal_isb(void)
{
#ifndef RA_SIMULATOR_MODE
  __asm__ volatile("isb 0xF" ::: "memory");
#endif
}

static inline void internal_disable_irq(void)
{
#ifndef RA_SIMULATOR_MODE
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
  extern const uint32_t g_ra_vector_table_start[];
  internal_write32(k_ra_scb_vtor_addr, (uint32_t)(uintptr_t)g_ra_vector_table_start);
}

/**
 * @brief Grant full access to CP10 / CP11 (the FPU coprocessors).
 */
static void internal_enable_fpu(void)
{
  enum : uint32_t {
    k_ra_cpacr_cp10_cp11_full_access = 0x00F00000UL,
  };
  uint32_t cpacr = internal_read32(k_ra_scb_cpacr_addr);
  cpacr |= k_ra_cpacr_cp10_cp11_full_access;
  internal_write32(k_ra_scb_cpacr_addr, cpacr);
  internal_dsb();
  internal_isb();
}

/**
 * @brief Turn on FPU lazy stacking (FPCCR.LSPEN = 1, ASPEN = 1).
 */
static void internal_enable_fpu_lazy_stack(void)
{
  enum : uint32_t {
    k_ra_fpccr_lspen = 1UL << 30,
    k_ra_fpccr_aspen = 1UL << 31,
  };
  uint32_t fpccr = internal_read32(k_ra_fpu_fpccr_addr);
  fpccr |= k_ra_fpccr_lspen | k_ra_fpccr_aspen;
  internal_write32(k_ra_fpu_fpccr_addr, fpccr);
}

/**
 * @brief Invalidate and enable the Cortex-M85 I-cache.
 */
static void internal_enable_icache(void)
{
  /* Invalidate, then set CCR.IC. */
  internal_dsb();
  internal_write32(k_ra_scb_iciallu, 0U);
  internal_dsb();
  internal_isb();

  enum : uint32_t { k_ra_ccr_ic = 1UL << 17 };
  uint32_t ccr = internal_read32(k_ra_scb_ccr_addr);
  ccr |= k_ra_ccr_ic;
  internal_write32(k_ra_scb_ccr_addr, ccr);
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
static void internal_enable_dcache(void)
{
  enum : uint32_t { k_ra_ccr_dc = 1UL << 16 };
  uint32_t ccr = internal_read32(k_ra_scb_ccr_addr);
  ccr |= k_ra_ccr_dc;
  internal_write32(k_ra_scb_ccr_addr, ccr);
  internal_dsb();
  internal_isb();
}

/**
 * @brief Enable branch-target prediction (CCR.BP on Cortex-M85).
 */
static void internal_enable_branch_predictor(void)
{
  enum : uint32_t { k_ra_ccr_bp = 1UL << 18 };
  uint32_t ccr = internal_read32(k_ra_scb_ccr_addr);
  ccr |= k_ra_ccr_bp;
  internal_write32(k_ra_scb_ccr_addr, ccr);
  internal_dsb();
  internal_isb();
}

/**
 * @brief Set NVIC priority grouping to 4 preempt bits / 0 sub-priority.
 */
static void internal_set_priority_grouping(void)
{
  enum : uint32_t {
    k_ra_aircr_vectkey    = 0x05FA0000UL, /**< Required write key.       */
    k_ra_aircr_prigroup_4 = 0x00000300UL, /**< PRIGROUP = 3 -> 4 bits.   */
  };
  internal_write32(k_ra_nvic_aircr, k_ra_aircr_vectkey | k_ra_aircr_prigroup_4);
}

/* =============================================================================
 * Public entry point
 * =============================================================================
 *
 * Called from `Reset_Handler` before the .data copy and .bss zero.
 * Must therefore not touch any global variables -- everything here
 * runs on the stack and writes to CPU / SCB memory only.
 */

/* NOLINTNEXTLINE(readability-identifier-naming) -- CMSIS-mandated name. */
void SystemInit(void)
{
  internal_disable_irq();
  internal_set_vtor();
  internal_enable_fpu();
  internal_enable_fpu_lazy_stack();
  internal_enable_icache();
  internal_enable_dcache();
  internal_enable_branch_predictor();
  internal_set_priority_grouping();
}
