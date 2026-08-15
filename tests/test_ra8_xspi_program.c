/**
 * @file test_ra8_xspi_program.c
 * @brief Unit tests for the xSPI flash program/erase path: page
 *        programming (round trips, page-tail clamp, multipage), sector
 *        erase, and the WIP/INTS wait-loop timeout + retry legs
 *
 * @details Split from test_ra8_xspi.c along the test-group seam; the
 * sibling test_ra8_xspi.c owns init/direct-command/read/id/status and
 * test_ra8_xspi_ctrl.c the controller lifecycle + XIP/DTR/DQS + MC/DC
 * suites. Shared fixture constants and prep_flash() live in
 * support/xspi_test_util.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_fake_xspi_flash.h"
#include "ra8_mstp.h"
#include "ra8_ospi_regs.h"
#include "ra8_system_regs.h"
#include "ra8_xspi.h"
#include "ra8_xspi_internal.h"
#include "support/xspi_test_util.h"
#include "unity_minimal.h"

/**
 * @enum xspi_program_test_lit_t
 * @brief Named constants for the register stamp patterns and literal
 *        test vectors previously inlined in this file's test bodies.
 */
typedef enum : uint32_t {
  k_xspi_program_lit_xff = 0xFFU, /**< Xspi program literal 0xFF. */
  k_xspi_program_lit_x60 = 0x60U, /**< Xspi program literal 0x60. */
  k_xspi_program_lit_x40 = 0x40U, /**< Xspi program literal 0x40. */
} xspi_program_test_lit_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_flash_program_address_overflow(void)
{
  TEST_BEGIN("ra8_xspi_flash_program rejects addr past 3-byte space");
  prep_flash();
  uint8_t src[16] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_xspi_flash_program((uint8_t)k_test_xspi_valid_inst0,
                                        (uint32_t)k_test_xspi_flash_addr_overflow,
                                        src,
                                        (uint32_t)k_test_xspi_len_small));
  TEST_END("ra8_xspi_flash_program rejects addr past 3-byte space");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_flash_program_and_read_round_trip(void)
{
  TEST_BEGIN("ra8_xspi_flash_program + read round-trip");
  prep_flash();

  /* Need to erase first so the fake flash is at 0xFF (init state). */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_flash_erase_sector((uint8_t)k_test_xspi_valid_inst0,
                                             (uint32_t)k_test_xspi_flash_addr_middle));

  uint8_t src[16];
  for (uint8_t i = 0U; i < 16U; i++) {
    src[i] = (uint8_t)(i + 0x10U);
  }
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_flash_program((uint8_t)k_test_xspi_valid_inst0,
                                        (uint32_t)k_test_xspi_flash_addr_middle,
                                        src,
                                        (uint32_t)k_test_xspi_len_small));

  uint8_t dst[16] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_flash_read((uint8_t)k_test_xspi_valid_inst0,
                                     (uint32_t)k_test_xspi_flash_addr_middle,
                                     dst,
                                     (uint32_t)k_test_xspi_len_small));
  for (uint8_t i = 0U; i < 16U; i++) {
    TEST_ASSERT_EQ(src[i], dst[i]);
  }
  TEST_END("ra8_xspi_flash_program + read round-trip");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path; no `&&` or `||` in the code under test that this case
 * touches)
 */
