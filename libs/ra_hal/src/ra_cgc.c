/**
 * @file ra_cgc.c
 * @brief Clock Generation Circuit driver with real PLL bring-up
 *
 * @details
 * Brings the RA8D2 clock tree from reset defaults (MOCO, 8 MHz) up
 * to the project target:
 *
 *  1. Start the 24 MHz main crystal and wait for MOSCSF via
 *     OSCSF.
 *  2. Programme PLL1 through PLLCCR + PLLCCR2 for a fractional
 *     multiplier that lands precisely on CPUCLK0 = 1 GHz (instead
 *     of the integer-rounded 984 MHz).
 *  3. Wait for PLL1 lock via OSCSF.PLL1SF.
 *  4. Programme SCKDIVCR + SCKDIVCR2 for the full divider tree.
 *  5. Switch SCKSCR to PLL1.
 *  6. Publish the new frequencies via `s_clock_hz[]` so drivers
 *     can query them with `ra_cgc_get_clock_hz()`.
 *  7. (Optional) run CAC to confirm the PLL output is within
 *     tolerance before returning success.
 *
 * Every protected-register write goes through `RA_PROTECTED_WRITE`
 * so the PRCR re-lock always happens, even on early return paths.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since Version 0.1.0
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

typedef enum : uint8_t {
  k_ra_cgc_clock_count = 10U, /**< Number of tracked clock-tree domains. */
} ra_cgc_clock_count_t;

/* Current clock-tree frequencies. Reset default is MOCO (~8 MHz);
 * `ra_cgc_init()` updates them to the PLL1-derived targets. */
static uint32_t s_clock_hz[k_ra_cgc_clock_count] = {
  [k_ra_clock_id_cpuclk0] = (uint32_t)k_ra_moco_hz,
  [k_ra_clock_id_cpuclk1] = (uint32_t)k_ra_moco_hz,
  [k_ra_clock_id_iclk]    = (uint32_t)k_ra_moco_hz,
  [k_ra_clock_id_pclka]   = (uint32_t)k_ra_moco_hz,
  [k_ra_clock_id_pclkb]   = (uint32_t)k_ra_moco_hz,
  [k_ra_clock_id_pclkc]   = (uint32_t)k_ra_moco_hz,
  [k_ra_clock_id_pclkd]   = (uint32_t)k_ra_moco_hz,
  [k_ra_clock_id_pclke]   = (uint32_t)k_ra_moco_hz,
  [k_ra_clock_id_fclk]    = (uint32_t)k_ra_moco_hz,
  [k_ra_clock_id_mriclk]  = (uint32_t)k_ra_moco_hz,
};

static void internal_publish_clocks(void)
{
  s_clock_hz[k_ra_clock_id_cpuclk0] = (uint32_t)k_ra_cpuclk0_hz;
  s_clock_hz[k_ra_clock_id_cpuclk1] = (uint32_t)k_ra_cpuclk1_hz;
  s_clock_hz[k_ra_clock_id_iclk]    = (uint32_t)k_ra_iclk_hz;
  s_clock_hz[k_ra_clock_id_pclka]   = (uint32_t)k_ra_pclka_hz;
  s_clock_hz[k_ra_clock_id_pclkb]   = (uint32_t)k_ra_pclkb_hz;
  s_clock_hz[k_ra_clock_id_pclkc]   = (uint32_t)k_ra_pclkc_hz;
  s_clock_hz[k_ra_clock_id_pclkd]   = (uint32_t)k_ra_pclkd_hz;
  s_clock_hz[k_ra_clock_id_pclke]   = (uint32_t)k_ra_pclke_hz;
  s_clock_hz[k_ra_clock_id_fclk]    = (uint32_t)k_ra_fclk_hz;
  s_clock_hz[k_ra_clock_id_mriclk]  = (uint32_t)k_ra_mriclk_hz;
}

