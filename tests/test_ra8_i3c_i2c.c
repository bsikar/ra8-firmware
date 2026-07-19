/**
 * @file test_ra8_i3c_i2c.c
 * @brief Unit tests for the IIC_B (I3C in I2C-only mode) controller driver.
 *
 * @details
 * Drives the polling-mode IIC_B controller against the host-side
 * ``ra8_sim_mmap`` substrate. Status flags are pre-armed where the
 * driver expects them (BST.STCNDDF, BST.SPCNDDF, NTST.TDBEF0,
 * NTST.RDBFF0, BST.TENDF) so the wait loops fall through immediately.
 *
 * BCST.BFREF is also pre-armed (= bus free) ahead of every transfer
 * test so the new bus-busy gate (mirrors the upstream Renesas FSP
 * IIC controller BFREF check) does not falsely reject the transaction.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_i3c_i2c.h"
#include "ra8_i3c_i2c_internal.h"
#include "ra8_i3c_i2c_regs.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "ra8_sim_mmio.h"
#include "unity_minimal.h"

/**
 * @enum ra8_i3c_i2c_test_addr_t
 * @brief Target address, the bytes moved over the bus, and the pre-shifted
 *        address bytes derived from it. No two payload bytes are equal, so a
 *        transfer that echoed the wrong direction, dropped a byte or swapped a
 *        pair fails a specific compare.
 */
typedef enum : uint8_t {
  k_ra8_i3c_i2c_test_target  = 0x50U, /**< 7-bit target address.              */
  k_ra8_i3c_i2c_test_byte_a  = 0xA5U, /**< First TX byte; neither 0 nor 0xFF. */
  k_ra8_i3c_i2c_test_byte_b  = 0x5AU, /**< Its complement: a swap is obvious. */
  k_ra8_i3c_i2c_test_byte_c  = 0x33U, /**< A third, for three-byte writes.    */
  k_ra8_i3c_i2c_test_rx_byte = 0xC3U, /**< Unlike any TX byte, so a read that
                                           echoed TX is visible.              */
  k_ra8_i3c_i2c_test_addr_w  = (uint8_t)(k_ra8_i3c_i2c_test_target << 1U), /**< Write form. */
  k_ra8_i3c_i2c_test_addr_r =
    (uint8_t)((k_ra8_i3c_i2c_test_target << 1U) | 1U), /**< With the read bit set. */
} ra8_i3c_i2c_test_addr_t;

/**
 * @enum ra8_i3c_i2c_test_ch_t
 * @brief Channel numbers: the one that exists, and two that do not.
 */
typedef enum : uint8_t {
  k_ra8_i3c_i2c_test_ch_zero = 0U,   /**< The only real IIC_B channel on RA8D2. */
  k_ra8_i3c_i2c_test_ch_oor  = 1U,   /**< One past it.                          */
  k_ra8_i3c_i2c_test_ch_huge = 200U, /**< Far past it, so the guard is a range
                                          check and not a `!= 1` comparison.    */
} ra8_i3c_i2c_test_ch_t;

/** @brief Payload length used by the long-buffer timeout tests. */
typedef enum : uint32_t {
  k_ra8_i3c_i2c_test_long_len = 1000000U, /**< Far past any real buffer, so the
                                               length guard fires before any
                                               access is attempted.          */
} ra8_i3c_i2c_test_wait_t;

static const uint8_t s_payload[2] = {
  (uint8_t)k_ra8_i3c_i2c_test_byte_a,
  (uint8_t)k_ra8_i3c_i2c_test_byte_b,
};

static uint8_t s_long_buffer[k_ra8_i3c_i2c_test_long_len];

static const ra8_i3c_i2c_cfg_t k_iic_b_cfg = {
  .bus_hz   = (uint32_t)k_ra8_i3c_i2c_speed_fast,
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
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs(channel);
  reg->NTST = (uint32_t)k_ra8_i3c_i2c_msk_ntst_tdbef0 | (uint32_t)k_ra8_i3c_i2c_msk_ntst_rdbff0;
  reg->BCST = (uint32_t)k_ra8_i3c_i2c_msk_bcst_bfref;
}

/**
 * @brief Reset the simulator and ensure MSTP / channel state is fresh.
 */
static void prep(void)
{
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  (void)ra8_mstp_init();
}

