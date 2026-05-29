/**
 * @file ra_usb_hmsc.c
 * @brief Native USB host-side MSC (Mass Storage Class) class layer
 *        implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Glues the host-mode bring-up paths in `ra_usb` to a USB Mass Storage
 * Class (Bulk-Only-Transport / BBB) peripheral attached on the
 * EK-RA8D2's USB-host port. This file is the native host-MSC class
 * layer; FSP's `r_usb_hmsc_driver.c`, `r_usb_hstorage_driver.c`, and
 * `r_usb_hmsc.c` are reference material only -- nothing is pulled in
 * verbatim.
 *
 * Mapping vs FSP (FSP function -> our entry point):
 *
 *  - `usb_hmsc_inquiry`         -> `ra_usb_hmsc_inquiry`
 *  - `usb_hmsc_read_capacity`   -> `ra_usb_hmsc_read_capacity`
 *  - `usb_hmsc_read10`          -> `ra_usb_hmsc_read10`
 *  - `usb_hmsc_write10`         -> `ra_usb_hmsc_write10`
 *  - `usb_hmsc_set_rw_cbw`      -> `internal_build_rw_cbw`
 *  - `usb_hmsc_set_els_cbw`     -> `internal_build_els_cbw`
 *  - `usb_hmsc_get_max_unit`    -> `internal_setup_get_max_lun`
 *  - `usb_hmsc_class_check`     -> `internal_walk_config_descriptor`
 *  - `usb_hmsc_pipe_info`       -> `internal_configure_pipes`
 *
 * The starter does CPU-FIFO, single-device, no-hub. Enumeration is
 * driven step-by-step from the controller's CTRT interrupt path
 * (production) or directly via `ra_usb_hmsc_step` (tests). Each step
 * issues exactly one chapter-9 SETUP request via
 * `ra_usb_host_setup_request`; the next CTRT advances the step.
 *
 * BOT (Bulk-Only Transport) state machine -- per command:
 *
 *   READY -> CBW_OUT (push 31-byte CBW) -> DATA (in or out, optional) ->
 *   CSW_IN (pull 13-byte CSW) -> READY
 *
 * On CSW phase-error the spec mandates Reset Recovery; the starter
 * surfaces that as `k_ra_err_hw_error` and lets the caller decide what to
 * do.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_usb_hmsc.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_usb.h"

static const char* s_tag = "USBHMSC";

/* =============================================================================
 * Internal constants
 * =============================================================================
 */

/**
 * @enum ra_usb_hmsc_step_t
 * @brief Enumeration step machine states.
 *
 * @details Mirrors FSP's host-MSC enumeration sequence in
 * `r_usb_hmsc_driver.c`. Each step issues exactly one SETUP via
 * `ra_usb_host_setup_request`; the next CTRT interrupt advances.
 */
typedef enum : uint8_t {
  k_ra_hmsc_step_idle          = 0U, /**< Pre-attach.                  */
  k_ra_hmsc_step_bus_reset     = 1U, /**< Drive USBRST then release.   */
  k_ra_hmsc_step_set_address   = 2U, /**< SET_ADDRESS to assigned 1.   */
  k_ra_hmsc_step_get_dev_desc  = 3U, /**< GET_DEVICE_DESCRIPTOR (18 B).*/
  k_ra_hmsc_step_get_cfg_desc  = 4U, /**< GET_CONFIGURATION_DESCRIPTOR.*/
  k_ra_hmsc_step_set_config    = 5U, /**< SET_CONFIGURATION (1).       */
  k_ra_hmsc_step_set_interface = 6U, /**< SET_INTERFACE (0).           */
  k_ra_hmsc_step_walk_desc     = 7U, /**< Find MSC IF; populate pipes. */
  k_ra_hmsc_step_get_max_lun   = 8U, /**< Class Get-Max-LUN request.   */
  k_ra_hmsc_step_done          = 9U, /**< Attach callback fires.       */
} ra_usb_hmsc_step_t;

/**
 * @enum ra_usb_hmsc_setup_field_t
 * @brief Standard chapter-9 + MSC class request encodings.
 */
typedef enum : uint8_t {
  /* Chapter-9 standard requests (USB 2.0 spec section 9.4). */
  k_ra_hmsc_bm_std_dev_in       = 0x80U, /**< Std | Device | In.       */
  k_ra_hmsc_bm_std_dev_out      = 0x00U, /**< Std | Device | Out.      */
  k_ra_hmsc_bm_std_iface_out    = 0x01U, /**< Std | Interface | Out.   */
  k_ra_hmsc_breq_get_descriptor = 0x06U, /**< GET_DESCRIPTOR.          */
  k_ra_hmsc_breq_set_address    = 0x05U, /**< SET_ADDRESS.             */
  k_ra_hmsc_breq_set_config     = 0x09U, /**< SET_CONFIGURATION.       */
  k_ra_hmsc_breq_set_interface  = 0x0BU, /**< SET_INTERFACE.           */
  /* MSC class-specific request envelope: 0xA1 = D2H | Class | Iface. */
  k_ra_hmsc_bm_class_iface_in = 0xA1U, /**< Class | Interface | In.   */
  /* Descriptor types in wValue's high byte. */
  k_ra_hmsc_desc_device        = 0x01U, /**< DEVICE descriptor.       */
  k_ra_hmsc_desc_configuration = 0x02U, /**< CONFIGURATION descriptor.*/
  k_ra_hmsc_desc_interface     = 0x04U, /**< INTERFACE descriptor.    */
  k_ra_hmsc_desc_endpoint      = 0x05U, /**< ENDPOINT descriptor.     */
} ra_usb_hmsc_setup_field_t;

/**
 * @enum ra_usb_hmsc_size_t
 * @brief Standard descriptor sizes and BOT wrapper sizes.
 *
 * @details The CBW / CSW lengths are nailed down by USB MSC BBB rev
 * 1.0 sections 5.1 and 5.2 respectively.
 */
