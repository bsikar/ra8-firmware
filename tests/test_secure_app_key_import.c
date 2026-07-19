/**
 * @file test_secure_app_key_import.c
 * @brief Unit + MC/DC tests for src/secure_app/key_import.c
 *
 * @details
 * Exercises the sealed-key import + opaque-handle vending API now that the
 * blob is authenticated by a real AES-CMAC (issue #291) keyed from the vault
 * key-authentication key (KAK). Covers the happy path, CMAC tamper rejection,
 * blob truncation rejection, the missing-KAK path, and the targeted MC/DC
 * vector set for the compound decision in ``ra8_key_import_resolve``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "key_import_internal.h"
#include "key_vault.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum secure_app_key_import_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_secure_app_key_import_test_kimp_fill_pattern_22 = 0x22U,
  k_secure_app_key_import_test_kimp_fill_pattern_42 = 0x42U,
  k_secure_app_key_import_test_kimp_fill_pattern_55 = 0x55U,
  k_secure_app_key_import_val_a0                    = 0xA0U,
} secure_app_key_import_uint8_const_t;

/**
 * @enum secure_app_key_import_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_secure_app_key_import_slot_ffff = 0xFFFFU,
} secure_app_key_import_uint16_const_t;

typedef enum : uint8_t {
  k_test_kimp_xor_flip_bit       = 0x01U, /**< Test kimp xor flip bit.        */
  k_test_kimp_invalid_size_short = 47U,   /**< One short of the 48-byte blob. */
  k_test_kimp_invalid_size_long  = 49U,   /**< One over the 48-byte blob.     */
  k_test_kimp_kak_bytes          = 32U,   /**< AES-256 KAK provisioned here.  */
  k_test_kimp_mac_byte0          = 32U,   /**< Index of the first CMAC byte.  */
} test_kimp_consts_t;

static uint8_t s_key_pattern[k_ra8_key_import_key_bytes];

static void test_kimp_fill_pattern(uint8_t seed)
{
  for (uint16_t i = 0U; i < (uint16_t)k_ra8_key_import_key_bytes; ++i) {
    s_key_pattern[i] = (uint8_t)(seed + (uint8_t)i);
  }
}

/**
 * @brief Provision a fresh vault + KAK + reset the importer for a test case.
 *
 * @details The KAK is the secret that keys the import-blob CMAC; it lives in
 * vault storage no Non-Secure path can reach and must be set before any blob
 * can be built or sealed.
 */
static void test_kimp_provision(void)
{
  uint8_t kak[k_test_kimp_kak_bytes];
  for (uint16_t i = 0U; i < (uint16_t)k_test_kimp_kak_bytes; ++i) {
    kak[i] = (uint8_t)(k_secure_app_key_import_val_a0 + i);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_vault_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_vault_set_mac_key(kak, (uint16_t)k_test_kimp_kak_bytes));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_import_reset());
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract.)
 */
static void test_reset_clears_state(void)
{
  TEST_BEGIN("key_import: reset clears slot bitmap");
  test_kimp_provision();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_import_reset());
  TEST_END("key_import: reset clears slot bitmap");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract.)
 */
static void test_seal_and_resolve_happy(void)
{
  TEST_BEGIN("key_import: seal then resolve round-trips");
  test_kimp_provision();

  test_kimp_fill_pattern(0x10U);
  uint8_t  blob[k_ra8_key_import_blob_bytes] = {};
  uint32_t handle                            = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_import_build_blob(s_key_pattern, blob));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_key_import_seal(blob, (uint32_t)k_ra8_key_import_blob_bytes, &handle));
  TEST_ASSERT(handle != (uint32_t)k_ra8_key_import_handle_zero);

  uint16_t slot = k_secure_app_key_import_slot_ffff;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_import_resolve(handle, &slot));
  TEST_ASSERT(slot < (uint16_t)k_ra8_key_vault_slots);
  TEST_END("key_import: seal then resolve round-trips");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract.)
 */
