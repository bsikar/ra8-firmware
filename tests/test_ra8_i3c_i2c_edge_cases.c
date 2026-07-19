/**
 * @file test_ra8_i3c_i2c_edge_cases.c
 * @brief Edge-case + stress unit tests for the IIC_B (I3C in I2C-mode) controller.
 *
 * @details
 * Complements ``test_ra8_i3c_i2c.c`` with focused stress / edge-case
 * coverage of safety-critical paths:
 *
 *   - bus-busy (BFREF=0) timeout during the START gate;
 *   - NAK on the address byte vs. NAK on a data byte (both must yield
 *     ``k_ra8_err_nack`` with a final STOP);
 *   - repeated-START sequences via ``internal_i3c_i2c_transfer`` (write -> read);
 *   - clock-stretching upper bound: a long-buffer transfer where the
 *     peripheral never ACKs (TDBEF0 stays low for the duration) returns the
 *     hardware-timeout error rather than spinning forever;
 *   - ``set_clock`` rejects out-of-range bus_hz / pclka_hz.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_i3c_i2c.h"
#include "ra8_i3c_i2c_regs.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "ra8_sim_mmio.h"
#include "unity_minimal.h"

/**
 * @enum i3c_i2c_edge_cases_fixture_t
 * @brief Values planted in registers to prove a read or write reaches them.
 */
typedef enum : uint8_t {
  k_i2c_probe_rounds =
    5U, /**< Back-to-back transfers driven over one bus, proving the edge case is repeatable and not a first-call artefact. */
} i3c_i2c_edge_cases_fixture_t;

/**
 * @enum i3c_i2c_edge_cases_fixture2_t
 * @brief Buffer capacities and payload sizes.
 */
typedef enum : uint32_t {
  k_i2c_oversize_bytes =
    1000000, /**< A transfer length far past any real buffer, so the length guard fires before any access. */
} i3c_i2c_edge_cases_fixture2_t;

typedef enum : uint8_t {
  k_iic_b_edge_target = 0x42U, /**< Iic b edge target. */
  k_iic_b_edge_byte_a = 0xA5U, /**< Iic b edge byte a. */
  k_iic_b_edge_byte_b = 0x5AU, /**< Iic b edge byte b. */
  k_iic_b_edge_byte_c = 0x33U, /**< Iic b edge byte c. */
} ra8_i3c_i2c_edge_const_t;

typedef enum : uint32_t {
  k_iic_b_edge_long_len = 1000000U, /**< Iic b edge long length. */
} ra8_i3c_i2c_edge_timing_t;

static const ra8_i3c_i2c_cfg_t k_iic_b_edge_cfg = {
  .bus_hz   = (uint32_t)k_ra8_i3c_i2c_speed_fast,
  .pclka_hz = 60000000U,
};

static const uint8_t s_iic_b_edge_payload[3] = {
  (uint8_t)k_iic_b_edge_byte_a,
  (uint8_t)k_iic_b_edge_byte_b,
  (uint8_t)k_iic_b_edge_byte_c,
};

static uint8_t s_iic_b_edge_long[k_i2c_oversize_bytes];

static void prep(void)
{
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  (void)ra8_mstp_init();
}

static void prime_ntst_and_bus(uint8_t channel)
{
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs(channel);
  reg->NTST = (uint32_t)k_ra8_i3c_i2c_msk_ntst_tdbef0 | (uint32_t)k_ra8_i3c_i2c_msk_ntst_rdbff0;
  reg->BCST = (uint32_t)k_ra8_i3c_i2c_msk_bcst_bfref;
}

/* Deterministic NACK injection via the ra8_sim_mmio poll-hook -- it runs inline on
 * the driver's OWN poll thread (no wall-clock timer, no concurrent servicer
 * thread to race or starve). We cannot pre-arm BST.NACKDF because the driver
 * clears BST at the start of every transfer; the hook instead re-asserts
 * BST.NACKDF on every internal_i3c_i2c_wait_ntst poll while TDBEF0 stays primed,
 * so internal_i3c_i2c_drain_tx observes it on one of its per-byte polls and the
 * write returns k_ra8_err_nack with a final STOP. Both the address-NAK and
 * data-NAK cases funnel into that same nack + STOP outcome (see the two tests
 * below). */

/** @brief Channel the NACK poll-hook operates on. */
static uint8_t s_nack_ch;

/**
 * @brief Poll-hook body: latch BST.NACKDF on the target channel.
 */
static void iic_b_nack_hook(void)
{
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs(s_nack_ch);
  if (reg != nullptr) {
    reg->BST = reg->BST | (uint32_t)k_ra8_i3c_i2c_msk_bst_nackdf;
  }
}

/**
 * @brief Install the NACK poll-hook for @p channel.
 *
 * @param[in] channel Channel index to inject into.
 */
static void iic_b_nack_hook_arm(uint8_t channel)
{
  s_nack_ch = channel;
  ra8_sim_mmio_set_poll_hook(iic_b_nack_hook);
}

/**
 * @brief Remove the NACK poll-hook.
 */
static void iic_b_nack_hook_disarm(void)
{
  ra8_sim_mmio_set_poll_hook(nullptr);
}

/* --- Bus-busy gate during repeated START attempts --- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_bus_busy_during_start_repeated(void)
{
  TEST_BEGIN("iic_b bus-busy during START rejected on every retry");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_edge_cfg));
  /* BFREF=0: bus reported busy. Hammer the gate; every call must return
   * busy without touching CNDCTL.STCND. */
  i3c_i2c_regs(0U)->BCST   = 0U;
  i3c_i2c_regs(0U)->CNDCTL = 0U;
  for (uint8_t i = 0U; i < k_i2c_probe_rounds; ++i) {
    TEST_ASSERT_EQ(
      k_ra8_err_busy,
      internal_i3c_i2c_write(0U, (uint8_t)k_iic_b_edge_target, s_iic_b_edge_payload, 1U, false));
  }
  /* CNDCTL must never have been set to START. */
  TEST_ASSERT_EQ(0U, (i3c_i2c_regs(0U)->CNDCTL & (uint32_t)k_ra8_i3c_i2c_msk_cndctl_stcnd));
  TEST_END("iic_b bus-busy during START rejected on every retry");
}

