/**
 * @file ra8_board_ek_ra8d2_usb.h
 * @brief USB-HS (J7) and USB-FS (J11) portion of the EK-RA8D2 v1
 *        board-support layer.
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * Sub-header of ``ra8_board_ek_ra8d2.h`` (the thin umbrella), carved out of
 * ``ra8_board_ek_ra8d2_peripherals.h`` when that file reached the 1000-line
 * maintainability cap. It carries what used to be Section 7 of that header
 * (the USBHS and USBFS board pin maps) plus the three USB members of the U15
 * I/O-expander block: the HS device/host role straps, the two chip-side
 * USBHS bring-up entry points, and the ``ra8_board_usb_port_init`` front
 * door that hides the FS-versus-HS asymmetry.
 *
 * Every declaration here was moved VERBATIM; no contract, Doxygen block, or
 * HUM/UM citation has changed. ``ra8_board_usbhs_pwr_set`` is the one new
 * entry point: the J7 host-power strap that nineteen example apps used to
 * drive by casting a board pin enum to a raw port pin.
 *
 * Consumers keep including ``ra8_board_ek_ra8d2.h``; this file is pulled in
 * by ``ra8_board_ek_ra8d2_peripherals.h`` and should not be included
 * directly.
 *
 * Authoritative source: ``docs/reference/ek-ra8d2-v1-users-manual.pdf``
 * (Rev 1.01, R20UT5523EG0101, October 2025).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_port_constants.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 7. USB (UM Section 5.4.1 + 6.2, Tables 22 + 28, p 30 + 34)
 * =============================================================================
 */

/**
 * @brief USBHS host/device routing per UM Table 28 p 34.
 *
 * @details
 * USBHS (J7, USB Type-C). The HS PHY DM/DP differential pair
 * (USBH_P/USBH_N) is internal to the chip and not a board-routed
 * port pin. VBUS sense (USBHS_cVBUS_CON) and CC pins are PHY-side.
 * Only the bus signals listed below are exposed to user firmware.
 *
 * USB-FS (J11, USB Type-C, UM Table 22 p 30) is the host/dev port
 * for the full-speed peripheral and follows the same arrangement.
 */
typedef enum : uint16_t {
  /* The HS D+/D- differential pair (USBH_P/USBH_N) is internal to the chip,
   * so only the two MCU-driven board signals below are port pins: the VBUS
   * sense input and the J7 host-power-switch enable. */
  k_ra8_board_usbhs_pin_vbus =
    RA8_PIN(k_ra8_port_4, k_ra8_pin_8), /**< P4_08 USBHS_VBUS sense. UM Table 28 p 34. */
  k_ra8_board_usbhs_pin_pwr =
    RA8_PIN(k_ra8_port_13,
            k_ra8_pin_7), /**< PD07 J7 host-power switch (HIGH = U18 drives 5 V VBUS). UM 6.2. */
} ra8_board_usbhs_pin_t;

/**
 * @brief Drive the J7 host-power switch (PD07) that straps the HS role.
 *
 * @details
 * PD07 is the one MCU-driven board signal on the high-speed port, and it
 * carries two meanings depending on which end of the cable the board is.
 * HIGH turns U18 on so the board supplies 5 V VBUS to a downstream device
 * (host role, UM 6.2). LOW keeps U18 off so it cannot back-feed VBUS into
 * somebody else's bus (device role, and the setting a board acting as an
 * FS host also wants on its unused HS port).
 *
 * Nineteen example apps used to cast ::k_ra8_board_usbhs_pin_pwr to a port
 * pin and drive it themselves. The pin identity was already a board fact
 * living here; now the drive is too, so an app states the intent (``on``)
 * instead of restating the polarity.
 *
 * @param[in] on ``true`` supplies VBUS from U18, ``false`` holds it off.
 *
 * @return ra8_err_t Result of the GPIO init.
 * @retval k_ra8_ok PD07 is a push-pull output at the level ``on`` implies.
 * @retval k_ra8_err_gpio_conflict PD07 is already owned by another driver.
 *
 * @pre IOPORT is powered.
 * @post PD07 is an output; no other pin is touched.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_usbhs_pwr_set(bool on);

/**
 * @brief USB-FS (J11) board-routed port pins, UM Table 22 p 30.
 *
 * @details
 * The full-speed peripheral on J11 (USB Type-C) exposes four board-routed
 * pins: the D+/D- differential pair, the VBUS sense input, and the
 * MCU-driven VBUS-enable GPIO. Route D+/D-/VBUS to the USBFS peripheral
 * function (``k_ra8_psel_usb_fs``) and drive VBUSEN as a GPIO output
 * (LOW = device role, HIGH = supply 5 V VBUS for host role).
 *
 * These are the single source of truth for the EK-RA8D2 USB-FS pinout:
 * applications reference these names instead of re-encoding the port/pin
 * pair, so the board fact lives in exactly one place.
 */
