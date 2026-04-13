/**
 * @file test_ra_xspi.c
 * @brief Unit tests for the xSPI driver (ra_xspi.c)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8d2_ospi_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "ra_xspi.h"
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
  k_test_xspi_flash_addr_overflow = 0x10000000UL,
  k_test_xspi_len_zero            = 0U,
  k_test_xspi_len_small           = 16U,
  k_test_xspi_len_too_big         = 8192U,
  k_test_xspi_expected_jedec      = 0xC2201AUL,
  k_test_xspi_expected_status     = 0x02U, /**< WEL=1 in simulator mode. */
} test_xspi_vals_t;

static void test_init_inst0_happy(void)
{
  TEST_BEGIN("ra_xspi_init instance 0");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_xspi_init((uint8_t)k_test_xspi_valid_inst0, k_ra_xspi_lio_1s1s1s));

  volatile r_xspi_regs_t* reg = ra_xspi((uint8_t)k_test_xspi_valid_inst0);
  TEST_ASSERT_NOT_NULL(reg);
  TEST_ASSERT_EQ(0, (int)reg->WRAPCFG);
  TEST_ASSERT_EQ(0, (int)reg->COMCFG);
  TEST_ASSERT_EQ((int)k_ra_xspi_lio_1s1s1s, (int)reg->LIOCFGCS[0]);
  TEST_END("ra_xspi_init instance 0");
}

static void test_init_inst1_happy(void)
{
  TEST_BEGIN("ra_xspi_init instance 1");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_xspi_init((uint8_t)k_test_xspi_valid_inst1, k_ra_xspi_lio_1s8s8s));

  volatile r_xspi_regs_t* reg = ra_xspi((uint8_t)k_test_xspi_valid_inst1);
  TEST_ASSERT_NOT_NULL(reg);
  TEST_ASSERT_EQ((int)k_ra_xspi_lio_1s8s8s, (int)reg->LIOCFGCS[0]);
  TEST_END("ra_xspi_init instance 1");
}

static void test_init_bad_instance(void)
{
  TEST_BEGIN("ra_xspi_init rejects out-of-range instance");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_xspi_init((uint8_t)k_test_xspi_bad_instance, k_ra_xspi_lio_1s1s1s));
  TEST_END("ra_xspi_init rejects out-of-range instance");
}

static void test_direct_command_null_buf(void)
{
  TEST_BEGIN("ra_xspi_direct_command rejects NULL buf");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_xspi_direct_command((uint8_t)k_test_xspi_valid_inst0, nullptr, 4U));
  TEST_END("ra_xspi_direct_command rejects NULL buf");
}

static void test_direct_command_too_long(void)
{
  TEST_BEGIN("ra_xspi_direct_command rejects len > 16");
  ra_sim_mmap_reset();
  uint8_t buf[32] = {};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_size,
                 (int)ra_xspi_direct_command((uint8_t)k_test_xspi_valid_inst0,
                                             buf,
                                             (uint8_t)k_test_xspi_too_many));
  TEST_END("ra_xspi_direct_command rejects len > 16");
}

static void test_direct_command_bad_instance(void)
{
  TEST_BEGIN("ra_xspi_direct_command rejects bad instance");
  ra_sim_mmap_reset();
  uint8_t buf[4] = {0x01U, 0x02U, 0x03U, 0x04U};
  TEST_ASSERT_EQ((int)k_ra_err_out_of_range,
                 (int)ra_xspi_direct_command((uint8_t)k_test_xspi_bad_instance, buf, 4U));
  TEST_END("ra_xspi_direct_command rejects bad instance");
}

static void test_direct_command_packs_aligned(void)
{
  TEST_BEGIN("ra_xspi_direct_command packs 4-byte-aligned payload");
  ra_sim_mmap_reset();

  uint8_t buf[4] = {0x11U, 0x22U, 0x33U, 0x44U};
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_xspi_direct_command((uint8_t)k_test_xspi_valid_inst0, buf, 4U));

  volatile r_xspi_regs_t* reg = ra_xspi((uint8_t)k_test_xspi_valid_inst0);
  TEST_ASSERT_NOT_NULL(reg);
  TEST_ASSERT_EQ((int)0x44332211L, (int)reg->CDBUF[0]);
  TEST_END("ra_xspi_direct_command packs 4-byte-aligned payload");
}

