/**
 * @file test_ra8_rsip_hmac_kat.c
 * @brief External RFC 4231 known-answer test for the RSIP HMAC-SHA-256 backend.
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * On the host build ``RA8_RSIP_SOFTWARE_BACKEND`` computes HMAC-SHA-256 (RFC 2104)
 * in software on top of the real FIPS 180-4 SHA-256. The audit noted the existing
 * RSIP HMAC test "computes the expected MAC with the same primitive under test"
 * -- a tautology. This test instead pins ra8_rsip_hmac_sha256_{init,update,final}
 * to the published RFC 4231 HMAC-SHA-256 test vectors, an EXTERNAL oracle that a
 * wrong ipad/opad, key-block preparation, or SHA-256 cannot satisfy. It covers a
 * short key, a text key, a block-sized key, a shorter-than-block key, and the two
 * larger-than-block keys that must be hashed first (TC1-4, TC6-7).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_mstp.h"
#include "ra8_rsip.h"
#include "ra8_rsip_core.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum rsip_hmac_kat_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_rsip_hmac_kat_val_152 = 152U,
  k_rsip_hmac_kat_val_28  = 28U,
  k_rsip_hmac_kat_val_54  = 54U,
} rsip_hmac_kat_uint8_const_t;

/** @brief Test constants for the RFC 4231 vectors. */
typedef enum : uint32_t {
  k_hmac_key_20  = 20U,   /**< TC1/TC3 key length.                   */
  k_hmac_key_25  = 25U,   /**< TC4 incrementing key length.          */
  k_hmac_key_131 = 131U,  /**< TC6/TC7 larger-than-block key length. */
  k_hmac_data_50 = 50U,   /**< TC3/TC4 repeated-byte data length.    */
  k_hmac_fill_0b = 0x0BU, /**< Hmac fill 0b.                         */
  k_hmac_fill_aa = 0xAAU, /**< Hmac fill aa.                         */
  k_hmac_fill_dd = 0xDDU, /**< Hmac fill dd.                         */
  k_hmac_fill_cd = 0xCDU, /**< Hmac fill cd.                         */
} hmac_kat_const_t;

/* Scratch buffers for the byte-pattern keys/data (largest case is 131 bytes). */
static uint8_t s_key[k_hmac_key_131];
static uint8_t s_data[k_hmac_data_50];

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
 * @brief HMAC-SHA-256 ``data`` under ``key`` and assert the hex known-answer.
 */
static void expect_hmac(const uint8_t* key,
                        uint32_t       key_len,
                        const uint8_t* data,
                        uint32_t       data_len,
                        const char*    expect_hex)
{
  uint8_t expect[k_ra8_rsip_sha256_digest_bytes];
  hex_to_bytes(expect_hex, expect, (size_t)k_ra8_rsip_sha256_digest_bytes);

  ra8_rsip_hmac_sha256_ctx_t ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_hmac_sha256_init(&ctx, key, key_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_hmac_sha256_update(&ctx, data, data_len));
  uint8_t mac[k_ra8_rsip_sha256_digest_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_hmac_sha256_final(&ctx, mac));
  TEST_ASSERT_EQ(0, memcmp(mac, expect, (size_t)k_ra8_rsip_sha256_digest_bytes));
}

/**
 * @brief Reset the sim and bring the RSIP up for the KAT vectors.
 * @return None.
 * @pre None.
 * @post The RSIP is initialised (or already-existing), ready for HMAC calls.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void hmac_kat_init(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
  const ra8_rsip_config_t cfg      = {.run_bist = false};
  const ra8_err_t         init_err = ra8_rsip_init(&cfg);
  if (init_err != k_ra8_ok) {
    TEST_ASSERT_EQ(k_ra8_err_exists, init_err);
  }
}

/**
 * @test test_hmac_sha256_rfc4231
 * @details The RFC 4231 HMAC-SHA-256 vectors TC1-4, TC6, TC7.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- straight KAT comparisons; the HMAC
 * decision logic is exercised by the ra8_rsip driver's own tests.)
 */
static void test_hmac_sha256_rfc4231(void)
{
  TEST_BEGIN("RSIP HMAC-SHA-256 RFC 4231 known-answer vectors");
  hmac_kat_init();

  /* TC1: 20-byte 0x0b key, "Hi There". */
  (void)memset(s_key, (int)k_hmac_fill_0b, (size_t)k_hmac_key_20);
  expect_hmac(s_key,
              k_hmac_key_20,
              (const uint8_t*)"Hi There",
              8U,
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

  /* TC2: "Jefe" key, "what do ya want for nothing?". */
  expect_hmac((const uint8_t*)"Jefe",
              4U,
              (const uint8_t*)"what do ya want for nothing?",
              k_rsip_hmac_kat_val_28,
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

  /* TC3: 20-byte 0xaa key, 50-byte 0xdd data. */
  (void)memset(s_key, (int)k_hmac_fill_aa, (size_t)k_hmac_key_20);
  (void)memset(s_data, (int)k_hmac_fill_dd, (size_t)k_hmac_data_50);
  expect_hmac(s_key,
              k_hmac_key_20,
              s_data,
              k_hmac_data_50,
              "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");

  /* TC4: 25-byte incrementing 0x01..0x19 key, 50-byte 0xcd data. */
  for (uint32_t i = 0U; i < k_hmac_key_25; ++i) {
    s_key[i] = (uint8_t)(i + 1U);
  }
  (void)memset(s_data, (int)k_hmac_fill_cd, (size_t)k_hmac_data_50);
  expect_hmac(s_key,
              k_hmac_key_25,
              s_data,
              k_hmac_data_50,
              "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");

  /* TC6: 131-byte 0xaa key (hashed first), long text. */
  (void)memset(s_key, (int)k_hmac_fill_aa, (size_t)k_hmac_key_131);
  expect_hmac(s_key,
              k_hmac_key_131,
              (const uint8_t*)"Test Using Larger Than Block-Size Key - Hash Key First",
              k_rsip_hmac_kat_val_54,
              "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

  /* TC7: 131-byte 0xaa key (hashed first), 152-byte text. */
  expect_hmac(s_key,
              k_hmac_key_131,
              (const uint8_t*)"This is a test using a larger than block-size key and a larger "
                              "than block-size data. The key needs to be hashed before being "
                              "used by the HMAC algorithm.",
              k_rsip_hmac_kat_val_152,
              "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");

  TEST_END("RSIP HMAC-SHA-256 RFC 4231 known-answer vectors");
}

int32_t main(void)
{
  test_hmac_sha256_rfc4231();
  (void)fprintf(stderr, "[OK ] test_ra8_rsip_hmac_kat.c\n");
  return 0;
}
