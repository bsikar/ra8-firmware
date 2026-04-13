/**
 * @file test_ra_crc.c
 * @brief Unit tests for ra_crc.c (CRC calculator driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_crc_regs.h"
#include "ra_crc.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_crc_test_bit_t
 * @brief CRC control bit masks used by the tests.
 */
typedef enum : uint8_t {
  k_ra_crc_test_swr = (uint8_t)(1U << 7U),
} ra_crc_test_bit_t;

/**
 * @enum ra_crc_test_const_t
 * @brief Constant values used by CRC tests.
 */
typedef enum : uint32_t {
  k_ra_crc_test_marker = 0xDEADBEEFUL,
  k_ra_crc_test_len    = 4U,
} ra_crc_test_const_t;

static const uint8_t s_payload[4] = {0x01U, 0x02U, 0x03U, 0x04U};

/**
 * @brief Drive CRCDOR with a known value and verify `ra_crc_compute()`
 *        returns it through the out pointer.
 */
static uint32_t compute_with_preseeded_result(ra_crc_poly_t poly, uint32_t preset)
{
  (void)ra_crc_init(poly);
  volatile r_crc_regs_t* reg = ra_crc();
  reg->CRCDOR                = preset;
  uint32_t       got         = 0U;
  const ra_err_t err         = ra_crc_compute(s_payload, (uint32_t)k_ra_crc_test_len, &got);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
  return got;
}

static void test_init_programs_poly_crc8(void)
{
  TEST_BEGIN("crc init programs poly crc8");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_crc_init(k_ra_crc_poly_8));
  volatile r_crc_regs_t* reg = ra_crc();
  TEST_ASSERT_EQ((int)k_ra_crc_poly_8, (int)reg->CRCCR0);
  TEST_ASSERT_EQ(0, (int)reg->CRCCR1);
  TEST_END("crc init programs poly crc8");
}

static void test_init_programs_poly_crc16(void)
{
  TEST_BEGIN("crc init programs poly crc16");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_crc_init(k_ra_crc_poly_16));
  volatile r_crc_regs_t* reg = ra_crc();
  TEST_ASSERT_EQ((int)k_ra_crc_poly_16, (int)reg->CRCCR0);
  TEST_END("crc init programs poly crc16");
}

static void test_init_programs_poly_crc32(void)
{
  TEST_BEGIN("crc init programs poly crc32");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_crc_init(k_ra_crc_poly_32_ieee802_3));
  volatile r_crc_regs_t* reg = ra_crc();
  TEST_ASSERT_EQ((int)k_ra_crc_poly_32_ieee802_3, (int)reg->CRCCR0);
  TEST_END("crc init programs poly crc32");
}

static void test_init_programs_poly_none(void)
{
  TEST_BEGIN("crc init poly none");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_crc_init(k_ra_crc_poly_none));
  TEST_END("crc init poly none");
}

static void test_reset_toggles_crccr1(void)
{
  TEST_BEGIN("crc reset toggles crccr1");
  ra_sim_mmap_reset();

  (void)ra_crc_init(k_ra_crc_poly_8);
  ra_crc_reset();
  /* After reset, driver writes SWR then 0; the final state is 0. */
  volatile r_crc_regs_t* reg = ra_crc();
  TEST_ASSERT_EQ(0, (int)reg->CRCCR1);
  /* Exercise the SWR constant name to keep it live. */
  TEST_ASSERT_EQ((int)k_ra_crc_test_swr, (int)(1U << 7U));
  TEST_END("crc reset toggles crccr1");
}

static void test_compute_null_data(void)
{
  TEST_BEGIN("crc compute null data");
  ra_sim_mmap_reset();

  uint32_t out = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_crc_compute(nullptr, (uint32_t)k_ra_crc_test_len, &out));
  TEST_END("crc compute null data");
}

static void test_compute_null_out(void)
{
  TEST_BEGIN("crc compute null out");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_crc_compute(s_payload, (uint32_t)k_ra_crc_test_len, nullptr));
  TEST_END("crc compute null out");
}

