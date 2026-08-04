/**
 * @file ra8_lvd_runtime.c
 * @brief PVD / LVD runtime control: IRQ / reset / CMPE toggles, filter,
 *        hysteresis, negate, and status read / clear.
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Companion translation unit to `ra8_lvd.c`. Holds the single-bit CR0
 * toggles (RIE / RI / RE / CMPE), the runtime filter / RHSEL / RN
 * updaters, and the PVDmSR status read / clear path. The shared
 * channel-map descriptor table and the cross-TU register helpers it
 * calls live in `ra8_lvd_internal.h`; every register access keeps its
 * HUM Ch 8 citation (R01UH1065EJ rev 1.30).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_lvd.h"
#include "ra8_lvd_internal.h"
#include "ra8_lvd_regs.h"

/**
 * @var s_tag
 * @brief Logging tag used by every error path in this TU.
 *
 * @details Private per-TU copy of the driver-wide "LVD" log tag.
 * @note PRIVATE.
 * @warning Do not modify at runtime.
 * @since 0.1.0
 */
static const char* s_tag = "LVD";

/* =============================================================================
 * IRQ / reset / CMPE single-bit toggles
 * =============================================================================
 */

/**
 * @brief Set CR0.RIE on an IRQ-capable PVDm channel.
 *
 * @details
 * Enables IRQ delivery via the standard CR0 RMW path so the reserved
 * bit-3 stays set (HUM Ch 12.2.4 "PVDmCR0" p 597).
 *
 * @param[in] channel PVDm channel id.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                IRQ enabled.
 * @retval k_ra8_err_invalid_arg   Channel mapping failed.
 * @retval k_ra8_err_not_supported Channel has no IRQ path.
 *
 * @pre Channel previously brought up via ``ra8_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post CR0.RIE reads as 1.
 * @post Reserved-bit invariant for the channel is preserved.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_lvd_enable_irq(ra8_lvd_channel_t channel)
{
  uint8_t         idx     = 0U;
  const ra8_err_t map_err = ra8_lvd_internal_channel_to_idx(channel, &idx);
  RA8_RETURN_ON_ERROR(map_err, s_tag, "lvd_enable_irq: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra8_lvd_channel_map_t map = s_lvd_map[idx];
  if (!map.has_irq) {
    return k_ra8_err_not_supported;
  }
  /* set RIE. */
  /* HUM Ch 8.2.4 "PVDmCR0 : Voltage Monitor m Circuit Control Register 0" p 305 */
  ra8_lvd_internal_cr0_rmw(&map, 0U, k_ra8_lvd_cr0_mask_rie);
  return k_ra8_ok;
}

/**
 * @brief Clear CR0.RIE on an IRQ-capable PVDm channel.
 *
 * @details
 * Disables IRQ delivery via the standard CR0 RMW path so the reserved
 * bit-3 stays set (HUM Ch 12.2.4 "PVDmCR0" p 597).
 *
 * @param[in] channel PVDm channel id.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                IRQ disabled.
 * @retval k_ra8_err_invalid_arg   Channel mapping failed.
 * @retval k_ra8_err_not_supported Channel has no IRQ path.
 *
 * @pre Channel previously brought up via ``ra8_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post CR0.RIE reads as 0.
 * @post Reserved-bit invariant for the channel is preserved.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_lvd_disable_irq(ra8_lvd_channel_t channel)
{
  uint8_t         idx     = 0U;
  const ra8_err_t map_err = ra8_lvd_internal_channel_to_idx(channel, &idx);
  RA8_RETURN_ON_ERROR(map_err, s_tag, "lvd_disable_irq: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra8_lvd_channel_map_t map = s_lvd_map[idx];
  if (!map.has_irq) {
    return k_ra8_err_not_supported;
  }
  /* HUM Ch 8.2.4 "PVDmCR0" p 305 */
  ra8_lvd_internal_cr0_rmw(&map, k_ra8_lvd_cr0_mask_rie, 0U);
  return k_ra8_ok;
}

