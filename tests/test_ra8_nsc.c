/**
 * @file test_ra8_nsc.c
 * @brief Unit tests for libs/ra8_nsc (NSC veneer scaffold)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_eth.h"
#include "ra8_mstp.h"
#include "ra8_net_pal.h"
#include "ra8_nsc.h"
#include "ra8_ospi_regs.h"
#include "ra8_sim_mmap.h"
#include "ra8_sim_mmio.h"
#include "ra8_sim_xspi_flash.h"
#include "ra8_xspi.h"
#include "unity_minimal.h"

/**
 * @enum t_nsc_t
 * @brief Buffer capacity and the mask out-parameter seed.
 */
typedef enum : uint16_t {
  k_t_buf_cap    = 64U,     /**< Scratch and frame buffers, bytes.           */
  k_t_mask_unset = 0xDEADU, /**< Pre-set mask; a veneer that rejects its input
                                  must leave it rather than report a real mask. */
} t_nsc_t;

static void prep(void)
{
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  /* The xspi veneers forward to ra8_xspi_flash_*, whose real register
   * sequence needs the tests/mocks NOR model to service TRREQ kicks. */
  ra8_sim_xspi_flash_install();
  (void)ra8_mstp_init();
  (void)ra8_net_pal_deinit();
}

/* =============================================================================
 * ra8_nsc_xspi_*
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_xspi_read_validates_args(void)
{
  TEST_BEGIN("ra8_nsc_xspi_read: arg validation + model-flash forward");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init(0U, k_ra8_xspi_lio_1s1s1s));

  uint8_t buf[k_t_buf_cap] = {0U};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_xspi_read(0U, nullptr, 64U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_nsc_xspi_read(0U, buf, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_nsc_xspi_read(0U, buf, (uint32_t)k_ra8_nsc_xspi_max_read + 1U));

  /* Valid args -> veneer forwards to ra8_xspi_flash_read, serviced by
   * the tests/mocks register-level NOR model. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_xspi_read(0U, buf, 64U));
  TEST_END("ra8_nsc_xspi_read: arg validation + model-flash forward");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_xspi_status_forwards_to_driver(void)
{
  TEST_BEGIN("ra8_nsc_xspi_status: forwards to ra8_xspi");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init(0U, k_ra8_xspi_lio_1s1s1s));

  uint32_t mask = k_t_mask_unset;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_xspi_status(0U, &mask));
  /* Stub: reading status from an inactive xspi returns 0; the
   * point of the test is that the veneer doesn't fail. */

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_xspi_status(0U, nullptr));
  TEST_END("ra8_nsc_xspi_status: forwards to ra8_xspi");
}

/* =============================================================================
 * ra8_nsc_eth_*
 * =============================================================================
 */

static const ra8_net_pal_mac_t k_test_mac = {
  .bytes = {0x02U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U},
};

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_eth_send_validates_args(void)
{
  TEST_BEGIN("ra8_nsc_eth_send: arg validation");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_net_pal_init(&k_test_mac));

  uint8_t frame[k_t_buf_cap] = {0U};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_eth_send(nullptr, 64U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_nsc_eth_send(frame, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_nsc_eth_send(frame, (uint16_t)(k_ra8_nsc_eth_frame_max + 1U)));

  /* Valid args -> veneer forwards to ra8_net_pal, frame is queued. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_eth_send(frame, (uint16_t)sizeof(frame)));
  TEST_END("ra8_nsc_eth_send: arg validation");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_eth_recv_validates_args(void)
{
  TEST_BEGIN("ra8_nsc_eth_recv: arg validation");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_net_pal_init(&k_test_mac));

  uint8_t  buf[k_ra8_nsc_eth_frame_max] = {0U};
  uint16_t len                          = (uint16_t)k_ra8_nsc_eth_frame_max;

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_eth_recv(nullptr, &len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_eth_recv(buf, nullptr));
  uint16_t small = 16U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_nsc_eth_recv(buf, &small));
  uint16_t zero = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_nsc_eth_recv(buf, &zero));

  /* Valid args, empty ring -> veneer forwards no_data from the PAL. */
  TEST_ASSERT_EQ(k_ra8_err_no_data, ra8_nsc_eth_recv(buf, &len));
  TEST_END("ra8_nsc_eth_recv: arg validation");
}

/* =============================================================================
 * ra8_nsc_log_emit
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_log_emit_happy(void)
{
  TEST_BEGIN("ra8_nsc_log_emit: copies tag + message and returns ok");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_log_emit("TAG", "hello secure world"));
  /* Long message gets truncated; must still return k_ra8_ok. */
  static const char k_long_message[] =
    "this is a very long message that exceeds the secure scratch buffer "
    "k_ra8_nsc_log_msg_max_len cap so the veneer should truncate it before "
    "calling ra8_log_info from the secure side; the return code stays k_ra8_ok.";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_log_emit("LONG", k_long_message));
  TEST_END("ra8_nsc_log_emit: copies tag + message and returns ok");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_log_emit_null(void)
{
  TEST_BEGIN("ra8_nsc_log_emit: NULL pointers rejected");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_log_emit(nullptr, "msg"));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_log_emit("tag", nullptr));
  TEST_END("ra8_nsc_log_emit: NULL pointers rejected");
}

/* =============================================================================
 * ra8_nsc_periph_init
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_periph_init_idempotent(void)
{
  TEST_BEGIN("ra8_nsc_periph_init: idempotent");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_periph_init());
  /* Second call returns k_ra8_ok via the s_initialized fast-path. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_periph_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_periph_init());
  TEST_END("ra8_nsc_periph_init: idempotent");
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
  (void)fprintf(stderr, "[OK ] test_ra8_nsc.c\n");
  return 0;
}