static void test_flash_program_and_read_multipage(void)
{
  TEST_BEGIN("ra8_xspi_flash_program + read round-trip across a page boundary");
  prep_flash();

  /* Regression for the 8-byte manual-command truncation: a transfer
   * longer than one CDBUF slot (8 bytes) -- and longer than one 256-byte
   * NOR page -- must round-trip every byte, not just the first chunk.
   * 320 bytes from addr 128 spans the 256-byte page boundary. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_flash_erase_sector((uint8_t)k_test_xspi_valid_inst0,
                                             (uint32_t)k_test_xspi_flash_addr_start));

  uint8_t src[k_test_xspi_len_multipage];
  for (uint32_t i = 0U; i < (uint32_t)k_test_xspi_len_multipage; i++) {
    src[i] = (uint8_t)(i & k_xspi_program_lit_xff);
  }
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_flash_program((uint8_t)k_test_xspi_valid_inst0,
                                        (uint32_t)k_test_xspi_flash_addr_middle,
                                        src,
                                        (uint32_t)k_test_xspi_len_multipage));

  uint8_t dst[k_test_xspi_len_multipage] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_flash_read((uint8_t)k_test_xspi_valid_inst0,
                                     (uint32_t)k_test_xspi_flash_addr_middle,
                                     dst,
                                     (uint32_t)k_test_xspi_len_multipage));
  for (uint32_t i = 0U; i < (uint32_t)k_test_xspi_len_multipage; i++) {
    TEST_ASSERT_EQ(src[i], dst[i]);
  }
  TEST_END("ra8_xspi_flash_program + read round-trip across a page boundary");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_flash_program_null_data(void)
{
  TEST_BEGIN("ra8_xspi_flash_program rejects NULL data");
  prep_flash();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_xspi_flash_program((uint8_t)k_test_xspi_valid_inst0,
                                        (uint32_t)k_test_xspi_flash_addr_start,
                                        nullptr,
                                        (uint32_t)k_test_xspi_len_small));
  TEST_END("ra8_xspi_flash_program rejects NULL data");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_flash_program_len_zero(void)
{
  TEST_BEGIN("ra8_xspi_flash_program rejects len=0");
  prep_flash();
  uint8_t src[1] = {0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_xspi_flash_program((uint8_t)k_test_xspi_valid_inst0,
                                        (uint32_t)k_test_xspi_flash_addr_start,
                                        src,
                                        (uint32_t)k_test_xspi_len_zero));
  TEST_END("ra8_xspi_flash_program rejects len=0");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_flash_program_too_large(void)
{
  TEST_BEGIN("ra8_xspi_flash_program rejects len > max");
  prep_flash();
  uint8_t src[1] = {0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_xspi_flash_program((uint8_t)k_test_xspi_valid_inst0,
                                        (uint32_t)k_test_xspi_flash_addr_start,
                                        src,
                                        (uint32_t)k_test_xspi_len_too_big));
  TEST_END("ra8_xspi_flash_program rejects len > max");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_flash_program_bad_instance(void)
{
  TEST_BEGIN("ra8_xspi_flash_program rejects bad instance");
  prep_flash();
  uint8_t src[4] = {0U};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_xspi_flash_program((uint8_t)k_test_xspi_bad_instance,
                                        (uint32_t)k_test_xspi_flash_addr_start,
                                        src,
                                        (uint32_t)k_test_xspi_len_small));
  TEST_END("ra8_xspi_flash_program rejects bad instance");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_flash_erase_happy(void)
{
  TEST_BEGIN("ra8_xspi_flash_erase_sector happy");
  prep_flash();

  /* Program something, then erase, then read back 0xFF. */
  uint8_t pattern[16];
  for (uint8_t i = 0U; i < 16U; i++) {
    pattern[i] = (uint8_t)i;
  }
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_flash_erase_sector((uint8_t)k_test_xspi_valid_inst0,
                                             (uint32_t)k_test_xspi_flash_addr_start));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_flash_program((uint8_t)k_test_xspi_valid_inst0,
                                        (uint32_t)k_test_xspi_flash_addr_start,
                                        pattern,
                                        (uint32_t)k_test_xspi_len_small));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_flash_erase_sector((uint8_t)k_test_xspi_valid_inst0,
                                             (uint32_t)k_test_xspi_flash_addr_start));

  uint8_t verify[16] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_flash_read((uint8_t)k_test_xspi_valid_inst0,
                                     (uint32_t)k_test_xspi_flash_addr_start,
                                     verify,
                                     (uint32_t)k_test_xspi_len_small));
  for (uint8_t i = 0U; i < 16U; i++) {
    TEST_ASSERT_EQ(0xFF, verify[i]);
  }
  TEST_END("ra8_xspi_flash_erase_sector happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_flash_erase_bad_instance(void)
{
  TEST_BEGIN("ra8_xspi_flash_erase_sector rejects bad instance");
  prep_flash();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_xspi_flash_erase_sector((uint8_t)k_test_xspi_bad_instance,
                                             (uint32_t)k_test_xspi_flash_addr_start));
  TEST_END("ra8_xspi_flash_erase_sector rejects bad instance");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_flash_erase_out_of_range_addr(void)
{
  TEST_BEGIN("ra8_xspi_flash_erase_sector rejects addr past 3-byte space");
  prep_flash();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_xspi_flash_erase_sector((uint8_t)k_test_xspi_valid_inst0,
                                             (uint32_t)k_test_xspi_flash_addr_overflow));
  TEST_END("ra8_xspi_flash_erase_sector rejects addr past 3-byte space");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the WIP-clear loop-exit is a
 * single-condition test of the modeled RDSR response)
 */
