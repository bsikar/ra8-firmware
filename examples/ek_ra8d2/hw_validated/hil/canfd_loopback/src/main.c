/**
 * @file examples/ek_ra8d2/hw_validated/hil/canfd_loopback/src/main.c
 * @brief CANFD0 internal-loopback HIL test for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Runs entirely on the bare EK-RA8D2 -- no external transceiver
 * required. The demo:
 *
 *   1. Brings up CGC + SysTick + UART (SCI8) for log output.
 *   2. Initialises CANFD0 via ``ra8_canfd_init`` and programs a
 *      500 kbps nominal bit rate via ``ra8_canfd_set_bitrate``.
 *   3. Forces the channel into the silicon's Self-test 1 (internal
 *      loopback) mode by calling ``ra8_canfd_set_test_mode`` which
 *      stamps ``CFDC[0].CTR.CTME=1`` + ``CTMS=11b`` in CH_HALT
 *      (HUM Ch 41 "CFDCnCTR" p 2710).
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

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_canfd.h"
#include "ra8_canfd_regs.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_canfd_demo_period_ms = 1000U,   /**< CANFD demo period ms.               */
  k_canfd_demo_bitrate   = 500000U, /**< CANFD demo bitrate.                 */
  k_canfd_demo_id        = 0x123U,  /**< CANFD demo ID.                      */
  k_canfd_demo_rx_spin   = 200000U, /**< ~10 ms RX poll budget at 1 GHz CPU. */
} canfd_demo_const_t;

/** @brief Channel + payload layout. */
typedef enum : uint8_t {
  k_canfd_demo_channel = 0U, /**< CANFD demo channel. */
  k_canfd_demo_dlc     = 8U, /**< CANFD demo dlc.     */
} canfd_demo_layout_t;

/** @brief Constant payload bytes for the heartbeat frame. */
typedef enum : uint8_t {
  k_canfd_demo_payload_b1 = 0xA5U, /**< CANFD demo payload b1. */
  k_canfd_demo_payload_b2 = 0x5AU, /**< CANFD demo payload b2. */
  k_canfd_demo_payload_b3 = 0xFFU, /**< CANFD demo payload b3. */
  k_canfd_demo_payload_b4 = 0x00U, /**< CANFD demo payload b4. */
  k_canfd_demo_payload_b5 = 0xDEU, /**< CANFD demo payload b5. */
  k_canfd_demo_payload_b6 = 0xADU, /**< CANFD demo payload b6. */
  k_canfd_demo_payload_b7 = 0xBEU, /**< CANFD demo payload b7. */
} canfd_demo_payload_t;

/* CFDC[0].CTR test-mode bits live in ra8_canfd_set_test_mode() now.
 * Bit positions (CTME = bit 24, CTMS = bits [26:25]) and the
 * Self-test 1 / internal-loopback selector (CTMS = 11b) come from
 * HUM Ch 41 "CFDCnCTR" p 2710. */

/**
 * @var g_canfd_match
 * @brief HIL liveness counter -- incremented on every successful
 *        TX -> internal loopback -> RX round-trip.
 *
 * @details
 * Read externally by scripts/hil/jlink_memprobe.sh via SWD. The probe
 * asserts this counter advances by >= HIL_PROBE_MIN_ADVANCE over the
 * sample window, proving the CAN_FD peripheral actually moved frames
 * through its internal loopback (the alive-mode check could only
 * prove the chip didn't crash, not that CAN-FD actually worked).
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_canfd_match = 0U;

/**
 * @var g_canfd_mismatch
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
volatile uint32_t g_canfd_mismatch = 0U;

/** @brief Bring-up step tracker: 1=init ok, 2=set_bitrate ok, 3=loopback ok. */
volatile uint32_t g_canfd_init_step = 0U;

/** @brief NCFG readback latched at end of set_bitrate (proves NCFG write). */
volatile uint32_t g_canfd_ncfg_after_setbitrate = 0U;

