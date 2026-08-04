/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/usbx/src/ux_device_class_storage_inquiry.c
 * @brief SPC-correct SCSI INQUIRY override for the USBX device storage class
 *
 * @par Tag
 * [Ring 4 / PORT] {World: S}
 *
 * @details
 * First-party replacement for the vendored
 * ``ux_device_class_storage_inquiry.c`` (excluded from the build in
 * ``cmake/usbx.cmake``). The vendor implementation has two defects that
 * macOS's SCSI layer rejects (observed on a real EK-RA8D2 against a Mac
 * host: three standard INQUIRYs, then Bulk-Only Mass Storage Reset, then
 * the device is abandoned -- no block device is created):
 *
 *  1. It reports RESPONSE DATA FORMAT = 0 (SCSI-1 era) and VERSION = 0
 *     in the standard INQUIRY data. SPC requires format 2; macOS
 *     refuses the device, Linux tolerates it.
 *  2. It ignores the EVPD bit (CDB byte 1 bit 0) and switches on the
 *     PAGE CODE byte alone, so a Supported-VPD-Pages request
 *     (EVPD=1, page 0x00) is answered with standard INQUIRY bytes.
 *
 * This override keys on EVPD properly (standard data, VPD page 0x00
 * supported-pages list, VPD page 0x80 unit serial from the class
 * instance), reports VERSION = SPC-2 / RESPONSE DATA FORMAT = 2, and
 * keeps the vendor's transport behaviour (direction check, residue +
 * IN-stall on short host length, CSW status codes) byte-for-byte.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-12
 * @since 0.1.0
 */

#define UX_SOURCE_CODE

#include <stdint.h>

#include "ux_api.h"
#include "ux_device_class_storage.h"
#include "ux_device_stack.h"

/**
 * @enum ra8_ux_inquiry_const_t
 * @brief SCSI INQUIRY wire-format constants (SPC-4 sec 6.6).
 */
typedef enum : uint8_t {
  k_inq_cdb_off_evpd    = 1U,    /**< CDB byte holding the EVPD flag.       */
  k_inq_cdb_evpd_bit    = 0x01U, /**< EVPD: vital-product-data request.     */
  k_inq_off_page_code   = 1U,    /**< VPD response: PAGE CODE byte.         */
  k_inq_off_version     = 2U,    /**< Standard response: VERSION byte.      */
  k_inq_off_data_format = 3U,    /**< Standard response: RESPONSE FORMAT.   */
  k_inq_off_addl_len    = 4U,    /**< Standard response: ADDITIONAL LENGTH. */
  k_inq_version_spc2    = 0x04U, /**< VERSION = SPC-2 (SPC-4 table 142).    */
  k_inq_format_spc      = 0x02U, /**< RESPONSE DATA FORMAT = 2 (SPC).       */
  k_inq_format_cdrom    = 0x32U, /**< Vendor's CD-ROM format byte.          */
  k_inq_addl_len_std    = 0x1FU, /**< 36-byte response: n - 5 = 31.         */
  k_inq_vpd0_count_off  = 3U,    /**< VPD page 0: PAGE LENGTH byte.         */
  k_inq_vpd0_first_off  = 4U,    /**< VPD page 0: first supported page.     */
  k_inq_vpd0_second_off = 5U,    /**< VPD page 0: second supported page.    */
  k_inq_vpd0_pages      = 2U,    /**< Pages listed (0x00 + 0x80).           */
  k_inq_vpd0_len        = 6U,    /**< VPD page 0 total response length.     */
  k_inq_vpd_hdr_len     = 4U,    /**< VPD header bytes before payload.      */
  k_inq_serial_len      = 20U,   /**< Serial payload bytes (vendor size).   */
  k_inq_vendor_id_len   = 8U,    /**< Vendor-identification field bytes.    */
  k_inq_product_id_len  = 16U,   /**< Product-identification field bytes.   */
  k_inq_product_rev_len = 4U,    /**< Product-revision field bytes.         */
  k_inq_serial_resp_len = 24U,   /**< VPD page 0x80 total response length.  */
  k_inq_dir_in_bit      = 0x80U, /**< CBW bmCBWFlags: data IN direction.    */
  k_inq_sense_illegal   = 0x05U, /**< Sense key: ILLEGAL REQUEST.           */
  k_inq_sense_asc_field = 0x26U, /**< ASC: INVALID FIELD IN PARAMETER.      */
  k_inq_sense_ascq      = 0x01U, /**< ASCQ used by the vendor class.        */
} ra8_ux_inquiry_const_t;

