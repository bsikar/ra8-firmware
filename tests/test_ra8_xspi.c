/**
 * @file test_ra8_xspi.c
 * @brief Unit tests for the xSPI driver (ra8_xspi.c)
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
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_xspi_bad_instance = 99U,
  k_test_xspi_valid_inst0  = 0U,
  k_test_xspi_valid_inst1  = 1U,
  k_test_xspi_too_many     = 20U,
} test_xspi_inst_t;

typedef enum : uint32_t {
  k_test_xspi_flash_addr_start    = 0U,
  k_test_xspi_flash_addr_middle   = 128U,
  k_test_xspi_flash_addr_pagetail = 250U,         /**< 6 bytes below the 256-byte page end. */
  k_test_xspi_flash_addr_overflow = 0x10000000UL, /**< Past 2^24 (3-byte space).            */
  k_test_xspi_flash_addr_near_top = 0xFFFFF8UL,   /**< 8 bytes below 2^24.                  */
  k_test_xspi_len_zero            = 0U,
  k_test_xspi_len_small           = 16U,
  k_test_xspi_len_multipage       = 320U, /**< > 256-byte page; crosses a page from addr 128. */
  k_test_xspi_len_too_big         = 8192U,
  k_test_xspi_expected_jedec      = 0x9D5A1AUL, /**< ISSI IS25LX512M (on-board).      */
  k_test_xspi_expected_status     = 0x02U,      /**< WEL=1 (model's implicit latch).  */
  k_test_xspi_wip_busy_polls      = 3U,         /**< RDSR busy polls for the retry.   */
  k_test_xspi_nth_wren            = 0U,         /**< 1st INTS wait: the WREN command. */
  k_test_xspi_nth_pp_or_se        = 1U,         /**< 2nd INTS wait: the PP / SE.      */
  k_test_xspi_nth_rdsr            = 2U,         /**< 3rd INTS wait: the WIP RDSR.     */
} test_xspi_vals_t;

/**
 * @brief Reset the sim world and install the register-level NOR model.
 *
 * @details
 * The ``ra8_sim_mmio_reset`` clears any installed poll hook, so the
 * ``ra8_sim_xspi_flash`` model (which services every ``TRREQ`` kick and
 * backs the flash data) must be re-installed afterwards, per test case.
 *
 * @pre Host test binary (``RA8_SIMULATOR_MODE`` + ``UNIT_TEST``).
 * @pre No other thread touches the xSPI registers (single-threaded test).
 * @post Register RAM is zeroed and the seam holds no armed faults.
 * @post The NOR model is installed with fully-erased backing arrays.
 *
 * @note Test-only helper; not thread-safe.
 * @since 0.1.0
 */
static void prep_flash(void)
{
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  ra8_sim_xspi_flash_install();
}

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

  uint8_t buf[4] = {0x11U, 0x22U, 0x33U, 0x44U};
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

  uint8_t buf[5] = {0x11U, 0x22U, 0x33U, 0x44U, 0x55U};
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
    src[i] = (uint8_t)(i & 0xFFU);
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
                 ra8_sim_xspi_flash_set_busy_polls((uint8_t)k_test_xspi_valid_inst0,
                                                   (uint32_t)k_ra8_sim_xspi_flash_busy_forever));

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
                 ra8_sim_xspi_flash_set_busy_polls((uint8_t)k_test_xspi_valid_inst0,
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
    src[i] = (uint8_t)(i + 0x60U);
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
    src[i] = (uint8_t)(i + 0x40U);
  }

  /* Leg 1: the WREN command never retires -> internal_flash_stage_program
   * propagates the CMDCMP timeout before any payload is staged. */
  prep_flash();
  volatile r_xspi_regs_t* reg = ra8_xspi((uint8_t)k_test_xspi_valid_inst0);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_sim_mmio_fail_nth_wait((const volatile void*)&reg->INTS, (uint32_t)k_test_xspi_nth_wren));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_xspi_flash_program((uint8_t)k_test_xspi_valid_inst0,
                                        (uint32_t)k_test_xspi_flash_addr_middle,
                                        src,
                                        (uint32_t)k_test_xspi_len_small));

  /* Leg 2: WREN retires but the PP kick never does ->
   * internal_flash_program_chunk propagates the kick timeout. */
  prep_flash();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sim_mmio_fail_nth_wait((const volatile void*)&reg->INTS,
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
    ra8_sim_mmio_fail_nth_wait((const volatile void*)&reg->INTS, (uint32_t)k_test_xspi_nth_rdsr));
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
    ra8_sim_mmio_fail_nth_wait((const volatile void*)&reg->INTS, (uint32_t)k_test_xspi_nth_wren));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_xspi_flash_erase_sector((uint8_t)k_test_xspi_valid_inst0,
                                             (uint32_t)k_test_xspi_flash_addr_start));

  /* Leg 2: WREN retires but the SE kick never does. */
  prep_flash();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sim_mmio_fail_nth_wait((const volatile void*)&reg->INTS,
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_mmio_fail_wait((const volatile void*)&reg->INTS));

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

