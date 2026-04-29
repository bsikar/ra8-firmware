/**
 * @file test_ra_usb.c
 * @brief Unit tests for the native USB device-mode driver (ra_usb.c)
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

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
}

static void test_init_fs_happy_path(void)
{
  TEST_BEGIN("ra_usb_device_init FS sets SYSCFG SCKE+USBE + IRQ enables");
  prep();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_usb_device_init(k_ra_usb_speed_fs));

  volatile r_usb_regs_t* reg   = ra_usb_fs();
  const uint16_t         scke  = (uint16_t)(1U << k_ra_syscfg_bit_scke);
  const uint16_t         usbe  = (uint16_t)(1U << k_ra_syscfg_bit_usbe);
  const uint16_t         mask  = (uint16_t)(scke | usbe);
  const uint16_t         hsbit = (uint16_t)(1U << k_ra_syscfg_bit_hse);
  TEST_ASSERT_EQ((int)mask, (int)(reg->SYSCFG & mask));
  TEST_ASSERT_EQ(0, (int)(reg->SYSCFG & hsbit));

  TEST_ASSERT_EQ(0, (int)reg->DCPCFG);
  TEST_ASSERT_EQ((int)k_test_usb_dcp_max_packet, (int)reg->DCPMAXP);
  TEST_ASSERT_EQ(0, (int)reg->DCPCTR);
  /* INTENB0 now carries the device-mode interrupt set. */
  TEST_ASSERT((reg->INTENB0 & (uint16_t)(1U << k_ra_int0_bit_brdy)) != 0);
  TEST_ASSERT((reg->INTENB0 & (uint16_t)(1U << k_ra_int0_bit_bemp)) != 0);
  TEST_ASSERT((reg->INTENB0 & (uint16_t)(1U << k_ra_int0_bit_ctrt)) != 0);
  TEST_ASSERT((reg->INTENB0 & (uint16_t)(1U << k_ra_int0_bit_dvst)) != 0);
  TEST_ASSERT_EQ(0, (int)reg->INTENB1);

  TEST_END("ra_usb_device_init FS sets SYSCFG SCKE+USBE + IRQ enables");
}

static void test_init_hs_sets_hse(void)
{
  TEST_BEGIN("ra_usb_device_init HS sets HSE bit in SYSCFG");
  prep();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_usb_device_init(k_ra_usb_speed_hs));
  volatile r_usb_regs_t* reg   = ra_usb_hs();
  const uint16_t         hsbit = (uint16_t)(1U << k_ra_syscfg_bit_hse);
  TEST_ASSERT((reg->SYSCFG & hsbit) != 0);

  TEST_END("ra_usb_device_init HS sets HSE bit in SYSCFG");
}

static void test_init_bad_speed(void)
{
  TEST_BEGIN("ra_usb_device_init rejects bogus speed");
  prep();
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_usb_device_init((ra_usb_speed_t)9U));
  TEST_END("ra_usb_device_init rejects bogus speed");
}

static void test_attach_sets_dprpu(void)
{
  TEST_BEGIN("ra_usb_device_attach true sets DPRPU");
  prep();

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
  prep();

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
  prep();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_usb_device_init(k_ra_usb_speed_hs));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_usb_device_attach(k_ra_usb_speed_hs, true));
  volatile r_usb_regs_t* reg   = ra_usb_hs();
  const uint16_t         dpbit = (uint16_t)(1U << k_ra_syscfg_bit_dprpu);
  TEST_ASSERT((reg->SYSCFG & dpbit) != 0);
  TEST_END("ra_usb_device_attach HS branch");
}

/* ---- IRQ + status ---- */

static uint32_t s_usb_cb_count;
static uint16_t s_usb_cb_last_mask;

static void stub_usb_cb(void* ctx, ra_usb_speed_t speed, uint16_t mask)
{
  (void)ctx;
  (void)speed;
  ++s_usb_cb_count;
  s_usb_cb_last_mask = mask;
}

static void prep_cb(void)
{
  prep();
  s_usb_cb_count     = 0U;
  s_usb_cb_last_mask = 0U;
}

static void test_deinit(void)
{
  TEST_BEGIN("usb deinit");
  prep_cb();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_device_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_device_deinit(k_ra_usb_speed_fs));
  /* deinit clears INTENB0 + SYSCFG. */
  TEST_ASSERT_EQ(0, (int)ra_usb_fs()->INTENB0);
  TEST_ASSERT_EQ(0, (int)ra_usb_fs()->SYSCFG);
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_device_deinit((ra_usb_speed_t)9U));
  TEST_END("usb deinit");
}

