/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_usb_cdc.h
 * @brief Native USB CDC ACM (Communications Device Class - Abstract
 * @ingroup grp_hal_usb
 *        Control Model) class layer
 *
 * @details
 * Glues the device-mode `ra8_usb` controller driver to a USB CDC ACM
 * function so the host enumerates the board as a serial port
 * (`/dev/ttyACM0` on Linux / macOS; `COMn` on Windows). The
 * implementation is from-scratch -- no FSP `r_usb_pcdc.c`, no
 * CherryUSB `cdc_acm.c`, no TinyUSB. It mirrors the surface FSP's
 * peripheral CDC class exposes: line-coding storage, DTR / RTS
 * tracking, and a bulk IN / bulk OUT byte pipe.
 *
 * The descriptor table built by `ra8_usb_cdc_init` advertises:
 *
 *  - One configuration with two interfaces (CDC control + data).
 *  - EP0 64-byte default control pipe.
 *  - EP1 IN, bulk, 64 bytes (data IN).
 *  - EP2 OUT, bulk, 64 bytes (data OUT).
 *  - EP3 IN, interrupt, 8 bytes (notification, unused but required).
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
 * @enum ra8_usb_cdc_pipe_t
 * @brief PIPE numbers used by the CDC ACM function.
 *
 * @details FSP / RA8D2 PIPE assignment rules constrain bulk pipes to
 * PIPE1..PIPE5 and interrupt pipes to PIPE6..PIPE9. We keep the
 * mapping deterministic to make wireshark / Linux dmesg traces
 * easier to follow.
 */
typedef enum : uint8_t {
  k_ra8_cdc_pipe_bulk_in  = 1U, /**< PIPE1 -> EP1 IN  (bulk).      */
  k_ra8_cdc_pipe_bulk_out = 2U, /**< PIPE2 -> EP2 OUT (bulk).      */
  k_ra8_cdc_pipe_intr_in  = 6U, /**< PIPE6 -> EP3 IN  (interrupt). */
} ra8_usb_cdc_pipe_t;

/**
 * @enum ra8_usb_cdc_ep_t
 * @brief USB endpoint addresses used by the CDC ACM function.
 */
typedef enum : uint8_t {
  k_ra8_cdc_ep_bulk_in_addr  = 1U, /**< EP1 IN address.  */
  k_ra8_cdc_ep_bulk_out_addr = 2U, /**< EP2 OUT address. */
  k_ra8_cdc_ep_intr_in_addr  = 3U, /**< EP3 IN address.  */
} ra8_usb_cdc_ep_t;

/**
 * @enum ra8_usb_cdc_packet_t
 * @brief Packet sizing for the CDC ACM bulk + interrupt pipes.
 */
typedef enum : uint16_t {
  k_ra8_cdc_bulk_max_packet_fs = 64U,  /**< Bulk size at full speed. */
  k_ra8_cdc_bulk_max_packet_hs = 512U, /**< Bulk size at high speed. */
  k_ra8_cdc_intr_max_packet    = 8U,   /**< Notification pipe size.  */
} ra8_usb_cdc_packet_t;

/**
 * @enum ra8_usb_cdc_request_t
 * @brief CDC class-specific request codes carried in `bRequest`.
 *
 * @details Numbered from the USB CDC PSTN subclass spec rev 1.20.
 */
typedef enum : uint8_t {
  k_ra8_cdc_req_set_line_coding        = 0x20U, /**< 7-byte payload. */
  k_ra8_cdc_req_get_line_coding        = 0x21U, /**< 7-byte payload. */
  k_ra8_cdc_req_set_control_line_state = 0x22U, /**< 0-byte payload. */
} ra8_usb_cdc_request_t;

/**
 * @enum ra8_usb_cdc_line_state_t
 * @brief Bits inside `wValue` for SET_CONTROL_LINE_STATE.
 */
typedef enum : uint16_t {
  k_ra8_cdc_line_state_dtr = 0x0001U, /**< Data terminal ready. */
  k_ra8_cdc_line_state_rts = 0x0002U, /**< Request to send.     */
} ra8_usb_cdc_line_state_t;

/**
 * @struct ra8_usb_cdc_line_coding_t
 * @brief 7-byte payload of SET_LINE_CODING / GET_LINE_CODING.
 *
 * @details Layout matches the on-the-wire serialisation; the host
 * sends little-endian.
 */
