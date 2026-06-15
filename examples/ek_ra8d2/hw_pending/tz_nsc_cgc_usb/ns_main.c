/**
 * @file examples/ek_ra8d2/hw_pending/tz_nsc_cgc_usb/ns_main.c
 * @brief Non-Secure image: prove the NSC CGC veneers return OK from NS (#60).
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * After the S-side ``trustzone_init`` programmes the SAU it BLXNS-es to
 * ``ns_reset_handler`` here. Once CPU0 is in NS state, the SAU's NS regions
 * (upper MRAM / SRAM / SDRAM + the NSC veneer alias) are reachable; any access
 * to S-only memory faults.
 *
 * Phase A+B proved the BLXNS landed (``ns_alive`` advances). This image adds the
 * Phase C veneer milestone: from genuine NS memory it calls the three NSC CGC
 * veneers --
 *
 *   - ``ra_nsc_cgc_pll2_enable``
 *   - ``ra_nsc_cgc_usbfs_clock_enable``
 *   - ``ra_nsc_cgc_get_clock_hz``
 *
 * -- which SG-trap into the Secure world, run the real ``ra_cgc_*`` driver, and
 * return. This is the app's whole point and the original blocker: the
 * ``cmse_check_address_range`` pointer guard inside the ``get_clock_hz`` veneer
 * used to REJECT the ``&hz`` argument because it pointed into Secure SRAM (no
 * real NS partition existed). With a genuine NS stack the pointer is NS-resident
 * and the guard passes.
 *
 * ``g_tz_nsc_cgc_usb_init_step`` is stamped before each veneer so a J-Link halt
 * pinpoints which one (if any) faults or returns non-OK. On success the worker
 * advances ``g_tz_nsc_cgc_usb_match`` forever (the HIL gate symbol).
 *
 * ## Memory layout
 *
 * | Symbol                           | Section         | Address          |
 * |----------------------------------|-----------------|------------------|
 * | ``g_ra_ns_vector_table``         | ``.ns_vectors`` | 0x02080000       |
 * | ``ns_reset_handler``             | ``.ns_text``    | 0x02080000+      |
 * | NS counters / step              | ``.ns_bss``     | 0x22100000+      |
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_nsc_cgc.h"

/* =============================================================================
 * NS-resident state (all .ns_bss -> NS_SRAM, J-Link readable)
 * =============================================================================
 */

/**
 * @var g_tz_nsc_cgc_usb_ns_alive
 * @brief BLXNS-landed heartbeat (Phase A+B). Advances once NS code runs.
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
__attribute__((section(".ns_bss"))) volatile uint32_t g_tz_nsc_cgc_usb_ns_alive;

/**
 * @var g_tz_nsc_cgc_usb_init_step
 * @brief Boot-step breadcrumb: which NSC veneer the NS code last reached.
 * @details 0 = before any veneer; 1 = entering pll2_enable; 2 = entering
 *          usbfs_clock_enable; 3 = entering get_clock_hz; 4 = all three OK.
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
__attribute__((section(".ns_bss"))) volatile uint32_t g_tz_nsc_cgc_usb_init_step;

/**
 * @var g_tz_nsc_cgc_usb_match
 * @brief Success counter: advances forever once all three veneers returned OK.
 * @note Read externally by J-Link only; the HIL gate probes this symbol.
 * @since 0.1.0
 */
__attribute__((section(".ns_bss"))) volatile uint32_t g_tz_nsc_cgc_usb_match;

/**
 * @var g_tz_nsc_cgc_usb_mismatch
 * @brief Failure counter: a veneer returned non-OK (vs faulting outright).
 * @note Read externally by J-Link only; the HIL gate fails if this is non-zero.
 * @since 0.1.0
 */
__attribute__((section(".ns_bss"))) volatile uint32_t g_tz_nsc_cgc_usb_mismatch;

/**
 * @var g_tz_nsc_cgc_usb_clock_hz
 * @brief NS-resident output slot for ::ra_nsc_cgc_get_clock_hz.
 * @details Lives in .ns_bss (NS_SRAM 0x22100000), squarely inside the SAU
 *          NS-SRAM region, so the veneer's NS-pointer check passes (a stack
 *          local near MSP_NS lands above the region limit).
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
__attribute__((section(".ns_bss"))) volatile uint32_t g_tz_nsc_cgc_usb_clock_hz;

/**
 * @var g_tz_nsc_cgc_usb_sp_probe
 * @brief Diagnostic: address of an NS stack local (to confirm MSP_NS placement).
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
__attribute__((section(".ns_bss"))) volatile uint32_t g_tz_nsc_cgc_usb_sp_probe;

/* =============================================================================
 * Veneer-milestone constants
 * =============================================================================
 */

