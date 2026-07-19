/**
 * @file ra8_ble_l2cap_advertise.c
 * @brief GAP legacy-advertising bring-up for the BLE host stack
 *
 * @details
 * Implements the host-side GAP advertising path split out of the
 * L2CAP core (ra8_ble_l2cap.c) to keep each TU under the file-size cap:
 *
 *   - Input validation (role / nullness / length / interval).
 *   - HCI LE_Set_Advertising_Parameters (Vol 4 Part E 7.8.5).
 *   - HCI LE_Set_Scan_Response_Data (Vol 4 Part E 7.8.8).
 *   - The public ra8_ble_host_advertise_start / _stop entry points that
 *     drive the controller bring-up sequence (Vol 6 Part B 4.4.2).
 *
 * Shares the singleton host-state struct (``s_ble_host_state``) and the
 * little-endian packer (``internal_pack_le16``) with the L2CAP core
 * via ra8_ble_host_internal.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_ble.h"
#include "ra8_ble_host.h"
#include "ra8_ble_host_internal.h"
#include "ra8_err.h"

/**
 * @brief Validate ra8_ble_host_advertise_start inputs.
 *
 * @details Centralizes the role / nullness / length / interval checks.
 *
 * @param[in] adv_data       Advertising-data buffer (may be NULL).
 * @param[in] adv_data_len   Advertising-data length.
 * @param[in] scan_resp      Scan-response buffer (may be NULL).
 * @param[in] scan_resp_len  Scan-response length.
 * @param[in] interval_ms    Advertising interval in milliseconds.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                   All inputs valid.
 * @retval k_ra8_err_not_initialized  Stack not yet initialized.
 * @retval k_ra8_err_invalid_arg      Bad role / length / interval.
 * @retval k_ra8_err_null_ptr         Length > 0 with NULL buffer.
 *
 * @pre None (operates only on inputs + module state).
 * @pre Module state field s_ble_host_state is well-defined.
 * @post No state mutation.
 * @post Return code reflects only input validation.
 *
 * @note Not thread-safe; called from the application thread.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_advertise_validate(const uint8_t* adv_data,
                                             uint8_t        adv_data_len,
                                             const uint8_t* scan_resp,
                                             uint8_t        scan_resp_len,
                                             uint16_t       interval_ms)
{
  enum : uint16_t {
    k_min_interval_ms = 20U,    /**< Minimum interval ms. */
    k_max_interval_ms = 10240U, /**< Maximum interval ms. */
  };

  if (s_ble_host_state.initialized == 0U) {
    return k_ra8_err_not_initialized;
  }
  if ((s_ble_host_state.role != k_ra8_ble_host_role_peripheral) &&
      (s_ble_host_state.role != k_ra8_ble_host_role_broadcaster)) {
    return k_ra8_err_invalid_arg;
  }
  if ((adv_data == nullptr) && (adv_data_len > 0U)) {
    return k_ra8_err_null_ptr;
  }
  if ((scan_resp == nullptr) && (scan_resp_len > 0U)) {
    return k_ra8_err_null_ptr;
  }
  if ((adv_data_len > k_ra8_ble_adv_data_max) || (scan_resp_len > k_ra8_ble_adv_data_max)) {
    return k_ra8_err_invalid_arg;
  }
  if ((interval_ms < k_min_interval_ms) || (interval_ms > k_max_interval_ms)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Issue HCI LE_Set_Advertising_Parameters for the given interval.
 *
 * @details Vol 4 Part E 7.8.5. All non-interval fields stay at default
 *          0x00 (ADV_IND, public address, no peer filter); channel map
 *          set to all primary channels (0x07).
 *
 * @param[in] interval_ms Advertising interval in milliseconds.
 *
 * @return ra8_err_t Whatever ra8_ble_hci_send_command returns.
 * @retval k_ra8_ok    HCI command sent.
 * @retval other      HCI failure.
 *
 * @pre Stack initialized.
 * @pre 20 <= interval_ms <= 10240 (validated upstream).
 * @post On success the controller's adv-params have been programmed.
 * @post On failure no state is mutated.
 *
 * @note Not thread-safe; called from the application thread.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_set_adv_params(uint16_t interval_ms)
{
  enum : uint16_t {
    /* 0.625 ms units per Vol 4 Part E 7.8.5. interval = ms * 1000 / 625 = ms*8/5. */
    k_us625_per_ms_num     = 8U,      /**< Us625 per ms number.  */
    k_us625_per_ms_den     = 5U,      /**< Us625 per ms den.     */
    k_op_le_set_adv_params = 0x2006U, /**< Op le set adv params. */
  };
  enum : uint8_t {
    k_adv_params_param_byte = 15U,   /**< Adv params param byte. */
    k_offset_chan_map       = 13U,   /**< Offset chan map.       */
    k_chan_map_all          = 0x07U, /**< Chan map all.          */
  };
  const uint16_t interval_625us =
    (uint16_t)((uint32_t)interval_ms * (uint32_t)k_us625_per_ms_num / (uint32_t)k_us625_per_ms_den);
  uint8_t adv_params[k_adv_params_param_byte] = {};
  internal_pack_le16(&adv_params[0], interval_625us);
  internal_pack_le16(&adv_params[2], interval_625us);
  adv_params[k_offset_chan_map] = k_chan_map_all;
  return ra8_ble_hci_send_command(k_op_le_set_adv_params, adv_params, k_adv_params_param_byte);
}

/**
 * @brief Issue HCI LE_Set_Scan_Response_Data (Vol 4 Part E 7.8.8).
 *
 * @details Packs the caller's scan-response payload into the fixed
 *          32-byte HCI parameter block (length octet followed by up to
 *          31 data bytes per Bluetooth Core 5.3 Vol 4 Part E 7.8.8) and
 *          forwards it to the controller through ra8_ble_hci_send_command.
 *          The advertise-start state machine only calls this helper
 *          when scan_resp_len > 0, so a zero-length payload is not a
 *          concern here.
 *
 * @param[in] scan_resp     Scan-response payload bytes.
 * @param[in] scan_resp_len Payload length (<= k_ra8_ble_adv_data_max).
 *
 * @return ra8_err_t Whatever ra8_ble_hci_send_command returns.
 * @retval k_ra8_ok HCI command sent.
 * @retval other   HCI failure.
 *
 * @pre scan_resp != NULL when scan_resp_len > 0.
 * @pre scan_resp_len <= k_ra8_ble_adv_data_max.
 * @post On success the controller's scan-response data has been set.
 * @post On failure no state is mutated.
 *
 * @note Not thread-safe; called from the application thread.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_set_scan_response(const uint8_t* scan_resp, uint8_t scan_resp_len)
{
  enum : uint16_t {
    k_op_le_set_scan_resp_data = 0x2009U, /**< Op le set scan resp data. */
  };
  enum : uint8_t {
    k_scan_resp_param_byte = 32U, /**< Scan resp param byte. */
  };
  uint8_t sr[k_scan_resp_param_byte] = {};
  sr[0]                              = scan_resp_len;
  (void)memcpy(&sr[1], scan_resp, scan_resp_len);
  return ra8_ble_hci_send_command(k_op_le_set_scan_resp_data, sr, k_scan_resp_param_byte);
}

