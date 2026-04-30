/**
 * @file test_ra_cec.c
 * @brief Unit tests for ra_cec.c (HDMI CEC placeholder driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra_cec.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_test_clock_div = 32U,
} test_cec_const_t;

typedef struct {
  uint32_t calls;
  uint8_t  last_opcode;
} test_handler_state_t;

static test_handler_state_t s_handler = {};

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_cec_close();
  (void)memset(&s_handler, 0, sizeof(s_handler));
}

static void rx_cb(const ra_cec_message_t* m, void* ctx)
{
  test_handler_state_t* h = (test_handler_state_t*)ctx;
  h->calls                = h->calls + 1U;
  h->last_opcode          = m->opcode;
}

static void test_open_null(void)
{
  TEST_BEGIN("open rejects NULL cfg");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_cec_open(nullptr));
  TEST_END("open rejects NULL cfg");
}

static void test_open_bad_args(void)
{
  TEST_BEGIN("open rejects bad address and zero clock_div");
  prep();
  ra_cec_cfg_t cfg = {.local_address = (ra_cec_logical_addr_t)16U,
                      .clock_div     = (uint16_t)k_test_clock_div};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_cec_open(&cfg));

  cfg.local_address = k_ra_cec_addr_tv;
  cfg.clock_div     = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_cec_open(&cfg));
  TEST_END("open rejects bad address and zero clock_div");
}

static void test_lifecycle(void)
{
  TEST_BEGIN("open / close lifecycle");
  prep();
  const ra_cec_cfg_t cfg = {.local_address = k_ra_cec_addr_playback_device_1,
                            .clock_div     = (uint16_t)k_test_clock_div};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cec_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_exists, (int32_t)ra_cec_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cec_close());
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_cec_close());
  TEST_END("open / close lifecycle");
}

static void test_send_validation(void)
{
  TEST_BEGIN("send_message validates state and args");
  prep();
  ra_cec_message_t m = {.source      = k_ra_cec_addr_playback_device_1,
                        .destination = k_ra_cec_addr_tv,
                        .opcode      = 0x36U,
                        .payload_len = 0U,
                        .payload     = {}};
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_cec_send_message(nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_cec_send_message(&m));

  const ra_cec_cfg_t cfg = {.local_address = k_ra_cec_addr_playback_device_1,
                            .clock_div     = (uint16_t)k_test_clock_div};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cec_open(&cfg));

  m.payload_len = (uint8_t)(k_ra_cec_payload_max + 1U);
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_cec_send_message(&m));

  m.payload_len = 0U;
  m.destination = (ra_cec_logical_addr_t)16U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_cec_send_message(&m));

  m.destination = k_ra_cec_addr_tv;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cec_send_message(&m));

  ra_cec_status_t st = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cec_status_get(&st));
  TEST_ASSERT_EQ((int32_t)1, (int32_t)st.tx_count);
  TEST_END("send_message validates state and args");
}

static void test_receive_path(void)
{
  TEST_BEGIN("inject + receive plus handler dispatch");
  prep();
  const ra_cec_cfg_t cfg = {.local_address = k_ra_cec_addr_audio_system,
                            .clock_div     = (uint16_t)k_test_clock_div};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cec_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cec_attach_handler(rx_cb, &s_handler));

  ra_cec_message_t out = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_no_data, (int32_t)ra_cec_receive_message(&out));

  const ra_cec_message_t in = {.source      = k_ra_cec_addr_tv,
                               .destination = k_ra_cec_addr_audio_system,
                               .opcode      = 0x70U,
                               .payload_len = 1U,
                               .payload     = {0xAAU}};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cec_inject_rx(&in));
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_handler.calls);
  TEST_ASSERT_EQ((int32_t)0x70, (int32_t)s_handler.last_opcode);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cec_receive_message(&out));
  TEST_ASSERT_EQ((int32_t)0x70, (int32_t)out.opcode);
  TEST_ASSERT_EQ((int32_t)k_ra_err_no_data, (int32_t)ra_cec_receive_message(&out));
  TEST_END("inject + receive plus handler dispatch");
}

static void test_status_null(void)
{
  TEST_BEGIN("status_get / receive null + state checks");
  prep();
  ra_cec_status_t st = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_cec_status_get(nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_cec_status_get(&st));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_cec_receive_message(nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_cec_inject_rx(nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_cec_attach_handler(rx_cb, nullptr));
  TEST_END("status_get / receive null + state checks");
}

static void test_inject_payload_too_long(void)
{
  TEST_BEGIN("inject_rx rejects oversize payload");
  prep();
  const ra_cec_cfg_t cfg = {.local_address = k_ra_cec_addr_tv,
                            .clock_div     = (uint16_t)k_test_clock_div};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_cec_open(&cfg));

  ra_cec_message_t big = {};
  big.source           = k_ra_cec_addr_unregistered;
  big.destination      = k_ra_cec_addr_tv;
  big.opcode           = 0x00U;
  big.payload_len      = (uint8_t)(k_ra_cec_payload_max + 1U);
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_cec_inject_rx(&big));
  TEST_END("inject_rx rejects oversize payload");
}

int32_t main(void)
{
  test_open_null();
  test_open_bad_args();
  test_lifecycle();
  test_send_validation();
  test_receive_path();
  test_status_null();
  test_inject_payload_too_long();
  (void)fprintf(stderr, "[OK ] test_ra_cec.c\n");
  return 0;
}
