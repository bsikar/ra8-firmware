/**
 * @file test_ra_bscan.c
 * @brief Unit tests for ra_bscan.c (boundary-scan TAP bookkeeping)
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
#include <stdio.h>

#include "ra8d2_bscan_regs.h"
#include "ra_bscan.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_bscan_test_const_t
 * @brief Magic-number replacements local to the test file.
 */
typedef enum : uint32_t {
  k_ra_bscan_test_idcode_expected = 0x085DA447UL, /**< HUM 50.2.2 p 3258. */
  k_ra_bscan_test_bad_mask        = 0xDEADBEEFUL, /**< Garbage clear mask. */
} ra_bscan_test_const_t;

/**
 * @enum ra_bscan_test_instr_t
 * @brief Reserved-opcode candidates rejected by the validator.
 */
typedef enum : uint8_t {
  k_ra_bscan_test_instr_reserved_2 = 0x2U, /**< 0x2 is reserved per 50.2.1. */
  k_ra_bscan_test_instr_reserved_4 = 0x4U, /**< 0x4 is reserved per 50.2.1. */
  k_ra_bscan_test_instr_reserved_a = 0xAU, /**< 0xA is reserved per 50.2.1. */
} ra_bscan_test_instr_t;

static void test_init_happy(void)
{
  TEST_BEGIN("bscan init happy");
  ra_sim_mmap_reset();
  (void)ra_bscan_deinit();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_bscan_init());

  ra_bscan_status_t status = {
    .initialised      = false,
    .last_instruction = k_ra_bscan_instr_extest,
    .expected_idcode  = 0U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_bscan_get_status(&status));
  TEST_ASSERT(status.initialised);
  TEST_ASSERT_EQ((int32_t)k_ra_bscan_instr_bypass, (int32_t)status.last_instruction);
  TEST_ASSERT_EQ((int32_t)k_ra_bscan_test_idcode_expected, (int32_t)status.expected_idcode);
  TEST_END("bscan init happy");
}

static void test_get_idcode_happy(void)
{
  TEST_BEGIN("bscan get_idcode happy");
  ra_sim_mmap_reset();
  (void)ra_bscan_deinit();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_bscan_init());

  uint32_t idcode = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_bscan_get_idcode(&idcode));
  TEST_ASSERT_EQ((int32_t)k_ra_bscan_test_idcode_expected, (int32_t)idcode);
  TEST_END("bscan get_idcode happy");
}

static void test_get_idcode_null(void)
{
  TEST_BEGIN("bscan get_idcode null");
  ra_sim_mmap_reset();
  (void)ra_bscan_init();

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_bscan_get_idcode(nullptr));
  TEST_END("bscan get_idcode null");
}

static void test_get_idcode_not_initialised(void)
{
  TEST_BEGIN("bscan get_idcode not initialised");
  ra_sim_mmap_reset();
  (void)ra_bscan_deinit();

  uint32_t idcode = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_bscan_get_idcode(&idcode));
  TEST_END("bscan get_idcode not initialised");
}

static void test_get_status_null(void)
{
  TEST_BEGIN("bscan get_status null");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_bscan_get_status(nullptr));
  TEST_END("bscan get_status null");
}

static void test_set_instruction_happy(void)
{
  TEST_BEGIN("bscan set_instruction happy");
  ra_sim_mmap_reset();
  (void)ra_bscan_deinit();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_bscan_init());

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_bscan_set_instruction(k_ra_bscan_instr_idcode));

  ra_bscan_status_t status = {
    .initialised      = false,
    .last_instruction = k_ra_bscan_instr_bypass,
    .expected_idcode  = 0U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_bscan_get_status(&status));
  TEST_ASSERT_EQ((int32_t)k_ra_bscan_instr_idcode, (int32_t)status.last_instruction);
  TEST_END("bscan set_instruction happy");
}

