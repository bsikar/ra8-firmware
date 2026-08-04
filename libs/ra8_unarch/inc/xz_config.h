/**
 * @file xz_config.h
 * @brief First-party porting header for the vendored xz-embedded decoder.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * xz-embedded (SOUP, `libs/third_party/xz_embedded/`) is written against the
 * Linux kernel environment; outside the kernel its private header includes a
 * platform-supplied `"xz_config.h"` for the allocator, byte-order, and
 * compiler shims. This is that platform header for ra8-firmware. It exists
 * so the vendored tree stays **byte-identical to upstream** (no local
 * patches; the SBOM records `modified: false`) while the decoder's
 * allocations route through the zero-heap bump arena in
 * `ra8_unarch_xz_pool.h` (NASA P10 Rule 3 -- this firmware traps `_sbrk`).
 *
 * The macro names below (`kmalloc`, `memeq`, `min_t`, ...) are not this
 * project's choice: they are the exact porting contract the SOUP sources
 * consume, so they cannot be renamed to project conventions. Every macro
 * here is the allowed "conditional compilation / porting seam" kind -- none
 * defines a bare integer constant.
 *
 * Feature selection (compile-time, decode-only):
 * - `XZ_DEC_PREALLOC` only: the zero-growth streaming mode the wrapper
 *   uses (decoder state + dictionary allocated once at init from the
 *   caller scratch). `XZ_DEC_DYNALLOC` is deliberately NOT defined: it
 *   would grow the dictionary on demand from hostile header values.
 *   `XZ_DEC_SINGLE` is likewise not defined: no consumer decodes from a
 *   fully-resident input buffer, and every enabled mode is dead weight.
 * - `XZ_USE_CRC64`: verify the CRC64 integrity check that `xz`(1) emits by
 *   default; without it every default-created `.xz` would be rejected or
 *   ride unverified.
 * - No `XZ_DEC_*` BCJ filter is enabled: e-reader content is data, not
 *   executables, so a stream using a BCJ filter is rejected fail-closed by
 *   the decoder (`XZ_OPTIONS_ERROR`).
 *
 * @note Include-path only for the vendored xz-embedded translation units and
 *       the first-party XZ wrapper; nothing else in the tree includes this.
 *
 * @see ra8_unarch_xz_pool.h The bump arena `kmalloc` / `vmalloc` map onto.
 * @see ra8_unarch_xz.h      The bounded decode wrapper over the SOUP.
 * @see docs/SOUP/xz_embedded.md Qualification record for the vendored tree.
 *
 * @since Version 0.1.0
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_unarch_xz_pool.h"

/* The porting-seam macro names below (kmalloc, memeq, min_t, get_le32, ...) are
 * fixed by the xz-embedded SOUP contract (see the @details above) and cannot be
 * renamed to project UPPER_CASE conventions without forking the vendored tree. */
/* NOLINTBEGIN(readability-identifier-naming) -- porting-seam names fixed by the xz-embedded SOUP contract. */

/**
 * @def XZ_DEC_PREALLOC
 * @brief Enable xz-embedded's preallocated-dictionary streaming mode.
 * @details Multi-call decoding with the decoder state and dictionary
 *          allocated once at init (from the caller scratch via the pool)
 *          and never grown. The only mode the `ra8_unarch_xz` wrapper
 *          uses; `XZ_DEC_SINGLE` / `XZ_DEC_DYNALLOC` stay disabled.
 * @note Build-configuration flag consumed by the SOUP sources.
 * @since Version 0.1.0
 */
#define XZ_DEC_PREALLOC

/**
 * @def XZ_USE_CRC64
 * @brief Verify CRC64 block/stream checks (the `xz`(1) default).
 * @details Compiles `xz_crc64.c` support in so default-created archives are
 *          integrity-verified instead of rejected as unsupported.
 * @note Build-configuration flag consumed by the SOUP sources.
 * @since Version 0.1.0
 */
#define XZ_USE_CRC64

#include "xz.h"

/**
 * @def kmalloc
 * @brief xz-embedded allocator seam -> zero-heap bump arena (state structs).
 * @details The `flags` argument is kernel GFP noise and is discarded. A NULL
 *          return makes the decoder abort its init cleanly, which the
 *          wrapper maps to a bounded `ra8_err_t` (fail-closed).
 * @note Name fixed by the SOUP porting contract; do not rename.
 * @since Version 0.1.0
 */
#define kmalloc(size, flags) ra8_unarch_xz_pool_alloc((uint32_t)(size))

/**
 * @def kfree
 * @brief xz-embedded free seam -- no-op under arena semantics.
 * @details The decoder frees only at `xz_dec_end`, immediately before the
 *          wrapper resets the whole arena, so individual frees are void.
 * @note Name fixed by the SOUP porting contract; do not rename.
 * @since Version 0.1.0
 */
#define kfree(ptr) ((void)(ptr))

/**
 * @def vmalloc
 * @brief xz-embedded large-allocation seam (dictionary) -> same bump arena.
 * @details Only the `XZ_PREALLOC` dictionary comes through here; its size is
 *          policy-bounded by the wrapper before init.
 * @note Name fixed by the SOUP porting contract; do not rename.
 * @since Version 0.1.0
 */
#define vmalloc(size) ra8_unarch_xz_pool_alloc((uint32_t)(size))

/**
 * @def vfree
 * @brief xz-embedded large-free seam -- no-op under arena semantics.
 * @details See @ref kfree; the arena reset reclaims everything at once.
 * @note Name fixed by the SOUP porting contract; do not rename.
 * @since Version 0.1.0
 */