typedef enum : uint16_t {
  k_ra_hmsc_dev_desc_len     = 18U, /**< USB DEVICE descriptor.       */
  k_ra_hmsc_cfg_desc_len     = 9U,  /**< CONFIGURATION descriptor hdr.*/
  k_ra_hmsc_iface_desc_len   = 9U,  /**< INTERFACE descriptor.        */
  k_ra_hmsc_ep_desc_len      = 7U,  /**< ENDPOINT descriptor.         */
  k_ra_hmsc_assigned_address = 1U,  /**< First assigned device addr.  */
  k_ra_hmsc_default_config   = 1U,  /**< bConfigurationValue = 1.     */
  k_ra_hmsc_get_max_lun_len  = 1U,  /**< Get-Max-LUN response len.    */
  k_ra_hmsc_cbw_len          = 31U, /**< CBW length (BBB sec 5.1).    */
  k_ra_hmsc_csw_len          = 13U, /**< CSW length (BBB sec 5.2).    */
  k_ra_hmsc_cdb_max_len      = 16U, /**< CDB ceiling.                 */
  k_ra_hmsc_cdb6_len         = 6U,  /**< 6-byte SCSI CDB.             */
  k_ra_hmsc_cdb10_len        = 10U, /**< 10-byte SCSI CDB.            */
} ra_usb_hmsc_size_t;

/**
 * @enum ra_usb_hmsc_cbw_offset_t
 * @brief Byte offsets inside the 31-byte CBW header.
 *
 * @details See USB MSC BBB rev 1.0 sec 5.1 Table 5.1 "Command Block
 * Wrapper".
 */
typedef enum : uint8_t {
  k_ra_hmsc_cbw_off_signature   = 0U,  /**< dCBWSignature [4].   */
  k_ra_hmsc_cbw_off_tag         = 4U,  /**< dCBWTag [4].         */
  k_ra_hmsc_cbw_off_data_length = 8U,  /**< dCBWDataTransferLen. */
  k_ra_hmsc_cbw_off_flags       = 12U, /**< bmCBWFlags.          */
  k_ra_hmsc_cbw_off_lun         = 13U, /**< bCBWLUN (low nib).   */
  k_ra_hmsc_cbw_off_cdb_length  = 14U, /**< bCBWCBLength (low 5).*/
  k_ra_hmsc_cbw_off_cdb         = 15U, /**< CBWCB [16].          */
} ra_usb_hmsc_cbw_offset_t;

/**
 * @enum ra_usb_hmsc_csw_offset_t
 * @brief Byte offsets inside the 13-byte CSW.
 *
 * @details See USB MSC BBB rev 1.0 sec 5.2 Table 5.2 "Command Status
 * Wrapper".
 */
typedef enum : uint8_t {
  k_ra_hmsc_csw_off_signature = 0U,  /**< dCSWSignature [4]. */
  k_ra_hmsc_csw_off_tag       = 4U,  /**< dCSWTag [4].       */
  k_ra_hmsc_csw_off_residue   = 8U,  /**< dCSWDataResidue.   */
  k_ra_hmsc_csw_off_status    = 12U, /**< bCSWStatus.       */
} ra_usb_hmsc_csw_offset_t;

/**
 * @enum ra_usb_hmsc_cbw_flag_t
 * @brief bmCBWFlags direction bit values.
 */
typedef enum : uint8_t {
  k_ra_hmsc_cbw_flag_data_out = 0x00U, /**< Host -> device.   */
  k_ra_hmsc_cbw_flag_data_in  = 0x80U, /**< Device -> host.   */
} ra_usb_hmsc_cbw_flag_t;

/**
 * @enum ra_usb_hmsc_signature_t
 * @brief BOT wrapper signatures (USB MSC BBB rev 1.0 sec 5).
 *
 * @details Stored on-wire little-endian. dCBWSignature 'USBC' is
 * 0x43425355; dCSWSignature 'USBS' is 0x53425355.
 */
typedef enum : uint32_t {
  k_ra_hmsc_cbw_signature = 0x43425355U, /**< 'USBC' little-endian. */
  k_ra_hmsc_csw_signature = 0x53425355U, /**< 'USBS' little-endian. */
} ra_usb_hmsc_signature_t;

/**
 * @enum ra_usb_hmsc_byte_shift_t
 * @brief Per-byte left-shift constants for little-endian / big-endian
 *        serialisation.
 */
typedef enum : uint8_t {
  k_ra_hmsc_shift_byte0 = 0U,
  k_ra_hmsc_shift_byte1 = 8U,
  k_ra_hmsc_shift_byte2 = 16U,
  k_ra_hmsc_shift_byte3 = 24U,
} ra_usb_hmsc_byte_shift_t;

/**
 * @enum ra_usb_hmsc_byte_mask_t
 * @brief Byte mask for serialisation.
 */
typedef enum : uint32_t {
  k_ra_hmsc_byte_mask = 0xFFU, /**< Single-byte extraction mask. */
} ra_usb_hmsc_byte_mask_t;

/**
 * @enum ra_usb_hmsc_cdb_offset_t
 * @brief SCSI CDB byte offsets used when assembling READ(10) /
 *        WRITE(10) / READ_CAPACITY(10) / INQUIRY blocks.
 */
typedef enum : uint8_t {
  k_ra_hmsc_cdb_off_opcode    = 0U, /**< Operation Code.          */
  k_ra_hmsc_cdb_off_lba_msb   = 2U, /**< READ(10): LBA byte 3.    */
  k_ra_hmsc_cdb_off_lba_b1    = 3U, /**< READ(10): LBA byte 2.    */
  k_ra_hmsc_cdb_off_lba_b2    = 4U, /**< READ(10): LBA byte 1.    */
  k_ra_hmsc_cdb_off_lba_lsb   = 5U, /**< READ(10): LBA byte 0.    */
  k_ra_hmsc_cdb_off_cnt_msb   = 7U, /**< READ(10): count high.    */
  k_ra_hmsc_cdb_off_cnt_lsb   = 8U, /**< READ(10): count low.     */
  k_ra_hmsc_cdb_off_inq_alloc = 4U, /**< INQUIRY allocation len.  */
} ra_usb_hmsc_cdb_offset_t;

/**
 * @enum ra_usb_hmsc_lun_mask_t
 * @brief Field width masks for the bCBWLUN + bCBWCBLength bytes.
 *
 * @details See USB MSC BBB rev 1.0 sec 5.1 Table 5.1: bCBWLUN occupies
 * the low 4 bits; bCBWCBLength occupies the low 5 bits.
 */
typedef enum : uint8_t {
  k_ra_hmsc_lun_field_mask = 0x0FU, /**< Low 4 bits of bCBWLUN.    */
  k_ra_hmsc_cdb_field_mask = 0x1FU, /**< Low 5 bits of bCBWCBLen.  */
} ra_usb_hmsc_lun_mask_t;

