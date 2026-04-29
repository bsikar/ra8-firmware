/**
 * @file test_ra_iic_b.c
 * @brief Unit tests for the IIC_B (I3C in I2C-only mode) master driver.
 *
 * @details
 * Drives the polling-mode IIC_B master against the host-side
 * ``ra_sim_mmap`` substrate. Status flags are pre-armed where the
 * driver expects them (BST.STCNDDF, BST.SPCNDDF, NTST.TDBEF0,
 * NTST.RDBFF0, BST.TENDF) so the wait loops fall through immediately.
 *
 * BCST.BFREF is also pre-armed (= bus free) ahead of every transfer
 * test so the new bus-busy gate (mirrors FSP
 * ``iic_b_master_run_hw_master`` BFREF check) does not falsely
 * reject the transaction.
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

/**
 * @enum ra_iic_b_test_addr_t
 * @brief Test addresses and payload constants.
 */
typedef enum : uint8_t {
  k_ra_iic_b_test_target  = 0x50U,
  k_ra_iic_b_test_byte_a  = 0xA5U,
  k_ra_iic_b_test_byte_b  = 0x5AU,
  k_ra_iic_b_test_byte_c  = 0x33U,
  k_ra_iic_b_test_rx_byte = 0xC3U,
} ra_iic_b_test_addr_t;

/**
 * @enum ra_iic_b_test_ch_t
 * @brief Channel constants used by tests.
 */
typedef enum : uint8_t {
  k_ra_iic_b_test_ch_zero = 0U,
  k_ra_iic_b_test_ch_oor  = 1U, /**< IIC_B has only ch 0 on RA8D2. */
  k_ra_iic_b_test_ch_huge = 200U,
} ra_iic_b_test_ch_t;

/**
 * @enum ra_iic_b_test_wait_t
 * @brief Alarm interval used by the long-buffer timeout tests.
 */
typedef enum : uint32_t {
  k_ra_iic_b_test_alarm_usec = 100U,
  k_ra_iic_b_test_long_len   = 1000000U,
} ra_iic_b_test_wait_t;

/**
 * @enum ra_iic_b_test_addr_byte_t
 * @brief Pre-shifted address bytes for write / read transactions.
 */
typedef enum : uint8_t {
  k_ra_iic_b_test_addr_w = (uint8_t)(k_ra_iic_b_test_target << 1U),       /**< 0xA0. */
  k_ra_iic_b_test_addr_r = (uint8_t)((k_ra_iic_b_test_target << 1U) | 1U) /**< 0xA1. */
} ra_iic_b_test_addr_byte_t;

static const uint8_t s_payload[2] = {
  (uint8_t)k_ra_iic_b_test_byte_a,
  (uint8_t)k_ra_iic_b_test_byte_b,
};

static uint8_t s_long_buffer[1000000];

static const ra_iic_b_cfg_t k_iic_b_cfg = {
  .bus_hz   = (uint32_t)k_ra_iic_b_speed_fast,
  .pclka_hz = 60000000U,
};

/**
 * @brief Pre-arm NTST so the driver's address + data wait loops fall
 *        through immediately, and BCST.BFREF so the bus-busy gate
 *        passes. The driver's clear_bst step does not touch NTST,
 *        so a one-shot pre-prime survives the transfer.
 */
static void prime_ntst(uint8_t channel)
{
  volatile r_iic_b_regs_t* reg = ra_iic_b(channel);
  reg->NTST = (uint32_t)k_ra_iic_b_msk_ntst_tdbef0 | (uint32_t)k_ra_iic_b_msk_ntst_rdbff0;
  reg->BCST = (uint32_t)k_ra_iic_b_msk_bcst_bfref;
}

/**
 * @brief Reset the simulator and ensure MSTP / channel state is fresh.
 */
static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
}

/* =============================================================================
 * Init / deinit
 * =============================================================================
 */

