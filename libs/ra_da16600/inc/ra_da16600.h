/**
 * @file ra_da16600.h
 * @brief Renesas DA16600 Wi-Fi 4 + BLE 5.0 combo-module driver (public API)
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The DA16600 is a Renesas (formerly Dialog) Wi-Fi 4 + Bluetooth LE 5.0
 * combo module that exposes its functionality through an AT-command
 * interface over a 3.3 V UART (default framing: 115200 8N1, no flow
 * control). On the EK-RA8D2 the module is wired through a
 * US159-DA16600EVZ PMOD daughter card plugged into Pmod1 (J26), which
 * routes the chip's TXD2/RXD2 alternate functions to SCI channel 2.
 *
 * This library is a thin AT-command wrapper layered on top of the
 * existing transport in @c ra_modem_at. Every public entry point here
 * formats one DA16600-specific AT command, sends it through
 * @c ra_modem_at_send_cmd / @c ra_modem_at_send_cmd_capture, parses the
 * response payload, and maps the result to an @c ra_err_t code.
 *
 * Every command implemented in this header is cited against Renesas
 * document UM-WI-046 "DA16200_DA16600 AT Command Set" (the only
 * authoritative AT reference for the DA16600). Section numbers and
 * page numbers in the implementation comments refer to the public
 * revision of that document.
 *
 * ## State / lifecycle
 *
 * The driver is single-instance and not thread-safe -- it shares the
 * same constraint as @c ra_modem_at. Lifecycle:
 *
 *   1. Caller brings up the SCI UART (e.g. via @c ra_sci_init).
 *   2. Caller wires byte-level TX/RX/now_ms callbacks into an
 *      @c ra_modem_at_io_t descriptor (any owner -- direct SCI poll
 *      or a higher-level FIFO is fine).
 *   3. Caller invokes @c ra_da16600_init, which calls
 *      @c ra_modem_at_init internally and runs the DA16600 "AT" probe
 *      to confirm the module is alive.
 *   4. After init, Wi-Fi / TCP / BLE entry points may be called in
 *      any order.
 *
 * ## Memory
 *
 * NASA Power-of-10 Rule 3 compliant -- zero dynamic allocation. The
 * caller owns the AT line buffer (handed in via @c ra_da16600_cfg_t)
 * and every per-command capture buffer is on the caller's stack.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra_err.h"
#include "ra_modem_at.h"

/* =============================================================================
 * Compile-time tunables
 * =============================================================================
 */

/**
 * @brief Buffer sizing limits for caller-owned working buffers.
 *
 * @details All values are typed C23 enums so the compiler picks an
 * exact width and each value appears by name in the debugger / disasm.
 */
typedef enum : uint16_t {
  k_ra_da16600_ssid_max_len   = 32U,  /**< IEEE 802.11 SSID max bytes (incl. NUL). */
  k_ra_da16600_passkey_max_len = 64U, /**< WPA2/WPA3 passphrase max bytes (incl. NUL). */
  k_ra_da16600_ip_str_len     = 16U,  /**< Room for "255.255.255.255" + NUL. */
  k_ra_da16600_min_line_buf   = 128U, /**< Floor on caller AT line buffer length. */
} ra_da16600_limits_t;

/**
 * @brief Default per-command timeouts (ms).
 *
 * @details The Wi-Fi-scan timeout in particular is generous because a
 * passive scan across all 2.4 GHz channels can run several seconds.
 */
typedef enum : uint16_t {
  k_ra_da16600_timeout_probe_ms   = 1000U,  /**< AT probe / sanity. */
  k_ra_da16600_timeout_connect_ms = 15000U, /**< Wi-Fi association + DHCP. */
  k_ra_da16600_timeout_scan_ms    = 10000U, /**< Active+passive scan sweep. */
  k_ra_da16600_timeout_socket_ms  = 5000U,  /**< Socket open/close/send. */
  k_ra_da16600_timeout_ble_ms     = 2000U,  /**< BLE advertising start/stop. */
} ra_da16600_timeouts_t;

/* =============================================================================
 * Configuration / lifecycle
 * =============================================================================
 */

