/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/cpu1_pingpong_ipc/cpu1_main.c
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

#include "ra8_err.h"
#include "ra8_ipc.h"

extern uint32_t g_ra8_ls_cpu1_stack_top;
extern uint32_t g_ra8_ls_cpu1_data_start;
extern uint32_t g_ra8_ls_cpu1_data_end;
extern uint32_t g_ra8_ls_cpu1_data_load;
extern uint32_t g_ra8_ls_cpu1_bss_start;
extern uint32_t g_ra8_ls_cpu1_bss_end;

[[noreturn]] void cpu1_reset_handler(void);

typedef enum : uint32_t {
  k_cpu1_pingpong_magic_ping = 0x1234U, /**< Cpu1 pingpong magic ping. */
  k_cpu1_pingpong_magic_pong = 0x4321U, /**< Cpu1 pingpong magic pong. */
} cpu1_main_const_t;

typedef enum : uint8_t {
  k_cpu1_pingpong_pair_zero = 0U, /**< Cpu1 pingpong pair zero. */
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
  k_cpu1_ipc_off_sta = 0x00U, /**< Cpu1 ipc off sta. */
  k_cpu1_ipc_off_txd = 0x08U, /**< Cpu1 ipc off txd. */
  k_cpu1_ipc_off_rxd = 0x0CU, /**< Cpu1 ipc off rxd. */
  k_cpu1_ipc_off_clr = 0x10U, /**< Cpu1 ipc off clr. */
} cpu1_ipc_off_t;

typedef enum : uint32_t {
  k_cpu1_ipc_sta_rdy = 0x00010000UL, /**< Cpu1 ipc sta rdy. */
  k_cpu1_ipc_clr_all = 0x030100FFUL, /**< Cpu1 ipc clr all. */
} cpu1_ipc_mask_t;

/**
 * @enum cpu1_sau_reg_t
 * @brief Armv8-M SAU programming registers, addressed directly.
 * @details These are Cortex-M33 CPU-architectural registers (Arm v8-M
 *          Architecture Reference Manual, "SAU registers"), not RA8D2
 *          peripheral registers, so they carry an Arm ARM reference rather
 *          than a HUM citation. The M33 HAL accessors are unreachable from
 *          this image, hence the direct addresses.
 * @invariant Every value is a word-aligned address inside the System Control
 *            Space at 0xE000E000.
 * @see cpu1_sau_region_t  The region base/limit values written through them.
 */
typedef enum : uintptr_t {
  k_cpu1_sau_ctrl_addr = 0xE000EDD0UL, /**< SAU_CTRL: SAU enable bit.        */
  k_cpu1_sau_rnr_addr  = 0xE000EDD8UL, /**< SAU_RNR: region number select.   */
  k_cpu1_sau_rbar_addr = 0xE000EDDCUL, /**< SAU_RBAR: region base address.   */
  k_cpu1_sau_rlar_addr = 0xE000EDE0UL, /**< SAU_RLAR: region limit + enable. */
} cpu1_sau_reg_t;

/**
 * @enum cpu1_sau_region_t
 * @brief Base and limit words for the four SAU regions this image programmes.
 * @details Each limit value carries the SAU_RLAR ENABLE bit (bit 0) already
 *          set, which is why every limit ends in 1 rather than 0.
 * @invariant The four regions do not overlap: overlapping SAU regions resolve
 *            to Secure, which a permanent-NS M33 cannot reach.
 * @see cpu1_sau_reg_t  The registers these are written to.
 */
typedef enum : uint32_t {
  k_cpu1_sau_periph_ns_base  = 0x50000000UL, /**< R0 base: peripheral NS alias. */
  k_cpu1_sau_periph_ns_limit = 0x5FFFFFE1UL, /**< R0 limit | ENABLE.            */
  k_cpu1_sau_periph_s_base   = 0x40000000UL, /**< R1 base: peripheral S alias.  */
  k_cpu1_sau_periph_s_limit  = 0x4FFFFFE1UL, /**< R1 limit | ENABLE.            */
  k_cpu1_sau_ns_sram_base    = 0x22100000UL, /**< R2 base: NS SRAM window.      */
  k_cpu1_sau_ns_sram_limit   = 0x221FFFE1UL, /**< R2 limit | ENABLE.            */
  k_cpu1_sau_mram_base       = 0x020C0000UL, /**< R3 base: MRAM_CPU1 image.     */
  k_cpu1_sau_mram_limit      = 0x020FFFE1UL, /**< R3 limit | ENABLE.            */
} cpu1_sau_region_t;

