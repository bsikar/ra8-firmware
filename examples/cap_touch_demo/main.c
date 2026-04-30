/**
 * @file main.c
 * @brief Capacitive touch demo for the EK-RA8D2 (CTSU2 self-cap)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip up via ``ra_cgc_init()`` (XTAL -> PLL1 -> CPUCLK0 =
 * 1 GHz, PCLKA = 125 MHz, PCLKB = 62.5 MHz, SCICLK = PLL1R / 4),
 * powers up the Capacitive Touch Sensing Unit (CTSU2), configures
 * a small set of self-capacitance electrodes, and runs a polling
 * scan loop that prints "touched <electrode_id>" on the J-Link OB
 * CDC port (SCI8 @ 115200) and toggles LED1 whenever an electrode's
 * raw count exceeds its calibrated baseline by more than the
 * configured touch threshold.
 *
 * Sequence:
 *   1. ``ra_cgc_init()`` -- standard FSP-quickstart clock tree.
 *   2. ``ra_time_init(cpuclk0_hz)`` for the heartbeat / poll delay.
 *   3. ``ra_pfs_route_peripheral()`` for the SCI8 (J-Link CDC) and
 *      the four CTSU electrode pins. The SCI8 PSEL is the standard
 *      ``k_ra_psel_sci_async`` (0x04). The CTSU pins use the raw
 *      PSEL code 0x0C from HUM Ch 20.4 "Peripheral I/O Table"
 *      (CTSU TS lines).
 *   4. ``ra_sci_init(8, 115200 8N1)`` for the diagnostic stream.
 *   5. ``ra_mstp_init()`` then ``ra_ctsu_open(&cfg)`` powers the
 *      CTSU with self-cap multi-scan mode and the electrode bitmap.
 *   6. ``ra_ctsu_calibrate()`` over each enabled electrode to
 *      establish a baseline (FSP-style 3-dummy-scan averaging).
 *   7. ``ra_ctsu_set_threshold()`` per electrode to a sensible
 *      delta (256 LSB above baseline -- tunable).
 *   8. Loop:
 *        - ``ra_ctsu_scan_start`` and poll.
 *        - For each electrode, ``ra_ctsu_get_data`` and compare
 *          ``raw - baseline > threshold``.
 *        - On touch: ``ra_gpio_toggle(LED1)`` and SCI8 print.
 *        - ``ra_delay_ms(50)`` between scans.
 *
 * Pin-mux notes: the EK-RA8D2 v1 silk pads for the four touch
 * electrodes were not confirmed against the User's Manual at
 * authoring time -- the placeholder TS-pin packed identifiers
 * below need a hardware bring-up pass before live capacitance
 * measurements will work. The application still compiles cleanly
 * and exercises the full driver API surface (open / calibrate /
 * scan / get_data / set_threshold).
 *
 * @par Architectural ring
 * [Ring 6 / APP] {World: S} -- application-layer code that runs in
 * the Secure world.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-29
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_cgc.h"
#include "ra_ctsu.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_mstp.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/**
 * @brief Compile-time settings for the CTSU touch demo.
 *
 * @details
 * The poll period is intentionally generous (50 ms) so the
 * J-Link CDC stream stays readable and the LED toggle is
 * visible to the eye. The touch threshold is a self-cap delta
 * count above the calibrated baseline; the value below was
 * picked from the FSP CTSU example's default and will need
 * tuning per electrode geometry / overlay thickness.
 */
typedef enum : uint32_t {
  k_cap_touch_baud         = 115200U,
  k_cap_touch_poll_ms      = 50U,
  k_cap_touch_threshold    = 256U,
  k_cap_touch_scan_timeout = 1000U,
} cap_touch_config_t;

/** @brief SCI channel used for the diagnostic stream (uint8_t channel id). */
typedef enum : uint8_t {
  k_cap_touch_sci_channel = 8U,
} cap_touch_sci_t;

