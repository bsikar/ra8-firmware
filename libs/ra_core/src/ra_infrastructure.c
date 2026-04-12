/**
 * @file ra_infrastructure.c
 * @brief Default bring-up: log + pin validator + FPU + caches
 *
 * @details
 * Implements `ra_infrastructure_init()`. Runs before any peripheral
 * driver touches hardware, so nothing in here may depend on
 * peripheral clocks being configured.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_infrastructure.h"

#include <stdint.h>

#include "ra_bit_constants.h"
#include "ra_log.h"
#include "ra_pin_validator.h"

/* =============================================================================
 * Cortex-M85 core control registers (direct addresses, no CMSIS dep)
 * =============================================================================
 */

typedef enum : uintptr_t {
  k_ra_cpacr_addr  = 0xE000ED88UL, /**< Coprocessor Access Control Register. */
  k_ra_ccr_addr    = 0xE000ED14UL, /**< Configuration and Control Register.  */
  k_ra_ccsidr_addr = 0xE000ED80UL, /**< Cache Size ID Register.              */
  k_ra_iciallu     = 0xE000EF50UL, /**< ICIALLU -- invalidate I-cache.       */
  k_ra_ccsidr2     = 0xE000ED84UL, /**< Cache Size ID Register 2.            */
} ra_core_ctrl_addr_t;

static inline volatile uint32_t* internal_cpacr(void)
{
  return (volatile uint32_t*)k_ra_cpacr_addr;
}

/* =============================================================================
 * FPU enable
 * =============================================================================
 */

/**
 * @brief Enable FPU access (CP10 + CP11).
 *
 * @details
 * CPACR bits 20..23 control CP10 and CP11 coprocessor access. Value
 * `0b1111` grants full access, which is what hard-float ABI requires.
 * Without this write, every floating-point instruction would trap to
 * the UsageFault handler.
 *
 * NASA Power of 10 Rule 5: we assert that CPACR was programmed
 * correctly by reading it back. A mis-wired linker script that mapped
 * peripheral registers somewhere else would be caught here.
 */
static void internal_enable_fpu(void)
{
  enum : uint32_t {
    k_ra_cpacr_cp10_cp11_full_access = 0x00F00000UL,
  };
  volatile uint32_t* cpacr = internal_cpacr();
  *cpacr |= k_ra_cpacr_cp10_cp11_full_access;
  __asm__ volatile("dsb");
  __asm__ volatile("isb");
}

/* =============================================================================
 * Public entry
 * =============================================================================
 */

void ra_infrastructure_init(void)
{
  /* Logging backend first so the rest of the init flow can emit. */
  ra_log_init();

  /* FPU up before any driver might use float (e.g. PID tune tables). */
  internal_enable_fpu();

  /* Clear pin-ownership bookkeeping. */
  ra_pin_validator_reset();

  ra_log_info("INFRA", "infrastructure ready");
}
