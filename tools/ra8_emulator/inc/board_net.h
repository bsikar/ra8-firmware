/**
 * @file board_net.h
 * @brief Virtual network peer for ra8_emulator -- talks TCP/IP to the firmware
 *
 * @details
 * ra8_emulator shims the firmware's ra8_eth frame API (ra8_eth_write / ra8_eth_read /
 * ra8_eth_link_status -- the same seam the NetX Duo driver uses) and routes the
 * Ethernet frames here. This module is the "other host on the wire": a tiny
 * userspace TCP/IP stack (Ethernet + ARP + IPv4 + ICMP + TCP) that resolves the
 * firmware (192.168.1.42), pings it, and connects to its echo server -- so a
 * NetX networking example runs end-to-end with no hardware and no host network
 * setup. main.c marshals guest memory to/from the plain byte buffers here, so
 * this code is portable, AppKit-free C.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reset the virtual network peer to its initial state.
 *
 * @param[in] trace Mirror per-frame activity to stderr when true.
 */
void board_net_init(bool trace);

/**
 * @brief Hand one frame the firmware transmitted to the peer (ra8_eth_write).
 *
 * @param[in] frame Ethernet frame bytes (no FCS).
 * @param[in] len   Frame length in bytes.
 */
void board_net_on_tx(const uint8_t* frame, uint32_t len);

/**
 * @brief Fetch the next frame the peer wants the firmware to receive.
 *
 * @param[out] buf Destination for the frame bytes.
 * @param[in]  max Capacity of @p buf.
 * @return Frame length copied (0 if the peer has nothing queued).
 */
uint32_t board_net_poll_rx(uint8_t* buf, uint32_t max);

/**
 * @brief Advance the peer's state machine one tick (ARP -> ping -> TCP echo).
 */
void board_net_tick(void);

/** @brief Print the end-of-run network summary (link / ARP / ping / TCP). */
void board_net_report(void);

#ifdef __cplusplus
}
#endif
