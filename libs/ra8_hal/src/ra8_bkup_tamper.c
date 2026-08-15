/**
 * @file ra8_bkup_tamper.c
 * @brief Battery Backup Function (VBATT) driver -- tamper detection
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Tamper-detection half of the RA8D2 battery-backup driver: the RTCIC
 * pad wiring, the per-channel tamper configuration composers, and the
 * input monitor. Split out of ``ra8_bkup.c`` to stay under the per-file
 * line cap; the core lifecycle, backup store and interrupt path stay
 * there, and the TrustZone attribution registers live in
 * ``ra8_bkup_security.c``.
 *
 * Everything here concerns the VBTICTLR / VBTICTLR2 / VBTIMONR /
 * VBTADCR1-3 / VBTNCWCR register group. The channel-bit helper and the
 * per-channel validator are private to this TU because no other part of
 * the driver addresses tamper channels.
 *
 * @par PRCR write protection (issue #131)
 * The whole tamper register file sits behind **PRC1** (HUM Ch 13.1
 * Table 13.1 "Association between PRCR bits and use of registers to be
 * protected" p 520-521). A write issued while PRC1 is locked is
 * silently discarded -- no bus fault, no status flag -- so every write
 * path here runs inside an ``RA8_PROTECTED_WRITE`` window. Reads are
 * never protected. See the ``ra8_bkup.c`` file comment for the
 * bench evidence behind that rule.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_bkup.h"
#include "ra8_bkup_internal.h"
#include "ra8_bkup_regs.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_register_protection.h"

/**
 * @enum ra8_bkup_tamper_internal_t
 * @brief Local numeric constants -- avoid magic numbers per CLAUDE.md.
 */
typedef enum : uint16_t {
  k_ra8_bkup_max_nc_width = (uint16_t)k_ra8_bkup_nc_width_1hz, /**< Highest legal VINCW encoding. */
} ra8_bkup_tamper_internal_t;

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Validate a per-channel tamper descriptor.
 *
 * @param[in] ch Channel descriptor.
 * @return ``k_ra8_ok`` if ``edge`` and ``capture_src`` are valid.
 *
 * @pre ch != nullptr.
 * @pre ch->edge / ch->capture_src are enum values.
 * @post No side effects.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 *
 * @post Side effects bounded to documented state.
 */
RA8_INTERNAL static ra8_err_t internal_validate_chan(const ra8_bkup_tamper_chan_cfg_t* ch)
{
  if (((uint8_t)ch->edge != k_ra8_bkup_edge_falling) &&
      ((uint8_t)ch->edge != k_ra8_bkup_edge_rising)) {
    return k_ra8_err_invalid_arg;
  }
  if (((uint8_t)ch->capture_src != k_ra8_bkup_capture_src_pin) &&
      ((uint8_t)ch->capture_src != k_ra8_bkup_capture_src_vbtadf)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Convert channel + bit-base mask into the actual masked bit.
 *
 * @details
 * Most VBT* registers lay each channel's bit on the same bit position
 * (VBTADF0 == bit 0 ... VBTADF2 == bit 2 ; VCH0EG == bit 4 ...
 * VCH2EG == bit 6). Helper computes the per-channel mask from the
 * channel-0 mask plus the channel index.
 *
 * @param[in] base_mask Channel-0 mask (e.g. ``0x01`` or ``0x10``).
 * @param[in] channel   Channel index 0..2.
 * @return ``base_mask`` left-shifted by ``channel``.
 *
 * @pre channel < k_ra8_bkup_chan_count.
 * @pre base_mask != 0.
 * @post Returned mask covers exactly one bit.
 *
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 *
 * @post Side effects bounded to documented state.
 */
RA8_INTERNAL static inline uint8_t internal_chan_mask(uint8_t base_mask, ra8_bkup_channel_t channel)
{
  return (uint8_t)((uint32_t)base_mask << (uint32_t)channel);
}

/**
 * @brief Validate every channel descriptor in a tamper config.
 *
 * @param[in] cfg Caller-supplied, null-checked tamper config.
 * @return ``k_ra8_ok`` if every channel passes ``internal_validate_chan``.
 *
 * @pre cfg != nullptr.
 * @pre cfg->channels has ``k_ra8_bkup_chan_count`` entries.
 * @post No side effects.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 *
 * @post Side effects bounded to documented state.
 */
RA8_INTERNAL static ra8_err_t internal_validate_tamper_channels(const ra8_bkup_tamper_config_t* cfg)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_bkup_chan_count; ++i) {
    const ra8_err_t err = internal_validate_chan(&cfg->channels[i]);
    RA8_RETURN_ON_ERROR(err,
                        g_bkup_tag,
                        "tamper_init: channel cfg out of range"); /* GCOVR_EXCL_BR_LINE */
  }
  return k_ra8_ok;
}