/**
 * @struct ra_da16600_cfg_t
 * @brief Configuration handed to ::ra_da16600_init.
 *
 * @details
 * The configuration is a pass-through to @c ra_modem_at_init plus a
 * couple of DA16600-specific knobs. The caller fully populates the
 * @c io vtable (TX/RX byte transport + monotonic clock) before
 * calling init; the driver does NOT touch the SCI peripheral
 * directly.
 *
 * @invariant ``io.tx_byte``, ``io.rx_byte``, ``io.now_ms`` non-NULL.
 * @invariant ``line_buf != nullptr``.
 * @invariant ``line_buf_len >= k_ra_da16600_min_line_buf``.
 */
/* cppcheck-suppress-begin unusedStructMember */
typedef struct {
  ra_modem_at_io_t io;                 /**< Byte-level transport (DIP seam). */
  uint8_t*         line_buf;           /**< Caller-owned AT line accumulator. */
  uint16_t         line_buf_len;       /**< Bytes in @c line_buf (>= floor). */
  uint16_t         default_timeout_ms; /**< 0 -> ra_modem_at default. */
} ra_da16600_cfg_t;
/* cppcheck-suppress-end unusedStructMember */

/**
 * @brief Bring up the DA16600 driver and verify the module is alive.
 *
 * @details
 * Steps:
 *  1. Validate @p cfg fields.
 *  2. Forward @p cfg into @c ra_modem_at_init.
 *  3. Send a bare ``AT\r`` and expect ``OK`` within
 *     @c k_ra_da16600_timeout_probe_ms (UM-WI-046 section 2.1
 *     "Basic AT Commands").
 *
 * @param[in] cfg Pointer to fully populated configuration.
 *
 * @return ::ra_err_t
 * @retval k_ra_ok                  Module is up and responding.
 * @retval k_ra_err_null_ptr        @p cfg, its line buffer, or any
 *                                  IO callback was NULL.
 * @retval k_ra_err_invalid_size    @c line_buf_len is below the floor.
 * @retval k_ra_err_hw_timeout      Module did not reply ``OK`` in time.
 * @retval k_ra_err_hw_error        Module replied ``ERROR``.
 *
 * @pre @c cfg is non-NULL and fully populated.
 * @pre Underlying SCI UART has been initialized by the caller.
 * @post On success, the driver is ready for Wi-Fi / TCP / BLE calls.
 * @post On any error path, the module is left in an undefined state
 *       and must be hard-reset before retrying.
 *
 * @note Not thread-safe; the AT facade is single-owner.
 * @see UM-WI-046 section 2.1 (AT probe).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_da16600_init(const ra_da16600_cfg_t* cfg);

/* =============================================================================
 * Wi-Fi
 * =============================================================================
 */

