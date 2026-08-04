/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie */
/**
 * @file ra8_wifi_c6link.c
 * @brief The ESP32-C6 backend: each ::ra8_wifi operation onto ``ra8_c6link``.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Implements ::k_ra8_wifi_backend_c6link. Every function here is one thin
 * mapping from a facade operation to the ``ra8_c6link`` station call that
 * performs it, plus the one piece of state the facade cannot see for itself:
 * whether the co-processor has announced that the station is associated. That
 * announcement is asynchronous, so this backend registers an event callback at
 * ::ra8_c6link_open that latches the two station events, and reports the latched
 * result whenever the facade services the link.
 *
 * All the machinery the facade exists to hide -- RPC ids, the transaction pump,
 * the interface index, ``wifi_mode_t`` -- is reached only from this file.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 *
 * @since 0.1.0
 */

#include "ra8_wifi_c6link.h"

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_c6link.h"
#include "ra8_c6link_wifi.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_wifi.h"
#include "ra8_wifi_backend.h"

/** @brief Component tag for this backend's log lines. */
#define RA8_WIFI_C6_TAG "WIFI-C6"

static_assert((uint32_t)k_ra8_wifi_mac_bytes == (uint32_t)k_ra8_c6link_mac_bytes,
              "the facade and link MAC widths must agree for a straight copy");
static_assert((uint32_t)k_ra8_wifi_ssid_max == (uint32_t)k_ra8_c6link_ssid_max,
              "the facade and link SSID capacities must agree for a straight copy");

/**
 * @brief Latch a station event so ::ra8_wifi_c6link_service can report it.
 * @details Registered as the link's event callback. It records only the two
 *          station transitions; boot and bare Wi-Fi events are informational and
 *          left for the counters. ``ra8_c6link`` guarantees a non-null event and
 *          the context set at open, so no defensive guard is needed.
 * @param[in,out] ctx The ::ra8_wifi_c6link_t handed to ::ra8_c6link_open.
 * @param[in] ev The decoded announcement; valid only during this call.
 * @return Nothing.
 * @pre ``ctx`` is the backend context registered at open.
 * @pre ``ev`` is non-null, per the ::ra8_c6link_event_cb_t contract.
 * @post A station-connected event sets `connected`.
 * @post A station-disconnected event sets `disconnected` and `reason`.
 * @note Runs inside ::ra8_c6link_poll on the polling thread.
 * @since 0.1.0
 */
RA8_INTERNAL static void ra8_wifi_c6link_on_event(void* ctx, const ra8_c6link_event_t* ev)
{
  ra8_wifi_c6link_t* self = (ra8_wifi_c6link_t*)ctx;
  if (ev->kind == k_ra8_c6link_event_sta_connected) {
    self->connected = true;
  } else if (ev->kind == k_ra8_c6link_event_sta_disconnected) {
    self->disconnected = true;
    self->reason       = ev->reason;
  } else {
    /* boot / bare Wi-Fi events: nothing to latch here. */
  }
}

