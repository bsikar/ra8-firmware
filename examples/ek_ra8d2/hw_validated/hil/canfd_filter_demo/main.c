/**
 * @file examples/ek_ra8d2/hw_validated/hil/canfd_filter_demo/main.c
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
 *   2. ``ra8_canfd_init`` + ``ra8_canfd_set_bitrate`` (500 kbps).
 *   3. ``ra8_canfd_filter_set(0, 0x100, 0x7FF, 8)``  -- exact match.
 *   4. ``ra8_canfd_filter_set(1, 0x110, 0x7F0, 8)``  -- mask lower 4.
 *   5. Self-test 1 / internal loopback (CFDC[0].CTR.CTME=1, CTMS=11b)
 *      via ``ra8_canfd_set_test_mode`` (HUM Ch 41 "CFDCnCTR" p 2710).
 *   6. Loop forever transmitting + checking RX FIFO.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_canfd.h"
#include "ra8_canfd_regs.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_canfd_filter_period_ms  = 500U,    /**< CANFD filter period ms.         */
  k_canfd_filter_bitrate    = 500000U, /**< CANFD filter bitrate.           */
  k_canfd_filter_id_exact   = 0x100U,  /**< CANFD filter ID exact.          */
  k_canfd_filter_id_mask    = 0x110U,  /**< CANFD filter ID mask.           */
  k_canfd_filter_mask_low4  = 0x7F0U,  /**< CANFD filter mask low4.         */
  k_canfd_filter_mask_full  = 0x7FFU,  /**< CANFD filter mask full.         */
  k_canfd_filter_id_nomatch = 0x200U,  /**< CANFD filter ID nomatch.        */
  k_canfd_filter_rx_spin    = 200000U, /**< ~10 ms RX poll budget at 1 GHz. */
  k_canfd_filter_drain_max  = 8U,      /**< Bounded RX FIFO drain count.    */
} canfd_filter_const_t;

/** @brief Channel + filter slots. */
typedef enum : uint8_t {
  k_canfd_filter_channel    = 0U,    /**< CANFD filter channel.    */
  k_canfd_filter_slot_a     = 0U,    /**< CANFD filter slot a.     */
  k_canfd_filter_slot_b     = 1U,    /**< CANFD filter slot b.     */
  k_canfd_filter_dlc        = 8U,    /**< CANFD filter dlc.        */
  k_canfd_filter_byte_shift = 8U,    /**< CANFD filter byte shift. */
  k_canfd_filter_payload_b2 = 0xA5U, /**< CANFD filter payload b2. */
  k_canfd_filter_payload_b3 = 0x5AU, /**< CANFD filter payload b3. */
  k_canfd_filter_payload_b4 = 0xDEU, /**< CANFD filter payload b4. */
  k_canfd_filter_payload_b5 = 0xADU, /**< CANFD filter payload b5. */
  k_canfd_filter_payload_b6 = 0xBEU, /**< CANFD filter payload b6. */
  k_canfd_filter_payload_b7 = 0xEFU, /**< CANFD filter payload b7. */
} canfd_filter_layout_t;

/* CFDC[0].CTR test-mode bits live in ra8_canfd_set_test_mode() now.
 * Bit positions (CTME = bit 24, CTMS = bits [26:25]) and the
 * Self-test 1 / internal-loopback selector (CTMS = 11b) come from
 * HUM Ch 41 "CFDCnCTR" p 2710. */

/**
 * @var g_canfd_filter_match
 * @brief HIL liveness counter -- incremented on every filter
 *        sub-round that behaved as expected (matched IDs round-trip,
 *        no-match ID gets rejected by the acceptance filter).
 *
 * @details
 * Read externally by scripts/hil/jlink_memprobe.sh via SWD. The probe
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

/**
 * @var g_canfd_filter_exact_match
 * @brief Per-sub-round diagnostic: exact-ID (0x100) round succeeded.
 * @note  Inspected by J-Link memprobe to localize filter regressions.
 * @since 0.1.0
 */
volatile uint32_t g_canfd_filter_exact_match = 0U;

/**
 * @var g_canfd_filter_exact_mismatch
 * @brief Per-sub-round diagnostic: exact-ID (0x100) round failed.
 * @since 0.1.0
 */
volatile uint32_t g_canfd_filter_exact_mismatch = 0U;

