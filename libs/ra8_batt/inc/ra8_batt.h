/**
 * @file ra8_batt.h
 * @brief Bounded, allocation-free low-battery nag policy.
 * @ingroup grp_board
 *
 * @details
 * `ra8_batt` decides *when* to raise a low-battery warning from a stream of
 * state-of-charge (SOC) readings. It is geometry-free and hardware-free: a
 * caller feeds it the percent + charge flag each tick (e.g. from a MAX17048
 * fuel gauge read over `ra8_smbus`) and it returns the nag to surface, if any.
 * The display / UART / e-reader chrome layer on top decide how to show it.
 *
 * The policy is **edge-triggered with hysteresis**, so a steady or jittering
 * battery does not spam the user:
 *   - A *low* nag (::k_ra8_batt_nag_low) fires once when SOC first falls to
 *     ::k_ra8_batt_low_pct (20%) or below.
 *   - A *critical* nag (::k_ra8_batt_nag_critical) fires once when SOC first
 *     falls to ::k_ra8_batt_critical_pct (10%) or below.
 *   - Each band re-arms (so it can fire again) only after SOC recovers above
 *     its threshold by ::k_ra8_batt_rearm_margin (hysteresis), or while the
 *     battery is charging. While charging, no nag fires.
 *
 * The decision is a single forward step with no loop, no recursion, and no
 * allocation (NASA P10 Rules 1-3), so it runs identically on the host test
 * harness and on the RA8D2.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 5 / UI]
 * {World: NS}
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_batt_nag_t
 * @brief The low-battery warning a single ::ra8_batt_update step raises.
 *
 * @details Returned by value through ::ra8_batt_update. ::k_ra8_batt_nag_none
 * means no new edge this step (the common case); the other two are the
 * one-shot warnings for the low and critical bands.
 */
typedef enum : uint8_t {
  k_ra8_batt_nag_none     = 0U, /**< No new warning this step.              */
  k_ra8_batt_nag_low      = 1U, /**< SOC fell to the low band (<=20%).      */
  k_ra8_batt_nag_critical = 2U, /**< SOC fell to the critical band (<=10%). */
} ra8_batt_nag_t;

/**
 * @enum ra8_batt_threshold_t
 * @brief Nag thresholds and hysteresis margin, in whole percent.
 *
 * @details The bands are inclusive at and below their threshold; a band
 * re-arms once SOC rises strictly above (threshold + ::k_ra8_batt_rearm_margin)
 * so a battery hovering at the boundary does not re-nag every step.
 */
typedef enum : uint8_t {
  k_ra8_batt_low_pct      = 20U,  /**< Low-battery threshold (inclusive).    */
  k_ra8_batt_critical_pct = 10U,  /**< Critical threshold (inclusive).       */
  k_ra8_batt_rearm_margin = 3U,   /**< Hysteresis above a band to re-arm it. */
  k_ra8_batt_pct_max      = 100U, /**< SOC clamp ceiling.                    */
} ra8_batt_threshold_t;

/**
 * @struct ra8_batt_monitor_t
 * @brief Caller-owned nag state carried across ::ra8_batt_update calls.
 *
 * @details Zero-initialise with ::ra8_batt_monitor_init before the first
 * update. The two flags record which bands have already nagged and not yet
 * re-armed, so each warning is one-shot per descent into its band.
 *
 * @invariant A flag is true only between the band's nag and its re-arm.
 */
typedef struct {
  bool low_raised;      /**< Low nag raised and not yet re-armed.      */
  bool critical_raised; /**< Critical nag raised and not yet re-armed. */
} ra8_batt_monitor_t;

/**
 * @brief Reset a nag monitor to the un-nagged, fully-armed state.
 *
 * @details Writes zero to both band-raised flags inside @p mon, placing the
 * monitor into the fully-armed state so that the first subsequent call to
 * ::ra8_batt_update can raise either the low or critical nag. The operation is
 * a single struct clear with no loops, no allocation, and no hardware access,
 * making it safe to call from any initialisation context on both the RA8D2
 * target and the host unit-test harness.
 *
 * @param[out] mon Monitor to initialise.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Monitor zeroed; both bands armed.
 * @retval k_ra8_err_null_ptr @p mon is nullptr.
 *
 * @pre @p mon points to writable storage.
 * @pre Called before the first ::ra8_batt_update on this monitor.
 * @post Both band flags are false (armed).
 * @post Subsequent ::ra8_batt_update can raise either band.
 *
 * @note Not thread-safe; one monitor per single-threaded reader.
 * @see ra8_batt_update()
 * @since 0.1.0
 */
ra8_err_t ra8_batt_monitor_init(ra8_batt_monitor_t* mon);

/**
 * @brief Fold one SOC reading into the monitor and report the nag to raise.
 *
 * @details Re-arms each band when charging or when @p soc_pct has recovered
 * above the band threshold by ::k_ra8_batt_rearm_margin, then, only while not
 * charging, raises the low and/or critical band the reading newly enters. When
 * a reading enters both bands at once (a large drop), the returned nag is the
 * worse one (::k_ra8_batt_nag_critical). @p soc_pct above ::k_ra8_batt_pct_max is
 * clamped.
 *
 * @param[in,out] mon      Monitor state (updated in place).
 * @param[in]     soc_pct  Current state-of-charge percent (clamped to 0..100).
 * @param[in]     charging True when the fuel gauge reports a charging rate.
 * @param[out]    out_nag  Receives the nag to surface this step.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Step folded; @p out_nag written.
 * @retval k_ra8_err_null_ptr @p mon or @p out_nag is nullptr.
 *
 * @pre @p mon was initialised by ::ra8_batt_monitor_init.
 * @pre @p out_nag points to writable storage.
 * @post @p out_nag holds one of ::ra8_batt_nag_t.
 * @post A band that fired is marked raised until it re-arms.
 *
 * @note Not thread-safe; one monitor per single-threaded reader.
 * @see ra8_batt_monitor_init()
 * @since 0.1.0
 */
ra8_err_t
ra8_batt_update(ra8_batt_monitor_t* mon, uint8_t soc_pct, bool charging, ra8_batt_nag_t* out_nag);

/**
 * @brief Map a nag level to a short, stable upper-case label.
 *
 * @param[in] nag Nag level.
 *
 * @return A static NUL-terminated label.
 * @retval "OK"       For ::k_ra8_batt_nag_none.
 * @retval "LOW"      For ::k_ra8_batt_nag_low.
 * @retval "CRITICAL" For ::k_ra8_batt_nag_critical.
 * @retval "?"        For any out-of-range value.
 *
 * @pre None.
 * @post The returned pointer is non-null (asserted) and immutable.
 *
 * @note Thread-safe (returns a pointer to static storage).
 * @since 0.1.0
 */
const char* ra8_batt_nag_str(ra8_batt_nag_t nag);

#ifdef __cplusplus
}
#endif
