/**
 * @file examples/ek_ra8d2/cpu1_pingpong/cpu1_main.c
 * @brief CPU1 (Cortex-M33) ping-pong responder
 *
 * @par Tag
 * [Ring 1 / app] {World: NS}
 *
 * @details Built as a separate ELF (-mcpu=cortex-m33). Receives 0x1234
 *          on the CPU0 -> CPU1 channel and replies with 0x4321 in a loop.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_err.h"
#include "ra_ipc.h"

extern uint32_t g_ra_ls_cpu1_stack_top;

[[noreturn]] void cpu1_reset_handler(void);

typedef enum : uint32_t {
  k_cpu1_pingpong_magic_ping = 0x1234U,
  k_cpu1_pingpong_magic_pong = 0x4321U,
} cpu1_main_const_t;

typedef enum : uint8_t {
  k_cpu1_pingpong_pair_zero = 0U,
} cpu1_main_pair_t;

/**
 * @brief CPU1 application loop.
 * @details See file header.
 * @pre Vector table installed.
 * @pre IPC channels reachable.
 * @post Loop never exits.
 * @post IPC channels remain initialized.
 * @note Single-threaded entry.
 * @since 0.1.0
 */
[[noreturn]] static void cpu1_main(void)
{
  uint8_t ch_recv = 0U;
  uint8_t ch_send = 0U;
  (void)ra_ipc_channel_for_recv(k_ra_ipc_core_cpu1, (uint8_t)k_cpu1_pingpong_pair_zero, &ch_recv);
  (void)ra_ipc_channel_for_send(k_ra_ipc_core_cpu1, (uint8_t)k_cpu1_pingpong_pair_zero, &ch_send);

  ra_ipc_config_t cfg_recv = {
    .channel      = ch_recv,
    .reset_fifo   = true,
    .clear_status = true,
    .event_mask   = (uint32_t)k_ra_ipc_event_msg_ready,
  };
  ra_ipc_config_t cfg_send = {
    .channel      = ch_send,
    .reset_fifo   = true,
    .clear_status = true,
    .event_mask   = 0U,
  };
  (void)ra_ipc_init(&cfg_recv);
  (void)ra_ipc_init(&cfg_send);

  while (1) {
    uint32_t got = 0U;
    if (ra_ipc_recv_message(ch_recv, &got) != k_ra_ok) {
      continue;
    }
    if (got != (uint32_t)k_cpu1_pingpong_magic_ping) {
      continue;
    }
    (void)ra_ipc_send_message(ch_send, (uint32_t)k_cpu1_pingpong_magic_pong);
  }
}

/**
 * @brief CPU1 reset handler.
 * @details Jumps to cpu1_main.
 * @pre SP initialized by hardware via MSPC1.
 * @pre CPU1 just exited reset.
 * @post Never returns.
 * @post CPU1 enters cpu1_main loop.
 * @note Called only from the CPU1 vector table.
 * @since 0.1.0
 */
[[noreturn]] void cpu1_reset_handler(void)
{
  cpu1_main();
}

/**
 * @brief Default fault handler.
 * @details All M33 exception slots route here.
 * @pre Hardware fault occurred.
 * @pre Caller is the M33 exception entry path.
 * @post CPU1 stops making forward progress.
 * @post Watchdog (if enabled) eventually resets the chip.
 * @note Used as default for all exception slots.
 * @since 0.1.0
 */
[[noreturn]] static void cpu1_fault_handler(void)
{
  while (1) {
    __asm volatile("nop");
  }
}

/**
 * @var g_cpu1_vector_table
 * @brief Minimal Armv8-M vector table for CPU1.
 * @details Initial-SP slot overridden by SYSC.MSPC1 at release.
 * @note Placed in .cpu1_vectors by the linker.
 * @warning Do not modify at runtime.
 * @since 0.1.0
 */
#ifndef RA_SIMULATOR_MODE
/* Vector table only built for the cross-compiled M33 image. The host
 * build does not link this TU as an executable -- it is compile-checked
 * only -- so we can drop the table without losing test coverage. */
__attribute__((used, section(".cpu1_vectors"))) const uintptr_t g_cpu1_vector_table[] = {
  (uintptr_t)&g_ra_ls_cpu1_stack_top,
  (uintptr_t)&cpu1_reset_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
};
#endif
