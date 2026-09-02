/**
 * @file libs/ra8_board_ek_ra8d2/src/boot/system_init.c
 * @brief Cortex-M85 / RA8D2 core bring-up (called from Reset_Handler)
 *
 * @details
 * `SystemInit()` follows the CMSIS naming convention and runs as the
 * first C function out of reset, *before* `Reset_Handler` copies
 * .data or zeroes .bss. Its responsibilities are strictly CPU-core
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
 * 4. Enable the configurable-fault handlers (SHCSR MEM/BUS/USG/
 * SECUREFAULTENA) and the divide-by-zero trap (CCR.DIV_0_TRP) so
 * every fault decodes with its true class instead of an anonymous
 * HardFault.
 * 5. Enable the I-cache and D-cache (Cortex-M85 has both). Without
 * this the core runs at a small fraction of its rated speed.
 * 6. Enable the branch-target predictor (CCR.BP).
 * 7. Programme NVIC priority grouping to 4 bits preempt / 0 bits
 * sub (the standard embedded pattern).
 * 8. Disable interrupts until the application is ready -- this
 * mirrors CMSIS convention. `main()` re-enables via
 * `__enable_irq()` once all drivers are up.
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

#include "ra8_board_ek_ra8d2_dualcore.h"
#include "ra8_boot_entry.h"
#include "ra8_boot_intrinsics.h"
#include "ra8_cache.h"
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
 * @details Invalidates all instruction-cache lines before setting CCR.IC and
 *   retires the state transition with data and instruction barriers.
 * @pre Execution is privileged during the single-threaded reset path.
 * @pre No application code depends on cached instructions yet.
 * @post CCR.IC is set and the instruction cache is enabled.
 * @post The invalidate and control-register write are globally observed.
 * @note Call once during reset before interrupts or secondary-core launch.
 * @since 0.1.0
 */
static void internal_enable_icache(void)
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
 * (`ra8_cache_dcache_invalidate_all()`) BEFORE setting `CCR.DC`, so no
 * random power-on line content is ever treated as valid once the cache
 * is enabled. This replaces the earlier "bulk CCR.DC = 1" shortcut,
 * which relied on an unverified boot-ROM guarantee that the lines came
 * up invalid. The function stays gated off (only address-taken in
 * `SystemInit`) until the coherency substrate -- clean-before-DMA and
 * the non-cacheable region -- lands; the actual enable is T4-06.
 * @pre Execution is privileged during the single-threaded reset path.
 * @pre No DMA or secondary-core transaction can observe cacheable data yet.
 * @post The data cache is invalidated and enabled through CCR.DC.
 * @post The cache and control-register changes are globally observed.
 * @note The board coherency policy must be installed before this is called.
 * @since 0.1.0
 */
static void internal_enable_dcache(void)
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
 * @details Sets the architectural branch-prediction enable bit and retires
 *   the control-register update with data and instruction barriers.
 * @pre Execution is privileged during the single-threaded reset path.
 * @pre No concurrent code modifies the System Control Block CCR.
 * @post CCR.BP is set and branch prediction is enabled.
 * @post The control-register write is globally observed.
 * @note Call once during reset before interrupts or secondary-core launch.
 * @since 0.1.0
 */
static void internal_enable_branch_predictor(void)
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
 * per-class exception trampolines (exc 4/5/6/7, which forward into
 * ra8_exception_report) never fire -- the real fault class is lost. Set the
 * four enables so each configurable fault is taken by its own handler and
 * reported with its true class. Always-on diagnostics, independent of the
 * cache / MPU build flag.
 *
 * SECUREFAULTENA (bit 19) scoping decision: the bit is banked and exists only
 * in the Secure view of SHCSR (from the Non-secure state it is RES0, so a
 * misplaced write is architecturally ignored). This shared SystemInit runs
 * out of reset, and the RA8D2 core always resets into the Secure state --
 * flat (non-TrustZone) apps simply never leave it -- so the write always
 * lands in the Secure bank. It is enabled unconditionally (no build flag)
 * because every image in this repository carries a decoded SecureFault
 * handler: the shared boot secure_exception.c trampoline, or a per-app
 * secure_exception.c override (all of which decode SFSR). With the bit off,
 * a TrustZone security violation (SG check, attribution mismatch, INVTRAN)
 * halts as an anonymous HardFault; with it on, the same violation reaches
 * SecureFault_Handler and is recorded with its true class + SFSR/SFAR. No
 * non-fault code path changes, so HW-validated apps see identical behaviour
 * until something actually faults.
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
   * Secure-bank bit (RES0 from NS); this boot executes in the Secure
   * state, see the function contract for the scoping rationale. */
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
 * 0 -- the strongest possible "garbage in, garbage onward" failure mode for
 * control code. Setting the trap makes a zero divisor raise UsageFault with
 * CFSR.DIVBYZERO, which internal_enable_fault_handlers() has already routed
 * to the decoding UsageFault_Handler trampoline, so the faulting PC and the
 * cause land in g_ra8_exception_last instead of propagating a bogus quotient.
 *
 * CCR.UNALIGN_TRP (bit 3) is deliberately NOT set. Evidence: this tree
 * compiles with arm-none-eabi-gcc's default `-munaligned-access` (confirmed
 * `[enabled]` for -mcpu=cortex-m85 via `-Q --help=target`), under which the
 * compiler is entitled to emit hardware-supported unaligned word accesses --
 * and does, e.g. when expanding the small fixed-size memcpy() idiom the
 * vendored SOUP decoders (miniz via MINIZ_UNALIGNED_USE_MEMCPY, stb) use for
 * byte-buffer reads, and for packed-struct field access. The prebuilt newlib
 * libc is compiled the same way. Trapping unaligned accesses would therefore
 * fault legitimate generated code; it can only ever be enabled together with
 * a whole-tree (and libc) rebuild under `-mno-unaligned-access`.
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
   * "quotient reads as 0" to a UsageFault with CFSR.DIVBYZERO set.
   * UNALIGN_TRP[3] stays 0 -- see the function contract for evidence. */
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

