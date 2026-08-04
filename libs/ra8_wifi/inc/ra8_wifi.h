/**
 * @file ra8_wifi.h
 * @brief A small, uniform Wi-Fi facade: init, connect, get an IP, disconnect.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The one place an application does Wi-Fi. Above this header there is no
 * esp-hosted RPC, no protobuf, no TLV envelope, no SPI transaction pump, no
 * interface index and no `wifi_mode_t`: there is a handle you initialise, a
 * network you connect to by SSID and passphrase, and a leased IP you wait for.
 * Everything specific to the radio that provides those operations lives behind
 * ::ra8_wifi_backend_t, a function-pointer seam this facade dispatches through
 * and never looks past.
 *
 * @par Why a vtable and not a direct call into ra8_c6link
 * The same Dependency-Inversion pattern ra8_display_pal uses for panels and
 * ra8_io uses for buses. The facade is a state machine over a backend it does
 * not name: ::ra8_c6link is the first backend (::k_ra8_wifi_backend_c6link in
 * `ra8_wifi_c6link.h`), a second radio would be a second backend, and a host
 * unit test drives the whole facade against a mock backend with no hardware at
 * all. That is why this translation unit includes nothing from ra8_c6link.
 *
 * @par The journey, in the order a caller makes it
 * @code
 * ra8_wifi_init(&wifi, &cfg);            // radio + link up, ready to associate
 * ra8_wifi_connect(&wifi, ssid, psk);    // blocks until the station is joined
 * ra8_wifi_lease_t lease = {};
 * ra8_wifi_wait_ip(&wifi, &lease);       // DHCP -- lease.ip is the address
 * ...                                    // use the socket API from here
 * ra8_wifi_disconnect(&wifi);            // leave and power the radio down
 * ra8_wifi_deinit(&wifi);
 * @endcode
 *
 * @par Where the IP comes from
 * Obtaining a lease is the IP stack's job, not the radio's, so it is not a
 * backend operation. The caller supplies an ::ra8_wifi_ip_bind_fn in the
 * configuration and ::ra8_wifi_wait_ip runs it once the station is associated.
 * That keeps this facade free of any NetX Duo or ThreadX dependency and fully
 * host-testable, while still letting the simple path reach a bound address. A
 * ready-made provider ships alongside the backend when a stack is available.
 *
 * @par Threading
 * A handle is single-threaded, like the link beneath it. Drive it from one
 * bring-up context. Once ::ra8_wifi_wait_ip has bound an address the IP stack
 * owns the wire; do not call ::ra8_wifi_poll after that point.
 *
 * @see ra8_wifi_c6link.h  The ESP32-C6 backend that fills the seam
 * @see ra8_wifi_backend.h  The seam a backend implements
 *
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

/**
 * @enum ra8_wifi_limits_t
 * @brief Fixed field capacities this facade works to.
 *
 * @details
 * Both come from IEEE 802.11 and match the co-processor's own limits, so a
 * value that fits here fits on the wire. They are restated rather than pulled
 * from a backend header so that a consumer of this facade needs no backend
 * include path.
 *
 * @invariant ::k_ra8_wifi_ssid_max is the longest SSID 802.11 allows.
 * @invariant ::k_ra8_wifi_mac_bytes is the octet count of an IEEE 802 address.
 *
 * @par Example:
 * @code
 * static_assert(k_ra8_wifi_ssid_max == 32U, "802.11 SSID bound");
 * @endcode
 *
 * @see ra8_wifi_ap_t
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_wifi_mac_bytes = 6U,  /**< Octets in an IEEE 802 address.     */
  k_ra8_wifi_ssid_max  = 32U, /**< Octets in the longest 802.11 SSID. */
} ra8_wifi_limits_t;

