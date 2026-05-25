/**
 * @file examples/ek_ra8d2/cpu1_pingpong_ipc/cpu1_main.c
 * @brief CPU1 stub for the IPC ping-pong work-in-progress experiment.
 *
 * @par Tag
 * [Ring 1 / app] {World: NS}
 *
 * @details Built as a separate ELF for CPU1 (Cortex-M33). The current
 *          phase of this app is a security-state probe on CPU0 only;
 *          CPU1 just idles so it can be released without trapping.
 *          A future commit fills the IPC poll loop here once the
 *          IPCSAR write path is unblocked.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

extern uint32_t g_ra_ls_cpu1_stack_top;
extern uint32_t g_ra_ls_cpu1_data_start;
extern uint32_t g_ra_ls_cpu1_data_end;
extern uint32_t g_ra_ls_cpu1_data_load;
extern uint32_t g_ra_ls_cpu1_bss_start;
extern uint32_t g_ra_ls_cpu1_bss_end;

[[noreturn]] void cpu1_reset_handler(void);

/**
 * @brief CPU1 idle loop -- placeholder for the IPC responder.
 *
 * @pre CPU1 has been released by CPU0 via ra_cpu1_release.
 * @pre Vector table installed at CPU1INITVTOR.
 * @post Loops forever; never returns.
 * @post Has no observable side effects.
 *
 * @note Single-threaded entry.
 * @since 0.1.0
 */
[[noreturn]] static void cpu1_main(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief CPU1 reset handler -- copies .data, zeroes .bss, jumps to main.
 *
 * @pre CPU1 was just released by ra_cpu1_release.
 * @pre Linker provided g_ra_ls_cpu1_data_* / bss_* symbols.
 * @post .data + .bss in SRAM_CPU1 are initialised.
 * @post cpu1_main runs and never returns.
 *
 * @note Single-threaded entry; runs at CPU1's reset.
 * @since 0.1.0
 */
[[noreturn]] void cpu1_reset_handler(void)
{
  /* Copy .data from MRAM load address to SRAM_CPU1. */
  uint32_t* src = &g_ra_ls_cpu1_data_load;
  uint32_t* dst = &g_ra_ls_cpu1_data_start;
  while (dst < &g_ra_ls_cpu1_data_end) {
    *dst++ = *src++;
  }
  /* Zero .bss. */
  uint32_t* bss = &g_ra_ls_cpu1_bss_start;
  while (bss < &g_ra_ls_cpu1_bss_end) {
    *bss++ = 0U;
  }
  cpu1_main();
}
