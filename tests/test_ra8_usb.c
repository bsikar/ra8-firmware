/**
 * @file test_ra8_usb.c
 * @brief Unit tests for the native USB device-mode driver (ra8_usb.c)
 *
 * @details
 * This sibling owns the core happy-path / error-rejection contract tests
 * (init, attach, status, endpoints, FIFO queue paths). The MC/DC vector
 * tests for the compound boolean decisions in ra8_usb.c live in
 * test_ra8_usb_mcdc.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "ra8_usb.h"
#include "ra8_usb_regs.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_test_usb_dcp_max_packet = 64U, /**< Test USB dcp maximum packet. */
} test_usb_dcp_t;

static void prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_fs_happy_path(void)
{
  TEST_BEGIN("ra8_usb_device_init FS sets SYSCFG SCKE+USBE + IRQ enables");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  volatile r_usb_regs_t* reg   = ra8_usb_fs();
  const uint16_t         scke  = (uint16_t)(1U << k_ra8_syscfg_bit_scke);
  const uint16_t         usbe  = (uint16_t)(1U << k_ra8_syscfg_bit_usbe);
  const uint16_t         mask  = (uint16_t)(scke | usbe);
  const uint16_t         hsbit = (uint16_t)(1U << k_ra8_syscfg_bit_hse);
  TEST_ASSERT_EQ(mask, (reg->SYSCFG & mask));
  TEST_ASSERT_EQ(0, (reg->SYSCFG & hsbit));

  TEST_ASSERT_EQ(0, reg->DCPCFG);
  TEST_ASSERT_EQ(k_test_usb_dcp_max_packet, reg->DCPMAXP);
  TEST_ASSERT_EQ(0, reg->DCPCTR);
  /* INTENB0 now carries the device-mode interrupt set. */
  TEST_ASSERT((reg->INTENB0 & (uint16_t)(1U << k_ra8_int0_bit_brdy)) != 0);
  TEST_ASSERT((reg->INTENB0 & (uint16_t)(1U << k_ra8_int0_bit_bemp)) != 0);
  TEST_ASSERT((reg->INTENB0 & (uint16_t)(1U << k_ra8_int0_bit_ctrt)) != 0);
  TEST_ASSERT((reg->INTENB0 & (uint16_t)(1U << k_ra8_int0_bit_dvst)) != 0);
  TEST_ASSERT_EQ(0, reg->INTENB1);

  TEST_END("ra8_usb_device_init FS sets SYSCFG SCKE+USBE + IRQ enables");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_hs_sets_hse(void)
{
  TEST_BEGIN("ra8_usb_device_init HS sets HSE bit in SYSCFG");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_hs));
  volatile r_usb_regs_t* reg   = ra8_usb_hs();
  const uint16_t         hsbit = (uint16_t)(1U << k_ra8_syscfg_bit_hse);
  TEST_ASSERT((reg->SYSCFG & hsbit) != 0);

  TEST_END("ra8_usb_device_init HS sets HSE bit in SYSCFG");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_speed(void)
{
  TEST_BEGIN("ra8_usb_device_init rejects bogus speed");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_device_init((ra8_usb_speed_t)9U));
  TEST_END("ra8_usb_device_init rejects bogus speed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_sets_dprpu(void)
{
  TEST_BEGIN("ra8_usb_device_attach true sets DPRPU");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_attach(k_ra8_usb_speed_fs, true));

  volatile r_usb_regs_t* reg   = ra8_usb_fs();
  const uint16_t         dpbit = (uint16_t)(1U << k_ra8_syscfg_bit_dprpu);
  TEST_ASSERT((reg->SYSCFG & dpbit) != 0);

  TEST_END("ra8_usb_device_attach true sets DPRPU");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_clears_dprpu(void)
{
  TEST_BEGIN("ra8_usb_device_attach false clears DPRPU");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_attach(k_ra8_usb_speed_fs, true));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_attach(k_ra8_usb_speed_fs, false));

  volatile r_usb_regs_t* reg   = ra8_usb_fs();
  const uint16_t         dpbit = (uint16_t)(1U << k_ra8_syscfg_bit_dprpu);
  TEST_ASSERT_EQ(0, (reg->SYSCFG & dpbit));

  TEST_END("ra8_usb_device_attach false clears DPRPU");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_hs(void)
{
  TEST_BEGIN("ra8_usb_device_attach HS branch");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_hs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_attach(k_ra8_usb_speed_hs, true));
  volatile r_usb_regs_t* reg   = ra8_usb_hs();
  const uint16_t         dpbit = (uint16_t)(1U << k_ra8_syscfg_bit_dprpu);
  TEST_ASSERT((reg->SYSCFG & dpbit) != 0);
  TEST_END("ra8_usb_device_attach HS branch");
}

