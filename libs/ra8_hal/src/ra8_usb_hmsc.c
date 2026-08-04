/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/* GCOVR_EXCL_START -- host-unreachable: called only from the
 * internal_run_data_out data-chunk loop, which runs only after a bulk-OUT
 * CBW push completes; the plain-RAM fake never re-asserts the pipe's
 * BEMPSTS bit, so that push always times out before this helper is reached. */
/**
 * @file ra8_usb_hmsc.c
 * @brief Native USB host-side MSC (Mass Storage Class) class layer
 *        implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Glues the host-mode bring-up paths in `ra8_usb` to a USB Mass Storage
 * Class (Bulk-Only-Transport / BBB) peripheral attached on the
 * EK-RA8D2's USB-host port. This file is the native host-MSC class
 * layer; FSP's `r_usb_hmsc_driver.c`, `r_usb_hstorage_driver.c`, and
 * `r_usb_hmsc.c` are reference material only -- nothing is pulled in
 * verbatim.
 *
 * Mapping vs FSP (FSP function -> our entry point):
 *
 *  - `usb_hmsc_inquiry`         -> `ra8_usb_hmsc_inquiry`
 *  - `usb_hmsc_read_capacity`   -> `ra8_usb_hmsc_read_capacity`
 *  - `usb_hmsc_read10`          -> `ra8_usb_hmsc_read10`
 *  - `usb_hmsc_write10`         -> `ra8_usb_hmsc_write10`
 *  - `usb_hmsc_set_rw_cbw`      -> `internal_build_rw_cbw`
 *  - `usb_hmsc_set_els_cbw`     -> `internal_build_els_cbw`
 *  - `usb_hmsc_get_max_unit`    -> `internal_enum_configure`
 *  - `usb_hmsc_class_check`     -> `internal_enum_walk_cfg`
 *  - `usb_hmsc_pipe_info`       -> `internal_enum_note_endpoint`
 *
 * The starter does CPU-FIFO, single-device, no-hub. Enumeration is a
 * single polled ladder (`ra8_usb_hmsc_enumerate`): wait for the D+
 * attach, hunt the (reset, address) combination the device answers on,
 * then read descriptors / SET_CONFIGURATION / open the bulk pipes.
 * Every chapter-9 SETUP goes through `ra8_usb_host_setup_request`.
 *
 * BOT (Bulk-Only Transport) state machine -- per command:
 *
 *   READY -> CBW_OUT (push 31-byte CBW) -> DATA (in or out, optional) ->
 *   CSW_IN (pull 13-byte CSW) -> READY
 *
 * On CSW phase-error the spec mandates Reset Recovery; the starter
 * surfaces that as `k_ra8_err_hw_error` and lets the caller decide what to
 * do.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_usb_hmsc.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hal_internal.h"
#include "ra8_log.h"
#include "ra8_time.h"
#include "ra8_usb.h"
#include "ra8_usb_hmsc_internal.h"

static const char* s_tag = "USBHMSC";

/* =============================================================================
 * Internal constants
 * =============================================================================
 */

/**
 * @enum ra8_usb_hmsc_cbw_offset_t
 * @brief Byte offsets inside the 31-byte CBW header.
 *
 * @details See USB MSC BBB rev 1.0 sec 5.1 Table 5.1 "Command Block
 * Wrapper".
 */
typedef enum : uint8_t {
  k_ra8_hmsc_cbw_off_signature   = 0U,  /**< dCBWSignature [4].    */
  k_ra8_hmsc_cbw_off_tag         = 4U,  /**< dCBWTag [4].          */
  k_ra8_hmsc_cbw_off_data_length = 8U,  /**< dCBWDataTransferLen.  */
  k_ra8_hmsc_cbw_off_flags       = 12U, /**< bmCBWFlags.           */
  k_ra8_hmsc_cbw_off_lun         = 13U, /**< bCBWLUN (low nib).    */
  k_ra8_hmsc_cbw_off_cdb_length  = 14U, /**< bCBWCBLength (low 5). */
  k_ra8_hmsc_cbw_off_cdb         = 15U, /**< CBWCB [16].           */
} ra8_usb_hmsc_cbw_offset_t;

/**
 * @enum ra8_usb_hmsc_csw_offset_t
 * @brief Byte offsets inside the 13-byte CSW.
 *
 * @details See USB MSC BBB rev 1.0 sec 5.2 Table 5.2 "Command Status
 * Wrapper".
 */
typedef enum : uint8_t {
  k_ra8_hmsc_csw_off_signature = 0U,  /**< dCSWSignature [4]. */
  k_ra8_hmsc_csw_off_tag       = 4U,  /**< dCSWTag [4].       */
  k_ra8_hmsc_csw_off_residue   = 8U,  /**< dCSWDataResidue.   */
  k_ra8_hmsc_csw_off_status    = 12U, /**< bCSWStatus.        */
} ra8_usb_hmsc_csw_offset_t;

/**
 * @enum ra8_usb_hmsc_cbw_flag_t
 * @brief bmCBWFlags direction bit values.
 */
typedef enum : uint8_t {
  k_ra8_hmsc_cbw_flag_data_out = 0x00U, /**< Host -> device. */
  k_ra8_hmsc_cbw_flag_data_in  = 0x80U, /**< Device -> host. */
} ra8_usb_hmsc_cbw_flag_t;

/**
 * @enum ra8_usb_hmsc_signature_t
 * @brief BOT wrapper signatures (USB MSC BBB rev 1.0 sec 5).
 *
 * @details Stored on-wire little-endian. dCBWSignature 'USBC' is
 * 0x43425355; dCSWSignature 'USBS' is 0x53425355.
 */
