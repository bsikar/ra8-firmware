/**
 * @file test_ra_ptp.c
 * @brief Unit tests for ra_ptp.c (IEEE 1588 PTP driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_ptp_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_ptp.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : int64_t {
  k_test_ptp_sec     = 1234567,
  k_test_ptp_nsec    = 500000000,
  k_test_ptp_bad_ns  = 1000000001,
  k_test_ptp_offset  = 250,
  k_test_ptp_neg_off = -250,
  k_test_ptp_ppb     = 12345,
  k_test_ctx_marker  = 0xCAFE,
} test_ptp_const_t;

static uint32_t          s_cb_count;
static ra_ptp_msg_type_t s_cb_last_type;
static uint64_t          s_cb_last_sec;
static uint32_t          s_cb_last_nsec;
static void*             s_cb_last_ctx;

static void stub_msg_cb(void* ctx, ra_ptp_msg_type_t type, uint64_t sec, uint32_t nsec)
{
  ++s_cb_count;
  s_cb_last_type = type;
  s_cb_last_sec  = sec;
  s_cb_last_nsec = nsec;
  s_cb_last_ctx  = ctx;
}

static ra_ptp_cfg_t default_cfg(void)
{
  ra_ptp_cfg_t cfg  = {};
  cfg.domain        = (uint8_t)k_ra_ptp_domain_default;
  cfg.sync_interval = k_ra_ptp_sync_int_1;
  cfg.clock_class   = k_ra_ptp_clock_class_default;
  cfg.mac_addr[0]   = 0x02U;
  cfg.mac_addr[1]   = 0x11U;
  cfg.mac_addr[2]   = 0x22U;
  cfg.mac_addr[3]   = 0x33U;
  cfg.mac_addr[4]   = 0x44U;
  cfg.mac_addr[5]   = 0x55U;
  return cfg;
}

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_cb_count     = 0U;
  s_cb_last_type = k_ra_ptp_msg_sync;
  s_cb_last_sec  = 0U;
  s_cb_last_nsec = 0U;
  s_cb_last_ctx  = nullptr;
  /* Ensure each test starts from a closed state. */
  (void)ra_ptp_close();
}

static void test_open_close(void)
{
  TEST_BEGIN("ptp open + close");
  prep();
  const ra_ptp_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_open(&cfg));
  TEST_ASSERT_EQ((int32_t)((uint32_t)k_ra_ptp_mask_port_en),
                 (int32_t)(ra_ptp_regs()->PTP_CTRL & (uint32_t)k_ra_ptp_mask_port_en));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_close());
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_ptp_close());
  TEST_END("ptp open + close");
}

static void test_open_null(void)
{
  TEST_BEGIN("ptp open null arg");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_ptp_open(nullptr));
  TEST_END("ptp open null arg");
}

static void test_open_bad_domain(void)
{
  TEST_BEGIN("ptp open bad domain");
  prep();
  ra_ptp_cfg_t cfg = default_cfg();
  cfg.domain       = 200U; /* > k_ra_ptp_domain_user_max */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_ptp_open(&cfg));
  TEST_END("ptp open bad domain");
}

static void test_set_role_transitions(void)
{
  TEST_BEGIN("ptp set_role transitions");
  prep();
  /* Pre-init must reject */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_ptp_set_role(k_ra_ptp_role_slave));

  const ra_ptp_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_open(&cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_set_role(k_ra_ptp_role_slave));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_set_role(k_ra_ptp_role_boundary_clock));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_set_role(k_ra_ptp_role_transparent_clock));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_set_role(k_ra_ptp_role_master));

  /* Out-of-range role rejected */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_ptp_set_role((ra_ptp_role_t)9U));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_close());
  TEST_END("ptp set_role transitions");
}

