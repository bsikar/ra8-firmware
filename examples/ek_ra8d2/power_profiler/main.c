/**
 * @file main.c
 * @brief Power-mode profiler smoke app for the EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Standalone EVM-tier app that drives the
 * ``libs/ra_power_profile/ra_power_profile.h`` accumulator across
 * the four interesting RA8D2 low-power modes -- ACTIVE, SLEEP,
 * DEEP_SLEEP, and SOFTWARE_STANDBY -- and reports the dwell time
 * of each region over SCI8.
 *
 * Each cycle:
 *
 *   1. ``ra_power_profile_mark_enter(ACTIVE)`` -- spin a short
 *      busy-loop so the analyser sees a recognisable shape.
 *   2. ``ra_power_profile_mark_enter(SLEEP)`` -> ``ra_lpm_enter_sleep
 *      (k_ra_sleep_mode_sleep)``. SysTick wakes the CPU within
 *      a millisecond.
 *   3. Same for DEEP_SLEEP and SOFTWARE_STANDBY (the latter is
 *      armed against the SysTick wake source so the demo does
 *      not require an external pin event on a bare EVM).
 *   4. After every region transition the profiler stats are
 *      printed as
 *      ``"pp: a=NN s=NN d=NN st=NN us\r\n"``.
 *
 * LED1 toggles per cycle. LED2 latches on if any HAL call hard-fails.
 *
 * Bare EK-RA8D2 only -- no extra pins required. The profiler hooks
 * into ``ra_time_ms`` for the timestamp source so accumulation works
 * on the host test build under RA_SIMULATOR_MODE as well.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_lpm.h"
#include "ra_mstp.h"
#include "ra_port_utils.h"
#include "ra_power_profile.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @brief App-wide tunables. */
typedef enum : uint32_t {
  k_pp_demo_baud        = 115200U,
  k_pp_demo_period_ms   = 1000U,
  k_pp_demo_busy_iters  = 100000U,
  k_pp_demo_us_per_ms   = 1000U,
  k_pp_demo_sci_channel = 8U,
} pp_demo_const_t;

/** @brief Single-byte ASCII conversion constants. */
typedef enum : uint8_t {
  k_pp_demo_ascii_zero   = '0',
  k_pp_demo_dec_base     = 10U,
  k_pp_demo_uint_dec_max = 10U, /**< Max digits in a uint32 base-10. */
  k_pp_demo_print_buf    = 96U, /**< Max bytes in one print line.    */
} pp_demo_byte_t;

/** @brief Pinout for SCI8 console. */
static const ra_port_pin_t k_pp_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_pp_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief Park forever after a fatal init failure.
 *
 * @pre Called only after a fatal error in boot.
 * @post CPU is parked; only a debugger or external reset wakes it.
 * @since 0.1.0
 */
static void pp_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Time-base hook for the profiler -- wraps ``ra_time_ms`` to us.
 *
 * @param[in] ctx Unused.
 * @return Microseconds (millisecond resolution).
 *
 * @pre ``ra_time_init`` succeeded.
 * @post No global state is modified.
 * @since 0.1.0
 */
static uint64_t pp_demo_now_us(void* ctx)
{
  (void)ctx;
  return (uint64_t)ra_time_ms() * (uint64_t)k_pp_demo_us_per_ms;
}

/**
 * @brief Bring CGC + SysTick + console + profiler up. Panic-halts on fail.
 *
 * @since 0.1.0
 */
/** @brief Bring CGC + SysTick + MSTP + console PFS up; return PCLKA. */
static void pp_demo_clocks_or_halt(uint32_t* out_pclka_hz)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    pp_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    pp_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, out_pclka_hz) != k_ra_ok) {
    pp_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    pp_demo_panic_halt();
  }
  if (ra_mstp_init() != k_ra_ok) {
    pp_demo_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_pp_demo_pin_txd, k_ra_psel_sci_async, "power_profiler.txd8") !=
      k_ra_ok) {
    pp_demo_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_pp_demo_pin_rxd, k_ra_psel_sci_async, "power_profiler.rxd8") !=
      k_ra_ok) {
    pp_demo_panic_halt();
  }
}

