/**
 * @file test_ra8_ptp.c
 * @brief Unit tests for ra8_ptp.c (IEEE 1588 PTP driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_ptp.h"
#include "ra8_ptp_regs.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ptp_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_ptp_domain_200                  = 200U,
  k_ptp_mac_addr_11                 = 0x11U,
  k_ptp_mac_addr_22                 = 0x22U,
  k_ptp_mac_addr_33                 = 0x33U,
  k_ptp_mac_addr_44                 = 0x44U,
  k_ptp_mac_addr_55                 = 0x55U,
  k_ptp_ra8_ptp_dispatch_message_42 = 42U,
  k_ptp_ra8_ptp_dispatch_message_7  = 7U,
  k_ptp_val_5                       = 5,
} ptp_uint8_const_t;

typedef enum : int64_t {
  k_test_ptp_sec     = 1234567,    /**< Test ptp sec.     */
  k_test_ptp_nsec    = 500000000,  /**< Test ptp nsec.    */
  k_test_ptp_bad_ns  = 1000000001, /**< Test ptp bad ns.  */
  k_test_ptp_offset  = 250,        /**< Test ptp offset.  */
  k_test_ptp_neg_off = -250,       /**< Test ptp neg off. */
  k_test_ptp_ppb     = 12345,      /**< Test ptp ppb.     */
  k_test_ctx_marker  = 0xCAFE,     /**< Test ctx marker.  */
} test_ptp_const_t;

static uint32_t           s_cb_count;
static ra8_ptp_msg_type_t s_cb_last_type;
static uint64_t           s_cb_last_sec;
static uint32_t           s_cb_last_nsec;
static void*              s_cb_last_ctx;

static void stub_msg_cb(void* ctx, ra8_ptp_msg_type_t type, uint64_t sec, uint32_t nsec)
{
  ++s_cb_count;
  s_cb_last_type = type;
  s_cb_last_sec  = sec;
  s_cb_last_nsec = nsec;
  s_cb_last_ctx  = ctx;
}

static ra8_ptp_cfg_t default_cfg(void)
{
  ra8_ptp_cfg_t cfg         = {.clock_class = k_ra8_ptp_clock_class_default};
  cfg.domain                = (uint8_t)k_ra8_ptp_domain_default;
  cfg.sync_interval         = k_ra8_ptp_sync_int_1;
  cfg.clock_class           = k_ra8_ptp_clock_class_default;
  cfg.mac_addr[0]           = 0x02U;
  cfg.mac_addr[1]           = k_ptp_mac_addr_11;
  cfg.mac_addr[2]           = k_ptp_mac_addr_22;
  cfg.mac_addr[3]           = k_ptp_mac_addr_33;
  cfg.mac_addr[4]           = k_ptp_mac_addr_44;
  cfg.mac_addr[k_ptp_val_5] = k_ptp_mac_addr_55;
  return cfg;
}

