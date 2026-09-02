/**
 * @file examples/ek_ra8d2/hw_validated/hil/dualcore_mailbox/inc/dualcore_mailbox.h
 * @brief Shared-SRAM mailbox layout for the M85 <-> M33 dual-core demo
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * This is the contract that lets the two cores of the RA8D2 talk to each
 * other. The Cortex-M85 (primary, "CPU0") and the Cortex-M33 (secondary,
 * "CPU1") do not share a cache, but they DO share the on-chip SRAM. This
 * header pins a small message struct at one fixed SRAM address that both
 * cores agree on, so a write by one core is seen by the other.
 *
 * Why a fixed address instead of a normal global? Each core is a separate
 * compiled image with its own linker script, so a `static` global in one
 * image is invisible to the other. A hard-coded shared address is the one
 * name both images can resolve identically. Neither linker script claims the
 * 0x22100000 mailbox word:
 *   - the M85 image (`linker_script.ld`) uses SRAM0+SRAM1, ending at
 *     0x22100000;
 *   - the M33 image (`linker_script_cpu1.ld`) uses a separate 64 KiB block
 *     at the top of on-chip SRAM (0x22190000-0x221A0000).
 * So the bytes at 0x22100000 are backed by the same physical SRAM on both
 * sides with no overlap.
 *
 * Coherency: this app's `system_init.c` leaves the M85 data cache OFF, so a
 * store from one core is visible to the other once a `dsb` has drained the
 * write buffer. No cache clean / invalidate dance is needed. The fields are
 * `volatile` so the compiler emits a real load/store on every access.
 *
 * Protocol (a request / reply handshake carried by sequence counters):
 *   1. M85 writes `request`, issues `dsb`, then increments `request_seq`.
 *   2. M33 spins until `request_seq` advances. It then reads `request`,
 *      writes `reply = request * 3 + 1`, increments `m33_replies`, issues
 *      `dsb`, and sets `reply_seq = request_seq`.
 *   3. M85 spins until `reply_seq` reaches the value it sent, then reads
 *      `reply` and checks it equals `request * 3 + 1`.
 * The `* 3 + 1` is deliberately a tiny computation, not a copy: a correct
 * reply proves the M33 actually executed arithmetic, not merely echoed a
 * byte.
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
 * @enum dualcore_mailbox_addr_t
 * @brief Where this app puts its mailbox inside the board's shared window.
 *
 * @details The mailbox sits at the base of the CPU0 <-> CPU1 window.
 * The window itself is a board fact, declared once in
 * ``ra8_board_ek_ra8d2_dualcore.h`` along with why both linker scripts leave
 * it free; this app only names its own slice of it.
 *
 * @invariant ::k_dualcore_mailbox_addr is 16-byte aligned (cache-line safe).
 * @see ra8_board_dualcore_addr_t
 * @see dualcore_mailbox_t
 * @since 0.1.0
 */
typedef enum : uintptr_t {
  k_dualcore_mailbox_addr =
    (uintptr_t)k_ra8_board_shared_ram_base, /**< Mailbox base = window base. */
} dualcore_mailbox_addr_t;

/**
 * @enum dualcore_mailbox_const_t
 * @brief Compile-time signature and transform constants.
 *
 * @details Shared by both core images so the M85 and M33 agree on what a valid
 * reply looks like.
 *
 * @invariant ::k_dualcore_m33_mul and ::k_dualcore_m33_add define the reply
 *            transform both images implement.
 * @see dualcore_mailbox_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_dualcore_m33_signature = 33U,        /**< M33 stamps this once it boots ("33"). */
  k_dualcore_m33_mul       = 3U,         /**< Reply = request * mul + add.          */
  k_dualcore_m33_add       = 1U,         /**< Reply = request * mul + add.          */
  k_dualcore_poll_budget   = 20000000UL, /**< Bounded spin iters per wait (Rule 2). */
} dualcore_mailbox_const_t;

/**
 * @struct dualcore_mailbox_t
 * @brief Cross-core message block backed by a fixed shared-SRAM address.
 *
 * @details All fields are `volatile` so each core re-reads memory every
 * iteration of its poll loop rather than caching a value in a register.
 * The M85 owns the `request*` fields (writer) and the M33 owns the `reply*`,
 * `m33_signature`, and `m33_replies` fields (writer); the opposite core only
 * reads them. The sequence counters carry the wakeup edge, the data words
 * carry the payload.
 *
 * @invariant `reply_seq` lags `request_seq` by 0 or 1.
 * @invariant Both sequence counters increase monotonically.
 * @invariant `m33_signature` is 0 until the M33 has booted, then
 *            ::k_dualcore_m33_signature forever.
 *
 * @par Example:
 * @code
 * volatile dualcore_mailbox_t* mb = dualcore_mailbox();
 * mb->request = 7U;
 * __asm volatile("dsb" ::: "memory");
 * mb->request_seq = 1U;            // wake the M33
 * @endcode
 *
 * @see dualcore_mailbox()
 * @since 0.1.0
 */
typedef struct {
  volatile uint32_t request_seq;   /**< M85 increments after writing `request`.    */
  volatile uint32_t reply_seq;     /**< M33 sets equal to `request_seq` when done. */
  volatile uint32_t request;       /**< M85 -> M33: the operand to transform.      */
  volatile uint32_t reply;         /**< M33 -> M85: `request * 3 + 1`.             */
  volatile uint32_t m33_signature; /**< M33 stamps ::k_dualcore_m33_signature.     */
  volatile uint32_t m33_replies;   /**< M33 increments per reply (liveness count). */
} dualcore_mailbox_t;

/**
 * @brief Typed pointer to the fixed-address shared mailbox.
 *
 * @details Inlined so both core images compute the identical address with no
 * shared translation unit. Returns the same physical SRAM location on the
 * M85 and the M33.
 *
 * @return Pointer to the mailbox at ::k_dualcore_mailbox_addr.
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
static inline volatile dualcore_mailbox_t* dualcore_mailbox(void)
{
  return (volatile dualcore_mailbox_t*)(uintptr_t)k_dualcore_mailbox_addr;
}

#ifdef __cplusplus
}
#endif
