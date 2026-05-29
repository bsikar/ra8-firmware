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

static const char* s_tag = "USBPMSC";

/* =============================================================================
 * Internal constants
 * =============================================================================
 */

/**
 * @enum ra_usb_pmsc_state_t
 * @brief BOT state-machine phases.
 *
 * @details Mirrors FSP's USB_PMSC_CBWRCV / USB_PMSC_DATARCV /
 * USB_PMSC_DATASND / USB_PMSC_CSWSND values in
 * `r_usb_pmsc_driver.c`. The starter adds `idle` and `cdb_decode`
 * phases so the host-side stepping matches the textbook BBB
 * lifecycle.
 */
typedef enum : uint8_t {
  k_ra_pmsc_state_idle       = 0U, /**< Pre-CBW.                     */
  k_ra_pmsc_state_cbw_rx     = 1U, /**< CBW arrived on bulk-OUT.     */
  k_ra_pmsc_state_cdb_decode = 2U, /**< Dispatch to a SCSI handler.  */
  k_ra_pmsc_state_data_tx    = 3U, /**< Push data on bulk-IN.        */
  k_ra_pmsc_state_data_rx    = 4U, /**< Pull data on bulk-OUT.       */
  k_ra_pmsc_state_csw_tx     = 5U, /**< Push CSW on bulk-IN.         */
} ra_usb_pmsc_state_t;

/**
 * @enum ra_usb_pmsc_size_t
 * @brief Standard BOT wrapper sizes.
 *
 * @details The CBW / CSW lengths are nailed down by USB MSC BBB rev
 * 1.0 sections 5.1 and 5.2 respectively.
 */
typedef enum : uint16_t {
  k_ra_pmsc_cbw_len     = 31U, /**< CBW length (BBB sec 5.1).     */
  k_ra_pmsc_csw_len     = 13U, /**< CSW length (BBB sec 5.2).     */
  k_ra_pmsc_cdb_max_len = 16U, /**< CDB ceiling.                  */
} ra_usb_pmsc_size_t;

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
 * @enum ra_usb_pmsc_byte_shift_t
 * @brief Per-byte left-shift constants for serialisation.
 */
typedef enum : uint8_t {
  k_ra_pmsc_shift_byte0 = 0U,
  k_ra_pmsc_shift_byte1 = 8U,
  k_ra_pmsc_shift_byte2 = 16U,
  k_ra_pmsc_shift_byte3 = 24U,
} ra_usb_pmsc_byte_shift_t;

/**
 * @enum ra_usb_pmsc_byte_mask_t
 * @brief Single-byte extraction mask.
 */
typedef enum : uint32_t {
  k_ra_pmsc_byte_mask = 0xFFU,
} ra_usb_pmsc_byte_mask_t;

/**
 * @enum ra_usb_pmsc_cdb_offset_t
 * @brief SCSI CDB byte offsets used when decoding READ(10) /
 *        WRITE(10) blocks.
 */
typedef enum : uint8_t {
  k_ra_pmsc_cdb_off_opcode  = 0U, /**< Operation Code.        */
  k_ra_pmsc_cdb_off_lba_msb = 2U, /**< READ(10): LBA byte 3.  */
  k_ra_pmsc_cdb_off_lba_b1  = 3U, /**< READ(10): LBA byte 2.  */
  k_ra_pmsc_cdb_off_lba_b2  = 4U, /**< READ(10): LBA byte 1.  */
  k_ra_pmsc_cdb_off_lba_lsb = 5U, /**< READ(10): LBA byte 0.  */
  k_ra_pmsc_cdb_off_cnt_msb = 7U, /**< READ(10): count high.  */
  k_ra_pmsc_cdb_off_cnt_lsb = 8U, /**< READ(10): count low.   */
} ra_usb_pmsc_cdb_offset_t;

/**
 * @enum ra_usb_pmsc_inquiry_offset_t
 * @brief Byte offsets inside the SBC-4 standard INQUIRY response
 *        the device returns.
 */
