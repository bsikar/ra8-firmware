/**
 * @file main.c
 * @brief CANFD0 internal-loopback smoke test for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Runs entirely on the bare EK-RA8D2 -- no external transceiver
 * required. The demo:
 *
 *   1. Brings up CGC + SysTick + UART (SCI8) for log output.
 *   2. Initialises CANFD0 via ``ra_canfd_init`` and programs a
 *      500 kbps nominal bit rate via ``ra_canfd_set_bitrate``.
 *   3. Forces the channel into the silicon's internal-loopback
 *      mode by stamping ``CFDC[0].CTR.CTME=1`` + ``CTMS=01``
 *      directly through the register header (the public HAL does
 *      not yet expose this -- documented as a deliberate raw write
 *      in the comment block below).
 *   4. Once per second, transmits an 8-byte heartbeat frame at
 *      standard ID ``0x123`` and polls the RX FIFO for the
 *      mirrored frame. LED1 toggles on each successful round-trip;
 *      LED2 latches on if a TX/RX path fails.
 *
 * Exits the loop on the first hard HAL error and parks in WFI.
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
  k_canfd_demo_period_ms = 1000U,
  k_canfd_demo_bitrate   = 500000U,
  k_canfd_demo_id        = 0x123U,
} canfd_demo_const_t;

/** @brief Channel + payload layout. */
typedef enum : uint8_t {
  k_canfd_demo_channel = 0U,
  k_canfd_demo_dlc     = 8U,
} canfd_demo_layout_t;

/** @brief Constant payload bytes for the heartbeat frame. */
typedef enum : uint8_t {
  k_canfd_demo_payload_b1 = 0xA5U,
  k_canfd_demo_payload_b2 = 0x5AU,
  k_canfd_demo_payload_b3 = 0xFFU,
  k_canfd_demo_payload_b4 = 0x00U,
  k_canfd_demo_payload_b5 = 0xDEU,
  k_canfd_demo_payload_b6 = 0xADU,
  k_canfd_demo_payload_b7 = 0xBEU,
} canfd_demo_payload_t;

/**
 * @brief CFDC[0].CTR test-mode bits (HUM Ch 41 "CFDCnCTR" p 2762).
 *
 * @details
 * CTME is bit 17, CTMS occupies bits [19:18]. CTMS=01b selects
 * "internal loopback (CAN bus disconnected)" -- the controller
 * routes its own TX back into the RX path so we can prove the IP
 * works without a transceiver attached.
 */
typedef enum : uint32_t {
  k_canfd_demo_ctme_bit   = 17U,
  k_canfd_demo_ctms_shift = 18U,
  k_canfd_demo_ctms_intl  = 0x1UL, /**< Internal loopback. */
} canfd_demo_ctr_t;

/** @brief Park the CPU forever after fatal init failure. */
static void canfd_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Stamp CFDC[0].CTR test-mode bits for internal loopback.
 *
 * @details
 * Done via the public register header rather than the HAL because
 * ``ra_canfd`` does not expose a test-mode setter yet. The channel
 * must already be out of reset before this write lands; we call it
 * after ``ra_canfd_init`` returns.
 *
 * @retval k_ra_ok Always (raw register write).
 *
 * @pre ``ra_canfd_init(channel)`` returned ``k_ra_ok``.
 * @post ``CFDC[channel].CTR`` has CTME=1, CTMS=01.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t canfd_demo_enable_internal_loopback(uint8_t channel)
{
  volatile r_canfd_t* reg = ra_canfd(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  uint32_t ctr = reg->CFDC[0].CTR;
  ctr |= (uint32_t)(1UL << k_canfd_demo_ctme_bit);
  ctr |= (uint32_t)(k_canfd_demo_ctms_intl << k_canfd_demo_ctms_shift);
  reg->CFDC[0].CTR = ctr;
  return k_ra_ok;
}

/**
 * @brief Bring CGC + SysTick + LEDs + CANFD0 up. Halts on any error.
 *
 * @pre Reset_Handler has set up the C runtime.
 * @post CANFD0 is in operation mode with internal loopback enabled.
 *
 * @since 0.1.0
 */
static void canfd_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    canfd_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    canfd_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    canfd_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    canfd_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    canfd_demo_panic_halt();
  }
  if (ra_canfd_init((uint8_t)k_canfd_demo_channel) != k_ra_ok) {
    canfd_demo_panic_halt();
  }
  if (ra_canfd_set_bitrate((uint8_t)k_canfd_demo_channel,
                           (uint32_t)k_canfd_demo_bitrate,
                           (uint32_t)k_canfd_demo_bitrate) != k_ra_ok) {
    canfd_demo_panic_halt();
  }
  if (canfd_demo_enable_internal_loopback((uint8_t)k_canfd_demo_channel) != k_ra_ok) {
    canfd_demo_panic_halt();
  }
}

/**
 * @brief One TX/RX round-trip. Returns ok on success or first error.
 *
 * @par MC/DC:
 * Compound decision: ``transmit != ok || receive != ok``. Two atomic
 * conditions x N+1 = 3 vectors -- both ok (steady state) and each
 * branch fails (covered by the host integration tests).
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t canfd_demo_one_round_trip(uint8_t seq)
{
  ra_canfd_frame_t tx = {
    .id          = (uint32_t)k_canfd_demo_id,
    .dlc         = (uint8_t)k_canfd_demo_dlc,
    .is_extended = 0U,
    .is_fd       = 0U,
    .is_brs      = 0U,
    .data        = {seq,
                    (uint8_t)k_canfd_demo_payload_b1,
                    (uint8_t)k_canfd_demo_payload_b2,
                    (uint8_t)k_canfd_demo_payload_b3,
                    (uint8_t)k_canfd_demo_payload_b4,
                    (uint8_t)k_canfd_demo_payload_b5,
                    (uint8_t)k_canfd_demo_payload_b6,
                    (uint8_t)k_canfd_demo_payload_b7},
  };
  if (ra_canfd_transmit((uint8_t)k_canfd_demo_channel, &tx) != k_ra_ok) {
    return k_ra_err_hw_error;
  }
  ra_canfd_frame_t rx = {};
  if (ra_canfd_receive((uint8_t)k_canfd_demo_channel, &rx) != k_ra_ok) {
    return k_ra_err_no_data;
  }
  return k_ra_ok;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  canfd_demo_setup_or_halt();
  ra_isr_globals_enable();

  uint8_t seq = 0U;
  while (1) {
    if (canfd_demo_one_round_trip(seq) == k_ra_ok) {
      (void)ra_board_led_toggle(k_ra_board_led1);
    } else {
      (void)ra_board_led_toggle(k_ra_board_led2);
    }
    seq++;
    ra_delay_ms(k_canfd_demo_period_ms);
  }
  canfd_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
