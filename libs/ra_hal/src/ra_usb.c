/**
 * @file ra_usb.c
 * @brief USB driver framework
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_usb.h"

#include <stdint.h>

#include "ra8d2_usb_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "USB";

static volatile r_usb_regs_t* internal_pick(ra_usb_speed_t speed)
{
  return (speed == k_ra_usb_speed_hs) ? ra_usb_hs() : ra_usb_fs();
}

ra_err_t ra_usb_device_init(ra_usb_speed_t speed)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  RA_CHECK_NULL_PTR(reg, s_tag, "speed out of range");

  /* Enable the module clock + function in device mode. */
  uint16_t syscfg = (uint16_t)(1U << k_ra_syscfg_bit_scke);
  syscfg |= (uint16_t)(1U << k_ra_syscfg_bit_usbe);
  if (speed == k_ra_usb_speed_hs) {
    syscfg |= (uint16_t)(1U << k_ra_syscfg_bit_hse);
  }
  reg->SYSCFG = syscfg;

  /* Default control pipe: max packet size 64, enabled. */
  enum : uint16_t { k_ra_dcp_max_packet = 64U };
  reg->DCPCFG  = 0U;
  reg->DCPMAXP = k_ra_dcp_max_packet;
  reg->DCPCTR  = 0U;

  /* Enable the VBUS IRQ and bus-reset IRQ for later. */
  reg->INTENB0 = 0U;
  reg->INTENB1 = 0U;

  ra_log_info_val(s_tag, "usb device init speed", (uint32_t)speed);
  return k_ra_ok;
}

ra_err_t ra_usb_device_attach(ra_usb_speed_t speed, bool attached)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  RA_CHECK_NULL_PTR(reg, s_tag, "speed out of range");

  uint16_t syscfg = reg->SYSCFG;
  if (attached) {
    syscfg |= (uint16_t)(1U << k_ra_syscfg_bit_dprpu);
  } else {
    syscfg &= (uint16_t)~(1U << k_ra_syscfg_bit_dprpu);
  }
  reg->SYSCFG = syscfg;
  return k_ra_ok;
}