static void test_flash_program_wip_clear_timeout(void)
{
  TEST_BEGIN("ra8_xspi_flash_program WIP-clear timeout");
  prep_flash();

  /* The modeled flash holds WIP asserted for longer than the driver's
   * whole RDSR poll budget: the post-program WIP wait exhausts its
   * budget and the program call returns the real timeout. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fake_xspi_flash_set_busy_polls((uint8_t)k_test_xspi_valid_inst0,
                                                    (uint32_t)k_ra8_fake_xspi_flash_busy_forever));

  uint8_t src[16];
  for (uint8_t i = 0U; i < 16U; i++) {
    src[i] = (uint8_t)i;
  }
  TEST_ASSERT_EQ(k_ra8_err_timeout,
                 ra8_xspi_flash_program((uint8_t)k_test_xspi_valid_inst0,
                                        (uint32_t)k_test_xspi_flash_addr_middle,
                                        src,
                                        (uint32_t)k_test_xspi_len_small));
  TEST_END("ra8_xspi_flash_program WIP-clear timeout");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the WIP-clear loop-exit is a
 * single-condition test of the modeled RDSR response)
 */
static void test_flash_program_wip_clear_retry(void)
{
  TEST_BEGIN("ra8_xspi_flash_program WIP-clear succeeds after N polls");
  prep_flash();

  /* The modeled flash reports WIP busy for the first three RDSR polls
   * after each page-program, then idle: the poll loop's continuation
   * branch runs before the program completes, and the data must still
   * round-trip. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fake_xspi_flash_set_busy_polls((uint8_t)k_test_xspi_valid_inst0,
                                                    (uint32_t)k_test_xspi_wip_busy_polls));

  uint8_t src[16];
  for (uint8_t i = 0U; i < 16U; i++) {
    src[i] = (uint8_t)(i + 0x20U);
  }
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_flash_program((uint8_t)k_test_xspi_valid_inst0,
                                        (uint32_t)k_test_xspi_flash_addr_middle,
                                        src,
                                        (uint32_t)k_test_xspi_len_small));

  uint8_t back[16] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_flash_read((uint8_t)k_test_xspi_valid_inst0,
                                     (uint32_t)k_test_xspi_flash_addr_middle,
                                     back,
                                     (uint32_t)k_test_xspi_len_small));
  for (uint8_t i = 0U; i < 16U; i++) {
    TEST_ASSERT_EQ(src[i], back[i]);
  }
  TEST_END("ra8_xspi_flash_program WIP-clear succeeds after N polls");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- drives the single-condition
 * page-boundary clamp `chunk > page_left` in ra8_xspi_flash_program:
 * addr 250 leaves 6 bytes in the 256-byte page, so the first PP chunk
 * must clamp from 8 to 6 and the data must still round-trip intact)
 */
static void test_flash_program_page_tail_clamp(void)
{
  TEST_BEGIN("ra8_xspi_flash_program clamps a chunk at the page boundary");
  prep_flash();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_flash_erase_sector((uint8_t)k_test_xspi_valid_inst0,
                                             (uint32_t)k_test_xspi_flash_addr_start));

  uint8_t src[16];
  for (uint8_t i = 0U; i < 16U; i++) {
    src[i] = (uint8_t)(i + k_xspi_program_lit_x60);
  }
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_flash_program((uint8_t)k_test_xspi_valid_inst0,
                                        (uint32_t)k_test_xspi_flash_addr_pagetail,
                                        src,
                                        (uint32_t)k_test_xspi_len_small));

  uint8_t back[16] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_flash_read((uint8_t)k_test_xspi_valid_inst0,
                                     (uint32_t)k_test_xspi_flash_addr_pagetail,
                                     back,
                                     (uint32_t)k_test_xspi_len_small));
  for (uint8_t i = 0U; i < 16U; i++) {
    TEST_ASSERT_EQ(src[i], back[i]);
  }
  TEST_END("ra8_xspi_flash_program clamps a chunk at the page boundary");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- each vector drives the
 * single-condition CMDCMP wait-exit / error-propagation branches of
 * one command in the WREN -> PP -> RDSR chain via the fail-nth seam)
 */
