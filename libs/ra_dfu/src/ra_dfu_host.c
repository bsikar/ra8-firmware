/**
 * @file ra_dfu_host.c
 * @brief Polled on-board USB-DFU host driver (factored from usb_selftest_dfu).
 *
 * @par Tag
 * [Ring 4 / Service] {World: S}
 *
 * @details
 * Enumerate -> DFU_DNLOAD -> DFU_ABORT -> DFU_UPLOAD -> byte-verify, on whichever
 * controller the caller names via `ra_usb_speed_t`. Built entirely on the
 * first-party polled `ra_usb_host_*` + `ra_usb_host_control_xfer` primitives,
 * with no USBX on the host side. Firmware-only (`RA_SIMULATOR_MODE` skips it).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_dfu_host.h"

#ifndef RA_SIMULATOR_MODE

#include <string.h>

#include "ra_time.h"
#include "ra_usb.h"

/** @brief Chapter-9 + DFU class request / descriptor constants. */
typedef enum : uint16_t {
  k_rdh_bm_std_dev_in     = 0x80U, /**< Std | Device | In.       */
  k_rdh_bm_std_dev_out    = 0x00U, /**< Std | Device | Out.      */
  k_rdh_bm_class_if_out   = 0x21U, /**< Class | Interface | Out. */
  k_rdh_bm_class_if_in    = 0xA1U, /**< Class | Interface | In.  */
  k_rdh_breq_get_desc     = 0x06U, /**< GET_DESCRIPTOR.          */
  k_rdh_breq_set_addr     = 0x05U, /**< SET_ADDRESS.             */
  k_rdh_breq_set_config   = 0x09U, /**< SET_CONFIGURATION.       */
  k_rdh_breq_dnload       = 0x01U, /**< DFU_DNLOAD.              */
  k_rdh_breq_upload       = 0x02U, /**< DFU_UPLOAD.              */
  k_rdh_breq_getstatus    = 0x03U, /**< DFU_GETSTATUS.           */
  k_rdh_breq_abort        = 0x06U, /**< DFU_ABORT (-> dfuIDLE).  */
  k_rdh_desc_device       = 0x01U, /**< DEVICE descriptor type.  */
  k_rdh_dev_desc_len      = 18U,   /**< DEVICE descriptor length.*/
  k_rdh_off_dev_pid       = 10U,   /**< idProduct LSB offset.    */
  k_rdh_byte_bits         = 8U,    /**< Bits per byte.           */
  k_rdh_getstatus_len     = 6U,    /**< DFU_GETSTATUS payload.   */
  k_rdh_off_status_state  = 4U,    /**< bState offset.           */
  k_rdh_state_dnload_idle = 5U,    /**< dfuDNLOAD-IDLE.          */
  k_rdh_state_idle        = 2U,    /**< dfuIDLE.                 */
  k_rdh_dev_addr          = 1U,    /**< Operating device address.*/
  k_rdh_config_val        = 1U,    /**< bConfigurationValue.     */
  k_rdh_intf              = 0U,    /**< DFU interface number.    */
  k_rdh_xfer_size         = 64U,   /**< wTransferSize per block. */
} rdh_proto_t;

/** @brief Timing / retry tunables for the polled enumeration + status polling. */
typedef enum : uint32_t {
  k_rdh_vbus_settle_ms = 200U,        /**< VBUS settle before probing.    */
  k_rdh_attach_to_ms   = 2000U,       /**< Wait for the D+ pull-up.       */
  k_rdh_debounce_ms    = 500U,        /**< Post-attach debounce.          */
  k_rdh_reset_hold_ms  = 50U,         /**< USB bus-reset hold (>=10 ms).  */
  k_rdh_recovery_ms    = 20U,         /**< Post-reset recovery (TRSTRCY). */
  k_rdh_addr_settle_ms = 5U,          /**< Post-SET_ADDRESS recovery.     */
  k_rdh_status_poll_ms = 2U,          /**< Pause between GETSTATUS polls.  */
  k_rdh_status_tries   = 50U,         /**< GETSTATUS polls before giving up.*/
  k_rdh_enum_tries     = 8U,          /**< Reset+probe attempts.          */
  k_rdh_attach_spin    = 50000000U,   /**< Attach spin cap.               */
  k_rdh_mismatch_none  = 0xFFFFFFFFU, /**< "no mismatch" sentinel.      */
} rdh_tune_t;