/**
 * @brief Begin LE legacy connectable advertising.
 *
 * @details Drives the controller bring-up sequence required to start
 *          advertising on the primary 3-channel set
 *          (Bluetooth Core 5.3 Vol 6 Part B 4.4.2):
 *          1. validate caller inputs,
 *          2. HCI LE_Set_Advertising_Parameters with the requested
 *             interval converted to 0.625 ms units,
 *          3. HCI LE_Set_Advertising_Data with the AD payload,
 *          4. HCI LE_Set_Scan_Response_Data when a scan response is
 *             supplied,
 *          5. HCI LE_Set_Advertising_Enable(1).
 *          Any failure in steps 2-5 short-circuits and propagates the
 *          underlying HCI error.
 *
 * @param[in] adv_data       Advertising-data payload bytes (may be NULL
 *                           when adv_data_len == 0).
 * @param[in] adv_data_len   Advertising-data length in bytes
 *                           (0..k_ra8_ble_adv_data_max).
 * @param[in] scan_resp      Scan-response payload bytes (may be NULL
 *                           when scan_resp_len == 0).
 * @param[in] scan_resp_len  Scan-response length in bytes
 *                           (0..k_ra8_ble_adv_data_max).
 * @param[in] interval_ms    Advertising interval in milliseconds
 *                           (Vol 6 Part B 4.4.2.2 valid range).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                   Advertising enabled.
 * @retval k_ra8_err_null_ptr         adv_data or scan_resp NULL with non-zero length.
 * @retval k_ra8_err_not_initialized  Stack not yet initialized.
 * @retval k_ra8_err_invalid_arg      Length or interval is out of range.
 * @retval other                     Underlying HCI command failure.
 *
 * @pre ra8_ble_host_init has succeeded.
 * @pre Caller is single-threaded.
 * @post On success the controller is broadcasting on the primary set
 *       with the supplied AD and (when present) scan-response payload.
 * @post On failure no host bookkeeping is mutated; the controller may
 *       be in a partially configured state but advertising is not
 *       enabled.
 *
 * @note Not thread-safe; called from the application thread.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_ble_host_advertise_start(const uint8_t* adv_data,
                                       uint8_t        adv_data_len,
                                       const uint8_t* scan_resp,
                                       uint8_t        scan_resp_len,
                                       uint16_t       interval_ms)
{
  ra8_err_t rc =
    internal_advertise_validate(adv_data, adv_data_len, scan_resp, scan_resp_len, interval_ms);
  if (rc != k_ra8_ok) {
    return rc;
  }

  rc = internal_set_adv_params(interval_ms);
  if (rc != k_ra8_ok) {
    return rc;
  }

  rc = ra8_ble_set_advertising_data(adv_data, adv_data_len);
  if (rc != k_ra8_ok) {
    return rc;
  }

  if (scan_resp_len > 0U) {
    rc = internal_set_scan_response(scan_resp, scan_resp_len);
    if (rc != k_ra8_ok) {
      return rc;
    }
  }

  return ra8_ble_set_advertising_enable(1U);
}

/**
 * @brief Stop advertising.
 *
 * @details Issues HCI LE_Set_Advertising_Enable(0)
 *          (Bluetooth Core 5.3 Vol 4 Part E 7.8.9).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                   Advertising disabled.
 * @retval k_ra8_err_not_initialized  Stack not yet initialized.
 * @retval other                     Underlying HCI error.
 *
 * @pre ra8_ble_host_init has succeeded.
 * @pre Caller is single-threaded.
 * @post Controller is no longer advertising.
 * @post No host bookkeeping is mutated beyond the controller state.
 *
 * @note Not thread-safe; called from the application thread.
 *
 * @since 0.1.0
 */
ra8_err_t ra8_ble_host_advertise_stop(void)
{
  if (s_ble_host_state.initialized == 0U) {
    return k_ra8_err_not_initialized;
  }
  return ra8_ble_set_advertising_enable(0U);
}
