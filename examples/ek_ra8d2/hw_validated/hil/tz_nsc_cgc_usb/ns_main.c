/**
 * @file examples/ek_ra8d2/hw_validated/hil/tz_nsc_cgc_usb/ns_main.c
 * @brief Non-Secure image: prove the NSC CGC veneers return OK from NS (#60).
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * After the S-side ``trustzone_init`` copies this image into the SRAM
 * Non-secure alias (0x3210_0000) and programmes the SAU, it BLXNS-es to
 * ``ns_reset_handler`` here. Once CPU0 is in NS state, the SAU's NS region
 * (0x3000_0000-0x3FFF_FFFF) and the NSC veneer alias are reachable; any
 * access to S-only memory faults.
 *
 * Phase A+B proved the BLXNS landed (``ns_alive`` advances). This image adds the
 * Phase C veneer milestone: from genuine NS memory it calls the three NSC CGC
 * veneers --
 *
 *   - ``ra8_nsc_cgc_pll2_enable``
 *   - ``ra8_nsc_cgc_usbfs_clock_enable``
 *   - ``ra8_nsc_cgc_get_clock_hz``
 *
 * -- which SG-trap into the Secure world, run the real ``ra8_cgc_*`` driver, and
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
 * | Symbol                           | Section         | Address (VMA)    |
 * |----------------------------------|-----------------|------------------|
 * | ``g_ra8_ns_vector_table``         | ``.ns_vectors`` | 0x32100000       |
 * | ``ns_reset_handler``             | ``.ns_text``    | 0x32100000+      |
 * | NS counters / step              | ``.ns_bss``     | 0x32100000+      |
 *
 * (Run-time VMA in the SRAM NS alias; the image is flashed in Secure MRAM
 * at the LMA 0x02080000 and copied here by the secure ``trustzone_init``.)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_cgc.h"
#include "ra8_err.h"
#include "tx_api.h" /* Non-Secure ThreadX (threadx_ns, TX_SINGLE_MODE_NON_SECURE) -- #96 */

/* =============================================================================
 * NS-side import view of the NSC CGC veneers
 * =============================================================================
 *
 * This is the Non-Secure half of a two-project TrustZone build (#96), so the
 * Secure side's SG veneers are supplied to this link via the CMSE import
 * library (gcc --out-implib on the Secure link; the import object is on this
 * image's link line). The import library binds the bare ``ra8_nsc_cgc_*`` names
 * to the Secure-Gateway stub addresses, so the NS code calls them normally and
 * the call transitions into Secure through the SG -- no by-address hack, no
 * cmse_nonsecure_entry attribute on this side. Signatures MUST match
 * ra8_nsc_cgc.h exactly.
 */
ra8_err_t ra8_nsc_cgc_pll2_enable(uint8_t mul_int, uint8_t mul_quarters, ra8_plodiv_t p_div_code);
ra8_err_t ra8_nsc_cgc_usbfs_clock_enable(void);
ra8_err_t ra8_nsc_cgc_get_clock_hz(ra8_clock_id_t id, uint32_t* hz_out);

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
[[gnu::section(".ns_bss")]] volatile uint32_t g_tz_nsc_cgc_usb_ns_alive;

/**
 * @var g_tz_nsc_cgc_usb_init_step
 * @brief Boot-step breadcrumb: which NSC veneer the NS code last reached.
 * @details 0 = before any veneer; 1 = entering pll2_enable; 2 = entering
 *          usbfs_clock_enable; 3 = entering get_clock_hz; 4 = all three OK.
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
[[gnu::section(".ns_bss")]] volatile uint32_t g_tz_nsc_cgc_usb_init_step;

/**
 * @var g_tz_nsc_cgc_usb_match
 * @brief Success counter: advances forever once all three veneers returned OK.
 * @note Read externally by J-Link only; the HIL gate probes this symbol.
 * @since 0.1.0
 */
[[gnu::section(".ns_bss")]] volatile uint32_t g_tz_nsc_cgc_usb_match;

/**
 * @var g_tz_nsc_cgc_usb_mismatch
 * @brief Failure counter: a veneer returned non-OK (vs faulting outright).
 * @note Read externally by J-Link only; the HIL gate fails if this is non-zero.
 * @since 0.1.0
 */