typedef struct {
  uint32_t dte_rate;    /**< Baud rate in bits/sec.          */
  uint8_t  char_format; /**< 0=1 stop, 1=1.5 stop, 2=2 stop. */
  uint8_t  parity_type; /**< 0=none, 1=odd, 2=even, ...      */
  uint8_t  data_bits;   /**< 5 / 6 / 7 / 8 / 16.             */
} ra8_usb_cdc_line_coding_t;

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Bring up the CDC ACM function on a chosen USB controller.
 *
 * @details
 * Initialises the underlying `ra8_usb` driver for `speed`, configures
 * PIPE1 (bulk IN), PIPE2 (bulk OUT), PIPE6 (interrupt IN), seeds a
 * default 9600/8/N/1 line coding, and stores DTR/RTS = 0. The D+
 * pull-up stays down -- the caller raises it via `ra8_usb_cdc_attach`
 * once it is ready to enumerate.
 *
 * @param[in] speed Which USB controller (FS or HS).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok CDC function ready.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 * @retval k_ra8_err_hw_init_failed Underlying `ra8_usb_device_init`
 *         failed.
 *
 * @pre Single-threaded init context.
 *
 * @post `ra8_usb_cdc_get_line_coding` returns 9600/8/N/1.
 * @post Bulk + interrupt pipes are configured but not attached.
 *
 * @note Not thread-safe.
 * @see ra8_usb_cdc_attach
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_cdc_init(ra8_usb_speed_t speed);

/**
 * @brief Tear down the CDC function and release the underlying
 *        controller.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Released.
 * @retval k_ra8_err_invalid_state CDC was never initialized.
 *
 * @pre Single-threaded shutdown context.
 *
 * @post Subsequent CDC API calls return `k_ra8_err_invalid_state`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_cdc_deinit(void);

/**
 * @brief Raise / drop the D+ pull-up to advertise the CDC function.
 *
 * @param[in] attached `true` to attach, `false` to detach.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok State updated.
 * @retval k_ra8_err_invalid_state CDC not initialized.
 *
 * @pre `ra8_usb_cdc_init` succeeded.
 *
 * @post On success, the controller's `SYSCFG.DPRPU` matches
 * `attached`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_cdc_attach(bool attached);

/* =============================================================================
 * Bulk byte pipe
 * =============================================================================
 */

/**
 * @brief Send a chunk of bytes IN (device -> host).
 *
 * @param[in] data Buffer to send.
 * @param[in] len Byte count, 0..bulk_max_packet of the speed.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Transfer queued.
 * @retval k_ra8_err_invalid_state CDC not initialized.
 * @retval k_ra8_err_invalid_arg Bad `data` / `len`.
 *
 * @pre `ra8_usb_cdc_init` succeeded.
 *
 * @post Bytes are visible on the host's TTY after the next IN token.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_cdc_send(const uint8_t* data, uint16_t len);

/**
 * @brief Drain a chunk of bytes OUT (host -> device).
 *
 * @param[out] out_buf Destination buffer.
 * @param[in,out] inout_len On entry: capacity. On exit: bytes received.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Bytes received.
 * @retval k_ra8_err_no_data Pipe was empty.
 * @retval k_ra8_err_invalid_state CDC not initialized.
 * @retval k_ra8_err_invalid_arg Bad pointers / capacity.
 *
 * @pre `out_buf`, `inout_len` non-NULL, `*inout_len > 0`.
 *
 * @post On success `*inout_len` reflects the actual byte count.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_cdc_recv(uint8_t* out_buf, uint16_t* inout_len);

/* =============================================================================
 * SETUP request handler
 * =============================================================================
 */

/**
 * @brief Process a class-specific SETUP packet.
 *
 * @details
 * Switches on `b_request` and either updates the stored line coding,
 * returns the line coding payload, or stores the DTR / RTS bits.
 * Standard (non-class) SETUPs are rejected with
 * `k_ra8_err_not_supported` so the caller can fall back to its own
 * standard-request handler.
 *
 * @param[in] setup The SETUP packet returned by `ra8_usb_read_setup_if_valid`.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok SETUP handled (status stage queued internally).
 * @retval k_ra8_err_invalid_state CDC not initialized.
 * @retval k_ra8_err_invalid_arg `setup` was NULL.
 * @retval k_ra8_err_not_supported `bRequest` is not a CDC ACM request
 *         this layer cares about.
 *
 * @pre `ra8_usb_cdc_init` succeeded.
 *
 * @post Stored line coding / line state updated.
 *
 * @note Call from the `CTRT` ISR path of `ra8_usb`.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_cdc_handle_setup(const ra8_usb_setup_t* setup);

/**
 * @brief Get the most recently negotiated line coding.
 *
 * @param[out] out Receives the 7-byte coding payload.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Copied.
 * @retval k_ra8_err_invalid_arg `out` was NULL.
 * @retval k_ra8_err_invalid_state CDC not initialized.
 *
 * @pre `out` non-NULL.
 *
 * @post No internal state mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_cdc_get_line_coding(ra8_usb_cdc_line_coding_t* out);

/**
 * @brief Get the most recently received DTR / RTS bits.
 *
 * @param[out] out_dtr `true` if DTR asserted.
 * @param[out] out_rts `true` if RTS asserted.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok State copied.
 * @retval k_ra8_err_invalid_arg `out_dtr` / `out_rts` were NULL.
 * @retval k_ra8_err_invalid_state CDC not initialized.
 *
 * @pre Output pointers non-NULL.
 *
 * @post No internal state mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_cdc_get_line_state(bool* out_dtr, bool* out_rts);

#ifdef __cplusplus
}
#endif
