/**
 * @file ra8_nsc_lpm.c
 * @brief NSC veneers: Secure ULPT0-timed deep-idle / Software Standby gateway
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * Wraps the Secure-only LPM / PRCR / WUPEN / IELSR / ULPT0 standby sequence so
 * Non-Secure firmware (such as the e-reader system idle path) can enter timed
 * Software Standby and recover cleanly on ULPT0 underflow without direct access
 * to Secure SYSC or ICU registers.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_lpm.h"
#include "ra8_nsc.h"
#include "ra8_nsc_veneer.h"

#ifdef RA8_APP_UART_LOG
#include "ra8_board_ek_ra8d2.h"
#endif

static const char* const s_tag = "NSCLPM";

static bool     s_nsc_lpm_ready      = false;
static uint32_t s_nsc_lpm_wake_count = 0U;

RA8_INTERNAL static void internal_nsc_lpm_flush_console(void)
{
#ifdef RA8_APP_UART_LOG
  /* Drain SCI8 before Software Standby gates PCLKA so prior logs do not clip. */
  (void)ra8_board_uart_console_flush();
#endif
}

/** @brief Implementation of `ra8_nsc_lpm_idle_prepare()` -- Secure LPM + ULPT0 bring-up. */
RA8_NSC_VENEER ra8_err_t ra8_nsc_lpm_idle_prepare(void)
{
  if (s_nsc_lpm_ready) {
    return k_ra8_ok;
  }

  const ra8_lpm_config_t cfg = {
    .io_port_keep     = false,
    .opa_bus_keep     = true,
    .sscr_fast_return = false,
    .dcdc_softstart   = k_ra8_lpm_dcssmode_128us,
    .sscr_low_power   = k_ra8_lpm_ss2lp_default,
  };

  ra8_err_t err = ra8_lpm_prcr_unlock();
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_err_t init_err   = ra8_lpm_init(&cfg);
  const ra8_err_t relock_err = ra8_lpm_prcr_relock();
  if (init_err != k_ra8_ok) {
    return init_err;
  }
  if (relock_err != k_ra8_ok) {
    return relock_err;
  }

  err = ra8_lpm_ulpt_wake_prepare(nullptr, nullptr, (uint8_t)k_ra8_lpm_ulpt_default_isr_prio);
  if (err != k_ra8_ok) {
    return err;
  }

  s_nsc_lpm_ready = true;
  return k_ra8_ok;
}

/** @brief Implementation of `ra8_nsc_lpm_standby_ms()` -- ULPT0-timed Software Standby. */
RA8_NSC_VENEER ra8_err_t ra8_nsc_lpm_standby_ms(uint32_t period_ms)
{
  uint32_t  ticks = 0U;
  ra8_err_t err   = ra8_lpm_ulpt_ms_to_ticks(period_ms, &ticks);
  if (err != k_ra8_ok) {
    return err;
  }

  if (!s_nsc_lpm_ready) {
    err = ra8_nsc_lpm_idle_prepare();
    if (err != k_ra8_ok) {
      return err;
    }
  }

  internal_nsc_lpm_flush_console();

  err = ra8_lpm_ulpt_standby_ticks(ticks);
  if (err != k_ra8_ok) {
    return err;
  }

  s_nsc_lpm_wake_count++;
  return k_ra8_ok;
}

/** @brief Implementation of `ra8_nsc_lpm_idle_disarm()` -- clear ULPT0 standby route. */
RA8_NSC_VENEER ra8_err_t ra8_nsc_lpm_idle_disarm(void)
{
  s_nsc_lpm_ready = false;
  return ra8_lpm_ulpt_wake_disarm();
}

/** @brief Implementation of `ra8_nsc_lpm_get_wake_count()` -- export wake counter to NS RAM. */
RA8_NSC_VENEER ra8_err_t ra8_nsc_lpm_get_wake_count(uint32_t* out_count)
{
  RA8_CHECK_NULL_PTR(out_count, s_tag, "lpm_get_wake_count: out_count");
  RA8_NSC_CHECK_NS_RANGE_RW(out_count, sizeof(*out_count));

  *out_count = s_nsc_lpm_wake_count;
  return k_ra8_ok;
}
