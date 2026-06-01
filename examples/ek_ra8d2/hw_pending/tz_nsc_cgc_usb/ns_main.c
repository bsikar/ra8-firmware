/**
 * @file examples/ek_ra8d2/hw_pending/tz_nsc_cgc_usb/ns_main.c
 * @brief Minimal Non-Secure image at 0x02080000 for the single-core
 *        Secure->NS BLXNS scaffolding (#55 Phase A+B).
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * After the S-side ``trustzone_init`` programmes the SAU it BLXNS-es to
 * ``ns_reset_handler`` here. Once CPU0 is in NS state, the SAU's NS
 * regions (upper MRAM / SRAM / SDRAM + NSC alias) are reachable; any
 * access to S-only memory (the lower halves) faults.
 *
 * The Phase A+B image is intentionally minimal -- it does not call any
 * function whose code lives in S MRAM (no ThreadX, no USBX, no
 * ra_log). It only:
 *
 *   1. Zeros its NS BSS (a single counter).
 *   2. Increments ``g_tz_nsc_cgc_usb_ns_alive`` in a tight loop.
 *   3. Spins in WFI between increments.
 *
 * Bench memprobe of ``g_tz_nsc_cgc_usb_ns_alive`` proves that the
 * S->NS BLXNS landed, the NS code began executing from NS_MRAM, and
 * NS writes to NS_SRAM succeed. With this checkpoint in place,
 * Phase C re-introduces the real ThreadX + USBX app (preserved in
 * ``ns_app_phase_c.c.disabled``) by extending the linker to place the
 * vendored ThreadX / USBX object files into ``NS_MRAM`` and the
 * SAU to NS-attribute the USB-FS peripheral.
 *
 * ## Memory layout
 *
 * | Symbol                           | Section         | Address          |
 * |----------------------------------|-----------------|------------------|
 * | ``g_ra_ns_vector_table``         | ``.ns_vectors`` | 0x02080000       |
 * | ``ns_reset_handler``             | ``.ns_text``    | 0x02080000+      |
 * | ``g_tz_nsc_cgc_usb_ns_alive``    | ``.ns_bss``     | 0x22100000+      |
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

/* =============================================================================
 * NS-resident state
 * =============================================================================
 */

/**
 * @var g_tz_nsc_cgc_usb_ns_alive
 * @brief NS-resident heartbeat counter. Incremented forever once the
 *        BLXNS lands. Read by J-Link to gate the HIL probe.
 *
 * @details
 * Placed in ``.ns_bss`` so the linker pins it into ``NS_SRAM``
 * (0x22100000+) which the SAU marks Non-Secure. The counter starts at
 * zero (BSS-zeroed by ``ns_reset_handler``) and advances at roughly
 * one tick per CPU loop iteration; the HIL probe checks for at least
 * a few hundred ticks across a 3 s window.
 *
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
__attribute__((section(".ns_bss"))) volatile uint32_t g_tz_nsc_cgc_usb_ns_alive;

/* =============================================================================
 * NS Reset handler
 * =============================================================================
 */

extern uint32_t g_ra_ls_ns_stack_top; /**< Linker symbol: top of NS stack.   */
extern uint32_t g_ra_ls_ns_bss_start; /**< Linker symbol: start of .ns_bss.  */
extern uint32_t g_ra_ls_ns_bss_end;   /**< Linker symbol: end of .ns_bss.    */

/**
 * @brief NS-world reset handler -- entered via BLXNS from the S side.
 *
 * @details
 * The S-side ``ra_tz_secure_boot_jump_ns`` writes the address of this
 * function into ``LR`` with bit 0 set to Thumb, programmes the SAU,
 * loads ``MSP_NS`` from slot 0 of ``g_ra_ns_vector_table`` (linker
 * symbol ``g_ra_ls_ns_stack_top``), and issues ``BLXNS``. When this
 * function returns, the CPU is in NS thread mode with an NS-attributed
 * stack and PC.
 *
 * Per the ARMv8-M C runtime contract, BSS must be zeroed and any
 * initialised globals copied from their LMA to VMA before user code
 * runs. The Phase A+B image has only one NS global (``ns_alive``,
 * uninitialised BSS) and no NS .data section, so this handler just
 * zeros ``.ns_bss`` and enters the counter loop.
 *
 * @return Never returns.
 *
 * @pre BLXNS from S side landed here with ``MSP_NS`` set to
 *      ``g_ra_ls_ns_stack_top`` and CPU in NS thread mode.
 * @pre The SAU partition makes ``NS_MRAM`` and ``NS_SRAM`` reachable
 *      from this code.
 * @post ``.ns_bss`` is zeroed.
 * @post ``g_tz_nsc_cgc_usb_ns_alive`` advances continually.
 *
 * @note Single-threaded; no concurrency.
 * @since 0.1.0
 */
__attribute__((section(".ns_text"), noreturn)) static void ns_reset_handler(void)
{
  /* Zero the NS BSS. Reset_Handler on the S side does this for the S
   * regions; the NS region has its own [start, end] linker symbols.
   * Walk via uintptr_t arithmetic rather than raw pointer arithmetic
   * because cppcheck (correctly) flags pointer comparisons between
   * two distinct externs as ISO C UB even though the linker fixes
   * them up to contiguous addresses. */
  const uintptr_t bss_start = (uintptr_t)&g_ra_ls_ns_bss_start;
  const uintptr_t bss_end   = (uintptr_t)&g_ra_ls_ns_bss_end;
  for (uintptr_t addr = bss_start; addr < bss_end; addr += sizeof(uint32_t)) {
    *(volatile uint32_t*)addr = 0U;
  }

  /* Heartbeat loop -- bench probes ``g_tz_nsc_cgc_usb_ns_alive`` to
   * verify NS execution. Runs at CPU speed (~MHz cadence); the SWD
   * probe samples twice across a 3 s window and easily sees hundreds
   * of millions of increments. No ``WFI`` between iterations: with
   * IRQs masked at NS entry, ``WFI`` would sleep until a debug halt
   * and the counter would barely move between two probe samples. */
  while (1) {
    g_tz_nsc_cgc_usb_ns_alive += 1U;
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
 * @var g_ra_ns_vector_table
 * @brief Non-Secure vector table, pinned to ``NS_MRAM`` (0x02080000).
 *
 * @details
 * The S-side ``ra_tz_secure_boot_jump_ns`` reads slot 0 (initial
 * ``MSP_NS``) and BLXNS-es to slot 1 (``ns_reset_handler``). Slots
 * 2..15 are stub exception handlers; the Phase A+B image masks IRQs in
 * its ``WFI`` loop so none of them should ever fire. Slot 2 (NMI)
 * remains a vector since NMIs cannot be masked; we point it at a halt
 * loop to keep the table self-consistent.
 *
 * The table must be 8-byte aligned per ARMv8-M B3.10 (the alignment
 * matches the lower 7 bits clear in ``VTOR_NS``); the ``.ns_vectors``
 * output section in the linker script aligns to 8.
 *
 * @since 0.1.0
 */
__attribute__((section(".ns_text"), noreturn)) static void ns_nmi_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

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
