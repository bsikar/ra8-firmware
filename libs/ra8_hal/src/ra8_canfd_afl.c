/**
 * @file ra8_canfd_afl.c
 * @brief CANFD Acceptance-Filter-List (AFL) configuration (split from ra8_canfd.c)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Acceptance-filter half of the RA8D2 CANFD driver, split out of
 * ``ra8_canfd.c`` purely to satisfy the per-file size cap. This
 * translation unit owns the list-oriented AFL programming path used to
 * hardware-match received CAN / CAN-FD frames by ID and mask and route
 * them into a receive FIFO instead of accepting everything in software:
 *
 *  - Assemble each ``ra8_canfd_afl_rule_t`` into the paged GAFL register
 *    quartet (``CFDGAFLID`` / ``CFDGAFLM`` / ``CFDGAFLP0`` / ``CFDGAFLP1``).
 *  - Drive the documented edit transaction: select the AFL page and
 *    unlock the data window via ``CFDGAFLECTR`` (AFLPN + AFLDAE), set the
 *    page-0 rule count via ``CFDGAFLCFG0.RNC0``, write each rule, then
 *    re-lock by clearing ``CFDGAFLECTR``.
 *
 * The single-rule ``ra8_canfd_filter_set`` (which wraps the GL_RESET
 * transition itself) lives in ``ra8_canfd.c``; this file provides the
 * list-config primitive that runs inside a caller-managed GL_RESET window.
 *
 * Every register access carries a HUM Ch 41 "CAN with Flexible
 * Data-rate (CANFD)" citation (pages 2702..2867, chapter map row 41).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_canfd.h"
#include "ra8_canfd_regs.h"
#include "ra8_check.h"
#include "ra8_err.h"

static const char* s_tag = "CANFD";

/**
 * @enum ra8_canfd_afl_local_t
 * @brief Local constants owned by the AFL list-config path.
 *
 * @details
 * ``k_ra8_canfd_afl_page0`` is the AFLPN page index this API targets --
 * all rules land in the single 16-entry page 0, matching the
 * ``k_ra8_canfd_afl_rule_capacity`` cap in the public header. It is a
 * read-only compile-time constant so this translation unit keeps its own
 * named value rather than reaching for a bare literal in the CFDGAFLECTR
 * write. HUM Ch 41.2.17 "CFDGAFLECTR : AFL Entry Control Register"
 * p 2734 (AFLPN field).
 */
typedef enum : uint32_t {
  k_ra8_canfd_afl_page0 = 0U, /**< AFLPN page index used by this API. */
} ra8_canfd_afl_local_t;