/** @brief GET_DESCRIPTOR(DEVICE) over the polled control engine. */
static ra_err_t internal_get_dev_desc(ra_usb_speed_t speed, uint8_t* desc)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_rdh_bm_std_dev_in,
    .b_request       = (uint8_t)k_rdh_breq_get_desc,
    .w_value         = (uint16_t)((uint16_t)k_rdh_desc_device << (uint16_t)k_rdh_byte_bits),
    .w_index         = 0U,
    .w_length        = (uint16_t)k_rdh_dev_desc_len,
  };
  uint16_t       rx = 0U;
  const ra_err_t err =
    ra_usb_host_control_xfer(speed, &setup, desc, (uint16_t)k_rdh_dev_desc_len, &rx);
  if (err != k_ra_ok) {
    return err;
  }
  return (rx == (uint16_t)k_rdh_dev_desc_len) ? k_ra_ok : k_ra_err_hw_error;
}

/** @brief Wait for attach, then bus-reset + read the device descriptor. */
static ra_err_t internal_enum_hunt(ra_usb_speed_t speed, uint8_t* desc)
{
  ra_delay_ms((uint32_t)k_rdh_vbus_settle_ms);
  const uint32_t t0 = ra_time_ms();
  for (uint32_t spin = 0U; spin < (uint32_t)k_rdh_attach_spin; spin++) {
    if (ra_usb_host_line_state(speed) != 0U) {
      break;
    }
    if ((ra_time_ms() - t0) > (uint32_t)k_rdh_attach_to_ms) {
      break;
    }
  }
  ra_delay_ms((uint32_t)k_rdh_debounce_ms);
  ra_err_t err = k_ra_err_hw_timeout;
  for (uint8_t attempt = 0U; attempt < (uint8_t)k_rdh_enum_tries; attempt++) {
    (void)ra_usb_host_bus_reset(speed, true);
    ra_delay_ms((uint32_t)k_rdh_reset_hold_ms);
    (void)ra_usb_host_bus_reset(speed, false);
    (void)ra_usb_host_set_uact(speed, true);
    ra_delay_ms((uint32_t)k_rdh_recovery_ms);
    (void)ra_usb_host_set_target(speed, 0U);
    err = internal_get_dev_desc(speed, desc);
    if (err == k_ra_ok) {
      return k_ra_ok;
    }
  }
  return err;
}

/** @brief SET_ADDRESS to k_rdh_dev_addr, then retarget the DCP. */
static ra_err_t internal_set_address(ra_usb_speed_t speed)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_rdh_bm_std_dev_out,
    .b_request       = (uint8_t)k_rdh_breq_set_addr,
    .w_value         = (uint16_t)k_rdh_dev_addr,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  const ra_err_t err = ra_usb_host_control_xfer(speed, &setup, nullptr, 0U, nullptr);
  if (err != k_ra_ok) {
    return err;
  }
  ra_delay_ms((uint32_t)k_rdh_addr_settle_ms);
  return ra_usb_host_set_target(speed, (uint8_t)k_rdh_dev_addr);
}

/** @brief SET_CONFIGURATION on the addressed device. */
static ra_err_t internal_set_config(ra_usb_speed_t speed)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_rdh_bm_std_dev_out,
    .b_request       = (uint8_t)k_rdh_breq_set_config,
    .w_value         = (uint16_t)k_rdh_config_val,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  return ra_usb_host_control_xfer(speed, &setup, nullptr, 0U, nullptr);
}

