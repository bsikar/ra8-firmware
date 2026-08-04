/**
 * @file ra8_dotf_power.c
 * @brief Decryption On The Fly (DOTF) high-level open/close + power control
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * High-level orchestration layer for the RA8D2 DOTF block, split out of
 * ``ra8_dotf.c`` purely to keep each translation unit under the per-file
 * size cap. Hosts the multi-step ``ra8_dotf_open`` bring-up sequence and its
 * internal sub-steps, the symmetric ``ra8_dotf_close``, the convenience
 * window helper ``ra8_dotf_set_region_window``, and the module-stop
 * enter/exit pair. Every register / clock-gate access carries a HUM Ch 45
 * citation. See ``ra8_dotf.h`` for the public surface.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_dotf.h"
#include "ra8_dotf_regs.h"
#include "ra8_err.h"
#include "ra8_hal_internal.h"
#include "ra8_log.h"
#include "ra8_mstp.h"

/**
 * @var s_tag
 * @brief Logging tag for ra8_log_* calls.
 */
static const char* s_tag = "DOTF";

/**
 * @var s_dotf_mstp_table
 * @brief Channel-index -> MSTP id lookup.
 *
 * @details
 * DOTF0 + XSPI0 share ``MSTPB16``; DOTF1 + XSPI1 share ``MSTPB17``
 * (HUM Ch 11.2.7 MSTPCRB description references both peripherals).
 * The MSTP wrapper enums in ``ra8_mstp_regs.h`` already encode this
 * as ``k_ra8_mstp_ospi0`` / ``k_ra8_mstp_ospi1`` -- the comments call
 * them out as "OSPI0+DOTF0" / "OSPI1+DOTF1" so we just reuse them
 * here rather than minting DOTF-specific aliases.
 */
static const ra8_mstp_t s_dotf_mstp_table[k_ra8_dotf_channel_count] = {
  k_ra8_mstp_ospi0,
  k_ra8_mstp_ospi1,
};

/* =============================================================================
 * Power
 * =============================================================================
 */

/**
 * @brief Validate ``ra8_dotf_open`` inputs and power up the DOTF block.
 *
 * @details
 * Internal sub-step of ``ra8_dotf_open``. Extracted so the public entry
 * point stays under the NASA Rule 4 / clang-tidy
 * ``readability-function-size`` and ``readability-function-cognitive-complexity``
 * thresholds without requiring an inline NOLINT override.
 *
 * Sequence:
 *   1. Range-check ``cfg->channel``.
 *   2. Idempotently power on both DOTF channels via ``ra8_dotf_init``
 *      (HUM Ch 45.6.1 p 3050).
 *
 * @param[in] cfg Non-NULL caller-supplied open config.
 *
 * @return ``ra8_err_t`` propagated from the underlying steps.
 * @retval k_ra8_ok                    Channel valid, DOTF block powered.
 * @retval k_ra8_err_invalid_arg       ``cfg->channel`` out of range.
 * @retval k_ra8_err_hw_init_failed    Channel-base mapping failed.
 *
 * @pre ``cfg`` is non-NULL.
 * @pre Caller is single-threaded (driver bring-up context).
 * @post On ``k_ra8_ok`` MSTPB16/17 are ungated for both channels.
 *
 * @note Not thread-safe; sole caller is ``ra8_dotf_open``.
 *
 * @see ra8_dotf_init
 *
 * @since
 */
RA8_INTERNAL
[[nodiscard]] static ra8_err_t internal_open_validate_init(const ra8_dotf_open_cfg_t* cfg)
{
  if (!ra8_dotf_internal_channel_in_range(cfg->channel)) {
    return k_ra8_err_invalid_arg;
  }
  /* Step 1: Power on the DOTF block (idempotent). HUM Ch 45.6.1 p 3050. */
  const ra8_err_t init_err = ra8_dotf_init();
  RA8_RETURN_ON_ERROR(init_err, s_tag, "open: init"); /* GCOVR_EXCL_BR_LINE */
  return k_ra8_ok;
}

