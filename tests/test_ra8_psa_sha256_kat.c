/**
 * @file test_ra8_psa_sha256_kat.c
 * @brief External known-answer test (KAT) for the PSA SHA-256 facade.
 *
 * @par Tag
 * [Ring 1 / Core] {World: S}
 *
 * @details
 * SHA-256 is on the root-of-trust verify path (ra8_rot_verify_image re-hashes the
 * image body and compares it to the signed trailer digest), so its correctness
 * is security-critical. The existing round-trip tests only prove the primitive
 * is self-consistent -- they "would pass for ROT13". This test instead pins
 * ra8_psa_hash_compute to the published FIPS 180-4 / NIST SHA-256 test vectors, an
 * EXTERNAL oracle that a byte-swapped or truncated digest cannot satisfy:
 *
 *   ""                                   -> e3b0c442...7852b855
 *   "abc"                                -> ba7816bf...f20015ad
 *   448-bit two-block message            -> 248d6a61...19db06c1
 *   1,000,000 x 'a'                       -> cdc76e5c...c7112cd0
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_psa_crypto.h"
#include "unity_minimal.h"

/**
 * @enum t_sha_kat_t
 * @brief Message lengths of the FIPS 180-4 SHA-256 known-answer vectors.
 *
 * @details
 * Both are part of the published vectors. The 56-byte case is the two-block
 * example; the one-million-'a' case is the long-message example, whose length
 * is what makes it exercise the update loop rather than a single block.
 */
typedef enum : uint32_t {
  k_t_two_block_len = 56U,      /**< The 448-bit two-block message.     */
  k_t_million_a_len = 1000000U, /**< The one-million-character message. */
} t_sha_kat_t;

/** @brief One-million-byte 'a' message buffer (NIST long-message vector). */
static uint8_t s_million_a[k_t_million_a_len];

/**
 * @brief Decode a hex string of ``n`` bytes into ``out`` (no compound logic).
 */
static void hex_to_bytes(const char* hex, uint8_t* out, size_t n)
{
  for (size_t i = 0U; i < n; ++i) {
    /* strtoul rather than sscanf("%2x"): sscanf cannot report a
     * conversion error, so a malformed vector would decode as 0x00. */
    const char          pair[3] = {hex[2U * i], hex[(2U * i) + 1U], '\0'};
    char*               end     = nullptr;
    const unsigned long byte    = strtoul(pair, &end, 16);
    TEST_ASSERT(end == &pair[2]);
    out[i] = (uint8_t)byte;
  }
}

/**
 * @brief Initialize the PSA facade, tolerating a prior init in this process.
 *
 * @details Both test cases run in one process; the second ra8_psa_crypto_init
 * returns k_ra8_err_exists rather than k_ra8_ok. Split, so there is no compound
 * decision to cover.
 */
static void ensure_psa_init(void)
{
  const ra8_err_t err = ra8_psa_crypto_init();
  if (err != k_ra8_ok) {
    TEST_ASSERT_EQ(k_ra8_err_exists, err);
  }
}

/**
 * @brief Hash ``msg`` and assert the digest equals the hex known-answer.
 */
static void expect_sha256(const uint8_t* msg, size_t msg_len, const char* expect_hex)
{
  uint8_t expect[k_ra8_psa_sha256_len];
  hex_to_bytes(expect_hex, expect, (size_t)k_ra8_psa_sha256_len);

  uint8_t digest[k_ra8_psa_sha256_len] = {};
  size_t  out_len                      = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_psa_hash_compute(k_ra8_psa_alg_sha_256, msg, msg_len, digest, sizeof(digest), &out_len));
  TEST_ASSERT_EQ(k_ra8_psa_sha256_len, out_len);
  TEST_ASSERT_EQ(0, memcmp(digest, expect, (size_t)k_ra8_psa_sha256_len));
}

/**
 * @test test_sha256_nist_short_vectors
 * @details The empty, "abc", and 448-bit two-block FIPS 180-4 example vectors.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- straight KAT comparisons; the facade
 * decision logic is exercised by test_ra8_psa_crypto.c.)
 */
static void test_sha256_nist_short_vectors(void)
{
  TEST_BEGIN("PSA SHA-256 KAT: empty / abc / 448-bit NIST vectors");
  ensure_psa_init();

  expect_sha256((const uint8_t*)"",
                0U,
                "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  expect_sha256((const uint8_t*)"abc",
                3U,
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  expect_sha256((const uint8_t*)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                k_t_two_block_len,
                "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

  TEST_END("PSA SHA-256 KAT: empty / abc / 448-bit NIST vectors");
}

/**
 * @test test_sha256_nist_million_a
 * @details The NIST long-message vector: 1,000,000 repetitions of 'a', which
 * exercises multi-block processing and the 64-bit length encoding.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- a single KAT comparison.)
 */
static void test_sha256_nist_million_a(void)
{
  TEST_BEGIN("PSA SHA-256 KAT: 1,000,000 x 'a' NIST long-message vector");
  ensure_psa_init();

  (void)memset(s_million_a, (int)'a', sizeof(s_million_a));
  expect_sha256(s_million_a,
                sizeof(s_million_a),
                "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

  TEST_END("PSA SHA-256 KAT: 1,000,000 x 'a' NIST long-message vector");
}

int32_t main(void)
{
  test_sha256_nist_short_vectors();
  test_sha256_nist_million_a();
  return 0;
}