/**
 * @brief Arm the reset response for a PVD channel.
 *
 * @details
 * For PVDm channels sets RI + RIE; for PVDn channels sets RE (the
 * only response option). See HUM Ch 12.2.4 / 12.2.5 (pp 597-598).
 *
 * @param[in] channel PVD channel id.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Reset response armed.
 * @retval k_ra8_err_invalid_arg   Channel mapping failed.
 *
 * @pre Channel previously brought up via ``ra8_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post Channel will reset the SoC on the configured crossing.
 * @post Reserved-bit invariant for the channel is preserved.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_lvd_enable_reset(ra8_lvd_channel_t channel)
{
  uint8_t         idx     = 0U;
  const ra8_err_t map_err = ra8_lvd_internal_channel_to_idx(channel, &idx);
  RA8_RETURN_ON_ERROR(map_err, s_tag, "lvd_enable_reset: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra8_lvd_channel_map_t map = s_lvd_map[idx];
  if (map.has_irq) {
    /* m channel: set RI then RIE. */
    /* HUM Ch 8.2.4 "PVDmCR0" p 305 */
    ra8_lvd_internal_cr0_rmw(&map, 0U, (uint8_t)(k_ra8_lvd_cr0_mask_ri | k_ra8_lvd_cr0_mask_rie));
  } else {
    /* n channel: set RE only -- it is the only response option. */
    /* HUM Ch 8.2.5 "PVDnCR0" p 306 */
    ra8_lvd_internal_cr0_rmw(&map, 0U, k_ra8_lvd_cr0_mask_re);
  }
  return k_ra8_ok;
}

/**
 * @brief Disarm the reset response for a PVD channel.
 *
 * @details
 * For PVDm channels clears RI; for PVDn channels clears RE. See HUM
 * Ch 12.2.4 / 12.2.5 (pp 597-598).
 *
 * @param[in] channel PVD channel id.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Reset response disarmed.
 * @retval k_ra8_err_invalid_arg   Channel mapping failed.
 *
 * @pre Channel previously brought up via ``ra8_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post Channel will not reset the SoC on subsequent crossings.
 * @post Reserved-bit invariant for the channel is preserved.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_lvd_disable_reset(ra8_lvd_channel_t channel)
{
  uint8_t         idx     = 0U;
  const ra8_err_t map_err = ra8_lvd_internal_channel_to_idx(channel, &idx);
  RA8_RETURN_ON_ERROR(map_err, s_tag, "lvd_disable_reset: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra8_lvd_channel_map_t map = s_lvd_map[idx];
  /* HUM Ch 8.2.4 "PVDmCR0" p 305 */
  uint8_t enable_bit = k_ra8_lvd_cr0_mask_re;
  if (map.has_irq) {
    enable_bit = k_ra8_lvd_cr0_mask_rie;
  }
  ra8_lvd_internal_cr0_rmw(&map, enable_bit, 0U);
  return k_ra8_ok;
}

/**
 * @brief Set CR0.CMPE so the comparator output drives the response logic.
 *
 * @details
 * Per HUM Ch 12.2.4 (p 597) CMPE gates whether a detected crossing
 * fans out to RIE/RE/RI; this helper toggles CMPE alone so the
 * caller can stage the bring-up.
 *
 * @param[in] channel PVD channel id.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               CMPE asserted.
 * @retval k_ra8_err_invalid_arg  Channel mapping failed.
 *
 * @pre Channel previously brought up via ``ra8_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post CR0.CMPE reads as 1.
 * @post Reserved-bit invariant for the channel is preserved.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_lvd_enable_cmpe(ra8_lvd_channel_t channel)
{
  uint8_t         idx     = 0U;
  const ra8_err_t map_err = ra8_lvd_internal_channel_to_idx(channel, &idx);
  RA8_RETURN_ON_ERROR(map_err, s_tag, "lvd_enable_cmpe: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra8_lvd_channel_map_t map = s_lvd_map[idx];
  /* HUM Ch 8.2.4 "PVDmCR0" p 305 */
  ra8_lvd_internal_cr0_rmw(&map, 0U, k_ra8_lvd_cr0_mask_cmpe);
  return k_ra8_ok;
}