/**
 * @brief Trigger a Wi-Fi scan and return the number of networks found.
 *
 * @details
 * Sends ``AT+WFSCAN\r`` (UM-WI-046 section 4.1 "Wi-Fi Scan"). The
 * module replies with one ``+WFSCAN:<bss list>`` line per call, where
 * the BSS list is a slash-separated string of
 * ``MAC,channel,RSSI,security,SSID``. This routine parses the line
 * and counts entries; it does not return per-entry detail (callers
 * that need the full scan list can use @c ra_da16600_wifi_scan_capture
 * once it lands).
 *
 * @param[out] out_count Number of BSSIDs reported by the module.
 *
 * @return ::ra_err_t
 * @retval k_ra_ok                  Scan completed (count may be 0).
 * @retval k_ra_err_null_ptr        @p out_count is NULL.
 * @retval k_ra_err_not_initialized ::ra_da16600_init not called yet.
 * @retval k_ra_err_hw_timeout      Scan did not finish in time.
 * @retval k_ra_err_hw_error        Module replied ``ERROR``.
 *
 * @pre ::ra_da16600_init returned ::k_ra_ok.
 * @pre @c out_count points to writable storage.
 * @post On success, ``*out_count`` reflects the entry count parsed.
 * @post On any error, ``*out_count`` is zeroed.
 *
 * @note Blocking call; may take several seconds.
 * @see UM-WI-046 section 4.1.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_da16600_wifi_scan(uint16_t* out_count);

/**
 * @brief Associate with a Wi-Fi network and acquire an IPv4 lease via DHCP.
 *
 * @details
 * Wraps ``AT+WFJAP=<ssid>,<security>,<key>`` (UM-WI-046 section 4.5
 * "Connect / Disconnect AP"). After association the DA16600 returns
 * an ``OK`` and asynchronously emits ``+WFJAP:1,<ssid>,<ip>`` as a
 * URC once it has acquired an IP. This routine blocks until the URC
 * arrives or @c k_ra_da16600_timeout_connect_ms expires.
 *
 * @param[in]  ssid       NUL-terminated SSID (1..31 bytes).
 * @param[in]  passkey    NUL-terminated WPA2 passphrase (8..63 bytes).
 * @param[out] out_ip_str Buffer for dotted-decimal IPv4 string. Must
 *                        be at least @c k_ra_da16600_ip_str_len bytes.
 * @param[in]  ip_str_len Capacity of @p out_ip_str.
 *
 * @return ::ra_err_t
 * @retval k_ra_ok                  Associated, DHCP lease acquired.
 * @retval k_ra_err_null_ptr        Any pointer arg was NULL.
 * @retval k_ra_err_invalid_size    SSID empty, passkey too short,
 *                                  or @p ip_str_len too small.
 * @retval k_ra_err_not_initialized ::ra_da16600_init not called yet.
 * @retval k_ra_err_hw_timeout      Association or DHCP timed out.
 * @retval k_ra_err_hw_error        Module replied ``ERROR``.
 *
 * @pre ::ra_da16600_init returned ::k_ra_ok.
 * @pre @p ssid, @p passkey, and @p out_ip_str are non-NULL.
 * @pre @p ip_str_len >= @c k_ra_da16600_ip_str_len.
 * @post On success, ``out_ip_str`` is a NUL-terminated dotted-decimal
 *       IPv4 string.
 * @post On any error, ``out_ip_str[0] = '\0'`` (empty NUL-terminated).
 *
 * @note Blocking call; can take 5+ seconds on a slow AP.
 * @see UM-WI-046 section 4.5.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_da16600_wifi_connect(const char* ssid,
                                                const char* passkey,
                                                char*       out_ip_str,
                                                size_t      ip_str_len);

/**
 * @brief Disassociate from the current Wi-Fi network.
 *
 * @details
 * Sends ``AT+WFQAP\r`` (UM-WI-046 section 4.5). The module replies
 * ``OK`` once association is torn down.
 *
 * @return ::ra_err_t
 * @retval k_ra_ok                  Disassociated.
 * @retval k_ra_err_not_initialized ::ra_da16600_init not called yet.
 * @retval k_ra_err_hw_timeout      Module did not reply in time.
 * @retval k_ra_err_hw_error        Module replied ``ERROR``.
 *
 * @pre ::ra_da16600_init returned ::k_ra_ok.
 * @post On success, the module is dis-associated.
 *
 * @see UM-WI-046 section 4.5.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_da16600_wifi_disconnect(void);

/* =============================================================================
 * TCP sockets
 * =============================================================================
 */

/**
 * @brief Opaque socket handle returned by ::ra_da16600_tcp_open.
 *
 * @details
 * The DA16600 multiplexes sockets by a small integer cid (0..3 per
 * UM-WI-046 section 5.2 "Transmission Control Protocol"). We expose
 * the cid behind a typedef so callers do not depend on the raw int
 * range.
 */
typedef uint8_t ra_da16600_socket_t;

/**
 * @enum ra_da16600_socket_role_t
 * @brief Listening vs connected socket roles.
 *
 * @details
 * The DA16600 has two distinct AT commands for the two roles
 * (``AT+TRTS`` for listen / accept and ``AT+TRTC`` for client
 * connect, UM-WI-046 section 5.2). We pick the right one based on
 * this enum.
 */