static void test_get_set_time(void)
{
  TEST_BEGIN("ptp get_time / set_time round-trip");
  prep();
  uint64_t sec  = 0U;
  uint32_t nsec = 0U;

  /* Pre-init returns invalid_state */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_ptp_get_time(&sec, &nsec));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_ptp_set_time((uint64_t)k_test_ptp_sec, (uint32_t)k_test_ptp_nsec));

  const ra_ptp_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_open(&cfg));

  /* NULL args rejected */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_ptp_get_time(nullptr, &nsec));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_ptp_get_time(&sec, nullptr));

  /* nsec out of range rejected */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ptp_set_time(0U, (uint32_t)k_test_ptp_bad_ns));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_ptp_set_time((uint64_t)k_test_ptp_sec, (uint32_t)k_test_ptp_nsec));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_get_time(&sec, &nsec));
  TEST_ASSERT_EQ((int32_t)k_test_ptp_sec, (int32_t)sec);
  TEST_ASSERT_EQ((int32_t)k_test_ptp_nsec, (int32_t)nsec);
  TEST_END("ptp get_time / set_time round-trip");
}

static void test_adjust_time_positive(void)
{
  TEST_BEGIN("ptp adjust_time positive");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_ptp_adjust_time((int32_t)k_test_ptp_offset));
  const ra_ptp_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_set_time(0U, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_adjust_time((int32_t)k_test_ptp_offset));

  uint64_t sec  = 0U;
  uint32_t nsec = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_get_time(&sec, &nsec));
  TEST_ASSERT_EQ((int32_t)k_test_ptp_offset, (int32_t)nsec);
  TEST_END("ptp adjust_time positive");
}

static void test_adjust_time_negative(void)
{
  TEST_BEGIN("ptp adjust_time negative");
  prep();
  const ra_ptp_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_set_time(5U, 1000U));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_adjust_time((int32_t)k_test_ptp_neg_off));

  int32_t offset = 0;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_get_offset(&offset));
  TEST_ASSERT_EQ((int32_t)k_test_ptp_neg_off, (int32_t)offset);
  TEST_END("ptp adjust_time negative");
}

static void test_adjust_rate(void)
{
  TEST_BEGIN("ptp adjust_rate");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_ptp_adjust_rate((int32_t)k_test_ptp_ppb));
  const ra_ptp_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_adjust_rate((int32_t)k_test_ptp_ppb));
  TEST_ASSERT_EQ((int32_t)k_test_ptp_ppb, (int32_t)ra_ptp_regs()->PTP_RATE_PPB);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_adjust_rate(-(int32_t)k_test_ptp_ppb));
  TEST_ASSERT_EQ(-(int32_t)k_test_ptp_ppb, (int32_t)ra_ptp_regs()->PTP_RATE_PPB);
  TEST_END("ptp adjust_rate");
}

static void test_send_sync_announce(void)
{
  TEST_BEGIN("ptp send_sync + send_announce");
  prep();
  /* Pre-init rejects */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_ptp_send_sync());
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_ptp_send_announce());

  const ra_ptp_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_open(&cfg));

  /* Master role -> ok */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_send_sync());
  TEST_ASSERT_EQ((int32_t)((uint32_t)k_ra_ptp_mask_tx_sync), (int32_t)ra_ptp_regs()->PTP_TX_TRIG);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_send_announce());
  TEST_ASSERT_EQ((int32_t)((uint32_t)k_ra_ptp_mask_tx_annc), (int32_t)ra_ptp_regs()->PTP_TX_TRIG);

  /* Slave role rejects send_* */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_set_role(k_ra_ptp_role_slave));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_ptp_send_sync());
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_ptp_send_announce());
  TEST_END("ptp send_sync + send_announce");
}