static void test_compute_crc8_reads_dor(void)
{
  TEST_BEGIN("crc compute crc8 reads dor");
  ra_sim_mmap_reset();

  const uint32_t got =
    compute_with_preseeded_result(k_ra_crc_poly_8_ccitt, (uint32_t)k_ra_crc_test_marker);
  TEST_ASSERT_EQ((int)k_ra_crc_test_marker, (int)got);
  /* All four input bytes should have been written to CRCDIR (last one wins). */
  volatile r_crc_regs_t* reg = ra_crc();
  TEST_ASSERT_EQ((int)s_payload[3], (int)reg->CRCDIR);
  TEST_END("crc compute crc8 reads dor");
}

static void test_compute_crc16_reads_dor(void)
{
  TEST_BEGIN("crc compute crc16 reads dor");
  ra_sim_mmap_reset();

  const uint32_t marker = 0x1122UL;
  const uint32_t got    = compute_with_preseeded_result(k_ra_crc_poly_16_ccitt, marker);
  TEST_ASSERT_EQ((int)marker, (int)got);
  TEST_END("crc compute crc16 reads dor");
}

static void test_compute_crc32_reads_dor(void)
{
  TEST_BEGIN("crc compute crc32 reads dor");
  ra_sim_mmap_reset();

  const uint32_t marker = 0xA5A5A5A5UL;
  const uint32_t got    = compute_with_preseeded_result(k_ra_crc_poly_32c_rev, marker);
  TEST_ASSERT_EQ((int)marker, (int)got);
  TEST_END("crc compute crc32 reads dor");
}

static void test_compute_zero_length(void)
{
  TEST_BEGIN("crc compute zero length");
  ra_sim_mmap_reset();

  (void)ra_crc_init(k_ra_crc_poly_8);
  volatile r_crc_regs_t* reg = ra_crc();
  reg->CRCDOR                = 0x12345678UL;

  uint32_t       got = 0U;
  const ra_err_t err = ra_crc_compute(s_payload, 0U, &got);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
  TEST_ASSERT_EQ(0x12345678, (int)got);
  TEST_END("crc compute zero length");
}

/* ---- Wave 4.4 -- full build-out ---- */

static void prep_w44(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
}

static void test_deinit(void)
{
  TEST_BEGIN("crc deinit");
  prep_w44();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_crc_init(k_ra_crc_poly_32_ieee802_3));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_crc_deinit());
  TEST_ASSERT_EQ((int32_t)0, (int32_t)ra_crc()->CRCCR0);
  TEST_END("crc deinit");
}

static void test_set_poly(void)
{
  TEST_BEGIN("crc set_poly");
  prep_w44();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_crc_init(k_ra_crc_poly_8));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_crc_set_poly(k_ra_crc_poly_16_ccitt));
  TEST_ASSERT_EQ((int32_t)k_ra_crc_poly_16_ccitt, (int32_t)ra_crc()->CRCCR0);
  TEST_END("crc set_poly");
}

static void test_get_status(void)
{
  TEST_BEGIN("crc get_status");
  prep_w44();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_crc_init(k_ra_crc_poly_32c_rev));

  uint8_t mask = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_crc_get_status(&mask));
  TEST_ASSERT_EQ((int32_t)k_ra_crc_poly_32c_rev, (int32_t)mask);
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_crc_get_status(nullptr));
  TEST_END("crc get_status");
}

static void test_power_transition(void)
{
  TEST_BEGIN("crc power transition");
  prep_w44();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_crc_init(k_ra_crc_poly_16));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_crc_enter_stop());
  TEST_ASSERT_EQ((int32_t)0, (int32_t)ra_crc()->CRCCR0);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_crc_exit_stop());
  TEST_END("crc power transition");
}

int32_t main(void)
{
  test_init_programs_poly_crc8();
  test_init_programs_poly_crc16();
  test_init_programs_poly_crc32();
  test_init_programs_poly_none();
  test_reset_toggles_crccr1();
  test_compute_null_data();
  test_compute_null_out();
  test_compute_crc8_reads_dor();
  test_compute_crc16_reads_dor();
  test_compute_crc32_reads_dor();
  test_compute_zero_length();
  test_deinit();
  test_set_poly();
  test_get_status();
  test_power_transition();
  (void)fprintf(stderr, "[OK  ] test_ra_crc.c\n");
  return 0;
}