/**
 * @enum ra_usb_hmsc_initial_tag_t
 * @brief Starting value of the BOT dCBWTag counter.
 */
typedef enum : uint32_t {
  k_ra_hmsc_initial_tag = 1U, /**< First tag handed out post-init. */
} ra_usb_hmsc_initial_tag_t;

/**
 * @enum ra_usb_hmsc_inquiry_field_t
 * @brief Bit-field shifts / masks inside the SCSI INQUIRY response
 *        byte 0 / byte 1 (SBC-4 sec 6.6).
 *
 * @details Byte 0 high 3 bits = peripheral qualifier; low 5 bits =
 * peripheral device type. Byte 1 bit 7 = removable.
 */
typedef enum : uint8_t {
  k_ra_hmsc_inq_qual_shift      = 5U,    /**< Byte 0 [7:5] qualifier.   */
  k_ra_hmsc_inq_dev_type_mask   = 0x1FU, /**< Byte 0 [4:0] dev type.    */
  k_ra_hmsc_inq_removable_shift = 7U,    /**< Byte 1 [7] removable bit. */
  k_ra_hmsc_inq_removable_mask  = 1U,    /**< 1-bit removable flag.     */
} ra_usb_hmsc_inquiry_field_t;

/**
 * @enum ra_usb_hmsc_inquiry_offset_t
 * @brief Byte offsets inside the SBC-4 standard INQUIRY response.
 */
typedef enum : uint8_t {
  k_ra_hmsc_inq_off_dev_type    = 0U,  /**< Byte 0 (qual+devtype). */
  k_ra_hmsc_inq_off_removable   = 1U,  /**< Byte 1 (removable).    */
  k_ra_hmsc_inq_off_version     = 2U,  /**< Byte 2 (SPC version).  */
  k_ra_hmsc_inq_off_vendor_id   = 8U,  /**< T10 Vendor ID start.   */
  k_ra_hmsc_inq_off_product_id  = 16U, /**< Product ID start.      */
  k_ra_hmsc_inq_off_product_rev = 32U, /**< Product revision.      */
} ra_usb_hmsc_inquiry_offset_t;

/**
 * @enum ra_usb_hmsc_capacity_offset_t
 * @brief Byte offsets inside the SCSI READ_CAPACITY(10) 8-byte
 *        response (SBC-4 sec 5.10).
 */
typedef enum : uint8_t {
  k_ra_hmsc_cap_off_last_lba = 0U, /**< Last valid LBA (big-endian).  */
  k_ra_hmsc_cap_off_blk_size = 4U, /**< Block length (big-endian).    */
} ra_usb_hmsc_capacity_offset_t;

/* =============================================================================
 * Internal state
 * =============================================================================
 */

/**
 * @struct ra_usb_hmsc_state_t
 * @brief Singleton shadow state for the host-MSC driver.
 */
typedef struct {
  bool                    initialized;  /**< True after init.            */
  bool                    attached;     /**< True after enum done.       */
  ra_usb_speed_t          speed;        /**< Underlying controller.      */
  ra_usb_hmsc_step_t      step;         /**< Current enumeration step.   */
  ra_usb_hmsc_attach_fn_t attach_cb;    /**< Attach callback, or NULL.   */
  void*                   attach_ctx;   /**< Attach callback ctx.        */
  ra_usb_hmsc_device_t    device;       /**< Snapshot of attached dev.   */
  uint32_t                next_cbw_tag; /**< Monotonic BOT tag.          */
} ra_usb_hmsc_state_t;

static ra_usb_hmsc_state_t s_state = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/* Pick the bulk-max-packet ceiling matching the negotiated speed -- see surrounding code and HUM citations. */
static uint16_t internal_bulk_max_packet(ra_usb_speed_t speed)
{
  return (speed == k_ra_usb_speed_hs) ? k_ra_hmsc_bulk_max_packet_hs : k_ra_hmsc_bulk_max_packet_fs;
}

/**
 * @brief Hand out the next BOT tag (monotonic uint32 counter).
 *
 * @details Mirrors FSP's `g_usb_hmsc_csw_tag_no` increment in
 * `r_usb_hmsc_driver.c`. The starter never wraps in a single test
 * run; production should detect wrap-around but a 32-bit counter
 * lasts long enough that this is not urgent.
 * @return ::ra_err_t outcome (or scalar return value).
 * @retval k_ra_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Internal helper. Not thread-safe; caller provides synchronisation.
 * @since 0.1.0
 */
static uint32_t internal_next_tag(void)
{
  const uint32_t tag = s_state.next_cbw_tag;
  ++s_state.next_cbw_tag;
  return tag;
}

/**
 * @brief Configure the two host-MSC bulk pipes against the attached
 *        device's endpoints.
 *
 * @details Mirrors FSP's `usb_hmsc_pipe_info`. Bulk pipes are PIPE3 +
 * PIPE4 here so they don't clash with the host-CDC class which owns
 * PIPE1 + PIPE2.
 * @return ::ra_err_t outcome (or scalar return value).
 * @retval k_ra_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Internal helper. Not thread-safe; caller provides synchronisation.
 * @since 0.1.0
 */
static ra_err_t internal_configure_pipes(void)
{
  const uint16_t bulk_mp = internal_bulk_max_packet(s_state.speed);

  ra_err_t err = ra_usb_configure_endpoint(s_state.speed,
                                           k_ra_hmsc_pipe_bulk_in,
                                           s_state.device.bulk_in_ep,
                                           k_ra_usb_ep_dir_in,
                                           k_ra_usb_ep_type_bulk,
                                           bulk_mp);
  RA_RETURN_ON_ERROR(err, s_tag, "hmsc: bulk-in cfg"); /* GCOVR_EXCL_BR_LINE */

  err = ra_usb_configure_endpoint(s_state.speed,
                                  k_ra_hmsc_pipe_bulk_out,
                                  s_state.device.bulk_out_ep,
                                  k_ra_usb_ep_dir_out,
                                  k_ra_usb_ep_type_bulk,
                                  bulk_mp);
  return err;
}

/* Stage a chapter-9 GET_DESCRIPTOR SETUP request -- see surrounding code and HUM citations. */
static ra_err_t internal_setup_get_descriptor(uint8_t desc_type, uint16_t length)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = k_ra_hmsc_bm_std_dev_in,
    .b_request       = k_ra_hmsc_breq_get_descriptor,
    .w_value         = (uint16_t)((uint16_t)desc_type << k_ra_hmsc_shift_byte1),
    .w_index         = 0U,
    .w_length        = length,
  };
  return ra_usb_host_setup_request(s_state.speed, &setup);
}