/**
 * @enum cpu1_probe_addr_t
 * @brief Bench probe words this image writes for a J-Link post-mortem.
 * @details CPU1 is a permanent-NS controller, so it reaches shared SRAM
 *          through the NS alias at 0x321.....; CPU0's J-Link memprobe sees
 *          the same backing store through the standard view. Each word marks
 *          one point in the boot and IPC sequence having been reached.
 * @invariant Every address lies inside an SAU region this image programmes NS.
 * @see cpu1_probe_val_t  The sentinel values written to them.
 */
typedef enum : uintptr_t {
  k_cpu1_probe_reset_addr   = 0x32100200UL, /**< Reset handler was entered.    */
  k_cpu1_probe_data_addr    = 0x32100204UL, /**< .data copy completed.         */
  k_cpu1_probe_bss_addr     = 0x32100208UL, /**< .bss zero-fill completed.     */
  k_cpu1_probe_main_addr    = 0x3210020CUL, /**< cpu1_main was entered.        */
  k_cpu1_probe_sau_addr     = 0x32100214UL, /**< SAU programming completed.    */
  k_cpu1_probe_iter_addr    = 0x32100220UL, /**< IPC loop iteration counter.   */
  k_cpu1_probe_rxd_addr     = 0x32100224UL, /**< Most recently received word.  */
  k_cpu1_probe_pong_addr    = 0x32100228UL, /**< Pong-sent counter.            */
  k_cpu1_probe_preread_addr = 0x32100230UL, /**< Set before each IPC read.     */
  k_cpu1_probe_sta_addr     = 0x32100234UL, /**< CH2 status read survived.     */
  k_cpu1_probe_sem_addr     = 0x32100238UL, /**< IPCSEM0 read survived.        */
  k_cpu1_probe_reset_s_addr = 0x22190200UL, /**< Reset marker, SRAM_CPU1 view. */
  k_cpu1_ipcsem0_ns_addr    = 0x50020000UL, /**< IPCSEM0 through the NS alias. */
} cpu1_probe_addr_t;

/**
 * @enum cpu1_probe_val_t
 * @brief Sentinel values written to the ::cpu1_probe_addr_t probe words.
 * @details Each is visually distinct in a memory dump so a bench operator can
 *          tell how far the image progressed from the raw hex alone.
 * @invariant No value is a plausible uninitialised-SRAM pattern.
 * @see cpu1_probe_addr_t  The addresses these are written to.
 */
typedef enum : uint32_t {
  k_cpu1_probe_reset_val = 0xC0DEDEADUL, /**< Written by cpu1_reset_handler.   */
  k_cpu1_probe_data_val  = 0xB055A55AUL, /**< Written after the .data copy.    */
  k_cpu1_probe_bss_val   = 0xBEEFCAFEUL, /**< Written after the .bss zero.     */
  k_cpu1_probe_main_val  = 0x11111111UL, /**< Written on cpu1_main entry.      */
  k_cpu1_probe_sau_val   = 0x33333333UL, /**< Written once the SAU is enabled. */
  k_cpu1_probe_pre_val   = 0xAAAAAAAAUL, /**< Written before each IPC read.    */
} cpu1_probe_val_t;

/**
 * @brief Programme CPU1's SAU and enable it.
 *
 * @details CPU1's M33 has its own SAU with 8 regions (SAU_TYPE.SREGION=8).
 *          Out of reset SAU is disabled and the IDAU default treats
 *          bit-28-clear as Secure. This programmes explicit NS regions for
 *          the peripheral window, the NS SRAM (which now also holds the CPU1
 *          SRAM bank), and the CPU1 MRAM image, then enables the SAU.
 * @pre Called from cpu1_main before any NS peripheral or IPC access.
 * @pre The M33 is still running with its reset-default SAU (disabled).
 * @post Regions 0-3 are programmed and SAU_CTRL.ENABLE is set.
 * @post k_cpu1_probe_sau_addr holds k_cpu1_probe_sau_val, so a bench
 *       post-mortem can tell this stage completed.
 * @note Not thread-safe; single-threaded boot context by construction.
 * @warning Overlapping SAU regions resolve to Secure, which the permanent-NS
 *          M33 cannot reach -- which is why one region covers both the shared
 *          markers and the CPU1 bank rather than two overlapping ones.
 * @since 0.1.0
 */
