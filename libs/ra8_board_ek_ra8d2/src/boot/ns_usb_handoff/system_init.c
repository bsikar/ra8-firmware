/**
 * @file libs/ra8_board_ek_ra8d2/src/boot/ns_usb_handoff/system_init.c
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
 * The function is C, not naked asm, so the stack must already be
 * usable. `Reset_Handler` loads SP from the vector table before
 * calling `SystemInit()`, so the stack is fine.
 *
 * .data and .bss ARE initialised here, and must be: `ra8_trustzone_init()`
 * BLXNS-es into the Non-Secure image and never returns, so any RAM init
 * sequenced after `SystemInit()` would never run. `ra8_cgc_init()` (its
 * `s_tag` and clock-rate cache), the USB hand-off's I/O-expander hook and
 * every Secure driver the NSC veneers reach all depend on it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "trustzone_init.h"
#include "ra8_mpu.h"

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
 * @brief Invalidate and enable the Cortex-M85 I-cache.
 */
static void internal_enable_icache(void)
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
static void internal_enable_dcache(void)
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
  /* Cache enable stays temporarily disabled -- it HardFaults on first reset
   * because the CCSIDR-driven invalidate-by-set/way loop is not yet
   * implemented. The MPU half of this arm is routed through the HAL:
   * ra8_mpu_apply_boot_map installs the canonical RA8D2 boot attribute map
   * (RA8FW-1074 did the same for secure_boot_ns_hil, RA8FW-1080 for
   * lowpower_holdpage, fixing the SRAM/shared-bank overlap and the
   * MAIR0[1] encoding-vs-comment mismatch the hand-rolled copy carried).
   * The whole arm stays unsequenced, so no ordering is claimed across it. */
  (void)internal_enable_icache;
  (void)internal_enable_dcache;
  (void)internal_enable_branch_predictor;
  (void)ra8_mpu_apply_boot_map;
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
