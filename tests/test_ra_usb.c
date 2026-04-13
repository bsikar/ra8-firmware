/**
 * @file test_ra_usb.c
 * @brief Unit tests for the USB driver (ra_usb.c)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_usb_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "ra_usb.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_test_usb_dcp_max_packet = 64U,
} test_usb_dcp_t;

static void test_init_fs_happy_path(void)
{
  TEST_BEGIN("ra_usb_device_init FS sets SYSCFG SCKE+USBE");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_usb_device_init(k_ra_usb_speed_fs));

  volatile r_usb_regs_t* reg   = ra_usb_fs();
  const uint16_t         scke  = (uint16_t)(1U << k_ra_syscfg_bit_scke);
  const uint16_t         usbe  = (uint16_t)(1U << k_ra_syscfg_bit_usbe);
  const uint16_t         mask  = (uint16_t)(scke | usbe);
  const uint16_t         hsbit = (uint16_t)(1U << k_ra_syscfg_bit_hse);
  TEST_ASSERT_EQ((int)mask, (int)(reg->SYSCFG & mask));
  /* HS bit must be cleared for FS instance. */
  TEST_ASSERT_EQ(0, (int)(reg->SYSCFG & hsbit));

  TEST_ASSERT_EQ(0, (int)reg->DCPCFG);
  TEST_ASSERT_EQ((int)k_test_usb_dcp_max_packet, (int)reg->DCPMAXP);
  TEST_ASSERT_EQ(0, (int)reg->DCPCTR);
  TEST_ASSERT_EQ(0, (int)reg->INTENB0);
  TEST_ASSERT_EQ(0, (int)reg->INTENB1);

  TEST_END("ra_usb_device_init FS sets SYSCFG SCKE+USBE");
}

static void test_init_hs_sets_hse(void)
{
  TEST_BEGIN("ra_usb_device_init HS sets HSE bit in SYSCFG");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_usb_device_init(k_ra_usb_speed_hs));
  volatile r_usb_regs_t* reg   = ra_usb_hs();
  const uint16_t         hsbit = (uint16_t)(1U << k_ra_syscfg_bit_hse);
  TEST_ASSERT((reg->SYSCFG & hsbit) != 0);

  TEST_END("ra_usb_device_init HS sets HSE bit in SYSCFG");
}

static void test_attach_sets_dprpu(void)
{
  TEST_BEGIN("ra_usb_device_attach true sets DPRPU");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_usb_device_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_usb_device_attach(k_ra_usb_speed_fs, true));

  volatile r_usb_regs_t* reg   = ra_usb_fs();
  const uint16_t         dpbit = (uint16_t)(1U << k_ra_syscfg_bit_dprpu);
  TEST_ASSERT((reg->SYSCFG & dpbit) != 0);

  TEST_END("ra_usb_device_attach true sets DPRPU");
}

static void test_attach_clears_dprpu(void)
{
  TEST_BEGIN("ra_usb_device_attach false clears DPRPU");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_usb_device_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_usb_device_attach(k_ra_usb_speed_fs, true));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_usb_device_attach(k_ra_usb_speed_fs, false));

  volatile r_usb_regs_t* reg   = ra_usb_fs();
  const uint16_t         dpbit = (uint16_t)(1U << k_ra_syscfg_bit_dprpu);
  TEST_ASSERT_EQ(0, (int)(reg->SYSCFG & dpbit));

  TEST_END("ra_usb_device_attach false clears DPRPU");
}

static void test_attach_hs(void)
{
  TEST_BEGIN("ra_usb_device_attach HS branch");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_usb_device_init(k_ra_usb_speed_hs));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_usb_device_attach(k_ra_usb_speed_hs, true));
  volatile r_usb_regs_t* reg   = ra_usb_hs();
  const uint16_t         dpbit = (uint16_t)(1U << k_ra_syscfg_bit_dprpu);
  TEST_ASSERT((reg->SYSCFG & dpbit) != 0);
  TEST_END("ra_usb_device_attach HS branch");
}

/* ---- Wave 6.2 -- lifecycle + status + IRQ + power ---- */

static uint32_t s_usb_cb_count;
static uint16_t s_usb_cb_last_mask;

static void stub_usb_cb(void* ctx, ra_usb_speed_t speed, uint16_t mask)
{
  (void)ctx;
  (void)speed;
  ++s_usb_cb_count;
  s_usb_cb_last_mask = mask;
}

static void prep_w62(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_usb_cb_count     = 0U;
  s_usb_cb_last_mask = 0U;
}

static void test_deinit(void)
{
  TEST_BEGIN("usb deinit");
  prep_w62();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_device_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_device_deinit(k_ra_usb_speed_fs));
  TEST_END("usb deinit");
}

static void test_status_read_and_clear(void)
{
  TEST_BEGIN("usb status read + clear");
  prep_w62();
  ra_usb_fs()->INTSTS0 = (uint16_t)0xABCDU;
  uint16_t mask        = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_get_status(k_ra_usb_speed_fs, &mask));
  TEST_ASSERT_EQ((int32_t)0xABCDU, (int32_t)mask);
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_usb_clear_status(k_ra_usb_speed_fs, (uint16_t)0x00F0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_usb_get_status(k_ra_usb_speed_fs, nullptr));
  TEST_END("usb status read + clear");
}

static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("usb attach + dispatch");
  prep_w62();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_usb_attach_handler(stub_usb_cb, (void*)(uintptr_t)0xAAU));
  ra_usb_fs()->INTSTS0 = (uint16_t)0xCAFEU;
  ra_usb_dispatch(k_ra_usb_speed_fs);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_usb_cb_count);
  TEST_ASSERT_EQ((int32_t)0xCAFEU, (int32_t)s_usb_cb_last_mask);
  TEST_END("usb attach + dispatch");
}

static void test_power_transition(void)
{
  TEST_BEGIN("usb power transition");
  prep_w62();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_device_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_enter_stop(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_exit_stop(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_enter_stop((ra_usb_speed_t)9U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_exit_stop((ra_usb_speed_t)9U));
  TEST_END("usb power transition");
}

static void test_hs_paths(void)
{
  TEST_BEGIN("usb HS paths");
  prep_w62();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_device_init(k_ra_usb_speed_hs));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_device_deinit(k_ra_usb_speed_hs));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_device_init(k_ra_usb_speed_hs));

  uint16_t mask = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_get_status(k_ra_usb_speed_hs, &mask));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_usb_clear_status(k_ra_usb_speed_hs, (uint16_t)0x0002U));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_attach_handler(stub_usb_cb, nullptr));
  ra_usb_hs()->INTSTS0 = (uint16_t)0xBABEU;
  ra_usb_dispatch(k_ra_usb_speed_hs);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_usb_cb_count);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_enter_stop(k_ra_usb_speed_hs));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_exit_stop(k_ra_usb_speed_hs));
  TEST_END("usb HS paths");
}

int32_t main(void)
{
  test_init_fs_happy_path();
  test_init_hs_sets_hse();
  test_attach_sets_dprpu();
  test_attach_clears_dprpu();
  test_attach_hs();
  test_deinit();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_power_transition();
  test_hs_paths();
  (void)fprintf(stderr, "[OK  ] test_ra_usb.c\n");
  return 0;
}
