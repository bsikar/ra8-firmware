/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_secure.h
 * @brief Secure-comparison primitives for the crypto / secure-boot paths
 * @ingroup grp_core
 *
 * @par Tag
 * [Ring 1 / Core] {World: S}
 *
 * @details
 * Data-dependent early-out in a byte comparison of a MAC, authentication tag,
 * or image digest leaks -- through timing -- how many leading bytes matched,
 * which an attacker turns into a byte-at-a-time forgery of the compared value.
 * The plain library ``memcmp`` returns as soon as it finds a mismatch and is
 * therefore unsafe on any security-verdict comparison. ::ra8_ct_equal compares
 * the full length unconditionally so its execution time does not depend on where
 * (or whether) the buffers differ.
 *
 * Route every MAC / tag / digest / key equality check through ::ra8_ct_equal;
 * keep plain ``memcmp`` only for non-secret, non-verdict comparisons.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/**
 * @brief Constant-time equality of two byte buffers.
 *
 * @details
 * OR-accumulates the per-byte XOR difference across all ``len`` bytes with no
 * early-out, then reports equality from the accumulator. The number of byte
 * comparisons -- and thus the timing -- depends only on ``len``, never on the
 * contents, so a caller comparing a secret (MAC / tag / digest / key) against
 * an attacker-supplied value does not leak how many leading bytes matched.
 *
 * @param[in] a   First buffer. Must be non-NULL.
 * @param[in] b   Second buffer. Must be non-NULL.
 * @param[in] len Number of bytes to compare.
 *
 * @return Whether the two buffers are byte-for-byte equal over ``len`` bytes.
 * @retval true  All ``len`` bytes are equal (vacuously true when ``len == 0``).
 * @retval false The buffers differ, or either pointer is NULL.
 *
 * @pre ``a`` is non-NULL.
 * @pre ``b`` is non-NULL.
 * @post Neither buffer is modified.
 * @post Execution time is independent of the buffer contents (constant-time).
 *
 * @note Use this -- never plain ``memcmp`` -- for any comparison whose result is
 *       a security verdict or whose operands are secret.
 * @note Thread-safe: reads only, no shared state.
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 2: the compare loop is bounded by the caller-supplied ``len``.
 * - Rule 5: 2 preconditions (both pointers non-NULL), 2 postconditions.
 */
[[nodiscard]] bool ra8_ct_equal(const void* a, const void* b, size_t len);

/**
 * @brief Securely zero a buffer such that the write cannot be optimised away.
 *
 * @details
 * Overwrites ``len`` bytes at ``ptr`` with zero through a ``volatile`` pointer.
 * A plain ``memset`` immediately before a buffer leaves scope is a dead store the
 * optimiser is free to delete, leaving secret key / MAC / digest material in
 * stack or static memory; the volatile access is an observable side effect the
 * compiler must preserve. Use this to scrub any transient that held a secret.
 *
 * @param[out] ptr Buffer to zero. A NULL pointer is a no-op.
 * @param[in]  len Number of bytes to zero. Zero length is a no-op.
 *
 * @return void.
 *
 * @pre ``ptr`` is NULL or addresses at least ``len`` writable bytes.
 * @pre ``len`` is the size of the secret region, in bytes.
 * @post On return, the first ``len`` bytes at a non-NULL ``ptr`` are zero.
 * @post The zeroing store is not elided even if ``ptr`` is never read again.
 *
 * @note Not a timing-safe operation itself; it exists to erase, not compare.
 * @note Thread-safe only with respect to distinct buffers.
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 2: the zeroing loop is bounded by the caller-supplied ``len``.
 * - Rule 5: 2 preconditions (NULL guard, zero-length guard), 2 postconditions.
 */
void ra8_secure_memzero(void* ptr, size_t len);

#ifdef __cplusplus
}
#endif
