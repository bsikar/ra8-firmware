/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/can_classic_loopback/main.c
 * @brief CAN 2.0B (classic, non-FD) internal-loopback HIL test
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Companion to ``canfd_loopback``, but exercises CAN 2.0B framing
 * only: nominal bit rate 250 kbps, ``data_bitrate_bps = 0`` so the
 * driver leaves DBR untouched and the controller stays in classic
 * (non-FD) mode. Each transmitted frame has ``is_fd = 0`` and an
 * 8-byte payload. Internal loopback is enabled via the same raw
 * CFDC[0].CTR write the FD demo uses (the public HAL does not yet
 * expose a test-mode setter -- see canfd_loopback for the citation
 * to HUM Ch 41 "CFDCnCTR" p 2762).
 *
 * Bare EK-RA8D2 only -- no transceiver required.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_canfd.h"
#include "ra8_canfd_regs.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_can_demo_period_ms = 1000U,   /**< CAN demo period ms. */
  k_can_demo_bitrate   = 250000U, /**< CAN demo bitrate.   */
  k_can_demo_id        = 0x456U,  /**< CAN demo ID.        */
} can_demo_const_t;

/** @brief Channel + payload layout. */
typedef enum : uint8_t {
  k_can_demo_channel = 0U, /**< CAN demo channel. */
  k_can_demo_dlc     = 8U, /**< CAN demo dlc.     */
} can_demo_layout_t;

/** @brief Constant payload bytes for the heartbeat frame. */
typedef enum : uint8_t {
  k_can_demo_byte_marker_a = 0xC1U, /**< CAN demo byte marker a. */
  k_can_demo_byte_marker_b = 0xA5U, /**< CAN demo byte marker b. */
  k_can_demo_byte_marker_c = 0x5CU, /**< CAN demo byte marker c. */
  k_can_demo_byte_marker_d = 0x10U, /**< CAN demo byte marker d. */
  k_can_demo_byte_marker_e = 0x20U, /**< CAN demo byte marker e. */
  k_can_demo_byte_marker_f = 0x30U, /**< CAN demo byte marker f. */
  k_can_demo_byte_marker_g = 0x40U, /**< CAN demo byte marker g. */
} can_demo_byte_t;

/* CFDC[0].CTR test-mode bits live in ra8_canfd_set_test_mode() now.
 * Bit positions (CTME = bit 24, CTMS = bits [26:25]) and the
 * Self-test 1 / internal-loopback selector (CTMS = 11b) come from
 * HUM Ch 41 "CFDCnCTR" p 2710. */

/**
 * @var g_can_match
 * @brief HIL liveness counter -- incremented on every successful
 *        TX -> internal loopback -> RX round-trip.
 *
 * @details
 * Read externally by scripts/hil/jlink_memprobe.sh via SWD. The probe
 * asserts this counter advances by >= HIL_PROBE_MIN_ADVANCE over the
 * sample window, proving the CAN_FD peripheral actually moved frames
 * through its internal loopback (the alive-mode check could only
 * prove the chip didn't crash, not that CAN actually worked).
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_can_match = 0U;

/**
 * @var g_can_mismatch
 * @brief HIL failure counter -- incremented every time TX or RX
 *        returned a non-ok status.
 *
 * @details
 * The memprobe asserts this stays at 0 (or below
 * HIL_PROBE_MAX_FAILURE). Catches the silent-failure mode where the
 * peripheral starts up but TX fails or RX times out -- previously
 * invisible because the chip kept iterating the main loop happily.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_can_mismatch = 0U;

/** @brief Park forever after fatal init failure. */
static void can_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Enable Self-test 1 (internal loopback) on @p channel.
 *
 * @par MC/DC:
 * Decision: ``reg == nullptr``. One atomic condition x 2 vectors --
 * valid channel here, bad-channel covered in
 * test_app_can_classic_loopback.
 *
 * @retval k_ra8_ok                Bits stamped, channel back in operation.
 * @retval k_ra8_err_invalid_arg   Channel index rejected by the HAL.
 *
 * @pre  ra8_canfd_init(channel) returned k_ra8_ok.
 * @pre  No TX/RX is in flight on @p channel.
 * @post CFDC[channel].CTR has CTME=1, CTMS=11b.
 * @post Channel is back in CH_OPERATION ready to TX.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t can_demo_enable_internal_loopback(uint8_t channel)
{
  return ra8_canfd_set_test_mode(channel, k_ra8_ctms_self_test_1);
}

