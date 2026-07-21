/**
 * @file main.c
 * @brief ESP32-C6 spike application: UART banner + blink, through the HAL only.
 *
 * @details
 * `app_main` is the composition root. It selects the "ours" register-level
 * backends (the ONLY place a concrete driver is named), prints a banner over
 * UART0, then toggles a configurable output pin in a bounded loop with a crude
 * busy-wait delay. Every hardware effect goes through an @ref esp_gpio_ops_t or
 * @ref esp_uart_ops_t function pointer -- this file includes no register header
 * and pokes no MMIO directly, so swapping in an ESP-IDF backend changes only the
 * two factory calls below.
 *
 * @note `app_main` is invoked by `boot/start.S` after stack/bss/data setup and
 *       returns into a `wfi` spin, so the loop is intentionally bounded rather
 *       than infinite (NASA P10 Rule 2).
 * @since 0.1.0 (spike)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "esp_hal.h"

/**
 * @enum app_pin_t
 * @brief Output pin the spike blinks.
 */
typedef enum : uint8_t {
  /*
     * GPIO8 is the DevKitC/DevKitM "user LED" but it is an ADDRESSABLE WS2812
     * on the RMT peripheral, not a plain GPIO. Driving it as a push-pull output
     * will not light it correctly; the real LED-pin mapping is a bench confirm.
     * The spike drives a generic pin to exercise the GPIO path end to end.
     * GPIO8 is also one of the pads whose IO_MUX Function 0 is already GPIO,
     * so the reset-default mux drives it (TRM Ch 7.12 Table 7.12-1 p 262).
     * Never pick GPIO16/GPIO17 here: they are U0TXD/U0RXD at reset -- the
     * ROM console this spike prints its banner through.
     */
  k_app_blink_pin = 8, /**< Generic output pin toggled by the blink loop. */
} app_pin_t;

/**
 * @enum app_cfg_t
 * @brief Application-level constants (no magic numbers).
 */
typedef enum : uint32_t {
  k_app_uart_baud    = 115200u,  /**< Console baud passed to uart init().           */
  k_app_delay_iters  = 2000000u, /**< Busy-wait iterations between toggles (crude). */
  k_app_blink_cycles = 20u,      /**< Bounded number of toggle cycles.              */
} app_cfg_t;

/**
 * @brief Crude bounded busy-wait (no timer peripheral in the spike).
 * @param[in] iters Iteration count; the loop variable is `volatile` so the
 *            compiler cannot elide it.
 * @return void
 * @pre @p iters is a finite, statically-bounded count at the call site.
 * @post At least @p iters volatile reads/writes have retired.
 * @note Wall-clock duration is unspecified; real timing is a roadmap timer.
 * @since 0.1.0 (spike)
 */
static void app_delay(uint32_t iters)
{
  for (volatile uint32_t i = 0u; i < iters; ++i) {
    /* Intentionally empty: the volatile counter is the delay. */
  }
}

/**
 * @brief Spike entry point, called from `boot/start.S` after crt0 setup.
 * @details Wires the "ours" backends, emits a banner, then blinks a pin a
 *          bounded number of times. Returns to the caller's `wfi` spin.
 * @return void
 * @pre The C runtime is up (stack, gp, zeroed .bss, copied .data).
 * @pre The GPIO and UART peripherals are powered (ROM default).
 * @post A banner has been queued to UART0 (best effort).
 * @post The blink pin has been toggled @ref k_app_blink_cycles times.
 * @note Single-threaded; return values are intentionally ignored after the
 *       null-guards below because the spike has no recovery path.
 * @since 0.1.0 (spike)
 */
void app_main(void)
{
  const esp_gpio_ops_t* gpio = esp_gpio_ours_ops();
  const esp_uart_ops_t* uart = esp_uart_ours_ops();

  /* Two separate guards (not a compound decision) so no MC/DC vector is owed. */
  if (gpio == nullptr) {
    return;
  }
  if (uart == nullptr) {
    return;
  }

  (void)uart->init(uart->ctx, k_app_uart_baud);
  (void)gpio->init(gpio->ctx, k_app_blink_pin);

  static const char banner[] = "esp32c6: hello from our-own HAL\n";
  (void)uart->write(uart->ctx, (const uint8_t*)banner, sizeof(banner) - 1u);

  for (uint32_t cycle = 0u; cycle < k_app_blink_cycles; ++cycle) {
    (void)gpio->toggle(gpio->ctx, k_app_blink_pin);
    (void)uart->write(uart->ctx, (const uint8_t*)".", 1u);
    app_delay(k_app_delay_iters);
  }

  (void)uart->flush(uart->ctx);
}