static void test_init_configured(void)
{
  TEST_BEGIN("ra_iic_b_init: BCTL.BUSE set, STDBR programmed");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));
  volatile const r_iic_b_regs_t* reg = ra_iic_b(0U);
  TEST_ASSERT((reg->BCTL & (uint32_t)k_ra_iic_b_msk_bctl_buse) != 0U);
  TEST_ASSERT(reg->STDBR != 0U);
  TEST_END("ra_iic_b_init: BCTL.BUSE set, STDBR programmed");
}

static void test_init_bad_inputs(void)
{
  TEST_BEGIN("ra_iic_b_init: bad inputs rejected");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_iic_b_init(0U, nullptr));
  ra_iic_b_cfg_t bad = k_iic_b_cfg;
  bad.bus_hz         = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_iic_b_init(0U, &bad));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_iic_b_init((uint8_t)k_ra_iic_b_test_ch_oor, &k_iic_b_cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_iic_b_init((uint8_t)k_ra_iic_b_test_ch_huge, &k_iic_b_cfg));
  TEST_END("ra_iic_b_init: bad inputs rejected");
}

static void test_deinit_releases(void)
{
  TEST_BEGIN("ra_iic_b_deinit: BCTL cleared, MSTP gated");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_deinit(0U));
  bool stopped = false;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mstp_is_stopped(k_ra_mstp_i3c, &stopped));
  TEST_ASSERT(stopped);
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_iic_b_deinit((uint8_t)k_ra_iic_b_test_ch_oor));
  TEST_END("ra_iic_b_deinit: BCTL cleared, MSTP gated");
}

static void test_set_clock_updates(void)
{
  TEST_BEGIN("ra_iic_b_set_clock: STDBR changes");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));
  const uint32_t before = ra_iic_b(0U)->STDBR;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_iic_b_set_clock(0U, (uint32_t)k_ra_iic_b_speed_standard, 60000000U));
  TEST_ASSERT(ra_iic_b(0U)->STDBR != before);
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_iic_b_set_clock(0U, 0U, 60000000U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_iic_b_set_clock(0U, 100000U, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_iic_b_set_clock((uint8_t)k_ra_iic_b_test_ch_oor, 100000U, 60000000U));
  TEST_END("ra_iic_b_set_clock: STDBR changes");
}

/* =============================================================================
 * Polling write
 * =============================================================================
 */

static void test_write_happy(void)
{
  TEST_BEGIN("ra_iic_b_write: 2-byte payload, START + STOP pulsed");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));
  prime_ntst(0U);
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_iic_b_write(0U, (uint8_t)k_ra_iic_b_test_target, s_payload, 2U, false));
  /* Final byte should land in NTDTBP0; CNDCTL last write should be the
   * STOP request (mirrors what the bus would observe after the
   * transaction). */
  volatile const r_iic_b_regs_t* reg = ra_iic_b(0U);
  TEST_ASSERT_EQ((int32_t)k_ra_iic_b_test_byte_b, (int32_t)(reg->NTDTBP0 & 0xFFU));
  TEST_ASSERT_EQ((int32_t)(uint32_t)k_ra_iic_b_msk_cndctl_spcnd, (int32_t)reg->CNDCTL);
  TEST_END("ra_iic_b_write: 2-byte payload, START + STOP pulsed");
}

static void test_write_zero_length(void)
{
  TEST_BEGIN("ra_iic_b_write: zero-length is a probe");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));
  prime_ntst(0U);
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_iic_b_write(0U, (uint8_t)k_ra_iic_b_test_target, s_payload, 0U, false));
  TEST_END("ra_iic_b_write: zero-length is a probe");
}

static void test_write_null_data(void)
{
  TEST_BEGIN("ra_iic_b_write: null data rejected");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_iic_b_write(0U, (uint8_t)k_ra_iic_b_test_target, nullptr, 1U, false));
  TEST_END("ra_iic_b_write: null data rejected");
}