static void prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
  s_cb_count     = 0U;
  s_cb_last_type = k_ra8_ptp_msg_sync;
  s_cb_last_sec  = 0U;
  s_cb_last_nsec = 0U;
  s_cb_last_ctx  = nullptr;
  /* Ensure each test starts from a closed state. */
  (void)ra8_ptp_close();
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_open_close(void)
{
  TEST_BEGIN("ptp open + close");
  prep();
  const ra8_ptp_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_open(&cfg));
  TEST_ASSERT_EQ(((uint32_t)k_ra8_ptp_mask_port_en),
                 (ra8_ptp_regs()->PTP_CTRL & (uint32_t)k_ra8_ptp_mask_port_en));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_close());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ptp_close());
  TEST_END("ptp open + close");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_open_null(void)
{
  TEST_BEGIN("ptp open null arg");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ptp_open(nullptr));
  TEST_END("ptp open null arg");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_open_bad_domain(void)
{
  TEST_BEGIN("ptp open bad domain");
  prep();
  ra8_ptp_cfg_t cfg = default_cfg();
  cfg.domain        = k_ptp_domain_200; /* > k_ra8_ptp_domain_user_max */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ptp_open(&cfg));
  TEST_END("ptp open bad domain");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_set_role_transitions(void)
{
  TEST_BEGIN("ptp set_role transitions");
  prep();
  /* Pre-init must reject */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ptp_set_role(k_ra8_ptp_role_peripheral));

  const ra8_ptp_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_open(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_set_role(k_ra8_ptp_role_peripheral));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_set_role(k_ra8_ptp_role_boundary_clock));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_set_role(k_ra8_ptp_role_transparent_clock));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_set_role(k_ra8_ptp_role_controller));

  /* Out-of-range role rejected */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ptp_set_role((ra8_ptp_role_t)9U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_close());
  TEST_END("ptp set_role transitions");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_get_set_time(void)
{
  TEST_BEGIN("ptp get_time / set_time round-trip");
  prep();
  uint64_t sec  = 0U;
  uint32_t nsec = 0U;

  /* Pre-init returns invalid_state */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ptp_get_time(&sec, &nsec));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_ptp_set_time((uint64_t)k_test_ptp_sec, (uint32_t)k_test_ptp_nsec));

  const ra8_ptp_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_open(&cfg));

  /* NULL args rejected */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ptp_get_time(nullptr, &nsec));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ptp_get_time(&sec, nullptr));

  /* nsec out of range rejected */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ptp_set_time(0U, (uint32_t)k_test_ptp_bad_ns));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_set_time((uint64_t)k_test_ptp_sec, (uint32_t)k_test_ptp_nsec));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_get_time(&sec, &nsec));
  TEST_ASSERT_EQ(k_test_ptp_sec, sec);
  TEST_ASSERT_EQ(k_test_ptp_nsec, nsec);
  TEST_END("ptp get_time / set_time round-trip");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_adjust_time_positive(void)
{
  TEST_BEGIN("ptp adjust_time positive");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ptp_adjust_time((int32_t)k_test_ptp_offset));
  const ra8_ptp_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_set_time(0U, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_adjust_time((int32_t)k_test_ptp_offset));

  uint64_t sec  = 0U;
  uint32_t nsec = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_get_time(&sec, &nsec));
  TEST_ASSERT_EQ(k_test_ptp_offset, nsec);
  TEST_END("ptp adjust_time positive");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_adjust_time_negative(void)
{
  TEST_BEGIN("ptp adjust_time negative");
  prep();
  const ra8_ptp_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_set_time(5U, 1000U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_adjust_time((int32_t)k_test_ptp_neg_off));

  int32_t offset = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_get_offset(&offset));
  TEST_ASSERT_EQ(k_test_ptp_neg_off, offset);
  TEST_END("ptp adjust_time negative");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_adjust_rate(void)
{
  TEST_BEGIN("ptp adjust_rate");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ptp_adjust_rate((int32_t)k_test_ptp_ppb));
  const ra8_ptp_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_adjust_rate((int32_t)k_test_ptp_ppb));
  TEST_ASSERT_EQ(k_test_ptp_ppb, ra8_ptp_regs()->PTP_RATE_PPB);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_adjust_rate(-(int32_t)k_test_ptp_ppb));
  TEST_ASSERT_EQ(-(int32_t)k_test_ptp_ppb, ra8_ptp_regs()->PTP_RATE_PPB);
  TEST_END("ptp adjust_rate");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_send_sync_announce(void)
{
  TEST_BEGIN("ptp send_sync + send_announce");
  prep();
  /* Pre-init rejects */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ptp_send_sync());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ptp_send_announce());

  const ra8_ptp_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_open(&cfg));

  /* Controller role -> ok */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_send_sync());
  TEST_ASSERT_EQ(((uint32_t)k_ra8_ptp_mask_tx_sync), ra8_ptp_regs()->PTP_TX_TRIG);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_send_announce());
  TEST_ASSERT_EQ(((uint32_t)k_ra8_ptp_mask_tx_annc), ra8_ptp_regs()->PTP_TX_TRIG);

  /* Peripheral role rejects send_* */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_set_role(k_ra8_ptp_role_peripheral));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ptp_send_sync());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ptp_send_announce());
  TEST_END("ptp send_sync + send_announce");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_message_handler_dispatch(void)
{
  TEST_BEGIN("ptp message handler dispatch");
  prep();
  const ra8_ptp_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_open(&cfg));

  /* No handler -> dispatch is a no-op (no crash). */
  ra8_ptp_dispatch_message(k_ra8_ptp_msg_sync, 1U, 2U);
  TEST_ASSERT_EQ(0, s_cb_count);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ptp_attach_message_handler(stub_msg_cb, (void*)(uintptr_t)k_test_ctx_marker));
  ra8_ptp_dispatch_message(k_ra8_ptp_msg_follow_up,
                           k_ptp_ra8_ptp_dispatch_message_7,
                           k_ptp_ra8_ptp_dispatch_message_42);
  TEST_ASSERT_EQ(1, s_cb_count);
  TEST_ASSERT_EQ(k_ra8_ptp_msg_follow_up, s_cb_last_type);
  TEST_ASSERT_EQ(7, s_cb_last_sec);
  TEST_ASSERT_EQ(42, s_cb_last_nsec);
  TEST_ASSERT_EQ(k_test_ctx_marker, (uintptr_t)s_cb_last_ctx);

  /* Try a few more message types to cover dispatch path. */
  ra8_ptp_dispatch_message(k_ra8_ptp_msg_delay_req, 0U, 0U);
  ra8_ptp_dispatch_message(k_ra8_ptp_msg_delay_resp, 0U, 0U);
  ra8_ptp_dispatch_message(k_ra8_ptp_msg_pdelay_req, 0U, 0U);
  ra8_ptp_dispatch_message(k_ra8_ptp_msg_pdelay_resp, 0U, 0U);
  TEST_ASSERT_EQ(5, s_cb_count);

  /* Detach */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_attach_message_handler(nullptr, nullptr));
  ra8_ptp_dispatch_message(k_ra8_ptp_msg_announce, 0U, 0U);
  TEST_ASSERT_EQ(5, s_cb_count);
  TEST_END("ptp message handler dispatch");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_get_offset(void)
{
  TEST_BEGIN("ptp get_offset");
  prep();
  int32_t offset = 0;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ptp_get_offset(&offset));

  const ra8_ptp_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_open(&cfg));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ptp_get_offset(nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_set_time(10U, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_adjust_time((int32_t)k_test_ptp_offset));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_get_offset(&offset));
  TEST_ASSERT_EQ(k_test_ptp_offset, offset);
  TEST_END("ptp get_offset");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_pre_init_rejects(void)
{
  TEST_BEGIN("ptp pre-init rejects everything but attach + dispatch");
  prep();
  /* Without ra8_ptp_open, every call that has state requirements returns
   * invalid_state (NULL-pointer checks happen first, so we pass valid
   * pointers). */
  uint64_t sec  = 0U;
  uint32_t nsec = 0U;
  int32_t  off  = 0;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ptp_get_time(&sec, &nsec));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ptp_set_time(0U, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ptp_adjust_time(0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ptp_adjust_rate(0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ptp_get_offset(&off));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ptp_send_sync());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ptp_send_announce());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ptp_set_role(k_ra8_ptp_role_controller));
  /* attach is allowed pre-init (it just stashes pointers) */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_attach_message_handler(nullptr, nullptr));
  TEST_END("ptp pre-init rejects");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_mac_address_packing(void)
{
  TEST_BEGIN("ptp mac address packing");
  prep();
  const ra8_ptp_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_open(&cfg));
  /* MAC was {0x02,0x11,0x22,0x33,0x44,0x55} -> MAC0 = 0x33221102 */
  TEST_ASSERT_EQ(0x33221102, ra8_ptp_regs()->PTP_MAC0);
  /* MAC1 = 0x00005544 */
  TEST_ASSERT_EQ(0x00005544, ra8_ptp_regs()->PTP_MAC1);
  TEST_END("ptp mac address packing");
}

