/**
 * @file ra8_lvd.c
 * @brief Programmable Voltage Detection (PVD / LVD) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Round-3 driver implementing the **full** PVD surface defined in
 * `ra8_lvd.h`. Every register access carries a HUM Ch 8 citation
 * (R01UH1065EJ rev 1.30).
 *
 * The PVD block has no module-stop bit -- it is part of the always-on
 * SYSC region -- so this driver does **not** call `ra8_mstp_enable`.
 * Per HUM 8.2.* register notes, every control-register write requires
 * PRCR.PRC3 = 1; the driver does NOT take the lock itself (callers
 * must wrap each call), but the host-side fake mmap has no lock
 * so unit tests still see writes land.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_lvd.h"

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_lvd_internal.h"
#include "ra8_lvd_regs.h"

/**
 * @brief Pure RN+RHSEL reject predicate -- see header for full contract.
 * @details Promoted helper so the line-494 AND can be driven under MC/DC.
 * @param[in] hvd_val          Numeric value of @c k_ra8_lvd_hysteresis_hvd.
 * @param[in] after_assert_val Numeric value of @c k_ra8_lvd_negate_after_assert.
 * @param[in] hysteresis       Candidate hysteresis value.
 * @param[in] negate           Candidate negate value.
 * @return Boolean reject predicate.
 * @retval true  Caller returns @c k_ra8_err_invalid_arg.
 * @retval false Combination is allowed.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool ra8_lvd_internal_reject_hvd_after(uint32_t hvd_val,
                                       uint32_t after_assert_val,
                                       uint32_t hysteresis,
                                       uint32_t negate)
{
  return (hysteresis == hvd_val) && (negate == after_assert_val);
}

/**
 * @brief Pure RI-bit predicate -- see header for full contract.
 * @details Promoted helper so the line-533 OR can be driven under MC/DC.
 * @param[in] reset_val         Numeric value of @c k_ra8_lvd_response_reset.
 * @param[in] reset_on_rise_val Numeric value of @c k_ra8_lvd_response_reset_on_rise.
 * @param[in] response          Candidate response value.
 * @return Boolean predicate.
 * @retval true  Caller ORs RI bit into CR0.
 * @retval false RI bit stays clear.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool ra8_lvd_internal_set_ri_bit(uint32_t reset_val, uint32_t reset_on_rise_val, uint32_t response)
{
  return (response == reset_val) || (response == reset_on_rise_val);
}
#include "ra8_log.h"

/**
 * @var s_tag
 * @brief Logging tag used by every error path in this TU.
 */
static const char* s_tag = "LVD";

/* =============================================================================
 * Channel-map helpers
 * =============================================================================
 */

/**
 * @var s_lvd_map
 * @brief Lookup table from ``ra8_lvd_map_idx_t`` to register offsets.
 *
 * @details
 * Single definition for the whole ra8_lvd driver. The descriptor type,
 * its index enum, and the matching `extern` declaration live in
 * ``ra8_lvd_internal.h`` so the runtime / events translation units can
 * read the same table.
 *
 * @note PRIVATE to the ra8_lvd driver TUs.
 * @warning Read-only; do not mutate.
 * @since 0.1.0
 */
