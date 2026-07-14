/**
 * @file ra8_mpc.c
 * @brief Multi-function Pin Controller facade implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Ring 3 / HAL substrate. Owns every write to ``PmnPFS`` and the
 * accompanying PWPR unlock/lock sequence. See ``ra8_mpc.h`` for
 * the API contract.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_mpc.h"

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_pfs_regs.h"

static const char* s_tag = "MPC";

/**
 * @brief Bounds-check a ``(port, pin)`` pair.
 *
 * @return ``k_ra8_ok`` if both indices are in range, otherwise the
 * appropriate ``k_ra8_err_gpio_*`` code.
 *
 * @details See implementation.
 * @param[in] port See implementation.
 * @param[in] pin See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra8_err_t internal_check(ra8_port_t port, ra8_pin_t pin)
{
  if ((uint8_t)port > k_ra8_port_max) {
    return k_ra8_err_gpio_invalid_port;
  }
  if ((uint8_t)pin > k_ra8_pin_max) {
    return k_ra8_err_gpio_invalid_pin;
  }
  return k_ra8_ok;
}

/**
 * @brief Update a single field of PmnPFS under PWPR unlock.
 *
 * @details
 * Read-modify-write the whole 32-bit register with the requested
 * field bits cleared and set. PWPR is unlocked once per call and
 * re-locked unconditionally, including on the no-op path.
 *
 * @param[in] port See implementation.
 * @param[in] pin See implementation.
 * @param[in] clear_mask See implementation.
 * @param[in] set_mask See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void
internal_update_pfs(ra8_port_t port, ra8_pin_t pin, uint32_t clear_mask, uint32_t set_mask)
{
  volatile uint32_t* pfs = ra8_pfs_pmn(port, pin);
  if (pfs == nullptr) { /* GCOVR_EXCL_BR_LINE -- bounds already validated by caller */
    return;
  }
  ra8_pfs_pwpr_unlock();
  /* HUM Ch 20.2 "PmnPFS : Pin Function Select Register", p 837 */
  const uint32_t cur    = *pfs;
  const uint32_t masked = cur & ~clear_mask;
  *pfs                  = masked | set_mask;
  ra8_pfs_pwpr_lock();
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
 *
 * @param[in] port See implementation.
 * @param[in] pin See implementation.
 * @param[in] set_mask See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_reset_pfs(ra8_port_t port, ra8_pin_t pin, uint32_t set_mask)
{
  volatile uint32_t* pfs = ra8_pfs_pmn(port, pin);
  if (pfs == nullptr) { /* GCOVR_EXCL_BR_LINE -- bounds already validated by caller */
    return;
  }
  ra8_pfs_pwpr_unlock();
  *pfs = set_mask;
  ra8_pfs_pwpr_lock();
}

ra8_err_t ra8_mpc_route_peripheral(ra8_port_t port, ra8_pin_t pin, ra8_mpc_psel_t psel)
{
  const ra8_err_t err = internal_check(port, pin);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint32_t psel_bits = ((uint32_t)psel << k_ra8_pfs_bit_psel0) & k_ra8_pfs_mask_psel;
  const uint32_t pmr       = k_ra8_pfs_mask_pmr;

  /* HUM Ch 20.2.4 "Notes on the PmnPFS Register Setting" p 859 -- PMR
   * must be cleared before a new PSEL is programmed. Three-step write:
   *   1. PMR := 0           (return pin to GPIO mode)
   *   2. PFS := new PSEL    (programme function while PMR is still 0)
   *   3. PFS := PSEL | PMR  (hand pin to the peripheral)
   * Single combined write is what FSP r_ioport_pfs_write avoids; doing
   * it would let the pin briefly drive whatever the *previous* PSEL
   * decoded to with PMR already 1.
   * @note PMSAR resets to all-Secure on RA8D2; we hold the unlock
   *       across the whole sequence so no extra PWPR write storms. */
  volatile uint32_t* pfs = ra8_pfs_pmn(port, pin);
  if (pfs == nullptr) { /* GCOVR_EXCL_BR_LINE -- bounds already validated */
    return k_ra8_err_gpio_invalid_pin;
  }
  ra8_pfs_pwpr_unlock();
  /* HUM Ch 20.2.4 "Notes on the PmnPFS Register Setting" p 859 */
  *pfs = 0U;              /* step 1: PMR = 0.      */
  *pfs = psel_bits;       /* step 2: PSEL, PMR=0.  */
  *pfs = psel_bits | pmr; /* step 3: PSEL | PMR=1. */
  ra8_pfs_pwpr_lock();

  ra8_log_info_val(s_tag, "route", (uint32_t)psel);
  return k_ra8_ok;
}

ra8_err_t ra8_mpc_set_gpio(ra8_port_t port, ra8_pin_t pin, ra8_mpc_dir_t dir)
{
  const ra8_err_t err = internal_check(port, pin);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint32_t pdr = (dir == k_ra8_mpc_dir_output) ? k_ra8_pfs_mask_pdr : 0U;
  internal_reset_pfs(port, pin, pdr);
  return k_ra8_ok;
}

ra8_err_t ra8_mpc_set_analog(ra8_port_t port, ra8_pin_t pin)
{
  const ra8_err_t err = internal_check(port, pin);
  if (err != k_ra8_ok) {
    return err;
  }
  internal_reset_pfs(port, pin, k_ra8_pfs_mask_asel);
  return k_ra8_ok;
}

ra8_err_t ra8_mpc_set_irq(ra8_port_t port, ra8_pin_t pin)
{
  const ra8_err_t err = internal_check(port, pin);
  if (err != k_ra8_ok) {
    return err;
  }
  internal_reset_pfs(port, pin, k_ra8_pfs_mask_isel);
  return k_ra8_ok;
}

ra8_err_t ra8_mpc_set_pull(ra8_port_t port, ra8_pin_t pin, ra8_mpc_pull_t pull)
{
  const ra8_err_t err = internal_check(port, pin);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint32_t set = (pull == k_ra8_mpc_pull_up) ? k_ra8_pfs_mask_pcr : 0U;
  internal_update_pfs(port, pin, k_ra8_pfs_mask_pcr, set);
  return k_ra8_ok;
}

ra8_err_t ra8_mpc_set_open_drain(ra8_port_t port, ra8_pin_t pin, bool enable)
{
  const ra8_err_t err = internal_check(port, pin);
  if (err != k_ra8_ok) {
    return err;
  }
  uint32_t set = 0U;
  if (enable) {
    set = k_ra8_pfs_mask_ncodr;
  }
  internal_update_pfs(port, pin, k_ra8_pfs_mask_ncodr, set);
  return k_ra8_ok;
}

ra8_err_t ra8_mpc_read_pfs(ra8_port_t port, ra8_pin_t pin, uint32_t* out_val)
{
  RA8_CHECK_NULL_PTR(out_val, s_tag, "out_val must not be NULL");
  const ra8_err_t err = internal_check(port, pin);
  if (err != k_ra8_ok) {
    return err;
  }
  volatile const uint32_t* pfs = ra8_pfs_pmn(port, pin);
  if (pfs == nullptr) { /* GCOVR_EXCL_BR_LINE -- bounds already validated */
    return k_ra8_err_gpio_invalid_pin;
  }
  *out_val = *pfs;
  return k_ra8_ok;
}
