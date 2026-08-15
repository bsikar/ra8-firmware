/**
 * @file examples/ek_ra8d2/hw_pending/i3c_i2c_peripheral_demo/main.c
 * @brief IIC_B peripheral-mode demo on EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings IIC_B0 up as a 7-bit-addressed peripheral at address 0x42, then
 * loops:
 *   1. Polls peripheral status, waiting for an Address-Matched (AAS) event.
 *   2. On AAS + RDBFF0, drains a single byte from the controller.
 *   3. On AAS + TDBEF0, pushes a one-byte response (echoing the
 *      previously received command byte).
 *   4. Toggles LED1 on every successful transaction.
 *
 * The demo is wired for an external IIC controller talking to the
 * EK-RA8D2 (e.g. a Raspberry Pi as ``i2cset``/``i2cget``); when no
 * controller is attached the peripheral simply idles. There is no
 * internal IIC loopback path on the RA8D2.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_i3c.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_iic_peripheral_period_ms = 1U, /**< Iic peripheral period ms. */
} iic_peripheral_const_t;

/** @brief Channel + peripheral address. */
typedef enum : uint8_t {
  k_iic_peripheral_channel = 0U,    /**< Iic peripheral channel.    */
  k_iic_peripheral_addr_7b = 0x42U, /**< Iic peripheral address 7b. */
} iic_peripheral_layout_t;

/**
 * @brief Park the CPU after a fatal initialization or service failure.
 * @details Enters a permanent wait-for-interrupt loop so peripheral state can
 *          be inspected without continuing with a partially initialized demo.
 * @pre Reset startup initialized the exception and stack environment.
 * @pre The caller has determined that normal peripheral service cannot continue.
 * @post Control never returns to the caller.
 * @post No further IIC peripheral transactions are initiated by the application.
 * @note An interrupt can wake one iteration, but the terminal loop resumes.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_iic_peripheral_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC + SysTick + LED1 + IIC_B peripheral up.
 * @details Initializes the clock tree and timebase, prepares the observable LED,
 *          then opens channel zero at the fixed seven-bit peripheral address.
 * @pre Reset startup completed data and BSS initialization.
 * @pre IIC_B channel zero and LED1 are not owned by another subsystem.
 * @post On return, timekeeping and LED1 are initialized and IIC_B accepts address 0x42.
 * @post Any failed prerequisite has entered the terminal panic loop.
 * @note Single-shot boot helper; it is not reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_iic_peripheral_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_iic_peripheral_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_iic_peripheral_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_iic_peripheral_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    internal_iic_peripheral_panic_halt();
  }
  const ra8_i3c_peripheral_cfg_t cfg = {
    .peripheral_addr_7b = (uint8_t)k_iic_peripheral_addr_7b,
    .general_call       = 0U,
  };
  if (ra8_i3c_peripheral_open((uint8_t)k_iic_peripheral_channel, &cfg) != k_ra8_ok) {
    internal_iic_peripheral_panic_halt();
  }
}

/**
 * @brief Service one event from the peripheral status mask.
 *
 * @par MC/DC:
 * Compound decision: ``(mask & rx_full) != 0 || (mask & tx_empty) != 0``.
 * Two atomic conditions x N+1 = 3 vectors -- rx-only, tx-only, both;
 * the host test exercises each independently by pre-loading the
 * status mask before calling this function.
 *
 * @param[in,out] last_byte Latched controller-write byte; echoed back
 *                          on the next controller-read.
 *
 * @retval k_ra8_ok           Event handled (or no event present).
 * @retval k_ra8_err_hw_error Underlying HAL surfaced an error.
 *
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_iic_peripheral_service(uint8_t* last_byte)
{
  uint8_t mask = 0U;
  if (ra8_i3c_peripheral_status((uint8_t)k_iic_peripheral_channel, &mask) != k_ra8_ok) {
    return k_ra8_err_hw_error;
  }
  bool did_anything = false;
  if ((mask & (uint8_t)k_ra8_i3c_peripheral_status_rx_full) != 0U) {
    uint8_t b = 0U;
    if (ra8_i3c_peripheral_receive((uint8_t)k_iic_peripheral_channel, &b, 1U) != k_ra8_ok) {
      return k_ra8_err_hw_error;
    }
    *last_byte   = b;
    did_anything = true;
  }
  if ((mask & (uint8_t)k_ra8_i3c_peripheral_status_tx_empty) != 0U) {
    if (ra8_i3c_peripheral_send((uint8_t)k_iic_peripheral_channel, last_byte, 1U) != k_ra8_ok) {
      return k_ra8_err_hw_error;
    }
    did_anything = true;
  }
  if (did_anything) {
    (void)ra8_board_led_toggle(k_ra8_board_led1);
  }
  return k_ra8_ok;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  internal_iic_peripheral_setup_or_halt();
  ra8_isr_globals_enable();

  uint8_t last_byte = 0U;
  while (1) {
    if (internal_iic_peripheral_service(&last_byte) != k_ra8_ok) {
      break;
    }
    ra8_delay_ms(k_iic_peripheral_period_ms);
  }
  internal_iic_peripheral_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