const ra8_lvd_channel_map_t s_lvd_map[k_ra8_lvd_map_idx_count] = {
  {.cmpcr   = k_ra8_lvd_pvd1_cmpcr_off,
   .cr0     = k_ra8_lvd_pvd1_cr0_off,
   .cr1     = k_ra8_lvd_pvd1_cr1_off,
   .sr      = k_ra8_lvd_pvd1_sr_off,
   .fcr     = k_ra8_lvd_pvd1_fcr_off,
   .has_irq = true},
  {.cmpcr   = k_ra8_lvd_pvd2_cmpcr_off,
   .cr0     = k_ra8_lvd_pvd2_cr0_off,
   .cr1     = k_ra8_lvd_pvd2_cr1_off,
   .sr      = k_ra8_lvd_pvd2_sr_off,
   .fcr     = k_ra8_lvd_pvd2_fcr_off,
   .has_irq = true},
  {.cmpcr   = k_ra8_lvd_pvd4_cmpcr_off,
   .cr0     = k_ra8_lvd_pvd4_cr0_off,
   .cr1     = k_ra8_lvd_pvd4_cr0_off, /* unused -- n channels have no CR1. */
   .sr      = k_ra8_lvd_pvd4_cr0_off, /* unused -- n channels have no SR.  */
   .fcr     = k_ra8_lvd_pvd4_fcr_off,
   .has_irq = false},
  {.cmpcr   = k_ra8_lvd_pvd5_cmpcr_off,
   .cr0     = k_ra8_lvd_pvd5_cr0_off,
   .cr1     = k_ra8_lvd_pvd5_cr0_off,
   .sr      = k_ra8_lvd_pvd5_cr0_off,
   .fcr     = k_ra8_lvd_pvd5_fcr_off,
   .has_irq = false},
};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/** @brief Implementation of `ra8_lvd_internal_channel_to_idx()` -- switch on PVD id. */
ra8_err_t ra8_lvd_internal_channel_to_idx(ra8_lvd_channel_t channel, uint8_t* out_idx)
{
  switch (channel) {
    case k_ra8_lvd_ch1:
      *out_idx = k_ra8_lvd_map_idx_ch1;
      return k_ra8_ok;
    case k_ra8_lvd_ch2:
      *out_idx = k_ra8_lvd_map_idx_ch2;
      return k_ra8_ok;
    case k_ra8_lvd_ch4:
      *out_idx = k_ra8_lvd_map_idx_ch4;
      return k_ra8_ok;
    case k_ra8_lvd_ch5:
      *out_idx = k_ra8_lvd_map_idx_ch5;
      return k_ra8_ok;
    default:
      return k_ra8_err_invalid_arg;
  }
}

/**
 * @brief Reject PVDLVL encodings outside the HUM-allowed range.
 *
 * @details
 * Per HUM Ch 12.2.4 "PVDmCR0" p 597 only PVDLVL values 0x03..0x0F
 * are valid; lower values map to under-spec voltages and 0x10..0x1F
 * are reserved.
 *
 * @param[in] threshold PVDLVL[4:0] candidate.
 * @return k_ra8_ok if 0x03 <= threshold <= 0x0F, k_ra8_err_invalid_arg otherwise.
 * @retval k_ra8_ok               Threshold within spec.
 * @retval k_ra8_err_invalid_arg  Threshold below min or above max.
 *
 * @pre threshold is any uint8_t.
 * @pre Caller has already mapped the enum to the 5-bit register field.
 * @post Hardware state unchanged.
 * @post Return value reflects the bounds check only.
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
static ra8_err_t internal_validate_threshold(ra8_lvd_pvdlvl_t threshold)
{
  if ((uint8_t)threshold < k_ra8_lvd_pvdlvl_min) {
    return k_ra8_err_invalid_arg;
  }
  if ((uint8_t)threshold > k_ra8_lvd_pvdlvl_max) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/** @brief Implementation of `ra8_lvd_internal_validate_div()` -- upper-bound check. */
