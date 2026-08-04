/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_nsc_cgc.h
 * @brief NSC veneers for the Clock Generation Circuit (CGC) driver
 * @ingroup grp_security
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * Exposes the secure-side CGC primitives needed to bring up the USB-FS
 * 48 MHz reference clock to Non-Secure callers across the TrustZone
 * boundary. The CGC PLL2 / USBCKCR / USBCKDIVCR register block lives
 * in the System Control region which is permanently Secure on the
 * RA8D2 -- writes from a Non-Secure context complete silently and
 * leave the registers in their reset state. The veneers in this file
 * are the only safe path for NS code to request CGC operations.
 *
 * Each entry is a Non-Secure Callable function (``cmse_nonsecure_entry``)
 * that simply forwards to the matching ``ra8_cgc_*`` API, with pointer
 * arguments (where present) range-checked to ensure they live in NS
 * memory before the secure driver dereferences them.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_cgc.h"
#include "ra8_cgc_regs.h"
#include "ra8_err.h"
#include "ra8_nsc_veneer.h"

/**
 * @brief NSC veneer: bring up PLL2 with the given multiplier and divider.
 *
 * @details
 * Forwards to ::ra8_cgc_pll2_enable. PLL2 is required for the USB-FS
 * 48 MHz reference clock; see ::ra8_nsc_cgc_usbfs_clock_enable for the
 * full bring-up sequence.
 *
 * @param[in] mul_int      PLL2 multiplier integer part (1..255).
 * @param[in] mul_quarters PLL2 multiplier quarter part (0..3).
 * @param[in] p_div_code   PLL2 P-divider code (::ra8_plodiv_t).
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok               PLL2 locked.
 * @retval k_ra8_err_invalid_arg  ``mul_int`` is 0 or ``mul_quarters > 3``.
 * @retval k_ra8_err_hw_timeout   PLL2SF handshake timed out.
 *
 * @pre TrustZone substrate up and ::ra8_cgc_init has been called.
 * @pre Single-threaded init context.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. Scalar arguments only.
 *
 * @note Thread-safe: serialised by the secure CGC driver.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_cgc_pll2_enable(uint8_t      mul_int,
                                                               uint8_t      mul_quarters,
                                                               ra8_plodiv_t p_div_code);

/**
 * @brief NSC veneer: bring up the USB-FS module clock (USBCKCR / USBCKDIVCR).
 *
 * @details
 * Forwards to ::ra8_cgc_usbfs_clock_enable. Must run before any NS
 * caller releases MSTPB11 (USBFS), otherwise the SREQ -> SRDY
 * handshake hangs silently. ::ra8_nsc_cgc_pll2_enable must have been
 * called first to lock PLL2 at the canonical 240 MHz target.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok               USB-FS module clock running at 48 MHz.
 * @retval k_ra8_err_hw_timeout   USBCKSRDY handshake never completed.
 *
 * @pre ::ra8_nsc_cgc_pll2_enable returned ::k_ra8_ok.
 * @pre TrustZone substrate up and ::ra8_cgc_init has been called.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. Argument-less call.
 *
 * @note Thread-safe: serialised by the secure CGC driver.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_cgc_usbfs_clock_enable(void);

/**
 * @brief NSC veneer: query a clock-tree frequency.
 *
 * @details
 * Forwards to ::ra8_cgc_get_clock_hz with NS range-validation of the
 * ``hz_out`` destination so the secure driver can never write outside
 * the Non-Secure data region.
 *
 * @param[in]  id     Clock identifier (::ra8_clock_id_t).
 * @param[out] hz_out NS destination for the current frequency in Hz.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok               Frequency stored at ``*hz_out``.
 * @retval k_ra8_err_null_ptr     ``hz_out`` was NULL.
 * @retval k_ra8_err_invalid_arg  ``hz_out`` outside NS region or bad id.
 *
 * @pre TrustZone substrate up and ::ra8_cgc_init has been called.
 * @pre ``hz_out`` lies in NS data region.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. ``hz_out`` is
 *   ``cmse_check_address_range``-validated.
 *
 * @note Thread-safe: serialised by the secure CGC driver.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_cgc_get_clock_hz(ra8_clock_id_t id,
                                                                uint32_t*      hz_out);

#ifdef __cplusplus
}
#endif