/* ---- IRQ + status ---- */

static uint32_t s_usb_cb_count;
static uint16_t s_usb_cb_last_mask;

static void stub_usb_cb(void* ctx, ra8_usb_speed_t speed, uint16_t mask)
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

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("usb deinit");
  prep_cb();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_deinit(k_ra8_usb_speed_fs));
  /* deinit clears INTENB0 + SYSCFG. */
  TEST_ASSERT_EQ(0, ra8_usb_fs()->INTENB0);
  TEST_ASSERT_EQ(0, ra8_usb_fs()->SYSCFG);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_device_deinit((ra8_usb_speed_t)9U));
  TEST_END("usb deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_read_and_clear(void)
{
  TEST_BEGIN("usb status read + clear");
  prep_cb();
  ra8_usb_fs()->INTSTS0 = (uint16_t)0xABCDU;
  uint16_t mask         = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_get_status(k_ra8_usb_speed_fs, &mask));
  TEST_ASSERT_EQ(0xABCDU, mask);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_clear_status(k_ra8_usb_speed_fs, (uint16_t)0x00F0U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_get_status(k_ra8_usb_speed_fs, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_get_status((ra8_usb_speed_t)9U, &mask));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_clear_status((ra8_usb_speed_t)9U, 0U));
  TEST_END("usb status read + clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("usb attach + dispatch");
  prep_cb();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_attach_handler(k_ra8_usb_speed_fs, stub_usb_cb, (void*)(uintptr_t)0xAAU));
  ra8_usb_fs()->INTSTS0 = (uint16_t)0xCAFEU;
  ra8_usb_dispatch(k_ra8_usb_speed_fs);
  TEST_ASSERT_EQ(1, s_usb_cb_count);
  TEST_ASSERT_EQ(0xCAFEU, s_usb_cb_last_mask);
  TEST_END("usb attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("usb power transition");
  prep_cb();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_enter_stop(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_exit_stop(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_enter_stop((ra8_usb_speed_t)9U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_exit_stop((ra8_usb_speed_t)9U));
  TEST_END("usb power transition");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_address(void)
{
  TEST_BEGIN("ra8_usb_set_address writes USBADDR low 7 bits");
  prep_cb();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_set_address(k_ra8_usb_speed_fs, 42U));
  TEST_ASSERT_EQ(42, (ra8_usb_fs()->USBADDR & 0x7FU));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_set_address(k_ra8_usb_speed_fs, 200U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_set_address((ra8_usb_speed_t)9U, 1U));
  TEST_END("ra8_usb_set_address writes USBADDR low 7 bits");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_device_state(void)
{
  TEST_BEGIN("ra8_usb_get_device_state decodes DVSQ");
  prep_cb();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  ra8_usb_dev_state_t state = k_ra8_usb_dev_state_powered;
  ra8_usb_fs()->INTSTS0     = (uint16_t)k_ra8_dvsq_default;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_get_device_state(k_ra8_usb_speed_fs, &state));
  TEST_ASSERT_EQ(k_ra8_usb_dev_state_default, state);

  ra8_usb_fs()->INTSTS0 = (uint16_t)k_ra8_dvsq_address;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_get_device_state(k_ra8_usb_speed_fs, &state));
  TEST_ASSERT_EQ(k_ra8_usb_dev_state_address, state);

  ra8_usb_fs()->INTSTS0 = (uint16_t)k_ra8_dvsq_configured;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_get_device_state(k_ra8_usb_speed_fs, &state));
  TEST_ASSERT_EQ(k_ra8_usb_dev_state_configured, state);

  ra8_usb_fs()->INTSTS0 = (uint16_t)k_ra8_dvsq_suspend;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_get_device_state(k_ra8_usb_speed_fs, &state));
  TEST_ASSERT_EQ(k_ra8_usb_dev_state_suspended, state);

  ra8_usb_fs()->INTSTS0 = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_get_device_state(k_ra8_usb_speed_fs, &state));
  TEST_ASSERT_EQ(k_ra8_usb_dev_state_powered, state);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_get_device_state(k_ra8_usb_speed_fs, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_get_device_state((ra8_usb_speed_t)9U, &state));
  TEST_END("ra8_usb_get_device_state decodes DVSQ");
}