/**
 * @test test_mcdc_ra8_ptp
 *
 * @par MC/DC:
 * Decision A: ``internal_send`` line 180,
 * libs/ra8_hal/src/ra8_ptp.c:
 * ``if ((s_role != CONTROLLER) && (s_role != BOUNDARY_CLOCK))``
 * (2 conditions, ``&&``). Threaded through ``ra8_ptp_send_sync``.
 * N+1 = 3:
 * - V1: role=CONTROLLER     -> dec F (send ok)
 * - V2: role=BOUNDARY_CLOCK -> dec F (send ok)
 * - V3: role=PERIPHERAL     -> dec T (invalid_state)
 * DO-178C 6.4.4.3 met.
 */
static void test_mcdc_ra8_ptp(void)
{
  TEST_BEGIN("ptp MC/DC: internal_send role 2-cond decision");
  prep();
  const ra8_ptp_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_set_role(k_ra8_ptp_role_controller));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_send_sync());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_set_role(k_ra8_ptp_role_boundary_clock));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_send_sync());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ptp_set_role(k_ra8_ptp_role_peripheral));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ptp_send_sync());
  TEST_END("ptp MC/DC: internal_send role 2-cond decision");
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
  test_mcdc_ra8_ptp();
  (void)fprintf(stderr, "[OK  ] test_ra8_ptp.c\n");
  return 0;
}
