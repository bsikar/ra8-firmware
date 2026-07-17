/**
 * @file ra8_usb_hmsc.h
 * @brief Native USB host-side MSC (Mass Storage Class) class layer
 * @ingroup grp_hal_usb
 *
 * @details
 * Glues the host-mode bring-up paths in `ra8_usb` to a USB Mass Storage
 * Class (BBB / Bulk-Only-Transport) peripheral - typically a thumb
 * drive or external SSD - attached on the EK-RA8D2's USB-host port.
 * Mirrors FSP's `r_usb_hmsc` host-MSC class flow but compiled as part
 * of this tree with no FSP / CherryUSB / TinyUSB binaries pulled in.
 *
 * Lifecycle (mirrors `ra8_usb_hcdc.h` shape):
 *
 *  1. `ra8_usb_hmsc_init(speed)` flips the controller to host mode,
 *     primes the BOT (Bulk-Only-Transport) tag counter to zero, and
 *     leaves the bus in the "wait for attach" state (UACT cleared).
 *  2. The driver runs the chapter-9 enumeration step machine
 *     (Reset -> SET_ADDRESS -> GET_DEVICE_DESCRIPTOR ->
 *     GET_CONFIG_DESCRIPTOR -> SET_CONFIG -> SET_INTERFACE) over the
 *     DCP, walks the configuration descriptor for the MSC interface
 *     (class=0x08 / subclass=0x06 SCSI / protocol=0x50 BBB), and
 *     issues a class Get-Max-LUN request.
 *  3. When enumeration completes the registered attach callback fires
 *     once with the discovered bulk-IN / bulk-OUT pipe handles plus
 *     the cached max-LUN value.
 *  4. SCSI commands (`_inquiry`, `_read_capacity`, `_read10`,
 *     `_write10`) are layered on top of BOT: each call constructs a
 *     31-byte CBW (Command Block Wrapper, signature 'USBC' /
 *     0x43425355), pushes it on the bulk-OUT pipe, runs the data
 *     stage, and consumes the 13-byte CSW (Command Status Wrapper,
 *     signature 'USBS' / 0x53425355) from the bulk-IN pipe.
 *  5. `ra8_usb_hmsc_close()` drops bus power and releases the device.
 *
 * The starter only tracks a single attached MSC device with up to
 * `k_ra8_hmsc_max_lun` logical units; hubs and CD-ROM (ATAPI) command
 * blocks are deferred follow-ups.
 *
 * Reference: USB Mass Storage Class Bulk-Only Transport spec rev 1.0
 * (USB-IF, 1999-09-31), and SCSI Primary Commands rev 4 (T10/1731-D)
 * for the SBC opcodes used here.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_usb.h"

/* =============================================================================
 * Constants
 * =============================================================================
 */

/**
 * @enum ra8_usb_hmsc_pipe_t
 * @brief PIPE numbers used by the host-MSC driver for the attached
 *        peripheral's bulk endpoints.
 *
 * @details FSP / RA8D2 PIPE assignment rules constrain bulk pipes to
 * PIPE1..PIPE5. The host-MSC class only uses two pipes: bulk-IN to
 * pull data + CSW, bulk-OUT to push CBW + data. See USB MSC BBB rev
 * 1.0 sec 3 "Functional Characteristics".
 */
typedef enum : uint8_t {
  k_ra8_hmsc_pipe_bulk_in  = 3U, /**< PIPE3 -> attached EP bulk IN.  */
  k_ra8_hmsc_pipe_bulk_out = 4U, /**< PIPE4 -> attached EP bulk OUT. */
} ra8_usb_hmsc_pipe_t;

/**
 * @enum ra8_usb_hmsc_packet_t
 * @brief Packet sizing for the attached device's bulk endpoints.
 */