static void test_status_read_and_clear(void)
{
  TEST_BEGIN("usb status read + clear");
  prep_cb();
  ra_usb_fs()->INTSTS0 = (uint16_t)0xABCDU;
  uint16_t mask        = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_get_status(k_ra_usb_speed_fs, &mask));
  TEST_ASSERT_EQ((int32_t)0xABCDU, (int32_t)mask);
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_usb_clear_status(k_ra_usb_speed_fs, (uint16_t)0x00F0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_usb_get_status(k_ra_usb_speed_fs, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_get_status((ra_usb_speed_t)9U, &mask));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_clear_status((ra_usb_speed_t)9U, 0U));
  TEST_END("usb status read + clear");
}

static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("usb attach + dispatch");
  prep_cb();
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
  prep_cb();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_device_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_enter_stop(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_exit_stop(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_enter_stop((ra_usb_speed_t)9U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_exit_stop((ra_usb_speed_t)9U));
  TEST_END("usb power transition");
}

static void test_set_address(void)
{
  TEST_BEGIN("ra_usb_set_address writes USBADDR low 7 bits");
  prep_cb();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_device_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_set_address(k_ra_usb_speed_fs, 42U));
  TEST_ASSERT_EQ((int32_t)42, (int32_t)(ra_usb_fs()->USBADDR & 0x7FU));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_set_address(k_ra_usb_speed_fs, 200U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_set_address((ra_usb_speed_t)9U, 1U));
  TEST_END("ra_usb_set_address writes USBADDR low 7 bits");
}

static void test_get_device_state(void)
{
  TEST_BEGIN("ra_usb_get_device_state decodes DVSQ");
  prep_cb();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_device_init(k_ra_usb_speed_fs));

  ra_usb_dev_state_t state = k_ra_usb_dev_state_powered;
  ra_usb_fs()->INTSTS0     = (uint16_t)k_ra_dvsq_default;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_get_device_state(k_ra_usb_speed_fs, &state));
  TEST_ASSERT_EQ((int32_t)k_ra_usb_dev_state_default, (int32_t)state);

  ra_usb_fs()->INTSTS0 = (uint16_t)k_ra_dvsq_address;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_get_device_state(k_ra_usb_speed_fs, &state));
  TEST_ASSERT_EQ((int32_t)k_ra_usb_dev_state_address, (int32_t)state);

  ra_usb_fs()->INTSTS0 = (uint16_t)k_ra_dvsq_configured;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_get_device_state(k_ra_usb_speed_fs, &state));
  TEST_ASSERT_EQ((int32_t)k_ra_usb_dev_state_configured, (int32_t)state);

  ra_usb_fs()->INTSTS0 = (uint16_t)k_ra_dvsq_suspend;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_get_device_state(k_ra_usb_speed_fs, &state));
  TEST_ASSERT_EQ((int32_t)k_ra_usb_dev_state_suspended, (int32_t)state);

  ra_usb_fs()->INTSTS0 = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_get_device_state(k_ra_usb_speed_fs, &state));
  TEST_ASSERT_EQ((int32_t)k_ra_usb_dev_state_powered, (int32_t)state);

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_usb_get_device_state(k_ra_usb_speed_fs, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_get_device_state((ra_usb_speed_t)9U, &state));
  TEST_END("ra_usb_get_device_state decodes DVSQ");
}