/**
 * @brief Fill the 36-byte standard INQUIRY response.
 *
 * @details Mirrors the vendor data (peripheral type, removable flag,
 * vendor/product/revision strings from the class instance) and fixes
 * the two SPC fields macOS checks: VERSION = SPC-2 and RESPONSE DATA
 * FORMAT = 2. The CD-ROM media type keeps the vendor's special format
 * (0x32) and additional-length (0x5B) values.
 *
 * @param[in]  storage Storage class instance.
 * @param[in]  lun     Logical unit the host addressed.
 * @param[out] buf     Response buffer (>= 36 bytes, pre-zeroed).
 *
 * @pre @p buf was zeroed for the full standard response length.
 * @pre @p lun indexes a configured LUN slot.
 * @post @p buf holds the full standard INQUIRY data.
 * @post No class state is modified.
 *
 * @note Pure data marshalling; no transport side effects.
 * @since 0.1.0
 */
static void internal_inquiry_fill_standard(UX_SLAVE_CLASS_STORAGE* storage, ULONG lun, UCHAR* buf)
{
  buf[UX_SLAVE_CLASS_STORAGE_INQUIRY_RESPONSE_PERIPHERAL_TYPE] =
    (UCHAR)storage->ux_slave_class_storage_lun[lun].ux_slave_class_storage_media_type;
  buf[UX_SLAVE_CLASS_STORAGE_INQUIRY_RESPONSE_REMOVABLE_MEDIA] =
    (UCHAR)storage->ux_slave_class_storage_lun[lun].ux_slave_class_storage_media_removable_flag;
  if (storage->ux_slave_class_storage_lun[lun].ux_slave_class_storage_media_type ==
      UX_SLAVE_CLASS_STORAGE_MEDIA_CDROM) {
    buf[k_inq_off_data_format] = (UCHAR)k_inq_format_cdrom;
    buf[k_inq_off_addl_len]    = UX_SLAVE_CLASS_STORAGE_INQUIRY_RESPONSE_LENGTH_CD_ROM;
  } else {
    buf[k_inq_off_version]     = (UCHAR)k_inq_version_spc2;
    buf[k_inq_off_data_format] = (UCHAR)k_inq_format_spc;
    buf[k_inq_off_addl_len]    = (UCHAR)k_inq_addl_len_std;
  }
  _ux_utility_memory_copy(buf + UX_SLAVE_CLASS_STORAGE_INQUIRY_RESPONSE_VENDOR_INFORMATION,
                          storage->ux_slave_class_storage_vendor_id,
                          (ULONG)k_inq_vendor_id_len); /* Use case of memcpy is verified. */
  _ux_utility_memory_copy(buf + UX_SLAVE_CLASS_STORAGE_INQUIRY_RESPONSE_PRODUCT_ID,
                          storage->ux_slave_class_storage_product_id,
                          (ULONG)k_inq_product_id_len); /* Use case of memcpy is verified. */
  _ux_utility_memory_copy(buf + UX_SLAVE_CLASS_STORAGE_INQUIRY_RESPONSE_PRODUCT_REVISION,
                          storage->ux_slave_class_storage_product_rev,
                          (ULONG)k_inq_product_rev_len); /* Use case of memcpy is verified. */
}