/**
 * @enum ra8_wifi_budget_t
 * @brief Bounds that turn "never associated" into a returned timeout.
 *
 * @details
 * ::ra8_wifi_connect drives association by asking the backend to service the
 * link a bounded number of times. The count exists to give that loop a
 * statically provable bound (NASA Power of 10 Rule 2), and the gap is what
 * turns that count into wall time: a service call costs whatever the radio
 * takes to answer, which on a link that is answering promptly is tens of
 * milliseconds, so a budget of attempts alone would be spent in a couple of
 * seconds -- less than an 802.11 association needs. The pair is the figure the
 * bench proved: two hundred attempts, fifty milliseconds apart.
 *
 * @invariant ::k_ra8_wifi_join_polls is non-zero, so at least one association
 *            attempt is always made.
 * @invariant ::k_ra8_wifi_poll_gap_ms is non-zero, so the wait cannot busy-spin.
 *
 * @par Example:
 * @code
 * for (uint16_t i = 0U; i < (uint16_t)k_ra8_wifi_join_polls; i++) { ... }
 * @endcode
 *
 * @see ra8_wifi_connect
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_wifi_join_polls  = 200U, /**< Times ::ra8_wifi_connect services the link
                                     while waiting for the join to complete. */
  k_ra8_wifi_poll_gap_ms = 50U,  /**< Milliseconds the facade idles between two
                                     association attempts.                   */
} ra8_wifi_budget_t;

/**
 * @enum ra8_wifi_state_t
 * @brief Where a handle is on the path from powered-off to holding an IP.
 *
 * @details
 * A strictly increasing lifecycle: each successful step advances one level, and
 * ::ra8_wifi_disconnect returns it to ::k_ra8_wifi_state_down. It is surfaced by
 * ::ra8_wifi_status so a caller can drive or display progress without inferring
 * it from a chain of return codes.
 *
 * @invariant The values order the lifecycle, so a numeric comparison such as
 *            `state >= k_ra8_wifi_state_associated` is a valid readiness test.
 * @invariant ::k_ra8_wifi_state_ip_bound is reached only through
 *            ::ra8_wifi_wait_ip.
 *
 * @par Example:
 * @code
 * ra8_wifi_status_t st = {};
 * (void)ra8_wifi_status(&wifi, &st);
 * if (st.state >= k_ra8_wifi_state_associated) { have_link(); }
 * @endcode
 *
 * @see ra8_wifi_status
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_wifi_state_down        = 0U, /**< Not associated; radio may be off. */
  k_ra8_wifi_state_associating = 1U, /**< A join has been asked for.        */
  k_ra8_wifi_state_associated  = 2U, /**< Station is joined; no IP yet.     */
  k_ra8_wifi_state_ip_bound    = 3U, /**< A DHCP lease is held.             */
} ra8_wifi_state_t;

/**
 * @enum ra8_wifi_link_t
 * @brief The instantaneous association state a backend reports.
 *
 * @details
 * Narrower than ::ra8_wifi_state_t: a backend knows only whether the station is
 * currently associated, and the facade folds that into the lifecycle. Returned
 * by ::ra8_wifi_poll and used internally by ::ra8_wifi_connect.
 *
 * @invariant ::k_ra8_wifi_link_up means the station is associated right now, not
 *            that it once was.
 * @invariant A backend never reports a value outside this enum.
 *
 * @par Example:
 * @code
 * ra8_wifi_link_t link = k_ra8_wifi_link_down;
 * (void)ra8_wifi_poll(&wifi, &link);
 * @endcode
 *
 * @see ra8_wifi_poll
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_wifi_link_down = 0U, /**< Station is not associated.        */
  k_ra8_wifi_link_up   = 1U, /**< Station is associated with an AP. */
} ra8_wifi_link_t;

/**
 * @struct ra8_wifi_mac
 * @brief One IEEE 802 address, by value.
 *
 * @details
 * A named type rather than a bare `uint8_t[6]` so it can be returned and copied
 * without an array-decay parameter that loses its length. It is what an IP
 * stack must use as its own hardware address.
 *
 * @invariant Exactly ::k_ra8_wifi_mac_bytes octets, in transmission order.
 * @invariant An all-zero value is never a real station address, so it doubles
 *            as the "not yet read" sentinel.
 *
 * @par Example:
 * @code
 * ra8_wifi_mac_t mac = {};
 * (void)ra8_wifi_get_mac(&wifi, &mac);
 * @endcode
 *
 * @see ra8_wifi_get_mac
 * @since 0.1.0
 */
