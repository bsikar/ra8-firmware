/**
 * @file ra_nsc_io.c
 * @brief NSC veneers for the -6 I/O drivers
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_nsc_io.h"

#include <stdint.h>

#include "ra_acmphs.h"
#include "ra_adc.h"
#include "ra_check.h"
#include "ra_crc.h"
#include "ra_dac_b.h"
#include "ra_err.h"
#include "ra_eth.h"
#include "ra_glcdc.h"
#include "ra_gpt.h"
#include "ra_nsc_veneer.h"
#include "ra_pdm.h"

static const char* s_tag = "NSCIO";

RA_NSC_VENEER ra_err_t ra_nsc_gpt_init(uint8_t channel, const ra_gpt_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR((void*)cfg, s_tag, "gpt_init: cfg");
  RA_NSC_CHECK_NS_RANGE_R(cfg, sizeof(*cfg));
  return ra_gpt_init(channel, cfg);
}

RA_NSC_VENEER ra_err_t ra_nsc_gpt_read(uint8_t channel, uint32_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "gpt_read: out");
  RA_NSC_CHECK_NS_RANGE_RW(out, sizeof(*out));
  return ra_gpt_read(channel, out);
}

RA_NSC_VENEER ra_err_t ra_nsc_adc_init(void)
{
  return ra_adc_init();
}

RA_NSC_VENEER ra_err_t ra_nsc_adc_read_channel(uint8_t channel, uint16_t* out_raw)
{
  RA_CHECK_NULL_PTR(out_raw, s_tag, "adc_read: out_raw");
  RA_NSC_CHECK_NS_RANGE_RW(out_raw, sizeof(*out_raw));
  return ra_adc_read_channel(channel, out_raw);
}

RA_NSC_VENEER ra_err_t ra_nsc_dac_b_init(void)
{
  return ra_dac_b_init();
}

RA_NSC_VENEER ra_err_t ra_nsc_dac_b_write(uint8_t channel, uint16_t value)
{
  return ra_dac_b_write(channel, value);
}

RA_NSC_VENEER ra_err_t ra_nsc_acmphs_init(void)
{
  return ra_acmphs_init();
}

RA_NSC_VENEER ra_err_t ra_nsc_acmphs_read_output(uint8_t channel, ra_level_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "acmphs_read: out");
  RA_NSC_CHECK_NS_RANGE_RW(out, sizeof(*out));
  return ra_acmphs_read_output(channel, out);
}

RA_NSC_VENEER ra_err_t ra_nsc_crc_init(ra_crc_poly_t poly)
{
  return ra_crc_init(poly);
}

RA_NSC_VENEER ra_err_t ra_nsc_crc_compute(const uint8_t* data, uint32_t len, uint32_t* out_crc)
{
  RA_CHECK_NULL_PTR((void*)data, s_tag, "crc_compute: data");
  RA_CHECK_NULL_PTR(out_crc, s_tag, "crc_compute: out_crc");
  RA_NSC_CHECK_NS_RANGE_R(data, len);
  RA_NSC_CHECK_NS_RANGE_RW(out_crc, sizeof(*out_crc));
  return ra_crc_compute(data, len, out_crc);
}

RA_NSC_VENEER ra_err_t ra_nsc_glcdc_init(const ra_glcdc_config_t* cfg)
{
  RA_CHECK_NULL_PTR((void*)cfg, s_tag, "glcdc_init: cfg");
  RA_NSC_CHECK_NS_RANGE_R(cfg, sizeof(*cfg));
  return ra_glcdc_init(cfg);
}

RA_NSC_VENEER ra_err_t ra_nsc_pdm_init(void)
{
  return ra_pdm_init();
}

RA_NSC_VENEER ra_err_t ra_nsc_eth_init(void)
{
  return ra_eth_init();
}