/**
 * @brief Range-check one ``ra8_canfd_afl_rule_t`` against the protocol limits.
 *
 * @details
 * Splits the std-vs-ext and routing checks into nested guards (no
 * compound decisions) so each condition is independently reachable. A
 * standard rule must keep ``id`` and ``mask`` inside the 11-bit field; an
 * extended rule inside the 29-bit field. The routing target must name an
 * existing RX FIFO.
 *
 * @param[in] rule Non-NULL rule descriptor to validate.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Rule fields are all in range.
 * @retval k_ra8_err_invalid_arg id / mask overflow or target_rx out of range.
 *
 * @pre ``rule`` is non-NULL (the caller dereferenced it already).
 * @pre ``rule->target_rx`` is the caller-supplied routing index.
 * @post No register or global state is modified (pure predicate).
 * @post On k_ra8_ok every field fits its documented range.
 *
 * @note Internal helper, not exported. Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_afl_validate_rule(const ra8_canfd_afl_rule_t* rule)
{
  if (rule->target_rx >= (uint8_t)k_ra8_canfd_rx_fifo_count) {
    return k_ra8_err_invalid_arg;
  }
  if (rule->extended) {
    if ((rule->id & ~(uint32_t)k_ra8_canfd_id_ext_mask) != 0U) {
      return k_ra8_err_invalid_arg;
    }
    if ((rule->mask & ~(uint32_t)k_ra8_canfd_id_ext_mask) != 0U) {
      return k_ra8_err_invalid_arg;
    }
    return k_ra8_ok;
  }
  if ((rule->id & ~(uint32_t)k_ra8_canfd_id_std_mask) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  if ((rule->mask & ~(uint32_t)k_ra8_canfd_id_std_mask) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Assemble the CFDGAFLID word: accept ID plus IDE / RTR flags.
 *
 * @details
 * Masks the ID to the std / ext field and OR-includes GAFLIDE (bit 31)
 * for extended rules and GAFLRTR (bit 30) for remote-frame rules, matching
 * the shared message-ID layout used by the TX / RX paths.
 *
 * @param[in] rule Non-NULL validated rule descriptor.
 *
 * @return uint32_t CFDGAFLID register value.
 * @retval 0 Standard data frame with ID and flags all zero.
 *
 * @pre ``rule`` is non-NULL and already range-checked.
 * @pre ``rule->id`` fits the field selected by ``rule->extended``.
 * @post No register state is modified (pure assembly).
 * @post The returned IDE bit matches ``rule->extended``.
 *
 * @note Internal helper, not exported. Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_afl_id_word(const ra8_canfd_afl_rule_t* rule)
{
  const uint32_t masked = rule->extended ? (rule->id & (uint32_t)k_ra8_canfd_id_ext_mask)
                                         : (rule->id & (uint32_t)k_ra8_canfd_id_std_mask);
  uint32_t       word   = masked;
  if (rule->extended) {
    word |= (uint32_t)k_ra8_canfd_id_ide;
  }
  if (rule->rtr) {
    word |= (uint32_t)k_ra8_canfd_id_rtr;
  }
  return word;
}

/**
 * @brief Assemble the CFDGAFLM word: ID mask plus IDE / RTR compare-enables.
 *
 * @details
 * Masks the caller mask to the std / ext field and always OR-includes
 * GAFLIDEM (bit 31) and GAFLRTRM (bit 30) so the IDE and RTR flags are
 * part of the acceptance match -- otherwise ``rule->extended`` and
 * ``rule->rtr`` would be programmed into the ID word but never compared.
 *
 * @param[in] rule Non-NULL validated rule descriptor.
 *
 * @return uint32_t CFDGAFLM register value.
 * @retval 0 Never returned: the IDEM / RTRM compare bits are always set.
 *
 * @pre ``rule`` is non-NULL and already range-checked.
 * @pre ``rule->mask`` fits the field selected by ``rule->extended``.
 * @post No register state is modified (pure assembly).
 * @post The returned word always asserts GAFLIDEM and GAFLRTRM.
 *
 * @note Internal helper, not exported. Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_afl_mask_word(const ra8_canfd_afl_rule_t* rule)
{
  const uint32_t masked = rule->extended ? (rule->mask & (uint32_t)k_ra8_canfd_id_ext_mask)
                                         : (rule->mask & (uint32_t)k_ra8_canfd_id_std_mask);
  return masked | (uint32_t)k_ra8_gaflm_bit_gaflidem | (uint32_t)k_ra8_gaflm_bit_gaflrtrm;
}

/**
 * @brief Assemble the CFDGAFLP1 word: route a match into RX FIFO target_rx.
 *
 * @details
 * GAFLFDP0 (bit 0) routes a matched frame into RX FIFO 0; the per-FIFO
 * routing bits are contiguous, so shifting that base bit left by
 * ``rule->target_rx`` selects RX FIFO ``target_rx``. At least one routing
 * bit must be set or the match is dropped instead of stored.
 *
 * @param[in] rule Non-NULL validated rule descriptor.
 *
 * @return uint32_t CFDGAFLP1 register value (single routing bit set).
 * @retval 1 target_rx == 0: route into RX FIFO 0.
 *
 * @pre ``rule`` is non-NULL and already range-checked.
 * @pre ``rule->target_rx`` < ::k_ra8_canfd_rx_fifo_count.
 * @post No register state is modified (pure assembly).
 * @post Exactly one GAFLFDP routing bit is set.
 *
 * @note Internal helper, not exported. Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_afl_p1_word(const ra8_canfd_afl_rule_t* rule)
{
  return (uint32_t)k_ra8_gaflp1_bit_gaflfdp0 << rule->target_rx;
}

/**
 * @brief Write one CFDGAFL slot (ID / M / P0 / P1) from a rule.
 *
 * @details
 * Stamps the four paged AFL words for ``slot`` inside the unlocked page-0
 * window. P0 is left zero (no RX message-buffer routing -- the match goes
 * to an RX FIFO via P1).
 *
 * @param[in] reg  Non-NULL CANFD register block for the target channel.
 * @param[in] slot Slot index inside the unlocked page (0..15).
 * @param[in] rule Non-NULL validated rule descriptor.
 *
 * @pre ``reg`` is non-NULL and the AFL window is unlocked (AFLDAE = 1).
 * @pre The global block is in GL_RESET (CFDGAFL entries are RESET-only).
 * @post CFDGAFL[slot] reflects the rule's id / mask / routing.
 * @post No other AFL slot is modified.
 *
 * @note Internal helper, not exported. Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void
internal_afl_write_slot(volatile r_canfd_t* reg, uint8_t slot, const ra8_canfd_afl_rule_t* rule)
{
  /* HUM Ch 41.2.19 "CFDGAFLID : Acceptance Filter List ID Register" p 2735 */
  reg->CFDGAFL[slot].ID = internal_afl_id_word(rule);
  /* HUM Ch 41.2.20 "CFDGAFLM : Acceptance Filter List ID Mask Register" p 2736 */
  reg->CFDGAFL[slot].M = internal_afl_mask_word(rule);
  /* HUM Ch 41.2.21 "CFDGAFLP0 : Acceptance Filter List Pointer 0 Register" p 2738 */
  reg->CFDGAFL[slot].P0 = 0U;
  /* HUM Ch 41.2.22 "CFDGAFLP1 : Acceptance Filter List Pointer 1 Register" p 2740 */
  reg->CFDGAFL[slot].P1 = internal_afl_p1_word(rule);
}