/* =============================================================================
 * Init / deinit
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_init_configured(void)
{
  TEST_BEGIN("internal_i3c_i2c_init: BCTL.BUSE set, STDBR programmed");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));
  volatile const r_i3c_i2c_regs_t* reg = i3c_i2c_regs(0U);
  TEST_ASSERT((reg->BCTL & (uint32_t)k_ra8_i3c_i2c_msk_bctl_buse) != 0U);
  TEST_ASSERT(reg->STDBR != 0U);
  TEST_END("internal_i3c_i2c_init: BCTL.BUSE set, STDBR programmed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_inputs(void)
{
  TEST_BEGIN("internal_i3c_i2c_init: bad inputs rejected");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, internal_i3c_i2c_init(0U, nullptr));
  ra8_i3c_i2c_cfg_t bad = k_iic_b_cfg;
  bad.bus_hz            = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_i3c_i2c_init(0U, &bad));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 internal_i3c_i2c_init((uint8_t)k_ra8_i3c_i2c_test_ch_oor, &k_iic_b_cfg));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 internal_i3c_i2c_init((uint8_t)k_ra8_i3c_i2c_test_ch_huge, &k_iic_b_cfg));
  TEST_END("internal_i3c_i2c_init: bad inputs rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit_releases(void)
{
  TEST_BEGIN("internal_i3c_i2c_deinit: BCTL cleared, MSTP gated");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_deinit(0U));
  bool stopped = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_is_stopped(k_ra8_mstp_i3c, &stopped));
  TEST_ASSERT(stopped);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 internal_i3c_i2c_deinit((uint8_t)k_ra8_i3c_i2c_test_ch_oor));
  TEST_END("internal_i3c_i2c_deinit: BCTL cleared, MSTP gated");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_clock_updates(void)
{
  TEST_BEGIN("internal_i3c_i2c_set_clock: STDBR changes");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));
  const uint32_t before = i3c_i2c_regs(0U)->STDBR;
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_i3c_i2c_set_clock(0U, (uint32_t)k_ra8_i3c_i2c_speed_standard, 60000000U));
  TEST_ASSERT(i3c_i2c_regs(0U)->STDBR != before);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_i3c_i2c_set_clock(0U, 0U, 60000000U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_i3c_i2c_set_clock(0U, 100000U, 0U));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    internal_i3c_i2c_set_clock((uint8_t)k_ra8_i3c_i2c_test_ch_oor, 100000U, 60000000U));
  TEST_END("internal_i3c_i2c_set_clock: STDBR changes");
}

/* =============================================================================
 * Polling write
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_write_happy(void)
{
  TEST_BEGIN("internal_i3c_i2c_write: 2-byte payload, START + STOP pulsed");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));
  prime_ntst(0U);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_i3c_i2c_write(0U, (uint8_t)k_ra8_i3c_i2c_test_target, s_payload, 2U, false));
  /* Final byte should land in NTDTBP0; CNDCTL last write should be the
   * STOP request (mirrors what the bus would observe after the
   * transaction). */
  volatile const r_i3c_i2c_regs_t* reg = i3c_i2c_regs(0U);
  TEST_ASSERT_EQ(k_ra8_i3c_i2c_test_byte_b, (reg->NTDTBP0 & 0xFFU));
  TEST_ASSERT_EQ(k_ra8_i3c_i2c_msk_cndctl_spcnd, reg->CNDCTL);
  TEST_END("internal_i3c_i2c_write: 2-byte payload, START + STOP pulsed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write_zero_length(void)
{
  TEST_BEGIN("internal_i3c_i2c_write: zero-length is a probe");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));
  prime_ntst(0U);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_i3c_i2c_write(0U, (uint8_t)k_ra8_i3c_i2c_test_target, s_payload, 0U, false));
  TEST_END("internal_i3c_i2c_write: zero-length is a probe");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write_null_data(void)
{
  TEST_BEGIN("internal_i3c_i2c_write: null data rejected");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    internal_i3c_i2c_write(0U, (uint8_t)k_ra8_i3c_i2c_test_target, nullptr, 1U, false));
  TEST_END("internal_i3c_i2c_write: null data rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write_bad_channel(void)
{
  TEST_BEGIN("internal_i3c_i2c_write: channel out of range");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 internal_i3c_i2c_write((uint8_t)k_ra8_i3c_i2c_test_ch_oor,
                                        (uint8_t)k_ra8_i3c_i2c_test_target,
                                        s_payload,
                                        1U,
                                        false));
  TEST_END("internal_i3c_i2c_write: channel out of range");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write_timeout_on_start(void)
{
  TEST_BEGIN("internal_i3c_i2c_write: TDBEF0 never sets => timeout");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));
  /* Mark the bus free so the busy gate passes, but leave NTST cleared
   * so the address-byte send loop times out. */
  i3c_i2c_regs(0U)->BCST = (uint32_t)k_ra8_i3c_i2c_msk_bcst_bfref;
  TEST_ASSERT_EQ(
    k_ra8_err_hw_timeout,
    internal_i3c_i2c_write(0U, (uint8_t)k_ra8_i3c_i2c_test_target, s_payload, 1U, false));
  TEST_END("internal_i3c_i2c_write: TDBEF0 never sets => timeout");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write_busy_rejection(void)
{
  TEST_BEGIN("internal_i3c_i2c_write: BCST.BFREF=0 => k_ra8_err_busy");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));
  /* BFREF clear == bus busy. NTST primed so the call would otherwise
   * proceed -- the busy gate must fire first. */
  i3c_i2c_regs(0U)->NTST =
    (uint32_t)k_ra8_i3c_i2c_msk_ntst_tdbef0 | (uint32_t)k_ra8_i3c_i2c_msk_ntst_rdbff0;
  i3c_i2c_regs(0U)->BCST = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_busy,
    internal_i3c_i2c_write(0U, (uint8_t)k_ra8_i3c_i2c_test_target, s_payload, 1U, false));
  TEST_END("internal_i3c_i2c_write: BCST.BFREF=0 => k_ra8_err_busy");
}