static void test_write_bad_channel(void)
{
  TEST_BEGIN("ra_iic_b_write: channel out of range");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_iic_b_write((uint8_t)k_ra_iic_b_test_ch_oor,
                                         (uint8_t)k_ra_iic_b_test_target,
                                         s_payload,
                                         1U,
                                         false));
  TEST_END("ra_iic_b_write: channel out of range");
}

static void test_write_timeout_on_start(void)
{
  TEST_BEGIN("ra_iic_b_write: TDBEF0 never sets => timeout");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));
  /* Mark the bus free so the busy gate passes, but leave NTST cleared
   * so the address-byte send loop times out. */
  ra_iic_b(0U)->BCST = (uint32_t)k_ra_iic_b_msk_bcst_bfref;
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_hw_timeout,
    (int32_t)ra_iic_b_write(0U, (uint8_t)k_ra_iic_b_test_target, s_payload, 1U, false));
  TEST_END("ra_iic_b_write: TDBEF0 never sets => timeout");
}

static void test_write_busy_rejection(void)
{
  TEST_BEGIN("ra_iic_b_write: BCST.BFREF=0 => k_ra_err_busy");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));
  /* BFREF clear == bus busy. NTST primed so the call would otherwise
   * proceed -- the busy gate must fire first. */
  ra_iic_b(0U)->NTST = (uint32_t)k_ra_iic_b_msk_ntst_tdbef0 | (uint32_t)k_ra_iic_b_msk_ntst_rdbff0;
  ra_iic_b(0U)->BCST = 0U;
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_busy,
    (int32_t)ra_iic_b_write(0U, (uint8_t)k_ra_iic_b_test_target, s_payload, 1U, false));
  TEST_END("ra_iic_b_write: BCST.BFREF=0 => k_ra_err_busy");
}

/* Channel that the SIGALRM handlers operate on. Shared by the
 * mid-transfer NACK injector below and the long-buffer break helpers
 * further down. */
static uint8_t s_alarm_channel = 0U;

/* Alarm handler used by the NACK-detection test to inject NACKDF
 * after the driver's start-of-transaction clear_bst step has run. The
 * setitimer interval is short enough that the alarm fires while the
 * driver is still spinning in the post-address NTST wait loop. */
static volatile bool s_inject_nack = false;

static void sigalarm_handler_nack(int sig)
{
  (void)sig;
  if (s_inject_nack) {
    volatile r_iic_b_regs_t* reg = ra_iic_b(s_alarm_channel);
    if (reg != nullptr) {
      reg->BST = (uint32_t)k_ra_iic_b_msk_bst_nackdf;
      /* Also clear NTST so the spin loop falls through to the
       * post-data status check that observes NACKDF. */
      reg->NTST = 0U;
    }
  }
}

static void arm_nack_alarm(uint8_t channel)
{
  s_alarm_channel = channel;
  s_inject_nack   = true;
  struct sigaction sa;
  sa.sa_handler = sigalarm_handler_nack;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  (void)sigaction(SIGALRM, &sa, nullptr);
  struct itimerval timer;
  timer.it_value.tv_sec     = 0;
  timer.it_value.tv_usec    = (long)k_ra_iic_b_test_alarm_usec;
  timer.it_interval.tv_sec  = 0;
  timer.it_interval.tv_usec = (long)k_ra_iic_b_test_alarm_usec;
  (void)setitimer(ITIMER_REAL, &timer, nullptr);
}

static void disarm_nack_alarm(void)
{
  s_inject_nack          = false;
  struct itimerval timer = {};
  (void)setitimer(ITIMER_REAL, &timer, nullptr);
  struct sigaction sa;
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  (void)sigaction(SIGALRM, &sa, nullptr);
}

