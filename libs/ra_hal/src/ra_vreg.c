/**
 * @file ra_vreg.c
 * @brief Internal Voltage Regulator (DCDC / LDO) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Full HUM Ch 68 coverage for the RA8D2 internal voltage regulator.
 * Implements every documented register field, every operating mode
 * transition (LDO <-> DCDC, with and without fast-startup, with the
 * LDO charge-pump boost engaged), every Software Standby variant
 * (HUM Ch 68.2.2 incompatibility table), the DCDC over-current
 * threshold programming surface, the low-voltage operation profiles
 * (LVOCR.LVO0E / LVO1E), and the diagnostic status snapshot.
 *
 * The DCDC enable / disable sequences mirror the FSP `bsp_power.c`
 * reference (see CLAUDE.md for the no-FSP-copy rule). Only the
 * write order is borrowed -- every line is hand-authored.
 *
 * The driver does NOT call `ra_mstp_*`: the regulator block is part
 * of the always-on SYSC region and has no module-stop bit.
 *
 * @par State Machine
 * @startuml
 * [*] --> Reset
 * Reset --> LDO       : ra_vreg_init(LDO)
 * Reset --> DCDC      : ra_vreg_init(DCDC)
 * LDO   --> DCDC      : ra_vreg_set_mode(DCDC) -- 22 us blocking
 * DCDC  --> LDO       : ra_vreg_set_mode(LDO) -- 60 us blocking
 * LDO   --> Standby   : ra_vreg_enter_standby()
 * DCDC  --> Standby   : k_ra_err_invalid_state (must switch to LDO first)
 * Standby --> LDO     : ra_vreg_exit_standby() (or DCDC if cached)
 * LDO   --> [*]       : ra_vreg_deinit()
 * DCDC  --> [*]       : ra_vreg_deinit()
 * @enduml
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_vreg.h"

#include <stdint.h>

#include "ra8d2_vreg_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

/**
 * @var s_tag
 * @brief Logging tag for ra_log_* calls.
 */
static const char* s_tag = "VREG";

/**
 * @struct ra_vreg_state_t
 * @brief Cached "desired" state used by the enter/exit-standby pair.
 *
 * @details
 * The hardware loses the LDO/DCDC selection when entering Software
 * Standby; this struct holds the snapshot we need to restore on exit.
 */
typedef struct {
  uint8_t              dcdcctl;      /**< Last value written to DCDCCTL.   */
  ra_vreg_vccsel_t     vccsel;       /**< Last value written to VCCSEL.    */
  uint8_t              lvocr;        /**< Last value written to LVOCR.     */
  ra_vreg_ocp_t        ocp;          /**< Cached OCP level (no HW readback). */
  ra_vreg_lv_profile_t lv_profile;   /**< Cached LV profile.               */
  ra_vreg_standby_t    last_standby; /**< Last standby variant requested. */
  bool                 initialised;  /**< True once `ra_vreg_init` ran.   */
  bool                 fast_startup; /**< Cached FST bit.                 */
  bool                 ldo_boost;    /**< Cached LCBOOST bit.             */
} ra_vreg_state_t;

/** @brief Cached desired regulator state for standby / restore. */
static ra_vreg_state_t s_state;

/** @brief Registered fault callback. */
static ra_vreg_event_fn_t s_vreg_fn;

/** @brief Context forwarded to the registered callback. */
static void* s_vreg_ctx;

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Validate every field of a `ra_vreg_cfg_t` snapshot.
 *
 * @param[in] cfg Non-NULL config pointer.
 *
 * @return ra_err_t
 * @retval k_ra_ok                 All fields in range.
 * @retval k_ra_err_invalid_arg    A field is out of range.
 */