/** @brief NSC veneer-call tunables + step breadcrumbs. */
typedef enum : uint8_t {
  k_ns_pll2_mul_int      = 80U, /**< PLL2 multiplier integer part.       */
  k_ns_pll2_mul_quarters = 0U,  /**< PLL2 multiplier quarter part.       */
  k_ns_step_start        = 0U,  /**< Before any veneer.                  */
  k_ns_step_pll2         = 1U,  /**< Entering ra_nsc_cgc_pll2_enable.    */
  k_ns_step_usbfs        = 2U,  /**< Entering ra_nsc_cgc_usbfs_clock_enable. */
  k_ns_step_query        = 3U,  /**< Entering ra_nsc_cgc_get_clock_hz.   */
  k_ns_step_veneers_ok   = 4U,  /**< All three veneers returned OK.      */
} ns_step_t;

/* =============================================================================
 * NS Reset handler
 * =============================================================================
 */

extern uint32_t g_ra_ls_ns_stack_top; /**< Linker symbol: top of NS stack.   */
extern uint32_t g_ra_ls_ns_bss_start; /**< Linker symbol: start of .ns_bss.  */
extern uint32_t g_ra_ls_ns_bss_end;   /**< Linker symbol: end of .ns_bss.    */

/**
 * @brief Park the NS core in WFI forever (a veneer faulted or returned non-OK).
 * @details Leaves ::g_tz_nsc_cgc_usb_init_step / ::g_tz_nsc_cgc_usb_mismatch
 *          frozen so a J-Link halt shows exactly where the NSC path broke.
 * @return Never returns.
 * @pre Reached from a failing veneer step.
 * @pre CPU is in NS thread mode.
 * @post The core spins in WFI; the bench counters stop advancing.
 * @post No further veneer calls are issued.
 * @note Single-threaded.
 * @since 0.1.0
 */