/**
 * @brief Compose the VBTICTLR (VCHnINEN) byte from per-channel flags.
 *
 * @param[in] cfg Tamper config holding the per-channel ``input_enable`` flags.
 * @return Composed VBTICTLR value.
 *
 * @pre cfg != nullptr.
 * @pre cfg->channels has ``k_ra8_bkup_chan_count`` entries.
 * @post Returned mask only sets VCHnINEN bits.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 *
 * @post Side effects bounded to documented state.
 */
RA8_INTERNAL static uint8_t internal_compose_vbtictlr(const ra8_bkup_tamper_config_t* cfg)
{
  uint8_t ictlr = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_bkup_chan_count; ++i) {
    if (cfg->channels[i].input_enable) {
      ictlr = (uint8_t)(ictlr | internal_chan_mask(k_ra8_bkup_vbtictlr_mask_vch0inen,
                                                   (ra8_bkup_channel_t)i));
    }
  }
  return ictlr;
}

/**
 * @brief Compose the VBTICTLR2 (VCHnNCE + VCHnEG) byte from per-channel flags.
 *
 * @param[in] cfg Tamper config holding noise-canceller and edge fields.
 * @return Composed VBTICTLR2 value.
 *
 * @pre cfg != nullptr.
 * @pre cfg->channels has ``k_ra8_bkup_chan_count`` entries.
 * @post Returned mask only sets VCHnNCE/VCHnEG bits.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 *
 * @post Side effects bounded to documented state.
 */
RA8_INTERNAL static uint8_t internal_compose_vbtictlr2(const ra8_bkup_tamper_config_t* cfg)
{
  uint8_t ictlr2 = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_bkup_chan_count; ++i) {
    if (cfg->channels[i].noise_canceller_en) {
      ictlr2 = (uint8_t)(ictlr2 | internal_chan_mask(k_ra8_bkup_vbtictlr2_mask_vch0nce,
                                                     (ra8_bkup_channel_t)i));
    }
    if ((uint8_t)cfg->channels[i].edge == k_ra8_bkup_edge_rising) {
      ictlr2 = (uint8_t)(ictlr2 | internal_chan_mask(k_ra8_bkup_vbtictlr2_mask_vch0eg,
                                                     (ra8_bkup_channel_t)i));
    }
  }
  return ictlr2;
}

/**
 * @brief Compose the VBTADCR1 (IRQ-enable + clear-backup) byte.
 *
 * @param[in] cfg Tamper config holding ``irq_enable`` / ``clear_backup`` flags.
 * @return Composed VBTADCR1 value.
 *
 * @pre cfg != nullptr.
 * @pre cfg->channels has ``k_ra8_bkup_chan_count`` entries.
 * @post Returned mask only sets VBTADIE0/VBTADCE0 family bits.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 *
 * @post Side effects bounded to documented state.
 */
RA8_INTERNAL static uint8_t internal_compose_vbtadcr1(const ra8_bkup_tamper_config_t* cfg)
{
  uint8_t adcr1 = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_bkup_chan_count; ++i) {
    if (cfg->channels[i].irq_enable) {
      adcr1 = (uint8_t)(adcr1 | internal_chan_mask(k_ra8_bkup_vbtadcr1_mask_vbtadie0,
                                                   (ra8_bkup_channel_t)i));
    }
    if (cfg->channels[i].clear_backup) {
      adcr1 = (uint8_t)(adcr1 | internal_chan_mask(k_ra8_bkup_vbtadcr1_mask_vbtadce0,
                                                   (ra8_bkup_channel_t)i));
    }
  }
  return adcr1;
}