static void test_direct_command_packs_unaligned(void)
{
  TEST_BEGIN("ra_xspi_direct_command flushes trailing partial word");
  ra_sim_mmap_reset();

  uint8_t buf[5] = {0x11U, 0x22U, 0x33U, 0x44U, 0x55U};
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_xspi_direct_command((uint8_t)k_test_xspi_valid_inst0, buf, 5U));

  volatile r_xspi_regs_t* reg = ra_xspi((uint8_t)k_test_xspi_valid_inst0);
  TEST_ASSERT_NOT_NULL(reg);
  TEST_ASSERT_EQ((int)0x44332211L, (int)reg->CDBUF[0]);
  TEST_ASSERT_EQ((int)0x55L, (int)reg->CDBUF[1]);
  TEST_END("ra_xspi_direct_command flushes trailing partial word");
}

static void test_direct_command_len_zero(void)
{
  TEST_BEGIN("ra_xspi_direct_command accepts len=0");
  ra_sim_mmap_reset();
  uint8_t buf[1] = {0U};
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_xspi_direct_command((uint8_t)k_test_xspi_valid_inst0, buf, 0U));
  TEST_END("ra_xspi_direct_command accepts len=0");
}

static void test_direct_command_full_16(void)
{
  TEST_BEGIN("ra_xspi_direct_command packs 16-byte payload");
  ra_sim_mmap_reset();

  uint8_t buf[16];
  for (uint8_t i = 0U; i < 16U; ++i) {
    buf[i] = (uint8_t)(i + 1U);
  }
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_xspi_direct_command((uint8_t)k_test_xspi_valid_inst0, buf, 16U));

  volatile r_xspi_regs_t* reg = ra_xspi((uint8_t)k_test_xspi_valid_inst0);
  TEST_ASSERT_NOT_NULL(reg);
  TEST_ASSERT_EQ((int)0x04030201L, (int)reg->CDBUF[0]);
  TEST_ASSERT_EQ((int)0x08070605L, (int)reg->CDBUF[1]);
  TEST_ASSERT_EQ((int)0x0C0B0A09L, (int)reg->CDBUF[2]);
  TEST_ASSERT_EQ((int)0x100F0E0DL, (int)reg->CDBUF[3]);
  TEST_END("ra_xspi_direct_command packs 16-byte payload");
}

static void test_flash_read_null_buf(void)
{
  TEST_BEGIN("ra_xspi_flash_read rejects NULL buf");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_xspi_flash_read((uint8_t)k_test_xspi_valid_inst0,
                                         (uint32_t)k_test_xspi_flash_addr_start,
                                         nullptr,
                                         (uint32_t)k_test_xspi_len_small));
  TEST_END("ra_xspi_flash_read rejects NULL buf");
}

static void test_flash_read_len_zero(void)
{
  TEST_BEGIN("ra_xspi_flash_read rejects len=0");
  ra_sim_mmap_reset();
  uint8_t buf[16] = {};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_xspi_flash_read((uint8_t)k_test_xspi_valid_inst0,
                                         (uint32_t)k_test_xspi_flash_addr_start,
                                         buf,
                                         (uint32_t)k_test_xspi_len_zero));
  TEST_END("ra_xspi_flash_read rejects len=0");
}

static void test_flash_read_too_large(void)
{
  TEST_BEGIN("ra_xspi_flash_read rejects len > max");
  ra_sim_mmap_reset();
  uint8_t buf[16] = {};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_xspi_flash_read((uint8_t)k_test_xspi_valid_inst0,
                                         (uint32_t)k_test_xspi_flash_addr_start,
                                         buf,
                                         (uint32_t)k_test_xspi_len_too_big));
  TEST_END("ra_xspi_flash_read rejects len > max");
}

static void test_flash_read_bad_instance(void)
{
  TEST_BEGIN("ra_xspi_flash_read rejects bad instance");
  ra_sim_mmap_reset();
  uint8_t buf[16] = {};
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_xspi_flash_read((uint8_t)k_test_xspi_bad_instance,
                                         (uint32_t)k_test_xspi_flash_addr_start,
                                         buf,
                                         (uint32_t)k_test_xspi_len_small));
  TEST_END("ra_xspi_flash_read rejects bad instance");
}