/**
 * @brief Fill the VPD page 0x00 (Supported VPD Pages) response.
 *
 * @details Lists the two pages this device serves: 0x00 (this list)
 * and 0x80 (unit serial number). SPC-4 sec 7.8.15.
 *
 * @param[out] buf Response buffer (>= 6 bytes, pre-zeroed).
 * @return Response length in bytes.
 * @retval 6 Always (header + two page codes).
 * @pre @p buf was zeroed.
 * @pre Caller clamps the host allocation length afterwards.
 * @post @p buf holds the supported-pages list.
 * @post No class state is modified.
 *
 * @note Pure data marshalling; no transport side effects.
 * @since 0.1.0
 */
static ULONG internal_inquiry_fill_vpd_pages(UCHAR* buf)
{
  buf[k_inq_off_page_code]   = UX_SLAVE_CLASS_STORAGE_INQUIRY_PAGE_CODE_STANDARD;
  buf[k_inq_vpd0_count_off]  = (UCHAR)k_inq_vpd0_pages;
  buf[k_inq_vpd0_first_off]  = UX_SLAVE_CLASS_STORAGE_INQUIRY_PAGE_CODE_STANDARD;
  buf[k_inq_vpd0_second_off] = UX_SLAVE_CLASS_STORAGE_INQUIRY_PAGE_CODE_SERIAL;
  return (ULONG)k_inq_vpd0_len;
}

/**
 * @brief Fill the VPD page 0x80 (Unit Serial Number) response.
 *
 * @details Byte-for-byte the vendor serial branch: big-endian page
 * code, 20-byte length, then the class instance's product serial.
 *
 * @param[in]  storage Storage class instance.
 * @param[out] buf     Response buffer (>= 24 bytes, pre-zeroed).
 * @return Response length in bytes.
 * @retval 24 Always (4-byte header + 20 serial bytes).
 * @pre @p buf was zeroed.
 * @pre The class instance carries a product serial string.
 * @post @p buf holds the unit-serial VPD page.
 * @post No class state is modified.
 *
 * @note Pure data marshalling; no transport side effects.
 * @since 0.1.0
 */
static ULONG internal_inquiry_fill_serial(UX_SLAVE_CLASS_STORAGE* storage, UCHAR* buf)
{
  _ux_utility_short_put_big_endian(buf, UX_SLAVE_CLASS_STORAGE_INQUIRY_PAGE_CODE_SERIAL);
  _ux_utility_short_put_big_endian(buf + k_inq_off_version, (USHORT)k_inq_serial_len);
  _ux_utility_memory_copy(buf + k_inq_vpd_hdr_len,
                          storage->ux_slave_class_storage_product_serial,
                          (ULONG)k_inq_serial_len); /* Use case of memcpy is verified. */
  return (ULONG)k_inq_serial_resp_len;
}

/**
 * @brief Reject an unsupported INQUIRY variant per the vendor contract.
 *
 * @details Stalls the IN endpoint, records ILLEGAL REQUEST sense data
 * for the addressed LUN, and fails the CSW -- identical to the vendor
 * default branch so REQUEST SENSE keeps reporting the same codes.
 *
 * @param[in,out] storage     Storage class instance.
 * @param[in]     lun         Logical unit the host addressed.
 * @param[in]     endpoint_in Bulk-IN endpoint to stall.
 * @return UX_ERROR always.
 * @retval UX_ERROR The command was rejected.
 * @pre The CBW for this command has been parsed.
 * @pre @p lun indexes a configured LUN slot.
 * @post The IN endpoint is stalled and sense data is latched.
 * @post The CSW status is FAILED.
 *
 * @note Mirrors the vendored default-branch behaviour exactly.
 * @since 0.1.0
 */