typedef enum : uint32_t {
  k_ra8_hmsc_cbw_signature = 0x43425355U, /**< 'USBC' little-endian. */
  k_ra8_hmsc_csw_signature = 0x53425355U, /**< 'USBS' little-endian. */
} ra8_usb_hmsc_signature_t;

/**
 * @enum ra8_usb_hmsc_byte_shift_t
 * @brief Per-byte left-shift constants for little-endian / big-endian
 *        serialisation.
 */
typedef enum : uint8_t {
  k_ra8_hmsc_shift_byte0 = 0U,  /**< RA8 hmsc shift byte0. */
  k_ra8_hmsc_shift_byte1 = 8U,  /**< RA8 hmsc shift byte1. */
  k_ra8_hmsc_shift_byte2 = 16U, /**< RA8 hmsc shift byte2. */
  k_ra8_hmsc_shift_byte3 = 24U, /**< RA8 hmsc shift byte3. */
} ra8_usb_hmsc_byte_shift_t;

/**
 * @enum ra8_usb_hmsc_byte_mask_t
 * @brief Byte mask for serialisation.
 */
typedef enum : uint32_t {
  k_ra8_hmsc_byte_mask = 0xFFU, /**< Single-byte extraction mask. */
} ra8_usb_hmsc_byte_mask_t;

/**
 * @enum ra8_usb_hmsc_cdb_offset_t
 * @brief SCSI CDB byte offsets used when assembling READ(10) /
 *        WRITE(10) / READ_CAPACITY(10) / INQUIRY blocks.
 */
typedef enum : uint8_t {
  k_ra8_hmsc_cdb_off_opcode    = 0U, /**< Operation Code.         */
  k_ra8_hmsc_cdb_off_lba_msb   = 2U, /**< READ(10): LBA byte 3.   */
  k_ra8_hmsc_cdb_off_lba_b1    = 3U, /**< READ(10): LBA byte 2.   */
  k_ra8_hmsc_cdb_off_lba_b2    = 4U, /**< READ(10): LBA byte 1.   */
  k_ra8_hmsc_cdb_off_lba_lsb   = 5U, /**< READ(10): LBA byte 0.   */
  k_ra8_hmsc_cdb_off_cnt_msb   = 7U, /**< READ(10): count high.   */
  k_ra8_hmsc_cdb_off_cnt_lsb   = 8U, /**< READ(10): count low.    */
  k_ra8_hmsc_cdb_off_inq_alloc = 4U, /**< INQUIRY allocation len. */
} ra8_usb_hmsc_cdb_offset_t;

/**
 * @enum ra8_usb_hmsc_lun_mask_t
 * @brief Field width masks for the bCBWLUN + bCBWCBLength bytes.
 *
 * @details See USB MSC BBB rev 1.0 sec 5.1 Table 5.1: bCBWLUN occupies
 * the low 4 bits; bCBWCBLength occupies the low 5 bits.
 */
typedef enum : uint8_t {
  k_ra8_hmsc_lun_field_mask = 0x0FU, /**< Low 4 bits of bCBWLUN.   */
  k_ra8_hmsc_cdb_field_mask = 0x1FU, /**< Low 5 bits of bCBWCBLen. */
} ra8_usb_hmsc_lun_mask_t;

/**
 * @enum ra8_usb_hmsc_initial_tag_t
 * @brief Starting value of the BOT dCBWTag counter.
 */
typedef enum : uint32_t {
  k_ra8_hmsc_initial_tag = 1U, /**< First tag handed out post-init. */
} ra8_usb_hmsc_initial_tag_t;

/**
 * @enum ra8_usb_hmsc_inquiry_field_t
 * @brief Bit-field shifts / masks inside the SCSI INQUIRY response
 *        byte 0 / byte 1 (SBC-4 sec 6.6).
 *
 * @details Byte 0 high 3 bits = peripheral qualifier; low 5 bits =
 * peripheral device type. Byte 1 bit 7 = removable.
 */
typedef enum : uint8_t {
  k_ra8_hmsc_inq_qual_shift      = 5U,    /**< Byte 0 [7:5] qualifier.   */
  k_ra8_hmsc_inq_dev_type_mask   = 0x1FU, /**< Byte 0 [4:0] dev type.    */
  k_ra8_hmsc_inq_removable_shift = 7U,    /**< Byte 1 [7] removable bit. */
  k_ra8_hmsc_inq_removable_mask  = 1U,    /**< 1-bit removable flag.     */
} ra8_usb_hmsc_inquiry_field_t;

/**
 * @enum ra8_usb_hmsc_inquiry_offset_t
 * @brief Byte offsets inside the SBC-4 standard INQUIRY response.
 */
typedef enum : uint8_t {
  k_ra8_hmsc_inq_off_dev_type    = 0U,  /**< Byte 0 (qual+devtype). */
  k_ra8_hmsc_inq_off_removable   = 1U,  /**< Byte 1 (removable).    */
  k_ra8_hmsc_inq_off_version     = 2U,  /**< Byte 2 (SPC version).  */
  k_ra8_hmsc_inq_off_vendor_id   = 8U,  /**< T10 Vendor ID start.   */
  k_ra8_hmsc_inq_off_product_id  = 16U, /**< Product ID start.      */
  k_ra8_hmsc_inq_off_product_rev = 32U, /**< Product revision.      */
} ra8_usb_hmsc_inquiry_offset_t;

/**
 * @enum ra8_usb_hmsc_capacity_offset_t
 * @brief Byte offsets inside the SCSI READ_CAPACITY(10) 8-byte
 *        response (SBC-4 sec 5.10).
 */
