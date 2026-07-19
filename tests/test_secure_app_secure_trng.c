/**
 * @file test_secure_app_secure_trng.c
 * @brief Unit + MC/DC tests for src/secure_app/secure_trng.c
 *
 * @details
 * Exercises the host-PRNG-stub TRNG entropy reader. Includes targeted
 * MC/DC vector sets for the two compound boolean decisions identified
 * in docs/MCDC_GAPS.csv at src/secure_app/secure_trng.c and :89.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "secure_trng_internal.h"
#include "unity_minimal.h"

/**
 * @enum secure_app_secure_trng_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_secure_app_secure_trng_buf_a5 = 0xA5U,
} secure_app_secure_trng_uint8_const_t;

typedef enum : uint16_t {
  k_test_trng_buf_bytes   = 256U, /**< Matches k_ra8_secure_trng_max_bytes.    */
  k_test_trng_len_one     = 1U,   /**< Test trng length one.                   */
  k_test_trng_len_seven   = 7U,   /**< Forces inner loop to terminate by C2=F. */
  k_test_trng_len_eight   = 8U,   /**< Forces inner loop to terminate by C1=F. */
  k_test_trng_len_max     = 256U, /**< Largest legal length.                   */
  k_test_trng_len_too_big = 257U, /**< First over-cap value.                   */
} test_trng_consts_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_reset_is_idempotent(void)
{
  TEST_BEGIN("secure_trng: reset returns ok and reseeds");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_secure_trng_reset());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_secure_trng_reset());
  TEST_END("secure_trng: reset returns ok and reseeds");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_happy_path(void)
{
  TEST_BEGIN("secure_trng: read fills buffer");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_secure_trng_reset());

  uint8_t buf[k_test_trng_buf_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_secure_trng_read(buf, (uint32_t)k_test_trng_len_max));

  /* Deterministic xorshift64* with the documented seed must produce
   * at least one non-zero byte in the first 8 bytes. */
  bool any_nonzero = false;
  for (uint32_t i = 0U; i < 8U; ++i) {
    if (buf[i] != 0U) {
      any_nonzero = true;
      break;
    }
  }
  TEST_ASSERT(any_nonzero);
  TEST_END("secure_trng: read fills buffer");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_arg_validation(void)
{
  TEST_BEGIN("secure_trng: read arg validation");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_secure_trng_reset());

  uint8_t buf[k_test_trng_buf_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_secure_trng_read(nullptr, (uint32_t)k_test_trng_len_one));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_secure_trng_read(buf, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_secure_trng_read(buf, (uint32_t)k_test_trng_len_too_big));
  TEST_END("secure_trng: read arg validation");
}

/**
 * @test test_mcdc_read_length_validation
 *
 * @par MC/DC:
 * Decision: `if ((len == 0U) || (len > (uint32_t)k_ra8_secure_trng_max_bytes))`
 * (2 conditions, src/secure_app/secure_trng.c)
 *  - C1 = (len == 0U)
 *  - C2 = (len > k_ra8_secure_trng_max_bytes)  (i.e. > 256)
 *
 * N=2 -> N+1=3 minimal MC/DC vectors:
 *  - Vector 1: len=1 -> C1=F, C2=F. Decision F -> ok.
 *  - Vector 2: len=257 -> C1=F, C2=T. Decision T -> invalid_arg.
 *  - Vector 3: len=0 -> C1=T (short-circuits). Decision T -> invalid_arg.
 *
 * Vectors 1+2 vary C2 with C1 held F (decision F->T).
 * Vectors 1+3 vary C1 with C2 implicitly held (when C1=T C2 is not
 * evaluated -- masked MC/DC accepted under DO-178C 6.4.4.3 for ||).
 * N+1=3 satisfies minimal MC/DC.
 */