/**
 * @brief Clear CR0.CMPE so the comparator stops driving the response logic.
 *
 * @details
 * Per HUM Ch 12.2.4 (p 597), used during shutdown or reconfiguration
 * to silence the channel's response path without dropping PVDE.
 *
 * @param[in] channel PVD channel id.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               CMPE cleared.
 * @retval k_ra8_err_invalid_arg  Channel mapping failed.
 *
 * @pre Channel previously brought up via ``ra8_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post CR0.CMPE reads as 0.
 * @post Reserved-bit invariant for the channel is preserved.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_lvd_disable_cmpe(ra8_lvd_channel_t channel)
{
  uint8_t         idx     = 0U;
  const ra8_err_t map_err = ra8_lvd_internal_channel_to_idx(channel, &idx);
  RA8_RETURN_ON_ERROR(map_err, s_tag, "lvd_disable_cmpe: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra8_lvd_channel_map_t map = s_lvd_map[idx];
  /* HUM Ch 8.2.4 "PVDmCR0" p 305 */
  ra8_lvd_internal_cr0_rmw(&map, k_ra8_lvd_cr0_mask_cmpe, 0U);
  return k_ra8_ok;
}

/* =============================================================================
 * Filter + RHSEL + RN runtime updates
 * =============================================================================
 */

/**
 * @brief Update FSAMP and DFDIS for a PVD channel at runtime.
 *
 * @details
 * FSAMP can only be modified while DFDIS = 1 (HUM Ch 12.2.4 / 12.2.5
 * pp 597-598). This helper temporarily sets DFDIS, writes FSAMP, and
 * drops DFDIS back when ``filter_en`` is true.
 *
 * @param[in] channel    PVD channel id.
 * @param[in] filter_div New FSAMP divider.
 * @param[in] filter_en  True to enable the digital filter.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Filter updated.
 * @retval k_ra8_err_invalid_arg   Channel or divider out of range.
 *
 * @pre Channel previously brought up via ``ra8_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post CR0.FSAMP reflects ``filter_div``.
 * @post CR0.DFDIS reflects the inverse of ``filter_en``.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t
ra8_lvd_set_filter(ra8_lvd_channel_t channel, ra8_lvd_loco_div_t filter_div, bool filter_en)
{
  uint8_t         idx     = 0U;
  const ra8_err_t map_err = ra8_lvd_internal_channel_to_idx(channel, &idx);
  RA8_RETURN_ON_ERROR(map_err, s_tag, "lvd_set_filter: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra8_err_t div_err = ra8_lvd_internal_validate_div(filter_div);
  RA8_RETURN_ON_ERROR(div_err, s_tag, "lvd_set_filter: bad div"); /* GCOVR_EXCL_BR_LINE */

  const ra8_lvd_channel_map_t map = s_lvd_map[idx];

  /* HUM Ch 8.2.4 "PVDmCR0" p 305 / HUM Ch 8.2.5 "PVDnCR0" p 306
   * -- FSAMP can only be modified while DFDIS = 1. */
  ra8_lvd_internal_cr0_rmw(&map, 0U, k_ra8_lvd_cr0_mask_dfdis);

  const uint8_t fsamp_bits =
    (uint8_t)(((uint8_t)filter_div << k_ra8_lvd_cr0_shift_fsamp) & k_ra8_lvd_cr0_mask_fsamp);
  ra8_lvd_internal_cr0_rmw(&map, k_ra8_lvd_cr0_mask_fsamp, fsamp_bits);

  if (filter_en) {
    /* Drop DFDIS only after FSAMP has landed. */
    ra8_lvd_internal_cr0_rmw(&map, k_ra8_lvd_cr0_mask_dfdis, 0U);
  }
  return k_ra8_ok;
}

