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
#include "ra_mstp.h"

static const char* s_tag = "USB";

static volatile r_usb_regs_t* internal_pick(ra_usb_speed_t speed)
{
  return (speed == k_ra_usb_speed_hs) ? ra_usb_hs() : ra_usb_fs();
}

ra_err_t ra_usb_device_init(ra_usb_speed_t speed)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  RA_CHECK_NULL_PTR(reg, s_tag, "speed out of range");

  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B", p 444 */
  const ra_mstp_t mstp_id = (speed == k_ra_usb_speed_hs) ? k_ra_mstp_usbhs : k_ra_mstp_usbfs;
  const ra_err_t  mst_err = ra_mstp_enable(mstp_id);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "usb_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

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

/* =============================================================================
 * Wave 6.2 -- lifecycle + status + IRQ + power
 * =============================================================================
 */

static ra_usb_event_fn_t s_usb_fn;
static void*             s_usb_ctx;

static ra_mstp_t internal_mstp(ra_usb_speed_t speed)
{
  return (speed == k_ra_usb_speed_hs) ? k_ra_mstp_usbhs : k_ra_mstp_usbfs;
}

ra_err_t ra_usb_device_deinit(ra_usb_speed_t speed)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  RA_CHECK_NULL_PTR(reg, s_tag, "speed out of range");

  reg->SYSCFG  = 0U;
  reg->INTENB0 = 0U;
  reg->INTENB1 = 0U;
  return ra_mstp_disable(internal_mstp(speed));
}

ra_err_t ra_usb_get_status(ra_usb_speed_t speed, uint16_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  volatile r_usb_regs_t* reg = internal_pick(speed);
  RA_CHECK_NULL_PTR(reg, s_tag, "speed out of range");
  *out_mask = reg->INTSTS0;
  return k_ra_ok;
}

ra_err_t ra_usb_clear_status(ra_usb_speed_t speed, uint16_t mask)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  RA_CHECK_NULL_PTR(reg, s_tag, "speed out of range");
  reg->INTSTS0 = (uint16_t)(reg->INTSTS0 & (uint16_t)~mask);
  return k_ra_ok;
}

ra_err_t ra_usb_attach_handler(ra_usb_event_fn_t fn, void* ctx)
{
  s_usb_fn  = fn;
  s_usb_ctx = ctx;
  return k_ra_ok;
}

void ra_usb_dispatch(ra_usb_speed_t speed)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) { /* GCOVR_EXCL_BR_LINE -- FS/HS always valid */
    return;             /* GCOVR_EXCL_LINE */
  }
  const uint16_t          mask = reg->INTSTS0;
  const ra_usb_event_fn_t fn   = s_usb_fn;
  void* const             ctx  = s_usb_ctx;
  reg->INTSTS0                 = 0U;
  if (fn != nullptr) {
    fn(ctx, speed, mask);
  }
}

ra_err_t ra_usb_enter_stop(ra_usb_speed_t speed)
{
  if (speed > k_ra_usb_speed_hs) {
    return k_ra_err_invalid_arg;
  }
  return ra_mstp_disable(internal_mstp(speed));
}

ra_err_t ra_usb_exit_stop(ra_usb_speed_t speed)
{
  if (speed > k_ra_usb_speed_hs) {
    return k_ra_err_invalid_arg;
  }
  return ra_mstp_enable(internal_mstp(speed));
}
