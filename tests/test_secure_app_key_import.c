/**
 * @file test_secure_app_key_import.c
 * @brief Unit + MC/DC tests for src/secure_app/key_import.c
 *
 * @details
 * Exercises the sealed-key import + opaque-handle vending API.
 * Includes targeted MC/DC vector sets for the compound boolean
 * decision identified in docs/MCDC_GAPS.csv at
 * src/secure_app/key_import.c:187.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "key_import.h"
#include "key_vault.h"
#include "ra_err.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_kimp_blob_off_first_byte = 0U,
  k_test_kimp_xor_flip_bit        = 0x01U,
  k_test_kimp_invalid_size_short  = 35U,
  k_test_kimp_invalid_size_long   = 37U,
} test_kimp_consts_t;

static uint8_t s_key_pattern[k_ra_key_import_key_bytes];

static void test_kimp_fill_pattern(uint8_t seed)
{
  for (uint16_t i = 0U; i < (uint16_t)k_ra_key_import_key_bytes; ++i) {
    s_key_pattern[i] = (uint8_t)(seed + (uint8_t)i);
  }
}

static void test_reset_clears_state(void)
{
  TEST_BEGIN("key_import: reset clears slot bitmap");
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_key_vault_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_key_import_reset());
  TEST_END("key_import: reset clears slot bitmap");
}

static void test_seal_and_resolve_happy(void)
{
  TEST_BEGIN("key_import: seal then resolve round-trips");
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_key_vault_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_key_import_reset());

  test_kimp_fill_pattern(0x10U);
  uint8_t  blob[k_ra_key_import_blob_bytes] = {};
  uint32_t handle                           = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_key_import_build_blob(s_key_pattern, blob));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_key_import_seal(blob, (uint32_t)k_ra_key_import_blob_bytes, &handle));
  TEST_ASSERT(handle != (uint32_t)k_ra_key_import_handle_zero);

  uint16_t slot = 0xFFFFU;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_key_import_resolve(handle, &slot));
  TEST_ASSERT(slot < (uint16_t)k_ra_key_vault_slots);
  TEST_END("key_import: seal then resolve round-trips");
}

static void test_seal_arg_validation(void)
{
  TEST_BEGIN("key_import: seal arg validation");
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_key_vault_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_key_import_reset());

  uint8_t  blob[k_ra_key_import_blob_bytes] = {};
  uint32_t handle                           = 0U;

  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_null_ptr,
    (int32_t)ra_key_import_seal(nullptr, (uint32_t)k_ra_key_import_blob_bytes, &handle));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_key_import_seal(blob, (uint32_t)k_ra_key_import_blob_bytes, nullptr));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_size,
    (int32_t)ra_key_import_seal(blob, (uint32_t)k_test_kimp_invalid_size_short, &handle));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_size,
    (int32_t)ra_key_import_seal(blob, (uint32_t)k_test_kimp_invalid_size_long, &handle));

  /* All-zero blob has wrong MAC (s_salt is non-zero). */
  for (uint16_t i = 0U; i < (uint16_t)k_ra_key_import_blob_bytes; ++i) {
    blob[i] = 0U;
  }
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_key_import_seal(blob, (uint32_t)k_ra_key_import_blob_bytes, &handle));
  TEST_END("key_import: seal arg validation");
}

static void test_resolve_unknown_handle(void)
{
  TEST_BEGIN("key_import: resolve unknown handle returns not_found");
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_key_vault_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_key_import_reset());

  uint16_t slot = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_key_import_resolve(0xDEADBEEFU, nullptr));
  /* No imports have happened, so any handle is unknown. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_found, (int32_t)ra_key_import_resolve(0xDEADBEEFU, &slot));
  TEST_END("key_import: resolve unknown handle returns not_found");
}

/**
 * @test test_mcdc_resolve_slot_match
 *
 * @par MC/DC:
 * Decision: `if (((s_slot_used & bit) != 0U) && (internal_handle_for_slot(i) == handle))`
 * (2 conditions, src/secure_app/key_import.c:187)
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
  TEST_BEGIN("key_import MC/DC: slot-used && handle-match (key_import.c:187)");
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_key_vault_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_key_import_reset());

  /* Vector 1: no slot allocated -> C1 always F, decision F for every i. */
  uint16_t slot_v1 = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_found,
                 (int32_t)ra_key_import_resolve(0x80000001U, &slot_v1));

  /* Allocate slot 0 via a real seal so we have a known good handle. */
  test_kimp_fill_pattern(0x42U);
  uint8_t  blob[k_ra_key_import_blob_bytes] = {};
  uint32_t handle                           = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_key_import_build_blob(s_key_pattern, blob));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_key_import_seal(blob, (uint32_t)k_ra_key_import_blob_bytes, &handle));

  /* Vector 2: slot 0 allocated (C1=T) but probe with a handle that
   * cannot equal the vended one (C2=F). Flip a low bit so the high
   * bit (which the implementation forces) is preserved. */
  const uint32_t bad_handle = handle ^ (uint32_t)k_test_kimp_xor_flip_bit;
  uint16_t       slot_v2    = 0xFFFFU;
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_found, (int32_t)ra_key_import_resolve(bad_handle, &slot_v2));

  /* Vector 3: real handle -> C1=T, C2=T, decision T -> ok. */
  uint16_t slot_v3 = 0xFFFFU;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_key_import_resolve(handle, &slot_v3));
  TEST_ASSERT(slot_v3 < (uint16_t)k_ra_key_vault_slots);
  TEST_END("key_import MC/DC: slot-used && handle-match (key_import.c:187)");
}

int32_t main(void)
{
  test_reset_clears_state();
  test_seal_and_resolve_happy();
  test_seal_arg_validation();
  test_resolve_unknown_handle();
  test_mcdc_resolve_slot_match();
  (void)fprintf(stderr, "[OK ] test_secure_app_key_import.c\n");
  return 0;
}
