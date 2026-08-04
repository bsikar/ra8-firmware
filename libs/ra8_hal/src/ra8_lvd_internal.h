/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_lvd_internal.h
 * @brief Test-access surface for ra8_lvd internal helpers (MC/DC).
 * @ingroup grp_hal_system
 *
 * @details
 * Not part of the public API. Tests under tests/ MAY include this
 * header to drive compound boolean decisions that sit in TU-private
 * helpers behind the public ra8_lvd facade.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_lvd.h"

/* =============================================================================
 * Cross-TU channel-map sharing
 *
 * The ra8_lvd driver is split across multiple translation units
 * (ra8_lvd.c plus ra8_lvd_runtime.c / ra8_lvd_events.c). The channel-map
 * descriptor, its index enum, the lookup table, and the handful of
 * register helpers below are referenced by more than one TU, so they
 * live here -- defined once in ra8_lvd.c, declared for every consumer.
 * =============================================================================
 */

/**
 * @struct ra8_lvd_channel_map_t
 * @brief Per-channel register-offset bundle.
 *
 * @details
 * Channels 1 and 2 use the m-series offsets and have valid CR1 / SR
 * registers. Channels 4 and 5 use the n-series offsets and have no
 * CR1 / SR (the ``has_irq`` flag flips them off). FCR is exposed for
 * all four channels (HUM 8.2.8 m and 8.2.9 n).
 */
typedef struct {
  ra8_lvd_off_t cmpcr;   /**< PVDxCMPCR offset.                      */
  ra8_lvd_off_t cr0;     /**< PVDxCR0 offset.                        */
  ra8_lvd_off_t cr1;     /**< PVDxCR1 (m only; n: same as cr0 stub). */
  ra8_lvd_off_t sr;      /**< PVDxSR  (m only; n: same as cr0 stub). */
  ra8_lvd_off_t fcr;     /**< PVDxFCR (RHSEL).                       */
  bool          has_irq; /**< true for monitor m channels (1, 2).    */
} ra8_lvd_channel_map_t;

/**
 * @enum ra8_lvd_map_idx_t
 * @brief Indices into ``s_lvd_map``.
 */
typedef enum : uint8_t {
  k_ra8_lvd_map_idx_ch1   = 0U, /**< PVD1.     */
  k_ra8_lvd_map_idx_ch2   = 1U, /**< PVD2.     */
  k_ra8_lvd_map_idx_ch4   = 2U, /**< PVD4.     */
  k_ra8_lvd_map_idx_ch5   = 3U, /**< PVD5.     */
  k_ra8_lvd_map_idx_count = 4U, /**< Sentinel. */
} ra8_lvd_map_idx_t;

/**
 * @var s_lvd_map
 * @brief Lookup table from ``ra8_lvd_map_idx_t`` to register offsets.
 *
 * @details
 * Defined once in ra8_lvd.c; declared here so the runtime / events
 * translation units can read the same descriptor table.
 *
 * @note PRIVATE to the ra8_lvd driver TUs.
 * @warning Read-only; do not mutate.
 * @since 0.1.0
 */
extern const ra8_lvd_channel_map_t s_lvd_map[k_ra8_lvd_map_idx_count];

/**
 * @brief Translate a public channel id to an index into ``s_lvd_map``.
 *
 * @param[in]  channel Public channel id.
 * @param[out] out_idx On success, the index in [0..3].
 * @return ``k_ra8_ok`` if mapping succeeded, ``k_ra8_err_invalid_arg`` otherwise.
 *
 * @details
 * Folds the public ::ra8_lvd_channel_t enumerator (which uses the
 * datasheet's PVD1/2/4/5 numbering) into a 0..3 array index used by
 * the internal ``s_lvd_map`` lookup table. Shared across the ra8_lvd
 * translation units.
 *
 * @retval k_ra8_ok               Mapping succeeded.
 * @retval k_ra8_err_invalid_arg  ``channel`` is not one of PVD1/2/4/5.
 *
 * @pre out_idx != nullptr.
 * @pre channel may be any uint8_t.
 * @post On success, *out_idx is in [0, k_ra8_lvd_map_idx_count).
 * @post On failure, *out_idx is unchanged.
 *
 * @note Pure helper; safe from any context. Driver-internal.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_lvd_internal_channel_to_idx(ra8_lvd_channel_t channel, uint8_t* out_idx);

/**
 * @brief Validate FSAMP[1:0] candidate (0..3).
 *
 * @details
 * FSAMP[1:0] selects the LOCO sample-divider for the digital filter
 * (HUM Ch 12.2.6 "PVDmCR1" p 599). All four encodings are valid.
 * Shared across the ra8_lvd translation units.
 *
 * @param[in] div Divider candidate.
 * @return k_ra8_ok or k_ra8_err_invalid_arg.
 * @retval k_ra8_ok               Divider within enum domain.
 * @retval k_ra8_err_invalid_arg  Divider above the documented maximum.
 *
 * @pre None.
 * @pre Caller has not yet committed FSAMP to a register.
 * @post Hardware state unchanged.
 * @post Return value reflects the bounds check only.
 *
 * @note Pure helper; safe from any context. Driver-internal.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_lvd_internal_validate_div(ra8_lvd_loco_div_t div);

/**
 * @brief Read PVDmCR0.RI for an m channel.
 *
 * @details
 * Returns the Reset-on-Voltage bit (HUM Ch 12.2.4 "PVDmCR0" p 597)
 * so the caller can decide whether to issue a software reset on a
 * detected voltage crossing. Shared across the ra8_lvd TUs.
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
 * @note Read-only; safe under simple races. Driver-internal.
 * @since 0.1.0
 */
