/**
 * @file examples/ek_ra8d2/hw_validated/hil/cache_coherency_hil/inc/cache_coherency_shared.h
 * @brief Shared-SRAM message layout for the M85<->M33 cache-coherency HIL test
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * This app proves that the Cortex-M85 (CPU0) and Cortex-M33 (CPU1) stay
 * coherent across the shared on-chip SRAM bank **with the M85 D-cache
 * ENABLED**, by routing every cross-core hand-off through the
 * non-cacheable MPU region (region 4) that the shared boot maps over
 * ``0x22100000..0x2219FFFF`` when ``RA8_BOOT_ENABLE_CACHE_MPU`` is set
 * (see ``libs/ra8_board_ek_ra8d2/src/boot/system_init.c``).
 *
 * Like ``cpu1_pingpong``, the cross-core channel is a small message
 * struct pinned at the start of the upper on-chip SRAM region
 * (``0x22100000`` = SRAM2), just above CPU0's allocated SRAM (which
 * ends at ``0x22100000``) and below CPU1's dedicated 64 KiB bank at the
 * top of SRAM (``0x22190000..0x221A0000``). Both CPUs' linker scripts
 * leave ``0x22100000`` unclaimed, so the same physical bytes back the
 * same struct on both sides. The IPC peripheral is intentionally NOT
 * used (CPU1 has SECEXT disabled and IPC channel attribution is
 * mutually exclusive S xor NS -- see ``cpu1_pingpong`` for the full
 * rationale).
 *
 * Protocol (monotonic sequence counters carry the wakeup signal; the
 * payload words carry the data being checked end-to-end):
 *
 *   1. CPU0 writes ``ping_payload`` = ::k_cache_coherency_ping_base + r,
 *      issues DSB, increments ``ping_seq``.
 *   2. CPU1 spins on ``ping_seq``; on each advance it reads
 *      ``ping_payload``, writes the transformed echo
 *      ``pong_payload`` = ``ping_payload`` + ::k_cache_coherency_delta,
 *      issues DSB, and sets ``pong_seq = ping_seq``.
 *   3. CPU0 spins on ``pong_seq``; on each advance it verifies
 *      ``pong_payload`` == ::k_cache_coherency_pong_base + r.
 *
 * Because the struct lives in the **non-cacheable** region, no manual
 * ``ra8_cache_dcache_clean_by_addr`` / ``..._invalidate_by_addr`` dance
 * (``ra8_cache.h``) is needed even though the M85 D-cache is on: a
 * cacheable placement would let the M85 read a stale ``pong_payload``
 * out of its own dirty/clean line (or hide its ``ping_payload`` write
 * from the cacheless M33), and the test would log a mismatch. The
 * non-cacheable region is exactly what removes that hazard.
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
 * @enum cache_coherency_addr_t
 * @brief Where this app puts its shared block inside the board's window.
 *
 * @details The block sits at the base of the CPU0 <-> CPU1 window, which the
 * boot MPU marks Normal non-cacheable -- the property this test exercises.
 * The window itself is a board fact, declared once in
 * ``ra8_board_ek_ra8d2_dualcore.h`` along with why both linker scripts leave
 * it free; this app only names its own slice of it.
 *
 * @invariant The address is 32-byte (cache-line) aligned; asserted below.
 * @see ra8_board_dualcore_addr_t
 * @since 0.1.0
 */
typedef enum : uintptr_t {
  k_cache_coherency_shared_addr =
    (uintptr_t)k_ra8_board_shared_ram_base, /**< Shared block = window base. */
} cache_coherency_addr_t;

/**
 * @enum cache_coherency_shared_const_t
 * @brief Compile-time magics + budgets for the shared block.
 *
 * @details
 * The payload check is data-carrying, not a constant magic: CPU0 sends
 * ``ping_base + r`` for round ``r`` and expects ``pong_base + r`` back,
 * where ``pong_base - ping_base`` == ::k_cache_coherency_delta. CPU1
 * produces that by adding the fixed delta to whatever it reads, so a
 * stale/garbage read on either side surfaces as a payload mismatch.
 */
typedef enum : uint32_t {
  k_cache_coherency_ping_base   = 0x1234U,   /**< CPU0 round-0 ping payload.        */
  k_cache_coherency_pong_base   = 0x4321U,   /**< Expected round-0 pong payload.    */
  k_cache_coherency_delta       = 0x30EDU,   /**< pong_base - ping_base (CPU1 add). */
  k_cache_coherency_rounds      = 8UL,       /**< Verified rounds before PASS.      */
  k_cache_coherency_poll_budget = 2000000UL, /**< Max spin iters per direction.     */
  k_cache_coherency_align_bytes = 32UL,      /**< Cache-line / region quantum.      */
} cache_coherency_shared_const_t;

/* The data-carry invariant the two cores rely on: CPU1 adds the fixed
 * delta to the ping payload, and that must land exactly on the value
 * CPU0 expects (pong_base + r == (ping_base + r) + delta). */
static_assert((uint32_t)k_cache_coherency_ping_base + (uint32_t)k_cache_coherency_delta ==
                (uint32_t)k_cache_coherency_pong_base,
              "ping_base + delta must equal pong_base");

/* The shared struct must start on a 32-byte (cache-line) boundary so an
 * enabled D-cache never straddles it with adjacent cacheable data. The
 * fixed SRAM2 base is 1 MiB aligned, so this holds by construction. */
static_assert((uintptr_t)k_cache_coherency_shared_addr % (uintptr_t)k_cache_coherency_align_bytes ==
                0U,
              "shared block must be 32-byte (cache-line) aligned");

/**
 * @struct cache_coherency_shared_t
 * @brief Cross-CPU message struct backed by a fixed non-cacheable SRAM address.
 *
 * @details Every field is ``volatile`` so the compiler emits a real
 * memory access each loop iteration on both cores. The struct is small
 * (16 bytes) and pinned at a 32-byte-aligned address, so with the M85
 * D-cache enabled it occupies a single cache line -- except the MPU
 * marks that line's region non-cacheable, so the accesses go straight
 * to SRAM and stay coherent with the cacheless M33.
 *
 * @invariant ``pong_seq`` only ever lags ``ping_seq`` by 0 or 1.
 * @invariant Both sequence counters monotonically increase.
 *
 * @see cache_coherency_shared
 * @since 0.1.0
 */
typedef struct {
  volatile uint32_t ping_seq;     /**< CPU0 increments after writing payload.   */
  volatile uint32_t pong_seq;     /**< CPU1 sets to match ping_seq after reply. */
  volatile uint32_t ping_payload; /**< CPU0 writes ping_base + r, CPU1 reads.   */
  volatile uint32_t pong_payload; /**< CPU1 writes ping_payload + delta.        */
} cache_coherency_shared_t;

/**
 * @brief Typed pointer to the fixed-address shared message block.
 *
 * @details Inlined so both CPUs land on identical addressing without
 * needing a separate translation unit.
 *
 * @return Pointer to the shared struct at ::k_cache_coherency_shared_addr.
 *
 * @pre None.
 * @pre Module state is consistent.
 * @post Returns a valid volatile pointer; never NULL.
 * @post No side effects.
 *
 * @note Both CPUs call this; the pointer arithmetic is identical.
 * @since 0.1.0
 */
RA8_INTERNAL static inline volatile cache_coherency_shared_t* internal_shared(void)
{
  return (volatile cache_coherency_shared_t*)(uintptr_t)k_cache_coherency_shared_addr;
}

#ifdef __cplusplus
}
#endif
