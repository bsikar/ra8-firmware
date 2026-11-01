/**
 * @file ra8_io_netif_internal.h
 * @brief Internal vtable type for the ra8_io netif facade.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Not part of the public API. Only the netif dispatcher and the backend
 * translation units include this file. Tests under `tests/` MAY include it
 * to drive MC/DC vectors against internal helpers; application code never
 * should.
 *
 * Defines ::ra8_io_netif_iface -- the function-pointer table each backend
 * fills in and binds into a caller-owned ::ra8_io_netif_t. It is forward
 * declared opaque in `ra8_io_netif.h`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_io_netif.h"

/**
 * @struct ra8_io_netif_iface
 * @brief One vtable per L2 transport. Each callback receives the backend's
 *        opaque `ctx` (the bound transport) so backends keep no static
 *        state.
 *
 * @details
 * Both rows are mandatory: both transports implement the full frame-mover
 * surface, so the dispatcher never NULL-checks a row beyond the binding
 * check on the handle itself.
 *
 * @invariant `send` and `poll` are non-NULL.
 *
 * @since 0.1.0
 */
struct ra8_io_netif_iface {
  /** @brief Hand one frame to the transport for transmit (its own verdict
   *         passes through). */
  ra8_err_t (*send)(void* ctx, const uint8_t* frame, uint16_t len);

  /** @brief Pump the transport and copy at most one received frame out;
   *         ::k_ra8_err_no_data when nothing is staged. */
  ra8_err_t (*poll)(void* ctx, uint8_t* out_buf, uint16_t* inout_len);
};

#ifdef __cplusplus
}
#endif