static void test_write_nack_returns_nack_and_stops(void)
{
  TEST_BEGIN("ra_iic_b_write: NACKDF latched mid-transfer => k_ra_err_nack + STOP");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));
  prime_ntst(0U);
  /* Use SIGALRM to inject NACKDF mid-transfer: the driver's
   * start-of-transaction clear_bst would otherwise wipe a pre-armed
   * value. The alarm fires while the driver is in the long payload
   * spin loop, mirroring how the real hardware would latch NACKDF
   * asynchronously when the slave declines. */
  arm_nack_alarm(0U);
  const ra_err_t err = ra_iic_b_write(0U,
                                      (uint8_t)k_ra_iic_b_test_target,
                                      s_long_buffer,
                                      (uint32_t)k_ra_iic_b_test_long_len,
                                      false);
  disarm_nack_alarm();
  TEST_ASSERT_EQ((int32_t)k_ra_err_nack, (int32_t)err);
  /* CNDCTL last write should be STOP. */
  TEST_ASSERT_EQ((int32_t)(uint32_t)k_ra_iic_b_msk_cndctl_spcnd, (int32_t)ra_iic_b(0U)->CNDCTL);
  TEST_END("ra_iic_b_write: NACKDF latched mid-transfer => k_ra_err_nack + STOP");
}

static void test_write_restart_holds_bus(void)
{
  TEST_BEGIN("ra_iic_b_write: restart=true holds bus, no STOP issued");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));
  prime_ntst(0U);
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_iic_b_write(0U, (uint8_t)k_ra_iic_b_test_target, s_payload, 1U, true));
  /* CNDCTL last write should be START (no subsequent STOP). */
  TEST_ASSERT_EQ((int32_t)(uint32_t)k_ra_iic_b_msk_cndctl_stcnd, (int32_t)ra_iic_b(0U)->CNDCTL);
  TEST_END("ra_iic_b_write: restart=true holds bus, no STOP issued");
}

/* =============================================================================
 * Polling read
 * =============================================================================
 */

static void test_read_happy(void)
{
  TEST_BEGIN("ra_iic_b_read: 2-byte read returns ok, ACKBT set on last byte");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));
  prime_ntst(0U);
  uint8_t buf[2] = {0U, 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_iic_b_read(0U, (uint8_t)k_ra_iic_b_test_target, buf, 2U, false));
  /* Real-bus byte exchange cannot be modelled by the host substrate
   * (NTDTBP0 is one register, not a FIFO that holds the address byte
   * separately from rx data). What we DO observe:
   *   1. ACKCTL was set to (ACKTWP | ACKT) somewhere in the flow to
   *      arm the NACK on the last byte; the driver then restores it
   *      to plain ACKTWP. The "restored to plain ACKTWP" final state
   *      is the ABI we lock in.
   *   2. CNDCTL last write is STOP. */
  TEST_ASSERT_EQ((int32_t)(uint32_t)k_ra_iic_b_msk_ackctl_acktwp, (int32_t)(ra_iic_b(0U)->ACKCTL));
  TEST_ASSERT_EQ((int32_t)(uint32_t)k_ra_iic_b_msk_cndctl_spcnd, (int32_t)ra_iic_b(0U)->CNDCTL);
  TEST_END("ra_iic_b_read: 2-byte read returns ok, ACKBT set on last byte");
}

static void test_read_zero_length_rejected(void)
{
  TEST_BEGIN("ra_iic_b_read: len==0 rejected");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));
  uint8_t buf = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_iic_b_read(0U, (uint8_t)k_ra_iic_b_test_target, &buf, 0U, false));
  TEST_END("ra_iic_b_read: len==0 rejected");
}

static void test_read_null_out(void)
{
  TEST_BEGIN("ra_iic_b_read: null buf rejected");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_iic_b_read(0U, (uint8_t)k_ra_iic_b_test_target, nullptr, 1U, false));
  TEST_END("ra_iic_b_read: null buf rejected");
}

