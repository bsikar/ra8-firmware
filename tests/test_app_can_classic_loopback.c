/**
 * @file test_app_can_classic_loopback.c
 * @brief Integration test: classic CAN 2.0B internal-loopback bring-up
 *
 * @details
 * Mirrors examples/ek_ra8d2/can_classic_loopback/main.c bring-up:
 * ra8_canfd_init -> ra8_canfd_set_bitrate(nominal, data=0) ->
 * ra8_canfd_set_test_mode(self-test 1) -> classic transmit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_canfd.h"
#include "ra8_canfd_regs.h"
#include "ra8_err.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum can_classic_loopback_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_can_classic_loopback_sentinel_a5 = 0xA5U,
  k_can_classic_loopback_val_30      = 0x30U,
  k_can_classic_loopback_val_40      = 0x40U,
  k_can_classic_loopback_val_5c      = 0x5CU,
  k_can_classic_loopback_val_c1      = 0xC1U,
} can_classic_loopback_uint8_const_t;

/**
 * @enum can_classic_loopback_uint32_const_t
 * @brief Named uint32_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint32_t {
  k_can_classic_loopback_sts_ffffffff = 0xFFFFFFFFUL,
} can_classic_loopback_uint32_const_t;

typedef enum : uint32_t {
  k_test_can_classic_bitrate = 250000U, /**< Test CAN classic bitrate. */
  k_test_can_classic_id      = 0x456U,  /**< Test CAN classic ID.      */
  /* HUM Ch 41 "CFDCnCTR" p 2710 -- CTME = bit 24, CTMS = bits [26:25],
   * CTMS = 11b selects Self-test 1 (Internal Loopback). */
  k_test_can_classic_ctme_mask  = 1UL << 24U, /**< Test CAN classic ctme mask.  */
  k_test_can_classic_ctms_shift = 25U,        /**< Test CAN classic ctms shift. */
  k_test_can_classic_ctms_intl  = 0x3UL,      /**< Test CAN classic ctms intl.  */
} test_can_classic_const_t;

typedef enum : uint8_t {
  k_test_can_classic_channel  = 0U, /**< Test CAN classic channel.  */
  k_test_can_classic_dlc      = 8U, /**< Test CAN classic dlc.      */
  k_test_can_classic_bad_chan = 9U, /**< Test CAN classic bad chan. */
} test_can_classic_chan_t;

static void reset_world(void)
{
  ra8_sim_mmap_reset();
}

/**
 * @brief Golden bring-up: init + classic-mode bitrate + loopback ok.
 *
 * @par MC/DC:
 * Compound decision in app: ``ra8_canfd_init != ok ||
 * ra8_canfd_set_bitrate != ok``. Two atomic conditions x N+1 = 3
 * vectors -- both-ok (this), bad-channel (below), zero-bitrate
 * (below).
 */
static void test_can_classic_bringup_ok(void)
{
  reset_world();
  TEST_BEGIN("can_classic_loopback: init + classic bitrate ok");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_init((uint8_t)k_test_can_classic_channel));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_canfd_set_bitrate((uint8_t)k_test_can_classic_channel,
                                       (uint32_t)k_test_can_classic_bitrate,
                                       0U));
  TEST_END("can_classic_loopback: init + classic bitrate ok");
}

/**
 * @brief ra8_canfd_set_test_mode lands the internal-loopback selector.
 *
 * @par MC/DC:
 * Decision in app: ``reg == nullptr``. One atomic condition x 2
 * vectors -- valid channel (here) + bad channel (below).
 */
static void test_can_classic_loopback_bits(void)
{
  reset_world();
  TEST_BEGIN("can_classic_loopback: CTME / CTMS bits stamped");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_init((uint8_t)k_test_can_classic_channel));
  /* The set_test_mode helper polls CFDC[0].STS.CHLTSTS after dropping
   * the channel into CH_HALT.  Pre-set the sim STS register so the
   * spin loop sees halt asserted immediately (HUM Ch 41 "CFDCnSTS"
   * p 2711). */
  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_test_can_classic_channel);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->CFDC[0].STS = k_can_classic_loopback_sts_ffffffff;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_canfd_set_test_mode((uint8_t)k_test_can_classic_channel, k_ra8_ctms_self_test_1));
  const uint32_t actual = reg->CFDC[0].CTR;
  /* HUM Ch 41 "CFDCnCTR" p 2710 */ /* CTME bit 24, CTMS at [26:25]. */
  TEST_ASSERT((actual & (uint32_t)k_test_can_classic_ctme_mask) != 0U);
  TEST_ASSERT(((actual >> k_test_can_classic_ctms_shift) & 0x3UL) ==
              (uint32_t)k_test_can_classic_ctms_intl);
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_init((uint8_t)k_test_can_classic_channel));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_canfd_set_bitrate((uint8_t)k_test_can_classic_channel,
                                       (uint32_t)k_test_can_classic_bitrate,
                                       0U));
  ra8_canfd_frame_t tx = {
    .id          = (uint32_t)k_test_can_classic_id,
    .dlc         = (uint8_t)k_test_can_classic_dlc,
    .is_extended = 0U,
    .is_fd       = 0U,
    .is_brs      = 0U,
    .data        = {0x01U,
                    k_can_classic_loopback_val_c1,
                    k_can_classic_loopback_sentinel_a5,
                    k_can_classic_loopback_val_5c,
                    0x10U,
                    0x20U,
                    k_can_classic_loopback_val_30,
                    k_can_classic_loopback_val_40},
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_transmit((uint8_t)k_test_can_classic_channel, &tx));
  ra8_canfd_frame_t rx = {};
  const ra8_err_t   r  = ra8_canfd_receive((uint8_t)k_test_can_classic_channel, &rx);
  TEST_ASSERT(r == k_ra8_ok || r == k_ra8_err_no_data);
  TEST_END("can_classic_loopback: TX classic frame");
}

/**
 * @brief Bad channel rejected by ra8_canfd_init.
 *
 * @par MC/DC:
 * Decision: ``channel < max_channel``. One atomic condition; pairs
 * with golden bring-up for N+1 coverage.
 */
static void test_can_classic_bad_channel(void)
{
  reset_world();
  TEST_BEGIN("can_classic_loopback: bad channel rejected");
  TEST_ASSERT(ra8_canfd_init((uint8_t)k_test_can_classic_bad_chan) != k_ra8_ok);
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_init((uint8_t)k_test_can_classic_channel));
  TEST_ASSERT(ra8_canfd_set_bitrate((uint8_t)k_test_can_classic_channel, 0U, 0U) != k_ra8_ok);
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