typedef enum : uint8_t {
  k_ra_da16600_socket_listen  = 0U, /**< Accept inbound TCP via AT+TRTS. */
  k_ra_da16600_socket_connect = 1U, /**< Outbound TCP client via AT+TRTC. */
} ra_da16600_socket_role_t;

/**
 * @brief Open a TCP socket (listen or connect).
 *
 * @details
 * For ``k_ra_da16600_socket_listen`` the driver sends
 * ``AT+TRTS=<port>\r`` (UM-WI-046 section 5.2.3) and waits for the
 * confirmation that a listening cid was allocated. For
 * ``k_ra_da16600_socket_connect`` it sends
 * ``AT+TRTC=<ip>,<port>\r`` (UM-WI-046 section 5.2.4) and blocks
 * until ``+TRTC:<cid>`` is reported.
 *
 * @param[in]  role        Listen vs connect role.
 * @param[in]  remote_ip   Dotted-decimal IPv4 string for the connect
 *                         role; ignored for listen (pass NULL or "").
 * @param[in]  port        TCP port number in host byte order.
 * @param[out] out_socket  Socket handle on success.
 *
 * @return ::ra_err_t
 * @retval k_ra_ok                  Socket opened.
 * @retval k_ra_err_null_ptr        @p out_socket NULL, or @p remote_ip
 *                                  NULL when role is connect.
 * @retval k_ra_err_invalid_arg     Unrecognized @p role.
 * @retval k_ra_err_not_initialized ::ra_da16600_init not called yet.
 * @retval k_ra_err_hw_timeout      Module did not reply in time.
 * @retval k_ra_err_hw_error        Module replied ``ERROR``.
 *
 * @pre ::ra_da16600_init returned ::k_ra_ok and Wi-Fi associated.
 * @pre @p out_socket points to writable storage.
 * @post On success, ``*out_socket`` is a valid cid.
 * @post On any error, ``*out_socket`` is zeroed.
 *
 * @see UM-WI-046 section 5.2.3 (TRTS), section 5.2.4 (TRTC).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_da16600_tcp_open(ra_da16600_socket_role_t role,
                                            const char*              remote_ip,
                                            uint16_t                 port,
                                            ra_da16600_socket_t*     out_socket);

/**
 * @brief Send a byte payload on an open TCP socket.
 *
 * @details
 * Wraps ``AT+TRDTS=<cid>,<len>,<payload>\r`` (UM-WI-046 section 5.2.5
 * "Send Data on TCP"). The byte payload is passed through verbatim;
 * the driver does NOT escape any AT-special bytes. Callers must
 * ensure their payload is < 1460 bytes (one MSS) to fit a single
 * AT-frame.
 *
 * @param[in] sock TCP socket handle from ::ra_da16600_tcp_open.
 * @param[in] data Payload bytes.
 * @param[in] len  Number of bytes in @p data (1..1460).
 *
 * @return ::ra_err_t
 * @retval k_ra_ok                  Payload accepted by module.
 * @retval k_ra_err_null_ptr        @p data NULL with @p len > 0.
 * @retval k_ra_err_invalid_size    @p len == 0 or > 1460.
 * @retval k_ra_err_not_initialized ::ra_da16600_init not called yet.
 * @retval k_ra_err_hw_timeout      Module did not reply in time.
 * @retval k_ra_err_hw_error        Module replied ``ERROR``.
 *
 * @pre ::ra_da16600_tcp_open returned ::k_ra_ok for @p sock.
 * @pre @p data is non-NULL when @p len > 0.
 * @post On success, the bytes have been queued by the DA16600 stack.
 *
 * @see UM-WI-046 section 5.2.5.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_da16600_tcp_send(ra_da16600_socket_t sock,
                                            const uint8_t*      data,
                                            size_t              len);

/**
 * @brief Block until a payload arrives on the socket or the timeout fires.
 *
 * @details
 * Drains the AT pipe and looks for ``+TRDTC:<cid>,<len>,<payload>``
 * URCs (UM-WI-046 section 5.2.6 "Receive Data on TCP"). Returns the
 * first matching payload up to @p cap bytes; further bytes from the
 * same line are silently truncated. ``*out_len`` reports the
 * number of bytes actually written to @p buf.
 *
 * @param[in]  sock       Socket handle to read from.
 * @param[out] buf        Caller buffer.
 * @param[in]  cap        Capacity of @p buf.
 * @param[out] out_len    Bytes actually delivered (0 on timeout).
 * @param[in]  timeout_ms Wait window in ms (0 -> driver default).
 *
 * @return ::ra_err_t
 * @retval k_ra_ok                  At least one byte was delivered.
 * @retval k_ra_err_null_ptr        @p buf or @p out_len NULL.
 * @retval k_ra_err_invalid_size    @p cap == 0.
 * @retval k_ra_err_not_initialized ::ra_da16600_init not called yet.
 * @retval k_ra_err_hw_timeout      No payload arrived in time.
 * @retval k_ra_err_hw_error        Module reported an error URC.
 *
 * @pre ::ra_da16600_tcp_open returned ::k_ra_ok for @p sock.
 * @pre @p buf points to @p cap bytes of writable storage.
 * @post On success, ``buf[0..*out_len-1]`` holds payload bytes.
 * @post On any error, ``*out_len = 0``.
 *
 * @see UM-WI-046 section 5.2.6.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_da16600_tcp_recv(ra_da16600_socket_t sock,
                                            uint8_t*            buf,
                                            size_t              cap,
                                            size_t*             out_len,
                                            uint16_t            timeout_ms);

/**
 * @brief Close a previously opened TCP socket.
 *
 * @details
 * Wraps ``AT+TRTRM=<cid>\r`` (UM-WI-046 section 5.2.7
 * "Terminate Session"). The module replies ``OK`` once the cid has
 * been released.
 *
 * @param[in] sock Socket handle to close.
 *
 * @return ::ra_err_t
 * @retval k_ra_ok                  Socket closed.
 * @retval k_ra_err_not_initialized ::ra_da16600_init not called yet.
 * @retval k_ra_err_hw_timeout      Module did not reply in time.
 * @retval k_ra_err_hw_error        Module replied ``ERROR``.
 *
 * @pre ::ra_da16600_tcp_open returned ::k_ra_ok for @p sock.
 * @post On success, the cid is no longer usable.
 *
 * @see UM-WI-046 section 5.2.7.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_da16600_tcp_close(ra_da16600_socket_t sock);

/* =============================================================================
 * Bluetooth Low Energy
 * =============================================================================
 */