static void test_read_bad_channel(void)
{
  TEST_BEGIN("ra_iic_b_read: channel out of range");
  prep();
  uint8_t buf = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_iic_b_read((uint8_t)k_ra_iic_b_test_ch_oor,
                                        (uint8_t)k_ra_iic_b_test_target,
                                        &buf,
                                        1U,
                                        false));
  TEST_END("ra_iic_b_read: channel out of range");
}

static void test_read_timeout_on_start(void)
{
  TEST_BEGIN("ra_iic_b_read: TDBEF0 never sets => timeout");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));
  ra_iic_b(0U)->BCST = (uint32_t)k_ra_iic_b_msk_bcst_bfref;
  uint8_t buf        = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_hw_timeout,
                 (int32_t)ra_iic_b_read(0U, (uint8_t)k_ra_iic_b_test_target, &buf, 1U, false));
  TEST_END("ra_iic_b_read: TDBEF0 never sets => timeout");
}

static void test_read_busy_rejection(void)
{
  TEST_BEGIN("ra_iic_b_read: BCST.BFREF=0 => k_ra_err_busy");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));
  ra_iic_b(0U)->NTST = (uint32_t)k_ra_iic_b_msk_ntst_tdbef0 | (uint32_t)k_ra_iic_b_msk_ntst_rdbff0;
  ra_iic_b(0U)->BCST = 0U;
  uint8_t buf        = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_busy,
                 (int32_t)ra_iic_b_read(0U, (uint8_t)k_ra_iic_b_test_target, &buf, 1U, false));
  TEST_END("ra_iic_b_read: BCST.BFREF=0 => k_ra_err_busy");
}

/* =============================================================================
 * Combined transfer
 * =============================================================================
 */

static void test_transfer_happy(void)
{
  TEST_BEGIN("ra_iic_b_transfer: write-then-RESTART-then-read");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));
  prime_ntst(0U);
  uint8_t       rx     = 0U;
  const uint8_t tx[1U] = {(uint8_t)k_ra_iic_b_test_byte_c};
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_iic_b_transfer(0U, (uint8_t)k_ra_iic_b_test_target, tx, 1U, &rx, 1U));
  /* CNDCTL last write should be STOP -- the read phase always closes
   * the bus regardless of the prior write phase having held it. */
  TEST_ASSERT_EQ((int32_t)(uint32_t)k_ra_iic_b_msk_cndctl_spcnd, (int32_t)ra_iic_b(0U)->CNDCTL);
  TEST_END("ra_iic_b_transfer: write-then-RESTART-then-read");
}

static void test_transfer_null_args_rejected(void)
{
  TEST_BEGIN("ra_iic_b_transfer: null buffers / zero lens rejected");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));
  uint8_t rx = 0U;

  /* Both lens zero -> invalid_arg. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_iic_b_transfer(0U, (uint8_t)k_ra_iic_b_test_target, nullptr, 0U, nullptr, 0U));
  /* tx_len > 0 with NULL tx -> null_ptr. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_null_ptr,
    (int32_t)ra_iic_b_transfer(0U, (uint8_t)k_ra_iic_b_test_target, nullptr, 1U, &rx, 1U));
  /* rx_len > 0 with NULL rx -> null_ptr. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_null_ptr,
    (int32_t)ra_iic_b_transfer(0U, (uint8_t)k_ra_iic_b_test_target, s_payload, 1U, nullptr, 1U));
  /* Out-of-range channel -> null_ptr (matches the rest of the API). */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_iic_b_transfer((uint8_t)k_ra_iic_b_test_ch_oor,
                                            (uint8_t)k_ra_iic_b_test_target,
                                            s_payload,
                                            1U,
                                            &rx,
                                            1U));
  TEST_END("ra_iic_b_transfer: null buffers / zero lens rejected");
}