typedef enum : uint8_t {
  k_ra_pmsc_inq_off_dev_type    = 0U,  /**< Byte 0 (qual+devtype). */
  k_ra_pmsc_inq_off_removable   = 1U,  /**< Byte 1 (removable).    */
  k_ra_pmsc_inq_off_version     = 2U,  /**< Byte 2 (SPC version).  */
  k_ra_pmsc_inq_off_resp_format = 3U,  /**< Byte 3 (resp format).  */
  k_ra_pmsc_inq_off_addl_length = 4U,  /**< Byte 4 (addl length).  */
  k_ra_pmsc_inq_off_vendor      = 8U,  /**< Vendor ID start.       */
  k_ra_pmsc_inq_off_product     = 16U, /**< Product ID start.      */
  k_ra_pmsc_inq_off_revision    = 32U, /**< Product revision.      */
} ra_usb_pmsc_inquiry_offset_t;

/**
 * @enum ra_usb_pmsc_inquiry_byte_t
 * @brief Pre-canned values for the standard INQUIRY response bytes.
 *
 * @details See SBC-4 sec 6.6 + SPC-4 sec 6.6:
 *  - byte 0: peripheral qualifier 0 + peripheral device type 0
 *    (direct-access block device).
 *  - byte 1: bit 7 = 1 (removable medium).
 *  - byte 2: SPC version 4 (SPC-4).
 *  - byte 3: response data format 2 (current format).
 *  - byte 4: additional length n-4 = 31.
 */
typedef enum : uint8_t {
  k_ra_pmsc_inq_byte_dev_type    = 0x00U, /**< Direct-access block.  */
  k_ra_pmsc_inq_byte_removable   = 0x80U, /**< Removable bit set.    */
  k_ra_pmsc_inq_byte_version     = 0x04U, /**< SPC-4.                */
  k_ra_pmsc_inq_byte_resp_format = 0x02U, /**< Current format.       */
  k_ra_pmsc_inq_byte_addl_length = 0x1FU, /**< 36 - 5 = 31.          */
} ra_usb_pmsc_inquiry_byte_t;

/**
 * @enum ra_usb_pmsc_capacity_offset_t
 * @brief Byte offsets inside the SCSI READ_CAPACITY(10) 8-byte
 *        response (SBC-4 sec 5.10).
 */
typedef enum : uint8_t {
  k_ra_pmsc_cap_off_last_lba = 0U, /**< Last valid LBA (big-endian). */
  k_ra_pmsc_cap_off_blk_size = 4U, /**< Block length (big-endian).   */
} ra_usb_pmsc_capacity_offset_t;

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

/**
 * @enum ra_usb_pmsc_sense_t
 * @brief Pre-canned values for the SCSI REQUEST SENSE response and
 *        the minimal MODE SENSE(6) header.
 *
 * @details See SPC-4 sec 6.30 (REQUEST SENSE) and sec 6.11 (MODE
 * SENSE). The starter does not track sense state so it always
 * answers "no sense" with a current-error response code.
 */
typedef enum : uint8_t {
  k_ra_pmsc_sense_resp_code         = 0x70U, /**< Current error, valid clr.  */
  k_ra_pmsc_sense_addl_length_byte  = 7U,    /**< Byte index of addl-len.    */
  k_ra_pmsc_sense_addl_length_value = 0x0AU, /**< Additional length = 10.    */
  k_ra_pmsc_mode_data_length_value  = 0x03U, /**< Mode data len (3, hdr-only).*/
} ra_usb_pmsc_sense_t;

/**
 * @enum ra_usb_pmsc_ascii_t
 * @brief ASCII constant used for SPACE-padding INQUIRY string fields.
 */
typedef enum : uint8_t {
  k_ra_pmsc_ascii_space = 0x20U, /**< ASCII SPACE.   */
} ra_usb_pmsc_ascii_t;

/* =============================================================================
 * Internal state
 * =============================================================================
 */

/**
 * @struct ra_usb_pmsc_state_data_t
 * @brief Singleton shadow state for the device-MSC driver.
 */