static UINT
internal_inquiry_reject(UX_SLAVE_CLASS_STORAGE* storage, ULONG lun, UX_SLAVE_ENDPOINT* endpoint_in)
{
#if !defined(UX_DEVICE_STANDALONE)
  _ux_device_stack_endpoint_stall(endpoint_in);
#else
  UX_PARAMETER_NOT_USED(endpoint_in);
#endif
  storage->ux_slave_class_storage_lun[lun].ux_slave_class_storage_request_sense_status =
    UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(k_inq_sense_illegal,
                                         k_inq_sense_asc_field,
                                         k_inq_sense_ascq);
  storage->ux_slave_class_storage_csw_status = UX_SLAVE_CLASS_STORAGE_CSW_FAILED;
  return UX_ERROR;
}

/**
 * @brief Run the vendor transport tail: data IN + residue / IN stall.
 *
 * @details Queues the clamped response on the bulk-IN endpoint and,
 * when the host asked for more bytes than were returned, records the
 * CSW residue and stalls IN -- byte-for-byte the vendored behaviour.
 * The standalone build variant instead primes the storage state
 * machine for its polled transfer path.
 *
 * @param[in,out] storage          Storage class instance.
 * @param[in]     endpoint_in      Bulk-IN endpoint.
 * @param[in,out] transfer_request IN transfer carrying the response.
 * @param[in]     inquiry_length   Clamped response length to send.
 *
 * @pre The response bytes are already in the transfer buffer.
 * @pre The CSW status for this command is already set.
 * @post The data phase is queued (or primed, standalone builds).
 * @post Residue/stall recorded when the host length was larger.
 *
 * @note Mirrors the vendored transport tail exactly.
 * @since 0.1.0
 */
static void internal_inquiry_send(UX_SLAVE_CLASS_STORAGE* storage,
                                  UX_SLAVE_ENDPOINT*      endpoint_in,
                                  UX_SLAVE_TRANSFER*      transfer_request,
                                  ULONG                   inquiry_length)
{
#if defined(UX_DEVICE_STANDALONE)
  UX_PARAMETER_NOT_USED(endpoint_in);
  storage->ux_device_class_storage_state         = UX_DEVICE_CLASS_STORAGE_STATE_TRANS_START;
  storage->ux_device_class_storage_cmd_state     = UX_DEVICE_CLASS_STORAGE_CMD_READ;
  storage->ux_device_class_storage_transfer      = transfer_request;
  storage->ux_device_class_storage_device_length = inquiry_length;
  storage->ux_device_class_storage_data_length   = inquiry_length;
  storage->ux_device_class_storage_data_count    = 0;
#else
  if (inquiry_length != 0U) {
    _ux_device_stack_transfer_request(transfer_request, inquiry_length, inquiry_length);
  }
  if (storage->ux_slave_class_storage_host_length != inquiry_length) {
    storage->ux_slave_class_storage_csw_residue =
      storage->ux_slave_class_storage_host_length - inquiry_length;
    _ux_device_stack_endpoint_stall(endpoint_in);
  }
#endif
}

/**
 * @brief SCSI INQUIRY command handler (USBX storage class entry).
 *
 * @details Drop-in replacement for the vendored handler; see the file
 * header for the behavioural differences. Dispatches on the EVPD bit
 * and PAGE CODE, fills the response, then runs the vendor's transport
 * tail (clamp to host allocation length, data IN, residue + IN stall
 * when the host asked for more than was returned).
 *
 * @param[in,out] storage      Storage class instance.
 * @param[in]     lun          Logical unit the host addressed.
 * @param[in]     endpoint_in  Bulk-IN endpoint.
 * @param[in]     endpoint_out Bulk-OUT endpoint (direction check only).
 * @param[in]     cbwcb        CDB bytes from the CBW.
 * @return Completion status.
 * @retval UX_SUCCESS The response (possibly zero-length) was queued.
 * @retval UX_ERROR   The command was rejected (sense data latched).
 * @pre Called by the storage class command dispatcher.
 * @pre @p cbwcb points at a 6-byte INQUIRY CDB.
 * @post On success the data phase is queued and CSW is PASSED.
 * @post On rejection the IN endpoint is stalled and CSW is FAILED.
 *
 * @note Non-standalone (ThreadX) build path; mirrors vendor 6.3.0.
 * @since 0.1.0
 */
