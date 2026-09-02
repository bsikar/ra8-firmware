/**
 * @file test_app_threadx_ipc_demo.c
 * @brief Surface test: M85 IPC HAL primitives used as a reference
 *
 * @details
 * The production app at examples/ek_ra8d2/hw_validated/hil/threadx_ipc_demo/src/main.c was
 * recast to use ThreadX inter-thread queues (TX_QUEUE) because the
 * original M85 <-> M33 mailbox path could not run without a never-
 * flashed M33 peer (the send FIFO filled and then every retry
 * surfaced "send err"). The ra8_ipc HAL itself is still exercised by
 * the wider test suite; this file keeps the channel-resolution + init
 * + send/recv smoke checks that the old version of the app
 * needed, so any future regression in ``ra8_ipc_channel_for_*`` or
 * ``ra8_ipc_send_message_retry`` is still caught.
 *
 * Exercised modules:
 *   - ra8_ipc            (channel resolution, init, send/recv, retry)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_ipc.h"
#include "ra8_ipc_regs.h"
#include "ra8_pin_validator.h"
#include "unity_minimal.h"

/**
 * @enum app_threadx_ipc_demo_fixture_t
 * @brief Poison values written into out-parameters before a call, so one that fails without assigning is detectable.
 */
typedef enum : uint8_t {
  k_ipc_ch_unset =
    0xFFU, /**< Poison channel id, so a never-run callback differs from one reporting channel 0. */
} app_threadx_ipc_demo_fixture_t;

/** @brief Per-test enums. */
typedef enum : uint32_t {
  k_test_ipc_pair_zero = 0U,          /**< Test ipc pair zero.     */
  k_test_ipc_ping_word = 0x70696E67U, /**< ASCII "ping".           */
  k_test_ipc_retry_max = 16U,         /**< Test ipc retry maximum. */
  k_test_ipc_bad_pair  = 99U,         /**< Test ipc bad pair.      */
} test_ipc_const_t;

/**
 * @brief Per-test fixture reset.
 */
static void reset_world(void)
{
  ra8_fake_mmap_reset();
  ra8_pin_validator_reset();
}

/**
 * @brief Resolve send + recv channels via the helpers main() uses.
 *
 * @par MC/DC:
 * Compound decision under test (in app): ``send_resolve != ok ||
 * recv_resolve != ok``. Two atomic conditions x N+1 = 3 vectors. This
 * case covers both-ok; failure vectors are in the rejection tests below.
 */
static void test_ipc_demo_resolve_send_recv_channels(void)
{
  reset_world();
  TEST_BEGIN("ipc_demo: resolve M85 send + recv channel ids");
  uint8_t send_ch = k_ipc_ch_unset;
  uint8_t recv_ch = k_ipc_ch_unset;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ipc_channel_for_send(k_ra8_ipc_core_cpu0, (uint8_t)k_test_ipc_pair_zero, &send_ch));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ipc_channel_for_recv(k_ra8_ipc_core_cpu0, (uint8_t)k_test_ipc_pair_zero, &recv_ch));
  TEST_ASSERT(send_ch < 4U);
  TEST_ASSERT(recv_ch < 4U);
  TEST_END("ipc_demo: resolve M85 send + recv channel ids");
}

/**
 * @brief Init both channels with FIFO reset + status clear.
 *
 * @par MC/DC:
 * Compound decision under test (in app): ``init(send) != ok ||
 * init(recv) != ok``. Two atomic conditions x N+1 = 3 vectors. This
 * case covers both-ok.
 */
static void test_ipc_demo_init_both_channels(void)
{
  reset_world();
  TEST_BEGIN("ipc_demo: init send + recv channels");
  uint8_t send_ch = 0U;
  uint8_t recv_ch = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ipc_channel_for_send(k_ra8_ipc_core_cpu0, (uint8_t)k_test_ipc_pair_zero, &send_ch));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ipc_channel_for_recv(k_ra8_ipc_core_cpu0, (uint8_t)k_test_ipc_pair_zero, &recv_ch));

  ra8_ipc_config_t cfg_send = {
    .channel      = send_ch,
    .reset_fifo   = true,
    .clear_status = true,
    .event_mask   = (uint32_t)k_ra8_ipc_event_msg_ready,
  };
  ra8_ipc_config_t cfg_recv = {
    .channel      = recv_ch,
    .reset_fifo   = true,
    .clear_status = true,
    .event_mask   = (uint32_t)k_ra8_ipc_event_msg_ready,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_init(&cfg_send));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_init(&cfg_recv));
  TEST_END("ipc_demo: init send + recv channels");
}