[[gnu::section(".ns_bss")]] volatile uint32_t g_tz_nsc_cgc_usb_mismatch;

/**
 * @var g_tz_nsc_cgc_usb_clock_hz
 * @brief NS-resident output slot for ::ra8_nsc_cgc_get_clock_hz.
 * @details Lives in .ns_bss (SRAM NS alias 0x32100000+), squarely inside
 *          the SAU NS region (0x30000000-0x3FFFFFFF), so the veneer's
 *          NS-pointer check passes.
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
[[gnu::section(".ns_bss")]] volatile uint32_t g_tz_nsc_cgc_usb_clock_hz;

/**
 * @var g_tz_nsc_cgc_usb_sp_probe
 * @brief Diagnostic: address of an NS stack local (to confirm MSP_NS placement).
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
[[gnu::section(".ns_bss")]] volatile uint32_t g_tz_nsc_cgc_usb_sp_probe;

/* =============================================================================
 * Veneer-milestone constants
 * =============================================================================
 */

/** @brief NSC veneer-call tunables + step breadcrumbs. */
typedef enum : uint8_t {
  k_ns_pll2_mul_int      = 80U, /**< PLL2 multiplier integer part.            */
  k_ns_pll2_mul_quarters = 0U,  /**< PLL2 multiplier quarter part.            */
  k_ns_step_start        = 0U,  /**< Before any veneer.                       */
  k_ns_step_pll2         = 1U,  /**< Entering ra8_nsc_cgc_pll2_enable.        */
  k_ns_step_usbfs        = 2U,  /**< Entering ra8_nsc_cgc_usbfs_clock_enable. */
  k_ns_step_query        = 3U,  /**< Entering ra8_nsc_cgc_get_clock_hz.       */
  k_ns_step_veneers_ok   = 4U,  /**< All three veneers returned OK.           */
} ns_step_t;

/* =============================================================================
 * NS Reset handler
 * =============================================================================
 */

extern uint32_t g_ra8_ls_ns_stack_top; /**< Linker symbol: top of NS stack.      */
extern uint32_t g_ra8_ls_ns_bss_start; /**< Linker symbol: start of .ns_bss.     */
extern uint32_t g_ra8_ls_ns_bss_end;   /**< Linker symbol: end of .ns_bss.       */
extern uint32_t g_ra8_ls_ns_run_start; /**< Linker symbol: NS vector table base. */