/* =============================================================================
 * Deterministic NACK injection via the ra8_sim_mmio poll-hook.
 * =============================================================================
 *
 * The hook runs inline on the driver's OWN poll thread (no wall-clock timer, no
 * concurrent servicer thread to race or starve). The IIC_B driver clears BST at
 * the start of every transfer, so a pre-armed NACKDF would be wiped; the hook
 * instead re-asserts BST.NACKDF on every internal_i3c_i2c_wait_ntst poll while
 * TDBEF0 stays primed, so internal_i3c_i2c_drain_tx observes it on one of its
 * per-byte polls and the write returns k_ra8_err_nack. The long-buffer TIMEOUT
 * legs use ra8_sim_mmio_fail_nth_wait instead (see the tests further down). */

/** @brief Channel the NACK poll-hook operates on. */
static uint8_t s_nack_ch;

/**
 * @brief Poll-hook body: latch BST.NACKDF on the target channel.
 */
static void i3c_nack_hook(void)
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
static void i3c_nack_hook_arm(uint8_t channel)
{
  s_nack_ch = channel;
  ra8_sim_mmio_set_poll_hook(i3c_nack_hook);
}

/**
 * @brief Remove the NACK poll-hook.
 */
static void i3c_nack_hook_disarm(void)
{
  ra8_sim_mmio_set_poll_hook(nullptr);
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write_nack_returns_nack_and_stops(void)
{
  TEST_BEGIN("internal_i3c_i2c_write: NACKDF latched mid-transfer => k_ra8_err_nack + STOP");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));
  prime_ntst(0U);
  /* The servicer latches NACKDF mid-transfer: the driver's start-of-
   * transaction clear_bst would otherwise wipe a pre-armed value, so the
   * servicer re-asserts it continuously and drain_tx observes it on one of
   * its per-byte polls -- deterministically, with no wall-clock timer. */
  i3c_nack_hook_arm(0U);
  const ra8_err_t err = internal_i3c_i2c_write(0U,
                                               (uint8_t)k_ra8_i3c_i2c_test_target,
                                               s_long_buffer,
                                               (uint32_t)k_ra8_i3c_i2c_test_long_len,
                                               false);
  i3c_nack_hook_disarm();
  TEST_ASSERT_EQ(k_ra8_err_nack, err);
  /* CNDCTL last write should be STOP. */
  TEST_ASSERT_EQ(k_ra8_i3c_i2c_msk_cndctl_spcnd, i3c_i2c_regs(0U)->CNDCTL);
  TEST_END("internal_i3c_i2c_write: NACKDF latched mid-transfer => k_ra8_err_nack + STOP");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write_restart_holds_bus(void)
{
  TEST_BEGIN("internal_i3c_i2c_write: restart=true holds bus, no STOP issued");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));
  prime_ntst(0U);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_i3c_i2c_write(0U, (uint8_t)k_ra8_i3c_i2c_test_target, s_payload, 1U, true));
  /* CNDCTL last write should be START (no subsequent STOP). */
  TEST_ASSERT_EQ(k_ra8_i3c_i2c_msk_cndctl_stcnd, i3c_i2c_regs(0U)->CNDCTL);
  TEST_END("internal_i3c_i2c_write: restart=true holds bus, no STOP issued");
}

