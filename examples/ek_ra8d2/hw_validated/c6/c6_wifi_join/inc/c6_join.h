/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_wifi_join/inc/c6_join.h
 * @brief Shared contract for the C6 Wi-Fi join + DHCP + reachability app.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * ``c6_wifi_link`` took the ESP32-C6's Wi-Fi station up through ``ra8_c6link``
 * and read its MAC, but joined no network -- that was left to #492. This
 * application is #492: it associates the station with the bench access point,
 * runs a NetX Duo DHCP client over the ``nx_ether_driver_c6`` link driver to
 * obtain a lease, and then proves the path carries traffic by pinging the
 * lease's gateway.
 *
 * Three modules, driven by ``main.c``:
 *
 *   - ``src/c6_join_console.c`` -- bounded console formatters, so the image
 *     links no newlib ``printf``;
 *   - ``src/c6_join_net.c``     -- the NetX Duo bring-up: packet pool, IP,
 *     DHCP client, and the ICMP reachability check;
 *   - ``main.c``               -- clocks, the station bring-up and join, then
 *     the IP bring-up and the single PASS / FAIL verdict.
 *
 * The image and build graph contain no SSID or passphrase. The worker accepts
 * one bounded versioned provisioning line over the debug UART after boot and
 * explicitly erases the decoded record after association.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_c6link.h"
#include "ra8_err.h"

/**
 * @enum c6_join_cfg_t
 * @brief Link, thread and pacing parameters this application chooses.
 * @details Only values an application owns live here. This image has no heap
 * (NASA Power of 10 Rule 3), so the worker stack and the decode arena are sized
 * here alongside the link timing. The SPI rate matches the rate the C6 RPC
 * round-trip and ``c6_wifi_link`` both ran at on silicon.
 * @invariant ::k_c6_join_sck_hz is the bench-proven bit rate, changed one
 *            variable at a time relative to ``c6_wifi_link``.
 * @invariant ::k_c6_join_arena_bytes is at least ::k_ra8_c6link_arena_min,
 *            which ::ra8_c6link_open enforces.
 * @par Example:
 * @code
 * const ra8_esp_hosted_port_cfg_t cfg = { .sck_hz = (uint32_t)k_c6_join_sck_hz };
 * @endcode
 * @see ra8_esp_hosted_port_cfg_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_c6_join_uart_baud    = 115200U,  /**< Console rate, 8N1, over the J-Link OB VCOM. */
  k_c6_join_sck_hz       = 5000000U, /**< SPI bit rate; the rate silicon ran at.      */
  k_c6_join_edge_poll_ms = 2U,       /**< Poll period for a side-band pin with no ICU
                                     channel.                                         */
  k_c6_join_boot_wait_ms = 200U,     /**< Settling delay before the first transaction. */
  k_c6_join_heartbeat_ms = 5000U,    /**< Heartbeat gap after the verdict, in
                                     milliseconds and therefore in ThreadX ticks.     */
  k_c6_join_worker_stack = 8192U,    /**< Worker-thread stack, in bytes. The protobuf
                                     decoder recurses through nested messages.        */
  k_c6_join_worker_prio  = 8U,       /**< Worker priority and preemption threshold. */
  k_c6_join_arena_bytes  = 4096U,    /**< Decode arena handed to the facade.        */
} c6_join_cfg_t;

/**
 * @enum c6_join_net_t
 * @brief Timing and retry bounds for association, DHCP and the ping.
 * @details The three waits an IP bring-up performs, all bounded so no loop can
 * spin forever (NASA Power of 10 Rule 2). ThreadX runs at one tick per
 * millisecond in this tree, so a millisecond value doubles as a tick count.
 * @invariant ::k_c6_join_assoc_tries multiplied by ::k_c6_join_assoc_gap_ms is
 *            the total association budget in milliseconds.
 * @invariant ::k_c6_join_ping_tries is non-zero, so at least one echo is sent.
 * @par Example:
 * @code
 * const UINT to = (UINT)k_c6_join_dhcp_wait_ms;
 * @endcode
 * @see c6_join_net_up
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_c6_join_assoc_tries  = 200U,   /**< Association poll attempts before giving up.  */
  k_c6_join_assoc_gap_ms = 50U,    /**< Gap between association polls, milliseconds. */
  k_c6_join_dhcp_wait_ms = 25000U, /**< DHCP lease wait budget, milliseconds/ticks.  */
  k_c6_join_ping_tries   = 4U,     /**< ICMP echoes sent to the gateway.             */
  k_c6_join_ping_wait_ms = 2000U,  /**< Per-echo reply timeout, milliseconds/ticks.  */
  k_c6_join_ping_gap_ms  = 250U,   /**< Gap between echoes, milliseconds.            */
} c6_join_net_t;

