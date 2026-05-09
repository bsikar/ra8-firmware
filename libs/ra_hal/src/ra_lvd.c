/**
 * @file ra_lvd.c
 * @brief Programmable Voltage Detection (PVD / LVD) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Round-3 driver implementing the **full** PVD surface defined in
 * `ra_lvd.h`. Every register access carries a HUM Ch 8 citation
 * (R01UH1065EJ rev 1.30).
 *
 * The PVD block has no module-stop bit -- it is part of the always-on
 * SYSC region -- so this driver does **not** call `ra_mstp_enable`.
 * Per HUM 8.2.* register notes, every control-register write requires
 * PRCR.PRC3 = 1; the driver does NOT take the lock itself (callers
 * must wrap each call), but the host-side simulator mmap has no lock
 * so unit tests still see writes land.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_lvd.h"

#include <stdint.h>

#include "ra8d2_lvd_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_lvd_internal.h"

/**
 * @brief Pure RN+RHSEL reject predicate -- see header for full contract.
 * @details Promoted helper so the line-494 AND can be driven under MC/DC.
 * @param[in] hvd_val          Numeric value of @c k_ra_lvd_hysteresis_hvd.
 * @param[in] after_assert_val Numeric value of @c k_ra_lvd_negate_after_assert.
 * @param[in] hysteresis       Candidate hysteresis value.
 * @param[in] negate           Candidate negate value.
 * @return Boolean reject predicate.
 * @retval true  Caller returns @c k_ra_err_invalid_arg.
 * @retval false Combination is allowed.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool ra_lvd_internal_reject_hvd_after(uint32_t hvd_val,
                                      uint32_t after_assert_val,
                                      uint32_t hysteresis,
                                      uint32_t negate)
{
  return (hysteresis == hvd_val) && (negate == after_assert_val);
}

/**
 * @brief Pure RI-bit predicate -- see header for full contract.
 * @details Promoted helper so the line-533 OR can be driven under MC/DC.
 * @param[in] reset_val         Numeric value of @c k_ra_lvd_response_reset.
 * @param[in] reset_on_rise_val Numeric value of @c k_ra_lvd_response_reset_on_rise.
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
bool ra_lvd_internal_set_ri_bit(uint32_t reset_val, uint32_t reset_on_rise_val, uint32_t response)
{
  return (response == reset_val) || (response == reset_on_rise_val);
}
#include "ra_log.h"

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
 * @struct ra_lvd_channel_map_t
 * @brief Per-channel register-offset bundle.
 *
 * @details
 * Channels 1 and 2 use the m-series offsets and have valid CR1 / SR
 * registers. Channels 4 and 5 use the n-series offsets and have no
 * CR1 / SR (the ``has_irq`` flag flips them off). FCR is exposed for
 * all four channels (HUM 8.2.8 m and 8.2.9 n).
 */
typedef struct {
  ra_lvd_off_t cmpcr;   /**< PVDxCMPCR offset.                        */
  ra_lvd_off_t cr0;     /**< PVDxCR0 offset.                          */
  ra_lvd_off_t cr1;     /**< PVDxCR1 (m only; n: same as cr0 stub).   */
  ra_lvd_off_t sr;      /**< PVDxSR  (m only; n: same as cr0 stub).   */
  ra_lvd_off_t fcr;     /**< PVDxFCR (RHSEL).                         */
  bool         has_irq; /**< true for monitor m channels (1, 2).      */
} ra_lvd_channel_map_t;

/**
 * @enum ra_lvd_map_idx_t
 * @brief Indices into ``s_lvd_map``.
 */
typedef enum : uint8_t {
  k_ra_lvd_map_idx_ch1   = 0U, /**< PVD1.       */
  k_ra_lvd_map_idx_ch2   = 1U, /**< PVD2.       */
  k_ra_lvd_map_idx_ch4   = 2U, /**< PVD4.       */
  k_ra_lvd_map_idx_ch5   = 3U, /**< PVD5.       */
  k_ra_lvd_map_idx_count = 4U, /**< Sentinel.   */
} ra_lvd_map_idx_t;

/**
 * @var s_lvd_map
 * @brief Lookup table from ``ra_lvd_map_idx_t`` to register offsets.
 *
 * @note PRIVATE.
 */
static const ra_lvd_channel_map_t s_lvd_map[k_ra_lvd_map_idx_count] = {
  {.cmpcr   = k_ra_lvd_pvd1_cmpcr_off,
   .cr0     = k_ra_lvd_pvd1_cr0_off,
   .cr1     = k_ra_lvd_pvd1_cr1_off,
   .sr      = k_ra_lvd_pvd1_sr_off,
   .fcr     = k_ra_lvd_pvd1_fcr_off,
   .has_irq = true},
  {.cmpcr   = k_ra_lvd_pvd2_cmpcr_off,
   .cr0     = k_ra_lvd_pvd2_cr0_off,
   .cr1     = k_ra_lvd_pvd2_cr1_off,
   .sr      = k_ra_lvd_pvd2_sr_off,
   .fcr     = k_ra_lvd_pvd2_fcr_off,
   .has_irq = true},
  {.cmpcr   = k_ra_lvd_pvd4_cmpcr_off,
   .cr0     = k_ra_lvd_pvd4_cr0_off,
   .cr1     = k_ra_lvd_pvd4_cr0_off, /* unused -- n channels have no CR1. */
   .sr      = k_ra_lvd_pvd4_cr0_off, /* unused -- n channels have no SR.  */
   .fcr     = k_ra_lvd_pvd4_fcr_off,
   .has_irq = false},
  {.cmpcr   = k_ra_lvd_pvd5_cmpcr_off,
   .cr0     = k_ra_lvd_pvd5_cr0_off,
   .cr1     = k_ra_lvd_pvd5_cr0_off,
   .sr      = k_ra_lvd_pvd5_cr0_off,
   .fcr     = k_ra_lvd_pvd5_fcr_off,
   .has_irq = false},
};

/* =============================================================================
 * Callback storage
 * =============================================================================
 */

/**
 * @var s_lvd_fn
 * @brief Shared crossing callback (set via `ra_lvd_attach_handler`).
 */
static ra_lvd_event_fn_t s_lvd_fn;

/**
 * @var s_lvd_ctx
 * @brief Context passed to ``s_lvd_fn``.
 */
static void* s_lvd_ctx;

/**
 * @var s_lvd_chan_fn
 * @brief Per-channel override callbacks for PVD1 / PVD2.
 *
 * @details
 * Indexed by ``ra_lvd_map_idx_t``; only entries [0] and [1] are used
 * (the n channels have no IRQ path). When non-null, takes precedence
 * over ``s_lvd_fn`` in `ra_lvd_dispatch`.
 */
static ra_lvd_event_fn_t s_lvd_chan_fn[k_ra_lvd_map_idx_count];

/**
 * @var s_lvd_chan_ctx
 * @brief Per-channel callback contexts paired with ``s_lvd_chan_fn``.
 */