/**
 * @brief Rejection legs: bogus speed, pipe range, endpoint range.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void configure_endpoint_rejects_ids(void)
{
  /* Bogus speed. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_configure_endpoint((ra8_usb_speed_t)9U,
                                            1U,
                                            1U,
                                            k_ra8_usb_ep_dir_in,
                                            k_ra8_usb_ep_type_bulk,
                                            64U));
  /* Pipe out of range. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_configure_endpoint(k_ra8_usb_speed_fs,
                                            0U,
                                            1U,
                                            k_ra8_usb_ep_dir_in,
                                            k_ra8_usb_ep_type_bulk,
                                            64U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_configure_endpoint(k_ra8_usb_speed_fs,
                                            99U,
                                            1U,
                                            k_ra8_usb_ep_dir_in,
                                            k_ra8_usb_ep_type_bulk,
                                            64U));
  /* Endpoint number out of range. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_configure_endpoint(k_ra8_usb_speed_fs,
                                            1U,
                                            0U,
                                            k_ra8_usb_ep_dir_in,
                                            k_ra8_usb_ep_type_bulk,
                                            64U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_configure_endpoint(k_ra8_usb_speed_fs,
                                            1U,
                                            99U,
                                            k_ra8_usb_ep_dir_in,
                                            k_ra8_usb_ep_type_bulk,
                                            64U));
}

/** @brief Rejection legs: bogus direction, type, and max packet. */
static void configure_endpoint_rejects_shape(void)
{
  /* Bogus direction / type. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_configure_endpoint(k_ra8_usb_speed_fs,
                                            1U,
                                            1U,
                                            (ra8_usb_ep_dir_t)9U,
                                            k_ra8_usb_ep_type_bulk,
                                            64U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_configure_endpoint(k_ra8_usb_speed_fs,
                                            1U,
                                            1U,
                                            k_ra8_usb_ep_dir_in,
                                            (ra8_usb_ep_type_t)99U,
                                            64U));
  /* Bad max packet. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_configure_endpoint(k_ra8_usb_speed_fs,
                                            1U,
                                            1U,
                                            k_ra8_usb_ep_dir_in,
                                            k_ra8_usb_ep_type_bulk,
                                            0U));
}

static void test_configure_endpoint(void)
{
  TEST_BEGIN("ra8_usb_configure_endpoint validates args + writes PIPECFG");
  prep_cb();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  configure_endpoint_rejects_ids();
  configure_endpoint_rejects_shape();

  /* Valid call. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_configure_endpoint(k_ra8_usb_speed_fs,
                                            1U,
                                            5U,
                                            k_ra8_usb_ep_dir_in,
                                            k_ra8_usb_ep_type_bulk,
                                            64U));
  volatile r_usb_regs_t* reg = ra8_usb_fs();
  /* configure_endpoint deselects the pipe window (PIPESEL=0) before returning. */
  TEST_ASSERT_EQ(0U, reg->PIPESEL);
  /* PIPECFG should encode EP=5, dir=IN, type=bulk. */
  TEST_ASSERT_EQ(5U, (reg->PIPECFG & k_ra8_pipecfg_epnum_mask));
  TEST_ASSERT((reg->PIPECFG & k_ra8_pipecfg_dir_in) != 0U);
  TEST_ASSERT((reg->PIPECFG & k_ra8_pipecfg_type_mask) == (uint16_t)k_ra8_pipecfg_type_bulk);
  TEST_ASSERT_EQ(64U, reg->PIPEMAXP);
  TEST_END("ra8_usb_configure_endpoint validates args + writes PIPECFG");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_stall_endpoint(void)
{
  TEST_BEGIN("ra8_usb_stall_endpoint sets PID = STALL");
  prep_cb();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_stall_endpoint(k_ra8_usb_speed_fs, 0U));
  volatile r_usb_regs_t* reg = ra8_usb_fs();
  TEST_ASSERT_EQ(k_ra8_pid_stall, (reg->DCPCTR & k_ra8_pid_mask));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_configure_endpoint(k_ra8_usb_speed_fs,
                                            1U,
                                            1U,
                                            k_ra8_usb_ep_dir_in,
                                            k_ra8_usb_ep_type_bulk,
                                            64U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_stall_endpoint(k_ra8_usb_speed_fs, 1U));
  TEST_ASSERT_EQ(k_ra8_pid_stall, (reg->PIPECTR[0] & k_ra8_pid_mask));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_stall_endpoint(k_ra8_usb_speed_fs, 99U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_stall_endpoint((ra8_usb_speed_t)9U, 1U));
  TEST_END("ra8_usb_stall_endpoint sets PID = STALL");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_control_response(void)
{
  TEST_BEGIN("ra8_usb_control_response programs DCPCTR PID + CCPL");
  prep_cb();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_control_response(k_ra8_usb_speed_fs, true));
  volatile r_usb_regs_t* reg = ra8_usb_fs();
  TEST_ASSERT_EQ(k_ra8_pid_buf, (reg->DCPCTR & k_ra8_pid_mask));
  TEST_ASSERT((reg->DCPCTR & (uint16_t)(1U << k_ra8_dcpctr_bit_ccpl)) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_control_response(k_ra8_usb_speed_fs, false));
  TEST_ASSERT_EQ(k_ra8_pid_stall, (reg->DCPCTR & k_ra8_pid_mask));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_control_response((ra8_usb_speed_t)9U, true));
  TEST_END("ra8_usb_control_response programs DCPCTR PID + CCPL");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_setup(void)
{
  TEST_BEGIN("ra8_usb_read_setup_if_valid decodes USBREQ/USBVAL/USBINDX/USBLENG");
  prep_cb();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  ra8_usb_setup_t setup = {};
  /* No VALID flag yet -> no_data. */
  TEST_ASSERT_EQ(k_ra8_err_no_data, ra8_usb_read_setup_if_valid(k_ra8_usb_speed_fs, &setup));

  volatile r_usb_regs_t* reg = ra8_usb_fs();
  reg->INTSTS0               = (uint16_t)k_ra8_intsts0_mask_valid;
  reg->USBREQ                = (uint16_t)0x2106U; /* bRequest=0x21 bm=0x06 */
  reg->USBVAL                = (uint16_t)0x1234U;
  reg->USBINDX               = (uint16_t)0x5678U;
  reg->USBLENG               = (uint16_t)0x000AU;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_read_setup_if_valid(k_ra8_usb_speed_fs, &setup));
  TEST_ASSERT_EQ(0x06U, setup.bm_request_type);
  TEST_ASSERT_EQ(0x21U, setup.b_request);
  TEST_ASSERT_EQ(0x1234U, setup.w_value);
  TEST_ASSERT_EQ(0x5678U, setup.w_index);
  TEST_ASSERT_EQ(0x000AU, setup.w_length);
  TEST_ASSERT_EQ(0, (reg->INTSTS0 & k_ra8_intsts0_mask_valid));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_read_setup_if_valid(k_ra8_usb_speed_fs, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_read_setup_if_valid((ra8_usb_speed_t)9U, &setup));
  TEST_END("ra8_usb_read_setup_if_valid decodes USBREQ/USBVAL/USBINDX/USBLENG");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_setup_unconditional(void)
{
  TEST_BEGIN("ra8_usb_read_setup_unconditional drains latch when VALID=0");
  prep_cb();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  volatile r_usb_regs_t* reg = ra8_usb_fs();
  /* Simulate the HS race: SETUP latch is populated but the SIE has
   * already auto-cleared INTSTS0.VALID before the polled worker runs. */
  reg->INTSTS0 = (uint16_t)(reg->INTSTS0 & (uint16_t)~k_ra8_intsts0_mask_valid);
  reg->USBREQ  = (uint16_t)0x8006U; /* bRequest=0x80 bm=0x06 (GET_DESCRIPTOR) */
  reg->USBVAL  = (uint16_t)0x0100U;
  reg->USBINDX = (uint16_t)0x0000U;
  reg->USBLENG = (uint16_t)0x0040U;

  ra8_usb_setup_t setup = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_read_setup_unconditional(k_ra8_usb_speed_fs, &setup));
  TEST_ASSERT_EQ(0x06U, setup.bm_request_type);
  TEST_ASSERT_EQ(0x80U, setup.b_request);
  TEST_ASSERT_EQ(0x0100U, setup.w_value);
  TEST_ASSERT_EQ(0x0000U, setup.w_index);
  TEST_ASSERT_EQ(0x0040U, setup.w_length);
  /* VALID stays cleared after drain (W0C is a no-op when already 0). */
  TEST_ASSERT_EQ(0, (reg->INTSTS0 & k_ra8_intsts0_mask_valid));

  /* Drain a second time: the latch is unchanged (no fresh SETUP), so
   * the unconditional drain still succeeds and yields the same bytes. */
  ra8_usb_setup_t setup_again = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_read_setup_unconditional(k_ra8_usb_speed_fs, &setup_again));
  TEST_ASSERT_EQ(0x06U, setup_again.bm_request_type);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_read_setup_unconditional(k_ra8_usb_speed_fs, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_read_setup_unconditional((ra8_usb_speed_t)9U, &setup));
  TEST_END("ra8_usb_read_setup_unconditional drains latch when VALID=0");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_queue_in_arg_validation(void)
{
  TEST_BEGIN("ra8_usb_queue_in arg validation");
  prep_cb();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  uint8_t buf[8] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_queue_in((ra8_usb_speed_t)9U, 1U, buf, 8U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_queue_in(k_ra8_usb_speed_fs, 0U, buf, 8U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_queue_in(k_ra8_usb_speed_fs, 99U, buf, 8U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_queue_in(k_ra8_usb_speed_fs, 1U, nullptr, 8U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_queue_in(k_ra8_usb_speed_fs, 1U, buf, 9999U));

  /* With FRDY pre-asserted, queue_in succeeds and writes the FIFO. */
  volatile r_usb_regs_t* reg = ra8_usb_fs();
  reg->CFIFOCTR              = (uint16_t)k_ra8_fifoctr_frdy;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_queue_in(k_ra8_usb_speed_fs, 1U, buf, 4U));
  /* CFIFOSEL has the pipe number + ISEL. */
  TEST_ASSERT_EQ(1U, (reg->CFIFOSEL & k_ra8_fifosel_curpipe));
  TEST_ASSERT((reg->CFIFOSEL & k_ra8_fifosel_isel) != 0U);
  /* CFIFOCTR has BVAL set. */
  TEST_ASSERT((reg->CFIFOCTR & k_ra8_fifoctr_bval) != 0U);
  TEST_END("ra8_usb_queue_in arg validation");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_queue_out_arg_validation(void)
{
  TEST_BEGIN("ra8_usb_queue_out arg validation");
  prep_cb();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  uint8_t  buf[8] = {};
  uint16_t len    = 8U;

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_usb_queue_out(k_ra8_usb_speed_fs, 1U, nullptr, &len, true));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_queue_out(k_ra8_usb_speed_fs, 1U, buf, nullptr, true));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_queue_out((ra8_usb_speed_t)9U, 1U, buf, &len, true));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_queue_out(k_ra8_usb_speed_fs, 0U, buf, &len, true));
  uint16_t zero = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_queue_out(k_ra8_usb_speed_fs, 1U, buf, &zero, true));

  /* With FRDY asserted but DTLN=0, queue_out reports no_data. */
  volatile r_usb_regs_t* reg = ra8_usb_fs();
  reg->CFIFOCTR              = (uint16_t)k_ra8_fifoctr_frdy;
  len                        = 8U;
  TEST_ASSERT_EQ(k_ra8_err_no_data, ra8_usb_queue_out(k_ra8_usb_speed_fs, 1U, buf, &len, true));
  TEST_ASSERT_EQ(0U, len);
  TEST_END("ra8_usb_queue_out arg validation");
}

