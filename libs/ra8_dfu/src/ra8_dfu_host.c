/**
 * @file ra8_dfu_host.c
 * @brief Polled on-board USB-DFU host driver (factored from usb_selftest_dfu).
 *
 * @par Tag
 * [Ring 4 / Service] {World: S}
 *
 * @details
 * Enumerate -> DFU_DNLOAD -> DFU_ABORT -> DFU_UPLOAD -> byte-verify, on whichever
 * controller the caller names via `ra8_usb_speed_t`. Built entirely on the
 * first-party polled `ra8_usb_host_*` + `ra8_usb_host_control_xfer` primitives,
 * with no USBX on the host side. Firmware-only (`RA8_OFF_TARGET` skips it).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_dfu_host.h"

#ifndef RA8_OFF_TARGET

#include <string.h>

#include "ra8_attributes.h"
#include "ra8_time.h"
#include "ra8_usb.h"

/** @brief Chapter-9 + DFU class request / descriptor constants. */
typedef enum : uint16_t {
  k_rdh_bm_std_dev_in     = 0x80U, /**< Std | Device | In.        */
  k_rdh_bm_std_dev_out    = 0x00U, /**< Std | Device | Out.       */
  k_rdh_bm_class_if_out   = 0x21U, /**< Class | Interface | Out.  */
  k_rdh_bm_class_if_in    = 0xA1U, /**< Class | Interface | In.   */
  k_rdh_breq_get_desc     = 0x06U, /**< GET_DESCRIPTOR.           */
  k_rdh_breq_set_addr     = 0x05U, /**< SET_ADDRESS.              */
  k_rdh_breq_set_config   = 0x09U, /**< SET_CONFIGURATION.        */
  k_rdh_breq_dnload       = 0x01U, /**< DFU_DNLOAD.               */
  k_rdh_breq_upload       = 0x02U, /**< DFU_UPLOAD.               */
  k_rdh_breq_getstatus    = 0x03U, /**< DFU_GETSTATUS.            */
  k_rdh_breq_abort        = 0x06U, /**< DFU_ABORT (-> dfuIDLE).   */
  k_rdh_desc_device       = 0x01U, /**< DEVICE descriptor type.   */
  k_rdh_dev_desc_len      = 18U,   /**< DEVICE descriptor length. */
  k_rdh_off_dev_pid       = 10U,   /**< idProduct LSB offset.     */
  k_rdh_byte_bits         = 8U,    /**< Bits per byte.            */
  k_rdh_getstatus_len     = 6U,    /**< DFU_GETSTATUS payload.    */
  k_rdh_off_status_state  = 4U,    /**< bState offset.            */
  k_rdh_state_dnload_idle = 5U,    /**< dfuDNLOAD-IDLE.           */
  k_rdh_state_idle        = 2U,    /**< dfuIDLE.                  */
  k_rdh_dev_addr          = 1U,    /**< Operating device address. */
  k_rdh_config_val        = 1U,    /**< bConfigurationValue.      */
  k_rdh_intf              = 0U,    /**< DFU interface number.     */
  k_rdh_xfer_size         = 64U,   /**< wTransferSize per block.  */
} rdh_proto_t;

/** @brief Timing / retry tunables for the polled enumeration + status polling. */
typedef enum : uint32_t {
  k_rdh_vbus_settle_ms = 200U,        /**< VBUS settle before probing.       */
  k_rdh_attach_to_ms   = 2000U,       /**< Wait for the D+ pull-up.          */
  k_rdh_debounce_ms    = 500U,        /**< Post-attach debounce.             */
  k_rdh_reset_hold_ms  = 50U,         /**< USB bus-reset hold (>=10 ms).     */
  k_rdh_recovery_ms    = 20U,         /**< Post-reset recovery (TRSTRCY).    */
  k_rdh_addr_settle_ms = 5U,          /**< Post-SET_ADDRESS recovery.        */
  k_rdh_status_poll_ms = 2U,          /**< Pause between GETSTATUS polls.    */
  k_rdh_status_tries   = 50U,         /**< GETSTATUS polls before giving up. */
  k_rdh_enum_tries     = 8U,          /**< Reset+probe attempts.             */
  k_rdh_attach_spin    = 50000000U,   /**< Attach spin cap.                  */
  k_rdh_mismatch_none  = 0xFFFFFFFFU, /**< "no mismatch" sentinel.           */
} rdh_tune_t;

