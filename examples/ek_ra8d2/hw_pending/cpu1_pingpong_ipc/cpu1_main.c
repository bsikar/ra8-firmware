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
/* Self-contained IPC for CPU1 -- avoid the M85 HAL whose accessors */ /* LEGACY-OK: ARMv8-M bus-architecture term */
/* the M33 cannot all reach through its NS-controller view. Channel windows
 * via the NS alias (bit 28 set) so the IPCSAR-attributed channels are
 * reachable. HUM Ch 3.2 p 205. */
typedef enum : uintptr_t {
  k_cpu1_ipc_ch0_addr = 0x500200C0UL, /**< CPU1 TX to CPU0 (NS alias).   */
  k_cpu1_ipc_ch2_addr = 0x50020100UL, /**< CPU1 RX from CPU0 (NS alias). */
} cpu1_ipc_addr_t;

typedef enum : uint8_t {
  k_cpu1_ipc_off_sta = 0x00U,
  k_cpu1_ipc_off_txd = 0x08U,
  k_cpu1_ipc_off_rxd = 0x0CU,
  k_cpu1_ipc_off_clr = 0x10U,
} cpu1_ipc_off_t;

typedef enum : uint32_t {
  k_cpu1_ipc_sta_rdy = 0x00010000UL,
  k_cpu1_ipc_clr_all = 0x030100FFUL,
} cpu1_ipc_mask_t;

[[noreturn]] static void cpu1_main(void)
{
  *(volatile uint32_t*)0x3210020CUL = 0x11111111UL; /* cpu1_main entry */

  /* CPU1 owns ch0 (TX -> CPU0) and ch2 (RX <- CPU0). CPU0's NS side
   * already reset both channels before releasing CPU1, so there is no
   * race window. Skipping the reset on CPU1 also avoids the corner
   * case where CPU1 reset clears a pending ping CPU0 already pushed
   * into the FIFO. */
  *(volatile uint32_t*)0x32100214UL = 0x33333333UL; /* skipped init */

  while (1) {
    *(volatile uint32_t*)0x32100220UL += 1U; /* loop iter counter */
    *(volatile uint32_t*)0x32100230UL = 0xAAAAAAAAUL; /* pre-STA-read marker */
    /* Poll CH2 RX for ping. */
    const uint32_t sta = *(volatile uint32_t*)(k_cpu1_ipc_ch2_addr + k_cpu1_ipc_off_sta);
    *(volatile uint32_t*)0x32100234UL = sta; /* STA read survived */
    if ((sta & (uint32_t)k_cpu1_ipc_sta_rdy) == 0U) {
      continue;
    }
    const uint32_t got = *(volatile uint32_t*)(k_cpu1_ipc_ch2_addr + k_cpu1_ipc_off_rxd);
    *(volatile uint32_t*)0x32100224UL = got; /* most recent rxd */
    if (got != (uint32_t)k_cpu1_pingpong_magic_ping) {
      continue;
    }
    /* Push pong onto CH0 TX. */
    *(volatile uint32_t*)(k_cpu1_ipc_ch0_addr + k_cpu1_ipc_off_txd) =
      (uint32_t)k_cpu1_pingpong_magic_pong;
    *(volatile uint32_t*)0x32100228UL += 1U; /* pong sent counter */
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
  /* CPU1 NS-alias-side marker. The M33 here has SECEXT disabled so it */ /* LEGACY-OK: ARMv8-M bus-architecture term */
  /* is hardware-locked to the NS controller state; the dedicated SRAM_CPU1
   * bank at 0x223F0000 is its physical alias for these BSS reads, but
   * CPU0's J-Link memprobe sees the same bytes through the standard
   * 0x223F0000 view. Bench tail: confirm 0xC0DEDEAD before the data
   * copy starts -- it tells us reset_handler actually ran and the
   * MRAM_CPU1 fetch worked. */
  /* Markers placed in NS_SRAM (CPU0 J-Link memprobe can read this view; */ /* LEGACY-OK: ARMv8-M bus-architecture term */
  /* CPU0's SAU NS_SRAM region 0x22100000-0x221FFFE0 maps NS, and CPU1
   * as a permanent NS controller can write to the same physical bytes
   * through the NS alias 0x32100200). The chip's two views of SRAM
   * see the same backing store. */
  *(volatile uint32_t*)0x32100200UL = 0xC0DEDEADUL;
  *(volatile uint32_t*)0x223F0200UL = 0xC0DEDEADUL;

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
  *(volatile uint32_t*)0x32100204UL = 0xB055A55AUL; /* survived .data copy */

  /* Zero .bss in SRAM_CPU1. */
  uint32_t* bss = &g_ra_ls_cpu1_bss_start;
  while (bss < &g_ra_ls_cpu1_bss_end) {
    *bss = 0U;
    bss++;
  }
  *(volatile uint32_t*)0x32100208UL = 0xBEEFCAFEUL; /* survived .bss zero */

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