/** @brief NS-state VTOR (0xE000ED08 is the current-domain alias in NS). */
typedef enum : uintptr_t {
  k_ns_scb_vtor_addr = 0xE000ED08U, /**< Ns scb vtor address. */
} ns_scb_addr_t;

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
[[gnu::section(".ns_text"), noreturn]] static void ns_park(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Exercise the three NSC CGC veneers from genuine NS memory.
 *
 * @details Stamps ::g_tz_nsc_cgc_usb_init_step before each veneer (so a J-Link
 *          halt localises a fault/error), calls ``ra8_nsc_cgc_pll2_enable``,
 *          ``ra8_nsc_cgc_usbfs_clock_enable`` and ``ra8_nsc_cgc_get_clock_hz``
 *          (the last with an NS-resident ``hz_out`` so its
 *          ``cmse_check_address_range`` guard passes), and records an NS stack
 *          address in ::g_tz_nsc_cgc_usb_sp_probe. On any non-OK return it
 *          bumps ::g_tz_nsc_cgc_usb_mismatch and reports failure.
 *
 * @return bool true if all three veneers returned ``k_ra8_ok``; false otherwise.
 * @pre CPU is in NS thread mode (post-BLXNS).
 * @pre The CMSE import library bound the ``ra8_nsc_cgc_*`` names to the SG stubs.
 * @post On success ::g_tz_nsc_cgc_usb_init_step == ::k_ns_step_veneers_ok.
 * @post On failure ::g_tz_nsc_cgc_usb_mismatch advanced by 1.
 * @note Single-threaded; IRQs stay masked.
 * @since 0.1.0
 */
[[gnu::section(".ns_text")]] static bool ns_exercise_veneers(void)
{
  /* ---- Phase C veneer milestone: call the 3 NSC CGC veneers from NS. ----
   * Plain calls; the CMSE import library binds these names to the Secure-
   * Gateway stubs so the call transitions into Secure (see the import note). */
  g_tz_nsc_cgc_usb_init_step = (uint32_t)k_ns_step_pll2;
  if (ra8_nsc_cgc_pll2_enable((uint8_t)k_ns_pll2_mul_int,
                              (uint8_t)k_ns_pll2_mul_quarters,
                              k_ra8_plodiv_div4) != k_ra8_ok) {
    g_tz_nsc_cgc_usb_mismatch += 1U;
    return false;
  }

  g_tz_nsc_cgc_usb_init_step = (uint32_t)k_ns_step_usbfs;
  if (ra8_nsc_cgc_usbfs_clock_enable() != k_ra8_ok) {
    g_tz_nsc_cgc_usb_mismatch += 1U;
    return false;
  }

  /* Capture an NS stack address so a J-Link read confirms where MSP_NS sits
   * relative to the SRAM NS alias (MSP_NS top = 0x32180000). A genuine NS
   * stack reads back in 0x321xxxxx; the old broken build showed a Secure
   * 0x220xxxxx stack here. */
  uint32_t sp_local          = 0U;
  g_tz_nsc_cgc_usb_sp_probe  = (uint32_t)(uintptr_t)&sp_local;
  g_tz_nsc_cgc_usb_init_step = (uint32_t)k_ns_step_query;
  /* hz_out points at an NS-resident global (in .ns_bss, inside the SAU NS
   * region) rather than a stack local near MSP_NS, so the veneer's
   * cmse_check_address_range guard passes. */
  if (ra8_nsc_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0,
                               (uint32_t*)(uintptr_t)&g_tz_nsc_cgc_usb_clock_hz) != k_ra8_ok) {
    g_tz_nsc_cgc_usb_mismatch += 1U;
    return false;
  }

  /* All three veneers SG-trapped into Secure, ran ra8_cgc_*, and returned OK
   * with an NS-resident pointer arg. The NSC wall works from real NS memory. */
  g_tz_nsc_cgc_usb_init_step = (uint32_t)k_ns_step_veneers_ok;
  return true;
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
 * @pre BLXNS from S side landed here with ``MSP_NS`` = ``g_ra8_ls_ns_stack_top``.
 * @pre The SAU exposes the NSC veneer alias + NS MRAM/SRAM to this code.
 * @post ``.ns_bss`` is zeroed.
 * @post On success ::g_tz_nsc_cgc_usb_match advances continually.
 *
 * @note Single-threaded; IRQs stay masked (no ThreadX/USBX yet).
 * @since 0.1.0
 */
[[gnu::section(".ns_text"), noreturn]] static void ns_reset_handler(void)
{
  /* Zero the NS BSS via uintptr_t arithmetic (cppcheck flags pointer
   * comparison between two distinct externs as ISO C UB even though the
   * linker fixes them to a contiguous range). */
  const uintptr_t bss_start = (uintptr_t)&g_ra8_ls_ns_bss_start;
  const uintptr_t bss_end   = (uintptr_t)&g_ra8_ls_ns_bss_end;
  for (uintptr_t addr = bss_start; addr < bss_end; addr += sizeof(uint32_t)) {
    *(volatile uint32_t*)addr = 0U;
  }

  /* Phase C: exercise the three NSC CGC veneers from genuine NS memory. */
  if (!ns_exercise_veneers()) {
    ns_park();
  }

  /* Point VTOR_NS at the NS vector table so ThreadX's PendSV / SysTick
   * exceptions vector to the NS handlers. ThreadX's tx_initialize_low_level
   * deliberately leaves VTOR alone (it expects a SystemInit, which the NS
   * image has no equivalent of), so set it here in NS state -- 0xE000ED08 is
   * the current-domain (NS) VTOR alias. */
  *(volatile uint32_t*)k_ns_scb_vtor_addr = (uint32_t)(uintptr_t)&g_ra8_ls_ns_run_start;

  /* Phase C (#96) milestone 1: hand off to ThreadX, running entirely inside
   * the NS image. tx_kernel_enter() initialises the kernel, calls
   * tx_application_define() (which spawns the worker below), and starts the
   * scheduler -- it never returns. The worker advances g_tz_nsc_cgc_usb_match,
   * so a continuing advance proves the NS-resident RTOS is scheduling. */
  tx_kernel_enter();
  ns_park(); /* Unreachable -- tx_kernel_enter() does not return. */
}

/* =============================================================================
 * ThreadX worker + SysTick (NS-resident RTOS) -- #96 milestone 1
 * =============================================================================
 */

/** @brief ThreadX context-switch handler (in libthreadx_ns.a NS text). */
extern void PendSV_Handler(void);
extern void _tx_timer_interrupt(void); /**< @brief ThreadX 1 ms tick worker. */
/** @brief Set by the kernel once tx_initialize_low_level has run. */
extern volatile uint32_t g_ra8_threadx_systick_ready;

/**
 * @brief NS SysTick handler -- drives the ThreadX 1 ms time base.
 * @details Slot 15 of the NS vector table. Forwards to _tx_timer_interrupt once
 *          ::g_ra8_threadx_systick_ready is set (the kernel sets it after
 *          tx_initialize_low_level), so an early tick cannot enter the kernel.
 * @return void.
 * @pre Entered from the NS SysTick exception.
 * @pre The NS image is running (post-BLXNS).
 * @post One ThreadX tick is processed once the kernel is ready.
 * @post No effect before the kernel is ready.
 * @note Runs at SysTick exception priority in NS.
 * @since 0.1.0
 */
[[gnu::section(".ns_text")]] static void ns_systick_handler(void)
{
  if (g_ra8_threadx_systick_ready != 0U) {
    _tx_timer_interrupt();
  }
}

/* The single ThreadX ``tx_application_define`` for this NS image lives in
 * ns_usb.c -- it spawns the USBX CDC worker, which advances
 * ``g_tz_nsc_cgc_usb_match`` (the HIL gate) from inside the polled-dispatch
 * loop. tx_kernel_enter() (called by ns_reset_handler above) invokes it. */

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
[[gnu::section(".ns_text"), noreturn]] static void ns_nmi_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @var g_ra8_ns_vector_table
 * @brief Non-Secure vector table; run-time VMA ``NS_SRAM_RUN`` (0x32100000).
 * @details Slot 0 = initial ``MSP_NS``, slot 1 = ``ns_reset_handler``. Slots
 *          14 (PendSV) and 15 (SysTick) drive the NS-resident ThreadX kernel
 *          (#96); fault slots halt. 8-byte aligned per ARMv8-M B3.10
 *          (``.ns_vectors`` aligns to 8).
 * @since 0.1.0
 */
[[gnu::section(".ns_vectors"), gnu::used]] const ns_exc_handler_t g_ra8_ns_vector_table[16] = {
  (ns_exc_handler_t)&g_ra8_ls_ns_stack_top, /* 0  Initial NS main stack pointer. */
  ns_reset_handler,                         /* 1  NS Reset vector.               */
  ns_nmi_halt,                              /* 2  NMI -- halt.                   */
  ns_nmi_halt,                              /* 3  HardFault -- halt.             */
  ns_nmi_halt,                              /* 4  MemManage -- halt.             */
  ns_nmi_halt,                              /* 5  BusFault -- halt.              */
  ns_nmi_halt,                              /* 6  UsageFault -- halt.            */
  ns_nmi_halt,                              /* 7  SecureFault -- halt.           */
  0,                                        /* 8  Reserved.                      */
  0,                                        /* 9  Reserved.                      */
  0,                                        /* 10 Reserved.                      */
  ns_nmi_halt,                              /* 11 SVCall -- unused (TX single).  */
  ns_nmi_halt,                              /* 12 DebugMonitor -- halt.          */
  0,                                        /* 13 Reserved.                      */
  PendSV_Handler,                           /* 14 PendSV -- ThreadX ctx switch.  */
  ns_systick_handler,                       /* 15 SysTick -- ThreadX tick.       */
};