/**
 * @brief Programme and enable the Cortex-M85 core MPU.
 *
 * @details
 * Installs five regions and enables the MPU with PRIVDEFENA so privileged
 * mode still sees the default memory map for anything not explicitly covered.
 * Activated (with the caches) behind the RA8_BOOT_ENABLE_CACHE_MPU build flag;
 * default OFF leaves the MPU disabled and the boot unchanged.
 *
 * - Region 0: MRAM code (0x02000000, 1 MiB) -- RO + execute, cacheable.
 * - Region 1: M85 private SRAM0+1 (0x22000000, 1 MiB) -- RW/NX, cacheable.
 * - Region 2: SDRAM (0x68000000, 64 MiB) -- RW/NX, cacheable.
 * - Region 3: Peripherals (0x40000000, 128 MiB) -- RW/NX, Device-nGnRE.
 * - Region 4: M85<->M33 shared SRAM2+3 -- RW/NX, Normal NON-cacheable
 *   (mailbox + CPU1 RAM stay coherent across cores). Its base and extent
 *   come from ``ra8_board_ek_ra8d2_dualcore.h``, which is the one place
 *   this board states the dual-core memory map: the shared window plus
 *   CPU1's private bank above it, 640 KiB in total.
 *
 * Attribute indirection table:
 * - MAIR0[0] = 0xFF (Normal inner + outer write-back / write-alloc).
 * - MAIR0[1] = 0x44 (Normal non-cacheable).
 * - MAIR0[2] = 0x04 (Device-nGnRE).
 *
 * Per ARMv8-M PMSAv8: RBAR[4:0] = SH[4:3] | AP[2:1] | XN[0] (permissions);
 * RLAR = LIMIT[31:5] | AttrIndx[3:1] | EN[0], where AttrIndx selects the
 * MAIR byte that sets the region's memory TYPE (cacheable vs Device).
 *
 * @note The shared SRAM bank (region 4) is non-cacheable, so cross-core
 * hand-offs need no software cache maintenance (the M33 is cacheless). The
 * private SRAM limit is now tight (ends at 0x220FFFFF). SDRAM (region 2)
 * stays cacheable -- DMA buffers there still need a non-cacheable placement
 * or an explicit clean/invalidate at the DMA boundary (T4-02).
 */
/* RBAR attribute bits[4:0] = { SH[4:3], AP[2:1], XN[0] }. AP: 0b01<<1 = RW
 * any-privilege, 0b11<<1 = RO any-privilege. The memory TYPE is NOT here --
 * it is the RLAR AttrIndx (see internal_mpu_set_region). */
enum : uint32_t {
  k_ra8_rbar_attr_ro_x  = 0x06U,    /**< AP=RO any-priv, SH=none, XN=0 (executable). */
  k_ra8_rbar_attr_rw_xn = 0x03U,    /**< AP=RW any-priv, SH=none, XN=1 (no execute). */
  k_ra8_mpu_rlar_enable = 1UL << 0, /**< RA8 MPU rlar enable.                        */
};

/* RLAR AttrIndx (bits[3:1]) selects which MAIR byte sets the region's memory
 * type. The old code tried to encode "device" in RBAR (k_ra8_rbar_attr_device_rw
 * = 0x23), which both spilled bit5 into the base address and left every RLAR at
 * AttrIndx 0 = MAIR0[0] = Normal cacheable -- so peripheral MMIO was mapped
 * cacheable. The type must be selected here instead. */