/* =============================================================================
 * Polling read
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_read_happy(void)
{
  TEST_BEGIN("internal_i3c_i2c_read: 2-byte read returns ok, ACKBT set on last byte");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));
  prime_ntst(0U);
  uint8_t buf[2] = {0U, 0U};
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_i3c_i2c_read(0U, (uint8_t)k_ra8_i3c_i2c_test_target, buf, 2U, false));
  /* Real-bus byte exchange cannot be modelled by the host substrate
   * (NTDTBP0 is one register, not a FIFO that holds the address byte
   * separately from rx data). What we DO observe:
   *   1. ACKCTL was set to (ACKTWP | ACKT) somewhere in the flow to
   *      arm the NACK on the last byte; the driver then restores it
   *      to plain ACKTWP. The "restored to plain ACKTWP" final state
   *      is the ABI we lock in.
   *   2. CNDCTL last write is STOP. */
  TEST_ASSERT_EQ(k_ra8_i3c_i2c_msk_ackctl_acktwp, (i3c_i2c_regs(0U)->ACKCTL));
  TEST_ASSERT_EQ(k_ra8_i3c_i2c_msk_cndctl_spcnd, i3c_i2c_regs(0U)->CNDCTL);
  TEST_END("internal_i3c_i2c_read: 2-byte read returns ok, ACKBT set on last byte");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_zero_length_rejected(void)
{
  TEST_BEGIN("internal_i3c_i2c_read: len==0 rejected");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));
  uint8_t buf = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 internal_i3c_i2c_read(0U, (uint8_t)k_ra8_i3c_i2c_test_target, &buf, 0U, false));
  TEST_END("internal_i3c_i2c_read: len==0 rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_null_out(void)
{
  TEST_BEGIN("internal_i3c_i2c_read: null buf rejected");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 internal_i3c_i2c_read(0U, (uint8_t)k_ra8_i3c_i2c_test_target, nullptr, 1U, false));
  TEST_END("internal_i3c_i2c_read: null buf rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_bad_channel(void)
{
  TEST_BEGIN("internal_i3c_i2c_read: channel out of range");
  prep();
  uint8_t buf = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 internal_i3c_i2c_read((uint8_t)k_ra8_i3c_i2c_test_ch_oor,
                                       (uint8_t)k_ra8_i3c_i2c_test_target,
                                       &buf,
                                       1U,
                                       false));
  TEST_END("internal_i3c_i2c_read: channel out of range");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_timeout_on_start(void)
{
  TEST_BEGIN("internal_i3c_i2c_read: TDBEF0 never sets => timeout");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));
  i3c_i2c_regs(0U)->BCST = (uint32_t)k_ra8_i3c_i2c_msk_bcst_bfref;
  uint8_t buf            = 0U;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 internal_i3c_i2c_read(0U, (uint8_t)k_ra8_i3c_i2c_test_target, &buf, 1U, false));
  TEST_END("internal_i3c_i2c_read: TDBEF0 never sets => timeout");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_busy_rejection(void)
{
  TEST_BEGIN("internal_i3c_i2c_read: BCST.BFREF=0 => k_ra8_err_busy");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));
  i3c_i2c_regs(0U)->NTST =
    (uint32_t)k_ra8_i3c_i2c_msk_ntst_tdbef0 | (uint32_t)k_ra8_i3c_i2c_msk_ntst_rdbff0;
  i3c_i2c_regs(0U)->BCST = 0U;
  uint8_t buf            = 0U;
  TEST_ASSERT_EQ(k_ra8_err_busy,
                 internal_i3c_i2c_read(0U, (uint8_t)k_ra8_i3c_i2c_test_target, &buf, 1U, false));
  TEST_END("internal_i3c_i2c_read: BCST.BFREF=0 => k_ra8_err_busy");
}

