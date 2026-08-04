/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_app_cpu1_pingpong.c
 * @brief Integration test: CPU1 release + IPC ping-pong contract
 *
 * @details
 * Mirrors examples/ek_ra8d2/cpu1_pingpong/main.c (CPU0-side):
 *   1. Release CPU1 via ra8_cpu1_release.
 *   2. Init the CPU0->CPU1 send channel and CPU1->CPU0 recv channel.
 *   3. Send 0x1234 on the send channel.
 *   4. (Mock CPU1 side) stage 0x4321 on the recv channel.
 *   5. Recv the reply and assert it equals 0x4321.
 *   6. Halt CPU1.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_dual_core.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_ipc.h"
#include "ra8_ipc_regs.h"
#include "unity_minimal.h"

/**
 * @enum app_cpu1_pingpong_fixture_t
 * @brief Poison values written into out-parameters before a call, so one that fails without assigning is detectable.
 */
typedef enum : uint8_t {
  k_ipc_ch_unset =
    0xFFU, /**< Poison channel id the callback starts from; unrun differs from channel 0. */
} app_cpu1_pingpong_fixture_t;

typedef enum : uint32_t {
  k_test_ppong_magic_ping  = 0x1234U,     /**< Test ppong magic ping.  */
  k_test_ppong_magic_pong  = 0x4321U,     /**< Test ppong magic pong.  */
  k_test_ppong_dummy_entry = 0x02080000U, /**< Test ppong dummy entry. */
  k_test_ppong_dummy_sp    = 0x22180000U, /**< Test ppong dummy sp.    */
} test_ppong_const_t;

typedef enum : uint8_t {
  k_test_ppong_pair_zero = 0U, /**< Test ppong pair zero. */
} test_ppong_pair_t;

/**
 * @brief Per-test fixture reset.
 * @details Clears the fake world and forces CPU1 to "stopped".
 * @pre None.
 * @pre None.
 * @post Fake mmap and CPU1 mock state at reset values.
 * @post ra8_cpu1_is_running() returns false.
 * @note Test-only helper.
 * @since 0.1.0
 */
static void reset_world(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_cpu1_halt();
}

