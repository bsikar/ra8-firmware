/**
 * @file ra_icu.c
 * @brief Interrupt Control Unit driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * rewrite. Adds IRQCRi + NMI programming while keeping
 * the previous legacy API (``ra_icu_route`` / ``ra_icu_nvic_*``)
 * available for the existing demo main.c and test_ra_icu.c suite.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_icu.h"

#include <stdint.h>

#include "ra8d2_icu_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "ICU";

/* =============================================================================
 * NVIC register addresses (Cortex-M85 architectural)
 * =============================================================================
 */
typedef enum : uintptr_t {
  k_ra_nvic_iser_base = 0xE000E100UL, /**< Interrupt Set-Enable. */
  k_ra_nvic_icer_base = 0xE000E180UL, /**< Interrupt Clear-Enable.*/
  k_ra_nvic_ipr_base  = 0xE000E400UL, /**< Priority byte array. */
} ra_nvic_addr_t;

typedef enum : uint32_t {
  k_ra_nvic_bits_per_word = 32U,
  k_ra_nvic_prio_shift    = 4U, /**< 8-bit priority, 4 MSBs used. */
} ra_nvic_layout_t;

/**
 * @enum ra_icu_clear_val_t
 * @brief Magic values used during ra_icu_init.
 */
typedef enum : uint32_t {
  /* FSP R_ICU_NMICLR bits 0..14 + 16..18 + 20 are write-1-to-clear; use
   * the full 32-bit mask to cover future extensions. */
  k_ra_icu_nmiclr_all = 0xFFFFFFFFUL,
  /* FSP R_ICU_WUPEN0 / WUPEN1 are RW, reset value 0; clearing every
   * bit puts the wake-up matrix back to its post-reset state. */
  k_ra_icu_wupen_clear = 0x00000000UL,
} ra_icu_clear_val_t;

/* =============================================================================
 * initialisation
 * =============================================================================
 */

/* Implementation of ra_icu_init (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_icu_init(void)
{
  ra_log_info(s_tag, "ra_icu_init");

  /* HUM Ch 14.2.12 "IRQCRi : IRQ Control Register", p 535 */
  for (uint8_t i = 0U; i < (uint8_t)k_ra_icu_num_irqs; ++i) {
    volatile uint8_t* irqcr = ra_icu_irqcr(i);
    if (irqcr != nullptr) { /* GCOVR_EXCL_BR_LINE -- bounds validated */
      *irqcr = 0U;
    }
  }

  /* HUM Ch 14.2.14 "NMIER : NMI Enable Register", p 542 */
  *ra_icu_nmier() = 0UL;

  /* HUM Ch 14.2.15 "NMICLR : NMI Status Clear Register", p 544 -- write
   * all-ones to clear every latched NMI status bit. */
  *ra_icu_nmiclr() = k_ra_icu_nmiclr_all;

  /* HUM Ch 14 "WUPEN0/WUPEN1 : Wake Up Interrupt Enable Register",
   * p 524..582 -- clear so no source can wake the core unexpectedly. */
  *ra_icu_wupen0() = k_ra_icu_wupen_clear;
  *ra_icu_wupen1() = k_ra_icu_wupen_clear;
  return k_ra_ok;
}

/* =============================================================================
 * external IRQ pin programming
 * =============================================================================
 */

/* Implementation of ra_icu_configure_irq_pin (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_icu_configure_irq_pin(uint8_t irq_num, const ra_icu_irq_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "irq cfg");
  if (irq_num >= (uint8_t)k_ra_icu_num_irqs) {
    return k_ra_err_invalid_arg;
  }
  volatile uint8_t* irqcr = ra_icu_irqcr(irq_num);
  if (irqcr == nullptr) { /* GCOVR_EXCL_BR_LINE -- bounds validated */
    return k_ra_err_invalid_arg;
  }

  const uint8_t irqmd = ((uint8_t)cfg->sense & k_ra_icu_irqcr_mask_irqmd);
  const uint8_t fclk  = (uint8_t)(((uint8_t)cfg->filter_div << k_ra_icu_irqcr_bit_fclksel) &
                                  k_ra_icu_irqcr_mask_fclksel);
  uint8_t       val   = (uint8_t)(irqmd | fclk);
  if (cfg->filter_en) {
    val |= k_ra_icu_irqcr_mask_flten;
  }
  /* HUM Ch 14.2.12 "IRQCRi : IRQ Control Register", p 535 */
  *irqcr = val;
  ra_log_info_val(s_tag, "irqcr pin", (uint32_t)irq_num);
  return k_ra_ok;
}

