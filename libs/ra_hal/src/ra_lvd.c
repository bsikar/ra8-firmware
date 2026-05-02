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
  if ((cfg->hysteresis == k_ra_lvd_hysteresis_hvd) &&
      (cfg->negate == k_ra_lvd_negate_after_assert)) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/**
 * @brief Compute the initial CR0 value for ``ra_lvd_channel_init``.
 *
 * @details
 * Encodes FSAMP, DFDIS, RN and RI per HUM Ch 8.2.4 / 8.2.5 (pp 305-306).
 * The DFDIS bit is left asserted; the caller drops it after the first
 * CR0 write if ``cfg->filter_en`` is true.
 *
 * @param[in] map Channel map (decides whether RN/RI exist).
 * @param[in] cfg Caller-supplied config.
 *
 * @return Encoded CR0 byte (without the reserved-bit overlay applied).
 *
 * @pre ``map`` and ``cfg`` are non-null and refer to the same channel.
 * @post No side effects.
 *
 * @note Internal helper, not thread-safe.
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
    if ((cfg->response == k_ra_lvd_response_reset) ||
        (cfg->response == k_ra_lvd_response_reset_on_rise)) {
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
 * @post CR1 reflects ``cfg``, DET cleared if ``cfg->clear_status``.
 *
 * @note Internal helper, not thread-safe.
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
 * @post CMPCR/FCR reflect ``cfg``; PVDE asserted.
 *
 * @note Internal helper, not thread-safe.
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
 * @post Channel is armed per ``cfg``.
 *
 * @note Internal helper, not thread-safe.
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

ra_err_t ra_lvd_channel_init(ra_lvd_channel_t channel, const ra_lvd_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");

  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_init: bad channel");

  const ra_lvd_channel_map_t map     = s_lvd_map[idx];
  const ra_err_t             cfg_err = internal_validate_cfg(&map, cfg);
  RA_RETURN_ON_ERROR(cfg_err, s_tag, "lvd_init: bad cfg");

  internal_lvd_program_cmpcr(&map, cfg);
  internal_lvd_program_cr0_chain(&map, cfg);

  ra_log_info_val(s_tag, "lvd_init ch", (uint32_t)channel);
  return k_ra_ok;
}

ra_err_t ra_lvd_channel_deinit(ra_lvd_channel_t channel)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_deinit: bad channel");

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

ra_err_t ra_lvd_set_threshold(ra_lvd_channel_t channel, ra_lvd_pvdlvl_t threshold)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_set_threshold: bad channel");

  const ra_err_t lvl_err = internal_validate_threshold(threshold);
  RA_RETURN_ON_ERROR(lvl_err, s_tag, "lvd_set_threshold: bad threshold");

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

ra_err_t ra_lvd_set_irq_edge(ra_lvd_channel_t channel, ra_lvd_edge_t edge)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_set_irq_edge: bad channel");

  const ra_err_t edge_err = internal_validate_edge(edge);
  RA_RETURN_ON_ERROR(edge_err, s_tag, "lvd_set_irq_edge: bad edge");

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

ra_err_t ra_lvd_set_irq_kind(ra_lvd_channel_t channel, ra_lvd_irq_type_t kind)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_set_irq_kind: bad channel");

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

ra_err_t ra_lvd_enable_irq(ra_lvd_channel_t channel)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_enable_irq: bad channel");

  const ra_lvd_channel_map_t map = s_lvd_map[idx];
  if (!map.has_irq) {
    return k_ra_err_not_supported;
  }
  /* set RIE. */
  /* HUM Ch 8.2.4 "PVDmCR0 : Voltage Monitor m Circuit Control Register 0" p 305*/
  internal_cr0_rmw(&map, 0U, k_ra_lvd_cr0_mask_rie);
  return k_ra_ok;
}

ra_err_t ra_lvd_disable_irq(ra_lvd_channel_t channel)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_disable_irq: bad channel");

  const ra_lvd_channel_map_t map = s_lvd_map[idx];
  if (!map.has_irq) {
    return k_ra_err_not_supported;
  }
  /* HUM Ch 8.2.4 "PVDmCR0" p 305 */
  internal_cr0_rmw(&map, k_ra_lvd_cr0_mask_rie, 0U);
  return k_ra_ok;
}

ra_err_t ra_lvd_enable_reset(ra_lvd_channel_t channel)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_enable_reset: bad channel");

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