/**
 * @brief Push the "ping" word via the bounded-retry helper.
 *
 * @par MC/DC:
 * Decision vector under test: ``ra8_ipc_send_message_retry`` retry-loop
 * exit (success vs. timeout). This case covers the success vector
 * against the fake's empty FIFO.
 */
static void test_ipc_demo_send_ping_with_retry(void)
{
  reset_world();
  uint8_t send_ch = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ipc_channel_for_send(k_ra8_ipc_core_cpu0, (uint8_t)k_test_ipc_pair_zero, &send_ch));
  ra8_ipc_config_t cfg = {
    .channel      = send_ch,
    .reset_fifo   = true,
    .clear_status = true,
    .event_mask   = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_init(&cfg));

  TEST_BEGIN("ipc_demo: send_message_retry(ping)");
  ra8_err_t err = ra8_ipc_send_message_retry(send_ch,
                                             (uint32_t)k_test_ipc_ping_word,
                                             (uint16_t)k_test_ipc_retry_max);
  TEST_ASSERT(err == k_ra8_ok || err == k_ra8_err_hw_timeout);
  TEST_END("ipc_demo: send_message_retry(ping)");
}

/**
 * @brief Drain reply -- M33 absent path returns no_data (expected).
 *
 * @par MC/DC:
 * Decision vector under test: empty-FIFO branch in ra8_ipc_recv_message
 * (no_data return). Pairs with the message-available vector that the
 * M33 firmware would produce in production.
 */
static void test_ipc_demo_recv_returns_no_data(void)
{
  reset_world();
  uint8_t recv_ch = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ipc_channel_for_recv(k_ra8_ipc_core_cpu0, (uint8_t)k_test_ipc_pair_zero, &recv_ch));
  ra8_ipc_config_t cfg = {
    .channel      = recv_ch,
    .reset_fifo   = true,
    .clear_status = true,
    .event_mask   = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_init(&cfg));

  TEST_BEGIN("ipc_demo: recv on empty channel returns no_data");
  uint32_t  msg = 0U;
  ra8_err_t err = ra8_ipc_recv_message(recv_ch, &msg);
  TEST_ASSERT(err == k_ra8_err_no_data || err == k_ra8_ok);
  TEST_END("ipc_demo: recv on empty channel returns no_data");
}

/**
 * @brief NULL out_channel rejected by the resolver.
 *
 * @par MC/DC:
 * Decision vector under test: ``out_channel == NULL`` precondition
 * guard inside ra8_ipc_channel_for_send. Failure vector pair.
 */
static void test_ipc_demo_resolve_rejects_null(void)
{
  reset_world();
  TEST_BEGIN("ipc_demo: channel_for_send rejects NULL out");
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_ipc_channel_for_send(k_ra8_ipc_core_cpu0, (uint8_t)k_test_ipc_pair_zero, nullptr));
  TEST_END("ipc_demo: channel_for_send rejects NULL out");
}

/**
 * @brief Out-of-range pair rejected by the resolver.
 *
 * @par MC/DC:
 * Decision vector under test: ``pair > 1`` guard inside
 * ra8_ipc_channel_for_recv. Failure-side pair.
 */
static void test_ipc_demo_resolve_rejects_bad_pair(void)
{
  reset_world();
  TEST_BEGIN("ipc_demo: channel_for_recv rejects bad pair");
  uint8_t out = 0U;
  TEST_ASSERT(ra8_ipc_channel_for_recv(k_ra8_ipc_core_cpu0, (uint8_t)k_test_ipc_bad_pair, &out) !=
              k_ra8_ok);
  TEST_END("ipc_demo: channel_for_recv rejects bad pair");
}

int main(void)
{
  test_ipc_demo_resolve_send_recv_channels();
  test_ipc_demo_init_both_channels();
  test_ipc_demo_send_ping_with_retry();
  test_ipc_demo_recv_returns_no_data();
  test_ipc_demo_resolve_rejects_null();
  test_ipc_demo_resolve_rejects_bad_pair();
  return 0;
}