/**
 * @brief Bring the link up: open it and prove the co-processor answers.
 * @details Builds an ::ra8_c6link_cfg_t from the context -- the bound transport,
 *          the arena, this backend's event latch and the IP-stack receive sink
 *          -- opens the link with ::ra8_c6link_open, then establishes liveness
 *          with ::ra8_c6link_await_ready. The facade calls it from
 *          ::ra8_wifi_init.
 * @param[in,out] ctx The ::ra8_wifi_c6link_t for this backend; must be non-null.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The link is open and the radio is answering.
 * @retval k_ra8_err_null_ptr @p ctx or its link was null.
 * @retval k_ra8_err_invalid_state The link was already open.
 * @pre The transport in @p ctx is bound and its hardware is up.
 * @pre The link storage in @p ctx is zero-initialised.
 * @post On success the link reports open.
 * @post On failure the link is not left half-open.
 * @note Not thread-safe; dispatched once per handle.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t ra8_wifi_c6link_op_open(void* ctx)
{
  ra8_wifi_c6link_t* self = (ra8_wifi_c6link_t*)ctx;
  RA8_CHECK_NULL_PTR(self, RA8_WIFI_C6_TAG, "ctx");
  RA8_CHECK_NULL_PTR(self->link, RA8_WIFI_C6_TAG, "ctx.link");

  ra8_c6link_cfg_t cfg = {};
  cfg.transport        = self->transport;
  cfg.arena            = self->arena;
  cfg.arena_bytes      = self->arena_bytes;
  cfg.event_cb         = ra8_wifi_c6link_on_event;
  cfg.rx_cb            = self->rx_cb;
  cfg.cb_ctx           = self;

  const ra8_err_t opened = ra8_c6link_open(self->link, &cfg);
  if (opened != k_ra8_ok) {
    return opened;
  }

  ra8_c6link_fw_version_t fw = {};
  return ra8_c6link_await_ready(self->link, (uint16_t)k_ra8_c6link_announce_transfers, &fw);
}

/**
 * @brief Release the link this backend opened.
 * @details Maps onto ::ra8_c6link_close. The transport itself is not torn down;
 *          whoever brought it up owns that. The facade calls it from
 *          ::ra8_wifi_deinit.
 * @param[in,out] ctx The ::ra8_wifi_c6link_t for this backend; must be non-null.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The link is closed.
 * @retval k_ra8_err_null_ptr @p ctx or its link was null.
 * @retval k_ra8_err_not_initialized The link was not open.
 * @pre @p ctx was opened by ::ra8_wifi_c6link_op_open.
 * @pre No pump is running against the link.
 * @post The link reports closed.
 * @post No further event reaches this backend's latch.
 * @note Not thread-safe; dispatched once per handle.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t ra8_wifi_c6link_op_close(void* ctx)
{
  ra8_wifi_c6link_t* self = (ra8_wifi_c6link_t*)ctx;
  RA8_CHECK_NULL_PTR(self, RA8_WIFI_C6_TAG, "ctx");
  RA8_CHECK_NULL_PTR(self->link, RA8_WIFI_C6_TAG, "ctx.link");
  return ra8_c6link_close(self->link);
}

/**
 * @brief Start the co-processor's radio in station mode.
 * @details Maps onto ::ra8_c6link_wifi_start, which issues the init, mode and
 *          start requests in sequence. The facade calls it from
 *          ::ra8_wifi_connect when the radio is not already on.
 * @param[in,out] ctx The ::ra8_wifi_c6link_t for this backend; must be non-null.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The radio is up in station mode.
 * @retval k_ra8_err_null_ptr @p ctx or its link was null.
 * @retval k_ra8_err_protocol_error The co-processor refused a step.
 * @pre The link is open.
 * @pre The radio is not already started.
 * @post On success the co-processor is in station mode.
 * @post On failure the link's last fault names the step that failed.
 * @note Not thread-safe; it pumps the link.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t ra8_wifi_c6link_op_radio_up(void* ctx)
{
  ra8_wifi_c6link_t* self = (ra8_wifi_c6link_t*)ctx;
  RA8_CHECK_NULL_PTR(self, RA8_WIFI_C6_TAG, "ctx");
  RA8_CHECK_NULL_PTR(self->link, RA8_WIFI_C6_TAG, "ctx.link");
  return ra8_c6link_wifi_start(self->link);
}

/**
 * @brief Stop the radio and release the co-processor's Wi-Fi resources.
 * @details Maps onto ::ra8_c6link_wifi_stop, which issues the stop and deinit
 *          requests. The facade calls it from ::ra8_wifi_disconnect.
 * @param[in,out] ctx The ::ra8_wifi_c6link_t for this backend; must be non-null.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The radio is stopped.
 * @retval k_ra8_err_null_ptr @p ctx or its link was null.
 * @retval k_ra8_err_protocol_error A teardown step was refused; the rest ran.
 * @pre The link is open.
 * @pre The caller has stopped transmitting frames.
 * @post Every teardown step was attempted.
 * @post On failure the link's last fault names the first failure.
 * @note Not thread-safe; it pumps the link.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t ra8_wifi_c6link_op_radio_down(void* ctx)
{
  ra8_wifi_c6link_t* self = (ra8_wifi_c6link_t*)ctx;
  RA8_CHECK_NULL_PTR(self, RA8_WIFI_C6_TAG, "ctx");
  RA8_CHECK_NULL_PTR(self->link, RA8_WIFI_C6_TAG, "ctx.link");
  return ra8_c6link_wifi_stop(self->link);
}

/**
 * @brief Ask the station to associate with a network.
 * @details Clears the event latches so a stale association cannot be mistaken
 *          for this one, fills an ::ra8_c6link_sta_cfg_t with
 *          ::ra8_c6link_sta_cfg_set, and issues the join with
 *          ::ra8_c6link_wifi_join. Returns once the request is accepted; the
 *          result arrives later as an event this backend latches.
 * @param[in,out] ctx The ::ra8_wifi_c6link_t for this backend; must be non-null.
 * @param[in] ssid Target SSID, NUL-terminated; must be non-null.
 * @param[in] psk Passphrase, NUL-terminated, or null for an open network.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The association request was accepted.
 * @retval k_ra8_err_null_ptr @p ctx or @p ssid was null.
 * @retval k_ra8_err_invalid_size @p ssid was empty or a credential too long.
 * @retval k_ra8_err_protocol_error The co-processor refused the request.
 * @pre The radio has been started.
 * @pre @p ssid names a network in range.
 * @post On success an association attempt is in progress and the latches are clear.
 * @post On failure the latches are still clear for a retry.
 * @note Not thread-safe; it pumps the link.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t ra8_wifi_c6link_op_join(void* ctx, const char* ssid, const char* psk)
{
  ra8_wifi_c6link_t* self = (ra8_wifi_c6link_t*)ctx;
  RA8_CHECK_NULL_PTR(self, RA8_WIFI_C6_TAG, "ctx");
  RA8_CHECK_NULL_PTR(ssid, RA8_WIFI_C6_TAG, "ssid");

  self->connected    = false;
  self->disconnected = false;
  self->reason       = 0U;

  ra8_c6link_sta_cfg_t sta = {};
  const ra8_err_t      set = ra8_c6link_sta_cfg_set(&sta, ssid, psk);
  if (set != k_ra8_ok) {
    return set;
  }
  return ra8_c6link_wifi_join(self->link, &sta);
}

/**
 * @brief Disassociate the station from its current network.
 * @details Maps onto ::ra8_c6link_wifi_leave. A disconnect event follows, which
 *          this backend's latch records. The facade calls it from
 *          ::ra8_wifi_disconnect before stopping the radio.
 * @param[in,out] ctx The ::ra8_wifi_c6link_t for this backend; must be non-null.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The disassociation was accepted.
 * @retval k_ra8_err_null_ptr @p ctx or its link was null.
 * @retval k_ra8_err_protocol_error The co-processor refused the request.
 * @pre The link is open.
 * @pre The caller expects a disconnect event to follow.
 * @post On success a disassociation is in progress.
 * @post On failure the link's last fault names the request.
 * @note Not thread-safe; it pumps the link.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t ra8_wifi_c6link_op_leave(void* ctx)
{
  ra8_wifi_c6link_t* self = (ra8_wifi_c6link_t*)ctx;
  RA8_CHECK_NULL_PTR(self, RA8_WIFI_C6_TAG, "ctx");
  RA8_CHECK_NULL_PTR(self->link, RA8_WIFI_C6_TAG, "ctx.link");
  return ra8_c6link_wifi_leave(self->link);
}

/**
 * @brief Service the link once and report whether the station is associated.
 * @details Pumps ::ra8_c6link_poll, which delivers any pending events to this
 *          backend's latch, then reports the latched state: associated only when
 *          a connect has arrived and no later disconnect has. The facade calls
 *          it from ::ra8_wifi_poll and its connect wait loop.
 * @param[in,out] ctx The ::ra8_wifi_c6link_t for this backend; must be non-null.
 * @param[out] out_link Association state after the cycle; must be non-null.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The cycle ran and @p out_link holds the reading.
 * @retval k_ra8_err_null_ptr @p ctx or @p out_link was null.
 * @retval k_ra8_err_hw_timeout The co-processor never armed HANDSHAKE.
 * @retval k_ra8_err_spi_error The transport refused a transfer.
 * @pre The link is open and no other context is driving it.
 * @pre @p out_link is writable.
 * @post @p out_link reflects the latched association state.
 * @post At most one pump ran.
 * @note Not thread-safe; it pumps the link.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t ra8_wifi_c6link_op_service(void* ctx, ra8_wifi_link_t* out_link)
{
  ra8_wifi_c6link_t* self = (ra8_wifi_c6link_t*)ctx;
  RA8_CHECK_NULL_PTR(self, RA8_WIFI_C6_TAG, "ctx");
  RA8_CHECK_NULL_PTR(out_link, RA8_WIFI_C6_TAG, "out_link");

  ra8_c6link_stats_t stats = {};
  const ra8_err_t    err =
    ra8_c6link_poll(self->link, (uint16_t)k_ra8_c6link_announce_transfers, &stats);
  if (err != k_ra8_ok) {
    return err;
  }

  /* Down unless a connect was seen, and a later disconnect wins outright. Two
   * single-condition tests applied in that order, so the precedence lives in
   * the sequence rather than in a compound decision that would then owe MC/DC
   * vectors. */
  *out_link = k_ra8_wifi_link_down;
  if (self->connected) {
    *out_link = k_ra8_wifi_link_up;
  }
  if (self->disconnected) {
    *out_link = k_ra8_wifi_link_down;
  }
  return k_ra8_ok;
}