ra8_err_t ra8_lvd_internal_validate_div(ra8_lvd_loco_div_t div)
{
  if ((uint8_t)div > k_ra8_lvd_loco_div_max) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Validate IDTSEL[1:0] candidate (0..2). 0b11 is HUM-prohibited.
 *
 * @details
 * Edge selects which crossing direction triggers the IRQ
 * (HUM Ch 12.2.6 "PVDmCR1" p 599). Only the rising / falling / both
 * combinations are valid; 0b11 is reserved.
 *
 * @param[in] edge Edge candidate.
 * @return k_ra8_ok or k_ra8_err_invalid_arg.
 * @retval k_ra8_ok               Edge encoding valid.
 * @retval k_ra8_err_invalid_arg  Edge encoding out of range or reserved.
 *
 * @pre None.
 * @pre ``edge`` originates from the public ::ra8_lvd_edge_t enum.
 * @post Hardware state unchanged.
 * @post Return value reflects the bounds check only.
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
static ra8_err_t internal_validate_edge(ra8_lvd_edge_t edge)
{
  /* HUM Ch 8.2.6 "PVDmCR1 : Voltage Monitor m Circuit Control Register" p 307 */
  if ((uint8_t)edge > k_ra8_lvd_edge_both) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/** @brief Implementation of `ra8_lvd_internal_read_ri()` -- masked CR0 read. */
uint8_t ra8_lvd_internal_read_ri(const ra8_lvd_channel_map_t* map)
{
  /* HUM Ch 8.2.4 "PVDmCR0 : Voltage Monitor m Circuit Control Register 0" p 305 */
  return (uint8_t)(*ra8_lvd_reg8(map->cr0) & k_ra8_lvd_cr0_mask_ri);
}

/**
 * @brief Build a PVDnCR0 byte with the chip-mandated bit-6 = 1 / bit-7 = 0
 *        reserved values.
 *
 * @details
 * PVDnCR0 has a reserved bit-6 = 1 / bit-7 = 0 pattern that must
 * survive every write (HUM Ch 12.2.5 "PVDnCR0" p 598). This helper
 * folds the pattern into a candidate ``base`` value before commit.
 *
 * @param[in] base CR0 candidate without reserved bits applied.
 * @return base | bit6 (HUM 12.2.5 p 598 "This bit is read as 1.
 *         The write value should be 1.").
 * @retval base|k_ra8_lvd_cr0_mask_n_bit6 OR-folded reserved-bit pattern.
 *
 * @pre None.
 * @pre Caller will write the result back to PVDnCR0.
 * @post Hardware state unchanged.
 * @post Bit-6 of return is set; bit-7 stays as in ``base``.
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
static uint8_t internal_n_cr0_with_reserved(uint8_t base)
{
  /* HUM Ch 8.2.5 "PVDnCR0 : Voltage Monitor n Circuit Control Register 0" p 306 */
  return (uint8_t)(base | k_ra8_lvd_cr0_mask_n_bit6);
}

/**
 * @brief Build a PVDmCR0 byte with bit3 forced to 1 (HUM-mandated).
 *
 * @details
 * PVDmCR0 has a HUM-mandated bit-3 = 1 reserved value that must
 * accompany every write (HUM Ch 12.2.4 "PVDmCR0" p 597). This helper
 * folds the bit into a candidate ``base`` value before commit.
 *
 * @param[in] base CR0 candidate without bit3 applied.
 * @return base | bit3 (HUM 12.2.4 p 597 "The write value should be 1.").
 * @retval base|k_ra8_lvd_cr0_mask_bit3 OR-folded reserved bit-3.
 *
 * @pre None.
 * @pre Caller will write the result back to PVDmCR0.
 * @post Hardware state unchanged.
 * @post Bit-3 of return is set; other bits stay as in ``base``.
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
static uint8_t internal_m_cr0_with_reserved(uint8_t base)
{
  /* HUM Ch 8.2.4 "PVDmCR0 : Voltage Monitor m Circuit Control Register 0" p 305 */
  return (uint8_t)(base | k_ra8_lvd_cr0_mask_bit3);
}

/**
 * @brief Apply the chip-specific reserved-bit pattern to a CR0 value.
 *
 * @details
 * Dispatches between ``internal_m_cr0_with_reserved`` (PVDmCR0 with
 * bit-3 = 1 reserved) and ``internal_n_cr0_with_reserved`` (PVDnCR0
 * with bit-6 = 1 reserved) per HUM Ch 12.2.4 / 12.2.5.
 *
 * @param[in] map  Channel map entry.
 * @param[in] base Pre-reserved CR0 value.
 * @return base ORed with the channel's mandatory reserved bits.
 * @retval base|reserved Combined value safe for direct CR0 write.
 *
 * @pre None.
 * @pre ``map`` reflects a valid channel descriptor.
 * @post Hardware state unchanged.
 * @post Caller can write the return value to PVDxCR0 without losing
 *       the reserved-bit invariant.
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
static uint8_t internal_cr0_apply_reserved(const ra8_lvd_channel_map_t* map, uint8_t base)
{
  if (map->has_irq) {
    return internal_m_cr0_with_reserved(base);
  }
  return internal_n_cr0_with_reserved(base);
}

/**
 * @brief Read-modify-write helper for PVDmCMPCR / PVDnCMPCR.
 *
 * @param[in] off       CMPCR offset.
 * @param[in] clear_mask Bits to clear before applying ``set_bits``.
 * @param[in] set_bits   Bits to set after the clear.
 *
 * @pre PRCR.PRC3 unlocked.
 * @post Register reads as ((old & ~clear_mask) | set_bits).
 */
[[maybe_unused]] static void
internal_cmpcr_rmw(ra8_lvd_off_t off, uint8_t clear_mask, uint8_t set_bits)
{
  /* HUM Ch 8.2.2 "PVDmCMPCR : Voltage Monitor m Comparator Control Register" p 303 */
  /* HUM Ch 8.2.3 "PVDnCMPCR : Voltage Monitor n Comparator Control Register" p 304 */
  const uint8_t prev = *ra8_lvd_reg8(off);
  *ra8_lvd_reg8(off) = (uint8_t)((prev & (uint8_t)~clear_mask) | set_bits);
}

