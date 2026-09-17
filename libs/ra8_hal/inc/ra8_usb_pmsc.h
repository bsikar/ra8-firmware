/**
 * @file ra8_usb_pmsc.h
 * @brief Native USB device-side MSC (Mass Storage Class) class layer
 * @ingroup grp_hal_usb
 *
 * @details
 * Mirrors FSP's `r_usb_pmsc` peripheral-mode Mass Storage Class
 * driver. With this layer the EK-RA8D2 can present an internal SRAM
 * region, an SD card, or any other block device the application wires
 * up as a USB drive to a host.
 *
 * Lifecycle (mirrors `ra8_usb_cdc.h` and `ra8_usb_hmsc.h` shape):
 *
 *  1. `ra8_usb_pmsc_init(speed)` flips the controller to device mode,
 *     configures the bulk-IN / bulk-OUT pipes for MSC, primes the
 *     BOT (Bulk-Only-Transport) state machine to the IDLE state, and
 *     leaves D+ pull-up off so the application can finalise its
 *     descriptor table before advertising on the bus.
 *  2. `ra8_usb_pmsc_attach_storage(storage)` registers a caller-owned
 *     storage backend. The four function-pointer hooks let the
 *     class layer answer the SCSI commands (INQUIRY,
 *     READ_CAPACITY(10), READ(10), WRITE(10), TEST_UNIT_READY,
 *     REQUEST_SENSE, MODE_SENSE(6)) that the host issues after
 *     enumeration. The storage layer can be SRAM, SD/MMC, OctoSPI
 *     flash, or anything that exposes block read / block write.
 *  3. `ra8_usb_pmsc_step()` is invoked by the application from the
 *     bulk-OUT completion ISR. Each call pumps the BOT state
 *     machine: IDLE -> CBW_RX -> CDB_DECODE -> DATA_IN/OUT ->
 *     CSW_TX -> IDLE.
 *  4. `ra8_usb_pmsc_close()` drops the bulk pipes and tears down the
 *     device-mode controller.
 *
 * Class-specific control requests (USB MSC BBB rev 1.0 sec 3.1):
 *
 *  - `Get Max LUN` (bmRequestType=0xA1 / bRequest=0xFE / wLength=1):
 *    the starter is single-LUN, so it returns 0.
 *  - `Mass Storage Reset` (bmRequestType=0x21 / bRequest=0xFF /
 *    wLength=0): resets the BOT state machine to IDLE.
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
 * @enum ra8_usb_pmsc_pipe_t
 * @brief PIPE numbers used by the device-MSC driver for the local
 *        bulk endpoints.
 *
 * @details FSP / RA8D2 PIPE assignment rules constrain bulk pipes to
 * PIPE1..PIPE5. The device-MSC class uses two pipes: bulk-IN to push
 * SCSI data + CSW back to the host, bulk-OUT to receive CBW + data
 * from the host. Pipes 3 / 4 mirror the host-MSC layer so a single
 * controller can switch roles without re-keying the pipe table.
 */
typedef enum : uint8_t {
  k_ra8_pmsc_pipe_bulk_in  = 3U, /**< PIPE3 -> local EP bulk IN.  */
  k_ra8_pmsc_pipe_bulk_out = 4U, /**< PIPE4 -> local EP bulk OUT. */
} ra8_usb_pmsc_pipe_t;

/**
 * @enum ra8_usb_pmsc_endpoint_t
 * @brief USB endpoint numbers advertised in the configuration
 *        descriptor.
 *
 * @details Bulk-IN at EP1, bulk-OUT at EP2; matches the layout most
 * stock MSC reference designs ship with.
 */
typedef enum : uint8_t {
  k_ra8_pmsc_ep_bulk_in  = 1U, /**< Bulk-IN endpoint number.  */
  k_ra8_pmsc_ep_bulk_out = 2U, /**< Bulk-OUT endpoint number. */
} ra8_usb_pmsc_endpoint_t;