/**
 * @brief Issue GET_DESCRIPTOR(DEVICE) over the polled control engine.
 *
 * @details
 * Builds a standard Chapter-9 GET_DESCRIPTOR setup packet requesting the
 * DEVICE descriptor (type 0x01, length 18 bytes) and executes it via
 * ra8_usb_host_control_xfer.  On success the 18-byte descriptor is stored in
 * @p desc.  If the transfer succeeds but the controller returns fewer than
 * k_rdh_dev_desc_len bytes the function treats this as a hardware error and
 * returns k_ra8_err_hw_error so that the caller can retry.
 *
 * @param[in]  speed  Controller speed (FS or HS) that selects the USB port.
 * @param[out] desc   Caller-allocated buffer of at least k_rdh_dev_desc_len
 *                    bytes; receives the raw DEVICE descriptor on success.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Descriptor received and fully length-checked.
 * @retval k_ra8_err_hw_error    Transfer succeeded but byte count was short.
 * @retval k_ra8_err_*           Underlying ra8_usb_host_control_xfer failure.
 *
 * @pre The USB host controller named by @p speed has been initialized.
 * @pre The device is held at address 0 (before SET_ADDRESS).
 * @post On k_ra8_ok, @p desc holds the complete 18-byte DEVICE descriptor.
 * @post On failure, @p desc content is undefined.
 *
 * @note Not thread-safe; called only from the single-threaded polled sequence.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_get_dev_desc(ra8_usb_speed_t speed, uint8_t* desc)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_rdh_bm_std_dev_in,
    .b_request       = (uint8_t)k_rdh_breq_get_desc,
    .w_value         = (uint16_t)((uint16_t)k_rdh_desc_device << (uint16_t)k_rdh_byte_bits),
    .w_index         = 0U,
    .w_length        = (uint16_t)k_rdh_dev_desc_len,
  };
  uint16_t        rx = 0U;
  const ra8_err_t err =
    ra8_usb_host_control_xfer(speed, &setup, desc, (uint16_t)k_rdh_dev_desc_len, &rx);
  if (err != k_ra8_ok) {
    return err;
  }
  return (rx == (uint16_t)k_rdh_dev_desc_len) ? k_ra8_ok : k_ra8_err_hw_error;
}

/**
 * @brief Wait for device attach, apply bus reset, and read the DEVICE descriptor.
 *
 * @details
 * Sequence:
 *   1. Waits k_rdh_vbus_settle_ms for VBUS to stabilise.
 *   2. Spins (up to k_rdh_attach_spin iterations or k_rdh_attach_to_ms) until
 *      the D+ line goes high (FS pull-up detected).
 *   3. Applies k_rdh_debounce_ms debounce delay.
 *   4. Retries up to k_rdh_enum_tries times: assert bus reset for
 *      k_rdh_reset_hold_ms, release, enable UACT, wait k_rdh_recovery_ms,
 *      retarget the DCP to address 0, then call internal_get_dev_desc.
 *   Returns immediately on the first successful descriptor read.
 *
 * @param[in]  speed  Controller speed (FS or HS) selecting the USB port.
 * @param[out] desc   Caller-allocated buffer of at least k_rdh_dev_desc_len
 *                    bytes; receives the raw DEVICE descriptor on success.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Device attached and descriptor received.
 * @retval k_ra8_err_hw_timeout  All k_rdh_enum_tries attempts failed.
 * @retval k_ra8_err_*           Last internal_get_dev_desc failure code.
 *
 * @pre The USB host controller named by @p speed has been initialized.
 * @pre VBUS is powered on the host jack.
 * @post On k_ra8_ok, the device is enumerated at address 0 and @p desc is valid.
 * @post On failure, bus state is indeterminate; the controller should be restarted.
 *
 * @note Not thread-safe; called only from the single-threaded polled sequence.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_enum_hunt(ra8_usb_speed_t speed, uint8_t* desc)
{
  ra8_delay_ms((uint32_t)k_rdh_vbus_settle_ms);
  const uint32_t t0 = ra8_time_ms();
  for (uint32_t spin = 0U; spin < (uint32_t)k_rdh_attach_spin; spin++) {
    if (ra8_usb_host_line_state(speed) != 0U) {
      break;
    }
    if ((ra8_time_ms() - t0) > (uint32_t)k_rdh_attach_to_ms) {
      break;
    }
  }
  ra8_delay_ms((uint32_t)k_rdh_debounce_ms);
  ra8_err_t err = k_ra8_err_hw_timeout;
  for (uint8_t attempt = 0U; attempt < (uint8_t)k_rdh_enum_tries; attempt++) {
    (void)ra8_usb_host_bus_reset(speed, true);
    ra8_delay_ms((uint32_t)k_rdh_reset_hold_ms);
    (void)ra8_usb_host_bus_reset(speed, false);
    (void)ra8_usb_host_set_uact(speed, true);
    ra8_delay_ms((uint32_t)k_rdh_recovery_ms);
    (void)ra8_usb_host_set_target(speed, 0U);
    err = internal_get_dev_desc(speed, desc);
    if (err == k_ra8_ok) {
      return k_ra8_ok;
    }
  }
  return err;
}

/**
 * @brief Issue SET_ADDRESS(1) and retarget the DCP to the new address.
 *
 * @details
 * Sends a standard Chapter-9 SET_ADDRESS control request assigning the device
 * address k_rdh_dev_addr (1).  After the successful status phase the function
 * waits k_rdh_addr_settle_ms for the device to latch the new address, then
 * calls ra8_usb_host_set_target to point the host DCP at address 1 so
 * subsequent control transfers reach the addressed device.
 *
 * @param[in]  speed  Controller speed (FS or HS) selecting the USB port.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              SET_ADDRESS completed; DCP retargeted to addr 1.
 * @retval k_ra8_err_*           ra8_usb_host_control_xfer or set_target failure.
 *
 * @pre The device has been enumerated at address 0 (internal_enum_hunt passed).
 * @pre The DCP is currently targeting device address 0.
 * @post On k_ra8_ok, all subsequent DCP transactions use device address 1.
 * @post k_rdh_addr_settle_ms has elapsed since the status phase.
 *
 * @note Not thread-safe; called only from the single-threaded polled sequence.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_set_address(ra8_usb_speed_t speed)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_rdh_bm_std_dev_out,
    .b_request       = (uint8_t)k_rdh_breq_set_addr,
    .w_value         = (uint16_t)k_rdh_dev_addr,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  const ra8_err_t err = ra8_usb_host_control_xfer(speed, &setup, nullptr, 0U, nullptr);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms((uint32_t)k_rdh_addr_settle_ms);
  return ra8_usb_host_set_target(speed, (uint8_t)k_rdh_dev_addr);
}

/**
 * @brief Issue SET_CONFIGURATION(1) on the addressed device.
 *
 * @details
 * Sends a standard Chapter-9 SET_CONFIGURATION control request selecting
 * bConfigurationValue k_rdh_config_val (1), which activates the DFU interface
 * and brings the device from the Addressed state into the Configured state.
 * The request has no data phase; success is indicated by the device accepting
 * the status phase.
 *
 * @param[in]  speed  Controller speed (FS or HS) selecting the USB port.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Device is now in the Configured state (dfuIDLE).
 * @retval k_ra8_err_*           ra8_usb_host_control_xfer failure.
 *
 * @pre SET_ADDRESS has completed successfully (internal_set_address passed).
 * @pre The DCP is targeting device address k_rdh_dev_addr.
 * @post On k_ra8_ok, the DFU interface is active and the device is in dfuIDLE.
 * @post The device is ready to accept DFU class requests.
 *
 * @note Not thread-safe; called only from the single-threaded polled sequence.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_set_config(ra8_usb_speed_t speed)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_rdh_bm_std_dev_out,
    .b_request       = (uint8_t)k_rdh_breq_set_config,
    .w_value         = (uint16_t)k_rdh_config_val,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  return ra8_usb_host_control_xfer(speed, &setup, nullptr, 0U, nullptr);
}

/**
 * @brief Issue DFU_GETSTATUS and return the bState byte.
 *
 * @details
 * Sends a DFU class-specific GET_STATUS (bmRequestType=0xA1, bRequest=0x03)
 * to interface k_rdh_intf and reads the 6-byte status response.  The function
 * validates that the transfer returned exactly k_rdh_getstatus_len (6) bytes,
 * then extracts the bState field from offset k_rdh_off_status_state (4) and
 * stores it in @p out_state.  The bStatus byte at offset 0 is intentionally
 * ignored; the caller only needs the state machine position.
 *
 * @param[in]  speed      Controller speed (FS or HS) selecting the USB port.
 * @param[out] out_state  Receives the bState byte from the GETSTATUS response.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Status received; @p out_state is valid.
 * @retval k_ra8_err_hw_error    Transfer succeeded but byte count was not 6.
 * @retval k_ra8_err_*           ra8_usb_host_control_xfer failure.
 *
 * @pre The device is in the Configured state and the DFU interface is active.
 * @pre @p out_state is a non-NULL pointer to a writable uint8_t.
 * @post On k_ra8_ok, @p out_state holds the current DFU bState value.
 * @post On failure, @p out_state is unchanged.
 *
 * @note Not thread-safe; called only from the single-threaded polled sequence.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_getstatus(ra8_usb_speed_t speed, uint8_t* out_state)
{
  uint8_t               status[k_rdh_getstatus_len] = {};
  const ra8_usb_setup_t setup                       = {
    .bm_request_type = (uint8_t)k_rdh_bm_class_if_in,
    .b_request       = (uint8_t)k_rdh_breq_getstatus,
    .w_value         = 0U,
    .w_index         = (uint16_t)k_rdh_intf,
    .w_length        = (uint16_t)k_rdh_getstatus_len,
  };
  uint16_t        rx = 0U;
  const ra8_err_t err =
    ra8_usb_host_control_xfer(speed, &setup, status, (uint16_t)k_rdh_getstatus_len, &rx);
  if (err != k_ra8_ok) {
    return err;
  }
  if (rx != (uint16_t)k_rdh_getstatus_len) {
    return k_ra8_err_hw_error;
  }
  *out_state = status[k_rdh_off_status_state];
  return k_ra8_ok;
}

/**
 * @brief Poll DFU_GETSTATUS until the device reaches @p want_state or the retry cap.
 *
 * @details
 * Loops up to k_rdh_status_tries times, calling internal_getstatus on each
 * iteration.  When the returned bState equals @p want_state the function
 * returns k_ra8_ok immediately.  Between unsuccessful polls it inserts a
 * k_rdh_status_poll_ms delay.  If the retry cap is exhausted without seeing
 * the target state the function returns k_ra8_err_hw_timeout.  Any
 * internal_getstatus failure terminates the loop early and propagates the
 * error to the caller.
 *
 * @param[in]  speed       Controller speed (FS or HS) selecting the USB port.
 * @param[in]  want_state  DFU bState value to wait for (e.g. dfuDNLOAD-IDLE=5,
 *                         dfuIDLE=2).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              The device entered @p want_state within the limit.
 * @retval k_ra8_err_hw_timeout  k_rdh_status_tries polls elapsed without match.
 * @retval k_ra8_err_*           internal_getstatus failure propagated.
 *
 * @pre The device is in the Configured state and the DFU interface is active.
 * @pre @p want_state is a valid DFU bState value for the expected transition.
 * @post On k_ra8_ok, the device bState at the last GETSTATUS poll equals @p want_state.
 * @post On k_ra8_err_hw_timeout, at least k_rdh_status_tries GETSTATUS polls
 *       were issued without observing the target state.
 *
 * @note Not thread-safe; called only from the single-threaded polled sequence.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_wait_state(ra8_usb_speed_t speed, uint8_t want_state)
{
  for (uint32_t i = 0U; i < (uint32_t)k_rdh_status_tries; i++) {
    uint8_t         state = 0U;
    const ra8_err_t err   = internal_getstatus(speed, &state);
    if (err != k_ra8_ok) {
      return err;
    }
    if (state == want_state) {
      return k_ra8_ok;
    }
    ra8_delay_ms((uint32_t)k_rdh_status_poll_ms);
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief Issue DFU_DNLOAD for one block, then poll to dfuDNLOAD-IDLE.
 *
 * @details
 * Sends a DFU class-specific DNLOAD request (bmRequestType=0x21, bRequest=0x01)
 * with wValue set to @p block and wLength set to @p len.  The OUT data phase
 * carries the bytes pointed to by @p data.  On transfer success the function
 * immediately calls internal_wait_state to poll DFU_GETSTATUS until the device
 * reaches dfuDNLOAD-IDLE (bState=5), confirming it has processed and buffered
 * the block before the next one is sent.
 *
 * @param[in]  speed  Controller speed (FS or HS) selecting the USB port.
 * @param[in]  block  DFU block sequence number (wValue in the setup packet).
 * @param[in]  data   Pointer to the block payload (exactly @p len bytes).
 * @param[in]  len    Number of bytes to download in this block.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Block delivered and device reached dfuDNLOAD-IDLE.
 * @retval k_ra8_err_hw_timeout  Device did not reach dfuDNLOAD-IDLE in time.
 * @retval k_ra8_err_*           ra8_usb_host_control_xfer or wait_state failure.
 *
 * @pre The device is in dfuDNLOAD-IDLE (or dfuIDLE for block 0) state.
 * @pre @p data points to a buffer of at least @p len valid bytes.
 * @post On k_ra8_ok, the device has accepted block @p block and is in dfuDNLOAD-IDLE.
 * @post On failure, DFU state is indeterminate; the sequence should be aborted.
 *
 * @note Not thread-safe; called only from the single-threaded polled sequence.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_dnload_block(ra8_usb_speed_t speed, uint16_t block, uint8_t* data, uint16_t len)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_rdh_bm_class_if_out,
    .b_request       = (uint8_t)k_rdh_breq_dnload,
    .w_value         = block,
    .w_index         = (uint16_t)k_rdh_intf,
    .w_length        = len,
  };
  const ra8_err_t err = ra8_usb_host_control_xfer(speed, &setup, data, len, nullptr);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_wait_state(speed, (uint8_t)k_rdh_state_dnload_idle);
}

/**
 * @brief Download the entire image in 64-byte blocks, then DFU_ABORT to dfuIDLE.
 *
 * @details
 * Iterates over @p img in k_rdh_xfer_size (64)-byte blocks, calling
 * internal_dnload_block for each.  After all blocks have been sent, instead
 * of issuing the standard zero-length manifest DFU_DNLOAD, this function
 * sends DFU_ABORT (bRequest=0x06) to transition the device from dfuDNLOAD-IDLE
 * back to dfuIDLE without triggering a manifest/reset cycle.  This keeps the
 * device enumerated and in a known idle state so the upload-verify phase can
 * immediately follow on the same enumeration.
 *
 * @param[in]  speed    Controller speed (FS or HS) selecting the USB port.
 * @param[in]  img      Pointer to the image buffer (non-NULL).
 * @param[in]  img_len  Total image length; must be a non-zero multiple of
 *                      k_rdh_xfer_size (64).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              All blocks downloaded; device is in dfuIDLE.
 * @retval k_ra8_err_hw_timeout  A block poll timed out or abort wait timed out.
 * @retval k_ra8_err_*           internal_dnload_block or DFU_ABORT xfer failure.
 *
 * @pre The device is in dfuIDLE state (SET_CONFIGURATION completed).
 * @pre @p img_len is a non-zero multiple of k_rdh_xfer_size and @p img is valid.
 * @post On k_ra8_ok, all image blocks are buffered in the device and it is in dfuIDLE.
 * @post The device did NOT enter dfuMANIFEST; it aborted cleanly to dfuIDLE.
 *
 * @note Not thread-safe; called only from the single-threaded polled sequence.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_download_all(ra8_usb_speed_t speed, const uint8_t* img, uint32_t img_len)
{
  const uint16_t blocks = (uint16_t)(img_len / (uint32_t)k_rdh_xfer_size);
  for (uint16_t b = 0U; b < blocks; b++) {
    uint8_t blk[k_rdh_xfer_size] = {};
    (void)memcpy(blk, &img[(uint32_t)b * (uint32_t)k_rdh_xfer_size], (size_t)k_rdh_xfer_size);
    const ra8_err_t err = internal_dnload_block(speed, b, blk, (uint16_t)k_rdh_xfer_size);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  /* DFU_ABORT closes the download (dfuDNLOAD-IDLE -> dfuIDLE) without the
   * manifest's wait-for-reset, so the UPLOAD phase runs on the same device. */
  const ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_rdh_bm_class_if_out,
    .b_request       = (uint8_t)k_rdh_breq_abort,
    .w_value         = 0U,
    .w_index         = (uint16_t)k_rdh_intf,
    .w_length        = 0U,
  };
  const ra8_err_t err = ra8_usb_host_control_xfer(speed, &setup, nullptr, 0U, nullptr);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_wait_state(speed, (uint8_t)k_rdh_state_idle);
}