static void test_transfer_busy_rejection(void)
{
  TEST_BEGIN("ra_iic_b_transfer: bus-busy => k_ra_err_busy");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));
  ra_iic_b(0U)->NTST = (uint32_t)k_ra_iic_b_msk_ntst_tdbef0 | (uint32_t)k_ra_iic_b_msk_ntst_rdbff0;
  ra_iic_b(0U)->BCST = 0U;
  uint8_t rx         = 0U;
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_busy,
    (int32_t)ra_iic_b_transfer(0U, (uint8_t)k_ra_iic_b_test_target, s_payload, 1U, &rx, 1U));
  TEST_END("ra_iic_b_transfer: bus-busy => k_ra_err_busy");
}

/* =============================================================================
 * Abort
 * =============================================================================
 */

static void test_abort_resets_channel(void)
{
  TEST_BEGIN("ra_iic_b_abort: BIE/NTIE masked, STOP issued, BST cleared");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));
  /* Pretend a transfer is in progress: BIE/NTIE non-zero, BST has
   * latched flags. */
  ra_iic_b(0U)->BIE  = (uint32_t)k_ra_iic_b_msk_bie_nackdie;
  ra_iic_b(0U)->NTIE = (uint32_t)k_ra_iic_b_msk_ntie_tdbeie0;
  ra_iic_b(0U)->BST  = (uint32_t)k_ra_iic_b_msk_bst_nackdf | (uint32_t)k_ra_iic_b_msk_bst_alf;

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_abort(0U));
  TEST_ASSERT_EQ(0, (int32_t)ra_iic_b(0U)->BIE);
  TEST_ASSERT_EQ(0, (int32_t)ra_iic_b(0U)->NTIE);
  TEST_ASSERT_EQ((int32_t)(uint32_t)k_ra_iic_b_msk_cndctl_spcnd, (int32_t)ra_iic_b(0U)->CNDCTL);
  /* NACK / AL flags should be W0C-cleared. */
  TEST_ASSERT((ra_iic_b(0U)->BST & (uint32_t)k_ra_iic_b_msk_bst_nackdf) == 0U);
  TEST_ASSERT((ra_iic_b(0U)->BST & (uint32_t)k_ra_iic_b_msk_bst_alf) == 0U);

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_iic_b_abort((uint8_t)k_ra_iic_b_test_ch_oor));
  TEST_END("ra_iic_b_abort: BIE/NTIE masked, STOP issued, BST cleared");
}

/* =============================================================================
 * Long-buffer breaks via SIGALRM clobber.
 * =============================================================================
 */

static void sigalarm_handler_iic(int sig)
{
  (void)sig;
  volatile r_iic_b_regs_t* reg = ra_iic_b(s_alarm_channel);
  if (reg != nullptr) {
    reg->NTST = 0U;
    reg->BST  = 0U;
  }
}

static void arm_clear_alarm(uint8_t channel)
{
  s_alarm_channel = channel;
  struct sigaction sa;
  sa.sa_handler = sigalarm_handler_iic;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  (void)sigaction(SIGALRM, &sa, nullptr);
  struct itimerval timer;
  timer.it_value.tv_sec     = 0;
  timer.it_value.tv_usec    = (long)k_ra_iic_b_test_alarm_usec;
  timer.it_interval.tv_sec  = 0;
  timer.it_interval.tv_usec = (long)k_ra_iic_b_test_alarm_usec;
  (void)setitimer(ITIMER_REAL, &timer, nullptr);
}

static void disarm_alarm(void)
{
  struct itimerval timer = {};
  (void)setitimer(ITIMER_REAL, &timer, nullptr);
  struct sigaction sa;
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  (void)sigaction(SIGALRM, &sa, nullptr);
}