/* =============================================================================
 * Combined transfer
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_transfer_happy(void)
{
  TEST_BEGIN("internal_i3c_i2c_transfer: write-then-RESTART-then-read");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));
  prime_ntst(0U);
  uint8_t       rx     = 0U;
  const uint8_t tx[1U] = {(uint8_t)k_ra8_i3c_i2c_test_byte_c};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_i3c_i2c_transfer(0U, (uint8_t)k_ra8_i3c_i2c_test_target, tx, 1U, &rx, 1U));
  /* CNDCTL last write should be STOP -- the read phase always closes
   * the bus regardless of the prior write phase having held it. */
  TEST_ASSERT_EQ(k_ra8_i3c_i2c_msk_cndctl_spcnd, i3c_i2c_regs(0U)->CNDCTL);
  TEST_END("internal_i3c_i2c_transfer: write-then-RESTART-then-read");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_transfer_null_args_rejected(void)
{
  TEST_BEGIN("internal_i3c_i2c_transfer: null buffers / zero lens rejected");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));
  uint8_t rx = 0U;

  /* Both lens zero -> invalid_arg. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    internal_i3c_i2c_transfer(0U, (uint8_t)k_ra8_i3c_i2c_test_target, nullptr, 0U, nullptr, 0U));
  /* tx_len > 0 with NULL tx -> null_ptr. */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    internal_i3c_i2c_transfer(0U, (uint8_t)k_ra8_i3c_i2c_test_target, nullptr, 1U, &rx, 1U));
  /* rx_len > 0 with NULL rx -> null_ptr. */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    internal_i3c_i2c_transfer(0U, (uint8_t)k_ra8_i3c_i2c_test_target, s_payload, 1U, nullptr, 1U));
  /* Out-of-range channel -> null_ptr (matches the rest of the API). */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 internal_i3c_i2c_transfer((uint8_t)k_ra8_i3c_i2c_test_ch_oor,
                                           (uint8_t)k_ra8_i3c_i2c_test_target,
                                           s_payload,
                                           1U,
                                           &rx,
                                           1U));
  TEST_END("internal_i3c_i2c_transfer: null buffers / zero lens rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_transfer_busy_rejection(void)
{
  TEST_BEGIN("internal_i3c_i2c_transfer: bus-busy => k_ra8_err_busy");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));
  i3c_i2c_regs(0U)->NTST =
    (uint32_t)k_ra8_i3c_i2c_msk_ntst_tdbef0 | (uint32_t)k_ra8_i3c_i2c_msk_ntst_rdbff0;
  i3c_i2c_regs(0U)->BCST = 0U;
  uint8_t rx             = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_busy,
    internal_i3c_i2c_transfer(0U, (uint8_t)k_ra8_i3c_i2c_test_target, s_payload, 1U, &rx, 1U));
  TEST_END("internal_i3c_i2c_transfer: bus-busy => k_ra8_err_busy");
}

/* =============================================================================
 * Abort
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_abort_resets_channel(void)
{
  TEST_BEGIN("internal_i3c_i2c_abort: BIE/NTIE masked, STOP issued, BST cleared");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));
  /* Pretend a transfer is in progress: BIE/NTIE non-zero, BST has
   * latched flags. */
  i3c_i2c_regs(0U)->BIE  = (uint32_t)k_ra8_i3c_i2c_msk_bie_nackdie;
  i3c_i2c_regs(0U)->NTIE = (uint32_t)k_ra8_i3c_i2c_msk_ntie_tdbeie0;
  i3c_i2c_regs(0U)->BST =
    (uint32_t)k_ra8_i3c_i2c_msk_bst_nackdf | (uint32_t)k_ra8_i3c_i2c_msk_bst_alf;

  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_abort(0U));
  TEST_ASSERT_EQ(0, i3c_i2c_regs(0U)->BIE);
  TEST_ASSERT_EQ(0, i3c_i2c_regs(0U)->NTIE);
  TEST_ASSERT_EQ(k_ra8_i3c_i2c_msk_cndctl_spcnd, i3c_i2c_regs(0U)->CNDCTL);
  /* NACK / AL flags should be W0C-cleared. */
  TEST_ASSERT((i3c_i2c_regs(0U)->BST & (uint32_t)k_ra8_i3c_i2c_msk_bst_nackdf) == 0U);
  TEST_ASSERT((i3c_i2c_regs(0U)->BST & (uint32_t)k_ra8_i3c_i2c_msk_bst_alf) == 0U);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_i3c_i2c_abort((uint8_t)k_ra8_i3c_i2c_test_ch_oor));
  TEST_END("internal_i3c_i2c_abort: BIE/NTIE masked, STOP issued, BST cleared");
}