typedef enum : uint16_t {
  k_ra8_hmsc_bulk_max_packet_fs = 64U,  /**< Bulk size at full speed. */
  k_ra8_hmsc_bulk_max_packet_hs = 512U, /**< Bulk size at high speed. */
} ra8_usb_hmsc_packet_t;

/**
 * @enum ra8_usb_hmsc_class_t
 * @brief Class / subclass / protocol triplet that identifies an MSC
 *        function within an attached USB device's descriptor walk.
 *
 * @details Numbered from the USB-IF "Class Codes" registry and the
 * USB MSC overview spec rev 1.4. Subclass 0x06 = transparent SCSI;
 * protocol 0x50 = Bulk-Only Transport (BBB).
 */
typedef enum : uint8_t {
  k_ra8_hmsc_class_msc     = 0x08U, /**< MSC interface class.       */
  k_ra8_hmsc_subclass_scsi = 0x06U, /**< Transparent SCSI subclass. */
  k_ra8_hmsc_protocol_bbb  = 0x50U, /**< Bulk-Only Transport.       */
} ra8_usb_hmsc_class_t;

/**
 * @enum ra8_usb_hmsc_request_t
 * @brief MSC class-specific request codes the host issues to the
 *        attached device.
 *
 * @details Per USB MSC BBB rev 1.0 sec 3.1 "Class-Specific Requests".
 * `Get_Max_LUN` returns the highest LUN index, `Reset_Recovery`
 * issues a Bulk-Only Mass Storage Reset (the spec's protocol-level
 * recovery handshake).
 */
typedef enum : uint8_t {
  k_ra8_hmsc_req_mass_storage_reset = 0xFFU, /**< 0-byte payload.  */
  k_ra8_hmsc_req_get_max_lun        = 0xFEU, /**< 1-byte response. */
} ra8_usb_hmsc_request_t;

/**
 * @enum ra8_usb_hmsc_max_lun_t
 * @brief Compile-time ceiling on the number of logical units the
 *        starter tracks per attached device.
 *
 * @details The MSC BBB spec allows up to 16 LUNs (4-bit field) but
 * 99% of consumer thumb drives expose LUN 0 only. We size the
 * internal tables for a small ceiling here.
 */
typedef enum : uint8_t {
  k_ra8_hmsc_max_lun = 4U, /**< Max LUNs the starter tracks. */
} ra8_usb_hmsc_max_lun_t;

/**
 * @enum ra8_usb_hmsc_csw_status_t
 * @brief CSW status field values returned by the device after a CBW.
 *
 * @details Per USB MSC BBB rev 1.0 sec 5.2 "Command Status Wrapper".
 * Anything outside this set is treated as a protocol error.
 */
typedef enum : uint8_t {
  k_ra8_hmsc_csw_status_passed      = 0x00U, /**< Command succeeded. */
  k_ra8_hmsc_csw_status_failed      = 0x01U, /**< Command failed.    */
  k_ra8_hmsc_csw_status_phase_error = 0x02U, /**< BBB phase error -- need
                                                  reset recovery.       */
} ra8_usb_hmsc_csw_status_t;

/**
 * @enum ra8_usb_hmsc_scsi_t
 * @brief SCSI opcodes the host-MSC class issues over BBB.
 *
 * @details A small subset of SCSI Primary Commands rev 4 + SCSI
 * Block Commands rev 4. Matches FSP's `usb_atapi_t` enumeration but
 * only the values our public API actually uses are spelled out.
 */
typedef enum : uint8_t {
  k_ra8_hmsc_scsi_test_unit_ready  = 0x00U, /**< TEST UNIT READY.   */
  k_ra8_hmsc_scsi_request_sense    = 0x03U, /**< REQUEST SENSE.     */
  k_ra8_hmsc_scsi_inquiry          = 0x12U, /**< INQUIRY.           */
  k_ra8_hmsc_scsi_read_capacity_10 = 0x25U, /**< READ CAPACITY(10). */
  k_ra8_hmsc_scsi_read_10          = 0x28U, /**< READ(10).          */
  k_ra8_hmsc_scsi_write_10         = 0x2AU, /**< WRITE(10).         */
} ra8_usb_hmsc_scsi_t;

