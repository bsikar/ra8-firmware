/**
 * @file ra8_nsc_io.c
 * @brief NSC veneers for the -6 I/O drivers
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_nsc_io.h"

#include <stdint.h>

#include "ra8_acmphs.h"
#include "ra8_adc.h"
#include "ra8_check.h"
#include "ra8_crc.h"
#include "ra8_dac_b.h"
#include "ra8_err.h"
#include "ra8_eth.h"
#include "ra8_glcdc.h"
#include "ra8_gpt.h"
#include "ra8_nsc_veneer.h"
#include "ra8_pdm.h"

static const char* s_tag = "NSCIO";

/**
 * @brief NSC veneer: bring up a GPT channel.
 *
 * @details Range-checks ``cfg`` in NS memory and forwards to
 *   ``ra8_gpt_init``.
 *
 * @param[in] channel GPT channel index.
 * @param[in] cfg     Caller configuration in NS memory.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok               Channel programmed.
 * @retval k_ra8_err_null_ptr     ``cfg`` was NULL.
 * @retval k_ra8_err_invalid_arg  ``cfg`` outside NS region or channel bad.
 *
 * @pre TrustZone substrate up.
 * @pre ``cfg`` lies in NS data region.
 * @post On success the GPT channel is counting per its mode.
 * @post On failure no module state was mutated.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. ``cfg`` is range-checked.
 *
 * @note Thread-safe: serialised by the secure GPT driver.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_gpt_init(uint8_t channel, const ra8_gpt_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "gpt_init: cfg");
  RA8_NSC_CHECK_NS_RANGE_R(cfg, sizeof(*cfg));
  return ra8_gpt_init(channel, cfg);
}

/**
 * @brief NSC veneer: read the current GPT counter value.
 *
 * @details Range-checks ``out`` in NS writable memory and forwards to
 *   ``ra8_gpt_read``.
 *
 * @param[in]  channel GPT channel index.
 * @param[out] out     NS destination for the counter value.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok               Counter read into ``*out``.
 * @retval k_ra8_err_null_ptr     ``out`` was NULL.
 * @retval k_ra8_err_invalid_arg  ``out`` outside NS region.
 *
 * @pre ``ra8_nsc_gpt_init`` succeeded for ``channel``.
 * @pre ``out`` lies in NS data region.
 * @post On success ``*out`` reflects the live counter value.
 * @post On failure ``*out`` unchanged.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. ``out`` is range-checked.
 *
 * @note Thread-safe: serialised by the secure GPT driver.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_gpt_read(uint8_t channel, uint32_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "gpt_read: out");
  RA8_NSC_CHECK_NS_RANGE_RW(out, sizeof(*out));
  return ra8_gpt_read(channel, out);
}

/**
 * @brief NSC veneer: bring up the ADC.
 *
 * @details Forwards to ``ra8_adc_init``. No arguments and no pointers
 *   cross the boundary.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok                 ADC ready.
 * @retval k_ra8_err_hw_init_failed Hardware refused init.
 *
 * @pre ``ra8_nsc_periph_init`` has been called.
 * @pre TrustZone substrate up.
 * @post On success the ADC is calibrated and idle.
 * @post On failure no module state was mutated.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. Argument-less scalar
 *   call -- nothing to range-check.
 *
 * @note Thread-safe: serialised by the secure ADC driver.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_adc_init(void)
{
  return ra8_adc_init();
}

/**
 * @brief NSC veneer: read one ADC channel sample.
 *
 * @details Range-checks ``out_raw`` in NS memory and forwards to
 *   ``ra8_adc_read_channel``.
 *
 * @param[in]  channel ADC channel index.
 * @param[out] out_raw NS destination for the 16-bit raw sample.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok               Sample stored at ``*out_raw``.
 * @retval k_ra8_err_null_ptr     ``out_raw`` was NULL.
 * @retval k_ra8_err_invalid_arg  Range outside NS region.
 *
 * @pre ``ra8_nsc_adc_init`` succeeded.
 * @pre ``out_raw`` lies in NS data region.
 * @post On success ``*out_raw`` holds the latest sample.
 * @post On failure ``*out_raw`` unchanged.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. ``out_raw`` is
 *   range-checked.
 *
 * @note Thread-safe: serialised by the secure ADC driver.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_adc_read_channel(uint8_t channel, uint16_t* out_raw)
{
  RA8_CHECK_NULL_PTR(out_raw, s_tag, "adc_read: out_raw");
  RA8_NSC_CHECK_NS_RANGE_RW(out_raw, sizeof(*out_raw));
  return ra8_adc_read_channel(channel, out_raw);
}

/**
 * @brief NSC veneer: bring up the DAC-B unit.
 *
 * @details Forwards to ``ra8_dac_b_init``.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok                 DAC ready.
 * @retval k_ra8_err_hw_init_failed Hardware refused init.
 *
 * @pre ``ra8_nsc_periph_init`` has been called.
 * @pre TrustZone substrate up.
 * @post On success the DAC channels are powered and idle.
 * @post On failure no module state was mutated.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. Argument-less scalar call.
 *
 * @note Thread-safe: serialised by the secure DAC driver.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_dac_b_init(void)
{
  return ra8_dac_b_init();
}

/**
 * @brief NSC veneer: write a 12-bit value to a DAC-B channel.
 *
 * @details Forwards to ``ra8_dac_b_write``. No pointer arguments.
 *
 * @param[in] channel DAC channel index.
 * @param[in] value   12-bit value (top 4 bits ignored).
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok               Value latched.
 * @retval k_ra8_err_invalid_arg  Bad channel index.
 *
 * @pre ``ra8_nsc_dac_b_init`` succeeded.
 * @pre TrustZone substrate up.
 * @post On success the DAC output is driving the new value.
 * @post On failure the DAC output is unchanged.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. Scalar arguments only.
 *
 * @note Thread-safe: serialised by the secure DAC driver.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_dac_b_write(uint8_t channel, uint16_t value)
{
  return ra8_dac_b_write(channel, value);
}

/**
 * @brief NSC veneer: bring up the high-speed analog comparator block.
 *
 * @details Forwards to ``ra8_acmphs_init``.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok                 Comparators ready.
 * @retval k_ra8_err_hw_init_failed Hardware refused init.
 *
 * @pre ``ra8_nsc_periph_init`` has been called.
 * @pre TrustZone substrate up.
 * @post On success the ACMPHS channels are powered.
 * @post On failure no module state was mutated.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. Argument-less call.
 *
 * @note Thread-safe: serialised by the secure ACMPHS driver.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_acmphs_init(void)
{
  return ra8_acmphs_init();
}

/**
 * @brief NSC veneer: read a comparator output level.
 *
 * @details Range-checks ``out`` in NS memory and forwards to
 *   ``ra8_acmphs_read_output``.
 *
 * @param[in]  channel Comparator channel index.
 * @param[out] out     NS destination for the level enum.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok               Level stored at ``*out``.
 * @retval k_ra8_err_null_ptr     ``out`` was NULL.
 * @retval k_ra8_err_invalid_arg  Range outside NS region.
 *
 * @pre ``ra8_nsc_acmphs_init`` succeeded.
 * @pre ``out`` lies in NS data region.
 * @post On success ``*out`` reflects the comparator level.
 * @post On failure ``*out`` unchanged.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. ``out`` is range-checked.
 *
 * @note Thread-safe: serialised by the secure ACMPHS driver.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_acmphs_read_output(uint8_t channel, ra8_level_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "acmphs_read: out");
  RA8_NSC_CHECK_NS_RANGE_RW(out, sizeof(*out));
  return ra8_acmphs_read_output(channel, out);
}

/**
 * @brief NSC veneer: bring up the CRC unit with a polynomial.
 *
 * @details Forwards to ``ra8_crc_init``. No pointer arguments.
 *
 * @param[in] poly Polynomial enum (CRC-8/16/32 selection).
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok               CRC engine programmed.
 * @retval k_ra8_err_invalid_arg  Unknown polynomial enum.
 *
 * @pre ``ra8_nsc_periph_init`` has been called.
 * @pre TrustZone substrate up.
 * @post On success the CRC engine is in its initial state.
 * @post On failure no module state was mutated.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. Scalar enum argument.
 *
 * @note Thread-safe: serialised by the secure CRC driver.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_crc_init(ra8_crc_poly_t poly)
{
  return ra8_crc_init(poly);
}

/**
 * @brief NSC veneer: compute CRC over a buffer.
 *
 * @details Range-checks ``data`` (read) and ``out_crc`` (write) in NS
 *   memory then forwards to ``ra8_crc_compute``.
 *
 * @param[in]  data    NS source buffer.
 * @param[in]  len     Byte count.
 * @param[out] out_crc NS destination for the 32-bit CRC value.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok               CRC stored at ``*out_crc``.
 * @retval k_ra8_err_null_ptr     A pointer argument was NULL.
 * @retval k_ra8_err_invalid_arg  Range outside NS region.
 *
 * @pre ``ra8_nsc_crc_init`` succeeded.
 * @pre Both pointers lie in NS data region.
 * @post On success ``*out_crc`` holds the CRC of ``[data,data+len)``.
 * @post On failure ``*out_crc`` unchanged.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. Both pointers are
 *   ``cmse_check_address_range``-validated so the secure driver only
 *   reads/writes NS memory.
 *
 * @note Thread-safe: serialised by the secure CRC driver.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_crc_compute(const uint8_t* data, uint32_t len, uint32_t* out_crc)
{
  RA8_CHECK_NULL_PTR(data, s_tag, "crc_compute: data");
  RA8_CHECK_NULL_PTR(out_crc, s_tag, "crc_compute: out_crc");
  RA8_NSC_CHECK_NS_RANGE_R(data, len);
  RA8_NSC_CHECK_NS_RANGE_RW(out_crc, sizeof(*out_crc));
  return ra8_crc_compute(data, len, out_crc);
}

/**
 * @brief NSC veneer: bring up the graphics LCD controller.
 *
 * @details Range-checks ``cfg`` in NS memory and forwards to
 *   ``ra8_glcdc_init``.
 *
 * @param[in] cfg Caller configuration in NS memory.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok               GLCDC programmed.
 * @retval k_ra8_err_null_ptr     ``cfg`` was NULL.
 * @retval k_ra8_err_invalid_arg  Range outside NS region or invalid config.
 *
 * @pre ``ra8_nsc_periph_init`` has been called.
 * @pre ``cfg`` lies in NS data region.
 * @post On success the GLCDC is scanning out the framebuffer.
 * @post On failure no module state was mutated.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. ``cfg`` is range-checked.
 *
 * @note Thread-safe: no -- GLCDC bring-up is single-threaded.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_glcdc_init(const ra8_glcdc_config_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "glcdc_init: cfg");
  RA8_NSC_CHECK_NS_RANGE_R(cfg, sizeof(*cfg));
  return ra8_glcdc_init(cfg);
}

/**
 * @brief NSC veneer: bring up the PDM microphone interface.
 *
 * @details Forwards to ``ra8_pdm_init``.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok                 PDM ready.
 * @retval k_ra8_err_hw_init_failed Hardware refused init.
 *
 * @pre ``ra8_nsc_periph_init`` has been called.
 * @pre TrustZone substrate up.
 * @post On success the PDM clock is running.
 * @post On failure no module state was mutated.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. Argument-less call.
 *
 * @note Thread-safe: serialised by the secure PDM driver.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_pdm_init(void)
{
  return ra8_pdm_init();
}

/**
 * @brief NSC veneer: bring up the ethernet switch module.
 *
 * @details Forwards to ``ra8_eth_init``.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok                 ESWM up.
 * @retval k_ra8_err_hw_init_failed Hardware refused init.
 *
 * @pre ``ra8_nsc_periph_init`` has been called.
 * @pre TrustZone substrate up.
 * @post On success the ESWM is in the link-up-pending state.
 * @post On failure no module state was mutated.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. Argument-less call.
 *
 * @note Thread-safe: no -- single-threaded bring-up.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_eth_init(void)
{
  return ra8_eth_init();
}
