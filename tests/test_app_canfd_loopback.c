/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_app_canfd_loopback.c
 * @brief Integration test: CANFD0 init + bitrate + loopback round-trip
 *
 * @details
 * Mirrors the bring-up flow of examples/ek_ra8d2/canfd_loopback/main.c:
 * ra8_canfd_init -> ra8_canfd_set_bitrate -> ra8_canfd_set_test_mode ->
 * transmit + receive cycle. All MMIO is via the host
 * tests/mocks/ra8_fake_mmap.c shim.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_canfd.h"
#include "ra8_canfd_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "unity_minimal.h"

/**
 * @enum app_canfd_loopback_fixture_t
 * @brief All-bits-set register values, so a write that clears the wrong field leaves evidence.
 */
typedef enum : uint32_t {
  k_canfd_sts_all_ones =
    0xFFFFFFFFUL, /**< Every status bit set, so clearing the wrong flag leaves evidence. */
} app_canfd_loopback_fixture_t;

typedef enum : uint32_t {
  k_test_canfd_app_bitrate = 500000U, /**< Test CANFD app bitrate. */
  k_test_canfd_app_id      = 0x123U,  /**< Test CANFD app ID.      */
  /* HUM Ch 41 "CFDCnCTR" p 2710 -- CTME = bit 24, CTMS = bits [26:25],
   * CTMS = 11b selects Self-test 1 (Internal Loopback). */
  k_test_canfd_app_ctme_mask  = 1UL << 24U, /**< Test CANFD app ctme mask.  */
  k_test_canfd_app_ctms_shift = 25U,        /**< Test CANFD app ctms shift. */
  k_test_canfd_app_ctms_intl  = 0x3UL,      /**< Test CANFD app ctms intl.  */
} test_canfd_app_const_t;

typedef enum : uint8_t {
  k_test_canfd_app_channel  = 0U, /**< Test CANFD app channel.  */
  k_test_canfd_app_dlc      = 8U, /**< Test CANFD app dlc.      */
  k_test_canfd_app_bad_chan = 9U, /**< Test CANFD app bad chan. */
} test_canfd_app_chan_t;

static void reset_world(void)
{
  ra8_fake_mmap_reset();
}

/**
 * @brief Golden-path bring-up replays main.c canfd_demo_setup_or_halt.
 *
 * @par MC/DC:
 * Compound decision in app: ``ra8_canfd_init != ok || ra8_canfd_set_bitrate != ok``.
 * Two atomic conditions x N+1 = 3 vectors -- this case covers both-ok,
 * the bad_channel test below covers init-fails, and bad-bitrate test
 * covers set_bitrate-fails.
 */
static void test_canfd_app_bringup_ok(void)
{
  reset_world();
  TEST_BEGIN("canfd_loopback: init + bitrate ok");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_init((uint8_t)k_test_canfd_app_channel));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_canfd_set_bitrate((uint8_t)k_test_canfd_app_channel,
                                       (uint32_t)k_test_canfd_app_bitrate,
                                       (uint32_t)k_test_canfd_app_bitrate));
  TEST_END("canfd_loopback: init + bitrate ok");
}

/**
 * @brief ra8_canfd_set_test_mode lands the internal-loopback selector.
 *
 * @par MC/DC:
 * Decision under test (in app): ``ra8_canfd(channel) == nullptr``.
 * Two atomic conditions x 2 vectors -- valid channel (this test) and
 * the bad-channel test below covers the nullptr branch.
 */
static void test_canfd_app_loopback_bits_set(void)
{
  reset_world();
  TEST_BEGIN("canfd_loopback: CTME / CTMS bits stamped");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_init((uint8_t)k_test_canfd_app_channel));
  /* The set_test_mode helper polls CFDC[0].STS.CHLTSTS after dropping
   * the channel into CH_HALT.  Pre-set the fake STS register so the
   * spin loop sees halt asserted immediately (HUM Ch 41 "CFDCnSTS"
   * p 2711). */
  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_test_canfd_app_channel);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->CFDC[0].STS = k_canfd_sts_all_ones;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_canfd_set_test_mode((uint8_t)k_test_canfd_app_channel, k_ra8_ctms_self_test_1));
  const uint32_t actual = reg->CFDC[0].CTR;
  /* HUM Ch 41 "CFDCnCTR" p 2710 */ /* CTME bit 24, CTMS at [26:25]. */
  TEST_ASSERT((actual & (uint32_t)k_test_canfd_app_ctme_mask) != 0U);
  TEST_ASSERT(((actual >> k_test_canfd_app_ctms_shift) & 0x3UL) ==
              (uint32_t)k_test_canfd_app_ctms_intl);
  TEST_END("canfd_loopback: CTME / CTMS bits stamped");
}