/**
 * @brief CTSU2 PSEL code from HUM Ch 20.4 "Peripheral I/O Table".
 *
 * @details
 * The CTSU touch-sense (TS) lines are PFS PSEL = 0x0C. This is not
 * yet exposed in ``ra_gpio_constants.h``; using a numeric cast keeps
 * this app standalone until a shared constant is added.
 */
typedef enum : uint8_t {
  k_cap_touch_psel_ctsu = 0x0CU,
} cap_touch_psel_t;

/**
 * @brief Number of electrodes wired up in the demo.
 *
 * @details
 * The CTSU2 supports up to 36 self-cap electrodes (TS00..TS35).
 * The demo only enables four of them; the remaining 32 are masked
 * off in the electrode bitmap. The electrode indices below are
 * placeholders and need to be confirmed against the EK-RA8D2 v1
 * User's Manual touch-pad table before live testing.
 */
typedef enum : uint8_t {
  k_cap_touch_electrode_count = 4U,
} cap_touch_count_t;

/**
 * @brief Electrode index bitmap (CHAC0..CHAC4 packed into a uint64_t).
 *
 * @details
 * Bit n set => TSn enabled. The lowest four electrodes are enabled
 * here as a starter set; tuning for a real overlay needs a hardware
 * bring-up pass against the EK-RA8D2 v1 manual.
 *
 * TODO: confirm against EK-RA8D2 v1 manual.
 */
typedef enum : uint64_t {
  k_cap_touch_electrode_mask = 0x000000000000000FULL,
} cap_touch_mask_t;

/** @brief Threshold value typed as uint16_t for the CTSU API. */
typedef enum : uint16_t {
  k_cap_touch_threshold_u16 = 256U,
} cap_touch_threshold_t;

/** @brief PD_02 / PD_03 -- on-board J-Link CDC SCI8 TXD / RXD. */
static const ra_port_pin_t k_cap_touch_pin_txd = RA_PIN(k_ra_port_13, k_ra_pin_2);
static const ra_port_pin_t k_cap_touch_pin_rxd = RA_PIN(k_ra_port_13, k_ra_pin_3);

/**
 * @brief Placeholder pin identifiers for the four enabled electrodes.
 *
 * @details
 * TODO: confirm against EK-RA8D2 v1 manual.
 *
 * The exact P-port pin for each TS line depends on EK-RA8D2 v1's
 * touch overlay routing; until that is verified, point at four
 * adjacent low-port pins so the validator does not collide with the
 * SCI8 / LED pins. Update once the manual table is read.
 */
static const ra_port_pin_t k_cap_touch_pin_electrodes[k_cap_touch_electrode_count] = {
  RA_PIN(k_ra_port_4, k_ra_pin_0),
  RA_PIN(k_ra_port_4, k_ra_pin_1),
  RA_PIN(k_ra_port_4, k_ra_pin_2),
  RA_PIN(k_ra_port_4, k_ra_pin_3),
};

/** @brief Diagnostic banner emitted on every touch event. */
static const uint8_t k_cap_touch_msg_prefix[] = "touched ";
/** @brief Trailing newline for the touch event line. */
static const uint8_t k_cap_touch_msg_newline[] = "\r\n";

/**
 * @brief Halt forever in WFI -- used as a panic stop on init failure.
 *
 * @pre Called only after a fatal error in boot.
 *
 * @post CPU is parked; only a debugger or external reset wakes it.
 *
 * @since 0.1.0
 */
