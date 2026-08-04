/**
 * @file ra8_nsc_cgc.c
 * @brief NSC veneers for the Clock Generation Circuit driver
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * Implements the Non-Secure Callable veneers declared in
 * ``ra8_nsc_cgc.h``. Each entry validates pointer arguments (when
 * present) and forwards to the secure ``ra8_cgc_*`` API. The CGC
 * register block (System Control) is permanently Secure on the
 * RA8D2; this file is the only TZ-safe path for Non-Secure code to
 * request clock-tree changes.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_nsc_cgc.h"

#include <stdint.h>

#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_nsc_veneer.h"

/**
 * @var s_tag
 * @brief Logging tag for the CGC veneers.
 * @note File-scope only.
 * @since 0.1.0
 */
static const char* s_tag = "NSCCGC";

/**
 * @brief NSC veneer: bring up PLL2 with the given multiplier and divider.
 *
 * @details Forwards to ::ra8_cgc_pll2_enable. No pointer arguments
 *   crossing the TZ boundary, so no NS range-check is needed.
 *
 * @param[in] mul_int      PLL2 multiplier integer part.
 * @param[in] mul_quarters PLL2 multiplier quarter part.
 * @param[in] p_div_code   PLL2 P-divider code.
 *
 * @return ::ra8_err_t from ::ra8_cgc_pll2_enable.
 * @retval k_ra8_ok               PLL2 locked.
 * @retval k_ra8_err_invalid_arg  Bad multiplier.
 * @retval k_ra8_err_hw_timeout   PLL2SF handshake timed out.
 *
 * @pre TrustZone substrate up.
 * @pre ::ra8_cgc_init has been called.
 * @post On success PLL2 is locked.
 * @post On failure no register state was mutated.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. Scalar arguments only.
 *
 * @note Thread-safe: serialised by the secure CGC driver.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_cgc_pll2_enable(uint8_t      mul_int,
                                                 uint8_t      mul_quarters,
                                                 ra8_plodiv_t p_div_code)
{
  return ra8_cgc_pll2_enable(mul_int, mul_quarters, p_div_code);
}

/**
 * @brief NSC veneer: bring up the USB-FS module clock.
 *
 * @details Forwards to ::ra8_cgc_usbfs_clock_enable.
 *
 * @return ::ra8_err_t from ::ra8_cgc_usbfs_clock_enable.
 * @retval k_ra8_ok               USB-FS clock running.
 * @retval k_ra8_err_hw_timeout   USBCKSRDY never came up.
 *
 * @pre TrustZone substrate up.
 * @pre PLL2 has been locked via ::ra8_nsc_cgc_pll2_enable.
 * @post On success USBCKCR is sourced from PLL2P at 48 MHz.
 * @post On failure no register state was mutated.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. Argument-less call.
 *
 * @note Thread-safe: serialised by the secure CGC driver.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_cgc_usbfs_clock_enable(void)
{
  return ra8_cgc_usbfs_clock_enable();
}

/**
 * @brief NSC veneer: query a clock-tree frequency.
 *
 * @details Range-checks ``hz_out`` against the NS data region and
 *   forwards to ::ra8_cgc_get_clock_hz.
 *
 * @param[in]  id     Clock identifier.
 * @param[out] hz_out NS destination for the frequency in Hz.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok               Frequency stored.
 * @retval k_ra8_err_null_ptr     ``hz_out`` was NULL.
 * @retval k_ra8_err_invalid_arg  ``hz_out`` outside NS region or bad id.
 *
 * @pre TrustZone substrate up.
 * @pre ``hz_out`` lies in NS data region.
 * @post On success ``*hz_out`` holds the frequency.
 * @post On failure ``*hz_out`` unchanged.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. ``hz_out`` is
 *   ``cmse_check_address_range``-validated.
 *
 * @note Thread-safe: serialised by the secure CGC driver.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_cgc_get_clock_hz(ra8_clock_id_t id, uint32_t* hz_out)
{
  RA8_CHECK_NULL_PTR(hz_out, s_tag, "cgc_get_clock_hz: hz_out");
  RA8_NSC_CHECK_NS_RANGE_RW(hz_out, sizeof(*hz_out));
  return ra8_cgc_get_clock_hz(id, hz_out);
}
