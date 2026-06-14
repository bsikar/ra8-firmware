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
 *  - `usb_hmsc_get_max_unit`    -> `internal_enum_configure`
 *  - `usb_hmsc_class_check`     -> `internal_enum_walk_cfg`
 *  - `usb_hmsc_pipe_info`       -> `internal_enum_note_endpoint`
 *
 * The starter does CPU-FIFO, single-device, no-hub. Enumeration is a
 * single polled ladder (`ra_usb_hmsc_enumerate`): wait for the D+
 * attach, hunt the (reset, address) combination the device answers on,
 * then read descriptors / SET_CONFIGURATION / open the bulk pipes.
 * Every chapter-9 SETUP goes through `ra_usb_host_setup_request`.
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
#include "ra_time.h"
#include "ra_usb.h"

static const char* s_tag = "USBHMSC";

/* =============================================================================
 * Internal constants
 * =============================================================================
 */

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
 * Polled enumeration ladder (hardware-proven; replaces the CTRT machine)
 * =============================================================================
 */

/**
 * @enum ra_usb_hmsc_enum_tune_t
 * @brief Timing / retry tunables for the polled enumeration ladder.
 */
typedef enum : uint32_t {
  k_ra_hmsc_vbus_settle_ms = 200U,  /**< Supply settle before probing.    */
  k_ra_hmsc_attach_to_ms   = 2000U, /**< Wait for the D+ pull-up.         */
  k_ra_hmsc_debounce_ms    = 500U,  /**< Post-attach debounce (>=100 ms). */
  k_ra_hmsc_reset_hold_ms  = 50U,   /**< USB bus-reset hold (>=10 ms).    */
  k_ra_hmsc_recovery_ms    = 20U,   /**< Post-reset recovery (TRSTRCY).   */
  k_ra_hmsc_addr_settle_ms = 5U,    /**< Post-SET_ADDRESS recovery.       */
  k_ra_hmsc_enum_tries     = 8U,    /**< (reset?, addr) hunt attempts.    */
  k_ra_hmsc_no_reset_tries = 4U,    /**< Attempts before using bus reset. */
  k_ra_hmsc_addr_alt_mask  = 0x03U, /**< Alternate addr 0..3 per attempt. */
  k_ra_hmsc_cfg_buf_len    = 128U,  /**< Full-configuration read buffer.  */
  /** P10 iteration bound on the attach wait: the loop is primarily
   * ms-bounded via `ra_time_ms`, but if the tick is frozen (simulator
   * builds, SysTick masked) the spin cap guarantees termination. */
  k_ra_hmsc_attach_spin_limit = 50000000UL,
} ra_usb_hmsc_enum_tune_t;

/**
 * @enum ra_usb_hmsc_walk_off_t
 * @brief Descriptor-walk byte offsets and identity codes.
 */
typedef enum : uint8_t {
  k_ra_hmsc_off_dlen        = 0U,    /**< Any descriptor: bLength.         */
  k_ra_hmsc_off_dtype       = 1U,    /**< Any descriptor: bDescriptorType. */
  k_ra_hmsc_off_iface_num   = 2U,    /**< Interface: bInterfaceNumber.     */
  k_ra_hmsc_off_iface_class = 5U,    /**< Interface: bInterfaceClass.      */
  k_ra_hmsc_off_iface_sub   = 6U,    /**< Interface: bInterfaceSubClass.   */
  k_ra_hmsc_off_iface_proto = 7U,    /**< Interface: bInterfaceProtocol.   */
  k_ra_hmsc_off_ep_addr     = 2U,    /**< Endpoint: bEndpointAddress.      */
  k_ra_hmsc_off_ep_attr     = 3U,    /**< Endpoint: bmAttributes.          */
  k_ra_hmsc_off_ep_mps      = 4U,    /**< Endpoint: wMaxPacketSize LSB.    */
  k_ra_hmsc_off_cfg_total   = 2U,    /**< Configuration: wTotalLength LSB. */
  k_ra_hmsc_off_cfg_value   = 5U,    /**< Configuration: bConfigValue.     */
  k_ra_hmsc_off_dev_vid     = 8U,    /**< Device: idVendor LSB.            */
  k_ra_hmsc_off_dev_pid     = 10U,   /**< Device: idProduct LSB.           */
  k_ra_hmsc_ep_dir_in_bit   = 0x80U, /**< bEndpointAddress direction bit.  */
  k_ra_hmsc_ep_num_mask     = 0x0FU, /**< bEndpointAddress number field.   */
  k_ra_hmsc_ep_attr_mask    = 0x03U, /**< bmAttributes transfer-type mask. */
  k_ra_hmsc_ep_attr_bulk    = 0x02U, /**< bmAttributes: bulk transfer.     */
  k_ra_hmsc_byte_bits       = 8U,    /**< Bit width of one byte.           */
} ra_usb_hmsc_walk_off_t;

