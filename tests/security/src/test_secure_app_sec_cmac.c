/**
 * @file test_secure_app_sec_cmac.c
 * @brief KAT + MC/DC tests for libs/ra8_secure_app/src/sec_cmac.c (AES-CMAC seam)
 *
 * @details
 * Pins the in-tree AES-CMAC reference backend (the default host build; the
 * ARM firmware routes the same API through TF-PSA-Crypto ``psa_mac_*``) to the
 * published NIST SP 800-38B known-answer vectors for AES-128 and AES-256, then
 * exercises the verify verdict (happy path, one-byte tamper, truncated tag)
 * with the MC/DC vector set for the compound decision in ``priv_ra8_sec_cmac_verify``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "sec_cmac_internal.h"
#include "unity_minimal.h"

/** @brief Message and tag sizes used by the SP 800-38B vectors. */
typedef enum : uint16_t {
  k_kat_len_0     = 0U,  /**< Empty message.                */
  k_kat_len_16    = 16U, /**< One full AES block.           */
  k_kat_len_40    = 40U, /**< 2.5 blocks (partial last).    */
  k_kat_len_64    = 64U, /**< Four full blocks.             */
  k_kat_flip_idx  = 3U,  /**< Byte index toggled to tamper. */
  k_kat_short_len = 15U, /**< Truncated (invalid) tag len.  */
} test_cmac_sizes_t;

/**
 * @brief NIST SP 800-38B example key: AES-128 (2b7e1516...).
 */
static const uint8_t s_key128[k_ra8_sec_cmac_key_128] = {
  0x2b,
  0x7e,
  0x15,
  0x16,
  0x28,
  0xae,
  0xd2,
  0xa6,
  0xab,
  0xf7,
  0x15,
  0x88,
  0x09,
  0xcf,
  0x4f,
  0x3c,
};

/**
 * @brief NIST SP 800-38B example key: AES-256 (603deb10...0914dff4).
 */
static const uint8_t s_key256[k_ra8_sec_cmac_key_256] = {
  0x60, 0x3d, 0xeb, 0x10, 0x15, 0xca, 0x71, 0xbe, 0x2b, 0x73, 0xae, 0xf0, 0x85, 0x7d, 0x77, 0x81,
  0x1f, 0x35, 0x2c, 0x07, 0x3b, 0x61, 0x08, 0xd7, 0x2d, 0x98, 0x10, 0xa3, 0x09, 0x14, 0xdf, 0xf4,
};

/**
 * @brief NIST SP 800-38B example message (four blocks of the standard text).
 */
static const uint8_t s_msg[k_kat_len_64] = {
  0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
  0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
  0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11, 0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef,
  0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17, 0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10,
};

/* SP 800-38B AES-128 expected tags for Mlen = 0, 16, 40, 64. */
static const uint8_t s_tag128_0[k_ra8_sec_cmac_tag_bytes] = {
  0xbb,
  0x1d,
  0x69,
  0x29,
  0xe9,
  0x59,
  0x37,
  0x28,
  0x7f,
  0xa3,
  0x7d,
  0x12,
  0x9b,
  0x75,
  0x67,
  0x46,
};
static const uint8_t s_tag128_16[k_ra8_sec_cmac_tag_bytes] = {
  0x07,
  0x0a,
  0x16,
  0xb4,
  0x6b,
  0x4d,
  0x41,
  0x44,
  0xf7,
  0x9b,
  0xdd,
  0x9d,
  0xd0,
  0x4a,
  0x28,
  0x7c,
};
static const uint8_t s_tag128_40[k_ra8_sec_cmac_tag_bytes] = {
  0xdf,
  0xa6,
  0x67,
  0x47,
  0xde,
  0x9a,
  0xe6,
  0x30,
  0x30,
  0xca,
  0x32,
  0x61,
  0x14,
  0x97,
  0xc8,
  0x27,
};
static const uint8_t s_tag128_64[k_ra8_sec_cmac_tag_bytes] = {
  0x51,
  0xf0,
  0xbe,
  0xbf,
  0x7e,
  0x3b,
  0x9d,
  0x92,
  0xfc,
  0x49,
  0x74,
  0x17,
  0x79,
  0x36,
  0x3c,
  0xfe,
};

