/**
 * @file examples/ek_ra8d2/canfd_filter_demo/main.c
 * @brief CAN-FD acceptance-filter demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Programs two acceptance-filter slots on CANFD0 -- one matching ID
 * 0x100 exactly, one wildcarding the lower 4 bits -- then transmits
 * frames at IDs 0x100 (matching), 0x101 (matches by mask), and 0x200
 * (no match) in internal-loopback. LED1 toggles on each *expected*
 * round-trip, LED2 latches if behaviour deviates.
 *
 * Sequence:
 *   1. CGC + SysTick + LEDs.
 *   2. ``ra_canfd_init`` + ``ra_canfd_set_bitrate`` (500 kbps).
 *   3. ``ra_canfd_filter_set(0, 0x100, 0x7FF, 8)``  -- exact match.
 *   4. ``ra_canfd_filter_set(1, 0x110, 0x7F0, 8)``  -- mask lower 4.
 *   5. Internal loopback test mode (CFDC[0].CTR.CTME=1, CTMS=01).
 *   6. Loop forever transmitting + checking RX FIFO.
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
  k_canfd_filter_period_ms  = 500U,
  k_canfd_filter_bitrate    = 500000U,
  k_canfd_filter_id_exact   = 0x100U,
  k_canfd_filter_id_mask    = 0x110U,
  k_canfd_filter_mask_low4  = 0x7F0U,
  k_canfd_filter_mask_full  = 0x7FFU,
  k_canfd_filter_id_nomatch = 0x200U,
} canfd_filter_const_t;

/** @brief Channel + filter slots. */
typedef enum : uint8_t {
  k_canfd_filter_channel    = 0U,
  k_canfd_filter_slot_a     = 0U,
  k_canfd_filter_slot_b     = 1U,
  k_canfd_filter_dlc        = 8U,
  k_canfd_filter_byte_shift = 8U,
  k_canfd_filter_payload_b2 = 0xA5U,
  k_canfd_filter_payload_b3 = 0x5AU,
  k_canfd_filter_payload_b4 = 0xDEU,
  k_canfd_filter_payload_b5 = 0xADU,
  k_canfd_filter_payload_b6 = 0xBEU,
  k_canfd_filter_payload_b7 = 0xEFU,
} canfd_filter_layout_t;

/** @brief Internal-loopback bits in CFDC[0].CTR (HUM Ch 41 p 2762). */
typedef enum : uint32_t {
  k_canfd_filter_ctme_bit   = 17U,
  k_canfd_filter_ctms_shift = 18U,
  k_canfd_filter_ctms_intl  = 0x1UL,
} canfd_filter_ctr_t;

/**
 * @var g_canfd_filter_match
 * @brief HIL liveness counter -- incremented on every filter
 *        sub-round that behaved as expected (matched IDs round-trip,
 *        no-match ID gets rejected by the acceptance filter).
 *
 * @details
 * Read externally by scripts/hil_jlink_memprobe.sh via SWD. The probe
 * asserts this counter advances by >= HIL_PROBE_MIN_ADVANCE over the
 * sample window, proving the acceptance-filter slots really gate the
 * RX FIFO (alive-mode could only prove the chip didn't crash, not
 * that the filters discriminated frames).
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_canfd_filter_match = 0U;

/**
 * @var g_canfd_filter_mismatch
 * @brief HIL failure counter -- incremented whenever filter behavior
 *        deviates: a matched-ID round failed, or a no-match-ID frame
 *        leaked through and produced a successful RX.
 *
 * @details
 * The memprobe asserts this stays at 0 (or below
 * HIL_PROBE_MAX_FAILURE). Catches both "TX/RX path broke" and
 * "acceptance filter let everything through" -- previously invisible
 * because the chip kept iterating the main loop happily.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_canfd_filter_mismatch = 0U;

/** @brief Park the CPU after a fatal init failure. */
static void canfd_filter_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Stamp CFDC[0].CTR test-mode bits for internal loopback.
 *
 * @retval k_ra_ok            Loopback bits programmed.
 * @retval k_ra_err_invalid_arg Channel index rejected.
 */