/**
 * @enum c6_join_fmt_t
 * @brief Bounds for the console formatters in ``src/c6_join_console.c``.
 * @details The image links no newlib ``printf``, so the serialisers do their own
 * digit extraction; every loop they run is bounded by a value from this
 * enumeration (NASA Power of 10 Rule 2).
 * @invariant ::k_c6_join_dec_digits holds the widest 32-bit decimal value.
 * @invariant ::k_c6_join_ip_octets is four, the octet count of an IPv4 address.
 * @par Example:
 * @code
 * c6_join_put_hex(chip_id, (uint8_t)k_c6_join_hex_byte);
 * @endcode
 * @see c6_join_put_u32
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_c6_join_str_max    = 256U,  /**< Longest string the console helper emits.   */
  k_c6_join_dec_radix  = 10U,   /**< Decimal radix.                             */
  k_c6_join_dec_digits = 10U,   /**< Digits in the widest 32-bit decimal value. */
  k_c6_join_hex_digits = 8U,    /**< Digits in the widest 32-bit hex value.     */
  k_c6_join_hex_bits   = 4U,    /**< Bits per hexadecimal digit.                */
  k_c6_join_hex_mask   = 0x0FU, /**< Nibble mask.                               */
  k_c6_join_hex_alpha  = 10U,   /**< First nibble value spelled with a letter.  */
  k_c6_join_hex_byte   = 2U,    /**< Hex digits printed for a byte-wide field.  */
  k_c6_join_ip_octets  = 4U,    /**< Octets in an IPv4 address.                 */
  k_c6_join_ip_mask    = 0xFFU, /**< Single-octet mask for IPv4 formatting.     */
} c6_join_fmt_t;

/**
 * @enum c6_join_ip_shift_t
 * @brief Byte shifts that split a packed IPv4 address into its four octets.
 * @details A NetX ``ULONG`` IP address is big-endian in value: octet one is the
 * most-significant byte. These shifts pull each octet for dotted-quad printing.
 * @invariant The four shifts are 24, 16, 8 and 0, most-significant octet first.
 * @par Example:
 * @code
 * const uint8_t o0 = (uint8_t)((ip >> k_c6_join_ip_shift_0) & k_c6_join_ip_mask);
 * @endcode
 * @see c6_join_put_ip
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_c6_join_ip_shift_0 = 24U, /**< Shift for the first (highest) octet. */
  k_c6_join_ip_shift_1 = 16U, /**< Shift for the second octet.          */
  k_c6_join_ip_shift_2 = 8U,  /**< Shift for the third octet.           */
  k_c6_join_ip_shift_3 = 0U,  /**< Shift for the fourth (lowest) octet. */
} c6_join_ip_shift_t;

/**
 * @struct c6_join_lease
 * @brief What the DHCP bring-up learned, for the verdict and the console.
 * @details Filled by ::c6_join_net_up. Every address is a NetX packed IPv4
 * ``ULONG`` (host order), so ::c6_join_put_ip prints it directly.
 * @invariant ``ip`` is non-zero exactly when a lease was obtained.
 * @invariant ``ping_ok`` is only meaningful when ``ip`` is non-zero.
 * @par Example:
 * @code
 * c6_join_lease_t lease = {};
 * if (c6_join_net_up(&link, &mac, &lease) == k_ra8_ok) { use(lease.ip); }
 * @endcode
 * @see c6_join_net_up
 * @since 0.1.0
 */
typedef struct c6_join_lease {
  uint32_t ip;          /**< Leased station address, or zero on failure.      */
  uint32_t mask;        /**< Subnet mask from the lease.                      */
  uint32_t gateway;     /**< Default gateway from the lease; the ping target. */
  uint32_t dhcp_server; /**< Address of the DHCP server that answered.        */
  bool     ping_ok;     /**< A gateway ICMP echo reply was received.          */
} c6_join_lease_t;

/**
 * @brief Bring the IP layer up over the associated C6 link and prove traffic.
 *
 * @details
 * Binds ::nx_ether_driver_c6 to @p link, hands it the station MAC, creates the
 * NetX Duo packet pool and IP instance, enables ARP / UDP / ICMP, runs the DHCP
 * client to a bound lease, then pings the gateway. It performs no ``ra8_c6link``
 * calls itself after ``nx_ip_create`` -- from that point the driver's RX worker
 * owns the wire -- so the caller must not poll the link directly once this runs.
 *
 * @param[in]  link Open, associated C6 link handle; must be non-null.
 * @param[in]  mac  Station MAC read with ``ra8_c6link_wifi_mac``; must be non-null.
 * @param[out] out  Lease and reachability result; must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok A lease was obtained; @p out->ping_ok reports reachability.
 * @retval k_ra8_err_null_ptr @p link, @p mac or @p out was null.
 * @retval k_ra8_err_not_initialized A NetX object could not be created.
 * @retval k_ra8_err_timeout No DHCP lease arrived within the budget.
 *
 * @pre The station has associated (::k_ra8_c6link_event_sta_connected seen).
 * @pre The ThreadX kernel is running and NetX has been system-initialised.
 * @post On success @p out holds a non-zero address; on failure @p out is cleared.
 * @post After this returns the C6 link is owned by the driver's RX worker.
 *
 * @note Not thread-safe; runs once on the application worker thread.
 * @warning Blocks up to ::k_c6_join_dhcp_wait_ms waiting for the lease.
 *
 * @par Example:
 * @code
 * c6_join_lease_t lease = {};
 * (void)c6_join_net_up(&s_link, &mac, &lease);
 * @endcode
 *
 * @see nx_ether_driver_c6
 * @since 0.1.0
 */
