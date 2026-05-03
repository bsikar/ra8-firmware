/**
 * @file examples/ek_ra8d2/ereader/power.c
 * @brief Sleep / wake / brown-out implementation for the ereader app.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Glue between the ereader's main thread loop and the underlying
 * ra_icu / ra_lvd / ra_lpm drivers. The actual register pokes live
 * in ``libs/ra_hal``; this file only owns the small state the app
 * needs (an LVD trip flag, a "last woken IRQ" slot) plus the ISR
 * shims that capture the wake-source so the caller knows which
 * button woke it.
 *
 * NASA Power-of-10 compliance:
 *   - Rule 2: every loop has a static bound (none in this file
 *     today; the WFI block is a single instruction).
 *   - Rule 3: zero malloc -- two file-scope flags, that's it.
 *   - Rule 5: every entry point has at least 2 pre/post checks.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "power.h"

#include <stdint.h>

#include "ra8d2_icu_regs.h"
#include "ra8d2_lpm_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_icu.h"
#include "ra_log.h"
#include "ra_lpm.h"
#include "ra_lvd.h"

/**
 * @var s_tag
 * @brief Logging tag used by every error path in this TU.
 */
static const char* s_tag = "EREADER_PWR";

/**
 * @enum ereader_power_state_t
 * @brief Lifecycle of the power surface.
 */
typedef enum : uint8_t {
  k_ereader_power_state_uninit = 0U,
  k_ereader_power_state_ready  = 1U,
} ereader_power_state_t;

/**
 * @var s_state
 * @brief One-shot guard so callers don't double-init.
 */
static ereader_power_state_t s_state = k_ereader_power_state_uninit;

/**
 * @var s_battery_low_flag
 * @brief Sticky flag set by the LVD ISR.
 *
 * @note ``volatile`` because the ISR writes it asynchronously.
 *       8-bit access is atomic on Cortex-M85.
 */
static volatile uint8_t s_battery_low_flag = 0U;

/**
 * @var s_last_wake_irq
 * @brief IRQ number of the most recent button-wake.
 *
 * @details
 * 0xFF means "woken by something else" (or never). The ICU
 * dispatcher writes this in the SW1/SW2 ISR shims. ``volatile``
 * because it crosses the ISR boundary.
 */
static volatile uint8_t s_last_wake_irq = (uint8_t)k_ereader_power_irq_none;

/**
 * @brief LVD callback -- arms the low-battery flag.
 *
 * @param[in] ctx Unused.
 * @param[in] ch  Channel that fired.
 */
static void internal_lvd_cb(void* ctx, ra_lvd_channel_t ch)
{
  (void)ctx;
  (void)ch;
  s_battery_low_flag = 1U;
}

/**
 * @brief Helper: build a default LVD config that trips below ~2.85 V.
 *
 * @details
 * Threshold matches ``threadx_filex_demo`` so the SD card has time
 * to flush before we panic; falling-edge IRQ; LOCO-divided digital
 * filter on so we don't false-trip during transient dips.
 *
 * @return Filled-in LVD config.
 */
static ra_lvd_cfg_t internal_default_lvd_cfg(void)
{
  const ra_lvd_cfg_t cfg = {
    .threshold    = k_ra_lvd_pvdlvl_2_85v,
    .edge         = k_ra_lvd_edge_fall,
    .irq_type     = k_ra_lvd_irq_maskable,
    .response     = k_ra_lvd_response_interrupt,
    .negate       = k_ra_lvd_negate_after_voltage,
    .hysteresis   = k_ra_lvd_hysteresis_lvd,
    .filter_div   = k_ra_lvd_loco_div_2,
    .filter_en    = true,
    .irq_enable   = true,
    .clear_status = true,
  };
  return cfg;
}

/**
 * @brief Helper: build a default ICU edge config for a button line.
 *
 * @details
 * Falling-edge sensitive, PCLKB/64 digital filter on -- the same
 * shape every previous ereader/threadx button example uses.
 *
 * @return Filled-in ICU IRQ config.
 */
static ra_icu_irq_cfg_t internal_default_button_cfg(void)
{
  const ra_icu_irq_cfg_t cfg = {
    .sense      = k_ra_icu_irqmd_falling,
    .filter_div = k_ra_icu_fclksel_pclkb_64,
    .filter_en  = true,
  };
  return cfg;
}