typedef struct ra8_wifi_mac {
  uint8_t octet[k_ra8_wifi_mac_bytes]; /**< Address octets, first on the wire first. */
} ra8_wifi_mac_t;

/**
 * @struct ra8_wifi_lease
 * @brief The IPv4 configuration an ::ra8_wifi_ip_bind_fn obtained.
 *
 * @details
 * Filled by ::ra8_wifi_wait_ip from the caller-supplied IP provider and cached
 * on the handle for ::ra8_wifi_get_ip. Every address is host byte order.
 *
 * @invariant `bound` is true exactly when `ip` is a usable, non-zero address.
 * @invariant When `bound` is false every address field is zero.
 *
 * @par Example:
 * @code
 * ra8_wifi_lease_t lease = {};
 * if (ra8_wifi_wait_ip(&wifi, &lease) == k_ra8_ok) { use(lease.ip); }
 * @endcode
 *
 * @see ra8_wifi_wait_ip
 * @since 0.1.0
 */
typedef struct ra8_wifi_lease {
  uint32_t ip;          /**< Leased station address, or zero if unbound. */
  uint32_t mask;        /**< Subnet mask from the lease.                 */
  uint32_t gateway;     /**< Default gateway from the lease.             */
  uint32_t dhcp_server; /**< Address of the DHCP server that answered.   */
  bool     bound;       /**< A usable lease is held.                     */
} ra8_wifi_lease_t;

/**
 * @struct ra8_wifi_ap
 * @brief What the backend knows about the AP the station is on.
 *
 * @details
 * Filled by ::ra8_wifi_get_ap from the radio's own association record. Useful
 * as a post-connect sanity check: the BSSID and channel here are what the radio
 * actually settled on.
 *
 * @invariant `ssid_len` never exceeds ::k_ra8_wifi_ssid_max and `ssid` is
 *            NUL-terminated at it.
 * @invariant `rssi` is in dBm and is negative for any real association.
 *
 * @par Example:
 * @code
 * ra8_wifi_ap_t ap = {};
 * if (ra8_wifi_get_ap(&wifi, &ap) == k_ra8_ok) { use(ap.rssi); }
 * @endcode
 *
 * @see ra8_wifi_get_ap
 * @since 0.1.0
 */
typedef struct ra8_wifi_ap {
  ra8_wifi_mac_t bssid;                          /**< Address of the associated AP.     */
  char           ssid[k_ra8_wifi_ssid_max + 1U]; /**< Its SSID, NUL-terminated.         */
  uint8_t        ssid_len;                       /**< Octets of `ssid`.                 */
  uint8_t        channel;                        /**< Primary channel in use.           */
  int8_t         rssi;                           /**< Signal level in dBm.              */
  int32_t        authmode;                       /**< Backend authentication-mode code. */
} ra8_wifi_ap_t;

/**
 * @struct ra8_wifi_status
 * @brief A single snapshot of where a handle is, without touching the wire.
 *
 * @details
 * Every field is a cached value the facade already holds, so reading status is
 * side-effect-free and safe to call as often as a display refresh wants. Call
 * ::ra8_wifi_poll first when a fresher association reading is needed.
 *
 * @invariant `associated` is true exactly when `state` is at least
 *            ::k_ra8_wifi_state_associated.
 * @invariant `ip_bound` is true exactly when `state` is
 *            ::k_ra8_wifi_state_ip_bound.
 *
 * @par Example:
 * @code
 * ra8_wifi_status_t st = {};
 * (void)ra8_wifi_status(&wifi, &st);
 * @endcode
 *
 * @see ra8_wifi_status
 * @since 0.1.0
 */
typedef struct ra8_wifi_status {
  ra8_wifi_state_t state;      /**< Lifecycle position.                       */
  bool             associated; /**< The station is joined.                    */
  bool             ip_bound;   /**< A DHCP lease is held.                     */
  int8_t           rssi;       /**< Last known signal level in dBm, or zero.  */
  ra8_wifi_lease_t ip;         /**< The current lease, valid when `ip_bound`. */
} ra8_wifi_status_t;