/**
 * @enum ra8_usb_pmsc_packet_t
 * @brief Packet sizing for the local bulk endpoints.
 */
typedef enum : uint16_t {
  k_ra8_pmsc_bulk_max_packet_fs = 64U,  /**< Bulk size at full speed. */
  k_ra8_pmsc_bulk_max_packet_hs = 512U, /**< Bulk size at high speed. */
} ra8_usb_pmsc_packet_t;

/**
 * @enum ra8_usb_pmsc_class_t
 * @brief Class / subclass / protocol triplet advertised in the
 *        device's interface descriptor.
 *
 * @details Per the USB-IF "Class Codes" registry and the USB MSC
 * overview spec rev 1.4. Subclass 0x06 = transparent SCSI; protocol
 * 0x50 = Bulk-Only Transport (BBB).
 */
typedef enum : uint8_t {
  k_ra8_pmsc_class_msc     = 0x08U, /**< MSC interface class.       */
  k_ra8_pmsc_subclass_scsi = 0x06U, /**< Transparent SCSI subclass. */
  k_ra8_pmsc_protocol_bbb  = 0x50U, /**< Bulk-Only Transport.       */
} ra8_usb_pmsc_class_t;

/**
 * @enum ra8_usb_pmsc_request_t
 * @brief MSC class-specific request codes the host issues to the
 *        device.
 *
 * @details Per USB MSC BBB rev 1.0 sec 3.1 "Class-Specific Requests".
 */
typedef enum : uint8_t {
  k_ra8_pmsc_req_mass_storage_reset = 0xFFU, /**< 0-byte payload.  */
  k_ra8_pmsc_req_get_max_lun        = 0xFEU, /**< 1-byte response. */
} ra8_usb_pmsc_request_t;

/**
 * @enum ra8_usb_pmsc_max_lun_t
 * @brief Number of logical units the starter exposes.
 *
 * @details Single-LUN device. Get-Max-LUN therefore returns 0 (the
 * highest valid LUN index).
 */
typedef enum : uint8_t {
  k_ra8_pmsc_max_lun = 0U, /**< Highest valid LUN index. */
} ra8_usb_pmsc_max_lun_t;

/**
 * @enum ra8_usb_pmsc_csw_status_t
 * @brief CSW status field values the device returns after a CBW.
 *
 * @details Per USB MSC BBB rev 1.0 sec 5.2 "Command Status Wrapper".
 */
typedef enum : uint8_t {
  k_ra8_pmsc_csw_status_passed      = 0x00U, /**< Command succeeded. */
  k_ra8_pmsc_csw_status_failed      = 0x01U, /**< Command failed.    */
  k_ra8_pmsc_csw_status_phase_error = 0x02U, /**< BBB phase error -- the
                                                  host must run reset
                                                  recovery.            */
} ra8_usb_pmsc_csw_status_t;

/**
 * @enum ra8_usb_pmsc_scsi_t
 * @brief SCSI opcodes the device-MSC class answers over BBB.
 *
 * @details A small subset of SCSI Primary Commands rev 4 + SCSI
 * Block Commands rev 4. Other opcodes generate a CSW with status
 * `k_ra8_pmsc_csw_status_failed` and a sense code of "Invalid Command
 * Operation Code".
 */
typedef enum : uint8_t {
  k_ra8_pmsc_scsi_test_unit_ready  = 0x00U, /**< TEST UNIT READY.   */
  k_ra8_pmsc_scsi_request_sense    = 0x03U, /**< REQUEST SENSE.     */
  k_ra8_pmsc_scsi_inquiry          = 0x12U, /**< INQUIRY.           */
  k_ra8_pmsc_scsi_mode_sense_6     = 0x1AU, /**< MODE SENSE(6).     */
  k_ra8_pmsc_scsi_read_capacity_10 = 0x25U, /**< READ CAPACITY(10). */
  k_ra8_pmsc_scsi_read_10          = 0x28U, /**< READ(10).          */
  k_ra8_pmsc_scsi_write_10         = 0x2AU, /**< WRITE(10).         */
} ra8_usb_pmsc_scsi_t;

