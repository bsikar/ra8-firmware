/**
 * @file ra_time.c
 * @brief SysTick tick counter implementation
 *
 * @details
 * SysTick control registers are architectural (part of the Cortex-M
 * System Control Block). Addresses:
 *
 *   SYST_CSR   @ 0xE000E010  -- Control and status
 *   SYST_RVR   @ 0xE000E014  -- Reload value
 *   SYST_CVR   @ 0xE000E018  -- Current value
 *   SYST_CALIB @ 0xE000E01C  -- Calibration
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_time.h"

#include <stdint.h>

#include "ra_err.h"
#include "ra_log.h"
#include "ra_time_constants.h"

static const char* s_tag = "TIME";

static volatile uint32_t s_tick_ms = 0U;

typedef enum : uintptr_t {
  k_ra_systick_csr_addr = 0xE000E010UL,
  k_ra_systick_rvr_addr = 0xE000E014UL,
  k_ra_systick_cvr_addr = 0xE000E018UL,
} ra_systick_addr_t;

typedef enum : uint32_t {
  k_ra_systick_csr_enable    = 0x00000001UL,
  k_ra_systick_csr_tickint   = 0x00000002UL,
  k_ra_systick_csr_clksource = 0x00000004UL, /**< 1 = CPU clock, 0 = ext ref. */
} ra_systick_csr_bit_t;

static inline volatile uint32_t* internal_csr(void)
{
  return (volatile uint32_t*)k_ra_systick_csr_addr;
}

static inline volatile uint32_t* internal_rvr(void)
{
  return (volatile uint32_t*)k_ra_systick_rvr_addr;
}

static inline volatile uint32_t* internal_cvr(void)
{
  return (volatile uint32_t*)k_ra_systick_cvr_addr;
}

ra_err_t ra_time_init(uint32_t cpu_hz)
{
  if (cpu_hz == 0U) {
    ra_log_error(s_tag, "cpu_hz must be non-zero");
    return k_ra_err_invalid_arg;
  }

  const uint32_t reload = (cpu_hz / k_ra_ms_per_sec) - 1U;
  if (reload == 0U) {
    ra_log_error(s_tag, "cpu_hz too low for 1kHz tick");
    return k_ra_err_invalid_arg;
  }

#ifndef RA_SIMULATOR_MODE
  /* Disable, programme reload, clear current count, re-enable with
   * IRQ and CPU clock source. The SysTick registers live in the
   * Cortex-M System Control Space and are not mapped on the host
   * test build. */
  *internal_csr() = 0U;
  *internal_rvr() = reload;
  *internal_cvr() = 0U;
  *internal_csr() = k_ra_systick_csr_clksource | k_ra_systick_csr_tickint | k_ra_systick_csr_enable;
#endif

  s_tick_ms = 0U;
  ra_log_info_val(s_tag, "systick reload", reload);
  return k_ra_ok;
}

uint32_t ra_time_ms(void)
{
  return s_tick_ms;
}

void ra_delay_ms(uint32_t ms)
{
  const uint32_t start = s_tick_ms;
  while ((uint32_t)(s_tick_ms - start) < ms) {
#ifndef RA_SIMULATOR_MODE
    __asm__ volatile("wfi");
#endif
  }
}

void ra_time_on_tick(void)
{
  s_tick_ms++;
}

/*
 * Optional: override the weak SysTick_Handler installed by
 * vector_table.c. Compilation units that want manual control over
 * the SysTick IRQ can define their own SysTick_Handler and this one
 * will be silently dropped.
 */
/* NOLINTNEXTLINE(misc-use-internal-linkage) -- linker symbol for vector table. */
__attribute__((weak)) void SysTick_Handler(void)
{
  ra_time_on_tick();
}
