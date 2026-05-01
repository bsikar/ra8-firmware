/**
 * @file ra_bkup.c
 * @brief Battery Backup Function (VBATT) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Driver for the RA8D2 battery-backup block (HUM Ch 12 p 498-519).
 * Mirrors the API laid out in ``ra_bkup.h``. Every register access
 * carries a HUM Ch 12 citation; the BAT* / VBT* registers physically
 * live inside the SYSC peripheral window so the offsets match
 * ``ra8d2_bkup_regs.h``.
 *
 * The block has **no MSTPCR bit** -- it is permanently powered as
 * long as VBATT or VCC is supplied (HUM Ch 12.1 p 498 -- "the
 * battery-powered area is powered by the main power supply"). So we
 * skip the usual ``ra_mstp_enable`` step that other drivers run in
 * their ``init``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_bkup.h"

#include <stdint.h>

#include "ra8d2_bkup_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

/**
 * @var s_tag
 * @brief Log tag used for this driver's diagnostics.
 */
static const char* s_tag = "BKUP";

/**
 * @var s_bkup_fn
 * @brief Currently attached low-battery / tamper callback or ``nullptr``.
 *
 * @note Direct pointer assignment; not atomic. Only mutate from
 *       single-threaded init or with IRQs masked.
 */
static ra_bkup_event_fn_t s_bkup_fn;

/**
 * @var s_bkup_ctx
 * @brief Opaque pointer forwarded to the ``s_bkup_fn`` callback.
 */
static void* s_bkup_ctx;

/**
 * @var s_initialized
 * @brief ``true`` once any of the lifecycle init helpers have run.
 *
 * @note Strictly used to gate ``ra_bkup_isr_handle`` with the
 *       ``k_ra_err_not_initialized`` return -- everything else is
 *       state-less so the flag is set generously.
 */
static bool s_initialized;

/**
 * @enum ra_bkup_internal_t
 * @brief Local numeric constants -- avoid magic numbers per CLAUDE.md.
 */
typedef enum : uint16_t {
  k_ra_bkup_max_vdet_level = (uint16_t)k_ra_bkup_vdet_1p75v, /**< Highest legal VDETLVL encoding. */
  k_ra_bkup_max_nc_width   = (uint16_t)k_ra_bkup_nc_width_1hz, /**< Highest legal VINCW encoding. */
  k_ra_bkup_no_switch_lvl_raw =
    0x06U, /**< 110b sentinel for VDETLVL "initial value" (HUM 12.3.7.3). */
  k_ra_bkup_status_clear_keep_mask =
    (uint16_t)((uint8_t)~k_ra_bkup_vbtbpsr_mask_vbporf), /**< W0C base for VBPORF. */
} ra_bkup_internal_t;

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Validate a ``ra_bkup_config_t`` against HUM constraints.
 *
 * @param[in] cfg Already null-checked configuration descriptor.
 *
 * @return ``k_ra_ok`` if every field is in range, else
 *         ``k_ra_err_invalid_arg``.
 *
 * @pre cfg != nullptr.
 * @pre cfg->vdet_level represents a valid VBTBPCR2.VDETLVL encoding.
 * @post No side effects.
 */
