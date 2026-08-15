/**
 * @file ra8_power_profile.c
 * @brief Power-profiling helper -- implementation
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details
 * See ``ra8_power_profile.h`` for the public contract. The implementation
 * is a flat array of accumulators plus two function-pointer hooks
 * (GPIO pulse, microsecond clock). Nothing in this file touches the
 * RA8D2 register map directly; that decoupling keeps the helper
 * trivially testable on the host.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_power_profile.h"

#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"

/**
 * @def RA8_POWER_PROFILE_TAG
 * @brief Logging tag passed to ``ra8_log_*`` from this TU.
 *
 * @details
 * Centralised so all log lines emitted by the profiler use a single,
 * grep-friendly prefix. Allowed by CLAUDE.md "Constants and Macros"
 * because it deduplicates the same string literal across multiple
 * callsites.
 *
 * @since 0.1.0
 */
#define RA8_POWER_PROFILE_TAG "PWRPROF"

/**
 * @var s_initialized
 * @brief Module initialisation flag.
 *
 * @details
 * Set to ``true`` by ``ra8_power_profile_init`` and never cleared.
 * Every public API except ``init`` rejects calls when this is
 * ``false`` via ``RA8_VALIDATE_INIT``.
 *
 * @note Single-threaded access only -- the entire profiler API is
 *       documented as not thread-safe.
 * @warning Direct modification by other TUs is forbidden; use the
 *          public ``ra8_power_profile_init`` entry point.
 *
 * @since 0.1.0
 */
static bool s_initialized = false;

/**
 * @var s_cfg
 * @brief Cached copy of the caller-supplied configuration.
 *
 * @details
 * Holds the GPIO pulse hook, microsecond-clock hook, and the two
 * opaque user contexts. Copied verbatim from
 * ``ra8_power_profile_init``'s argument so the caller is free to
 * release the source buffer afterwards.
 *
 * @note File-scope, accessed only after ``s_initialized == true``.
 * @warning Do not modify outside ``ra8_power_profile_init``.
 *
 * @since 0.1.0
 */
static ra8_power_profile_config_t s_cfg = {};

/**
 * @var s_stats
 * @brief Per-region accumulator array (NASA Rule 3 -- static).
 *
 * @details
 * Indexed by ``ra8_power_profile_region_id_t`` value. Capacity is
 * ``k_ra8_power_profile_max_regions`` (16) so the entire structure
 * fits well within Cortex-M85 SRAM with room to spare.
 *
 * @invariant For every index, ``entries >= exits``.
 * @note File-scope, single-threaded access only.
 * @warning Reset via ``ra8_power_profile_reset_stats``, not by direct
 *          modification.
 *
 * @since 0.1.0
 */
static ra8_power_profile_stats_t s_stats = {};

