/**
 * @file secure_trng.c
 * @brief Secure-side TRNG read implementation (host PRNG stub)
 *
 * @par Tag
 * [Ring 5 / SECAPP] {World: S}
 *
 * @details
 * Host-friendly xorshift64* core. The real RSIP TRNG hookup lands
 * in ; the wrapper interface here is the part the NSC
 * veneer ``ra8_nsc_trng_read`` depends on, so the seam is committed
 * now.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "secure_trng_internal.h"

static const char* s_tag = "SECTRNG";

/*
 * Fail-closed stub-crypto gate (issue #180). The xorshift64* core below is a
 * DETERMINISTIC PRNG masquerading as a TRNG -- predictable "random" bytes were
 * the #1 severity finding in the security audit (predictable keys). It is only
 * safe under an off-target build or an explicitly-declared insecure dev/eval image.
 * A real production/HIL image (neither flag set) compiles the #else branch,
 * where every entry point hard-errors so predictable entropy can never be
 * drawn. scripts/checks/check_stub_crypto_guarded.py enforces that this guard
 * stays wrapped around the insecure body.
 */
#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)

/**
 * @brief xorshift64* tuning constants (named to satisfy the
 *        readability-magic-numbers clang-tidy check).
 *
 * Values are the standard Marsaglia xorshift64* triple
 * (12, 25, 27) plus the multiplier 0x2545F4914F6CDD1D.
 * The seed is the golden-ratio constant 0x9E3779B97F4A7C15.
 */
typedef enum : uint64_t {
  k_xorshift_seed       = 0x9E3779B97F4A7C15ULL, /**< Initial state seed. */
  k_xorshift_multiplier = 0x2545F4914F6CDD1DULL, /**< Output multiplier.  */
} ra8_secure_trng_consts64_t;

typedef enum : uint8_t {
  k_xorshift_shift_a = 12U, /**< First xorshift shift width.  */
  k_xorshift_shift_b = 25U, /**< Second xorshift shift width. */
  k_xorshift_shift_c = 27U, /**< Third xorshift shift width.  */
  k_byte_bits        = 8U,  /**< Bits in one byte.            */
  k_bytes_per_u64    = 8U,  /**< Bytes per 64-bit word.       */
} ra8_secure_trng_consts8_t;

typedef enum : uint32_t {
  k_byte_mask = 0xFFU, /**< Low-byte extraction mask. */
} ra8_secure_trng_consts32_t;

/**
 * @var s_state
 * @brief 64-bit xorshift64* state.
 *
 * @details Updated on every call to ``ra8_secure_trng_read``.
 * @warning Direct modification outside this TU is forbidden.
 * @since 0.1.0
 */
static uint64_t s_state = k_xorshift_seed;

/**
 * @brief Advance the xorshift64* state and return one 64-bit word.
 *
 * @details
 * Standard Marsaglia (12, 25, 27) xorshift triple followed by the
 * 0x2545F4914F6CDD1D multiplier. Used to fan random bytes out into
 * the caller buffer in 8-byte chunks.
 *
 * @return Next pseudo-random 64-bit word.
 * @retval Any uint64_t value; output cycle length 2^64 - 1.
 *
 * @pre ``s_state`` has been seeded by ::ra8_secure_trng_reset or boot default.
 * @pre Caller is in the secure-side dispatch path.
 * @post ``s_state`` is advanced to the next state in the sequence.
 * @post No other state is modified.
 *
 * @note Not thread-safe; secure-side serial dispatch only.
 * @since 0.1.0
 */
static uint64_t internal_xorshift64(void)
{
  uint64_t x = s_state;
  x ^= x >> k_xorshift_shift_a;
  x ^= x << k_xorshift_shift_b;
  x ^= x >> k_xorshift_shift_c;
  s_state = x;
  return x * k_xorshift_multiplier;
}

/**
 * @brief Re-seed the xorshift64* state to the boot default.
 *
 * @details
 * Used between unit-test scenarios so reads are reproducible.
 * The real RSIP TRNG hookup will replace this in .
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always; the operation cannot fail.
 *
 * @pre Caller is in the secure-side init/test path.
 * @pre No NS-side TRNG read is in flight.
 * @post ``s_state == k_xorshift_seed``.
 * @post No other state is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_secure_trng_reset(void)
{
  s_state = k_xorshift_seed;
  return k_ra8_ok;
}

/**
 * @brief Implementation of ra8_secure_trng_read (see header for the
 *        public contract).
 * @details Drives ::internal_xorshift64 in a loop, splitting each
 *          64-bit word into eight bytes and writing them to ``out``
 *          until ``len`` bytes have been emitted. Loop bound is the
 *          per-call cap so the function is NASA Rule 2 compliant.
 * @param[out] out Destination buffer.
 * @param[in]  len Number of bytes to emit; 1..k_ra8_secure_trng_max_bytes.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Buffer filled.
 * @retval k_ra8_err_null_ptr       ``out`` was NULL.
 * @retval k_ra8_err_invalid_arg    ``len`` zero or above the per-call cap.
 * @pre ``out`` is non-NULL and spans at least ``len`` bytes.
 * @pre ``len`` is within the documented per-call cap.
 * @post On success, ``out[0..len-1]`` is filled with PRNG output.
 * @post ``s_state`` is advanced by ceil(len/8) iterations.
 * @note Not thread-safe; secure-side serial dispatch only.
 * @since 0.1.0
 */
ra8_err_t ra8_secure_trng_read(uint8_t* out, uint32_t len)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "trng_read: out");
  if ((len == 0U) || (len > (uint32_t)k_ra8_secure_trng_max_bytes)) {
    return k_ra8_err_invalid_arg;
  }
  uint32_t written = 0U;
  /* Loop bound is the per-call cap (NASA Rule 2). */
  while (written < len) {
    const uint64_t word = internal_xorshift64();
    /* Inner loop bound is constant 8. */
    for (uint32_t b = 0U; (b < (uint32_t)k_bytes_per_u64) && (written < len); ++b) {
      out[written] = (uint8_t)((word >> (b * (uint32_t)k_byte_bits)) & k_byte_mask);
      ++written;
    }
  }
  return k_ra8_ok;
}

#else /* production build: neither RA8_INSECURE_STUB_CRYPTO nor RA8_OFF_TARGET */

/*
 * Fail-closed production variant. Without a real RSIP TRNG backend the
 * deterministic PRNG above must never run, so both entry points return a hard
 * error (never k_ra8_ok). A production image that forgot to provide real
 * entropy therefore cannot silently draw predictable "random" bytes.
 */

ra8_err_t ra8_secure_trng_reset(void)
{
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_secure_trng_read(uint8_t* out, uint32_t len)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "trng_read: out");
  (void)len;
  return k_ra8_err_not_supported;
}

#endif /* RA8_INSECURE_STUB_CRYPTO || RA8_OFF_TARGET */