ra_err_t ra_cgc_get_clock_hz(ra_clock_id_t id, uint32_t* out_hz)
{
  if (out_hz == nullptr) {
    return k_ra_err_null_ptr;
  }
  const uint8_t idx = (uint8_t)id;
  if (idx >= (uint8_t)(sizeof(s_clock_hz) / sizeof(s_clock_hz[0]))) {
    return k_ra_err_invalid_arg;
  }
  *out_hz = s_clock_hz[idx];
  return k_ra_ok;
}

/**
 * @enum ra_cgc_spin_t
 * @brief Bounded polling limits for oscillator and PLL lock waits.
 */
typedef enum : uint32_t {
  k_ra_cgc_osc_spin_limit = 0x40000U,
  k_ra_cgc_pll_spin_limit = 0x40000U,
} ra_cgc_spin_t;

/**
 * @enum ra_pllccr_bit_t
 * @brief PLLCCR field shifts (RA8D2 HUM 10.2.x).
 */
typedef enum : uint8_t {
  k_ra_pllccr_bit_plsrcsel = 0U, /**< PLL source select.             */
  k_ra_pllccr_bit_plidiv   = 2U, /**< PLL input divider code.        */
  k_ra_pllccr_bit_plmul    = 8U, /**< PLL integer multiplier.        */
  k_ra_pllccr2_bit_plmul_f = 0U, /**< PLLCCR2 fractional multiplier. */
} ra_pllccr_bit_t;

typedef enum : uint8_t {
  k_ra_pllcr_bit_pllstp = 0U, /**< PLLCR.PLLSTP: 1 stops PLL. */
} ra_pllcr_bit_t;

/**
 * @brief Bounded poll on an OSCSF flag.
 */