/**
 * @brief Read the 18-byte device descriptor over the polled control engine.
 *
 * @details GET_DESCRIPTOR(DEVICE) at whatever address the DCP currently
 * targets; requires the full 18 bytes back.
 *
 * @param[out] desc Receives the descriptor (18 bytes).
 * @return Read outcome.
 * @retval k_ra_ok           All 18 bytes arrived.
 * @retval k_ra_err_hw_error A short descriptor came back.
 * @pre The bus is reset and UACT is on.
 * @pre @p desc holds at least 18 bytes.
 * @post @p desc carries the device descriptor on success.
 * @post No state is modified.
 * @note Blocking (polled control transfer).
 * @since 0.1.0
 */
static ra_err_t internal_enum_read_dev_desc(uint8_t* desc)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = k_ra_hmsc_bm_std_dev_in,
    .b_request       = k_ra_hmsc_breq_get_descriptor,
    .w_value         = (uint16_t)((uint16_t)k_ra_hmsc_desc_device << k_ra_hmsc_byte_bits),
    .w_index         = 0U,
    .w_length        = k_ra_hmsc_dev_desc_len,
  };
  uint16_t       rx = 0U;
  const ra_err_t err =
    ra_usb_host_control_xfer(s_state.speed, &setup, desc, (uint16_t)k_ra_hmsc_dev_desc_len, &rx);
  RA_RETURN_ON_ERROR(err, s_tag, "enum: dev desc"); /* GCOVR_EXCL_BR_LINE */
  if (rx != (uint16_t)k_ra_hmsc_dev_desc_len) {
    return k_ra_err_hw_error;
  }
  return k_ra_ok;
}

/**
 * @brief Wait for a device to attach, then hunt for its address.
 *
 * @details Waits for the D+ pull-up (LNST leaves SE0) plus the spec
 * debounce, then tries each (reset?, address) combination: four gentle
 * attempts at addresses 0..3 without touching the bus, then four more
 * with a full bus reset (which also returns a previously addressed
 * device to address 0). The first combination whose device-descriptor
 * read returns all 18 bytes wins.
 *
 * @param[out] desc     Receives the winning 18-byte device descriptor.
 * @param[out] out_addr Receives the address the device answered at.
 * @return Hunt outcome.
 * @retval k_ra_ok              The device answered.
 * @retval k_ra_err_hw_timeout  Nothing attached / nothing answered.
 * @pre ::ra_usb_hmsc_init ran (host mode up, VBUS supplied).
 * @pre ::ra_time_init has run (the ladder uses millisecond delays).
 * @post On success the DCP targets `*out_addr` with UACT on.
 * @post On failure the bus state is whatever the last attempt left.
 * @note Blocking; worst case a few seconds.
 * @since 0.1.0
 */
static ra_err_t internal_enum_hunt(uint8_t* desc, uint8_t* out_addr)
{
  ra_delay_ms(k_ra_hmsc_vbus_settle_ms);
  const uint32_t t0 = ra_time_ms();
  for (uint32_t spin = 0U; spin < (uint32_t)k_ra_hmsc_attach_spin_limit; spin++) {
    if (ra_usb_host_line_state(s_state.speed) != 0U) {
      break;
    }
    if ((ra_time_ms() - t0) > (uint32_t)k_ra_hmsc_attach_to_ms) {
      break;
    }
  }
  ra_delay_ms(k_ra_hmsc_debounce_ms);
  ra_err_t err = k_ra_err_hw_timeout;
  for (uint8_t attempt = 0U; attempt < (uint8_t)k_ra_hmsc_enum_tries; attempt++) {
    if (attempt >= (uint8_t)k_ra_hmsc_no_reset_tries) {
      (void)ra_usb_host_bus_reset(s_state.speed, true);
      ra_delay_ms(k_ra_hmsc_reset_hold_ms);
      (void)ra_usb_host_bus_reset(s_state.speed, false);
    }
    (void)ra_usb_host_set_uact(s_state.speed, true);
    ra_delay_ms(k_ra_hmsc_recovery_ms);
    const uint8_t addr = (uint8_t)(attempt & (uint8_t)k_ra_hmsc_addr_alt_mask);
    (void)ra_usb_host_set_target(s_state.speed, addr);
    err = internal_enum_read_dev_desc(desc);
    if (err == k_ra_ok) {
      *out_addr = addr;
      return k_ra_ok;
    }
  }
  return err;
}