ra_err_t ra_lvd_disable_reset(ra_lvd_channel_t channel)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_disable_reset: bad channel");

  const ra_lvd_channel_map_t map = s_lvd_map[idx];
  /* HUM Ch 8.2.4 "PVDmCR0" p 305 */
  uint8_t enable_bit = k_ra_lvd_cr0_mask_re;
  if (map.has_irq) {
    enable_bit = k_ra_lvd_cr0_mask_rie;
  }
  internal_cr0_rmw(&map, enable_bit, 0U);
  return k_ra_ok;
}

ra_err_t ra_lvd_enable_cmpe(ra_lvd_channel_t channel)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_enable_cmpe: bad channel");

  const ra_lvd_channel_map_t map = s_lvd_map[idx];
  /* HUM Ch 8.2.4 "PVDmCR0" p 305 */
  internal_cr0_rmw(&map, 0U, k_ra_lvd_cr0_mask_cmpe);
  return k_ra_ok;
}

ra_err_t ra_lvd_disable_cmpe(ra_lvd_channel_t channel)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_disable_cmpe: bad channel");

  const ra_lvd_channel_map_t map = s_lvd_map[idx];
  /* HUM Ch 8.2.4 "PVDmCR0" p 305 */
  internal_cr0_rmw(&map, k_ra_lvd_cr0_mask_cmpe, 0U);
  return k_ra_ok;
}

/* =============================================================================
 * Filter + RHSEL + RN runtime updates
 * =============================================================================
 */

ra_err_t ra_lvd_set_filter(ra_lvd_channel_t channel, ra_lvd_loco_div_t filter_div, bool filter_en)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_set_filter: bad channel");

  const ra_err_t div_err = internal_validate_div(filter_div);
  RA_RETURN_ON_ERROR(div_err, s_tag, "lvd_set_filter: bad div");

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

ra_err_t ra_lvd_set_hysteresis_mode(ra_lvd_channel_t channel, ra_lvd_hysteresis_t hyst)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_set_hysteresis_mode: bad channel");

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

ra_err_t ra_lvd_set_negate_mode(ra_lvd_channel_t channel, ra_lvd_negate_t negate)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_set_negate_mode: bad channel");

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

ra_err_t ra_lvd_get_status(ra_lvd_channel_t channel, ra_lvd_status_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");

  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_get_status: bad channel");

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

ra_err_t ra_lvd_clear_status(ra_lvd_channel_t channel)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_clear_status: bad channel");

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

ra_err_t ra_lvd_unlock_n_channels(void)
{
  /* HUM Ch 8.2.10 "PVDLR : Voltage Monitor Lock Register" p 309*/
  *ra_lvd_reg8(k_ra_lvd_pvdlr_off) = k_ra_lvd_pvdlr_value_unlock;
  return k_ra_ok;
}

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

ra_err_t ra_lvd_enable_elc_event(ra_lvd_channel_t channel)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_enable_elc_event: bad channel");

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

ra_err_t ra_lvd_disable_elc_event(ra_lvd_channel_t channel)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_disable_elc_event: bad channel");

  const ra_lvd_channel_map_t map = s_lvd_map[idx];
  if (!map.has_irq) {
    return k_ra_err_not_supported;
  }
  /* HUM Ch 8.2.4 "PVDmCR0" p 305 -- clear CMPE so the ELC line goes
   * inactive (HUM 8.7 p 315). */
  internal_cr0_rmw(&map, k_ra_lvd_cr0_mask_cmpe, 0U);
  return k_ra_ok;
}

ra_err_t ra_lvd_configure_for_standby(ra_lvd_channel_t channel)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_configure_for_standby: bad channel");

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

ra_err_t ra_lvd_attach_handler(ra_lvd_event_fn_t fn, void* ctx)
{
  s_lvd_fn  = fn;
  s_lvd_ctx = ctx;
  return k_ra_ok;
}

ra_err_t ra_lvd_attach_channel_handler(ra_lvd_channel_t channel, ra_lvd_event_fn_t fn, void* ctx)
{
  uint8_t        idx     = 0U;
  const ra_err_t map_err = internal_channel_to_idx(channel, &idx);
  RA_RETURN_ON_ERROR(map_err, s_tag, "lvd_attach_channel_handler: bad channel");

  const ra_lvd_channel_map_t map = s_lvd_map[idx];
  if (!map.has_irq) {
    return k_ra_err_not_supported;
  }
  s_lvd_chan_fn[idx]  = fn;
  s_lvd_chan_ctx[idx] = ctx;
  return k_ra_ok;
}

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
