/**
 * @file ra_io_stream_usbcdc.h
 * @brief ra_io byte-stream sink over a USB-CDC endpoint.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Streams bytes to the host over a USB-CDC bulk-IN endpoint via `ra_usb_pal` --
 * the "print to the host over USB serial" target. The application must have
 * brought the USB device stack and the CDC class up first. The data path is
 * exercised on board_sim / HIL; on the host this sink binds and validates only.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"
#include "ra_io_stream.h"

/**
 * @struct ra_io_stream_usbcdc_state_t
 * @brief Caller-owned private state for a USB-CDC stream sink.
 *
 * @details Zero-initialise and pass to ::ra_io_stream_usbcdc_init. Treat the
 *          field as private.
 *
 * @invariant `ep_addr` is a configured bulk-IN endpoint address.
 *
 * @since 0.1.0
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t ep_addr; /**< USB bulk-IN endpoint address (private). */
} ra_io_stream_usbcdc_state_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Bind a USB-CDC stream sink into a caller-owned stream handle.
 *
 * @param[out] s       Stream handle to bind (zero-initialised by the caller).
 * @param[out] state   Caller-owned sink state to populate.
 * @param[in]  ep_addr USB bulk-IN endpoint address to write to.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           Sink bound; `s` is usable.
 * @retval k_ra_err_null_ptr `s` or `state` was NULL.
 *
 * @pre The USB device stack + CDC class are up and `ep_addr` is configured.
 * @pre `s` and `state` out-live every call made through the stream.
 * @post On success `s` sends bytes through `ep_addr`.
 * @post On any non-ok return `s` and `state` are left unbound/untouched.
 *
 * @note Not thread-safe with respect to the same stream.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_io_stream_usbcdc_init(ra_io_stream_t* s, ra_io_stream_usbcdc_state_t* state, uint8_t ep_addr);

#ifdef __cplusplus
}
#endif