/**
 * @brief Download the entire image in 64-byte blocks, then send a zero-length manifest.
 *
 * @details
 * Iterates over @p img in k_rdh_xfer_size (64)-byte blocks, calling
 * internal_dnload_block for each.  After the last data block the function
 * sends a zero-length DFU_DNLOAD (wValue = block count, wLength = 0), which
 * signals end-of-download per the DFU 1.1 specification.  The device commits
 * the slot header and enters dfuMANIFEST.  Unlike internal_download_all this
 * function does NOT issue DFU_ABORT and does NOT poll for dfuIDLE; the caller
 * is responsible for confirming the commit via ra8_dfu_device_committed because
 * the device half runs on the same chip and would deadlock if polled here.
 *
 * @param[in]  speed    Controller speed (FS or HS) selecting the USB port.
 * @param[in]  img      Pointer to the image buffer (non-NULL).
 * @param[in]  img_len  Total image length; must be a non-zero multiple of
 *                      k_rdh_xfer_size (64).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              All blocks and the zero-length manifest were accepted.
 * @retval k_ra8_err_hw_timeout  A block poll timed out before the manifest was sent.
 * @retval k_ra8_err_*           internal_dnload_block or manifest xfer failure.
 *
 * @pre The device is in dfuIDLE state (SET_CONFIGURATION completed).
 * @pre @p img_len is a non-zero multiple of k_rdh_xfer_size and @p img is valid.
 * @post On k_ra8_ok, the device has received all data blocks and the manifest request.
 * @post The device is entering dfuMANIFEST; the caller must not re-enumerate it
 *       until the commit is confirmed via ra8_dfu_device_committed.
 *
 * @note Not thread-safe; called only from the single-threaded polled sequence.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_download_manifest(ra8_usb_speed_t speed, const uint8_t* img, uint32_t img_len)
{
  const uint16_t blocks = (uint16_t)(img_len / (uint32_t)k_rdh_xfer_size);
  for (uint16_t b = 0U; b < blocks; b++) {
    uint8_t blk[k_rdh_xfer_size] = {};
    (void)memcpy(blk, &img[(uint32_t)b * (uint32_t)k_rdh_xfer_size], (size_t)k_rdh_xfer_size);
    const ra8_err_t err = internal_dnload_block(speed, b, blk, (uint16_t)k_rdh_xfer_size);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  /* Zero-length DFU_DNLOAD = end-of-download: the device commits the slot header
   * and enters dfuMANIFEST. This is the real flash flow (what dfu-util does),
   * versus internal_download_all's DFU_ABORT that the self-test round-trip uses
   * to keep the device enumerable for the UPLOAD comparison. The caller drives
   * the device half on the same chip, so it polls ::ra8_dfu_device_committed
   * rather than waiting on a manifest-state GETSTATUS here. */
  const ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_rdh_bm_class_if_out,
    .b_request       = (uint8_t)k_rdh_breq_dnload,
    .w_value         = blocks,
    .w_index         = (uint16_t)k_rdh_intf,
    .w_length        = 0U,
  };
  return ra8_usb_host_control_xfer(speed, &setup, nullptr, 0U, nullptr);
}