static void cpu1_sau_init(void)
{
  /* Region 0: peripherals NS alias (0x50000000-0x5FFFFFE0). */
  *(volatile uint32_t*)k_cpu1_sau_rnr_addr  = 0x00000000UL;                        /* SAU_RNR  */
  *(volatile uint32_t*)k_cpu1_sau_rbar_addr = (uint32_t)k_cpu1_sau_periph_ns_base; /* SAU_RBAR */
  *(volatile uint32_t*)k_cpu1_sau_rlar_addr =
    (uint32_t)k_cpu1_sau_periph_ns_limit; /* SAU_RLAR limit | enable */
  /* Region 1: peripherals S alias (0x40000000-0x4FFFFFE0). */
  *(volatile uint32_t*)k_cpu1_sau_rnr_addr  = 0x00000001UL;
  *(volatile uint32_t*)k_cpu1_sau_rbar_addr = (uint32_t)k_cpu1_sau_periph_s_base;
  *(volatile uint32_t*)k_cpu1_sau_rlar_addr = (uint32_t)k_cpu1_sau_periph_s_limit;
  /* Region 2: NS SRAM range (0x22100000-0x221FFFE0). The CPU1 SRAM bank
   * (0x22190000-0x2219FFE0) now lives inside this window, so one NS region
   * covers both the shared markers and the bank -- a separate bank region
   * would overlap this one, and overlapping SAU regions resolve to Secure,
   * which the permanent-NS M33 cannot reach. */
  *(volatile uint32_t*)k_cpu1_sau_rnr_addr  = 0x00000002UL;
  *(volatile uint32_t*)k_cpu1_sau_rbar_addr = (uint32_t)k_cpu1_sau_ns_sram_base;
  *(volatile uint32_t*)k_cpu1_sau_rlar_addr = (uint32_t)k_cpu1_sau_ns_sram_limit;
  /* Region 3: MRAM_CPU1 code (0x020C0000-0x020FFFE0). */
  *(volatile uint32_t*)k_cpu1_sau_rnr_addr  = 0x00000003UL;
  *(volatile uint32_t*)k_cpu1_sau_rbar_addr = (uint32_t)k_cpu1_sau_mram_base;
  *(volatile uint32_t*)k_cpu1_sau_rlar_addr = (uint32_t)k_cpu1_sau_mram_limit;
  /* Enable SAU (no ALLNS, so unprogrammed defaults to IDAU). */
  *(volatile uint32_t*)k_cpu1_sau_ctrl_addr = 0x00000001UL;
  __asm__ volatile("dsb 0xf" ::: "memory");
  __asm__ volatile("isb 0xf" ::: "memory");
  *(volatile uint32_t*)k_cpu1_probe_sau_addr = (uint32_t)k_cpu1_probe_sau_val; /* SAU configured */
}