UINT _ux_device_class_storage_inquiry(UX_SLAVE_CLASS_STORAGE* storage,
                                      ULONG                   lun,
                                      UX_SLAVE_ENDPOINT*      endpoint_in,
                                      UX_SLAVE_ENDPOINT*      endpoint_out,
                                      UCHAR*                  cbwcb)
{
  UX_PARAMETER_NOT_USED(endpoint_out);
  UX_ASSERT(UX_SLAVE_REQUEST_DATA_MAX_LENGTH >= (ULONG)k_inq_serial_resp_len);
  UX_TRACE_IN_LINE_INSERT(UX_TRACE_DEVICE_CLASS_STORAGE_INQUIRY,
                          storage,
                          lun,
                          0,
                          0,
                          UX_TRACE_DEVICE_CLASS_EVENTS,
                          0,
                          0)

#if !defined(UX_DEVICE_STANDALONE)
  /* A data phase the host marked OUT cannot satisfy INQUIRY: phase error. */
  if (storage->ux_slave_class_storage_host_length != 0U) {
    if ((storage->ux_slave_class_storage_cbw_flags & (UCHAR)k_inq_dir_in_bit) == 0U) {
      _ux_device_stack_endpoint_stall(endpoint_out);
      storage->ux_slave_class_storage_csw_status = UX_SLAVE_CLASS_STORAGE_CSW_PHASE_ERROR;
      return (UX_ERROR);
    }
  }
#endif

  UX_SLAVE_TRANSFER* transfer_request = &endpoint_in->ux_slave_endpoint_transfer_request;
  UCHAR*             buf              = transfer_request->ux_slave_transfer_request_data_pointer;
  const UCHAR        evpd = (UCHAR)(cbwcb[k_inq_cdb_off_evpd] & (UCHAR)k_inq_cdb_evpd_bit);
  const UCHAR        page = cbwcb[UX_SLAVE_CLASS_STORAGE_INQUIRY_PAGE_CODE];
  ULONG              inquiry_length = storage->ux_slave_class_storage_host_length;

  _ux_utility_memory_set(buf,
                         0,
                         UX_SLAVE_CLASS_STORAGE_INQUIRY_RESPONSE_LENGTH); /* Use case of memset is
                                                                             verified. */
  storage->ux_slave_class_storage_csw_status = UX_SLAVE_CLASS_STORAGE_CSW_PASSED;

  ULONG response_length = 0U;
  if (evpd == 0U) {
    /* Standard INQUIRY: SPC requires PAGE CODE = 0 when EVPD is clear. */
    if (page != (UCHAR)UX_SLAVE_CLASS_STORAGE_INQUIRY_PAGE_CODE_STANDARD) {
      return internal_inquiry_reject(storage, lun, endpoint_in);
    }
    internal_inquiry_fill_standard(storage, lun, buf);
    response_length = UX_SLAVE_CLASS_STORAGE_INQUIRY_RESPONSE_LENGTH;
  } else if (page == (UCHAR)UX_SLAVE_CLASS_STORAGE_INQUIRY_PAGE_CODE_STANDARD) {
    response_length = internal_inquiry_fill_vpd_pages(buf);
  } else if (page == (UCHAR)UX_SLAVE_CLASS_STORAGE_INQUIRY_PAGE_CODE_SERIAL) {
    response_length = internal_inquiry_fill_serial(storage, buf);
  } else {
    return internal_inquiry_reject(storage, lun, endpoint_in);
  }

  if (inquiry_length > response_length) {
    inquiry_length = response_length;
  }

  internal_inquiry_send(storage, endpoint_in, transfer_request, inquiry_length);
  return (UX_SUCCESS);
}