/**
 * @typedef ra8_wifi_ip_bind_fn
 * @brief Caller-supplied hook that turns an associated station into a lease.
 *
 * @details
 * The seam between this radio facade and whatever IP stack the application
 * runs. ::ra8_wifi_wait_ip invokes it once the station is associated, hands it
 * the station's MAC, and caches the lease it returns. Keeping it a caller hook
 * is what lets this facade stay free of any particular stack.
 *
 * @param[in] ip_ctx Opaque context from ::ra8_wifi_cfg::ip_ctx.
 * @param[in] mac Station MAC the stack must adopt as its own; never null.
 * @param[out] out Lease to fill; never null. Leave `bound` false on failure.
 *
 * @return ra8_err_t ::k_ra8_ok when @p out holds a usable lease, otherwise an
 *         error the provider chooses.
 *
 * @note Runs on the caller's thread inside ::ra8_wifi_wait_ip. It may block for
 *       as long as obtaining a lease takes.
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_wifi_ip_bind_fn)(void*                 ip_ctx,
                                         const ra8_wifi_mac_t* mac,
                                         ra8_wifi_lease_t*     out);

/**
 * @typedef ra8_wifi_backend_t
 * @brief One radio's implementation of the operations this facade dispatches.
 *
 * @details
 * Opaque here on purpose: an application selects a backend by taking the
 * address of one a driver exports (for example ::k_ra8_wifi_backend_c6link) and
 * never constructs or inspects the table. The full layout, for backend authors
 * and tests, is in `ra8_wifi_backend.h`.
 *
 * @see ra8_wifi_backend.h
 * @see ra8_wifi_c6link.h
 * @since 0.1.0
 */
typedef struct ra8_wifi_backend ra8_wifi_backend_t;

/**
 * @struct ra8_wifi_cfg
 * @brief Everything ::ra8_wifi_init needs that the facade cannot discover.
 *
 * @details
 * The backend and its context select and drive the radio; the IP hook and its
 * context obtain a lease. All four are required: a facade with a half-wired
 * configuration would fail deep in a call rather than at init, which is the
 * wrong place to find out.
 *
 * @invariant `backend` points at a table with every function pointer set.
 * @invariant `ip_bind` is non-null; ::ra8_wifi_wait_ip has nothing to call
 *            otherwise.
 *
 * @par Example:
 * @code
 * ra8_wifi_cfg_t cfg = {};
 * (void)ra8_wifi_c6link_setup(&c6, &bcfg, &cfg); // fills backend + backend_ctx
 * cfg.ip_bind = my_dhcp;
 * cfg.ip_ctx  = &s_link;
 * @endcode
 *
 * @see ra8_wifi_init
 * @since 0.1.0
 */
typedef struct ra8_wifi_cfg {
  const ra8_wifi_backend_t* backend;     /**< Radio operations; every row set.      */
  void*                     backend_ctx; /**< Opaque state handed to the backend.   */
  ra8_wifi_ip_bind_fn       ip_bind;     /**< IP-stack hook for ::ra8_wifi_wait_ip. */
  void*                     ip_ctx;      /**< Opaque context handed to `ip_bind`.   */
} ra8_wifi_cfg_t;

/**
 * @struct ra8_wifi
 * @brief Caller-allocated Wi-Fi handle. Treat every field as private.
 *
 * @details
 * Zero-initialise (`= {}`) and pass to ::ra8_wifi_init. It holds only the bound
 * configuration and a little cached state, so it is small enough for file scope
 * and carries no buffers of its own -- the backend owns those.
 *
 * @invariant `open` is true exactly between a successful ::ra8_wifi_init and the
 *            matching ::ra8_wifi_deinit.
 * @invariant `state` never exceeds ::k_ra8_wifi_state_ip_bound.
 *
 * @par Example:
 * @code
 * static ra8_wifi_t s_wifi;
 * @endcode
 *
 * @see ra8_wifi_init
 * @since 0.1.0
 */