/* ---- full build-out ---- */

static uint32_t s_xspi_cb_count;
static uint32_t s_xspi_cb_last_mask;

static void stub_xspi_cb(void* ctx, uint32_t mask)
{
  (void)ctx;
  ++s_xspi_cb_count;
  s_xspi_cb_last_mask = mask;
}

static void prep_w51(void)
{
  prep_flash();
  (void)ra8_mstp_init();
  s_xspi_cb_count     = 0U;
  s_xspi_cb_last_mask = 0U;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("xspi deinit");
  prep_w51();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init((uint8_t)k_test_xspi_valid_inst0, k_ra8_xspi_lio_1s1s1s));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_deinit((uint8_t)k_test_xspi_valid_inst0));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_xspi_deinit((uint8_t)k_test_xspi_bad_instance));
  TEST_END("xspi deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_read_and_clear(void)
{
  TEST_BEGIN("xspi status read + clear");
  prep_w51();
  ra8_xspi((uint8_t)k_test_xspi_valid_inst0)->COMSTT = 0x12345678U;

  uint32_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_get_status((uint8_t)k_test_xspi_valid_inst0, &mask));
  TEST_ASSERT_EQ(0x12345678U, mask);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_clear_status((uint8_t)k_test_xspi_valid_inst0, 0xFFFFFFFFU));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_xspi_get_status((uint8_t)k_test_xspi_valid_inst0, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_xspi_clear_status((uint8_t)k_test_xspi_bad_instance, 0U));
  TEST_END("xspi status read + clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("xspi attach + dispatch");
  prep_w51();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_attach_handler((uint8_t)k_test_xspi_valid_inst0, stub_xspi_cb, nullptr));
  /* Dispatch snapshots INTS (command-complete + error flags) and
   * hands it to the callback -- match that in the host test by
   * poking the INTS backing word before dispatching. */
  ra8_xspi((uint8_t)k_test_xspi_valid_inst0)->INTS = 0xCAFEBABEUL;
  ra8_xspi_dispatch((uint8_t)k_test_xspi_valid_inst0);
  TEST_ASSERT_EQ(1, s_xspi_cb_count);
  TEST_ASSERT_EQ(0xCAFEBABEUL, s_xspi_cb_last_mask);

  ra8_xspi_dispatch((uint8_t)k_test_xspi_bad_instance);
  TEST_ASSERT_EQ(1, s_xspi_cb_count);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_xspi_attach_handler((uint8_t)k_test_xspi_bad_instance, stub_xspi_cb, nullptr));
  TEST_END("xspi attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("xspi power transition");
  prep_w51();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init((uint8_t)k_test_xspi_valid_inst0, k_ra8_xspi_lio_1s1s1s));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_enter_stop((uint8_t)k_test_xspi_valid_inst0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_exit_stop((uint8_t)k_test_xspi_valid_inst0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_xspi_enter_stop((uint8_t)k_test_xspi_bad_instance));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_xspi_exit_stop((uint8_t)k_test_xspi_bad_instance));
  TEST_END("xspi power transition");
}

