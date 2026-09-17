/**
 * @file ra8_icu.c
 * @brief Interrupt Control Unit driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Adds IRQCRi + NMI programming for the RA8D2 ICU. The low-level
 * IELSR event allocator lives in ``ra8_isr``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_icu.h"

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_icu_regs.h"
#include "ra8_log.h"

static const char* s_tag = "ICU";

/**
 * @enum ra8_icu_clear_val_t
 * @brief Magic values used during ra8_icu_init.
 */
typedef enum : uint32_t {
  /* FSP R_ICU_NMICLR bits 0..14 + 16..18 + 20 are write-1-to-clear; use
   * the full 32-bit mask to cover future extensions. */
  k_ra8_icu_nmiclr_all = 0xFFFFFFFFUL, /**< RA8 icu nmiclr all. */
  /* FSP R_ICU_WUPEN0 / WUPEN1 are RW, reset value 0; clearing every
   * bit puts the wake-up matrix back to its post-reset state. */
  k_ra8_icu_wupen_clear = 0x00000000UL, /**< RA8 icu wupen clear. */
} ra8_icu_clear_val_t;

/* =============================================================================
 * initialisation
 * =============================================================================
 */

ra8_err_t ra8_icu_init(void)
{
  ra8_log_info(s_tag, "ra8_icu_init");

  /* HUM Ch 14.2.12 "IRQCRi : IRQ Control Register", p 535 */
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_icu_num_irqs; ++i) {
    volatile uint8_t* irqcr = ra8_icu_irqcr(i);
    if (irqcr != nullptr) { /* GCOVR_EXCL_BR_LINE -- bounds validated */
      *irqcr = 0U;
    }
  }

  /* HUM Ch 14.2.14 "NMIER : NMI Enable Register", p 542 */
  *ra8_icu_nmier() = 0UL;

  /* HUM Ch 14.2.15 "NMICLR : NMI Status Clear Register", p 544 -- write
   * all-ones to clear every latched NMI status bit. */
  *ra8_icu_nmiclr() = k_ra8_icu_nmiclr_all;

  /* HUM Ch 14 "WUPEN0/WUPEN1 : Wake Up Interrupt Enable Register",
   * p 524..582 -- clear so no source can wake the core unexpectedly. */
  *ra8_icu_wupen0() = k_ra8_icu_wupen_clear;
  *ra8_icu_wupen1() = k_ra8_icu_wupen_clear;
  return k_ra8_ok;
}

/* =============================================================================
 * external IRQ pin programming
 * =============================================================================
 */

ra8_err_t ra8_icu_configure_irq_pin(uint8_t irq_num, const ra8_icu_irq_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "irq cfg");
  if (irq_num >= (uint8_t)k_ra8_icu_num_irqs) {
    return k_ra8_err_invalid_arg;
  }
  volatile uint8_t* irqcr = ra8_icu_irqcr(irq_num);
  if (irqcr == nullptr) { /* GCOVR_EXCL_BR_LINE -- bounds validated */
    return k_ra8_err_invalid_arg;
  }

  const uint8_t irqmd = ((uint8_t)cfg->sense & k_ra8_icu_irqcr_mask_irqmd);
  const uint8_t fclk  = (uint8_t)(((uint8_t)cfg->filter_div << k_ra8_icu_irqcr_bit_fclksel) &
                                  k_ra8_icu_irqcr_mask_fclksel);
  uint8_t       val   = (uint8_t)(irqmd | fclk);
  if (cfg->filter_en) {
    val |= k_ra8_icu_irqcr_mask_flten;
  }
  /* HUM Ch 14.2.12 "IRQCRi : IRQ Control Register", p 535 */
  *irqcr = val;
  ra8_log_info_val(s_tag, "irqcr pin", (uint32_t)irq_num);
  return k_ra8_ok;
}

ra8_err_t ra8_icu_read_irqcr(uint8_t irq_num, uint8_t* out_val)
{
  RA8_CHECK_NULL_PTR(out_val, s_tag, "irqcr out");
  if (irq_num >= (uint8_t)k_ra8_icu_num_irqs) {
    return k_ra8_err_invalid_arg;
  }
  volatile const uint8_t* irqcr = ra8_icu_irqcr(irq_num);
  if (irqcr == nullptr) { /* GCOVR_EXCL_BR_LINE -- bounds validated */
    return k_ra8_err_invalid_arg;
  }
  *out_val = *irqcr;
  return k_ra8_ok;
}

/* =============================================================================
 * NMI management
 * =============================================================================
 */

ra8_err_t ra8_icu_nmi_enable(uint32_t mask)
{
  /* HUM Ch 14.2.14 "NMIER : NMI Enable Register", p 542 */
  volatile uint32_t* nmier = ra8_icu_nmier();
  *nmier                   = (*nmier | mask);
  return k_ra8_ok;
}

ra8_err_t ra8_icu_nmi_disable(uint32_t mask)
{
  /* HUM Ch 14.2.14 "NMIER : NMI Enable Register", p 542 */
  volatile uint32_t* nmier = ra8_icu_nmier();
  *nmier                   = (*nmier & ~mask);
  return k_ra8_ok;
}

ra8_err_t ra8_icu_nmi_clear(uint32_t mask)
{
  /* HUM Ch 14.2.15 "NMICLR : NMI Status Clear Register", p 544 */
  *ra8_icu_nmiclr() = mask;
  return k_ra8_ok;
}

ra8_err_t ra8_icu_nmi_status(uint32_t* out_status)
{
  RA8_CHECK_NULL_PTR(out_status, s_tag, "nmi status out");
  *out_status = *ra8_icu_nmisr();
  return k_ra8_ok;
}
