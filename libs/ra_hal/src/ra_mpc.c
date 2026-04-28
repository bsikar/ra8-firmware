/**
 * @file ra_mpc.c
 * @brief Multi-function Pin Controller facade implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * substrate. Owns every write to ``PmnPFS`` and the
 * accompanying PWPR unlock/lock sequence. See ``ra_mpc.h`` for
 * the API contract.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_mpc.h"

#include <stdint.h>

#include "ra8d2_pfs_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "MPC";

/**
 * @brief Bounds-check a ``(port, pin)`` pair.
 *
 * @return ``k_ra_ok`` if both indices are in range, otherwise the
 * appropriate ``k_ra_err_gpio_*`` code.
 */
static ra_err_t internal_check(ra_port_t port, ra_pin_t pin)
{
  if ((uint8_t)port > (uint8_t)k_ra_port_max) {
    return k_ra_err_gpio_invalid_port;
  }
  if ((uint8_t)pin > (uint8_t)k_ra_pin_max) {
    return k_ra_err_gpio_invalid_pin;
  }
  return k_ra_ok;
}

/**
 * @brief Update a single field of PmnPFS under PWPR unlock.
 *
 * @details
 * Read-modify-write the whole 32-bit register with the requested
 * field bits cleared and set. PWPR is unlocked once per call and
 * re-locked unconditionally, including on the no-op path.
 */
static void
internal_update_pfs(ra_port_t port, ra_pin_t pin, uint32_t clear_mask, uint32_t set_mask)
{
  volatile uint32_t* pfs = ra_pfs_pmn(port, pin);
  if (pfs == nullptr) { /* GCOVR_EXCL_BR_LINE -- bounds already validated by caller */
    return;
  }
  ra_pfs_pwpr_unlock();
  /* HUM Ch 20.2 "PmnPFS : Pin Function Select Register", p 837 */
  const uint32_t cur    = *pfs;
  const uint32_t masked = cur & ~clear_mask;
  *pfs                  = masked | set_mask;
  ra_pfs_pwpr_lock();
}

/**
 * @brief All-fields reset + new-fields write under PWPR unlock.
 *
 * @details
 * Used by the "route" and "set_*" helpers that want to take a
 * pin from whatever state it is in and put it in a clean known
 * state. The register is rewritten from 0 with only the new
 * field bits set; any leftover PSEL / ISEL / ASEL / PDR bits are
 * cleared in the same cycle.
 */
static void internal_reset_pfs(ra_port_t port, ra_pin_t pin, uint32_t set_mask)
{
  volatile uint32_t* pfs = ra_pfs_pmn(port, pin);
  if (pfs == nullptr) { /* GCOVR_EXCL_BR_LINE -- bounds already validated by caller */
    return;
  }
  ra_pfs_pwpr_unlock();
  *pfs = set_mask;
  ra_pfs_pwpr_lock();
}

ra_err_t ra_mpc_route_peripheral(ra_port_t port, ra_pin_t pin, ra_mpc_psel_t psel)
{
  const ra_err_t err = internal_check(port, pin);
  if (err != k_ra_ok) {
    return err;
  }
  const uint32_t psel_bits =
    ((uint32_t)psel << (uint8_t)k_ra_pfs_bit_psel0) & (uint32_t)k_ra_pfs_mask_psel;
  const uint32_t pmr = (uint32_t)k_ra_pfs_mask_pmr;
  internal_reset_pfs(port, pin, psel_bits | pmr);
  ra_log_info_val(s_tag, "route", (uint32_t)psel);
  return k_ra_ok;
}

ra_err_t ra_mpc_set_gpio(ra_port_t port, ra_pin_t pin, ra_mpc_dir_t dir)
{
  const ra_err_t err = internal_check(port, pin);
  if (err != k_ra_ok) {
    return err;
  }
  const uint32_t pdr = (dir == k_ra_mpc_dir_output) ? (uint32_t)k_ra_pfs_mask_pdr : 0U;
  internal_reset_pfs(port, pin, pdr);
  return k_ra_ok;
}

ra_err_t ra_mpc_set_analog(ra_port_t port, ra_pin_t pin)
{
  const ra_err_t err = internal_check(port, pin);
  if (err != k_ra_ok) {
    return err;
  }
  internal_reset_pfs(port, pin, (uint32_t)k_ra_pfs_mask_asel);
  return k_ra_ok;
}

ra_err_t ra_mpc_set_irq(ra_port_t port, ra_pin_t pin)
{
  const ra_err_t err = internal_check(port, pin);
  if (err != k_ra_ok) {
    return err;
  }
  internal_reset_pfs(port, pin, (uint32_t)k_ra_pfs_mask_isel);
  return k_ra_ok;
}

ra_err_t ra_mpc_set_pull(ra_port_t port, ra_pin_t pin, ra_mpc_pull_t pull)
{
  const ra_err_t err = internal_check(port, pin);
  if (err != k_ra_ok) {
    return err;
  }
  const uint32_t set = (pull == k_ra_mpc_pull_up) ? (uint32_t)k_ra_pfs_mask_pcr : 0U;
  internal_update_pfs(port, pin, (uint32_t)k_ra_pfs_mask_pcr, set);
  return k_ra_ok;
}

ra_err_t ra_mpc_set_open_drain(ra_port_t port, ra_pin_t pin, bool enable)
{
  const ra_err_t err = internal_check(port, pin);
  if (err != k_ra_ok) {
    return err;
  }
  uint32_t set = 0U;
  if (enable) {
    set = (uint32_t)k_ra_pfs_mask_ncodr;
  }
  internal_update_pfs(port, pin, (uint32_t)k_ra_pfs_mask_ncodr, set);
  return k_ra_ok;
}

ra_err_t ra_mpc_read_pfs(ra_port_t port, ra_pin_t pin, uint32_t* out_val)
{
  RA_CHECK_NULL_PTR(out_val, s_tag, "out_val must not be NULL");
  const ra_err_t err = internal_check(port, pin);
  if (err != k_ra_ok) {
    return err;
  }
  volatile const uint32_t* pfs = ra_pfs_pmn(port, pin);
  if (pfs == nullptr) { /* GCOVR_EXCL_BR_LINE -- bounds already validated */
    return k_ra_err_gpio_invalid_pin;
  }
  *out_val = *pfs;
  return k_ra_ok;
}
