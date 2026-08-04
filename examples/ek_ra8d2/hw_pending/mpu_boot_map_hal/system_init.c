/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_pending/mpu_boot_map_hal/system_init.c
 * @brief Cortex-M85 / RA8D2 core bring-up -- MPU map via the ra8_mpu HAL (#576)
 *
 * @details
 * Per-app override of the shared board `SystemInit()` that installs the boot
 * MPU memory-attribute map through the `ra8_mpu` HAL
 * (`ra8_mpu_apply_boot_map()`) instead of hand-rolling the MAIR / RBAR / RLAR /
 * CTRL register pokes. It is kept ALONGSIDE the raw-poke reference
 * (`libs/ra8_board_ek_ra8d2/boot/system_init.c`, used by e.g. `cache_mpu_hil`):
 * every other bring-up step here is byte-identical, so a `diff` of the two
 * files shows exactly the raw-pokes-vs-HAL boot difference, and the two apps
 * can be compared on HIL.
 *
 * `SystemInit()` follows the CMSIS naming convention and runs as the first C
 * function out of reset, *before* `Reset_Handler` copies .data or zeroes .bss.
 * `ra8_mpu_apply_boot_map()` is safe in that window: it reads only the
 * driver-owned `const` region table (in `.rodata`) and MMIO, writes no
 * `.data`/`.bss`, and never logs -- exactly as this same function already calls
 * `ra8_cache_dcache_invalidate_all()` from boot.
 *
 * ## Bring-up sequence
 *
 * 1. Set VTOR to the physical address of the vector table.
 * 2. Enable CP10 / CP11 so the FPU is usable before any FP instruction runs.
 * 3. Enable FPU lazy stacking (FPCCR.LSPEN).
 * 4. Enable the configurable-fault handlers and the divide-by-zero trap.
 * 5. (cache+MPU build) Install the boot MPU map via the HAL, then enable the
 *    I-cache / D-cache / branch predictor.
 * 6. Programme NVIC priority grouping to 4 bits preempt / 0 bits sub.
 * 7. Disable interrupts until the application is ready.
 */

#include <stdint.h>

#include "ra8_boot_entry.h"
#include "ra8_boot_intrinsics.h"
#include "ra8_cache.h"
#include "ra8_err.h"
#include "ra8_mpu.h"
#include "trustzone_init.h"

extern const uint32_t g_ra8_vector_table_start[];

/* =============================================================================
 * Core / SCB / NVIC register addresses (direct, no CMSIS dep)
 * =============================================================================
 * The MPU register block (MPU_TYPE/CTRL/RNR/RBAR/RLAR/MAIR0/MAIR1) is
 * deliberately absent here: this app reaches the MPU through the ra8_mpu HAL,
 * which owns those addresses, rather than poking them from boot.
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
 * Core init steps
 * =============================================================================
 */

/**
 * @brief Point VTOR at the physical vector table base.
 * @details Writes the absolute vector-table address into SCB->VTOR so
 *   exceptions dispatch through the linker-pinned table in MRAM.
 * @pre Running privileged out of reset.
 * @pre g_ra8_vector_table_start resolves to the .vectors base.
 * @post SCB->VTOR holds the vector-table base address.
 * @post No other core state is changed.
 * @note Not thread-safe; single-threaded boot only.
 * @since 0.1.0
 */
static void internal_set_vtor(void)
{
  /* Vector table sits at the start of the .vectors section, which
   * the linker pins to the start of MRAM (`0x02000000`). The SCB
   * VTOR register stores the absolute address. */
  ra8_boot_write32(k_ra8_scb_vtor_addr, (uint32_t)(uintptr_t)g_ra8_vector_table_start);
}

/**
 * @brief Grant full access to CP10 / CP11 (the FPU coprocessors).
 * @details Sets the CPACR access bits then barriers so the FPU is usable
 *   before any floating-point instruction executes.
 * @pre Running privileged out of reset.
 * @pre No floating-point instruction has executed yet.
 * @post CP10/CP11 have full access; the FPU is usable.
 * @post A dsb/isb pair has retired the change.
 * @note Not thread-safe; single-threaded boot only.
 * @since 0.1.0
 */
static void internal_enable_fpu(void)
{
  enum : uint32_t {
    k_ra8_cpacr_cp10_cp11_full_access = 0x00F00000UL, /**< RA8 cpacr cp10 cp11 full access. */
  };
  uint32_t cpacr = ra8_boot_read32(k_ra8_scb_cpacr_addr);
  cpacr |= k_ra8_cpacr_cp10_cp11_full_access;
  ra8_boot_write32(k_ra8_scb_cpacr_addr, cpacr);
  ra8_boot_dsb();
  ra8_boot_isb();
}

/**
 * @brief Turn on FPU lazy stacking (FPCCR.LSPEN = 1, ASPEN = 1).
 * @details Lets ISRs skip the full FP-state save unless they touch the
 *   FPU, reducing worst-case interrupt latency.
 * @pre Running privileged out of reset.
 * @pre internal_enable_fpu() has granted CP10/CP11 access.
 * @post FPCCR.LSPEN and FPCCR.ASPEN are set.
 * @post No other core state is changed.
 * @note Not thread-safe; single-threaded boot only.
 * @since 0.1.0
 */
static void internal_enable_fpu_lazy_stack(void)
{
  enum : uint32_t {
    k_ra8_fpccr_lspen = 1UL << 30, /**< RA8 fpccr lspen. */
    k_ra8_fpccr_aspen = 1UL << 31, /**< RA8 fpccr aspen. */
  };
  uint32_t fpccr = ra8_boot_read32(k_ra8_fpu_fpccr_addr);
  fpccr |= k_ra8_fpccr_lspen | k_ra8_fpccr_aspen;
  ra8_boot_write32(k_ra8_fpu_fpccr_addr, fpccr);
}

/**
 * @brief Invalidate and enable the Cortex-M85 I-cache.
 * @details Invalidates the whole I-cache (ICIALLU) with the surrounding
 *   barriers, then sets CCR.IC. Gated with the D-cache and MPU behind the
 *   cache+MPU build flag.
 * @pre Running privileged out of reset with the vector table installed.
 * @pre The MPU attribute map is installed so instruction fetches see the
 *   correct memory type.
 * @post CCR.IC is set and the I-cache is enabled.
 * @post A dsb/isb pair retires the change.
 * @note Not thread-safe; single-threaded boot only.
 * @since 0.1.0
 */
[[maybe_unused]] static void internal_enable_icache(void)
{
  /* Invalidate, then set CCR.IC. */
  ra8_boot_dsb();
  ra8_boot_write32(k_ra8_scb_iciallu, 0U);
  ra8_boot_dsb();
  ra8_boot_isb();

  enum : uint32_t { k_ra8_ccr_ic = 1UL << 17 /**< RA8 ccr ic. */ };
  uint32_t ccr = ra8_boot_read32(k_ra8_scb_ccr_addr);
  ccr |= k_ra8_ccr_ic;
  ra8_boot_write32(k_ra8_scb_ccr_addr, ccr);
  ra8_boot_dsb();
  ra8_boot_isb();
}

/**
 * @brief Invalidate and enable the Cortex-M85 D-cache.
 *
 * @details
 * Runs the full CCSIDR-driven invalidate-by-set/way pass
 * (`ra8_cache_dcache_invalidate_all()`) BEFORE setting `CCR.DC`, so no random
 * power-on line content is ever treated as valid once the cache is enabled.
 *
 * @pre The MPU attribute map is installed (non-cacheable shared SRAM +
 *   Device-nGnRE MMIO) so the D-cache never caches what must stay coherent.
 * @pre Running privileged out of reset.
 * @post CCR.DC is set and the D-cache is enabled.
 * @post A dsb/isb pair retires the change.
 * @note Not thread-safe; single-threaded boot only.
 * @since 0.1.0
 */
[[maybe_unused]] static void internal_enable_dcache(void)
{
  /* Architectural ARMv8-M set/way invalidate of the whole D-cache,
   * driven by CCSIDR (implemented in libs/ra8_hal ra8_cache). */
  ra8_cache_dcache_invalidate_all();
  enum : uint32_t { k_ra8_ccr_dc = 1UL << 16 /**< RA8 ccr dc. */ };
  uint32_t ccr = ra8_boot_read32(k_ra8_scb_ccr_addr);
  ccr |= k_ra8_ccr_dc;
  ra8_boot_write32(k_ra8_scb_ccr_addr, ccr);
  ra8_boot_dsb();
  ra8_boot_isb();
}

/**
 * @brief Enable branch-target prediction (CCR.BP on Cortex-M85).
 * @details Sets CCR.BP so the branch predictor is active with the caches.
 * @pre Running privileged out of reset.
 * @pre The I-cache is already enabled.
 * @post CCR.BP is set.
 * @post A dsb/isb pair retires the change.
 * @note Not thread-safe; single-threaded boot only.
 * @since 0.1.0
 */
[[maybe_unused]] static void internal_enable_branch_predictor(void)
{
  enum : uint32_t { k_ra8_ccr_bp = 1UL << 18 /**< RA8 ccr bp. */ };
  uint32_t ccr = ra8_boot_read32(k_ra8_scb_ccr_addr);
  ccr |= k_ra8_ccr_bp;
  ra8_boot_write32(k_ra8_scb_ccr_addr, ccr);
  ra8_boot_dsb();
  ra8_boot_isb();
}

/**
 * @brief Enable the configurable-fault handlers so faults decode by class.
 *
 * @details
 * Out of reset SHCSR.{MEM,BUS,USG,SECURE}FAULTENA are 0, so MemManage,
 * BusFault, UsageFault and SecureFault all escalate to HardFault and the
 * per-class exception trampolines never fire. Set the four enables so each
 * configurable fault is taken by its own handler and reported with its true
 * class. Always-on diagnostics, independent of the cache / MPU build flag.
 *
 * @pre Running privileged out of reset with the vector table installed.
 * @pre The per-app vector table forwards exc 4/5/6/7 into ra8_exception_report.
 * @post SHCSR.MEMFAULTENA, BUSFAULTENA, USGFAULTENA and SECUREFAULTENA are set.
 * @post A dsb/isb pair retires the change before the first fault can be taken.
 * @note Not thread-safe; single-threaded boot only.
 * @since 0.1.0
 */
static void internal_enable_fault_handlers(void)
{
  /* ARMv8-M SCB->SHCSR: MEMFAULTENA[16], BUSFAULTENA[17], USGFAULTENA[18],
   * SECUREFAULTENA[19] take MemManage/BusFault/UsageFault/SecureFault to
   * their own handlers instead of escalating to HardFault, so
   * ra8_exception_report sees the real class. SECUREFAULTENA is a
   * Secure-bank bit (RES0 from NS); this boot executes in the Secure state. */
  enum : uint32_t {
    k_ra8_shcsr_memfaultena    = 1UL << 16, /**< RA8 shcsr memfaultena.    */
    k_ra8_shcsr_busfaultena    = 1UL << 17, /**< RA8 shcsr busfaultena.    */
    k_ra8_shcsr_usgfaultena    = 1UL << 18, /**< RA8 shcsr usgfaultena.    */
    k_ra8_shcsr_securefaultena = 1UL << 19, /**< RA8 shcsr securefaultena. */
  };
  uint32_t shcsr = ra8_boot_read32(k_ra8_scb_shcsr_addr);
  shcsr |= k_ra8_shcsr_memfaultena | k_ra8_shcsr_busfaultena | k_ra8_shcsr_usgfaultena |
           k_ra8_shcsr_securefaultena;
  ra8_boot_write32(k_ra8_scb_shcsr_addr, shcsr);
  ra8_boot_dsb();
  ra8_boot_isb();
}

/**
 * @brief Make integer divide-by-zero trap as a decoded UsageFault.
 *
 * @details
 * Out of reset CCR.DIV_0_TRP is 0 and an SDIV/UDIV by zero silently returns
 * 0. Setting the trap makes a zero divisor raise UsageFault with
 * CFSR.DIVBYZERO, which internal_enable_fault_handlers() has already routed
 * to the decoding UsageFault_Handler trampoline. CCR.UNALIGN_TRP stays 0 --
 * the toolchain emits hardware-supported unaligned accesses by default.
 *
 * @pre Running privileged out of reset with the vector table installed.
 * @pre internal_enable_fault_handlers() has set SHCSR.USGFAULTENA.
 * @post CCR.DIV_0_TRP is set; a zero divisor now raises UsageFault.
 * @post A dsb/isb pair retires the change before any divide executes.
 * @note Not thread-safe; single-threaded boot only.
 * @since 0.1.0
 */
static void internal_enable_div0_trap(void)
{
  /* ARMv8-M SCB->CCR: DIV_0_TRP[4] promotes SDIV/UDIV-by-zero from
   * "quotient reads as 0" to a UsageFault with CFSR.DIVBYZERO set. */
  enum : uint32_t {
    k_ra8_ccr_div_0_trp = 1UL << 4, /**< RA8 ccr div 0 trp. */
  };
  uint32_t ccr = ra8_boot_read32(k_ra8_scb_ccr_addr);
  ccr |= k_ra8_ccr_div_0_trp;
  ra8_boot_write32(k_ra8_scb_ccr_addr, ccr);
  ra8_boot_dsb();
  ra8_boot_isb();
}

/**
 * @brief Set NVIC priority grouping to 4 preempt bits / 0 sub-priority.
 * @details Writes AIRCR with the VECTKEY and PRIGROUP=3, the standard
 *   embedded split (all priority bits are preempt, none sub-priority).
 * @pre Running privileged out of reset.
 * @pre Interrupts are still masked.
 * @post AIRCR.PRIGROUP = 3 (4 preempt bits, 0 sub-priority bits).
 * @post No reset was triggered (correct VECTKEY used).
 * @note Not thread-safe; single-threaded boot only.
 * @since 0.1.0
 */
static void internal_set_priority_grouping(void)
{
  enum : uint32_t {
    k_ra8_aircr_vectkey    = 0x05FA0000UL, /**< Required write key.     */
    k_ra8_aircr_prigroup_4 = 0x00000300UL, /**< PRIGROUP = 3 -> 4 bits. */
  };
  ra8_boot_write32(k_ra8_nvic_aircr, k_ra8_aircr_vectkey | k_ra8_aircr_prigroup_4);
}

/* =============================================================================
 * Public entry point
 * =============================================================================
 *
 * Called from `Reset_Handler` before the .data copy and .bss zero.
 * Must therefore not touch any global variables -- everything here
 * runs on the stack and writes to CPU / SCB memory only. The MPU HAL call
 * reads a `const` .rodata table only, so it is safe in this window.
 */

/* NOLINTNEXTLINE(readability-identifier-naming) -- CMSIS-mandated name. */
void SystemInit(void)
{
  /* Mask global IRQs so the application gets a deterministic init
   * window. main() must call ``ra8_isr_globals_enable`` once every
   * driver / handler is wired up before any IRQ source can fire. */
  ra8_boot_disable_irq();

  internal_set_vtor();
  internal_enable_fpu();
  internal_enable_fpu_lazy_stack();
  /* Always-on fault diagnostics: take MemManage/BusFault/UsageFault/
   * SecureFault to their own decoded handlers and trap divide-by-zero
   * as a UsageFault (independent of the cache / MPU build flag). */
  internal_enable_fault_handlers();
  internal_enable_div0_trap();
#ifdef RA8_BOOT_ENABLE_CACHE_MPU
  /* #576 demo: install the MPU memory-attribute map through the ra8_mpu HAL
   * (ra8_mpu_apply_boot_map) instead of the raw MAIR/RBAR/RLAR/CTRL pokes the
   * shared board system_init.c uses -- the two boot paths are otherwise
   * identical and diff cleanly on HIL. The map goes down FIRST (so the D-cache
   * sees Device-nGnRE MMIO and the non-cacheable shared SRAM, not Normal
   * cacheable), then the caches. Enable the caches only if the map installed. */
  if (ra8_mpu_apply_boot_map() == k_ra8_ok) {
    internal_enable_icache();
    internal_enable_dcache();
    internal_enable_branch_predictor();
  }
#else
  /* Default OFF: caches + MPU stay disabled -- no behaviour change. */
  (void)internal_enable_icache;
  (void)internal_enable_dcache;
  (void)internal_enable_branch_predictor;
#endif
  (void)ra8_trustzone_init; /* TrustZone has its own gate (RA8_TRUSTZONE_ENABLE). */
  internal_set_priority_grouping();
}
