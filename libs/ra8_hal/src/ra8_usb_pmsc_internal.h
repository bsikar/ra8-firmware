/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_usb_pmsc_internal.h
 * @brief Cross-TU surface shared between the device-MSC BOT state
 * @ingroup grp_hal_usb
 *        machine (ra8_usb_pmsc.c) and the SCSI command handlers
 *        (ra8_usb_pmsc_scsi.c).
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The native device-MSC class layer is split across two translation
 * units so each stays under the 1000-line file-size cap. This
 * src/-local header carries the shadow-state singleton, the small
 * shared serialisation helpers, and the SCSI handler declarations
 * that the BOT dispatcher invokes. It is NOT part of the public API:
 * the public contract lives in `ra8_usb_pmsc.h`.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_usb.h"
#include "ra8_usb_pmsc.h"

/**
 * @enum ra8_usb_pmsc_state_t
 * @brief BOT state-machine phases.
 *
 * @details Mirrors FSP's USB_PMSC_CBWRCV / USB_PMSC_DATARCV /
 * USB_PMSC_DATASND / USB_PMSC_CSWSND values in
 * `r_usb_pmsc_driver.c`. The starter adds `idle` and `cdb_decode`
 * phases so the host-side stepping matches the textbook BBB
 * lifecycle.
 */
typedef enum : uint8_t {
  k_ra8_pmsc_state_idle       = 0U, /**< Pre-CBW.                    */
  k_ra8_pmsc_state_cbw_rx     = 1U, /**< CBW arrived on bulk-OUT.    */
  k_ra8_pmsc_state_cdb_decode = 2U, /**< Dispatch to a SCSI handler. */
  k_ra8_pmsc_state_data_tx    = 3U, /**< Push data on bulk-IN.       */
  k_ra8_pmsc_state_data_rx    = 4U, /**< Pull data on bulk-OUT.      */
  k_ra8_pmsc_state_csw_tx     = 5U, /**< Push CSW on bulk-IN.        */
} ra8_usb_pmsc_state_t;

/**
 * @enum ra8_usb_pmsc_size_t
 * @brief Standard BOT wrapper sizes.
 *
 * @details The CBW / CSW lengths are nailed down by USB MSC BBB rev
 * 1.0 sections 5.1 and 5.2 respectively.
 */
typedef enum : uint16_t {
  k_ra8_pmsc_cbw_len     = 31U, /**< CBW length (BBB sec 5.1). */
  k_ra8_pmsc_csw_len     = 13U, /**< CSW length (BBB sec 5.2). */
  k_ra8_pmsc_cdb_max_len = 16U, /**< CDB ceiling.              */
} ra8_usb_pmsc_size_t;

/**
 * @enum ra8_usb_pmsc_byte_shift_t
 * @brief Per-byte left-shift constants for serialisation.
 */
typedef enum : uint8_t {
  k_ra8_pmsc_shift_byte0 = 0U,  /**< RA8 pmsc shift byte0. */
  k_ra8_pmsc_shift_byte1 = 8U,  /**< RA8 pmsc shift byte1. */
  k_ra8_pmsc_shift_byte2 = 16U, /**< RA8 pmsc shift byte2. */
  k_ra8_pmsc_shift_byte3 = 24U, /**< RA8 pmsc shift byte3. */
} ra8_usb_pmsc_byte_shift_t;

/**
 * @enum ra8_usb_pmsc_byte_mask_t
 * @brief Single-byte extraction mask.
 */
typedef enum : uint32_t {
  k_ra8_pmsc_byte_mask = 0xFFU, /**< RA8 pmsc byte mask. */
} ra8_usb_pmsc_byte_mask_t;

/**
 * @enum ra8_usb_pmsc_cdb_opcode_offset_t
 * @brief Offset of the SCSI operation code inside the cached CDB.
 *
 * @details Shared by the BOT dispatcher (`ra8_usb_pmsc.c`) and the
 * SCSI handler TU (`ra8_usb_pmsc_scsi.c`). Per SPC-4 the CDB
 * operation code is always byte 0.
 */
typedef enum : uint8_t {
  k_ra8_pmsc_cdb_off_opcode = 0U, /**< Operation Code (CDB byte 0). */
} ra8_usb_pmsc_cdb_opcode_offset_t;

/**
 * @struct ra8_usb_pmsc_state_data_t
 * @brief Singleton shadow state for the device-MSC driver.
 */
typedef struct {
  bool                   initialized;                     /**< True after init.             */
  bool                   storage_attached;                /**< True after attach_storage.   */
  ra8_usb_speed_t        speed;                           /**< Underlying controller.       */
  ra8_usb_pmsc_state_t   bot_state;                       /**< BOT state machine phase.     */
  ra8_usb_pmsc_storage_t storage;                         /**< Storage backend snapshot.    */
  uint32_t               cbw_tag;                         /**< Cached dCBWTag.              */
  uint32_t               cbw_data_length;                 /**< Cached dCBWDataTransferLen.  */
  bool                   cbw_dir_in;                      /**< Cached bmCBWFlags direction. */
  uint8_t                cbw_lun;                         /**< Cached bCBWLUN.              */
  uint8_t                cbw_cdb[k_ra8_pmsc_cdb_max_len]; /**< Cached CDB.                  */
  uint8_t                cbw_cdb_len;                     /**< Cached bCBWCBLength.         */
  uint32_t               last_data_len;                   /**< Last data byte count.        */
} ra8_usb_pmsc_state_data_t;