static volatile uint16_t* test_usb_cfifoh(volatile r_usb_regs_t* reg)
{
  return (volatile uint16_t*)((uintptr_t)&reg->CFIFO + 2U);
}

static volatile uint8_t* test_usb_cfifohh(volatile r_usb_regs_t* reg)
{
  return (volatile uint8_t*)((uintptr_t)&reg->CFIFO + 3U);
}

static volatile uint32_t* test_usb_cfifo32(volatile r_usb_regs_t* reg)
{
  return (volatile uint32_t*)(uintptr_t)&reg->CFIFO;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises FIFO width/tail
 * branch coverage through the public queue_in API)
 */
static void test_queue_in_fifo_tail_paths(void)
{
  TEST_BEGIN("ra8_usb_queue_in covers FS and HS FIFO tail paths");
  prep_cb();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  uint8_t                fs_odd[3] = {0x11U, 0x22U, 0x33U};
  volatile r_usb_regs_t* freg      = ra8_usb_fs();
  freg->CFIFOCTR                   = (uint16_t)k_ra8_fifoctr_frdy;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_queue_in(k_ra8_usb_speed_fs, 1U, fs_odd, 3U));
  /* USBFS trailing-odd-byte path now writes via MBW=8 to the low CFIFO
   * byte instead of a uint16 store. The even loop stores 0x2211 first,
   * then the byte write replaces the LOW byte with 0x33, so CFIFO reads
   * back as 0x2233 in the sim (where MMIO has no DTLN model). Mirrors
   * FSP hw_usb_write_fifo8 for USBFS; see internal_fifo_write in
   * libs/ra8_hal/src/ra8_usb.c (HUM Ch 36.2.6 CFIFOSEL.MBW). */
  TEST_ASSERT_EQ(0x2233U, freg->CFIFO);
  TEST_ASSERT_EQ(0x33U, *(volatile uint8_t*)(uintptr_t)&freg->CFIFO);

  prep_cb();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_hs));
  volatile r_usb_regs_t* hreg = ra8_usb_hs();
  const uint16_t         hsel = (uint16_t)(k_ra8_fifosel_mbw_32 | k_ra8_fifosel_isel | 1U);

  uint8_t hs_head[4] = {0x10U, 0x20U, 0x30U, 0x40U};
  hreg->CFIFOCTR     = (uint16_t)k_ra8_fifoctr_frdy;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_queue_in(k_ra8_usb_speed_hs, 1U, hs_head, 4U));
  TEST_ASSERT_EQ(0x40302010U, (*test_usb_cfifo32(hreg)));
  TEST_ASSERT_EQ(hsel, hreg->CFIFOSEL);

  uint8_t hs_half_tail[2] = {0x55U, 0x66U};
  hreg->CFIFOCTR          = (uint16_t)k_ra8_fifoctr_frdy;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_queue_in(k_ra8_usb_speed_hs, 1U, hs_half_tail, 2U));
  TEST_ASSERT_EQ(0x6655U, (*test_usb_cfifoh(hreg)));
  TEST_ASSERT_EQ(hsel, hreg->CFIFOSEL);

  uint8_t hs_byte_tail[3] = {0x77U, 0x88U, 0x99U};
  hreg->CFIFOCTR          = (uint16_t)k_ra8_fifoctr_frdy;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_queue_in(k_ra8_usb_speed_hs, 1U, hs_byte_tail, 3U));
  TEST_ASSERT_EQ(0x99U, (*test_usb_cfifohh(hreg)));
  TEST_ASSERT_EQ(hsel, hreg->CFIFOSEL);

  TEST_END("ra8_usb_queue_in covers FS and HS FIFO tail paths");
}

