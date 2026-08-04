/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_ceu_init_regs.c
 * @brief Capture Engine Unit (CEU) init-time register programming
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Configuration-time half of the RA8D2 Capture Engine Unit driver,
 * split out of ``ra8_ceu.c`` to keep both translation units under the
 * file-size cap. This TU owns the ``ra8_ceu_config_t``-to-register
 * packing (CAMCR / CAPCR / CAIFR / CFLCR / CFSZR / CDOCR) and the
 * three ``ra8_ceu_program_*`` phases that ``ra8_ceu_init`` drives. The
 * runtime-control entry points (capture start/stop, plane-B shadow,
 * status, dispatch) remain in ``ra8_ceu.c``. Register sequences are
 * cited out of HUM Ch 60 (p 3626-3682).
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_ceu.h"
#include "ra8_ceu_internal.h"
#include "ra8_ceu_regs.h"

/**
 * @brief Build the CAMCR value from a configuration descriptor.
 *
 * @param[in] cfg Non-NULL config (caller already validated).
 * @return Packed CAMCR word.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_pack_camcr(const ra8_ceu_config_t* cfg)
{
  uint32_t camcr = 0U;
  camcr |= ((uint32_t)cfg->hsync_polarity) << k_ra8_ceu_camcr_shift_hdpol;
  camcr |= ((uint32_t)cfg->vsync_polarity) << k_ra8_ceu_camcr_shift_vdpol;
  camcr |= ((uint32_t)cfg->capture_format) << k_ra8_ceu_camcr_shift_jpg;
  camcr |= ((uint32_t)cfg->input_order) << k_ra8_ceu_camcr_shift_dtary;
  camcr |= ((uint32_t)cfg->data_bus) << k_ra8_ceu_camcr_shift_dtif;
  camcr |= ((uint32_t)cfg->field_polarity) << k_ra8_ceu_camcr_shift_fldpol;
  camcr |= ((uint32_t)cfg->edge.data) << k_ra8_ceu_camcr_shift_dsel;
  camcr |= ((uint32_t)cfg->edge.field) << k_ra8_ceu_camcr_shift_fldsel;
  camcr |= ((uint32_t)cfg->edge.hsync) << k_ra8_ceu_camcr_shift_hdsel;
  camcr |= ((uint32_t)cfg->edge.vsync) << k_ra8_ceu_camcr_shift_vdsel;
  return camcr;
}

/**
 * @brief Build the CAPCR value from a configuration descriptor.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] cfg See header declaration for direction and constraints.
 * @return ``ra8_err_t`` error code (or void if the signature returns void).
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_pack_capcr(const ra8_ceu_config_t* cfg)
{
  uint32_t capcr = 0U;
  if (cfg->capture_mode == k_ra8_ceu_capture_continuous) {
    capcr |= (1U << k_ra8_ceu_capcr_shift_ctncp);
  }
  capcr |= ((uint32_t)cfg->burst_mode) << k_ra8_ceu_capcr_shift_mtcm;
  capcr |= ((uint32_t)cfg->frame_drop) << k_ra8_ceu_capcr_shift_fdrp;
  return capcr;
}

/**
 * @brief Build the CAIFR value from a configuration descriptor.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] cfg See header declaration for direction and constraints.
 * @return ``ra8_err_t`` error code (or void if the signature returns void).
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_pack_caifr(const ra8_ceu_config_t* cfg)
{
  uint32_t caifr = 0U;
  caifr |= ((uint32_t)cfg->first_field) << k_ra8_ceu_caifr_shift_fci;
  if (cfg->one_field_only) {
    caifr |= (1U << k_ra8_ceu_caifr_shift_cim);
  }
  if (cfg->interlace) {
    caifr |= (1U << k_ra8_ceu_caifr_shift_ifs);
  }
  return caifr;
}

/**
 * @brief Build the CFLCR value from a configuration descriptor.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] cfg See header declaration for direction and constraints.
 * @return ``ra8_err_t`` error code (or void if the signature returns void).
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_pack_cflcr(const ra8_ceu_config_t* cfg)
{
  uint32_t cflcr = 0U;
  cflcr |= (((uint32_t)cfg->scale.h_fraction) & k_ra8_ceu_cflcr_mask_hfrac)
           << k_ra8_ceu_cflcr_shift_hfrac;
  cflcr |= ((((uint32_t)cfg->scale.h_mantissa) << k_ra8_ceu_cflcr_shift_hmant) &
            k_ra8_ceu_cflcr_mask_hmant);
  cflcr |= ((((uint32_t)cfg->scale.v_fraction) << k_ra8_ceu_cflcr_shift_vfrac) &
            k_ra8_ceu_cflcr_mask_vfrac);
  cflcr |= ((((uint32_t)cfg->scale.v_mantissa) << k_ra8_ceu_cflcr_shift_vmant) &
            k_ra8_ceu_cflcr_mask_vmant);
  return cflcr;
}

/**
 * @brief Build the CFSZR value from a configuration descriptor.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] cfg See header declaration for direction and constraints.
 * @return ``ra8_err_t`` error code (or void if the signature returns void).
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_pack_cfszr(const ra8_ceu_config_t* cfg)
{
  uint32_t cfszr = 0U;
  cfszr |= (((uint32_t)cfg->scale.h_output_clip) << k_ra8_ceu_cfszr_shift_hfclp) &
           k_ra8_ceu_cfszr_mask_hfclp;
  cfszr |= (((uint32_t)cfg->scale.v_output_clip) << k_ra8_ceu_cfszr_shift_vfclp) &
           k_ra8_ceu_cfszr_mask_vfclp;
  return cfszr;
}

/**
 * @brief Build the CDOCR value from a configuration descriptor.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] cfg See header declaration for direction and constraints.
 * @return ``ra8_err_t`` error code (or void if the signature returns void).
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_pack_cdocr(const ra8_ceu_config_t* cfg)
{
  uint32_t cdocr = 0U;
  if (cfg->byte_swap.swap_8_bit) {
    cdocr |= k_ra8_ceu_cdocr_mask_cobs;
  }
  if (cfg->byte_swap.swap_16_bit) {
    cdocr |= k_ra8_ceu_cdocr_mask_cows;
  }
  if (cfg->byte_swap.swap_32_bit) {
    cdocr |= k_ra8_ceu_cdocr_mask_cols;
  }
  cdocr |= (((uint32_t)cfg->output_format) << k_ra8_ceu_cdocr_shift_cds) & k_ra8_ceu_cdocr_mask_cds;
  if (cfg->bundle_write) {
    cdocr |= k_ra8_ceu_cdocr_mask_cbe;
  }
  return cdocr;
}

void ra8_ceu_program_geometry(const ra8_ceu_config_t* cfg)
{
  /* HUM Ch 60.2.4 "CMCYR : Camera Interface Cycle Register" p 3641 */
  const uint32_t cmcyr =
    (((uint32_t)cfg->height_px) << k_ra8_ceu_cmcyr_shift_vcyl) | (uint32_t)cfg->width_px;
  *ra8_ceu_reg32(k_ra8_ceu_off_cmcyr) = cmcyr;

  /* HUM Ch 60.2.5 "CAMOR : Camera Interface Offset Register" p 3641 */
  const uint32_t camor =
    (((uint32_t)cfg->y_start_px) << k_ra8_ceu_camor_shift_vofst) | (uint32_t)cfg->x_start_px;
  *ra8_ceu_reg32(k_ra8_ceu_off_camor) = camor;

  /* HUM Ch 60.2.6 "CAPWR : Camera Interface Width Register" p 3643 */
  uint16_t hwdth = cfg->width_px;
  if (cfg->x_capture_px != 0U) {
    hwdth = cfg->x_capture_px;
  }
  uint16_t vwdth = cfg->height_px;
  if (cfg->y_capture_lines != 0U) {
    vwdth = cfg->y_capture_lines;
  }
  const uint32_t capwr = (((uint32_t)vwdth) << k_ra8_ceu_capwr_shift_vwdth) | (uint32_t)hwdth;
  *ra8_ceu_reg32(k_ra8_ceu_off_capwr) = capwr;
}

