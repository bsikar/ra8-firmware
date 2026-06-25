/**
 * @file ra_usb_pmsc.c
 * @brief Native USB device-side MSC (Mass Storage Class) class layer
 *        implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Glues the device-mode bring-up paths in `ra_usb` to a USB Mass
 * Storage Class (Bulk-Only-Transport / BBB) function so the EK-RA8D2
 * appears as a USB drive on the host side. This file is the native
 * device-MSC class layer; FSP's `r_usb_pmsc_driver.c`,
 * `r_usb_pmsc.c`, and `r_media_driver_api.c` are reference material
 * only -- no FSP source is pulled in verbatim.
 *
 * Mapping vs FSP (FSP function -> our entry point):
 *
 *  - `usb_pmsc_init`          -> `ra_usb_pmsc_init`
 *  - `usb_pmsc_check_cbw`     -> `internal_check_cbw`
 *  - `usb_pmsc_setcsw`        -> `internal_setcsw`
 *  - `usb_pmsc_csw_transfer`  -> CSW_TX phase in `internal_pump`
 *  - `usb_pmsc_receive_cbw`   -> IDLE phase in `internal_pump`
 *  - `usb_pmsc_get_max_lun`   -> `internal_handle_get_max_lun`
 *  - `pmsc_atapi_command_processing` -> `internal_dispatch_scsi`
 *
 * The starter is single-LUN, single-instance. Get-Max-LUN returns 0;
 * Mass Storage Reset rewinds the state machine to IDLE.
 *
 * BOT (Bulk-Only Transport) state machine -- per command:
 *
 *   IDLE -> CBW_RX (pull 31-byte CBW) -> CDB_DECODE ->
 *   DATA_TX (push read data) | DATA_RX (pull write data) ->
 *   CSW_TX (push 13-byte CSW) -> IDLE
 *
 * On invalid CBW signature the spec mandates phase-error CSW, which
 * the starter emits by transitioning straight to CSW_TX with status
 * `k_ra_pmsc_csw_status_phase_error`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_usb_pmsc.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_usb.h"
#include "ra_usb_pmsc_internal.h"

static const char* s_tag = "USBPMSC";

/* =============================================================================
 * Internal constants
 * =============================================================================
 */

/**
 * @enum ra_usb_pmsc_cbw_offset_t
 * @brief Byte offsets inside the 31-byte CBW header.
 *
 * @details See USB MSC BBB rev 1.0 sec 5.1 Table 5.1 "Command Block
 * Wrapper".
 */
typedef enum : uint8_t {
  k_ra_pmsc_cbw_off_signature   = 0U,  /**< dCBWSignature [4].   */
  k_ra_pmsc_cbw_off_tag         = 4U,  /**< dCBWTag [4].         */
  k_ra_pmsc_cbw_off_data_length = 8U,  /**< dCBWDataTransferLen. */
  k_ra_pmsc_cbw_off_flags       = 12U, /**< bmCBWFlags.          */
  k_ra_pmsc_cbw_off_lun         = 13U, /**< bCBWLUN (low nib).   */
  k_ra_pmsc_cbw_off_cdb_length  = 14U, /**< bCBWCBLength (low 5).*/
  k_ra_pmsc_cbw_off_cdb         = 15U, /**< CBWCB [16].          */
} ra_usb_pmsc_cbw_offset_t;

/**
 * @enum ra_usb_pmsc_csw_offset_t
 * @brief Byte offsets inside the 13-byte CSW.
 *
 * @details See USB MSC BBB rev 1.0 sec 5.2 Table 5.2 "Command
 * Status Wrapper".
 */
typedef enum : uint8_t {
  k_ra_pmsc_csw_off_signature = 0U,  /**< dCSWSignature [4]. */
  k_ra_pmsc_csw_off_tag       = 4U,  /**< dCSWTag [4].       */
  k_ra_pmsc_csw_off_residue   = 8U,  /**< dCSWDataResidue.   */
  k_ra_pmsc_csw_off_status    = 12U, /**< bCSWStatus.        */
} ra_usb_pmsc_csw_offset_t;

/**
 * @enum ra_usb_pmsc_cbw_flag_t
 * @brief bmCBWFlags direction bit values.
 */