/**
 * @brief FS leg: 1-byte and 2-byte CFIFO tail reads via queue_out.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises FIFO width/tail
 * branch coverage through the public queue_out API)
 */
static void queue_out_fs_tails(void)
{
  prep_cb();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  uint8_t                out[4]   = {};
  uint16_t               len      = 1U;
  volatile r_usb_regs_t* freg     = ra8_usb_fs();
  static const uint16_t  pipe_bit = (uint16_t)(1U << 1U);
  freg->BRDYSTS                   = pipe_bit;
  freg->CFIFOCTR                  = (uint16_t)(k_ra8_fifoctr_frdy | 1U);
  freg->CFIFO                     = (uint16_t)0xBBAAU;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_queue_out(k_ra8_usb_speed_fs, 1U, out, &len, true));
  TEST_ASSERT_EQ(1U, len);
  TEST_ASSERT_EQ(0xAAU, out[0]);

  out[0]         = 0U;
  out[1]         = 0U;
  len            = 2U;
  freg->BRDYSTS  = pipe_bit;
  freg->CFIFOCTR = (uint16_t)(k_ra8_fifoctr_frdy | 2U);
  freg->CFIFO    = (uint16_t)0xDDCCU;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_queue_out(k_ra8_usb_speed_fs, 1U, out, &len, true));
  TEST_ASSERT_EQ(2U, len);
  TEST_ASSERT_EQ(0xCCU, out[0]);
  TEST_ASSERT_EQ(0xDDU, out[1]);
}