typedef struct ra8_wifi {
  const ra8_wifi_backend_t* backend;     /**< Bound radio operations.     */
  void*                     backend_ctx; /**< Backend's opaque state.     */
  ra8_wifi_ip_bind_fn       ip_bind;     /**< Bound IP-stack hook.        */
  void*                     ip_ctx;      /**< Context for `ip_bind`.      */
  ra8_wifi_mac_t            mac;         /**< Cached station address.     */
  ra8_wifi_lease_t          lease;       /**< Cached DHCP lease.          */
  ra8_wifi_state_t          state;       /**< Lifecycle position.         */
  int8_t                    rssi;        /**< Last signal reading, dBm.   */
  bool                      open;        /**< The handle is initialised.  */
  bool                      radio_on;    /**< The radio has been started. */
  bool                      mac_valid;   /**< `mac` has been read.        */
} ra8_wifi_t;

/**
 * @brief Bring the radio and its link up and make a handle usable.
 *
 * @details
 * Validates the configuration, copies it into the handle, and asks the backend
 * to open: bind its transport, open its link, and prove the radio answers. No
 * network is joined here -- that is ::ra8_wifi_connect.
 *
 * @param[out] wifi Handle to initialise; must be non-null.
 * @param[in] cfg Configuration; must be non-null with a complete backend table
 *                and an `ip_bind` hook.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The handle is open and the radio is answering.
 * @retval k_ra8_err_null_ptr @p wifi, @p cfg, `cfg->backend`, a backend row, or
 *         `cfg->ip_bind` was null.
 * @retval k_ra8_err_invalid_state @p wifi is already open.
 * @retval k_ra8_err_timeout The backend could not prove the radio is answering.
 * @retval k_ra8_err_hw_timeout The radio never armed its handshake.
 * @retval k_ra8_err_spi_error The backend's transport refused a transfer.
 *
 * @pre The backend's hardware bring-up (clocks, pins, bus) has already run.
 * @pre @p wifi is zero-initialised, or has been deinitialised.
 * @post On success the handle reports open and ::k_ra8_wifi_state_down.
 * @post On failure @p wifi is left closed rather than half-open.
 *
 * @note Not thread-safe; initialise once from a single-threaded bring-up path.
 * @warning Everything the configuration points at must outlive the handle.
 *
 * @par Example:
 * @code
 * if (ra8_wifi_init(&wifi, &cfg) != k_ra8_ok) { report(); }
 * @endcode
 *
 * @see ra8_wifi_deinit
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 5: preconditions on every configuration row and two postconditions.
 */
[[nodiscard]] ra8_err_t ra8_wifi_init(ra8_wifi_t* wifi, const ra8_wifi_cfg_t* cfg);