static void test_flash_program_command_timeout_legs(void)
{
  TEST_BEGIN("ra8_xspi_flash_program per-command CMDCMP timeout legs");
  uint8_t src[16];
  for (uint8_t i = 0U; i < 16U; i++) {
    src[i] = (uint8_t)(i + k_xspi_program_lit_x40);
  }

  /* Leg 1: the WREN command never retires -> internal_flash_stage_program
   * propagates the CMDCMP timeout before any payload is staged. */
  prep_flash();
  volatile r_xspi_regs_t* reg = ra8_xspi((uint8_t)k_test_xspi_valid_inst0);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_fake_mmio_fail_nth_wait((const volatile void*)&reg->INTS, (uint32_t)k_test_xspi_nth_wren));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_xspi_flash_program((uint8_t)k_test_xspi_valid_inst0,
                                        (uint32_t)k_test_xspi_flash_addr_middle,
                                        src,
                                        (uint32_t)k_test_xspi_len_small));

  /* Leg 2: WREN retires but the PP kick never does ->
   * internal_flash_program_chunk propagates the kick timeout. */
  prep_flash();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fake_mmio_fail_nth_wait((const volatile void*)&reg->INTS,
                                             (uint32_t)k_test_xspi_nth_pp_or_se));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_xspi_flash_program((uint8_t)k_test_xspi_valid_inst0,
                                        (uint32_t)k_test_xspi_flash_addr_middle,
                                        src,
                                        (uint32_t)k_test_xspi_len_small));

  /* Leg 3: WREN + PP retire but the first WIP-poll RDSR command never
   * does -> internal_poll_wip_clear propagates read_status's fault. */
  prep_flash();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_fake_mmio_fail_nth_wait((const volatile void*)&reg->INTS, (uint32_t)k_test_xspi_nth_rdsr));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_xspi_flash_program((uint8_t)k_test_xspi_valid_inst0,
                                        (uint32_t)k_test_xspi_flash_addr_middle,
                                        src,
                                        (uint32_t)k_test_xspi_len_small));
  TEST_END("ra8_xspi_flash_program per-command CMDCMP timeout legs");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- each vector drives the
 * single-condition error-propagation branch after one command of the
 * WREN -> SE erase chain via the fail-nth seam)
 */
static void test_flash_erase_command_timeout_legs(void)
{
  TEST_BEGIN("ra8_xspi_flash_erase_sector per-command CMDCMP timeout legs");

  /* Leg 1: the WREN command never retires. */
  prep_flash();
  volatile r_xspi_regs_t* reg = ra8_xspi((uint8_t)k_test_xspi_valid_inst0);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_fake_mmio_fail_nth_wait((const volatile void*)&reg->INTS, (uint32_t)k_test_xspi_nth_wren));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_xspi_flash_erase_sector((uint8_t)k_test_xspi_valid_inst0,
                                             (uint32_t)k_test_xspi_flash_addr_start));

  /* Leg 2: WREN retires but the SE kick never does. */
  prep_flash();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fake_mmio_fail_nth_wait((const volatile void*)&reg->INTS,
                                             (uint32_t)k_test_xspi_nth_pp_or_se));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_xspi_flash_erase_sector((uint8_t)k_test_xspi_valid_inst0,
                                             (uint32_t)k_test_xspi_flash_addr_start));
  TEST_END("ra8_xspi_flash_erase_sector per-command CMDCMP timeout legs");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the CMDCMP wait-exit is a
 * single-condition seam consult; this drives its timeout leg for the
 * read / status / id command paths)
 */
static void test_flash_cmdcmp_timeout(void)
{
  TEST_BEGIN("ra8_xspi flash ops surface a CMDCMP timeout");
  prep_flash();

  /* Arm INTS so no CMDCMP wait ever satisfies: every command the model
   * still services at register level, but the driver's bounded poll
   * exhausts and each public operation surfaces the hardware timeout. */
  volatile r_xspi_regs_t* reg = ra8_xspi((uint8_t)k_test_xspi_valid_inst0);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)&reg->INTS));

  uint8_t buf[8] = {};
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_xspi_flash_read((uint8_t)k_test_xspi_valid_inst0,
                                     (uint32_t)k_test_xspi_flash_addr_start,
                                     buf,
                                     (uint32_t)sizeof(buf)));

  uint8_t status = 0U;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_xspi_flash_read_status((uint8_t)k_test_xspi_valid_inst0, &status));

  uint32_t id = 0U;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_xspi_flash_read_id((uint8_t)k_test_xspi_valid_inst0, &id));
  TEST_END("ra8_xspi flash ops surface a CMDCMP timeout");
}

int32_t main(void)
{
  test_flash_program_address_overflow();
  test_flash_program_and_read_round_trip();
  test_flash_program_and_read_multipage();
  test_flash_program_null_data();
  test_flash_program_len_zero();
  test_flash_program_too_large();
  test_flash_program_bad_instance();
  test_flash_erase_happy();
  test_flash_erase_bad_instance();
  test_flash_erase_out_of_range_addr();
  test_flash_program_wip_clear_timeout();
  test_flash_program_wip_clear_retry();
  test_flash_program_page_tail_clamp();
  test_flash_program_command_timeout_legs();
  test_flash_erase_command_timeout_legs();
  test_flash_cmdcmp_timeout();
  return 0;
}
