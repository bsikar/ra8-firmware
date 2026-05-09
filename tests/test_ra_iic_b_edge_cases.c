/**
 * @file test_ra_iic_b_edge_cases.c
 * @brief Edge-case + stress unit tests for the IIC_B (I3C in I2C-mode) controller.
 *
 * @details
 * Complements ``test_ra_iic_b.c`` with focused stress / edge-case
 * coverage of safety-critical paths:
 *
 *   - bus-busy (BFREF=0) timeout during the START gate;
 *   - NAK on the address byte vs. NAK on a data byte (both must yield
 *     ``k_ra_err_nack`` with a final STOP);
 *   - repeated-START sequences via ``ra_iic_b_transfer`` (write -> read);
 *   - clock-stretching upper bound: a long-buffer transfer where the
 *     peripheral never ACKs (TDBEF0 stays low for the duration) returns the
 *     hardware-timeout error rather than spinning forever;
 *   - ``set_clock`` rejects out-of-range bus_hz / pclka_hz.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <signal.h>
#include <stdint.h>
#include <sys/time.h>

#include "ra8d2_iic_b_regs.h"
#include "ra_err.h"
#include "ra_iic_b.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_iic_b_edge_target = 0x42U,
  k_iic_b_edge_byte_a = 0xA5U,
  k_iic_b_edge_byte_b = 0x5AU,
  k_iic_b_edge_byte_c = 0x33U,
} ra_iic_b_edge_const_t;

typedef enum : uint32_t {
  k_iic_b_edge_alarm_us = 100U,
  k_iic_b_edge_long_len = 1000000U,
} ra_iic_b_edge_timing_t;

static const ra_iic_b_cfg_t k_iic_b_edge_cfg = {
  .bus_hz   = (uint32_t)k_ra_iic_b_speed_fast,
  .pclka_hz = 60000000U,
};

static const uint8_t s_iic_b_edge_payload[3] = {
  (uint8_t)k_iic_b_edge_byte_a,
  (uint8_t)k_iic_b_edge_byte_b,
  (uint8_t)k_iic_b_edge_byte_c,
};

static uint8_t s_iic_b_edge_long[1000000];

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
}

static void prime_ntst_and_bus(uint8_t channel)
{
  volatile r_iic_b_regs_t* reg = ra_iic_b(channel);
  reg->NTST = (uint32_t)k_ra_iic_b_msk_ntst_tdbef0 | (uint32_t)k_ra_iic_b_msk_ntst_rdbff0;
  reg->BCST = (uint32_t)k_ra_iic_b_msk_bcst_bfref;
}

/* SIGALRM-driven NACK injection. We cannot pre-arm BST.NACKDF because the
 * driver clears BST at the start of every transfer; injecting via signal
 * mid-transfer mirrors the asynchronous hardware path. */
static volatile bool s_iic_b_inject_nack_addr = false;
static volatile bool s_iic_b_inject_nack_data = false;
static uint8_t       s_iic_b_alarm_channel    = 0U;
static uint32_t      s_iic_b_byte_seen        = 0U;

static void edge_alarm(int sig)
{
  (void)sig;
  volatile r_iic_b_regs_t* reg = ra_iic_b(s_iic_b_alarm_channel);
  if (reg == nullptr) {
    return;
  }
  if (s_iic_b_inject_nack_addr) {
    /* Address-byte NAK: latch NACKDF before the driver advances past
     * the address-out wait. */
    reg->BST  = (uint32_t)k_ra_iic_b_msk_bst_nackdf;
    reg->NTST = 0U;
  } else if (s_iic_b_inject_nack_data) {
    /* Data-byte NAK: only fire after enough TDBEF0 cycles to advance
     * past the address byte. */
    ++s_iic_b_byte_seen;
    if (s_iic_b_byte_seen >= 2U) {
      reg->BST  = (uint32_t)k_ra_iic_b_msk_bst_nackdf;
      reg->NTST = 0U;
    }
  }
}

static void arm_alarm(uint8_t channel)
{
  s_iic_b_alarm_channel = channel;
  s_iic_b_byte_seen     = 0U;
  struct sigaction sa;
  sa.sa_handler = edge_alarm;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  (void)sigaction(SIGALRM, &sa, nullptr);
  struct itimerval t;
  t.it_value.tv_sec     = 0;
  t.it_value.tv_usec    = (long)k_iic_b_edge_alarm_us;
  t.it_interval.tv_sec  = 0;
  t.it_interval.tv_usec = (long)k_iic_b_edge_alarm_us;
  (void)setitimer(ITIMER_REAL, &t, nullptr);
}