/**
 * @brief Release a handle and power its radio down.
 *
 * @details
 * Asks the backend to close its link and drop the transport binding, then marks
 * the handle closed. Safe to call from any state; a handle that never
 * associated closes just as cleanly as one that held a lease.
 *
 * @param[in,out] wifi Open handle; must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The handle is closed.
 * @retval k_ra8_err_null_ptr @p wifi was null.
 * @retval k_ra8_err_not_initialized @p wifi was not open.
 *
 * @pre No other context is driving @p wifi.
 * @pre The caller no longer needs the cached lease or status.
 * @post The handle reports closed.
 * @post The backend has released its link.
 *
 * @note Not thread-safe; close from the context that opened.
 *
 * @par Example:
 * @code
 * (void)ra8_wifi_deinit(&wifi);
 * @endcode
 *
 * @see ra8_wifi_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wifi_deinit(ra8_wifi_t* wifi);

/**
 * @brief Join a network by SSID and passphrase, blocking until associated.
 *
 * @details
 * Starts the radio if it is not already on, reads and caches the station MAC,
 * asks the backend to associate, and services the link until the station joins
 * or the attempt budget (::k_ra8_wifi_join_polls) is spent, idling
 * ::k_ra8_wifi_poll_gap_ms between attempts. On success the handle is at
 * ::k_ra8_wifi_state_associated and ::ra8_wifi_wait_ip is the next step.
 *
 * @par A quiet link is not a failed one
 * While an association is in flight the radio routinely has nothing to say, and
 * a backend is entitled to report that as an error -- ::k_ra8_wifi_backend_c6link
 * returns ::k_ra8_err_hw_timeout when the co-processor does not arm its
 * handshake line. Such a reading ends the attempt, never the wait: the loop
 * keeps servicing until the budget is spent, and only surfaces a service error
 * when *no* attempt in the whole budget succeeded, which is the reading that
 * really does mean the radio is gone.
 *
 * @param[in,out] wifi Open handle; must be non-null.
 * @param[in] ssid Target SSID, NUL-terminated; must be non-null and 1..
 *                 ::k_ra8_wifi_ssid_max octets.
 * @param[in] psk Passphrase, NUL-terminated, or null for an open network.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The station is associated.
 * @retval k_ra8_err_null_ptr @p wifi or @p ssid was null.
 * @retval k_ra8_err_not_initialized @p wifi is not open.
 * @retval k_ra8_err_invalid_size @p ssid was empty or a credential was too long.
 * @retval k_ra8_err_timeout The join did not complete within the budget.
 * @retval k_ra8_err_protocol_error The radio refused a step of the join.
 * @retval k_ra8_err_spi_error The backend's transport refused every transfer of
 *         the whole budget.
 * @retval k_ra8_err_hw_timeout The radio answered no attempt of the whole budget.
 *
 * @pre @p wifi has been initialised.
 * @pre @p ssid names a network that is in range.
 * @post On success the handle reports ::k_ra8_wifi_state_associated.
 * @post On failure the handle is not left reporting associated.
 *
 * @note Not thread-safe; it drives the link.
 * @warning Blocks for as long as association takes, up to the poll budget.
 *
 * @par Example:
 * @code
 * (void)ra8_wifi_connect(&wifi, "ra8-bench", secret);
 * @endcode
 *
 * @see ra8_wifi_wait_ip
 * @see ra8_wifi_disconnect
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 2: the association wait is bounded by ::k_ra8_wifi_join_polls.
 * - Rule 5: two preconditions and two postconditions are checked.
 */
[[nodiscard]] ra8_err_t ra8_wifi_connect(ra8_wifi_t* wifi, const char* ssid, const char* psk);

/**
 * @brief Disassociate the station and power the radio down.
 *
 * @details
 * Asks the backend to leave the network and stop the radio, clears the cached
 * lease, and returns the handle to ::k_ra8_wifi_state_down. The handle stays
 * open and can associate again with a fresh ::ra8_wifi_connect.
 *
 * @param[in,out] wifi Open handle; must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The station has left and the radio is stopped.
 * @retval k_ra8_err_null_ptr @p wifi was null.
 * @retval k_ra8_err_not_initialized @p wifi is not open.
 * @retval k_ra8_err_protocol_error A teardown step was refused; the rest still
 *         ran.
 * @retval k_ra8_err_spi_error The backend's transport refused a transfer.
 *
 * @pre @p wifi has been initialised.
 * @pre The caller has stopped using the IP stack over this link.
 * @post The handle reports ::k_ra8_wifi_state_down.
 * @post The cached lease is cleared.
 *
 * @note Not thread-safe; it drives the link.
 *
 * @par Example:
 * @code
 * (void)ra8_wifi_disconnect(&wifi);
 * @endcode
 *
 * @see ra8_wifi_connect
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wifi_disconnect(ra8_wifi_t* wifi);

/**
 * @brief Obtain an IP address for the associated station, blocking until bound.
 *
 * @details
 * Runs the configured ::ra8_wifi_ip_bind_fn with the station MAC, caches the
 * lease it returns, and advances the handle to ::k_ra8_wifi_state_ip_bound. The
 * provider owns the mechanism (DHCP today) and the wait; from here the caller
 * uses the IP stack's socket API and must not call ::ra8_wifi_poll.
 *
 * @param[in,out] wifi Open, associated handle; must be non-null.
 * @param[out] out Lease to fill; must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok @p out holds a usable lease.
 * @retval k_ra8_err_null_ptr @p wifi or @p out was null.
 * @retval k_ra8_err_not_initialized @p wifi is not open.
 * @retval k_ra8_err_invalid_state The station is not associated.
 * @retval k_ra8_err_timeout The provider did not obtain a lease.
 *
 * @pre ::ra8_wifi_connect has succeeded on @p wifi.
 * @pre The configured IP provider can reach a DHCP server over the link.
 * @post On success the handle reports ::k_ra8_wifi_state_ip_bound.
 * @post On failure @p out is cleared rather than left half-written.
 *
 * @note Not thread-safe; it drives the IP provider.
 * @warning Blocks for as long as the provider takes to obtain a lease.
 *
 * @par Example:
 * @code
 * ra8_wifi_lease_t lease = {};
 * (void)ra8_wifi_wait_ip(&wifi, &lease);
 * @endcode
 *
 * @see ra8_wifi_get_ip
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 5: two preconditions and two postconditions are checked.
 */
