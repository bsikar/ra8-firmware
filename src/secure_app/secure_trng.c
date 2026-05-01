/**
 * @file secure_trng.c
 * @brief Secure-side TRNG read implementation (host PRNG stub)
 *
 * @par Tag
 * [Ring 5 / SECAPP] {World: S}
 *
 * @details
 * Host-friendly xorshift64* core. The real RSIP TRNG hookup lands
 * in Wave 14; the wrapper interface here is the part the NSC
 * veneer ``ra_nsc_trng_read`` depends on, so the seam is committed
 * now.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "secure_trng.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"

static const char* s_tag = "SECTRNG";

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
} ra_secure_trng_consts64_t;

typedef enum : uint8_t {
  k_xorshift_shift_a = 12U, /**< First xorshift shift width.  */
  k_xorshift_shift_b = 25U, /**< Second xorshift shift width. */
  k_xorshift_shift_c = 27U, /**< Third xorshift shift width.  */
  k_byte_bits        = 8U,  /**< Bits in one byte.            */
  k_bytes_per_u64    = 8U,  /**< Bytes per 64-bit word.       */
} ra_secure_trng_consts8_t;

typedef enum : uint32_t {
  k_byte_mask = 0xFFU, /**< Low-byte extraction mask. */
} ra_secure_trng_consts32_t;

/**
 * @var s_state
 * @brief 64-bit xorshift64* state.
 *
 * @details Updated on every call to ``ra_secure_trng_read``.
 * @warning Direct modification outside this TU is forbidden.
 * @since 0.1.0
 */
static uint64_t s_state = (uint64_t)k_xorshift_seed;

static uint64_t internal_xorshift64(void)
{
  uint64_t x = s_state;
  x ^= x >> (uint64_t)k_xorshift_shift_a;
  x ^= x << (uint64_t)k_xorshift_shift_b;
  x ^= x >> (uint64_t)k_xorshift_shift_c;
  s_state = x;
  return x * (uint64_t)k_xorshift_multiplier;
}

ra_err_t ra_secure_trng_reset(void)
{
  s_state = (uint64_t)k_xorshift_seed;
  return k_ra_ok;
}

ra_err_t ra_secure_trng_read(uint8_t* out, uint32_t len)
{
  RA_CHECK_NULL_PTR(out, s_tag, "trng_read: out");
  if ((len == 0U) || (len > (uint32_t)k_ra_secure_trng_max_bytes)) {
    return k_ra_err_invalid_arg;
  }
  uint32_t written = 0U;
  /* Loop bound is the per-call cap (NASA Rule 2). */
  while (written < len) {
    const uint64_t word = internal_xorshift64();
    /* Inner loop bound is constant 8. */
    for (uint32_t b = 0U; (b < (uint32_t)k_bytes_per_u64) && (written < len); ++b) {
      out[written] = (uint8_t)((word >> (b * (uint32_t)k_byte_bits)) & (uint32_t)k_byte_mask);
      ++written;
    }
  }
  return k_ra_ok;
}
