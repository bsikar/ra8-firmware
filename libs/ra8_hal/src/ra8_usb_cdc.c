/**
 * @file ra8_usb_cdc.c
 * @brief Native USB CDC ACM class layer implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Glues the device-mode `ra8_usb` driver to a CDC ACM endpoint set so
 * the host enumerates the board as `/dev/ttyACM*` (Linux / macOS) or
 * `COMn` (Windows). Class-specific SETUP requests
 * (SET_LINE_CODING / GET_LINE_CODING / SET_CONTROL_LINE_STATE) are
 * answered locally; the CDC standard descriptors live in the
 * caller's stack so this file does not own VID / PID.
 *
 * The implementation is from-scratch -- there is no FSP `r_usb_pcdc`
 * source pulled in. Behaviourally it tracks the small subset that
 * the CDC PSTN spec rev 1.20 makes mandatory for an ACM device:
 *
 *  - Persistent line-coding shadow (default 9600/8/N/1).
 *  - DTR / RTS shadow.
 *  - Bulk byte pipe via `ra8_usb_queue_in` / `ra8_usb_queue_out`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_usb_cdc.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_usb.h"
#include "ra8_usb_cdc_internal.h"

static const char* s_tag = "USBCDC";

/**
 * @enum ra8_usb_cdc_setup_field_t
 * @brief Constants used to decode CDC class-specific SETUPs.
 */
typedef enum : uint8_t {
  k_ra8_cdc_bm_class_recip_iface = 0x21U, /**< Class | Interface | Out. */
  k_ra8_cdc_bm_class_recip_in    = 0xA1U, /**< Class | Interface | In.  */
  k_ra8_cdc_default_data_bits    = 8U,    /**< 8 data bits.             */
  k_ra8_cdc_default_stop_bits    = 0U,    /**< 1 stop bit.              */
  k_ra8_cdc_default_parity_none  = 0U,    /**< No parity.               */
  k_ra8_cdc_line_coding_len      = 7U,    /**< 7-byte payload length.   */
} ra8_usb_cdc_setup_field_t;

/**
 * @enum ra8_usb_cdc_data_stage_t
 * @brief Bound on the polled control-OUT data-stage drain.
 * @details This layer is polled throughout (::ra8_usb_cdc_recv drives the bulk
 *          pipe the same way), so the SET_LINE_CODING data stage is drained by
 *          a bounded poll rather than an ISR callback. The bound exists so the
 *          SETUP path can never spin: the RA8 SIE ACKs the host's OUT packet
 *          into the DCP bank as soon as ::ra8_usb_dcp_out_arm sets PID = BUF,
 *          so the packet lands within a few bus microframes or the host has
 *          abandoned the transfer. Exhausting the bound is not an error path
 *          for the caller -- the request is still ACKed, exactly as it was
 *          before the data stage was captured at all.
 * @invariant The loop in ::internal_pull_data_stage runs at most
 *            `k_ra8_cdc_data_stage_polls` times (NASA P10 Rule 2).
 * @see internal_pull_data_stage()
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_cdc_data_stage_polls = 1000U, /**< Max ::ra8_usb_dcp_out_read attempts. */
} ra8_usb_cdc_data_stage_t;

/**
 * @enum ra8_usb_cdc_default_baud_t
 * @brief 9600 baud assembled from the two low bytes above.
 */
typedef enum : uint32_t {
  k_ra8_cdc_default_baud = 9600U, /**< RA8 cdc default baud. */
} ra8_usb_cdc_default_baud_t;

/**
 * @enum ra8_usb_cdc_byte_shift_t
 * @brief Per-byte left-shift constants for the little-endian baud
 * accumulator in `internal_apply_line_coding`.
 */
typedef enum : uint8_t {
  k_ra8_cdc_shift_byte0 = 0U,  /**< RA8 cdc shift byte0. */
  k_ra8_cdc_shift_byte1 = 8U,  /**< RA8 cdc shift byte1. */
  k_ra8_cdc_shift_byte2 = 16U, /**< RA8 cdc shift byte2. */
  k_ra8_cdc_shift_byte3 = 24U, /**< RA8 cdc shift byte3. */
} ra8_usb_cdc_byte_shift_t;

/**
 * @enum ra8_usb_cdc_byte_idx_t
 * @brief Indices into the 7-byte SET_LINE_CODING payload.
 */
typedef enum : uint8_t {
  k_ra8_cdc_idx_baud_b0     = 0U, /**< RA8 cdc index baud b0.     */
  k_ra8_cdc_idx_baud_b1     = 1U, /**< RA8 cdc index baud b1.     */
  k_ra8_cdc_idx_baud_b2     = 2U, /**< RA8 cdc index baud b2.     */
  k_ra8_cdc_idx_baud_b3     = 3U, /**< RA8 cdc index baud b3.     */
  k_ra8_cdc_idx_char_format = 4U, /**< RA8 cdc index char format. */
  k_ra8_cdc_idx_parity_type = 5U, /**< RA8 cdc index parity type. */
  k_ra8_cdc_idx_data_bits   = 6U, /**< RA8 cdc index data bits.   */
} ra8_usb_cdc_byte_idx_t;