static void test_set_instruction_reserved(void)
{
  TEST_BEGIN("bscan set_instruction reserved");
  ra_sim_mmap_reset();
  (void)ra_bscan_deinit();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_bscan_init());

  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_bscan_set_instruction((ra_bscan_instr_t)k_ra_bscan_test_instr_reserved_2));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_bscan_set_instruction((ra_bscan_instr_t)k_ra_bscan_test_instr_reserved_4));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_bscan_set_instruction((ra_bscan_instr_t)k_ra_bscan_test_instr_reserved_a));
  TEST_END("bscan set_instruction reserved");
}

static void test_set_instruction_not_initialised(void)
{
  TEST_BEGIN("bscan set_instruction not initialised");
  ra_sim_mmap_reset();
  (void)ra_bscan_deinit();

  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized,
                 (int32_t)ra_bscan_set_instruction(k_ra_bscan_instr_extest));
  TEST_END("bscan set_instruction not initialised");
}

static void test_clear_status_happy(void)
{
  TEST_BEGIN("bscan clear_status happy");
  ra_sim_mmap_reset();
  (void)ra_bscan_deinit();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_bscan_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_bscan_set_instruction(k_ra_bscan_instr_extest));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_bscan_clear_status(0U));

  ra_bscan_status_t status = {
    .initialised      = false,
    .last_instruction = k_ra_bscan_instr_extest,
    .expected_idcode  = 0U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_bscan_get_status(&status));
  TEST_ASSERT_EQ((int32_t)k_ra_bscan_instr_bypass, (int32_t)status.last_instruction);
  TEST_ASSERT(status.initialised);
  TEST_END("bscan clear_status happy");
}

static void test_clear_status_bad_mask(void)
{
  TEST_BEGIN("bscan clear_status bad mask");
  ra_sim_mmap_reset();
  (void)ra_bscan_init();

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_bscan_clear_status((uint32_t)k_ra_bscan_test_bad_mask));
  TEST_END("bscan clear_status bad mask");
}

static void test_clear_status_not_initialised(void)
{
  TEST_BEGIN("bscan clear_status not initialised");
  ra_sim_mmap_reset();
  (void)ra_bscan_deinit();

  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_bscan_clear_status(0U));
  TEST_END("bscan clear_status not initialised");
}

static void test_deinit_idempotent(void)
{
  TEST_BEGIN("bscan deinit idempotent");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_bscan_deinit());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_bscan_deinit());

  ra_bscan_status_t status = {
    .initialised      = true,
    .last_instruction = k_ra_bscan_instr_extest,
    .expected_idcode  = 0U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_bscan_get_status(&status));
  TEST_ASSERT(!status.initialised);
  TEST_ASSERT_EQ((int32_t)k_ra_bscan_instr_bypass, (int32_t)status.last_instruction);
  TEST_END("bscan deinit idempotent");
}

static void test_idcode_constant_matches_header(void)
{
  TEST_BEGIN("bscan idcode constant matches header");
  /* Sanity check that the bookkeeping value the driver emits matches
   * the ``k_ra_bscan_jtidr_reset`` constant in the regs header (HUM
   * 50.2.2 p 3258 fixed value 0x085D_A447). If anyone ever ports this
   * file to a different RA-family member, this assertion fires. */
  TEST_ASSERT_EQ((int32_t)k_ra_bscan_test_idcode_expected, (int32_t)k_ra_bscan_jtidr_reset);
  TEST_END("bscan idcode constant matches header");
}

int32_t main(void)
{
  test_init_happy();
  test_get_idcode_happy();
  test_get_idcode_null();
  test_get_idcode_not_initialised();
  test_get_status_null();
  test_set_instruction_happy();
  test_set_instruction_reserved();
  test_set_instruction_not_initialised();
  test_clear_status_happy();
  test_clear_status_bad_mask();
  test_clear_status_not_initialised();
  test_deinit_idempotent();
  test_idcode_constant_matches_header();
  (void)fprintf(stderr, "[OK  ] test_ra_bscan.c\n");
  return 0;
}
