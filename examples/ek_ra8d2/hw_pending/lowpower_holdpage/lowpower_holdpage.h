/**
 * @file examples/ek_ra8d2/hw_pending/lowpower_holdpage/lowpower_holdpage.h
 * @brief Shared-SRAM contract for the low-power "hold the page" dual-core demo
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * This header pins the cross-core message block for the #150 low-power-mode
 * demo at one fixed shared-SRAM address both core images agree on. The
 * Cortex-M85 (primary, "CPU0") renders a page, hands the workload to the
 * Cortex-M33 (secondary, "CPU1"), and parks; the M33 then HOLDS the page and
 * services the user switch at a fraction of the power. The mailbox carries the
 * handoff state both directions.
 *
 * Why a fixed address: each core is a separate compiled image with its own
 * linker script, so a `static` global in one image is invisible to the other.
 * Both linker scripts leave SRAM2 (which starts at 0x22100000) unclaimed -- the
 * M85 image uses SRAM0+SRAM1 (ending at 0x22100000) and the M33 image uses a
 * separate 64 KiB block -- so the bytes at 0x22100000 are backed by the same
 * physical SRAM on both sides with no overlap.
 *
 * Coherency: this app's `system_init.c` leaves the M85 data cache OFF, so a
 * store from one core is visible to the other once a `dsb` has drained the
 * write buffer; no cache clean / invalidate dance is needed. The fields are
 * `volatile` so the compiler emits a real load/store on every access.
 *
 * Protocol (the low-power handoff):
 *   1. M85 renders page 0 (a page marker stands in for a GLCDC render here),
 *      writes `page_num`, sets `active_core = M33`, stamps `magic`, `dsb`.
 *   2. M85 releases the M33 and PARKS (WFI). The M33 owns the page now.
 *   3. M33 holds the page: bumps `m33_heartbeat` each loop, lights the
 *      "active core = M33" LED, and on a SW1 (P009) press edge bumps
 *      `sw1_events` and advances `page_num` (a held-page page-turn the slow
 *      core can do without waking the M85).
 * Waking the M85 for a heavy re-render is the remaining piece tracked in #150.
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
 * @enum lowpower_mailbox_addr_t
 * @brief Fixed shared-SRAM base address of the mailbox.
 *
 * @details The mailbox sits at the base of the CPU0 <-> CPU1 window.
 * The window itself is a board fact, declared once in
 * ``ra8_board_ek_ra8d2_dualcore.h`` along with why both linker scripts leave
 * it free; this app only names its own slice of it.
 *
 * @invariant The address is 16-byte aligned (cache-line safe).
 * @see ra8_board_dualcore_addr_t
 * @see lowpower_mailbox()
 * @since 0.1.0
 */
typedef enum : uintptr_t {
  k_lowpower_mailbox_addr =
    (uintptr_t)k_ra8_board_shared_ram_base, /**< Mailbox base = window base. */
} lowpower_mailbox_addr_t;

/**
 * @enum lowpower_const_t
 * @brief Signature + active-core enumerators shared by both core images.
 *
 * @details `k_lowpower_magic` ("HOLD") lets each core confirm the mailbox is
 * live before trusting its fields; `active_core` names which core currently
 * owns the page -- the whole point of the demo is that it flips from M85 to M33
 * when low-power mode engages.
 *
 * @see lowpower_mailbox_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_lowpower_magic    = 0x484F4C44U, /**< "HOLD" -- M85 stamps it when ready.  */
  k_lowpower_core_m85 = 0U,          /**< active_core: the M85 owns the page.  */
  k_lowpower_core_m33 = 1U,          /**< active_core: the M33 holds the page. */
} lowpower_const_t;

/**
 * @struct lowpower_mailbox_t
 * @brief Cross-core handoff block backed by a fixed shared-SRAM address.
 *
 * @details All fields are `volatile` so each core re-reads memory rather than
 * caching a value in a register. The M85 owns `magic` and the initial
 * `page_num` / `active_core` (writer); the M33 owns `m33_heartbeat`,
 * `sw1_events`, and the post-handoff `page_num` (writer). The opposite core
 * only reads what it does not own.
 *
 * @invariant `magic` is 0 until the M85 stamps ::k_lowpower_magic, then holds.
 * @invariant `active_core` is ::k_lowpower_core_m33 once the M85 has parked.
 * @invariant `m33_heartbeat` increases monotonically while the M33 holds.
 *
 * @par Example:
 * @code
 * volatile lowpower_mailbox_t* mb = lowpower_mailbox();
 * mb->page_num    = 0U;
 * mb->active_core = (uint32_t)k_lowpower_core_m33;
 * mb->magic       = (uint32_t)k_lowpower_magic;
 * __asm volatile("dsb" ::: "memory");      // publish before releasing the M33
 * @endcode
 *
 * @see lowpower_mailbox()
 * @since 0.1.0
 */
typedef struct {
  volatile uint32_t magic;         /**< M85 stamps ::k_lowpower_magic when ready.      */
  volatile uint32_t page_num;      /**< Held page; M85 sets 0, M33 bumps on turn.      */
  volatile uint32_t active_core;   /**< Which core owns the page (::lowpower_const_t). */
  volatile uint32_t m33_heartbeat; /**< M33 increments each hold-loop iteration.       */
  volatile uint32_t sw1_events;    /**< M33 counts SW1 (page-turn) press edges.        */
} lowpower_mailbox_t;

/**
 * @brief Typed pointer to the fixed-address shared mailbox.
 *
 * @details Inlined so both core images compute the identical address with no
 * shared translation unit. Returns the same physical SRAM location on the M85
 * and the M33.
 *
 * @return Pointer to the mailbox at ::k_lowpower_mailbox_addr.
 * @retval non-NULL Always; the address is a compile-time constant.
 *
 * @pre The linker scripts of both images leave SRAM2 unallocated.
 * @pre The M85 data cache is disabled (see file header).
 * @post Returns a valid `volatile` pointer; never NULL.
 * @post No side effects.
 *
 * @note Callable from either core; the pointer arithmetic is identical.
 * @since 0.1.0
 */
static inline volatile lowpower_mailbox_t* lowpower_mailbox(void)
{
  return (volatile lowpower_mailbox_t*)(uintptr_t)k_lowpower_mailbox_addr;
}

#ifdef __cplusplus
}
#endif