/**
 * @struct ra8_usb_cdc_state_t
 * @brief Singleton shadow state for the CDC function.
 */
typedef struct {
  bool                      initialized; /**< True after `ra8_usb_cdc_init`. */
  ra8_usb_speed_t           speed;       /**< Underlying controller.         */
  ra8_usb_cdc_line_coding_t coding;      /**< Most recent line coding.       */
  bool                      dtr;         /**< DTR shadow.                    */
  bool                      rts;         /**< RTS shadow.                    */
} ra8_usb_cdc_state_t;

static ra8_usb_cdc_state_t s_state = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Internal helper.
 * @details See implementation.
 * @param[in] coding See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_default_coding(ra8_usb_cdc_line_coding_t* coding)
{
  coding->dte_rate    = k_ra8_cdc_default_baud;
  coding->char_format = k_ra8_cdc_default_stop_bits;
  coding->parity_type = k_ra8_cdc_default_parity_none;
  coding->data_bits   = k_ra8_cdc_default_data_bits;
}

/**
 * @brief Configure the three CDC pipes (bulk IN / OUT, intr IN).
 *
 * @details See implementation.
 * @param[in] speed See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_configure_pipes(ra8_usb_speed_t speed)
{
  const uint16_t bulk_mp =
    (speed == k_ra8_usb_speed_hs) ? k_ra8_cdc_bulk_max_packet_hs : k_ra8_cdc_bulk_max_packet_fs;

  ra8_err_t err = ra8_usb_configure_endpoint(speed,
                                             k_ra8_cdc_pipe_bulk_in,
                                             k_ra8_cdc_ep_bulk_in_addr,
                                             k_ra8_usb_ep_dir_in,
                                             k_ra8_usb_ep_type_bulk,
                                             bulk_mp);
  RA8_RETURN_ON_ERROR(err, /* GCOVR_EXCL_BR_LINE -- valid speed + static valid bulk-IN tuple */
                      s_tag,
                      "cdc: bulk-in cfg");

  err = ra8_usb_configure_endpoint(speed,
                                   k_ra8_cdc_pipe_bulk_out,
                                   k_ra8_cdc_ep_bulk_out_addr,
                                   k_ra8_usb_ep_dir_out,
                                   k_ra8_usb_ep_type_bulk,
                                   bulk_mp);
  RA8_RETURN_ON_ERROR(err, /* GCOVR_EXCL_BR_LINE -- valid speed + static valid bulk-OUT tuple */
                      s_tag,
                      "cdc: bulk-out cfg");

  err = ra8_usb_configure_endpoint(speed,
                                   k_ra8_cdc_pipe_intr_in,
                                   k_ra8_cdc_ep_intr_in_addr,
                                   k_ra8_usb_ep_dir_in,
                                   k_ra8_usb_ep_type_intr,
                                   k_ra8_cdc_intr_max_packet);
  return err;
}