/**
 * @brief Move the device to address 1 when it answered at the default.
 *
 * @details SET_CONFIGURATION is only legal from the Address state (sticks
 * STALL it at the default address), so assign address 1, honour the
 * set-address recovery, and retarget the DCP. Skipped when the hunt
 * already found the device addressed.
 *
 * @param[in,out] dev_addr In: hunt result. Out: the operating address.
 * @return First failing step's error, or k_ra_ok.
 * @retval k_ra_ok The DCP targets the operating address.
 * @pre ::internal_enum_hunt succeeded.
 * @pre The bus is active (UACT on).
 * @post `*dev_addr` is non-zero on success.
 * @post Later transfers carry tokens to the new address.
 * @note Blocking (one polled control transfer + settle).
 * @since 0.1.0
 */
static ra_err_t internal_enum_assign_addr(uint8_t* dev_addr)
{
  if (*dev_addr != 0U) {
    return k_ra_ok;
  }
  const ra_usb_setup_t setup = {
    .bm_request_type = k_ra_hmsc_bm_std_dev_out,
    .b_request       = k_ra_hmsc_breq_set_address,
    .w_value         = k_ra_hmsc_assigned_address,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  ra_err_t err = ra_usb_host_control_xfer(s_state.speed, &setup, nullptr, 0U, nullptr);
  RA_RETURN_ON_ERROR(err, s_tag, "enum: set address"); /* GCOVR_EXCL_BR_LINE */
  ra_delay_ms(k_ra_hmsc_addr_settle_ms);
  err = ra_usb_host_set_target(s_state.speed, (uint8_t)k_ra_hmsc_assigned_address);
  RA_RETURN_ON_ERROR(err, s_tag, "enum: set target"); /* GCOVR_EXCL_BR_LINE */
  *dev_addr = (uint8_t)k_ra_hmsc_assigned_address;
  return k_ra_ok;
}

/**
 * @brief Record one bulk endpoint descriptor into the device snapshot.
 *
 * @details Filters for bmAttributes == bulk and slots the endpoint into
 * the IN or OUT position (first match wins) with its wMaxPacketSize.
 *
 * @param[in] d Pointer to an endpoint descriptor.
 * @pre @p d points at a descriptor with bDescriptorType ENDPOINT.
 * @pre The unfilled `s_state.device` endpoint slots are zero.
 * @post A matching bulk endpoint is recorded once.
 * @post Non-bulk endpoints leave the snapshot untouched.
 * @note Pure helper for the config-descriptor walk.
 * @since 0.1.0
 */
static void internal_enum_note_endpoint(const uint8_t* d)
{
  const uint8_t attr = (uint8_t)(d[k_ra_hmsc_off_ep_attr] & (uint8_t)k_ra_hmsc_ep_attr_mask);
  if (attr != (uint8_t)k_ra_hmsc_ep_attr_bulk) {
    return;
  }
  const uint8_t  ea = d[k_ra_hmsc_off_ep_addr];
  const uint16_t mps =
    (uint16_t)((uint16_t)d[k_ra_hmsc_off_ep_mps] |
               (uint16_t)((uint16_t)d[k_ra_hmsc_off_ep_mps + 1U] << k_ra_hmsc_byte_bits));
  if ((ea & (uint8_t)k_ra_hmsc_ep_dir_in_bit) != 0U) {
    if (s_state.device.bulk_in_ep == 0U) {
      s_state.device.bulk_in_ep         = (uint8_t)(ea & (uint8_t)k_ra_hmsc_ep_num_mask);
      s_state.device.bulk_in_max_packet = mps;
    }
  } else {
    if (s_state.device.bulk_out_ep == 0U) {
      s_state.device.bulk_out_ep         = (uint8_t)(ea & (uint8_t)k_ra_hmsc_ep_num_mask);
      s_state.device.bulk_out_max_packet = mps;
    }
  }
}

/**
 * @brief Test whether an interface descriptor is MSC SCSI Bulk-Only.
 *
 * @details Matches class 0x08 (mass storage), subclass 0x06 (SCSI
 * transparent), protocol 0x50 (Bulk-Only Transport) -- the trio every
 * consumer thumb drive reports.
 *
 * @param[in] d Interface descriptor bytes (9 valid bytes).
 * @return true when the interface is MSC SCSI BOT.
 * @retval false Any of the three class fields differs.
 * @pre @p d is non-NULL and points at an interface descriptor.
 * @pre The descriptor passed the walker's length check.
 * @post No state changes.
 * @post @p d is unmodified.
 * @note Pure helper for ::internal_enum_walk_cfg.
 * @since 0.1.0
 */
static bool internal_enum_iface_is_msc(const uint8_t* d)
{
  if (d[k_ra_hmsc_off_iface_class] != (uint8_t)k_ra_hmsc_class_msc) {
    return false;
  }
  if (d[k_ra_hmsc_off_iface_sub] != (uint8_t)k_ra_hmsc_subclass_scsi) {
    return false;
  }
  if (d[k_ra_hmsc_off_iface_proto] != (uint8_t)k_ra_hmsc_protocol_bbb) {
    return false;
  }
  return true;
}

/**
 * @brief Walk a configuration blob for the MSC interface + bulk endpoints.
 *
 * @details Strides descriptor-by-descriptor; an interface descriptor with
 * class 0x08 / subclass 0x06 / protocol 0x50 opens the MSC scope, and the
 * bulk endpoints inside it populate the device snapshot.
 *
 * @param[in] cfg Configuration descriptor bytes.
 * @param[in] len Valid byte count in @p cfg.
 * @return Walk outcome.
 * @retval k_ra_ok           Both bulk endpoints were found.
 * @retval k_ra_err_hw_error No MSC bulk endpoint pair in the blob.
 * @pre @p cfg is non-NULL with @p len valid bytes.
 * @pre The device snapshot endpoint slots start zeroed.
 * @post On k_ra_ok the snapshot carries eps, max packets, and iface.
 * @post @p cfg is unmodified.
 * @note Pure helper for ::internal_enum_read_config.
 * @since 0.1.0
 */
static ra_err_t internal_enum_walk_cfg(const uint8_t* cfg, uint16_t len)
{
  uint16_t off    = 0U;
  bool     in_msc = false;
  while (off < len) {
    const uint8_t dlen = cfg[off + (uint16_t)k_ra_hmsc_off_dlen];
    if (dlen == 0U) {
      break;
    }
    const uint8_t dtype = cfg[off + (uint16_t)k_ra_hmsc_off_dtype];
    if (dtype == (uint8_t)k_ra_hmsc_desc_interface) {
      in_msc = internal_enum_iface_is_msc(&cfg[off]);
      if (in_msc) {
        s_state.device.interface_number = cfg[off + (uint16_t)k_ra_hmsc_off_iface_num];
      }
    }
    if (dtype == (uint8_t)k_ra_hmsc_desc_endpoint) {
      if (in_msc) {
        internal_enum_note_endpoint(&cfg[off]);
      }
    }
    off = (uint16_t)(off + dlen);
  }
  if (s_state.device.bulk_in_ep == 0U) {
    return k_ra_err_hw_error;
  }
  if (s_state.device.bulk_out_ep == 0U) {
    return k_ra_err_hw_error;
  }
  return k_ra_ok;
}

/**
 * @brief Read + parse the configuration descriptor set.
 *
 * @details Reads the 9-byte header for wTotalLength + bConfigurationValue,
 * re-reads the full set (clamped to the local buffer), and walks it for
 * the MSC interface and bulk endpoints.
 *
 * @param[out] out_cfg_value Receives bConfigurationValue.
 * @return First failing step's error, or k_ra_ok.
 * @retval k_ra_ok The device snapshot carries the MSC endpoints.
 * @pre The device is addressed and answering control reads.
 * @pre @p out_cfg_value is non-NULL.
 * @post On success the snapshot endpoints + interface are filled.
 * @post `*out_cfg_value` holds the value SET_CONFIGURATION needs.
 * @note Blocking (two polled control reads).
 * @since 0.1.0
 */
static ra_err_t internal_enum_read_config(uint8_t* out_cfg_value)
{
  uint8_t        cfg[k_ra_hmsc_cfg_buf_len] = {};
  uint16_t       rx                         = 0U;
  ra_usb_setup_t setup                      = {
    .bm_request_type = k_ra_hmsc_bm_std_dev_in,
    .b_request       = k_ra_hmsc_breq_get_descriptor,
    .w_value         = (uint16_t)((uint16_t)k_ra_hmsc_desc_configuration << k_ra_hmsc_byte_bits),
    .w_index         = 0U,
    .w_length        = k_ra_hmsc_cfg_desc_len,
  };
  ra_err_t err =
    ra_usb_host_control_xfer(s_state.speed, &setup, cfg, (uint16_t)k_ra_hmsc_cfg_desc_len, &rx);
  RA_RETURN_ON_ERROR(err, s_tag, "enum: cfg header"); /* GCOVR_EXCL_BR_LINE */
  uint16_t total =
    (uint16_t)((uint16_t)cfg[k_ra_hmsc_off_cfg_total] |
               (uint16_t)((uint16_t)cfg[k_ra_hmsc_off_cfg_total + 1U] << k_ra_hmsc_byte_bits));
  if (total > (uint16_t)k_ra_hmsc_cfg_buf_len) {
    total = (uint16_t)k_ra_hmsc_cfg_buf_len;
  }
  *out_cfg_value = cfg[k_ra_hmsc_off_cfg_value];
  setup.w_length = total;
  err            = ra_usb_host_control_xfer(s_state.speed, &setup, cfg, total, &rx);
  RA_RETURN_ON_ERROR(err, s_tag, "enum: cfg full"); /* GCOVR_EXCL_BR_LINE */
  return internal_enum_walk_cfg(cfg, rx);
}

/**
 * @brief SET_CONFIGURATION, best-effort GET_MAX_LUN, and pipe setup.
 *
 * @details Activates the parsed configuration (strict status), issues the
 * class GET_MAX_LUN (devices may STALL it, which legally means LUN 0, so
 * failures default to 0), then programs the bulk pipes against the
 * snapshot endpoints at @p dev_addr.
 *
 * @param[in] dev_addr  Address the device answers at.
 * @param[in] cfg_value bConfigurationValue to activate.
 * @return First failing step's error, or k_ra_ok.
 * @retval k_ra_ok The device is configured and both pipes are ready.
 * @pre ::internal_enum_read_config filled the snapshot.
 * @pre The DCP targets @p dev_addr.
 * @post The bulk pipes are configured (DATA0, parked NAK).
 * @post `s_state.device.max_lun` is filled (0 on GET_MAX_LUN failure).
 * @note Blocking (polled control transfers).
 * @since 0.1.0
 */
static ra_err_t internal_enum_configure(uint8_t dev_addr, uint8_t cfg_value)
{
  const ra_usb_setup_t set_cfg = {
    .bm_request_type = k_ra_hmsc_bm_std_dev_out,
    .b_request       = k_ra_hmsc_breq_set_config,
    .w_value         = cfg_value,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  ra_err_t err = ra_usb_host_control_xfer(s_state.speed, &set_cfg, nullptr, 0U, nullptr);
  RA_RETURN_ON_ERROR(err, s_tag, "enum: set config"); /* GCOVR_EXCL_BR_LINE */
  const ra_usb_setup_t get_lun = {
    .bm_request_type = k_ra_hmsc_bm_class_iface_in,
    .b_request       = k_ra_hmsc_req_get_max_lun,
    .w_value         = 0U,
    .w_index         = s_state.device.interface_number,
    .w_length        = k_ra_hmsc_get_max_lun_len,
  };
  uint8_t        lun     = 0U;
  uint16_t       lun_rx  = 0U;
  const ra_err_t lun_err = ra_usb_host_control_xfer(s_state.speed,
                                                    &get_lun,
                                                    &lun,
                                                    (uint16_t)k_ra_hmsc_get_max_lun_len,
                                                    &lun_rx);
  s_state.device.max_lun = 0U;
  if (lun_err == k_ra_ok) {
    if (lun_rx == (uint16_t)k_ra_hmsc_get_max_lun_len) {
      s_state.device.max_lun = lun;
    }
  }
  err = ra_usb_host_pipe_setup(s_state.speed,
                               k_ra_hmsc_pipe_bulk_in,
                               dev_addr,
                               s_state.device.bulk_in_ep,
                               true,
                               s_state.device.bulk_in_max_packet);
  RA_RETURN_ON_ERROR(err, s_tag, "enum: pipe in"); /* GCOVR_EXCL_BR_LINE */
  return ra_usb_host_pipe_setup(s_state.speed,
                                k_ra_hmsc_pipe_bulk_out,
                                dev_addr,
                                s_state.device.bulk_out_ep,
                                false,
                                s_state.device.bulk_out_max_packet);
}

/**
 * @brief Implementation of `ra_usb_hmsc_enumerate()`.
 * @details See the public header for the documented contract; runs the
 *          hardware-proven polled ladder: attach wait, (reset, address)
 *          hunt, address assignment, configuration parse + activate,
 *          GET_MAX_LUN, bulk pipe setup, then fires the attach callback.
 * @param[out] out_device See header (may be NULL).
 * @return Result code.
 * @retval k_ra_ok Device enumerated; SCSI calls may follow.
 * @pre ::ra_usb_hmsc_init succeeded and VBUS reaches the device.
 * @pre ::ra_time_init has run.
 * @post On success `s_state.attached` is true and the snapshot is filled.
 * @post The registered attach callback (if any) has fired.
 * @note Blocking; bounded by the ladder timeouts.
 * @since 0.1.0
 */
/**
 * @brief Unpack VID/PID from a device descriptor into the snapshot.
 *
 * @details Little-endian 16-bit fields at idVendor/idProduct.
 *
 * @param[in] desc Device descriptor bytes (18 valid bytes).
 * @pre @p desc is non-NULL and holds a device descriptor.
 * @pre The snapshot was reset for this enumeration pass.
 * @post `s_state.device.vendor_id` / `.product_id` are filled.
 * @post @p desc is unmodified.
 * @note Pure helper for ::ra_usb_hmsc_enumerate.
 * @since 0.1.0
 */
static void internal_enum_fill_ids(const uint8_t* desc)
{
  s_state.device.vendor_id =
    (uint16_t)((uint16_t)desc[k_ra_hmsc_off_dev_vid] |
               (uint16_t)((uint16_t)desc[k_ra_hmsc_off_dev_vid + 1U] << k_ra_hmsc_byte_bits));
  s_state.device.product_id =
    (uint16_t)((uint16_t)desc[k_ra_hmsc_off_dev_pid] |
               (uint16_t)((uint16_t)desc[k_ra_hmsc_off_dev_pid + 1U] << k_ra_hmsc_byte_bits));
}

/**
 * @brief Publish a completed enumeration: snapshot, callback, out-copy.
 *
 * @details Stores the address, flips the attached flag, fires the
 * registered attach callback, and copies the snapshot to the caller.
 *
 * @param[in]  dev_addr   Address the device answers at.
 * @param[out] out_device Caller's snapshot copy (may be NULL).
 * @pre The bulk pipes are configured and SCSI calls may follow.
 * @pre The snapshot carries VID/PID, endpoints, and max-LUN.
 * @post `s_state.attached` is true; the callback (if any) has fired.
 * @post `*out_device` holds the snapshot when @p out_device is non-NULL.
 * @note Helper for ::ra_usb_hmsc_enumerate.
 * @since 0.1.0
 */
static void internal_enum_publish(uint8_t dev_addr, ra_usb_hmsc_device_t* out_device)
{
  s_state.device.device_address = dev_addr;
  s_state.attached              = true;
  if (s_state.attach_cb != nullptr) {
    s_state.attach_cb(s_state.attach_ctx, &s_state.device);
  }
  if (out_device != nullptr) {
    *out_device = s_state.device;
  }
}

/**
 * @brief Run the enumeration ladder: hunt, address, configure, pipes.
 *
 * @details Waits for the attach, hunts the (reset, address) combination
 * the device answers on, unpacks VID/PID, assigns address 1, parses +
 * activates the configuration, and programs the bulk pipes.
 *
 * @param[out] out_addr Receives the address the device answers at.
 * @return First failing step's error, or k_ra_ok.
 * @retval k_ra_ok The device is configured and both pipes are ready.
 * @pre ::ra_usb_hmsc_init succeeded and VBUS reaches the device.
 * @pre The snapshot was reset for this enumeration pass.
 * @post On success the snapshot carries IDs, endpoints, and max-LUN.
 * @post On failure the controller may need a fresh attach cycle.
 * @note Helper for ::ra_usb_hmsc_enumerate (statement-count split).
 * @since 0.1.0
 */
static ra_err_t internal_enum_ladder(uint8_t* out_addr)
{
  uint8_t  desc[k_ra_hmsc_dev_desc_len] = {};
  ra_err_t err                          = internal_enum_hunt(desc, out_addr);
  RA_RETURN_ON_ERROR(err, s_tag, "enumerate: hunt"); /* GCOVR_EXCL_BR_LINE */
  internal_enum_fill_ids(desc);

  err = internal_enum_assign_addr(out_addr);
  RA_RETURN_ON_ERROR(err, s_tag, "enumerate: address"); /* GCOVR_EXCL_BR_LINE */
  uint8_t cfg_value = 0U;
  err               = internal_enum_read_config(&cfg_value);
  RA_RETURN_ON_ERROR(err, s_tag, "enumerate: config"); /* GCOVR_EXCL_BR_LINE */
  err = internal_enum_configure(*out_addr, cfg_value);
  RA_RETURN_ON_ERROR(err, s_tag, "enumerate: configure"); /* GCOVR_EXCL_BR_LINE */
  return k_ra_ok;
}

ra_err_t ra_usb_hmsc_enumerate(ra_usb_hmsc_device_t* out_device)
{
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  s_state.attached = false;
  s_state.device   = (ra_usb_hmsc_device_t){};

  uint8_t        dev_addr = 0U;
  const ra_err_t err      = internal_enum_ladder(&dev_addr);
  if (err != k_ra_ok) {
    return err;
  }
  internal_enum_publish(dev_addr, out_device);
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
  return ra_usb_host_bulk_out(s_state.speed, k_ra_hmsc_pipe_bulk_out, cbw, k_ra_hmsc_cbw_len);
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
  const uint16_t cap = *inout_len;
  return ra_usb_host_bulk_in(s_state.speed, k_ra_hmsc_pipe_bulk_in, dst, cap, inout_len);
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

/* Build CBW + push it on bulk-OUT -- see surrounding code and HUM citations. */
static ra_err_t internal_issue_cbw(uint8_t        target_lun,
                                   uint32_t       xfer_len,
                                   bool           data_in,
                                   const uint8_t* cdb,
                                   uint8_t        cdb_len,
                                   uint32_t*      out_tag)
{
  uint8_t        cbw[k_ra_hmsc_cbw_len] = {};
  const ra_err_t cbw_err = ra_usb_hmsc_build_cbw(target_lun, xfer_len, data_in, cdb, cdb_len, cbw);
  RA_RETURN_ON_ERROR(cbw_err, s_tag, "issue_cbw: build cbw"); /* GCOVR_EXCL_BR_LINE */
  *out_tag = internal_unpack_u32_le(&cbw[k_ra_hmsc_cbw_off_tag]);
  return internal_send_cbw(cbw);
}

/**
 * @brief Read and validate the 13-byte CSW that closes a BOT exchange.
 *
 * @details Pulls the CSW from the bulk-IN pipe, requires the full 13
 * bytes, validates the signature + tag echo via ::ra_usb_hmsc_decode_csw,
 * and maps any non-PASSED status to ::k_ra_err_hw_error.
 *
 * @param[in] expected_tag dCBWTag of the CBW that opened the exchange.
 * @return Exchange outcome.
 * @retval k_ra_ok           CSW signature/tag matched and status PASSED.
 * @retval k_ra_err_hw_error Short CSW, bad signature/tag, or FAILED status.
 * @pre The CBW (and any data stage) for this exchange completed.
 * @pre The bulk pipes are configured (::ra_usb_hmsc_enumerate).
 * @post The exchange is closed; the device is ready for the next CBW.
 * @post No state is modified on success.
 * @note Blocking (one bounded bulk-IN wait).
 * @since 0.1.0
 */
static ra_err_t internal_read_csw(uint32_t expected_tag)
{
  uint8_t  csw[k_ra_hmsc_csw_len] = {};
  uint16_t len                    = k_ra_hmsc_csw_len;
  ra_err_t err                    = internal_recv_bytes(csw, &len);
  RA_RETURN_ON_ERROR(err, s_tag, "read_csw: bulk in"); /* GCOVR_EXCL_BR_LINE */
  if (len != (uint16_t)k_ra_hmsc_csw_len) {
    return k_ra_err_hw_error;
  }
  ra_usb_hmsc_csw_status_t status = k_ra_hmsc_csw_status_phase_error;
  err                             = ra_usb_hmsc_decode_csw(csw, expected_tag, &status);
  RA_RETURN_ON_ERROR(err, s_tag, "read_csw: decode"); /* GCOVR_EXCL_BR_LINE */
  if (status != k_ra_hmsc_csw_status_passed) {
    return k_ra_err_hw_error;
  }
  return k_ra_ok;
}

/* function -- see surrounding code and HUM citations. */
static ra_err_t internal_run_data_in(uint8_t        target_lun,
                                     const uint8_t* cdb,
                                     uint8_t        cdb_len,
                                     uint8_t*       out_buf,
                                     uint16_t*      inout_len)
{
  uint32_t       tag     = 0U;
  const ra_err_t cbw_err = internal_issue_cbw(target_lun, *inout_len, true, cdb, cdb_len, &tag);
  RA_RETURN_ON_ERROR(cbw_err, s_tag, "run_data_in: issue cbw"); /* GCOVR_EXCL_BR_LINE */
  const ra_err_t derr = internal_recv_bytes(out_buf, inout_len);
  RA_RETURN_ON_ERROR(derr, s_tag, "run_data_in: data"); /* GCOVR_EXCL_BR_LINE */
  return internal_read_csw(tag);
}

/* function -- see surrounding code and HUM citations. */
static ra_err_t internal_run_data_out(uint8_t        target_lun,
                                      const uint8_t* cdb,
                                      uint8_t        cdb_len,
                                      const uint8_t* in_buf,
                                      uint16_t       push_len)
{
  uint32_t       tag = 0U;
  const ra_err_t cbw_err =
    internal_issue_cbw(target_lun, (uint32_t)push_len, false, cdb, cdb_len, &tag);
  RA_RETURN_ON_ERROR(cbw_err, s_tag, "run_data_out: issue cbw"); /* GCOVR_EXCL_BR_LINE */
  /* The bulk-OUT primitive ships one packet per call, so chunk at the
   * DEVICE's enumerated bulk-OUT wMaxPacketSize -- NOT the host
   * controller's native ceiling. On a HS host driving an FS device (the
   * self-loop) the controller speed is HS (512) but the endpoint is FS
   * (64); chunking at 512 advances the offset 8x faster than the 64-byte
   * packet actually sent, so the device receives only 1/8 of the data
   * and the WRITE wedges. The pipe's PIPEMAXP is already this value
   * (see the enum pipe setup). Fall back to the speed ceiling only if
   * enumeration left it unset. */
  uint16_t mps = s_state.device.bulk_out_max_packet;
  if (mps == 0U) {
    mps = internal_bulk_max_packet(s_state.speed);
  }
  uint16_t offset = 0U;
  while (offset < push_len) {
    uint16_t chunk = (uint16_t)(push_len - offset);
    if (chunk > mps) {
      chunk = mps;
    }
    const ra_err_t werr =
      ra_usb_host_bulk_out(s_state.speed, k_ra_hmsc_pipe_bulk_out, &in_buf[offset], chunk);
    RA_RETURN_ON_ERROR(werr, s_tag, "run_data_out: data chunk"); /* GCOVR_EXCL_BR_LINE */
    offset = (uint16_t)(offset + chunk);
  }
  return internal_read_csw(tag);
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
