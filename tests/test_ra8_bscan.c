/**
 * @file test_ra8_bscan.c
 * @brief Unit tests for ra8_bscan.c (boundary-scan TAP bookkeeping)
 *
 * @details
 * The boundary-scan TAP registers are not CPU-accessible (HUM
 * Ch 50.2.3 explicit note, p 3259), so this driver carries no
 * register I/O. The tests therefore exercise the firmware-side
 * bookkeeping object only: lifecycle, idcode constant, status
 * snapshot, instruction validation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_bscan.h"
#include "ra8_bscan_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra8_bscan_test_const_t
 * @brief Magic-number replacements local to the test file.
 */
typedef enum : uint32_t {
  k_ra8_bscan_test_idcode_expected = 0x085DA447UL, /**< HUM 50.2.2 p 3258.  */
  k_ra8_bscan_test_bad_mask        = 0xDEADBEEFUL, /**< Garbage clear mask. */
} ra8_bscan_test_const_t;

/**
 * @enum ra8_bscan_test_instr_t
 * @brief Reserved-opcode candidates rejected by the validator.
 */
typedef enum : uint8_t {
  k_ra8_bscan_test_instr_reserved_2 = 0x2U, /**< 0x2 is reserved per 50.2.1. */
  k_ra8_bscan_test_instr_reserved_4 = 0x4U, /**< 0x4 is reserved per 50.2.1. */
  k_ra8_bscan_test_instr_reserved_a = 0xAU, /**< 0xA is reserved per 50.2.1. */
} ra8_bscan_test_instr_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_happy(void)
{
  TEST_BEGIN("bscan init happy");
  ra8_fake_mmap_reset();
  (void)ra8_bscan_deinit();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_bscan_init());

  ra8_bscan_status_t status = {
    .initialized      = false,
    .last_instruction = k_ra8_bscan_instr_extest,
    .expected_idcode  = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bscan_get_status(&status));
  TEST_ASSERT(status.initialized);
  TEST_ASSERT_EQ(k_ra8_bscan_instr_bypass, status.last_instruction);
  TEST_ASSERT_EQ(k_ra8_bscan_test_idcode_expected, status.expected_idcode);
  TEST_END("bscan init happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_idcode_happy(void)
{
  TEST_BEGIN("bscan get_idcode happy");
  ra8_fake_mmap_reset();
  (void)ra8_bscan_deinit();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bscan_init());

  uint32_t idcode = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bscan_get_idcode(&idcode));
  TEST_ASSERT_EQ(k_ra8_bscan_test_idcode_expected, idcode);
  TEST_END("bscan get_idcode happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_idcode_null(void)
{
  TEST_BEGIN("bscan get_idcode null");
  ra8_fake_mmap_reset();
  (void)ra8_bscan_init();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_bscan_get_idcode(nullptr));
  TEST_END("bscan get_idcode null");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_idcode_not_initialized(void)
{
  TEST_BEGIN("bscan get_idcode not initialized");
  ra8_fake_mmap_reset();
  (void)ra8_bscan_deinit();

  uint32_t idcode = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_bscan_get_idcode(&idcode));
  TEST_END("bscan get_idcode not initialized");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_status_null(void)
{
  TEST_BEGIN("bscan get_status null");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_bscan_get_status(nullptr));
  TEST_END("bscan get_status null");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_instruction_happy(void)
{
  TEST_BEGIN("bscan set_instruction happy");
  ra8_fake_mmap_reset();
  (void)ra8_bscan_deinit();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bscan_init());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_bscan_set_instruction(k_ra8_bscan_instr_idcode));

  ra8_bscan_status_t status = {
    .initialized      = false,
    .last_instruction = k_ra8_bscan_instr_bypass,
    .expected_idcode  = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bscan_get_status(&status));
  TEST_ASSERT_EQ(k_ra8_bscan_instr_idcode, status.last_instruction);
  TEST_END("bscan set_instruction happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_instruction_reserved(void)
{
  TEST_BEGIN("bscan set_instruction reserved");
  ra8_fake_mmap_reset();
  (void)ra8_bscan_deinit();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bscan_init());

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_bscan_set_instruction((ra8_bscan_instr_t)k_ra8_bscan_test_instr_reserved_2));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_bscan_set_instruction((ra8_bscan_instr_t)k_ra8_bscan_test_instr_reserved_4));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_bscan_set_instruction((ra8_bscan_instr_t)k_ra8_bscan_test_instr_reserved_a));
  TEST_END("bscan set_instruction reserved");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_instruction_not_initialized(void)
{
  TEST_BEGIN("bscan set_instruction not initialized");
  ra8_fake_mmap_reset();
  (void)ra8_bscan_deinit();

  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_bscan_set_instruction(k_ra8_bscan_instr_extest));
  TEST_END("bscan set_instruction not initialized");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_status_happy(void)
{
  TEST_BEGIN("bscan clear_status happy");
  ra8_fake_mmap_reset();
  (void)ra8_bscan_deinit();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bscan_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bscan_set_instruction(k_ra8_bscan_instr_extest));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_bscan_clear_status(0U));

  ra8_bscan_status_t status = {
    .initialized      = false,
    .last_instruction = k_ra8_bscan_instr_extest,
    .expected_idcode  = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bscan_get_status(&status));
  TEST_ASSERT_EQ(k_ra8_bscan_instr_bypass, status.last_instruction);
  TEST_ASSERT(status.initialized);
  TEST_END("bscan clear_status happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_status_bad_mask(void)
{
  TEST_BEGIN("bscan clear_status bad mask");
  ra8_fake_mmap_reset();
  (void)ra8_bscan_init();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_bscan_clear_status((uint32_t)k_ra8_bscan_test_bad_mask));
  TEST_END("bscan clear_status bad mask");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_status_not_initialized(void)
{
  TEST_BEGIN("bscan clear_status not initialized");
  ra8_fake_mmap_reset();
  (void)ra8_bscan_deinit();

  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_bscan_clear_status(0U));
  TEST_END("bscan clear_status not initialized");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit_idempotent(void)
{
  TEST_BEGIN("bscan deinit idempotent");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_bscan_deinit());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bscan_deinit());

  ra8_bscan_status_t status = {
    .initialized      = true,
    .last_instruction = k_ra8_bscan_instr_extest,
    .expected_idcode  = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_bscan_get_status(&status));
  TEST_ASSERT(!status.initialized);
  TEST_ASSERT_EQ(k_ra8_bscan_instr_bypass, status.last_instruction);
  TEST_END("bscan deinit idempotent");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_idcode_constant_matches_header(void)
{
  TEST_BEGIN("bscan idcode constant matches header");
  /* Sanity check that the bookkeeping value the driver emits matches
   * the ``k_ra8_bscan_jtidr_reset`` constant in the regs header (HUM
   * 50.2.2 p 3258 fixed value 0x085D_A447). If anyone ever ports this
   * file to a different RA-family member, this assertion fires. */
  TEST_ASSERT_EQ(k_ra8_bscan_test_idcode_expected, k_ra8_bscan_jtidr_reset);
  TEST_END("bscan idcode constant matches header");
}

int main(void)
{
  test_init_happy();
  test_get_idcode_happy();
  test_get_idcode_null();
  test_get_idcode_not_initialized();
  test_get_status_null();
  test_set_instruction_happy();
  test_set_instruction_reserved();
  test_set_instruction_not_initialized();
  test_clear_status_happy();
  test_clear_status_bad_mask();
  test_clear_status_not_initialized();
  test_deinit_idempotent();
  test_idcode_constant_matches_header();
  return 0;
}