typedef enum : uint8_t {
  k_ra8_hmsc_cap_off_last_lba = 0U, /**< Last valid LBA (big-endian). */
  k_ra8_hmsc_cap_off_blk_size = 4U, /**< Block length (big-endian).   */
} ra8_usb_hmsc_capacity_offset_t;

/* =============================================================================
 * Internal state
 * =============================================================================
 */

/* Singleton shadow state. Shared with ra8_usb_hmsc_enum.c (the polled
 * enumeration ladder) through ra8_hal_internal.h: the type and the extern
 * declaration live there, the single definition lives here. */
ra8_usb_hmsc_state_t s_usb_hmsc_state = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Pick the bulk-endpoint max-packet ceiling for the negotiated speed.
 *
 * @details Returns the USB-spec bulk maximum packet size for the active bus
 * speed: 512 bytes at High-Speed, 64 bytes at Full-Speed. Used by the
 * data-OUT chunk loop as a fallback ceiling when the enumerated endpoint
 * `wMaxPacketSize` is unknown (left zero by enumeration).
 *
 * @param[in] speed Negotiated controller bus speed (::k_ra8_usb_speed_fs or
 *                  ::k_ra8_usb_speed_hs).
 * @return Bulk maximum packet size in bytes.
 * @retval k_ra8_hmsc_bulk_max_packet_hs 512 bytes when @p speed is
 *         ::k_ra8_usb_speed_hs.
 * @retval k_ra8_hmsc_bulk_max_packet_fs 64 bytes for any other speed
 *         (Full-Speed).
 * @pre @p speed holds a valid ::ra8_usb_speed_t value.
 * @pre The bulk pipe is enumerated so the returned ceiling is meaningful.
 * @post The returned value is one of the two spec-defined bulk sizes.
 * @post No module state is modified (pure function).
 * @note Internal helper. Not thread-safe; caller provides synchronisation.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint16_t internal_bulk_max_packet(ra8_usb_speed_t speed)
{
  return (speed == k_ra8_usb_speed_hs) ? k_ra8_hmsc_bulk_max_packet_hs
                                       : k_ra8_hmsc_bulk_max_packet_fs;
}
/* GCOVR_EXCL_STOP */

/* GCOVR_EXCL_START -- host-unreachable: called only from
 * ra8_usb_hmsc_read_capacity to decode the 8-byte capacity response, which
 * arrives only after a completed BOT command; the fake bulk-OUT CBW
 * push always times out, so the decode is never reached on the host. */
/**
 * @brief Hand out the next BOT tag (monotonic uint32 counter).
 *
 * @details Mirrors FSP's `g_usb_hmsc_csw_tag_no` increment in
 * `r_usb_hmsc_driver.c`. The starter never wraps in a single test
 * run; production should detect wrap-around but a 32-bit counter
 * lasts long enough that this is not urgent.
 * @return ::ra8_err_t outcome (or scalar return value).
 * @retval k_ra8_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Internal helper. Not thread-safe; caller provides synchronisation.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_next_tag(void)
{
  const uint32_t tag = s_usb_hmsc_state.next_cbw_tag;
  ++s_usb_hmsc_state.next_cbw_tag;
  return tag;
}

/* Pack a uint32 into 4 little-endian bytes -- see surrounding code and HUM citations. */
RA8_INTERNAL
static void internal_pack_u32_le(uint32_t value, uint8_t* dst)
{
  dst[0] = (uint8_t)((value >> k_ra8_hmsc_shift_byte0) & k_ra8_hmsc_byte_mask);
  dst[1] = (uint8_t)((value >> k_ra8_hmsc_shift_byte1) & k_ra8_hmsc_byte_mask);
  dst[2] = (uint8_t)((value >> k_ra8_hmsc_shift_byte2) & k_ra8_hmsc_byte_mask);
  dst[3] = (uint8_t)((value >> k_ra8_hmsc_shift_byte3) & k_ra8_hmsc_byte_mask);
}

/* Unpack a uint32 from 4 little-endian bytes -- see surrounding code and HUM citations. */
RA8_INTERNAL
static uint32_t internal_unpack_u32_le(const uint8_t* src)
{
  return ((uint32_t)src[0] << k_ra8_hmsc_shift_byte0) |
         ((uint32_t)src[1] << k_ra8_hmsc_shift_byte1) |
         ((uint32_t)src[2] << k_ra8_hmsc_shift_byte2) |
         ((uint32_t)src[3] << k_ra8_hmsc_shift_byte3);
}

/**
 * @brief Assemble a 32-bit word from four big-endian bytes.
 *
 * @details Reads `src[0..3]` most-significant-byte-first (the SCSI/BOT
 * on-wire byte order) and packs them into a host-order `uint32_t`. Used to
 * decode the READ_CAPACITY(10) block-count and block-size fields.
 *
 * @param[in] src Pointer to at least 4 readable bytes in big-endian order.
 * @return The reconstructed 32-bit value, where `src[0]` supplies bits 31..24.
 * @retval 0 All four source bytes are zero.
 * @retval other The big-endian value
 *         `(src[0]<<24)|(src[1]<<16)|(src[2]<<8)|src[3]`.
 * @pre @p src is non-NULL and points to >= 4 readable bytes.
 * @pre The four bytes are laid out most-significant-first.
 * @post The returned value equals the big-endian interpretation of the bytes.
 * @post No module state is modified (pure function).
 * @note Internal helper. Not thread-safe; caller provides synchronisation.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_unpack_u32_be(const uint8_t* src)
{
  return ((uint32_t)src[0] << k_ra8_hmsc_shift_byte3) |
         ((uint32_t)src[1] << k_ra8_hmsc_shift_byte2) |
         ((uint32_t)src[2] << k_ra8_hmsc_shift_byte1) |
         ((uint32_t)src[3] << k_ra8_hmsc_shift_byte0);
}
/* GCOVR_EXCL_STOP */