/* Implementation of ra_icu_read_irqcr (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_icu_read_irqcr(uint8_t irq_num, uint8_t* out_val)
{
  RA_CHECK_NULL_PTR(out_val, s_tag, "irqcr out");
  if (irq_num >= (uint8_t)k_ra_icu_num_irqs) {
    return k_ra_err_invalid_arg;
  }
  volatile const uint8_t* irqcr = ra_icu_irqcr(irq_num);
  if (irqcr == nullptr) { /* GCOVR_EXCL_BR_LINE -- bounds validated */
    return k_ra_err_invalid_arg;
  }
  *out_val = *irqcr;
  return k_ra_ok;
}

/* =============================================================================
 * NMI management
 * =============================================================================
 */

/* Implementation of ra_icu_nmi_enable (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_icu_nmi_enable(uint32_t mask)
{
  /* HUM Ch 14.2.14 "NMIER : NMI Enable Register", p 542 */
  volatile uint32_t* nmier = ra_icu_nmier();
  *nmier                   = (*nmier | mask);
  return k_ra_ok;
}

/* Implementation of ra_icu_nmi_disable (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_icu_nmi_disable(uint32_t mask)
{
  /* HUM Ch 14.2.14 "NMIER : NMI Enable Register", p 542 */
  volatile uint32_t* nmier = ra_icu_nmier();
  *nmier                   = (*nmier & ~mask);
  return k_ra_ok;
}

/* Implementation of ra_icu_nmi_clear (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_icu_nmi_clear(uint32_t mask)
{
  /* HUM Ch 14.2.15 "NMICLR : NMI Status Clear Register", p 544 */
  *ra_icu_nmiclr() = mask;
  return k_ra_ok;
}

/* Implementation of ra_icu_nmi_status (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_icu_nmi_status(uint32_t* out_status)
{
  RA_CHECK_NULL_PTR(out_status, s_tag, "nmi status out");
  *out_status = *ra_icu_nmisr();
  return k_ra_ok;
}

/* =============================================================================
 * Legacy NVIC + IELSR direct access (preserved, deprecated)
 * =============================================================================
 */

/* Implementation of ra_icu_route (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_icu_route(uint16_t nvic_index, ra_elc_event_t event)
{
  volatile uint32_t* ielsr = ra_icu_ielsr(nvic_index);
  if (ielsr == nullptr) {
    return k_ra_err_out_of_range;
  }
  /* HUM Ch 14.2 "IELSRn : ICU Event Link Setting Register n", p 524 */
  *ielsr = (uint32_t)event & k_ra_ielsr_iels_mask;
  ra_log_info_val(s_tag, "route nvic", (uint32_t)nvic_index);
  return k_ra_ok;
}

/* Implementation of ra_icu_nvic_enable (see header for full contract) -- see header for the documented contract. */
void ra_icu_nvic_enable(uint16_t nvic_index)
{
  const uint16_t     word = nvic_index / (uint16_t)k_ra_nvic_bits_per_word;
  const uint16_t     bit  = nvic_index % (uint16_t)k_ra_nvic_bits_per_word;
  volatile uint32_t* iser =
    (volatile uint32_t*)(k_ra_nvic_iser_base + ((uintptr_t)word * sizeof(uint32_t)));
  *iser = (uint32_t)(1UL << bit);
}

/* Implementation of ra_icu_nvic_disable (see header for full contract) -- see header for the documented contract. */
void ra_icu_nvic_disable(uint16_t nvic_index)
{
  const uint16_t     word = nvic_index / (uint16_t)k_ra_nvic_bits_per_word;
  const uint16_t     bit  = nvic_index % (uint16_t)k_ra_nvic_bits_per_word;
  volatile uint32_t* icer =
    (volatile uint32_t*)(k_ra_nvic_icer_base + ((uintptr_t)word * sizeof(uint32_t)));
  *icer = (uint32_t)(1UL << bit);
}

/* Implementation of ra_icu_nvic_set_priority (see header for full contract) -- see header for the documented contract. */
void ra_icu_nvic_set_priority(uint16_t nvic_index, uint8_t priority)
{
  volatile uint8_t* ipr = (volatile uint8_t*)(k_ra_nvic_ipr_base + (uintptr_t)nvic_index);
  *ipr                  = (uint8_t)(priority << k_ra_nvic_prio_shift);
}