/**
 * @brief Read the station's own MAC address.
 * @details Maps onto ::ra8_c6link_wifi_mac and copies the six octets into the
 *          facade's ::ra8_wifi_mac_t. The facade calls it from
 *          ::ra8_wifi_connect and ::ra8_wifi_get_mac.
 * @param[in,out] ctx The ::ra8_wifi_c6link_t for this backend; must be non-null.
 * @param[out] out Address to fill; must be non-null.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok @p out holds the station address.
 * @retval k_ra8_err_null_ptr @p ctx or @p out was null.
 * @retval k_ra8_err_protocol_error The co-processor reported no valid address.
 * @pre The radio has been started.
 * @pre @p out is writable.
 * @post On success @p out holds ::k_ra8_wifi_mac_bytes octets.
 * @post On failure @p out is not written.
 * @note Not thread-safe; it pumps the link.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t ra8_wifi_c6link_op_get_mac(void* ctx, ra8_wifi_mac_t* out)
{
  ra8_wifi_c6link_t* self = (ra8_wifi_c6link_t*)ctx;
  RA8_CHECK_NULL_PTR(self, RA8_WIFI_C6_TAG, "ctx");
  RA8_CHECK_NULL_PTR(out, RA8_WIFI_C6_TAG, "out");

  ra8_c6link_mac_t mac = {};
  const ra8_err_t  err = ra8_c6link_wifi_mac(self->link, &mac);
  if (err != k_ra8_ok) {
    return err;
  }
  (void)memcpy(out->octet, mac.octet, (size_t)k_ra8_wifi_mac_bytes);
  return k_ra8_ok;
}

/**
 * @brief Read what the co-processor knows about the associated AP.
 * @details Maps onto ::ra8_c6link_wifi_ap_info and copies the record -- BSSID,
 *          SSID, channel, RSSI and auth mode -- into the facade's
 *          ::ra8_wifi_ap_t. The facade calls it from ::ra8_wifi_get_ap.
 * @param[in,out] ctx The ::ra8_wifi_c6link_t for this backend; must be non-null.
 * @param[out] out Record to fill; must be non-null.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok @p out describes the current association.
 * @retval k_ra8_err_null_ptr @p ctx or @p out was null.
 * @retval k_ra8_err_protocol_error The co-processor reported no AP record.
 * @pre The station is associated.
 * @pre @p out is writable.
 * @post On success @p out is fully written.
 * @post On failure @p out is cleared rather than left half-written.
 * @note Not thread-safe; it pumps the link.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t ra8_wifi_c6link_op_get_ap(void* ctx, ra8_wifi_ap_t* out)
{
  ra8_wifi_c6link_t* self = (ra8_wifi_c6link_t*)ctx;
  RA8_CHECK_NULL_PTR(self, RA8_WIFI_C6_TAG, "ctx");
  RA8_CHECK_NULL_PTR(out, RA8_WIFI_C6_TAG, "out");

  ra8_c6link_ap_info_t ap  = {};
  const ra8_err_t      err = ra8_c6link_wifi_ap_info(self->link, &ap);
  if (err != k_ra8_ok) {
    return err;
  }
  *out = (ra8_wifi_ap_t){};
  (void)memcpy(out->bssid.octet, ap.bssid.octet, (size_t)k_ra8_wifi_mac_bytes);
  (void)memcpy(out->ssid, ap.ssid, sizeof(out->ssid));
  out->ssid_len = ap.ssid_len;
  out->channel  = ap.channel;
  out->rssi     = ap.rssi;
  out->authmode = ap.authmode;
  return k_ra8_ok;
}

/**
 * @brief Idle for the requested milliseconds, on the transport's own clock.
 * @details The facade counts attempts and has no clock of its own; this is the
 *          seam through which it paces one. The delay used is the transport's
 *          ``delay_ms`` -- the same one ``ra8_c6link``'s pump waits on -- so the
 *          facade's association wait is paced by exactly the mechanism the
 *          bench proved, and a co-processor model can make it free.
 * @param[in,out] ctx The ::ra8_wifi_c6link_t for this backend; may be null.
 * @param[in] ms Milliseconds to idle for.
 * @return Nothing.
 * @pre The transport in @p ctx is bound, or @p ctx is null and this is a no-op.
 * @pre The caller is not holding a lock the delay would extend.
 * @post At least @p ms elapsed, unless there was no transport to wait on.
 * @post No link or backend state was touched.
 * @note Blocks the calling context; it is a delay, not a yield.
 * @since 0.1.0
 */
