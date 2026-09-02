/**
 * @file ra8_board_ra8p1.c
 * @brief Board-support implementation for the RA8P1 foundation board
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * Translates board-coordinate APIs ("LED1", "SW1", "the console") into HAL
 * calls for the RA8P1 (R7KA8P1KFLCAC). The BSP itself never touches MCU
 * registers; ``ra8_gpio_*`` / ``ra8_pfs_route_peripheral`` /
 * ``ra8_icu_configure_irq_pin`` / ``ra8_sci_*`` carry every register write, and
 * each of those HAL entry points carries its own RA8P1-identical HUM citation.
 *
 * The pin tables below are PROVISIONAL, mirrored from the pin-compatible
 * EK-RA8D2 layout because no RA8P1 board is defined yet (no committed EK-RA8P1
 * User's Manual, no ``ra8p1_kicad/`` PCB). See the header for the full
 * ``TODO(EK-RA8P1 UM / ra8p1_kicad)`` rationale; every pin is a valid GPIO / SCI
 * alternate on the RA8P1 (chip HUM R01UH1064EJ Ch 20 "I/O Ports").
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_board_ra8p1.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_cgc.h"
#include "ra8_elc_regs.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_icu.h"
#include "ra8_icu_regs.h"
#include "ra8_isr.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_sci.h"

/* =============================================================================
 * Board identity
 * =============================================================================
 */

const char* const k_ra8_board_name    = "RA8P1 foundation board";
const char* const k_ra8_board_doc_rev = "R01UH1064EJ (chip HUM)";
const char* const k_ra8_board_mcu     = "R7KA8P1KFLCAC";