/**
 * @brief Compose the VBTADCR2 (capture-source) byte.
 *
 * @param[in] cfg Tamper config holding ``capture_src``.
 * @return Composed VBTADCR2 value.
 *
 * @pre cfg != nullptr.
 * @pre cfg->channels has ``k_ra8_bkup_chan_count`` entries.
 * @post Returned mask only sets VBRTCES0 family bits.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 *
 * @post Side effects bounded to documented state.
 */
RA8_INTERNAL static uint8_t internal_compose_vbtadcr2(const ra8_bkup_tamper_config_t* cfg)
{
  uint8_t adcr2 = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_bkup_chan_count; ++i) {
    if ((uint8_t)cfg->channels[i].capture_src == k_ra8_bkup_capture_src_vbtadf) {
      adcr2 = (uint8_t)(adcr2 | internal_chan_mask(k_ra8_bkup_vbtadcr2_mask_vbrtces0,
                                                   (ra8_bkup_channel_t)i));
    }
  }
  return adcr2;
}

/**
 * @brief Compose the VBTADCR3 (HUK-zeroize) byte.
 *
 * @param[in] cfg Tamper config holding ``zeroize_huk``.
 * @return Composed VBTADCR3 value.
 *
 * @pre cfg != nullptr.
 * @pre cfg->channels has ``k_ra8_bkup_chan_count`` entries.
 * @post Returned mask only sets VBTADZE0 family bits.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 *
 * @post Side effects bounded to documented state.
 */
RA8_INTERNAL static uint8_t internal_compose_vbtadcr3(const ra8_bkup_tamper_config_t* cfg)
{
  uint8_t adcr3 = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_bkup_chan_count; ++i) {
    if (cfg->channels[i].zeroize_huk) {
      adcr3 = (uint8_t)(adcr3 | internal_chan_mask(k_ra8_bkup_vbtadcr3_mask_vbtadze0,
                                                   (ra8_bkup_channel_t)i));
    }
  }
  return adcr3;
}