static void test_configure_endpoint(void)
{
  TEST_BEGIN("ra_usb_configure_endpoint validates args + writes PIPECFG");
  prep_cb();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_device_init(k_ra_usb_speed_fs));

  /* Bogus speed. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_configure_endpoint((ra_usb_speed_t)9U,
                                                    1U,
                                                    1U,
                                                    k_ra_usb_ep_dir_in,
                                                    k_ra_usb_ep_type_bulk,
                                                    64U));
  /* Pipe out of range. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_configure_endpoint(k_ra_usb_speed_fs,
                                                    0U,
                                                    1U,
                                                    k_ra_usb_ep_dir_in,
                                                    k_ra_usb_ep_type_bulk,
                                                    64U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_configure_endpoint(k_ra_usb_speed_fs,
                                                    99U,
                                                    1U,
                                                    k_ra_usb_ep_dir_in,
                                                    k_ra_usb_ep_type_bulk,
                                                    64U));
  /* Endpoint number out of range. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_configure_endpoint(k_ra_usb_speed_fs,
                                                    1U,
                                                    0U,
                                                    k_ra_usb_ep_dir_in,
                                                    k_ra_usb_ep_type_bulk,
                                                    64U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_configure_endpoint(k_ra_usb_speed_fs,
                                                    1U,
                                                    99U,
                                                    k_ra_usb_ep_dir_in,
                                                    k_ra_usb_ep_type_bulk,
                                                    64U));
  /* Bogus direction / type. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_configure_endpoint(k_ra_usb_speed_fs,
                                                    1U,
                                                    1U,
                                                    (ra_usb_ep_dir_t)9U,
                                                    k_ra_usb_ep_type_bulk,
                                                    64U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_configure_endpoint(k_ra_usb_speed_fs,
                                                    1U,
                                                    1U,
                                                    k_ra_usb_ep_dir_in,
                                                    (ra_usb_ep_type_t)99U,
                                                    64U));
  /* Bad max packet. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_configure_endpoint(k_ra_usb_speed_fs,
                                                    1U,
                                                    1U,
                                                    k_ra_usb_ep_dir_in,
                                                    k_ra_usb_ep_type_bulk,
                                                    0U));

  /* Valid call. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_usb_configure_endpoint(k_ra_usb_speed_fs,
                                                    1U,
                                                    5U,
                                                    k_ra_usb_ep_dir_in,
                                                    k_ra_usb_ep_type_bulk,
                                                    64U));
  volatile r_usb_regs_t* reg = ra_usb_fs();
  TEST_ASSERT_EQ((int32_t)1U, (int32_t)reg->PIPESEL);
  /* PIPECFG should encode EP=5, dir=IN, type=bulk. */
  TEST_ASSERT_EQ((int32_t)5U, (int32_t)(reg->PIPECFG & k_ra_pipecfg_epnum_mask));
  TEST_ASSERT((reg->PIPECFG & k_ra_pipecfg_dir_in) != 0U);
  TEST_ASSERT((reg->PIPECFG & k_ra_pipecfg_type_mask) == (uint16_t)k_ra_pipecfg_type_bulk);
  TEST_ASSERT_EQ((int32_t)64U, (int32_t)reg->PIPEMAXP);
  TEST_END("ra_usb_configure_endpoint validates args + writes PIPECFG");
}

static void test_stall_endpoint(void)
{
  TEST_BEGIN("ra_usb_stall_endpoint sets PID = STALL");
  prep_cb();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_device_init(k_ra_usb_speed_fs));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_stall_endpoint(k_ra_usb_speed_fs, 0U));
  volatile r_usb_regs_t* reg = ra_usb_fs();
  TEST_ASSERT_EQ((int32_t)k_ra_pid_stall, (int32_t)(reg->DCPCTR & k_ra_pid_mask));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_usb_configure_endpoint(k_ra_usb_speed_fs,
                                                    1U,
                                                    1U,
                                                    k_ra_usb_ep_dir_in,
                                                    k_ra_usb_ep_type_bulk,
                                                    64U));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_stall_endpoint(k_ra_usb_speed_fs, 1U));
  TEST_ASSERT_EQ((int32_t)k_ra_pid_stall, (int32_t)(reg->PIPECTR[0] & k_ra_pid_mask));

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_stall_endpoint(k_ra_usb_speed_fs, 99U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_stall_endpoint((ra_usb_speed_t)9U, 1U));
  TEST_END("ra_usb_stall_endpoint sets PID = STALL");
}

static void test_control_response(void)
{
  TEST_BEGIN("ra_usb_control_response programs DCPCTR PID + CCPL");
  prep_cb();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_device_init(k_ra_usb_speed_fs));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_control_response(k_ra_usb_speed_fs, true));
  volatile r_usb_regs_t* reg = ra_usb_fs();
  TEST_ASSERT_EQ((int32_t)k_ra_pid_buf, (int32_t)(reg->DCPCTR & k_ra_pid_mask));
  TEST_ASSERT((reg->DCPCTR & (uint16_t)(1U << k_ra_dcpctr_bit_ccpl)) != 0U);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_control_response(k_ra_usb_speed_fs, false));
  TEST_ASSERT_EQ((int32_t)k_ra_pid_stall, (int32_t)(reg->DCPCTR & k_ra_pid_mask));

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_control_response((ra_usb_speed_t)9U, true));
  TEST_END("ra_usb_control_response programs DCPCTR PID + CCPL");
}