/**
 * @enum ra8_usb_hmsc_resp_size_t
 * @brief Standard SCSI response payload sizes.
 *
 * @details The SCSI INQUIRY response is fixed at 36 bytes (5+31, see
 * SBC-4 sec 6.6) and the READ_CAPACITY(10) response is 8 bytes (4-
 * byte returned-LBA + 4-byte block-length, SBC-4 sec 5.10).
 */
typedef enum : uint16_t {
  k_ra8_hmsc_inquiry_resp_len       = 36U,  /**< INQUIRY response len. */
  k_ra8_hmsc_read_capacity_resp_len = 8U,   /**< READ_CAPACITY(10).    */
  k_ra8_hmsc_block_size_default     = 512U, /**< SCSI default block.   */
} ra8_usb_hmsc_resp_size_t;

/**
 * @struct ra8_usb_hmsc_device_t
 * @brief Snapshot of the attached MSC device, passed to the attach
 *        callback.
 *
 * @details Populated by the host-MSC driver once the descriptor walk
 * completes and Get-Max-LUN comes back. The pipe numbers are the
 * controller-side handles wired up against the attached device's
 * bulk endpoints. The recipient should hold onto this struct (or
 * copy it) so subsequent SCSI calls land on the right LUN.
 */
typedef struct {
  uint8_t  device_address;      /**< Assigned USB address (1..127).     */
  uint8_t  bulk_in_ep;          /**< Attached device's bulk IN EP num.  */
  uint8_t  bulk_out_ep;         /**< Attached device's bulk OUT EP num. */
  uint8_t  max_lun;             /**< Get-Max-LUN response (0..15).      */
  uint8_t  interface_number;    /**< MSC bInterfaceNumber.              */
  uint16_t bulk_in_max_packet;  /**< Attached bulk-IN wMaxPacketSize.   */
  uint16_t bulk_out_max_packet; /**< Attached bulk-OUT wMaxPacketSize.  */
  uint16_t vendor_id;           /**< idVendor from device descriptor.   */
  uint16_t product_id;          /**< idProduct from device descriptor.  */
} ra8_usb_hmsc_device_t;

/**
 * @struct ra8_usb_hmsc_inquiry_response_t
 * @brief Decoded SCSI INQUIRY response.
 *
 * @details Mirrors SBC-4 sec 6.6 standard INQUIRY data layout. The
 * three trailing strings are SPACE-padded, NOT NUL-terminated, on the
 * wire; the driver copies them in unchanged.
 */
typedef struct {
  uint8_t peripheral_qualifier;   /**< Bits [7:5] of byte 0.    */
  uint8_t peripheral_device_type; /**< Bits [4:0] of byte 0.    */
  uint8_t removable;              /**< 1 = removable medium.    */
  uint8_t version;                /**< SPC version code.        */
  uint8_t vendor_id[8];           /**< T10 Vendor ID, ASCII.    */
  uint8_t product_id[16];         /**< Product ID, ASCII.       */
  uint8_t product_revision[4];    /**< Product revision, ASCII. */
} ra8_usb_hmsc_inquiry_response_t;

/**
 * @typedef ra8_usb_hmsc_attach_fn_t
 * @brief Attach-callback signature.
 *
 * @param[in] ctx Caller-supplied context registered with
 *                `ra8_usb_hmsc_attach_callback`.
 * @param[in] device Snapshot of the attached MSC device. The pointer
 *                   remains valid only for the duration of the call;
 *                   copy out anything you need.
 *
 * @note Invoked from the dispatch site (typically ISR context) after
 * enumeration completes successfully.
 */