/**
 * @enum ra8_usb_pmsc_resp_size_t
 * @brief Standard SCSI response payload sizes.
 *
 * @details The SCSI INQUIRY response is fixed at 36 bytes (5+31, see
 * SBC-4 sec 6.6); READ_CAPACITY(10) is 8 bytes (4-byte returned-LBA
 * + 4-byte block-length, SBC-4 sec 5.10); REQUEST SENSE is 18 bytes
 * (SPC-4 sec 6.30); MODE SENSE(6) is 4 bytes for the minimal
 * "header-only" reply.
 */
typedef enum : uint16_t {
  k_ra8_pmsc_inquiry_resp_len       = 36U,  /**< INQUIRY response len.   */
  k_ra8_pmsc_read_capacity_resp_len = 8U,   /**< READ_CAPACITY(10).      */
  k_ra8_pmsc_request_sense_resp_len = 18U,  /**< REQUEST SENSE response. */
  k_ra8_pmsc_mode_sense_resp_len    = 4U,   /**< MODE SENSE(6) response. */
  k_ra8_pmsc_block_size_default     = 512U, /**< SCSI default block.     */
} ra8_usb_pmsc_resp_size_t;

/**
 * @enum ra8_usb_pmsc_inquiry_field_t
 * @brief Sizes of the three INQUIRY ASCII strings the storage backend
 *        returns.
 *
 * @details Per SBC-4 sec 6.6 the T10 vendor ID is 8 bytes, the
 * product ID is 16 bytes, and the product revision is 4 bytes. All
 * three are SPACE-padded, NOT NUL-terminated, on the wire.
 */
typedef enum : uint8_t {
  k_ra8_pmsc_inq_vendor_len   = 8U,  /**< Vendor ID byte count.  */
  k_ra8_pmsc_inq_product_len  = 16U, /**< Product ID byte count. */
  k_ra8_pmsc_inq_revision_len = 4U,  /**< Revision byte count.   */
} ra8_usb_pmsc_inquiry_field_t;

/* =============================================================================
 * Storage backend interface (Dependency Inversion)
 * =============================================================================
 */

/**
 * @typedef ra8_usb_pmsc_read_block_fn_t
 * @brief Storage-backend read hook.
 *
 * @param[in] ctx Caller-supplied context registered with
 *                `ra8_usb_pmsc_attach_storage`.
 * @param[in] lba Starting logical block address.
 * @param[in] block_count Number of blocks to read (1..65535).
 * @param[out] buf Destination buffer, sized to at least
 *                 `block_count * block_size` bytes.
 *
 * @return `ra8_err_t` error code. Non-zero failures land in the CSW
 *         as `k_ra8_pmsc_csw_status_failed`.
 */
typedef ra8_err_t (*ra8_usb_pmsc_read_block_fn_t)(void*    ctx,
                                                  uint32_t lba,
                                                  uint32_t block_count,
                                                  uint8_t* buf);

/**
 * @typedef ra8_usb_pmsc_write_block_fn_t
 * @brief Storage-backend write hook.
 *
 * @param[in] ctx Caller-supplied context.
 * @param[in] lba Starting logical block address.
 * @param[in] block_count Number of blocks to write (1..65535).
 * @param[in] buf Source buffer, sized to at least
 *                `block_count * block_size` bytes.
 *
 * @return `ra8_err_t` error code.
 */
typedef ra8_err_t (*ra8_usb_pmsc_write_block_fn_t)(void*          ctx,
                                                   uint32_t       lba,
                                                   uint32_t       block_count,
                                                   const uint8_t* buf);

