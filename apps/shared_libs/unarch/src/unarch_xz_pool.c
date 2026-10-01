/**
 * @file unarch_xz_pool.c
 * @brief Bump-arena implementation behind xz-embedded's allocator seam.
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * See `unarch_xz_pool.h` for the contract. The arena arithmetic is no longer
 * written here: one module-static ::ra8_imgdec_scratch_t holds the installed
 * store and every entry point forwards to the shared decoder-scratch
 * contract (`ra8_imgdec_scratch.h`, issue RA8FW-308), which is where rounding,
 * capacity and cursor accounting live for every image-path bump arena in the
 * tree. What stays local is the policy this seam publishes and the contract
 * does not have: the install-time alignment precondition, the fail-closed
 * refusal of a second concurrent install, and the notion of being
 * *uninstalled* at all (the contract has no unbind, so `reset` drains through
 * it and then drops the store).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 *
 */
#include "unarch_xz_pool.h"

#include "ra8_check.h"
#include "ra8_imgdec_scratch.h"

/**
 * @brief The seam's published alignment may never exceed what now keeps it.
 * @details `unarch_xz_pool.h` promises ::k_unarch_xz_pool_align storage to
 *          the vendored decoder's `uint64_t`-bearing structs. That promise is
 *          now kept by the shared contract's rounding, so the two constants
 *          are pinned against each other rather than left to drift.
 */
static_assert((uint32_t)k_ra8_imgdec_scratch_align >= (uint32_t)k_unarch_xz_pool_align,
              "the shared scratch must align at least as strictly as the XZ pool promises");

/**
 * @var s_pool
 * @brief The installed arena, or an all-zero (uninstalled) scratch.
 * @details `s_pool.base == nullptr` is the uninstalled state: allocations are
 *          refused and ::unarch_xz_pool_used reports zero. Installed, every
 *          bump is served by ::ra8_imgdec_scratch_alloc, so the cursor
 *          invariant is the contract's, not this file's.
 * @note Module-private; mutate only through the install/reset API.
 * @warning Never modify directly -- outstanding pointers depend on it.
 * @since Version 0.1.0
 */
static ra8_imgdec_scratch_t s_pool = {};

ra8_err_t unarch_xz_pool_install(void* base, uint32_t len)
{
  static const char* const tag = "ra8_xz_pool";
  RA8_CHECK_NULL_PTR(base, tag, "install: null base");
  if (len == 0U) {
    return k_ra8_err_invalid_size;
  }
  const uintptr_t base_address = (uintptr_t)base;
  if ((base_address % (uintptr_t)k_unarch_xz_pool_align) != 0U) {
    return k_ra8_err_invalid_size;
  }
  if (s_pool.base != nullptr) {
    return k_ra8_err_busy;
  }
  return ra8_imgdec_scratch_init(&s_pool, base, (size_t)len);
}

void unarch_xz_pool_reset(void)
{
  ra8_imgdec_scratch_reset(&s_pool);
  s_pool.base = nullptr;
  s_pool.cap  = 0U;
}

void* unarch_xz_pool_alloc(uint32_t size)
{
  return ra8_imgdec_scratch_alloc(&s_pool, (size_t)size);
}

uint32_t unarch_xz_pool_used(void)
{
  return (uint32_t)s_pool.offset;
}
