/**
 * @file gpio.c
 * @brief High-level GPIO driver on top of PORT + PFS
 *
 * @details
 * Implementation of the `ra_gpio_*` helpers declared in
 * `ra_port_utils.h`. Each public function validates arguments, claims
 * the pin (for init calls), unlocks PWPR, programmes the PFS entry,
 * re-locks PWPR, and then uses the PORT atomic set/reset registers
 * to drive or read the pin.
 *
 * ## Why PFS and not PCNTR1 bitfields
 *
 * On RA-family chips the pin mux (PSEL), drive strength, pull-up,
 * open-drain and analog-mux *all* live in the 32-bit `PmnPFS`
 * register, and the per-port PCNTR1 only exposes the direction +
 * output-latch bits. For the direction/level setup to be consistent
 * with any later peripheral routing we do all of it through PFS.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_port_utils.h"

#include <stdint.h>

#include "ra8d2_pfs_regs.h"
#include "ra8d2_port_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_pin_validator.h"
#include "ra_port_constants.h"

static const char* s_tag = "GPIO";

/**
 * @brief Claim the pin in the validator and return the port / pin
 *        indices if the claim succeeded.
 */
static ra_err_t internal_claim(ra_port_pin_t pin, ra_port_t* out_port, ra_pin_t* out_pin)
{
  const ra_port_t p = RA_PIN_PORT(pin);
  const ra_pin_t  b = RA_PIN_PIN(pin);
  if ((uint8_t)p > (uint8_t)k_ra_port_max) {
    return k_ra_err_gpio_invalid_port;
  }
  if ((uint8_t)b > (uint8_t)k_ra_pin_max) {
    return k_ra_err_gpio_invalid_pin;
  }
  const ra_err_t err = ra_pin_validator_claim(pin, s_tag);
  if (err != k_ra_ok) {
    return err;
  }
  *out_port = p;
  *out_pin  = b;
  return k_ra_ok;
}

ra_err_t ra_gpio_output_init(ra_port_pin_t pin, ra_level_t init_level)
{
  ra_port_t port = k_ra_port_0;
  ra_pin_t  bit  = k_ra_pin_0;

  ra_err_t err = internal_claim(pin, &port, &bit);
  if (err != k_ra_ok) {
    ra_log_error_val(s_tag, "claim failed", (uint32_t)err);
    return err;
  }

  /* Programme PFS: PMR=0 (GPIO), PDR=1 (output), PODR=init level. */
  volatile uint32_t* pfs = ra_pfs_pmn(port, bit);
  if (pfs == nullptr) {
    return k_ra_err_hw_unmapped;
  }

  const uint32_t new_val =
    (uint32_t)((init_level == k_ra_level_high) ? k_ra_pfs_mask_podr : 0U) |
    (uint32_t)k_ra_pfs_mask_pdr;

  ra_pfs_pwpr_unlock();
  *pfs = new_val;
  ra_pfs_pwpr_lock();

  ra_log_info_val(s_tag, "output init pin", (uint32_t)pin);
  return k_ra_ok;
}

ra_err_t ra_gpio_input_init(ra_port_pin_t pin, ra_pin_pull_t pull)
{
  ra_port_t port = k_ra_port_0;
  ra_pin_t  bit  = k_ra_pin_0;

  ra_err_t err = internal_claim(pin, &port, &bit);
  if (err != k_ra_ok) {
    ra_log_error_val(s_tag, "claim failed", (uint32_t)err);
    return err;
  }

  volatile uint32_t* pfs = ra_pfs_pmn(port, bit);
  if (pfs == nullptr) {
    return k_ra_err_hw_unmapped;
  }

  /* PMR=0 (GPIO), PDR=0 (input), PCR according to pull. */
  uint32_t new_val = 0U;
  if (pull == k_ra_pull_up) {
    new_val |= (uint32_t)k_ra_pfs_mask_pcr;
  }

  ra_pfs_pwpr_unlock();
  *pfs = new_val;
  ra_pfs_pwpr_lock();

  ra_log_info_val(s_tag, "input init pin", (uint32_t)pin);
  return k_ra_ok;
}

ra_err_t ra_gpio_write(ra_port_pin_t pin, ra_level_t level)
{
  const ra_port_t port = RA_PIN_PORT(pin);
  const ra_pin_t  bit  = RA_PIN_PIN(pin);
  if ((uint8_t)port > (uint8_t)k_ra_port_max) {
    return k_ra_err_gpio_invalid_port;
  }
  if ((uint8_t)bit > (uint8_t)k_ra_pin_max) {
    return k_ra_err_gpio_invalid_pin;
  }

  volatile r_port_regs_t* port_regs = ra_port(port);
  if (port_regs == nullptr) {
    return k_ra_err_hw_unmapped;
  }

  /* PCNTR3 high half = PORR (clear), low half = POSR (set). Writing
   * a 1 to the relevant half drives the pin; writing 0 leaves other
   * pins alone -- race-free. */
  const uint32_t bit_mask = (uint32_t)(1UL << (uint32_t)bit);
  if (level == k_ra_level_high) {
    port_regs->PCNTR3 = bit_mask; /* POSR in low half. */
  } else {
    port_regs->PCNTR3 = bit_mask << (uint32_t)k_ra_pcntr_high_half_shift;
  }
  return k_ra_ok;
}

ra_err_t ra_gpio_toggle(ra_port_pin_t pin)
{
  const ra_port_t port = RA_PIN_PORT(pin);
  const ra_pin_t  bit  = RA_PIN_PIN(pin);
  if ((uint8_t)port > (uint8_t)k_ra_port_max) {
    return k_ra_err_gpio_invalid_port;
  }
  if ((uint8_t)bit > (uint8_t)k_ra_pin_max) {
    return k_ra_err_gpio_invalid_pin;
  }

  volatile r_port_regs_t* port_regs = ra_port(port);
  if (port_regs == nullptr) {
    return k_ra_err_hw_unmapped;
  }

  /* Read PODR (high half of PCNTR1) and invert the bit. */
  const uint32_t pcntr1_val = port_regs->PCNTR1;
  const uint32_t podr       = (pcntr1_val >> (uint32_t)k_ra_pcntr_high_half_shift);
  const uint32_t bit_mask   = (uint32_t)(1UL << (uint32_t)bit);
  if ((podr & bit_mask) != 0U) {
    port_regs->PCNTR3 = bit_mask << (uint32_t)k_ra_pcntr_high_half_shift; /* PORR clear */
  } else {
    port_regs->PCNTR3 = bit_mask;                                         /* POSR set   */
  }
  return k_ra_ok;
}

ra_err_t ra_gpio_read(ra_port_pin_t pin, ra_level_t* out_level)
{
  RA_CHECK_NULL_PTR(out_level, s_tag, "out_level must not be nullptr");
  const ra_port_t port = RA_PIN_PORT(pin);
  const ra_pin_t  bit  = RA_PIN_PIN(pin);
  if ((uint8_t)port > (uint8_t)k_ra_port_max) {
    return k_ra_err_gpio_invalid_port;
  }
  if ((uint8_t)bit > (uint8_t)k_ra_pin_max) {
    return k_ra_err_gpio_invalid_pin;
  }

  volatile r_port_regs_t* port_regs = ra_port(port);
  if (port_regs == nullptr) {
    return k_ra_err_hw_unmapped;
  }

  const uint32_t pidr     = port_regs->PCNTR2;
  const uint32_t bit_mask = (uint32_t)(1UL << (uint32_t)bit);
  *out_level              = ((pidr & bit_mask) != 0U) ? k_ra_level_high : k_ra_level_low;
  return k_ra_ok;
}