/**
 * @var s_usb_pmsc_state
 * @brief Singleton shadow state shared by both device-MSC TUs.
 *
 * @details Defined in `ra8_usb_pmsc.c`; the SCSI handler TU reads the
 * cached CDB and storage backend through this extern declaration.
 *
 * @note Single-instance, single-LUN; not thread-safe.
 * @warning Direct modification outside the driver TUs is forbidden.
 * @since 0.1.0
 */
extern ra8_usb_pmsc_state_data_t s_usb_pmsc_state;

/**
 * @brief Zero `len` bytes at `dst` byte-by-byte.
 *
 * @details Avoids `memset` so the project's clang-tidy
 * `clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling`
 * gate stays clean. Defined in `ra8_usb_pmsc.c`, shared with the SCSI
 * handler TU.
 *
 * @param[out] dst Destination buffer (non-null, at least `len` bytes).
 * @param[in]  len Byte count to clear.
 * @pre `dst` is non-null and spans at least `len` bytes.
 * @pre Module state is consistent.
 * @post All `len` bytes at `dst` are zero.
 * @post No other state mutated.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_PRIV void internal_zero_bytes(uint8_t* dst, uint32_t len);

/**
 * @brief Build the 36-byte SCSI INQUIRY response.
 *
 * @details Asks the storage backend for the three ASCII strings
 * (vendor, product, revision) and bakes them into the response per
 * SBC-4 sec 6.6. SPACE-padding is applied if the backend returns a
 * shorter string.
 *
 * @param[out] data_buf Destination response buffer.
 * @param[in]  capacity Capacity of `data_buf` in bytes.
 * @param[out] out_len  Receives the response length.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @retval k_ra8_err_invalid_size Buffer too small.
 * @pre Module state is consistent.
 * @pre Storage backend is attached.
 * @post Caller-visible state matches the documented contract.
 * @post `*out_len` holds the response length on success.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t internal_handle_inquiry(uint8_t* data_buf, uint32_t capacity, uint32_t* out_len);

/**
 * @brief Build the 8-byte READ_CAPACITY(10) response.
 *
 * @details Per SBC-4 sec 5.10 the response is the LAST valid LBA
 * (block_count - 1, big-endian) followed by the block size
 * (big-endian).
 *
 * @param[out] data_buf Destination response buffer.
 * @param[in]  capacity Capacity of `data_buf` in bytes.
 * @param[out] out_len  Receives the response length.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @retval k_ra8_err_invalid_size Buffer too small.
 * @pre Module state is consistent.
 * @pre Storage backend is attached.
 * @post Caller-visible state matches the documented contract.
 * @post `*out_len` holds the response length on success.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t internal_handle_read_capacity(uint8_t*  data_buf,
                                                 uint32_t  capacity,
                                                 uint32_t* out_len);

/**
 * @brief Build the 18-byte REQUEST SENSE response.
 *
 * @details Per SPC-4 sec 6.30 the device returns "no sense" (0x00)
 * once the CHECK CONDITION has been cleared. The starter does not
 * track sense state; it always answers "no sense" so subsequent
 * commands proceed.
 *
 * @param[out] data_buf Destination response buffer.
 * @param[in]  capacity Capacity of `data_buf` in bytes.
 * @param[out] out_len  Receives the response length.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @retval k_ra8_err_invalid_size Buffer too small.
 * @pre Module state is consistent.
 * @pre Storage backend is attached.
 * @post Caller-visible state matches the documented contract.
 * @post `*out_len` holds the response length on success.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t internal_handle_request_sense(uint8_t*  data_buf,
                                                 uint32_t  capacity,
                                                 uint32_t* out_len);

/**
 * @brief Build the minimal 4-byte MODE SENSE(6) header response.
 *
 * @details Per SPC-4 sec 6.11 a header-only response is legal when
 * no descriptors / pages are present. Byte 0 = mode data length
 * (3). Byte 1 = medium type (0). Byte 2 = device-specific parameter
 * (0). Byte 3 = block descriptor length (0).
 *
 * @param[out] data_buf Destination response buffer.
 * @param[in]  capacity Capacity of `data_buf` in bytes.
 * @param[out] out_len  Receives the response length.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @retval k_ra8_err_invalid_size Buffer too small.
 * @pre Module state is consistent.
 * @pre Storage backend is attached.
 * @post Caller-visible state matches the documented contract.
 * @post `*out_len` holds the response length on success.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t internal_handle_mode_sense(uint8_t*  data_buf,
                                              uint32_t  capacity,
                                              uint32_t* out_len);

/**
 * @brief Run a SCSI READ(10).
 *
 * @details Decodes the cached READ(10) CDB, asks the backend for the
 * geometry, and reads the requested blocks into `data_buf`.
 *
 * @param[out] data_buf Destination data buffer.
 * @param[in]  capacity Capacity of `data_buf` in bytes.
 * @param[out] out_len  Receives the transferred byte count.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @retval k_ra8_err_invalid_size Buffer too small for the read.
 * @pre Module state is consistent.
 * @pre Storage backend is attached.
 * @post Caller-visible state matches the documented contract.
 * @post `*out_len` holds the transferred byte count on success.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t internal_handle_read10(uint8_t* data_buf, uint32_t capacity, uint32_t* out_len);

/**
 * @brief Run a SCSI WRITE(10) -- the data buffer holds the
 *        host-supplied payload.
 *
 * @details Decodes the cached WRITE(10) CDB, asks the backend for the
 * geometry, and writes the supplied blocks through the backend.
 *
 * @param[in]  data_buf Host-supplied payload buffer.
 * @param[out] out_len  Receives the transferred byte count.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Storage backend is attached.
 * @post Caller-visible state matches the documented contract.
 * @post `*out_len` holds the transferred byte count on success.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t internal_handle_write10(const uint8_t* data_buf, uint32_t* out_len);

#ifdef __cplusplus
}
#endif
