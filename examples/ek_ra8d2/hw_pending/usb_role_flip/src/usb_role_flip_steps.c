/**
 * @file usb_role_flip_steps.c
 * @brief Implementation of runtime USB role swap choreography.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Drives port routing reconfiguration and driver initialization/teardown
 * across two phases:
 *  - Config A: FS Device (J11) + HS Host (J7)
 *  - Config B: FS Host (J11) + HS Device (J7)
 *
 * Exercises ux_dcd_ra8_usb_uninitialize, ra8_usb_host_deinit, and ra8_usb_pal_deinit
 * to prove that neither controller leaks state or blocks role reversal.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "usb_role_flip.h"

#include <stdbool.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_pin_validator.h"
#include "ra8_usb.h"
#include "ra8_usb_host.h"
#include "ra8_usb_pal.h"

#ifndef RA8_OFF_TARGET
#include "ux_api.h"
#include "ux_dcd_ra8_usb.h"
#else
/* Host test mock declarations matching port/usbx */
extern ra8_err_t ux_dcd_ra8_usb_initialize(ra8_usb_speed_t speed);
extern ra8_err_t ux_dcd_ra8_usb_uninitialize(void);
#endif

/**
 * @brief Teardown wrapper for host controller driver.
 *
 * @param[in] speed Which controller (FS or HS).
 * @return ra8_err_t outcome.
 */
static ra8_err_t ux_hcd_ra_usb_uninitialize(ra8_usb_speed_t speed)
{
  return ra8_usb_host_deinit(speed);
}

/**
 * @brief Release USB-FS and USB-HS port pins so the alternate configuration can bind them.
 */
static void usb_role_flip_release_pins(void)
{
  (void)ra8_pin_validator_release((ra8_port_pin_t)k_ra8_board_usbfs_pin_vbus);
  (void)ra8_pin_validator_release((ra8_port_pin_t)k_ra8_board_usbfs_pin_dp);
  (void)ra8_pin_validator_release((ra8_port_pin_t)k_ra8_board_usbfs_pin_dm);
  (void)ra8_pin_validator_release((ra8_port_pin_t)k_ra8_board_usbfs_pin_vbusen);
  (void)ra8_pin_validator_release((ra8_port_pin_t)k_ra8_board_usbhs_pin_pwr);
}

ra8_err_t usb_role_flip_setup_cfg_a(void)
{
  ra8_err_t err = ra8_board_usb_port_init(k_ra8_board_usb_port_fs, k_ra8_board_usb_role_device);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_board_usb_port_init(k_ra8_board_usb_port_hs, k_ra8_board_usb_role_host);
  if (err != k_ra8_ok) {
    return err;
  }

  err = ra8_usb_pal_init(k_ra8_usb_speed_fs);
  if (err != k_ra8_ok) {
    return err;
  }

  err = ux_dcd_ra8_usb_initialize(k_ra8_usb_speed_fs);
  if (err != k_ra8_ok) {
    (void)ra8_usb_pal_deinit();
    return err;
  }

  err = ra8_usb_host_init(k_ra8_usb_speed_hs);
  if (err != k_ra8_ok) {
    (void)ux_dcd_ra8_usb_uninitialize();
    (void)ra8_usb_pal_deinit();
    return err;
  }

  (void)ra8_usb_device_attach(k_ra8_usb_speed_fs, true);
  (void)ra8_usb_pal_attach(true);
  return k_ra8_ok;
}

ra8_err_t usb_role_flip_teardown_cfg_a(void)
{
  (void)ra8_usb_pal_attach(false);
  (void)ra8_usb_device_attach(k_ra8_usb_speed_fs, false);

  const ra8_err_t dcd_err = ux_dcd_ra8_usb_uninitialize();
  if (dcd_err != k_ra8_ok) {
    return dcd_err;
  }

  const ra8_err_t pal_err = ra8_usb_pal_deinit();
  if (pal_err != k_ra8_ok) {
    return pal_err;
  }

  const ra8_err_t hcd_err = ux_hcd_ra_usb_uninitialize(k_ra8_usb_speed_hs);
  if (hcd_err != k_ra8_ok) {
    return hcd_err;
  }

  usb_role_flip_release_pins();
  return k_ra8_ok;
}

ra8_err_t usb_role_flip_setup_cfg_b(void)
{
  ra8_err_t err = ra8_board_usb_port_init(k_ra8_board_usb_port_fs, k_ra8_board_usb_role_host);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_board_usb_port_init(k_ra8_board_usb_port_hs, k_ra8_board_usb_role_device);
  if (err != k_ra8_ok) {
    return err;
  }

  err = ra8_usb_pal_init(k_ra8_usb_speed_hs);
  if (err != k_ra8_ok) {
    return err;
  }

  err = ux_dcd_ra8_usb_initialize(k_ra8_usb_speed_hs);
  if (err != k_ra8_ok) {
    (void)ra8_usb_pal_deinit();
    return err;
  }

  err = ra8_usb_host_init(k_ra8_usb_speed_fs);
  if (err != k_ra8_ok) {
    (void)ux_dcd_ra8_usb_uninitialize();
    (void)ra8_usb_pal_deinit();
    return err;
  }

  (void)ra8_usb_device_attach(k_ra8_usb_speed_hs, true);
  (void)ra8_usb_pal_attach(true);
  return k_ra8_ok;
}

ra8_err_t usb_role_flip_teardown_cfg_b(void)
{
  (void)ra8_usb_pal_attach(false);
  (void)ra8_usb_device_attach(k_ra8_usb_speed_hs, false);

  const ra8_err_t dcd_err = ux_dcd_ra8_usb_uninitialize();
  if (dcd_err != k_ra8_ok) {
    return dcd_err;
  }

  const ra8_err_t pal_err = ra8_usb_pal_deinit();
  if (pal_err != k_ra8_ok) {
    return pal_err;
  }

  const ra8_err_t hcd_err = ux_hcd_ra_usb_uninitialize(k_ra8_usb_speed_fs);
  if (hcd_err != k_ra8_ok) {
    return hcd_err;
  }

  usb_role_flip_release_pins();
  return k_ra8_ok;
}

ra8_err_t usb_role_flip_run_cycle(void)
{
  /* Phase 1: FS Device + HS Host */
  ra8_err_t err = usb_role_flip_setup_cfg_a();
  if (err != k_ra8_ok) {
    return err;
  }
  err = usb_role_flip_teardown_cfg_a();
  if (err != k_ra8_ok) {
    return err;
  }

  /* Phase 2: Role flip -> FS Host + HS Device */
  err = usb_role_flip_setup_cfg_b();
  if (err != k_ra8_ok) {
    return err;
  }
  err = usb_role_flip_teardown_cfg_b();
  if (err != k_ra8_ok) {
    return err;
  }

  return k_ra8_ok;
}