ra8_err_t c6_join_net_up(ra8_c6link_t* link, const ra8_c6link_mac_t* mac, c6_join_lease_t* out);

/**
 * @brief Write a NUL-terminated string to the board console.
 * @details Measures at most the fixed string bound and forwards the resulting
 *          byte extent to the initialized board-console transmitter.
 * @param[in] text String to emit; null is ignored and the length is capped at
 *                 ::k_c6_join_str_max.
 * @return Nothing.
 * @pre ``ra8_board_uart_console_init`` has succeeded.
 * @pre @p text is NUL-terminated within ::k_c6_join_str_max bytes.
 * @post The bytes are queued on the console transmitter.
 * @post No application state is modified.
 * @note Not thread-safe; only bring-up and the single worker thread call it.
 * @see c6_join_put_u32
 * @since 0.1.0
 */
void c6_join_puts(const char* text);

/**
 * @brief Emit an unsigned 32-bit value in decimal.
 * @details Converts digits into a fixed local buffer in reverse order, then
 *          emits the used suffix without allocation or stdio.
 * @param[in] value Value to print; the whole 32-bit range is representable.
 * @return Nothing.
 * @pre The console is up.
 * @pre The caller wants no padding; zero prints as a single digit.
 * @post Between one and ::k_c6_join_dec_digits characters were emitted.
 * @post No application state is modified.
 * @note Not thread-safe, for the same reason as ::c6_join_puts.
 * @see c6_join_put_ip
 * @since 0.1.0
 */
void c6_join_put_u32(uint32_t value);

/**
 * @brief Emit a value as a fixed-width lower-case hexadecimal field.
 * @details Extracts one nibble per requested digit from most to least
 *          significant position and emits a fixed local character buffer.
 * @param[in] value Value to print.
 * @param[in] digits Field width, 1..::k_c6_join_hex_digits; an out-of-range
 *                   width prints nothing rather than overrunning the array.
 * @return Nothing.
 * @pre The console is up.
 * @pre @p digits is within 1..::k_c6_join_hex_digits.
 * @post Exactly @p digits characters were emitted, or none on a bad width.
 * @post No application state is modified.
 * @note The loop is bounded by the range-checked @p digits (NASA Rule 2).
 * @see c6_join_put_u32
 * @since 0.1.0
 */
void c6_join_put_hex(uint32_t value, uint8_t digits);

/**
 * @brief Emit a packed IPv4 address as a dotted quad.
 * @details Extracts each bounded network octet from most to least significant
 *          position and separates decimal forms with periods.
 * @param[in] ip NetX host-order packed IPv4 address.
 * @return Nothing.
 * @pre The console is up.
 * @pre @p ip is a NetX ``ULONG`` address, most-significant octet first.
 * @post Between seven and fifteen characters were emitted.
 * @post No application state is modified.
 * @note The loop is bounded by ::k_c6_join_ip_octets (NASA Rule 2).
 * @see c6_join_put_u32
 * @since 0.1.0
 */
void c6_join_put_ip(uint32_t ip);

/**
 * @brief Emit an IEEE 802 address as six colon-separated hexadecimal octets.
 * @details Walks the fixed address extent in wire order, printing two hex
 *          digits per octet and a colon between adjacent octets.
 * @param[in] mac Address to print; null prints nothing.
 * @return Nothing.
 * @pre The console is up.
 * @pre @p mac was filled by ::ra8_c6link_wifi_mac or is the zero address.
 * @post Seventeen characters were emitted, or none on a null argument.
 * @post No application state is modified.
 * @note The loop is bounded by ::k_ra8_c6link_mac_bytes (NASA Rule 2).
 * @see ra8_c6link_wifi_mac
 * @since 0.1.0
 */
void c6_join_put_mac(const ra8_c6link_mac_t* mac);

/**
 * @brief Print the banner: identity, clocks and link parameters.
 * @details Emits the application identity followed by measured CPU and
 *          peripheral clocks and immutable C6 transport geometry.
 * @param[in] cpuclk_hz Live CPUCLK0 rate in hertz.
 * @param[in] pclka_hz Live PCLKA rate in hertz, the SCI baud-clock source.
 * @return Nothing.
 * @pre The console is up.
 * @pre Both rates were read from the CGC rather than assumed.
 * @post Three banner lines were emitted.
 * @post No application state is modified.
 * @note The SPI mode is the co-processor's build (``CONFIG_ESP_SPI_MODE=3``).
 * @see c6_join_puts
 * @since 0.1.0
 */
void c6_join_print_banner(uint32_t cpuclk_hz, uint32_t pclka_hz);