ra8_err_t ra8_board_get_info(ra8_board_info_t* out)
{
  if (out == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  out->name    = k_ra8_board_name;
  out->doc_rev = k_ra8_board_doc_rev;
  out->mcu     = k_ra8_board_mcu;
  return k_ra8_ok;
}

/* =============================================================================
 * 1. User LEDs (provisional; mirrored from EK-RA8D2)
 * =============================================================================
 */

/**
 * @brief Lookup table from ``ra8_board_led_id_t`` to ``ra8_port_pin_t``.
 *
 * @details
 * Index by ``ra8_board_led_id_t`` (LED1=0, LED2=1, LED3=2). Provisional pins
 * mirrored from the EK-RA8D2 layout; valid GPIO outputs on the RA8P1 (chip HUM
 * R01UH1064EJ Ch 20). TODO(EK-RA8P1 UM / ra8p1_kicad): re-derive from schematic.
 */
static const ra8_port_pin_t s_led_pins[k_ra8_board_led_count] = {
  [k_ra8_board_led1] =
    (ra8_port_pin_t)RA8_PIN(k_ra8_port_6, k_ra8_pin_0), /**< P600 (provisional). */
  [k_ra8_board_led2] =
    (ra8_port_pin_t)RA8_PIN(k_ra8_port_3, k_ra8_pin_3), /**< P303 (provisional). */
  [k_ra8_board_led3] =
    (ra8_port_pin_t)RA8_PIN(k_ra8_port_10, k_ra8_pin_7), /**< PA07 (provisional). */
};

ra8_err_t ra8_board_led_pin(ra8_board_led_id_t led, ra8_port_pin_t* out_pin)
{
  if (out_pin == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if ((uint8_t)led >= (uint8_t)k_ra8_board_led_count) {
    return k_ra8_err_invalid_arg;
  }
  *out_pin = s_led_pins[led];
  return k_ra8_ok;
}

ra8_err_t ra8_board_led_init(ra8_board_led_id_t led)
{
  ra8_port_pin_t pin = k_ra8_pin_none;
  ra8_err_t      err = ra8_board_led_pin(led, &pin);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_gpio_output_init(pin, k_ra8_level_low);
}

ra8_err_t ra8_board_led_on(ra8_board_led_id_t led)
{
  ra8_port_pin_t pin = k_ra8_pin_none;
  ra8_err_t      err = ra8_board_led_pin(led, &pin);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_gpio_write(pin, k_ra8_level_high);
}

ra8_err_t ra8_board_led_off(ra8_board_led_id_t led)
{
  ra8_port_pin_t pin = k_ra8_pin_none;
  ra8_err_t      err = ra8_board_led_pin(led, &pin);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_gpio_write(pin, k_ra8_level_low);
}

ra8_err_t ra8_board_led_toggle(ra8_board_led_id_t led)
{
  ra8_port_pin_t pin = k_ra8_pin_none;
  ra8_err_t      err = ra8_board_led_pin(led, &pin);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_gpio_toggle(pin);
}

/* =============================================================================
 * 2. User switches (provisional; mirrored from EK-RA8D2)
 * =============================================================================
 */

/**
 * @brief Lookup table from ``ra8_board_sw_id_t`` to ``ra8_port_pin_t``.
 *
 * @details
 * SW1 -> P009, SW2 -> P008 (provisional; valid IRQ-capable GPIO inputs on the
 * RA8P1, chip HUM R01UH1064EJ Ch 20). TODO(EK-RA8P1 UM / ra8p1_kicad).
 */
static const ra8_port_pin_t s_sw_pins[k_ra8_board_sw_count] = {
  [k_ra8_board_sw1] =
    (ra8_port_pin_t)RA8_PIN(k_ra8_port_0, k_ra8_pin_9), /**< P009 (provisional). */
  [k_ra8_board_sw2] =
    (ra8_port_pin_t)RA8_PIN(k_ra8_port_0, k_ra8_pin_8), /**< P008 (provisional). */
};

/** @brief Lookup table from ``ra8_board_sw_id_t`` to ICU IRQ channel. */
static const uint8_t s_sw_irq_nums[k_ra8_board_sw_count] = {
  [k_ra8_board_sw1] = (uint8_t)k_ra8_board_sw1_irq, /**< 13 (provisional). */
  [k_ra8_board_sw2] = (uint8_t)k_ra8_board_sw2_irq, /**< 12 (provisional). */
};

ra8_err_t ra8_board_sw_pin(ra8_board_sw_id_t sw, ra8_port_pin_t* out_pin)
{
  if (out_pin == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if ((uint8_t)sw >= (uint8_t)k_ra8_board_sw_count) {
    return k_ra8_err_invalid_arg;
  }
  *out_pin = s_sw_pins[sw];
  return k_ra8_ok;
}

ra8_err_t ra8_board_sw_init(ra8_board_sw_id_t sw)
{
  ra8_port_pin_t pin = k_ra8_pin_none;
  ra8_err_t      err = ra8_board_sw_pin(sw, &pin);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_gpio_input_init(pin, k_ra8_pull_up);
}

ra8_err_t ra8_board_sw_read(ra8_board_sw_id_t sw, ra8_board_sw_state_t* out_pressed)
{
  if (out_pressed == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  ra8_port_pin_t pin = k_ra8_pin_none;
  ra8_err_t      err = ra8_board_sw_pin(sw, &pin);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_level_t lvl = k_ra8_level_high;
  err             = ra8_gpio_read(pin, &lvl);
  if (err != k_ra8_ok) {
    /* ra8_gpio_read returns k_ra8_ok for valid port-0 SW pins off-target. */
    return err; /* GCOVR_EXCL_LINE -- static board pins/channels cannot fail validation */
  }
  /* Buttons are active-low: low level == pressed. */
  *out_pressed = (lvl == k_ra8_level_low) ? k_ra8_board_sw_pressed : k_ra8_board_sw_released;
  return k_ra8_ok;
}

ra8_err_t ra8_board_sw_attach_irq(ra8_board_sw_id_t sw, ra8_board_sw_irq_cb_t cb, void* ctx)
{
  if (cb == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if ((uint8_t)sw >= (uint8_t)k_ra8_board_sw_count) {
    return k_ra8_err_invalid_arg;
  }
  /* Step 1: configure the ICU IRQ pin for falling-edge detection so a button
   * press (active-low) latches the IRQCR channel. The digital filter samples at
   * PCLKB to debounce contact bounce (RA8P1 ICU is byte-identical to the RA8D2,
   * HUM R01UH1064EJ Ch 14 "Interrupt Controller Unit"). */
  const ra8_icu_irq_cfg_t cfg = {
    .sense      = k_ra8_icu_irqmd_falling,
    .filter_div = k_ra8_icu_fclksel_pclkb,
    .filter_en  = true,
  };
  ra8_err_t err = ra8_icu_configure_irq_pin(s_sw_irq_nums[sw], &cfg);
  if (err != k_ra8_ok) {
    /* ra8_icu_configure_irq_pin succeeds for IRQ12/13 (well within 32-channel limit). */
    return err; /* GCOVR_EXCL_LINE -- static board pins/channels cannot fail validation */
  }

  /* Step 2: route the ELC event for the IRQ channel through an IELSR slot and
   * enable the matching NVIC line. SW1 -> IRQ13, SW2 -> IRQ12 (provisional). */
  const ra8_elc_event_t evt =
    (sw == k_ra8_board_sw1) ? k_ra8_elc_event_icu_irq13 : k_ra8_elc_event_icu_irq12;
  return ra8_isr_register(evt, (ra8_isr_handler_t)cb, ctx, k_ra8_isr_prio_default, nullptr);
}

/* =============================================================================
 * 3. Debug UART console (provisional; mirrored from EK-RA8D2 J-Link OB VCOM)
 * =============================================================================
 */

/**
 * @brief Tracks whether ``ra8_board_uart_console_init`` has succeeded.
 *
 * @details
 * Set true after ``ra8_sci_init`` returns ok and the PFS routes are programmed.
 * The write/read/flush helpers refuse to forward to ra8_sci while this flag is
 * false so callers cannot drive an unconfigured channel.
 *
 * @note Written once during console init; read by the write/read/flush helpers.
 * @warning Do not modify outside ``ra8_board_uart_console_init``.
 * @since 0.1.0
 */
static bool s_uart_console_initialized = false;

/**
 * @enum ra8_board_uart_console_clock_t
 * @brief Minimum SCI module operating clock accepted by the console init.
 *
 * @details
 * The RA8P1 SCI_B baud-rate generator is clocked from PCLKA (byte-identical to
 * the RA8D2, chip HUM R01UH1064EJ Ch 38 "Serial Communications Interface").
 * Before ``ra8_cgc_init()`` brings the PLL up the chip sits on MOCO (~8 MHz),
 * far too low for a usable 115200 baud divisor, so the console init refuses to
 * come up until CGC has published a post-PLL PCLKA value. This floor sits well
 * above MOCO and below the target so any reasonable CGC tree still qualifies.
 *
 * @invariant Value is a plausible SCI operating-clock lower bound in Hz.
 */
typedef enum : uint32_t {
  k_ra8_board_uart_console_min_pclka_hz = 16000000UL, /**< 16 MHz PCLKA floor. */
} ra8_board_uart_console_clock_t;

ra8_err_t ra8_board_uart_console_init(uint32_t baud)
{
  if (baud == 0U) {
    return k_ra8_err_invalid_arg;
  }

  /* SCI8's baud generator is fed by PCLKA. Read it from CGC at runtime so the
   * BRR tracks whatever the application's CGC tree settled on instead of an
   * assumed constant (the classic 2x-too-fast UART bug). */
  uint32_t  pclka_hz = 0U;
  ra8_err_t err      = ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &pclka_hz);
  if (err != k_ra8_ok) {
    /* ra8_cgc_get_clock_hz with a valid clock-id and non-null out always returns k_ra8_ok. */
    return err; /* GCOVR_EXCL_LINE -- static board pins/channels cannot fail validation */
  }
  if (pclka_hz < (uint32_t)k_ra8_board_uart_console_min_pclka_hz) {
    return k_ra8_err_not_initialized;
  }

  /* Route PD02 -> TXD8, PD03 -> RXD8 (PSEL = SCI async). */
  err = ra8_pfs_route_peripheral((ra8_port_pin_t)k_ra8_board_uart_console_pin_txd,
                                 k_ra8_psel_sci_async,
                                 "ra8_board.uart.console.txd");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral((ra8_port_pin_t)k_ra8_board_uart_console_pin_rxd,
                                 k_ra8_psel_sci_async,
                                 "ra8_board.uart.console.rxd");
  if (err != k_ra8_ok) {
    return err;
  }

  const ra8_sci_cfg_t cfg = {
    .baud      = baud,
    .data_bits = k_ra8_sci_data_8,
    .parity    = k_ra8_sci_parity_none,
    .stop_bits = k_ra8_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  err = ra8_sci_init((uint8_t)k_ra8_board_uart_console_sci_channel, &cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  s_uart_console_initialized = true;
  return k_ra8_ok;
}

ra8_err_t ra8_board_uart_console_write(const uint8_t* data, size_t len)
{
  if (len == 0U) {
    return k_ra8_ok;
  }
  if (data == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (!s_uart_console_initialized) {
    return k_ra8_err_not_initialized;
  }
  return ra8_sci_write_polling((uint8_t)k_ra8_board_uart_console_sci_channel, data, (uint32_t)len);
}

ra8_err_t ra8_board_uart_console_read(uint8_t* out, size_t cap, size_t* out_len)
{
  if (out_len == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  *out_len = 0U;
  if (cap == 0U) {
    return k_ra8_ok;
  }
  if (out == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (!s_uart_console_initialized) {
    return k_ra8_err_not_initialized;
  }

  /* Polled non-blocking drain: pull bytes while data is available, stop the
   * moment ra8_sci_getc_polling reports nothing. The cap bounds the loop
   * (NASA Rule 2). */
  for (size_t i = 0U; i < cap; ++i) {
    uint8_t         byte = 0U;
    const ra8_err_t err =
      ra8_sci_getc_polling((uint8_t)k_ra8_board_uart_console_sci_channel, &byte);
    if (err != k_ra8_ok) {
      /* No byte available yet -- treat as a non-blocking stop. */
      return k_ra8_ok;
    }
    out[i] = byte;
    *out_len += 1U;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_board_uart_console_flush(void)
{
  if (!s_uart_console_initialized) {
    return k_ra8_err_not_initialized;
  }
  /* Defer to ra8_sci_flush, which spins on CSR.TEND for the SCI8 channel and
   * carries the register-access HUM citation. Lets a panic handler drain the
   * failure log before WFI gates the SCI clock. */
  return ra8_sci_flush((uint8_t)k_ra8_board_uart_console_sci_channel);
}