/**
 * @brief Bring CGC + SysTick + LEDs + CANFD0 (classic mode) up.
 *
 * @details
 * Calls ``ra8_canfd_set_bitrate(channel, nominal, 0)`` -- the
 * trailing 0 means "no separate data-phase bit rate", which is the
 * documented way to keep the controller in classic CAN 2.0B mode
 * (DBR is left at reset).
 *
 * @pre Reset_Handler set up the C runtime.
 * @post CANFD0 is in operation mode with internal loopback on.
 *
 * @since 0.1.0
 */
static void can_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    can_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    can_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    can_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    can_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    can_demo_panic_halt();
  }
  if (ra8_canfd_init((uint8_t)k_can_demo_channel) != k_ra8_ok) {
    can_demo_panic_halt();
  }
  /* Classic CAN: data_bitrate_bps == 0. */
  if (ra8_canfd_set_bitrate((uint8_t)k_can_demo_channel, (uint32_t)k_can_demo_bitrate, 0U) !=
      k_ra8_ok) {
    can_demo_panic_halt();
  }
  if (can_demo_enable_internal_loopback((uint8_t)k_can_demo_channel) != k_ra8_ok) {
    can_demo_panic_halt();
  }
}

/**
 * @brief One classic-CAN TX/RX round-trip.
 *
 * @par MC/DC:
 * Compound decision: ``transmit != ok || receive != ok``. Two atomic
 * conditions x N+1 = 3 vectors -- success path (steady state),
 * tx-fail (test mock), rx-empty / rx-fail (test mock).
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t can_demo_one_round_trip(uint8_t seq)
{
  ra8_canfd_frame_t tx = {
    .id          = (uint32_t)k_can_demo_id,
    .dlc         = (uint8_t)k_can_demo_dlc,
    .is_extended = 0U,
    .is_fd       = 0U, /* classic */
    .is_brs      = 0U,
    .data        = {seq,
                    (uint8_t)k_can_demo_byte_marker_a,
                    (uint8_t)k_can_demo_byte_marker_b,
                    (uint8_t)k_can_demo_byte_marker_c,
                    (uint8_t)k_can_demo_byte_marker_d,
                    (uint8_t)k_can_demo_byte_marker_e,
                    (uint8_t)k_can_demo_byte_marker_f,
                    (uint8_t)k_can_demo_byte_marker_g},
  };
  if (ra8_canfd_transmit((uint8_t)k_can_demo_channel, &tx) != k_ra8_ok) {
    return k_ra8_err_hw_error;
  }
  ra8_canfd_frame_t rx = {};
  if (ra8_canfd_receive((uint8_t)k_can_demo_channel, &rx) != k_ra8_ok) {
    return k_ra8_err_no_data;
  }
  return k_ra8_ok;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  can_demo_setup_or_halt();
  ra8_isr_globals_enable();

  uint8_t seq = 0U;
  while (1) {
    if (can_demo_one_round_trip(seq) == k_ra8_ok) {
      (void)ra8_board_led_toggle(k_ra8_board_led1);
      g_can_match += 1U;
    } else {
      (void)ra8_board_led_toggle(k_ra8_board_led2);
      g_can_mismatch += 1U;
    }
    seq++;
    ra8_delay_ms((uint32_t)k_can_demo_period_ms);
  }
  can_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