/**
 * @brief Update FCR.RHSEL (hysteresis mode) for a PVD channel.
 *
 * @details
 * Per HUM Ch 12.2.8 "PVDmFCR" p 600 RHSEL must not be set to 1 when
 * PVDmCR0.RI = 0 on m channels. RHSEL can only be modified when
 * every PVDE is 0; this helper preserves and restores the channel's
 * own PVDE around the write.
 *
 * @param[in] channel PVD channel id.
 * @param[in] hyst    New hysteresis mode.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  Hysteresis mode updated.
 * @retval k_ra8_err_invalid_arg     Channel or mode out of range.
 * @retval k_ra8_err_invalid_state   HVD requested with RI=0 (m channel only).
 *
 * @pre Channel previously brought up via ``ra8_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post FCR.RHSEL reflects ``hyst``.
 * @post CMPCR.PVDE state is preserved across the write.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_lvd_set_hysteresis_mode(ra8_lvd_channel_t channel, ra8_lvd_hysteresis_t hyst)
{
  uint8_t         idx     = 0U;
  const ra8_err_t map_err = ra8_lvd_internal_channel_to_idx(channel, &idx);
  RA8_RETURN_ON_ERROR(map_err,
                      s_tag,
                      "lvd_set_hysteresis_mode: bad channel"); /* GCOVR_EXCL_BR_LINE */

  if ((uint8_t)hyst > k_ra8_lvd_hysteresis_hvd) {
    return k_ra8_err_invalid_arg;
  }

  const ra8_lvd_channel_map_t map = s_lvd_map[idx];

  /* HUM Ch 8.2.8 "PVDmFCR" p 308 last bullet -- "RHSEL must not be set
   * to 1 when PVDmCR0.RI = 0." Enforce only on m channels (n channels
   * have no RI bit; their RHSEL is always legal). */
  if (map.has_irq && (hyst == k_ra8_lvd_hysteresis_hvd)) {
    if (ra8_lvd_internal_read_ri(&map) == 0U) {
      return k_ra8_err_invalid_state;
    }
  }

  /* HUM Ch 8.2.8 / 8.2.9 -- RHSEL can only be modified when every
   * PVDE is 0; preserve and restore this channel's PVDE. */
  const uint8_t prev_cmpcr = *ra8_lvd_reg8(map.cmpcr);
  const uint8_t pvde_was   = (uint8_t)(prev_cmpcr & k_ra8_lvd_cmpcr_mask_pvde);
  *ra8_lvd_reg8(map.cmpcr) = (uint8_t)(prev_cmpcr & (uint8_t)~k_ra8_lvd_cmpcr_mask_pvde);

  /* HUM Ch 8.2.8 "PVDmFCR" p 308 */
  *ra8_lvd_reg8(map.fcr) = (uint8_t)((uint8_t)hyst & k_ra8_lvd_fcr_mask_rhsel);

  /* Restore PVDE if it had been set. */
  *ra8_lvd_reg8(map.cmpcr) = (uint8_t)(*ra8_lvd_reg8(map.cmpcr) | pvde_was);
  return k_ra8_ok;
}

/**
 * @brief Update CR0.RN (negate-after-assert) for a PVDm channel.
 *
 * @details
 * Per HUM Ch 12.2.4 "PVDmCR0" p 597, RN=1 is rejected when RHSEL=1
 * (HVD mode). This helper enforces the constraint and writes RN.
 *
 * @param[in] channel PVDm channel id.
 * @param[in] negate  Negation behaviour.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  RN updated.
 * @retval k_ra8_err_invalid_arg     Channel or mode out of range.
 * @retval k_ra8_err_not_supported   Channel has no IRQ path (PVDn).
 * @retval k_ra8_err_invalid_state   RN=1 conflicts with FCR.RHSEL=1.
 *
 * @pre Channel previously brought up via ``ra8_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post CR0.RN reflects ``negate``.
 * @post Reserved-bit invariant for the channel is preserved.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_lvd_set_negate_mode(ra8_lvd_channel_t channel, ra8_lvd_negate_t negate)
{
  uint8_t         idx     = 0U;
  const ra8_err_t map_err = ra8_lvd_internal_channel_to_idx(channel, &idx);
  RA8_RETURN_ON_ERROR(map_err, s_tag, "lvd_set_negate_mode: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra8_lvd_channel_map_t map = s_lvd_map[idx];
  if (!map.has_irq) {
    return k_ra8_err_not_supported;
  }
  if ((uint8_t)negate > k_ra8_lvd_negate_after_assert) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 8.2.4 "PVDmCR0" p 306 */
  /* HUM Ch 8.2.8 "PVDmFCR" p 308 */
  if (negate == k_ra8_lvd_negate_after_assert) {
    const uint8_t fcr = *ra8_lvd_reg8(map.fcr);
    if ((fcr & k_ra8_lvd_fcr_mask_rhsel) != 0U) {
      return k_ra8_err_invalid_state;
    }
  }

  /* HUM Ch 8.2.4 "PVDmCR0" p 305 */
  ra8_lvd_internal_cr0_rmw(&map,
                           k_ra8_lvd_cr0_mask_rn,
                           (negate == k_ra8_lvd_negate_after_assert) ? k_ra8_lvd_cr0_mask_rn : 0U);
  return k_ra8_ok;
}

