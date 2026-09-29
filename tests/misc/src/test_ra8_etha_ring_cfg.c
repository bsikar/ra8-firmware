/**
 * @file test_ra8_etha_ring_cfg.c
 * @brief Unit tests for ::ra8_etha_descriptor_ring_init_cfg (issue #1028).
 *
 * @par Tag
 * [Ring 3 / Test] {World: NS}
 *
 * @details
 * ``ra8_etha_ring_cfg_t`` is the const-pointer spelling of the descriptor-ring
 * sizing arguments. Before #1028 the type was declared in
 * ``libs/ra8_hal/inc/ra8_etha_types.h`` and no entry point accepted it, so a
 * caller following its doc-comment could not compile. These cases pin the
 * forwarder that closes that gap:
 *
 *   - the struct path and the scalar path land byte-identical
 *     ``ring_tx`` / ``ring_rx`` / ``ring_buf`` in ::ra8_etha_get_stats, so the
 *     forwarder cannot quietly reorder or drop a field;
 *   - each field is asserted individually against a DISTINCT value, so a
 *     transposed ``num_tx`` / ``num_rx`` fails rather than passing on symmetry;
 *   - every rejection the scalar path performs is reached through the struct
 *     (channel out of range, ring depth 0 / above 4096, buffer below 64 /
 *     above 16383), and a rejected call leaves the previous stats intact;
 *   - a null config is rejected with ::k_ra8_err_null_ptr, distinct from the
 *     invalid-argument code the range checks return.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_etha.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum ring_cfg_fixture_t
 * @brief Fixture sizes. Every value is distinct so a transposed field fails.
 */
typedef enum : uint16_t {
  k_ring_cfg_tx      = 32U,    /**< TX depth, distinct from rx and buf.    */
  k_ring_cfg_rx      = 64U,    /**< RX depth, distinct from tx and buf.    */
  k_ring_cfg_buf     = 1518U,  /**< Buffer bytes, distinct from tx/rx.     */
  k_ring_cfg_tx_alt  = 128U,   /**< Second TX depth for the overwrite leg. */
  k_ring_cfg_rx_alt  = 256U,   /**< Second RX depth for the overwrite leg. */
  k_ring_cfg_buf_alt = 1024U,  /**< Second buffer size for that leg.       */
  k_ring_cfg_over    = 4097U,  /**< One past the 4096 descriptor ceiling.  */
  k_ring_cfg_buf_low = 16U,    /**< Below the 64-byte 802.3 minimum.       */
  k_ring_cfg_buf_hi  = 17000U, /**< Above the 14-bit 16383-byte cap.       */
} ring_cfg_fixture_t;

static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
}

static void bring_up_port_0(void)
{
  const ra8_etha_config_t cfg = {
    .initial_mode = k_ra8_etha_opc_config,
    .eaeie0_mask  = 0U,
    .eaeie1_mask  = 0U,
    .eaeie2_mask  = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &cfg));
}

/* --- The struct spelling reaches the same state as the scalar one --- */

/**
 * @par MC/DC:
 * (no compound decisions in the forwarder under test -- the single
 * null check is a one-condition decision covered by the null case below)
 */
static void test_cfg_matches_scalar_path(void)
{
  TEST_BEGIN("etha ring cfg: struct path lands the same stats as the scalar path");
  prep();
  bring_up_port_0();

  const ra8_etha_ring_cfg_t cfg = {
    .num_tx      = (uint16_t)k_ring_cfg_tx,
    .num_rx      = (uint16_t)k_ring_cfg_rx,
    .buffer_size = (uint16_t)k_ring_cfg_buf,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_descriptor_ring_init_cfg(k_ra8_etha_port_0, &cfg));

  ra8_etha_port_stats_t via_cfg = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_get_stats(k_ra8_etha_port_0, &via_cfg));
  /* Field by field, not a memcmp: a transposed tx/rx would survive a
   * comparison against a struct built the same wrong way. */
  TEST_ASSERT_EQ((uint16_t)k_ring_cfg_tx, via_cfg.ring_tx);
  TEST_ASSERT_EQ((uint16_t)k_ring_cfg_rx, via_cfg.ring_rx);
  TEST_ASSERT_EQ((uint16_t)k_ring_cfg_buf, via_cfg.ring_buf);

  /* Same three values through the scalar entry point on a fresh port. */
  prep();
  bring_up_port_0();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_etha_descriptor_ring_init(k_ra8_etha_port_0,
                                               (uint16_t)k_ring_cfg_tx,
                                               (uint16_t)k_ring_cfg_rx,
                                               (uint16_t)k_ring_cfg_buf));
  ra8_etha_port_stats_t via_scalar = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_get_stats(k_ra8_etha_port_0, &via_scalar));
  TEST_ASSERT_EQ(via_scalar.ring_tx, via_cfg.ring_tx);
  TEST_ASSERT_EQ(via_scalar.ring_rx, via_cfg.ring_rx);
  TEST_ASSERT_EQ(via_scalar.ring_buf, via_cfg.ring_buf);
  TEST_END("etha ring cfg: struct path lands the same stats as the scalar path");
}

/* --- A second accepted call overwrites the first --- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path; the decisions it reaches are covered by test_ra8_etha_mcdc.c)
 */