/* SP 800-38B AES-256 expected tags for Mlen = 0, 16, 40, 64. */
static const uint8_t s_tag256_0[k_ra8_sec_cmac_tag_bytes] = {
  0x02,
  0x89,
  0x62,
  0xf6,
  0x1b,
  0x7b,
  0xf8,
  0x9e,
  0xfc,
  0x6b,
  0x55,
  0x1f,
  0x46,
  0x67,
  0xd9,
  0x83,
};
static const uint8_t s_tag256_16[k_ra8_sec_cmac_tag_bytes] = {
  0x28,
  0xa7,
  0x02,
  0x3f,
  0x45,
  0x2e,
  0x8f,
  0x82,
  0xbd,
  0x4b,
  0xf2,
  0x8d,
  0x8c,
  0x37,
  0xc3,
  0x5c,
};
static const uint8_t s_tag256_40[k_ra8_sec_cmac_tag_bytes] = {
  0xaa,
  0xf3,
  0xd8,
  0xf1,
  0xde,
  0x56,
  0x40,
  0xc2,
  0x32,
  0xf5,
  0xb1,
  0x69,
  0xb9,
  0xc9,
  0x11,
  0xe6,
};
static const uint8_t s_tag256_64[k_ra8_sec_cmac_tag_bytes] = {
  0xe1,
  0x99,
  0x21,
  0x90,
  0x54,
  0x9f,
  0x6e,
  0xd5,
  0x69,
  0x6a,
  0x2c,
  0x05,
  0x6c,
  0x31,
  0x54,
  0x10,
};

static void
test_cmac_one_kat(const uint8_t* key, uint16_t key_len, uint32_t msg_len, const uint8_t* expected)
{
  uint8_t mac[k_ra8_sec_cmac_tag_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_ok, priv_ra8_sec_cmac_compute(key, key_len, s_msg, msg_len, mac));
  TEST_ASSERT_EQ(0, memcmp(mac, expected, (size_t)k_ra8_sec_cmac_tag_bytes));
  /* The same tag must verify. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    priv_ra8_sec_cmac_verify(key, key_len, s_msg, msg_len, mac, k_ra8_sec_cmac_tag_bytes));
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it pins compute+verify to the
 * published NIST SP 800-38B vectors.)
 */
static void test_cmac_kat_vectors(void)
{
  TEST_BEGIN("sec_cmac: NIST SP 800-38B AES-128 + AES-256 known-answer tests");
  test_cmac_one_kat(s_key128, k_ra8_sec_cmac_key_128, k_kat_len_0, s_tag128_0);
  test_cmac_one_kat(s_key128, k_ra8_sec_cmac_key_128, k_kat_len_16, s_tag128_16);
  test_cmac_one_kat(s_key128, k_ra8_sec_cmac_key_128, k_kat_len_40, s_tag128_40);
  test_cmac_one_kat(s_key128, k_ra8_sec_cmac_key_128, k_kat_len_64, s_tag128_64);
  test_cmac_one_kat(s_key256, k_ra8_sec_cmac_key_256, k_kat_len_0, s_tag256_0);
  test_cmac_one_kat(s_key256, k_ra8_sec_cmac_key_256, k_kat_len_16, s_tag256_16);
  test_cmac_one_kat(s_key256, k_ra8_sec_cmac_key_256, k_kat_len_40, s_tag256_40);
  test_cmac_one_kat(s_key256, k_ra8_sec_cmac_key_256, k_kat_len_64, s_tag256_64);
  TEST_END("sec_cmac: NIST SP 800-38B AES-128 + AES-256 known-answer tests");
}

/**
 * @test test_mcdc_cmac_verify_verdict
 *
 * @par MC/DC:
 * Decision: `if ((mac_len != k_ra8_sec_cmac_tag_bytes) ||
 * !ra8_ct_equal(computed, mac, k_ra8_sec_cmac_tag_bytes))` (2 conditions,
 * libs/ra8_secure_app/src/sec_cmac.c priv_ra8_sec_cmac_verify)
 *  - C1 = the supplied tag length is not the expected 16 bytes.
 *  - C2 = the recomputed CMAC does not equal the supplied tag.
 *
 * N=2 -> N+1=3 minimal MC/DC vectors:
 *  - Vector 1 (C1=F, C2=F): correct length + authentic tag -> decision F ->
 *    k_ra8_ok (control: both conditions false).
 *  - Vector 2 (C1=T): a truncated 15-byte tag -> decision T (short-circuits
 *    C2) -> k_ra8_err_invalid_arg (varies C1 only).
 *  - Vector 3 (C1=F, C2=T): correct length + one flipped tag byte -> decision
 *    T -> k_ra8_err_invalid_arg (varies C2 with C1 held F).
 * Vectors 1+2 prove C1 independently drives the verdict; 1+3 prove the same
 * for C2. N+1=3 satisfies DO-178C 6.4.4.2 minimal MC/DC.
 */
static void test_mcdc_cmac_verify_verdict(void)
{
  TEST_BEGIN("sec_cmac MC/DC: verify verdict (len-bad || tag-bad)");
  uint8_t good[k_ra8_sec_cmac_tag_bytes] = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    priv_ra8_sec_cmac_compute(s_key128, k_ra8_sec_cmac_key_128, s_msg, k_kat_len_16, good));

  /* Vector 1: C1=F, C2=F -> ok. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 priv_ra8_sec_cmac_verify(s_key128,
                                          k_ra8_sec_cmac_key_128,
                                          s_msg,
                                          k_kat_len_16,
                                          good,
                                          k_ra8_sec_cmac_tag_bytes));

  /* Vector 2: C1=T (truncated tag) -> reject. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_ra8_sec_cmac_verify(s_key128,
                                          k_ra8_sec_cmac_key_128,
                                          s_msg,
                                          k_kat_len_16,
                                          good,
                                          k_kat_short_len));

  /* Vector 3: C1=F, C2=T (one flipped tag byte) -> reject. */
  uint8_t tampered[k_ra8_sec_cmac_tag_bytes] = {};
  (void)memcpy(tampered, good, (size_t)k_ra8_sec_cmac_tag_bytes);
  tampered[k_kat_flip_idx] = (uint8_t)(tampered[k_kat_flip_idx] ^ 1U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_ra8_sec_cmac_verify(s_key128,
                                          k_ra8_sec_cmac_key_128,
                                          s_msg,
                                          k_kat_len_16,
                                          tampered,
                                          k_ra8_sec_cmac_tag_bytes));
  TEST_END("sec_cmac MC/DC: verify verdict (len-bad || tag-bad)");
}