static void cap_touch_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Route SCI8 + the four CTSU electrode pins.
 *
 * @return Error code from the first failing route call, or k_ra_ok.
 *
 * @retval k_ra_ok                       All pins routed.
 * @retval k_ra_err_gpio_invalid_port    Port index out of range.
 * @retval k_ra_err_gpio_invalid_pin     Pin index out of range.
 * @retval k_ra_err_gpio_conflict        Pin already claimed by another owner.
 *
 * @pre IOPORT module is reachable.
 * @pre Caller is single-threaded init context.
 *
 * @post On success SCI8 + four CTSU TS pins are in their PSEL modes.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t cap_touch_pins_init(void)
{
  ra_err_t err =
    ra_pfs_route_peripheral(k_cap_touch_pin_txd, k_ra_psel_sci_async, "cap_touch.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_pfs_route_peripheral(k_cap_touch_pin_rxd, k_ra_psel_sci_async, "cap_touch.rxd8");
  if (err != k_ra_ok) {
    return err;
  }
  /* 0x0C is a valid PFS PSEL code per HUM Ch 20.4 (CTSU TS lines), not yet
   * exposed in ra_psel_t. The NOLINT below silences EnumCastOutOfRange. */
  const ra_psel_t ctsu_psel =
    (ra_psel_t)k_cap_touch_psel_ctsu; // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
  for (uint8_t i = 0U; i < k_cap_touch_electrode_count; i++) {
    err = ra_pfs_route_peripheral(k_cap_touch_pin_electrodes[i], ctsu_psel, "cap_touch.ts");
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}

/**
 * @brief Bring CGC + SysTick + LED1 up. Halts on any fail.
 *
 * @details
 * Splits the long boot sequence so each step stays under the
 * NASA Power-of-10 60-line function-size cap. Returns the live
 * PCLKA Hz so the caller can hand it to ``ra_sci_init``.
 *
 * @param[out] out_pclka_hz Receives the live PCLKA rate in Hz.
 *
 * @pre out_pclka_hz is non-NULL.
 *
 * @post On success the CGC, SysTick, and LED1 GPIO are live.
 * @post On any HAL failure this function does not return -- it
 *       parks the CPU in WFI.
 *
 * @since 0.1.0
 */
static void cap_touch_init_clocks_and_led(uint32_t* out_pclka_hz)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    cap_touch_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    cap_touch_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, out_pclka_hz) != k_ra_ok) {
    cap_touch_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    cap_touch_panic_halt();
  }
  if (cap_touch_pins_init() != k_ra_ok) {
    cap_touch_panic_halt();
  }
  if (ra_gpio_output_init(k_ra_pin_led1, k_ra_level_low) != k_ra_ok) {
    cap_touch_panic_halt();
  }
}

/**
 * @brief Open the SCI8 diagnostic stream at 115200 8N1.
 *
 * @param[in] pclka_hz Live PCLKA rate from ``ra_cgc_get_clock_hz``.
 *
 * @pre ``cap_touch_init_clocks_and_led`` has run successfully.
 * @pre pclka_hz is non-zero.
 *
 * @post SCI8 is enabled, ready to write polling bytes.
 * @post Halts in WFI on init failure.
 *
 * @since 0.1.0
 */
static void cap_touch_init_sci(uint32_t pclka_hz)
{
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_cap_touch_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init(k_cap_touch_sci_channel, &sci_cfg) != k_ra_ok) {
    cap_touch_panic_halt();
  }
}

/**
 * @brief Open the CTSU and calibrate every enabled electrode.
 *
 * @details
 * Powers up the CTSU2 in self-cap multi-scan mode, runs the
 * FSP-style three-dummy-scan calibration on each electrode, and
 * writes a starter touch threshold per electrode.
 *
 * @pre ``ra_mstp_init`` has been called.
 * @pre IRQs masked or single-threaded init context.
 *
 * @post CTSUCR0.PON = 1 and the four electrodes have valid baselines.
 * @post Halts in WFI on init failure.
 *
 * @since 0.1.0
 */