/* =============================================================================
 * Sweep 6 extensions: XIP toggle, DTR, DQS calibration, suspend / resume
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_xip_set_mode_enable_then_disable(void)
{
  TEST_BEGIN("xspi set_xip_mode enable+disable");
  prep_w51();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init((uint8_t)k_test_xspi_valid_inst0, k_ra8_xspi_lio_1s8s8s));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_set_xip_mode((uint8_t)k_test_xspi_valid_inst0, true, 0xEBU, 3U));
  volatile r_xspi_regs_t* reg = ra8_xspi((uint8_t)k_test_xspi_valid_inst0);
  TEST_ASSERT_EQ(k_ra8_xspi_bmctl0_read_only, reg->BMCTL0);
  TEST_ASSERT_EQ(k_ra8_xspi_cmctlch_xipen_mask, reg->CMCTLCH[0]);
  TEST_ASSERT_EQ(k_ra8_xspi_cmctlch_xipen_mask, reg->CMCTLCH[1]);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_set_xip_mode((uint8_t)k_test_xspi_valid_inst0, false, 0xEBU, 3U));
  TEST_ASSERT_EQ(0, reg->CMCTLCH[0]);
  TEST_ASSERT_EQ(0, reg->CMCTLCH[1]);
  TEST_ASSERT_EQ(k_ra8_xspi_bmctl0_read_write, reg->BMCTL0);
  TEST_END("xspi set_xip_mode enable+disable");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_xip_set_mode_invalid_args(void)
{
  TEST_BEGIN("xspi set_xip_mode rejects bad args");
  prep_w51();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_xspi_set_xip_mode((uint8_t)k_test_xspi_bad_instance, true, 0xEBU, 3U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_xspi_set_xip_mode((uint8_t)k_test_xspi_valid_inst0, true, 0xEBU, 5U));
  TEST_END("xspi set_xip_mode rejects bad args");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_dtr_mode(void)
{
  TEST_BEGIN("xspi set_dtr_mode toggles DDREN");
  prep_w51();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init((uint8_t)k_test_xspi_valid_inst0, k_ra8_xspi_lio_8d8d8d));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_set_dtr_mode((uint8_t)k_test_xspi_valid_inst0, true));
  volatile r_xspi_regs_t* reg = ra8_xspi((uint8_t)k_test_xspi_valid_inst0);
  TEST_ASSERT_EQ(k_ra8_xspi_liocfgcs_mask_ddren,
                 (reg->LIOCFGCS[0] & k_ra8_xspi_liocfgcs_mask_ddren));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_set_dtr_mode((uint8_t)k_test_xspi_valid_inst0, false));
  TEST_ASSERT_EQ(0, (reg->LIOCFGCS[0] & k_ra8_xspi_liocfgcs_mask_ddren));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_xspi_set_dtr_mode((uint8_t)k_test_xspi_bad_instance, true));
  TEST_END("xspi set_dtr_mode toggles DDREN");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_calibrate_dqs(void)
{
  TEST_BEGIN("xspi calibrate_dqs happy / retry / timeout legs");
  prep_w51();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init((uint8_t)k_test_xspi_valid_inst0, k_ra8_xspi_lio_8d8d8d));
  volatile r_xspi_regs_t* reg = ra8_xspi((uint8_t)k_test_xspi_valid_inst0);

  /* Happy path: the CAEN auto-clear wait is seam-satisfied on its
   * first poll. The driver's own arm write leaves CAEN set in host RAM
   * (only the real controller clears it on silicon). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_calibrate_dqs((uint8_t)k_test_xspi_valid_inst0));
  TEST_ASSERT_EQ(k_ra8_xspi_ccctl0_mask_caen, (reg->CCCTLCS[0] & k_ra8_xspi_ccctl0_mask_caen));

  /* Retry leg: the phase-scan "completes" on the 3rd poll. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_mmio_satisfy_after((const volatile void*)&reg->CCCTLCS[0], 3U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_calibrate_dqs((uint8_t)k_test_xspi_valid_inst0));
  ra8_sim_mmio_reset();

  /* Timeout leg: CAEN never auto-clears -> bounded poll exhausts. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_mmio_fail_wait((const volatile void*)&reg->CCCTLCS[0]));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_xspi_calibrate_dqs((uint8_t)k_test_xspi_valid_inst0));
  ra8_sim_mmio_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_xspi_calibrate_dqs((uint8_t)k_test_xspi_bad_instance));
  TEST_END("xspi calibrate_dqs happy / retry / timeout legs");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- drives the OCTACKCR SREQ/SRDY
 * handshake timeout legs of the one-shot clock-block init, isolated
 * per wait-loop with fail-nth. Must run BEFORE any successful
 * ra8_xspi_init in this binary: the handshake is guarded by a static
 * "already inited" latch that is only set on success.)
 */
