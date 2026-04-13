/**
 * @file ra_usb.c
 * @brief USB 2.0 Full-Speed + High-Speed driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Wave 6 driver for the RA8D2 USB peripherals. Both speeds
 * (USBFS at 0x40250000, USBHS at 0x40351000) share the same
 * SYSCFG / INTSTS0 / INTENB0 / INTENB1 / DCPCFG / DCPMAXP /
 * DCPCTR register layout, so the driver multiplexes them via a
 * ``ra_usb_speed_t`` parameter and ``internal_pick(speed)``
 * picks the right base. This file therefore satisfies BOTH the
 * ``ra_usb_fs`` (HUM Ch 36) and ``ra_usb_hs`` (HUM Ch 37) ROADMAP
 * entries -- the per-instance ROADMAP rows just point here.
 *
 * Surface: device-mode init, attach (D+ pull-up), deinit, status
 * get/clear, IRQ dispatch, power transition. Full pipe / endpoint
 * descriptor wiring + DMA delivery is deferred to the first
 * USB-stack consumer (CherryUSB or similar). Every register
 * access carries a HUM Ch 36 / Ch 37 citation depending on the
 * speed it applies to.
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

  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B" p 444 */
  const ra_mstp_t mstp_id = (speed == k_ra_usb_speed_hs) ? k_ra_mstp_usbhs : k_ra_mstp_usbfs;
  const ra_err_t  mst_err = ra_mstp_enable(mstp_id);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "usb_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 36 "USB 2.0 Full-Speed Module (USBFS)" p 1965 */
  /* HUM Ch 37 "USB 2.0 High-Speed Module (USBHS)" p 2059 */
  /* SYSCFG.SCKE + USBE + (HSE for HS) gates module clock + enables. */
  uint16_t syscfg = (uint16_t)(1U << k_ra_syscfg_bit_scke);
  syscfg |= (uint16_t)(1U << k_ra_syscfg_bit_usbe);
  if (speed == k_ra_usb_speed_hs) {
    syscfg |= (uint16_t)(1U << k_ra_syscfg_bit_hse);
  }
  reg->SYSCFG = syscfg;

  /* HUM Ch 36 "USB 2.0 Full-Speed Module (USBFS)" p 1965 */
  /* HUM Ch 37 "USB 2.0 High-Speed Module (USBHS)" p 2059 */
  /* Default control pipe: max packet 64, enabled. */
  enum : uint16_t { k_ra_dcp_max_packet = 64U };
  reg->DCPCFG  = 0U;
  reg->DCPMAXP = k_ra_dcp_max_packet;
  reg->DCPCTR  = 0U;

  /* HUM Ch 36 "USB 2.0 Full-Speed Module (USBFS)" p 1965 */
  /* HUM Ch 37 "USB 2.0 High-Speed Module (USBHS)" p 2059 */
  /* IRQ enables stay clear until the application opts in. */
  reg->INTENB0 = 0U;
  reg->INTENB1 = 0U;

  ra_log_info_val(s_tag, "usb device init speed", (uint32_t)speed);
  return k_ra_ok;
}

ra_err_t ra_usb_device_attach(ra_usb_speed_t speed, bool attached)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  RA_CHECK_NULL_PTR(reg, s_tag, "speed out of range");

  /* HUM Ch 36 "USB 2.0 Full-Speed Module (USBFS)" p 1965 */
  /* HUM Ch 37 "USB 2.0 High-Speed Module (USBHS)" p 2059 */
  /* SYSCFG.DPRPU enables the D+ pull-up so the host sees attach. */
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

  /* HUM Ch 36 "USB 2.0 Full-Speed Module (USBFS)" p 1965 */
  /* HUM Ch 37 "USB 2.0 High-Speed Module (USBHS)" p 2059 */
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
  /* HUM Ch 36 "USB 2.0 Full-Speed Module (USBFS)" p 1965 */
  /* HUM Ch 37 "USB 2.0 High-Speed Module (USBHS)" p 2059 */
  *out_mask = reg->INTSTS0;
  return k_ra_ok;
}

ra_err_t ra_usb_clear_status(ra_usb_speed_t speed, uint16_t mask)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  RA_CHECK_NULL_PTR(reg, s_tag, "speed out of range");
  /* HUM Ch 36 "USB 2.0 Full-Speed Module (USBFS)" p 1965 */
  /* HUM Ch 37 "USB 2.0 High-Speed Module (USBHS)" p 2059 */
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
