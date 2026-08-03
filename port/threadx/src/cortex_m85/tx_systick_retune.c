/**
 * @file port/threadx/src/cortex_m85/tx_systick_retune.c
 * @brief Implementation of the ThreadX SysTick kernel-tick retune.
 *
 * @details
 * See `port/threadx/inc/ra8_threadx.h` for the public contract.
 * ::ra8_threadx_systick_reload_for derives the SYST_RVR reload from the live
 * core clock and the kernel tick rate; ::ra8_threadx_systick_retune then
 * programs it through the shared `ra8_core` SysTick timebase primitive
 * (::ra8_systick_set_reload), which owns the SYST_RVR / SYST_CVR access. That
 * primitive replaced this file's private address-cast accessors, so the SysTick
 * registers are now programmed from exactly one place in the tree. The SysTick
 * registers are architectural (Arm), not Renesas peripherals, so they carry an
 * Arm v8-M reference rather than a HUM citation. On the host unit-test build
 * the primitive writes to the fake MMIO map, so the retune runs end-to-end and
 * the clock query + reload arithmetic remain testable.
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
#include "ra8_systick.h"
#include "ra8_threadx.h"

static const char* s_tag = "TX_SYST";

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

  /* Re-arm SysTick for the live clock through the shared timebase primitive.
   * Only the reload + current-value words change; the enable / clock-source /
   * tick-interrupt bits set by _tx_initialize_low_level.S are left as-is
   * (ra8_systick_set_reload writes SYST_RVR and clears SYST_CVR, never
   * SYST_CSR). The reload already fit the 24-bit field in reload_for above, so
   * this write cannot be rejected -- but the return is still checked. */
  RA8_RETURN_ON_ERROR(ra8_systick_set_reload(reload), s_tag, "SysTick reload program failed");

  ra8_log_info_val(s_tag, "systick retuned reload", reload);
  return k_ra8_ok;
}