/** @brief Bring LPM + power profiler + LEDs up; panic-halts on fail. */
static void pp_demo_modules_or_halt(void)
{
  const ra_lpm_config_t lpm_cfg = {
    .io_port_keep     = false,
    .opa_bus_keep     = true,
    .sscr_fast_return = true,
    .dcdc_softstart   = k_ra_lpm_dcssmode_128us,
    .sscr_low_power   = k_ra_lpm_ss2lp_default,
  };
  if (ra_lpm_init(&lpm_cfg) != k_ra_ok) {
    pp_demo_panic_halt();
  }
  const ra_power_profile_config_t pp_cfg = {
    .pulse         = nullptr,
    .now_us        = pp_demo_now_us,
    .user_ctx_gpio = nullptr,
    .user_ctx_time = nullptr,
  };
  if (ra_power_profile_init(&pp_cfg) != k_ra_ok) {
    pp_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    pp_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    pp_demo_panic_halt();
  }
}

static void pp_demo_setup_or_halt(void)
{
  uint32_t pclka_hz = 0U;
  pp_demo_clocks_or_halt(&pclka_hz);
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_pp_demo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_pp_demo_sci_channel, &sci_cfg) != k_ra_ok) {
    pp_demo_panic_halt();
  }
  pp_demo_modules_or_halt();
}

/**
 * @brief Cycle through ACTIVE -> SLEEP -> DEEP_SLEEP -> SOFTWARE_STANDBY,
 *        marking entry/exit on each.
 *
 * @details
 * Each region is "open" for at least one millisecond so the
 * profiler accumulator records a measurable delta. SOFTWARE_STANDBY
 * is the deepest mode safely entered from this demo because deep-
 * standby resets the CPU and would not return into this loop.
 *
 * @return ``true`` when every enter/exit and ``ra_lpm_enter_sleep``
 *         call returns ``k_ra_ok``.
 *
 * @pre ``ra_lpm_init`` and ``ra_power_profile_init`` succeeded.
 * @post All four region accumulators have one new closed pair.
 * @since 0.1.0
 */
static bool pp_demo_cycle_modes(void)
{
  if (ra_power_profile_mark_enter(k_ra_power_profile_region_active) != k_ra_ok) {
    return false;
  }
  for (volatile uint32_t i = 0U; i < (uint32_t)k_pp_demo_busy_iters; i++) {
    /* Busy spin so the analyser sees a clear ACTIVE shape. */
  }
  if (ra_power_profile_mark_exit(k_ra_power_profile_region_active) != k_ra_ok) {
    return false;
  }

  if (ra_power_profile_mark_enter(k_ra_power_profile_region_sleep) != k_ra_ok) {
    return false;
  }
  if (ra_lpm_enter_sleep(k_ra_sleep_mode_sleep) != k_ra_ok) {
    return false;
  }
  if (ra_power_profile_mark_exit(k_ra_power_profile_region_sleep) != k_ra_ok) {
    return false;
  }

  if (ra_power_profile_mark_enter(k_ra_power_profile_region_deep_standby) != k_ra_ok) {
    return false;
  }
  if (ra_lpm_enter_sleep(k_ra_sleep_mode_deep_sleep) != k_ra_ok) {
    return false;
  }
  if (ra_power_profile_mark_exit(k_ra_power_profile_region_deep_standby) != k_ra_ok) {
    return false;
  }

  if (ra_power_profile_mark_enter(k_ra_power_profile_region_software_standby) != k_ra_ok) {
    return false;
  }
  if (ra_lpm_enter_sleep(k_ra_sleep_mode_software_std) != k_ra_ok) {
    return false;
  }
  if (ra_power_profile_mark_exit(k_ra_power_profile_region_software_standby) != k_ra_ok) {
    return false;
  }
  return true;
}

/**
 * @brief Write a decimal uint64 (low 32 bits) into ``buf``.
 *
 * @param[out] buf Destination (must hold at least 10 chars).
 * @param[in]  v   Value to render.
 * @return Number of ASCII digits written.
 *
 * @pre ``buf`` is non-NULL with >= 10 bytes capacity.
 * @post No null terminator is written.
 * @since 0.1.0
 */