/**
 * @brief Stage the wrapped key, IV, and conversion region for ``ra8_dotf_open``.
 *
 * @details
 * Internal sub-step of ``ra8_dotf_open``. Performs steps 2..5: wrapped-key
 * install (HUM Ch 45.3 p 3049 REG03 staging), IV stage (HUM Ch 45.1
 * p 3048 counter mode), region descriptor stage (HUM Ch 45.3.1 / 45.3.2
 * p 3049), and live-region promotion via ``ra8_dotf_select_region``.
 *
 * @param[in] cfg Non-NULL caller-supplied open config (already validated).
 *
 * @return ``ra8_err_t`` propagated from the underlying steps.
 * @retval k_ra8_ok                    Key, IV, and region all staged.
 * @retval k_ra8_err_invalid_arg       Underlying primitive rejected input.
 * @retval k_ra8_err_conflict          Region overlaps live region of other
 *                                    channel.
 * @retval k_ra8_err_invalid_state     ``ra8_dotf_select_region`` rejected.
 *
 * @pre ``cfg`` is non-NULL and has passed ``internal_open_validate_init``.
 * @pre DOTF MSTP gate is open for ``cfg->channel``.
 * @post On ``k_ra8_ok`` REG03 holds wrapped key + IV, CONVAREAST/ED reflect
 *       the requested region, and ``s_dotf_state[ch].active_region_id``
 *       equals ``cfg->region.region_id``.
 *
 * @note Not thread-safe; sole caller is ``ra8_dotf_open``.
 *
 * @see ra8_dotf_install_key
 * @see ra8_dotf_set_iv
 * @see ra8_dotf_set_region
 * @see ra8_dotf_select_region
 *
 * @since
 */
RA8_INTERNAL
[[nodiscard]] static ra8_err_t internal_open_stage_key_iv_region(const ra8_dotf_open_cfg_t* cfg)
{
  /* Step 2: Install the wrapped key. HUM Ch 45.3 p 3049 (REG03 staging). */
  const ra8_err_t key_err = ra8_dotf_install_key(cfg->channel, &cfg->key);
  RA8_RETURN_ON_ERROR(key_err, s_tag, "open: install_key"); /* GCOVR_EXCL_BR_LINE */

  /* Step 3: Stage the IV. HUM Ch 45.1 p 3048 (counter mode). */
  const ra8_err_t iv_err = ra8_dotf_set_iv(cfg->channel, cfg->iv_words);
  RA8_RETURN_ON_ERROR(iv_err, s_tag, "open: set_iv"); /* GCOVR_EXCL_BR_LINE */

  /* Step 4: Stage the conversion region. HUM Ch 45.3.1 / 45.3.2 p 3049. */
  const ra8_err_t reg_err = ra8_dotf_set_region(cfg->channel, &cfg->region);
  RA8_RETURN_ON_ERROR(reg_err, s_tag, "open: set_region"); /* GCOVR_EXCL_BR_LINE */

  /* Step 5: Promote to live region. */
  const ra8_err_t sel_err = ra8_dotf_select_region(cfg->channel, cfg->region.region_id);
  RA8_RETURN_ON_ERROR(sel_err, s_tag, "open: select_region"); /* GCOVR_EXCL_BR_LINE */
  return k_ra8_ok;
}

/**
 * @brief Apply SCA level and (optionally) arm the AES core.
 *
 * @details
 * Internal sub-step of ``ra8_dotf_open``. Performs step 6 (REG00 SCA bits
 * via ``ra8_dotf_set_sca_level``) and step 7 (gated by
 * ``cfg->enable_after``: arm the AES enable bit via ``ra8_dotf_enable``).
 * HUM Ch 45.3 "Register Descriptions" p 3049.
 *
 * @param[in] cfg Non-NULL caller-supplied open config (already validated).
 *
 * @return ``ra8_err_t`` propagated from the underlying steps.
 * @retval k_ra8_ok                    SCA applied; AES armed if requested.
 * @retval k_ra8_err_invalid_arg       Underlying primitive rejected input.
 *
 * @pre ``cfg`` is non-NULL.
 * @pre Key, IV, and region staging have already succeeded.
 * @post Cached SCA level reflects ``cfg->sca_level``.
 * @post On ``k_ra8_ok`` and ``cfg->enable_after``, REG00 has the AES
 *       enable bit set.
 *
 * @note Not thread-safe; sole caller is ``ra8_dotf_open``.
 *
 * @see ra8_dotf_set_sca_level
 * @see ra8_dotf_enable
 *
 * @since
 */