/**
 * @brief Apply a host SET_LINE_CODING payload to the cached CDC line coding.
 *
 * @details Decodes the little-endian seven-byte USB CDC PSTN line-coding
 * structure. The 32-bit DTE baud rate is assembled from bytes zero through
 * three, followed by the character-format, parity-type, and data-bit fields.
 * A null pointer or a short payload leaves the cached coding untouched.
 *
 * @param[in] data Pointer to the line-coding payload in host byte order; may
 *                 be null to exercise the reject path.
 * @param[in] len  Length of @p data in bytes.
 *
 * @pre The CDC state has been initialized by `ra8_usb_cdc_init()`.
 * @pre When @p data is non-null, it references at least @p len readable bytes.
 * @post A valid seven-byte payload replaces every cached line-coding field.
 * @post A null or short payload leaves the cached line coding unchanged.
 *
 * @note Not thread-safe; production calls originate in the USB control path.
 *
 * @par MC/DC:
 * Decision `(data == nullptr) || (len < 7)` uses N+1 = 3 vectors:
 * - non-null data, length 7: false control vector
 * - null data, length 7: first condition independently makes the result true
 * - non-null data, length 6: second condition independently makes it true
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_apply_line_coding(const uint8_t* data, uint16_t len)
{
  if ((data == nullptr) || (len < k_ra8_cdc_line_coding_len)) {
    return;
  }
  uint32_t baud = (uint32_t)data[k_ra8_cdc_idx_baud_b0];
  baud |= (uint32_t)((uint32_t)data[k_ra8_cdc_idx_baud_b1] << k_ra8_cdc_shift_byte1);
  baud |= (uint32_t)((uint32_t)data[k_ra8_cdc_idx_baud_b2] << k_ra8_cdc_shift_byte2);
  baud |= (uint32_t)((uint32_t)data[k_ra8_cdc_idx_baud_b3] << k_ra8_cdc_shift_byte3);
  s_state.coding.dte_rate    = baud;
  s_state.coding.char_format = data[k_ra8_cdc_idx_char_format];
  s_state.coding.parity_type = data[k_ra8_cdc_idx_parity_type];
  s_state.coding.data_bits   = data[k_ra8_cdc_idx_data_bits];
}

RA8_TEST_HELPER
void ra8_usb_cdc_test_apply_line_coding(const uint8_t* data, uint16_t len)
{
  internal_apply_line_coding(data, len);
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

ra8_err_t ra8_usb_cdc_init(ra8_usb_speed_t speed)
{
  if ((speed != k_ra8_usb_speed_fs) && (speed != k_ra8_usb_speed_hs)) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t usb_err = ra8_usb_device_init(speed);
  if (usb_err != k_ra8_ok) {
    ra8_log_error_val(s_tag, "ra8_usb_device_init failed", (uint32_t)usb_err);
    return k_ra8_err_hw_init_failed;
  }

  s_state.speed = speed;
  s_state.dtr   = false;
  s_state.rts   = false;
  internal_default_coding(&s_state.coding);

  const ra8_err_t pipes_err = internal_configure_pipes(speed);
  if (pipes_err != k_ra8_ok) {
    (void)ra8_usb_device_deinit(speed);
    return pipes_err;
  }
  s_state.initialized = true;
  ra8_log_info(s_tag, "CDC ready");
  return k_ra8_ok;
}

ra8_err_t ra8_usb_cdc_deinit(void)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  (void)ra8_usb_device_attach(s_state.speed, false);
  const ra8_err_t err = ra8_usb_device_deinit(s_state.speed);
  s_state.initialized = false;
  s_state.dtr         = false;
  s_state.rts         = false;
  return err;
}

ra8_err_t ra8_usb_cdc_attach(bool attached)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  return ra8_usb_device_attach(s_state.speed, attached);
}

/* =============================================================================
 * Bulk byte pipe
 * =============================================================================
 */

ra8_err_t ra8_usb_cdc_send(const uint8_t* data, uint16_t len)
{
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if ((data == nullptr) && (len != 0U)) {
    return k_ra8_err_invalid_arg;
  }
  return ra8_usb_queue_in(s_state.speed, k_ra8_cdc_pipe_bulk_in, data, len);
}

ra8_err_t ra8_usb_cdc_recv(uint8_t* out_buf, uint16_t* inout_len)
{
  RA8_CHECK_NULL_PTR(out_buf, s_tag, "cdc_recv: out_buf");
  RA8_CHECK_NULL_PTR(inout_len, s_tag, "cdc_recv: inout_len");
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if (*inout_len == 0U) {
    return k_ra8_err_invalid_arg;
  }
  /* rearm = true: re-arm PID=BUF so the bulk-OUT pipe keeps ACKing
   * host data between cdc_recv calls (the legacy queue_out behaviour). */
  return ra8_usb_queue_out(s_state.speed, k_ra8_cdc_pipe_bulk_out, out_buf, inout_len, true);
}

/* =============================================================================
 * SETUP request handler
 * =============================================================================
 */

RA8_INTERNAL
RA8_BOUNDED_LOOP(k_ra8_cdc_data_stage_polls)
/**
 * @brief Pull the SET_LINE_CODING data stage off EP0 (DCP) once it lands.
 *
 * @details Drives the two-step control-OUT receive the device driver exposes:
 * ::ra8_usb_dcp_out_arm sets `DCPCTR.PID = BUF` so the SIE ACKs the host's OUT
 * token into the DCP bank, then ::ra8_usb_dcp_out_read drains that bank. The
 * read reports ::k_ra8_err_no_data until the packet actually lands, so this
 * polls it up to ::k_ra8_cdc_data_stage_polls times -- matching the polled
 * style of the rest of this layer (::ra8_usb_cdc_recv drives the bulk-OUT pipe
 * the same way) rather than requiring the caller to own a BRDY interrupt.
 *
 * The bound is fail-soft by design: if the payload never arrives, the caller
 * simply leaves the cached line coding untouched and still ACKs the status
 * stage, which is exactly what the layer did before the data stage was
 * captured. A host that does complete the transfer now has its baud rate,
 * framing and parity recorded and visible through
 * ::ra8_usb_cdc_get_line_coding.
 *
 * @param[out] buf     Destination for the payload; must hold @p cap bytes.
 * @param[in]  cap     Capacity of @p buf in bytes.
 * @param[out] out_len Receives the host's payload length in bytes.
 *
 * @return Result code.
 * @retval k_ra8_ok               A packet was drained; `*out_len` is its length.
 * @retval k_ra8_err_null_ptr     @p buf or @p out_len is NULL.
 * @retval k_ra8_err_invalid_arg  @p cap is zero.
 * @retval k_ra8_err_no_data      The payload did not land within the poll bound.
 * @retval other                  Propagated from the arm / read pair.
 *
 * @pre A control-OUT SETUP with `wLength > 0` was just decoded.
 * @pre `s_state.speed` names the controller that saw the SETUP.
 * @post On k_ra8_ok `*out_len` bytes of payload are resident in @p buf.
 * @post On any error the cached line coding is left untouched.
 *
 * @note Not thread-safe; bounded, so it cannot spin.
 * @see ra8_usb_dcp_out_arm()
 * @see ra8_usb_dcp_out_read()
 * @since 0.1.0
 */