/**
 * @brief CPU1 release rejects NULL entry / NULL sp / mis-aligned addresses.
 *
 * @par MC/DC:
 * Decision under test: ``entry == nullptr || sp == nullptr`` (2 cond.)
 * - Vector 1: entry=valid, sp=valid -> false (control)
 * - Vector 2: entry=NULL,  sp=valid -> true  (varies entry)
 * - Vector 3: entry=valid, sp=NULL  -> true  (varies sp)
 * N+1 = 3 vectors for N=2: minimal MC/DC. Plus alignment guards.
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post Test world reset before exit.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_ppong_release_validates_inputs(void)
{
  reset_world();
  TEST_BEGIN("cpu1_pingpong: release validates inputs");
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_cpu1_release((void*)(uintptr_t)k_test_ppong_dummy_entry,
                                  (void*)(uintptr_t)k_test_ppong_dummy_sp));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_cpu1_release(nullptr, (void*)(uintptr_t)k_test_ppong_dummy_sp));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_cpu1_release((void*)(uintptr_t)k_test_ppong_dummy_entry, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_cpu1_release((void*)((uintptr_t)k_test_ppong_dummy_entry + 1U),
                                  (void*)(uintptr_t)k_test_ppong_dummy_sp));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_cpu1_release((void*)(uintptr_t)k_test_ppong_dummy_entry,
                                  (void*)((uintptr_t)k_test_ppong_dummy_sp + 1U)));
  TEST_END("cpu1_pingpong: release validates inputs");
}

/**
 * @brief Successful release transitions CPU1 to running.
 * @par MC/DC: not applicable -- straight-line release.
 * @pre None.
 * @pre None.
 * @post CPU1 mock running at exit.
 * @post Fake mmap reset before next test.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_ppong_release_then_running(void)
{
  reset_world();
  TEST_BEGIN("cpu1_pingpong: release -> is_running");
  TEST_ASSERT_EQ(0, ra8_cpu1_is_running());
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_cpu1_release((void*)(uintptr_t)k_test_ppong_dummy_entry,
                                  (void*)(uintptr_t)k_test_ppong_dummy_sp));
  TEST_ASSERT_EQ(1, ra8_cpu1_is_running());
  TEST_END("cpu1_pingpong: release -> is_running");
}

/**
 * @brief Halt after release transitions CPU1 back to stopped.
 * @par MC/DC: not applicable -- sequential chain.
 * @pre None.
 * @pre None.
 * @post CPU1 mock stopped at exit.
 * @post Fake mmap reset before next test.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_ppong_halt_round_trip(void)
{
  reset_world();
  TEST_BEGIN("cpu1_pingpong: halt round-trip");
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_cpu1_release((void*)(uintptr_t)k_test_ppong_dummy_entry,
                                  (void*)(uintptr_t)k_test_ppong_dummy_sp));
  TEST_ASSERT_EQ(1, ra8_cpu1_is_running());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cpu1_halt());
  TEST_ASSERT_EQ(0, ra8_cpu1_is_running());
  TEST_END("cpu1_pingpong: halt round-trip");
}

/**
 * @brief Channel-pair resolution honours direction conventions.
 *
 * @par MC/DC:
 * Decision under test in ra8_ipc_channel_for_send:
 *   ``out == NULL || core out-of-range || pair out-of-range`` (3 cond.)
 * - V1: out=valid, core=cpu0,    pair=0  -> false (control)
 * - V2: out=NULL,  core=cpu0,    pair=0  -> true  (varies out)
 * - V3: out=valid, core=invalid, pair=0  -> true  (varies core)
 * - V4: out=valid, core=cpu0,    pair=99 -> true  (varies pair)
 * N+1 = 4 vectors for N=3: minimal MC/DC.
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post Fake mmap reset before next test.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_ppong_channel_pair_resolution(void)
{
  reset_world();
  TEST_BEGIN("cpu1_pingpong: channel-pair resolution MC/DC");
  uint8_t ch = k_ipc_ch_unset;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ipc_channel_for_send(k_ra8_ipc_core_cpu0, (uint8_t)k_test_ppong_pair_zero, &ch));
  TEST_ASSERT_EQ(2, ch);
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_ipc_channel_for_send(k_ra8_ipc_core_cpu0, (uint8_t)k_test_ppong_pair_zero, nullptr));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_ipc_channel_for_send((ra8_ipc_core_id_t)0xFFU, (uint8_t)k_test_ppong_pair_zero, &ch));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ipc_channel_for_send(k_ra8_ipc_core_cpu0, 99U, &ch));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ipc_channel_for_recv(k_ra8_ipc_core_cpu1, (uint8_t)k_test_ppong_pair_zero, &ch));
  TEST_ASSERT_EQ(2, ch);
  TEST_END("cpu1_pingpong: channel-pair resolution MC/DC");
}

/**
 * @brief End-to-end ping-pong flow against the IPC mock.
 * @details CPU0 sends 0x1234 on chan 2, mock CPU1 stages 0x4321 by
 *          poking RXD + STA.RDY directly, CPU0 reads chan 0 and
 *          asserts 0x4321.
 *
 * @par MC/DC:
 * Decision under test (in app loop):
 *   ``ra8_ipc_send_message(ch_send, ping) != ok`` -- single-cond.
 * happy-path here; failure-path covered by tests/test_ra8_ipc.c.
 *
 * @pre None.
 * @pre None.
 * @post Fake mmap reset before next test.
 * @post No persistent side effects on channel mocks.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_ppong_send_recv_round_trip(void)
{
  reset_world();
  TEST_BEGIN("cpu1_pingpong: send/recv ping-pong round trip");

  uint8_t ch_send_cpu0 = k_ipc_ch_unset;
  uint8_t ch_recv_cpu0 = k_ipc_ch_unset;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ipc_channel_for_send(k_ra8_ipc_core_cpu0, (uint8_t)k_test_ppong_pair_zero, &ch_send_cpu0));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ipc_channel_for_recv(k_ra8_ipc_core_cpu0, (uint8_t)k_test_ppong_pair_zero, &ch_recv_cpu0));

  ra8_ipc_config_t cfg_send = {
    .channel      = ch_send_cpu0,
    .reset_fifo   = true,
    .clear_status = true,
    .event_mask   = (uint32_t)k_ra8_ipc_event_msg_ready,
  };
  ra8_ipc_config_t cfg_recv = {
    .channel      = ch_recv_cpu0,
    .reset_fifo   = true,
    .clear_status = true,
    .event_mask   = (uint32_t)k_ra8_ipc_event_msg_ready,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_init(&cfg_send));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_init(&cfg_recv));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_send_message(ch_send_cpu0, (uint32_t)k_test_ppong_magic_ping));

  volatile r_ipc_channel_regs_t* recv_reg = ra8_ipc_channel(ch_recv_cpu0);
  TEST_ASSERT_NOT_NULL((void*)recv_reg);
  recv_reg->RXD = (uint32_t)k_test_ppong_magic_pong;
  recv_reg->STA = (uint32_t)k_ra8_ipc_sta_mask_rdy;

  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_recv_message(ch_recv_cpu0, &got));
  TEST_ASSERT_EQ(k_test_ppong_magic_pong, got);

  TEST_END("cpu1_pingpong: send/recv ping-pong round trip");
}

int main(void)
{
  test_ppong_release_validates_inputs();
  test_ppong_release_then_running();
  test_ppong_halt_round_trip();
  test_ppong_channel_pair_resolution();
  test_ppong_send_recv_round_trip();
  return 0;
}