/**
 * @brief Validate a region ID against the static array bound.
 *
 * @details
 * Pulled out of the public API path so each entry point has the
 * same single-source-of-truth bounds check, and so coverage of the
 * out-of-range branch only needs to be exercised once.
 *
 * @param[in] region_id Region identifier to validate.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                     ``region_id`` is in range.
 * @retval k_ra8_err_range_check_failed ``region_id`` is out of range.
 *
 * @pre None.
 * @post Return value is one of the documented retvals.
 *
 * @note Internal helper; not exported.
 * @since 0.1.0
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 */
RA8_INTERNAL static ra8_err_t internal_validate_region(ra8_power_profile_region_id_t region_id)
{
  if ((uint8_t)region_id >= (uint8_t)k_ra8_power_profile_max_regions) {
    ra8_log_error(RA8_POWER_PROFILE_TAG, "region id out of range");
    return k_ra8_err_range_check_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Read the current timestamp via the configured clock hook.
 *
 * @details
 * Returns 0 if no clock hook was supplied. Centralising the null
 * check here keeps the enter/exit fast paths free of branches that
 * never change after init.
 *
 * @return Current monotonic timestamp in microseconds, or 0 if no
 *         clock hook was configured.
 *
 * @pre Module is initialized.
 * @post Return value is monotonically non-decreasing across calls.
 *
 * @note Internal helper; not exported.
 * @since 0.1.0
 *
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 */
RA8_INTERNAL static uint64_t internal_now_us(void)
{
  if (s_cfg.now_us == nullptr) {
    return 0U;
  }
  return s_cfg.now_us(s_cfg.user_ctx_time);
}

/**
 * @brief Fire the GPIO pulse hook if one was configured.
 *
 * @details
 * Wrapper around the function-pointer dispatch so the public APIs
 * can call a single helper instead of repeating the null guard.
 *
 * @param[in] region_id Region whose edge is being marked.
 * @param[in] entering  ``true`` for an enter edge, ``false`` for exit.
 *
 * @pre Module is initialized.
 * @post Either the hook ran exactly once, or no hook was configured.
 *
 * @note Internal helper; not exported.
 * @since 0.1.0
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 */
RA8_INTERNAL static void internal_fire_pulse(ra8_power_profile_region_id_t region_id, bool entering)
{
  if (s_cfg.pulse == nullptr) {
    return;
  }
  s_cfg.pulse(s_cfg.user_ctx_gpio, region_id, entering);
}

ra8_err_t ra8_power_profile_init(const ra8_power_profile_config_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, RA8_POWER_PROFILE_TAG, "cfg must not be nullptr");

  s_cfg         = *cfg;
  s_stats       = (ra8_power_profile_stats_t){};
  s_initialized = true;
  return k_ra8_ok;
}

ra8_err_t ra8_power_profile_mark_enter(ra8_power_profile_region_id_t region_id)
{
  RA8_VALIDATE_INIT(s_initialized, RA8_POWER_PROFILE_TAG, "init not called");

  const ra8_err_t range_err = internal_validate_region(region_id);
  if (range_err != k_ra8_ok) {
    return range_err;
  }

  ra8_power_profile_region_stats_t* slot = &s_stats.regions[(uint8_t)region_id];
  slot->entries += 1U;
  slot->last_enter_us = internal_now_us();
  slot->is_open       = true;

  internal_fire_pulse(region_id, true);
  return k_ra8_ok;
}

ra8_err_t ra8_power_profile_mark_exit(ra8_power_profile_region_id_t region_id)
{
  RA8_VALIDATE_INIT(s_initialized, RA8_POWER_PROFILE_TAG, "init not called");

  const ra8_err_t range_err = internal_validate_region(region_id);
  if (range_err != k_ra8_ok) {
    return range_err;
  }

  ra8_power_profile_region_stats_t* slot = &s_stats.regions[(uint8_t)region_id];
  slot->exits += 1U;

  ra8_err_t ret = k_ra8_ok;
  if (slot->is_open) {
    const uint64_t now = internal_now_us();
    if (now >= slot->last_enter_us) {
      slot->total_time_us += (now - slot->last_enter_us);
    }
    slot->is_open = false;
  } else {
    ra8_log_error(RA8_POWER_PROFILE_TAG, "exit without matching enter");
    ret = k_ra8_err_invalid_state;
  }

  internal_fire_pulse(region_id, false);
  return ret;
}

ra8_err_t ra8_power_profile_get_stats(ra8_power_profile_stats_t* out_stats)
{
  RA8_VALIDATE_INIT(s_initialized, RA8_POWER_PROFILE_TAG, "init not called");
  RA8_CHECK_NULL_PTR(out_stats, RA8_POWER_PROFILE_TAG, "out_stats must not be nullptr");

  (void)memcpy(out_stats, &s_stats, sizeof(*out_stats));
  return k_ra8_ok;
}

ra8_err_t ra8_power_profile_reset_stats(void)
{
  RA8_VALIDATE_INIT(s_initialized, RA8_POWER_PROFILE_TAG, "init not called");

  s_stats = (ra8_power_profile_stats_t){};
  return k_ra8_ok;
}