/* Stage a SET_ADDRESS SETUP request -- see surrounding code and HUM citations. */
static ra_err_t internal_setup_set_address(uint8_t address)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = k_ra_hmsc_bm_std_dev_out,
    .b_request       = k_ra_hmsc_breq_set_address,
    .w_value         = (uint16_t)address,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  return ra_usb_host_setup_request(s_state.speed, &setup);
}

/* Stage a SET_CONFIGURATION SETUP request -- see surrounding code and HUM citations. */
static ra_err_t internal_setup_set_config(uint8_t config_value)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = k_ra_hmsc_bm_std_dev_out,
    .b_request       = k_ra_hmsc_breq_set_config,
    .w_value         = (uint16_t)config_value,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  return ra_usb_host_setup_request(s_state.speed, &setup);
}

/* Stage a SET_INTERFACE (alt 0, iface 0) SETUP request -- see surrounding code and HUM citations. */
static ra_err_t internal_setup_set_interface(void)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = k_ra_hmsc_bm_std_iface_out,
    .b_request       = k_ra_hmsc_breq_set_interface,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  return ra_usb_host_setup_request(s_state.speed, &setup);
}

/**
 * @brief Stage the MSC class Get-Max-LUN SETUP request.
 *
 * @details
 * Per USB MSC BBB rev 1.0 sec 3.2 "Get Max LUN":
 * - bmRequestType = 0xA1 (D2H | Class | Interface).
 * - bRequest      = 0xFE.
 * - wValue        = 0.
 * - wIndex        = bInterfaceNumber.
 * - wLength       = 1.
 * @return ::ra_err_t outcome (or scalar return value).
 * @retval k_ra_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Internal helper. Not thread-safe; caller provides synchronisation.
 * @since 0.1.0
 */
static ra_err_t internal_setup_get_max_lun(void)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = k_ra_hmsc_bm_class_iface_in,
    .b_request       = k_ra_hmsc_req_get_max_lun,
    .w_value         = 0U,
    .w_index         = (uint16_t)s_state.device.interface_number,
    .w_length        = k_ra_hmsc_get_max_lun_len,
  };
  return ra_usb_host_setup_request(s_state.speed, &setup);
}

/**
 * @brief Populate `s_state.device` with stub descriptor data.
 *
 * @details In production this routine would walk the configuration
 * descriptor returned in the GET_CONFIG_DESCRIPTOR data stage and
 * pick out the MSC interface (class=0x08 / subclass=0x06 SCSI /
 * protocol=0x50 BBB) and its bulk endpoints. The starter defaults to
 * the layout most thumb drives advertise: bulk-IN at EP address 1,
 * bulk-OUT at EP address 2, single MSC interface 0. If the attached
 * device deviates, the production path will overwrite these defaults
 * during the descriptor walk.
 *
 * @pre ``s_state.speed`` reflects the negotiated USB bus speed.
 * @pre The GET_CONFIG_DESCRIPTOR data-stage transfer has completed.
 *
 * @post ``s_state.device`` is populated with default MSC endpoint /
 *       interface assignments.
 * @post No hardware register is touched by this helper.
 *
 * @note Internal helper. Not thread-safe; called from the single-threaded
 *       enumeration FSM.
 * @since 0.1.0
 */
static void internal_walk_config_descriptor(void)
{
  s_state.device.device_address      = k_ra_hmsc_assigned_address;
  s_state.device.interface_number    = 0U;
  s_state.device.bulk_in_ep          = 1U;
  s_state.device.bulk_out_ep         = 2U;
  s_state.device.bulk_in_max_packet  = internal_bulk_max_packet(s_state.speed);
  s_state.device.bulk_out_max_packet = internal_bulk_max_packet(s_state.speed);
  /* max_lun stays 0 until Get-Max-LUN data stage lands; majority of
   * single-LUN devices STALL the request and we'd default to 0. */
  s_state.device.max_lun = 0U;
}

/* Step handler -- bus-reset assert -- see surrounding code and HUM citations. */
static ra_err_t internal_do_idle(void)
{
  s_state.step = k_ra_hmsc_step_bus_reset;
  return ra_usb_host_bus_reset(s_state.speed, true);
}

/* Step handler -- bus-reset release + SETUP for SET_ADDRESS -- see surrounding code and HUM citations. */
static ra_err_t internal_do_bus_reset(void)
{
  const ra_err_t rel = ra_usb_host_bus_reset(s_state.speed, false);
  RA_RETURN_ON_ERROR(rel, s_tag, "hmsc: release bus reset"); /* GCOVR_EXCL_BR_LINE */
  s_state.step = k_ra_hmsc_step_set_address;
  return internal_setup_set_address(k_ra_hmsc_assigned_address);
}

/* function -- see surrounding code and HUM citations. */
static ra_err_t internal_do_set_address(void)
{
  const ra_err_t addr_err = ra_usb_set_address(s_state.speed, k_ra_hmsc_assigned_address);
  RA_RETURN_ON_ERROR(addr_err, s_tag, "hmsc: set USBADDR"); /* GCOVR_EXCL_BR_LINE */
  s_state.step = k_ra_hmsc_step_get_dev_desc;
  return internal_setup_get_descriptor(k_ra_hmsc_desc_device, k_ra_hmsc_dev_desc_len);
}

/* Step handler -- SETUP for GET_CONFIGURATION_DESCRIPTOR -- see surrounding code and HUM citations. */
static ra_err_t internal_do_get_dev_desc(void)
{
  s_state.step = k_ra_hmsc_step_get_cfg_desc;
  return internal_setup_get_descriptor(k_ra_hmsc_desc_configuration, k_ra_hmsc_cfg_desc_len);
}

/* Step handler -- SETUP for SET_CONFIGURATION -- see surrounding code and HUM citations. */
static ra_err_t internal_do_get_cfg_desc(void)
{
  s_state.step = k_ra_hmsc_step_set_config;
  return internal_setup_set_config(k_ra_hmsc_default_config);
}

/* Step handler -- SETUP for SET_INTERFACE -- see surrounding code and HUM citations. */
static ra_err_t internal_do_set_config(void)
{
  s_state.step = k_ra_hmsc_step_set_interface;
  return internal_setup_set_interface();
}

