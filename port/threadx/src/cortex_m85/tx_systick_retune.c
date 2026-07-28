/**
 * @file port/threadx/src/cortex_m85/tx_systick_retune.c
 * @brief Implementation of the ThreadX SysTick kernel-tick retune.
 *
 * @details
 * See `port/threadx/inc/ra8_threadx.h` for the public contract. The
 * SysTick registers touched here (SYST_RVR @ 0xE000E014, SYST_CVR @
 * 0xE000E018) live in the Cortex-M System Control Space and are
 * architectural (Arm), not Renesas peripherals -- so they carry no HUM
 * citation, matching the sibling accessors in
 * `libs/ra8_core/src/ra8_time.c`. On the host unit-test build the SCS is
 * unmapped, so the register writes compile out under `RA8_OFF_TARGET`
 * while the clock query + reload arithmetic still run and are testable.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_threadx.h"

static const char* s_tag = "TX_SYST";

/**
 * @enum ra8_syst_addr_t
 * @brief Cortex-M System Control Space SysTick register addresses.
 */
typedef enum : uintptr_t {
  k_ra8_syst_rvr_addr = 0xE000E014UL, /**< SYST_RVR reload value register.  */
  k_ra8_syst_cvr_addr = 0xE000E018UL, /**< SYST_CVR current value register. */
} ra8_syst_addr_t;

#ifndef RA8_OFF_TARGET
/**
 * @brief Get the SysTick Reload Value Register (SYST_RVR) pointer.
 *
 * @details Trivial address-cast helper; mirrors `ra8_time.c`.
 *
 * @return Volatile pointer to SYST_RVR.
 * @retval (volatile uint32_t*)k_ra8_syst_rvr_addr Always.
 *
 * @pre SCS region is mapped (always true on Cortex-M).
 * @pre None beyond the above.
 * @post No state modified.
 * @post Returned pointer is valid for the program lifetime.
 *
 * @note Trivially thread-safe.
 * @since 0.1.0
 */
static inline volatile uint32_t* internal_syst_rvr(void)
{
  return (volatile uint32_t*)k_ra8_syst_rvr_addr;
}

/**
 * @brief Get the SysTick Current Value Register (SYST_CVR) pointer.
 *
 * @details Trivial address-cast helper; mirrors `ra8_time.c`.
 *
 * @return Volatile pointer to SYST_CVR.
 * @retval (volatile uint32_t*)k_ra8_syst_cvr_addr Always.
 *
 * @pre SCS region is mapped (always true on Cortex-M).
 * @pre None beyond the above.
 * @post No state modified.
 * @post Returned pointer is valid for the program lifetime.
 *
 * @note Trivially thread-safe.
 * @since 0.1.0
 */
static inline volatile uint32_t* internal_syst_cvr(void)
{
  return (volatile uint32_t*)k_ra8_syst_cvr_addr;
}
#endif /* !RA8_OFF_TARGET */

ra8_err_t ra8_threadx_systick_reload_for(uint32_t cpuclk_hz, uint32_t tick_hz, uint32_t* out_reload)
{
  RA8_CHECK_NULL_PTR(out_reload, s_tag, "out_reload is NULL");

  /* Reject a zero clock or a zero tick rate: both would make the reload
   * arithmetic meaningless (division by zero / a 0 Hz core). */
  if ((cpuclk_hz == 0U) || (tick_hz == 0U)) {
    ra8_log_error(s_tag, "cpuclk_hz / tick_hz must be non-zero");
    return k_ra8_err_invalid_arg;
  }

  const uint32_t ticks = cpuclk_hz / tick_hz;
  if (ticks == 0U) {
    /* Clock slower than one tick period -> reload would underflow. */
    ra8_log_error(s_tag, "cpuclk_hz below one tick period");
    return k_ra8_err_invalid_arg;
  }

  const uint32_t reload = ticks - 1U;
  if (reload > (uint32_t)k_ra8_systick_reload_max) {
    /* Clock too high for the 24-bit SYST_RVR field: refuse rather than
     * truncate (which would silently make the tick far too fast). */
    ra8_log_error(s_tag, "reload exceeds 24-bit SysTick range");
    return k_ra8_err_out_of_range;
  }

  *out_reload = reload;
  return k_ra8_ok;
}

ra8_err_t ra8_threadx_systick_retune(void)
{
  uint32_t cpuclk_hz = 0U;
  RA8_RETURN_ON_ERROR(ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk_hz),
                      s_tag,
                      "CPUCLK0 query failed");

  uint32_t reload = 0U;
  RA8_RETURN_ON_ERROR(
    ra8_threadx_systick_reload_for(cpuclk_hz, (uint32_t)k_ra8_threadx_tick_hz, &reload),
    s_tag,
    "SysTick reload computation failed");

#ifndef RA8_OFF_TARGET
  /* Re-arm SysTick for the live clock. Only the reload + current-value
   * words change; the enable / clock-source / tick-interrupt bits set by
   * _tx_initialize_low_level.S are left as-is. Writing SYST_CVR (any
   * value) clears it so the next count starts from the new reload. */
  *internal_syst_rvr() = reload;
  *internal_syst_cvr() = 0U;
#endif

  ra8_log_info_val(s_tag, "systick retuned reload", reload);
  return k_ra8_ok;
}