typedef enum : uint8_t {
  k_ra_pmsc_cbw_flag_data_out = 0x00U, /**< Host -> device.   */
  k_ra_pmsc_cbw_flag_data_in  = 0x80U, /**< Device -> host.   */
} ra_usb_pmsc_cbw_flag_t;

/**
 * @enum ra_usb_pmsc_signature_t
 * @brief BOT wrapper signatures (USB MSC BBB rev 1.0 sec 5).
 *
 * @details Stored on-wire little-endian. dCBWSignature 'USBC' is
 * 0x43425355; dCSWSignature 'USBS' is 0x53425355.
 */
typedef enum : uint32_t {
  k_ra_pmsc_cbw_signature = 0x43425355U, /**< 'USBC' little-endian. */
  k_ra_pmsc_csw_signature = 0x53425355U, /**< 'USBS' little-endian. */
} ra_usb_pmsc_signature_t;

/**
 * @enum ra_usb_pmsc_initial_tag_t
 * @brief Initial value of the cached BOT dCBWTag.
 */
typedef enum : uint32_t {
  k_ra_pmsc_initial_tag = 0U, /**< Reset value.                     */
} ra_usb_pmsc_initial_tag_t;

/**
 * @enum ra_usb_pmsc_lun_mask_t
 * @brief Field-width masks for bCBWLUN + bCBWCBLength bytes.
 *
 * @details See USB MSC BBB rev 1.0 sec 5.1 Table 5.1: bCBWLUN
 * occupies the low 4 bits; bCBWCBLength occupies the low 5 bits.
 */
typedef enum : uint8_t {
  k_ra_pmsc_lun_field_mask = 0x0FU, /**< Low 4 bits of bCBWLUN.    */
  k_ra_pmsc_cdb_field_mask = 0x1FU, /**< Low 5 bits of bCBWCBLen.  */
} ra_usb_pmsc_lun_mask_t;

/* =============================================================================
 * Internal state
 * =============================================================================
 */

ra_usb_pmsc_state_data_t s_state = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Pick the bulk-max-packet ceiling matching the negotiated
 *        speed.
 *
 * @details See implementation.
 * @param[in] speed See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint16_t internal_bulk_max_packet(ra_usb_speed_t speed)
{
  return (speed == k_ra_usb_speed_hs) ? k_ra_pmsc_bulk_max_packet_hs : k_ra_pmsc_bulk_max_packet_fs;
}

/**
 * @brief Configure the two device-MSC bulk pipes against the local
 *        endpoints.
 *
 * @details Mirrors FSP's `usb_pstd_pipe_table` writes condensed for
 * the MSC case. PIPE3 = bulk-IN, PIPE4 = bulk-OUT, matching the
 * host-MSC layer for symmetry.
 *
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_configure_pipes(void)
{
  const uint16_t bulk_mp = internal_bulk_max_packet(s_state.speed);

  ra_err_t err = ra_usb_configure_endpoint(s_state.speed,
                                           k_ra_pmsc_pipe_bulk_in,
                                           k_ra_pmsc_ep_bulk_in,
                                           k_ra_usb_ep_dir_in,
                                           k_ra_usb_ep_type_bulk,
                                           bulk_mp);
  RA_RETURN_ON_ERROR(err, s_tag, "pmsc: bulk-in cfg"); /* GCOVR_EXCL_BR_LINE */

  err = ra_usb_configure_endpoint(s_state.speed,
                                  k_ra_pmsc_pipe_bulk_out,
                                  k_ra_pmsc_ep_bulk_out,
                                  k_ra_usb_ep_dir_out,
                                  k_ra_usb_ep_type_bulk,
                                  bulk_mp);
  return err;
}