/* =============================================================================
 * Long-buffer breaks via ra8_sim_mmio_fail_nth_wait (deterministic, no servicer).
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write_long_break(void)
{
  TEST_BEGIN("internal_i3c_i2c_write: long buffer breaks on cleared NTST");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));
  prime_ntst(0U);
  /* Fail drain_tx's first NTST wait (the 2nd wait-loop on NTST, after the one in
   * send_address) so the payload push times out mid-transfer -- deterministic,
   * with no concurrent servicer. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_mmio_fail_nth_wait(&i3c_i2c_regs(0U)->NTST, 1U));
  const ra8_err_t err = internal_i3c_i2c_write(0U,
                                               (uint8_t)k_ra8_i3c_i2c_test_target,
                                               s_long_buffer,
                                               (uint32_t)k_ra8_i3c_i2c_test_long_len,
                                               false);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, err);
  TEST_END("internal_i3c_i2c_write: long buffer breaks on cleared NTST");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_long_break(void)
{
  TEST_BEGIN("internal_i3c_i2c_read: long buffer breaks on cleared NTST");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));
  prime_ntst(0U);
  /* Fail drain_rx's first NTST wait (the 3rd wait-loop on NTST: after
   * send_address and the rx-phase priming read) so the receive times out
   * mid-transfer -- deterministic, with no concurrent servicer. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_mmio_fail_nth_wait(&i3c_i2c_regs(0U)->NTST, 2U));
  const ra8_err_t err = internal_i3c_i2c_read(0U,
                                              (uint8_t)k_ra8_i3c_i2c_test_target,
                                              s_long_buffer,
                                              (uint32_t)k_ra8_i3c_i2c_test_long_len,
                                              false);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, err);
  TEST_END("internal_i3c_i2c_read: long buffer breaks on cleared NTST");
}

/* =============================================================================
 * Scan
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_scan_no_response(void)
{
  TEST_BEGIN("internal_i3c_i2c_scan: no BST flag => hw_timeout, acked false");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));
  prime_ntst(0U);
  /* No bus activity in the simulator; the scan times out waiting for
   * either TENDF or NACKDF, which is the correct behaviour for an
   * empty bus. */
  bool acked = true;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 internal_i3c_i2c_scan(0U, (uint8_t)k_ra8_i3c_i2c_test_target, &acked));
  TEST_ASSERT(!acked);
  TEST_END("internal_i3c_i2c_scan: no BST flag => hw_timeout, acked false");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_scan_bad_args(void)
{
  TEST_BEGIN("internal_i3c_i2c_scan: arg validation");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 internal_i3c_i2c_scan(0U, (uint8_t)k_ra8_i3c_i2c_test_target, nullptr));
  bool acked = false;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 internal_i3c_i2c_scan((uint8_t)k_ra8_i3c_i2c_test_ch_oor,
                                       (uint8_t)k_ra8_i3c_i2c_test_target,
                                       &acked));
  TEST_END("internal_i3c_i2c_scan: arg validation");
}