enum : uint32_t {
  k_ra8_mpu_region_quantum       = 32U, /**< ARMv8-M PMSAv8 region granule, bytes.    */
  k_ra8_mpu_rlar_attridx_shift   = 1U,  /**< RA8 MPU rlar attridx shift.              */
  k_ra8_mpu_attridx_normal_wbwa  = 0U,  /**< MAIR0[0] 0xFF: Normal inner/outer WB/WA. */
  k_ra8_mpu_attridx_noncacheable = 1U,  /**< MAIR0[1] 0x44: Normal non-cacheable.     */
  k_ra8_mpu_attridx_device       = 2U,  /**< MAIR0[2] 0x04: Device-nGnRE (MMIO).      */
};

/* Region base and limit addresses. RLAR limits are <region-end> minus
 * the ARMv8-M 32-byte region quantum, OR-ed with the enable bit at write time. */
enum : uint32_t {
  k_ra8_mpu_mram_base  = 0x02000000UL, /**< 1 MiB MRAM code region.                */
  k_ra8_mpu_mram_limit = 0x020FFFE0UL, /**< RA8 MPU MRAM limit.                    */
  k_ra8_mpu_sram_base  = 0x22000000UL, /**< M85 private SRAM0+1, 1 MiB, cacheable. */
  k_ra8_mpu_sram_limit = 0x220FFFE0UL, /**< RA8 MPU SRAM limit.                    */
  k_ra8_mpu_shram_base =
    (uint32_t)k_ra8_board_shared_ram_base, /**< Shared SRAM2+3: M33 mailbox + CPU1 RAM. */
  k_ra8_mpu_shram_limit =
    (uint32_t)k_ra8_board_cpu1_sram_base + (uint32_t)k_ra8_board_cpu1_sram_size_bytes -
    k_ra8_mpu_region_quantum,           /**< Non-cacheable -> M85<->M33 stays coherent. */
  k_ra8_mpu_sdram_base  = 0x68000000UL, /**< 64 MiB external SDRAM.                     */
  k_ra8_mpu_sdram_limit = 0x6BFFFFE0UL, /**< RA8 MPU SDRAM limit.                       */
  k_ra8_mpu_peri_base   = 0x40000000UL, /**< Peripheral bus window base.                */
  k_ra8_mpu_peri_limit  = 0x47FFFFE0UL, /**< RA8 MPU peri limit.                        */
};

/**
 * @brief Program a single MPU region via RNR/RBAR/RLAR.
 *
 * @details @p attr_idx (written into RLAR AttrIndx, bits[3:1]) selects the
 *   region's MAIR byte and therefore its memory type -- this is what makes
 *   the peripheral region Device-nGnRE rather than Normal cacheable.
 * @param[in] region MPU region number to select.
 * @param[in] base_attr Aligned base address combined with RBAR attributes.
 * @param[in] limit_enable Aligned limit combined with RLAR enable.
 * @param[in] attr_idx MAIR attribute index for the region's memory type.
 * @pre Execution is privileged and the MPU is disabled.
 * @pre All address and attribute fields satisfy the ARMv8-M MPU encoding.
 * @post The selected region contains the supplied RBAR and RLAR values.
 * @post No other MPU region is modified.
 * @note This helper does not validate caller-prepared register encodings.
 * @since 0.1.0
 */
static void internal_mpu_set_region(uint32_t region,
                                    uint32_t base_attr,
                                    uint32_t limit_enable,
                                    uint32_t attr_idx)
{
  ra8_boot_write32(k_ra8_mpu_rnr_addr, region);
  ra8_boot_write32(k_ra8_mpu_rbar_addr, base_attr);
  ra8_boot_write32(k_ra8_mpu_rlar_addr, limit_enable | (attr_idx << k_ra8_mpu_rlar_attridx_shift));
}

/**
 * @brief Programme and enable the Cortex-M85 core MPU.
 * @details Installs code, private SRAM, SDRAM, peripheral, and shared-memory
 *   regions with explicit MAIR types before enabling the privileged map.
 * @pre Execution is privileged during the single-threaded reset path.
 * @pre No application or interrupt accesses memory under an earlier MPU map.
 * @post The five board regions and their MAIR attributes are installed.
 * @post The MPU is enabled with PRIVDEFENA and barriers have retired the map.
 * @note Call once during reset before caches, interrupts, or secondary-core launch.
 * @since 0.1.0
 */
