/**
 * @file unarch_xz_pool.c
 * @brief Bump-arena implementation behind xz-embedded's allocator seam.
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * See `unarch_xz_pool.h` for the contract. The arena is three module
 * statics (base, length, cursor); install/reset pairs are strictly nested
 * and the single-threaded reader loop is the only client. Alignment is
 * enforced on install (caller buffer) and on every bump (request rounding),
 * so the vendored decoder's `uint64_t`-bearing structs are always aligned.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 *
 */
#include "unarch_xz_pool.h"

#include "ra8_check.h"

/**
 * @var s_pool_base
 * @brief Start of the installed arena (NULL when uninstalled).
 * @details Set by ::unarch_xz_pool_install, cleared by
 *          ::unarch_xz_pool_reset; every allocation is carved from
 *          `[s_pool_base, s_pool_base + s_pool_len)`.
 * @note Module-private; mutate only through the install/reset API.
 * @warning Never modify directly -- the cursor invariant depends on it.
 * @since Version 0.1.0
 */
static uint8_t* s_pool_base = nullptr;

/**
 * @var s_pool_len
 * @brief Length of the installed arena in bytes (0 when uninstalled).
 * @details Upper bound for the bump cursor ::s_pool_off.
 * @note Module-private; mutate only through the install/reset API.
 * @warning Never modify directly -- the cursor invariant depends on it.
 * @since Version 0.1.0
 */
static uint32_t s_pool_len = 0U;

/**
 * @var s_pool_off
 * @brief Bump cursor: bytes consumed from the installed arena.
 * @details Invariant `s_pool_off <= s_pool_len`; advances only in
 *          ::unarch_xz_pool_alloc by 8-aligned amounts.
 * @note Module-private; mutate only through the pool API.
 * @warning Never modify directly -- outstanding pointers depend on it.
 * @since Version 0.1.0
 */
static uint32_t s_pool_off = 0U;

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
  if (s_pool_base != nullptr) {
    return k_ra8_err_busy;
  }
  s_pool_base = (uint8_t*)base;
  s_pool_len  = len;
  s_pool_off  = 0U;
  return k_ra8_ok;
}

void unarch_xz_pool_reset(void)
{
  s_pool_base = nullptr;
  s_pool_len  = 0U;
  s_pool_off  = 0U;
}

void* unarch_xz_pool_alloc(uint32_t size)
{
  if (s_pool_base == nullptr) {
    return nullptr;
  }
  if (size == 0U) {
    return nullptr;
  }
  const uint32_t align = (uint32_t)k_unarch_xz_pool_align;
  if (size > (UINT32_MAX - (align - 1U))) {
    return nullptr; /* rounding would wrap: refuse */
  }
  const uint32_t need = (size + (align - 1U)) & ~(align - 1U);
  if (need > (s_pool_len - s_pool_off)) {
    return nullptr; /* arena exhausted: xz-embedded aborts init cleanly */
  }
  void* const p = &s_pool_base[s_pool_off];
  s_pool_off += need;
  return p;
}

uint32_t unarch_xz_pool_used(void)
{
  if (s_pool_base == nullptr) {
    return 0U;
  }
  return s_pool_off;
}