RA8_PRIV uint8_t ra8_lvd_internal_read_ri(const ra8_lvd_channel_map_t* map);

/**
 * @brief Read-modify-write helper for PVDmCR0 / PVDnCR0 with reserved-bit
 *        rewrite forced.
 *
 * @details
 * Reads PVDxCR0, masks out ``clear_mask``, ORs in ``set_bits``, then
 * folds the channel's mandatory reserved-bit pattern back in
 * (HUM Ch 12.2.4 / 12.2.5). Shared across the ra8_lvd TUs.
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
 * @note Internal helper; not thread-safe. Driver-internal.
 * @since 0.1.0
 */
RA8_PRIV void
ra8_lvd_internal_cr0_rmw(const ra8_lvd_channel_map_t* map, uint8_t clear_mask, uint8_t set_bits);

/**
 * @brief Pure predicate for "RN=1 prohibited when RHSEL=1" rejection.
 *
 * @details
 * Returns true iff @p hysteresis equals the HVD code AND @p negate
 * equals the after-assert code.  Promoted from the inline compound
 * AND at libs/ra8_hal/src/ra8_lvd.c.
 *
 * @param[in] hvd_val           Numeric value of @c k_ra8_lvd_hysteresis_hvd.
 * @param[in] after_assert_val  Numeric value of @c k_ra8_lvd_negate_after_assert.
 * @param[in] hysteresis        Candidate hysteresis value.
 * @param[in] negate            Candidate negate value.
 *
 * @return Boolean reject-config predicate.
 * @retval true  Caller must return @c k_ra8_err_invalid_arg.
 * @retval false Combination is allowed.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the four inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * 2-condition AND; N+1 = 3 vectors:
 *  - hyst!=hvd, neg=after  -> false
 *  - hyst=hvd,  neg=after  -> true  (varies hyst)
 *  - hyst=hvd,  neg!=after -> false (varies neg)
 *
 * @since 0.1.0
 */
RA8_PRIV bool ra8_lvd_internal_reject_hvd_after(uint32_t hvd_val,
                                                uint32_t after_assert_val,
                                                uint32_t hysteresis,
                                                uint32_t negate);

/**
 * @brief Pure predicate for the CR0 "set RI bit" decision.
 *
 * @details
 * Returns true iff @p response equals the reset code OR equals the
 * reset-on-rise code.  Promoted from the inline compound OR at
 * libs/ra8_hal/src/ra8_lvd.c inside @c internal_compose_cr0.
 *
 * @param[in] reset_val          Numeric value of @c k_ra8_lvd_response_reset.
 * @param[in] reset_on_rise_val  Numeric value of @c k_ra8_lvd_response_reset_on_rise.
 * @param[in] response           Candidate response value.
 *
 * @return Boolean set-RI predicate.
 * @retval true  Caller must OR @c k_ra8_lvd_cr0_mask_ri into CR0.
 * @retval false RI bit stays clear.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the three inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * 2-condition OR; N+1 = 3 vectors:
 *  - resp=interrupt    -> false
 *  - resp=reset        -> true (varies left)
 *  - resp=reset_on_rise-> true (varies right)
 *
 * @since 0.1.0
 */
RA8_PRIV bool
ra8_lvd_internal_set_ri_bit(uint32_t reset_val, uint32_t reset_on_rise_val, uint32_t response);

#ifdef __cplusplus
}
#endif
