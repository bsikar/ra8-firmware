/**
 * @file ra_dfu_launch.c
 * @brief Copy-to-run launch: copy an image to the SRAM run base and branch to it.
 *
 * @par Tag
 * [Ring 4 / Service] {World: S}
 *
 * @details
 * The shared copy-to-run hand-off behind the `dfu_bootloader` (which launches a
 * validated slot body) and the `dfu_copy_to_run` HIL demo (which launches an
 * embedded image). It copies `img_len` bytes from `src` to the fixed SRAM run
 * base (::k_ra_dfu_run_base), then points the Secure VTOR there, loads the
 * image's initial MSP, and branches to its reset vector -- a same-world (Secure)
 * branch (the reset vector keeps its Thumb bit). Because every payload is linked
 * at the one run base, the same image runs wherever it was staged.
 *
 * The destination is the trusted ::k_ra_dfu_run_base constant, never a
 * caller-supplied `entry`, so a corrupted `entry` cannot redirect the copy: it
 * only fails the ::ra_dfu_run_target_valid cross-check and the launch returns.
 *
 * The I/D caches are disabled on the EK-RA8D2 (libs/ra_board_ek_ra8d2/boot/
 * system_init.c keeps them off), so the write-then-execute is made coherent by a
 * DSB after the copy plus a DSB/ISB before the branch -- no cache maintenance.
 * If caches are ever enabled there, a clean-DCache + invalidate-ICache must be
 * added before the branch.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_dfu.h"

/**
 * @enum ra_dfu_launch_scb_t
 * @brief Cortex-M85 System Control Block address used by the hand-off.
 * @details VTOR is the Secure alias; the bootloader and apps run Secure. The
 *          Renesas HUM defers the Cortex-M85 core (SCB) registers to the ARMv8-M
 *          ARM (B3.2.2 "VTOR, Vector Table Offset Register").
 */
typedef enum : uintptr_t {
  k_ra_dfu_launch_vtor_addr = 0xE000ED08U, /**< SCB->VTOR (Secure). */
} ra_dfu_launch_scb_t;

void ra_dfu_launch(uintptr_t src, uint32_t img_len, uint32_t entry)
{
  if (src == 0U) {
    return; /* nothing to copy */
  }
  if (!ra_dfu_run_target_valid(entry, img_len)) {
    return; /* entry not the run base, or bad length -> caller drops to fallback */
  }
#ifndef RA_SIMULATOR_MODE
  const volatile uint32_t* s     = (const volatile uint32_t*)src;
  volatile uint32_t*       d     = (volatile uint32_t*)(uintptr_t)k_ra_dfu_run_base;
  const uint32_t           words = img_len / (uint32_t)sizeof(uint32_t);

  __asm__ volatile("cpsid i" ::: "memory");
  /* Bounded by img_len (<= k_ra_dfu_img_max via ra_dfu_run_target_valid): a
   * statically bounded copy -- NASA Rule 2 compliant. */
  for (uint32_t i = 0U; i < words; ++i) {
    d[i] = s[i];
  }
  __asm__ volatile("dsb 0xF" ::: "memory"); /* image stores reach SRAM before fetch */

  const uint32_t initial_sp  = d[0];
  const uint32_t reset_entry = d[1];

  *(volatile uint32_t*)(uintptr_t)k_ra_dfu_launch_vtor_addr = (uint32_t)k_ra_dfu_run_base;
  __asm__ volatile("dsb 0xF\n isb 0xF\n" ::: "memory");
  __asm__ volatile("msr msp, %0\n"
                   "bx %1\n"
                   :
                   : "r"(initial_sp), "r"(reset_entry)
                   : "memory");
  __builtin_unreachable();
#endif /* !RA_SIMULATOR_MODE */
}
