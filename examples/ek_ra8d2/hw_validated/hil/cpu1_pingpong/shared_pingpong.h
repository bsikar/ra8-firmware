/**
 * @file examples/ek_ra8d2/hw_validated/hil/cpu1_pingpong/shared_pingpong.h
 * @brief Shared-SRAM message layout for the CPU0 <-> CPU1 ping-pong demo
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * The CPU0 (M85) <-> CPU1 (M33) ping-pong cannot use the IPC channels'
 * TXD/RXD registers because the chip's IPC security-attribution
 * (SAIPCIRn) is mutually exclusive S xor NS, and CPU1 has SECEXT
 * disabled (HUM Ch 2) so it can only execute non-secure. With CPU0
 * also booting non-secure in this app's build, neither core can write
 * IPCSAR to flip the channel to NS, and the default secure-only
 * channel blocks both ends.
 *
 * This file pins a small message struct at the start of the upper
 * on-chip SRAM region (0x22100000), just above CPU0's allocated SRAM
 * (which ends at 0x22100000) and below CPU1's dedicated 64 KiB bank at
 * the top of SRAM (0x22190000-0x221A0000). Both CPUs' linker scripts
 * leave 0x22100000 unclaimed, so the same physical bytes back the same
 * struct on both sides.
 *
 * Protocol: monotonic sequence counters carry the wakeup signal; the
 * payload words carry the data.
 *
 *   1. CPU0 writes ``ping_payload``, issues DSB, increments ``ping_seq``.
 *   2. CPU1 spins on ``ping_seq != last_seen_ping``. When it advances,
 *      CPU1 reads ``ping_payload``, writes ``pong_payload``, issues DSB,
 *      and sets ``pong_seq = ping_seq``.
 *   3. CPU0 spins on ``pong_seq != prev_pong``. When it advances, CPU0
 *      reads ``pong_payload``.
 *
 * No IPC peripheral is touched.
 *
 * The D-cache is disabled in this app's ``system_init.c`` (see the
 * ``internal_enable_dcache`` call site -- it's referenced as
 * ``(void)internal_enable_dcache;`` so the function isn't actually
 * invoked), so no cache-clean / invalidate dance is needed across the
 * two CPUs.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2_dualcore.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum cpu1_pingpong_addr_t
 * @brief Where this app puts its shared block inside the board's window.
 *
 * @details The block sits at the base of the CPU0 <-> CPU1 window.
 * The window itself is a board fact, declared once in
 * ``ra8_board_ek_ra8d2_dualcore.h`` along with why both linker scripts leave
 * it free; this app only names its own slice of it.
 *
 * @invariant The address is the base of the board's shared window.
 * @see ra8_board_dualcore_addr_t
 * @since 0.1.0
 */
typedef enum : uintptr_t {
  k_cpu1_pingpong_shared_addr =
    (uintptr_t)k_ra8_board_shared_ram_base, /**< Shared block = window base. */
} cpu1_pingpong_addr_t;

/**
 * @enum cpu1_pingpong_shared_const_t
 * @brief Compile-time magics + spin budget for the shared block.
 *
 * @details Both images agree on these so a stale or garbage read on either
 * side surfaces as a payload mismatch rather than as a hang.
 *
 * @invariant ::k_cpu1_pingpong_poll_budget bounds every spin (NASA Rule 2).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cpu1_pingpong_magic_ping  = 0x1234U,   /**< CPU0 ping payload.            */
  k_cpu1_pingpong_magic_pong  = 0x4321U,   /**< CPU1 pong payload.            */
  k_cpu1_pingpong_poll_budget = 2000000UL, /**< Max spin iters per direction. */
} cpu1_pingpong_shared_const_t;

/**
 * @struct cpu1_pingpong_shared_t
 * @brief Cross-CPU message struct backed by a fixed SRAM address.
 *
 * @details Volatile so the compiler emits real memory accesses each
 * iteration of the poll loops. Aligned to 16 bytes so the struct
 * starts on a cache-line / strongly-ordered boundary even if the
 * D-cache is later enabled.
 *
 * @invariant ``pong_seq`` only ever lags ``ping_seq`` by 0 or 1.
 * @invariant Both sequence counters monotonically increase.
 *
 * @see g_cpu1_pingpong_shared
 * @since 0.1.0
 */
typedef struct {
  volatile uint32_t ping_seq;     /**< CPU0 increments after writing payload.   */
  volatile uint32_t pong_seq;     /**< CPU1 sets to match ping_seq after reply. */
  volatile uint32_t ping_payload; /**< CPU0 writes, CPU1 reads.                 */
  volatile uint32_t pong_payload; /**< CPU1 writes, CPU0 reads.                 */
} cpu1_pingpong_shared_t;

/**
 * @brief Typed pointer to the fixed-address shared message block.
 *
 * @details Inlined so both CPUs land on identical addressing without
 * needing a separate translation unit.
 *
 * @return Pointer to the shared struct at ::k_cpu1_pingpong_shared_addr.
 *
 * @pre None.
 * @pre Module state is consistent.
 * @post Returns a valid volatile pointer; never NULL.
 * @post No side effects.
 *
 * @note Both CPUs call this; the pointer arithmetic is identical.
 * @since 0.1.0
 */
RA8_INTERNAL static inline volatile cpu1_pingpong_shared_t* internal_shared(void)
{
  return (volatile cpu1_pingpong_shared_t*)(uintptr_t)k_cpu1_pingpong_shared_addr;
}

#ifdef __cplusplus
}
#endif