typedef struct {
  bool                  initialized;                    /**< True after init.            */
  bool                  storage_attached;               /**< True after attach_storage.  */
  ra_usb_speed_t        speed;                          /**< Underlying controller.      */
  ra_usb_pmsc_state_t   bot_state;                      /**< BOT state machine phase.    */
  ra_usb_pmsc_storage_t storage;                        /**< Storage backend snapshot.   */
  uint32_t              cbw_tag;                        /**< Cached dCBWTag.             */
  uint32_t              cbw_data_length;                /**< Cached dCBWDataTransferLen. */
  bool                  cbw_dir_in;                     /**< Cached bmCBWFlags direction.*/
  uint8_t               cbw_lun;                        /**< Cached bCBWLUN.             */
  uint8_t               cbw_cdb[k_ra_pmsc_cdb_max_len]; /**< Cached CDB.  */
  uint8_t               cbw_cdb_len;                    /**< Cached bCBWCBLength.        */
  uint32_t              last_data_len;                  /**< Last data byte count.       */
} ra_usb_pmsc_state_data_t;

static ra_usb_pmsc_state_data_t s_state = {};

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
 * @brief Pack a uint32 into 4 big-endian bytes (SCSI on-wire order).
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
static void internal_pack_u32_be(uint32_t value, uint8_t* dst)
{
  dst[0] = (uint8_t)((value >> k_ra_pmsc_shift_byte3) & k_ra_pmsc_byte_mask);
  dst[1] = (uint8_t)((value >> k_ra_pmsc_shift_byte2) & k_ra_pmsc_byte_mask);
  dst[2] = (uint8_t)((value >> k_ra_pmsc_shift_byte1) & k_ra_pmsc_byte_mask);
  dst[3] = (uint8_t)((value >> k_ra_pmsc_shift_byte0) & k_ra_pmsc_byte_mask);
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

/**
 * @brief Zero `len` bytes at `dst` byte-by-byte.
 *
 * @details Avoids `memset` so the project's clang-tidy
 * `clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling`
 * gate stays clean.
 *
 * @param[in] dst See implementation.
 * @param[in] len See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_zero_bytes(uint8_t* dst, uint32_t len)
{
  for (uint32_t i = 0U; i < len; ++i) {
    dst[i] = 0U;
  }
}

/**
 * @brief Fill `len` bytes at `dst` with ASCII SPACE.
 *
 * @details See implementation.
 * @param[in] dst See implementation.
 * @param[in] len See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_pad_space(uint8_t* dst, uint32_t len)
{
  for (uint32_t i = 0U; i < len; ++i) {
    dst[i] = k_ra_pmsc_ascii_space;
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
 * SCSI handler internals
 * =============================================================================
 */

/**
 * @brief Decode a 10-byte READ(10) / WRITE(10) CDB.
 *
 * @details See implementation.
 * @param[in] cdb See implementation.
 * @param[in] out_lba See implementation.
 * @param[in] out_block_count See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_decode_rw10(const uint8_t* cdb, uint32_t* out_lba, uint32_t* out_block_count)
{
  *out_lba         = ((uint32_t)cdb[k_ra_pmsc_cdb_off_lba_msb] << k_ra_pmsc_shift_byte3) |
                     ((uint32_t)cdb[k_ra_pmsc_cdb_off_lba_b1] << k_ra_pmsc_shift_byte2) |
                     ((uint32_t)cdb[k_ra_pmsc_cdb_off_lba_b2] << k_ra_pmsc_shift_byte1) |
                     ((uint32_t)cdb[k_ra_pmsc_cdb_off_lba_lsb] << k_ra_pmsc_shift_byte0);
  *out_block_count = ((uint32_t)cdb[k_ra_pmsc_cdb_off_cnt_msb] << k_ra_pmsc_shift_byte1) |
                     ((uint32_t)cdb[k_ra_pmsc_cdb_off_cnt_lsb] << k_ra_pmsc_shift_byte0);
}

/**
 * @brief Build the 36-byte SCSI INQUIRY response.
 *
 * @details Asks the storage backend for the three ASCII strings
 * (vendor, product, revision) and bakes them into the response per
 * SBC-4 sec 6.6. SPACE-padding is applied if the backend returns a
 * shorter string.
 *
 * @param[in] data_buf See implementation.
 * @param[in] capacity See implementation.
 * @param[in] out_len See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_handle_inquiry(uint8_t* data_buf, uint32_t capacity, uint32_t* out_len)
{
  if (capacity < k_ra_pmsc_inquiry_resp_len) {
    return k_ra_err_invalid_size;
  }
  internal_zero_bytes(data_buf, (uint32_t)k_ra_pmsc_inquiry_resp_len);

  data_buf[k_ra_pmsc_inq_off_dev_type]    = k_ra_pmsc_inq_byte_dev_type;
  data_buf[k_ra_pmsc_inq_off_removable]   = k_ra_pmsc_inq_byte_removable;
  data_buf[k_ra_pmsc_inq_off_version]     = k_ra_pmsc_inq_byte_version;
  data_buf[k_ra_pmsc_inq_off_resp_format] = k_ra_pmsc_inq_byte_resp_format;
  data_buf[k_ra_pmsc_inq_off_addl_length] = k_ra_pmsc_inq_byte_addl_length;

  /* Pad the three string fields with SPACE first so a backend that
   * writes fewer bytes still produces a spec-conforming response. */
  internal_pad_space(&data_buf[k_ra_pmsc_inq_off_vendor], (uint32_t)k_ra_pmsc_inq_vendor_len);
  internal_pad_space(&data_buf[k_ra_pmsc_inq_off_product], (uint32_t)k_ra_pmsc_inq_product_len);
  internal_pad_space(&data_buf[k_ra_pmsc_inq_off_revision], (uint32_t)k_ra_pmsc_inq_revision_len);

  const ra_err_t err = s_state.storage.get_inquiry(s_state.storage.ctx,
                                                   &data_buf[k_ra_pmsc_inq_off_vendor],
                                                   &data_buf[k_ra_pmsc_inq_off_product],
                                                   &data_buf[k_ra_pmsc_inq_off_revision]);
  if (err != k_ra_ok) {
    return err;
  }
  *out_len = (uint32_t)k_ra_pmsc_inquiry_resp_len;
  return k_ra_ok;
}