/**
 * @brief Pack a uint32 into 4 little-endian bytes.
 *
 * @details See implementation.
 * @param[in] value See implementation.
 * @param[in] dst See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_pack_u32_le(uint32_t value, uint8_t* dst)
{
  dst[0] = (uint8_t)((value >> k_ra_pmsc_shift_byte0) & k_ra_pmsc_byte_mask);
  dst[1] = (uint8_t)((value >> k_ra_pmsc_shift_byte1) & k_ra_pmsc_byte_mask);
  dst[2] = (uint8_t)((value >> k_ra_pmsc_shift_byte2) & k_ra_pmsc_byte_mask);
  dst[3] = (uint8_t)((value >> k_ra_pmsc_shift_byte3) & k_ra_pmsc_byte_mask);
}

/**
 * @brief Unpack a uint32 from 4 little-endian bytes.
 *
 * @details See implementation.
 * @param[in] src See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint32_t internal_unpack_u32_le(const uint8_t* src)
{
  return ((uint32_t)src[0] << k_ra_pmsc_shift_byte0) | ((uint32_t)src[1] << k_ra_pmsc_shift_byte1) |
         ((uint32_t)src[2] << k_ra_pmsc_shift_byte2) | ((uint32_t)src[3] << k_ra_pmsc_shift_byte3);
}

void internal_zero_bytes(uint8_t* dst, uint32_t len)
{
  for (uint32_t i = 0U; i < len; ++i) {
    dst[i] = 0U;
  }
}

/**
 * @brief Copy `len` bytes from `src` to `dst` byte-by-byte.
 *
 * @details See implementation.
 * @param[in] dst See implementation.
 * @param[in] src See implementation.
 * @param[in] len See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_copy_bytes(uint8_t* dst, const uint8_t* src, uint32_t len)
{
  for (uint32_t i = 0U; i < len; ++i) {
    dst[i] = src[i];
  }
}

/* =============================================================================
 * BOT state machine -- public entry points
 * =============================================================================
 */

ra_err_t ra_usb_pmsc_feed_cbw(const uint8_t* cbw)
{
  RA_CHECK_NULL_PTR(cbw, s_tag, "feed_cbw: cbw");
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  if (!s_state.storage_attached) {
    return k_ra_err_invalid_state;
  }

  /* USB MSC BBB rev 1.0 sec 6.2.1 "Valid CBW" -- signature must be
   * 'USBC' little-endian. */
  const uint32_t signature = internal_unpack_u32_le(&cbw[k_ra_pmsc_cbw_off_signature]);
  if (signature != k_ra_pmsc_cbw_signature) {
    /* Spec sec 6.6.1 "CBW Not Valid": stall both bulk pipes and
     * await reset recovery. The starter surfaces this as a CSW
     * with status `phase_error` so the caller's state machine can
     * proceed deterministically. */
    s_state.bot_state = k_ra_pmsc_state_csw_tx;
    s_state.cbw_tag   = internal_unpack_u32_le(&cbw[k_ra_pmsc_cbw_off_tag]);
    return k_ra_err_invalid_arg;
  }

  s_state.cbw_tag         = internal_unpack_u32_le(&cbw[k_ra_pmsc_cbw_off_tag]);
  s_state.cbw_data_length = internal_unpack_u32_le(&cbw[k_ra_pmsc_cbw_off_data_length]);
  s_state.cbw_dir_in      = (cbw[k_ra_pmsc_cbw_off_flags] & k_ra_pmsc_cbw_flag_data_in) != 0U;
  s_state.cbw_lun         = (uint8_t)(cbw[k_ra_pmsc_cbw_off_lun] & k_ra_pmsc_lun_field_mask);
  s_state.cbw_cdb_len     = (uint8_t)(cbw[k_ra_pmsc_cbw_off_cdb_length] & k_ra_pmsc_cdb_field_mask);
  internal_copy_bytes(s_state.cbw_cdb,
                      &cbw[k_ra_pmsc_cbw_off_cdb],
                      (uint32_t)k_ra_pmsc_cdb_max_len);
  s_state.bot_state     = k_ra_pmsc_state_cdb_decode;
  s_state.last_data_len = 0U;
  return k_ra_ok;
}

