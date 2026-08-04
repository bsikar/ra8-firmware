/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/netxduo/inc/nx_ether_driver_c6.h
 * @brief NetX Duo network driver shim that bridges onto the ESP32-C6 link.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The twin of ``nx_ether_driver_ra8_eth`` for the wireless path: where that
 * driver hands NetX-supplied frames to the on-chip Ethernet MAC, this one hands
 * them to the ESP32-C6 over the ``ra8_c6link`` facade -- ``ra8_c6link_eth_send``
 * for transmit and the facade's 802.3 receive callback for receive. Above it
 * NetX Duo neither knows nor cares that the wire is a Wi-Fi station rather than
 * a copper port: the interface is Ethernet II framing either way.
 *
 * @par How the pieces fit
 * The C6 link is a single-threaded, polled SPI transport -- ``ra8_c6link_poll``
 * clocks transactions and ``ra8_c6link_eth_send`` pumps its own -- so both must
 * never touch the wire at once. This driver serialises them behind one ThreadX
 * mutex: transmit takes it around ``ra8_c6link_eth_send``, and a dedicated RX
 * poll worker takes it around ``ra8_c6link_poll``. Received frames arrive inside
 * that poll, on the worker thread, through ::nx_ether_driver_c6_rx, which the
 * application registers as the link's receive callback at ``ra8_c6link_open``.
 *
 * @par The order the application must follow
 * -# open the link with ``rx_cb = nx_ether_driver_c6_rx`` and bring the Wi-Fi
 *    station all the way to associated (``ra8_c6link_wifi_start`` /
 *    ``ra8_c6link_wifi_join`` / wait for ::k_ra8_c6link_event_sta_connected);
 * -# ::nx_ether_driver_c6_bind the open handle to this driver;
 * -# ::nx_ether_driver_c6_set_mac the station address read back from the C6;
 * -# ``nx_ip_create`` naming ::nx_ether_driver_c6 as the link driver, which
 *    fires ``NX_LINK_INITIALIZE`` and spawns the RX worker.
 *
 * @see nx_ether_driver_ra8_eth.h  The on-chip twin this mirrors
 * @see ra8_c6link.h  The link this sits on
 *
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "nx_api.h"
#include "ra8_c6link.h"

/**
 * @brief NetX Duo link driver entry point for the ESP32-C6 Wi-Fi station.
 *
 * @details
 * Pass this symbol as the ``driver`` argument to ``nx_ip_create``. NetX Duo
 * calls it for every link command -- INITIALIZE, ENABLE, PACKET_SEND,
 * GET_STATUS and the rest -- and it dispatches each to the matching handler.
 * The transmit path builds the 14-byte Ethernet header from the destination
 * MAC NetX resolved, the station MAC set by ::nx_ether_driver_c6_set_mac and
 * the EtherType implied by the command, then forwards the whole frame to
 * ``ra8_c6link_eth_send``.
 *
 * @param[in,out] driver_req NetX Duo driver request block; NetX never passes
 *                           null, but a null is tolerated as a no-op.
 *
 * @return Nothing; the outcome is written to @p driver_req->nx_ip_driver_status.
 *
 * @pre ::nx_ether_driver_c6_bind has run with an open, associated link.
 * @pre The caller is NetX Duo's IP thread (the dispatch is not re-entrant).
 * @post @p driver_req->nx_ip_driver_status is set to NX_SUCCESS or an error.
 * @post Any packet carried by a send command is released exactly once.
 *
 * @note Not thread-safe against itself; NetX serialises link commands on the
 *       IP thread. Wire access is serialised against the RX worker by a mutex.
 *
 * @since 0.1.0
 */
void nx_ether_driver_c6(NX_IP_DRIVER* driver_req);

/**
 * @brief Bind an open C6 link handle to the driver and arm its mutex.
 *
 * @details
 * The driver reaches the co-processor only through this handle. Call it once,
 * after the link is open and the station is associated, and before
 * ``nx_ip_create``. It also creates the ThreadX mutex that serialises transmit
 * against the RX poll worker, so it must run after the ThreadX kernel is up.
 *
 * @param[in] link Open, associated C6 link handle; must be non-null and must
 *                 outlive every NetX operation on this interface.
 *
 * @return Nothing.
 *
 * @pre The ThreadX kernel is running (``tx_application_define`` has returned).
 * @pre @p link is open and its receive callback is ::nx_ether_driver_c6_rx.
 * @post The driver forwards transmit and receive through @p link.
 * @post The transmit/receive serialisation mutex exists.
 *
 * @note Idempotent for the mutex: a second bind re-points the handle but does
 *       not recreate the mutex.
 *
 * @since 0.1.0
 */
void nx_ether_driver_c6_bind(ra8_c6link_t* link);

/**
 * @brief Tell the driver the station MAC address to stamp on outgoing frames.
 *
 * @details
 * NetX Duo learns the interface MAC from ``nx_ip_interface_physical_address_set``,
 * but ``nx_ip_create`` fires ``NX_LINK_INITIALIZE`` before the application can
 * call that, so the driver would otherwise stamp a zero source MAC on the first
 * frames. Read the station address with ``ra8_c6link_wifi_mac`` and hand it here
 * before ``nx_ip_create`` so INITIALIZE has the real value.
 *
 * @param[in] mac Six-octet station MAC; must be non-null.
 *
 * @return Nothing.
 *
 * @pre @p mac points at ::k_ra8_c6link_mac_bytes readable octets.
 * @pre Called before ``nx_ip_create`` for deterministic INITIALIZE framing.
 * @post The driver stamps @p mac as the source MAC on every transmitted frame.
 * @post A later ``NX_LINK_INITIALIZE`` prefers this value over the interface.
 *
 * @note Not thread-safe; call it during single-threaded bring-up.
 *
 * @since 0.1.0
 */
void nx_ether_driver_c6_set_mac(const uint8_t mac[6]);

/**
 * @brief Receive callback the application registers with ``ra8_c6link_open``.
 *
 * @details
 * The facade invokes this for every 802.3 frame the co-processor forwards from
 * the associated AP, synchronously inside ``ra8_c6link_poll`` (or inside a
 * transmit pump). It allocates an ``NX_PACKET`` from the IP's default pool,
 * copies the frame in, and hands it to NetX Duo's deferred receive path, keyed
 * by EtherType. Before the interface is up (``NX_LINK_INITIALIZE`` has not run)
 * it drops the frame, which is correct: no data frames flow before association
 * and IP bring-up.
 *
 * @param[in] ctx   Unused context pointer from the link configuration.
 * @param[in] frame Whole Ethernet II frame including the 14-byte header; must
 *                  be non-null when @p len is non-zero.
 * @param[in] len   Frame length in octets.
 *
 * @return Nothing; a frame that cannot be delivered is dropped and counted.
 *
 * @pre The driver has been bound and, for delivery, ``NX_LINK_INITIALIZE`` ran.
 * @pre @p len is at least the 14-byte Ethernet header for delivery.
 * @post A well-formed frame is queued to NetX or dropped with the drop count
 *       incremented; the packet pool is never leaked.
 * @post Runt or pre-initialise frames leave NetX state untouched.
 *
 * @note Runs on the RX worker (or the transmit caller) with the wire mutex
 *       held; it performs no further wire access, only NetX packet operations.
 *
 * @since 0.1.0
 */
void nx_ether_driver_c6_rx(void* ctx, const uint8_t* frame, uint16_t len);

#ifdef __cplusplus
}
#endif