/**
 * @typedef ra8_usb_pmsc_get_capacity_fn_t
 * @brief Storage-backend capacity hook -- answers SCSI READ
 *        CAPACITY(10).
 *
 * @param[in] ctx Caller-supplied context.
 * @param[out] block_count Total block count (NOT block_count - 1).
 * @param[out] block_size Block size in bytes (typically 512).
 *
 * @return `ra8_err_t` error code.
 */
typedef ra8_err_t (*ra8_usb_pmsc_get_capacity_fn_t)(void*     ctx,
                                                    uint32_t* block_count,
                                                    uint32_t* block_size);

/**
 * @typedef ra8_usb_pmsc_get_inquiry_fn_t
 * @brief Storage-backend INQUIRY hook -- answers SCSI INQUIRY.
 *
 * @details The driver populates the standard fields (peripheral
 * device type, removable bit, SPC version, response data format)
 * and copies the three caller-supplied ASCII strings into byte
 * positions 8..15, 16..31, and 32..35 of the 36-byte INQUIRY
 * response.
 *
 * @param[in] ctx Caller-supplied context.
 * @param[out] vendor8 8-byte T10 vendor ID, SPACE-padded.
 * @param[out] product16 16-byte product ID, SPACE-padded.
 * @param[out] revision4 4-byte product revision, SPACE-padded.
 *
 * @return `ra8_err_t` error code.
 */
typedef ra8_err_t (*ra8_usb_pmsc_get_inquiry_fn_t)(void*    ctx,
                                                   uint8_t* vendor8,
                                                   uint8_t* product16,
                                                   uint8_t* revision4);

/**
 * @struct ra8_usb_pmsc_storage_t
 * @brief Caller-owned storage backend bound to the device-MSC class.
 *
 * @details All four function pointers must be non-NULL; the device
 * cannot answer SCSI commands without them. `ctx` is opaque to the
 * driver and threaded back into each callback unchanged.
 *
 * @invariant All four callbacks remain valid for the lifetime of the
 *            attached storage.
 *
 * @code
 * static ra8_err_t my_read(void* ctx, uint32_t lba, uint32_t n, uint8_t* b);
 * static ra8_err_t my_write(void* ctx, uint32_t lba, uint32_t n, const uint8_t* b);
 * static ra8_err_t my_cap(void* ctx, uint32_t* nb, uint32_t* bs);
 * static ra8_err_t my_inq(void* ctx, uint8_t* v, uint8_t* p, uint8_t* r);
 *
 * static const ra8_usb_pmsc_storage_t s_storage = {
 *   .read_block   = my_read,
 *   .write_block  = my_write,
 *   .get_capacity = my_cap,
 *   .get_inquiry  = my_inq,
 *   .ctx          = nullptr,
 * };
 * @endcode
 */
typedef struct {
  ra8_usb_pmsc_read_block_fn_t   read_block;   /**< Block read hook.  */
  ra8_usb_pmsc_write_block_fn_t  write_block;  /**< Block write hook. */
  ra8_usb_pmsc_get_capacity_fn_t get_capacity; /**< Capacity hook.    */
  ra8_usb_pmsc_get_inquiry_fn_t  get_inquiry;  /**< INQUIRY hook.     */
  void*                          ctx;          /**< Opaque context.   */
} ra8_usb_pmsc_storage_t;

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Bring up the device-MSC driver on a chosen USB controller.
 *
 * @details
 * Initialises the underlying `ra8_usb` driver in DEVICE mode for
 * `speed`, configures the bulk-IN / bulk-OUT pipes (PIPE3 / PIPE4)
 * with the speed-appropriate maximum packet size, primes the BOT
 * state machine to the IDLE state, and zeroes the BOT command tag
 * counter. D+ pull-up is left off so the application can publish the
 * configuration descriptor before the host enumerates it.
 *
 * @param[in] speed Which USB controller (FS or HS).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Device-MSC ready, awaiting `attach_storage`.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 * @retval k_ra8_err_hw_init_failed Underlying `ra8_usb_device_init`
 *         failed.
 *
 * @pre Single-threaded init context.
 * @pre `ra8_mstp_init` and `ra8_pwr_init` already ran.
 *
 * @post `ra8_usb_device_init` succeeded for `speed`.
 * @post Internal BOT state machine in IDLE; storage backend NULL.
 *
 * @note Not thread-safe.
 * @see ra8_usb_pmsc_attach_storage
 * @see ra8_usb_pmsc_close
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pmsc_init(ra8_usb_speed_t speed);