typedef void (*ra8_usb_hmsc_attach_fn_t)(void* ctx, const ra8_usb_hmsc_device_t* device);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Bring up the host-MSC driver on a chosen USB controller.
 *
 * @details
 * Initialises the underlying `ra8_usb` driver in HOST mode for
 * `speed`, primes the BOT tag counter, leaves the bus in the "wait
 * for attach" state (UACT cleared), and arms the internal enumeration
 * step machine.
 *
 * @param[in] speed Which USB controller (FS or HS).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Host-MSC ready, awaiting attach.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 * @retval k_ra8_err_hw_init_failed Underlying `ra8_usb_host_init`
 *         failed.
 *
 * @pre Single-threaded init context.
 * @pre `ra8_mstp_init` and `ra8_pwr_init` already ran.
 *
 * @post `ra8_usb_host_init` succeeded for `speed`.
 * @post Internal step machine armed; attach callback has not fired.
 *
 * @note Not thread-safe.
 * @see ra8_usb_hmsc_attach_callback
 * @see ra8_usb_hmsc_close
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hmsc_init(ra8_usb_speed_t speed);

/**
 * @brief Tear down the host-MSC driver and release the controller.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Released.
 * @retval k_ra8_err_invalid_state Driver was never initialized.
 *
 * @pre Single-threaded shutdown context.
 *
 * @post `ra8_usb_host_deinit` ran; SOF generation halted; bus power
 *       dropped; subsequent host-MSC API calls return
 *       `k_ra8_err_invalid_state`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hmsc_close(void);

/**
 * @brief Enumerate the attached MSC device end to end (polled).
 *
 * @details
 * Runs the hardware-proven ladder on the controller selected by
 * `ra8_usb_hmsc_init`: waits for the D+ attach, hunts the (reset, address)
 * combination the device answers at, assigns address 1 when it sat at
 * the default, reads + parses the configuration descriptor for the MSC
 * BOT interface and its bulk endpoints, issues SET_CONFIGURATION and a
 * best-effort GET_MAX_LUN, and configures the bulk pipes. On success the
 * registered attach callback fires with the device snapshot and the SCSI
 * entry points (`ra8_usb_hmsc_inquiry` / `_read_capacity` / `_read10` /
 * `_write10`) are ready.
 *
 * @param[out] out_device Optional copy of the device snapshot (may be
 *                        NULL).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok                Device enumerated and pipes configured.
 * @retval k_ra8_err_invalid_state `ra8_usb_hmsc_init` has not run.
 * @retval k_ra8_err_hw_timeout    Nothing attached / nothing answered.
 * @retval k_ra8_err_hw_error      A stage failed (STALL / short response).
 *
 * @pre `ra8_usb_hmsc_init` succeeded and VBUS reaches the device.
 * @pre `ra8_time_init` has run (the ladder uses millisecond delays).
 *
 * @post On success the attach callback (if registered) has fired.
 * @post The bulk pipes target the device's MSC endpoints.
 *
 * @note Blocking; bounded by the ladder timeouts (a few seconds max).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hmsc_enumerate(ra8_usb_hmsc_device_t* out_device);

/* =============================================================================
 * Attach callback
 * =============================================================================
 */

/**
 * @brief Register (or detach) the attach callback.
 *
 * @details
 * The supplied callback fires exactly once per attach event, after
 * the descriptor walk identifies an MSC interface (class=0x08 /
 * subclass=0x06 SCSI / protocol=0x50 BBB) and the Get-Max-LUN class
 * request lands. Pass `NULL` to detach.
 *
 * @param[in] on_attach Callback. NULL detaches.
 * @param[in] ctx Context pointer threaded back into `on_attach`.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Callback installed.
 * @retval k_ra8_err_invalid_state Driver was never initialized.
 *
 * @pre `ra8_usb_hmsc_init` has run.
 *
 * @post On a subsequent attach, `on_attach(ctx, &device)` fires once.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hmsc_attach_callback(ra8_usb_hmsc_attach_fn_t on_attach, void* ctx);

/* =============================================================================
 * SCSI commands (Bulk-Only Transport)
 * =============================================================================
 */