/**
 * @var g_canfd_filter_mask_match
 * @brief Per-sub-round diagnostic: mask-ID (0x110/0x7F0) round succeeded.
 * @since 0.1.0
 */
volatile uint32_t g_canfd_filter_mask_match = 0U;

/**
 * @var g_canfd_filter_mask_mismatch
 * @brief Per-sub-round diagnostic: mask-ID (0x110/0x7F0) round failed.
 * @since 0.1.0
 */
volatile uint32_t g_canfd_filter_mask_mismatch = 0U;

/**
 * @var g_canfd_filter_nomatch_match
 * @brief Per-sub-round diagnostic: no-match-ID (0x200) round behaved as
 *        expected (frame was rejected by the filter).
 * @since 0.1.0
 */
volatile uint32_t g_canfd_filter_nomatch_match = 0U;

/**
 * @var g_canfd_filter_nomatch_mismatch
 * @brief Per-sub-round diagnostic: no-match-ID (0x200) frame leaked
 *        through the filter -- this is the key diagnostic for whether
 *        GAFLLB+self-test really gates the FIFO.
 * @since 0.1.0
 */
volatile uint32_t g_canfd_filter_nomatch_mismatch = 0U;

/**
 * @brief Park the processor after a fatal CAN-FD filter demo failure.
 *
 * @details Retains the acceptance-filter, controller, and diagnostic counter
 *          state in a permanent wait-for-interrupt loop for debugger inspection.
 *
 * @return None.
 *
 * @pre The caller has determined filter validation cannot continue.
 * @pre Any per-round mismatch state required by the failure is recorded.
 * @post The function never returns to its caller.
 * @post No later CAN-FD frame or filter operation is attempted.
 *
 * @note Fatal-path helper for this single-core image only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_canfd_filter_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Enable Self-test 1 (internal loopback) on @p channel.
 *
 * @details
 * Forwards to ::ra8_canfd_set_test_mode which bounces the channel
 * through CH_HALT (the only mode where CTME/CTMS are writable per
 * HUM Ch 41 "CFDCnCTR" p 2710).
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
 * @note No frame may be active during the controller mode transition.
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_canfd_filter_loopback(uint8_t channel)
{
  return ra8_canfd_set_test_mode(channel, k_ra8_ctms_self_test_1);
}

/**
 * @brief Program both acceptance-filter slots used by the demo.
 *
 * @par MC/DC:
 * Compound decision: ``slot_a != ok || slot_b != ok``. Two atomic
 * conditions x N+1 = 3 vectors -- both ok / a fails / b fails;
 * exercised by the host test.
 *
 * @return ra8_err_t Status from programming the two acceptance slots.
 * @retval k_ra8_ok              Both filters programmed.
 * @retval k_ra8_err_invalid_arg HAL rejected an argument.
 *
 * @pre CAN-FD global configuration is available for filter updates.
 * @pre The two selected filter slots are reserved by this demo.
 * @post On success both exact-ID and masked-ID rules are installed.
 * @post If slot A fails, slot B is not attempted.
 *
 * @note Slot programming is ordered so the first error is preserved.
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_canfd_filter_program_slots(void)
{
  ra8_err_t err = ra8_canfd_filter_set((uint16_t)k_canfd_filter_slot_a,
                                       (uint32_t)k_canfd_filter_id_exact,
                                       (uint32_t)k_canfd_filter_mask_full,
                                       (uint8_t)k_canfd_filter_dlc);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_canfd_filter_set((uint16_t)k_canfd_filter_slot_b,
                              (uint32_t)k_canfd_filter_id_mask,
                              (uint32_t)k_canfd_filter_mask_low4,
                              (uint8_t)k_canfd_filter_dlc);
}

/**
 * @brief Bring the chip + LEDs + CANFD0 + filter slots up.
 *
 * @details Initializes timing and status LEDs, opens CANFD0, applies both bit
 *          rates, programs the demonstration filter slots, and enables internal
 *          loopback. Any failed dependency enters the permanent panic halt.
 *
 * @return None.
 *
 * @pre Reset-time initialization configured the core and C runtime.
 * @pre CANFD0, both filter slots, and LED1/LED2 are available to this image.
 * @post On success CANFD0 operates in internal loopback with both filters active.
 * @post On failure the function never returns to its caller.
 *
 * @note Single-shot boot helper; it is not reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_canfd_filter_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_canfd_filter_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_canfd_filter_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_canfd_filter_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    internal_canfd_filter_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    internal_canfd_filter_panic_halt();
  }
  if (ra8_canfd_init((uint8_t)k_canfd_filter_channel) != k_ra8_ok) {
    internal_canfd_filter_panic_halt();
  }
  if (ra8_canfd_set_bitrate((uint8_t)k_canfd_filter_channel,
                            (uint32_t)k_canfd_filter_bitrate,
                            (uint32_t)k_canfd_filter_bitrate) != k_ra8_ok) {
    internal_canfd_filter_panic_halt();
  }
  if (internal_canfd_filter_program_slots() != k_ra8_ok) {
    internal_canfd_filter_panic_halt();
  }
  if (internal_canfd_filter_loopback((uint8_t)k_canfd_filter_channel) != k_ra8_ok) {
    internal_canfd_filter_panic_halt();
  }
}

/**
 * @brief Send one frame at @p id, attempt to receive it back.
 *
 * @param[in] id Arbitration ID to send.
 * @return k_ra8_ok if both TX + RX succeed, error otherwise.
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_canfd_filter_one_round(uint32_t id)
{
  ra8_canfd_frame_t tx = {.id          = id,
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
  if (ra8_canfd_transmit((uint8_t)k_canfd_filter_channel, &tx) != k_ra8_ok) {
    return k_ra8_err_hw_error;
  }
  /* Poll for the loopback frame: 500 kbit/s + 8 data bytes is ~240 us
   * round-trip, so wait up to ~10 ms before declaring no_data. */
  ra8_canfd_frame_t rx = {};
  for (uint32_t i = 0U; i < (uint32_t)k_canfd_filter_rx_spin; i++) {
    if (ra8_canfd_receive((uint8_t)k_canfd_filter_channel, &rx) == k_ra8_ok) {
      return k_ra8_ok;
    }
  }
  return k_ra8_err_no_data;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  internal_canfd_filter_setup_or_halt();
  ra8_isr_globals_enable();

  while (1) {
    /* Drain any stale frames left in RX FIFO 0 before this iteration's
     * three sub-rounds. Without this, a late-arriving frame from the
     * mask round can land in the FIFO while the no-match round is
     * spinning on ra8_canfd_receive and report a false "match" for
     * what should be a rejected frame. */
    ra8_canfd_frame_t scratch = {};
    for (uint8_t i = 0U; i < (uint8_t)k_canfd_filter_drain_max; i++) {
      if (ra8_canfd_receive((uint8_t)k_canfd_filter_channel, &scratch) != k_ra8_ok) {
        break;
      }
    }

    /* Two IDs the filters accept and one they should reject. The
     * loopback path mirrors every TX, but only filter-matched frames
     * land in the RX FIFO. */
    if (internal_canfd_filter_one_round((uint32_t)k_canfd_filter_id_exact) == k_ra8_ok) {
      (void)ra8_board_led_toggle(k_ra8_board_led1);
      g_canfd_filter_exact_match += 1U;
      g_canfd_filter_match += 1U;
    } else {
      g_canfd_filter_exact_mismatch += 1U;
      g_canfd_filter_mismatch += 1U;
    }
    if (internal_canfd_filter_one_round((uint32_t)k_canfd_filter_id_mask) == k_ra8_ok) {
      (void)ra8_board_led_toggle(k_ra8_board_led1);
      g_canfd_filter_mask_match += 1U;
      g_canfd_filter_match += 1U;
    } else {
      g_canfd_filter_mask_mismatch += 1U;
      g_canfd_filter_mismatch += 1U;
    }
    /* No-match: receive should report no_data; if it doesn't, latch LED2. */
    if (internal_canfd_filter_one_round((uint32_t)k_canfd_filter_id_nomatch) == k_ra8_ok) {
      (void)ra8_board_led_toggle(k_ra8_board_led2);
      g_canfd_filter_nomatch_mismatch += 1U;
      g_canfd_filter_mismatch += 1U;
    } else {
      g_canfd_filter_nomatch_match += 1U;
      g_canfd_filter_match += 1U;
    }
    ra8_delay_ms(k_canfd_filter_period_ms);
  }
  internal_canfd_filter_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