static void* s_lvd_chan_ctx[k_ra_lvd_map_idx_count];

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Translate a public channel id to an index into ``s_lvd_map``.
 *
 * @param[in]  channel Public channel id.
 * @param[out] out_idx On success, the index in [0..3].
 * @return ``k_ra_ok`` if mapping succeeded, ``k_ra_err_invalid_arg`` otherwise.
 *
 * @details
 * Folds the public ::ra_lvd_channel_t enumerator (which uses the
 * datasheet's PVD1/2/4/5 numbering) into a 0..3 array index used by
 * the internal ``s_lvd_map`` lookup table. Channels 0 and 3 are
 * intentionally absent; PVD3 is the always-on power-loss detector.
 *
 * @retval k_ra_ok               Mapping succeeded.
 * @retval k_ra_err_invalid_arg  ``channel`` is not one of PVD1/2/4/5.
 *
 * @pre out_idx != nullptr.
 * @pre channel may be any uint8_t.
 * @post On success, *out_idx is in [0, k_ra_lvd_map_idx_count).
 * @post On failure, *out_idx is unchanged.
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
static ra_err_t internal_channel_to_idx(ra_lvd_channel_t channel, uint8_t* out_idx)
{
  switch (channel) {
    case k_ra_lvd_ch1:
      *out_idx = k_ra_lvd_map_idx_ch1;
      return k_ra_ok;
    case k_ra_lvd_ch2:
      *out_idx = k_ra_lvd_map_idx_ch2;
      return k_ra_ok;
    case k_ra_lvd_ch4:
      *out_idx = k_ra_lvd_map_idx_ch4;
      return k_ra_ok;
    case k_ra_lvd_ch5:
      *out_idx = k_ra_lvd_map_idx_ch5;
      return k_ra_ok;
    default:
      return k_ra_err_invalid_arg;
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
 * @return k_ra_ok if 0x03 <= threshold <= 0x0F, k_ra_err_invalid_arg otherwise.
 * @retval k_ra_ok               Threshold within spec.
 * @retval k_ra_err_invalid_arg  Threshold below min or above max.
 *
 * @pre threshold is any uint8_t.
 * @pre Caller has already mapped the enum to the 5-bit register field.
 * @post Hardware state unchanged.
 * @post Return value reflects the bounds check only.
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
static ra_err_t internal_validate_threshold(ra_lvd_pvdlvl_t threshold)
{
  if ((uint8_t)threshold < k_ra_lvd_pvdlvl_min) {
    return k_ra_err_invalid_arg;
  }
  if ((uint8_t)threshold > k_ra_lvd_pvdlvl_max) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/**
 * @brief Validate FSAMP[1:0] candidate (0..3).
 *
 * @details
 * FSAMP[1:0] selects the LOCO sample-divider for the digital filter
 * (HUM Ch 12.2.6 "PVDmCR1" p 599). All four encodings are valid.
 *
 * @param[in] div Divider candidate.
 * @return k_ra_ok or k_ra_err_invalid_arg.
 * @retval k_ra_ok               Divider within enum domain.
 * @retval k_ra_err_invalid_arg  Divider above the documented maximum.
 *
 * @pre None.
 * @pre Caller has not yet committed FSAMP to a register.
 * @post Hardware state unchanged.
 * @post Return value reflects the bounds check only.
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
static ra_err_t internal_validate_div(ra_lvd_loco_div_t div)
{
  if ((uint8_t)div > k_ra_lvd_loco_div_max) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
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
 * @return k_ra_ok or k_ra_err_invalid_arg.
 * @retval k_ra_ok               Edge encoding valid.
 * @retval k_ra_err_invalid_arg  Edge encoding out of range or reserved.
 *
 * @pre None.
 * @pre ``edge`` originates from the public ::ra_lvd_edge_t enum.
 * @post Hardware state unchanged.
 * @post Return value reflects the bounds check only.
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
static ra_err_t internal_validate_edge(ra_lvd_edge_t edge)
{
  /* HUM Ch 8.2.6 "PVDmCR1 : Voltage Monitor m Circuit Control Register" p 307*/
  if ((uint8_t)edge > k_ra_lvd_edge_both) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/**
 * @brief Read PVDmCR0.RI for an m channel.
 *
 * @details
 * Returns the Reset-on-Voltage bit (HUM Ch 12.2.4 "PVDmCR0" p 597)
 * so the caller can decide whether to issue a software reset on a
 * detected voltage crossing.
 *
 * @param[in] map Channel map entry (must be has_irq=true).
 * @return Non-zero if RI is set.
 * @retval 0     RI bit clear -- IRQ-only path.
 * @retval !=0   RI bit set -- channel will reset the SoC on crossing.
 *
 * @pre map.has_irq == true.
 * @pre PRCR.PRC3 unlocked (caller-managed).
 * @post Hardware state unchanged.
 * @post No CR0 write has been issued.
 *
 * @note Read-only; safe under simple races.
 * @since 0.1.0
 */
static uint8_t internal_read_ri(const ra_lvd_channel_map_t* map)
{
  /* HUM Ch 8.2.4 "PVDmCR0 : Voltage Monitor m Circuit Control Register 0" p 305*/
  return (uint8_t)(*ra_lvd_reg8(map->cr0) & k_ra_lvd_cr0_mask_ri);
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
 * @retval base|k_ra_lvd_cr0_mask_n_bit6 OR-folded reserved-bit pattern.
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
  /* HUM Ch 8.2.5 "PVDnCR0 : Voltage Monitor n Circuit Control Register 0" p 306*/
  return (uint8_t)(base | k_ra_lvd_cr0_mask_n_bit6);
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
 * @retval base|k_ra_lvd_cr0_mask_bit3 OR-folded reserved bit-3.
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
  /* HUM Ch 8.2.4 "PVDmCR0 : Voltage Monitor m Circuit Control Register 0" p 305*/
  return (uint8_t)(base | k_ra_lvd_cr0_mask_bit3);
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
static uint8_t internal_cr0_apply_reserved(const ra_lvd_channel_map_t* map, uint8_t base)
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
internal_cmpcr_rmw(ra_lvd_off_t off, uint8_t clear_mask, uint8_t set_bits)
{
  /* HUM Ch 8.2.2 "PVDmCMPCR : Voltage Monitor m Comparator Control Register" p 303*/
  /* HUM Ch 8.2.3 "PVDnCMPCR : Voltage Monitor n Comparator Control Register" p 304*/
  const uint8_t prev = *ra_lvd_reg8(off);
  *ra_lvd_reg8(off)  = (uint8_t)((prev & (uint8_t)~clear_mask) | set_bits);
}

/**
 * @brief Read-modify-write helper for PVDmCR0 / PVDnCR0 with reserved-bit
 *        rewrite forced.
 *
 * @details
 * Reads PVDxCR0, masks out ``clear_mask``, ORs in ``set_bits``, then
 * folds the channel's mandatory reserved-bit pattern back in via
 * ``internal_cr0_apply_reserved`` (HUM Ch 12.2.4 / 12.2.5).
 *
 * @param[in] map        Channel map.
 * @param[in] clear_mask Bits to clear.
 * @param[in] set_bits   Bits to set.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre Caller has serialised access to the CR0 register.
 * @post Register reads as ((old & ~clear_mask) | set_bits | reserved).
 * @post Reserved-bit invariant for the channel is preserved.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static void internal_cr0_rmw(const ra_lvd_channel_map_t* map, uint8_t clear_mask, uint8_t set_bits)
{
  /* HUM Ch 8.2.4 "PVDmCR0 : Voltage Monitor m Circuit Control Register 0" p 305*/
  /* HUM Ch 8.2.5 "PVDnCR0 : Voltage Monitor n Circuit Control Register 0" p 306*/
  const uint8_t prev     = *ra_lvd_reg8(map->cr0);
  const uint8_t base     = (uint8_t)((prev & (uint8_t)~clear_mask) | set_bits);
  *ra_lvd_reg8(map->cr0) = internal_cr0_apply_reserved(map, base);
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
 * @return k_ra_ok or one of the *_err_* codes.
 * @retval k_ra_ok               All fields in spec.
 * @retval k_ra_err_invalid_arg  At least one field outside the documented domain.
 *
 * @pre cfg != nullptr.
 * @pre map != nullptr.
 * @post Hardware state unchanged.
 * @post Return reflects the strictest validator that fired.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_validate_cfg(const ra_lvd_channel_map_t* map, const ra_lvd_cfg_t* cfg)
{
  ra_err_t err = internal_validate_threshold(cfg->threshold);
  if (err != k_ra_ok) {
    return err;
  }
  err = internal_validate_div(cfg->filter_div);
  if (err != k_ra_ok) {
    return err;
  }
  if (map->has_irq) {
    err = internal_validate_edge(cfg->edge);
    if (err != k_ra_ok) {
      return err;
    }
  } else {
    /* HUM Ch 8.2.5 "PVDnCR0 : Voltage Monitor n Circuit Control Register 0" p 306*/
    /* n channels only have RE -- reject IRQ / NMI responses. */
    if ((cfg->response == k_ra_lvd_response_interrupt) ||
        (cfg->response == k_ra_lvd_response_nmi)) {
      return k_ra_err_not_supported;
    }
  }
  /* HUM Ch 8.2.4 "PVDmCR0 : Voltage Monitor m Circuit Control Register 0" p 306*/
  /* RN=1 prohibited when RHSEL=1. */
  if (ra_lvd_internal_reject_hvd_after((uint32_t)k_ra_lvd_hysteresis_hvd,
                                       (uint32_t)k_ra_lvd_negate_after_assert,
                                       (uint32_t)cfg->hysteresis,
                                       (uint32_t)cfg->negate)) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/**
 * @brief Compute the initial CR0 value for ``ra_lvd_channel_init``.
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
static uint8_t internal_compose_cr0(const ra_lvd_channel_map_t* map, const ra_lvd_cfg_t* cfg)
{
  uint8_t cr0 = (uint8_t)((uint8_t)cfg->filter_div << k_ra_lvd_cr0_shift_fsamp);
  cr0 &= k_ra_lvd_cr0_mask_fsamp;
  cr0 |= k_ra_lvd_cr0_mask_dfdis; /* DFDIS = 1 while writing FSAMP. */
  if (map->has_irq) {
    if (cfg->negate == k_ra_lvd_negate_after_assert) {
      cr0 |= k_ra_lvd_cr0_mask_rn;
    }
    if (ra_lvd_internal_set_ri_bit((uint32_t)k_ra_lvd_response_reset,
                                   (uint32_t)k_ra_lvd_response_reset_on_rise,
                                   (uint32_t)cfg->response)) {
      cr0 |= k_ra_lvd_cr0_mask_ri;
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
static void internal_program_cr1(const ra_lvd_channel_map_t* map, const ra_lvd_cfg_t* cfg)
{
  /* HUM Ch 8.2.6 "PVDmCR1 : Voltage Monitor m Circuit Control Register" p 307*/
  uint8_t cr1 = (uint8_t)((uint8_t)cfg->edge & k_ra_lvd_cr1_mask_idtsel);
  if (cfg->irq_type == k_ra_lvd_irq_maskable) {
    cr1 |= k_ra_lvd_cr1_mask_irqsel;
  }
  *ra_lvd_reg8(map->cr1) = cr1;

  if (cfg->clear_status) {
    /* HUM Ch 8.2.7 "PVDmSR : Voltage Monitor m Circuit Status Register" p 307*/
    *ra_lvd_reg8(map->sr) = 0U;
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
static void internal_lvd_program_cmpcr(const ra_lvd_channel_map_t* map, const ra_lvd_cfg_t* cfg)
{
  /* Step 1: disable everything; HUM Ch 8.2.4 / 8.2.2 */
  *ra_lvd_reg8(map->cr0)   = internal_cr0_apply_reserved(map, 0U);
  *ra_lvd_reg8(map->cmpcr) = 0U;

  /* Step 2: PVDLVL with PVDE = 0 */
  uint8_t cmpcr            = (uint8_t)((uint8_t)cfg->threshold & k_ra_lvd_cmpcr_mask_pvdlvl);
  *ra_lvd_reg8(map->cmpcr) = cmpcr;

  /* Step 3: HUM Ch 8.2.8 / 8.2.9 -- RHSEL on FCR */
  *ra_lvd_reg8(map->fcr) = (uint8_t)((uint8_t)cfg->hysteresis & k_ra_lvd_fcr_mask_rhsel);

  /* Step 4: re-assert PVDE */
  cmpcr |= k_ra_lvd_cmpcr_mask_pvde;
  *ra_lvd_reg8(map->cmpcr) = cmpcr;
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
 * @pre ``ra_lvd_channel_init`` already ran the CMPCR steps.
 * @pre PRCR.PRC3 unlocked (caller-managed).
 * @post Channel is armed per ``cfg``.
 * @post CMPE bit is set on the matching CR0 register.
 *
 * @note Internal helper, not thread-safe.
 * @since 0.1.0
 */
static void internal_lvd_program_cr0_chain(const ra_lvd_channel_map_t* map, const ra_lvd_cfg_t* cfg)
{
  uint8_t cr0            = internal_compose_cr0(map, cfg);
  *ra_lvd_reg8(map->cr0) = internal_cr0_apply_reserved(map, cr0);

  if (cfg->filter_en) {
    cr0 &= (uint8_t)~k_ra_lvd_cr0_mask_dfdis;
    *ra_lvd_reg8(map->cr0) = internal_cr0_apply_reserved(map, cr0);
  }

  if (map->has_irq) {
    internal_program_cr1(map, cfg);
  }

  if (cfg->irq_enable && (cfg->response != k_ra_lvd_response_none)) {
    /* RIE (m-channel: IRQ/reset enable) and RE (n-channel: reset-only)
     * sit at the same CR0 bit position (0x01) on RA8D2; the semantic
     * distinction lives in `map->has_irq`. Either mask gates bit 0. */
    cr0 |= k_ra_lvd_cr0_mask_rie;
    *ra_lvd_reg8(map->cr0) = internal_cr0_apply_reserved(map, cr0);
  }

  cr0 |= k_ra_lvd_cr0_mask_cmpe;
  *ra_lvd_reg8(map->cr0) = internal_cr0_apply_reserved(map, cr0);
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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                  Channel armed per ``cfg``.
 * @retval k_ra_err_null_ptr        ``cfg`` was NULL.
 * @retval k_ra_err_invalid_arg     Channel or config field out of range.
 * @retval k_ra_err_not_supported   Response unavailable for the channel kind.
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
ra_err_t ra_lvd_channel_init(ra_lvd_channel_t channel, const ra_lvd_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");

  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_init: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra_lvd_channel_map_t map     = s_lvd_map[idx];
  const ra_err_t             cfg_err = internal_validate_cfg(&map, cfg);
  RA_RETURN_ON_ERROR(cfg_err, s_tag, "lvd_init: bad cfg"); /* GCOVR_EXCL_BR_LINE */

  internal_lvd_program_cmpcr(&map, cfg);
  internal_lvd_program_cr0_chain(&map, cfg);

  ra_log_info_val(s_tag, "lvd_init ch", (uint32_t)channel);
  return k_ra_ok;
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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Channel disabled.
 * @retval k_ra_err_invalid_arg   Channel mapping failed.
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
ra_err_t ra_lvd_channel_deinit(ra_lvd_channel_t channel)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_deinit: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra_lvd_channel_map_t map = s_lvd_map[idx];

  /* === HUM Table 8.5 step 1 / Table 8.7 step 1 -- drop CMPE. */
  internal_cr0_rmw(&map, k_ra_lvd_cr0_mask_cmpe, 0U);

  /* === HUM Table 8.5 step 3 / Table 8.7 step 3 -- drop RIE / RE. */
  uint8_t enable_bit = k_ra_lvd_cr0_mask_re;
  if (map.has_irq) {
    enable_bit = k_ra_lvd_cr0_mask_rie;
  }
  internal_cr0_rmw(&map, enable_bit, 0U);

  /* === HUM Table 8.5 step 4 / Table 8.7 step 4 -- DFDIS = 1 (filter off). */
  internal_cr0_rmw(&map, 0U, k_ra_lvd_cr0_mask_dfdis);

  /* === HUM Table 8.5 step 5 / Table 8.7 step 5 -- drop PVDE. */
  /* HUM Ch 8.2.2 "PVDmCMPCR" p 303*/
  *ra_lvd_reg8(map.cmpcr) = 0U;

  /* Cleanly drop the rest of the per-channel state. */
  *ra_lvd_reg8(map.cr0) = internal_cr0_apply_reserved(&map, 0U);
  *ra_lvd_reg8(map.fcr) = 0U;
  if (map.has_irq) {
    /* HUM Ch 8.2.6 "PVDmCR1 : Voltage Monitor m Circuit Control Register" p 307*/
    *ra_lvd_reg8(map.cr1) = 0U;
    /* HUM Ch 8.2.7 "PVDmSR : Voltage Monitor m Circuit Status Register" p 307*/
    *ra_lvd_reg8(map.sr) = 0U;
  }
  return k_ra_ok;
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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Threshold updated.
 * @retval k_ra_err_invalid_arg   Channel or threshold out of range.
 *
 * @pre Channel previously brought up via ``ra_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post CMPCR.PVDLVL reflects ``threshold``.
 * @post CMPCR.PVDE state is preserved across the write.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_lvd_set_threshold(ra_lvd_channel_t channel, ra_lvd_pvdlvl_t threshold)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_set_threshold: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra_err_t lvl_err = internal_validate_threshold(threshold);
  RA_RETURN_ON_ERROR(lvl_err, s_tag, "lvd_set_threshold: bad threshold"); /* GCOVR_EXCL_BR_LINE */

  const ra_lvd_channel_map_t map = s_lvd_map[idx];

  /* HUM Ch 8.2.2 "PVDmCMPCR : Voltage Monitor m Comparator Control Register" p 303*/
  /* Preserve the PVDE bit, drop it around the PVDLVL write, restore it afterwards. */
  const uint8_t prev     = *ra_lvd_reg8(map.cmpcr);
  const uint8_t pvde_was = (uint8_t)(prev & k_ra_lvd_cmpcr_mask_pvde);

  *ra_lvd_reg8(map.cmpcr) = 0U;

  uint8_t cmpcr = (uint8_t)((uint8_t)threshold & k_ra_lvd_cmpcr_mask_pvdlvl);
  cmpcr |= pvde_was;
  *ra_lvd_reg8(map.cmpcr) = cmpcr;
  return k_ra_ok;
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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Edge updated.
 * @retval k_ra_err_invalid_arg   Channel or edge out of range.
 * @retval k_ra_err_not_supported Channel has no IRQ path (PVDn).
 *
 * @pre Channel previously brought up via ``ra_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post CR1.IDTSEL reflects ``edge``.
 * @post CR1.IRQSEL is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_lvd_set_irq_edge(ra_lvd_channel_t channel, ra_lvd_edge_t edge)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_set_irq_edge: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra_err_t edge_err = internal_validate_edge(edge);
  RA_RETURN_ON_ERROR(edge_err, s_tag, "lvd_set_irq_edge: bad edge"); /* GCOVR_EXCL_BR_LINE */

  const ra_lvd_channel_map_t map = s_lvd_map[idx];
  if (!map.has_irq) {
    return k_ra_err_not_supported;
  }

  /* HUM Ch 8.2.6 "PVDmCR1 : Voltage Monitor m Circuit Control Register" p 307*/
  const uint8_t prev    = *ra_lvd_reg8(map.cr1);
  const uint8_t next    = (uint8_t)((prev & (uint8_t)~k_ra_lvd_cr1_mask_idtsel) |
                                    ((uint8_t)edge & k_ra_lvd_cr1_mask_idtsel));
  *ra_lvd_reg8(map.cr1) = next;
  return k_ra_ok;
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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Kind updated.
 * @retval k_ra_err_invalid_arg   Channel mapping failed.
 * @retval k_ra_err_not_supported Channel has no IRQ path.
 *
 * @pre Channel previously brought up via ``ra_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post CR1.IRQSEL reflects ``kind``.
 * @post CR1.IDTSEL is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_lvd_set_irq_kind(ra_lvd_channel_t channel, ra_lvd_irq_type_t kind)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_set_irq_kind: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra_lvd_channel_map_t map = s_lvd_map[idx];
  if (!map.has_irq) {
    return k_ra_err_not_supported;
  }

  /* HUM Ch 8.2.6 "PVDmCR1 : Voltage Monitor m Circuit Control Register" p 307*/
  const uint8_t prev = *ra_lvd_reg8(map.cr1);
  uint8_t       next = (uint8_t)(prev & (uint8_t)~k_ra_lvd_cr1_mask_irqsel);
  if (kind == k_ra_lvd_irq_maskable) {
    next |= k_ra_lvd_cr1_mask_irqsel;
  }
  *ra_lvd_reg8(map.cr1) = next;
  return k_ra_ok;
}

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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                IRQ enabled.
 * @retval k_ra_err_invalid_arg   Channel mapping failed.
 * @retval k_ra_err_not_supported Channel has no IRQ path.
 *
 * @pre Channel previously brought up via ``ra_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post CR0.RIE reads as 1.
 * @post Reserved-bit invariant for the channel is preserved.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_lvd_enable_irq(ra_lvd_channel_t channel)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_enable_irq: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra_lvd_channel_map_t map = s_lvd_map[idx];
  if (!map.has_irq) {
    return k_ra_err_not_supported;
  }
  /* set RIE. */
  /* HUM Ch 8.2.4 "PVDmCR0 : Voltage Monitor m Circuit Control Register 0" p 305*/
  internal_cr0_rmw(&map, 0U, k_ra_lvd_cr0_mask_rie);
  return k_ra_ok;
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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                IRQ disabled.
 * @retval k_ra_err_invalid_arg   Channel mapping failed.
 * @retval k_ra_err_not_supported Channel has no IRQ path.
 *
 * @pre Channel previously brought up via ``ra_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post CR0.RIE reads as 0.
 * @post Reserved-bit invariant for the channel is preserved.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_lvd_disable_irq(ra_lvd_channel_t channel)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_disable_irq: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra_lvd_channel_map_t map = s_lvd_map[idx];
  if (!map.has_irq) {
    return k_ra_err_not_supported;
  }
  /* HUM Ch 8.2.4 "PVDmCR0" p 305 */
  internal_cr0_rmw(&map, k_ra_lvd_cr0_mask_rie, 0U);
  return k_ra_ok;
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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Reset response armed.
 * @retval k_ra_err_invalid_arg   Channel mapping failed.
 *
 * @pre Channel previously brought up via ``ra_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post Channel will reset the SoC on the configured crossing.
 * @post Reserved-bit invariant for the channel is preserved.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_lvd_enable_reset(ra_lvd_channel_t channel)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_enable_reset: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra_lvd_channel_map_t map = s_lvd_map[idx];
  if (map.has_irq) {
    /* m channel: set RI then RIE. */
    /* HUM Ch 8.2.4 "PVDmCR0" p 305*/
    internal_cr0_rmw(&map, 0U, (uint8_t)(k_ra_lvd_cr0_mask_ri | k_ra_lvd_cr0_mask_rie));
  } else {
    /* n channel: set RE only -- it is the only response option. */
    /* HUM Ch 8.2.5 "PVDnCR0" p 306*/
    internal_cr0_rmw(&map, 0U, k_ra_lvd_cr0_mask_re);
  }
  return k_ra_ok;
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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Reset response disarmed.
 * @retval k_ra_err_invalid_arg   Channel mapping failed.
 *
 * @pre Channel previously brought up via ``ra_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post Channel will not reset the SoC on subsequent crossings.
 * @post Reserved-bit invariant for the channel is preserved.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_lvd_disable_reset(ra_lvd_channel_t channel)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_disable_reset: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra_lvd_channel_map_t map = s_lvd_map[idx];
  /* HUM Ch 8.2.4 "PVDmCR0" p 305 */
  uint8_t enable_bit = k_ra_lvd_cr0_mask_re;
  if (map.has_irq) {
    enable_bit = k_ra_lvd_cr0_mask_rie;
  }
  internal_cr0_rmw(&map, enable_bit, 0U);
  return k_ra_ok;
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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok               CMPE asserted.
 * @retval k_ra_err_invalid_arg  Channel mapping failed.
 *
 * @pre Channel previously brought up via ``ra_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post CR0.CMPE reads as 1.
 * @post Reserved-bit invariant for the channel is preserved.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_lvd_enable_cmpe(ra_lvd_channel_t channel)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_enable_cmpe: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra_lvd_channel_map_t map = s_lvd_map[idx];
  /* HUM Ch 8.2.4 "PVDmCR0" p 305 */
  internal_cr0_rmw(&map, 0U, k_ra_lvd_cr0_mask_cmpe);
  return k_ra_ok;
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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok               CMPE cleared.
 * @retval k_ra_err_invalid_arg  Channel mapping failed.
 *
 * @pre Channel previously brought up via ``ra_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post CR0.CMPE reads as 0.
 * @post Reserved-bit invariant for the channel is preserved.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_lvd_disable_cmpe(ra_lvd_channel_t channel)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_disable_cmpe: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra_lvd_channel_map_t map = s_lvd_map[idx];
  /* HUM Ch 8.2.4 "PVDmCR0" p 305 */
  internal_cr0_rmw(&map, k_ra_lvd_cr0_mask_cmpe, 0U);
  return k_ra_ok;
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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Filter updated.
 * @retval k_ra_err_invalid_arg   Channel or divider out of range.
 *
 * @pre Channel previously brought up via ``ra_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post CR0.FSAMP reflects ``filter_div``.
 * @post CR0.DFDIS reflects the inverse of ``filter_en``.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_lvd_set_filter(ra_lvd_channel_t channel, ra_lvd_loco_div_t filter_div, bool filter_en)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_set_filter: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra_err_t div_err = internal_validate_div(filter_div);
  RA_RETURN_ON_ERROR(div_err, s_tag, "lvd_set_filter: bad div"); /* GCOVR_EXCL_BR_LINE */

  const ra_lvd_channel_map_t map = s_lvd_map[idx];

  /* HUM Ch 8.2.4 "PVDmCR0" p 305 / HUM Ch 8.2.5 "PVDnCR0" p 306
   * -- FSAMP can only be modified while DFDIS = 1. */
  internal_cr0_rmw(&map, 0U, k_ra_lvd_cr0_mask_dfdis);

  const uint8_t fsamp_bits =
    (uint8_t)(((uint8_t)filter_div << k_ra_lvd_cr0_shift_fsamp) & k_ra_lvd_cr0_mask_fsamp);
  internal_cr0_rmw(&map, k_ra_lvd_cr0_mask_fsamp, fsamp_bits);

  if (filter_en) {
    /* Drop DFDIS only after FSAMP has landed. */
    internal_cr0_rmw(&map, k_ra_lvd_cr0_mask_dfdis, 0U);
  }
  return k_ra_ok;
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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                  Hysteresis mode updated.
 * @retval k_ra_err_invalid_arg     Channel or mode out of range.
 * @retval k_ra_err_invalid_state   HVD requested with RI=0 (m channel only).
 *
 * @pre Channel previously brought up via ``ra_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post FCR.RHSEL reflects ``hyst``.
 * @post CMPCR.PVDE state is preserved across the write.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_lvd_set_hysteresis_mode(ra_lvd_channel_t channel, ra_lvd_hysteresis_t hyst)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err,
                     s_tag,
                     "lvd_set_hysteresis_mode: bad channel"); /* GCOVR_EXCL_BR_LINE */

  if ((uint8_t)hyst > k_ra_lvd_hysteresis_hvd) {
    return k_ra_err_invalid_arg;
  }

  const ra_lvd_channel_map_t map = s_lvd_map[idx];

  /* HUM Ch 8.2.8 "PVDmFCR" p 308 last bullet -- "RHSEL must not be set
   * to 1 when PVDmCR0.RI = 0." Enforce only on m channels (n channels
   * have no RI bit; their RHSEL is always legal). */
  if (map.has_irq && (hyst == k_ra_lvd_hysteresis_hvd)) {
    if (internal_read_ri(&map) == 0U) {
      return k_ra_err_invalid_state;
    }
  }

  /* HUM Ch 8.2.8 / 8.2.9 -- RHSEL can only be modified when every
   * PVDE is 0; preserve and restore this channel's PVDE. */
  const uint8_t prev_cmpcr = *ra_lvd_reg8(map.cmpcr);
  const uint8_t pvde_was   = (uint8_t)(prev_cmpcr & k_ra_lvd_cmpcr_mask_pvde);
  *ra_lvd_reg8(map.cmpcr)  = (uint8_t)(prev_cmpcr & (uint8_t)~k_ra_lvd_cmpcr_mask_pvde);

  /* HUM Ch 8.2.8 "PVDmFCR" p 308 */
  *ra_lvd_reg8(map.fcr) = (uint8_t)((uint8_t)hyst & k_ra_lvd_fcr_mask_rhsel);

  /* Restore PVDE if it had been set. */
  *ra_lvd_reg8(map.cmpcr) = (uint8_t)(*ra_lvd_reg8(map.cmpcr) | pvde_was);
  return k_ra_ok;
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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                  RN updated.
 * @retval k_ra_err_invalid_arg     Channel or mode out of range.
 * @retval k_ra_err_not_supported   Channel has no IRQ path (PVDn).
 * @retval k_ra_err_invalid_state   RN=1 conflicts with FCR.RHSEL=1.
 *
 * @pre Channel previously brought up via ``ra_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post CR0.RN reflects ``negate``.
 * @post Reserved-bit invariant for the channel is preserved.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_lvd_set_negate_mode(ra_lvd_channel_t channel, ra_lvd_negate_t negate)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_set_negate_mode: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra_lvd_channel_map_t map = s_lvd_map[idx];
  if (!map.has_irq) {
    return k_ra_err_not_supported;
  }
  if ((uint8_t)negate > k_ra_lvd_negate_after_assert) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 8.2.4 "PVDmCR0" p 306 */
  /* HUM Ch 8.2.8 "PVDmFCR" p 308 */
  if (negate == k_ra_lvd_negate_after_assert) {
    const uint8_t fcr = *ra_lvd_reg8(map.fcr);
    if ((fcr & k_ra_lvd_fcr_mask_rhsel) != 0U) {
      return k_ra_err_invalid_state;
    }
  }

  /* HUM Ch 8.2.4 "PVDmCR0" p 305*/
  internal_cr0_rmw(&map,
                   k_ra_lvd_cr0_mask_rn,
                   (negate == k_ra_lvd_negate_after_assert) ? k_ra_lvd_cr0_mask_rn : 0U);
  return k_ra_ok;
}

/* =============================================================================
 * Status read / clear
 * =============================================================================
 */

/**
 * @brief Read PVDmSR and decode DET / MON into ::ra_lvd_status_t.
 *
 * @details
 * Reads the status register documented at HUM Ch 12.2.7 "PVDmSR"
 * p 600 and unpacks the latched-crossing (DET) and live-comparator
 * (MON) bits.
 *
 * @param[in]  channel PVDm channel id.
 * @param[out] out     Receives the decoded status.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                  Status decoded.
 * @retval k_ra_err_null_ptr        ``out`` was NULL.
 * @retval k_ra_err_invalid_arg     Channel mapping failed.
 * @retval k_ra_err_not_supported   Channel has no IRQ path (PVDn).
 *
 * @pre ``out`` non-NULL.
 * @pre Channel previously brought up via ``ra_lvd_channel_init``.
 * @post ``*out`` reflects the live SR contents.
 * @post Hardware state is unchanged.
 *
 * @note Read-only; safe under simple races.
 * @since 0.1.0
 */
ra_err_t ra_lvd_get_status(ra_lvd_channel_t channel, ra_lvd_status_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");

  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_get_status: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra_lvd_channel_map_t map = s_lvd_map[idx];
  if (!map.has_irq) {
    return k_ra_err_not_supported;
  }

  /* HUM Ch 8.2.7 "PVDmSR : Voltage Monitor m Circuit Status Register" p 307*/
  const uint8_t sr = *ra_lvd_reg8(map.sr);
  out->crossed     = ((sr & k_ra_lvd_sr_mask_det) != 0U);
  out->above       = ((sr & k_ra_lvd_sr_mask_mon) != 0U);
  return k_ra_ok;
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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                  DET acknowledged.
 * @retval k_ra_err_invalid_arg     Channel mapping failed.
 * @retval k_ra_err_not_supported   Channel has no IRQ path (PVDn).
 *
 * @pre Channel previously brought up via ``ra_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post SR.DET reads as 0.
 * @post SR.MON is unchanged.
 *
 * @note Not thread-safe (read-modify-write on SR).
 * @since 0.1.0
 */
ra_err_t ra_lvd_clear_status(ra_lvd_channel_t channel)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_clear_status: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra_lvd_channel_map_t map = s_lvd_map[idx];
  if (!map.has_irq) {
    return k_ra_err_not_supported;
  }

  /* HUM Ch 8.2.7 "PVDmSR : Voltage Monitor m Circuit Status Register" p 307
   * Note 1 -- only 0 can be written to DET. MON is preserved by
   * clearing only the DET bit. */
  const uint8_t sr     = *ra_lvd_reg8(map.sr);
  const uint8_t next   = (uint8_t)(sr & (uint8_t)~k_ra_lvd_sr_mask_det);
  *ra_lvd_reg8(map.sr) = next;
  return k_ra_ok;
}

/* =============================================================================
 * Security attribution + n-channel lock
 * =============================================================================
 */

/**
 * @brief Program PVDSAR security attribution for PVD channels.
 *
 * @details
 * Writes the security attribution mask (HUM Ch 12.2.1 "PVDSAR"
 * p 594) so each PVD channel can be claimed by Secure or Non-secure.
 *
 * @param[in] mask Bitwise-OR of ``k_ra_lvd_pvdsar_*`` constants.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Mask written.
 * @retval k_ra_err_invalid_arg   ``mask`` includes undefined bits.
 *
 * @pre PRCR.PRC3 unlocked (caller-managed).
 * @pre Caller is in Secure world (PVDSAR is S-only).
 * @post PVDSAR reflects ``mask``.
 * @post Subsequent PVD register accesses honour the new attribution.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_lvd_set_security(uint32_t mask)
{
  /* HUM Ch 8.2.1 "PVDSAR : Programmable Voltage Detection Security
   * Attribution Register" p 302 */
  if ((mask & ~k_ra_lvd_pvdsar_mask_all) != 0U) {
    return k_ra_err_invalid_arg;
  }
  *ra_lvd_reg32(k_ra_lvd_pvdsar_off) = mask;
  return k_ra_ok;
}

/**
 * @brief Unlock PVDLR so PVDn channels accept further writes.
 *
 * @details
 * Writes the documented unlock pattern to PVDLR (HUM Ch 12.2.10
 * "PVDLR" p 601) so subsequent ``ra_lvd_*`` calls on PVDn channels
 * are honoured.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Lock released.
 *
 * @pre PRCR.PRC3 unlocked (caller-managed).
 * @pre Caller plans to update PVDn state next.
 * @post PVDLR.LOCK reads as 0.
 * @post PVDn channel writes are accepted.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_lvd_unlock_n_channels(void)
{
  /* HUM Ch 8.2.10 "PVDLR : Voltage Monitor Lock Register" p 309*/
  *ra_lvd_reg8(k_ra_lvd_pvdlr_off) = k_ra_lvd_pvdlr_value_unlock;
  return k_ra_ok;
}

/**
 * @brief Relock PVDLR after a PVDn maintenance window.
 *
 * @details
 * Writes any non-unlock pattern to PVDLR (HUM Ch 12.2.10 p 601 --
 * "if you write an arbitrary value to the LOCK, the LOCK bit is
 * fixed to 1.").
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Lock re-engaged.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre PVDn updates are complete and safe to freeze.
 * @post PVDLR.LOCK reads as 1.
 * @post Further PVDn writes are silently ignored.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_lvd_relock_n_channels(void)
{
  /* HUM Ch 8.2.10 "PVDLR : Voltage Monitor Lock Register" p 309 --
   * "if you write an arbitrary value to the LOCK, the LOCK bit is
   * fixed to 1." */
  *ra_lvd_reg8(k_ra_lvd_pvdlr_off) = k_ra_lvd_pvdlr_value_relock;
  return k_ra_ok;
}

/* =============================================================================
 * ELC + standby helpers
 * =============================================================================
 */

/**
 * @brief Arm the ELC event line for a PVDm channel.
 *
 * @details
 * Per HUM Ch 12.7 "Event Link Controller (ELC) Output" p 606 the
 * event line tracks CMPE; this helper acknowledges any latched DET
 * before raising CMPE so the ELC consumer sees a clean transition.
 *
 * @param[in] channel PVDm channel id.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                  ELC line armed.
 * @retval k_ra_err_invalid_arg     Channel mapping failed.
 * @retval k_ra_err_not_supported   Channel has no IRQ path (PVDn).
 *
 * @pre Channel previously brought up via ``ra_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post SR.DET reads as 0; CR0.CMPE reads as 1.
 * @post ELC consumer receives subsequent crossings as events.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_lvd_enable_elc_event(ra_lvd_channel_t channel)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_enable_elc_event: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra_lvd_channel_map_t map = s_lvd_map[idx];
  if (!map.has_irq) {
    return k_ra_err_not_supported;
  }

  /* HUM Ch 8.7 "Event Link Controller (ELC) Output" p 315 -- the event
   * line tracks the comparator output enable: clear DET first, then
   * raise CMPE. */
  /* HUM Ch 8.2.7 "PVDmSR : Voltage Monitor m Circuit Status Register" p 307*/
  const uint8_t sr     = *ra_lvd_reg8(map.sr);
  *ra_lvd_reg8(map.sr) = (uint8_t)(sr & (uint8_t)~k_ra_lvd_sr_mask_det);
  /* HUM Ch 8.2.4 "PVDmCR0" p 305*/
  internal_cr0_rmw(&map, 0U, k_ra_lvd_cr0_mask_cmpe);
  return k_ra_ok;
}

/**
 * @brief Disarm the ELC event line for a PVDm channel.
 *
 * @details
 * Per HUM Ch 12.7 p 606, clears CMPE so the ELC line goes inactive.
 *
 * @param[in] channel PVDm channel id.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                  ELC line disarmed.
 * @retval k_ra_err_invalid_arg     Channel mapping failed.
 * @retval k_ra_err_not_supported   Channel has no IRQ path (PVDn).
 *
 * @pre Channel previously brought up via ``ra_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post CR0.CMPE reads as 0.
 * @post ELC consumer no longer receives crossings as events.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_lvd_disable_elc_event(ra_lvd_channel_t channel)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_disable_elc_event: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra_lvd_channel_map_t map = s_lvd_map[idx];
  if (!map.has_irq) {
    return k_ra_err_not_supported;
  }
  /* HUM Ch 8.2.4 "PVDmCR0" p 305 -- clear CMPE so the ELC line goes
   * inactive (HUM 8.7 p 315). */
  internal_cr0_rmw(&map, k_ra_lvd_cr0_mask_cmpe, 0U);
  return k_ra_ok;
}

/**
 * @brief Reconfigure a PVD channel for entry into Software / Deep Standby.
 *
 * @details
 * Per HUM Ch 12.5 (p 602-603), the digital filter must be disabled
 * and (for m channels) RI / RN cleared before entering standby.
 *
 * @param[in] channel PVD channel id.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Channel reconfigured for standby.
 * @retval k_ra_err_invalid_arg   Channel mapping failed.
 *
 * @pre Channel previously brought up via ``ra_lvd_channel_init``.
 * @pre Caller is preparing the SoC for standby entry.
 * @post CR0.DFDIS reads as 1.
 * @post For m channels CR0.RI and CR0.RN read as 0.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_lvd_configure_for_standby(ra_lvd_channel_t channel)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err,
                     s_tag,
                     "lvd_configure_for_standby: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra_lvd_channel_map_t map = s_lvd_map[idx];

  /* HUM Ch 8.5(1) "Setting in Software Standby mode" p 311 +
   * HUM Ch 8.5(2) "Settings in Deep Software Standby mode" p 312 --
   * disable the digital filter and (for m channels) clear RI + RN. */
  uint8_t set_bits = k_ra_lvd_cr0_mask_dfdis;
  uint8_t clr_bits = 0U;
  if (map.has_irq) {
    clr_bits |= k_ra_lvd_cr0_mask_ri;
    clr_bits |= k_ra_lvd_cr0_mask_rn;
  }
  /* HUM Ch 8.2.4 "PVDmCR0" p 305*/
  internal_cr0_rmw(&map, clr_bits, set_bits);
  return k_ra_ok;
}

/**
 * @brief Drop CR0.RI on every NMI-capable channel (deep-standby exit).
 *
 * @details
 * Per HUM Ch 12.2.4 (p 597), prepares the PVD subsystem for normal
 * operation after exiting Deep Standby by clearing the per-channel
 * RI bit (which is documented to be UD-only across DPSTBY).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Path cleared on every NMI channel.
 *
 * @pre PRCR.PRC3 unlocked (caller-managed).
 * @pre Caller has just exited Deep Software Standby.
 * @post CR0.RI reads as 0 on every NMI-capable channel.
 * @post Subsequent crossings will not auto-reset the SoC.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_lvd_cancel_deep_standby_path(void)
{
  /* HUM Ch 8.2.4 "PVDmCR0" p 305 */
  for (uint8_t i = 0U; i < k_ra_lvd_nmi_channel_count; ++i) {
    internal_cr0_rmw(&s_lvd_map[i], k_ra_lvd_cr0_mask_ri, 0U);
  }
  return k_ra_ok;
}

/* =============================================================================
 * Filter timing helper (pure)
 * =============================================================================
 */

/**
 * @brief Compute the digital-filter settle delay in microseconds.
 *
 * @details
 * Implements the "2s + 3 cycles of the LOCO" formula from HUM
 * Table 12.4 / 12.6 step 8 (s = 2^(div+1)). Matches FSP's
 * ``r_lvd_filter_delay`` rounded-up microseconds.
 *
 * @param[in] div     LOCO divider used by the channel.
 * @param[in] loco_hz LOCO frequency in Hz (0 -> default 32_768 Hz).
 *
 * @return Settle delay in microseconds (rounded up by +1 us).
 * @retval >=1 Microseconds the caller must wait after FSAMP changes.
 *
 * @pre None.
 * @pre Caller will block / sleep at least the returned amount.
 * @post Hardware state is unchanged.
 * @post Return value is in microseconds.
 *
 * @note Pure function; safe from any context.
 * @since 0.1.0
 */
uint32_t ra_lvd_filter_delay_us(ra_lvd_loco_div_t div, uint32_t loco_hz)
{
  /* HUM Table 8.4 step 8 (p 312) / Table 8.6 step 8 (p 315): wait for
   * "2s + 3 cycles of the LOCO" where s = 2^(div+1). */
  uint8_t safe_div = (uint8_t)div;
  if (safe_div > k_ra_lvd_loco_div_max) {
    safe_div = k_ra_lvd_loco_div_max;
  }
  const uint32_t s_factor    = (uint32_t)1U << (safe_div + 1U);
  const uint32_t loco_cycles = (k_ra_lvd_filter_factor * s_factor) + k_ra_lvd_filter_extra;
  const uint32_t hz          = (loco_hz != 0U) ? loco_hz : k_ra_lvd_loco_hz_default;
  /* +1 us round-up matches FSP's r_lvd_filter_delay computation. */
  return ((loco_cycles * k_ra_lvd_us_per_sec) / hz) + 1U;
}

/* =============================================================================
 * Callback registration + ISR demux
 * =============================================================================
 */

/**
 * @brief Register the shared LVD ISR callback (NULL detaches).
 *
 * @details
 * Stores ``(fn, ctx)`` in the shared slot consulted by every
 * ``ra_lvd_dispatch`` call when no per-channel handler is attached.
 *
 * @param[in] fn  Callback function or NULL to detach.
 * @param[in] ctx Opaque value forwarded verbatim to ``fn``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok               Slot updated.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Lifetime of ``ctx`` outlives the next ``ra_lvd_dispatch`` call.
 * @post Shared callback slot reflects ``(fn, ctx)``.
 * @post Per-channel slots are unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_lvd_attach_handler(ra_lvd_event_fn_t fn, void* ctx)
{
  s_lvd_fn  = fn;
  s_lvd_ctx = ctx;
  return k_ra_ok;
}

/**
 * @brief Register a per-channel LVD callback for a PVDm channel.
 *
 * @details
 * Per-channel handlers take precedence over the shared callback set
 * by ``ra_lvd_attach_handler``. Available only on PVDm channels.
 *
 * @param[in] channel PVDm channel id.
 * @param[in] fn      Callback function or NULL to detach.
 * @param[in] ctx     Opaque value forwarded verbatim to ``fn``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Slot updated.
 * @retval k_ra_err_invalid_arg   Channel mapping failed.
 * @retval k_ra_err_not_supported Channel has no IRQ path (PVDn).
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Lifetime of ``ctx`` outlives the next ``ra_lvd_dispatch`` call.
 * @post Per-channel slot for ``channel`` reflects ``(fn, ctx)``.
 * @post Shared callback slot is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_lvd_attach_channel_handler(ra_lvd_channel_t channel, ra_lvd_event_fn_t fn, void* ctx)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err,
                     s_tag,
                     "lvd_attach_channel_handler: bad channel"); /* GCOVR_EXCL_BR_LINE */

  const ra_lvd_channel_map_t map = s_lvd_map[idx];
  if (!map.has_irq) {
    return k_ra_err_not_supported;
  }
  s_lvd_chan_fn[idx]  = fn;
  s_lvd_chan_ctx[idx] = ctx;
  return k_ra_ok;
}

/**
 * @brief Demux a PVD ISR -- ack DET, fire per-channel + shared callbacks.
 *
 * @details
 * Snapshots PVDmSR.DET; if no crossing is latched the call is a
 * no-op. Otherwise invokes the per-channel callback (if any), then
 * the shared callback (if any), then clears DET via the W0C
 * sequence (HUM Ch 12.2.7 "PVDmSR" p 600).
 *
 * @param[in] channel PVD channel id that fired the IRQ / NMI.
 *
 * @pre Called from PVD ISR or NMI context.
 * @pre PRCR.PRC3 unlocked (caller-managed during ack).
 *
 * @post If a callback was attached, it has been invoked exactly once.
 * @post PVDmSR.DET is 0 on return.
 *
 * @note Thread safety: ISR-context only.
 * @since 0.1.0
 */
void ra_lvd_dispatch(ra_lvd_channel_t channel)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  if (map_err != k_ra_ok) {
    return;
  }
  const ra_lvd_channel_map_t map = s_lvd_map[idx];

  /* Only m channels have a status register / IRQ path. */
  if (!map.has_irq) {
    return;
  }

  /* HUM Ch 8.2.7 "PVDmSR : Voltage Monitor m Circuit Status Register" p 307
   * -- only fire the callback if a crossing was actually latched. */
  const uint8_t sr = *ra_lvd_reg8(map.sr);
  if ((sr & k_ra_lvd_sr_mask_det) == 0U) {
    return;
  }

  /* Per-channel callback wins; otherwise fall back to the shared one. */
  const ra_lvd_event_fn_t chan_fn  = s_lvd_chan_fn[idx];
  void* const             chan_ctx = s_lvd_chan_ctx[idx];
  const ra_lvd_event_fn_t fn       = (chan_fn != nullptr) ? chan_fn : s_lvd_fn;
  void* const             ctx      = (chan_fn != nullptr) ? chan_ctx : s_lvd_ctx;
  if (fn != nullptr) {
    fn(ctx, channel);
  }

  /* Write 0 to DET so the next crossing can latch (HUM 8.2.7 Note 1 p 307). */
  const uint8_t cleared = (uint8_t)(*ra_lvd_reg8(map.sr) & (uint8_t)~k_ra_lvd_sr_mask_det);
  *ra_lvd_reg8(map.sr)  = cleared;
}
