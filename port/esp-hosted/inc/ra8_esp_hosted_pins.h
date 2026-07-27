/**
 * @file port/esp-hosted/inc/ra8_esp_hosted_pins.h
 * @brief THE single point of change for the RA8 <-> ESP32-C6 harness map.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Every RA8-side pin fact about the esp-hosted link is stated once, here,
 * and nowhere else. ``port_esp_hosted_host_config.h`` derives the
 * upstream-spelled ``H_GPIO_*`` macros from these enumerators, and
 * ``ra8_esp_hosted_pins.c`` derives the interrupt-routing table from the
 * same list. Re-encoding any of it elsewhere is a defect.
 *
 * @par Why this file exists as its own header
 * The C6 side of the link is settled and bench-proven: chip select on
 * GPIO0, controller-out on GPIO1, controller-in on GPIO2, clock on GPIO3,
 * DATA_READY on GPIO4, HANDSHAKE on GPIO6, reset not wired
 * (``coprocessor/esp32c6/pins.env``). The **RA8 side is not**: the landed
 * probe ``examples/ek_ra8d2/hw_pending/c6_spi_probe`` established that the
 * module is not currently connected to the Pmod1 pins it probed, and the
 * harness is being rebuilt. When the rebuilt harness is characterised,
 * the only edit needed anywhere in this port is to the four
 * ::ra8_esp_hosted_pin_t rows below (and, if a side-band pin lands on an
 * ICU-capable pin, its row in ``k_ra8_esp_hosted_irq_map``).
 *
 * @par Pin identity comes from the board layer
 * No EK-RA8D2 pin number is written here. Each row names a
 * ``k_ra8_board_pmod1_*`` enumerator from
 * ``libs/ra8_board_ek_ra8d2/inc/ra8_board_ek_ra8d2_connectors.h``, which
 * carries the board User's Manual citation for the physical net. This
 * mirrors how ``c6_spi_probe`` builds its own pin tables.
 *
 * @par Interrupt routing is a per-pin fact, not an assumption
 * On this package the ICU external-interrupt inputs are concentrated on
 * port 0, so of the four Pmod1 side-band nets only P006 has an IRQ
 * channel (IRQ11); P402, P412 and P413 have none. The port therefore does
 * not assume every side-band pin can raise an edge: a pin with a channel
 * is serviced by the ICU, and a pin without one is serviced by the port's
 * software edge detector at a bounded poll period. Both deliver the same
 * callback, so the vendored driver is unaware of the difference and the
 * choice moves with the harness rather than with the code.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_board_ek_ra8d2_connectors.h"
#include "ra8_port_constants.h"

/**
 * @enum ra8_esp_hosted_pin_t
 * @brief RA8-side pin assignment for every esp-hosted link signal.
 *
 * @details
 * Values are packed ``ra8_port_pin_t`` codes taken straight from the
 * board connector header. Signal directions, as seen by the RA8:
 *
 *   - ``chip_select`` -- output, driven low for the whole 1600-byte frame.
 *   - ``handshake``   -- input, driven by the C6; asserted when the
 *                        co-processor is ready for the next transaction
 *                        and deasserted on the chip-select edge (the C6
 *                        image sets ``CONFIG_ESP_SPI_DEASSERT_HS_ON_CS``).
 *   - ``data_ready``  -- input, driven by the C6; asserted while its
 *                        transmit queue holds a frame for the host.
 *   - ``reset``       -- output, drives the co-processor reset net.
 *
 * @invariant Every value is either ``k_ra8_pin_none`` or a legal
 *            ``RA8_PIN(port, pin)`` with ``port <= k_ra8_port_max`` and
 *            ``pin <= k_ra8_pin_max``.
 * @invariant ``chip_select``, ``handshake`` and ``data_ready`` are
 *            distinct; the port rejects a duplicated assignment at init.
 *
 * @par Example:
 * @code
 * const ra8_port_pin_t hs = (ra8_port_pin_t)k_ra8_esp_hosted_pin_handshake;
 * ra8_level_t level = k_ra8_level_low;
 * (void)ra8_gpio_read(hs, &level);
 * @endcode
 *
 * @see ra8_esp_hosted_pin_irq_num
 * @see ra8_board_pmod1_gpio_pin_t
 * @since 0.1.0
 */