static void test_octack_handshake_timeout_legs(void)
{
  TEST_BEGIN("xspi OCTACKCR handshake timeout legs");

  /* Leg 1: OCTACKSRDY=1 never acknowledges (first wait-loop). */
  prep_w51();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sim_mmio_fail_nth_wait((const volatile void*)ra8_sys_octackcr(), 0U));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_xspi_init((uint8_t)k_test_xspi_valid_inst0, k_ra8_xspi_lio_1s1s1s));

  /* Leg 2: OCTACKSRDY=0 never acknowledges (second wait-loop). */
  prep_w51();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sim_mmio_fail_nth_wait((const volatile void*)ra8_sys_octackcr(), 1U));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_xspi_init((uint8_t)k_test_xspi_valid_inst0, k_ra8_xspi_lio_1s1s1s));

  TEST_END("xspi OCTACKCR handshake timeout legs");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_suspend_resume(void)
{
  TEST_BEGIN("xspi suspend + resume");
  prep_w51();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init((uint8_t)k_test_xspi_valid_inst0, k_ra8_xspi_lio_1s1s1s));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_suspend((uint8_t)k_test_xspi_valid_inst0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_resume((uint8_t)k_test_xspi_valid_inst0));
  TEST_END("xspi suspend + resume");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_suspend_resume_null(void)
{
  TEST_BEGIN("xspi suspend / resume reject bad instance");
  prep_w51();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_xspi_suspend((uint8_t)k_test_xspi_bad_instance));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_xspi_resume((uint8_t)k_test_xspi_bad_instance));
  TEST_END("xspi suspend / resume reject bad instance");
}

/**
 * @enum test_xspi_mcdc_t
 * @brief Numeric vectors used by the MC/DC tests below.
 */
typedef enum : uint8_t {
  k_test_xspi_addr_bytes_3    = 3U, /**< Valid 24-bit address mode.   */
  k_test_xspi_addr_bytes_4    = 4U, /**< Valid 32-bit address mode.   */
  k_test_xspi_addr_bytes_bad  = 5U, /**< Neither 3 nor 4: rejected.   */
  k_test_xspi_reset_bytes_1s  = 1U, /**< Reset cmd width for 1S mode. */
  k_test_xspi_reset_bytes_8d  = 2U, /**< Reset cmd width for 8D mode. */
  k_test_xspi_reset_bytes_bad = 3U, /**< Neither 1 nor 2: rejected.   */
  k_test_xspi_xip_read_cmd    = 0xEBU,
} test_xspi_mcdc_t;

/**
 * @test test_set_xip_mode_mcdc_addr_bytes
 *
 * @par MC/DC:
 * Decision: `if ((addr_bytes != k_ra8_xspi_addr_bytes_3) &&
 *               (addr_bytes != k_ra8_xspi_addr_bytes_4))`
 * (2 conditions, libs/ra8_hal/src/ra8_xspi.c line 889)
 * - Vector 1: addr_bytes=3 -> false (control: C1 short-circuits to F)
 * - Vector 2: addr_bytes=4 -> false (varies C2 only; C1 held T)
 * - Vector 3: addr_bytes=5 -> true  (varies C1 vs vec1; both true)
 * Vectors 1+3 prove `addr_bytes != 3` independently flips outcome
 * (with C2 held implicitly true since 5 != 4 and 3 trivially != 4 is
 * unreachable -- C2 not evaluated when C1=F, so the masking pair is
 * vec1 (decision F) vs vec3 (decision T) varying C1). Vectors 2+3
 * prove `addr_bytes != 4` independently flips outcome with C1 held T.
 * N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 */
