/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/esp-hosted/src/ra8_esp_hosted_pins.c
 * @brief ICU external-interrupt routing table for the esp-hosted link pins.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Companion to ``ra8_esp_hosted_pins.h``. The header states which RA8 pin
 * carries which link signal; this file states which of those pins the
 * package routes to an ICU external-interrupt channel, which is a separate
 * fact and the one that decides whether a side-band net gets a hardware
 * edge or the port's software edge detector.
 *
 * The table lists every Pmod1 net the link can plausibly land on --
 * including the ones with no channel -- so a "no channel" answer is an
 * explicit recorded fact rather than the result of falling off the end of
 * a shorter list. On this package the ICU external-interrupt inputs are
 * concentrated on port 0 and a handful of others, so of the four Pmod1
 * side-band nets only P006 has a channel. As the harness is wired that is
 * HANDSHAKE, and DATA_READY on P402 is polled.
 *
 * @since 0.1.0
 */

#include "ra8_esp_hosted_pins.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2_connectors.h"
#include "ra8_port_constants.h"

/**
 * @enum ra8_esp_hosted_irq_route_t
 * @brief ICU channel numbers the EK-RA8D2 board layer records for Pmod1.
 *
 * @details
 * Named here rather than written as bare numbers so the table below reads
 * as signal routing rather than arithmetic. Both values come from the
 * board connector header, which carries the board User's Manual citation
 * for each net.
 *
 * @invariant Every value is within the 0..15 range
 *            ``ra8_gpio_attach_irq`` accepts.
 *
 * @par Example:
 * @code
 * const uint8_t ch = (uint8_t)k_ra8_esp_hosted_irq_pmod1_sideband;
 * @endcode
 *
 * @see ra8_esp_hosted_pin_irq_num
 * @since 0.1.0
 */
typedef enum : uint8_t {
  /** IRQ11, the channel P006 (J26-7) is routed to. It is deep-standby
      capable, which is why the board layer names it IRQ11-DS. As the
      harness is wired, P006 carries HANDSHAKE, so this is the channel the
      link's hardware edge runs on. */
  k_ra8_esp_hosted_irq_pmod1_sideband = 11U,
  /** IRQ14, the channel P804 (J26-1) is routed to. The link drives that
      pin as a chip-select output, so the channel goes unused here; it is
      recorded because a future harness could move a side-band net onto
      J26-1 and would then inherit a hardware edge for free. */
  k_ra8_esp_hosted_irq_pmod1_cs = 14U,
} ra8_esp_hosted_irq_route_t;

/**
 * @struct ra8_esp_hosted_irq_route
 * @brief One row of the pin-to-ICU-channel routing table.
 *
 * @details
 * A flat pair rather than a sparse array indexed by pin: the table holds a
 * handful of rows, and a linear scan over a compile-time-sized array
 * cannot be indexed out of range, which keeps the lookup inside NASA Power
 * of 10 Rule 2 without a bounds check on caller-supplied data.
 *
 * @invariant ``pin`` is a packed ``RA8_PIN(port, pin)`` code.
 * @invariant ``irq`` is 0..15 or ::k_ra8_esp_hosted_irq_none.
 *
 * @par Example:
 * @code
 * const ra8_esp_hosted_irq_route_row_t row = { .pin = 0x0006U, .irq = 11U };
 * @endcode
 *
 * @see ra8_esp_hosted_pin_irq_num
 * @since 0.1.0
 */
typedef struct ra8_esp_hosted_irq_route {
  uint16_t pin; /**< Packed ``RA8_PIN(port, pin)`` code for the net. */
  uint8_t  irq; /**< ICU channel, or ::k_ra8_esp_hosted_irq_none.    */
} ra8_esp_hosted_irq_route_row_t;

/**
 * @var k_ra8_esp_hosted_irq_map
 * @brief Pin-to-ICU-channel routing for every Pmod1 net the link may use.
 * @details Rows carrying ::k_ra8_esp_hosted_irq_none are deliberate: they
 * record that the package routes no external-interrupt channel to that
 * net, so the port must poll it. Removing such a row would turn a stated
 * fact into an accident of table length.
 * @note Read-only; the only consumer is ::ra8_esp_hosted_pin_irq_num.
 * @warning Edit this together with ``ra8_esp_hosted_pin_t`` when the
 *          harness changes; the two describe the same wiring.
 * @since 0.1.0
 */
static const ra8_esp_hosted_irq_route_row_t k_ra8_esp_hosted_irq_map[] = {
  {.pin = (uint16_t)k_ra8_board_pmod1_irq, .irq = (uint8_t)k_ra8_esp_hosted_irq_pmod1_sideband},
  {.pin = (uint16_t)k_ra8_board_pmod1_spi_cs, .irq = (uint8_t)k_ra8_esp_hosted_irq_pmod1_cs},
  {.pin = (uint16_t)k_ra8_board_pmod1_reset, .irq = (uint8_t)k_ra8_esp_hosted_irq_none},
  {.pin = (uint16_t)k_ra8_board_pmod1_gpio_a, .irq = (uint8_t)k_ra8_esp_hosted_irq_none},
  {.pin = (uint16_t)k_ra8_board_pmod1_gpio_b, .irq = (uint8_t)k_ra8_esp_hosted_irq_none},
};

/**
 * @var k_ra8_esp_hosted_irq_map_rows
 * @brief Row count of ::k_ra8_esp_hosted_irq_map.
 * @details Derived from the array so the loop bound cannot drift from the
 * data it bounds.
 * @note Read-only; compile-time constant.
 * @warning Never write a literal row count in its place.
 * @since 0.1.0
 */
static const uint32_t k_ra8_esp_hosted_irq_map_rows =
  (uint32_t)(sizeof(k_ra8_esp_hosted_irq_map) / sizeof(k_ra8_esp_hosted_irq_map[0]));

/** @brief Implementation of `ra8_esp_hosted_pin_irq_num()` -- linear scan
 *  over a compile-time-sized routing table, so no caller-supplied value
 *  ever indexes it. */
uint8_t ra8_esp_hosted_pin_irq_num(ra8_port_pin_t pin)
{
  const uint16_t wanted = (uint16_t)pin;

  for (uint32_t row = 0U; row < k_ra8_esp_hosted_irq_map_rows; row++) {
    if (k_ra8_esp_hosted_irq_map[row].pin == wanted) {
      return k_ra8_esp_hosted_irq_map[row].irq;
    }
  }

  return (uint8_t)k_ra8_esp_hosted_irq_none;
}
