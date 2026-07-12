/**
 * @file examples/ek_ra8d2/hw_pending/secure_boot_ns_hil/ns_main.c
 * @brief Non-Secure image: liveness beacon proving the BLXNS RoT gate passed (#172).
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * The Non-Secure half of the ``secure_boot_ns_hil`` two-project TrustZone build.
 * The Secure side copies this image into the SRAM Non-secure alias (0x3210_0000),
 * programmes the SAU, and -- only if ``ra8_rot_verify_image`` authenticates the
 * image's ECDSA-P256 signature -- BLXNS-es to ``ns_reset_handler`` here.
 *
 * This image is deliberately minimal: it makes NO NS->Secure calls (no CMSE
 * import library, no veneers). After zeroing ``.ns_bss`` it advances a single
 * liveness counter, ::g_sbns_ns_alive, forever. A J-Link memprobe that samples
 * the counter twice and sees it ADVANCE proves the whole chain worked:
 *   - the Secure RoT gate authenticated a genuine signed NS image,
 *   - VTOR_NS was armed and BLXNS switched CPU0 to Non-Secure state,
 *   - and the NS reset vector ran.
 *
 * A TAMPERED NS image never reaches this code: the Secure gate default-denies,
 * ``ra8_tz_secure_boot_jump_ns`` returns an error, and the counter stays 0.
 *
 * ## Memory layout
 *
 * | Symbol                    | Section         | Address (VMA)    |
 * |---------------------------|-----------------|------------------|
 * | ``g_ra8_ns_vector_table``  | ``.ns_vectors`` | 0x32100000       |
 * | ``.ns_rot_header``        | ``.ns_rot_header`` | 0x32100040    |
 * | ``ns_reset_handler``      | ``.ns_text``    | 0x32100048+      |
 * | ``g_sbns_ns_alive``       | ``.ns_bss``     | 0x321000xx       |
 *
 * (Run-time VMA in the SRAM NS alias; the image is flashed in Secure MRAM at
 * the LMA 0x02080000 and copied here by the Secure ``trustzone_init``.)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

/* =============================================================================
 * NS-resident state (all .ns_bss -> NS_SRAM alias, J-Link readable)
 * =============================================================================
 */

/**
 * @var g_sbns_ns_alive
 * @brief Liveness counter: advances forever once the NS reset handler runs.
 *
 * @details
 * Zeroed by ``ns_reset_handler`` on entry, then incremented in a tight loop.
 * The HIL gate probes this symbol: a genuine signed NS image makes it advance;
 * a tampered image is rejected before BLXNS so it stays 0. Lives in ``.ns_bss``
 * (SRAM NS alias 0x32100000+), squarely inside the SAU NS region.
 *
 * @note Read externally by J-Link only; firmware never reads it back.
 * @warning Do not modify from outside the NS reset handler.
 * @since 0.1.0
 */
[[gnu::section(".ns_bss")]] volatile uint32_t g_sbns_ns_alive;

/* =============================================================================
 * NS reset handler + vector table
 * =============================================================================
 */

extern uint32_t g_ra8_ls_ns_stack_top; /**< Linker symbol: top of NS stack.  */
extern uint32_t g_ra8_ls_ns_bss_start; /**< Linker symbol: start of .ns_bss. */
extern uint32_t g_ra8_ls_ns_bss_end;   /**< Linker symbol: end of .ns_bss.   */

/**
 * @typedef ns_exc_handler_t
 * @brief Function-pointer type for entries in the NS vector table.
 */
typedef void (*ns_exc_handler_t)(void);

/**
 * @brief NMI / fault halt vector for the NS table.
 *
 * @details Any unmasked NS fault lands here and parks, so a J-Link halt plus a
 *          ::g_sbns_ns_alive read shows whether the NS code was making progress
 *          when the fault hit.
 *
 * @return Never returns.
 * @pre Reached from an NS exception vector.
 * @pre CPU is in NS thread or handler mode.
 * @post The core spins in WFI.
 * @post ::g_sbns_ns_alive stops advancing.
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
 * @brief NS-world reset handler -- entered via BLXNS from the Secure side.
 *
 * @details
 * Zeros ``.ns_bss`` via ``uintptr_t`` arithmetic (cppcheck flags a pointer
 * comparison between two distinct externs as ISO C UB even though the linker
 * fixes them to a contiguous range), then advances ::g_sbns_ns_alive forever so
 * a J-Link memprobe observes NS liveness.
 *
 * @return Never returns.
 * @pre BLXNS from the Secure side landed here with ``MSP_NS`` =
 *      ``g_ra8_ls_ns_stack_top`` (the Secure RoT gate must have passed first).
 * @pre The SAU exposes the NS SRAM alias to this code.
 * @post ``.ns_bss`` is zeroed.
 * @post ::g_sbns_ns_alive advances continually.
 * @note Single-threaded; IRQs stay masked (no drivers, no RTOS).
 * @since 0.1.0
 */
[[gnu::section(".ns_text"), noreturn]] static void ns_reset_handler(void)
{
  const uintptr_t bss_start = (uintptr_t)&g_ra8_ls_ns_bss_start;
  const uintptr_t bss_end   = (uintptr_t)&g_ra8_ls_ns_bss_end;
  for (uintptr_t addr = bss_start; addr < bss_end; addr += sizeof(uint32_t)) {
    *(volatile uint32_t*)addr = 0U;
  }

  /* Liveness beacon: a genuine, RoT-authenticated NS image reaches this loop;
   * a tampered image is default-denied before BLXNS and never gets here. */
  while (1) {
    g_sbns_ns_alive += 1U;
  }
}

/**
 * @var g_ra8_ns_vector_table
 * @brief Non-Secure vector table; run-time VMA ``NS_SRAM_RUN`` (0x32100000).
 *
 * @details
 * Slot 0 = initial ``MSP_NS``, slot 1 = ``ns_reset_handler``. Every fault slot
 * halts in ::ns_nmi_halt. MUST stay exactly 16 entries (64 bytes): the
 * ``.ns_rot_header`` the Secure verifier reads is placed immediately after it
 * (see ``ns_image.ld`` + ::k_ra8_tz_ns_rot_header_offset). 8-byte aligned per
 * ARMv8-M B3.10 (``.ns_vectors`` aligns to 8).
 *
 * @invariant Exactly 16 entries so the RoT header lands at ns_base + 0x40.
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
  ns_nmi_halt,                              /* 11 SVCall -- unused.              */
  ns_nmi_halt,                              /* 12 DebugMonitor -- halt.          */
  0,                                        /* 13 Reserved.                      */
  ns_nmi_halt,                              /* 14 PendSV -- unused.              */
  ns_nmi_halt,                              /* 15 SysTick -- unused.             */
};