/* =============================================================================
 * Status read / clear
 * =============================================================================
 */

/**
 * @brief Read PVDmSR and decode DET / MON into ::ra8_lvd_status_t.
 *
 * @details
 * Reads the status register documented at HUM Ch 12.2.7 "PVDmSR"
 * p 600 and unpacks the latched-crossing (DET) and live-comparator
 * (MON) bits.
 *
 * @param[in]  channel PVDm channel id.
 * @param[out] out     Receives the decoded status.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  Status decoded.
 * @retval k_ra8_err_null_ptr        ``out`` was NULL.
 * @retval k_ra8_err_invalid_arg     Channel mapping failed.
 * @retval k_ra8_err_not_supported   Channel has no IRQ path (PVDn).
 *
 * @pre ``out`` non-NULL.
 * @pre Channel previously brought up via ``ra8_lvd_channel_init``.
 * @post ``*out`` reflects the live SR contents.
 * @post Hardware state is unchanged.
 *
 * @note Read-only; safe under simple races.
 * @since 0.1.0
 */
ra8_err_t ra8_lvd_get_status(ra8_lvd_channel_t channel, ra8_lvd_status_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");

  uint8_t         idx     = 0U;
  const ra8_err_t map_err = ra8_lvd_internal_channel_to_idx(channel, &idx);
  RA8_RETURN_ON_ERROR(map_err, s_tag, "lvd_get_status: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra8_lvd_channel_map_t map = s_lvd_map[idx];
  if (!map.has_irq) {
    return k_ra8_err_not_supported;
  }

  /* HUM Ch 8.2.7 "PVDmSR : Voltage Monitor m Circuit Status Register" p 307 */
  const uint8_t sr = *ra8_lvd_reg8(map.sr);
  out->crossed     = ((sr & k_ra8_lvd_sr_mask_det) != 0U);
  out->above       = ((sr & k_ra8_lvd_sr_mask_mon) != 0U);
  return k_ra8_ok;
}

/**
 * @brief Acknowledge PVDmSR.DET via write-0-to-clear.
 *
 * @details
 * Per HUM Ch 12.2.7 "PVDmSR" p 600 Note 1, only 0 can be written to
 * DET; MON is preserved by clearing only the DET bit.
 *
 * @param[in] channel PVDm channel id.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  DET acknowledged.
 * @retval k_ra8_err_invalid_arg     Channel mapping failed.
 * @retval k_ra8_err_not_supported   Channel has no IRQ path (PVDn).
 *
 * @pre Channel previously brought up via ``ra8_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post SR.DET reads as 0.
 * @post SR.MON is unchanged.
 *
 * @note Not thread-safe (read-modify-write on SR).
 * @since 0.1.0
 */
ra8_err_t ra8_lvd_clear_status(ra8_lvd_channel_t channel)
{
  uint8_t         idx     = 0U;
  const ra8_err_t map_err = ra8_lvd_internal_channel_to_idx(channel, &idx);
  RA8_RETURN_ON_ERROR(map_err, s_tag, "lvd_clear_status: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra8_lvd_channel_map_t map = s_lvd_map[idx];
  if (!map.has_irq) {
    return k_ra8_err_not_supported;
  }

  /* HUM Ch 8.2.7 "PVDmSR : Voltage Monitor m Circuit Status Register" p 307
   * Note 1 -- only 0 can be written to DET. MON is preserved by
   * clearing only the DET bit. */
  const uint8_t sr      = *ra8_lvd_reg8(map.sr);
  const uint8_t next    = (uint8_t)(sr & (uint8_t)~k_ra8_lvd_sr_mask_det);
  *ra8_lvd_reg8(map.sr) = next;
  return k_ra8_ok;
}