static void test_message_handler_dispatch(void)
{
  TEST_BEGIN("ptp message handler dispatch");
  prep();
  const ra_ptp_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_open(&cfg));

  /* No handler -> dispatch is a no-op (no crash). */
  ra_ptp_dispatch_message(k_ra_ptp_msg_sync, 1U, 2U);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)s_cb_count);

  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_ptp_attach_message_handler(stub_msg_cb, (void*)(uintptr_t)k_test_ctx_marker));
  ra_ptp_dispatch_message(k_ra_ptp_msg_follow_up, 7U, 42U);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_cb_count);
  TEST_ASSERT_EQ((int32_t)k_ra_ptp_msg_follow_up, (int32_t)s_cb_last_type);
  TEST_ASSERT_EQ((int32_t)7, (int32_t)s_cb_last_sec);
  TEST_ASSERT_EQ((int32_t)42, (int32_t)s_cb_last_nsec);
  TEST_ASSERT_EQ((int32_t)k_test_ctx_marker, (int32_t)(uintptr_t)s_cb_last_ctx);

  /* Try a few more message types to cover dispatch path. */
  ra_ptp_dispatch_message(k_ra_ptp_msg_delay_req, 0U, 0U);
  ra_ptp_dispatch_message(k_ra_ptp_msg_delay_resp, 0U, 0U);
  ra_ptp_dispatch_message(k_ra_ptp_msg_pdelay_req, 0U, 0U);
  ra_ptp_dispatch_message(k_ra_ptp_msg_pdelay_resp, 0U, 0U);
  TEST_ASSERT_EQ((int32_t)5, (int32_t)s_cb_count);

  /* Detach */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_attach_message_handler(nullptr, nullptr));
  ra_ptp_dispatch_message(k_ra_ptp_msg_announce, 0U, 0U);
  TEST_ASSERT_EQ((int32_t)5, (int32_t)s_cb_count);
  TEST_END("ptp message handler dispatch");
}

static void test_get_offset(void)
{
  TEST_BEGIN("ptp get_offset");
  prep();
  int32_t offset = 0;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_ptp_get_offset(&offset));

  const ra_ptp_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_open(&cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_ptp_get_offset(nullptr));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_set_time(10U, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_adjust_time((int32_t)k_test_ptp_offset));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_get_offset(&offset));
  TEST_ASSERT_EQ((int32_t)k_test_ptp_offset, (int32_t)offset);
  TEST_END("ptp get_offset");
}

static void test_pre_init_rejects(void)
{
  TEST_BEGIN("ptp pre-init rejects everything but attach + dispatch");
  prep();
  /* Without ra_ptp_open, every call that has state requirements returns
   * invalid_state (NULL-pointer checks happen first, so we pass valid
   * pointers). */
  uint64_t sec  = 0U;
  uint32_t nsec = 0U;
  int32_t  off  = 0;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_ptp_get_time(&sec, &nsec));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_ptp_set_time(0U, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_ptp_adjust_time(0));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_ptp_adjust_rate(0));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_ptp_get_offset(&off));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_ptp_send_sync());
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_ptp_send_announce());
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_ptp_set_role(k_ra_ptp_role_master));
  /* attach is allowed pre-init (it just stashes pointers) */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_attach_message_handler(nullptr, nullptr));
  TEST_END("ptp pre-init rejects");
}

static void test_mac_address_packing(void)
{
  TEST_BEGIN("ptp mac address packing");
  prep();
  const ra_ptp_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ptp_open(&cfg));
  /* MAC was {0x02,0x11,0x22,0x33,0x44,0x55} -> MAC0 = 0x33221102 */
  TEST_ASSERT_EQ((int32_t)0x33221102, (int32_t)ra_ptp_regs()->PTP_MAC0);
  /* MAC1 = 0x00005544 */
  TEST_ASSERT_EQ((int32_t)0x00005544, (int32_t)ra_ptp_regs()->PTP_MAC1);
  TEST_END("ptp mac address packing");
}

int32_t main(void)
{
  test_open_close();
  test_open_null();
  test_open_bad_domain();
  test_set_role_transitions();
  test_get_set_time();
  test_adjust_time_positive();
  test_adjust_time_negative();
  test_adjust_rate();
  test_send_sync_announce();
  test_message_handler_dispatch();
  test_get_offset();
  test_pre_init_rejects();
  test_mac_address_packing();
  (void)fprintf(stderr, "[OK  ] test_ra_ptp.c\n");
  return 0;
}
