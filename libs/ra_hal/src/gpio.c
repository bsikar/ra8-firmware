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

#include <stdint.h>

#include "ra8d2_pfs_regs.h"
#include "ra8d2_port_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_icu.h"
#include "ra_isr.h"
#include "ra_log.h"
#include "ra_pin_interface.h"
#include "ra_pin_validator.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"

/**
 * @enum ra_gpio_irq_limits_t
 * @brief Local limits for IRQ attach API.
 */
typedef enum : uint8_t {
  k_ra_gpio_irq_num_max    = 15U, /**< External IRQ pins 0..15. */
  k_ra_gpio_irq_event_base = 1U,  /**< ELC event for IRQ0. */
} ra_gpio_irq_limits_t;

static const char* s_tag = "GPIO";

/**
 * @brief Claim the pin in the validator and return the port / pin
 * indices if the claim succeeded.
 *
 * @details See implementation.
 * @param[in] pin See implementation.
 * @param[in] out_port See implementation.
 * @param[in] out_pin See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_claim(ra_port_pin_t pin, ra_port_t* out_port, ra_pin_t* out_pin)
{
  const ra_port_t p = RA_PIN_PORT(pin);
  const ra_pin_t  b = RA_PIN_PIN(pin);
  if ((uint8_t)p > k_ra_port_max) {
    return k_ra_err_gpio_invalid_port;
  }
  if ((uint8_t)b > k_ra_pin_max) {
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

/* Implementation of ra_gpio_output_init (see header for full contract) -- see header for the documented contract. */
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
    (uint32_t)((init_level == k_ra_level_high) ? k_ra_pfs_mask_podr : 0U) | k_ra_pfs_mask_pdr;

  ra_pfs_pwpr_unlock();
  *pfs = new_val;
  ra_pfs_pwpr_lock();

  ra_log_info_val(s_tag, "output init pin", (uint32_t)pin);
  return k_ra_ok;
}

/* Implementation of ra_gpio_input_init (see header for full contract) -- see header for the documented contract. */
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
    new_val |= k_ra_pfs_mask_pcr;
  }

  ra_pfs_pwpr_unlock();
  *pfs = new_val;
  ra_pfs_pwpr_lock();

  ra_log_info_val(s_tag, "input init pin", (uint32_t)pin);
  return k_ra_ok;
}

/* Implementation of ra_gpio_write (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_gpio_write(ra_port_pin_t pin, ra_level_t level)
{
  const ra_port_t port = RA_PIN_PORT(pin);
  const ra_pin_t  bit  = RA_PIN_PIN(pin);
  if ((uint8_t)port > k_ra_port_max) {
    return k_ra_err_gpio_invalid_port;
  }
  if ((uint8_t)bit > k_ra_pin_max) {
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

/* Implementation of ra_gpio_toggle (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_gpio_toggle(ra_port_pin_t pin)
{
  const ra_port_t port = RA_PIN_PORT(pin);
  const ra_pin_t  bit  = RA_PIN_PIN(pin);
  if ((uint8_t)port > k_ra_port_max) {
    return k_ra_err_gpio_invalid_port;
  }
  if ((uint8_t)bit > k_ra_pin_max) {
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
    port_regs->PCNTR3 = bit_mask; /* POSR set */
  }
  return k_ra_ok;
}

/* Implementation of ra_gpio_read (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_gpio_read(ra_port_pin_t pin, ra_level_t* out_level)
{
  RA_CHECK_NULL_PTR(out_level, s_tag, "out_level must not be nullptr");
  const ra_port_t port = RA_PIN_PORT(pin);
  const ra_pin_t  bit  = RA_PIN_PIN(pin);
  if ((uint8_t)port > k_ra_port_max) {
    return k_ra_err_gpio_invalid_port;
  }
  if ((uint8_t)bit > k_ra_pin_max) {
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

/* Implementation of ra_gpio_release (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_gpio_release(ra_port_pin_t pin)
{
  return ra_pin_validator_release(pin);
}

/* Implementation of ra_pfs_route_peripheral (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_pfs_route_peripheral(ra_port_pin_t pin, ra_psel_t psel, const char* owner)
{
  RA_CHECK_NULL_PTR(owner, s_tag, "owner must not be nullptr");

  const ra_port_t port = RA_PIN_PORT(pin);
  const ra_pin_t  bit  = RA_PIN_PIN(pin);
  if ((uint8_t)port > k_ra_port_max) {
    return k_ra_err_gpio_invalid_port;
  }
  if ((uint8_t)bit > k_ra_pin_max) {
    return k_ra_err_gpio_invalid_pin;
  }

  const ra_err_t claim = ra_pin_validator_claim(pin, owner);
  if (claim != k_ra_ok) {
    ra_log_error_val(s_tag, "peripheral claim failed", (uint32_t)claim);
    return claim;
  }

  volatile uint32_t* pfs = ra_pfs_pmn(port, bit);
  if (pfs == nullptr) {
    (void)ra_pin_validator_release(pin);
    return k_ra_err_hw_unmapped;
  }

  /* HUM Ch 20.2.4 "Notes on the PmnPFS Register Setting" p 859 -- PMR
   * MUST be cleared before specifying a new PSEL, otherwise the pin can
   * glitch through whatever the previous peripheral function happened
   * to be while the new PSEL is decoded. The correct sequence is:
   *   1. PMR := 0    (force GPIO mode)
   *   2. PFS := PSEL only, PMR still 0  (programme the new function)
   *   3. PFS := PSEL | PMR=1            (hand the pin to the peripheral)
   * @note PMSAR defaults to all-Secure on RA8D2 reset, so writing PWPR
   *       (NS) and PWPRS (S) unconditionally is harmless: the chip
   *       silently drops whichever side does not own the port. */
  const uint32_t psel_only = (((uint32_t)psel) << (uint32_t)k_ra_pfs_bit_psel0);
  const uint32_t new_val   = k_ra_pfs_mask_pmr | psel_only;

  ra_pfs_pwpr_unlock();
  /* HUM Ch 20.2.4 "Notes on the PmnPFS Register Setting" p 859 */
  *pfs = 0U;        /* step 1: PMR = 0. */
  *pfs = psel_only; /* step 2: PSEL with PMR=0. */
  *pfs = new_val;   /* step 3: PSEL | PMR=1. */
  ra_pfs_pwpr_lock();

  ra_log_info_val(s_tag, "peripheral route pin", (uint32_t)pin);
  return k_ra_ok;
}