static void test_read_setup(void)
{
  TEST_BEGIN("ra_usb_read_setup decodes USBREQ/USBVAL/USBINDX/USBLENG");
  prep_cb();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_device_init(k_ra_usb_speed_fs));

  ra_usb_setup_t setup = {};
  /* No VALID flag yet -> no_data. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_no_data, (int32_t)ra_usb_read_setup(k_ra_usb_speed_fs, &setup));

  volatile r_usb_regs_t* reg = ra_usb_fs();
  reg->INTSTS0               = (uint16_t)k_ra_intsts0_mask_valid;
  reg->USBREQ                = (uint16_t)0x2106U; /* bRequest=0x21 bm=0x06 */
  reg->USBVAL                = (uint16_t)0x1234U;
  reg->USBINDX               = (uint16_t)0x5678U;
  reg->USBLENG               = (uint16_t)0x000AU;

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_read_setup(k_ra_usb_speed_fs, &setup));
  TEST_ASSERT_EQ((int32_t)0x06U, (int32_t)setup.bm_request_type);
  TEST_ASSERT_EQ((int32_t)0x21U, (int32_t)setup.b_request);
  TEST_ASSERT_EQ((int32_t)0x1234U, (int32_t)setup.w_value);
  TEST_ASSERT_EQ((int32_t)0x5678U, (int32_t)setup.w_index);
  TEST_ASSERT_EQ((int32_t)0x000AU, (int32_t)setup.w_length);
  TEST_ASSERT_EQ(0, (int)(reg->INTSTS0 & k_ra_intsts0_mask_valid));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_usb_read_setup(k_ra_usb_speed_fs, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_read_setup((ra_usb_speed_t)9U, &setup));
  TEST_END("ra_usb_read_setup decodes USBREQ/USBVAL/USBINDX/USBLENG");
}

static void test_queue_in_arg_validation(void)
{
  TEST_BEGIN("ra_usb_queue_in arg validation");
  prep_cb();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_device_init(k_ra_usb_speed_fs));

  uint8_t buf[8] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_queue_in((ra_usb_speed_t)9U, 1U, buf, 8U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_queue_in(k_ra_usb_speed_fs, 0U, buf, 8U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_queue_in(k_ra_usb_speed_fs, 99U, buf, 8U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_queue_in(k_ra_usb_speed_fs, 1U, nullptr, 8U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_queue_in(k_ra_usb_speed_fs, 1U, buf, 9999U));

  /* With FRDY pre-asserted, queue_in succeeds and writes the FIFO. */
  volatile r_usb_regs_t* reg = ra_usb_fs();
  reg->CFIFOCTR              = (uint16_t)k_ra_fifoctr_frdy;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_queue_in(k_ra_usb_speed_fs, 1U, buf, 4U));
  /* CFIFOSEL has the pipe number + ISEL. */
  TEST_ASSERT_EQ((int32_t)1U, (int32_t)(reg->CFIFOSEL & k_ra_fifosel_curpipe));
  TEST_ASSERT((reg->CFIFOSEL & k_ra_fifosel_isel) != 0U);
  /* CFIFOCTR has BVAL set. */
  TEST_ASSERT((reg->CFIFOCTR & k_ra_fifoctr_bval) != 0U);
  TEST_END("ra_usb_queue_in arg validation");
}

static void test_queue_out_arg_validation(void)
{
  TEST_BEGIN("ra_usb_queue_out arg validation");
  prep_cb();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_device_init(k_ra_usb_speed_fs));

  uint8_t  buf[8] = {};
  uint16_t len    = 8U;

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_usb_queue_out(k_ra_usb_speed_fs, 1U, nullptr, &len));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_usb_queue_out(k_ra_usb_speed_fs, 1U, buf, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_queue_out((ra_usb_speed_t)9U, 1U, buf, &len));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_queue_out(k_ra_usb_speed_fs, 0U, buf, &len));
  uint16_t zero = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_queue_out(k_ra_usb_speed_fs, 1U, buf, &zero));

  /* With FRDY asserted but DTLN=0, queue_out reports no_data. */
  volatile r_usb_regs_t* reg = ra_usb_fs();
  reg->CFIFOCTR              = (uint16_t)k_ra_fifoctr_frdy;
  len                        = 8U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_no_data,
                 (int32_t)ra_usb_queue_out(k_ra_usb_speed_fs, 1U, buf, &len));
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)len);
  TEST_END("ra_usb_queue_out arg validation");
}

static void test_hs_paths(void)
{
  TEST_BEGIN("usb HS paths");
  prep_cb();
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
  test_init_bad_speed();
  test_attach_sets_dprpu();
  test_attach_clears_dprpu();
  test_attach_hs();
  test_deinit();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_power_transition();
  test_set_address();
  test_get_device_state();
  test_configure_endpoint();
  test_stall_endpoint();
  test_control_response();
  test_read_setup();
  test_queue_in_arg_validation();
  test_queue_out_arg_validation();
  test_hs_paths();
  (void)fprintf(stderr, "[OK ] test_ra_usb.c\n");
  return 0;
}