__attribute__((section(".ns_text"), noreturn)) static void ns_park(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief NS-world reset handler -- entered via BLXNS from the S side.
 *
 * @details
 * Zeros ``.ns_bss``, then exercises the three NSC CGC veneers from NS memory
 * (stamping ::g_tz_nsc_cgc_usb_init_step before each so a fault/error is
 * localised). On full success it advances ::g_tz_nsc_cgc_usb_match (and the
 * legacy ::g_tz_nsc_cgc_usb_ns_alive) forever; any veneer error parks the core.
 *
 * @return Never returns.
 *
 * @pre BLXNS from S side landed here with ``MSP_NS`` = ``g_ra_ls_ns_stack_top``.
 * @pre The SAU exposes the NSC veneer alias + NS MRAM/SRAM to this code.
 * @post ``.ns_bss`` is zeroed.
 * @post On success ::g_tz_nsc_cgc_usb_match advances continually.
 *
 * @note Single-threaded; IRQs stay masked (no ThreadX/USBX yet).
 * @since 0.1.0
 */
__attribute__((section(".ns_text"), noreturn)) static void ns_reset_handler(void)
{
  /* Zero the NS BSS via uintptr_t arithmetic (cppcheck flags pointer
   * comparison between two distinct externs as ISO C UB even though the
   * linker fixes them to a contiguous range). */
  const uintptr_t bss_start = (uintptr_t)&g_ra_ls_ns_bss_start;
  const uintptr_t bss_end   = (uintptr_t)&g_ra_ls_ns_bss_end;
  for (uintptr_t addr = bss_start; addr < bss_end; addr += sizeof(uint32_t)) {
    *(volatile uint32_t*)addr = 0U;
  }

  /* ---- Phase C veneer milestone: call the 3 NSC CGC veneers from NS. ---- */
  g_tz_nsc_cgc_usb_init_step = (uint32_t)k_ns_step_pll2;
  if (ra_nsc_cgc_pll2_enable((uint8_t)k_ns_pll2_mul_int,
                             (uint8_t)k_ns_pll2_mul_quarters,
                             k_ra_plodiv_div4) != k_ra_ok) {
    g_tz_nsc_cgc_usb_mismatch += 1U;
    ns_park();
  }

  g_tz_nsc_cgc_usb_init_step = (uint32_t)k_ns_step_usbfs;
  if (ra_nsc_cgc_usbfs_clock_enable() != k_ra_ok) {
    g_tz_nsc_cgc_usb_mismatch += 1U;
    ns_park();
  }

  /* Capture an NS stack address so a J-Link read confirms where MSP_NS sits
   * relative to the SAU NS-SRAM region (0x22100000..0x221FFFE0). */
  uint32_t sp_local          = 0U;
  g_tz_nsc_cgc_usb_sp_probe  = (uint32_t)(uintptr_t)&sp_local;
  g_tz_nsc_cgc_usb_init_step = (uint32_t)k_ns_step_query;
  /* hz_out points at an NS-resident global (in .ns_bss, inside the SAU NS
   * region) rather than a stack local near MSP_NS, so the veneer's
   * cmse_check_address_range guard passes. */
  if (ra_nsc_cgc_get_clock_hz(k_ra_clock_id_cpuclk0,
                              (uint32_t*)(uintptr_t)&g_tz_nsc_cgc_usb_clock_hz) != k_ra_ok) {
    g_tz_nsc_cgc_usb_mismatch += 1U;
    ns_park();
  }

  /* All three veneers SG-trapped into Secure, ran ra_cgc_*, and returned OK
   * with an NS-resident pointer arg. The NSC wall works from real NS memory. */
  g_tz_nsc_cgc_usb_init_step = (uint32_t)k_ns_step_veneers_ok;
  while (1) {
    g_tz_nsc_cgc_usb_ns_alive += 1U;
    g_tz_nsc_cgc_usb_match += 1U;
  }
}

/* =============================================================================
 * NS vector table
 * =============================================================================
 */

/**
 * @typedef ns_exc_handler_t
 * @brief Function-pointer type for entries in the NS vector table.
 */
typedef void (*ns_exc_handler_t)(void);

/**
 * @brief NMI / fault halt vector for the NS table.
 * @details Any unmasked NS fault (incl. a SecureFault escalated to NS) lands
 *          here and parks, so a J-Link halt + ::g_tz_nsc_cgc_usb_init_step read
 *          shows which veneer step faulted.
 * @return Never returns.
 * @pre Reached from an NS exception vector.
 * @pre IRQs are otherwise masked in the main path.
 * @post The core spins in WFI.
 * @post Bench counters reflect the last step reached.
 * @note Shared by every non-reset NS vector slot.
 * @since 0.1.0
 */
__attribute__((section(".ns_text"), noreturn)) static void ns_nmi_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @var g_ra_ns_vector_table
 * @brief Non-Secure vector table, pinned to ``NS_MRAM`` (0x02080000).
 * @details Slot 0 = initial ``MSP_NS``, slot 1 = ``ns_reset_handler``. The
 *          veneer milestone keeps IRQs masked, so the fault/SVCall/PendSV/
 *          SysTick slots stay halt vectors (Phase C-full repoints 11/14/15 at
 *          ThreadX once the RTOS comes up in NS). 8-byte aligned per ARMv8-M
 *          B3.10 (``.ns_vectors`` aligns to 8).
 * @since 0.1.0
 */
__attribute__((section(".ns_vectors"), used)) const ns_exc_handler_t g_ra_ns_vector_table[16] = {
  (ns_exc_handler_t)&g_ra_ls_ns_stack_top, /* 0  Initial NS main stack pointer. */
  ns_reset_handler,                        /* 1  NS Reset vector.               */
  ns_nmi_halt,                             /* 2  NMI -- halt.                   */
  ns_nmi_halt,                             /* 3  HardFault -- halt.             */
  ns_nmi_halt,                             /* 4  MemManage -- halt.             */
  ns_nmi_halt,                             /* 5  BusFault -- halt.              */
  ns_nmi_halt,                             /* 6  UsageFault -- halt.            */
  ns_nmi_halt,                             /* 7  SecureFault -- halt.           */
  0,                                       /* 8  Reserved.                      */
  0,                                       /* 9  Reserved.                      */
  0,                                       /* 10 Reserved.                      */
  ns_nmi_halt,                             /* 11 SVCall -- halt.                */
  ns_nmi_halt,                             /* 12 DebugMonitor -- halt.          */
  0,                                       /* 13 Reserved.                      */
  ns_nmi_halt,                             /* 14 PendSV -- halt.                */
  ns_nmi_halt,                             /* 15 SysTick -- halt.               */
};