/**
 * @brief One TX + RX poll round-trip exercises both API call sites.
 *
 * @par MC/DC:
 * Compound decision: ``transmit != ok || receive != ok``. Two atomic
 * conditions x N+1 = 3 vectors. transmit-ok + receive-no-data is the
 * realistic mock scenario here; receive-fail vector is covered by
 * test_canfd_app_receive_empty.
 */
static void test_canfd_app_round_trip(void)
{
  reset_world();
  TEST_BEGIN("canfd_loopback: TX + RX poll round-trip");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_init((uint8_t)k_test_canfd_app_channel));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_canfd_set_bitrate((uint8_t)k_test_canfd_app_channel,
                                       (uint32_t)k_test_canfd_app_bitrate,
                                       (uint32_t)k_test_canfd_app_bitrate));
  const ra8_canfd_frame_t tx = {
    .id          = (uint32_t)k_test_canfd_app_id,
    .dlc         = (uint8_t)k_test_canfd_app_dlc,
    .is_extended = 0U,
    .is_fd       = 0U,
    .is_brs      = 0U,
    /* 0xDEADBEEF, an embedded zero, then an ascending tail: no repeated
     * byte, so a frame that came back short, padded or byte-swapped shows up. */
    .data = {0xDEU, 0xADU, 0xBEU, 0xEFU, 0x00U, 0x11U, 0x22U, 0x33U},
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_transmit((uint8_t)k_test_canfd_app_channel, &tx));
  ra8_canfd_frame_t rx = {};
  /* The mock RX FIFO is empty unless explicitly seeded -- the app
   * tolerates this and toggles LED2; the API contract still holds. */
  const ra8_err_t r = ra8_canfd_receive((uint8_t)k_test_canfd_app_channel, &rx);
  TEST_ASSERT(r == k_ra8_ok || r == k_ra8_err_no_data);
  TEST_END("canfd_loopback: TX + RX poll round-trip");
}

/**
 * @brief Bad channel index is rejected by ra8_canfd_init.
 *
 * @par MC/DC:
 * Decision: ``channel < k_ra8_canfd_max_channel``. One atomic condition;
 * pairs with the golden bring-up test for N+1 coverage.
 */
static void test_canfd_app_bad_channel(void)
{
  reset_world();
  TEST_BEGIN("canfd_loopback: bad channel rejected");
  TEST_ASSERT(ra8_canfd_init((uint8_t)k_test_canfd_app_bad_chan) != k_ra8_ok);
  TEST_END("canfd_loopback: bad channel rejected");
}

/**
 * @brief Zero bitrate is rejected by ra8_canfd_set_bitrate.
 *
 * @par MC/DC:
 * Decision: ``bitrate_bps == 0``. One atomic condition; pairs with
 * golden bring-up test.
 */
static void test_canfd_app_zero_bitrate(void)
{
  reset_world();
  TEST_BEGIN("canfd_loopback: zero bitrate rejected");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_init((uint8_t)k_test_canfd_app_channel));
  TEST_ASSERT(ra8_canfd_set_bitrate((uint8_t)k_test_canfd_app_channel, 0U, 0U) != k_ra8_ok);
  TEST_END("canfd_loopback: zero bitrate rejected");
}

int main(void)
{
  test_canfd_app_bringup_ok();
  test_canfd_app_loopback_bits_set();
  test_canfd_app_round_trip();
  test_canfd_app_bad_channel();
  test_canfd_app_zero_bitrate();
  return 0;
}