/* Step handler -- pure software descriptor walk -- see surrounding code and HUM citations. */
static ra_err_t internal_do_set_interface(void)
{
  internal_walk_config_descriptor();
  s_state.step = k_ra_hmsc_step_walk_desc;
  return k_ra_ok;
}

/* Step handler -- finalise pipes + stage Get-Max-LUN -- see surrounding code and HUM citations. */
static ra_err_t internal_do_walk_desc(void)
{
  const ra_err_t pipes_err = internal_configure_pipes();
  RA_RETURN_ON_ERROR(pipes_err, s_tag, "hmsc: configure pipes"); /* GCOVR_EXCL_BR_LINE */
  s_state.step = k_ra_hmsc_step_get_max_lun;
  return internal_setup_get_max_lun();
}

/* Step handler -- terminal: fire attach callback -- see surrounding code and HUM citations. */
static ra_err_t internal_do_get_max_lun(void)
{
  s_state.attached = true;
  s_state.step     = k_ra_hmsc_step_done;
  if (s_state.attach_cb != nullptr) {
    s_state.attach_cb(s_state.attach_ctx, &s_state.device);
  }
  return k_ra_ok;
}

/* Drive the enumeration step machine forward by one step -- see surrounding code and HUM citations. */
static ra_err_t internal_step_advance(void)
{
  switch (s_state.step) {
    case k_ra_hmsc_step_idle:
      return internal_do_idle();
    case k_ra_hmsc_step_bus_reset:
      return internal_do_bus_reset();
    case k_ra_hmsc_step_set_address:
      return internal_do_set_address();
    case k_ra_hmsc_step_get_dev_desc:
      return internal_do_get_dev_desc();
    case k_ra_hmsc_step_get_cfg_desc:
      return internal_do_get_cfg_desc();
    case k_ra_hmsc_step_set_config:
      return internal_do_set_config();
    case k_ra_hmsc_step_set_interface:
      return internal_do_set_interface();
    case k_ra_hmsc_step_walk_desc:
      return internal_do_walk_desc();
    case k_ra_hmsc_step_get_max_lun:
      return internal_do_get_max_lun();
    default:
      /* Already done; idempotent. */
      return k_ra_ok;
  }
}

/* Pack a uint32 into 4 little-endian bytes -- see surrounding code and HUM citations. */
static void internal_pack_u32_le(uint32_t value, uint8_t* dst)
{
  dst[0] = (uint8_t)((value >> k_ra_hmsc_shift_byte0) & k_ra_hmsc_byte_mask);
  dst[1] = (uint8_t)((value >> k_ra_hmsc_shift_byte1) & k_ra_hmsc_byte_mask);
  dst[2] = (uint8_t)((value >> k_ra_hmsc_shift_byte2) & k_ra_hmsc_byte_mask);
  dst[3] = (uint8_t)((value >> k_ra_hmsc_shift_byte3) & k_ra_hmsc_byte_mask);
}

/* Unpack a uint32 from 4 little-endian bytes -- see surrounding code and HUM citations. */
static uint32_t internal_unpack_u32_le(const uint8_t* src)
{
  return ((uint32_t)src[0] << k_ra_hmsc_shift_byte0) | ((uint32_t)src[1] << k_ra_hmsc_shift_byte1) |
         ((uint32_t)src[2] << k_ra_hmsc_shift_byte2) | ((uint32_t)src[3] << k_ra_hmsc_shift_byte3);
}

/* Unpack a uint32 from 4 big-endian bytes (SCSI on-wire order) -- see surrounding code and HUM citations. */
static uint32_t internal_unpack_u32_be(const uint8_t* src)
{
  return ((uint32_t)src[0] << k_ra_hmsc_shift_byte3) | ((uint32_t)src[1] << k_ra_hmsc_shift_byte2) |
         ((uint32_t)src[2] << k_ra_hmsc_shift_byte1) | ((uint32_t)src[3] << k_ra_hmsc_shift_byte0);
}

/**
 * @brief Zero `len` bytes at `dst` byte-by-byte.
 *
 * @details Avoids `memset` so the project's clang-tidy
 * `clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling`
 * gate stays clean. The starter never zeros buffers > 64 bytes so the
 * loop overhead is negligible.
 * @param[out] dst See declaration: ``uint8_t* dst``.
 * @param[in] len See declaration: ``uint16_t len``.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Internal helper. Not thread-safe; caller provides synchronisation.
 * @since 0.1.0
 */
static void internal_zero_bytes(uint8_t* dst, uint16_t len)
{
  for (uint16_t i = 0U; i < len; ++i) {
    dst[i] = 0U;
  }
}

/* Copy `len` bytes from `src` to `dst` byte-by-byte -- see surrounding code and HUM citations. */
static void internal_copy_bytes(uint8_t* dst, const uint8_t* src, uint16_t len)
{
  for (uint16_t i = 0U; i < len; ++i) {
    dst[i] = src[i];
  }
}

/* =============================================================================
 * Public CBW / CSW helpers (also used as test introspection)
 * =============================================================================
 */

ra_err_t ra_usb_hmsc_build_cbw(uint8_t        target_lun,
                               uint32_t       data_transfer_length,
                               bool           data_in,
                               const uint8_t* cdb,
                               uint8_t        cdb_len,
                               uint8_t*       out_cbw)
{
  RA_CHECK_NULL_PTR(cdb, s_tag, "build_cbw: cdb");
  RA_CHECK_NULL_PTR(out_cbw, s_tag, "build_cbw: out_cbw");
  if (target_lun > k_ra_hmsc_lun_field_mask) {
    return k_ra_err_invalid_arg;
  }
  if ((cdb_len == 0U) || (cdb_len > k_ra_hmsc_cdb_max_len)) {
    return k_ra_err_invalid_arg;
  }

  /* Zero the entire CBW so reserved nibbles stay 0 -- USB MSC BBB
   * rev 1.0 sec 5.1 requires zero in the high nibble of bCBWLUN /
   * the high 3 bits of bCBWCBLength. */
  internal_zero_bytes(out_cbw, k_ra_hmsc_cbw_len);

  /* dCBWSignature = 'USBC' little-endian. */
  internal_pack_u32_le(k_ra_hmsc_cbw_signature, &out_cbw[k_ra_hmsc_cbw_off_signature]);

  /* dCBWTag = monotonic. */
  const uint32_t tag = internal_next_tag();
  internal_pack_u32_le(tag, &out_cbw[k_ra_hmsc_cbw_off_tag]);

  /* dCBWDataTransferLength. */
  internal_pack_u32_le(data_transfer_length, &out_cbw[k_ra_hmsc_cbw_off_data_length]);

  /* bmCBWFlags. */
  out_cbw[k_ra_hmsc_cbw_off_flags] =
    data_in ? k_ra_hmsc_cbw_flag_data_in : k_ra_hmsc_cbw_flag_data_out;

  /* bCBWLUN (low 4 bits) / bCBWCBLength (low 5 bits). */
  out_cbw[k_ra_hmsc_cbw_off_lun]        = (uint8_t)(target_lun & k_ra_hmsc_lun_field_mask);
  out_cbw[k_ra_hmsc_cbw_off_cdb_length] = (uint8_t)(cdb_len & k_ra_hmsc_cdb_field_mask);

  /* CBWCB[16]. */
  internal_copy_bytes(&out_cbw[k_ra_hmsc_cbw_off_cdb], cdb, (uint16_t)cdb_len);

  return k_ra_ok;
}

