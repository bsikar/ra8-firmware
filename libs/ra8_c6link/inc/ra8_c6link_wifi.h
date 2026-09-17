/**
 * @file ra8_c6link_wifi.h
 * @brief Wi-Fi station bring-up and association over the C6 link.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * A station join is eleven RPC ids out of the several hundred the esp-hosted
 * protocol defines, and the only "configuration" the co-processor needs is an
 * SSID, a passphrase and a handful of scalars. This header is that, and nothing
 * else: it does not reproduce ESP-IDF's `wifi_*_t` types, because the C6
 * decodes protobuf messages with named fields and no layout on this side is
 * ever transmitted (see the rationale in `ra8_c6link.h`).
 *
 * @par The sequence, and why it is three calls rather than eight
 * ESP-IDF's station bring-up is `esp_wifi_init`, `esp_wifi_set_mode`,
 * `esp_wifi_set_config`, `esp_wifi_start`, `esp_wifi_connect`. Each is a
 * separate RPC and each can fail, but there is exactly one useful order and no
 * caller in this tree wants a different one. So they are grouped into the three
 * operations a network stack actually performs -- start the radio, join a
 * network, leave -- and the failing step is recoverable through
 * ::ra8_c6link_last_fault rather than through eight entry points that must be
 * called in the right order or not at all.
 *
 * @par Association is asynchronous
 * ::ra8_c6link_wifi_join returns once the co-processor has accepted the
 * request. The association itself completes later and is announced as
 * ::k_ra8_c6link_event_sta_connected, or fails as
 * ::k_ra8_c6link_event_sta_disconnected carrying an 802.11 reason code. A
 * caller that needs to block waits by pumping ::ra8_c6link_poll until one of
 * those arrives; there is no hidden wait inside this library, because the
 * length of that wait is a policy the IP stack above owns.
 *
 * @par Example:
 * @code
 * ra8_c6link_sta_cfg_t sta = {};
 * (void)ra8_c6link_sta_cfg_set(&sta, "ra8-bench", secret);
 * if (ra8_c6link_wifi_start(&link) == k_ra8_ok) {
 *   (void)ra8_c6link_wifi_join(&link, &sta);
 * }
 * @endcode
 *
 * @see ra8_c6link.h  The link this sits on
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

#include "ra8_c6link.h"
#include "ra8_err.h"

/**
 * @enum ra8_c6link_wifi_limits_t
 * @brief Field capacities the station configuration works to.
 *
 * @details
 * Both come from 802.11 and from the protobuf field comments upstream ships
 * ("SSID of target AP. 32char", "Password of target AP. 64char"), so they are
 * the co-processor's limits as much as this host's.
 *
 * @invariant ::k_ra8_c6link_pass_max is the longest WPA passphrase, which is
 *            also long enough for a 64-character PSK written out in hex.
 * @invariant Both storage arrays carry one extra octet for a NUL, so a
 *            maximum-length value is still a usable C string.
 *
 * @par Example:
 * @code
 * static_assert(k_ra8_c6link_pass_max == 64U, "WPA passphrase bound");
 * @endcode
 *
 * @see ra8_c6link_sta_cfg_t
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_c6link_pass_max = 64U,
  /**< Longest passphrase the co-processor accepts, in octets. */
  k_ra8_c6link_channel_max = 14U,
  /**< Highest 2.4 GHz channel number; zero means "scan for it". */
} ra8_c6link_wifi_limits_t;

/**
 * @struct ra8_c6link_sta_cfg
 * @brief What this host must tell the co-processor to join one network.
 *
 * @details
 * Fill it with ::ra8_c6link_sta_cfg_set rather than by hand, so the lengths are
 * derived once and the NUL terminators are guaranteed. The optional fields
 * below the credentials narrow the search: a known channel skips a full scan,
 * and a known BSSID pins the association to one radio.
 *
 * @invariant `ssid_len` is non-zero and at most ::k_ra8_c6link_ssid_max.
 * @invariant `pass_len` is zero for an open network, otherwise at most
 *            ::k_ra8_c6link_pass_max.
 *
 * @par Example:
 * @code
 * ra8_c6link_sta_cfg_t sta = {};
 * (void)ra8_c6link_sta_cfg_set(&sta, "ra8-bench", psk);
 * sta.channel = 6U;
 * @endcode
 *
 * @see ra8_c6link_wifi_join
 * @since 0.1.0
 */