/**
 * @brief Set CFDGAFLCFG0.RNC0 to @p count so the AFL consults @p count rules.
 *
 * @details
 * Read-modify-write of the RNC0 field (page-0 rule count) so the
 * surrounding CFDGAFLCFG0 bits are preserved. RNC0 is RESET-only, so the
 * caller must already hold the global block in GL_RESET.
 *
 * @param[in] reg   Non-NULL CANFD register block for the target channel.
 * @param[in] count Rule count, 1..::k_ra8_canfd_afl_rule_capacity.
 *
 * @pre ``reg`` is non-NULL and the global block is in GL_RESET.
 * @pre ``count`` <= ::k_ra8_canfd_afl_rule_capacity (fits the 5-bit field).
 * @post CFDGAFLCFG0.RNC0 == ``count``.
 * @post No CFDGAFLCFG0 bits outside RNC0 are modified.
 *
 * @note Internal helper, not exported. Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_afl_set_rule_count(volatile r_canfd_t* reg, uint8_t count)
{
  /* HUM Ch 41.2.18 "CFDGAFLCFG0 : Global AFL Configuration Register 0" p 2735 */
  const uint32_t cfg0 = reg->CFDGAFLCFG0;
  const uint32_t rnc  = ((uint32_t)count & k_ra8_gaflcfg0_mask_rnc0) << k_ra8_gaflcfg0_shift_rnc0;
  /* HUM Ch 41.2.18 "CFDGAFLCFG0 : Global AFL Configuration Register 0" p 2735 */
  reg->CFDGAFLCFG0 = (cfg0 & ~(k_ra8_gaflcfg0_mask_rnc0 << k_ra8_gaflcfg0_shift_rnc0)) | rnc;
}

/**
 * @brief Validate every rule in @p rules before any register is touched.
 *
 * @details
 * Bounded loop over the caller array, delegating per-rule range checks to
 * ::internal_afl_validate_rule. Validating up front keeps the register
 * write phase all-or-nothing -- a bad rule never lands a partial edit.
 *
 * @param[in] rules Non-NULL array of @p count rule descriptors.
 * @param[in] count Number of rules, already capacity-checked by the caller.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              All rules are in range.
 * @retval k_ra8_err_invalid_arg The first failing rule's error.
 *
 * @pre ``rules`` is non-NULL with at least ``count`` elements.
 * @pre ``count`` <= ::k_ra8_canfd_afl_rule_capacity (loop bound).
 * @post No register or global state is modified (pure predicate).
 * @post On k_ra8_ok every rule passed ::internal_afl_validate_rule.
 *
 * @note Internal helper, not exported. Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_afl_validate_all(const ra8_canfd_afl_rule_t* rules, uint8_t count)
{
  for (uint8_t i = 0U; i < count; i++) {
    const ra8_err_t v = internal_afl_validate_rule(&rules[i]);
    if (v != k_ra8_ok) {
      return v;
    }
  }
  return k_ra8_ok;
}

ra8_err_t
ra8_canfd_set_accept_filter(uint8_t channel, const ra8_canfd_afl_rule_t* rules, uint8_t count)
{
  RA8_CHECK_NULL_PTR(rules, s_tag, "rules must not be nullptr");
  volatile r_canfd_t* reg = ra8_canfd(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  if (count == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (count > (uint8_t)k_ra8_canfd_afl_rule_capacity) {
    return k_ra8_err_invalid_arg;
  }

  const ra8_err_t v = internal_afl_validate_all(rules, count);
  if (v != k_ra8_ok) {
    return v;
  }

  /* Unlock the AFL data window and select page 0 (AFLPN + AFLDAE). */
  /* HUM Ch 41.2.17 "CFDGAFLECTR : AFL Entry Control Register" p 2734 */
  reg->CFDGAFLECTR =
    ((uint32_t)k_ra8_canfd_afl_page0 & k_ra8_gaflectr_mask_aflpn) | k_ra8_gaflectr_bit_afldae;

  internal_afl_set_rule_count(reg, count);

  for (uint8_t i = 0U; i < count; i++) {
    internal_afl_write_slot(reg, i, &rules[i]);
  }

  /* Re-lock the AFL data window (clear AFLDAE). */
  /* HUM Ch 41.2.17 "CFDGAFLECTR : AFL Entry Control Register" p 2734 */
  reg->CFDGAFLECTR = 0U;
  return k_ra8_ok;
}