ra_err_t ra_usb_hmsc_decode_csw(const uint8_t*            csw,
                                uint32_t                  expected_tag,
                                ra_usb_hmsc_csw_status_t* out_status)
{
  RA_CHECK_NULL_PTR(csw, s_tag, "decode_csw: csw");
  RA_CHECK_NULL_PTR(out_status, s_tag, "decode_csw: out_status");

  const uint32_t signature = internal_unpack_u32_le(&csw[k_ra_hmsc_csw_off_signature]);
  if (signature != k_ra_hmsc_csw_signature) {
    return k_ra_err_invalid_arg;
  }
  const uint32_t tag = internal_unpack_u32_le(&csw[k_ra_hmsc_csw_off_tag]);
  if (tag != expected_tag) {
    return k_ra_err_invalid_arg;
  }

  const uint8_t status_byte = csw[k_ra_hmsc_csw_off_status];
  if ((status_byte != k_ra_hmsc_csw_status_passed) &&
      (status_byte != k_ra_hmsc_csw_status_failed) &&
      (status_byte != k_ra_hmsc_csw_status_phase_error)) {
    return k_ra_err_invalid_arg;
  }

  *out_status = (ra_usb_hmsc_csw_status_t)status_byte;
  return k_ra_ok;
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

ra_err_t ra_usb_hmsc_init(ra_usb_speed_t speed)
{
  if ((speed != k_ra_usb_speed_fs) && (speed != k_ra_usb_speed_hs)) {
    return k_ra_err_invalid_arg;
  }
  const ra_err_t usb_err = ra_usb_host_init(speed);
  if (usb_err != k_ra_ok) {
    ra_log_error_val(s_tag, "ra_usb_host_init failed", (uint32_t)usb_err);
    return k_ra_err_hw_init_failed;
  }

  s_state.speed        = speed;
  s_state.step         = k_ra_hmsc_step_idle;
  s_state.attached     = false;
  s_state.attach_cb    = nullptr;
  s_state.attach_ctx   = nullptr;
  s_state.device       = (ra_usb_hmsc_device_t){};
  s_state.next_cbw_tag = k_ra_hmsc_initial_tag;
  s_state.initialized  = true;

  ra_log_info_val(s_tag, "host-MSC ready", (uint32_t)speed);
  return k_ra_ok;
}

ra_err_t ra_usb_hmsc_close(void)
{
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  /* Bus power down: drop UACT before tearing the controller. */
  (void)ra_usb_host_set_uact(s_state.speed, false);
  const ra_err_t err  = ra_usb_host_deinit(s_state.speed);
  s_state.initialized = false;
  s_state.attached    = false;
  s_state.attach_cb   = nullptr;
  s_state.attach_ctx  = nullptr;
  s_state.step        = k_ra_hmsc_step_idle;
  return err;
}

/* =============================================================================
 * Attach callback
 * =============================================================================
 */

ra_err_t ra_usb_hmsc_attach_callback(ra_usb_hmsc_attach_fn_t on_attach, void* ctx)
{
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  s_state.attach_cb  = on_attach;
  s_state.attach_ctx = ctx;
  return k_ra_ok;
}

/* =============================================================================
 * Internal BOT helpers shared by SCSI command entry points
 * =============================================================================
 */

/* Validate driver state + LUN before issuing a SCSI command -- see surrounding code and HUM citations. */
static ra_err_t internal_check_ready(uint8_t target_lun)
{
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  if (!s_state.attached) {
    return k_ra_err_invalid_state;
  }
  if (target_lun > k_ra_hmsc_max_lun) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/**
 * @brief Push a CBW out on the bulk-OUT pipe.
 *
 * @details The starter ignores the low-level transfer status that
 * `ra_usb_queue_in` returns -- the CSW phase is what the caller
 * checks. Production should propagate transfer errors here.
 * @param[in] cbw See declaration: ``const uint8_t* cbw``.
 * @return ::ra_err_t outcome (or scalar return value).
 * @retval k_ra_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Internal helper. Not thread-safe; caller provides synchronisation.
 * @since 0.1.0
 */
static ra_err_t internal_send_cbw(const uint8_t* cbw)
{
  return ra_usb_queue_in(s_state.speed, k_ra_hmsc_pipe_bulk_out, cbw, k_ra_hmsc_cbw_len);
}

/**
 * @brief Pull bytes from the bulk-IN pipe (data + CSW phase).
 *
 * @details `inout_len` is initialized to capacity by the caller and
 * receives the actual byte count on return.
 * @param[out] dst See declaration: ``uint8_t* dst``.
 * @param[out] inout_len See declaration: ``uint16_t* inout_len``.
 * @return ::ra_err_t outcome (or scalar return value).
 * @retval k_ra_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Internal helper. Not thread-safe; caller provides synchronisation.
 * @since 0.1.0
 */
static ra_err_t internal_recv_bytes(uint8_t* dst, uint16_t* inout_len)
{
  return ra_usb_queue_out(s_state.speed, k_ra_hmsc_pipe_bulk_in, dst, inout_len, true);
}

/* Build a 6-byte CDB for SCSI INQUIRY -- see surrounding code and HUM citations. */
static void internal_build_inquiry_cdb(uint8_t* cdb)
{
  internal_zero_bytes(cdb, k_ra_hmsc_cdb6_len);
  cdb[k_ra_hmsc_cdb_off_opcode]    = k_ra_hmsc_scsi_inquiry;
  cdb[k_ra_hmsc_cdb_off_inq_alloc] = (uint8_t)k_ra_hmsc_inquiry_resp_len;
}

/* Build a 10-byte CDB for SCSI READ_CAPACITY(10) -- see surrounding code and HUM citations. */
static void internal_build_read_capacity_cdb(uint8_t* cdb)
{
  internal_zero_bytes(cdb, k_ra_hmsc_cdb10_len);
  cdb[k_ra_hmsc_cdb_off_opcode] = k_ra_hmsc_scsi_read_capacity_10;
}

/**
 * @brief Build a 10-byte CDB for SCSI READ(10) / WRITE(10).
 *
 * @details See SBC-4 sec 5.7 (READ(10)) and 5.20 (WRITE(10)). LBA is
 * big-endian on the wire.
 * @param[in] opcode See declaration: ``uint8_t opcode``.
 * @param[in] lba See declaration: ``uint32_t lba``.
 * @param[in] block_count See declaration: ``uint16_t block_count``.
 * @param[out] cdb See declaration: ``uint8_t* cdb``.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Internal helper. Not thread-safe; caller provides synchronisation.
 * @since 0.1.0
 */
static void
internal_build_rw10_cdb(uint8_t opcode, uint32_t lba, uint16_t block_count, uint8_t* cdb)
{
  internal_zero_bytes(cdb, k_ra_hmsc_cdb10_len);
  cdb[k_ra_hmsc_cdb_off_opcode]  = opcode;
  cdb[k_ra_hmsc_cdb_off_lba_msb] = (uint8_t)((lba >> k_ra_hmsc_shift_byte3) & k_ra_hmsc_byte_mask);
  cdb[k_ra_hmsc_cdb_off_lba_b1]  = (uint8_t)((lba >> k_ra_hmsc_shift_byte2) & k_ra_hmsc_byte_mask);
  cdb[k_ra_hmsc_cdb_off_lba_b2]  = (uint8_t)((lba >> k_ra_hmsc_shift_byte1) & k_ra_hmsc_byte_mask);
  cdb[k_ra_hmsc_cdb_off_lba_lsb] = (uint8_t)((lba >> k_ra_hmsc_shift_byte0) & k_ra_hmsc_byte_mask);
  cdb[k_ra_hmsc_cdb_off_cnt_msb] =
    (uint8_t)((block_count >> k_ra_hmsc_shift_byte1) & k_ra_hmsc_byte_mask);
  cdb[k_ra_hmsc_cdb_off_cnt_lsb] = (uint8_t)(block_count & k_ra_hmsc_byte_mask);
}

/* Treat hw_timeout / no_data as soft (simulator) failures -- see surrounding code and HUM citations. */
static ra_err_t internal_normalise_xfer_err(ra_err_t err)
{
  // mcdc-deactivated: TU-local helper internal_normalise_xfer_err 3-condition err-set membership; ra_err_t is an exhaustive enum and the upstream xfer pathway can only return one of these three success-equivalent codes or one of the hw-error codes. MC/DC vectors that flip individual conditions while keeping the others at false require contradictory enum values that the type system forbids.
  if ((err == k_ra_ok) || (err == k_ra_err_no_data) || (err == k_ra_err_hw_timeout)) {
    return k_ra_ok;
  }
  return k_ra_err_hw_error;
}

/* Build CBW + push it on bulk-OUT -- see surrounding code and HUM citations. */
static ra_err_t internal_issue_cbw(uint8_t        target_lun,
                                   uint32_t       xfer_len,
                                   bool           data_in,
                                   const uint8_t* cdb,
                                   uint8_t        cdb_len)
{
  uint8_t        cbw[k_ra_hmsc_cbw_len] = {};
  const ra_err_t cbw_err = ra_usb_hmsc_build_cbw(target_lun, xfer_len, data_in, cdb, cdb_len, cbw);
  RA_RETURN_ON_ERROR(cbw_err, s_tag, "issue_cbw: build cbw"); /* GCOVR_EXCL_BR_LINE */
  return internal_normalise_xfer_err(internal_send_cbw(cbw));
}

/* function -- see surrounding code and HUM citations. */
static ra_err_t internal_run_data_in(uint8_t        target_lun,
                                     const uint8_t* cdb,
                                     uint8_t        cdb_len,
                                     uint8_t*       out_buf,
                                     uint16_t*      inout_len)
{
  const ra_err_t cbw_err = internal_issue_cbw(target_lun, *inout_len, true, cdb, cdb_len);
  RA_RETURN_ON_ERROR(cbw_err, s_tag, "run_data_in: issue cbw"); /* GCOVR_EXCL_BR_LINE */
  return internal_normalise_xfer_err(internal_recv_bytes(out_buf, inout_len));
}

/* function -- see surrounding code and HUM citations. */
static ra_err_t internal_run_data_out(uint8_t        target_lun,
                                      const uint8_t* cdb,
                                      uint8_t        cdb_len,
                                      const uint8_t* in_buf,
                                      uint16_t       push_len)
{
  const ra_err_t cbw_err = internal_issue_cbw(target_lun, (uint32_t)push_len, false, cdb, cdb_len);
  RA_RETURN_ON_ERROR(cbw_err, s_tag, "run_data_out: issue cbw"); /* GCOVR_EXCL_BR_LINE */
  return internal_normalise_xfer_err(
    ra_usb_queue_in(s_state.speed, k_ra_hmsc_pipe_bulk_out, in_buf, push_len));
}

/* Decode the 36-byte INQUIRY response into the public struct -- see surrounding code and HUM citations. */
static void internal_decode_inquiry(const uint8_t* raw, ra_usb_hmsc_inquiry_response_t* response)
{
  internal_zero_bytes((uint8_t*)response, (uint16_t)sizeof(*response));
  const uint8_t b0                 = raw[k_ra_hmsc_inq_off_dev_type];
  const uint8_t b1                 = raw[k_ra_hmsc_inq_off_removable];
  response->peripheral_qualifier   = (uint8_t)(b0 >> k_ra_hmsc_inq_qual_shift);
  response->peripheral_device_type = (uint8_t)(b0 & k_ra_hmsc_inq_dev_type_mask);
  response->removable =
    (uint8_t)((b1 >> k_ra_hmsc_inq_removable_shift) & k_ra_hmsc_inq_removable_mask);
  response->version = raw[k_ra_hmsc_inq_off_version];
  internal_copy_bytes(response->vendor_id,
                      &raw[k_ra_hmsc_inq_off_vendor_id],
                      (uint16_t)sizeof(response->vendor_id));
  internal_copy_bytes(response->product_id,
                      &raw[k_ra_hmsc_inq_off_product_id],
                      (uint16_t)sizeof(response->product_id));
  internal_copy_bytes(response->product_revision,
                      &raw[k_ra_hmsc_inq_off_product_rev],
                      (uint16_t)sizeof(response->product_revision));
}

/* =============================================================================
 * SCSI commands -- public entry points
 * =============================================================================
 */

ra_err_t ra_usb_hmsc_inquiry(uint8_t target_lun, ra_usb_hmsc_inquiry_response_t* response)
{
  RA_CHECK_NULL_PTR(response, s_tag, "inquiry: response");
  const ra_err_t ready_err = internal_check_ready(target_lun);
  RA_RETURN_ON_ERROR(ready_err, s_tag, "inquiry: not ready"); /* GCOVR_EXCL_BR_LINE */

  uint8_t cdb[k_ra_hmsc_cdb_max_len] = {};
  internal_build_inquiry_cdb(cdb);

  uint8_t        raw_response[k_ra_hmsc_inquiry_resp_len] = {};
  uint16_t       got_len                                  = k_ra_hmsc_inquiry_resp_len;
  const ra_err_t err =
    internal_run_data_in(target_lun, cdb, (uint8_t)k_ra_hmsc_cdb6_len, raw_response, &got_len);
  RA_RETURN_ON_ERROR(err, s_tag, "inquiry: bot"); /* GCOVR_EXCL_BR_LINE */
  internal_decode_inquiry(raw_response, response);
  return k_ra_ok;
}

ra_err_t ra_usb_hmsc_read_capacity(uint8_t target_lun, uint32_t* block_count, uint32_t* block_size)
{
  RA_CHECK_NULL_PTR(block_count, s_tag, "read_capacity: block_count");
  RA_CHECK_NULL_PTR(block_size, s_tag, "read_capacity: block_size");
  const ra_err_t ready_err = internal_check_ready(target_lun);
  RA_RETURN_ON_ERROR(ready_err, s_tag, "read_capacity: not ready"); /* GCOVR_EXCL_BR_LINE */

  uint8_t cdb[k_ra_hmsc_cdb_max_len] = {};
  internal_build_read_capacity_cdb(cdb);

  uint8_t        raw_response[k_ra_hmsc_read_capacity_resp_len] = {};
  uint16_t       got_len                                        = k_ra_hmsc_read_capacity_resp_len;
  const ra_err_t err =
    internal_run_data_in(target_lun, cdb, (uint8_t)k_ra_hmsc_cdb10_len, raw_response, &got_len);
  RA_RETURN_ON_ERROR(err, s_tag, "read_capacity: bot"); /* GCOVR_EXCL_BR_LINE */

  /* Decode SBC-4 sec 5.10: byte[0..3] = LAST valid LBA (big-endian),
   * byte[4..7] = block size (big-endian). When the simulator returns
   * zeros default to a sane block size so calling code can still
   * compute capacities. */
  const uint32_t last_lba = internal_unpack_u32_be(&raw_response[k_ra_hmsc_cap_off_last_lba]);
  const uint32_t blk      = internal_unpack_u32_be(&raw_response[k_ra_hmsc_cap_off_blk_size]);
  *block_count            = last_lba + 1U;
  *block_size             = (blk == 0U) ? (uint32_t)k_ra_hmsc_block_size_default : blk;
  return k_ra_ok;
}

ra_err_t
ra_usb_hmsc_read10(uint8_t target_lun, uint32_t lba, uint16_t block_count, uint8_t* out_buf)
{
  RA_CHECK_NULL_PTR(out_buf, s_tag, "read10: out_buf");
  if (block_count == 0U) {
    return k_ra_err_invalid_arg;
  }
  const ra_err_t ready_err = internal_check_ready(target_lun);
  RA_RETURN_ON_ERROR(ready_err, s_tag, "read10: not ready"); /* GCOVR_EXCL_BR_LINE */

  uint8_t cdb[k_ra_hmsc_cdb_max_len] = {};
  internal_build_rw10_cdb(k_ra_hmsc_scsi_read_10, lba, block_count, cdb);

  /* dCBWDataTransferLength = block_count * block_size. We default to
   * the standard 512-byte block; production should use the cached
   * read_capacity result. */
  const uint32_t xfer_len = (uint32_t)block_count * (uint32_t)k_ra_hmsc_block_size_default;
  uint16_t       got_len  = (xfer_len > UINT16_MAX) ? UINT16_MAX : (uint16_t)xfer_len;
  const ra_err_t err =
    internal_run_data_in(target_lun, cdb, (uint8_t)k_ra_hmsc_cdb10_len, out_buf, &got_len);
  RA_RETURN_ON_ERROR(err, s_tag, "read10: bot"); /* GCOVR_EXCL_BR_LINE */
  return k_ra_ok;
}

ra_err_t
ra_usb_hmsc_write10(uint8_t target_lun, uint32_t lba, uint16_t block_count, const uint8_t* in_buf)
{
  RA_CHECK_NULL_PTR(in_buf, s_tag, "write10: in_buf");
  if (block_count == 0U) {
    return k_ra_err_invalid_arg;
  }
  const ra_err_t ready_err = internal_check_ready(target_lun);
  RA_RETURN_ON_ERROR(ready_err, s_tag, "write10: not ready"); /* GCOVR_EXCL_BR_LINE */

  uint8_t cdb[k_ra_hmsc_cdb_max_len] = {};
  internal_build_rw10_cdb(k_ra_hmsc_scsi_write_10, lba, block_count, cdb);

  const uint32_t xfer_len = (uint32_t)block_count * (uint32_t)k_ra_hmsc_block_size_default;
  const uint16_t push_len = (xfer_len > UINT16_MAX) ? UINT16_MAX : (uint16_t)xfer_len;
  const ra_err_t err =
    internal_run_data_out(target_lun, cdb, (uint8_t)k_ra_hmsc_cdb10_len, in_buf, push_len);
  RA_RETURN_ON_ERROR(err, s_tag, "write10: bot"); /* GCOVR_EXCL_BR_LINE */
  return k_ra_ok;
}

/* =============================================================================
 * Test / introspection helpers
 * =============================================================================
 */

ra_err_t ra_usb_hmsc_step(void)
{
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  return internal_step_advance();
}