/* =============================================================================
 * External IRQ attachment
 * =============================================================================
 */

/**
 * @brief Internal helper.
 * @details See implementation.
 * @param[in] irq_num See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_elc_event_t internal_event_for_irq(uint8_t irq_num)
{
  return (ra_elc_event_t)((uint16_t)k_ra_gpio_irq_event_base + (uint16_t)irq_num);
}

/* Implementation of ra_gpio_attach_irq (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_gpio_attach_irq(ra_port_pin_t            pin,
                            uint8_t                  irq_num,
                            const ra_gpio_irq_cfg_t* cfg,
                            ra_isr_handler_t         handler,
                            void*                    ctx)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  RA_CHECK_NULL_PTR(handler, s_tag, "handler must not be nullptr");
  if (irq_num > k_ra_gpio_irq_num_max) {
    return k_ra_err_invalid_arg;
  }

  ra_err_t err = ra_gpio_input_init(pin, cfg->pull);
  if (err != k_ra_ok) {
    return err;
  }

  const ra_icu_irq_cfg_t icu_cfg = {
    .sense      = cfg->sense,
    .filter_div = cfg->filter_div,
    .filter_en  = cfg->filter_en,
  };
  err = ra_icu_configure_irq_pin(irq_num, &icu_cfg);
  if (err != k_ra_ok) { /* GCOVR_EXCL_BR_LINE -- irq_num bounded */
    /* GCOVR_EXCL_START */
    (void)ra_pin_validator_release(pin);
    return err;
    /* GCOVR_EXCL_STOP */
  }

  const ra_elc_event_t event = internal_event_for_irq(irq_num);
  err                        = ra_isr_register(event, handler, ctx, cfg->priority, nullptr);
  if (err != k_ra_ok) { /* GCOVR_EXCL_BR_LINE -- fresh reset_irq_state */
    /* GCOVR_EXCL_START */
    const ra_icu_irq_cfg_t zero_cfg = {
      .sense      = (ra_icu_irqmd_t)0U,
      .filter_div = (ra_icu_fclksel_t)0U,
      .filter_en  = false,
    };
    (void)ra_icu_configure_irq_pin(irq_num, &zero_cfg);
    (void)ra_pin_validator_release(pin);
    return err;
    /* GCOVR_EXCL_STOP */
  }

  ra_log_info_val(s_tag, "attach irq pin", (uint32_t)pin);
  return k_ra_ok;
}

/* Implementation of ra_gpio_detach_irq (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_gpio_detach_irq(ra_port_pin_t pin, uint8_t irq_num)
{
  if (irq_num > k_ra_gpio_irq_num_max) {
    return k_ra_err_invalid_arg;
  }

  const ra_elc_event_t event = internal_event_for_irq(irq_num);
  const ra_err_t       err   = ra_isr_unregister(event);
  if (err != k_ra_ok) {
    return err;
  }

  const ra_icu_irq_cfg_t zero_cfg = {
    .sense      = (ra_icu_irqmd_t)0U,
    .filter_div = (ra_icu_fclksel_t)0U,
    .filter_en  = false,
  };
  (void)ra_icu_configure_irq_pin(irq_num, &zero_cfg);
  (void)ra_pin_validator_release(pin);

  ra_log_info_val(s_tag, "detach irq pin", (uint32_t)pin);
  return k_ra_ok;
}

/* =============================================================================
 * Concrete DI vtable (forwarded thunks)
 * =============================================================================
 */

/**
 * @brief Internal helper.
 * @details See implementation.
 * @param[in] ctx See implementation.
 * @param[in] pin See implementation.
 * @param[in] level See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_pin_if_output_init(void* ctx, ra_port_pin_t pin, ra_level_t level)
{
  (void)ctx;
  return ra_gpio_output_init(pin, level);
}

/**
 * @brief Internal helper.
 * @details See implementation.
 * @param[in] ctx See implementation.
 * @param[in] pin See implementation.
 * @param[in] level See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_pin_if_write(void* ctx, ra_port_pin_t pin, ra_level_t level)
{
  (void)ctx;
  return ra_gpio_write(pin, level);
}

/**
 * @brief Internal helper.
 * @details See implementation.
 * @param[in] ctx See implementation.
 * @param[in] pin See implementation.
 * @param[in] out See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_pin_if_read(void* ctx, ra_port_pin_t pin, ra_level_t* out)
{
  (void)ctx;
  return ra_gpio_read(pin, out);
}

/**
 * @brief Internal helper.
 * @details See implementation.
 * @param[in] ctx See implementation.
 * @param[in] pin See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_pin_if_toggle(void* ctx, ra_port_pin_t pin)
{
  (void)ctx;
  return ra_gpio_toggle(pin);
}

const ra_pin_interface_t g_ra_gpio_pin_interface = {
  .output_init = internal_pin_if_output_init,
  .write       = internal_pin_if_write,
  .read        = internal_pin_if_read,
  .toggle      = internal_pin_if_toggle,
  .ctx         = nullptr,
};