/** @brief DFU_GETSTATUS -> bState. */
static ra_err_t internal_getstatus(ra_usb_speed_t speed, uint8_t* out_state)
{
  uint8_t              status[k_rdh_getstatus_len] = {};
  const ra_usb_setup_t setup                       = {
    .bm_request_type = (uint8_t)k_rdh_bm_class_if_in,
    .b_request       = (uint8_t)k_rdh_breq_getstatus,
    .w_value         = 0U,
    .w_index         = (uint16_t)k_rdh_intf,
    .w_length        = (uint16_t)k_rdh_getstatus_len,
  };
  uint16_t       rx = 0U;
  const ra_err_t err =
    ra_usb_host_control_xfer(speed, &setup, status, (uint16_t)k_rdh_getstatus_len, &rx);
  if (err != k_ra_ok) {
    return err;
  }
  if (rx != (uint16_t)k_rdh_getstatus_len) {
    return k_ra_err_hw_error;
  }
  *out_state = status[k_rdh_off_status_state];
  return k_ra_ok;
}

/** @brief Poll DFU_GETSTATUS until @p want_state or the retry cap. */
static ra_err_t internal_wait_state(ra_usb_speed_t speed, uint8_t want_state)
{
  for (uint32_t i = 0U; i < (uint32_t)k_rdh_status_tries; i++) {
    uint8_t        state = 0U;
    const ra_err_t err   = internal_getstatus(speed, &state);
    if (err != k_ra_ok) {
      return err;
    }
    if (state == want_state) {
      return k_ra_ok;
    }
    ra_delay_ms((uint32_t)k_rdh_status_poll_ms);
  }
  return k_ra_err_hw_timeout;
}

/** @brief DFU_DNLOAD one block, then poll to dfuDNLOAD-IDLE. */
static ra_err_t
internal_dnload_block(ra_usb_speed_t speed, uint16_t block, uint8_t* data, uint16_t len)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_rdh_bm_class_if_out,
    .b_request       = (uint8_t)k_rdh_breq_dnload,
    .w_value         = block,
    .w_index         = (uint16_t)k_rdh_intf,
    .w_length        = len,
  };
  const ra_err_t err = ra_usb_host_control_xfer(speed, &setup, data, len, nullptr);
  if (err != k_ra_ok) {
    return err;
  }
  return internal_wait_state(speed, (uint8_t)k_rdh_state_dnload_idle);
}

/** @brief Download the whole image, then DFU_ABORT back to dfuIDLE. */
static ra_err_t internal_download_all(ra_usb_speed_t speed, const uint8_t* img, uint32_t img_len)
{
  const uint16_t blocks = (uint16_t)(img_len / (uint32_t)k_rdh_xfer_size);
  for (uint16_t b = 0U; b < blocks; b++) {
    uint8_t blk[k_rdh_xfer_size] = {};
    (void)memcpy(blk, &img[(uint32_t)b * (uint32_t)k_rdh_xfer_size], (size_t)k_rdh_xfer_size);
    const ra_err_t err = internal_dnload_block(speed, b, blk, (uint16_t)k_rdh_xfer_size);
    if (err != k_ra_ok) {
      return err;
    }
  }
  /* DFU_ABORT closes the download (dfuDNLOAD-IDLE -> dfuIDLE) without the
   * manifest's wait-for-reset, so the UPLOAD phase runs on the same device. */
  const ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_rdh_bm_class_if_out,
    .b_request       = (uint8_t)k_rdh_breq_abort,
    .w_value         = 0U,
    .w_index         = (uint16_t)k_rdh_intf,
    .w_length        = 0U,
  };
  const ra_err_t err = ra_usb_host_control_xfer(speed, &setup, nullptr, 0U, nullptr);
  if (err != k_ra_ok) {
    return err;
  }
  return internal_wait_state(speed, (uint8_t)k_rdh_state_idle);
}