/**
 * @brief Begin LE advertising with the DA16600 default profile.
 *
 * @details
 * Wraps ``AT+BLEADVSTART\r`` (UM-WI-046 section 7.4
 * "BLE Advertising Control"). The module replies ``OK`` once
 * advertising has started; the advertising payload itself is
 * configured at module-flash time and is out of scope for this
 * driver.
 *
 * @return ::ra_err_t
 * @retval k_ra_ok                  Advertising armed.
 * @retval k_ra_err_not_initialized ::ra_da16600_init not called yet.
 * @retval k_ra_err_hw_timeout      Module did not reply in time.
 * @retval k_ra_err_hw_error        Module replied ``ERROR``.
 *
 * @pre ::ra_da16600_init returned ::k_ra_ok.
 * @post On success, the module is advertising connectable LE PDUs.
 *
 * @see UM-WI-046 section 7.4.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_da16600_ble_advertise_start(void);

/**
 * @brief Stop LE advertising.
 *
 * @details
 * Wraps ``AT+BLEADVSTOP\r`` (UM-WI-046 section 7.4).
 *
 * @return ::ra_err_t
 * @retval k_ra_ok                  Advertising stopped.
 * @retval k_ra_err_not_initialized ::ra_da16600_init not called yet.
 * @retval k_ra_err_hw_timeout      Module did not reply in time.
 * @retval k_ra_err_hw_error        Module replied ``ERROR``.
 *
 * @pre ::ra_da16600_init returned ::k_ra_ok.
 * @post On success, the module is no longer advertising.
 *
 * @see UM-WI-046 section 7.4.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_da16600_ble_advertise_stop(void);

#ifdef __cplusplus
}
#endif
