/**
 * @file ra8_board_ek_ra8d2_usb_port.c
 * @brief EK-RA8D2 BSP -- USB port routing and role strapping for J11 and J7.
 * @ingroup grp_board
 * @details Holds the pin choreography nineteen example apps wrote out
 *          longhand in their own ``*_route_usb_or_halt`` helpers. The
 *          full-speed arm is implemented here; the high-speed arms defer
 *          to the existing ``ra8_board_usbhs_device_init`` /
 *          ``ra8_board_usbhs_host_init``, so the PD07-versus-U15 role
 *          subtlety stays in the one place that already documents it.
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2_peripherals.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_port_utils.h"

/**
 * @brief Route the three USB-FS peripheral pins on J11.
 *
 * @details D+/D- carry the differential pair and P4_07 senses VBUS. All
 *          three take ``k_ra8_psel_usb_fs``; the fourth FS pin, VBUSEN,
 *          is deliberately not routed here because it has to stay a GPIO
 *          (see ::internal_usbfs_strap_role).
 *
 * @return ra8_err_t First routing failure, or ``k_ra8_ok``.
 * @retval k_ra8_ok Three pins carry the USBFS peripheral function.
 * @retval k_ra8_err_gpio_conflict A pin is already owned by another driver.
 *
 * @pre IOPORT is powered.
 * @post On failure the pins routed before it keep their new function.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_usbfs_route_pins(void)
{
  ra8_err_t err = ra8_pfs_route_peripheral((ra8_port_pin_t)k_ra8_board_usbfs_pin_vbus,
                                           k_ra8_psel_usb_fs,
                                           "board.usbfs.vbus");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral((ra8_port_pin_t)k_ra8_board_usbfs_pin_dp,
                                 k_ra8_psel_usb_fs,
                                 "board.usbfs.dp");
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_pfs_route_peripheral((ra8_port_pin_t)k_ra8_board_usbfs_pin_dm,
                                  k_ra8_psel_usb_fs,
                                  "board.usbfs.dm");
}

/**
 * @brief Drive P5_00 VBUSEN as a GPIO to strap the full-speed role.
 *
 * @details This is the board fact worth having in one place. VBUSEN must
 *          stay a GPIO: routing it to the USBFS peripheral function makes
 *          the controller drive it as host VBUSEN, and device enumeration
 *          never completes. LOW is device, HIGH supplies 5 V for host.
 *
 * @param[in] role Which end of the cable this port is meant to be.
 *
 * @return ra8_err_t Result of the GPIO init.
 * @retval k_ra8_ok The role line is driven.
 * @retval k_ra8_err_gpio_conflict P5_00 is already owned.
 *
 * @pre IOPORT is powered.
 * @post P5_00 is a push-pull output at the level the role implies.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_usbfs_strap_role(ra8_board_usb_role_t role)
{
  const ra8_level_t level =
    (role == k_ra8_board_usb_role_host) ? k_ra8_level_high : k_ra8_level_low;
  return ra8_gpio_output_init((ra8_port_pin_t)k_ra8_board_usbfs_pin_vbusen, level);
}

ra8_err_t ra8_board_usbhs_pwr_set(bool on)
{
  const ra8_level_t level = on ? k_ra8_level_high : k_ra8_level_low;
  return ra8_gpio_output_init((ra8_port_pin_t)k_ra8_board_usbhs_pin_pwr, level);
}

ra8_err_t ra8_board_usb_port_init(ra8_board_usb_port_t port, ra8_board_usb_role_t role)
{
  if ((role != k_ra8_board_usb_role_device) && (role != k_ra8_board_usb_role_host)) {
    return k_ra8_err_invalid_arg;
  }

  if (port == k_ra8_board_usb_port_fs) {
    const ra8_err_t err = internal_usbfs_route_pins();
    if (err != k_ra8_ok) {
      return err;
    }
    return internal_usbfs_strap_role(role);
  }

  if (port == k_ra8_board_usb_port_hs) {
    return (role == k_ra8_board_usb_role_host) ? ra8_board_usbhs_host_init()
                                               : ra8_board_usbhs_device_init();
  }

  return k_ra8_err_invalid_arg;
}