static void test_set_xip_mode_mcdc_addr_bytes(void)
{
  TEST_BEGIN("xspi set_xip_mode MC/DC: addr_bytes != 3 && addr_bytes != 4");
  prep_w51();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init((uint8_t)k_test_xspi_valid_inst0, k_ra8_xspi_lio_1s8s8s));

  /* Vector 1: addr_bytes=3. C1=(3!=3)=F short-circuits. Decision F.
   * Function proceeds to programme XIP and returns ok. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_set_xip_mode((uint8_t)k_test_xspi_valid_inst0,
                                       false,
                                       (uint8_t)k_test_xspi_xip_read_cmd,
                                       (uint8_t)k_test_xspi_addr_bytes_3));

  /* Vector 2: addr_bytes=4. C1=(4!=3)=T, C2=(4!=4)=F. Decision F.
   * Function returns ok. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_set_xip_mode((uint8_t)k_test_xspi_valid_inst0,
                                       false,
                                       (uint8_t)k_test_xspi_xip_read_cmd,
                                       (uint8_t)k_test_xspi_addr_bytes_4));

  /* Vector 3: addr_bytes=5. C1=(5!=3)=T, C2=(5!=4)=T. Decision T,
   * function returns invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_xspi_set_xip_mode((uint8_t)k_test_xspi_valid_inst0,
                                       false,
                                       (uint8_t)k_test_xspi_xip_read_cmd,
                                       (uint8_t)k_test_xspi_addr_bytes_bad));
  TEST_END("xspi set_xip_mode MC/DC: addr_bytes != 3 && addr_bytes != 4");
}

/**
 * @test test_software_reset_mcdc_cmd_bytes
 *
 * @par MC/DC:
 * Decision: `if ((cmd_bytes != (uint8_t)k_ra8_xspi_reset_cmd_bytes_1s) &&
 *               (cmd_bytes != (uint8_t)k_ra8_xspi_reset_cmd_bytes_8d))`
 * (2 conditions, libs/ra8_hal/src/ra8_xspi.c line 1026)
 * - Vector 1: cmd_bytes=1 -> false (C1=(1!=1)=F short-circuits)
 * - Vector 2: cmd_bytes=2 -> false (C1=(2!=1)=T, C2=(2!=2)=F)
 * - Vector 3: cmd_bytes=3 -> true  (C1=T, C2=T -> invalid_arg)
 * Vectors 1+3 vary C1 (decision flips); vectors 2+3 vary C2 (decision
 * flips). N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 */
static void test_software_reset_mcdc_cmd_bytes(void)
{
  TEST_BEGIN("xspi software_reset MC/DC: cmd_bytes != 1 && cmd_bytes != 2");
  prep_w51();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init((uint8_t)k_test_xspi_valid_inst0, k_ra8_xspi_lio_1s1s1s));

  /* Vector 1: cmd_bytes=1 (1S-1S-1S reset width). Decision F, function
   * proceeds and issues RSTEN + RST opcodes; returns ok. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_xspi_software_reset((uint8_t)k_test_xspi_valid_inst0, (uint8_t)k_test_xspi_reset_bytes_1s));

  /* Vector 2: cmd_bytes=2 (8D-8D-8D reset width). Decision F, ok. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_xspi_software_reset((uint8_t)k_test_xspi_valid_inst0, (uint8_t)k_test_xspi_reset_bytes_8d));

  /* Vector 3: cmd_bytes=3. Decision T, returns invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_xspi_software_reset((uint8_t)k_test_xspi_valid_inst0,
                                         (uint8_t)k_test_xspi_reset_bytes_bad));
  TEST_END("xspi software_reset MC/DC: cmd_bytes != 1 && cmd_bytes != 2");
}

int32_t main(void)
{
  /* Runs first: the OCTACKCR handshake is a one-shot latch that only
   * sets on success, so its timeout legs are reachable only before the
   * first successful ra8_xspi_init in this process. */
  test_octack_handshake_timeout_legs();
  test_init_inst0_happy();
  test_init_inst1_happy();
  test_init_bad_instance();
  test_direct_command_null_buf();
  test_direct_command_too_long();
  test_direct_command_bad_instance();
  test_direct_command_packs_aligned();
  test_direct_command_packs_unaligned();
  test_direct_command_len_zero();
  test_direct_command_full_16();
  test_flash_read_null_buf();
  test_flash_read_len_zero();
  test_flash_read_too_large();
  test_flash_read_bad_instance();
  test_flash_read_address_overflow();
  test_flash_read_past_end();
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
  test_flash_read_status_happy();
  test_flash_read_status_null();
  test_flash_read_status_bad_instance();
  test_flash_read_id_happy();
  test_deinit();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_power_transition();
  test_flash_read_id_null();
  test_flash_read_id_bad_instance();
  test_xip_set_mode_enable_then_disable();
  test_xip_set_mode_invalid_args();
  test_set_dtr_mode();
  test_calibrate_dqs();
  test_suspend_resume();
  test_suspend_resume_null();
  test_set_xip_mode_mcdc_addr_bytes();
  test_software_reset_mcdc_cmd_bytes();
  (void)fprintf(stderr, "[OK ] test_ra8_xspi.c\n");
  return 0;
}
