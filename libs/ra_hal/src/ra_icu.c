/**
 * @file ra_icu.c
 * @brief Interrupt Control Unit driver implementation
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_icu.h"

#include <stdint.h>

#include "ra8d2_icu_regs.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "ICU";

/* NVIC register addresses (Cortex-M85 architectural). */
typedef enum : uintptr_t {
  k_ra_nvic_iser_base = 0xE000E100UL, /**< Interrupt Set-Enable.  */
  k_ra_nvic_icer_base = 0xE000E180UL, /**< Interrupt Clear-Enable.*/
  k_ra_nvic_ipr_base  = 0xE000E400UL, /**< Priority byte array.   */
} ra_nvic_addr_t;

typedef enum : uint32_t {
  k_ra_nvic_bits_per_word = 32U,
  k_ra_nvic_prio_shift    = 4U, /**< 8-bit priority, 4 MSBs used. */
} ra_nvic_layout_t;

ra_err_t ra_icu_route(uint16_t nvic_index, ra_elc_event_t event)
{
  volatile uint32_t* ielsr = ra_icu_ielsr(nvic_index);
  if (ielsr == nullptr) {
    return k_ra_err_out_of_range;
  }
  *ielsr = (uint32_t)event & k_ra_ielsr_iels_mask;
  ra_log_info_val(s_tag, "route nvic", (uint32_t)nvic_index);
  return k_ra_ok;
}

void ra_icu_nvic_enable(uint16_t nvic_index)
{
  const uint16_t     word = nvic_index / (uint16_t)k_ra_nvic_bits_per_word;
  const uint16_t     bit  = nvic_index % (uint16_t)k_ra_nvic_bits_per_word;
  volatile uint32_t* iser =
    (volatile uint32_t*)(k_ra_nvic_iser_base + ((uintptr_t)word * sizeof(uint32_t)));
  *iser = (uint32_t)(1UL << bit);
}

void ra_icu_nvic_disable(uint16_t nvic_index)
{
  const uint16_t     word = nvic_index / (uint16_t)k_ra_nvic_bits_per_word;
  const uint16_t     bit  = nvic_index % (uint16_t)k_ra_nvic_bits_per_word;
  volatile uint32_t* icer =
    (volatile uint32_t*)(k_ra_nvic_icer_base + ((uintptr_t)word * sizeof(uint32_t)));
  *icer = (uint32_t)(1UL << bit);
}

void ra_icu_nvic_set_priority(uint16_t nvic_index, uint8_t priority)
{
  volatile uint8_t* ipr = (volatile uint8_t*)(k_ra_nvic_ipr_base + (uintptr_t)nvic_index);
  *ipr                  = (uint8_t)(priority << k_ra_nvic_prio_shift);
}