static uint8_t pp_demo_uint_to_dec(uint8_t* buf, uint64_t v)
{
  uint8_t  tmp[k_pp_demo_uint_dec_max];
  uint8_t  n = 0U;
  uint64_t r = v;
  if (r == 0U) {
    buf[0] = (uint8_t)k_pp_demo_ascii_zero;
    return 1U;
  }
  while ((r > 0U) && (n < (uint8_t)k_pp_demo_uint_dec_max)) {
    tmp[n] = (uint8_t)((uint8_t)k_pp_demo_ascii_zero + (uint8_t)(r % (uint64_t)k_pp_demo_dec_base));
    r      = r / (uint64_t)k_pp_demo_dec_base;
    n++;
  }
  for (uint8_t i = 0U; i < n; i++) {
    buf[i] = tmp[(uint8_t)(n - 1U - i)];
  }
  return n;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Profiles power modes once a second.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On clean entry the CPU stays in the profile + blink loop.
 * @post On any HAL hard error LED2 latches ON.
 *
 * @since 0.1.0
 */
/**
 * @brief Append @p tag then the decimal of @p val to @p buf at @p off.
 *
 * @param[in,out] buf Destination buffer.
 * @param[in]     off Current offset (updated in-place).
 * @param[in]     tag NUL-terminated label.
 * @param[in]     val 64-bit decimal value to append.
 * @return New offset after the append.
 *
 * @pre ``buf`` and ``tag`` are non-NULL.
 * @post ``buf[off..]`` contains tag + decimal digits.
 *
 * @since 0.1.0
 */
static uint32_t pp_demo_append(uint8_t* buf, uint32_t off, const char* tag, uint64_t val)
{
  uint32_t i = 0U;
  while (tag[i] != '\0') {
    buf[off++] = (uint8_t)tag[i];
    i++;
  }
  off += pp_demo_uint_to_dec(&buf[off], val);
  return off;
}

/**
 * @brief Format the per-cycle stats line into @p out.
 *
 * @param[out] out   Destination buffer (>= k_pp_demo_print_buf bytes).
 * @param[in]  stats Stats snapshot.
 * @return Number of bytes written.
 *
 * @pre ``out`` is non-NULL with >= k_pp_demo_print_buf capacity.
 * @post No bytes beyond the returned length are touched.
 *
 * @since 0.1.0
 */
static uint32_t pp_demo_format_line(uint8_t* out, const ra_power_profile_stats_t* stats)
{
  uint32_t off = 0U;
  off          = pp_demo_append(out,
                                off,
                                "pp: a=",
                                stats->regions[k_ra_power_profile_region_active].total_time_us);
  off =
    pp_demo_append(out, off, " s=", stats->regions[k_ra_power_profile_region_sleep].total_time_us);
  off = pp_demo_append(out,
                       off,
                       " d=",
                       stats->regions[k_ra_power_profile_region_deep_standby].total_time_us);
  off = pp_demo_append(out,
                       off,
                       " st=",
                       stats->regions[k_ra_power_profile_region_software_standby].total_time_us);
  const uint8_t suffix[] = " us\r\n";
  for (uint32_t i = 0U; i < sizeof(suffix) - 1U; i++) {
    out[off++] = suffix[i];
  }
  return off;
}

int32_t main(void)
{
  pp_demo_setup_or_halt();
  ra_isr_globals_enable();

  while (1) {
    if (!pp_demo_cycle_modes()) {
      (void)ra_board_led_on(k_ra_board_led2);
    }
    ra_power_profile_stats_t stats = {};
    if (ra_power_profile_get_stats(&stats) != k_ra_ok) {
      (void)ra_board_led_on(k_ra_board_led2);
    }
    uint8_t        out[k_pp_demo_print_buf] = {};
    const uint32_t off                      = pp_demo_format_line(out, &stats);
    if (ra_sci_write_polling((uint8_t)k_pp_demo_sci_channel, out, off) != k_ra_ok) {
      break;
    }
    if (ra_board_led_toggle(k_ra_board_led1) != k_ra_ok) {
      break;
    }
    ra_delay_ms(k_pp_demo_period_ms);
  }

  pp_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