static void internal_mpu_init(void)
{
  enum : uint32_t {
    k_ra8_mpu_ctrl_enable     = 1UL << 0,     /**< RA8 MPU control enable.     */
    k_ra8_mpu_ctrl_privdefena = 1UL << 2,     /**< RA8 MPU control privdefena. */
    k_ra8_mair0_default       = 0x000444FFUL, /**< RA8 mair0 default.          */
  };

  /* MAIR0: idx0 = Normal WB/WA, idx1 = Normal non-cacheable, idx2 = Device-nGnRE. */
  ra8_boot_write32(k_ra8_mpu_mair0_addr, k_ra8_mair0_default);
  ra8_boot_write32(k_ra8_mpu_mair1_addr, 0U);

  /* Code = RO + executable cacheable; RAM = RW/NX cacheable; peripherals =
   * Device-nGnRE (NOT cacheable -- the AttrIndx selects the type). */
  internal_mpu_set_region(0U,
                          (k_ra8_mpu_mram_base | k_ra8_rbar_attr_ro_x),
                          (k_ra8_mpu_mram_limit | k_ra8_mpu_rlar_enable),
                          k_ra8_mpu_attridx_normal_wbwa);
  internal_mpu_set_region(1U,
                          (k_ra8_mpu_sram_base | k_ra8_rbar_attr_rw_xn),
                          (k_ra8_mpu_sram_limit | k_ra8_mpu_rlar_enable),
                          k_ra8_mpu_attridx_normal_wbwa);
  internal_mpu_set_region(2U,
                          (k_ra8_mpu_sdram_base | k_ra8_rbar_attr_rw_xn),
                          (k_ra8_mpu_sdram_limit | k_ra8_mpu_rlar_enable),
                          k_ra8_mpu_attridx_normal_wbwa);
  internal_mpu_set_region(3U,
                          (k_ra8_mpu_peri_base | k_ra8_rbar_attr_rw_xn),
                          (k_ra8_mpu_peri_limit | k_ra8_mpu_rlar_enable),
                          k_ra8_mpu_attridx_device);
  /* Shared M85<->M33 SRAM bank: Normal NON-cacheable so cross-core hand-offs
   * (mailbox at 0x22100000, CPU1 RAM) stay coherent with the M85 D-cache on. */
  internal_mpu_set_region(4U,
                          (k_ra8_mpu_shram_base | k_ra8_rbar_attr_rw_xn),
                          (k_ra8_mpu_shram_limit | k_ra8_mpu_rlar_enable),
                          k_ra8_mpu_attridx_noncacheable);

  /* Enable MPU with PRIVDEFENA so unclassified privileged accesses
   * still succeed through the default system memory map. */
  ra8_boot_dsb();
  ra8_boot_write32(k_ra8_mpu_ctrl_addr, k_ra8_mpu_ctrl_enable | k_ra8_mpu_ctrl_privdefena);
  ra8_boot_dsb();
  ra8_boot_isb();
}

/* =============================================================================
 * Public entry point
 * =============================================================================
 *
 * Called from `Reset_Handler` before the .data copy and .bss zero.
 * Must therefore not touch any global variables -- everything here
 * runs on the stack and writes to CPU / SCB memory only.
 */

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
  /* T4-06: MPU memory-attribute map FIRST (so the D-cache sees Device-nGnRE
   * MMIO and the non-cacheable shared SRAM, not Normal cacheable), then the
   * caches. Each *_enable does its own architectural invalidate before setting
   * CCR. Region 4 (non-cacheable shared SRAM) keeps M85<->M33 hand-offs
   * coherent with no software maintenance. */
  internal_mpu_init();
#ifdef RA8_BOOT_CACHE_VIA_HAL
  /* Issue #577: bring the L1 caches up through the ra8_cache HAL instead of the
   * hand-rolled internal_enable_icache / internal_enable_dcache pokes. The HAL
   * primitives encode the identical ICIALLU + CCR.IC / CCR.DC sequence (each
   * runs its architectural invalidate before setting the enable bit), so this is
   * a drop-in with no behaviour change. The raw helpers stay compiled just above
   * as the reference path; exactly the app that opts in takes the HAL route. */
  ra8_cache_icache_enable();
  ra8_cache_dcache_enable();
  (void)internal_enable_icache;
  (void)internal_enable_dcache;
#else
  internal_enable_icache();
  internal_enable_dcache();
#endif
  internal_enable_branch_predictor();
#else
  /* Default OFF: caches + MPU stay disabled -- no behaviour change. */
  (void)internal_enable_icache;
  (void)internal_enable_dcache;
  (void)internal_enable_branch_predictor;
  (void)internal_mpu_init;
#endif
  (void)ra8_trustzone_init; /* TrustZone has its own gate (RA8_TRUSTZONE_ENABLE). */
  internal_set_priority_grouping();
}