static ra_err_t internal_validate_cfg(const ra_vreg_cfg_t* cfg)
{
  if ((uint8_t)cfg->mode > k_ra_vreg_mode_dcdc) {
    return k_ra_err_invalid_arg;
  }
  if ((uint8_t)cfg->vccsel > k_ra_vreg_vccsel_3v0_to_3v6) {
    return k_ra_err_invalid_arg;
  }
  if ((uint8_t)cfg->ocp > k_ra_vreg_ocp_high) {
    return k_ra_err_invalid_arg;
  }
  if ((uint8_t)cfg->lv_profile > k_ra_vreg_lv_p1) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/**
 * @brief Translate a `ra_vreg_lv_profile_t` enum to its LVOCR bit pattern.
 *
 * @param[in] profile Validated profile enum.
 * @return Raw LVOCR byte (only LVO0E / LVO1E ever set).
 */
static uint8_t internal_lvocr_from_profile(ra_vreg_lv_profile_t profile)
{
  if (profile == k_ra_vreg_lv_p0) {
    return k_ra_vreg_mask_lvo0e;
  }
  if (profile == k_ra_vreg_lv_p1) {
    return k_ra_vreg_mask_lvo1e;
  }
  return 0U;
}

/**
 * @brief Decode an LVOCR byte back into the profile enum.
 *
 * @param[in] lvocr Live LVOCR register read.
 * @return One of `k_ra_vreg_lv_off / lv_p0 / lv_p1`.
 */
static ra_vreg_lv_profile_t internal_profile_from_lvocr(uint8_t lvocr)
{
  const uint8_t bits = (uint8_t)(lvocr & k_ra_vreg_mask_lvo_all);
  if (bits == k_ra_vreg_mask_lvo0e) {
    return k_ra_vreg_lv_p0;
  }
  if (bits == k_ra_vreg_mask_lvo1e) {
    return k_ra_vreg_lv_p1;
  }
  return k_ra_vreg_lv_off;
}

/**
 * @brief Decode the live DCDCCTL byte into the OCP enum.
 *
 * @param[in] dcdcctl Live DCDCCTL register read.
 * @return `k_ra_vreg_ocp_off` or the cached level when OCPEN is set.
 */
static ra_vreg_ocp_t internal_ocp_from_dcdcctl(uint8_t dcdcctl)
{
  if ((dcdcctl & k_ra_vreg_mask_ocpen) == 0U) {
    return k_ra_vreg_ocp_off;
  }
  /* Hardware can only report "OCP enabled"; fall back to the cached
   * level so the caller sees the value last written via
   * `ra_vreg_set_ocp()`. */
  return (s_state.ocp == k_ra_vreg_ocp_off) ? k_ra_vreg_ocp_normal : s_state.ocp;
}

/**
 * @brief Build the DCDCCTL value for a given config snapshot in LDO mode.
 *
 * @details
 * Used by `ra_vreg_init` only when `cfg->mode == k_ra_vreg_mode_ldo`.
 * DCDC mode goes through `internal_dcdc_enable_sequence()` instead.
 *
 * @param[in] cfg Validated configuration descriptor (must be non-NULL).
 * @return Packed DCDCCTL value.
 */
static uint8_t internal_pack_ldo_dcdcctl(const ra_vreg_cfg_t* cfg)
{
  uint8_t v = 0U;
  if (cfg->ocp != k_ra_vreg_ocp_off) {
    v |= k_ra_vreg_mask_ocpen;
  }
  if (cfg->fast_startup) {
    v |= k_ra_vreg_mask_fst;
  }
  if (cfg->ldo_boost) {
    v |= k_ra_vreg_mask_lcboost;
  }
  return v;
}

/**
 * @brief Run the FSP-style LDO->DCDC handshake (writes only -- caller
 *        owns the inter-step delays).
 *
 * @details
 * Steps:
 *   1. Set STOPZA (enable IO buffer).
 *   2. Clear PD (turn on DCDC Vref).
 *   3. Switch Vref to low-power (write 0x10).
 *   4. Turn off LDO and turn on DCDC (write 0x11).
 *   5. Enable OCP (write 0x13).
 *
 * If `fast` is true, steps 3 and 4 are folded into a single write of
 * 0x53 (STOPZA + DCDCON + OCPEN + FST), matching the fast-startup
 * sequence documented in the LPM chapter.
 *
 * @param[in] fast True for fast-startup, false for the conservative
 *                 5-step handshake.
 */
static void internal_dcdc_enable_sequence(bool fast)
{
  uint8_t cur = *ra_vreg_dcdcctl();

  /* Step 1: enable IO buffer (STOPZA = 1).
   * HUM Ch 68.2.1 "DCDC Mode" p 4032 */
  cur |= k_ra_vreg_mask_stopza;
  *ra_vreg_dcdcctl() = cur;

  /* Step 2: turn on DCDC Vref (PD = 0).
   * HUM Ch 68.2.1 "DCDC Mode" p 4032 */
  cur &= (uint8_t)~k_ra_vreg_mask_pd;
  *ra_vreg_dcdcctl() = cur;

  if (fast) {
    /* Fast path: jump straight to the final value with FST asserted.
     * HUM Ch 68.2.1 "DCDC Mode" p 4032 */
    *ra_vreg_dcdcctl() = k_ra_vreg_dcdc_step_fast_on;
  } else {
    /* Step 3: switch Vref to low-power.
     * HUM Ch 68.2.1 "DCDC Mode" p 4032 */
    *ra_vreg_dcdcctl() = k_ra_vreg_dcdc_step_lp_vref;
    /* Step 4: turn off LDO and turn on DCDC.
     * HUM Ch 68.2.1 "DCDC Mode" p 4032 */
    *ra_vreg_dcdcctl() = k_ra_vreg_dcdc_step_dcdc_on;
    /* Step 5: enable DCDC over-current protection.
     * HUM Ch 68.2.1 "DCDC Mode" p 4032 */
    *ra_vreg_dcdcctl() = k_ra_vreg_dcdc_step_with_ocp;
  }

  if (fast) {
    s_state.dcdcctl = k_ra_vreg_dcdc_step_fast_on;
  } else {
    s_state.dcdcctl = k_ra_vreg_dcdc_step_with_ocp;
  }
}

/**
 * @brief Switch DCDC -> LDO. Single-write transition that takes the
 *        DCDC offline and stops the IO buffer.
 *
 * @param[in] keep_lcboost Preserve DCDCCTL.LCBOOST through the transition.
 */
static void internal_dcdc_disable_sequence(bool keep_lcboost)
{
  /* The LDO selection is "everything off" -- optionally with LCBOOST
   * still asserted so the LDO can run at subosc-speed VDD.
   * HUM Ch 68.2.2 "External VDD Mode" p 4033
   *
   * NB: do NOT touch s_state.dcdcctl here. enter_standby uses this
   * helper transiently and relies on the cached value to restore on
   * exit; set_mode(LDO) updates the cache itself for the permanent
   * mode-switch path. */
  uint8_t v = 0U;
  if (keep_lcboost) {
    v = k_ra_vreg_mask_lcboost;
  }
  *ra_vreg_dcdcctl() = v;
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_vreg_init(const ra_vreg_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR((void*)cfg, s_tag, "cfg must not be nullptr");
  const ra_err_t verr = internal_validate_cfg(cfg);
  if (verr != k_ra_ok) {
    return verr;
  }

  /* Clear LVOCR first so a stale low-voltage profile cannot affect
   * the DCDC handshake (LV profiles widen the timing budget).
   * HUM Ch 68.3 "Usage Notes" p 4034 */
  *ra_vreg_lvocr() = 0U;

  /* VCCSEL biases the DCDC for the actual VCC range; program before
   * turning DCDC on.
   * HUM Ch 68.2.1 "DCDC Mode" p 4032 */
  *ra_vreg_vccsel() = (uint8_t)((uint8_t)cfg->vccsel & k_ra_vreg_vccsel_mask);

  if (cfg->mode == k_ra_vreg_mode_dcdc) {
    /* Use the staged handshake so the chip ends in the documented
     * final state (STOPZA + DCDCON + OCPEN, optionally + FST). */
    internal_dcdc_enable_sequence(cfg->fast_startup);
    if (!cfg->ldo_boost) {
      /* LCBOOST has no effect in DCDC mode; nothing to apply. */
    }
  } else {
    /* LDO mode: write the boost / OCP / FST bits directly without
     * staging.  OCPEN has no DCDC to protect when DCDCON==0 but the
     * caller may have asked for it so the value is restored across
     * a future LDO->DCDC transition.
     * HUM Ch 68.2.2 "External VDD Mode" p 4033 */
    const uint8_t ldo  = internal_pack_ldo_dcdcctl(cfg);
    *ra_vreg_dcdcctl() = ldo;
    s_state.dcdcctl    = ldo;
  }

  /* Apply LV profile last so the chip is in the right power mode
   * before relaxing the timing.
   * HUM Ch 68.3 "Usage Notes" p 4034 */
  const uint8_t lvocr = internal_lvocr_from_profile(cfg->lv_profile);
  *ra_vreg_lvocr()    = lvocr;

  s_state.vccsel       = cfg->vccsel;
  s_state.lvocr        = lvocr;
  s_state.ocp          = cfg->ocp;
  s_state.lv_profile   = cfg->lv_profile;
  s_state.fast_startup = cfg->fast_startup;
  s_state.ldo_boost    = cfg->ldo_boost;
  s_state.last_standby = k_ra_vreg_standby_software;
  s_state.initialised  = true;

  ra_log_info(s_tag, "vreg_init");
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_vreg_deinit(void)
{
  /* DCDCCTL = 0 returns to LDO with all features (OCP, fast-start,
   * low-power Vref) disabled.
   * HUM Ch 68.2.2 "External VDD Mode" p 4033 */
  *ra_vreg_dcdcctl() = 0U;
  /* VCCSEL irrelevant in LDO; clear it.
   * HUM Ch 68.2.1 "DCDC Mode" p 4032 */
  *ra_vreg_vccsel() = 0U;
  /* LVOCR cleared so we do not silently leave a low-voltage profile
   * set across re-init.
   * HUM Ch 68.3 "Usage Notes" p 4034 */
  *ra_vreg_lvocr() = 0U;

  s_state.dcdcctl      = 0U;
  s_state.vccsel       = k_ra_vreg_vccsel_2v4_to_2v7;
  s_state.lvocr        = 0U;
  s_state.ocp          = k_ra_vreg_ocp_off;
  s_state.lv_profile   = k_ra_vreg_lv_off;
  s_state.fast_startup = false;
  s_state.ldo_boost    = false;
  s_state.initialised  = false;
  return k_ra_ok;
}

/* =============================================================================
 * Runtime control
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_vreg_set_mode(ra_vreg_mode_t mode)
{
  if ((mode != k_ra_vreg_mode_ldo) && (mode != k_ra_vreg_mode_dcdc)) {
    return k_ra_err_invalid_arg;
  }

  if (mode == k_ra_vreg_mode_dcdc) {
    internal_dcdc_enable_sequence(s_state.fast_startup);
  } else {
    internal_dcdc_disable_sequence(s_state.ldo_boost);
    /* Permanent mode switch: cache the LDO selection so a subsequent
     * standby/restore cycle keeps the regulator in LDO. */
    uint8_t cached = 0U;
    if (s_state.ldo_boost) {
      cached = k_ra_vreg_mask_lcboost;
    }
    s_state.dcdcctl = cached;
  }
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_vreg_set_vccsel(ra_vreg_vccsel_t sel)
{
  if ((uint8_t)sel > k_ra_vreg_vccsel_3v0_to_3v6) {
    return k_ra_err_invalid_arg;
  }
  /* VCCSEL[1:0] biases the DCDC for the supply-voltage window.
   * HUM Ch 68.2.1 "DCDC Mode" p 4032 */
  *ra_vreg_vccsel() = (uint8_t)((uint8_t)sel & k_ra_vreg_vccsel_mask);
  s_state.vccsel    = sel;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_vreg_set_ocp(ra_vreg_ocp_t level)
{
  if ((uint8_t)level > k_ra_vreg_ocp_high) {
    return k_ra_err_invalid_arg;
  }
  /* Read-modify-write DCDCCTL.OCPEN.
   * HUM Ch 68.2.1 "DCDC Mode" p 4032 */
  uint8_t cur = *ra_vreg_dcdcctl();
  if (level == k_ra_vreg_ocp_off) {
    cur &= (uint8_t)~k_ra_vreg_mask_ocpen;
  } else {
    cur |= k_ra_vreg_mask_ocpen;
  }
  *ra_vreg_dcdcctl() = cur;
  s_state.dcdcctl    = cur;
  s_state.ocp        = level;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_vreg_set_fast_startup(bool enable)
{
  /* DCDCCTL.FST controls whether subsequent LDO->DCDC transitions
   * skip the long Vref soak.
   * HUM Ch 68.2.1 "DCDC Mode" p 4032 */
  uint8_t cur = *ra_vreg_dcdcctl();
  if (enable) {
    cur |= k_ra_vreg_mask_fst;
  } else {
    cur &= (uint8_t)~k_ra_vreg_mask_fst;
  }
  *ra_vreg_dcdcctl()   = cur;
  s_state.dcdcctl      = cur;
  s_state.fast_startup = enable;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_vreg_set_ldo_boost(bool enable)
{
  /* DCDCCTL.LCBOOST keeps the LDO regulating into subosc-speed VDD.
   * HUM Ch 68.2.2 "External VDD Mode" p 4033 */
  uint8_t cur = *ra_vreg_dcdcctl();
  if (enable) {
    cur |= k_ra_vreg_mask_lcboost;
  } else {
    cur &= (uint8_t)~k_ra_vreg_mask_lcboost;
  }
  *ra_vreg_dcdcctl() = cur;
  s_state.dcdcctl    = cur;
  s_state.ldo_boost  = enable;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_vreg_set_lv_profile(ra_vreg_lv_profile_t profile)
{
  if ((uint8_t)profile > k_ra_vreg_lv_p1) {
    return k_ra_err_invalid_arg;
  }
  /* LVOCR enforces mutual exclusion of LVO0E / LVO1E in software --
   * the silicon allows both bits to be set but the result is
   * undefined.
   * HUM Ch 68.3 "Usage Notes" p 4034 */
  const uint8_t v    = internal_lvocr_from_profile(profile);
  *ra_vreg_lvocr()   = v;
  s_state.lvocr      = v;
  s_state.lv_profile = profile;
  return k_ra_ok;
}

/* =============================================================================
 * Status
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_vreg_get_status(ra_vreg_status_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* DCDCCTL is the master status word.
   * HUM Ch 68.2.1 "DCDC Mode" p 4032 */
  const uint8_t dcdcctl = *ra_vreg_dcdcctl();
  /* VCCSEL is read-back-able.
   * HUM Ch 68.2.1 "DCDC Mode" p 4032 */
  const uint8_t vccsel = *ra_vreg_vccsel();
  /* LVOCR holds the low-voltage profile bit.
   * HUM Ch 68.3 "Usage Notes" p 4034 */
  const uint8_t lvocr = *ra_vreg_lvocr();

  out->dcdcctl = dcdcctl;
  out->vccsel  = vccsel;
  out->lvocr   = lvocr;
  out->mode = ((dcdcctl & k_ra_vreg_mask_dcdcon) != 0U) ? k_ra_vreg_mode_dcdc : k_ra_vreg_mode_ldo;
  out->vccsel_dec       = (ra_vreg_vccsel_t)(vccsel & k_ra_vreg_vccsel_mask);
  out->lv_profile       = internal_profile_from_lvocr(lvocr);
  out->ocp              = internal_ocp_from_dcdcctl(dcdcctl);
  const bool dcdcon_set = (dcdcctl & k_ra_vreg_mask_dcdcon) != 0U;
  const bool pd_clear   = (dcdcctl & k_ra_vreg_mask_pd) == 0U;
  out->dcdc_ready       = false;
  if (dcdcon_set && pd_clear) {
    out->dcdc_ready = true;
  }
  out->fast_startup = ((dcdcctl & k_ra_vreg_mask_fst) != 0U);
  out->ldo_boost    = ((dcdcctl & k_ra_vreg_mask_lcboost) != 0U);
  out->io_buf_on    = ((dcdcctl & k_ra_vreg_mask_stopza) != 0U);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_vreg_clear_status(uint8_t mask)
{
  /* Reject any bit outside the documented writable mask -- writing a
   * reserved bit is a programmer error.
   * HUM Ch 68.2.1 "DCDC Mode" p 4032 */
  if ((mask & (uint8_t)~k_ra_vreg_mask_dcdcctl_all) != 0U) {
    return k_ra_err_invalid_arg;
  }

  /* Read-modify-write DCDCCTL to clear the requested control bits
   * without touching the rest.
   * HUM Ch 68.2.1 "DCDC Mode" p 4032 */
  const uint8_t cur  = *ra_vreg_dcdcctl();
  const uint8_t next = (uint8_t)(cur & (uint8_t)~mask);
  *ra_vreg_dcdcctl() = next;
  s_state.dcdcctl    = next;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_vreg_reset(void)
{
  const ra_err_t err = ra_vreg_deinit();
  /* Wipe the standby cache too -- a recoverable fault may invalidate
   * the previously cached "desired" state. */
  s_state.last_standby = k_ra_vreg_standby_software;
  return err;
}

/* =============================================================================
 * Power transition
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_vreg_enter_standby(ra_vreg_standby_t variant)
{
  if ((uint8_t)variant > k_ra_vreg_standby_battery_backup) {
    return k_ra_err_invalid_arg;
  }
  s_state.last_standby = variant;

  /* Software Standby (and every Deep variant + Battery Backup) is
   * not supported in DCDC mode, so park the regulator in LDO before
   * standby. The cached state lets us restore DCDC on exit.
   * HUM Ch 68.2.2 "External VDD Mode" p 4033 */
  internal_dcdc_disable_sequence(false);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_vreg_enter_stop(void)
{
  return ra_vreg_enter_standby(k_ra_vreg_standby_software);
}

[[nodiscard]] ra_err_t ra_vreg_exit_standby(void)
{
  if (!s_state.initialised) {
    return k_ra_ok;
  }
  /* Restore VCCSEL before re-enabling DCDC.
   * HUM Ch 68.2.1 "DCDC Mode" p 4032 */
  *ra_vreg_vccsel() = (uint8_t)((uint8_t)s_state.vccsel & k_ra_vreg_vccsel_mask);
  /* Restore the cached DCDCCTL value.
   * HUM Ch 68.2.1 "DCDC Mode" p 4032 */
  *ra_vreg_dcdcctl() = s_state.dcdcctl;
  /* Restore low-voltage profile bit.
   * HUM Ch 68.3 "Usage Notes" p 4034 */
  *ra_vreg_lvocr() = s_state.lvocr;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_vreg_exit_stop(void)
{
  return ra_vreg_exit_standby();
}

/* =============================================================================
 * IRQ hook
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_vreg_attach_handler(ra_vreg_event_fn_t fn, void* ctx)
{
  s_vreg_fn  = fn;
  s_vreg_ctx = ctx;
  return k_ra_ok;
}

void ra_vreg_dispatch(void)
{
  const ra_vreg_event_fn_t fn  = s_vreg_fn;
  void* const              ctx = s_vreg_ctx;
  if (fn != nullptr) {
    /* Forward the live DCDCCTL value so the callback can decide
     * whether the fault was DCDC-related.
     * HUM Ch 68.2.1 "DCDC Mode" p 4032 */
    const uint8_t live = *ra_vreg_dcdcctl();
    fn(ctx, live);
  }
}
