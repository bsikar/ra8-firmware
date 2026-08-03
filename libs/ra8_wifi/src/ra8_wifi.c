/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie */
/**
 * @file ra8_wifi.c
 * @brief The Wi-Fi facade state machine, dispatched over a backend vtable.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Implements `ra8_wifi.h`. Every entry point is a short step in the lifecycle
 * down->associating->associated->ip_bound, expressed purely in terms of the
 * ::ra8_wifi_backend_t function pointers and the caller-supplied
 * ::ra8_wifi_ip_bind_fn. This translation unit names no radio and includes no
 * ra8_c6link header, which is exactly what makes it host-testable against a mock
 * backend.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 *
 * @since 0.1.0
 */

#include "ra8_wifi.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_wifi_backend.h"

/** @brief Component tag for this facade's log lines. */
#define RA8_WIFI_TAG "WIFI"

/**
 * @brief Validate the backend rows that change the radio's state.
 *
 * @details
 * The lifecycle half of ::ra8_wifi_backend_t -- opening and closing the
 * transport and raising or lowering the radio. The table is validated in three
 * role-sized pieces rather than one function carrying ten expansions of
 * ::RA8_CHECK_NULL_PTR, which is what pushed it past the complexity budget.
 *
 * @param[in] b Candidate backend table; never null (the caller checked).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Every lifecycle row is set.
 * @retval k_ra8_err_null_ptr One of the lifecycle rows was null.
 *
 * @pre @p b is non-null.
 * @pre The caller treats any failure as a fatal configuration error.
 * @post No state is modified.
 * @post On success every lifecycle row is safe to dispatch through.
 *
 * @note Pure validation; touches no state and is safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t ra8_wifi_check_backend_lifecycle(const ra8_wifi_backend_t* b)
{
  RA8_CHECK_NULL_PTR(b, RA8_WIFI_TAG, "backend");
  RA8_CHECK_NULL_PTR(b->open, RA8_WIFI_TAG, "backend.open");
  RA8_CHECK_NULL_PTR(b->close, RA8_WIFI_TAG, "backend.close");
  RA8_CHECK_NULL_PTR(b->radio_up, RA8_WIFI_TAG, "backend.radio_up");
  RA8_CHECK_NULL_PTR(b->radio_down, RA8_WIFI_TAG, "backend.radio_down");
  return k_ra8_ok;
}

/**
 * @brief Validate the backend rows that join and leave a network.
 *
 * @details
 * The session half of ::ra8_wifi_backend_t. Separate from the lifecycle rows
 * because a radio can be up with no network attached, so these are the rows a
 * reconnect exercises without touching the transport.
 *
 * @param[in] b Candidate backend table; never null (the caller checked).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Both session rows are set.
 * @retval k_ra8_err_null_ptr One of the session rows was null.
 *
 * @pre @p b is non-null.
 * @pre The caller treats any failure as a fatal configuration error.
 * @post No state is modified.
 * @post On success both session rows are safe to dispatch through.
 *
 * @note Pure validation; touches no state and is safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t ra8_wifi_check_backend_session(const ra8_wifi_backend_t* b)
{
  RA8_CHECK_NULL_PTR(b, RA8_WIFI_TAG, "backend");
  RA8_CHECK_NULL_PTR(b->join, RA8_WIFI_TAG, "backend.join");
  RA8_CHECK_NULL_PTR(b->leave, RA8_WIFI_TAG, "backend.leave");
  return k_ra8_ok;
}

/**
 * @brief Validate the backend rows that only report state.
 *
 * @details
 * The query half of ::ra8_wifi_backend_t -- pumping the transport and reading
 * back the station address and the associated AP. None of these change the
 * radio, which is why they are validated apart from the lifecycle rows.
 *
 * @param[in] b Candidate backend table; never null (the caller checked).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Every query row is set.
 * @retval k_ra8_err_null_ptr One of the query rows was null.
 *
 * @pre @p b is non-null.
 * @pre The caller treats any failure as a fatal configuration error.
 * @post No state is modified.
 * @post On success every query row is safe to dispatch through.
 *
 * @note Pure validation; touches no state and is safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t ra8_wifi_check_backend_query(const ra8_wifi_backend_t* b)
{
  RA8_CHECK_NULL_PTR(b, RA8_WIFI_TAG, "backend");
  RA8_CHECK_NULL_PTR(b->service, RA8_WIFI_TAG, "backend.service");
  RA8_CHECK_NULL_PTR(b->get_mac, RA8_WIFI_TAG, "backend.get_mac");
  RA8_CHECK_NULL_PTR(b->get_ap, RA8_WIFI_TAG, "backend.get_ap");
  return k_ra8_ok;
}

/**
 * @brief Validate every row of a candidate backend table.
 *
 * @details
 * The single entry point ::ra8_wifi_init uses. It runs the three role-sized
 * validators in turn and stops at the first gap, so the error names the row
 * that is missing rather than reporting only that "the table is wrong".
 *
 * @param[in] b Candidate backend table; may be null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Every function pointer is set.
 * @retval k_ra8_err_null_ptr @p b or one of its rows was null.
 *
 * @pre @p b is the table an application selected by address.
 * @pre The caller treats any failure as a fatal configuration error.
 * @post No state is modified.
 * @post On success the table is safe to dispatch through.
 *
 * @note Pure validation; touches no state and is safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t ra8_wifi_check_backend(const ra8_wifi_backend_t* b)
{
  const ra8_err_t lifecycle = ra8_wifi_check_backend_lifecycle(b);
  if (lifecycle != k_ra8_ok) {
    return lifecycle;
  }
  const ra8_err_t session = ra8_wifi_check_backend_session(b);
  if (session != k_ra8_ok) {
    return session;
  }
  return ra8_wifi_check_backend_query(b);
}

ra8_err_t ra8_wifi_init(ra8_wifi_t* wifi, const ra8_wifi_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(wifi, RA8_WIFI_TAG, "wifi");
  RA8_CHECK_NULL_PTR(cfg, RA8_WIFI_TAG, "cfg");
  RA8_CHECK_NULL_PTR(cfg->ip_bind, RA8_WIFI_TAG, "cfg.ip_bind");

  const ra8_err_t table = ra8_wifi_check_backend(cfg->backend);
  if (table != k_ra8_ok) {
    return table;
  }
  if (wifi->open) {
    return k_ra8_err_invalid_state;
  }

  const ra8_err_t opened = cfg->backend->open(cfg->backend_ctx);
  if (opened != k_ra8_ok) {
    return opened;
  }

  *wifi             = (ra8_wifi_t){};
  wifi->backend     = cfg->backend;
  wifi->backend_ctx = cfg->backend_ctx;
  wifi->ip_bind     = cfg->ip_bind;
  wifi->ip_ctx      = cfg->ip_ctx;
  wifi->state       = k_ra8_wifi_state_down;
  wifi->open        = true;
  return k_ra8_ok;
}

ra8_err_t ra8_wifi_deinit(ra8_wifi_t* wifi)
{
  RA8_CHECK_NULL_PTR(wifi, RA8_WIFI_TAG, "wifi");
  if (!wifi->open) {
    return k_ra8_err_not_initialized;
  }

  const ra8_err_t closed = wifi->backend->close(wifi->backend_ctx);
  wifi->open             = false;
  wifi->radio_on         = false;
  wifi->state            = k_ra8_wifi_state_down;
  return closed;
}

/**
 * @brief Raise the radio if it is not already up.
 *
 * @details
 * Idempotent by design: ::ra8_wifi_connect may be called repeatedly (a retry
 * after a failed join, a move to another SSID) and must not cycle a radio that
 * is already running, because a backend is entitled to treat a second
 * ``radio_up`` as an error.
 *
 * @param[in,out] wifi Open handle whose radio should be running on return.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The radio is up, whether or not this call raised it.
 * @retval k_ra8_err_null_ptr @p wifi was null.
 *
 * @pre @p wifi is open (::ra8_wifi_init succeeded).
 * @pre The backend table has been validated.
 * @post On success ``wifi->radio_on`` is true.
 * @post On failure no handle field has changed.
 *
 * @note Not thread-safe; one handle belongs to one caller.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t ra8_wifi_ensure_radio_up(ra8_wifi_t* wifi)
{
  RA8_CHECK_NULL_PTR(wifi, RA8_WIFI_TAG, "wifi");
  if (wifi->radio_on) {
    return k_ra8_ok;
  }
  const ra8_err_t up = wifi->backend->radio_up(wifi->backend_ctx);
  if (up != k_ra8_ok) {
    return up;
  }
  wifi->radio_on = true;
  return k_ra8_ok;
}

/**
 * @brief Pump the backend until the station associates or the budget runs out.
 *
 * @details
 * A join request only asks; association completes asynchronously, so the
 * transport has to be serviced until the link reports up. The poll count is a
 * compile-time bound (::k_ra8_wifi_join_polls), so this terminates whether or
 * not the AP ever answers -- NASA Power of 10 Rule 2.
 *
 * @param[in,out] wifi Open handle with a join already requested.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The link came up; the handle is in the associated state.
 * @retval k_ra8_err_timeout The budget was exhausted with the link still down.
 * @retval k_ra8_err_null_ptr @p wifi was null.
 *
 * @pre ::ra8_wifi_connect has already issued the backend join.
 * @pre @p wifi is open and its radio is up.
 * @post On success ``wifi->state`` is ::k_ra8_wifi_state_associated.
 * @post On timeout the handle is left in the associating state for a retry.
 *
 * @note Not thread-safe; one handle belongs to one caller.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_BOUNDED_LOOP(k_ra8_wifi_join_polls)
static ra8_err_t ra8_wifi_await_association(ra8_wifi_t* wifi)
{
  RA8_CHECK_NULL_PTR(wifi, RA8_WIFI_TAG, "wifi");
  for (uint16_t i = 0U; i < (uint16_t)k_ra8_wifi_join_polls; i++) {
    ra8_wifi_link_t link     = k_ra8_wifi_link_down;
    const ra8_err_t serviced = wifi->backend->service(wifi->backend_ctx, &link);
    if (serviced != k_ra8_ok) {
      return serviced;
    }
    if (link == k_ra8_wifi_link_up) {
      wifi->state = k_ra8_wifi_state_associated;
      return k_ra8_ok;
    }
  }
  return k_ra8_err_timeout;
}

ra8_err_t ra8_wifi_connect(ra8_wifi_t* wifi, const char* ssid, const char* psk)
{
  RA8_CHECK_NULL_PTR(wifi, RA8_WIFI_TAG, "wifi");
  RA8_CHECK_NULL_PTR(ssid, RA8_WIFI_TAG, "ssid");
  if (!wifi->open) {
    return k_ra8_err_not_initialized;
  }

  const ra8_err_t powered = ra8_wifi_ensure_radio_up(wifi);
  if (powered != k_ra8_ok) {
    return powered;
  }

  const ra8_err_t got_mac = wifi->backend->get_mac(wifi->backend_ctx, &wifi->mac);
  if (got_mac != k_ra8_ok) {
    return got_mac;
  }
  wifi->mac_valid = true;

  wifi->state           = k_ra8_wifi_state_associating;
  const ra8_err_t asked = wifi->backend->join(wifi->backend_ctx, ssid, psk);
  if (asked != k_ra8_ok) {
    return asked;
  }
  return ra8_wifi_await_association(wifi);
}

ra8_err_t ra8_wifi_disconnect(ra8_wifi_t* wifi)
{
  RA8_CHECK_NULL_PTR(wifi, RA8_WIFI_TAG, "wifi");
  if (!wifi->open) {
    return k_ra8_err_not_initialized;
  }

  const ra8_err_t left    = wifi->backend->leave(wifi->backend_ctx);
  const ra8_err_t stopped = wifi->backend->radio_down(wifi->backend_ctx);
  wifi->radio_on          = false;
  wifi->state             = k_ra8_wifi_state_down;
  wifi->lease             = (ra8_wifi_lease_t){};
  return (left != k_ra8_ok) ? left : stopped;
}

ra8_err_t ra8_wifi_wait_ip(ra8_wifi_t* wifi, ra8_wifi_lease_t* out)
{
  RA8_CHECK_NULL_PTR(wifi, RA8_WIFI_TAG, "wifi");
  RA8_CHECK_NULL_PTR(out, RA8_WIFI_TAG, "out");
  *out = (ra8_wifi_lease_t){};
  if (!wifi->open) {
    return k_ra8_err_not_initialized;
  }
  if (wifi->state < k_ra8_wifi_state_associated) {
    return k_ra8_err_invalid_state;
  }

  ra8_wifi_lease_t lease = {};
  const ra8_err_t  bound = wifi->ip_bind(wifi->ip_ctx, &wifi->mac, &lease);
  if (bound != k_ra8_ok) {
    wifi->lease = (ra8_wifi_lease_t){};
    return bound;
  }
  lease.bound = (lease.ip != 0U);
  if (!lease.bound) {
    wifi->lease = (ra8_wifi_lease_t){};
    return k_ra8_err_timeout;
  }

  wifi->lease = lease;
  wifi->state = k_ra8_wifi_state_ip_bound;
  *out        = lease;
  return k_ra8_ok;
}

ra8_err_t ra8_wifi_get_ip(const ra8_wifi_t* wifi, ra8_wifi_lease_t* out)
{
  RA8_CHECK_NULL_PTR(wifi, RA8_WIFI_TAG, "wifi");
  RA8_CHECK_NULL_PTR(out, RA8_WIFI_TAG, "out");
  if (!wifi->open) {
    return k_ra8_err_not_initialized;
  }
  *out = wifi->lease;
  return k_ra8_ok;
}

ra8_err_t ra8_wifi_status(const ra8_wifi_t* wifi, ra8_wifi_status_t* out)
{
  RA8_CHECK_NULL_PTR(wifi, RA8_WIFI_TAG, "wifi");
  RA8_CHECK_NULL_PTR(out, RA8_WIFI_TAG, "out");
  if (!wifi->open) {
    return k_ra8_err_not_initialized;
  }

  *out            = (ra8_wifi_status_t){};
  out->state      = wifi->state;
  out->associated = (wifi->state >= k_ra8_wifi_state_associated);
  out->ip_bound   = (wifi->state == k_ra8_wifi_state_ip_bound);
  out->rssi       = wifi->rssi;
  out->ip         = wifi->lease;
  return k_ra8_ok;
}

ra8_err_t ra8_wifi_poll(ra8_wifi_t* wifi, ra8_wifi_link_t* out)
{
  RA8_CHECK_NULL_PTR(wifi, RA8_WIFI_TAG, "wifi");
  RA8_CHECK_NULL_PTR(out, RA8_WIFI_TAG, "out");
  *out = k_ra8_wifi_link_down;
  if (!wifi->open) {
    return k_ra8_err_not_initialized;
  }
  if (wifi->state == k_ra8_wifi_state_ip_bound) {
    return k_ra8_err_invalid_state;
  }

  ra8_wifi_link_t link     = k_ra8_wifi_link_down;
  const ra8_err_t serviced = wifi->backend->service(wifi->backend_ctx, &link);
  if (serviced != k_ra8_ok) {
    return serviced;
  }

  wifi->state = (link == k_ra8_wifi_link_up) ? k_ra8_wifi_state_associated : k_ra8_wifi_state_down;
  *out        = link;
  return k_ra8_ok;
}

ra8_err_t ra8_wifi_get_mac(ra8_wifi_t* wifi, ra8_wifi_mac_t* out)
{
  RA8_CHECK_NULL_PTR(wifi, RA8_WIFI_TAG, "wifi");
  RA8_CHECK_NULL_PTR(out, RA8_WIFI_TAG, "out");
  if (!wifi->open) {
    return k_ra8_err_not_initialized;
  }

  const ra8_err_t got = wifi->backend->get_mac(wifi->backend_ctx, out);
  if (got != k_ra8_ok) {
    *out = (ra8_wifi_mac_t){};
    return got;
  }
  wifi->mac       = *out;
  wifi->mac_valid = true;
  return k_ra8_ok;
}

ra8_err_t ra8_wifi_get_ap(ra8_wifi_t* wifi, ra8_wifi_ap_t* out)
{
  RA8_CHECK_NULL_PTR(wifi, RA8_WIFI_TAG, "wifi");
  RA8_CHECK_NULL_PTR(out, RA8_WIFI_TAG, "out");
  if (!wifi->open) {
    return k_ra8_err_not_initialized;
  }
  if (wifi->state < k_ra8_wifi_state_associated) {
    return k_ra8_err_invalid_state;
  }

  const ra8_err_t got = wifi->backend->get_ap(wifi->backend_ctx, out);
  if (got != k_ra8_ok) {
    *out = (ra8_wifi_ap_t){};
    return got;
  }
  wifi->rssi = out->rssi;
  return k_ra8_ok;
}