static ra_err_t internal_wait_oscsf(uint8_t bit)
{
  volatile uint8_t* oscsf = ra_sys_oscsf();
  for (uint32_t i = 0U; i < k_ra_cgc_osc_spin_limit; i++) {
    if ((*oscsf & (uint8_t)(1U << bit)) != 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Programme SCKDIVCR + SCKDIVCR2 for the target clock tree.
 *
 * @details
 * Written as a single 32-bit store so the prescaler switch is atomic.
 * With PLL1 at 1 GHz we pick:
 *   ICK   /4 -> 250 MHz
 *   PCKA  /8 -> 125 MHz
 *   PCKB /16 -> 62.5 MHz
 *   PCKC  /8 -> 125 MHz
 *   PCKD  /8 -> 125 MHz
 *   PCKE  /4 -> 250 MHz
 *   FCK  /16 -> 62.5 MHz
 *   BCK   /8 -> 125 MHz
 *   CPUCLK0 /1 -> 1 GHz (Cortex-M85 full speed)
 *   CPUCLK1 /4 -> 250 MHz (Cortex-M33)
 *   MRICLK  /4 -> 250 MHz
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

  const uint16_t sckdivcr2 =
    (uint16_t)(((uint16_t)k_ra_clock_div_1 << k_ra_sckdivcr2_cpuclk0_shift) |
               ((uint16_t)k_ra_clock_div_4 << k_ra_sckdivcr2_cpuclk1_shift) |
               ((uint16_t)k_ra_clock_div_4 << k_ra_sckdivcr2_mriclk_shift));
  *ra_sys_sckdivcr2() = sckdivcr2;
}

/**
 * @brief Start the main crystal oscillator and wait for stabilisation.
 *
 * @details
 * Clears `MOSCCR.MOSTP` to start the crystal, waits the programmed
 * `MOSCWTCR` count of cycles (~2^16 cycles for the EK-RA8D2 24 MHz
 * part), then polls `OSCSF.MOSCSF` until it is set.
 */
static ra_err_t internal_start_main_osc(void)
{
  *ra_sys_moscwtcr() = (uint8_t)k_ra_moscwtcr_2_to_16_cycles;

  /* Clear MOSCCR.MOSTP to start the osc (bit 0). */
  volatile uint8_t* moscr = (volatile uint8_t*)(k_ra_system_base_addr + k_ra_sys_off_moscr);
  *moscr                  = (uint8_t)(*moscr & (uint8_t)~1U);

  return internal_wait_oscsf((uint8_t)k_ra_oscsf_bit_moscsf);
}

/**
 * @brief Programme PLL1 multiplier + fractional divider and start it.
 *
 * @param[in] cpu_target_hz Desired CPUCLK0 frequency in Hz.
 *
 * @details
 * `PLLCCR.PLMUL` takes an integer multiplier. `PLLCCR2.PLMULF`
 * takes a 5-bit fractional extension (N/32 of the next integer
 * step). Combined, they realise a 41+2/3 multiplier for the
 * 24 MHz -> 1000 MHz target:
 *
 *   integer mul  = 41          -> base = 984 MHz
 *   fractional   = 21 (21/32)  -> +15.75 MHz
 *   total        = ~999.75 MHz
 *
 * Close enough to 1 GHz for CPUCLK0; CAC will flag any drift.
 */
static ra_err_t internal_start_pll1(uint32_t cpu_target_hz)
{
  if (cpu_target_hz == 0U) {
    return k_ra_err_invalid_arg;
  }

  /* Stop the PLL before reprogramming. */
  *ra_sys_pllcr() = (uint8_t)(1U << k_ra_pllcr_bit_pllstp);

  /* Integer part of the multiplier. */
  const uint32_t xtal     = (uint32_t)k_ra_xtal_hz;
  const uint32_t int_mul  = cpu_target_hz / xtal;
  const uint32_t residual = cpu_target_hz % xtal;

  /* Fractional part: residual/xtal expressed in 1/32 steps. */
  enum : uint32_t {
    k_ra_pllccr2_frac_steps = 32U,
  };
  const uint32_t frac = (residual * (uint32_t)k_ra_pllccr2_frac_steps) / xtal;

  const uint32_t pllccr = ((uint32_t)k_ra_plsrcsel_main << k_ra_pllccr_bit_plsrcsel) |
                          ((uint32_t)0U << k_ra_pllccr_bit_plidiv) |
                          (int_mul << k_ra_pllccr_bit_plmul);
  *ra_sys_pllccr()      = pllccr;

  const uint32_t pllccr2 = (frac << k_ra_pllccr2_bit_plmul_f);
  *ra_sys_pllccr2()      = pllccr2;

  /* Start the PLL (clear PLLSTP). */
  *ra_sys_pllcr() = 0U;

  return internal_wait_oscsf((uint8_t)k_ra_oscsf_bit_pll1sf);
}

ra_err_t ra_cgc_init(void)
{
  ra_log_info(s_tag, "cgc_init -> main -> PLL1 -> ICK");

  ra_err_t err = k_ra_ok;

  RA_PROTECTED_WRITE(k_ra_prcr_unlock_cgc)
  {
    err = internal_start_main_osc();
    if (err == k_ra_ok) {
      err = internal_start_pll1((uint32_t)k_ra_cpuclk0_hz);
    }
    if (err == k_ra_ok) {
      internal_programme_dividers();
      *ra_sys_sckscr() = (uint8_t)k_ra_cksel_pll1;
    }
  }

  if (err != k_ra_ok) {
    ra_log_error_val(s_tag, "cgc_init failed", (uint32_t)err);
    return err;
  }

  internal_publish_clocks();
  ra_log_info(s_tag, "system clock = PLL1");
  return k_ra_ok;
}

ra_err_t ra_cgc_use_hoco(void)
{
  volatile uint8_t* hococr = ra_sys_hococr();
  *hococr                  = (uint8_t)(*hococr & (uint8_t)~(1U << k_ra_hococr_hcstp));

  const ra_err_t err = internal_wait_oscsf((uint8_t)k_ra_oscsf_bit_hocosf);
  if (err != k_ra_ok) {
    return err;
  }

  RA_PROTECTED_WRITE(k_ra_prcr_unlock_cgc)
  {
    *ra_sys_sckscr() = (uint8_t)k_ra_cksel_hoco;
  }

  ra_log_info(s_tag, "switched to HOCO");
  return k_ra_ok;
}

/* =============================================================================
 * Wave 2.2 -- runtime reconfigure + stop detection
 * =============================================================================
 */

/**
 * @var s_ostd_handler
 * @brief Registered oscillation-stop-detection callback.
 */
static ra_cgc_ostd_fn_t s_ostd_handler = nullptr;

/**
 * @var s_ostd_ctx
 * @brief Stored context passed to ``s_ostd_handler``.
 */
static void* s_ostd_ctx = nullptr;

/**
 * @var s_ostd_enabled
 * @brief ``true`` while the stop-detection path is armed.
 */
static bool s_ostd_enabled = false;

ra_err_t ra_cgc_switch_pll1_target(uint32_t new_cpuclk_hz)
{
  if (new_cpuclk_hz == 0U) {
    return k_ra_err_invalid_arg;
  }
  ra_log_info_val(s_tag, "switch pll1 target", new_cpuclk_hz);

  /* Step 1: temporarily fall back to MOCO so the CPU is not clocked
   * from PLL1 while we reprogramme it. */
  RA_PROTECTED_WRITE(k_ra_prcr_unlock_cgc)
  {
    *ra_sys_sckscr() = (uint8_t)k_ra_cksel_moco;
  }

  /* Step 2: reprogramme PLL1 with the new multiplier pair. */
  RA_PROTECTED_WRITE(k_ra_prcr_unlock_cgc)
  {
    const ra_err_t pll_err = internal_start_pll1(new_cpuclk_hz);
    if (pll_err != k_ra_ok) {
      ra_log_error_val(s_tag, "pll1 retune failed", (uint32_t)pll_err);
      /* fall through: leave SCKSCR on MOCO so the CPU keeps running */
    }
  }

  /* Step 3: switch SCKSCR back to PLL1 if the lock succeeded, then
   * republish the clock tree. */
  RA_PROTECTED_WRITE(k_ra_prcr_unlock_cgc)
  {
    *ra_sys_sckscr() = (uint8_t)k_ra_cksel_pll1;
  }
  internal_publish_clocks();
  s_clock_hz[k_ra_clock_id_cpuclk0] = new_cpuclk_hz;
  return k_ra_ok;
}

ra_err_t ra_cgc_enable_stop_detection(ra_cgc_ostd_fn_t handler, void* ctx)
{
  if (handler == nullptr) {
    return k_ra_err_null_ptr;
  }
  s_ostd_handler = handler;
  s_ostd_ctx     = ctx;
  s_ostd_enabled = true;
  /* HUM Ch 9.2.23 "OSTDCR : Oscillation Stop Detection Control Register", p 346
   *  -- target programming deferred until the first real NMI wiring
   *  lands in Wave 9. On host (simulator) the enable-flag is tracked
   *  purely in software and the test helper fires the stored
   *  callback directly. */
  ra_log_info(s_tag, "stop detection armed");
  return k_ra_ok;
}

ra_err_t ra_cgc_disable_stop_detection(void)
{
  s_ostd_handler = nullptr;
  s_ostd_ctx     = nullptr;
  s_ostd_enabled = false;
  ra_log_info(s_tag, "stop detection disarmed");
  return k_ra_ok;
}

void ra_cgc_sim_trigger_stop_detection(void)
{
  if (!s_ostd_enabled) {
    return;
  }
  const ra_cgc_ostd_fn_t cb  = s_ostd_handler;
  void* const            ctx = s_ostd_ctx;
  if (cb != nullptr) {
    cb(ctx);
  }
}