static void test_flash_read_address_overflow(void)
{
  TEST_BEGIN("ra_xspi_flash_read rejects addr >= fake-flash size");
  ra_sim_mmap_reset();
  uint8_t buf[16] = {};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_xspi_flash_read((uint8_t)k_test_xspi_valid_inst0,
                                         (uint32_t)k_test_xspi_flash_addr_overflow,
                                         buf,
                                         (uint32_t)k_test_xspi_len_small));
  TEST_END("ra_xspi_flash_read rejects addr >= fake-flash size");
}

static void test_flash_read_past_end(void)
{
  TEST_BEGIN("ra_xspi_flash_read rejects addr + len > fake-flash size");
  ra_sim_mmap_reset();
  uint8_t buf[16] = {};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_xspi_flash_read((uint8_t)k_test_xspi_valid_inst0,
                                         4090U,
                                         buf,
                                         (uint32_t)k_test_xspi_len_small));
  TEST_END("ra_xspi_flash_read rejects addr + len > fake-flash size");
}

static void test_flash_program_address_overflow(void)
{
  TEST_BEGIN("ra_xspi_flash_program rejects addr >= fake-flash size");
  ra_sim_mmap_reset();
  uint8_t src[16] = {};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_xspi_flash_program((uint8_t)k_test_xspi_valid_inst0,
                                            (uint32_t)k_test_xspi_flash_addr_overflow,
                                            src,
                                            (uint32_t)k_test_xspi_len_small));
  TEST_END("ra_xspi_flash_program rejects addr >= fake-flash size");
}

static void test_flash_program_and_read_round_trip(void)
{
  TEST_BEGIN("ra_xspi_flash_program + read round-trip");
  ra_sim_mmap_reset();

  /* Need to erase first so the fake flash is at 0xFF (init state). */
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_xspi_flash_erase_sector((uint8_t)k_test_xspi_valid_inst0,
                                                 (uint32_t)k_test_xspi_flash_addr_middle));

  uint8_t src[16];
  for (uint8_t i = 0U; i < 16U; i++) {
    src[i] = (uint8_t)(i + 0x10U);
  }
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_xspi_flash_program((uint8_t)k_test_xspi_valid_inst0,
                                            (uint32_t)k_test_xspi_flash_addr_middle,
                                            src,
                                            (uint32_t)k_test_xspi_len_small));

  uint8_t dst[16] = {};
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_xspi_flash_read((uint8_t)k_test_xspi_valid_inst0,
                                         (uint32_t)k_test_xspi_flash_addr_middle,
                                         dst,
                                         (uint32_t)k_test_xspi_len_small));
  for (uint8_t i = 0U; i < 16U; i++) {
    TEST_ASSERT_EQ((int)src[i], (int)dst[i]);
  }
  TEST_END("ra_xspi_flash_program + read round-trip");
}

static void test_flash_program_null_data(void)
{
  TEST_BEGIN("ra_xspi_flash_program rejects NULL data");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_xspi_flash_program((uint8_t)k_test_xspi_valid_inst0,
                                            (uint32_t)k_test_xspi_flash_addr_start,
                                            nullptr,
                                            (uint32_t)k_test_xspi_len_small));
  TEST_END("ra_xspi_flash_program rejects NULL data");
}

static void test_flash_program_len_zero(void)
{
  TEST_BEGIN("ra_xspi_flash_program rejects len=0");
  ra_sim_mmap_reset();
  uint8_t src[1] = {0U};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_xspi_flash_program((uint8_t)k_test_xspi_valid_inst0,
                                            (uint32_t)k_test_xspi_flash_addr_start,
                                            src,
                                            (uint32_t)k_test_xspi_len_zero));
  TEST_END("ra_xspi_flash_program rejects len=0");
}

static void test_flash_program_too_large(void)
{
  TEST_BEGIN("ra_xspi_flash_program rejects len > max");
  ra_sim_mmap_reset();
  uint8_t src[1] = {0U};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_xspi_flash_program((uint8_t)k_test_xspi_valid_inst0,
                                            (uint32_t)k_test_xspi_flash_addr_start,
                                            src,
                                            (uint32_t)k_test_xspi_len_too_big));
  TEST_END("ra_xspi_flash_program rejects len > max");
}