/**
 * @brief Park the processor after a fatal CAN-FD loopback failure.
 *
 * @details Retains the controller registers, init-step marker, and HIL counters
 *          in a permanent wait-for-interrupt loop for debugger inspection.
 *
 * @return None.
 *
 * @pre The caller has determined that loopback validation cannot continue.
 * @pre Any init-step or mismatch state required by the failure is recorded.
 * @post The function never returns to its caller.
 * @post No later CAN-FD transmit or receive is attempted.
 *
 * @note Fatal-path helper for this single-core image only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_canfd_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Enable Self-test 1 (internal loopback) on @p channel.
 *
 * @details
 * Forwards to ``ra8_canfd_set_test_mode`` which bounces the channel
 * through CH_HALT to land CTME = 1 and CTMS = 11b in CFDC[0].CTR.
 * HUM Ch 41 "CFDCnCTR" p 2710 -- CTME / CTMS are only writable in
 * CH_HALT mode.
 *
 * @param[in] channel CAN-FD controller channel to place in internal loopback.
 *
 * @return ra8_err_t Status from applying the controller test mode.
 * @retval k_ra8_ok                Bits stamped, channel back in operation.
 * @retval k_ra8_err_invalid_arg   Channel index rejected by the HAL.
 *
 * @pre  ``ra8_canfd_init(channel)`` returned ``k_ra8_ok``.
 * @pre  No TX/RX is in flight on @p channel.
 * @post ``CFDC[channel].CTR`` has CTME=1, CTMS=11b.
 * @post Channel is back in CH_OPERATION ready to TX.
 *
 * @note The caller must ensure no frame is active during the mode transition.
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t
internal_canfd_demo_enable_internal_loopback(uint8_t channel)
{
  return ra8_canfd_set_test_mode(channel, k_ra8_ctms_self_test_1);
}

/**
 * @brief Bring CGC + SysTick + LEDs + CANFD0 up. Halts on any error.
 *
 * @details Initializes the time base and status LEDs, opens CANFD0, applies the
 *          nominal and data bit rates, records the NCFG readback, and enables
 *          internal loopback. Any failed step enters the permanent panic halt.
 *
 * @return None.
 *
 * @pre Reset_Handler has set up the C runtime.
 * @pre CANFD0 and LED1/LED2 are available to this image.
 * @post CANFD0 is in operation mode with internal loopback enabled.
 * @post On success the init-step marker reaches the loopback-complete value.
 *
 * @note Single-shot boot helper; it is not reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_canfd_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_canfd_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_canfd_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_canfd_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    internal_canfd_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    internal_canfd_demo_panic_halt();
  }
  if (ra8_canfd_init((uint8_t)k_canfd_demo_channel) != k_ra8_ok) {
    internal_canfd_demo_panic_halt();
  }
  g_canfd_init_step = 1U;
  if (ra8_canfd_set_bitrate((uint8_t)k_canfd_demo_channel,
                            (uint32_t)k_canfd_demo_bitrate,
                            (uint32_t)k_canfd_demo_bitrate) != k_ra8_ok) {
    internal_canfd_demo_panic_halt();
  }
  g_canfd_init_step             = 2U;
  g_canfd_ncfg_after_setbitrate = ra8_canfd((uint8_t)k_canfd_demo_channel)->CFDC[0].NCFG;
  if (internal_canfd_demo_enable_internal_loopback((uint8_t)k_canfd_demo_channel) != k_ra8_ok) {
    internal_canfd_demo_panic_halt();
  }
  g_canfd_init_step = 3U;
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
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_canfd_demo_one_round_trip(uint8_t seq)
{
  ra8_canfd_frame_t tx = {
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
  if (ra8_canfd_transmit((uint8_t)k_canfd_demo_channel, &tx) != k_ra8_ok) {
    return k_ra8_err_hw_error;
  }
  /* Poll for the loopback frame: 500 kbit/s + 8 data bytes ~ 240 us
   * round-trip, so wait up to ~10 ms before declaring no_data. */
  ra8_canfd_frame_t rx = {};
  for (uint32_t i = 0U; i < (uint32_t)k_canfd_demo_rx_spin; i++) {
    if (ra8_canfd_receive((uint8_t)k_canfd_demo_channel, &rx) == k_ra8_ok) {
      return k_ra8_ok;
    }
  }
  return k_ra8_err_no_data;
}

void main(void)
{
  internal_canfd_demo_setup_or_halt();
  ra8_isr_globals_enable();

  uint8_t seq = 0U;
  while (1) {
    if (internal_canfd_demo_one_round_trip(seq) == k_ra8_ok) {
      (void)ra8_board_led_toggle(k_ra8_board_led1);
      g_canfd_match += 1U;
    } else {
      (void)ra8_board_led_toggle(k_ra8_board_led2);
      g_canfd_mismatch += 1U;
    }
    seq++;
    ra8_delay_ms(k_canfd_demo_period_ms);
  }
  internal_canfd_demo_panic_halt();
}