[[nodiscard]] ra_err_t ereader_power_init(void)
{
  if (s_state != k_ereader_power_state_uninit) {
    return k_ra_err_invalid_state;
  }

  ra_err_t err = ra_icu_init();
  if (err != k_ra_ok) {
    ra_log_error(s_tag, "icu_init failed");
    return k_ra_err_hw_init_failed;
  }

  const ra_icu_irq_cfg_t btn_cfg = internal_default_button_cfg();
  err = ra_icu_configure_irq_pin((uint8_t)k_ereader_power_irq_sw1, &btn_cfg);
  if (err != k_ra_ok) {
    return k_ra_err_hw_init_failed;
  }
  err = ra_icu_configure_irq_pin((uint8_t)k_ereader_power_irq_sw2, &btn_cfg);
  if (err != k_ra_ok) {
    return k_ra_err_hw_init_failed;
  }

  /* PVD2 -- the chip-side battery / Vcc monitor. */
  const ra_lvd_cfg_t lvd_cfg = internal_default_lvd_cfg();
  err                        = ra_lvd_channel_init(k_ra_lvd_ch2, &lvd_cfg);
  if (err != k_ra_ok) {
    ra_log_error(s_tag, "lvd_channel_init failed");
    return k_ra_err_hw_init_failed;
  }
  err = ra_lvd_attach_channel_handler(k_ra_lvd_ch2, internal_lvd_cb, nullptr);
  if (err != k_ra_ok) {
    return k_ra_err_hw_init_failed;
  }

  /* LPM defaults: keep IO levels across sleep so the GLCDC pins
   * don't glitch on wake. STBYCR.OPE = 1 keeps the bus driven. */
  const ra_lpm_config_t lpm_cfg = {
    .io_port_keep     = true,
    .opa_bus_keep     = true,
    .sscr_fast_return = true,
    .dcdc_softstart   = k_ra_lpm_dcssmode_128us,
    .sscr_low_power   = k_ra_lpm_ss2lp_default,
  };
  err = ra_lpm_init(&lpm_cfg);
  if (err != k_ra_ok) {
    ra_log_error(s_tag, "lpm_init failed");
    return k_ra_err_hw_init_failed;
  }

  s_state = k_ereader_power_state_ready;
  ra_log_info(s_tag, "power surface armed");
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ereader_power_sleep_until_button(uint8_t* out_woken_irq)
{
  RA_CHECK_NULL_PTR(out_woken_irq, s_tag, "sleep: out_woken_irq null");
  if (s_state != k_ereader_power_state_ready) {
    return k_ra_err_invalid_state;
  }

  /* Reset the ISR-side wake slot so a stale value doesn't masquerade
   * as the next press. */
  s_last_wake_irq = (uint8_t)k_ereader_power_irq_none;

  /* k_ra_sleep_mode_sleep is plain Sleep (peripherals run, CPU
   * stops at WFI). The ICU IRQs are still enabled at the NVIC, so
   * pressing SW1 / SW2 returns the CPU to this thread. */
  ra_err_t err = ra_lpm_enter_sleep(k_ra_sleep_mode_sleep);
  if (err != k_ra_ok) {
    return err;
  }

  *out_woken_irq = s_last_wake_irq;
  return k_ra_ok;
}

bool ereader_power_check_battery(void)
{
  const uint8_t was  = s_battery_low_flag;
  s_battery_low_flag = 0U;
  return was != 0U;
}

/* =============================================================================
 * Public ISR shims -- wired up in vector_table.c by name.
 *
 * The actual NVIC routing happens in vector_table.c via the existing
 * IRQn_Type table; defining these as plain externs here lets the
 * vector table point its IRQ11 / IRQ12 slots at us without dragging
 * in any extra plumbing. ``ra_icu_dispatch_irq`` clears the IELSR
 * IR flag for us so the IRQ doesn't immediately re-fire.
 * =============================================================================
 */

/**
 * @brief SW1 wake-up shim.
 *
 * @details
 * Records the wake source for ``ereader_power_sleep_until_button``
 * and returns. The ICU clears the IR flag automatically when the
 * IELSR is read in the dispatcher.
 */
void ereader_power_sw1_isr(void);
void ereader_power_sw1_isr(void)
{
  s_last_wake_irq = (uint8_t)k_ereader_power_irq_sw1;
}

/**
 * @brief SW2 wake-up shim. Same shape as the SW1 handler.
 */
void ereader_power_sw2_isr(void);
void ereader_power_sw2_isr(void)
{
  s_last_wake_irq = (uint8_t)k_ereader_power_irq_sw2;
}