typedef struct ra8_c6link_sta_cfg {
  char             ssid[k_ra8_c6link_ssid_max + 1U]; /**< Target SSID, NUL-terminated.    */
  char             pass[k_ra8_c6link_pass_max + 1U]; /**< Passphrase, NUL-terminated.     */
  ra8_c6link_mac_t bssid;                            /**< Target AP address when pinned.  */
  uint8_t          ssid_len;                         /**< Octets of `ssid` to transmit.   */
  uint8_t          pass_len;                         /**< Octets of `pass` to transmit.   */
  uint8_t          channel;                          /**< Channel to try first, or zero.  */
  bool             bssid_set;                        /**< Pin the association to `bssid`. */
} ra8_c6link_sta_cfg_t;

/**
 * @struct ra8_c6link_ap_info
 * @brief What the co-processor knows about the AP the station is on.
 *
 * @details
 * Filled by ::ra8_c6link_wifi_ap_info from the co-processor's own AP record.
 * Useful as a post-association sanity check -- the BSSID and channel here are
 * what the radio actually settled on, which is not necessarily what was asked
 * for when the request left the channel and BSSID open.
 *
 * @invariant `ssid_len` never exceeds ::k_ra8_c6link_ssid_max and `ssid` is
 *            NUL-terminated at it.
 * @invariant `rssi` is in dBm and is negative for any real association.
 *
 * @par Example:
 * @code
 * ra8_c6link_ap_info_t ap = {};
 * if (ra8_c6link_wifi_ap_info(&link, &ap) == k_ra8_ok) { use(ap.rssi); }
 * @endcode
 *
 * @see ra8_c6link_wifi_ap_info
 * @since 0.1.0
 */
typedef struct ra8_c6link_ap_info {
  ra8_c6link_mac_t bssid;                            /**< Address of the associated AP.    */
  char             ssid[k_ra8_c6link_ssid_max + 1U]; /**< Its SSID, NUL-terminated.        */
  uint8_t          ssid_len;                         /**< Octets of `ssid`.                */
  uint8_t          channel;                          /**< Primary channel in use.          */
  int8_t           rssi;                             /**< Signal level in dBm.             */
  int32_t          authmode;                         /**< Co-processor `wifi_auth_mode_t`. */
} ra8_c6link_ap_info_t;

/**
 * @brief Fill a station configuration from an SSID and a passphrase.
 *
 * @details
 * Copies both strings into the record, bounded, and derives their lengths. An
 * empty or null passphrase means an open network, which is a legitimate
 * configuration and not an error. Everything else in the record is cleared, so
 * a stale channel or BSSID from a previous use cannot survive.
 *
 * @param[out] cfg Record to fill; must be non-null.
 * @param[in] ssid NUL-terminated SSID; must be non-null and 1..
 *                 ::k_ra8_c6link_ssid_max octets.
 * @param[in] pass NUL-terminated passphrase, or null for an open network; at
 *                 most ::k_ra8_c6link_pass_max octets.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The record is filled and ready for ::ra8_c6link_wifi_join.
 * @retval k_ra8_err_null_ptr @p cfg or @p ssid was null.
 * @retval k_ra8_err_invalid_size @p ssid was empty or either string was longer
 *         than its field.
 *
 * @pre @p ssid is NUL-terminated within ::k_ra8_c6link_ssid_max + 1 octets.
 * @pre @p pass, when non-null, is NUL-terminated within
 *      ::k_ra8_c6link_pass_max + 1 octets.
 * @post On success both lengths are consistent with the copied strings.
 * @post On failure @p cfg is left cleared rather than half-filled.
 *
 * @note Pure formatting; touches no hardware and is safe from any thread.
 * @warning The passphrase is held in the caller's record in plain text for as
 *          long as the record lives. Clear it once the join has been issued.
 *
 * @par Example:
 * @code
 * (void)ra8_c6link_sta_cfg_set(&sta, "ra8-bench", "correct horse battery");
 * @endcode
 *
 * @see ra8_c6link_wifi_join
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_c6link_sta_cfg_set(ra8_c6link_sta_cfg_t* cfg, const char* ssid, const char* pass);

/**
 * @brief Start the co-processor's Wi-Fi radio in station mode.
 *
 * @details
 * Issues `Req_WifiInit`, `Req_SetWifiMode` for station mode, and
 * `Req_WifiStart`, in that order, stopping at the first one the co-processor
 * refuses. The initialisation configuration transmitted is the ESP-IDF
 * default set for this co-processor, field by field -- see
 * `ra8_c6link_wifi_init_t` in the implementation for what each value is and
 * why it has that value.
 *
 * @param[in,out] link Open handle; must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The radio is up in station mode.
 * @retval k_ra8_err_null_ptr @p link was null.
 * @retval k_ra8_err_not_initialized @p link is not open.
 * @retval k_ra8_err_busy A request is already outstanding on @p link.
 * @retval k_ra8_err_timeout The co-processor did not answer a step.
 * @retval k_ra8_err_hw_timeout The co-processor never armed HANDSHAKE, so no
 *         transaction was clocked.
 * @retval k_ra8_err_protocol_error A step was answered with a failure code;
 *         ::ra8_c6link_last_fault names which.
 * @retval k_ra8_err_spi_error The transport refused a transfer.
 *
 * @pre The transport is up and the co-processor has booted.
 * @pre The radio is not already started; starting twice is refused by the
 *      far side, not by this host.
 * @post On success the co-processor is in station mode with its radio on.
 * @post On failure the handle's last fault names the step that failed.
 *
 * @note Not thread-safe; it pumps.
 *
 * @par Example:
 * @code
 * if (ra8_c6link_wifi_start(&link) != k_ra8_ok) { report(&link); }
 * @endcode
 *
 * @see ra8_c6link_wifi_stop
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 5: two preconditions and two postconditions are checked.
 */