/**
 * @brief Issue a SCSI INQUIRY (opcode 0x12) over BBB.
 *
 * @details
 * Builds a 6-byte SCSI CDB, wraps it in a 31-byte CBW (signature
 * 'USBC' / 0x43425355), pushes CBW on bulk-OUT, drains the 36-byte
 * response on bulk-IN, then drains the 13-byte CSW (signature 'USBS'
 * / 0x53425355). On success `*response` is filled in.
 *
 * @param[in] target_lun Logical unit (0..max_lun).
 * @param[out] response Decoded INQUIRY response.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok INQUIRY succeeded.
 * @retval k_ra8_err_null_ptr `response` was NULL.
 * @retval k_ra8_err_invalid_state Driver not initialized, or no device
 *         attached.
 * @retval k_ra8_err_invalid_arg `target_lun` out of range.
 * @retval k_ra8_err_hw_error BBB transfer failed (CSW status != 0).
 *
 * @pre `ra8_usb_hmsc_init` succeeded.
 * @pre Attach callback already fired.
 *
 * @post On success, `*response` reflects the device's INQUIRY data.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hmsc_inquiry(uint8_t                          target_lun,
                                             ra8_usb_hmsc_inquiry_response_t* response);

/**
 * @brief Issue a SCSI READ CAPACITY(10) (opcode 0x25) over BBB.
 *
 * @details
 * Reads the device's logical block count and block size. The 8-byte
 * response holds the LAST valid LBA (block count - 1) and the block
 * size, both big-endian on the wire; this function decodes both and
 * adds 1 to the LBA so `*block_count` is the inclusive total.
 *
 * @param[in] target_lun Logical unit (0..max_lun).
 * @param[out] block_count Total number of blocks on the LUN.
 * @param[out] block_size Block size in bytes (typically 512).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Capacity read.
 * @retval k_ra8_err_null_ptr Either pointer was NULL.
 * @retval k_ra8_err_invalid_state Driver not initialized, or no device
 *         attached.
 * @retval k_ra8_err_invalid_arg `target_lun` out of range.
 * @retval k_ra8_err_hw_error BBB transfer failed.
 *
 * @pre `ra8_usb_hmsc_init` succeeded.
 * @pre Attach callback already fired.
 *
 * @post On success, both outputs are non-zero.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_usb_hmsc_read_capacity(uint8_t target_lun, uint32_t* block_count, uint32_t* block_size);

/**
 * @brief Issue a SCSI READ(10) (opcode 0x28) over BBB.
 *
 * @details
 * Reads `block_count` contiguous blocks starting at `lba` into
 * `out_buf`. Caller is responsible for sizing `out_buf` to at least
 * `block_count * block_size` bytes (use the value returned by
 * `ra8_usb_hmsc_read_capacity`).
 *
 * @param[in] target_lun Logical unit (0..max_lun).
 * @param[in] lba Starting logical block address.
 * @param[in] block_count Number of blocks to read (1..65535).
 * @param[out] out_buf Destination buffer, sized appropriately.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Read complete.
 * @retval k_ra8_err_null_ptr `out_buf` was NULL.
 * @retval k_ra8_err_invalid_state Driver not initialized, or no device
 *         attached.
 * @retval k_ra8_err_invalid_arg Argument out of range (zero block
 *         count, bogus LUN).
 * @retval k_ra8_err_hw_error BBB transfer failed.
 *
 * @pre `ra8_usb_hmsc_init` succeeded.
 * @pre Attach callback already fired.
 *
 * @post On success, `out_buf` holds the read data.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_usb_hmsc_read10(uint8_t target_lun, uint32_t lba, uint16_t block_count, uint8_t* out_buf);

/**
 * @brief Issue a SCSI WRITE(10) (opcode 0x2A) over BBB.
 *
 * @details
 * Writes `block_count` contiguous blocks starting at `lba` from
 * `in_buf`. Caller is responsible for sizing `in_buf` to at least
 * `block_count * block_size` bytes.
 *
 * @param[in] target_lun Logical unit (0..max_lun).
 * @param[in] lba Starting logical block address.
 * @param[in] block_count Number of blocks to write (1..65535).
 * @param[in] in_buf Source buffer, sized appropriately.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Write complete.
 * @retval k_ra8_err_null_ptr `in_buf` was NULL.
 * @retval k_ra8_err_invalid_state Driver not initialized, or no device
 *         attached.
 * @retval k_ra8_err_invalid_arg Argument out of range.
 * @retval k_ra8_err_hw_error BBB transfer failed.
 *
 * @pre `ra8_usb_hmsc_init` succeeded.
 * @pre Attach callback already fired.
 *
 * @post On success the device has accepted all `block_count` blocks.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_usb_hmsc_write10(uint8_t target_lun, uint32_t lba, uint16_t block_count, const uint8_t* in_buf);

/* =============================================================================
 * Test / introspection helpers
 * =============================================================================
 */