static void disarm_alarm(void)
{
  s_iic_b_inject_nack_addr = false;
  s_iic_b_inject_nack_data = false;
  struct itimerval t       = {};
  (void)setitimer(ITIMER_REAL, &t, nullptr);
  struct sigaction sa;
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  (void)sigaction(SIGALRM, &sa, nullptr);
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
  TEST_ASSERT_EQ(k_ra_ok, ra_iic_b_init(0U, &k_iic_b_edge_cfg));
  /* BFREF=0: bus reported busy. Hammer the gate; every call must return
   * busy without touching CNDCTL.STCND. */
  ra_iic_b(0U)->BCST   = 0U;
  ra_iic_b(0U)->CNDCTL = 0U;
  for (uint8_t i = 0U; i < 5U; ++i) {
    TEST_ASSERT_EQ(
      k_ra_err_busy,
      ra_iic_b_write(0U, (uint8_t)k_iic_b_edge_target, s_iic_b_edge_payload, 1U, false));
  }
  /* CNDCTL must never have been set to START. */
  TEST_ASSERT_EQ(0U, (ra_iic_b(0U)->CNDCTL & (uint32_t)k_ra_iic_b_msk_cndctl_stcnd));
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
  TEST_ASSERT_EQ(k_ra_ok, ra_iic_b_init(0U, &k_iic_b_edge_cfg));
  prime_ntst_and_bus(0U);
  s_iic_b_inject_nack_addr = true;
  s_iic_b_inject_nack_data = false;
  arm_alarm(0U);
  const ra_err_t r = ra_iic_b_write(0U,
                                    (uint8_t)k_iic_b_edge_target,
                                    s_iic_b_edge_long,
                                    (uint32_t)k_iic_b_edge_long_len,
                                    false);
  disarm_alarm();
  TEST_ASSERT_EQ(k_ra_err_nack, r);
  TEST_ASSERT_EQ(k_ra_iic_b_msk_cndctl_spcnd, ra_iic_b(0U)->CNDCTL);
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
  TEST_ASSERT_EQ(k_ra_ok, ra_iic_b_init(0U, &k_iic_b_edge_cfg));
  prime_ntst_and_bus(0U);
  s_iic_b_inject_nack_addr = false;
  s_iic_b_inject_nack_data = true;
  arm_alarm(0U);
  const ra_err_t r = ra_iic_b_write(0U,
                                    (uint8_t)k_iic_b_edge_target,
                                    s_iic_b_edge_long,
                                    (uint32_t)k_iic_b_edge_long_len,
                                    false);
  disarm_alarm();
  /* Both paths funnel into the same return code today, but the post-state
   * STOP must be observable regardless of where the NACK was latched. */
  TEST_ASSERT_EQ(k_ra_err_nack, r);
  TEST_ASSERT_EQ(k_ra_iic_b_msk_cndctl_spcnd, ra_iic_b(0U)->CNDCTL);
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
  TEST_ASSERT_EQ(k_ra_ok, ra_iic_b_init(0U, &k_iic_b_edge_cfg));
  prime_ntst_and_bus(0U);
  uint8_t       rx     = 0U;
  const uint8_t tx[2U] = {(uint8_t)k_iic_b_edge_byte_a, (uint8_t)k_iic_b_edge_byte_b};
  TEST_ASSERT_EQ(k_ra_ok, ra_iic_b_transfer(0U, (uint8_t)k_iic_b_edge_target, tx, 2U, &rx, 1U));
  /* Combined transfer must close with STOP. The read phase overwrites
   * NTDTBP0 with the rx byte, so we only assert the bus-close state. */
  TEST_ASSERT_EQ(k_ra_iic_b_msk_cndctl_spcnd, ra_iic_b(0U)->CNDCTL);
  /* ACKCTL must be left in the plain ACKTWP state (NACK-on-last-byte
   * cleared by the read phase). */
  TEST_ASSERT_EQ(k_ra_iic_b_msk_ackctl_acktwp, ra_iic_b(0U)->ACKCTL);
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
  TEST_ASSERT_EQ(k_ra_ok, ra_iic_b_init(0U, &k_iic_b_edge_cfg));
  /* Bus free so the busy gate passes, but NTST left clear -- the peripheral is
   * holding SCL low (or just never ACKing). The driver must fail with
   * hw_timeout instead of looping forever. */
  ra_iic_b(0U)->BCST = (uint32_t)k_ra_iic_b_msk_bcst_bfref;
  ra_iic_b(0U)->NTST = 0U;
  TEST_ASSERT_EQ(k_ra_err_hw_timeout,
                 ra_iic_b_write(0U, (uint8_t)k_iic_b_edge_target, s_iic_b_edge_payload, 3U, false));
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
  TEST_ASSERT_EQ(k_ra_ok, ra_iic_b_init(0U, &k_iic_b_edge_cfg));
  /* bus_hz = 0, pclka_hz = 0 -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_iic_b_set_clock(0U, 0U, 60000000U));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_iic_b_set_clock(0U, 100000U, 0U));
  /* Channel out of range. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_iic_b_set_clock(99U, 100000U, 60000000U));
  /* Standard + Fast modes both succeed. */
  TEST_ASSERT_EQ(k_ra_ok, ra_iic_b_set_clock(0U, (uint32_t)k_ra_iic_b_speed_standard, 60000000U));
  TEST_ASSERT_EQ(k_ra_ok, ra_iic_b_set_clock(0U, (uint32_t)k_ra_iic_b_speed_fast, 60000000U));
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
  (void)fprintf(stderr, "[OK  ] test_ra_iic_b_edge_cases.c\n");
  return 0;
}
