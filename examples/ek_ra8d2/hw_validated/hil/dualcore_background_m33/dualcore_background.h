/**
 * @file examples/ek_ra8d2/hw_validated/hil/dualcore_background_m33/dualcore_background.h
 * @brief Shared-SRAM control block for the M33 autonomous counter demo
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * This header defines the single shared-memory struct that lets the M85
 * observe the M33 running autonomously. The layout is intentionally minimal:
 * three words at a fixed SRAM address that both cores can reach without any
 * linker-visible symbol on the other core's image.
 *
 * Why a fixed address? Each core is a separate compiled image with its own
 * linker script, so a C-level global in one image is invisible to the other.
 * Hard-coding the address is the one name both images resolve identically.
 *
 * Address choice: 0x22100000 is the start of the upper on-chip SRAM region,
 * just above the M85 secure SRAM the linker claims (0x22000000 + 1024K). The
 * M33 image's 64 KiB bank lives far above it at the top of SRAM
 * (0x22190000-0x221A0000), so neither linker script allocates these three
 * words, leaving them free for shared use. Both cores can access this address
 * because the on-chip SRAM bus is shared (HUM Ch 5.1 "Address Space"
 * Table 5.1 p 239 -- dual-core SRAM spans 0x22000000..0x221A0000).
 *
 * Coherency: the M85 leaves its data cache off in system_init.c, so a
 * single `dsb` drains the write buffer and makes a store visible to the other
 * core. The fields are `volatile` so the compiler always emits real
 * load/store instructions.
 *
 * Protocol:
 *   1. M33 boots, writes ::k_bg_m33_signature to `m33_sig`.
 *   2. M33 increments `counter` ::k_bg_target_count times.
 *   3. M33 sets `done = 1`.
 *   4. M85 polls `m33_sig` until it sees the signature (M33 alive).
 *   5. M85 polls `done` until it is 1 (M33 finished).
 *   6. M85 reads `counter` and logs the result.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_board_ek_ra8d2_dualcore.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum bg_addr_t
 * @brief Fixed SRAM address for the shared control block.
 *
 * @details The shared block sits at 0x22100000, the start of the upper
 * on-chip SRAM region just above the M85 secure SRAM allocation (which ends
 * at 0x22000000 + 1 MiB = 0x22100000). The M33 image's 64 KiB bank sits far
 * above it at the top of on-chip SRAM (0x22190000-0x221A0000), so no linker
 * script claims this range.
 *
 * @see dualcore_bg()
 * @since 0.1.0
 */
typedef enum : uintptr_t {
  k_bg_sram_base = (uintptr_t)k_ra8_board_shared_ram_base, /**< Shared block = window base. */
} bg_addr_t;

/**
 * @enum bg_const_t
 * @brief Protocol constants shared by both core images.
 *
 * @details ::k_bg_m33_signature is a distinctive bit pattern the M33 stamps
 * on boot so the M85 can prove the second core left reset. The low byte (0x33)
 * and the high word (0x3333) together make the value easy to spot in a
 * memory-view debugger window.
 *
 * @invariant ::k_bg_target_count fits in a uint32_t; the M33 increments once
 * per iteration so `counter` equals this at completion.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_bg_m33_signature = 0x33330033UL, /**< Boot sentinel written by M33.  */
  k_bg_target_count  = 1000U,        /**< Increment iterations M33 runs. */
} bg_const_t;

/**
 * @struct dualcore_bg_t
 * @brief Shared control block for the autonomous M33 counter demo.
 *
 * @details All fields are `volatile` so each core reloads from SRAM on every
 * access rather than using a cached register value. The M33 owns the write
 * side of every field; the M85 only reads them after releasing the M33.
 *
 * @invariant `done` is 0 until the M33 finishes all ::k_bg_target_count
 *            increments, then 1 forever.
 * @invariant `m33_sig` is 0 until the M33 boots, then ::k_bg_m33_signature.
 * @invariant `counter` increases monotonically from 0 to ::k_bg_target_count.
 *
 * @par Example:
 * @code
 * volatile dualcore_bg_t* bg = dualcore_bg();
 * while (bg->done == 0U) {}              // wait for M33
 * uint32_t n = bg->counter;              // read result
 * @endcode
 *
 * @see dualcore_bg()
 * @since 0.1.0
 */
typedef struct {
  volatile uint32_t counter; /**< Incremented ::k_bg_target_count times by M33. */
  volatile uint32_t done;    /**< Set to 1 by M33 once counting is complete.    */
  volatile uint32_t m33_sig; /**< M33 stamps ::k_bg_m33_signature here on boot. */
} dualcore_bg_t;

/**
 * @brief Typed pointer to the fixed-address shared control block.
 *
 * @details Returns the same physical SRAM address whether called from the M85
 * image or the M33 image. Inlined so no shared translation unit is required.
 *
 * @return Pointer to the shared block at ::k_bg_sram_base.
 * @retval non-NULL Always; the address is a compile-time constant.
 *
 * @pre Both linker scripts leave the range at ::k_bg_sram_base unclaimed.
 * @pre The M85 data cache is disabled so dsb-after-write is sufficient.
 * @post Returns a valid `volatile` pointer; no side effects.
 * @post Pointer arithmetic is identical on both cores.
 *
 * @note Safe to call from either core; the cast is the same on both images.
 * @since 0.1.0
 */
static inline volatile dualcore_bg_t* dualcore_bg(void)
{
  return (volatile dualcore_bg_t*)(uintptr_t)k_bg_sram_base;
}

#ifdef __cplusplus
}
#endif