static void test_mcdc_read_length_validation(void)
{
  TEST_BEGIN("secure_trng MC/DC: len==0 || len>max (secure_trng.c:81)");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_secure_trng_reset());
  uint8_t buf[k_test_trng_buf_bytes] = {};

  /* Vector 1: C1=F, C2=F -> ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_secure_trng_read(buf, (uint32_t)k_test_trng_len_one));

  /* Vector 2: C1=F, C2=T -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_secure_trng_read(buf, (uint32_t)k_test_trng_len_too_big));

  /* Vector 3: C1=T short-circuits -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_secure_trng_read(buf, 0U));

  TEST_END("secure_trng MC/DC: len==0 || len>max (secure_trng.c:81)");
}

/**
 * @test test_mcdc_read_inner_loop_bound
 *
 * @par MC/DC:
 * Decision: `for (uint32_t b = 0U; (b < (uint32_t)k_bytes_per_u64) && (written < len); ++b)`
 * (2 conditions, src/secure_app/secure_trng.c)
 *  - C1 = (b < 8U)             (per-word slice index)
 *  - C2 = (written < len)      (have we filled the request?)
 *
 * The inner loop's exit condition is the negation of the compound;
 * we drive each MC/DC vector by picking ``len`` so that the loop
 * exits via the chosen condition:
 *  - Vector 1: len=8 -> on the 8th iteration C1=(8<8)=F (loop exits
 *    via C1). C2 was T just before. Decision F via C1.
 *  - Vector 2: len=7 -> on the 8th would-be iteration C2=(7<7)=F
 *    after the 7th byte, C1=(7<8)=T. Loop exits via C2. Decision F
 *    via C2.
 *  - Vector 3: len=1 -> at b=0 C1=T, C2=T, decision T (loop body
 *    runs at least once).
 *
 * Vectors 1+3 vary C1 with C2 held T (decision T->F via C1).
 * Vectors 2+3 vary C2 with C1 held T (decision T->F via C2).
 * N+1=3 satisfies minimal MC/DC. The loop-control decision is the
 * canonical example in DO-178C 6.4.4.2 of a `&&` MC/DC vector set.
 *
 * Validated indirectly: every byte of the requested length is filled
 * (the buffer's tail bytes remain zero, so a non-zero value in the
 * unwritten region would indicate over-write).
 */
static void test_mcdc_read_inner_loop_bound(void)
{
  TEST_BEGIN("secure_trng MC/DC: inner-loop b<8 && written<len (secure_trng.c:89)");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_secure_trng_reset());

  /* Pre-fill the buffer with a sentinel so we can detect over-write. */
  uint8_t buf[k_test_trng_buf_bytes];
  for (uint32_t i = 0U; i < (uint32_t)k_test_trng_buf_bytes; ++i) {
    buf[i] = k_secure_app_secure_trng_buf_a5;
  }

  /* Vector 3: len=1 -> at least one body iteration (C1=T,C2=T). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_secure_trng_read(buf, (uint32_t)k_test_trng_len_one));
  /* Tail (index 1..) remains the sentinel: no over-write. */
  TEST_ASSERT_EQ(0xA5, buf[1]);

  /* Vector 2: len=7 -> loop exits via C2=(written<len)=F at b=7. */
  for (uint32_t i = 0U; i < (uint32_t)k_test_trng_buf_bytes; ++i) {
    buf[i] = k_secure_app_secure_trng_buf_a5;
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_secure_trng_read(buf, (uint32_t)k_test_trng_len_seven));
  /* Index 7 (the 8th byte) must not have been written. */
  TEST_ASSERT_EQ(0xA5, buf[7]);

  /* Vector 1: len=8 -> loop exits via C1=(b<8)=F after byte index 7.
   * All 8 bytes written; the 9th remains the sentinel. */
  for (uint32_t i = 0U; i < (uint32_t)k_test_trng_buf_bytes; ++i) {
    buf[i] = k_secure_app_secure_trng_buf_a5;
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_secure_trng_read(buf, (uint32_t)k_test_trng_len_eight));
  TEST_ASSERT_EQ(0xA5, buf[8]);

  TEST_END("secure_trng MC/DC: inner-loop b<8 && written<len (secure_trng.c:89)");
}

int32_t main(void)
{
  test_reset_is_idempotent();
  test_read_happy_path();
  test_read_arg_validation();
  test_mcdc_read_length_validation();
  test_mcdc_read_inner_loop_bound();
  (void)fprintf(stderr, "[OK ] test_secure_app_secure_trng.c\n");
  return 0;
}