void ra8_ceu_program_format(const ra8_ceu_config_t* cfg)
{
  *ra8_ceu_reg32(k_ra8_ceu_off_cflcr) = internal_pack_cflcr(cfg);
  *ra8_ceu_reg32(k_ra8_ceu_off_caifr) = internal_pack_caifr(cfg);
  *ra8_ceu_reg32(k_ra8_ceu_off_capcr) = internal_pack_capcr(cfg);
  *ra8_ceu_reg32(k_ra8_ceu_off_camcr) = internal_pack_camcr(cfg);
}

void ra8_ceu_program_destination(const ra8_ceu_config_t* cfg)
{
  /* HUM Ch 60.2.11 "CFSZR : Capture Filter Size Clip Register" p 3653 */
  *ra8_ceu_reg32(k_ra8_ceu_off_cfszr) = internal_pack_cfszr(cfg);

  /* HUM Ch 60.2.12 "CDWDR : Capture Destination Width Register" p 3654 */
  *ra8_ceu_reg32(k_ra8_ceu_off_cdwdr) = (uint32_t)cfg->dst_stride;

  /* HUM Ch 60.2.18 "CFWCR : Firewall Operation Control Register" p 3661 */
  *ra8_ceu_reg32(k_ra8_ceu_off_cfwcr) = 0U;

  /* HUM Ch 60.2.19 "CLFCR : Capture Low-Pass Filter Control" p 3662 */
  uint32_t clfcr = 0U;
  if (cfg->low_pass_filter) {
    clfcr = 1U;
  }
  *ra8_ceu_reg32(k_ra8_ceu_off_clfcr) = clfcr;

  /* HUM Ch 60.2.20 "CDOCR : Capture Data Output Control Register" p 3662 */
  *ra8_ceu_reg32(k_ra8_ceu_off_cdocr) = internal_pack_cdocr(cfg);

  /* HUM Ch 60.2.22 "CETCR : Capture Event Flag Clear Register" p 3669 */
  *ra8_ceu_reg32(k_ra8_ceu_off_cetcr) = 0U;
}