[[nodiscard]] ra8_err_t ra8_c6link_wifi_start(ra8_c6link_t* link);

/**
 * @brief Stop the radio and release the co-processor's Wi-Fi resources.
 *
 * @details
 * Issues `Req_WifiStop` then `Req_WifiDeinit`. Unlike the start sequence this
 * continues past a refusal: a teardown that stops at the first error leaves the
 * co-processor half-configured, which is worse than reporting the first fault
 * and finishing the job.
 *
 * @param[in,out] link Open handle; must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Both steps succeeded.
 * @retval k_ra8_err_null_ptr @p link was null.
 * @retval k_ra8_err_not_initialized @p link is not open.
 * @retval k_ra8_err_busy A request is already outstanding on @p link.
 * @retval k_ra8_err_timeout A step went unanswered; the rest still ran.
 * @retval k_ra8_err_hw_timeout The co-processor never armed HANDSHAKE, so no
 *         transaction was clocked.
 * @retval k_ra8_err_protocol_error A step reported a failure code; the rest
 *         still ran and ::ra8_c6link_last_fault names the first failure.
 * @retval k_ra8_err_spi_error The transport refused a transfer.
 *
 * @pre The transport is up.
 * @pre The caller has stopped transmitting Ethernet frames.
 * @post Every teardown step was attempted.
 * @post On failure the handle's last fault names the first step that failed.
 *
 * @note Not thread-safe; it pumps.
 *
 * @par Example:
 * @code
 * (void)ra8_c6link_wifi_stop(&link);
 * @endcode
 *
 * @see ra8_c6link_wifi_start
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_c6link_wifi_stop(ra8_c6link_t* link);

/**
 * @brief Ask the station to associate with the configured network.
 *
 * @details
 * Issues `Req_WifiSetConfig` for the station interface and then
 * `Req_WifiConnect`. Returns as soon as the co-processor has accepted the
 * request; the association result arrives later as an event.
 *
 * @param[in,out] link Open handle; must be non-null.
 * @param[in] cfg Station configuration; must be non-null and consistent, which
 *                ::ra8_c6link_sta_cfg_set guarantees.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The co-processor accepted the association request.
 * @retval k_ra8_err_null_ptr @p link or @p cfg was null.
 * @retval k_ra8_err_not_initialized @p link is not open.
 * @retval k_ra8_err_invalid_size A length in @p cfg exceeds its field.
 * @retval k_ra8_err_busy A request is already outstanding on @p link.
 * @retval k_ra8_err_timeout The co-processor did not answer a step.
 * @retval k_ra8_err_hw_timeout The co-processor never armed HANDSHAKE, so no
 *         transaction was clocked.
 * @retval k_ra8_err_protocol_error A step reported a failure code.
 * @retval k_ra8_err_spi_error The transport refused a transfer.
 *
 * @pre ::ra8_c6link_wifi_start has succeeded on @p link.
 * @pre @p cfg holds credentials for a network that is in range.
 * @post On success an association attempt is in progress.
 * @post On failure the handle's last fault names the step that failed.
 *
 * @note Not thread-safe; it pumps.
 * @warning Success here means "asked", not "joined". Wait for
 *          ::k_ra8_c6link_event_sta_connected.
 *
 * @par Example:
 * @code
 * (void)ra8_c6link_wifi_join(&link, &sta);
 * @endcode
 *
 * @see ra8_c6link_wifi_leave
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_c6link_wifi_join(ra8_c6link_t* link, const ra8_c6link_sta_cfg_t* cfg);

/**
 * @brief Disassociate the station from its current network.
 *
 * @param[in,out] link Open handle; must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The co-processor accepted the disassociation.
 * @retval k_ra8_err_null_ptr @p link was null.
 * @retval k_ra8_err_not_initialized @p link is not open.
 * @retval k_ra8_err_busy A request is already outstanding on @p link.
 * @retval k_ra8_err_timeout The co-processor did not answer.
 * @retval k_ra8_err_hw_timeout The co-processor never armed HANDSHAKE, so no
 *         transaction was clocked.
 * @retval k_ra8_err_protocol_error The answer reported a failure code.
 * @retval k_ra8_err_spi_error The transport refused a transfer.
 *
 * @pre The transport is up.
 * @pre The caller expects a ::k_ra8_c6link_event_sta_disconnected to follow.
 * @post On success a disassociation is in progress.
 * @post On failure the handle's last fault names this request.
 *
 * @note Not thread-safe; it pumps.
 *
 * @par Example:
 * @code
 * (void)ra8_c6link_wifi_leave(&link);
 * @endcode
 *
 * @see ra8_c6link_wifi_join
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_c6link_wifi_leave(ra8_c6link_t* link);

/**
 * @brief Read the station interface's MAC address.
 *
 * @details
 * The address an IP stack above must use as its own. It is a property of the
 * co-processor's efuses, so it is stable across resets and is available as soon
 * as the radio has been initialised.
 *
 * @param[in,out] link Open handle; must be non-null.
 * @param[out] out Address to fill; must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok @p out holds the station address.
 * @retval k_ra8_err_null_ptr @p link or @p out was null.
 * @retval k_ra8_err_not_initialized @p link is not open.
 * @retval k_ra8_err_busy A request is already outstanding on @p link.
 * @retval k_ra8_err_timeout The co-processor did not answer.
 * @retval k_ra8_err_hw_timeout The co-processor never armed HANDSHAKE, so no
 *         transaction was clocked.
 * @retval k_ra8_err_protocol_error The answer reported a failure, or carried
 *         an address of the wrong length.
 * @retval k_ra8_err_spi_error The transport refused a transfer.
 *
 * @pre ::ra8_c6link_wifi_start has succeeded on @p link.
 * @pre The caller treats an all-zero result as a protocol failure, which this
 *      call reports rather than returns.
 * @post On success @p out holds ::k_ra8_c6link_mac_bytes octets.
 * @post On failure @p out is cleared rather than left half-written.
 *
 * @note Not thread-safe; it pumps.
 *
 * @par Example:
 * @code
 * ra8_c6link_mac_t mac = {};
 * (void)ra8_c6link_wifi_mac(&link, &mac);
 * @endcode
 *
 * @see ra8_c6link_mac_t
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_c6link_wifi_mac(ra8_c6link_t* link, ra8_c6link_mac_t* out);

/**
 * @brief Read what the co-processor knows about the associated AP.
 *
 * @param[in,out] link Open handle; must be non-null.
 * @param[out] out Record to fill; must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok @p out describes the current association.
 * @retval k_ra8_err_null_ptr @p link or @p out was null.
 * @retval k_ra8_err_not_initialized @p link is not open.
 * @retval k_ra8_err_busy A request is already outstanding on @p link.
 * @retval k_ra8_err_timeout The co-processor did not answer.
 * @retval k_ra8_err_hw_timeout The co-processor never armed HANDSHAKE, so no
 *         transaction was clocked.
 * @retval k_ra8_err_protocol_error The answer reported a failure -- which is
 *         what an unassociated station returns -- or carried no record.
 * @retval k_ra8_err_spi_error The transport refused a transfer.
 *
 * @pre The station is associated; asking otherwise is answered with a failure
 *      code by the co-processor.
 * @pre The transport is up.
 * @post On success @p out is fully written.
 * @post On failure @p out is cleared rather than left half-written.
 *
 * @note Not thread-safe; it pumps.
 *
 * @par Example:
 * @code
 * ra8_c6link_ap_info_t ap = {};
 * (void)ra8_c6link_wifi_ap_info(&link, &ap);
 * @endcode
 *
 * @see ra8_c6link_ap_info
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_c6link_wifi_ap_info(ra8_c6link_t* link, ra8_c6link_ap_info_t* out);

#ifdef __cplusplus
}
#endif