/** @brief DFU_UPLOAD each block and byte-compare to @p img. */
static ra_err_t internal_upload_verify(ra_usb_speed_t        speed,
                                       const uint8_t*        img,
                                       uint32_t              img_len,
                                       ra_dfu_host_result_t* out)
{
  const uint16_t blocks = (uint16_t)(img_len / (uint32_t)k_rdh_xfer_size);
  out->blocks_ok        = 0U;
  for (uint16_t b = 0U; b < blocks; b++) {
    uint8_t              got[k_rdh_xfer_size] = {};
    const ra_usb_setup_t setup                = {
      .bm_request_type = (uint8_t)k_rdh_bm_class_if_in,
      .b_request       = (uint8_t)k_rdh_breq_upload,
      .w_value         = b,
      .w_index         = (uint16_t)k_rdh_intf,
      .w_length        = (uint16_t)k_rdh_xfer_size,
    };
    uint16_t       rx = 0U;
    const ra_err_t err =
      ra_usb_host_control_xfer(speed, &setup, got, (uint16_t)k_rdh_xfer_size, &rx);
    if (err != k_ra_ok) {
      return err;
    }
    if (rx != (uint16_t)k_rdh_xfer_size) {
      out->mismatch = (uint32_t)b;
      return k_ra_err_invalid_size;
    }
    if (memcmp(got, &img[(uint32_t)b * (uint32_t)k_rdh_xfer_size], (size_t)k_rdh_xfer_size) != 0) {
      out->mismatch = (uint32_t)b;
      return k_ra_err_invalid_state;
    }
    out->blocks_ok++;
  }
  return k_ra_ok;
}

/** @brief Enumerate + download + abort + upload-verify (no host teardown). */
static ra_err_t internal_run_seq(ra_usb_speed_t        speed,
                                 const uint8_t*        img,
                                 uint32_t              img_len,
                                 ra_dfu_host_result_t* out)
{
  uint8_t  desc[k_rdh_dev_desc_len] = {};
  ra_err_t err                      = internal_enum_hunt(speed, desc);
  if (err != k_ra_ok) {
    return err;
  }
  out->pid = (uint32_t)desc[k_rdh_off_dev_pid] |
             ((uint32_t)desc[(uint32_t)k_rdh_off_dev_pid + 1U] << (uint32_t)k_rdh_byte_bits);
  err      = internal_set_address(speed);
  if (err != k_ra_ok) {
    return err;
  }
  err = internal_set_config(speed);
  if (err != k_ra_ok) {
    return err;
  }
  err = internal_download_all(speed, img, img_len);
  if (err != k_ra_ok) {
    return err;
  }
  return internal_upload_verify(speed, img, img_len, out);
}

ra_err_t ra_dfu_host_run(ra_usb_speed_t        host_speed,
                         const uint8_t*        img,
                         uint32_t              img_len,
                         ra_dfu_host_result_t* out)
{
  if ((img == nullptr) || (out == nullptr)) {
    return k_ra_err_null_ptr;
  }
  out->pid       = 0U;
  out->blocks_ok = 0U;
  out->mismatch  = (uint32_t)k_rdh_mismatch_none;
  out->last_err  = k_ra_ok;
  if ((img_len == 0U) || ((img_len % (uint32_t)k_rdh_xfer_size) != 0U)) {
    out->last_err = k_ra_err_invalid_arg;
    return k_ra_err_invalid_arg;
  }
  ra_err_t err = ra_usb_host_init(host_speed);
  if (err != k_ra_ok) {
    out->last_err = err;
    return err;
  }
  err           = internal_run_seq(host_speed, img, img_len, out);
  out->last_err = err;
  if (err != k_ra_ok) {
    (void)ra_usb_host_deinit(host_speed);
  }
  return err;
}

#endif /* !RA_SIMULATOR_MODE */