/* =============================================================================
 * Tamper-detection / RTCIC pad wiring
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_bkup_tamper_init(const ra8_bkup_tamper_config_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, g_bkup_tag, "tamper cfg must not be nullptr");
  if ((uint16_t)cfg->nc_width > k_ra8_bkup_max_nc_width) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t v_err = internal_validate_tamper_channels(cfg);
  RA8_RETURN_ON_ERROR(v_err,
                      g_bkup_tag,
                      "tamper_init: channel cfg out of range"); /* GCOVR_EXCL_BR_LINE */

  /* The whole tamper register file is PRC1 (HUM Ch 13.1 Table 13.1 p 521). */
  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_lpm)
  {
    /* HUM Ch 12.3.5 p 516 + Ch 12.3.7.4 step 0: disable VCHnNCE / VBTADCRn
     * before changing VINCW. */
    /* HUM Ch 12.2.9 "VBTICTLR2 : VBATT Input Control Register 2", p 506 */
    *ra8_bkup_vbtictlr2() = 0U;
    /* HUM Ch 12.2.15 "VBTADCR1 : VBATT Tamper Detection Control Register 1", p 510 */
    *ra8_bkup_vbtadcr1() = 0U;
    /* HUM Ch 12.2.16 "VBTADCR2 : VBATT Tamper Detection Control Register 2", p 511 */
    *ra8_bkup_vbtadcr2() = 0U;
    /* HUM Ch 12.2.17 "VBTADCR3 : VBATT Tamper Detection Control Register 3", p 511 */
    *ra8_bkup_vbtadcr3() = 0U;

    /* Tamper-init step 1 (12.3.7.4 p 518): VCHnINEN. */
    /* HUM Ch 12.2.8 "VBTICTLR : VBATT Input Control Register", p 505 */
    *ra8_bkup_vbtictlr() = internal_compose_vbtictlr(cfg);

    /* Tamper-init step 3 (12.3.7.4 p 518): VINCW. */
    /* HUM Ch 12.2.18 "VBTNCWCR : VBATT Noise Canceler Width Control Register", p 512 */
    *ra8_bkup_vbtncwcr() = (uint8_t)((uint8_t)cfg->nc_width & k_ra8_bkup_vbtncwcr_mask_vincw);

    /* Tamper-init step 4 (12.3.7.4 p 518): VCHnNCE + VCHnEG. */
    /* HUM Ch 12.2.9 "VBTICTLR2 : VBATT Input Control Register 2", p 506 */
    *ra8_bkup_vbtictlr2() = internal_compose_vbtictlr2(cfg);

    /* HUM Ch 12.3.7.4 step 7: dummy-read + W0C VBTADFn (pseudo-edge from
     * the edge-control register write may have set them). */
    /* HUM Ch 12.2.14 "VBTADSR : VBATT Tamper Detection Status Register", p 509 */
    (void)*ra8_bkup_vbtadsr();
    *ra8_bkup_vbtadsr() = 0U;

    /* Tamper-init step 8 (12.3.7.4 p 518): VBTADCR1/2/3. */
    /* HUM Ch 12.2.15 "VBTADCR1 : VBATT Tamper Detection Control Register 1", p 510 */
    *ra8_bkup_vbtadcr1() = internal_compose_vbtadcr1(cfg);
    /* HUM Ch 12.2.16 "VBTADCR2 : VBATT Tamper Detection Control Register 2", p 511 */
    *ra8_bkup_vbtadcr2() = internal_compose_vbtadcr2(cfg);
    /* HUM Ch 12.2.17 "VBTADCR3 : VBATT Tamper Detection Control Register 3", p 511 */
    *ra8_bkup_vbtadcr3() = internal_compose_vbtadcr3(cfg);
  }

  s_bkup_initialized = true;
  ra8_log_info(g_bkup_tag, "bkup_tamper_init");
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_bkup_tamper_disable(void)
{
  /* All six are PRC1-protected (HUM Ch 13.1 Table 13.1 p 521). */
  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_lpm)
  {
    /* HUM Ch 12.2.15 "VBTADCR1 : VBATT Tamper Detection Control Register 1", p 510 */
    *ra8_bkup_vbtadcr1() = 0U;
    /* HUM Ch 12.2.16 "VBTADCR2 : VBATT Tamper Detection Control Register 2", p 511 */
    *ra8_bkup_vbtadcr2() = 0U;
    /* HUM Ch 12.2.17 "VBTADCR3 : VBATT Tamper Detection Control Register 3", p 511 */
    *ra8_bkup_vbtadcr3() = 0U;
    /* HUM Ch 12.2.9 "VBTICTLR2 : VBATT Input Control Register 2", p 506 */
    *ra8_bkup_vbtictlr2() = 0U;
    /* HUM Ch 12.2.8 "VBTICTLR : VBATT Input Control Register", p 505 */
    *ra8_bkup_vbtictlr() = 0U;
    /* HUM Ch 12.2.14 "VBTADSR : VBATT Tamper Detection Status Register", p 509 */
    *ra8_bkup_vbtadsr() = 0U;
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_bkup_read_input(ra8_bkup_channel_t channel, bool* high_out)
{
  RA8_CHECK_NULL_PTR(high_out, g_bkup_tag, "high_out must not be nullptr");
  if ((uint8_t)channel >= (uint8_t)k_ra8_bkup_chan_count) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 12.2.10 "VBTIMONR : VBATT Input Monitor Register", p 506 */
  const uint8_t mask = internal_chan_mask(k_ra8_bkup_vbtimonr_mask_vch0mon, channel);
  *high_out          = ((*ra8_bkup_vbtimonr() & mask) != 0U);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_bkup_set_input_enable(ra8_bkup_channel_t channel, bool enable)
{
  if ((uint8_t)channel >= (uint8_t)k_ra8_bkup_chan_count) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 12.2.8 "VBTICTLR : VBATT Input Control Register", p 505 */
  const uint8_t mask = internal_chan_mask(k_ra8_bkup_vbtictlr_mask_vch0inen, channel);
  /* VBTICTLR is PRC1 (HUM Ch 13.1 Table 13.1 p 521). */
  priv_ra8_bkup_internal_rmw8(ra8_bkup_vbtictlr(), mask, enable, (uint16_t)k_ra8_prcr_unlock_lpm);
  return k_ra8_ok;
}