/**
 * @brief Construct the 31-byte CBW header for the given SCSI command.
 *
 * @details
 * Test / debug entry point. Production code uses this internally; the
 * unit-test build calls it directly to prove the CBW layout (signature
 * 'USBC', tag, dCBWDataTransferLength, bmCBWFlags direction bit, LUN,
 * CDB length, CDB bytes) matches USB MSC BBB rev 1.0 sec 5.1.
 *
 * @param[in] target_lun Logical unit (0..15).
 * @param[in] data_transfer_length dCBWDataTransferLength field.
 * @param[in] data_in `true` for IN (device-to-host), `false` for OUT.
 * @param[in] cdb 6 / 10 / 12-byte SCSI Command Descriptor Block.
 * @param[in] cdb_len Length of the CDB in bytes (1..16).
 * @param[out] out_cbw Receives the 31-byte CBW header.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok CBW constructed.
 * @retval k_ra8_err_null_ptr `cdb` or `out_cbw` was NULL.
 * @retval k_ra8_err_invalid_arg `cdb_len` out of range or
 *         `target_lun` out of range.
 *
 * @pre `cdb`, `out_cbw` non-NULL.
 *
 * @post `out_cbw[0..3]` == `0x55, 0x53, 0x42, 0x43` (CBW signature
 *       'USBC', little-endian on wire).
 * @post `out_cbw[14]` == `cdb_len`.
 *
 * @note Not thread-safe; bumps the internal BOT tag counter.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hmsc_build_cbw(uint8_t        target_lun,
                                               uint32_t       data_transfer_length,
                                               bool           data_in,
                                               const uint8_t* cdb,
                                               uint8_t        cdb_len,
                                               uint8_t*       out_cbw);

/**
 * @brief Decode a 13-byte CSW.
 *
 * @details
 * Test / debug entry point. Validates the signature ('USBS'), the
 * tag echo, and returns the decoded status byte.
 *
 * @param[in] csw Pointer to the 13-byte CSW buffer.
 * @param[in] expected_tag dCBWTag from the CBW that initiated the
 *                         transfer.
 * @param[out] out_status Decoded CSW status byte.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Signature + tag match; `*out_status` valid.
 * @retval k_ra8_err_null_ptr Either pointer was NULL.
 * @retval k_ra8_err_invalid_arg Signature mismatch or tag mismatch.
 *
 * @pre `csw`, `out_status` non-NULL.
 *
 * @post On success `*out_status` is one of
 *       `k_ra8_hmsc_csw_status_passed`, `..._failed`, `..._phase_error`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_hmsc_decode_csw(const uint8_t*             csw,
                                                uint32_t                   expected_tag,
                                                ra8_usb_hmsc_csw_status_t* out_status);

#ifdef __cplusplus
}
#endif