/**
 * @brief DFU_UPLOAD each block and byte-compare against @p img.
 *
 * @details
 * Iterates over each k_rdh_xfer_size (64)-byte block in @p img_len.  For
 * each block it issues a DFU class-specific UPLOAD request (bmRequestType=0xA1,
 * bRequest=0x02, wValue=block index, wLength=64) and validates that exactly
 * k_rdh_xfer_size bytes were returned.  It then calls memcmp to check the
 * received buffer against the corresponding region of @p img.  The function
 * increments out->blocks_ok for each passing block and records the first
 * failing block index in out->mismatch on a size or data error.  Processing
 * stops at the first error.
 *
 * @param[in]  speed    Controller speed (FS or HS) selecting the USB port.
 * @param[in]  img      Expected image bytes to compare against.
 * @param[in]  img_len  Total image length; must be a non-zero multiple of
 *                      k_rdh_xfer_size (64).
 * @param[out] out      Diagnostics structure; out->blocks_ok is updated on
 *                      each passing block; out->mismatch is set on first error.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                All blocks uploaded and byte-matched @p img.
 * @retval k_ra8_err_invalid_size  An upload block returned the wrong byte count.
 * @retval k_ra8_err_invalid_state An upload block's data differed from @p img.
 * @retval k_ra8_err_*             ra8_usb_host_control_xfer failure.
 *
 * @pre The device is in dfuIDLE state (DFU_ABORT or prior state transition done).
 * @pre @p img and @p out are non-NULL; @p img_len is a non-zero multiple of 64.
 * @post On k_ra8_ok, out->blocks_ok equals img_len / k_rdh_xfer_size.
 * @post On failure, out->mismatch holds the index of the first bad block.
 *
 * @note Not thread-safe; called only from the single-threaded polled sequence.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_upload_verify(ra8_usb_speed_t        speed,
                                                     const uint8_t*         img,
                                                     uint32_t               img_len,
                                                     ra8_dfu_host_result_t* out)
{
  const uint16_t blocks = (uint16_t)(img_len / (uint32_t)k_rdh_xfer_size);
  out->blocks_ok        = 0U;
  for (uint16_t b = 0U; b < blocks; b++) {
    uint8_t               got[k_rdh_xfer_size] = {};
    const ra8_usb_setup_t setup                = {
      .bm_request_type = (uint8_t)k_rdh_bm_class_if_in,
      .b_request       = (uint8_t)k_rdh_breq_upload,
      .w_value         = b,
      .w_index         = (uint16_t)k_rdh_intf,
      .w_length        = (uint16_t)k_rdh_xfer_size,
    };
    uint16_t        rx = 0U;
    const ra8_err_t err =
      ra8_usb_host_control_xfer(speed, &setup, got, (uint16_t)k_rdh_xfer_size, &rx);
    if (err != k_ra8_ok) {
      return err;
    }
    if (rx != (uint16_t)k_rdh_xfer_size) {
      out->mismatch = (uint32_t)b;
      return k_ra8_err_invalid_size;
    }
    if (memcmp(got, &img[(uint32_t)b * (uint32_t)k_rdh_xfer_size], (size_t)k_rdh_xfer_size) != 0) {
      out->mismatch = (uint32_t)b;
      return k_ra8_err_invalid_state;
    }
    out->blocks_ok++;
  }
  return k_ra8_ok;
}

/**
 * @brief Run the full self-test sequence: enumerate, download, abort, upload-verify.
 *
 * @details
 * Orchestrates the round-trip DFU self-test in five ordered steps:
 *   1. internal_enum_hunt -- wait for attach, bus reset, read DEVICE descriptor
 *      and populate out->pid from bytes k_rdh_off_dev_pid and +1.
 *   2. internal_set_address -- assign operating address k_rdh_dev_addr.
 *   3. internal_set_config -- activate the DFU interface (dfuIDLE).
 *   4. internal_download_all -- stream @p img to the device and DFU_ABORT.
 *   5. internal_upload_verify -- stream back all blocks and byte-compare.
 * Any step failure propagates immediately; the caller (ra8_dfu_host_run) tears
 * down the host controller on error.
 *
 * @param[in]  speed    Controller speed (FS or HS) selecting the USB port.
 * @param[in]  img      Image to download and then verify via upload.
 * @param[in]  img_len  Total image length; non-zero multiple of 64.
 * @param[out] out      Diagnostics structure; out->pid set after enumeration.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              All five steps completed successfully.
 * @retval k_ra8_err_hw_timeout  Attach or GETSTATUS wait exhausted.
 * @retval k_ra8_err_*           Error from any of the five sub-functions.
 *
 * @pre The USB host controller named by @p speed has been initialized via ra8_usb_host_init.
 * @pre @p img and @p out are non-NULL; @p img_len is a non-zero multiple of 64.
 * @post On k_ra8_ok, out->pid is set and out->blocks_ok equals img_len / 64.
 * @post On failure, the host controller is NOT torn down here; the caller handles it.
 *
 * @note Not thread-safe; called only from the single-threaded polled sequence.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_run_seq(ra8_usb_speed_t        speed,
                                               const uint8_t*         img,
                                               uint32_t               img_len,
                                               ra8_dfu_host_result_t* out)
{
  uint8_t   desc[k_rdh_dev_desc_len] = {};
  ra8_err_t err                      = internal_enum_hunt(speed, desc);
  if (err != k_ra8_ok) {
    return err;
  }
  out->pid = (uint32_t)desc[k_rdh_off_dev_pid] |
             ((uint32_t)desc[(uint32_t)k_rdh_off_dev_pid + 1U] << (uint32_t)k_rdh_byte_bits);
  err      = internal_set_address(speed);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_set_config(speed);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_download_all(speed, img, img_len);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_upload_verify(speed, img, img_len, out);
}

ra8_err_t ra8_dfu_host_run(ra8_usb_speed_t        host_speed,
                           const uint8_t*         img,
                           uint32_t               img_len,
                           ra8_dfu_host_result_t* out)
{
  if ((img == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  out->pid       = 0U;
  out->blocks_ok = 0U;
  out->mismatch  = (uint32_t)k_rdh_mismatch_none;
  out->last_err  = k_ra8_ok;
  if ((img_len == 0U) || ((img_len % (uint32_t)k_rdh_xfer_size) != 0U)) {
    out->last_err = k_ra8_err_invalid_arg;
    return k_ra8_err_invalid_arg;
  }
  ra8_err_t err = ra8_usb_host_init(host_speed);
  if (err != k_ra8_ok) {
    out->last_err = err;
    return err;
  }
  err           = internal_run_seq(host_speed, img, img_len, out);
  out->last_err = err;
  if (err != k_ra8_ok) {
    (void)ra8_usb_host_deinit(host_speed);
  }
  return err;
}

/**
 * @brief Run the real-flash sequence: enumerate, download, then send manifest.
 *
 * @details
 * Orchestrates the real DFU programming flow in four ordered steps:
 *   1. internal_enum_hunt -- wait for attach, bus reset, read DEVICE descriptor
 *      and populate out->pid from bytes k_rdh_off_dev_pid and +1.
 *   2. internal_set_address -- assign operating address k_rdh_dev_addr.
 *   3. internal_set_config -- activate the DFU interface (dfuIDLE).
 *   4. internal_download_manifest -- stream @p img to the device and send the
 *      zero-length manifest DFU_DNLOAD to trigger commit.
 * There is no ABORT or upload-verify step.  The device half (running on the
 * same chip) commits the slot and the caller confirms via ra8_dfu_device_committed.
 * Any step failure propagates immediately; the caller (ra8_dfu_host_program)
 * tears down the host controller on error.
 *
 * @param[in]  speed    Controller speed (FS or HS) selecting the USB port.
 * @param[in]  img      Bootable slot image to flash into the device.
 * @param[in]  img_len  Total image length; non-zero multiple of 64.
 * @param[out] out      Diagnostics structure; out->pid set after enumeration.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              All four steps completed; device is in dfuMANIFEST.
 * @retval k_ra8_err_hw_timeout  Attach or GETSTATUS wait exhausted.
 * @retval k_ra8_err_*           Error from any of the four sub-functions.
 *
 * @pre The USB host controller named by @p speed has been initialized via ra8_usb_host_init.
 * @pre @p img and @p out are non-NULL; @p img_len is a non-zero multiple of 64.
 * @post On k_ra8_ok, out->pid is set and the device has received the manifest request.
 * @post On failure, the host controller is NOT torn down here; the caller handles it.
 *
 * @note Not thread-safe; called only from the single-threaded polled sequence.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_program_seq(ra8_usb_speed_t        speed,
                                                   const uint8_t*         img,
                                                   uint32_t               img_len,
                                                   ra8_dfu_host_result_t* out)
{
  uint8_t   desc[k_rdh_dev_desc_len] = {};
  ra8_err_t err                      = internal_enum_hunt(speed, desc);
  if (err != k_ra8_ok) {
    return err;
  }
  out->pid = (uint32_t)desc[k_rdh_off_dev_pid] |
             ((uint32_t)desc[(uint32_t)k_rdh_off_dev_pid + 1U] << (uint32_t)k_rdh_byte_bits);
  err      = internal_set_address(speed);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_set_config(speed);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_download_manifest(speed, img, img_len);
}

ra8_err_t ra8_dfu_host_program(ra8_usb_speed_t        host_speed,
                               const uint8_t*         img,
                               uint32_t               img_len,
                               ra8_dfu_host_result_t* out)
{
  if ((img == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  out->pid       = 0U;
  out->blocks_ok = 0U;
  out->mismatch  = (uint32_t)k_rdh_mismatch_none;
  out->last_err  = k_ra8_ok;
  if ((img_len == 0U) || ((img_len % (uint32_t)k_rdh_xfer_size) != 0U)) {
    out->last_err = k_ra8_err_invalid_arg;
    return k_ra8_err_invalid_arg;
  }
  ra8_err_t err = ra8_usb_host_init(host_speed);
  if (err != k_ra8_ok) {
    out->last_err = err;
    return err;
  }
  err           = internal_program_seq(host_speed, img, img_len, out);
  out->last_err = err;
  if (err != k_ra8_ok) {
    (void)ra8_usb_host_deinit(host_speed);
  }
  return err;
}

#endif /* !RA8_OFF_TARGET */