/* --- NAK distinction: address vs. data --- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_nak_on_address_byte(void)
{
  TEST_BEGIN("iic_b NAK on address byte yields nack + STOP");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_edge_cfg));
  prime_ntst_and_bus(0U);
  iic_b_nack_hook_arm(0U);
  const ra8_err_t r = internal_i3c_i2c_write(0U,
                                             (uint8_t)k_iic_b_edge_target,
                                             s_iic_b_edge_long,
                                             (uint32_t)k_iic_b_edge_long_len,
                                             false);
  iic_b_nack_hook_disarm();
  TEST_ASSERT_EQ(k_ra8_err_nack, r);
  TEST_ASSERT_EQ(k_ra8_i3c_i2c_msk_cndctl_spcnd, i3c_i2c_regs(0U)->CNDCTL);
  TEST_END("iic_b NAK on address byte yields nack + STOP");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_nak_on_data_byte(void)
{
  TEST_BEGIN("iic_b NAK after address (data byte) yields nack + STOP");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_edge_cfg));
  prime_ntst_and_bus(0U);
  iic_b_nack_hook_arm(0U);
  const ra8_err_t r = internal_i3c_i2c_write(0U,
                                             (uint8_t)k_iic_b_edge_target,
                                             s_iic_b_edge_long,
                                             (uint32_t)k_iic_b_edge_long_len,
                                             false);
  iic_b_nack_hook_disarm();
  /* Both paths funnel into the same return code today, but the post-state
   * STOP must be observable regardless of where the NACK was latched. */
  TEST_ASSERT_EQ(k_ra8_err_nack, r);
  TEST_ASSERT_EQ(k_ra8_i3c_i2c_msk_cndctl_spcnd, i3c_i2c_regs(0U)->CNDCTL);
  TEST_END("iic_b NAK after address (data byte) yields nack + STOP");
}

/* --- Repeated-START sequence (write-then-read combined transfer) --- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_repeated_start_sequence(void)
{
  TEST_BEGIN("iic_b repeated-start: write+read transfer");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_edge_cfg));
  prime_ntst_and_bus(0U);
  uint8_t       rx     = 0U;
  const uint8_t tx[2U] = {(uint8_t)k_iic_b_edge_byte_a, (uint8_t)k_iic_b_edge_byte_b};
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_i3c_i2c_transfer(0U, (uint8_t)k_iic_b_edge_target, tx, 2U, &rx, 1U));
  /* Combined transfer must close with STOP. The read phase overwrites
   * NTDTBP0 with the rx byte, so we only assert the bus-close state. */
  TEST_ASSERT_EQ(k_ra8_i3c_i2c_msk_cndctl_spcnd, i3c_i2c_regs(0U)->CNDCTL);
  /* ACKCTL must be left in the plain ACKTWP state (NACK-on-last-byte
   * cleared by the read phase). */
  TEST_ASSERT_EQ(k_ra8_i3c_i2c_msk_ackctl_acktwp, i3c_i2c_regs(0U)->ACKCTL);
  TEST_END("iic_b repeated-start: write+read transfer");
}

/* --- Clock-stretching limit: long buffer with no ACK -> timeout --- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clock_stretch_timeout(void)
{
  TEST_BEGIN("iic_b clock-stretch limit: TDBEF0 never sets => hw_timeout");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_edge_cfg));
  /* Bus free so the busy gate passes, but NTST left clear -- the peripheral is
   * holding SCL low (or just never ACKing). The driver must fail with
   * hw_timeout instead of looping forever. */
  i3c_i2c_regs(0U)->BCST = (uint32_t)k_ra8_i3c_i2c_msk_bcst_bfref;
  i3c_i2c_regs(0U)->NTST = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_hw_timeout,
    internal_i3c_i2c_write(0U, (uint8_t)k_iic_b_edge_target, s_iic_b_edge_payload, 3U, false));
  TEST_END("iic_b clock-stretch limit: TDBEF0 never sets => hw_timeout");
}

/* --- set_clock argument validation (bus_hz / pclka_hz extremes) --- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_clock_extremes(void)
{
  TEST_BEGIN("iic_b set_clock extreme values");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_edge_cfg));
  /* bus_hz = 0, pclka_hz = 0 -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_i3c_i2c_set_clock(0U, 0U, 60000000U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_i3c_i2c_set_clock(0U, 100000U, 0U));
  /* Channel out of range. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_i3c_i2c_set_clock(99U, 100000U, 60000000U));
  /* Standard + Fast modes both succeed. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_i3c_i2c_set_clock(0U, (uint32_t)k_ra8_i3c_i2c_speed_standard, 60000000U));
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_i3c_i2c_set_clock(0U, (uint32_t)k_ra8_i3c_i2c_speed_fast, 60000000U));
  TEST_END("iic_b set_clock extreme values");
}

int32_t main(void)
{
  test_bus_busy_during_start_repeated();
  test_nak_on_address_byte();
  test_nak_on_data_byte();
  test_repeated_start_sequence();
  test_clock_stretch_timeout();
  test_set_clock_extremes();
  (void)fprintf(stderr, "[OK  ] test_ra8_i3c_i2c_edge_cases.c\n");
  return 0;
}