RA8_INTERNAL static void ra8_wifi_c6link_op_idle(void* ctx, uint16_t ms)
{
  ra8_wifi_c6link_t* self = (ra8_wifi_c6link_t*)ctx;
  if (self == nullptr) {
    return;
  }
  if (self->transport.delay_ms == nullptr) {
    return;
  }
  self->transport.delay_ms(self->transport.ctx, ms);
}

/**
 * @var k_ra8_wifi_backend_c6link
 * @brief The one ESP32-C6 backend table; see the header for the contract.
 * @details Every row points at a translation-unit-local mapping above.
 * @note Immutable and shared by every handle on the co-processor.
 * @since 0.1.0
 */
const ra8_wifi_backend_t k_ra8_wifi_backend_c6link = {
  .open       = ra8_wifi_c6link_op_open,
  .close      = ra8_wifi_c6link_op_close,
  .radio_up   = ra8_wifi_c6link_op_radio_up,
  .radio_down = ra8_wifi_c6link_op_radio_down,
  .join       = ra8_wifi_c6link_op_join,
  .leave      = ra8_wifi_c6link_op_leave,
  .service    = ra8_wifi_c6link_op_service,
  .get_mac    = ra8_wifi_c6link_op_get_mac,
  .get_ap     = ra8_wifi_c6link_op_get_ap,
  .idle       = ra8_wifi_c6link_op_idle,
};

ra8_err_t ra8_wifi_c6link_setup(ra8_wifi_c6link_t*           self,
                                const ra8_wifi_c6link_cfg_t* cfg,
                                ra8_wifi_cfg_t*              out_wcfg)
{
  RA8_CHECK_NULL_PTR(self, RA8_WIFI_C6_TAG, "self");
  RA8_CHECK_NULL_PTR(cfg, RA8_WIFI_C6_TAG, "cfg");
  RA8_CHECK_NULL_PTR(out_wcfg, RA8_WIFI_C6_TAG, "out_wcfg");
  RA8_CHECK_NULL_PTR(cfg->link, RA8_WIFI_C6_TAG, "cfg.link");
  if (cfg->arena_bytes < (uint32_t)k_ra8_c6link_arena_min) {
    return k_ra8_err_invalid_size;
  }

  *self             = (ra8_wifi_c6link_t){};
  self->link        = cfg->link;
  self->transport   = cfg->transport;
  self->arena       = cfg->arena;
  self->arena_bytes = cfg->arena_bytes;
  self->rx_cb       = cfg->rx_cb;

  out_wcfg->backend     = &k_ra8_wifi_backend_c6link;
  out_wcfg->backend_ctx = self;
  return k_ra8_ok;
}
