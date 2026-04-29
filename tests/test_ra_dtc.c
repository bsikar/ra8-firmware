/**
 * @file test_ra_dtc.c
 * @brief Unit tests for ra_dtc.c (Data Transfer Controller)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_dtc_regs.h"
#include "ra_dtc.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uintptr_t {
  k_ra_dtc_test_vector_addr  = 0x22000400UL, /**< Arbitrary SRAM-region pointer. */
  k_ra_dtc_test_vector_addr2 = 0x22000800UL, /**< Secondary reconfig target.     */
} ra_dtc_test_addr_t;

/* Compile-time check: the regs block matches FSP R_DTC_Type
 * (size = 0x30 / 48 bytes per RA8D2 CMSIS R_DTC_Type). */
static_assert(sizeof(r_dtc_regs_t) == 0x30U, "r_dtc_regs_t must be 48 bytes (FSP R_DTC_Type)");
static_assert(sizeof(r_dtc_xfer_info_t) == 16U, "r_dtc_xfer_info_t must be 16 bytes (HUM 18.2)");

static uint32_t s_dtc_cb_count;
static uint16_t s_dtc_cb_last_mask;

static void stub_dtc_cb(void* ctx, uint16_t mask)
{
  (void)ctx;
  ++s_dtc_cb_count;
  s_dtc_cb_last_mask = mask;
}

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_dtc_cb_count     = 0U;
  s_dtc_cb_last_mask = 0U;
}

static void test_init_null_vector(void)
{
  TEST_BEGIN("dtc init null vector");
  prep();
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_dtc_init(nullptr));
  TEST_END("dtc init null vector");
}

static void test_init_happy(void)
{
  TEST_BEGIN("dtc init happy");
  prep();

  void*          vec = (void*)(uintptr_t)k_ra_dtc_test_vector_addr;
  const ra_err_t err = ra_dtc_init(vec);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);

  volatile r_dtc_regs_t* reg = ra_dtc();
  TEST_ASSERT_EQ(0, (int)reg->DTCCR);
  TEST_ASSERT_EQ(0, (int)reg->DTCST);
  TEST_ASSERT_EQ((int)k_ra_dtc_test_vector_addr, (int)reg->DTCVBR);
  TEST_END("dtc init happy");
}

static void test_enable_then_disable(void)
{
  TEST_BEGIN("dtc enable then disable");
  prep();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dtc_enable());
  volatile r_dtc_regs_t* reg = ra_dtc();
  TEST_ASSERT_EQ((int)k_ra_dtcst_dtcst_msk, (int)reg->DTCST);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dtc_disable());
  TEST_ASSERT_EQ(0, (int)reg->DTCST);
  TEST_END("dtc enable then disable");
}

static void test_deinit(void)
{
  TEST_BEGIN("dtc deinit");
  prep();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dtc_init((void*)(uintptr_t)k_ra_dtc_test_vector_addr));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dtc_deinit());
  volatile r_dtc_regs_t* reg = ra_dtc();
  TEST_ASSERT_EQ(0, (int)reg->DTCVBR);
  TEST_END("dtc deinit");
}

static void test_reconfigure(void)
{
  TEST_BEGIN("dtc reconfigure");
  prep();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dtc_init((void*)(uintptr_t)k_ra_dtc_test_vector_addr));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_dtc_reconfigure(nullptr));

  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_dtc_reconfigure((void*)(uintptr_t)k_ra_dtc_test_vector_addr2));
  volatile r_dtc_regs_t* reg = ra_dtc();
  TEST_ASSERT_EQ((int)k_ra_dtc_test_vector_addr2, (int)reg->DTCVBR);
  TEST_ASSERT_EQ(0, (int)reg->DTCST);
  /* FSP-aligned: reconfigure leaves DTCCR with RRS enabled (0x18). */
  TEST_ASSERT_EQ((int)k_ra_dtccr_rrs_enable, (int)reg->DTCCR);
  TEST_END("dtc reconfigure");
}

static void test_status_read_and_clear(void)
{
  TEST_BEGIN("dtc status read + clear");
  prep();

  ra_dtc()->DTCSTS = 0xBEADU;
  uint16_t mask    = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dtc_get_status(&mask));
  TEST_ASSERT_EQ((int)0xBEADU, (int)mask);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dtc_clear_status(0x00FFU));
  TEST_ASSERT_EQ((int)(0xBEADU & ~0x00FFU), (int)ra_dtc()->DTCSTS);

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_dtc_get_status(nullptr));
  TEST_END("dtc status read + clear");
}

static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("dtc attach + dispatch");
  prep();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dtc_attach_handler(stub_dtc_cb, (void*)(uintptr_t)0xD0U));
  ra_dtc()->DTCSTS = 0xCAFEU;
  ra_dtc_dispatch();
  TEST_ASSERT_EQ((int)1, (int)s_dtc_cb_count);
  TEST_ASSERT_EQ((int)0xCAFEU, (int)s_dtc_cb_last_mask);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dtc_attach_handler(nullptr, nullptr));
  ra_dtc()->DTCSTS = 0xBEEFU;
  ra_dtc_dispatch();
  TEST_ASSERT_EQ((int)1, (int)s_dtc_cb_count);
  TEST_END("dtc attach + dispatch");
}

static void test_power_transition(void)
{
  TEST_BEGIN("dtc power transition");
  prep();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dtc_init((void*)(uintptr_t)k_ra_dtc_test_vector_addr));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dtc_enter_stop());
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dtc_exit_stop());
  TEST_END("dtc power transition");
}

int32_t main(void)
{
  test_init_null_vector();
  test_init_happy();
  test_enable_then_disable();
  test_deinit();
  test_reconfigure();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_power_transition();
  (void)fprintf(stderr, "[OK  ] test_ra_dtc.c\n");
  return 0;
}