[[nodiscard]] static ra_err_t canfd_filter_loopback(uint8_t channel)
{
  volatile r_canfd_t* reg = ra_canfd(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  uint32_t ctr = reg->CFDC[0].CTR;
  ctr |= (uint32_t)(1UL << k_canfd_filter_ctme_bit);
  ctr |= (uint32_t)(k_canfd_filter_ctms_intl << k_canfd_filter_ctms_shift);
  reg->CFDC[0].CTR = ctr;
  return k_ra_ok;
}

/**
 * @brief Program both acceptance-filter slots used by the demo.
 *
 * @par MC/DC:
 * Compound decision: ``slot_a != ok || slot_b != ok``. Two atomic
 * conditions x N+1 = 3 vectors -- both ok / a fails / b fails;
 * exercised by the host test.
 *
 * @retval k_ra_ok              Both filters programmed.
 * @retval k_ra_err_invalid_arg HAL rejected an argument.
 */
[[nodiscard]] static ra_err_t canfd_filter_program_slots(void)
{
  ra_err_t err = ra_canfd_filter_set((uint16_t)k_canfd_filter_slot_a,
                                     (uint32_t)k_canfd_filter_id_exact,
                                     (uint32_t)k_canfd_filter_mask_full,
                                     (uint8_t)k_canfd_filter_dlc);
  if (err != k_ra_ok) {
    return err;
  }
  return ra_canfd_filter_set((uint16_t)k_canfd_filter_slot_b,
                             (uint32_t)k_canfd_filter_id_mask,
                             (uint32_t)k_canfd_filter_mask_low4,
                             (uint8_t)k_canfd_filter_dlc);
}

/**
 * @brief Bring the chip + LEDs + CANFD0 + filter slots up.
 */
static void canfd_filter_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    canfd_filter_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    canfd_filter_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    canfd_filter_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    canfd_filter_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    canfd_filter_panic_halt();
  }
  if (ra_canfd_init((uint8_t)k_canfd_filter_channel) != k_ra_ok) {
    canfd_filter_panic_halt();
  }
  if (ra_canfd_set_bitrate((uint8_t)k_canfd_filter_channel,
                           (uint32_t)k_canfd_filter_bitrate,
                           (uint32_t)k_canfd_filter_bitrate) != k_ra_ok) {
    canfd_filter_panic_halt();
  }
  if (canfd_filter_program_slots() != k_ra_ok) {
    canfd_filter_panic_halt();
  }
  if (canfd_filter_loopback((uint8_t)k_canfd_filter_channel) != k_ra_ok) {
    canfd_filter_panic_halt();
  }
}

/**
 * @brief Send one frame at @p id, attempt to receive it back.
 *
 * @param[in] id Arbitration ID to send.
 * @return k_ra_ok if both TX + RX succeed, error otherwise.
 */
[[nodiscard]] static ra_err_t canfd_filter_one_round(uint32_t id)
{
  ra_canfd_frame_t tx = {.id          = id,
                         .dlc         = (uint8_t)k_canfd_filter_dlc,
                         .is_extended = 0U,
                         .is_fd       = 0U,
                         .is_brs      = 0U,
                         .data        = {(uint8_t)id,
                                         (uint8_t)(id >> k_canfd_filter_byte_shift),
                                         (uint8_t)k_canfd_filter_payload_b2,
                                         (uint8_t)k_canfd_filter_payload_b3,
                                         (uint8_t)k_canfd_filter_payload_b4,
                                         (uint8_t)k_canfd_filter_payload_b5,
                                         (uint8_t)k_canfd_filter_payload_b6,
                                         (uint8_t)k_canfd_filter_payload_b7}};
  if (ra_canfd_transmit((uint8_t)k_canfd_filter_channel, &tx) != k_ra_ok) {
    return k_ra_err_hw_error;
  }
  ra_canfd_frame_t rx = {};
  if (ra_canfd_receive((uint8_t)k_canfd_filter_channel, &rx) != k_ra_ok) {
    return k_ra_err_no_data;
  }
  return k_ra_ok;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  canfd_filter_setup_or_halt();
  ra_isr_globals_enable();

  while (1) {
    /* Two IDs the filters accept and one they should reject. The
     * loopback path mirrors every TX, but only filter-matched frames
     * land in the RX FIFO. */
    if (canfd_filter_one_round((uint32_t)k_canfd_filter_id_exact) == k_ra_ok) {
      (void)ra_board_led_toggle(k_ra_board_led1);
      g_canfd_filter_match += 1U;
    } else {
      g_canfd_filter_mismatch += 1U;
    }
    if (canfd_filter_one_round((uint32_t)k_canfd_filter_id_mask) == k_ra_ok) {
      (void)ra_board_led_toggle(k_ra_board_led1);
      g_canfd_filter_match += 1U;
    } else {
      g_canfd_filter_mismatch += 1U;
    }
    /* No-match: receive should report no_data; if it doesn't, latch LED2. */
    if (canfd_filter_one_round((uint32_t)k_canfd_filter_id_nomatch) == k_ra_ok) {
      (void)ra_board_led_toggle(k_ra_board_led2);
      g_canfd_filter_mismatch += 1U;
    } else {
      g_canfd_filter_match += 1U;
    }
    ra_delay_ms(k_canfd_filter_period_ms);
  }
  canfd_filter_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