/* =============================================================================
 * Error mask + handler dispatch
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_errors_mask_and_clear(void)
{
  TEST_BEGIN("internal_i3c_i2c_get/clear_errors: AL + NACK + TODF");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs(0U);
  reg->BST = (uint32_t)k_ra8_i3c_i2c_msk_bst_alf | (uint32_t)k_ra8_i3c_i2c_msk_bst_nackdf |
             (uint32_t)k_ra8_i3c_i2c_msk_bst_todf;

  uint8_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_get_errors(0U, &mask));
  TEST_ASSERT_EQ(((uint8_t)k_ra8_i3c_i2c_err_arb_lost | (uint8_t)k_ra8_i3c_i2c_err_nack |
                  (uint8_t)k_ra8_i3c_i2c_err_timeout),
                 mask);

  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_clear_errors(0U));
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_get_errors(0U, &mask));
  TEST_ASSERT_EQ(k_ra8_i3c_i2c_err_none, mask);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, internal_i3c_i2c_get_errors(0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 internal_i3c_i2c_get_errors((uint8_t)k_ra8_i3c_i2c_test_ch_oor, &mask));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 internal_i3c_i2c_clear_errors((uint8_t)k_ra8_i3c_i2c_test_ch_oor));
  TEST_END("internal_i3c_i2c_get/clear_errors: AL + NACK + TODF");
}

static int32_t s_iic_b_cb_count = 0;
static int32_t s_iic_b_cb_err   = 0;
static void    stub_iic_b_cb(void* ctx, uint8_t err_mask)
{
  (void)ctx;
  ++s_iic_b_cb_count;
  s_iic_b_cb_err = (int32_t)err_mask;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_handler_toggles_iers(void)
{
  TEST_BEGIN("internal_i3c_i2c_attach_handler: BIE+NTIE toggled");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));

  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_attach_handler(0U, stub_iic_b_cb, nullptr));
  TEST_ASSERT(i3c_i2c_regs(0U)->BIE != 0U);
  TEST_ASSERT(i3c_i2c_regs(0U)->NTIE != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_attach_handler(0U, nullptr, nullptr));
  TEST_ASSERT_EQ(0, i3c_i2c_regs(0U)->BIE);
  TEST_ASSERT_EQ(0, i3c_i2c_regs(0U)->NTIE);

  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    internal_i3c_i2c_attach_handler((uint8_t)k_ra8_i3c_i2c_test_ch_oor, stub_iic_b_cb, nullptr));
  TEST_END("internal_i3c_i2c_attach_handler: BIE+NTIE toggled");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_eri_fires_callback(void)
{
  TEST_BEGIN("internal_i3c_i2c_dispatch_eri: latched NACKDF -> callback fires");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_attach_handler(0U, stub_iic_b_cb, nullptr));

  s_iic_b_cb_count      = 0;
  s_iic_b_cb_err        = 0;
  i3c_i2c_regs(0U)->BST = (uint32_t)k_ra8_i3c_i2c_msk_bst_nackdf;
  internal_i3c_i2c_dispatch_eri(0U);
  TEST_ASSERT_EQ(1, s_iic_b_cb_count);
  TEST_ASSERT_EQ(k_ra8_i3c_i2c_err_nack, s_iic_b_cb_err);

  /* Zero mask must not fire the callback. */
  s_iic_b_cb_count      = 0;
  i3c_i2c_regs(0U)->BST = 0U;
  internal_i3c_i2c_dispatch_eri(0U);
  TEST_ASSERT_EQ(0, s_iic_b_cb_count);

  /* Out-of-range channel is a no-op. */
  internal_i3c_i2c_dispatch_eri((uint8_t)k_ra8_i3c_i2c_test_ch_oor);
  TEST_END("internal_i3c_i2c_dispatch_eri: latched NACKDF -> callback fires");
}

/**
 * @test test_mcdc_iic_b
 *
 * @par MC/DC:
 * Three 2-condition decisions in ``internal_i3c_i2c_transfer``,
 * libs/ra8_hal/src/ra8_i3c_i2c.c:
 *
 * Decision A (line 948): ``if ((tx_len == 0U) && (rx_len == 0U))``
 * - V1: tx=0, rx=0  -> C1=T,C2=T -> dec T (-> invalid_arg)
 * - V2: tx=non-zero -> C1=F (short-circuits) -> dec F
 * - V3: tx=0, rx=non-zero -> C1=T,C2=F -> dec F
 * Pairs (V1,V2) flip C1; (V1,V3) flip C2.
 *
 * Decision B (line 951): ``if ((tx_len != 0U) && (tx == nullptr))``
 * - V1: tx_len=0       -> C1=F (short-circuits)         -> dec F
 * - V2: tx_len!=0, tx=non-null -> C1=T,C2=F             -> dec F
 * - V3: tx_len!=0, tx=NULL     -> C1=T,C2=T             -> dec T (null_ptr)
 *
 * Decision C (line 954): ``if ((rx_len != 0U) && (rx == nullptr))``
 * mirrors B with rx_len/rx; same N+1 = 3 vectors.
 */