/**
 * @par MC/DC:
 * (no compound decisions exercised here beyond the shared arg check -- each
 * case toggles exactly one precondition.)
 */
static void test_cmac_arg_validation(void)
{
  TEST_BEGIN("sec_cmac: argument validation");
  uint8_t mac[k_ra8_sec_cmac_tag_bytes] = {};

  /* NULL key / output. */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    priv_ra8_sec_cmac_compute(nullptr, k_ra8_sec_cmac_key_128, s_msg, k_kat_len_16, mac));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    priv_ra8_sec_cmac_compute(s_key128, k_ra8_sec_cmac_key_128, s_msg, k_kat_len_16, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 priv_ra8_sec_cmac_verify(s_key128,
                                          k_ra8_sec_cmac_key_128,
                                          s_msg,
                                          k_kat_len_16,
                                          nullptr,
                                          k_ra8_sec_cmac_tag_bytes));

  /* Unsupported key length (192-bit not wired here). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_ra8_sec_cmac_compute(s_key256, k_kat_len_40, s_msg, k_kat_len_16, mac));

  /* NULL message with non-zero length. */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    priv_ra8_sec_cmac_compute(s_key128, k_ra8_sec_cmac_key_128, nullptr, k_kat_len_16, mac));

  /* Oversize message rejected by the static cap. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 priv_ra8_sec_cmac_compute(s_key128,
                                           k_ra8_sec_cmac_key_128,
                                           s_msg,
                                           (uint32_t)k_ra8_sec_cmac_max_msg_bytes + 1U,
                                           mac));

  /* Empty message with a NULL pointer is legal. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    priv_ra8_sec_cmac_compute(s_key128, k_ra8_sec_cmac_key_128, nullptr, k_kat_len_0, mac));
  TEST_ASSERT_EQ(0, memcmp(mac, s_tag128_0, (size_t)k_ra8_sec_cmac_tag_bytes));
  TEST_END("sec_cmac: argument validation");
}

int main(void)
{
  test_cmac_kat_vectors();
  test_mcdc_cmac_verify_verdict();
  test_cmac_arg_validation();
  return 0;
}