RA8_INTERNAL
[[nodiscard]] static ra8_err_t internal_open_finalise(const ra8_dotf_open_cfg_t* cfg)
{
  /* Step 6: Side-channel level. */
  const ra8_err_t sca_err = ra8_dotf_set_sca_level(cfg->channel, cfg->sca_level);
  RA8_RETURN_ON_ERROR(sca_err, s_tag, "open: set_sca_level"); /* GCOVR_EXCL_BR_LINE */

  /* Step 7: Optionally arm the AES core. */
  if (cfg->enable_after) {
    const ra8_err_t en_err = ra8_dotf_enable(cfg->channel);
    RA8_RETURN_ON_ERROR(en_err, s_tag, "open: enable"); /* GCOVR_EXCL_BR_LINE */
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_dotf_open(const ra8_dotf_open_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");

  const ra8_err_t vi_err = internal_open_validate_init(cfg);
  RA8_RETURN_ON_ERROR(vi_err, s_tag, "open: validate_init"); /* GCOVR_EXCL_BR_LINE */

  const ra8_err_t st_err = internal_open_stage_key_iv_region(cfg);
  RA8_RETURN_ON_ERROR(st_err, s_tag, "open: stage"); /* GCOVR_EXCL_BR_LINE */

  const ra8_err_t fi_err = internal_open_finalise(cfg);
  RA8_RETURN_ON_ERROR(fi_err, s_tag, "open: finalise"); /* GCOVR_EXCL_BR_LINE */
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_dotf_close(void)
{
  /* Symmetric companion to ra8_dotf_open. HUM Ch 45.6.1 p 3050. */
  return ra8_dotf_deinit();
}

[[nodiscard]] ra8_err_t ra8_dotf_set_region_window(uint8_t channel, uint32_t start, uint32_t len)
{
  if (!ra8_dotf_internal_channel_in_range(channel)) {
    return k_ra8_err_invalid_arg;
  }
  if (len == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if ((start & k_ra8_dotf_addr_low_mask) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  if ((len & k_ra8_dotf_addr_low_mask) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 45.3.1 "CONVAREAST" p 3049 */
  /* HUM Ch 45.3.2 "CONVAREAD" p 3049 */
  /* end address is inclusive. */
  const ra8_dotf_region_t r = {
    .start_addr = start,
    .end_addr   = start + len - 1U,
    .key_index  = 0U,
    .region_id  = 0U,
  };
  return ra8_dotf_set_region(channel, &r);
}

[[nodiscard]] ra8_err_t ra8_dotf_enter_stop(void)
{
  for (uint8_t ch = 0U; ch < k_ra8_dotf_channel_count; ++ch) {
    /* HUM Ch 45.6.1 "Module-stop Function" p 3050 */
    (void)ra8_mstp_disable(s_dotf_mstp_table[ch]);
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_dotf_exit_stop(void)
{
  for (uint8_t ch = 0U; ch < k_ra8_dotf_channel_count; ++ch) {
    /* HUM Ch 45.6.1 "Module-stop Function" p 3050 */
    const ra8_err_t mst_err = ra8_mstp_enable(s_dotf_mstp_table[ch]);
    RA8_RETURN_ON_ERROR(mst_err, s_tag, "exit_stop: mstp enable failed"); /* GCOVR_EXCL_BR_LINE */
  }
  return k_ra8_ok;
}
