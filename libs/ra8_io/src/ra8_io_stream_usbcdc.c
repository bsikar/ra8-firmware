/**
 * @file ra8_io_stream_usbcdc.c
 * @brief USB-CDC byte-stream sink -- bulk-IN writes through ra8_usb_pal.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Binds ::ra8_io_stream_iface to `ra8_usb_pal_ep_send`. The PAL takes a 16-bit
 * length, so a larger write is split into 65535-byte chunks. Touches no raw
 * MMIO -- the PAL owns the USB controller access.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_io_stream_usbcdc.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_io_stream_internal.h"
#include "ra8_usb_pal.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_stream_usbcdc";

/**
 * @enum ra8_io_usbcdc_const_t
 * @brief USB-CDC sink constants.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_usbcdc_max_chunk = 65535, /**< Largest single ra8_usb_pal_ep_send length. */
} ra8_io_usbcdc_const_t;

/**
 * @brief USB-CDC sink: send all bytes through the bulk-IN endpoint.
 *
 * @details
 * Splits the request into at most 65535-byte PAL transfers and stops at the
 * first non-ok status, reporting how many bytes were sent.
 *
 * @param[in]  ctx         USB-CDC sink state (as a void cookie).
 * @param[in]  buf         Source bytes.
 * @param[in]  len         Number of bytes to send.
 * @param[out] out_written Sent byte count, or NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           All bytes sent.
 * @retval k_ra8_err_null_ptr `ctx` or `buf` was NULL.
 * @retval k_ra8_err_*        Propagated from `ra8_usb_pal_ep_send`.
 *
 * @pre `ctx` is a populated USB-CDC sink state.
 * @pre The USB device + CDC class are up.
 * @post On success all `len` bytes were queued to the endpoint.
 * @post `*out_written` (when provided) holds the sent count.
 *
 * @note Not thread-safe with respect to the same endpoint.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t usbcdc_write(void* ctx, const uint8_t* buf, uint32_t len, uint32_t* out_written)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  const ra8_io_stream_usbcdc_state_t* st   = (const ra8_io_stream_usbcdc_state_t*)ctx;
  uint32_t                            done = 0;
  while (done < len) {
    const uint32_t rem = len - done;
    const uint32_t chunk =
      (rem > (uint32_t)k_usbcdc_max_chunk) ? (uint32_t)k_usbcdc_max_chunk : rem;
    const ra8_err_t e = ra8_usb_pal_ep_send(st->ep_addr, &buf[done], (uint16_t)chunk);
    if (e != k_ra8_ok) {
      if (out_written != nullptr) {
        *out_written = done;
      }
      return e;
    }
    done += chunk;
  }
  if (out_written != nullptr) {
    *out_written = len;
  }
  return k_ra8_ok;
}

/** @brief USB-CDC stream sink vtable. `flush` is NULL: the PAL queues directly. */
static const ra8_io_stream_iface_t k_usbcdc_iface = {
  .write = usbcdc_write,
  .flush = nullptr,
};

ra8_err_t
ra8_io_stream_usbcdc_init(ra8_io_stream_t* s, ra8_io_stream_usbcdc_state_t* state, uint8_t ep_addr)
{
  RA8_CHECK_NULL_PTR(s, s_tag, "s must not be nullptr");
  RA8_CHECK_NULL_PTR(state, s_tag, "state must not be nullptr");
  state->ep_addr = ep_addr;
  s->iface       = &k_usbcdc_iface;
  s->ctx         = state;
  return k_ra8_ok;
}