/**
 * @brief Dispatch the cached CDB to the right SCSI handler.
 *
 * @details Helper for `ra_usb_pmsc_dispatch_command`. Returns
 * `k_ra_ok` and a populated `*data_len` on a supported opcode;
 * sets `*csw_status` to `failed` on an unsupported opcode and
 * leaves `*data_len` zero.
 *
 * @param[in] data_buf See implementation.
 * @param[in] data_buf_capacity See implementation.
 * @param[in] data_len See implementation.
 * @param[in] csw_status See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_dispatch_scsi(uint8_t*                  data_buf,
                                       uint32_t                  data_buf_capacity,
                                       uint32_t*                 data_len,
                                       ra_usb_pmsc_csw_status_t* csw_status)
{
  const uint8_t opcode = s_state.cbw_cdb[k_ra_pmsc_cdb_off_opcode];
  switch (opcode) {
    case k_ra_pmsc_scsi_test_unit_ready:
      /* Passing -- no data phase. */
      return k_ra_ok;
    case k_ra_pmsc_scsi_inquiry:
      return internal_handle_inquiry(data_buf, data_buf_capacity, data_len);
    case k_ra_pmsc_scsi_read_capacity_10:
      return internal_handle_read_capacity(data_buf, data_buf_capacity, data_len);
    case k_ra_pmsc_scsi_request_sense:
      return internal_handle_request_sense(data_buf, data_buf_capacity, data_len);
    case k_ra_pmsc_scsi_mode_sense_6:
      return internal_handle_mode_sense(data_buf, data_buf_capacity, data_len);
    case k_ra_pmsc_scsi_read_10:
      return internal_handle_read10(data_buf, data_buf_capacity, data_len);
    case k_ra_pmsc_scsi_write_10:
      return internal_handle_write10(data_buf, data_len);
    default:
      /* Unsupported opcode -> CSW status FAILED, no data. */
      *csw_status = k_ra_pmsc_csw_status_failed;
      return k_ra_ok;
  }
}

/**
 * @brief Validate `dispatch_command` preconditions (driver state +
 *        bot phase + buffer capacity).
 *
 * @details See implementation.
 * @param[in] data_buf_capacity See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_dispatch_preconditions(uint32_t data_buf_capacity)
{
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  if (!s_state.storage_attached) {
    return k_ra_err_invalid_state;
  }
  if (s_state.bot_state != k_ra_pmsc_state_cdb_decode) {
    return k_ra_err_invalid_state;
  }
  if (data_buf_capacity == 0U) {
    return k_ra_err_invalid_size;
  }
  return k_ra_ok;
}

/**
 * @brief Advance BOT state after a successful CDB dispatch.
 *
 * @details See implementation.
 * @param[in] data_len See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_dispatch_advance_state(uint32_t data_len)
{
  s_state.last_data_len = data_len;
  if (data_len > 0U) {
    s_state.bot_state = s_state.cbw_dir_in ? k_ra_pmsc_state_data_tx : k_ra_pmsc_state_data_rx;
  } else {
    s_state.bot_state = k_ra_pmsc_state_csw_tx;
  }
}

ra_err_t ra_usb_pmsc_dispatch_command(uint8_t*                  data_buf,
                                      uint32_t                  data_buf_capacity,
                                      uint32_t*                 data_len,
                                      ra_usb_pmsc_csw_status_t* csw_status)
{
  RA_CHECK_NULL_PTR(data_buf, s_tag, "dispatch: data_buf");
  RA_CHECK_NULL_PTR(data_len, s_tag, "dispatch: data_len");
  RA_CHECK_NULL_PTR(csw_status, s_tag, "dispatch: csw_status");
  const ra_err_t pre_err = internal_dispatch_preconditions(data_buf_capacity);
  if (pre_err != k_ra_ok) {
    return pre_err;
  }

  *data_len          = 0U;
  *csw_status        = k_ra_pmsc_csw_status_passed;
  const ra_err_t err = internal_dispatch_scsi(data_buf, data_buf_capacity, data_len, csw_status);
  if (err != k_ra_ok) {
    *csw_status = k_ra_pmsc_csw_status_failed;
    *data_len   = 0U;
  }
  internal_dispatch_advance_state(*data_len);
  return k_ra_ok;
}

ra_err_t
ra_usb_pmsc_build_csw(ra_usb_pmsc_csw_status_t csw_status, uint32_t residue, uint8_t* out_csw)
{
  RA_CHECK_NULL_PTR(out_csw, s_tag, "build_csw: out_csw");
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }

  internal_zero_bytes(out_csw, (uint32_t)k_ra_pmsc_csw_len);
  internal_pack_u32_le(k_ra_pmsc_csw_signature, &out_csw[k_ra_pmsc_csw_off_signature]);
  internal_pack_u32_le(s_state.cbw_tag, &out_csw[k_ra_pmsc_csw_off_tag]);
  internal_pack_u32_le(residue, &out_csw[k_ra_pmsc_csw_off_residue]);
  out_csw[k_ra_pmsc_csw_off_status] = (uint8_t)csw_status;
  s_state.bot_state                 = k_ra_pmsc_state_idle;
  return k_ra_ok;
}

ra_err_t ra_usb_pmsc_step(void)
{
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  if (!s_state.storage_attached) {
    return k_ra_err_invalid_state;
  }

  /* Each invocation advances by exactly one phase. The IDLE phase
   * is observable through the public state; in production the
   * caller pumps `step` from the bulk-OUT-completion ISR until the
   * machine returns to IDLE waiting for the next CBW. The CBW_RX
   * and CDB_DECODE phases are caller-driven: the application drains
   * the bulk-OUT FIFO and invokes `ra_usb_pmsc_feed_cbw` /
   * `ra_usb_pmsc_dispatch_command` directly, so `step` leaves
   * those phases untouched. */
  switch (s_state.bot_state) {
    case k_ra_pmsc_state_idle:
      /* Awaiting CBW. */
      s_state.bot_state = k_ra_pmsc_state_cbw_rx;
      break;
    case k_ra_pmsc_state_cbw_rx:
    case k_ra_pmsc_state_cdb_decode:
      /* Caller drives the CBW capture / CDB dispatch directly. */
      break;
    case k_ra_pmsc_state_data_tx:
    case k_ra_pmsc_state_data_rx:
      /* Caller pushes / pulls the data phase; transition to CSW. */
      s_state.bot_state = k_ra_pmsc_state_csw_tx;
      break;
    case k_ra_pmsc_state_csw_tx:
    default:
      /* CSW sent (or unknown state); rewind to IDLE. */
      s_state.bot_state = k_ra_pmsc_state_idle;
      break;
  }
  return k_ra_ok;
}