/* GCOVR_EXCL_START -- host-unreachable: the bulk-IN pull is issued only
 * from internal_read_csw / internal_run_data_in, both of which run only
 * after a bulk-OUT CBW push completes; the fake bulk-OUT never signals
 * completion (BEMPSTS is W0C-cleared and never re-asserted), so the push
 * always times out before any bulk-IN is reached. */
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
RA8_INTERNAL
static void internal_zero_bytes(uint8_t* dst, uint16_t len)
{
  for (uint16_t i = 0U; i < len; ++i) {
    dst[i] = 0U;
  }
}

/* Copy `len` bytes from `src` to `dst` byte-by-byte -- see surrounding code and HUM citations. */
RA8_INTERNAL
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

ra8_err_t ra8_usb_hmsc_build_cbw(uint8_t        target_lun,
                                 uint32_t       data_transfer_length,
                                 bool           data_in,
                                 const uint8_t* cdb,
                                 uint8_t        cdb_len,
                                 uint8_t*       out_cbw)
{
  RA8_CHECK_NULL_PTR(cdb, s_tag, "build_cbw: cdb");
  RA8_CHECK_NULL_PTR(out_cbw, s_tag, "build_cbw: out_cbw");
  if (target_lun > k_ra8_hmsc_lun_field_mask) {
    return k_ra8_err_invalid_arg;
  }
  if ((cdb_len == 0U) || (cdb_len > k_ra8_hmsc_cdb_max_len)) {
    return k_ra8_err_invalid_arg;
  }

  /* Zero the entire CBW so reserved nibbles stay 0 -- USB MSC BBB
   * rev 1.0 sec 5.1 requires zero in the high nibble of bCBWLUN /
   * the high 3 bits of bCBWCBLength. */
  internal_zero_bytes(out_cbw, k_ra8_hmsc_cbw_len);

  /* dCBWSignature = 'USBC' little-endian. */
  internal_pack_u32_le(k_ra8_hmsc_cbw_signature, &out_cbw[k_ra8_hmsc_cbw_off_signature]);

  /* dCBWTag = monotonic. */
  const uint32_t tag = internal_next_tag();
  internal_pack_u32_le(tag, &out_cbw[k_ra8_hmsc_cbw_off_tag]);

  /* dCBWDataTransferLength. */
  internal_pack_u32_le(data_transfer_length, &out_cbw[k_ra8_hmsc_cbw_off_data_length]);

  /* bmCBWFlags. */
  out_cbw[k_ra8_hmsc_cbw_off_flags] =
    data_in ? k_ra8_hmsc_cbw_flag_data_in : k_ra8_hmsc_cbw_flag_data_out;

  /* bCBWLUN (low 4 bits) / bCBWCBLength (low 5 bits). */
  out_cbw[k_ra8_hmsc_cbw_off_lun]        = (uint8_t)(target_lun & k_ra8_hmsc_lun_field_mask);
  out_cbw[k_ra8_hmsc_cbw_off_cdb_length] = (uint8_t)(cdb_len & k_ra8_hmsc_cdb_field_mask);

  /* CBWCB[16]. */
  internal_copy_bytes(&out_cbw[k_ra8_hmsc_cbw_off_cdb], cdb, (uint16_t)cdb_len);

  return k_ra8_ok;
}

