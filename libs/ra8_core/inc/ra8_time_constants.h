/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_time_constants.h
 * @brief Named Time Unit Constants for Delay / Timeout / Tick Math
 * @ingroup grp_core
 *
 * @details
 * Collects every magic number related to time (microseconds,
 * milliseconds, Hz) into a single typed-enum vocabulary so firmware
 * code never writes a raw `1000`, `1000000`, or `0.001f`.
 *
 * Example rewrite:
 * @code{.c}
 * // BAD: reader has to decode the arithmetic
 * for (uint32_t i = 0; i < 240 * 1000; i++) { ... }
 *
 * // GOOD: the intent is in the constant names
 * for (uint32_t i = 0; i < k_ra8_cpu_mhz * k_ra8_us_per_ms; i++) { ... }
 * @endcode
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* =============================================================================
 * Time unit conversions
 * =============================================================================
 */

/**
 * @enum ra8_time_unit_t
 * @brief Integer conversion factors between common time units.
 */
typedef enum : uint32_t {
  k_ra8_ns_per_us    = 1000UL,       /**< Nanoseconds per microsecond.  */
  k_ra8_us_per_ms    = 1000UL,       /**< Microseconds per millisecond. */
  k_ra8_ms_per_sec   = 1000UL,       /**< Milliseconds per second.      */
  k_ra8_ns_per_ms    = 1000000UL,    /**< Nanoseconds per millisecond.  */
  k_ra8_us_per_sec   = 1000000UL,    /**< Microseconds per second.      */
  k_ra8_ns_per_sec   = 1000000000UL, /**< Nanoseconds per second.       */
  k_ra8_sec_per_min  = 60UL,         /**< Seconds per minute.           */
  k_ra8_min_per_hour = 60UL,         /**< Minutes per hour.             */
  k_ra8_hour_per_day = 24UL,         /**< Hours per day.                */
  k_ra8_sec_per_hour = 3600UL,       /**< Seconds per hour.             */
  k_ra8_sec_per_day  = 86400UL,      /**< Seconds per day.              */
} ra8_time_unit_t;

/* =============================================================================
 * Clock frequencies (expected post-PLL values for this project)
 * =============================================================================
 */

/**
 * @enum ra8_clock_hz_t
 * @brief Expected clock frequencies after PLL bring-up.
 *
 * @details
 * These are the *target* values the CGC driver programmes. Real-world
 * values are measured at runtime via the CAC (Clock Accuracy Check) and
 * validated to be within +/- 0.1% before any time-critical peripheral
 * is enabled. Adjust here and the CGC driver will follow.
 *
 * @note The main Cortex-M85 core runs at `k_ra8_cpuclk0_hz`; the secondary
 *       Cortex-M33 runs at `k_ra8_cpuclk1_hz` from the same PLL. All
 *       peripheral clocks are derived dividers of the PLL.
 */
typedef enum : uint32_t {
  k_ra8_xtal_hz    = 24000000UL,   /**< 24 MHz main crystal on EK-RA8D2.        */
  k_ra8_loco_hz    = 32768UL,      /**< Low-speed on-chip oscillator.           */
  k_ra8_moco_hz    = 8000000UL,    /**< Middle-speed on-chip oscillator.        */
  k_ra8_hoco_hz    = 20000000UL,   /**< High-speed on-chip oscillator.          */
  k_ra8_pll1p_hz   = 1000000000UL, /**< PLL1P = (24/3)*250/2 = 1000 MHz.        */
  k_ra8_pll1q_hz   = 333333333UL,  /**< PLL1Q = (24/3)*250/6 ~= 333 MHz.        */
  k_ra8_pll1r_hz   = 400000000UL,  /**< PLL1R = (24/3)*250/5 = 400 MHz.         */
  k_ra8_cpuclk0_hz = 1000000000UL, /**< CPUCLK0 = PLL1P/1 = 1 GHz.              */
  k_ra8_cpuclk1_hz = 250000000UL,  /**< CPUCLK1 = PLL1P/4 = 250 MHz.            */
  k_ra8_iclk_hz    = 250000000UL,  /**< ICLK    = PLL1P/4 = 250 MHz.            */
  k_ra8_pclka_hz   = 125000000UL,  /**< PCLKA   = PLL1P/8 = 125 MHz.            */
  k_ra8_pclkb_hz   = 62500000UL,   /**< PCLKB   = PLL1P/16 = 62.5 MHz.          */
  k_ra8_pclkc_hz   = 125000000UL,  /**< PCLKC   = PLL1P/8 = 125 MHz.            */
  k_ra8_pclkd_hz   = 250000000UL,  /**< PCLKD   = PLL1P/4 = 250 MHz.            */
  k_ra8_pclke_hz   = 250000000UL,  /**< PCLKE   = PLL1P/4 = 250 MHz.            */
  k_ra8_bclk_hz    = 125000000UL,  /**< BCLK    = PLL1P/8 = 125 MHz.            */
  k_ra8_fclk_hz    = 125000000UL,  /**< FCK (MRPCLK) = PLL1P/8 = 125 MHz.       */
  k_ra8_mriclk_hz  = 250000000UL,  /**< MRICLK  = PLL1P/4 = 250 MHz (MRAM I/F). */
  /** Highest ICLK the part is rated for. This is a DEVICE LIMIT, not the
   *  configured rate: it is the denominator in HUM Ch 58.3.7 "Wait State"
   *  p 3540, where SRAMWTSC.WTEN must be set once ICLK exceeds half of it.
   *  R7KA8D2KFLCAC is a 250 MHz-ICLK part; the 200 MHz and 150 MHz classes
   *  in that section belong to other RA8 variants. */
  k_ra8_iclk_max_hz = 250000000UL,
} ra8_clock_hz_t;

/* =============================================================================
 * Common timeout values
 * =============================================================================
 */

/**
 * @enum ra8_timeout_ms_t
 * @brief Canonical timeouts used across drivers.
 *
 * @details
 * Centralised so the whole firmware agrees on "short", "default",
 * "long" timeouts. Individual drivers may override for good reason
 * but the default should come from here.
 */
typedef enum : uint32_t {
  k_ra8_timeout_none_ms      = 0UL,     /**< Poll once, never block.  */
  k_ra8_timeout_short_ms     = 10UL,    /**< 10 ms -- quick bus op.   */
  k_ra8_timeout_default_ms   = 100UL,   /**< 100 ms -- default.       */
  k_ra8_timeout_long_ms      = 1000UL,  /**< 1 s -- slow devices.     */
  k_ra8_timeout_very_long_ms = 10000UL, /**< 10 s -- boot / recovery. */
} ra8_timeout_ms_t;

#ifdef __cplusplus
}
#endif