static ra8_err_t internal_pull_data_stage(uint8_t* buf, uint16_t cap, uint16_t* out_len)
{
  RA8_CHECK_NULL_PTR(buf, s_tag, "pull_data_stage: buf");
  RA8_CHECK_NULL_PTR(out_len, s_tag, "pull_data_stage: out_len");
  if (cap == 0U) {
    return k_ra8_err_invalid_arg;
  }
  *out_len              = 0U;
  const ra8_err_t armed = ra8_usb_dcp_out_arm(s_state.speed);
  if (armed != k_ra8_ok) {
    return armed;
  }
  ra8_err_t rc = k_ra8_err_no_data;
  for (uint16_t i = 0U; i < k_ra8_cdc_data_stage_polls; ++i) {
    rc = ra8_usb_dcp_out_read(s_state.speed, buf, cap, out_len);
    if (rc != k_ra8_err_no_data) {
      return rc;
    }
  }
  return rc;
}

/**
 * @brief Decode a CDC class-specific SETUP packet body.
 *
 * @return ra8_ok on success, or an error code that the public
 * `ra8_usb_cdc_handle_setup` propagates verbatim.
 *
 * @details See implementation.
 * @param[in] setup See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_dispatch_class_setup(const ra8_usb_setup_t* setup)
{
  switch (setup->b_request) {
    case k_ra8_cdc_req_set_line_coding: {
      uint8_t         buf[k_ra8_cdc_line_coding_len + 1U] = {};
      uint16_t        plen                                = 0U;
      const ra8_err_t pull = internal_pull_data_stage(buf, (uint16_t)sizeof(buf), &plen);
      if (pull == k_ra8_ok) {
        internal_apply_line_coding(buf, plen);
      }
      /* The status stage is ACKed either way: a host that abandoned the data
       * stage still gets a well-formed control-write completion, and the cached
       * coding simply keeps its previous value. */
      return ra8_usb_control_response(s_state.speed, true);
    }
    case k_ra8_cdc_req_get_line_coding: {
      return ra8_usb_control_response(s_state.speed, true);
    }
    case k_ra8_cdc_req_set_control_line_state: {
      const uint16_t mask = setup->w_value;
      s_state.dtr         = ((mask & k_ra8_cdc_line_state_dtr) != 0U);
      s_state.rts         = ((mask & k_ra8_cdc_line_state_rts) != 0U);
      return ra8_usb_control_response(s_state.speed, true);
    }
    default: {
      return k_ra8_err_not_supported;
    }
  }
}

ra8_err_t ra8_usb_cdc_handle_setup(const ra8_usb_setup_t* setup)
{
  RA8_CHECK_NULL_PTR(setup, s_tag, "handle_setup: setup");
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  if ((setup->bm_request_type != k_ra8_cdc_bm_class_recip_iface) &&
      (setup->bm_request_type != k_ra8_cdc_bm_class_recip_in)) {
    return k_ra8_err_not_supported;
  }
  return internal_dispatch_class_setup(setup);
}

ra8_err_t ra8_usb_cdc_get_line_coding(ra8_usb_cdc_line_coding_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "get_line_coding: out");
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  *out = s_state.coding;
  return k_ra8_ok;
}

ra8_err_t ra8_usb_cdc_get_line_state(bool* out_dtr, bool* out_rts)
{
  RA8_CHECK_NULL_PTR(out_dtr, s_tag, "get_line_state: out_dtr");
  RA8_CHECK_NULL_PTR(out_rts, s_tag, "get_line_state: out_rts");
  if (!s_state.initialized) {
    return k_ra8_err_invalid_state;
  }
  *out_dtr = s_state.dtr;
  *out_rts = s_state.rts;
  return k_ra8_ok;
}
