/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/netxduo/inc/nx_ether_driver_ra8_eth.h
 * @brief NetX Duo network driver shim that bridges onto the RA8 ``ra8_eth`` HAL
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Declares the single NetX Duo network-driver entry point
 * ``nx_ether_driver_ra8_eth`` that ``nx_ip_create`` calls back into.
 * The function dispatches on ``nx_ip_driver->nx_ip_driver_command``
 * and translates each request into the matching ``ra8_eth_*`` polled
 * frame primitive exported from ``libs/ra8_hal/inc/ra8_eth.h``.
 *
 * Request mapping (see NetX Duo `nx_api.h` constants):
 *
 * | NetX request                       | Action                         |
 * |------------------------------------|--------------------------------|
 * | ``NX_LINK_INITIALIZE``             | ``ra8_eth_open(cfg)``           |
 * | ``NX_LINK_ENABLE``                 | mark interface up              |
 * | ``NX_LINK_DISABLE``                | mark interface down            |
 * | ``NX_LINK_PACKET_SEND``            | ``ra8_eth_write``               |
 * | ``NX_LINK_PACKET_BROADCAST``       | ``ra8_eth_write``               |
 * | ``NX_LINK_ARP_SEND``               | ``ra8_eth_write``               |
 * | ``NX_LINK_ARP_RESPONSE_SEND``      | ``ra8_eth_write``               |
 * | ``NX_LINK_RARP_SEND``              | ``ra8_eth_write``               |
 * | ``NX_LINK_DEFERRED_PROCESSING``    | drain ``ra8_eth_read`` -> stack |
 * | ``NX_LINK_GET_STATUS``             | ``ra8_eth_link_status``         |
 * | ``NX_LINK_UNINITIALIZE``           | ``ra8_eth_close``               |
 *
 * Every other command returns ``NX_NOT_SUCCESSFUL`` -- the firmware
 * does not support multicast filtering, IP-level offload, or 6LowPAN.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* nx_api.h drags in the NX_IP_DRIVER struct definition the function
 * signature below references. Including it here keeps consumers from
 * having to remember the include order. */
#include <stdint.h>

#include "nx_api.h"

/**
 * @brief NetX Duo network-driver entry point bridging onto ``ra8_eth_*``.
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
 * 2. Call ``ra8_eth_write`` with the staging buffer.
 * 3. Always release the packet via ``nx_packet_transmit_release``
 *    so the NetX pool does not leak.
 *
 * Frame RX path (``NX_LINK_DEFERRED_PROCESSING``):
 * 1. Loop calling ``ra8_eth_read`` until it returns
 *    ``k_ra8_err_no_data``.
 * 2. For each frame, allocate an NX_PACKET from the IP's default
 *    packet pool, copy the bytes in, and pass it to
 *    ``_nx_ip_packet_receive`` (NetX consumes the packet).
 *
 * @param[in,out] driver_req NetX Duo driver request structure. The
 *   driver reads ``nx_ip_driver_command``, ``nx_ip_driver_packet``,
 *   ``nx_ip_driver_interface``, and writes ``nx_ip_driver_status``.
 *
 * @pre ``driver_req != NULL``.
 * @pre PHY pins routed and ``ra8_eth_init`` reachable.
 *
 * @post On success ``driver_req->nx_ip_driver_status == NX_SUCCESS``.
 * @post On failure ``driver_req->nx_ip_driver_status == NX_NOT_SUCCESSFUL``.
 *
 * @note Polled implementation. Must run from a ThreadX context (the
 *       NetX Duo IP thread). Not safe to call from an ISR.
 *
 * @since 0.1.0
 */
void nx_ether_driver_ra8_eth(NX_IP_DRIVER* driver_req);

/**
 * @brief Install the local MAC address used by the driver at NX_LINK_INITIALIZE.
 *
 * @details
 * `nx_ip_create` calls the driver's INITIALIZE callback synchronously
 * during IP-thread spin-up, BEFORE the app can call
 * `nx_ip_interface_physical_address_set`. The interface's
 * physical_address_msw / _lsw fields read zero at that point, so the
 * driver has no MAC to program into the RMAC's MRMAC0/MRMAC1
 * registers and RX silently filters every frame. Apps must call this
 * function BEFORE nx_ip_create so the driver has the MAC at hand
 * when INITIALIZE fires.
 *
 * @param[in] mac 6-byte unicast MAC in network byte order.
 *
 * @pre `mac != nullptr`.
 * @pre Called from a single-threaded init context (before nx_ip_create).
 * @post The driver's static cache holds `mac` and will use it during
 *       its next INITIALIZE callback.
 * @post Subsequent INITIALIZE callbacks ignore the (zero) interface
 *       physical_address fields and program the RMAC from the cache.
 *
 * @note Not thread-safe; call once during app setup.
 * @since 0.1.0
 */
void nx_ether_driver_ra8_eth_set_mac(const uint8_t mac[6]);

#ifdef __cplusplus
}
#endif