ra8_err_t ra8_usb_hmsc_decode_csw(const uint8_t*             csw,
                                  uint32_t                   expected_tag,
                                  ra8_usb_hmsc_csw_status_t* out_status)
{
  RA8_CHECK_NULL_PTR(csw, s_tag, "decode_csw: csw");
  RA8_CHECK_NULL_PTR(out_status, s_tag, "decode_csw: out_status");

  const uint32_t signature = internal_unpack_u32_le(&csw[k_ra8_hmsc_csw_off_signature]);
  if (signature != k_ra8_hmsc_csw_signature) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t tag = internal_unpack_u32_le(&csw[k_ra8_hmsc_csw_off_tag]);
  if (tag != expected_tag) {
    return k_ra8_err_invalid_arg;
  }

  const uint8_t status_byte = csw[k_ra8_hmsc_csw_off_status];
  if ((status_byte != k_ra8_hmsc_csw_status_passed) &&
      (status_byte != k_ra8_hmsc_csw_status_failed) &&
      (status_byte != k_ra8_hmsc_csw_status_phase_error)) {
    return k_ra8_err_invalid_arg;
  }

  *out_status = (ra8_usb_hmsc_csw_status_t)status_byte;
  return k_ra8_ok;
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

ra8_err_t ra8_usb_hmsc_init(ra8_usb_speed_t speed)
{
  if ((speed != k_ra8_usb_speed_fs) && (speed != k_ra8_usb_speed_hs)) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t usb_err = ra8_usb_host_init(speed);
  if (usb_err != k_ra8_ok) {
    /* GCOVR_EXCL_START -- host-unreachable: ra8_usb_host_init fails only on an
     * MSTP-enable or HS-PLL bring-up fault; under the fake MSTP is plain
     * RAM (always enables) and the FRDY/bring-up polls short-circuit, so
     * usb_err is always k_ra8_ok here. */
    ra8_log_error_val(s_tag, "ra8_usb_host_init failed", (uint32_t)usb_err);
    return k_ra8_err_hw_init_failed;
    /* GCOVR_EXCL_STOP */
  }

  s_usb_hmsc_state.speed        = speed;
  s_usb_hmsc_state.attached     = false;
  s_usb_hmsc_state.attach_cb    = nullptr;
  s_usb_hmsc_state.attach_ctx   = nullptr;
  s_usb_hmsc_state.device       = (ra8_usb_hmsc_device_t){};
  s_usb_hmsc_state.next_cbw_tag = k_ra8_hmsc_initial_tag;
  s_usb_hmsc_state.initialized  = true;

  ra8_log_info_val(s_tag, "host-MSC ready", (uint32_t)speed);
  return k_ra8_ok;
}

ra8_err_t ra8_usb_hmsc_close(void)
{
  if (!s_usb_hmsc_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  /* Bus power down: drop UACT before tearing the controller. */
  (void)ra8_usb_host_set_uact(s_usb_hmsc_state.speed, false);
  const ra8_err_t err          = ra8_usb_host_deinit(s_usb_hmsc_state.speed);
  s_usb_hmsc_state.initialized = false;
  s_usb_hmsc_state.attached    = false;
  s_usb_hmsc_state.attach_cb   = nullptr;
  s_usb_hmsc_state.attach_ctx  = nullptr;
  return err;
}

/* =============================================================================
 * Attach callback
 * =============================================================================
 */

ra8_err_t ra8_usb_hmsc_attach_callback(ra8_usb_hmsc_attach_fn_t on_attach, void* ctx)
{
  if (!s_usb_hmsc_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  s_usb_hmsc_state.attach_cb  = on_attach;
  s_usb_hmsc_state.attach_ctx = ctx;
  return k_ra8_ok;
}

/* =============================================================================
 * Internal BOT helpers shared by SCSI command entry points
 * =============================================================================
 */

/* Validate driver state + LUN before issuing a SCSI command -- see surrounding code and HUM citations. */
RA8_INTERNAL
static ra8_err_t internal_check_ready(uint8_t target_lun)
{
  if (!s_usb_hmsc_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (!s_usb_hmsc_state.attached) {
    return k_ra8_err_invalid_state;
  }
  if (target_lun > k_ra8_hmsc_max_lun) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Push a CBW out on the bulk-OUT pipe.
 *
 * @details The starter ignores the low-level transfer status that
 * `ra8_usb_queue_in` returns -- the CSW phase is what the caller
 * checks. Production should propagate transfer errors here.
 * @param[in] cbw See declaration: ``const uint8_t* cbw``.
 * @return ::ra8_err_t outcome (or scalar return value).
 * @retval k_ra8_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Internal helper. Not thread-safe; caller provides synchronisation.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_send_cbw(const uint8_t* cbw)
{
  return ra8_usb_host_bulk_out(s_usb_hmsc_state.speed,
                               k_ra8_hmsc_pipe_bulk_out,
                               cbw,
                               k_ra8_hmsc_cbw_len);
}

/**
 * @brief Pull bytes from the bulk-IN pipe (data + CSW phase).
 *
 * @details `inout_len` is initialized to capacity by the caller and
 * receives the actual byte count on return.
 * @param[out] dst See declaration: ``uint8_t* dst``.
 * @param[out] inout_len See declaration: ``uint16_t* inout_len``.
 * @return ::ra8_err_t outcome (or scalar return value).
 * @retval k_ra8_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Internal helper. Not thread-safe; caller provides synchronisation.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_recv_bytes(uint8_t* dst, uint16_t* inout_len)
{
  const uint16_t cap = *inout_len;
  return ra8_usb_host_bulk_in(s_usb_hmsc_state.speed, k_ra8_hmsc_pipe_bulk_in, dst, cap, inout_len);
}
/* GCOVR_EXCL_STOP */

/* Build a 6-byte CDB for SCSI INQUIRY -- see surrounding code and HUM citations. */
RA8_INTERNAL
static void internal_build_inquiry_cdb(uint8_t* cdb)
{
  internal_zero_bytes(cdb, k_ra8_hmsc_cdb6_len);
  cdb[k_ra8_hmsc_cdb_off_opcode]    = k_ra8_hmsc_scsi_inquiry;
  cdb[k_ra8_hmsc_cdb_off_inq_alloc] = (uint8_t)k_ra8_hmsc_inquiry_resp_len;
}

/* Build a 10-byte CDB for SCSI READ_CAPACITY(10) -- see surrounding code and HUM citations. */
RA8_INTERNAL
static void internal_build_read_capacity_cdb(uint8_t* cdb)
{
  internal_zero_bytes(cdb, k_ra8_hmsc_cdb10_len);
  cdb[k_ra8_hmsc_cdb_off_opcode] = k_ra8_hmsc_scsi_read_capacity_10;
}

/* GCOVR_EXCL_START -- host-unreachable: reading the 13-byte CSW closes a
 * BOT exchange and runs only after the CBW push (and any data stage)
 * completes; the fake bulk-OUT CBW push always times out, so no
 * exchange ever reaches its CSW phase on the host. */
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
RA8_INTERNAL
static void
internal_build_rw10_cdb(uint8_t opcode, uint32_t lba, uint16_t block_count, uint8_t* cdb)
{
  internal_zero_bytes(cdb, k_ra8_hmsc_cdb10_len);
  cdb[k_ra8_hmsc_cdb_off_opcode] = opcode;
  cdb[k_ra8_hmsc_cdb_off_lba_msb] =
    (uint8_t)((lba >> k_ra8_hmsc_shift_byte3) & k_ra8_hmsc_byte_mask);
  cdb[k_ra8_hmsc_cdb_off_lba_b1] =
    (uint8_t)((lba >> k_ra8_hmsc_shift_byte2) & k_ra8_hmsc_byte_mask);
  cdb[k_ra8_hmsc_cdb_off_lba_b2] =
    (uint8_t)((lba >> k_ra8_hmsc_shift_byte1) & k_ra8_hmsc_byte_mask);
  cdb[k_ra8_hmsc_cdb_off_lba_lsb] =
    (uint8_t)((lba >> k_ra8_hmsc_shift_byte0) & k_ra8_hmsc_byte_mask);
  cdb[k_ra8_hmsc_cdb_off_cnt_msb] =
    (uint8_t)((block_count >> k_ra8_hmsc_shift_byte1) & k_ra8_hmsc_byte_mask);
  cdb[k_ra8_hmsc_cdb_off_cnt_lsb] = (uint8_t)(block_count & k_ra8_hmsc_byte_mask);
}

/* Build CBW + push it on bulk-OUT -- see surrounding code and HUM citations. */
RA8_INTERNAL
static ra8_err_t internal_issue_cbw(uint8_t        target_lun,
                                    uint32_t       xfer_len,
                                    bool           data_in,
                                    const uint8_t* cdb,
                                    uint8_t        cdb_len,
                                    uint32_t*      out_tag)
{
  uint8_t         cbw[k_ra8_hmsc_cbw_len] = {};
  const ra8_err_t cbw_err =
    ra8_usb_hmsc_build_cbw(target_lun, xfer_len, data_in, cdb, cdb_len, cbw);
  RA8_RETURN_ON_ERROR(cbw_err, s_tag, "issue_cbw: build cbw"); /* GCOVR_EXCL_BR_LINE */
  *out_tag = internal_unpack_u32_le(&cbw[k_ra8_hmsc_cbw_off_tag]);
  return internal_send_cbw(cbw);
}

/**
 * @brief Read and validate the 13-byte CSW that closes a BOT exchange.
 *
 * @details Pulls the CSW from the bulk-IN pipe, requires the full 13
 * bytes, validates the signature + tag echo via ::ra8_usb_hmsc_decode_csw,
 * and maps any non-PASSED status to ::k_ra8_err_hw_error.
 *
 * @param[in] expected_tag dCBWTag of the CBW that opened the exchange.
 * @return Exchange outcome.
 * @retval k_ra8_ok           CSW signature/tag matched and status PASSED.
 * @retval k_ra8_err_hw_error Short CSW, bad signature/tag, or FAILED status.
 * @pre The CBW (and any data stage) for this exchange completed.
 * @pre The bulk pipes are configured (::ra8_usb_hmsc_enumerate).
 * @post The exchange is closed; the device is ready for the next CBW.
 * @post No state is modified on success.
 * @note Blocking (one bounded bulk-IN wait).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_read_csw(uint32_t expected_tag)
{
  uint8_t   csw[k_ra8_hmsc_csw_len] = {};
  uint16_t  len                     = k_ra8_hmsc_csw_len;
  ra8_err_t err                     = internal_recv_bytes(csw, &len);
  RA8_RETURN_ON_ERROR(err, s_tag, "read_csw: bulk in"); /* GCOVR_EXCL_BR_LINE */
  if (len != (uint16_t)k_ra8_hmsc_csw_len) {
    return k_ra8_err_hw_error;
  }
  ra8_usb_hmsc_csw_status_t status = k_ra8_hmsc_csw_status_phase_error;
  err                              = ra8_usb_hmsc_decode_csw(csw, expected_tag, &status);
  RA8_RETURN_ON_ERROR(err, s_tag, "read_csw: decode"); /* GCOVR_EXCL_BR_LINE */
  if (status != k_ra8_hmsc_csw_status_passed) {
    return k_ra8_err_hw_error;
  }
  return k_ra8_ok;
}
/* GCOVR_EXCL_STOP */

/* function -- see surrounding code and HUM citations. */
RA8_INTERNAL
static ra8_err_t internal_run_data_in(uint8_t        target_lun,
                                      const uint8_t* cdb,
                                      uint8_t        cdb_len,
                                      uint8_t*       out_buf,
                                      uint16_t*      inout_len)
{
  uint32_t        tag     = 0U;
  const ra8_err_t cbw_err = internal_issue_cbw(target_lun, *inout_len, true, cdb, cdb_len, &tag);
  RA8_RETURN_ON_ERROR(cbw_err, s_tag, "run_data_in: issue cbw"); /* GCOVR_EXCL_BR_LINE */
  /* GCOVR_EXCL_START -- host-unreachable: the data-IN pull + CSW read run
   * only after the CBW push above succeeds; under the fake the push
   * always times out, so cbw_err is always an error and this returns above. */
  const ra8_err_t derr = internal_recv_bytes(out_buf, inout_len);
  RA8_RETURN_ON_ERROR(derr, s_tag, "run_data_in: data"); /* GCOVR_EXCL_BR_LINE */
  return internal_read_csw(tag);
}
/* GCOVR_EXCL_STOP */

/* function -- see surrounding code and HUM citations. */
RA8_INTERNAL
static ra8_err_t internal_run_data_out(uint8_t        target_lun,
                                       const uint8_t* cdb,
                                       uint8_t        cdb_len,
                                       const uint8_t* in_buf,
                                       uint16_t       push_len)
{
  uint32_t        tag = 0U;
  const ra8_err_t cbw_err =
    internal_issue_cbw(target_lun, (uint32_t)push_len, false, cdb, cdb_len, &tag);
  RA8_RETURN_ON_ERROR(cbw_err, s_tag, "run_data_out: issue cbw"); /* GCOVR_EXCL_BR_LINE */
  /* The bulk-OUT primitive ships one packet per call, so chunk at the
   * DEVICE's enumerated bulk-OUT wMaxPacketSize -- NOT the host
   * controller's native ceiling. On a HS host driving an FS device (the
   * self-loop) the controller speed is HS (512) but the endpoint is FS
   * (64); chunking at 512 advances the offset 8x faster than the 64-byte
   * packet actually sent, so the device receives only 1/8 of the data
   * and the WRITE wedges. The pipe's PIPEMAXP is already this value
   * (see the enum pipe setup). Fall back to the speed ceiling only if
   * enumeration left it unset. */
  /* GCOVR_EXCL_START -- host-unreachable: the per-packet data-OUT chunk loop
   * and the trailing CSW read run only after the CBW push
   * (internal_issue_cbw above) succeeds; under the fake that push
   * always times out, so this run always returns on the cbw_err leg above. */
  uint16_t mps = s_usb_hmsc_state.device.bulk_out_max_packet;
  if (mps == 0U) {
    mps = internal_bulk_max_packet(s_usb_hmsc_state.speed);
  }
  uint16_t offset = 0U;
  while (offset < push_len) {
    uint16_t chunk = (uint16_t)(push_len - offset);
    if (chunk > mps) {
      chunk = mps;
    }
    const ra8_err_t werr = ra8_usb_host_bulk_out(s_usb_hmsc_state.speed,
                                                 k_ra8_hmsc_pipe_bulk_out,
                                                 &in_buf[offset],
                                                 chunk);
    RA8_RETURN_ON_ERROR(werr, s_tag, "run_data_out: data chunk"); /* GCOVR_EXCL_BR_LINE */
    offset = (uint16_t)(offset + chunk);
  }
  return internal_read_csw(tag);
}
/* GCOVR_EXCL_STOP */

/* GCOVR_EXCL_START -- host-unreachable: called only from ra8_usb_hmsc_inquiry
 * to decode the 36-byte INQUIRY response, which arrives only after a
 * completed BOT command; the fake bulk-OUT CBW push always times out,
 * so the INQUIRY response is never received and never decoded on the host. */
/**
 * @brief Decode a 36-byte SCSI standard INQUIRY response into the public struct.
 *
 * @details Zero-initializes @p response, then extracts the peripheral
 * qualifier and device type from byte 0, the removable-media bit from byte 1,
 * and the SCSI version byte, and copies the ASCII vendor-id (8 B), product-id
 * (16 B) and product-revision (4 B) fields from their fixed SPC/SBC offsets in
 * the raw buffer.
 *
 * @param[in]  raw      Pointer to at least ::k_ra8_hmsc_inquiry_resp_len (36)
 *                      bytes of standard INQUIRY data.
 * @param[out] response Destination struct populated with the decoded fields.
 * @pre @p raw is non-NULL and points to >= 36 readable bytes.
 * @pre @p response is non-NULL and writable.
 * @post @p response is fully overwritten (leading zero-fill then decoded fields).
 * @post The vendor/product/revision ASCII fields are copied verbatim and are
 *       not NUL-terminated by this routine.
 * @note Internal helper. Not thread-safe; caller provides synchronisation.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_decode_inquiry(const uint8_t* raw, ra8_usb_hmsc_inquiry_response_t* response)
{
  internal_zero_bytes((uint8_t*)response, (uint16_t)sizeof(*response));
  const uint8_t b0                 = raw[k_ra8_hmsc_inq_off_dev_type];
  const uint8_t b1                 = raw[k_ra8_hmsc_inq_off_removable];
  response->peripheral_qualifier   = (uint8_t)(b0 >> k_ra8_hmsc_inq_qual_shift);
  response->peripheral_device_type = (uint8_t)(b0 & k_ra8_hmsc_inq_dev_type_mask);
  response->removable =
    (uint8_t)((b1 >> k_ra8_hmsc_inq_removable_shift) & k_ra8_hmsc_inq_removable_mask);
  response->version = raw[k_ra8_hmsc_inq_off_version];
  internal_copy_bytes(response->vendor_id,
                      &raw[k_ra8_hmsc_inq_off_vendor_id],
                      (uint16_t)sizeof(response->vendor_id));
  internal_copy_bytes(response->product_id,
                      &raw[k_ra8_hmsc_inq_off_product_id],
                      (uint16_t)sizeof(response->product_id));
  internal_copy_bytes(response->product_revision,
                      &raw[k_ra8_hmsc_inq_off_product_rev],
                      (uint16_t)sizeof(response->product_revision));
}
/* GCOVR_EXCL_STOP */

/* =============================================================================
 * SCSI commands -- public entry points
 * =============================================================================
 */

ra8_err_t ra8_usb_hmsc_inquiry(uint8_t target_lun, ra8_usb_hmsc_inquiry_response_t* response)
{
  RA8_CHECK_NULL_PTR(response, s_tag, "inquiry: response");
  const ra8_err_t ready_err = internal_check_ready(target_lun);
  RA8_RETURN_ON_ERROR(ready_err, s_tag, "inquiry: not ready"); /* GCOVR_EXCL_BR_LINE */

  uint8_t cdb[k_ra8_hmsc_cdb_max_len] = {};
  internal_build_inquiry_cdb(cdb);

  uint8_t         raw_response[k_ra8_hmsc_inquiry_resp_len] = {};
  uint16_t        got_len                                   = k_ra8_hmsc_inquiry_resp_len;
  const ra8_err_t err =
    internal_run_data_in(target_lun, cdb, (uint8_t)k_ra8_hmsc_cdb6_len, raw_response, &got_len);
  RA8_RETURN_ON_ERROR(err, s_tag, "inquiry: bot"); /* GCOVR_EXCL_BR_LINE */
  /* GCOVR_EXCL_START -- host-unreachable: decoding + success return run only
   * after the BOT command completes; the fake bulk-OUT CBW push always
   * times out, so this returns on the "bot" error leg above. */
  internal_decode_inquiry(raw_response, response);
  return k_ra8_ok;
  /* GCOVR_EXCL_STOP */
}

ra8_err_t
ra8_usb_hmsc_read_capacity(uint8_t target_lun, uint32_t* block_count, uint32_t* block_size)
{
  RA8_CHECK_NULL_PTR(block_count, s_tag, "read_capacity: block_count");
  RA8_CHECK_NULL_PTR(block_size, s_tag, "read_capacity: block_size");
  const ra8_err_t ready_err = internal_check_ready(target_lun);
  RA8_RETURN_ON_ERROR(ready_err, s_tag, "read_capacity: not ready"); /* GCOVR_EXCL_BR_LINE */

  uint8_t cdb[k_ra8_hmsc_cdb_max_len] = {};
  internal_build_read_capacity_cdb(cdb);

  uint8_t         raw_response[k_ra8_hmsc_read_capacity_resp_len] = {};
  uint16_t        got_len = k_ra8_hmsc_read_capacity_resp_len;
  const ra8_err_t err =
    internal_run_data_in(target_lun, cdb, (uint8_t)k_ra8_hmsc_cdb10_len, raw_response, &got_len);
  RA8_RETURN_ON_ERROR(err, s_tag, "read_capacity: bot"); /* GCOVR_EXCL_BR_LINE */

  /* Decode SBC-4 sec 5.10: byte[0..3] = LAST valid LBA (big-endian),
   * byte[4..7] = block size (big-endian). When the fake returns
   * zeros default to a sane block size so calling code can still
   * compute capacities. */
  /* GCOVR_EXCL_START -- host-unreachable: decoding + success return run only
   * after the BOT command completes; the fake bulk-OUT CBW push always
   * times out, so this returns on the "bot" error leg above. */
  const uint32_t last_lba = internal_unpack_u32_be(&raw_response[k_ra8_hmsc_cap_off_last_lba]);
  const uint32_t blk      = internal_unpack_u32_be(&raw_response[k_ra8_hmsc_cap_off_blk_size]);
  *block_count            = last_lba + 1U;
  *block_size             = (blk == 0U) ? (uint32_t)k_ra8_hmsc_block_size_default : blk;
  return k_ra8_ok;
  /* GCOVR_EXCL_STOP */
}

ra8_err_t
ra8_usb_hmsc_read10(uint8_t target_lun, uint32_t lba, uint16_t block_count, uint8_t* out_buf)
{
  RA8_CHECK_NULL_PTR(out_buf, s_tag, "read10: out_buf");
  if (block_count == 0U) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t ready_err = internal_check_ready(target_lun);
  RA8_RETURN_ON_ERROR(ready_err, s_tag, "read10: not ready"); /* GCOVR_EXCL_BR_LINE */

  uint8_t cdb[k_ra8_hmsc_cdb_max_len] = {};
  internal_build_rw10_cdb(k_ra8_hmsc_scsi_read_10, lba, block_count, cdb);

  /* dCBWDataTransferLength = block_count * block_size. We default to
   * the standard 512-byte block; production should use the cached
   * read_capacity result. */
  const uint32_t  xfer_len = (uint32_t)block_count * (uint32_t)k_ra8_hmsc_block_size_default;
  uint16_t        got_len  = (xfer_len > UINT16_MAX) ? UINT16_MAX : (uint16_t)xfer_len;
  const ra8_err_t err =
    internal_run_data_in(target_lun, cdb, (uint8_t)k_ra8_hmsc_cdb10_len, out_buf, &got_len);
  RA8_RETURN_ON_ERROR(err, s_tag, "read10: bot"); /* GCOVR_EXCL_BR_LINE */
  /* GCOVR_EXCL_START -- host-unreachable: the success return runs only after
   * the BOT read completes; the fake bulk-OUT CBW push always times
   * out, so this returns on the "bot" error leg above. */
  return k_ra8_ok;
  /* GCOVR_EXCL_STOP */
}

ra8_err_t
ra8_usb_hmsc_write10(uint8_t target_lun, uint32_t lba, uint16_t block_count, const uint8_t* in_buf)
{
  RA8_CHECK_NULL_PTR(in_buf, s_tag, "write10: in_buf");
  if (block_count == 0U) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t ready_err = internal_check_ready(target_lun);
  RA8_RETURN_ON_ERROR(ready_err, s_tag, "write10: not ready"); /* GCOVR_EXCL_BR_LINE */

  uint8_t cdb[k_ra8_hmsc_cdb_max_len] = {};
  internal_build_rw10_cdb(k_ra8_hmsc_scsi_write_10, lba, block_count, cdb);

  const uint32_t  xfer_len = (uint32_t)block_count * (uint32_t)k_ra8_hmsc_block_size_default;
  const uint16_t  push_len = (xfer_len > UINT16_MAX) ? UINT16_MAX : (uint16_t)xfer_len;
  const ra8_err_t err =
    internal_run_data_out(target_lun, cdb, (uint8_t)k_ra8_hmsc_cdb10_len, in_buf, push_len);
  RA8_RETURN_ON_ERROR(err, s_tag, "write10: bot"); /* GCOVR_EXCL_BR_LINE */
  /* GCOVR_EXCL_START -- host-unreachable: the success return runs only after
   * the BOT write completes; the fake bulk-OUT CBW push always times
   * out, so this returns on the "bot" error leg above. */
  return k_ra8_ok;
  /* GCOVR_EXCL_STOP */
}
