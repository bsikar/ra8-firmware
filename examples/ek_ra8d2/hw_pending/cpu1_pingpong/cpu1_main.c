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
extern uint32_t g_ra_ls_cpu1_data_start;
extern uint32_t g_ra_ls_cpu1_data_end;
extern uint32_t g_ra_ls_cpu1_data_load;
extern uint32_t g_ra_ls_cpu1_bss_start;
extern uint32_t g_ra_ls_cpu1_bss_end;

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

  /* HUM Ch 3.2.14 "IPC0CLR0.RST" p 217 -- writing 1 to RST drains the
   * receive FIFO and clears STA.RDY/STA.FULL. The IPC peripheral is a
   * single shared block: CPU0 already issued ``reset_fifo=true`` on
   * both directions before releasing CPU1 (see ra_cpu1_release ordering
   * in main.c). If CPU1 also writes RST on ch_recv (= IPC1_0, the
   * CPU0 -> CPU1 FIFO that CPU0 may have already loaded with 0x1234
   * during the race window between CPU1ACTCSR.ACT asserting and CPU1
   * finishing its .data/.bss init), the pending ping gets discarded
   * and CPU0's bounded ``recv_blocking`` poll on ch 0 never sees a
   * response -- ``g_cpu1_pingpong_match`` and ``g_cpu1_pingpong_mismatch``
   * both stayed at zero across the 5 s HIL probe window because the
   * first ping was lost and every subsequent ping arrived while CPU1
   * was already past its init blocking. Skip the reset on CPU1; CPU0
   * is the FIFO owner of both directions. ``clear_status`` is also
   * dropped because CPU0 already cleared status during its own init
   * and the only bits that could be set after CPU0's clear are RDY
   * (set by CPU0's own TXD write -- discarding that is exactly the
   * race we are avoiding). */
  ra_ipc_config_t cfg_recv = {
    .channel      = ch_recv,
    .reset_fifo   = false,
    .clear_status = false,
    .event_mask   = (uint32_t)k_ra_ipc_event_msg_ready,
  };
  ra_ipc_config_t cfg_send = {
    .channel      = ch_send,
    .reset_fifo   = false,
    .clear_status = false,
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
 *
 * @details Runs the minimal C-runtime init the M33 image needs before
 * branching into ``cpu1_main``:
 *   1. Copy ``.data`` from its MRAM_CPU1 load image into SRAM_CPU1.
 *   2. Zero ``.bss`` in SRAM_CPU1.
 *
 * Without these passes the CPU1 image's globals (e.g. the
 * ``s_ipc_channels`` array in ``ra_ipc.c`` -- ``.bss`` -- and any
 * initialised file-scope statics -- ``.data``) hold whatever pattern
 * SRAM_CPU1 contained at boot. That was the reason CPU1 silently
 * stayed wedged after Agent D embedded the CPU1 binary: the M33
 * jumped straight into ``cpu1_main`` with uninitialised globals, so
 * ``ra_ipc_init`` / ``ra_ipc_recv_message`` operated on garbage state
 * structures and the channel pair never came alive.
 *
 * @pre Initial SP loaded by hardware from the first slot of
 *      ``.cpu1_vectors`` (= ``g_ra_ls_cpu1_stack_top``).
 * @pre CPU1 has just exited reset via the ACTREQ handshake driven by
 *      ``ra_cpu1_release`` on CPU0.
 * @post ``.data`` mirrors the MRAM_CPU1 load image.
 * @post ``.bss`` is zero-filled.
 * @post Never returns; ``cpu1_main`` enters its infinite IPC loop.
 *
 * @note Called only from the CPU1 vector table; runs in M33 thread mode.
 * @since 0.1.0
 */
[[noreturn]] void cpu1_reset_handler(void)
{
  /* Copy .data from MRAM_CPU1 load address into SRAM_CPU1. The linker
   * defines ``g_ra_ls_cpu1_data_load`` as LOADADDR(.data) so this
   * works regardless of the absolute MRAM_CPU1 base. */
  uint32_t* dst = &g_ra_ls_cpu1_data_start;
  uint32_t* src = &g_ra_ls_cpu1_data_load;
  while (dst < &g_ra_ls_cpu1_data_end) {
    *dst = *src;
    dst++;
    src++;
  }
  /* Zero .bss in SRAM_CPU1. */
  uint32_t* bss = &g_ra_ls_cpu1_bss_start;
  while (bss < &g_ra_ls_cpu1_bss_end) {
    *bss = 0U;
    bss++;
  }
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
