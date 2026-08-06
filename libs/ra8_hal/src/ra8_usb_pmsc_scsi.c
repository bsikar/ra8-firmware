/**
 * @file ra8_usb_pmsc_scsi.c
 * @brief Native USB device-side MSC (Mass Storage Class) SCSI command
 *        handlers
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Companion translation unit to `ra8_usb_pmsc.c`. Holds the SCSI
 * command handlers (INQUIRY, READ_CAPACITY(10), REQUEST SENSE, MODE
 * SENSE(6), READ(10), WRITE(10)) that the BOT dispatcher in
 * `ra8_usb_pmsc.c` invokes once a CDB has been decoded. The two TUs
 * share the `s_usb_pmsc_state` shadow singleton and a handful of serialisation
 * helpers via `ra8_usb_pmsc_internal.h`. FSP's
 * `pmsc_atapi_command_processing` is reference material only -- no FSP
 * source is pulled in verbatim.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_usb.h"
#include "ra8_usb_pmsc.h"
#include "ra8_usb_pmsc_internal.h"

/* =============================================================================
 * Internal constants
 * =============================================================================
 */

/**
 * @enum ra8_usb_pmsc_cdb_offset_t
 * @brief SCSI CDB byte offsets used when decoding READ(10) /
 *        WRITE(10) blocks.
 *
 * @details The operation-code offset (byte 0) is shared with the BOT
 * dispatcher and lives in `ra8_usb_pmsc_internal.h` as
 * `k_ra8_pmsc_cdb_off_opcode`.
 */
typedef enum : uint8_t {
  k_ra8_pmsc_cdb_off_lba_msb = 2U, /**< READ(10): LBA byte 3. */
  k_ra8_pmsc_cdb_off_lba_b1  = 3U, /**< READ(10): LBA byte 2. */
  k_ra8_pmsc_cdb_off_lba_b2  = 4U, /**< READ(10): LBA byte 1. */
  k_ra8_pmsc_cdb_off_lba_lsb = 5U, /**< READ(10): LBA byte 0. */
  k_ra8_pmsc_cdb_off_cnt_msb = 7U, /**< READ(10): count high. */
  k_ra8_pmsc_cdb_off_cnt_lsb = 8U, /**< READ(10): count low.  */
} ra8_usb_pmsc_cdb_offset_t;

/**
 * @enum ra8_usb_pmsc_inquiry_offset_t
 * @brief Byte offsets inside the SBC-4 standard INQUIRY response
 *        the device returns.
 */
typedef enum : uint8_t {
  k_ra8_pmsc_inq_off_dev_type    = 0U,  /**< Byte 0 (qual+devtype). */
  k_ra8_pmsc_inq_off_removable   = 1U,  /**< Byte 1 (removable).    */
  k_ra8_pmsc_inq_off_version     = 2U,  /**< Byte 2 (SPC version).  */
  k_ra8_pmsc_inq_off_resp_format = 3U,  /**< Byte 3 (resp format).  */
  k_ra8_pmsc_inq_off_addl_length = 4U,  /**< Byte 4 (addl length).  */
  k_ra8_pmsc_inq_off_vendor      = 8U,  /**< Vendor ID start.       */
  k_ra8_pmsc_inq_off_product     = 16U, /**< Product ID start.      */
  k_ra8_pmsc_inq_off_revision    = 32U, /**< Product revision.      */
} ra8_usb_pmsc_inquiry_offset_t;

/**
 * @enum ra8_usb_pmsc_inquiry_byte_t
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
  k_ra8_pmsc_inq_byte_dev_type    = 0x00U, /**< Direct-access block. */
  k_ra8_pmsc_inq_byte_removable   = 0x80U, /**< Removable bit set.   */
  k_ra8_pmsc_inq_byte_version     = 0x04U, /**< SPC-4.               */
  k_ra8_pmsc_inq_byte_resp_format = 0x02U, /**< Current format.      */
  k_ra8_pmsc_inq_byte_addl_length = 0x1FU, /**< 36 - 5 = 31.         */
} ra8_usb_pmsc_inquiry_byte_t;

/**
 * @enum ra8_usb_pmsc_capacity_offset_t
 * @brief Byte offsets inside the SCSI READ_CAPACITY(10) 8-byte
 *        response (SBC-4 sec 5.10).
 */
typedef enum : uint8_t {
  k_ra8_pmsc_cap_off_last_lba = 0U, /**< Last valid LBA (big-endian). */
  k_ra8_pmsc_cap_off_blk_size = 4U, /**< Block length (big-endian).   */
} ra8_usb_pmsc_capacity_offset_t;

/**
 * @enum ra8_usb_pmsc_sense_t
 * @brief Pre-canned values for the SCSI REQUEST SENSE response and
 *        the minimal MODE SENSE(6) header.
 *
 * @details See SPC-4 sec 6.30 (REQUEST SENSE) and sec 6.11 (MODE
 * SENSE). The starter does not track sense state so it always
 * answers "no sense" with a current-error response code.
 */