/* =============================================================================
 * Storage backend
 * =============================================================================
 */

ra_err_t ra_usb_pmsc_attach_storage(const ra_usb_pmsc_storage_t* storage)
{
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  RA_CHECK_NULL_PTR(storage, s_tag, "attach_storage: storage");
  RA_CHECK_NULL_PTR(storage->read_block, s_tag, "attach_storage: read_block");
  RA_CHECK_NULL_PTR(storage->write_block, s_tag, "attach_storage: write_block");
  RA_CHECK_NULL_PTR(storage->get_capacity, s_tag, "attach_storage: get_capacity");
  RA_CHECK_NULL_PTR(storage->get_inquiry, s_tag, "attach_storage: get_inquiry");

  s_state.storage          = *storage;
  s_state.storage_attached = true;
  s_state.bot_state        = k_ra_pmsc_state_idle;
  ra_log_info(s_tag, "storage attached");
  return k_ra_ok;
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

ra_err_t ra_usb_pmsc_init(ra_usb_speed_t speed)
{
  if ((speed != k_ra_usb_speed_fs) && (speed != k_ra_usb_speed_hs)) {
    return k_ra_err_invalid_arg;
  }
  const ra_err_t usb_err = ra_usb_device_init(speed);
  if (usb_err != k_ra_ok) {
    ra_log_error_val(s_tag, "ra_usb_device_init failed", (uint32_t)usb_err);
    return k_ra_err_hw_init_failed;
  }

  s_state.speed            = speed;
  s_state.bot_state        = k_ra_pmsc_state_idle;
  s_state.storage_attached = false;
  s_state.storage          = (ra_usb_pmsc_storage_t){};
  s_state.cbw_tag          = k_ra_pmsc_initial_tag;
  s_state.cbw_data_length  = 0U;
  s_state.cbw_dir_in       = false;
  s_state.cbw_lun          = 0U;
  s_state.cbw_cdb_len      = 0U;
  s_state.last_data_len    = 0U;
  internal_zero_bytes(s_state.cbw_cdb, (uint32_t)k_ra_pmsc_cdb_max_len);
  s_state.initialized = true;

  const ra_err_t pipes_err = internal_configure_pipes();
  if (pipes_err != k_ra_ok) {
    (void)ra_usb_device_deinit(speed);
    s_state.initialized = false;
    return pipes_err;
  }

  ra_log_info_val(s_tag, "device-MSC ready", (uint32_t)speed);
  return k_ra_ok;
}

ra_err_t ra_usb_pmsc_close(void)
{
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  /* Drop D+ pull-up so the host sees a clean detach. */
  (void)ra_usb_device_attach(s_state.speed, false);
  const ra_err_t err       = ra_usb_device_deinit(s_state.speed);
  s_state.initialized      = false;
  s_state.storage_attached = false;
  s_state.bot_state        = k_ra_pmsc_state_idle;
  return err;
}