static ra_err_t internal_validate_cfg(const ra_bkup_config_t* cfg)
{
  if ((uint16_t)cfg->vdet_level > k_ra_bkup_max_vdet_level) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/**
 * @brief Validate a per-channel tamper descriptor.
 *
 * @param[in] ch Channel descriptor.
 * @return ``k_ra_ok`` if ``edge`` and ``capture_src`` are valid.
 *
 * @pre ch != nullptr.
 * @pre ch->edge / ch->capture_src are enum values.
 * @post No side effects.
 */
static ra_err_t internal_validate_chan(const ra_bkup_tamper_chan_cfg_t* ch)
{
  if (((uint8_t)ch->edge != k_ra_bkup_edge_falling) &&
      ((uint8_t)ch->edge != k_ra_bkup_edge_rising)) {
    return k_ra_err_invalid_arg;
  }
  if (((uint8_t)ch->capture_src != k_ra_bkup_capture_src_pin) &&
      ((uint8_t)ch->capture_src != k_ra_bkup_capture_src_vbtadf)) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
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
 * @pre channel < k_ra_bkup_chan_count.
 * @pre base_mask != 0.
 * @post Returned mask covers exactly one bit.
 */
static inline uint8_t internal_chan_mask(uint8_t base_mask, ra_bkup_channel_t channel)
{
  return (uint8_t)((uint32_t)base_mask << (uint32_t)channel);
}

/**
 * @brief Set or clear a bit in an 8-bit RMW register.
 *
 * @param[in,out] reg     Pointer to the live 8-bit register.
 * @param[in]     mask    Bit mask to manipulate.
 * @param[in]     enable  ``true`` to set, ``false`` to clear.
 *
 * @pre reg != nullptr.
 * @pre mask != 0.
 * @post Bits in ``mask`` reflect ``enable``; other bits unchanged.
 */
static inline void internal_rmw8(volatile uint8_t* reg, uint8_t mask, bool enable)
{
  const uint8_t live = *reg;
  if (enable) {
    *reg = (uint8_t)(live | mask);
  } else {
    *reg = (uint8_t)(live & (uint8_t)~mask);
  }
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_bkup_init(const ra_bkup_config_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  const ra_err_t v_err = internal_validate_cfg(cfg);
  RA_RETURN_ON_ERROR(v_err, s_tag, "bkup_init: cfg out of range");

  if (cfg->enable_switch) {
    /* HUM Ch 12.2.11 "VBTBPCR1 : VBATT Battery Power Supply Control Register 1", p 507 */
    *ra_bkup_vbtbpcr1() = 0U;

    /* HUM Ch 12.2.12 "VBTBPCR2 : VBATT Battery Power Supply Control Register 2", p 508 */
    /* Program VDETLVL with VDETE = 0 first per the HUM stabilisation note. */
    *ra_bkup_vbtbpcr2() = (uint8_t)((uint8_t)cfg->vdet_level & k_ra_bkup_vbtbpcr2_mask_lvl);

    /* HUM Ch 12.2.12 "VBTBPCR2 : VBATT Battery Power Supply Control Register 2", p 508 */
    /* Now set VDETE so VCC drop detection arms. */
    *ra_bkup_vbtbpcr2() = (uint8_t)(((uint8_t)cfg->vdet_level & k_ra_bkup_vbtbpcr2_mask_lvl) |
                                    k_ra_bkup_vbtbpcr2_mask_vdete);
  } else {
    /* HUM Ch 12.2.11 "VBTBPCR1 : VBATT Battery Power Supply Control Register 1", p 507 */
    *ra_bkup_vbtbpcr1() = k_ra_bkup_vbtbpcr1_mask_bpwswstp;
  }

  /* HUM Ch 12.2.6 "VBTBER : VBATT Backup Enable Register", p 504 */
  if (cfg->enable_backup) {
    *ra_bkup_vbtber() = k_ra_bkup_vbtber_mask_vbae;
  } else {
    *ra_bkup_vbtber() = 0U;
  }

  /* HUM Ch 12.2.13 "VBTBPSR : VBATT Battery Power Supply Status Register", p 509 */
  *ra_bkup_vbtbpsr() =
    (uint8_t)k_ra_bkup_status_clear_keep_mask; /* W0C: clear VBPORF, leave others. */

  /* HUM Ch 12.2.14 "VBTADSR : VBATT Tamper Detection Status Register", p 509 */
  *ra_bkup_vbtadsr() = 0U;

  s_initialized = true;
  ra_log_info(s_tag, "bkup_init");
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_bkup_deinit(void)
{
  /* HUM Ch 12.2.6 "VBTBER : VBATT Backup Enable Register", p 504 */
  /* HUM warns: VBAE must be 0 before VBATT cutover or VBTBKRn is lost. */
  *ra_bkup_vbtber() = 0U;

  /* HUM Ch 12.2.11 "VBTBPCR1 : VBATT Battery Power Supply Control Register 1", p 507 */
  *ra_bkup_vbtbpcr1() = k_ra_bkup_vbtbpcr1_mask_bpwswstp;

  s_initialized = false;
  ra_log_info(s_tag, "bkup_deinit");
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_bkup_cold_start_init(ra_bkup_vdet_level_t level, uint32_t timeout_iters)
{
  if ((uint16_t)level > k_ra_bkup_max_vdet_level) {
    return k_ra_err_invalid_arg;
  }
  if (timeout_iters == 0U) {
    return k_ra_err_invalid_arg;
  }

  /* Cold-start step 1 (12.3.7.1 p 517): wait for VBPORM == 1. */
  /* HUM Ch 12.2.13 "VBTBPSR : VBATT Battery Power Supply Status Register", p 509 */
  bool vbporm_ok = false;
  for (uint32_t i = 0U; i < timeout_iters; ++i) {
    if ((*ra_bkup_vbtbpsr() & k_ra_bkup_vbtbpsr_mask_vbporm) != 0U) {
      vbporm_ok = true;
      break;
    }
  }
  if (!vbporm_ok) {
    return k_ra_err_hw_timeout;
  }

  /* Cold-start step 2 (12.3.7.1 p 517): clear VBPORF. */
  /* HUM Ch 12.2.13 "VBTBPSR : VBATT Battery Power Supply Status Register", p 509 */
  *ra_bkup_vbtbpsr() = (uint8_t)k_ra_bkup_status_clear_keep_mask;

  /* Cold-start step 3 (12.3.7.1 p 517): programme VDETLVL with VDETE still 0. */
  /* HUM Ch 12.2.12 "VBTBPCR2 : VBATT Battery Power Supply Control Register 2", p 508 */
  *ra_bkup_vbtbpcr2() = (uint8_t)((uint8_t)level & k_ra_bkup_vbtbpcr2_mask_lvl);

  /* Cold-start step 4 (12.3.7.1 p 517): external tDETWT wait -- caller's job. */

  /* Cold-start step 5 (12.3.7.1 p 517): arm VDETE. */
  /* HUM Ch 12.2.12 "VBTBPCR2 : VBATT Battery Power Supply Control Register 2", p 508 */
  *ra_bkup_vbtbpcr2() =
    (uint8_t)(((uint8_t)level & k_ra_bkup_vbtbpcr2_mask_lvl) | k_ra_bkup_vbtbpcr2_mask_vdete);

  /* HUM Ch 12.2.11 "VBTBPCR1 : VBATT Battery Power Supply Control Register 1", p 507 */
  *ra_bkup_vbtbpcr1() = 0U; /* Switch enabled. */

  s_initialized = true;
  ra_log_info(s_tag, "bkup_cold_start_init");
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_bkup_warm_start_check(bool* needs_reinit, uint32_t timeout_iters)
{
  RA_CHECK_NULL_PTR(needs_reinit, s_tag, "needs_reinit must not be nullptr");
  if (timeout_iters == 0U) {
    return k_ra_err_invalid_arg;
  }

  /* Warm-start step 1 (12.3.7.2 p 517): wait for VBPORM == 1. */
  /* HUM Ch 12.2.13 "VBTBPSR : VBATT Battery Power Supply Status Register", p 509 */
  bool vbporm_ok = false;
  for (uint32_t i = 0U; i < timeout_iters; ++i) {
    if ((*ra_bkup_vbtbpsr() & k_ra_bkup_vbtbpsr_mask_vbporm) != 0U) {
      vbporm_ok = true;
      break;
    }
  }
  if (!vbporm_ok) {
    return k_ra_err_hw_timeout;
  }

  /* Warm-start step 2 (12.3.7.2 p 517): read VBPORF. */
  /* HUM Ch 12.2.13 "VBTBPSR : VBATT Battery Power Supply Status Register", p 509 */
  *needs_reinit = ((*ra_bkup_vbtbpsr() & k_ra_bkup_vbtbpsr_mask_vbporf) != 0U);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_bkup_no_switch_init(uint32_t timeout_iters)
{
  if (timeout_iters == 0U) {
    return k_ra_err_invalid_arg;
  }

  /* No-switch step 1 (12.3.7.3 p 518): stop the switch. */
  /* HUM Ch 12.2.11 "VBTBPCR1 : VBATT Battery Power Supply Control Register 1", p 507 */
  *ra_bkup_vbtbpcr1() = k_ra_bkup_vbtbpcr1_mask_bpwswstp;

  /* No-switch step 2 (12.3.7.3 p 518): wait for VBPORM == 0 (rail tied to VCC). */
  /* HUM Ch 12.2.13 "VBTBPSR : VBATT Battery Power Supply Status Register", p 509 */
  bool vbporm_low = false;
  for (uint32_t i = 0U; i < timeout_iters; ++i) {
    if ((*ra_bkup_vbtbpsr() & k_ra_bkup_vbtbpsr_mask_vbporm) == 0U) {
      vbporm_low = true;
      break;
    }
  }
  if (!vbporm_low) {
    return k_ra_err_hw_timeout;
  }

  /* No-switch steps 3-4 (12.3.7.3 p 518): VDETE = 0, VDETLVL = 110b. */
  /* HUM Ch 12.2.12 "VBTBPCR2 : VBATT Battery Power Supply Control Register 2", p 508 */
  *ra_bkup_vbtbpcr2() = (uint8_t)k_ra_bkup_no_switch_lvl_raw;

  /* No-switch step 5 (12.3.7.3 p 518): W0C VBPORF. */
  /* HUM Ch 12.2.13 "VBTBPSR : VBATT Battery Power Supply Status Register", p 509 */
  *ra_bkup_vbtbpsr() = (uint8_t)k_ra_bkup_status_clear_keep_mask;

  /* No-switch steps 7-8 (12.3.7.3 p 518): zero pad / tamper / backup-clear paths. */
  /* HUM Ch 12.2.8 "VBTICTLR : VBATT Input Control Register", p 505 */
  *ra_bkup_vbtictlr() = 0U;
  /* HUM Ch 12.2.9 "VBTICTLR2 : VBATT Input Control Register 2", p 506 */
  *ra_bkup_vbtictlr2() = 0U;
  /* HUM Ch 12.2.14 "VBTADSR : VBATT Tamper Detection Status Register", p 509 */
  *ra_bkup_vbtadsr() = 0U;
  /* HUM Ch 12.2.15 "VBTADCR1 : VBATT Tamper Detection Control Register 1", p 510 */
  *ra_bkup_vbtadcr1() = 0U;
  /* HUM Ch 12.2.16 "VBTADCR2 : VBATT Tamper Detection Control Register 2", p 511 */
  *ra_bkup_vbtadcr2() = 0U;

  s_initialized = true;
  ra_log_info(s_tag, "bkup_no_switch_init");
  return k_ra_ok;
}

/* =============================================================================
 * Status
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_bkup_get_status(ra_bkup_status_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");

  /* HUM Ch 12.2.13 "VBTBPSR : VBATT Battery Power Supply Status Register", p 509 */
  const uint8_t bpsr = *ra_bkup_vbtbpsr();
  /* HUM Ch 12.2.14 "VBTADSR : VBATT Tamper Detection Status Register", p 509 */
  const uint8_t adsr = *ra_bkup_vbtadsr();

  out->raw_vbtbpsr = bpsr;
  if ((bpsr & k_ra_bkup_vbtbpsr_mask_swm) != 0U) {
    out->source = k_ra_bkup_source_vcc;
  } else {
    out->source = k_ra_bkup_source_vbatt;
  }
  out->vbatt_r_ok   = ((bpsr & k_ra_bkup_vbtbpsr_mask_vbporm) != 0U);
  out->por_detected = ((bpsr & k_ra_bkup_vbtbpsr_mask_vbporf) != 0U);
  out->tamper_flags = (uint8_t)(adsr & k_ra_bkup_vbtadsr_mask_all);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_bkup_clear_status(uint8_t mask)
{
  if ((mask & k_ra_bkup_vbtbpsr_mask_vbporf) != 0U) {
    /* HUM Ch 12.2.13 "VBTBPSR : VBATT Battery Power Supply Status Register", p 509 */
    /* W0C: write 0 to bits we want cleared, 1 to bits to leave alone. */
    const uint8_t live = *ra_bkup_vbtbpsr();
    *ra_bkup_vbtbpsr() = (uint8_t)(live & (uint8_t)~k_ra_bkup_vbtbpsr_mask_vbporf);
  }
  const uint8_t adf_bits = (uint8_t)(mask & k_ra_bkup_vbtadsr_mask_all);
  if (adf_bits != 0U) {
    /* HUM Ch 12.2.14 "VBTADSR : VBATT Tamper Detection Status Register", p 509 */
    const uint8_t live = *ra_bkup_vbtadsr();
    *ra_bkup_vbtadsr() = (uint8_t)(live & (uint8_t)~adf_bits);
  }
  return k_ra_ok;
}

/* =============================================================================
 * VBTBKRn access
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_bkup_read_word(uint8_t word_index, uint32_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if ((uint16_t)word_index >= k_ra_bkup_word_count) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 12.2.7 "VBTBKRn : VBATT Backup Register", p 505 */
  volatile uint32_t* slot = ra_bkup_vbtbkr_word(word_index);
  RA_CHECK_NULL_PTR(slot, s_tag, "vbtbkr word slot mapping failed");
  *out = *slot;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_bkup_write_word(uint8_t word_index, uint32_t value)
{
  if ((uint16_t)word_index >= k_ra_bkup_word_count) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 12.2.7 "VBTBKRn : VBATT Backup Register", p 505 */
  volatile uint32_t* slot = ra_bkup_vbtbkr_word(word_index);
  RA_CHECK_NULL_PTR(slot, s_tag, "vbtbkr word slot mapping failed");
  *slot = value;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_bkup_read_byte(uint16_t index, uint8_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (index >= k_ra_bkup_reg_count) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 12.2.7 "VBTBKRn : VBATT Backup Register", p 505 */
  volatile uint8_t* slot = ra_bkup_vbtbkr(index);
  RA_CHECK_NULL_PTR(slot, s_tag, "vbtbkr byte slot mapping failed");
  *out = *slot;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_bkup_write_byte(uint16_t index, uint8_t value)
{
  if (index >= k_ra_bkup_reg_count) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 12.2.7 "VBTBKRn : VBATT Backup Register", p 505 */
  volatile uint8_t* slot = ra_bkup_vbtbkr(index);
  RA_CHECK_NULL_PTR(slot, s_tag, "vbtbkr byte slot mapping failed");
  *slot = value;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_bkup_zero_all(void)
{
  for (uint16_t i = 0U; i < k_ra_bkup_reg_count; ++i) {
    /* HUM Ch 12.2.7 "VBTBKRn : VBATT Backup Register", p 505 */
    volatile uint8_t* slot = ra_bkup_vbtbkr(i);
    if (slot != nullptr) {
      *slot = 0U;
    }
  }
  return k_ra_ok;
}

/* =============================================================================
 * Tamper-detection / RTCIC pad wiring
 * =============================================================================
 */

/**
 * @brief Validate every channel descriptor in a tamper config.
 *
 * @param[in] cfg Caller-supplied, null-checked tamper config.
 * @return ``k_ra_ok`` if every channel passes ``internal_validate_chan``.
 *
 * @pre cfg != nullptr.
 * @pre cfg->channels has ``k_ra_bkup_chan_count`` entries.
 * @post No side effects.
 */
static ra_err_t internal_validate_tamper_channels(const ra_bkup_tamper_config_t* cfg)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra_bkup_chan_count; ++i) {
    const ra_err_t err = internal_validate_chan(&cfg->channels[i]);
    RA_RETURN_ON_ERROR(err, s_tag, "tamper_init: channel cfg out of range");
  }
  return k_ra_ok;
}

/**
 * @brief Compose the VBTICTLR (VCHnINEN) byte from per-channel flags.
 *
 * @param[in] cfg Tamper config holding the per-channel ``input_enable`` flags.
 * @return Composed VBTICTLR value.
 *
 * @pre cfg != nullptr.
 * @pre cfg->channels has ``k_ra_bkup_chan_count`` entries.
 * @post Returned mask only sets VCHnINEN bits.
 */
static uint8_t internal_compose_vbtictlr(const ra_bkup_tamper_config_t* cfg)
{
  uint8_t ictlr = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra_bkup_chan_count; ++i) {
    if (cfg->channels[i].input_enable) {
      ictlr = (uint8_t)(ictlr |
                        internal_chan_mask(k_ra_bkup_vbtictlr_mask_vch0inen, (ra_bkup_channel_t)i));
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
 * @pre cfg->channels has ``k_ra_bkup_chan_count`` entries.
 * @post Returned mask only sets VCHnNCE/VCHnEG bits.
 */
static uint8_t internal_compose_vbtictlr2(const ra_bkup_tamper_config_t* cfg)
{
  uint8_t ictlr2 = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra_bkup_chan_count; ++i) {
    if (cfg->channels[i].noise_canceller_en) {
      ictlr2 = (uint8_t)(ictlr2 | internal_chan_mask(k_ra_bkup_vbtictlr2_mask_vch0nce,
                                                     (ra_bkup_channel_t)i));
    }
    if ((uint8_t)cfg->channels[i].edge == k_ra_bkup_edge_rising) {
      ictlr2 = (uint8_t)(ictlr2 |
                         internal_chan_mask(k_ra_bkup_vbtictlr2_mask_vch0eg, (ra_bkup_channel_t)i));
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
 * @pre cfg->channels has ``k_ra_bkup_chan_count`` entries.
 * @post Returned mask only sets VBTADIE0/VBTADCE0 family bits.
 */
static uint8_t internal_compose_vbtadcr1(const ra_bkup_tamper_config_t* cfg)
{
  uint8_t adcr1 = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra_bkup_chan_count; ++i) {
    if (cfg->channels[i].irq_enable) {
      adcr1 = (uint8_t)(adcr1 |
                        internal_chan_mask(k_ra_bkup_vbtadcr1_mask_vbtadie0, (ra_bkup_channel_t)i));
    }
    if (cfg->channels[i].clear_backup) {
      adcr1 = (uint8_t)(adcr1 |
                        internal_chan_mask(k_ra_bkup_vbtadcr1_mask_vbtadce0, (ra_bkup_channel_t)i));
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
 * @pre cfg->channels has ``k_ra_bkup_chan_count`` entries.
 * @post Returned mask only sets VBRTCES0 family bits.
 */
static uint8_t internal_compose_vbtadcr2(const ra_bkup_tamper_config_t* cfg)
{
  uint8_t adcr2 = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra_bkup_chan_count; ++i) {
    if ((uint8_t)cfg->channels[i].capture_src == k_ra_bkup_capture_src_vbtadf) {
      adcr2 = (uint8_t)(adcr2 |
                        internal_chan_mask(k_ra_bkup_vbtadcr2_mask_vbrtces0, (ra_bkup_channel_t)i));
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
 * @pre cfg->channels has ``k_ra_bkup_chan_count`` entries.
 * @post Returned mask only sets VBTADZE0 family bits.
 */
static uint8_t internal_compose_vbtadcr3(const ra_bkup_tamper_config_t* cfg)
{
  uint8_t adcr3 = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra_bkup_chan_count; ++i) {
    if (cfg->channels[i].zeroize_huk) {
      adcr3 = (uint8_t)(adcr3 |
                        internal_chan_mask(k_ra_bkup_vbtadcr3_mask_vbtadze0, (ra_bkup_channel_t)i));
    }
  }
  return adcr3;
}

[[nodiscard]] ra_err_t ra_bkup_tamper_init(const ra_bkup_tamper_config_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "tamper cfg must not be nullptr");
  if ((uint16_t)cfg->nc_width > k_ra_bkup_max_nc_width) {
    return k_ra_err_invalid_arg;
  }
  const ra_err_t v_err = internal_validate_tamper_channels(cfg);
  RA_RETURN_ON_ERROR(v_err, s_tag, "tamper_init: channel cfg out of range");

  /* HUM Ch 12.3.5 p 516 + Ch 12.3.7.4 step 0: disable VCHnNCE / VBTADCRn
   * before changing VINCW. */
  /* HUM Ch 12.2.9 "VBTICTLR2 : VBATT Input Control Register 2", p 506 */
  *ra_bkup_vbtictlr2() = 0U;
  /* HUM Ch 12.2.15 "VBTADCR1 : VBATT Tamper Detection Control Register 1", p 510 */
  *ra_bkup_vbtadcr1() = 0U;
  /* HUM Ch 12.2.16 "VBTADCR2 : VBATT Tamper Detection Control Register 2", p 511 */
  *ra_bkup_vbtadcr2() = 0U;
  /* HUM Ch 12.2.17 "VBTADCR3 : VBATT Tamper Detection Control Register 3", p 511 */
  *ra_bkup_vbtadcr3() = 0U;

  /* Tamper-init step 1 (12.3.7.4 p 518): VCHnINEN. */
  /* HUM Ch 12.2.8 "VBTICTLR : VBATT Input Control Register", p 505 */
  *ra_bkup_vbtictlr() = internal_compose_vbtictlr(cfg);

  /* Tamper-init step 3 (12.3.7.4 p 518): VINCW. */
  /* HUM Ch 12.2.18 "VBTNCWCR : VBATT Noise Canceler Width Control Register", p 511 */
  *ra_bkup_vbtncwcr() = (uint8_t)((uint8_t)cfg->nc_width & k_ra_bkup_vbtncwcr_mask_vincw);

  /* Tamper-init step 4 (12.3.7.4 p 518): VCHnNCE + VCHnEG. */
  /* HUM Ch 12.2.9 "VBTICTLR2 : VBATT Input Control Register 2", p 506 */
  *ra_bkup_vbtictlr2() = internal_compose_vbtictlr2(cfg);

  /* HUM Ch 12.3.7.4 step 7: dummy-read + W0C VBTADFn (pseudo-edge from
   * the edge-control register write may have set them). */
  /* HUM Ch 12.2.14 "VBTADSR : VBATT Tamper Detection Status Register", p 509 */
  (void)*ra_bkup_vbtadsr();
  *ra_bkup_vbtadsr() = 0U;

  /* Tamper-init step 8 (12.3.7.4 p 518): VBTADCR1/2/3. */
  /* HUM Ch 12.2.15 "VBTADCR1 : VBATT Tamper Detection Control Register 1", p 510 */
  *ra_bkup_vbtadcr1() = internal_compose_vbtadcr1(cfg);
  /* HUM Ch 12.2.16 "VBTADCR2 : VBATT Tamper Detection Control Register 2", p 511 */
  *ra_bkup_vbtadcr2() = internal_compose_vbtadcr2(cfg);
  /* HUM Ch 12.2.17 "VBTADCR3 : VBATT Tamper Detection Control Register 3", p 511 */
  *ra_bkup_vbtadcr3() = internal_compose_vbtadcr3(cfg);

  s_initialized = true;
  ra_log_info(s_tag, "bkup_tamper_init");
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_bkup_tamper_disable(void)
{
  /* HUM Ch 12.2.15 "VBTADCR1 : VBATT Tamper Detection Control Register 1", p 510 */
  *ra_bkup_vbtadcr1() = 0U;
  /* HUM Ch 12.2.16 "VBTADCR2 : VBATT Tamper Detection Control Register 2", p 511 */
  *ra_bkup_vbtadcr2() = 0U;
  /* HUM Ch 12.2.17 "VBTADCR3 : VBATT Tamper Detection Control Register 3", p 511 */
  *ra_bkup_vbtadcr3() = 0U;
  /* HUM Ch 12.2.9 "VBTICTLR2 : VBATT Input Control Register 2", p 506 */
  *ra_bkup_vbtictlr2() = 0U;
  /* HUM Ch 12.2.8 "VBTICTLR : VBATT Input Control Register", p 505 */
  *ra_bkup_vbtictlr() = 0U;
  /* HUM Ch 12.2.14 "VBTADSR : VBATT Tamper Detection Status Register", p 509 */
  *ra_bkup_vbtadsr() = 0U;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_bkup_read_input(ra_bkup_channel_t channel, bool* high_out)
{
  RA_CHECK_NULL_PTR(high_out, s_tag, "high_out must not be nullptr");
  if ((uint8_t)channel >= (uint8_t)k_ra_bkup_chan_count) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 12.2.10 "VBTIMONR : VBATT Input Monitor Register", p 506 */
  const uint8_t mask = internal_chan_mask(k_ra_bkup_vbtimonr_mask_vch0mon, channel);
  *high_out          = ((*ra_bkup_vbtimonr() & mask) != 0U);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_bkup_set_input_enable(ra_bkup_channel_t channel, bool enable)
{
  if ((uint8_t)channel >= (uint8_t)k_ra_bkup_chan_count) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 12.2.8 "VBTICTLR : VBATT Input Control Register", p 505 */
  const uint8_t mask = internal_chan_mask(k_ra_bkup_vbtictlr_mask_vch0inen, channel);
  internal_rmw8(ra_bkup_vbtictlr(), mask, enable);
  return k_ra_ok;
}

/* =============================================================================
 * VBATT analog voltage monitor
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_bkup_set_voltage_monitor(bool enable)
{
  /* HUM Ch 12.2.5 "VBATTMNSELR : Battery Backup Voltage Monitor Function Select Register", p 503 */
  internal_rmw8(ra_bkup_vbattmnselr(), k_ra_bkup_vbattmnselr_mask_vbtmnsel, enable);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_bkup_get_voltage_monitor_enabled(bool* enabled_out)
{
  RA_CHECK_NULL_PTR(enabled_out, s_tag, "enabled_out must not be nullptr");
  /* HUM Ch 12.2.5 "VBATTMNSELR : Battery Backup Voltage Monitor Function Select Register", p 503 */
  *enabled_out = ((*ra_bkup_vbattmnselr() & k_ra_bkup_vbattmnselr_mask_vbtmnsel) != 0U);
  return k_ra_ok;
}

/* =============================================================================
 * TrustZone partitioning
 * =============================================================================
 */

/**
 * @brief Validate one boundary-address register value.
 *
 * @param[in] addr Candidate value (lower-16 of the boundary).
 * @return ``k_ra_ok`` if 32-byte aligned and below saba_max.
 *
 * @pre None.
 * @post No side effects.
 */
static ra_err_t internal_validate_boundary(uint16_t addr)
{
  if ((addr & k_ra_bkup_saba_align_mask) != 0U) {
    return k_ra_err_invalid_arg;
  }
  if (addr > k_ra_bkup_saba_max) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/**
 * @brief Validate every field of an ``ra_bkup_security_config_t``.
 *
 * @param[in] cfg Caller-supplied, null-checked security config.
 * @return ``k_ra_ok`` if every field is in range.
 *
 * @pre cfg != nullptr.
 * @pre cfg->bbfsar / cfg->saba / cfg->pabas / cfg->pabans are populated.
 * @post No side effects.
 */
static ra_err_t internal_validate_security_cfg(const ra_bkup_security_config_t* cfg)
{
  if ((cfg->bbfsar & ~k_ra_bkup_bbfsar_mask_all) != 0U) {
    return k_ra_err_invalid_arg;
  }
  ra_err_t err = internal_validate_boundary(cfg->saba);
  RA_RETURN_ON_ERROR(err, s_tag, "security_apply: saba bad");
  err = internal_validate_boundary(cfg->pabas);
  RA_RETURN_ON_ERROR(err, s_tag, "security_apply: pabas bad");
  err = internal_validate_boundary(cfg->pabans);
  RA_RETURN_ON_ERROR(err, s_tag, "security_apply: pabans bad");
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_bkup_security_apply(const ra_bkup_security_config_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "security cfg must not be nullptr");
  const ra_err_t v_err = internal_validate_security_cfg(cfg);
  RA_RETURN_ON_ERROR(v_err, s_tag, "security_apply: cfg bad");

  /* HUM Ch 12.2.1 "BBFSAR : Battery Backup Function Security Attribute Register", p 500 */
  *ra_bkup_bbfsar() = (uint32_t)(cfg->bbfsar & k_ra_bkup_bbfsar_mask_all);
  /* HUM Ch 12.2.2 "VBRSABAR : VBATT Backup Register Security Attribute Boundary Address Register", p 502 */
  *ra_bkup_vbrsabar() = cfg->saba;
  /* HUM Ch 12.2.3 "VBRPABARS : VBATT Backup Register Privilege Attribute Boundary Address Register for Secure Region", p 502 */
  *ra_bkup_vbrpabars() = cfg->pabas;
  /* HUM Ch 12.2.4 "VBRPABARNS : VBATT Backup Register Privilege Attribute Boundary Address Register for Non-secure Region", p 503 */
  *ra_bkup_vbrpabarns() = cfg->pabans;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_bkup_security_get(ra_bkup_security_config_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "security cfg must not be nullptr");
  /* HUM Ch 12.2.1 "BBFSAR : Battery Backup Function Security Attribute Register", p 500 */
  cfg->bbfsar = (uint32_t)(*ra_bkup_bbfsar() & k_ra_bkup_bbfsar_mask_all);
  /* HUM Ch 12.2.2 "VBRSABAR : VBATT Backup Register Security Attribute Boundary Address Register", p 502 */
  cfg->saba = *ra_bkup_vbrsabar();
  /* HUM Ch 12.2.3 "VBRPABARS : VBATT Backup Register Privilege Attribute Boundary Address Register for Secure Region", p 502 */
  cfg->pabas = *ra_bkup_vbrpabars();
  /* HUM Ch 12.2.4 "VBRPABARNS : VBATT Backup Register Privilege Attribute Boundary Address Register for Non-secure Region", p 503 */
  cfg->pabans = *ra_bkup_vbrpabarns();
  return k_ra_ok;
}

/* =============================================================================
 * IRQ path
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_bkup_attach_handler(ra_bkup_event_fn_t fn, void* ctx)
{
  s_bkup_fn  = fn;
  s_bkup_ctx = ctx;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_bkup_isr_handle(void)
{
  if (!s_initialized) {
    return k_ra_err_not_initialized;
  }

  /* HUM Ch 12.4 "Interrupt Sources" Table 12.2 p 518: VBATTADI fires when
   * any (VBTADFn AND VBTADIEn) is true. Read both, AND them, and
   * dispatch the masked subset. */
  /* HUM Ch 12.2.14 "VBTADSR : VBATT Tamper Detection Status Register", p 509 */
  const uint8_t flags = (uint8_t)(*ra_bkup_vbtadsr() & k_ra_bkup_vbtadsr_mask_all);
  /* HUM Ch 12.2.15 "VBTADCR1 : VBATT Tamper Detection Control Register 1", p 510 */
  const uint8_t enables = (uint8_t)(*ra_bkup_vbtadcr1() & k_ra_bkup_vbtadcr1_mask_ie_all);
  const uint8_t fired   = (uint8_t)(flags & enables);

  if (fired != 0U) {
    /* HUM Ch 12.2.14 "VBTADSR : VBATT Tamper Detection Status Register", p 509 */
    /* W0C: write 0 to the bits we are dispatching, leave others alone. */
    const uint8_t live = *ra_bkup_vbtadsr();
    *ra_bkup_vbtadsr() = (uint8_t)(live & (uint8_t)~fired);
    ra_bkup_dispatch(fired);
  }
  return k_ra_ok;
}

void ra_bkup_dispatch(uint8_t tamper_flags)
{
  const ra_bkup_event_fn_t fn  = s_bkup_fn;
  void* const              ctx = s_bkup_ctx;
  if (fn != nullptr) {
    fn(ctx, tamper_flags);
  }
}