typedef enum : uint8_t {
  k_ra8_pmsc_sense_resp_code         = 0x70U, /**< Current error, valid clr.    */
  k_ra8_pmsc_sense_addl_length_byte  = 7U,    /**< Byte index of addl-len.      */
  k_ra8_pmsc_sense_addl_length_value = 0x0AU, /**< Additional length = 10.      */
  k_ra8_pmsc_mode_data_length_value  = 0x03U, /**< Mode data len (3, hdr-only). */
} ra8_usb_pmsc_sense_t;

/**
 * @enum ra8_usb_pmsc_ascii_t
 * @brief ASCII constant used for SPACE-padding INQUIRY string fields.
 */
typedef enum : uint8_t {
  k_ra8_pmsc_ascii_space = 0x20U, /**< ASCII SPACE. */
} ra8_usb_pmsc_ascii_t;

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

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
RA8_INTERNAL
static void internal_pack_u32_be(uint32_t value, uint8_t* dst)
{
  dst[0] = (uint8_t)((value >> k_ra8_pmsc_shift_byte3) & k_ra8_pmsc_byte_mask);
  dst[1] = (uint8_t)((value >> k_ra8_pmsc_shift_byte2) & k_ra8_pmsc_byte_mask);
  dst[2] = (uint8_t)((value >> k_ra8_pmsc_shift_byte1) & k_ra8_pmsc_byte_mask);
  dst[3] = (uint8_t)((value >> k_ra8_pmsc_shift_byte0) & k_ra8_pmsc_byte_mask);
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
RA8_INTERNAL
static void internal_pad_space(uint8_t* dst, uint32_t len)
{
  for (uint32_t i = 0U; i < len; ++i) {
    dst[i] = k_ra8_pmsc_ascii_space;
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
RA8_INTERNAL
static void internal_decode_rw10(const uint8_t* cdb, uint32_t* out_lba, uint32_t* out_block_count)
{
  *out_lba = ((uint32_t)cdb[k_ra8_pmsc_cdb_off_lba_msb] << k_ra8_pmsc_shift_byte3) |
             ((uint32_t)cdb[k_ra8_pmsc_cdb_off_lba_b1] << k_ra8_pmsc_shift_byte2) |
             ((uint32_t)cdb[k_ra8_pmsc_cdb_off_lba_b2] << k_ra8_pmsc_shift_byte1) |
             ((uint32_t)cdb[k_ra8_pmsc_cdb_off_lba_lsb] << k_ra8_pmsc_shift_byte0);
  *out_block_count = ((uint32_t)cdb[k_ra8_pmsc_cdb_off_cnt_msb] << k_ra8_pmsc_shift_byte1) |
                     ((uint32_t)cdb[k_ra8_pmsc_cdb_off_cnt_lsb] << k_ra8_pmsc_shift_byte0);
}

ra8_err_t internal_handle_inquiry(uint8_t* data_buf, uint32_t capacity, uint32_t* out_len)
{
  if (capacity < k_ra8_pmsc_inquiry_resp_len) {
    return k_ra8_err_invalid_size;
  }
  internal_zero_bytes(data_buf, (uint32_t)k_ra8_pmsc_inquiry_resp_len);

  data_buf[k_ra8_pmsc_inq_off_dev_type]    = k_ra8_pmsc_inq_byte_dev_type;
  data_buf[k_ra8_pmsc_inq_off_removable]   = k_ra8_pmsc_inq_byte_removable;
  data_buf[k_ra8_pmsc_inq_off_version]     = k_ra8_pmsc_inq_byte_version;
  data_buf[k_ra8_pmsc_inq_off_resp_format] = k_ra8_pmsc_inq_byte_resp_format;
  data_buf[k_ra8_pmsc_inq_off_addl_length] = k_ra8_pmsc_inq_byte_addl_length;

  /* Pad the three string fields with SPACE first so a backend that
   * writes fewer bytes still produces a spec-conforming response. */
  internal_pad_space(&data_buf[k_ra8_pmsc_inq_off_vendor], (uint32_t)k_ra8_pmsc_inq_vendor_len);
  internal_pad_space(&data_buf[k_ra8_pmsc_inq_off_product], (uint32_t)k_ra8_pmsc_inq_product_len);
  internal_pad_space(&data_buf[k_ra8_pmsc_inq_off_revision], (uint32_t)k_ra8_pmsc_inq_revision_len);

  const ra8_err_t err =
    s_usb_pmsc_state.storage.get_inquiry(s_usb_pmsc_state.storage.ctx,
                                         &data_buf[k_ra8_pmsc_inq_off_vendor],
                                         &data_buf[k_ra8_pmsc_inq_off_product],
                                         &data_buf[k_ra8_pmsc_inq_off_revision]);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_len = (uint32_t)k_ra8_pmsc_inquiry_resp_len;
  return k_ra8_ok;
}

ra8_err_t internal_handle_read_capacity(uint8_t* data_buf, uint32_t capacity, uint32_t* out_len)
{
  if (capacity < k_ra8_pmsc_read_capacity_resp_len) {
    return k_ra8_err_invalid_size;
  }
  uint32_t        block_count = 0U;
  uint32_t        block_size  = 0U;
  const ra8_err_t err =
    s_usb_pmsc_state.storage.get_capacity(s_usb_pmsc_state.storage.ctx, &block_count, &block_size);
  if (err != k_ra8_ok) {
    return err;
  }
  /* Block count of zero is meaningless; degrade gracefully. */
  const uint32_t last_lba = (block_count == 0U) ? 0U : (block_count - 1U);
  internal_zero_bytes(data_buf, (uint32_t)k_ra8_pmsc_read_capacity_resp_len);
  internal_pack_u32_be(last_lba, &data_buf[k_ra8_pmsc_cap_off_last_lba]);
  internal_pack_u32_be(block_size, &data_buf[k_ra8_pmsc_cap_off_blk_size]);
  *out_len = (uint32_t)k_ra8_pmsc_read_capacity_resp_len;
  return k_ra8_ok;
}

ra8_err_t internal_handle_request_sense(uint8_t* data_buf, uint32_t capacity, uint32_t* out_len)
{
  if (capacity < k_ra8_pmsc_request_sense_resp_len) {
    return k_ra8_err_invalid_size;
  }
  internal_zero_bytes(data_buf, (uint32_t)k_ra8_pmsc_request_sense_resp_len);
  /* Byte 0 = response code (0x70 current error, valid bit clear). */
  data_buf[0] = k_ra8_pmsc_sense_resp_code;
  /* Byte 7 = additional sense length (n - 7 = 10). */
  data_buf[k_ra8_pmsc_sense_addl_length_byte] = k_ra8_pmsc_sense_addl_length_value;
  *out_len                                    = (uint32_t)k_ra8_pmsc_request_sense_resp_len;
  return k_ra8_ok;
}

ra8_err_t internal_handle_mode_sense(uint8_t* data_buf, uint32_t capacity, uint32_t* out_len)
{
  if (capacity < k_ra8_pmsc_mode_sense_resp_len) {
    return k_ra8_err_invalid_size;
  }
  internal_zero_bytes(data_buf, (uint32_t)k_ra8_pmsc_mode_sense_resp_len);
  /* Mode data length (3, header-only response). */
  data_buf[0] = k_ra8_pmsc_mode_data_length_value;
  *out_len    = (uint32_t)k_ra8_pmsc_mode_sense_resp_len;
  return k_ra8_ok;
}

ra8_err_t internal_handle_read10(uint8_t* data_buf, uint32_t capacity, uint32_t* out_len)
{
  uint32_t lba         = 0U;
  uint32_t block_count = 0U;
  internal_decode_rw10(s_usb_pmsc_state.cbw_cdb, &lba, &block_count);
  if (block_count == 0U) {
    *out_len = 0U;
    return k_ra8_ok;
  }
  uint32_t        device_block_count = 0U;
  uint32_t        block_size         = 0U;
  const ra8_err_t cap_err = s_usb_pmsc_state.storage.get_capacity(s_usb_pmsc_state.storage.ctx,
                                                                  &device_block_count,
                                                                  &block_size);
  if (cap_err != k_ra8_ok) {
    return cap_err;
  }
  if (block_size == 0U) {
    block_size = (uint32_t)k_ra8_pmsc_block_size_default;
  }
  const uint32_t bytes = block_count * block_size;
  if (bytes > capacity) {
    return k_ra8_err_invalid_size;
  }
  const ra8_err_t err =
    s_usb_pmsc_state.storage.read_block(s_usb_pmsc_state.storage.ctx, lba, block_count, data_buf);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_len = bytes;
  return k_ra8_ok;
}

ra8_err_t internal_handle_write10(const uint8_t* data_buf, uint32_t* out_len)
{
  uint32_t lba         = 0U;
  uint32_t block_count = 0U;
  internal_decode_rw10(s_usb_pmsc_state.cbw_cdb, &lba, &block_count);
  if (block_count == 0U) {
    *out_len = 0U;
    return k_ra8_ok;
  }
  uint32_t        device_block_count = 0U;
  uint32_t        block_size         = 0U;
  const ra8_err_t cap_err = s_usb_pmsc_state.storage.get_capacity(s_usb_pmsc_state.storage.ctx,
                                                                  &device_block_count,
                                                                  &block_size);
  if (cap_err != k_ra8_ok) {
    return cap_err;
  }
  if (block_size == 0U) {
    block_size = (uint32_t)k_ra8_pmsc_block_size_default;
  }
  const ra8_err_t err =
    s_usb_pmsc_state.storage.write_block(s_usb_pmsc_state.storage.ctx, lba, block_count, data_buf);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_len = block_count * block_size;
  return k_ra8_ok;
}
