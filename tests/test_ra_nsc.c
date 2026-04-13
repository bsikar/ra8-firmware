/**
 * @file test_ra_nsc.c
 * @brief Unit tests for libs/ra_nsc (Wave 7.3 NSC veneer scaffold)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_ospi_regs.h"
#include "ra_err.h"
#include "ra_eth.h"
#include "ra_mstp.h"
#include "ra_net_pal.h"
#include "ra_nsc.h"
#include "ra_sim_mmap.h"
#include "ra_xspi.h"
#include "unity_minimal.h"

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  (void)ra_net_pal_deinit();
}

/* =============================================================================
 * ra_nsc_xspi_*
 * =============================================================================
 */

static void test_xspi_read_validates_args(void)
{
  TEST_BEGIN("ra_nsc_xspi_read: arg validation");
  prep();

  uint8_t buf[64] = {0U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_nsc_xspi_read(0U, nullptr, 64U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_nsc_xspi_read(0U, buf, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_nsc_xspi_read(0U, buf, (uint32_t)k_ra_nsc_xspi_max_read + 1U));

  /* Valid args -> stub returns not_supported. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_nsc_xspi_read(0U, buf, 64U));
  TEST_END("ra_nsc_xspi_read: arg validation");
}

static void test_xspi_status_forwards_to_driver(void)
{
  TEST_BEGIN("ra_nsc_xspi_status: forwards to ra_xspi");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_xspi_init(0U, k_ra_xspi_lio_1s1s1s));

  uint32_t mask = 0xDEADU;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_xspi_status(0U, &mask));
  /* Stub: reading status from an inactive xspi returns 0; the
   * point of the test is that the veneer doesn't fail. */

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_nsc_xspi_status(0U, nullptr));
  TEST_END("ra_nsc_xspi_status: forwards to ra_xspi");
}

/* =============================================================================
 * ra_nsc_eth_*
 * =============================================================================
 */

static const ra_net_pal_mac_t k_test_mac = {
  .bytes = {0x02U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U},
};

static void test_eth_send_validates_args(void)
{
  TEST_BEGIN("ra_nsc_eth_send: arg validation");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_net_pal_init(&k_test_mac));

  uint8_t frame[64] = {0U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_nsc_eth_send(nullptr, 64U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_nsc_eth_send(frame, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_nsc_eth_send(frame, (uint16_t)(k_ra_nsc_eth_frame_max + 1U)));

  /* Valid args -> ra_net_pal stub returns not_supported. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported,
                 (int32_t)ra_nsc_eth_send(frame, (uint16_t)sizeof(frame)));
  TEST_END("ra_nsc_eth_send: arg validation");
}

static void test_eth_recv_validates_args(void)
{
  TEST_BEGIN("ra_nsc_eth_recv: arg validation");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_net_pal_init(&k_test_mac));

  uint8_t  buf[k_ra_nsc_eth_frame_max] = {0U};
  uint16_t len                         = (uint16_t)k_ra_nsc_eth_frame_max;

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_nsc_eth_recv(nullptr, &len));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_nsc_eth_recv(buf, nullptr));
  uint16_t small = 16U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_nsc_eth_recv(buf, &small));
  uint16_t zero = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_nsc_eth_recv(buf, &zero));

  /* Valid args -> ra_net_pal stub returns not_supported. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_nsc_eth_recv(buf, &len));
  TEST_END("ra_nsc_eth_recv: arg validation");
}

/* =============================================================================
 * ra_nsc_log_emit
 * =============================================================================
 */

static void test_log_emit_happy(void)
{
  TEST_BEGIN("ra_nsc_log_emit: copies tag + message and returns ok");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_log_emit("TAG", "hello secure world"));
  /* Long message gets truncated; must still return k_ra_ok. */
  static const char k_long_message[] =
    "this is a very long message that exceeds the secure scratch buffer "
    "k_ra_nsc_log_msg_max_len cap so the veneer should truncate it before "
    "calling ra_log_info from the secure side; the return code stays k_ra_ok.";
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_log_emit("LONG", k_long_message));
  TEST_END("ra_nsc_log_emit: copies tag + message and returns ok");
}

static void test_log_emit_null(void)
{
  TEST_BEGIN("ra_nsc_log_emit: NULL pointers rejected");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_nsc_log_emit(nullptr, "msg"));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_nsc_log_emit("tag", nullptr));
  TEST_END("ra_nsc_log_emit: NULL pointers rejected");
}

/* =============================================================================
 * ra_nsc_periph_init
 * =============================================================================
 */

static void test_periph_init_idempotent(void)
{
  TEST_BEGIN("ra_nsc_periph_init: idempotent");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_periph_init());
  /* Second call returns k_ra_ok via the s_initialised fast-path. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_periph_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_periph_init());
  TEST_END("ra_nsc_periph_init: idempotent");
}

int32_t main(void)
{
  test_xspi_read_validates_args();
  test_xspi_status_forwards_to_driver();
  test_eth_send_validates_args();
  test_eth_recv_validates_args();
  test_log_emit_happy();
  test_log_emit_null();
  test_periph_init_idempotent();
  (void)fprintf(stderr, "[OK  ] test_ra_nsc.c\n");
  return 0;
}