[[nodiscard]] ra8_err_t ra8_wifi_wait_ip(ra8_wifi_t* wifi, ra8_wifi_lease_t* out);

/**
 * @brief Read the cached DHCP lease without touching the wire.
 *
 * @details
 * Returns the lease ::ra8_wifi_wait_ip last obtained, straight from the handle.
 * It performs no I/O, so it is the cheap way for an application to read back its
 * address after binding; `out->bound` distinguishes a real lease from none.
 *
 * @param[in] wifi Open handle; must be non-null.
 * @param[out] out Lease to fill; must be non-null. `bound` is false when no
 *                 lease has been obtained.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok @p out holds the cached lease.
 * @retval k_ra8_err_null_ptr @p wifi or @p out was null.
 * @retval k_ra8_err_not_initialized @p wifi is not open.
 *
 * @pre @p wifi has been initialised.
 * @pre The caller reads `out->bound` before trusting the addresses.
 * @post No handle state is modified.
 * @post @p out is fully written, including on the unbound path.
 *
 * @note Safe from any context; it copies cached fields.
 *
 * @par Example:
 * @code
 * ra8_wifi_lease_t lease = {};
 * (void)ra8_wifi_get_ip(&wifi, &lease);
 * @endcode
 *
 * @see ra8_wifi_wait_ip
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wifi_get_ip(const ra8_wifi_t* wifi, ra8_wifi_lease_t* out);

/**
 * @brief Read a handle's cached status without touching the wire.
 *
 * @details
 * Fills @p out from state the handle already holds -- lifecycle position, the
 * associated and IP-bound flags derived from it, the last signal reading and
 * the current lease. It performs no I/O and is safe to poll for a display; call
 * ::ra8_wifi_poll first when a fresher association reading is needed.
 *
 * @param[in] wifi Open handle; must be non-null.
 * @param[out] out Status to fill; must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok @p out describes the handle.
 * @retval k_ra8_err_null_ptr @p wifi or @p out was null.
 * @retval k_ra8_err_not_initialized @p wifi is not open.
 *
 * @pre @p wifi has been initialised.
 * @pre The caller has called ::ra8_wifi_poll first if a fresh reading matters.
 * @post No handle state is modified.
 * @post @p out is fully written.
 *
 * @note Safe from any context; it copies cached fields.
 *
 * @par Example:
 * @code
 * ra8_wifi_status_t st = {};
 * (void)ra8_wifi_status(&wifi, &st);
 * @endcode
 *
 * @see ra8_wifi_state_t
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wifi_status(const ra8_wifi_t* wifi, ra8_wifi_status_t* out);

/**
 * @brief Service the link once and refresh the cached association state.
 *
 * @details
 * Drives the backend for one service cycle -- draining events, noticing a
 * disconnection -- and folds the result into the handle's lifecycle. Use it to
 * watch a link that is associated but has not yet obtained an IP; it must not be
 * called once ::ra8_wifi_wait_ip has bound an address, because from that point
 * the IP stack owns the wire.
 *
 * @param[in,out] wifi Open handle; must be non-null.
 * @param[out] out Association state after the cycle; must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The link was serviced; @p out holds the reading.
 * @retval k_ra8_err_null_ptr @p wifi or @p out was null.
 * @retval k_ra8_err_not_initialized @p wifi is not open.
 * @retval k_ra8_err_invalid_state @p wifi already holds an IP; the IP stack owns
 *         the wire and must not be pre-empted.
 * @retval k_ra8_err_hw_timeout The radio never armed its handshake.
 * @retval k_ra8_err_spi_error The backend's transport refused a transfer.
 *
 * @pre @p wifi has been initialised and has not bound an IP.
 * @pre No IP-stack thread is driving the same link.
 * @post The cached state reflects the reading just taken.
 * @post At most one service cycle was run.
 *
 * @note Not thread-safe; it drives the link.
 *
 * @par Example:
 * @code
 * ra8_wifi_link_t link = k_ra8_wifi_link_down;
 * (void)ra8_wifi_poll(&wifi, &link);
 * @endcode
 *
 * @see ra8_wifi_connect
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 5: two preconditions and two postconditions are checked.
 */