#define vfree(ptr) ((void)(ptr))

/**
 * @def memeq
 * @brief Byte-equality shim the SOUP uses for magic/footer comparison.
 * @details Thin `memcmp == 0` wrapper (code de-duplication macro).
 * @note Name fixed by the SOUP porting contract; do not rename.
 * @since Version 0.1.0
 */
#define memeq(a, b, size) (memcmp((a), (b), (size)) == 0)

/**
 * @def memzero
 * @brief Zero-fill shim the SOUP uses to reset decoder state.
 * @details Thin `memset(0)` wrapper (code de-duplication macro).
 * @note Name fixed by the SOUP porting contract; do not rename.
 * @since Version 0.1.0
 */
#define memzero(buf, size) memset((buf), 0, (size))

/**
 * @def min
 * @brief Untyped minimum shim used by the SOUP's buffer clamping.
 * @details Evaluates to the smaller of @p x and @p y, exactly as the kernel
 *          macro the sources were written against (both operands are always
 *          same-typed at the call sites).
 * @note Name fixed by the SOUP porting contract; do not rename.
 * @since Version 0.1.0
 */
#ifndef min
/** @brief Minimum. */
#define min(x, y) (((x) < (y)) ? (x) : (y))
#endif

/**
 * @def min_t
 * @brief Typed minimum shim used by the SOUP's buffer clamping.
 * @details Evaluates to the smaller of @p x and @p y after casting both to
 *          @p type, exactly as the kernel macro the sources were written
 *          against.
 * @note Name fixed by the SOUP porting contract; do not rename.
 * @since Version 0.1.0
 */
#define min_t(type, x, y) (((type)(x) < (type)(y)) ? (type)(x) : (type)(y))

/**
 * @def fallthrough
 * @brief Explicit switch-case fall-through marker for the SOUP sources.
 * @details Maps to the C23 attribute so `-Wimplicit-fallthrough` stays
 *          meaningful inside the vendored decoder.
 * @note Name fixed by the SOUP porting contract; do not rename.
 * @since Version 0.1.0
 */
#ifndef fallthrough
/** @brief Fallthrough. */
#define fallthrough [[fallthrough]]
#endif

/**
 * @def __always_inline
 * @brief Kernel-style force-inline marker used by the SOUP hot paths.
 * @details Kept as a plain attribute-inline mapping; the decoder is fast
 *          enough on the M85 without further tuning.
 * @note Name fixed by the SOUP porting contract; do not rename.
 * @since Version 0.1.0
 */
/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) -- the
 * xz_embedded SOUP sources spell this marker `__always_inline`; the name is
 * fixed by the porting contract and renaming it would fork the vendored tree. */
#ifndef __always_inline
/** @brief Always inline. */
#define __always_inline inline __attribute__((__always_inline__)) /* ATTR-OK: SOUP porting glue */
#endif
/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */

/**
 * @enum ra8_xz_cfg_shift_t
 * @brief Byte-lane shift distances for the little-endian load shim.
 * @details Named per the no-magic-numbers rule; used only by
 *          ::ra8_xz_cfg_get_unaligned_le32.
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_xz_cfg_shift_b1 = 8U,  /**< Second byte lane shift. */
  k_ra8_xz_cfg_shift_b2 = 16U, /**< Third byte lane shift.  */
  k_ra8_xz_cfg_shift_b3 = 24U, /**< Fourth byte lane shift. */
} ra8_xz_cfg_shift_t;

/**
 * @brief Load an unaligned little-endian 32-bit value byte by byte.
 * @details Portable (alignment- and endian-safe) implementation of the
 *          kernel helper the SOUP calls for CRC and header fields.
 * @param[in] buf Source bytes (>= 4 readable).
 * @return The 32-bit value assembled little-endian from `buf[0..3]`.
 * @retval uint32_t Any value; pure assembly of the four input bytes.
 * @pre @p buf addresses at least four readable bytes.
 * @pre No alignment requirement on @p buf.
 * @post No state is modified (pure read).
 * @post The result depends only on the four input bytes.
 * @note Thread-safe: pure read.
 * @since Version 0.1.0
 */
static inline uint32_t ra8_xz_cfg_get_unaligned_le32(const uint8_t* buf)
{
  return (uint32_t)buf[0] | ((uint32_t)buf[1] << (uint8_t)k_ra8_xz_cfg_shift_b1) |
         ((uint32_t)buf[2] << (uint8_t)k_ra8_xz_cfg_shift_b2) |
         ((uint32_t)buf[3] << (uint8_t)k_ra8_xz_cfg_shift_b3);
}

/**
 * @def get_unaligned_le32
 * @brief SOUP porting alias onto ::ra8_xz_cfg_get_unaligned_le32.
 * @details Kernel-contract name for the unaligned LE32 load.
 * @note Name fixed by the SOUP porting contract; do not rename.
 * @since Version 0.1.0
 */
#define get_unaligned_le32(buf) ra8_xz_cfg_get_unaligned_le32(buf)

/**
 * @def get_le32
 * @brief SOUP porting alias: aligned LE32 load via the unaligned helper.
 * @details Upstream uses the generic unaligned load for aligned accesses
 *          too; this keeps that behaviour.
 * @note Name fixed by the SOUP porting contract; do not rename.
 * @since Version 0.1.0
 */
#define get_le32(buf) ra8_xz_cfg_get_unaligned_le32(buf)
/* NOLINTEND(readability-identifier-naming) */