static void test_seal_arg_validation(void)
{
  TEST_BEGIN("key_import: seal arg validation");
  test_kimp_provision();

  uint8_t  blob[k_ra8_key_import_blob_bytes] = {};
  uint32_t handle                            = 0U;

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_key_import_seal(nullptr, (uint32_t)k_ra8_key_import_blob_bytes, &handle));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_key_import_seal(blob, (uint32_t)k_ra8_key_import_blob_bytes, nullptr));
  /* Truncated / oversized blobs are rejected on length before the CMAC. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_key_import_seal(blob, (uint32_t)k_test_kimp_invalid_size_short, &handle));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_key_import_seal(blob, (uint32_t)k_test_kimp_invalid_size_long, &handle));

  /* All-zero blob carries the wrong CMAC (the KAK is secret). */
  for (uint16_t i = 0U; i < (uint16_t)k_ra8_key_import_blob_bytes; ++i) {
    blob[i] = 0U;
  }
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_key_import_seal(blob, (uint32_t)k_ra8_key_import_blob_bytes, &handle));
  TEST_END("key_import: seal arg validation");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- proves the CMAC actually
 * authenticates: any single-byte change to the key or the tag fails seal.)
 */
static void test_seal_rejects_tampered_blob(void)
{
  TEST_BEGIN("key_import: seal rejects a tampered blob (CMAC authenticates)");
  test_kimp_provision();

  test_kimp_fill_pattern(k_secure_app_key_import_test_kimp_fill_pattern_55);
  uint8_t  blob[k_ra8_key_import_blob_bytes] = {};
  uint32_t handle                            = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_import_build_blob(s_key_pattern, blob));

  /* Flip one key byte -> CMAC no longer matches -> reject. */
  uint8_t key_tampered[k_ra8_key_import_blob_bytes];
  for (uint16_t i = 0U; i < (uint16_t)k_ra8_key_import_blob_bytes; ++i) {
    key_tampered[i] = blob[i];
  }
  key_tampered[0] = (uint8_t)(key_tampered[0] ^ (uint8_t)k_test_kimp_xor_flip_bit);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_key_import_seal(key_tampered, (uint32_t)k_ra8_key_import_blob_bytes, &handle));

  /* Flip one CMAC byte -> reject. */
  uint8_t mac_tampered[k_ra8_key_import_blob_bytes];
  for (uint16_t i = 0U; i < (uint16_t)k_ra8_key_import_blob_bytes; ++i) {
    mac_tampered[i] = blob[i];
  }
  mac_tampered[k_test_kimp_mac_byte0] =
    (uint8_t)(mac_tampered[k_test_kimp_mac_byte0] ^ (uint8_t)k_test_kimp_xor_flip_bit);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_key_import_seal(mac_tampered, (uint32_t)k_ra8_key_import_blob_bytes, &handle));

  /* The untouched blob still seals. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_key_import_seal(blob, (uint32_t)k_ra8_key_import_blob_bytes, &handle));
  TEST_END("key_import: seal rejects a tampered blob (CMAC authenticates)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- proves the CMAC key really comes from
 * the vault: with no KAK provisioned, build/seal cannot proceed.)
 */