static void test_write_long_break(void)
{
  TEST_BEGIN("ra_iic_b_write: long buffer breaks on cleared NTST");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));
  prime_ntst(0U);
  arm_clear_alarm(0U);
  const ra_err_t err = ra_iic_b_write(0U,
                                      (uint8_t)k_ra_iic_b_test_target,
                                      s_long_buffer,
                                      (uint32_t)k_ra_iic_b_test_long_len,
                                      false);
  disarm_alarm();
  TEST_ASSERT_EQ((int32_t)k_ra_err_hw_timeout, (int32_t)err);
  TEST_END("ra_iic_b_write: long buffer breaks on cleared NTST");
}

static void test_read_long_break(void)
{
  TEST_BEGIN("ra_iic_b_read: long buffer breaks on cleared NTST");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));
  prime_ntst(0U);
  arm_clear_alarm(0U);
  const ra_err_t err = ra_iic_b_read(0U,
                                     (uint8_t)k_ra_iic_b_test_target,
                                     s_long_buffer,
                                     (uint32_t)k_ra_iic_b_test_long_len,
                                     false);
  disarm_alarm();
  TEST_ASSERT_EQ((int32_t)k_ra_err_hw_timeout, (int32_t)err);
  TEST_END("ra_iic_b_read: long buffer breaks on cleared NTST");
}

/* =============================================================================
 * Scan
 * =============================================================================
 */

static void test_scan_no_response(void)
{
  TEST_BEGIN("ra_iic_b_scan: no BST flag => hw_timeout, acked false");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));
  prime_ntst(0U);
  /* No bus activity in the simulator; the scan times out waiting for
   * either TENDF or NACKDF, which is the correct behaviour for an
   * empty bus. */
  bool acked = true;
  TEST_ASSERT_EQ((int32_t)k_ra_err_hw_timeout,
                 (int32_t)ra_iic_b_scan(0U, (uint8_t)k_ra_iic_b_test_target, &acked));
  TEST_ASSERT(!acked);
  TEST_END("ra_iic_b_scan: no BST flag => hw_timeout, acked false");
}

static void test_scan_bad_args(void)
{
  TEST_BEGIN("ra_iic_b_scan: arg validation");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_iic_b_scan(0U, (uint8_t)k_ra_iic_b_test_target, nullptr));
  bool acked = false;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_iic_b_scan((uint8_t)k_ra_iic_b_test_ch_oor,
                                        (uint8_t)k_ra_iic_b_test_target,
                                        &acked));
  TEST_END("ra_iic_b_scan: arg validation");
}

/* =============================================================================
 * Error mask + handler dispatch
 * =============================================================================
 */

static void test_errors_mask_and_clear(void)
{
  TEST_BEGIN("ra_iic_b_get/clear_errors: AL + NACK + TODF");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));
  volatile r_iic_b_regs_t* reg = ra_iic_b(0U);
  reg->BST = (uint32_t)k_ra_iic_b_msk_bst_alf | (uint32_t)k_ra_iic_b_msk_bst_nackdf |
             (uint32_t)k_ra_iic_b_msk_bst_todf;

  uint8_t mask = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_get_errors(0U, &mask));
  TEST_ASSERT_EQ((int32_t)((uint8_t)k_ra_iic_b_err_arb_lost | (uint8_t)k_ra_iic_b_err_nack |
                           (uint8_t)k_ra_iic_b_err_timeout),
                 (int32_t)mask);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_clear_errors(0U));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_get_errors(0U, &mask));
  TEST_ASSERT_EQ((int32_t)k_ra_iic_b_err_none, (int32_t)mask);

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_iic_b_get_errors(0U, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_iic_b_get_errors((uint8_t)k_ra_iic_b_test_ch_oor, &mask));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_iic_b_clear_errors((uint8_t)k_ra_iic_b_test_ch_oor));
  TEST_END("ra_iic_b_get/clear_errors: AL + NACK + TODF");
}

static int32_t s_iic_b_cb_count = 0;
static int32_t s_iic_b_cb_err   = 0;
static void    stub_iic_b_cb(void* ctx, uint8_t err_mask)
{
  (void)ctx;
  ++s_iic_b_cb_count;
  s_iic_b_cb_err = (int32_t)err_mask;
}

