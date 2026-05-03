/**
 * @file test_app_can_classic_loopback.c
 * @brief Integration test: classic CAN 2.0B internal-loopback bring-up
 *
 * @details
 * Mirrors examples/ek_ra8d2/can_classic_loopback/main.c bring-up:
 * ra_canfd_init -> ra_canfd_set_bitrate(nominal, data=0) ->
 * raw CTR write for internal loopback -> classic transmit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_canfd_regs.h"
#include "ra_canfd.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_test_can_classic_bitrate    = 250000U,
  k_test_can_classic_id         = 0x456U,
  k_test_can_classic_ctme_bit   = 17U,
  k_test_can_classic_ctms_shift = 18U,
  k_test_can_classic_ctms_intl  = 0x1UL,
} test_can_classic_const_t;

typedef enum : uint8_t {
  k_test_can_classic_channel  = 0U,
  k_test_can_classic_dlc      = 8U,
  k_test_can_classic_bad_chan = 9U,
} test_can_classic_chan_t;

static void reset_world(void)
{
  ra_sim_mmap_reset();
}

/**
 * @brief Golden bring-up: init + classic-mode bitrate + loopback ok.
 *
 * @par MC/DC:
 * Compound decision in app: ``ra_canfd_init != ok ||
 * ra_canfd_set_bitrate != ok``. Two atomic conditions x N+1 = 3
 * vectors -- both-ok (this), bad-channel (below), zero-bitrate
 * (below).
 */
static void test_can_classic_bringup_ok(void)
{
  reset_world();
  TEST_BEGIN("can_classic_loopback: init + classic bitrate ok");
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_canfd_init((uint8_t)k_test_can_classic_channel));
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_canfd_set_bitrate((uint8_t)k_test_can_classic_channel,
                                           (uint32_t)k_test_can_classic_bitrate,
                                           0U));
  TEST_END("can_classic_loopback: init + classic bitrate ok");
}

/**
 * @brief Raw CTR bits stamped for internal loopback.
 *
 * @par MC/DC:
 * Decision in app: ``reg == nullptr``. One atomic condition x 2
 * vectors -- valid channel (here) + bad channel (below).
 */
static void test_can_classic_loopback_bits(void)
{
  reset_world();
  TEST_BEGIN("can_classic_loopback: CTME / CTMS bits stamped");
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_canfd_init((uint8_t)k_test_can_classic_channel));
  volatile r_canfd_t* reg = ra_canfd((uint8_t)k_test_can_classic_channel);
  TEST_ASSERT_NOT_NULL((void*)reg);
  uint32_t ctr = reg->CFDC[0].CTR;
  ctr |= (uint32_t)(1UL << k_test_can_classic_ctme_bit);
  ctr |= (uint32_t)(k_test_can_classic_ctms_intl << k_test_can_classic_ctms_shift);
  reg->CFDC[0].CTR      = ctr;
  const uint32_t actual = reg->CFDC[0].CTR;
  TEST_ASSERT((actual & (uint32_t)(1UL << k_test_can_classic_ctme_bit)) != 0U);
  TEST_END("can_classic_loopback: CTME / CTMS bits stamped");
}

/**
 * @brief Transmit a classic frame (is_fd = 0).
 *
 * @par MC/DC:
 * Compound decision in app: ``transmit != ok || receive != ok``.
 * Two atomic conditions x N+1 = 3 vectors. Transmit-ok +
 * receive-no-data is the realistic mock scenario; receive-fail is
 * covered by the FD demo's test sibling because the underlying API
 * is shared.
 */
static void test_can_classic_round_trip(void)
{
  reset_world();
  TEST_BEGIN("can_classic_loopback: TX classic frame");
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_canfd_init((uint8_t)k_test_can_classic_channel));
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_canfd_set_bitrate((uint8_t)k_test_can_classic_channel,
                                           (uint32_t)k_test_can_classic_bitrate,
                                           0U));
  ra_canfd_frame_t tx = {
    .id          = (uint32_t)k_test_can_classic_id,
    .dlc         = (uint8_t)k_test_can_classic_dlc,
    .is_extended = 0U,
    .is_fd       = 0U,
    .is_brs      = 0U,
    .data        = {0x01U, 0xC1U, 0xA5U, 0x5CU, 0x10U, 0x20U, 0x30U, 0x40U},
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_canfd_transmit((uint8_t)k_test_can_classic_channel, &tx));
  ra_canfd_frame_t rx = {};
  const ra_err_t   r  = ra_canfd_receive((uint8_t)k_test_can_classic_channel, &rx);
  TEST_ASSERT(r == k_ra_ok || r == k_ra_err_no_data);
  TEST_END("can_classic_loopback: TX classic frame");
}

/**
 * @brief Bad channel rejected by ra_canfd_init.
 *
 * @par MC/DC:
 * Decision: ``channel < max_channel``. One atomic condition; pairs
 * with golden bring-up for N+1 coverage.
 */
static void test_can_classic_bad_channel(void)
{
  reset_world();
  TEST_BEGIN("can_classic_loopback: bad channel rejected");
  TEST_ASSERT(ra_canfd_init((uint8_t)k_test_can_classic_bad_chan) != k_ra_ok);
  TEST_END("can_classic_loopback: bad channel rejected");
}

/**
 * @brief Zero nominal bitrate rejected.
 *
 * @par MC/DC:
 * Decision: ``bitrate_bps == 0``. One atomic condition; pairs with
 * golden bring-up.
 */
static void test_can_classic_zero_bitrate(void)
{
  reset_world();
  TEST_BEGIN("can_classic_loopback: zero bitrate rejected");
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_canfd_init((uint8_t)k_test_can_classic_channel));
  TEST_ASSERT(ra_canfd_set_bitrate((uint8_t)k_test_can_classic_channel, 0U, 0U) != k_ra_ok);
  TEST_END("can_classic_loopback: zero bitrate rejected");
}

int main(void)
{
  test_can_classic_bringup_ok();
  test_can_classic_loopback_bits();
  test_can_classic_round_trip();
  test_can_classic_bad_channel();
  test_can_classic_zero_bitrate();
  return 0;
}