/** @brief HS leg: 4/2/3-byte CFIFO tail reads at MBW=32 via queue_out. */
static void queue_out_hs_tails(void)
{
  uint8_t               out[4]   = {};
  uint16_t              len      = 0U;
  static const uint16_t pipe_bit = (uint16_t)(1U << 1U);
  prep_cb();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_hs));
  volatile r_usb_regs_t* hreg = ra8_usb_hs();

  out[0]                  = 0U;
  out[1]                  = 0U;
  out[2]                  = 0U;
  out[3]                  = 0U;
  len                     = 4U;
  hreg->BRDYSTS           = pipe_bit;
  hreg->CFIFOCTR          = (uint16_t)(k_ra8_fifoctr_frdy | 4U);
  *test_usb_cfifo32(hreg) = 0x44332211U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_queue_out(k_ra8_usb_speed_hs, 1U, out, &len, true));
  TEST_ASSERT_EQ(4U, len);
  TEST_ASSERT_EQ(0x11U, out[0]);
  TEST_ASSERT_EQ(0x22U, out[1]);
  TEST_ASSERT_EQ(0x33U, out[2]);
  TEST_ASSERT_EQ(0x44U, out[3]);

  /* USBHS tail read at MBW=32: internal_fifo_read_hs_tail reads CFIFO+0
   * as one uint32 and extracts the low `tail` bytes -- it does NOT read
   * CFIFOH/CFIFOHH because narrow reads on USBHS do not advance the FIFO
   * pointer reliably (HUM Ch 37.2.7; see comment in
   * internal_fifo_read_hs_tail). Seed the tail bytes via CFIFO+0
   * accordingly. */
  out[0]                  = 0U;
  out[1]                  = 0U;
  len                     = 2U;
  hreg->BRDYSTS           = pipe_bit;
  hreg->CFIFOCTR          = (uint16_t)(k_ra8_fifoctr_frdy | 2U);
  *test_usb_cfifo32(hreg) = 0x00006655U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_queue_out(k_ra8_usb_speed_hs, 1U, out, &len, true));
  TEST_ASSERT_EQ(2U, len);
  TEST_ASSERT_EQ(0x55U, out[0]);
  TEST_ASSERT_EQ(0x66U, out[1]);

  out[0]                  = 0U;
  out[1]                  = 0U;
  out[2]                  = 0U;
  len                     = 3U;
  hreg->BRDYSTS           = pipe_bit;
  hreg->CFIFOCTR          = (uint16_t)(k_ra8_fifoctr_frdy | 3U);
  *test_usb_cfifo32(hreg) = 0x00887766U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_queue_out(k_ra8_usb_speed_hs, 1U, out, &len, true));
  TEST_ASSERT_EQ(3U, len);
  TEST_ASSERT_EQ(0x66U, out[0]);
  TEST_ASSERT_EQ(0x77U, out[1]);
  TEST_ASSERT_EQ(0x88U, out[2]);
}