/**
 * @brief Tear down the device-MSC driver and release the controller.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Released.
 * @retval k_ra8_err_invalid_state Driver was never initialized.
 *
 * @pre Single-threaded shutdown context.
 *
 * @post `ra8_usb_device_deinit` ran; D+ pull-up dropped; subsequent
 *       device-MSC API calls return `k_ra8_err_invalid_state`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pmsc_close(void);

/* =============================================================================
 * Storage backend
 * =============================================================================
 */

/**
 * @brief Bind a caller-owned storage backend to the device-MSC class.
 *
 * @details
 * The four function pointers in `storage` are validated for NULL and
 * the struct is copied into internal state. After this call the BOT
 * state machine can answer SCSI commands the host issues.
 *
 * @param[in] storage Storage backend snapshot. Copied by value.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Backend installed.
 * @retval k_ra8_err_invalid_state Driver not initialized.
 * @retval k_ra8_err_null_ptr `storage` was NULL or any callback was
 *         NULL.
 *
 * @pre `ra8_usb_pmsc_init` has run.
 * @pre All four callbacks non-NULL.
 *
 * @post Subsequent SCSI commands route through the supplied
 *       callbacks.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pmsc_attach_storage(const ra8_usb_pmsc_storage_t* storage);

/* =============================================================================
 * BOT state machine
 * =============================================================================
 */

/**
 * @brief Drive the BOT (Bulk-Only Transport) state machine forward
 *        by one step.
 *
 * @details
 * Production code calls this from the bulk-OUT completion ISR. The
 * state machine cycles through:
 *
 *  - IDLE: wait for the bulk-OUT pipe to deliver a 31-byte CBW.
 *  - CBW_RX: parse the CBW, validate the 'USBC' signature, decode
 *    the CDB.
 *  - CDB_DECODE: dispatch to one of the SCSI command handlers
 *    (INQUIRY / READ_CAPACITY / READ(10) / WRITE(10) /
 *    TEST_UNIT_READY / REQUEST_SENSE / MODE_SENSE(6)).
 *  - DATA_TX: push read data on bulk-IN.
 *  - DATA_RX: drain write data from bulk-OUT.
 *  - CSW_TX: push the 13-byte CSW on bulk-IN.
 *  - IDLE.
 *
 * Each call advances by exactly one phase; the caller pumps the
 * state machine until it idles back to CBW reception.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Step advanced.
 * @retval k_ra8_err_invalid_state Driver not initialized, or storage
 *         not attached.
 *
 * @pre `ra8_usb_pmsc_init` has run.
 * @pre `ra8_usb_pmsc_attach_storage` has run.
 *
 * @post Internal BOT state machine advances by one phase.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pmsc_step(void);

/* =============================================================================
 * Test / introspection helpers
 * =============================================================================
 */