static void test_attach_handler_toggles_iers(void)
{
  TEST_BEGIN("ra_iic_b_attach_handler: BIE+NTIE toggled");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_attach_handler(0U, stub_iic_b_cb, nullptr));
  TEST_ASSERT(ra_iic_b(0U)->BIE != 0U);
  TEST_ASSERT(ra_iic_b(0U)->NTIE != 0U);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_attach_handler(0U, nullptr, nullptr));
  TEST_ASSERT_EQ(0, (int32_t)ra_iic_b(0U)->BIE);
  TEST_ASSERT_EQ(0, (int32_t)ra_iic_b(0U)->NTIE);

  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_iic_b_attach_handler((uint8_t)k_ra_iic_b_test_ch_oor, stub_iic_b_cb, nullptr));
  TEST_END("ra_iic_b_attach_handler: BIE+NTIE toggled");
}

static void test_dispatch_eri_fires_callback(void)
{
  TEST_BEGIN("ra_iic_b_dispatch_eri: latched NACKDF -> callback fires");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_init(0U, &k_iic_b_cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_b_attach_handler(0U, stub_iic_b_cb, nullptr));

  s_iic_b_cb_count  = 0;
  s_iic_b_cb_err    = 0;
  ra_iic_b(0U)->BST = (uint32_t)k_ra_iic_b_msk_bst_nackdf;
  ra_iic_b_dispatch_eri(0U);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_iic_b_cb_count);
  TEST_ASSERT_EQ((int32_t)(uint8_t)k_ra_iic_b_err_nack, (int32_t)s_iic_b_cb_err);

  /* Zero mask must not fire the callback. */
  s_iic_b_cb_count  = 0;
  ra_iic_b(0U)->BST = 0U;
  ra_iic_b_dispatch_eri(0U);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)s_iic_b_cb_count);

  /* Out-of-range channel is a no-op. */
  ra_iic_b_dispatch_eri((uint8_t)k_ra_iic_b_test_ch_oor);
  TEST_END("ra_iic_b_dispatch_eri: latched NACKDF -> callback fires");
}

/* =============================================================================
 * Legacy NSC pass-throughs
 * =============================================================================
 */

static void test_legacy_pass_throughs(void)
{
  TEST_BEGIN("ra_iic_init/write/read: NSC pass-throughs forward");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_init(0U, &k_iic_b_cfg));
  prime_ntst(0U);
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_iic_write(0U, (uint8_t)k_ra_iic_b_test_target, s_payload, 2U));
  prime_ntst(0U);
  uint8_t buf = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_iic_read(0U, (uint8_t)k_ra_iic_b_test_target, &buf, 1U));
  TEST_END("ra_iic_init/write/read: NSC pass-throughs forward");
}

int32_t main(void)
{
  test_init_configured();
  test_init_bad_inputs();
  test_deinit_releases();
  test_set_clock_updates();
  test_write_happy();
  test_write_zero_length();
  test_write_null_data();
  test_write_bad_channel();
  test_write_timeout_on_start();
  test_write_busy_rejection();
  test_write_nack_returns_nack_and_stops();
  test_write_restart_holds_bus();
  test_read_happy();
  test_read_zero_length_rejected();
  test_read_null_out();
  test_read_bad_channel();
  test_read_timeout_on_start();
  test_read_busy_rejection();
  test_transfer_happy();
  test_transfer_null_args_rejected();
  test_transfer_busy_rejection();
  test_abort_resets_channel();
  test_write_long_break();
  test_read_long_break();
  test_scan_no_response();
  test_scan_bad_args();
  test_errors_mask_and_clear();
  test_attach_handler_toggles_iers();
  test_dispatch_eri_fires_callback();
  test_legacy_pass_throughs();
  (void)fprintf(stderr, "[OK ] test_ra_iic_b.c\n");
  return 0;
}
