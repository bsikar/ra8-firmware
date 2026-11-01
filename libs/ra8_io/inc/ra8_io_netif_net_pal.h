/**
 * @file ra8_io_netif_net_pal.h
 * @brief ESWM Ethernet backend binder for the ra8_io netif facade.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Binds a caller-owned ::ra8_io_netif_t to the ESWM Ethernet MAC through
 * the ::ra8_net_pal singleton frame mover, via thin trampolines to
 * ::ra8_net_pal_send_frame / ::ra8_net_pal_recv_frame (unmodified). The PAL
 * must already be initialized -- the facade owns frame movement only,
 * never bring-up. There is exactly one ESWM MAC, so the backend needs no
 * per-handle context: the bind fills the vtable and leaves the backend
 * context NULL.
 *
 * @par Example:
 * @code
 * ra8_io_netif_t netif = {};
 * (void)ra8_io_netif_bind_net_pal(&netif);
 * (void)ra8_io_netif_send(&netif, frame, len);
 * @endcode
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_err.h"
#include "ra8_io_netif.h"

/**
 * @brief Bind the ESWM Ethernet singleton transport into a netif handle.
 *
 * @details
 * Fills `netif` with the ESWM backend vtable whose trampolines forward
 * ::ra8_io_netif_send to ::ra8_net_pal_send_frame and
 * ::ra8_io_netif_poll to ::ra8_net_pal_recv_frame. The backend context is
 * left NULL: the PAL is a singleton and keeps no per-handle state.
 *
 * @param[out] netif Caller-allocated handle; zero-initialised.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Backend bound.
 * @retval k_ra8_err_null_ptr `netif` was NULL.
 *
 * @pre `netif` is zero-initialised (or is being re-bound over an old
 *      backend; the old vtable is simply replaced).
 * @post On success `netif->iface` is non-NULL and every `ra8_io_netif_*`
 *       call routes to the PAL until the next bind.
 *
 * @see ra8_io_netif_bind_c6link  The ESP32-C6 twin.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_netif_bind_net_pal(ra8_io_netif_t* netif);

#ifdef __cplusplus
}
#endif