typedef enum : uint16_t {
  k_ra8_board_usbfs_pin_dp =
    RA8_PIN(k_ra8_port_8, k_ra8_pin_14), /**< P8_14 D+. UM Table 22 p 30. */
  k_ra8_board_usbfs_pin_dm =
    RA8_PIN(k_ra8_port_8, k_ra8_pin_15), /**< P8_15 D-. UM Table 22 p 30. */
  k_ra8_board_usbfs_pin_vbus =
    RA8_PIN(k_ra8_port_4, k_ra8_pin_7), /**< P4_07 VBUS sense. UM Table 22 p 30. */
  k_ra8_board_usbfs_pin_vbusen =
    RA8_PIN(k_ra8_port_5, k_ra8_pin_0), /**< P5_00 VBUSEN GPIO. UM Table 22 p 30. */
} ra8_board_usbfs_pin_t;

/**
 * @brief Drive the U15 PI4IOE5V6408 I/O expander to select USB-HS device mode.
 *
 * @details
 * The EK-RA8D2 v1 carries a PI4IOE5V6408 8-bit I2C I/O expander at U15
 * (I2C address 0x43, EK-RA8D2 v1 UM Rev 1.01 Section 5.5.3 p 32 +
 * Section 4 p 16). U15 sits in parallel with the eight DIP switches of
 * SW4 -- when configured as outputs, U15's port pins override SW4 and
 * gate the same on-board mux that SW4 drives, including SW4-8 which
 * selects USB function on J7 (USB-HS): OFF = Device, ON = Host.
 *
 * U15 register convention (from the PI4IOE5V6408 datasheet register map):
 *  - 0x01 Device-ID  (expect 0xA0 or 0xA2)
 *  - 0x03 I/O direction      (1 = output)
 *  - 0x05 Output state       (1 = HIGH = SW4 OFF, 0 = LOW = SW4 ON)
 *  - 0x07 Output Hi-Z        (1 = Hi-Z)
 *  - 0x0D Pull-up / pull-down select
 *
 * Polarity: the PI4IOE5V6408 datasheet defines a HIGH output bit as the
 * released (pulled-up) level, so SW4-8 OFF (the silk-screen "Device"
 * position) corresponds to U15.P7 = 1. The exact bit<->SW4-channel mapping is
 * not in the UM (it is in the Design Package schematic) and is pending
 * on-hardware verification on this EVM. We write 0xFF (all bits HIGH = all
 * SW4 channels in their
 * default OFF position) which puts USB-HS into Device mode and leaves
 * the other muxed peripherals at their EK-RA8D2 default routing.
 *
 * Routes P512 -> SCL1 and P511 -> SDA1 and initializes RIIC channel 1 at
 * 100 kHz.
 *
 * @return ``ra8_err_t`` Error code.
 * @retval k_ra8_ok All three register writes succeeded; U15 is driving
 *                 SW4-8 = OFF (Device mode).
 * @retval k_ra8_err_gpio_conflict P512/P511 already owned.
 * @retval k_ra8_err_hw_init_failed RIIC1 initialization failed.
 * @retval k_ra8_err_nack U15 didn't ACK the register write.
 *
 * @pre IOPORT module powered (reset default).
 * @pre ``ra8_mstp_init`` has run.
 * @post P512/P511 are routed to SCL1/SDA1; RIIC1 is initialized at
 *       100 kHz; U15.P0..P7 are configured as outputs driven HIGH.
 *
 * @note Not thread-safe; call once from the boot context immediately
 *       before ``ra8_board_usbhs_device_init``.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_io_expander_set_usbhs_device_mode(void);

/**
 * @brief Drive the U15 I/O expander to select USB-HS HOST mode.
 *
 * @details
 * Same U15 mechanism as ``ra8_board_io_expander_set_usbhs_device_mode``
 * (see that function for the expander register convention), but writes
 * ``k_ra8_board_pi4ioe_output_usbhs_host`` (0x72): the project-default
 * SW4 layout with bit 7 (SW4-8, USBHS role) driven LOW = ON = Host.
 * In the host position the board's J7 VBUS switch supplies bus power
 * to an attached device; in the default OFF/device position J7 expects
 * VBUS from an external host and an attached USB stick stays dark.
 *
 * @return ``ra8_err_t`` Error code.
 * @retval k_ra8_ok U15 is driving SW4-8 = ON (Host mode).
 * @retval k_ra8_err_gpio_conflict P512/P511 already owned.
 * @retval k_ra8_err_hw_init_failed RIIC1 initialization failed.
 * @retval k_ra8_err_nack U15 didn't ACK a register write.
 *
 * @pre IOPORT module powered (reset default).
 * @pre ``ra8_mstp_init`` has run.
 * @post U15.P0..P7 are outputs driving 0x72; J7 supplies VBUS.
 * @post P512/P511 are routed to SCL1/SDA1 with RIIC1 at 100 kHz.
 *
 * @note Not thread-safe; call once from the boot context before the
 *       USB host bring-up.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_io_expander_set_usbhs_host_mode(void);

/**
 * @brief Bring the chip USBHS module up in device mode (HS PHY).
 *
 * @retval k_ra8_ok / k_ra8_err_not_supported (until USBHS HAL lands)
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_usbhs_device_init(void);

/**
 * @brief Bring the chip USBHS module up in host mode.
 *
 * @retval k_ra8_ok / k_ra8_err_not_supported (until USBHS HAL lands)
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_usbhs_host_init(void);

/**
 * @brief Which of the two USB controllers a call is about.
 *
 * @details
 * The EK-RA8D2 exposes both: the full-speed peripheral on J11 and the
 * high-speed peripheral on J7. They differ in more than speed. FS routes
 * its D+/D- pair through board port pins and straps its role with the
 * P5_00 VBUSEN GPIO; HS keeps its differential pair inside the chip and
 * straps its role with PD07 plus, optionally, the U15 SW4-8 override.
 * ``ra8_board_usb_port_init`` hides that asymmetry.
 */