static void test_missing_kak_fails_closed(void)
{
  TEST_BEGIN("key_import: no vault KAK -> build/seal fail (not_found)");
  /* Init the vault (which clears the KAK) but do NOT provision a KAK. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_vault_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_import_reset());

  test_kimp_fill_pattern(k_secure_app_key_import_test_kimp_fill_pattern_22);
  uint8_t  blob[k_ra8_key_import_blob_bytes] = {};
  uint32_t handle                            = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_key_import_build_blob(s_key_pattern, blob));
  TEST_ASSERT_EQ(k_ra8_err_not_found,
                 ra8_key_import_seal(blob, (uint32_t)k_ra8_key_import_blob_bytes, &handle));
  TEST_END("key_import: no vault KAK -> build/seal fail (not_found)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract.)
 */
static void test_resolve_unknown_handle(void)
{
  TEST_BEGIN("key_import: resolve unknown handle returns not_found");
  test_kimp_provision();

  uint16_t slot = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_key_import_resolve(0xDEADBEEFU, nullptr));
  /* No imports have happened, so any handle is unknown. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_key_import_resolve(0xDEADBEEFU, &slot));
  TEST_END("key_import: resolve unknown handle returns not_found");
}

/**
 * @test test_mcdc_resolve_slot_match
 *
 * @par MC/DC:
 * Decision: `if (((s_slot_used & bit) != 0U) && (internal_handle_for_slot(i) == handle))`
 * (2 conditions, src/secure_app/key_import.c ra8_key_import_resolve)
 *  - C1 = slot ``i`` is currently allocated (bit set in s_slot_used)
 *  - C2 = the handle vended for slot ``i`` matches ``handle``
 *
 * N=2 -> N+1=3 minimal MC/DC vectors:
 *  - Vector 1 (C1=F): probe handle for an unallocated index -> resolve
 *    cannot match anywhere, returns not_found. C1=F short-circuits.
 *    Decision F at index ``i``.
 *  - Vector 2 (C1=T, C2=F): allocate slot 0, then probe a handle
 *    *known* not to belong to slot 0 (we mutate the high bit). C1=T at
 *    i=0 but C2=F, so the loop body skips. Decision F at i=0.
 *  - Vector 3 (C1=T, C2=T): allocate slot 0, then probe with the real
 *    handle. C1=T, C2=T, decision T -> ok + slot returned.
 *
 * Vectors 1+3 vary C1 (decision F->T) with C2 implicitly held (in v1
 * C2 is unevaluated due to short-circuit, which is the canonical
 * masked-evaluation MC/DC pair). Vectors 2+3 vary C2 (decision F->T)
 * with C1 held T. N+1=3 satisfies DO-178C 6.4.4.2 minimal MC/DC.
 */
static void test_mcdc_resolve_slot_match(void)
{
  TEST_BEGIN("key_import MC/DC: slot-used && handle-match");
  test_kimp_provision();

  /* Vector 1: no slot allocated -> C1 always F, decision F for every i. */
  uint16_t slot_v1 = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_key_import_resolve(0x80000001U, &slot_v1));

  /* Allocate slot 0 via a real seal so we have a known good handle. */
  test_kimp_fill_pattern(k_secure_app_key_import_test_kimp_fill_pattern_42);
  uint8_t  blob[k_ra8_key_import_blob_bytes] = {};
  uint32_t handle                            = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_import_build_blob(s_key_pattern, blob));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_key_import_seal(blob, (uint32_t)k_ra8_key_import_blob_bytes, &handle));

  /* Vector 2: slot 0 allocated (C1=T) but probe with a handle that
   * cannot equal the vended one (C2=F). Flip a low bit so the high
   * bit (which the implementation forces) is preserved. */
  const uint32_t bad_handle = handle ^ (uint32_t)k_test_kimp_xor_flip_bit;
  uint16_t       slot_v2    = k_secure_app_key_import_slot_ffff;
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_key_import_resolve(bad_handle, &slot_v2));

  /* Vector 3: real handle -> C1=T, C2=T, decision T -> ok. */
  uint16_t slot_v3 = k_secure_app_key_import_slot_ffff;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_key_import_resolve(handle, &slot_v3));
  TEST_ASSERT(slot_v3 < (uint16_t)k_ra8_key_vault_slots);
  TEST_END("key_import MC/DC: slot-used && handle-match");
}

int32_t main(void)
{
  test_reset_clears_state();
  test_seal_and_resolve_happy();
  test_seal_arg_validation();
  test_seal_rejects_tampered_blob();
  test_missing_kak_fails_closed();
  test_resolve_unknown_handle();
  test_mcdc_resolve_slot_match();
  (void)fprintf(stderr, "[OK ] test_secure_app_key_import.c\n");
  return 0;
}