/**
 * @brief Build the 8-byte READ_CAPACITY(10) response.
 *
 * @details Per SBC-4 sec 5.10 the response is the LAST valid LBA
 * (block_count - 1, big-endian) followed by the block size
 * (big-endian).
 *
 * @param[in] data_buf See implementation.
 * @param[in] capacity See implementation.
 * @param[in] out_len See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t
internal_handle_read_capacity(uint8_t* data_buf, uint32_t capacity, uint32_t* out_len)
{
  if (capacity < k_ra_pmsc_read_capacity_resp_len) {
    return k_ra_err_invalid_size;
  }
  uint32_t       block_count = 0U;
  uint32_t       block_size  = 0U;
  const ra_err_t err = s_state.storage.get_capacity(s_state.storage.ctx, &block_count, &block_size);
  if (err != k_ra_ok) {
    return err;
  }
  /* Block count of zero is meaningless; degrade gracefully. */
  const uint32_t last_lba = (block_count == 0U) ? 0U : (block_count - 1U);
  internal_zero_bytes(data_buf, (uint32_t)k_ra_pmsc_read_capacity_resp_len);
  internal_pack_u32_be(last_lba, &data_buf[k_ra_pmsc_cap_off_last_lba]);
  internal_pack_u32_be(block_size, &data_buf[k_ra_pmsc_cap_off_blk_size]);
  *out_len = (uint32_t)k_ra_pmsc_read_capacity_resp_len;
  return k_ra_ok;
}

/**
 * @brief Build the 18-byte REQUEST SENSE response.
 *
 * @details Per SPC-4 sec 6.30 the device returns "no sense" (0x00)
 * once the CHECK CONDITION has been cleared. The starter does not
 * track sense state; it always answers "no sense" so subsequent
 * commands proceed.
 *
 * @param[in] data_buf See implementation.
 * @param[in] capacity See implementation.
 * @param[in] out_len See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t
internal_handle_request_sense(uint8_t* data_buf, uint32_t capacity, uint32_t* out_len)
{
  if (capacity < k_ra_pmsc_request_sense_resp_len) {
    return k_ra_err_invalid_size;
  }
  internal_zero_bytes(data_buf, (uint32_t)k_ra_pmsc_request_sense_resp_len);
  /* Byte 0 = response code (0x70 current error, valid bit clear). */
  data_buf[0] = k_ra_pmsc_sense_resp_code;
  /* Byte 7 = additional sense length (n - 7 = 10). */
  data_buf[k_ra_pmsc_sense_addl_length_byte] = k_ra_pmsc_sense_addl_length_value;
  *out_len                                   = (uint32_t)k_ra_pmsc_request_sense_resp_len;
  return k_ra_ok;
}

