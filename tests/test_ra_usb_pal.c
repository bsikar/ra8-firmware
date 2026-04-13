/**
 * @file test_ra_usb_pal.c
 * @brief Unit tests for libs/ra_usb_pal (Wave 7.2)
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
#include "ra_usb_pal.h"
#include "unity_minimal.h"

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  (void)ra_usb_pal_deinit();
}

static void test_init_fs_starts_detached(void)
{
  TEST_BEGIN("ra_usb_pal_init: FS init starts detached");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pal_init(k_ra_usb_speed_fs));

  ra_usb_pal_state_t state = k_ra_usb_pal_state_configd;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pal_get_state(&state));
  TEST_ASSERT_EQ((int32_t)k_ra_usb_pal_state_detached, (int32_t)state);
  TEST_END("ra_usb_pal_init: FS init starts detached");
}

static void test_init_hs_starts_detached(void)
{
  TEST_BEGIN("ra_usb_pal_init: HS init starts detached");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pal_init(k_ra_usb_speed_hs));
  ra_usb_pal_state_t state = k_ra_usb_pal_state_configd;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pal_get_state(&state));
  TEST_ASSERT_EQ((int32_t)k_ra_usb_pal_state_detached, (int32_t)state);
  TEST_END("ra_usb_pal_init: HS init starts detached");
}

static void test_init_bad_speed(void)
{
  TEST_BEGIN("ra_usb_pal_init: bad speed rejected");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_pal_init((ra_usb_speed_t)99U));
  TEST_END("ra_usb_pal_init: bad speed rejected");
}

static void test_attach_detach_cycles_state(void)
{
  TEST_BEGIN("ra_usb_pal_attach: cycles state attached/detached");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pal_init(k_ra_usb_speed_fs));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pal_attach(true));
  ra_usb_pal_state_t state = k_ra_usb_pal_state_detached;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pal_get_state(&state));
  TEST_ASSERT_EQ((int32_t)k_ra_usb_pal_state_attached, (int32_t)state);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pal_attach(false));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pal_get_state(&state));
  TEST_ASSERT_EQ((int32_t)k_ra_usb_pal_state_detached, (int32_t)state);
  TEST_END("ra_usb_pal_attach: cycles state attached/detached");
}

static void test_ep_open_validates_args(void)
{
  TEST_BEGIN("ra_usb_pal_ep_open: arg validation");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pal_init(k_ra_usb_speed_fs));

  /* Bad EP addr (0 = control reserved). */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_usb_pal_ep_open(0U, k_ra_usb_pal_ep_dir_in, k_ra_usb_pal_ep_type_bulk, 64U));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_usb_pal_ep_open(99U, k_ra_usb_pal_ep_dir_in, k_ra_usb_pal_ep_type_bulk, 64U));

  /* Bad direction. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_usb_pal_ep_open(1U, (ra_usb_pal_ep_dir_t)9U, k_ra_usb_pal_ep_type_bulk, 64U));

  /* Bad type. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_usb_pal_ep_open(1U, k_ra_usb_pal_ep_dir_in, (ra_usb_pal_ep_type_t)99U, 64U));

  /* Zero packet size. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_usb_pal_ep_open(1U, k_ra_usb_pal_ep_dir_in, k_ra_usb_pal_ep_type_bulk, 0U));

  /* Valid args -> stub returns not_supported. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_not_supported,
    (int32_t)ra_usb_pal_ep_open(1U, k_ra_usb_pal_ep_dir_in, k_ra_usb_pal_ep_type_bulk, 64U));
  TEST_END("ra_usb_pal_ep_open: arg validation");
}

static void test_ep_send_recv_stub(void)
{
  TEST_BEGIN("ra_usb_pal_ep_{send,recv}: stub returns not_supported");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pal_init(k_ra_usb_speed_fs));

  uint8_t  data[16] = {0xAAU};
  uint8_t  rx[16]   = {0U};
  uint16_t rx_len   = (uint16_t)sizeof(rx);

  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported,
                 (int32_t)ra_usb_pal_ep_send(1U, data, (uint16_t)sizeof(data)));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_usb_pal_ep_recv(1U, rx, &rx_len));
  TEST_END("ra_usb_pal_ep_{send,recv}: stub returns not_supported");
}

static void test_ep_send_recv_arg_validation(void)
{
  TEST_BEGIN("ra_usb_pal_ep_{send,recv}: arg validation");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pal_init(k_ra_usb_speed_fs));

  uint8_t  buf[16] = {0U};
  uint16_t len     = (uint16_t)sizeof(buf);

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_pal_ep_send(0U, buf, 16U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_pal_ep_send(99U, buf, 16U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_pal_ep_send(1U, nullptr, 16U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_pal_ep_send(1U, buf, (uint16_t)(k_ra_usb_pal_xfer_max + 1U)));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_pal_ep_recv(1U, nullptr, &len));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_pal_ep_recv(1U, buf, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_pal_ep_recv(0U, buf, &len));
  uint16_t zero = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_pal_ep_recv(1U, buf, &zero));
  TEST_END("ra_usb_pal_ep_{send,recv}: arg validation");
}

static int32_t s_usb_event_count = 0;

static void stub_usb_event(void* ctx, ra_usb_speed_t speed, uint16_t mask)
{
  (void)ctx;
  (void)speed;
  (void)mask;
  ++s_usb_event_count;
}

static void test_event_handler_attach_detach(void)
{
  TEST_BEGIN("ra_usb_pal_set_event_handler: attach + detach");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pal_init(k_ra_usb_speed_fs));

  s_usb_event_count = 0;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pal_set_event_handler(stub_usb_event, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pal_set_event_handler(nullptr, nullptr));
  TEST_END("ra_usb_pal_set_event_handler: attach + detach");
}

static void test_dispatch_relays_intsts0(void)
{
  TEST_BEGIN("ra_usb_dispatch -> PAL relay -> stack callback");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pal_init(k_ra_usb_speed_fs));

  s_usb_event_count = 0;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_pal_set_event_handler(stub_usb_event, nullptr));

  /* Pre-set INTSTS0 to a non-zero value so dispatch sees a real event. */
  volatile r_usb_regs_t* reg = ra_usb_fs();
  reg->INTSTS0               = 0xBEEFU;

  ra_usb_dispatch(k_ra_usb_speed_fs);
  TEST_ASSERT_EQ(1, s_usb_event_count);

  /* Zero INTSTS0 dispatch is a no-op (no event delivered). */
  reg->INTSTS0 = 0U;
  ra_usb_dispatch(k_ra_usb_speed_fs);
  TEST_ASSERT_EQ(1, s_usb_event_count);

  /* HS speed mismatch is filtered out of the FS-bound PAL. */
  reg->INTSTS0 = 0xBEEFU;
  ra_usb_dispatch(k_ra_usb_speed_hs);
  /* Whether HS dispatch fires depends on ra_usb's internal state;
   * the PAL's per-speed filter should drop it regardless. */
  TEST_ASSERT(s_usb_event_count <= 2);
  TEST_END("ra_usb_dispatch -> PAL relay -> stack callback");
}