/** @brief Implementation of `ra8_lvd_internal_cr0_rmw()` -- RMW with reserved-bit refold. */
void ra8_lvd_internal_cr0_rmw(const ra8_lvd_channel_map_t* map,
                              uint8_t                      clear_mask,
                              uint8_t                      set_bits)
{
  /* HUM Ch 8.2.4 "PVDmCR0 : Voltage Monitor m Circuit Control Register 0" p 305 */
  /* HUM Ch 8.2.5 "PVDnCR0 : Voltage Monitor n Circuit Control Register 0" p 306 */
  const uint8_t prev      = *ra8_lvd_reg8(map->cr0);
  const uint8_t base      = (uint8_t)((prev & (uint8_t)~clear_mask) | set_bits);
  *ra8_lvd_reg8(map->cr0) = internal_cr0_apply_reserved(map, base);
}

/* =============================================================================
 * Channel init / deinit -- follows HUM Table 8.4 / Table 8.6
 * =============================================================================
 */

/**
 * @brief Validate the cfg fields that the per-channel init reads.
 *
 * @details
 * Sequentially calls the per-field validators (threshold, divider,
 * edge for IRQ-capable channels) so a single error code surfaces
 * without partial register writes. See HUM Ch 12 "Low Voltage
 * Detection (LVD)" pp 593-624.
 *
 * @param[in] map Channel map for the requested channel.
 * @param[in] cfg Caller-supplied configuration.
 * @return k_ra8_ok or one of the *_err_* codes.
 * @retval k_ra8_ok               All fields in spec.
 * @retval k_ra8_err_invalid_arg  At least one field outside the documented domain.
 *
 * @pre cfg != nullptr.
 * @pre map != nullptr.
 * @post Hardware state unchanged.
 * @post Return reflects the strictest validator that fired.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t internal_validate_cfg(const ra8_lvd_channel_map_t* map, const ra8_lvd_cfg_t* cfg)
{
  ra8_err_t err = internal_validate_threshold(cfg->threshold);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_lvd_internal_validate_div(cfg->filter_div);
  if (err != k_ra8_ok) {
    return err;
  }
  if (map->has_irq) {
    err = internal_validate_edge(cfg->edge);
    if (err != k_ra8_ok) {
      return err;
    }
  } else {
    /* HUM Ch 8.2.5 "PVDnCR0 : Voltage Monitor n Circuit Control Register 0" p 306 */
    /* n channels only have RE -- reject IRQ / NMI responses. */
    if ((cfg->response == k_ra8_lvd_response_interrupt) ||
        (cfg->response == k_ra8_lvd_response_nmi)) {
      return k_ra8_err_not_supported;
    }
  }
  /* HUM Ch 8.2.4 "PVDmCR0 : Voltage Monitor m Circuit Control Register 0" p 306 */
  /* RN=1 prohibited when RHSEL=1. */
  if (ra8_lvd_internal_reject_hvd_after((uint32_t)k_ra8_lvd_hysteresis_hvd,
                                        (uint32_t)k_ra8_lvd_negate_after_assert,
                                        (uint32_t)cfg->hysteresis,
                                        (uint32_t)cfg->negate)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Compute the initial CR0 value for ``ra8_lvd_channel_init``.
 *
 * @details
 * Encodes FSAMP, DFDIS, RN and RI per HUM Ch 12.2.4 / 12.2.5
 * (pp 597-598). DFDIS stays asserted; the caller drops it after the
 * first CR0 write if ``cfg->filter_en`` is true.
 *
 * @param[in] map Channel map (decides whether RN/RI exist).
 * @param[in] cfg Caller-supplied config.
 *
 * @return Encoded CR0 byte (without the reserved-bit overlay applied).
 * @retval 0..0xFF Bit-packed CR0 value with DFDIS already set.
 *
 * @pre ``map`` and ``cfg`` are non-null and refer to the same channel.
 * @pre Caller will fold the reserved-bit pattern in via
 *      ``internal_cr0_apply_reserved`` before commit.
 * @post No side effects.
 * @post DFDIS bit is set in the returned value.
 *
 * @note Internal helper, not thread-safe.
 * @since 0.1.0
 */
static uint8_t internal_compose_cr0(const ra8_lvd_channel_map_t* map, const ra8_lvd_cfg_t* cfg)
{
  uint8_t cr0 = (uint8_t)((uint8_t)cfg->filter_div << k_ra8_lvd_cr0_shift_fsamp);
  cr0 &= k_ra8_lvd_cr0_mask_fsamp;
  cr0 |= k_ra8_lvd_cr0_mask_dfdis; /* DFDIS = 1 while writing FSAMP. */
  if (map->has_irq) {
    if (cfg->negate == k_ra8_lvd_negate_after_assert) {
      cr0 |= k_ra8_lvd_cr0_mask_rn;
    }
    if (ra8_lvd_internal_set_ri_bit((uint32_t)k_ra8_lvd_response_reset,
                                    (uint32_t)k_ra8_lvd_response_reset_on_rise,
                                    (uint32_t)cfg->response)) {
      cr0 |= k_ra8_lvd_cr0_mask_ri;
    }
  }
  return cr0;
}

/**
 * @brief Program CR1 + status-clear for the m-only steps 9-11.
 *
 * @details
 * HUM Table 8.4 steps 9-11 (only m channels have CR1 / SR). Writes the
 * IDTSEL/IRQSEL pair and clears DET when the caller asked for it.
 *
 * @param[in] map Channel map.
 * @param[in] cfg Caller-supplied config.
 *
 * @pre ``map`` describes an m channel (caller checks ``map->has_irq``).
 * @pre PRCR.PRC3 unlocked (caller-managed).
 * @post CR1 reflects ``cfg``, DET cleared if ``cfg->clear_status``.
 * @post Reserved-bit invariant for the channel is preserved.
 *
 * @note Internal helper, not thread-safe.
 * @since 0.1.0
 */
static void internal_program_cr1(const ra8_lvd_channel_map_t* map, const ra8_lvd_cfg_t* cfg)
{
  /* HUM Ch 8.2.6 "PVDmCR1 : Voltage Monitor m Circuit Control Register" p 307 */
  uint8_t cr1 = (uint8_t)((uint8_t)cfg->edge & k_ra8_lvd_cr1_mask_idtsel);
  if (cfg->irq_type == k_ra8_lvd_irq_maskable) {
    cr1 |= k_ra8_lvd_cr1_mask_irqsel;
  }
  *ra8_lvd_reg8(map->cr1) = cr1;

  if (cfg->clear_status) {
    /* HUM Ch 8.2.7 "PVDmSR : Voltage Monitor m Circuit Status Register" p 307 */
    *ra8_lvd_reg8(map->sr) = 0U;
  }
}

/**
 * @brief Run HUM Table 8.4 / 8.6 steps 1-4 (CMPCR programming).
 *
 * @details
 * Disables the channel, writes PVDLVL with PVDE clear, programs RHSEL
 * on FCR, and re-enables PVDE so the comparator can stabilise.
 *
 * @param[in] map Channel map.
 * @param[in] cfg Caller-supplied config.
 *
 * @pre Module clock ungated.
 * @pre PRCR.PRC3 unlocked (caller-managed).
 * @post CMPCR/FCR reflect ``cfg``; PVDE asserted.
 * @post Channel comparator is armed and stabilising.
 *
 * @note Internal helper, not thread-safe.
 * @since 0.1.0
 */
static void internal_lvd_program_cmpcr(const ra8_lvd_channel_map_t* map, const ra8_lvd_cfg_t* cfg)
{
  /* Step 1: disable everything; HUM Ch 8.2.4 / 8.2.2 */
  *ra8_lvd_reg8(map->cr0)   = internal_cr0_apply_reserved(map, 0U);
  *ra8_lvd_reg8(map->cmpcr) = 0U;

  /* Step 2: PVDLVL with PVDE = 0 */
  uint8_t cmpcr             = (uint8_t)((uint8_t)cfg->threshold & k_ra8_lvd_cmpcr_mask_pvdlvl);
  *ra8_lvd_reg8(map->cmpcr) = cmpcr;

  /* Step 3: HUM Ch 8.2.8 / 8.2.9 -- RHSEL on FCR */
  *ra8_lvd_reg8(map->fcr) = (uint8_t)((uint8_t)cfg->hysteresis & k_ra8_lvd_fcr_mask_rhsel);

  /* Step 4: re-assert PVDE */
  cmpcr |= k_ra8_lvd_cmpcr_mask_pvde;
  *ra8_lvd_reg8(map->cmpcr) = cmpcr;
}

/**
 * @brief Run HUM Table 8.4 / 8.6 steps 6-13 (CR0 / CR1 / status).
 *
 * @details
 * Programs FSAMP/RN/RI then optionally drops DFDIS, CR1+DET on m-only,
 * then RIE/RE and finally CMPE. Returns the final CR0 value for
 * caller logging or further updates.
 *
 * @param[in] map Channel map.
 * @param[in] cfg Caller-supplied config.
 *
 * @pre ``ra8_lvd_channel_init`` already ran the CMPCR steps.
 * @pre PRCR.PRC3 unlocked (caller-managed).
 * @post Channel is armed per ``cfg``.
 * @post CMPE bit is set on the matching CR0 register.
 *
 * @note Internal helper, not thread-safe.
 * @since 0.1.0
 */
static void internal_lvd_program_cr0_chain(const ra8_lvd_channel_map_t* map,
                                           const ra8_lvd_cfg_t*         cfg)
{
  uint8_t cr0             = internal_compose_cr0(map, cfg);
  *ra8_lvd_reg8(map->cr0) = internal_cr0_apply_reserved(map, cr0);

  if (cfg->filter_en) {
    cr0 &= (uint8_t)~k_ra8_lvd_cr0_mask_dfdis;
    *ra8_lvd_reg8(map->cr0) = internal_cr0_apply_reserved(map, cr0);
  }

  if (map->has_irq) {
    internal_program_cr1(map, cfg);
  }

  if (cfg->irq_enable && (cfg->response != k_ra8_lvd_response_none)) {
    /* RIE (m-channel: IRQ/reset enable) and RE (n-channel: reset-only)
     * sit at the same CR0 bit position (0x01) on RA8D2; the semantic
     * distinction lives in `map->has_irq`. Either mask gates bit 0. */
    cr0 |= k_ra8_lvd_cr0_mask_rie;
    *ra8_lvd_reg8(map->cr0) = internal_cr0_apply_reserved(map, cr0);
  }

  cr0 |= k_ra8_lvd_cr0_mask_cmpe;
  *ra8_lvd_reg8(map->cr0) = internal_cr0_apply_reserved(map, cr0);
}

/**
 * @brief Bring up one PVD channel per HUM Ch 12 Table 12.4 / 12.6.
 *
 * @details
 * Validates ``cfg``, then runs the comparator-then-CR0 sequence
 * documented at HUM Ch 12.3.1 "Operating LVD" pp 600-602.
 *
 * @param[in] channel PVD channel id (PVD1 / PVD2 / PVD4 / PVD5).
 * @param[in] cfg     Non-NULL configuration block.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  Channel armed per ``cfg``.
 * @retval k_ra8_err_null_ptr        ``cfg`` was NULL.
 * @retval k_ra8_err_invalid_arg     Channel or config field out of range.
 * @retval k_ra8_err_not_supported   Response unavailable for the channel kind.
 *
 * @pre ``cfg`` non-NULL.
 * @pre PRCR.PRC3 unlocked (caller-managed).
 *
 * @post On success, the channel comparator is armed and CMPE is set.
 * @post On error, no register has been written for the channel.
 *
 * @note Not thread-safe; pair with init-context IRQ masking.
 * @since 0.1.0
 */
ra8_err_t ra8_lvd_channel_init(ra8_lvd_channel_t channel, const ra8_lvd_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");

  uint8_t         idx     = 0U;
  const ra8_err_t map_err = ra8_lvd_internal_channel_to_idx(channel, &idx);
  RA8_RETURN_ON_ERROR(map_err, s_tag, "lvd_init: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra8_lvd_channel_map_t map     = s_lvd_map[idx];
  const ra8_err_t             cfg_err = internal_validate_cfg(&map, cfg);
  RA8_RETURN_ON_ERROR(cfg_err, s_tag, "lvd_init: bad cfg"); /* GCOVR_EXCL_BR_LINE */

  internal_lvd_program_cmpcr(&map, cfg);
  internal_lvd_program_cr0_chain(&map, cfg);

  ra8_log_info_val(s_tag, "lvd_init ch", (uint32_t)channel);
  return k_ra8_ok;
}

/**
 * @brief Tear down one PVD channel per HUM Ch 12 Table 12.5 / 12.7.
 *
 * @details
 * Walks the documented disable sequence (CMPE -> RIE/RE -> DFDIS ->
 * PVDE) so the comparator is in a known idle state.
 *
 * @param[in] channel PVD channel id.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Channel disabled.
 * @retval k_ra8_err_invalid_arg   Channel mapping failed.
 *
 * @pre PRCR.PRC3 unlocked (caller-managed).
 * @pre Caller is in single-threaded shutdown context.
 *
 * @post All channel control registers read as 0 (or reserved-only).
 * @post CMPCR is 0; the comparator is fully disarmed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_lvd_channel_deinit(ra8_lvd_channel_t channel)
{
  uint8_t         idx     = 0U;
  const ra8_err_t map_err = ra8_lvd_internal_channel_to_idx(channel, &idx);
  RA8_RETURN_ON_ERROR(map_err, s_tag, "lvd_deinit: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra8_lvd_channel_map_t map = s_lvd_map[idx];

  /* === HUM Table 8.5 step 1 / Table 8.7 step 1 -- drop CMPE. */
  ra8_lvd_internal_cr0_rmw(&map, k_ra8_lvd_cr0_mask_cmpe, 0U);

  /* === HUM Table 8.5 step 3 / Table 8.7 step 3 -- drop RIE / RE. */
  uint8_t enable_bit = k_ra8_lvd_cr0_mask_re;
  if (map.has_irq) {
    enable_bit = k_ra8_lvd_cr0_mask_rie;
  }
  ra8_lvd_internal_cr0_rmw(&map, enable_bit, 0U);

  /* === HUM Table 8.5 step 4 / Table 8.7 step 4 -- DFDIS = 1 (filter off). */
  ra8_lvd_internal_cr0_rmw(&map, 0U, k_ra8_lvd_cr0_mask_dfdis);

  /* === HUM Table 8.5 step 5 / Table 8.7 step 5 -- drop PVDE. */
  /* HUM Ch 8.2.2 "PVDmCMPCR" p 303 */
  *ra8_lvd_reg8(map.cmpcr) = 0U;

  /* Cleanly drop the rest of the per-channel state. */
  *ra8_lvd_reg8(map.cr0) = internal_cr0_apply_reserved(&map, 0U);
  *ra8_lvd_reg8(map.fcr) = 0U;
  if (map.has_irq) {
    /* HUM Ch 8.2.6 "PVDmCR1 : Voltage Monitor m Circuit Control Register" p 307 */
    *ra8_lvd_reg8(map.cr1) = 0U;
    /* HUM Ch 8.2.7 "PVDmSR : Voltage Monitor m Circuit Status Register" p 307 */
    *ra8_lvd_reg8(map.sr) = 0U;
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Threshold + edge + kind setters
 * =============================================================================
 */

/**
 * @brief Update PVDLVL for an already-armed channel.
 *
 * @details
 * Drops PVDE around the PVDLVL write so the comparator does not
 * glitch (HUM Ch 12.2.2 "PVDmCMPCR" p 595), then restores PVDE.
 *
 * @param[in] channel  PVD channel id.
 * @param[in] threshold New PVDLVL[4:0] encoding (0x03..0x0F).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Threshold updated.
 * @retval k_ra8_err_invalid_arg   Channel or threshold out of range.
 *
 * @pre Channel previously brought up via ``ra8_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post CMPCR.PVDLVL reflects ``threshold``.
 * @post CMPCR.PVDE state is preserved across the write.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_lvd_set_threshold(ra8_lvd_channel_t channel, ra8_lvd_pvdlvl_t threshold)
{
  uint8_t         idx     = 0U;
  const ra8_err_t map_err = ra8_lvd_internal_channel_to_idx(channel, &idx);
  RA8_RETURN_ON_ERROR(map_err, s_tag, "lvd_set_threshold: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra8_err_t lvl_err = internal_validate_threshold(threshold);
  RA8_RETURN_ON_ERROR(lvl_err, s_tag, "lvd_set_threshold: bad threshold"); /* GCOVR_EXCL_BR_LINE */

  const ra8_lvd_channel_map_t map = s_lvd_map[idx];

  /* HUM Ch 8.2.2 "PVDmCMPCR : Voltage Monitor m Comparator Control Register" p 303 */
  /* Preserve the PVDE bit, drop it around the PVDLVL write, restore it afterwards. */
  const uint8_t prev     = *ra8_lvd_reg8(map.cmpcr);
  const uint8_t pvde_was = (uint8_t)(prev & k_ra8_lvd_cmpcr_mask_pvde);

  *ra8_lvd_reg8(map.cmpcr) = 0U;

  uint8_t cmpcr = (uint8_t)((uint8_t)threshold & k_ra8_lvd_cmpcr_mask_pvdlvl);
  cmpcr |= pvde_was;
  *ra8_lvd_reg8(map.cmpcr) = cmpcr;
  return k_ra8_ok;
}

/**
 * @brief Update CR1.IDTSEL for an IRQ-capable PVDm channel.
 *
 * @details
 * Read-modify-writes CR1 (HUM Ch 12.2.6 "PVDmCR1" p 599) so the
 * caller can change which crossing edge raises the IRQ without
 * disturbing IRQSEL.
 *
 * @param[in] channel PVDm channel id (PVD1 / PVD2).
 * @param[in] edge    New edge encoding.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Edge updated.
 * @retval k_ra8_err_invalid_arg   Channel or edge out of range.
 * @retval k_ra8_err_not_supported Channel has no IRQ path (PVDn).
 *
 * @pre Channel previously brought up via ``ra8_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post CR1.IDTSEL reflects ``edge``.
 * @post CR1.IRQSEL is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_lvd_set_irq_edge(ra8_lvd_channel_t channel, ra8_lvd_edge_t edge)
{
  uint8_t         idx     = 0U;
  const ra8_err_t map_err = ra8_lvd_internal_channel_to_idx(channel, &idx);
  RA8_RETURN_ON_ERROR(map_err, s_tag, "lvd_set_irq_edge: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra8_err_t edge_err = internal_validate_edge(edge);
  RA8_RETURN_ON_ERROR(edge_err, s_tag, "lvd_set_irq_edge: bad edge"); /* GCOVR_EXCL_BR_LINE */

  const ra8_lvd_channel_map_t map = s_lvd_map[idx];
  if (!map.has_irq) {
    return k_ra8_err_not_supported;
  }

  /* HUM Ch 8.2.6 "PVDmCR1 : Voltage Monitor m Circuit Control Register" p 307 */
  const uint8_t prev     = *ra8_lvd_reg8(map.cr1);
  const uint8_t next     = (uint8_t)((prev & (uint8_t)~k_ra8_lvd_cr1_mask_idtsel) |
                                     ((uint8_t)edge & k_ra8_lvd_cr1_mask_idtsel));
  *ra8_lvd_reg8(map.cr1) = next;
  return k_ra8_ok;
}

/**
 * @brief Update CR1.IRQSEL (maskable IRQ vs NMI) for a PVDm channel.
 *
 * @details
 * Read-modify-writes CR1 (HUM Ch 12.2.6 "PVDmCR1" p 599) so the
 * caller can switch the IRQ delivery vector at runtime.
 *
 * @param[in] channel PVDm channel id.
 * @param[in] kind    Maskable IRQ or NMI.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Kind updated.
 * @retval k_ra8_err_invalid_arg   Channel mapping failed.
 * @retval k_ra8_err_not_supported Channel has no IRQ path.
 *
 * @pre Channel previously brought up via ``ra8_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post CR1.IRQSEL reflects ``kind``.
 * @post CR1.IDTSEL is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_lvd_set_irq_kind(ra8_lvd_channel_t channel, ra8_lvd_irq_type_t kind)
{
  uint8_t         idx     = 0U;
  const ra8_err_t map_err = ra8_lvd_internal_channel_to_idx(channel, &idx);
  RA8_RETURN_ON_ERROR(map_err, s_tag, "lvd_set_irq_kind: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra8_lvd_channel_map_t map = s_lvd_map[idx];
  if (!map.has_irq) {
    return k_ra8_err_not_supported;
  }

  /* HUM Ch 8.2.6 "PVDmCR1 : Voltage Monitor m Circuit Control Register" p 307 */
  const uint8_t prev = *ra8_lvd_reg8(map.cr1);
  uint8_t       next = (uint8_t)(prev & (uint8_t)~k_ra8_lvd_cr1_mask_irqsel);
  if (kind == k_ra8_lvd_irq_maskable) {
    next |= k_ra8_lvd_cr1_mask_irqsel;
  }
  *ra8_lvd_reg8(map.cr1) = next;
  return k_ra8_ok;
}