[[noreturn]] static void cpu1_main(void)
{
  *(volatile uint32_t*)k_cpu1_probe_main_addr =
    (uint32_t)k_cpu1_probe_main_val; /* cpu1_main entry */

  cpu1_sau_init();

  while (1) {
    *(volatile uint32_t*)k_cpu1_probe_iter_addr += 1U; /* loop iter counter */
    *(volatile uint32_t*)k_cpu1_probe_preread_addr =
      (uint32_t)k_cpu1_probe_pre_val; /* pre-IPC-read marker */
    /* TEST: read IPCSEM0 (the simplest IPC reg) first to isolate fault.
     * IPCSEM0 is at 0x40020000 / NS alias 0x50020000.
     * IPCSAR.SAIPCSEM0 (bit 0) was set NS too. */
    const uint32_t sem                         = *(volatile uint32_t*)k_cpu1_ipcsem0_ns_addr;
    *(volatile uint32_t*)k_cpu1_probe_sem_addr = sem; /* IPCSEM0 read survived */
    /* Poll CH2 RX for ping. */
    const uint32_t sta = *(volatile uint32_t*)(k_cpu1_ipc_ch2_addr + k_cpu1_ipc_off_sta);
    *(volatile uint32_t*)k_cpu1_probe_sta_addr = sta; /* STA read survived */
    if ((sta & (uint32_t)k_cpu1_ipc_sta_rdy) == 0U) {
      continue;
    }
    const uint32_t got = *(volatile uint32_t*)(k_cpu1_ipc_ch2_addr + k_cpu1_ipc_off_rxd);
    *(volatile uint32_t*)k_cpu1_probe_rxd_addr = got; /* most recent rxd */
    if (got != (uint32_t)k_cpu1_pingpong_magic_ping) {
      continue;
    }
    /* Push pong onto CH0 TX. */
    *(volatile uint32_t*)(k_cpu1_ipc_ch0_addr + k_cpu1_ipc_off_txd) =
      (uint32_t)k_cpu1_pingpong_magic_pong;
    *(volatile uint32_t*)k_cpu1_probe_pong_addr += 1U; /* pong sent counter */
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
 * ``s_ipc_channels`` array in ``ra8_ipc.c`` -- ``.bss`` -- and any
 * initialised file-scope statics -- ``.data``) hold whatever pattern
 * SRAM_CPU1 contained at boot. That was the reason CPU1 silently
 * stayed wedged after Agent D embedded the CPU1 binary: the M33
 * jumped straight into ``cpu1_main`` with uninitialised globals, so
 * ``ra8_ipc_init`` / ``ra8_ipc_recv_message`` operated on garbage state
 * structures and the channel pair never came alive.
 *
 * @pre Initial SP loaded by hardware from the first slot of
 *      ``.cpu1_vectors`` (= ``g_ra8_ls_cpu1_stack_top``).
 * @pre CPU1 has just exited reset via the ACTREQ handshake driven by
 *      ``ra8_cpu1_release`` on CPU0.
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
   * bank at 0x22190000 is its physical alias for these BSS reads, but
   * CPU0's J-Link memprobe sees the same bytes through the standard
   * 0x22190000 view. Bench tail: confirm 0xC0DEDEAD before the data
   * copy starts -- it tells us reset_handler actually ran and the
   * MRAM_CPU1 fetch worked. */
  /* Markers placed in NS_SRAM (CPU0 J-Link memprobe can read this view; */ /* LEGACY-OK: ARMv8-M bus-architecture term */
  /* CPU0's SAU NS_SRAM region 0x22100000-0x221FFFE0 maps NS, and CPU1
   * as a permanent NS controller can write to the same physical bytes
   * through the NS alias 0x32100200). The chip's two views of SRAM
   * see the same backing store. */
  *(volatile uint32_t*)k_cpu1_probe_reset_addr   = (uint32_t)k_cpu1_probe_reset_val;
  *(volatile uint32_t*)k_cpu1_probe_reset_s_addr = (uint32_t)k_cpu1_probe_reset_val;

  /* Copy .data from MRAM_CPU1 load address into SRAM_CPU1. The linker
   * defines ``g_ra8_ls_cpu1_data_load`` as LOADADDR(.data) so this
   * works regardless of the absolute MRAM_CPU1 base. */
  uint32_t* dst = &g_ra8_ls_cpu1_data_start;
  uint32_t* src = &g_ra8_ls_cpu1_data_load;
  while (dst < &g_ra8_ls_cpu1_data_end) {
    *dst = *src;
    dst++;
    src++;
  }
  *(volatile uint32_t*)k_cpu1_probe_data_addr =
    (uint32_t)k_cpu1_probe_data_val; /* survived .data copy */

  /* Zero .bss in SRAM_CPU1. */
  uint32_t* bss = &g_ra8_ls_cpu1_bss_start;
  while (bss < &g_ra8_ls_cpu1_bss_end) {
    *bss = 0U;
    bss++;
  }
  *(volatile uint32_t*)k_cpu1_probe_bss_addr =
    (uint32_t)k_cpu1_probe_bss_val; /* survived .bss zero */

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
#ifndef RA8_OFF_TARGET
/* Vector table only built for the cross-compiled M33 image. The host
 * build does not link this TU as an executable -- it is compile-checked
 * only -- so we can drop the table without losing test coverage. */
[[gnu::used, gnu::section(".cpu1_vectors")]] const uintptr_t g_cpu1_vector_table[] = {
  (uintptr_t)&g_ra8_ls_cpu1_stack_top,
  (uintptr_t)&cpu1_reset_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
};
#endif
