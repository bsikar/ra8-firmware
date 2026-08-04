/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_unarch_xz_pool.h
 * @brief Caller-owned bump arena backing xz-embedded's allocator seam.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * The vendored xz-embedded decoder (SOUP, `libs/third_party/xz_embedded/`)
 * allocates its state through the kernel-style `kmalloc` / `vmalloc` seam
 * that its porting header (`xz_config.h`, first-party) maps onto this pool.
 * This firmware traps the allocator (`_sbrk`, NASA P10 Rule 3), so the pool
 * is a deterministic bump arena over a **caller-provided** scratch buffer:
 * the XZ wrapper installs the caller's buffer before `xz_dec_init`, the
 * decoder's fixed set of init-time allocations bump from it, and the wrapper
 * resets the pool when the decode ends. Frees are no-ops (arena semantics --
 * xz-embedded only frees at `xz_dec_end`, right before the wrapper resets).
 *
 * The pool is intentionally module-static and single-client: the e-reader's
 * single-threaded content loop opens one XZ stream at a time, and the
 * wrapper install/reset pairs are strictly nested. A second concurrent
 * install is refused fail-closed.
 *
 * @note Not thread-safe; the single-threaded reader loop serialises access.
 *
 * @see ra8_unarch_xz.h  The bounded XZ decode wrapper that installs this pool.
 * @see xz_config.h      The porting header mapping kmalloc/vmalloc here.
 *
 * @since Version 0.1.0
 */
#pragma once

#include <stdint.h>

#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum ra8_unarch_xz_pool_dims_t
 * @brief Alignment and sizing constants for the XZ bump arena.
 * @details Every allocation is rounded up to ::k_ra8_unarch_xz_pool_align so
 *          the decoder's structs (which hold `uint64_t` fields) are always
 *          correctly aligned regardless of request order.
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_unarch_xz_pool_align = 8U, /**< Bump-allocation alignment, bytes. */
} ra8_unarch_xz_pool_dims_t;

/**
 * @brief Install a caller-owned scratch buffer as the XZ allocation arena.
 *
 * @details Binds `[base, base + len)` as the arena xz-embedded's `kmalloc` /
 *          `vmalloc` calls bump from. Refused when an arena is already
 *          installed (strictly nested wrapper usage) so two concurrent XZ
 *          decodes cannot silently share state.
 *
 * @param[in] base Scratch buffer start (non-NULL, 8-byte aligned).
 * @param[in] len  Scratch buffer length in bytes (> 0).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Arena installed; allocations may begin.
 * @retval k_ra8_err_null_ptr     @p base was NULL.
 * @retval k_ra8_err_invalid_size @p len was 0 or @p base was misaligned.
 * @retval k_ra8_err_busy         An arena is already installed.
 *
 * @pre @p base is aligned to ::k_ra8_unarch_xz_pool_align.
 * @pre No other XZ decode is in flight (single-threaded reader loop).
 * @post On k_ra8_ok the pool serves ::ra8_unarch_xz_pool_alloc calls.
 * @post On any error the pool state is unchanged.
 *
 * @note Not thread-safe.
 * @see ra8_unarch_xz_pool_reset()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_unarch_xz_pool_install(void* base, uint32_t len);

/**
 * @brief Release the installed arena (invalidates every pool allocation).
 *
 * @details Unbinds the arena and zeroes the bump cursor. Idempotent: calling
 *          with no arena installed is a no-op, so teardown paths may call it
 *          unconditionally.
 *
 * @pre Any pointers previously handed out are dead after this returns.
 * @pre The owning decode (xz_dec_end) has finished with them.
 * @post The pool is uninstalled; ::ra8_unarch_xz_pool_alloc returns NULL.
 * @post A fresh ::ra8_unarch_xz_pool_install may follow.
 *
 * @note Not thread-safe.
 * @see ra8_unarch_xz_pool_install()
 * @since Version 0.1.0
 */
void ra8_unarch_xz_pool_reset(void);

/**
 * @brief Bump-allocate @p size bytes from the installed arena.
 *
 * @details The `kmalloc` / `vmalloc` target for the vendored decoder (via
 *          the first-party `xz_config.h`). Returns 8-aligned storage carved
 *          from the installed scratch, or NULL when no arena is installed,
 *          the request is zero, or the remaining space is too small --
 *          xz-embedded treats a NULL return as allocation failure and
 *          aborts its init cleanly (the wrapper maps that to a bounded
 *          `ra8_err_t`), so exhaustion fails closed.
 *
 * @param[in] size Bytes requested (> 0).
 *
 * @return Pointer to @p size bytes of 8-aligned storage, or NULL.
 * @retval NULL No arena installed, zero @p size, or arena exhausted.
 *
 * @pre ::ra8_unarch_xz_pool_install succeeded (else NULL is returned).
 * @pre @p size is non-zero (else NULL is returned).
 * @post A non-NULL result stays valid until ::ra8_unarch_xz_pool_reset.
 * @post The bump cursor advanced by the aligned request size.
 *
 * @note Not thread-safe. Frees are no-ops (arena semantics).
 * @see ra8_unarch_xz_pool_used()
 * @since Version 0.1.0
 */
[[nodiscard]] void* ra8_unarch_xz_pool_alloc(uint32_t size);

/**
 * @brief Bytes currently bump-allocated from the installed arena.
 *
 * @details Introspection for tests and RAM-high-water assertions: the exact
 *          number of scratch bytes the current decode has consumed. Zero
 *          when no arena is installed.
 *
 * @return Consumed bytes (aligned sum of every allocation so far).
 * @retval 0 No arena installed or nothing allocated yet.
 *
 * @pre None -- safe to call in any pool state.
 * @pre No allocation races (single-threaded reader loop).
 * @post No state is modified (pure read).
 * @post The result is monotonic between install and reset.
 *
 * @note Not thread-safe.
 * @see ra8_unarch_xz_pool_alloc()
 * @since Version 0.1.0
 */
[[nodiscard]] uint32_t ra8_unarch_xz_pool_used(void);

#ifdef __cplusplus
}
#endif
