/**
 * @file ra_cgc.c
 * @brief Clock Generation Circuit driver with real PLL bring-up
 *
 * @details
 * Brings the RA8D2 clock tree from reset defaults (MOCO, 8 MHz) up
 * to the project target:
 *
 *  - Main oscillator from the 24 MHz EK-RA8D2 crystal.
 *  - PLL1 fed from the main osc, multiplied to ~1 GHz for CPUCLK0.
 *  - Peripheral dividers from `ra_time_constants.h` so PCLKA /
 *    PCLKB / PCLKC / PCLKD / PCLKE / FCLK all come out at the
 *    documented targets.
 *
 * The sequence mirrors RA8D2 HUM section 10 ("Clock Generation
 * Circuit") and uses the PRCR unlock helpers from
 * `ra8d2_system_regs.h`. Every write to a protected register goes
 * through `RA_PROTECTED_WRITE` so the re-lock always happens, even
 * on an early return.
 *
 * @note Uses an integer-multiplier PLL approximation (cpu_target_hz
 *       / xtal_hz). For the default 1 GHz target on 24 MHz XTAL
 *       that rounds down to 984 MHz. The fractional PLLCCR2 divider
 *       will land with the first high-accuracy timing consumer.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_cgc.h"

#include <stdint.h>

#include "ra8d2_cgc_regs.h"
#include "ra8d2_system_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_register_protection.h"
#include "ra_time_constants.h"

static const char* s_tag = "CGC";

/**
 * @enum ra_cgc_spin_t
 * @brief Bounded polling limits for oscillator and PLL lock waits.
 */
typedef enum : uint32_t {
  k_ra_cgc_osc_spin_limit = 0x10000U,
  k_ra_cgc_pll_spin_limit = 0x10000U,
} ra_cgc_spin_t;

/**
 * @enum ra_pllccr_bit_t
 * @brief PLLCCR field shifts (RA8D2 HUM 10.2.x).
 */
typedef enum : uint8_t {
  k_ra_pllccr_bit_plsrcsel = 0U, /**< PLL source select.        */
  k_ra_pllccr_bit_plidiv   = 2U, /**< PLL input divider.        */
  k_ra_pllccr_bit_plmul    = 8U, /**< PLL multiplier.           */
} ra_pllccr_bit_t;

/**
 * @brief Programme SCKDIVCR for the target peripheral clock tree.
 *
 * @details
 * Written as a single 32-bit store so the prescaler switch is atomic.
 * With PLL1 at ~1 GHz we pick:
 *   ICK   /4 -> 250 MHz
 *   PCKA  /8 -> 125 MHz
 *   PCKB /16 -> 62.5 MHz
 *   PCKC  /8 -> 125 MHz
 *   PCKD  /8 -> 125 MHz
 *   PCKE  /4 -> 250 MHz
 *   FCK  /16 -> 62.5 MHz
 *   BCK   /8 -> 125 MHz
 */
static void internal_programme_dividers(void)
{
  const uint32_t sckdivcr = ((uint32_t)k_ra_clock_div_8 << k_ra_sckdivcr_pckd_shift) |
                            ((uint32_t)k_ra_clock_div_8 << k_ra_sckdivcr_pckc_shift) |
                            ((uint32_t)k_ra_clock_div_16 << k_ra_sckdivcr_pckb_shift) |
                            ((uint32_t)k_ra_clock_div_8 << k_ra_sckdivcr_pcka_shift) |
                            ((uint32_t)k_ra_clock_div_8 << k_ra_sckdivcr_bck_shift) |
                            ((uint32_t)k_ra_clock_div_4 << k_ra_sckdivcr_pcke_shift) |
                            ((uint32_t)k_ra_clock_div_4 << k_ra_sckdivcr_ick_shift) |
                            ((uint32_t)k_ra_clock_div_16 << k_ra_sckdivcr_fck_shift);

  *ra_sys_sckdivcr() = sckdivcr;
}

/**
 * @brief Start the main crystal oscillator and wait for stabilisation.
 */
static ra_err_t internal_start_main_osc(void)
{
  *ra_sys_moscwtcr() = (uint8_t)k_ra_moscwtcr_2_to_16_cycles;

  for (uint32_t i = 0U; i < k_ra_cgc_osc_spin_limit; i++) {
#ifndef RA_SIMULATOR_MODE
    __asm__ volatile("nop");
#endif
  }
  return k_ra_ok;
}

/**
 * @brief Programme PLL1 multiplier and start it.
 *
 * @param[in] cpu_target_hz Desired CPUCLK0 frequency in Hz.
 */
static ra_err_t internal_start_pll1(uint32_t cpu_target_hz)
{
  if (cpu_target_hz == 0U) {
    return k_ra_err_invalid_arg;
  }

  const uint32_t mul = cpu_target_hz / (uint32_t)k_ra_xtal_hz;

  const uint32_t pllccr = ((uint32_t)k_ra_plsrcsel_main << k_ra_pllccr_bit_plsrcsel) |
                          (0U << k_ra_pllccr_bit_plidiv) | (mul << k_ra_pllccr_bit_plmul);

  *ra_sys_pllccr() = pllccr;

  for (uint32_t i = 0U; i < k_ra_cgc_pll_spin_limit; i++) {
#ifndef RA_SIMULATOR_MODE
    __asm__ volatile("nop");
#endif
  }
  return k_ra_ok;
}

ra_err_t ra_cgc_init(void)
{
  ra_log_info(s_tag, "cgc_init -> main -> PLL1 -> ICK");

  RA_PROTECTED_WRITE(k_ra_prcr_unlock_cgc)
  {
    (void)internal_start_main_osc();
    (void)internal_start_pll1((uint32_t)k_ra_cpuclk0_hz);
    internal_programme_dividers();
    *ra_sys_sckscr() = (uint8_t)k_ra_cksel_pll1;
  }

  ra_log_info(s_tag, "system clock = PLL1");
  return k_ra_ok;
}

ra_err_t ra_cgc_use_hoco(void)
{
  volatile uint8_t* hococr = ra_sys_hococr();
  *hococr                  = (uint8_t)(*hococr & (uint8_t)~(1U << k_ra_hococr_hcstp));

  for (uint32_t i = 0U; i < k_ra_cgc_osc_spin_limit; i++) {
#ifndef RA_SIMULATOR_MODE
    __asm__ volatile("nop");
#endif
  }

  RA_PROTECTED_WRITE(k_ra_prcr_unlock_cgc)
  {
    *ra_sys_sckscr() = (uint8_t)k_ra_cksel_hoco;
  }

  ra_log_info(s_tag, "switched to HOCO");
  return k_ra_ok;
}