static void test_flash_program_bad_instance(void)
{
  TEST_BEGIN("ra_xspi_flash_program rejects bad instance");
  ra_sim_mmap_reset();
  uint8_t src[4] = {0U};
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_xspi_flash_program((uint8_t)k_test_xspi_bad_instance,
                                            (uint32_t)k_test_xspi_flash_addr_start,
                                            src,
                                            (uint32_t)k_test_xspi_len_small));
  TEST_END("ra_xspi_flash_program rejects bad instance");
}

static void test_flash_erase_happy(void)
{
  TEST_BEGIN("ra_xspi_flash_erase_sector happy");
  ra_sim_mmap_reset();

  /* Program something, then erase, then read back 0xFF. */
  uint8_t pattern[16];
  for (uint8_t i = 0U; i < 16U; i++) {
    pattern[i] = (uint8_t)i;
  }
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_xspi_flash_erase_sector((uint8_t)k_test_xspi_valid_inst0,
                                                 (uint32_t)k_test_xspi_flash_addr_start));
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_xspi_flash_program((uint8_t)k_test_xspi_valid_inst0,
                                            (uint32_t)k_test_xspi_flash_addr_start,
                                            pattern,
                                            (uint32_t)k_test_xspi_len_small));
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_xspi_flash_erase_sector((uint8_t)k_test_xspi_valid_inst0,
                                                 (uint32_t)k_test_xspi_flash_addr_start));

  uint8_t verify[16] = {};
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_xspi_flash_read((uint8_t)k_test_xspi_valid_inst0,
                                         (uint32_t)k_test_xspi_flash_addr_start,
                                         verify,
                                         (uint32_t)k_test_xspi_len_small));
  for (uint8_t i = 0U; i < 16U; i++) {
    TEST_ASSERT_EQ(0xFF, (int)verify[i]);
  }
  TEST_END("ra_xspi_flash_erase_sector happy");
}

static void test_flash_erase_bad_instance(void)
{
  TEST_BEGIN("ra_xspi_flash_erase_sector rejects bad instance");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_xspi_flash_erase_sector((uint8_t)k_test_xspi_bad_instance,
                                                 (uint32_t)k_test_xspi_flash_addr_start));
  TEST_END("ra_xspi_flash_erase_sector rejects bad instance");
}

static void test_flash_erase_out_of_range_addr(void)
{
  TEST_BEGIN("ra_xspi_flash_erase_sector rejects out-of-range addr");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_xspi_flash_erase_sector((uint8_t)k_test_xspi_valid_inst0,
                                                 (uint32_t)k_test_xspi_flash_addr_overflow));
  TEST_END("ra_xspi_flash_erase_sector rejects out-of-range addr");
}

static void test_flash_read_status_happy(void)
{
  TEST_BEGIN("ra_xspi_flash_read_status happy");
  ra_sim_mmap_reset();

  uint8_t status = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_xspi_flash_read_status((uint8_t)k_test_xspi_valid_inst0, &status));
  TEST_ASSERT_EQ((int)k_test_xspi_expected_status, (int)status);
  TEST_END("ra_xspi_flash_read_status happy");
}

static void test_flash_read_status_null(void)
{
  TEST_BEGIN("ra_xspi_flash_read_status rejects NULL out");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_xspi_flash_read_status((uint8_t)k_test_xspi_valid_inst0, nullptr));
  TEST_END("ra_xspi_flash_read_status rejects NULL out");
}

static void test_flash_read_status_bad_instance(void)
{
  TEST_BEGIN("ra_xspi_flash_read_status rejects bad instance");
  ra_sim_mmap_reset();
  uint8_t status = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_xspi_flash_read_status((uint8_t)k_test_xspi_bad_instance, &status));
  TEST_END("ra_xspi_flash_read_status rejects bad instance");
}

static void test_flash_read_id_happy(void)
{
  TEST_BEGIN("ra_xspi_flash_read_id happy");
  ra_sim_mmap_reset();

  uint32_t id = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_xspi_flash_read_id((uint8_t)k_test_xspi_valid_inst0, &id));
  TEST_ASSERT_EQ((int64_t)k_test_xspi_expected_jedec, (int64_t)id);
  TEST_END("ra_xspi_flash_read_id happy");
}