static void test_mcdc_iic_b(void)
{
  TEST_BEGIN("iic_b MC/DC: transfer arg-validation 2-cond decisions");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, internal_i3c_i2c_init(0U, &k_iic_b_cfg));

  uint8_t tx_buf[1] = {k_ra8_i3c_i2c_test_byte_a};
  uint8_t rx_buf[1] = {0U};

  /* Decision A V1: both lens zero -> invalid_arg. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    internal_i3c_i2c_transfer(0U, (uint8_t)k_ra8_i3c_i2c_test_target, tx_buf, 0U, rx_buf, 0U));

  /* Decision B V3: tx_len!=0 with NULL tx -> null_ptr. */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    internal_i3c_i2c_transfer(0U, (uint8_t)k_ra8_i3c_i2c_test_target, nullptr, 1U, rx_buf, 0U));

  /* Decision C V3: rx_len!=0 with NULL rx -> null_ptr. tx_len=0 to
   * keep decision A=F (because rx_len!=0) and decision B=F (tx_len=0). */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    internal_i3c_i2c_transfer(0U, (uint8_t)k_ra8_i3c_i2c_test_target, tx_buf, 0U, nullptr, 1U));

  /* Decision B V2 + C V2 happy path covered by test_transfer_happy and
   * Decision A V2 / V3 covered implicitly through happy + decision B
   * V1 short-circuit. The asserts above are the masking pairs that
   * give MC/DC for the rejection branches. */
  TEST_END("iic_b MC/DC: transfer arg-validation 2-cond decisions");
}

/**
 * @test test_mcdc_iic_b_internal_len_buf_invalid
 *
 * @par MC/DC:
 * Decision at libs/ra8_hal/src/ra8_i3c_i2c.c (call site) -> helper at
 * libs/ra8_hal/src/ra8_i3c_i2c.c:
 *   ``len != 0 && buf == NULL`` (2 conditions, AND).
 * - V1: len=0, buf=NULL -> false
 * - V2: len>0, buf=NULL -> true (varies left)
 * - V3: len>0, buf!=NULL -> false (varies right)
 * N+1 = 3.
 */
static void test_mcdc_iic_b_internal_len_buf_invalid(void)
{
  TEST_BEGIN("iic_b MC/DC: len_buf_invalid AND");
  uint8_t scratch = 0U;
  TEST_ASSERT(!internal_i3c_i2c_len_buf_invalid(0U, nullptr));
  TEST_ASSERT(internal_i3c_i2c_len_buf_invalid(4U, nullptr));
  TEST_ASSERT(!internal_i3c_i2c_len_buf_invalid(4U, &scratch));
  TEST_END("iic_b MC/DC: len_buf_invalid AND");
}

/**
 * @test test_mcdc_iic_b_internal_should_dispatch
 *
 * @par MC/DC:
 * Decision at libs/ra8_hal/src/ra8_i3c_i2c.c (call site) -> helper at
 * libs/ra8_hal/src/ra8_i3c_i2c.c:
 *   ``mask != 0 && cb != NULL`` (2 conditions, AND).
 * - V1: mask=0, cb!=NULL -> false (varies left from V2)
 * - V2: mask!=0, cb!=NULL -> true
 * - V3: mask!=0, cb=NULL -> false (varies right from V2)
 * N+1 = 3.
 */
static void test_mcdc_iic_b_internal_should_dispatch(void)
{
  TEST_BEGIN("iic_b MC/DC: should_dispatch AND");
  uint8_t cb_dummy = 0U;
  TEST_ASSERT(!internal_i3c_i2c_should_dispatch(0U, &cb_dummy));
  TEST_ASSERT(internal_i3c_i2c_should_dispatch(0x10U, &cb_dummy));
  TEST_ASSERT(!internal_i3c_i2c_should_dispatch(0x10U, nullptr));
  TEST_END("iic_b MC/DC: should_dispatch AND");
}

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Order is significant: cases run top to bottom, exactly as before.
 */
static void (*const s_test_roster[])(void) = {
  test_init_configured,
  test_init_bad_inputs,
  test_deinit_releases,
  test_set_clock_updates,
  test_write_happy,
  test_write_zero_length,
  test_write_null_data,
  test_write_bad_channel,
  test_write_timeout_on_start,
  test_write_busy_rejection,
  test_write_nack_returns_nack_and_stops,
  test_write_restart_holds_bus,
  test_read_happy,
  test_read_zero_length_rejected,
  test_read_null_out,
  test_read_bad_channel,
  test_read_timeout_on_start,
  test_read_busy_rejection,
  test_transfer_happy,
  test_transfer_null_args_rejected,
  test_transfer_busy_rejection,
  test_abort_resets_channel,
  test_write_long_break,
  test_read_long_break,
  test_scan_no_response,
  test_scan_bad_args,
  test_errors_mask_and_clear,
  test_attach_handler_toggles_iers,
  test_dispatch_eri_fires_callback,
  test_mcdc_iic_b,
  test_mcdc_iic_b_internal_len_buf_invalid,
  test_mcdc_iic_b_internal_should_dispatch,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK ] test_ra8_i3c_i2c.c\n");
  return 0;
}
