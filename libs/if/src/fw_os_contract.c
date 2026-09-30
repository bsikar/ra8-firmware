/**
 * @file fw_os_contract.c
 * @brief Compile-time conformance for the `fw_os` port contract.
 * @ingroup grp_fw_os
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details
 * `fw_os.h` declares an OS port that no binding implements yet. A header with
 * no translation unit behind it is never compiled, and a header nobody
 * compiles rots: `arch/arch.h` sat in this tree carrying a documented
 * `K_ARCH_FAULT_RAW_MAX` that was defined nowhere, precisely because nothing
 * ever fed it to a compiler.
 *
 * This translation unit exists so that does not happen here. It is picked up by
 * the same source discovery every other file in this directory uses,
 * so the ordinary build compiles the contract. It emits no code and defines no
 * runtime symbol: everything below is a compile-time assertion about the
 * contract's own consistency.
 *
 * What it checks that the header cannot check about itself:
 *
 *   - the caller-owned storage really is large enough for a control block of
 *     the size the binding declared, and is aligned for one;
 *   - the portable priority band is ordered and contiguous, so a binding may
 *     map it onto a scale with a subtraction rather than a switch;
 *   - the timeout sentinels cannot collide with a real millisecond value a
 *     caller would plausibly pass.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

#include "fw_os.h"

/* Storage is expressed in 64-bit words so every handle is 8-byte aligned,
 * which is the strictest alignment any binding control block in this tree
 * needs. A binding that needs more says so by failing here. */
static_assert(alignof(fw_os_thread_t) >= alignof(uint64_t),
              "fw_os_thread_t must be 8-byte aligned for a binding control block");
static_assert(alignof(fw_os_mutex_t) >= alignof(uint64_t),
              "fw_os_mutex_t must be 8-byte aligned for a binding control block");
static_assert(alignof(fw_os_sem_t) >= alignof(uint64_t),
              "fw_os_sem_t must be 8-byte aligned for a binding control block");

static_assert(sizeof(fw_os_thread_t) == K_FW_OS_THREAD_STORAGE_WORDS * sizeof(uint64_t),
              "fw_os_thread_t carries exactly the storage its caps constant declares");
static_assert(sizeof(fw_os_mutex_t) == K_FW_OS_MUTEX_STORAGE_WORDS * sizeof(uint64_t),
              "fw_os_mutex_t carries exactly the storage its caps constant declares");
static_assert(sizeof(fw_os_sem_t) == K_FW_OS_SEM_STORAGE_WORDS * sizeof(uint64_t),
              "fw_os_sem_t carries exactly the storage its caps constant declares");

/* The band is mapped by subtraction in a binding, so it must stay ordered and
 * contiguous from zero. A gap or a reorder silently shifts every priority a
 * binding computes. */
static_assert(k_fw_os_priority_idle == 0U, "the portable priority band starts at zero");
static_assert(k_fw_os_priority_low == k_fw_os_priority_idle + 1U, "the band is contiguous");
static_assert(k_fw_os_priority_normal == k_fw_os_priority_low + 1U, "the band is contiguous");
static_assert(k_fw_os_priority_high == k_fw_os_priority_normal + 1U, "the band is contiguous");

/* K_FW_OS_NO_WAIT is a real duration (zero) and K_FW_OS_WAIT_FOREVER is not,
 * so the forever sentinel has to sit outside any millisecond count a caller
 * could mean. 2^32-1 ms is ~49.7 days, the same point fw_os_uptime_ms wraps. */
static_assert(K_FW_OS_NO_WAIT == 0UL, "no-wait is the zero duration, not a sentinel");
static_assert(K_FW_OS_WAIT_FOREVER == UINT32_MAX,
              "wait-forever must sit at the top of the millisecond range it excludes");

#if FW_OS_HAS_QUEUE
static_assert(alignof(fw_os_queue_t) >= alignof(uint64_t),
              "fw_os_queue_t must be 8-byte aligned for a binding control block");
static_assert(sizeof(fw_os_queue_t) == K_FW_OS_QUEUE_STORAGE_WORDS * sizeof(uint64_t),
              "fw_os_queue_t carries exactly the storage its caps constant declares");
#endif