static void test_flash_read_id_null(void)
{
  TEST_BEGIN("ra_xspi_flash_read_id rejects NULL out");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_xspi_flash_read_id((uint8_t)k_test_xspi_valid_inst0, nullptr));
  TEST_END("ra_xspi_flash_read_id rejects NULL out");
}

static void test_flash_read_id_bad_instance(void)
{
  TEST_BEGIN("ra_xspi_flash_read_id rejects bad instance");
  ra_sim_mmap_reset();
  uint32_t id = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_xspi_flash_read_id((uint8_t)k_test_xspi_bad_instance, &id));
  TEST_END("ra_xspi_flash_read_id rejects bad instance");
}

/* ---- Wave 5.1 -- full build-out ---- */

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
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_xspi_cb_count     = 0U;
  s_xspi_cb_last_mask = 0U;
}

static void test_deinit(void)
{
  TEST_BEGIN("xspi deinit");
  prep_w51();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_xspi_init((uint8_t)k_test_xspi_valid_inst0, k_ra_xspi_lio_1s1s1s));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_xspi_deinit((uint8_t)k_test_xspi_valid_inst0));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_xspi_deinit((uint8_t)k_test_xspi_bad_instance));
  TEST_END("xspi deinit");
}

static void test_status_read_and_clear(void)
{
  TEST_BEGIN("xspi status read + clear");
  prep_w51();
  ra_xspi((uint8_t)k_test_xspi_valid_inst0)->COMSTT = 0x12345678U;

  uint32_t mask = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_xspi_get_status((uint8_t)k_test_xspi_valid_inst0, &mask));
  TEST_ASSERT_EQ((int32_t)0x12345678U, (int32_t)mask);
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_xspi_clear_status((uint8_t)k_test_xspi_valid_inst0, 0xFFFFFFFFU));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_xspi_get_status((uint8_t)k_test_xspi_valid_inst0, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_xspi_clear_status((uint8_t)k_test_xspi_bad_instance, 0U));
  TEST_END("xspi status read + clear");
}

static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("xspi attach + dispatch");
  prep_w51();

  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_xspi_attach_handler((uint8_t)k_test_xspi_valid_inst0, stub_xspi_cb, nullptr));
  /* Dispatch snapshots INTS (command-complete + error flags) and
   * hands it to the callback -- match that in the host test by
   * poking the INTS backing word before dispatching. */
  ra_xspi((uint8_t)k_test_xspi_valid_inst0)->INTS = 0xCAFEBABEUL;
  ra_xspi_dispatch((uint8_t)k_test_xspi_valid_inst0);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_xspi_cb_count);
  TEST_ASSERT_EQ((int32_t)0xCAFEBABEUL, (int32_t)s_xspi_cb_last_mask);

  ra_xspi_dispatch((uint8_t)k_test_xspi_bad_instance);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_xspi_cb_count);

  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_xspi_attach_handler((uint8_t)k_test_xspi_bad_instance, stub_xspi_cb, nullptr));
  TEST_END("xspi attach + dispatch");
}

static void test_power_transition(void)
{
  TEST_BEGIN("xspi power transition");
  prep_w51();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_xspi_init((uint8_t)k_test_xspi_valid_inst0, k_ra_xspi_lio_1s1s1s));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_xspi_enter_stop((uint8_t)k_test_xspi_valid_inst0));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_xspi_exit_stop((uint8_t)k_test_xspi_valid_inst0));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_xspi_enter_stop((uint8_t)k_test_xspi_bad_instance));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_xspi_exit_stop((uint8_t)k_test_xspi_bad_instance));
  TEST_END("xspi power transition");
}

int32_t main(void)
{
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
  test_flash_program_null_data();
  test_flash_program_len_zero();
  test_flash_program_too_large();
  test_flash_program_bad_instance();
  test_flash_erase_happy();
  test_flash_erase_bad_instance();
  test_flash_erase_out_of_range_addr();
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
  (void)fprintf(stderr, "[OK  ] test_ra_xspi.c\n");
  return 0;
}
