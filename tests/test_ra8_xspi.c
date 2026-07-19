/**
 * @file test_ra8_xspi.c
 * @brief Unit tests for the xSPI driver (ra8_xspi.c)
 *
 * @details Init, direct-command packing, flash read, and status/JEDEC
 * id tests. The program/erase + WIP/INTS timeout legs live in the
 * sibling test_ra8_xspi_program.c and the controller lifecycle +
 * XIP/DTR/DQS + MC/DC suites in test_ra8_xspi_ctrl.c; shared fixture
 * constants and prep_flash() live in support/xspi_test_util.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_ospi_regs.h"
#include "ra8_sim_mmap.h"
#include "ra8_sim_mmio.h"
#include "ra8_sim_xspi_flash.h"
#include "ra8_system_regs.h"
#include "ra8_xspi.h"
#include "ra8_xspi_internal.h"
#include "support/xspi_test_util.h"
#include "unity_minimal.h"

/**
 * @enum xspi_test_lit_t
 * @brief Named constants for the register stamp patterns and literal
 *        test vectors previously inlined in this file's test bodies.
 */
typedef enum : uint32_t {
  k_xspi_lit_x11 = 0x11U, /**< Xspi literal 0x11. */
  k_xspi_lit_x22 = 0x22U, /**< Xspi literal 0x22. */
  k_xspi_lit_x33 = 0x33U, /**< Xspi literal 0x33. */
  k_xspi_lit_x44 = 0x44U, /**< Xspi literal 0x44. */
  k_xspi_lit_5   = 5,     /**< Xspi literal 5.    */
  k_xspi_lit_x55 = 0x55U, /**< Xspi literal 0x55. */
} xspi_test_lit_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_inst0_happy(void)
{
  TEST_BEGIN("ra8_xspi_init instance 0");
  prep_flash();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init((uint8_t)k_test_xspi_valid_inst0, k_ra8_xspi_lio_1s1s1s));

  volatile r_xspi_regs_t* reg = ra8_xspi((uint8_t)k_test_xspi_valid_inst0);
  TEST_ASSERT_NOT_NULL(reg);
  TEST_ASSERT_EQ(0, reg->WRAPCFG);
  TEST_ASSERT_EQ(0, reg->COMCFG);
  /* Protocol config lands in the on-board chip-select's LIOCFGCS slot
   * (CS1 on the EK-RA8D2). */
  TEST_ASSERT_EQ(k_ra8_xspi_lio_1s1s1s, reg->LIOCFGCS[1]);
  TEST_END("ra8_xspi_init instance 0");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_inst1_happy(void)
{
  TEST_BEGIN("ra8_xspi_init instance 1");
  prep_flash();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init((uint8_t)k_test_xspi_valid_inst1, k_ra8_xspi_lio_1s8s8s));

  volatile r_xspi_regs_t* reg = ra8_xspi((uint8_t)k_test_xspi_valid_inst1);
  TEST_ASSERT_NOT_NULL(reg);
  TEST_ASSERT_EQ(k_ra8_xspi_lio_1s8s8s, reg->LIOCFGCS[1]);
  TEST_END("ra8_xspi_init instance 1");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_instance(void)
{
  TEST_BEGIN("ra8_xspi_init rejects out-of-range instance");
  prep_flash();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_xspi_init((uint8_t)k_test_xspi_bad_instance, k_ra8_xspi_lio_1s1s1s));
  TEST_END("ra8_xspi_init rejects out-of-range instance");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_direct_command_null_buf(void)
{
  TEST_BEGIN("ra8_xspi_direct_command rejects NULL buf");
  prep_flash();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_xspi_direct_command((uint8_t)k_test_xspi_valid_inst0, nullptr, 4U));
  TEST_END("ra8_xspi_direct_command rejects NULL buf");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_direct_command_too_long(void)
{
  TEST_BEGIN("ra8_xspi_direct_command rejects len > 16");
  prep_flash();
  uint8_t buf[32] = {};
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_xspi_direct_command((uint8_t)k_test_xspi_valid_inst0, buf, (uint8_t)k_test_xspi_too_many));
  TEST_END("ra8_xspi_direct_command rejects len > 16");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_direct_command_bad_instance(void)
{
  TEST_BEGIN("ra8_xspi_direct_command rejects bad instance");
  prep_flash();
  uint8_t buf[4] = {0x01U, 0x02U, 0x03U, 0x04U};
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_xspi_direct_command((uint8_t)k_test_xspi_bad_instance, buf, 4U));
  TEST_END("ra8_xspi_direct_command rejects bad instance");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_direct_command_packs_aligned(void)
{
  TEST_BEGIN("ra8_xspi_direct_command packs 4-byte-aligned payload");
  prep_flash();

  uint8_t buf[4] = {k_xspi_lit_x11, k_xspi_lit_x22, k_xspi_lit_x33, k_xspi_lit_x44};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_direct_command((uint8_t)k_test_xspi_valid_inst0, buf, 4U));

  volatile r_xspi_regs_t* reg = ra8_xspi((uint8_t)k_test_xspi_valid_inst0);
  TEST_ASSERT_NOT_NULL(reg);
  TEST_ASSERT_EQ(0x44332211L, reg->CDBUF[0]);
  TEST_END("ra8_xspi_direct_command packs 4-byte-aligned payload");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_direct_command_packs_unaligned(void)
{
  TEST_BEGIN("ra8_xspi_direct_command flushes trailing partial word");
  prep_flash();

  uint8_t buf[k_xspi_lit_5] = {k_xspi_lit_x11,
                               k_xspi_lit_x22,
                               k_xspi_lit_x33,
                               k_xspi_lit_x44,
                               k_xspi_lit_x55};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_direct_command((uint8_t)k_test_xspi_valid_inst0, buf, 5U));

  volatile r_xspi_regs_t* reg = ra8_xspi((uint8_t)k_test_xspi_valid_inst0);
  TEST_ASSERT_NOT_NULL(reg);
  TEST_ASSERT_EQ(0x44332211L, reg->CDBUF[0]);
  TEST_ASSERT_EQ(0x55L, reg->CDBUF[1]);
  TEST_END("ra8_xspi_direct_command flushes trailing partial word");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_direct_command_len_zero(void)
{
  TEST_BEGIN("ra8_xspi_direct_command accepts len=0");
  prep_flash();
  uint8_t buf[1] = {0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_direct_command((uint8_t)k_test_xspi_valid_inst0, buf, 0U));
  TEST_END("ra8_xspi_direct_command accepts len=0");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_direct_command_full_16(void)
{
  TEST_BEGIN("ra8_xspi_direct_command packs 16-byte payload");
  prep_flash();

  uint8_t buf[16];
  for (uint8_t i = 0U; i < 16U; ++i) {
    buf[i] = (uint8_t)(i + 1U);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_direct_command((uint8_t)k_test_xspi_valid_inst0, buf, 16U));

  volatile r_xspi_regs_t* reg = ra8_xspi((uint8_t)k_test_xspi_valid_inst0);
  TEST_ASSERT_NOT_NULL(reg);
  TEST_ASSERT_EQ(0x04030201L, reg->CDBUF[0]);
  TEST_ASSERT_EQ(0x08070605L, reg->CDBUF[1]);
  TEST_ASSERT_EQ(0x0C0B0A09L, reg->CDBUF[2]);
  TEST_ASSERT_EQ(0x100F0E0DL, reg->CDBUF[3]);
  TEST_END("ra8_xspi_direct_command packs 16-byte payload");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_flash_read_null_buf(void)
{
  TEST_BEGIN("ra8_xspi_flash_read rejects NULL buf");
  prep_flash();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_xspi_flash_read((uint8_t)k_test_xspi_valid_inst0,
                                     (uint32_t)k_test_xspi_flash_addr_start,
                                     nullptr,
                                     (uint32_t)k_test_xspi_len_small));
  TEST_END("ra8_xspi_flash_read rejects NULL buf");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_flash_read_len_zero(void)
{
  TEST_BEGIN("ra8_xspi_flash_read rejects len=0");
  prep_flash();
  uint8_t buf[16] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_xspi_flash_read((uint8_t)k_test_xspi_valid_inst0,
                                     (uint32_t)k_test_xspi_flash_addr_start,
                                     buf,
                                     (uint32_t)k_test_xspi_len_zero));
  TEST_END("ra8_xspi_flash_read rejects len=0");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_flash_read_too_large(void)
{
  TEST_BEGIN("ra8_xspi_flash_read rejects len > max");
  prep_flash();
  uint8_t buf[16] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_xspi_flash_read((uint8_t)k_test_xspi_valid_inst0,
                                     (uint32_t)k_test_xspi_flash_addr_start,
                                     buf,
                                     (uint32_t)k_test_xspi_len_too_big));
  TEST_END("ra8_xspi_flash_read rejects len > max");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_flash_read_bad_instance(void)
{
  TEST_BEGIN("ra8_xspi_flash_read rejects bad instance");
  prep_flash();
  uint8_t buf[16] = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_xspi_flash_read((uint8_t)k_test_xspi_bad_instance,
                                     (uint32_t)k_test_xspi_flash_addr_start,
                                     buf,
                                     (uint32_t)k_test_xspi_len_small));
  TEST_END("ra8_xspi_flash_read rejects bad instance");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_flash_read_address_overflow(void)
{
  TEST_BEGIN("ra8_xspi_flash_read rejects addr past 3-byte space");
  prep_flash();
  uint8_t buf[16] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_xspi_flash_read((uint8_t)k_test_xspi_valid_inst0,
                                     (uint32_t)k_test_xspi_flash_addr_overflow,
                                     buf,
                                     (uint32_t)k_test_xspi_len_small));
  TEST_END("ra8_xspi_flash_read rejects addr past 3-byte space");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_flash_read_past_end(void)
{
  TEST_BEGIN("ra8_xspi_flash_read rejects addr + len past 3-byte space");
  prep_flash();
  uint8_t buf[16] = {};
  /* addr itself is addressable (8 bytes below 2^24) but the 16-byte
   * window runs past the last 3-byte-addressable byte. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_xspi_flash_read((uint8_t)k_test_xspi_valid_inst0,
                                     (uint32_t)k_test_xspi_flash_addr_near_top,
                                     buf,
                                     (uint32_t)k_test_xspi_len_small));
  TEST_END("ra8_xspi_flash_read rejects addr + len past 3-byte space");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_flash_read_status_happy(void)
{
  TEST_BEGIN("ra8_xspi_flash_read_status happy");
  prep_flash();

  uint8_t status = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_flash_read_status((uint8_t)k_test_xspi_valid_inst0, &status));
  TEST_ASSERT_EQ(k_test_xspi_expected_status, status);
  TEST_END("ra8_xspi_flash_read_status happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_flash_read_status_null(void)
{
  TEST_BEGIN("ra8_xspi_flash_read_status rejects NULL out");
  prep_flash();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_xspi_flash_read_status((uint8_t)k_test_xspi_valid_inst0, nullptr));
  TEST_END("ra8_xspi_flash_read_status rejects NULL out");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_flash_read_status_bad_instance(void)
{
  TEST_BEGIN("ra8_xspi_flash_read_status rejects bad instance");
  prep_flash();
  uint8_t status = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_xspi_flash_read_status((uint8_t)k_test_xspi_bad_instance, &status));
  TEST_END("ra8_xspi_flash_read_status rejects bad instance");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_flash_read_id_happy(void)
{
  TEST_BEGIN("ra8_xspi_flash_read_id happy");
  prep_flash();

  uint32_t id = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_flash_read_id((uint8_t)k_test_xspi_valid_inst0, &id));
  TEST_ASSERT_EQ(k_test_xspi_expected_jedec, id);
  TEST_END("ra8_xspi_flash_read_id happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_flash_read_id_null(void)
{
  TEST_BEGIN("ra8_xspi_flash_read_id rejects NULL out");
  prep_flash();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_xspi_flash_read_id((uint8_t)k_test_xspi_valid_inst0, nullptr));
  TEST_END("ra8_xspi_flash_read_id rejects NULL out");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_flash_read_id_bad_instance(void)
{
  TEST_BEGIN("ra8_xspi_flash_read_id rejects bad instance");
  prep_flash();
  uint32_t id = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_xspi_flash_read_id((uint8_t)k_test_xspi_bad_instance, &id));
  TEST_END("ra8_xspi_flash_read_id rejects bad instance");
}

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Order is significant: cases run top to bottom, exactly as before.
 */
static void (*const s_test_roster[])(void) = {
  test_init_inst0_happy,
  test_init_inst1_happy,
  test_init_bad_instance,
  test_direct_command_null_buf,
  test_direct_command_too_long,
  test_direct_command_bad_instance,
  test_direct_command_packs_aligned,
  test_direct_command_packs_unaligned,
  test_direct_command_len_zero,
  test_direct_command_full_16,
  test_flash_read_null_buf,
  test_flash_read_len_zero,
  test_flash_read_too_large,
  test_flash_read_bad_instance,
  test_flash_read_address_overflow,
  test_flash_read_past_end,
  test_flash_read_status_happy,
  test_flash_read_status_null,
  test_flash_read_status_bad_instance,
  test_flash_read_id_happy,
  test_flash_read_id_null,
  test_flash_read_id_bad_instance,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK ] test_ra8_xspi.c\n");
  return 0;
}