typedef enum : uint8_t {
  k_ra8_board_usb_port_fs = 0U, /**< J11 full-speed peripheral. UM Table 22 p 30. */
  k_ra8_board_usb_port_hs = 1U, /**< J7 high-speed peripheral. UM Table 28 p 34.  */
} ra8_board_usb_port_t;

/** @brief Which end of the cable the port is meant to be. */
typedef enum : uint8_t {
  k_ra8_board_usb_role_device = 0U, /**< The board enumerates on somebody else's bus. */
  k_ra8_board_usb_role_host   = 1U, /**< The board drives the bus and supplies VBUS.  */
} ra8_board_usb_role_t;

/**
 * @brief Route a USB port's pins and strap it for the role it is to play.
 *
 * @details
 * The eight-step choreography nineteen example apps write out longhand,
 * in one call. For @p port ``fs`` it routes P4_07 VBUS sense, P8_14 D+ and
 * P8_15 D- to ``k_ra8_psel_usb_fs`` and drives the P5_00 VBUSEN GPIO to
 * match the role. That GPIO is the subtle one: it has to be a GPIO driven
 * LOW for device mode, because routing it to the peripheral function
 * instead forces host VBUSEN and the board never enumerates. For @p port
 * ``hs`` it defers to ``ra8_board_usbhs_device_init`` /
 * ``ra8_board_usbhs_host_init``, which already own the PD07-versus-U15
 * subtlety, so no HS logic is restated here.
 *
 * FS bring-up stops at the pins. The FS controller clock is
 * ``ra8_cgc_usbfs_clock_enable`` and stays the caller's call, because
 * apps order it against ``ra8_cgc_init`` differently and some route the
 * pins long before they touch the clock tree. The HS arms do bring their
 * clock up, because the existing helpers they delegate to always have.
 *
 * @param[in] port Which controller. Out-of-range values are refused.
 * @param[in] role Device or host. Out-of-range values are refused.
 *
 * @return ``ra8_err_t`` Error code.
 * @retval k_ra8_ok               Pins routed and the role line strapped.
 * @retval k_ra8_err_invalid_arg  @p port or @p role is not an enumerator.
 * @retval k_ra8_err_gpio_conflict A pin this port needs is already owned.
 *
 * @pre IOPORT is powered (reset default) and ``ra8_mstp_init`` has run.
 * @post On success the port's pins carry their peripheral function and the
 *       role line is driven.
 * @post On failure routing may be partly applied; the port is not usable.
 *
 * @note Not thread-safe; call once per port from the boot context.
 * @see ra8_board_usbhs_device_init
 * @see ra8_board_usbhs_host_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_usb_port_init(ra8_board_usb_port_t port,
                                                ra8_board_usb_role_t role);

#ifdef __cplusplus
}
#endif