static void test_cfg_reconfigure_overwrites(void)
{
  TEST_BEGIN("etha ring cfg: a second accepted config replaces the first");
  prep();
  bring_up_port_0();

  const ra8_etha_ring_cfg_t first = {
    .num_tx      = (uint16_t)k_ring_cfg_tx,
    .num_rx      = (uint16_t)k_ring_cfg_rx,
    .buffer_size = (uint16_t)k_ring_cfg_buf,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_descriptor_ring_init_cfg(k_ra8_etha_port_0, &first));

  const ra8_etha_ring_cfg_t second = {
    .num_tx      = (uint16_t)k_ring_cfg_tx_alt,
    .num_rx      = (uint16_t)k_ring_cfg_rx_alt,
    .buffer_size = (uint16_t)k_ring_cfg_buf_alt,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_descriptor_ring_init_cfg(k_ra8_etha_port_0, &second));

  ra8_etha_port_stats_t stats = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_get_stats(k_ra8_etha_port_0, &stats));
  TEST_ASSERT_EQ((uint16_t)k_ring_cfg_tx_alt, stats.ring_tx);
  TEST_ASSERT_EQ((uint16_t)k_ring_cfg_rx_alt, stats.ring_rx);
  TEST_ASSERT_EQ((uint16_t)k_ring_cfg_buf_alt, stats.ring_buf);
  TEST_END("etha ring cfg: a second accepted config replaces the first");
}

/* --- Rejections reach the caller through the struct, state untouched --- */

/**
 * @par MC/DC:
 * (the six-condition AND inside internal_ring_args_ok is covered by
 * test_ra8_etha_mcdc.c; this case proves each outcome propagates through
 * the forwarder rather than being swallowed or remapped)
 */
static void test_cfg_rejections_propagate(void)
{
  TEST_BEGIN("etha ring cfg: out-of-range fields are rejected and change nothing");
  prep();
  bring_up_port_0();

  const ra8_etha_ring_cfg_t good = {
    .num_tx      = (uint16_t)k_ring_cfg_tx,
    .num_rx      = (uint16_t)k_ring_cfg_rx,
    .buffer_size = (uint16_t)k_ring_cfg_buf,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_descriptor_ring_init_cfg(k_ra8_etha_port_0, &good));

  const ra8_etha_ring_cfg_t bad[] = {
    {.num_tx = 0U, .num_rx = (uint16_t)k_ring_cfg_rx, .buffer_size = (uint16_t)k_ring_cfg_buf},
    {.num_tx      = (uint16_t)k_ring_cfg_over,
     .num_rx      = (uint16_t)k_ring_cfg_rx,
     .buffer_size = (uint16_t)k_ring_cfg_buf},
    {.num_tx = (uint16_t)k_ring_cfg_tx, .num_rx = 0U, .buffer_size = (uint16_t)k_ring_cfg_buf},
    {.num_tx      = (uint16_t)k_ring_cfg_tx,
     .num_rx      = (uint16_t)k_ring_cfg_over,
     .buffer_size = (uint16_t)k_ring_cfg_buf},
    {.num_tx      = (uint16_t)k_ring_cfg_tx,
     .num_rx      = (uint16_t)k_ring_cfg_rx,
     .buffer_size = (uint16_t)k_ring_cfg_buf_low},
    {.num_tx      = (uint16_t)k_ring_cfg_tx,
     .num_rx      = (uint16_t)k_ring_cfg_rx,
     .buffer_size = (uint16_t)k_ring_cfg_buf_hi},
  };
  for (uint8_t i = 0U; i < (uint8_t)(sizeof bad / sizeof bad[0]); ++i) {
    TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                   ra8_etha_descriptor_ring_init_cfg(k_ra8_etha_port_0, &bad[i]));
  }

  /* An out-of-range port is rejected through the struct too. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_descriptor_ring_init_cfg(
                   (ra8_etha_port_t)(uint8_t)k_ra8_etha_port_count, &good));

  /* Every rejection above left the accepted config in place. */
  ra8_etha_port_stats_t stats = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_get_stats(k_ra8_etha_port_0, &stats));
  TEST_ASSERT_EQ((uint16_t)k_ring_cfg_tx, stats.ring_tx);
  TEST_ASSERT_EQ((uint16_t)k_ring_cfg_rx, stats.ring_rx);
  TEST_ASSERT_EQ((uint16_t)k_ring_cfg_buf, stats.ring_buf);
  TEST_END("etha ring cfg: out-of-range fields are rejected and change nothing");
}

/* --- A null config is its own error code --- */

/**
 * @par MC/DC:
 * C1 = (cfg == nullptr). V1: cfg non-null -> C1=F -> forwarded (covered by
 * test_cfg_matches_scalar_path). V2: cfg null -> C1=T -> k_ra8_err_null_ptr.
 */
static void test_cfg_null_rejected(void)
{
  TEST_BEGIN("etha ring cfg: null config returns null_ptr, not invalid_arg");
  prep();
  bring_up_port_0();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_etha_descriptor_ring_init_cfg(k_ra8_etha_port_0, nullptr));
  /* Null is checked before the port, so a bad port with a null cfg still
   * reports the pointer fault rather than the range fault. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_etha_descriptor_ring_init_cfg(
                   (ra8_etha_port_t)(uint8_t)k_ra8_etha_port_count, nullptr));
  TEST_END("etha ring cfg: null config returns null_ptr, not invalid_arg");
}

int main(void)
{
  test_cfg_matches_scalar_path();
  test_cfg_reconfigure_overwrites();
  test_cfg_rejections_propagate();
  test_cfg_null_rejected();
  return 0;
}
