/**
 * @file ra8_usb_compose.c
 * @brief The dispatch behind ra8_usb_device_compose (#766).
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Every byte here comes out of an encoder in `ra8_usb_desc.c`. This file adds
 * only the dispatch on the class kind and the argument checking a caller
 * would otherwise repeat at three call sites, so a converted app's framework
 * set is byte-identical to the three calls it replaces.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_usb_compose.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_usb_desc.h"

/** @brief The one class count the encoders underneath can model. */
enum : uint8_t {
  k_internal_single_function = 1U, /**< Entries a composition may carry. */
};

/**
 * @brief Encode the device framework for the one class entry in @p cfg.
 *
 * @param[in]  cfg The device to describe.
 * @param[out] fw  Destination buffers; only @c device is written.
 *
 * @return ra8_err_t Result of the encode.
 * @retval k_ra8_ok              Framework written, @c device_len set.
 * @retval k_ra8_err_invalid_arg The class kind is unset or unknown.
 *
 * @note Propagates the encoder's own refusals unchanged.
 * @since 0.1.0
 */
static ra8_err_t internal_encode_device(const ra8_usb_device_cfg_t*  cfg,
                                        ra8_usb_device_frameworks_t* fw)
{
  const ra8_usb_class_t* cls = &cfg->classes[0];

  switch (cls->kind) {
    case k_ra8_usb_class_cdc_acm:
      return ra8_usb_desc_build_cdc_acm(cfg->desc,
                                        &cls->cdc_acm,
                                        fw->device,
                                        fw->device_cap,
                                        &fw->device_len);
    case k_ra8_usb_class_hid:
      return ra8_usb_desc_build_hid(cfg->desc,
                                    &cls->hid,
                                    fw->device,
                                    fw->device_cap,
                                    &fw->device_len);
    case k_ra8_usb_class_msc:
      return ra8_usb_desc_build_msc(cfg->desc,
                                    &cls->msc,
                                    fw->device,
                                    fw->device_cap,
                                    &fw->device_len);
    case k_ra8_usb_class_dfu:
      return ra8_usb_desc_build_dfu(cfg->desc,
                                    &cls->dfu,
                                    fw->device,
                                    fw->device_cap,
                                    &fw->device_len);
    case k_ra8_usb_class_none:
    default:
      return k_ra8_err_invalid_arg;
  }
}

/**
 * @brief Reject a configuration the encoders cannot serve.
 *
 * @param[in] cfg The device to describe.
 * @param[in] fw  Destination buffers.
 *
 * @return ra8_err_t Whether composition may proceed.
 * @retval k_ra8_ok                All arguments present and modelled.
 * @retval k_ra8_err_invalid_arg   A NULL pointer or a zero class count.
 * @retval k_ra8_err_not_supported More than one class entry.
 *
 * @since 0.1.0
 */
static ra8_err_t internal_check_args(const ra8_usb_device_cfg_t*        cfg,
                                     const ra8_usb_device_frameworks_t* fw)
{
  if ((cfg == NULL) || (fw == NULL)) {
    return k_ra8_err_invalid_arg;
  }
  if ((cfg->desc == NULL) || (cfg->classes == NULL) || (cfg->class_count == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if ((fw->device == NULL) || (fw->strings == NULL) || (fw->langid == NULL)) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->class_count > k_internal_single_function) {
    return k_ra8_err_not_supported;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_usb_device_compose(const ra8_usb_device_cfg_t* cfg, ra8_usb_device_frameworks_t* fw)
{
  ra8_err_t err = internal_check_args(cfg, fw);
  if (err != k_ra8_ok) {
    return err;
  }

  err = internal_encode_device(cfg, fw);
  if (err != k_ra8_ok) {
    return err;
  }

  err = ra8_usb_desc_build_strings(cfg->desc, fw->strings, fw->strings_cap, &fw->strings_len);
  if (err != k_ra8_ok) {
    return err;
  }

  return ra8_usb_desc_build_langid(cfg->desc->langid, fw->langid, fw->langid_cap, &fw->langid_len);
}
