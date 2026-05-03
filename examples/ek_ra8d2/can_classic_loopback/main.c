/**
 * @file examples/ek_ra8d2/can_classic_loopback/main.c
 * @brief CAN 2.0B (classic, non-FD) internal-loopback smoke test
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
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_canfd_regs.h"
#include "ra_board_ek_ra8d2.h"
#include "ra_canfd.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_can_demo_period_ms = 1000U,
  k_can_demo_bitrate   = 250000U,
  k_can_demo_id        = 0x456U,
} can_demo_const_t;

/** @brief Channel + payload layout. */
typedef enum : uint8_t {
  k_can_demo_channel = 0U,
  k_can_demo_dlc     = 8U,
} can_demo_layout_t;

/** @brief Constant payload bytes for the heartbeat frame. */
typedef enum : uint8_t {
  k_can_demo_byte_marker_a = 0xC1U,
  k_can_demo_byte_marker_b = 0xA5U,
  k_can_demo_byte_marker_c = 0x5CU,
  k_can_demo_byte_marker_d = 0x10U,
  k_can_demo_byte_marker_e = 0x20U,
  k_can_demo_byte_marker_f = 0x30U,
  k_can_demo_byte_marker_g = 0x40U,
} can_demo_byte_t;

/** @brief CFDC[0].CTR test-mode bits (HUM Ch 41 "CFDCnCTR" p 2762). */
typedef enum : uint32_t {
  k_can_demo_ctme_bit   = 17U,
  k_can_demo_ctms_shift = 18U,
  k_can_demo_ctms_intl  = 0x1UL,
} can_demo_ctr_t;

/** @brief Park forever after fatal init failure. */
static void can_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Stamp internal-loopback into CFDC[0].CTR. See canfd_loopback.
 *
 * @par MC/DC:
 * Decision: ``reg == nullptr``. One atomic condition x 2 vectors --
 * valid channel here, bad-channel covered in
 * test_app_can_classic_loopback.
 *
 * @retval k_ra_ok                Bits stamped.
 * @retval k_ra_err_invalid_arg   Channel out of range (reg == NULL).
 *
 * @pre ra_canfd_init(channel) returned k_ra_ok.
 * @post CFDC[channel].CTR has CTME=1, CTMS=01.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t can_demo_enable_internal_loopback(uint8_t channel)
{
  volatile r_canfd_t* reg = ra_canfd(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  uint32_t ctr = reg->CFDC[0].CTR;
  ctr |= (uint32_t)(1UL << k_can_demo_ctme_bit);
  ctr |= (uint32_t)(k_can_demo_ctms_intl << k_can_demo_ctms_shift);
  reg->CFDC[0].CTR = ctr;
  return k_ra_ok;
}

/**
 * @brief Bring CGC + SysTick + LEDs + CANFD0 (classic mode) up.
 *
 * @details
 * Calls ``ra_canfd_set_bitrate(channel, nominal, 0)`` -- the
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
  if (ra_cgc_init() != k_ra_ok) {
    can_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    can_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    can_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    can_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    can_demo_panic_halt();
  }
  if (ra_canfd_init((uint8_t)k_can_demo_channel) != k_ra_ok) {
    can_demo_panic_halt();
  }
  /* Classic CAN: data_bitrate_bps == 0. */
  if (ra_canfd_set_bitrate((uint8_t)k_can_demo_channel, (uint32_t)k_can_demo_bitrate, 0U) !=
      k_ra_ok) {
    can_demo_panic_halt();
  }
  if (can_demo_enable_internal_loopback((uint8_t)k_can_demo_channel) != k_ra_ok) {
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
[[nodiscard]] static ra_err_t can_demo_one_round_trip(uint8_t seq)
{
  ra_canfd_frame_t tx = {
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
  if (ra_canfd_transmit((uint8_t)k_can_demo_channel, &tx) != k_ra_ok) {
    return k_ra_err_hw_error;
  }
  ra_canfd_frame_t rx = {};
  if (ra_canfd_receive((uint8_t)k_can_demo_channel, &rx) != k_ra_ok) {
    return k_ra_err_no_data;
  }
  return k_ra_ok;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  can_demo_setup_or_halt();
  ra_isr_globals_enable();

  uint8_t seq = 0U;
  while (1) {
    if (can_demo_one_round_trip(seq) == k_ra_ok) {
      (void)ra_board_led_toggle(k_ra_board_led1);
    } else {
      (void)ra_board_led_toggle(k_ra_board_led2);
    }
    seq++;
    ra_delay_ms((uint32_t)k_can_demo_period_ms);
  }
  can_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