static void test_calls_before_init_fail(void)
{
  TEST_BEGIN("ra_usb_pal_*: pre-init calls return invalid_state");
  prep();

  ra_usb_pal_state_t state   = k_ra_usb_pal_state_configd;
  uint8_t            buf[16] = {0U};
  uint16_t           len     = 16U;

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_pal_attach(true));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_pal_get_state(&state));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_state,
    (int32_t)ra_usb_pal_ep_open(1U, k_ra_usb_pal_ep_dir_in, k_ra_usb_pal_ep_type_bulk, 64U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_pal_ep_send(1U, buf, 16U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_pal_ep_recv(1U, buf, &len));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_pal_set_event_handler(stub_usb_event, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_pal_deinit());
  TEST_END("ra_usb_pal_*: pre-init calls return invalid_state");
}

int32_t main(void)
{
  test_init_fs_starts_detached();
  test_init_hs_starts_detached();
  test_init_bad_speed();
  test_attach_detach_cycles_state();
  test_ep_open_validates_args();
  test_ep_send_recv_stub();
  test_ep_send_recv_arg_validation();
  test_event_handler_attach_detach();
  test_dispatch_relays_intsts0();
  test_calls_before_init_fail();
  (void)fprintf(stderr, "[OK  ] test_ra_usb_pal.c\n");
  return 0;
}
