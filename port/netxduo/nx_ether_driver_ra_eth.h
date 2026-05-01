/**
 * @file port/netxduo/nx_ether_driver_ra_eth.h
 * @brief NetX Duo network driver shim that bridges onto the RA8D2 ``ra_eth`` HAL
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Declares the single NetX Duo network-driver entry point
 * ``nx_ether_driver_ra_eth`` that ``nx_ip_create`` calls back into.
 * The function dispatches on ``nx_ip_driver->nx_ip_driver_command``
 * and translates each request into the matching ``ra_eth_*`` polled
 * frame primitive exported from ``libs/ra_hal/inc/ra_eth.h``.
 *
 * Request mapping (see NetX Duo `nx_api.h` constants):
 *
 * | NetX request                       | Action                         |
 * |------------------------------------|--------------------------------|
 * | ``NX_LINK_INITIALIZE``             | ``ra_eth_open(cfg)``           |
 * | ``NX_LINK_ENABLE``                 | mark interface up              |
 * | ``NX_LINK_DISABLE``                | mark interface down            |
 * | ``NX_LINK_PACKET_SEND``            | ``ra_eth_write``               |
 * | ``NX_LINK_PACKET_BROADCAST``       | ``ra_eth_write``               |
 * | ``NX_LINK_ARP_SEND``               | ``ra_eth_write``               |
 * | ``NX_LINK_ARP_RESPONSE_SEND``      | ``ra_eth_write``               |
 * | ``NX_LINK_RARP_SEND``              | ``ra_eth_write``               |
 * | ``NX_LINK_DEFERRED_PROCESSING``    | drain ``ra_eth_read`` -> stack |
 * | ``NX_LINK_GET_STATUS``             | ``ra_eth_link_status``         |
 * | ``NX_LINK_UNINITIALIZE``           | ``ra_eth_close``               |
 *
 * Every other command returns ``NX_NOT_SUCCESSFUL`` -- the firmware
 * does not support multicast filtering, IP-level offload, or 6LowPAN.
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* nx_api.h drags in the NX_IP_DRIVER struct definition the function
 * signature below references. Including it here keeps consumers from
 * having to remember the include order. */
#include "nx_api.h"

/**
 * @brief NetX Duo network-driver entry point bridging onto ``ra_eth_*``.
 *
 * @details
 * Pass this function as the ``ip_link_driver`` argument to
 * ``nx_ip_create()``. NetX Duo invokes it once per driver-level
 * request, indicating the request kind in
 * ``nx_ip_driver->nx_ip_driver_command`` and reporting completion
 * via ``nx_ip_driver->nx_ip_driver_status`` (set to ``NX_SUCCESS``
 * on success or ``NX_NOT_SUCCESSFUL`` on any underlying ESWM failure).
 *
 * Frame TX path (``NX_LINK_PACKET_SEND`` and friends):
 * 1. Walk the chained ``NX_PACKET`` (if any) and copy the payload
 *    into a contiguous staging buffer (1514 bytes max).
 * 2. Call ``ra_eth_write`` with the staging buffer.
 * 3. Always release the packet via ``nx_packet_transmit_release``
 *    so the NetX pool does not leak.
 *
 * Frame RX path (``NX_LINK_DEFERRED_PROCESSING``):
 * 1. Loop calling ``ra_eth_read`` until it returns
 *    ``k_ra_err_no_data``.
 * 2. For each frame, allocate an NX_PACKET from the IP's default
 *    packet pool, copy the bytes in, and pass it to
 *    ``_nx_ip_packet_receive`` (NetX consumes the packet).
 *
 * @param[in,out] driver_req NetX Duo driver request structure. The
 *   driver reads ``nx_ip_driver_command``, ``nx_ip_driver_packet``,
 *   ``nx_ip_driver_interface``, and writes ``nx_ip_driver_status``.
 *
 * @pre ``driver_req != NULL``.
 * @pre PHY pins routed and ``ra_eth_init`` reachable.
 *
 * @post On success ``driver_req->nx_ip_driver_status == NX_SUCCESS``.
 * @post On failure ``driver_req->nx_ip_driver_status == NX_NOT_SUCCESSFUL``.
 *
 * @note Polled implementation. Must run from a ThreadX context (the
 *       NetX Duo IP thread). Not safe to call from an ISR.
 *
 * @since 0.1.0
 */
void nx_ether_driver_ra_eth(NX_IP_DRIVER* driver_req);

#ifdef __cplusplus
}
#endif