typedef enum : uint16_t {
  /** Chip select, RA8 output. Pmod1.1; the probe resolved this net to the
      SPI position of the board's Pmod1 mode mux, which needs SW4-3 ON. */
  k_ra8_esp_hosted_pin_chip_select = (uint16_t)k_ra8_board_pmod1_spi_cs,
  /** Controller-out peripheral-in data, RA8 output, routed to the SCI
      channel rather than driven as a GPIO. */
  k_ra8_esp_hosted_pin_copi = (uint16_t)k_ra8_board_pmod1_spi_copi,
  /** Controller-in peripheral-out data, RA8 input, routed to the SCI
      channel rather than sampled as a GPIO. */
  k_ra8_esp_hosted_pin_cipo = (uint16_t)k_ra8_board_pmod1_spi_cipo,
  /** Serial clock, RA8 output, routed to the SCI channel. */
  k_ra8_esp_hosted_pin_sck = (uint16_t)k_ra8_board_pmod1_spi_sck,
  /** HANDSHAKE, RA8 input, C6 output. No ICU channel on this net, so the
      port services it with its software edge detector. */
  k_ra8_esp_hosted_pin_handshake = (uint16_t)k_ra8_board_pmod1_gpio_a,
  /** DATA_READY, RA8 input, C6 output. Sits on the one Pmod1 side-band
      net with an ICU channel, so it gets the hardware edge path -- it is
      the latency-sensitive one, since it is what tells the host a frame
      is waiting. */
  k_ra8_esp_hosted_pin_data_ready = (uint16_t)k_ra8_board_pmod1_irq,
  /** Co-processor reset, RA8 output. ``k_ra8_pin_none`` until the rebuilt
      harness wires one: ``pins.env`` records the C6 reset input as
      disconnected, and inventing a pin here would make the port drive an
      unrelated net during bring-up. */
  k_ra8_esp_hosted_pin_reset = (uint16_t)k_ra8_pin_none,
} ra8_esp_hosted_pin_t;

/**
 * @enum ra8_esp_hosted_irq_slot_t
 * @brief Sentinel for "this pin has no ICU external-interrupt channel".
 *
 * @details
 * ``ra8_gpio_attach_irq`` accepts channels 0 through 15, so no legal
 * channel can collide with this value. ::ra8_esp_hosted_pin_irq_num
 * returns it for any pin the package does not route to the ICU, and the
 * port's interrupt-configuration slot reads it as "use the software edge
 * detector for this pin".
 *
 * @invariant Numerically outside the 0..15 range the GPIO HAL accepts.
 *
 * @par Example:
 * @code
 * if (ra8_esp_hosted_pin_irq_num(pin) == k_ra8_esp_hosted_irq_none) {
 *   use_software_edge_detector(pin);
 * }
 * @endcode
 *
 * @see ra8_esp_hosted_pin_irq_num
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_esp_hosted_irq_none = 0xFFU, /**< No ICU channel routes this pin. */
} ra8_esp_hosted_irq_slot_t;

/**
 * @brief Report the ICU external-interrupt channel that serves a pin.
 *
 * @details
 * Looks the packed pin up in the port's routing table, which records only
 * the nets this link uses and the channel the package assigns to each. The
 * table is a small linear scan rather than a sparse array indexed by pin,
 * because it holds at most a handful of rows and a scan cannot be indexed
 * out of range.
 *
 * A pin that is not in the table has no channel as far as this port is
 * concerned; that is reported as ::k_ra8_esp_hosted_irq_none rather than
 * guessed, so a harness change can never silently attach an edge to the
 * wrong channel.
 *
 * @param[in] pin Packed ``RA8_PIN(port, pin)`` code to look up. Any value
 *                is accepted, including ``k_ra8_pin_none``.
 *
 * @return The ICU channel number, 0..15, or the no-channel sentinel.
 * @retval k_ra8_esp_hosted_irq_none The pin has no ICU routing on this
 *         package, or is not one of the link's side-band nets.
 *
 * @pre The board connector header describes this package.
 * @pre No interrupt is currently attached to the returned channel by
 *      another module.
 * @post The routing table is unchanged.
 * @post The return value is either 0..15 or the sentinel; nothing else.
 *
 * @note Pure lookup, no hardware access; safe from any context including
 *       an interrupt handler.
 *
 * @par Example:
 * @code
 * const uint8_t irq =
 *   ra8_esp_hosted_pin_irq_num((ra8_port_pin_t)k_ra8_esp_hosted_pin_data_ready);
 * @endcode
 *
 * @see ra8_esp_hosted_pin_t
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 2: the scan is bounded by the compile-time table length.
 */
[[nodiscard]] uint8_t ra8_esp_hosted_pin_irq_num(ra8_port_pin_t pin);