static void test_queue_out_fifo_tail_paths(void)
{
  TEST_BEGIN("ra8_usb_queue_out covers FS and HS FIFO tail paths");
  queue_out_fs_tails();
  queue_out_hs_tails();
  TEST_END("ra8_usb_queue_out covers FS and HS FIFO tail paths");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_hs_paths(void)
{
  TEST_BEGIN("usb HS paths");
  prep_cb();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_hs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_deinit(k_ra8_usb_speed_hs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_hs));

  uint16_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_get_status(k_ra8_usb_speed_hs, &mask));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_clear_status(k_ra8_usb_speed_hs, (uint16_t)0x0002U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_attach_handler(k_ra8_usb_speed_hs, stub_usb_cb, nullptr));
  ra8_usb_hs()->INTSTS0 = (uint16_t)0xBABEU;
  ra8_usb_dispatch(k_ra8_usb_speed_hs);
  TEST_ASSERT_EQ(1, s_usb_cb_count);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_enter_stop(k_ra8_usb_speed_hs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_exit_stop(k_ra8_usb_speed_hs));
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
  test_read_setup_unconditional();
  test_queue_in_arg_validation();
  test_queue_out_arg_validation();
  test_queue_in_fifo_tail_paths();
  test_queue_out_fifo_tail_paths();
  test_hs_paths();
  (void)fprintf(stderr, "[OK ] test_ra8_usb.c\n");
  return 0;
}