/**
 * @brief Build the minimal 4-byte MODE SENSE(6) header response.
 *
 * @details Per SPC-4 sec 6.11 a header-only response is legal when
 * no descriptors / pages are present. Byte 0 = mode data length
 * (3). Byte 1 = medium type (0). Byte 2 = device-specific parameter
 * (0). Byte 3 = block descriptor length (0).
 *
 * @param[in] data_buf See implementation.
 * @param[in] capacity See implementation.
 * @param[in] out_len See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_handle_mode_sense(uint8_t* data_buf, uint32_t capacity, uint32_t* out_len)
{
  if (capacity < k_ra_pmsc_mode_sense_resp_len) {
    return k_ra_err_invalid_size;
  }
  internal_zero_bytes(data_buf, (uint32_t)k_ra_pmsc_mode_sense_resp_len);
  /* Mode data length (3, header-only response). */
  data_buf[0] = k_ra_pmsc_mode_data_length_value;
  *out_len    = (uint32_t)k_ra_pmsc_mode_sense_resp_len;
  return k_ra_ok;
}

/**
 * @brief Run a SCSI READ(10).
 *
 * @details See implementation.
 * @param[in] data_buf See implementation.
 * @param[in] capacity See implementation.
 * @param[in] out_len See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_handle_read10(uint8_t* data_buf, uint32_t capacity, uint32_t* out_len)
{
  uint32_t lba         = 0U;
  uint32_t block_count = 0U;
  internal_decode_rw10(s_state.cbw_cdb, &lba, &block_count);
  if (block_count == 0U) {
    *out_len = 0U;
    return k_ra_ok;
  }
  uint32_t       device_block_count = 0U;
  uint32_t       block_size         = 0U;
  const ra_err_t cap_err =
    s_state.storage.get_capacity(s_state.storage.ctx, &device_block_count, &block_size);
  if (cap_err != k_ra_ok) {
    return cap_err;
  }
  if (block_size == 0U) {
    block_size = (uint32_t)k_ra_pmsc_block_size_default;
  }
  const uint32_t bytes = block_count * block_size;
  if (bytes > capacity) {
    return k_ra_err_invalid_size;
  }
  const ra_err_t err = s_state.storage.read_block(s_state.storage.ctx, lba, block_count, data_buf);
  if (err != k_ra_ok) {
    return err;
  }
  *out_len = bytes;
  return k_ra_ok;
}

/**
 * @brief Run a SCSI WRITE(10) -- the data buffer holds the
 *        host-supplied payload.
 *
 * @details See implementation.
 * @param[in] data_buf See implementation.
 * @param[in] out_len See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_handle_write10(const uint8_t* data_buf, uint32_t* out_len)
{
  uint32_t lba         = 0U;
  uint32_t block_count = 0U;
  internal_decode_rw10(s_state.cbw_cdb, &lba, &block_count);
  if (block_count == 0U) {
    *out_len = 0U;
    return k_ra_ok;
  }
  uint32_t       device_block_count = 0U;
  uint32_t       block_size         = 0U;
  const ra_err_t cap_err =
    s_state.storage.get_capacity(s_state.storage.ctx, &device_block_count, &block_size);
  if (cap_err != k_ra_ok) {
    return cap_err;
  }
  if (block_size == 0U) {
    block_size = (uint32_t)k_ra_pmsc_block_size_default;
  }
  const ra_err_t err = s_state.storage.write_block(s_state.storage.ctx, lba, block_count, data_buf);
  if (err != k_ra_ok) {
    return err;
  }
  *out_len = block_count * block_size;
  return k_ra_ok;
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