[[nodiscard]] ra8_err_t ra8_wifi_poll(ra8_wifi_t* wifi, ra8_wifi_link_t* out);

/**
 * @brief Read the station's own MAC address.
 *
 * @details
 * Asks the backend for the station address, caches it on the handle and copies
 * it out. The address is a property of the radio's efuses, so it is stable
 * across resets and available once the radio has been started.
 *
 * That stability is also the fallback: when the backend cannot be asked -- an
 * associated station shares its link with the traffic the AP has begun
 * forwarding, and a query can lose that race -- the cached address ::ra8_wifi_connect
 * already read is returned instead. Only a handle that has never held a valid
 * address reports the failure, because only then is there no answer to give.
 *
 * @param[in,out] wifi Open handle; must be non-null.
 * @param[out] out Address to fill; must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok @p out holds the station address, freshly read or cached.
 * @retval k_ra8_err_null_ptr @p wifi or @p out was null.
 * @retval k_ra8_err_not_initialized @p wifi is not open.
 * @retval k_ra8_err_protocol_error The backend reported no valid address and no
 *         address has ever been cached.
 * @retval k_ra8_err_spi_error The backend's transport refused a transfer and no
 *         address has ever been cached.
 *
 * @pre @p wifi has been initialised.
 * @pre The radio has been started at least once, which ::ra8_wifi_connect does.
 * @post On success @p out holds ::k_ra8_wifi_mac_bytes octets.
 * @post On failure @p out is cleared rather than left half-written.
 *
 * @note Not thread-safe; it may drive the link.
 *
 * @par Example:
 * @code
 * ra8_wifi_mac_t mac = {};
 * (void)ra8_wifi_get_mac(&wifi, &mac);
 * @endcode
 *
 * @see ra8_wifi_mac_t
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wifi_get_mac(ra8_wifi_t* wifi, ra8_wifi_mac_t* out);

/**
 * @brief Read what the backend knows about the associated AP.
 *
 * @details
 * Asks the backend for the co-processor's own association record -- BSSID,
 * SSID, channel, signal level and auth mode -- and caches the signal level for
 * ::ra8_wifi_status. A useful post-connect sanity check on what the radio
 * actually settled on.
 *
 * @param[in,out] wifi Open handle; must be non-null.
 * @param[out] out Record to fill; must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok @p out describes the current association.
 * @retval k_ra8_err_null_ptr @p wifi or @p out was null.
 * @retval k_ra8_err_not_initialized @p wifi is not open.
 * @retval k_ra8_err_invalid_state The station is not associated.
 * @retval k_ra8_err_protocol_error The backend reported no AP record.
 * @retval k_ra8_err_spi_error The backend's transport refused a transfer.
 *
 * @pre @p wifi is associated; asking otherwise is refused.
 * @pre @p wifi has been initialised.
 * @post On success @p out is fully written and `rssi` is cached for status.
 * @post On failure @p out is cleared rather than left half-written.
 *
 * @note Not thread-safe; it may drive the link.
 *
 * @par Example:
 * @code
 * ra8_wifi_ap_t ap = {};
 * (void)ra8_wifi_get_ap(&wifi, &ap);
 * @endcode
 *
 * @see ra8_wifi_ap_t
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wifi_get_ap(ra8_wifi_t* wifi, ra8_wifi_ap_t* out);

#ifdef __cplusplus
}
#endif