/**
 * @brief Inject a CBW directly into the BOT state machine.
 *
 * @details
 * Test / debug entry point. Production code receives CBWs from the
 * bulk-OUT pipe; tests bypass the FIFO and feed the 31-byte buffer
 * directly. Returns `k_ra8_err_invalid_arg` on signature mismatch
 * (USB MSC BBB rev 1.0 sec 6.2.1 "Valid CBW").
 *
 * @param[in] cbw Pointer to a 31-byte CBW.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok CBW accepted; state machine now in CDB_DECODE.
 * @retval k_ra8_err_null_ptr `cbw` was NULL.
 * @retval k_ra8_err_invalid_state Driver not initialized, or storage
 *         not attached.
 * @retval k_ra8_err_invalid_arg Signature mismatch (CBW invalid). In
 *         this case the BOT machine transitions to a phase-error
 *         CSW_TX as required by USB MSC BBB rev 1.0 sec 6.6.1
 *         "CBW Not Valid".
 *
 * @pre `cbw` non-NULL.
 *
 * @post On `k_ra8_ok` the cached SCSI opcode and data-length are
 *       valid for `ra8_usb_pmsc_dispatch_command`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pmsc_feed_cbw(const uint8_t* cbw);

/**
 * @brief Run the SCSI command currently parked in the BOT state.
 *
 * @details
 * Test / debug entry point. After `ra8_usb_pmsc_feed_cbw` accepts a
 * CBW, this routine invokes the storage backend (if any) and stages
 * the data-phase output buffer / pulls the CSW status byte. On
 * unsupported opcodes the CSW status is set to
 * `k_ra8_pmsc_csw_status_failed`.
 *
 * @param[out] data_buf Buffer that receives the data-IN payload (or
 *                      provides the data-OUT payload). Caller sized
 *                      to at least `data_buf_capacity` bytes.
 * @param[in] data_buf_capacity Capacity of `data_buf` in bytes.
 * @param[out] data_len Receives the actual data byte count produced
 *                      / consumed.
 * @param[out] csw_status Receives the CSW status byte the device
 *                        will emit at the end of the transaction.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Command dispatched (the CSW status byte may still
 *         be `k_ra8_pmsc_csw_status_failed`).
 * @retval k_ra8_err_null_ptr Any output pointer was NULL.
 * @retval k_ra8_err_invalid_state Driver not initialized, storage
 *         not attached, or no CBW currently in-flight.
 * @retval k_ra8_err_invalid_size `data_buf_capacity` was 0.
 *
 * @pre `ra8_usb_pmsc_feed_cbw` returned `k_ra8_ok` immediately prior.
 *
 * @post On success `*data_len` is the byte count to push / pull and
 *       `*csw_status` is the CSW status byte.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_pmsc_dispatch_command(uint8_t*                   data_buf,
                                                      uint32_t                   data_buf_capacity,
                                                      uint32_t*                  data_len,
                                                      ra8_usb_pmsc_csw_status_t* csw_status);

/**
 * @brief Build the 13-byte CSW for the in-flight command.
 *
 * @details
 * Test / debug entry point. Production code calls this internally
 * during the CSW_TX phase. The CSW is laid out per USB MSC BBB rev
 * 1.0 sec 5.2: dCSWSignature='USBS', dCSWTag echoed from the CBW,
 * dCSWDataResidue computed from the CBW dCBWDataTransferLength
 * minus the actual byte count produced, bCSWStatus from
 * `csw_status`.
 *
 * @param[in] csw_status Status byte to embed in the CSW.
 * @param[in] residue Bytes that were NOT transferred (CBW expected
 *                    minus actual).
 * @param[out] out_csw Receives the 13-byte CSW buffer.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok CSW constructed.
 * @retval k_ra8_err_null_ptr `out_csw` was NULL.
 * @retval k_ra8_err_invalid_state Driver not initialized, or no CBW
 *         currently in-flight.
 *
 * @pre `out_csw` non-NULL.
 * @pre A prior `ra8_usb_pmsc_feed_cbw` set the dCBWTag we echo.
 *
 * @post `out_csw[0..3]` == `0x55, 0x53, 0x42, 0x53` (CSW signature
 *       'USBS', little-endian on wire).
 * @post `out_csw[12]` == `csw_status`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_usb_pmsc_build_csw(ra8_usb_pmsc_csw_status_t csw_status, uint32_t residue, uint8_t* out_csw);

#ifdef __cplusplus
}
#endif