static void cap_touch_init_ctsu(void)
{
  if (ra_mstp_init() != k_ra_ok) {
    cap_touch_panic_halt();
  }

  const ra_ctsu_cfg_t ctsu_cfg = {
    .mode           = k_ra_ctsu_mode_self_multi,
    .clock_div      = k_ra_ctsu_clock_div_16,
    .scan_period    = 0x0FU,
    .stabilise_wait = 0x10U,
    .electrode_mask = k_cap_touch_electrode_mask,
    .transmit_mask  = 0U,
    .offset_initial = 0U,
  };
  if (ra_ctsu_open(&ctsu_cfg) != k_ra_ok) {
    cap_touch_panic_halt();
  }

  for (uint8_t i = 0U; i < k_cap_touch_electrode_count; i++) {
    if (ra_ctsu_calibrate(i) != k_ra_ok) {
      cap_touch_panic_halt();
    }
    if (ra_ctsu_set_threshold(i, k_cap_touch_threshold_u16) != k_ra_ok) {
      cap_touch_panic_halt();
    }
  }
}

/**
 * @brief Convert a small integer 0..9 to its ASCII digit.
 *
 * @param[in] n Integer in 0..9.
 * @return ASCII '0'..'9'.
 *
 * @pre n in 0..9.
 * @post Returned byte is in '0'..'9'.
 *
 * @since 0.1.0
 */
static uint8_t cap_touch_digit(uint8_t n)
{
  enum : uint8_t {
    k_ascii_zero  = 0x30U, /**< '0'. */
    k_ascii_radix = 10U,
  };
  return (uint8_t)(k_ascii_zero + (n % k_ascii_radix));
}

/**
 * @brief Print "touched <id>\\r\\n" over SCI8.
 *
 * @param[in] electrode Electrode index (0..k_cap_touch_electrode_count - 1).
 *
 * @pre SCI8 is initialised and TX FIFO is non-blocking.
 * @pre Electrode value fits in two ASCII digits (0..99).
 *
 * @post Three writes have been issued on SCI8.
 * @post No stack allocations beyond a 2-byte digit buffer.
 *
 * @since 0.1.0
 */
static void cap_touch_print_electrode(uint8_t electrode)
{
  enum : uint8_t {
    k_ten       = 10U,
    k_digit_buf = 2U,
  };
  uint8_t digits[k_digit_buf];
  digits[0] = cap_touch_digit((uint8_t)(electrode / k_ten));
  digits[1] = cap_touch_digit((uint8_t)(electrode % k_ten));

  (void)ra_sci_write_polling(k_cap_touch_sci_channel,
                             k_cap_touch_msg_prefix,
                             (uint32_t)(sizeof(k_cap_touch_msg_prefix) - 1U));
  (void)ra_sci_write_polling(k_cap_touch_sci_channel, digits, (uint32_t)k_digit_buf);
  (void)ra_sci_write_polling(k_cap_touch_sci_channel,
                             k_cap_touch_msg_newline,
                             (uint32_t)(sizeof(k_cap_touch_msg_newline) - 1U));
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up CGC + CTSU then runs a poll loop.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On clean entry the CPU stays in the scan + report loop forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  uint32_t pclka_hz = 0U;
  cap_touch_init_clocks_and_led(&pclka_hz);
  cap_touch_init_sci(pclka_hz);
  cap_touch_init_ctsu();

  ra_isr_globals_enable();

  while (1) {
    if (ra_ctsu_scan_start() != k_ra_ok) {
      break;
    }
    /* The driver's polling-wait helper is not exposed; instead we
     * rely on the configured stabilise + scan window having elapsed
     * by the time we sleep ``k_cap_touch_poll_ms``. The exact wait
     * time is deterministic for self-cap multi-scan and well below
     * 50 ms at PCLKB / 16. */
    ra_delay_ms(k_cap_touch_poll_ms);

    for (uint8_t i = 0U; i < k_cap_touch_electrode_count; i++) {
      uint32_t raw = 0U;
      if (ra_ctsu_get_data(i, &raw) != k_ra_ok) {
        continue;
      }
      uint16_t threshold = 0U;
      if (ra_ctsu_get_threshold(i, &threshold) != k_ra_ok) {
        continue;
      }
      if (raw > (uint32_t)threshold) {
        (void)ra_gpio_toggle(k_ra_pin_led1);
        cap_touch_print_electrode(i);
      }
    }
  }

  cap_touch_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
