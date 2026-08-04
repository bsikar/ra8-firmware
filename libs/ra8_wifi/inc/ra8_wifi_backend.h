/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie */
/**
 * @file ra8_wifi_backend.h
 * @brief The radio-operation seam ::ra8_wifi dispatches through.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The full layout of ::ra8_wifi_backend_t, which `ra8_wifi.h` forward-declares
 * as opaque. Only two kinds of translation unit include this file: a backend
 * that fills a `static const ra8_wifi_backend_t` and exports its address (the
 * way ::k_ra8_wifi_backend_c6link does), and a host test that constructs a mock
 * table to drive the facade with no radio present. Application code never
 * includes it -- it selects a backend by address and calls only `ra8_wifi.h`.
 *
 * @par Contract
 * Every function pointer is required; ::ra8_wifi_init rejects a table with a
 * null row. Each callback receives the backend's opaque `ctx` so a backend
 * keeps its state private, and returns an ::ra8_err_t the facade propagates.
 * Together the rows are the smallest set that expresses a station's life:
 * bring the link up, start and stop the radio, join and leave a network,
 * service the wire, read the two facts an IP stack needs -- the station MAC and
 * the associated AP -- and idle between attempts so a bounded wait measures
 * seconds rather than however fast the backend happens to answer.
 *
 * @see ra8_wifi.h
 * @see ra8_wifi_c6link.h
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_wifi.h"

/**
 * @struct ra8_wifi_backend
 * @brief One vtable per radio. Each callback takes the backend's opaque `ctx`.
 *
 * @details
 * A backend author fills a `static const ra8_wifi_backend_t` in one translation
 * unit and publishes its address through a header of their own. The facade in
 * `ra8_wifi.c` is the only runtime consumer; it never stores state here, so a
 * single `const` instance serves every handle that selects the backend.
 *
 * @invariant Every function pointer is non-null.
 * @invariant `open` runs exactly once per handle before any other row, and
 *            `close` exactly once after the last.
 *
 * @par Example:
 * @code
 * static const ra8_wifi_backend_t k_backend = {
 *   .open = my_open, .close = my_close, .radio_up = my_up, ...
 * };
 * @endcode
 *
 * @see ra8_wifi_init
 * @since 0.1.0
 */
struct ra8_wifi_backend {
  /**
   * @brief Bring the radio's link up and prove it answers.
   * @details Binds the transport, opens the link, and establishes liveness. The
   *          facade calls it once from ::ra8_wifi_init.
   * @param[in,out] ctx Backend context from ::ra8_wifi_cfg::backend_ctx.
   * @return ::k_ra8_ok when the radio is answering, otherwise an error.
   */
  ra8_err_t (*open)(void* ctx);

  /**
   * @brief Release the link the backend opened.
   * @details Mirror of `open`; the facade calls it from ::ra8_wifi_deinit.
   * @param[in,out] ctx Backend context.
   * @return ::k_ra8_ok when the link is released.
   */
  ra8_err_t (*close)(void* ctx);

  /**
   * @brief Start the radio in station mode.
   * @param[in,out] ctx Backend context.
   * @return ::k_ra8_ok when the radio is up in station mode.
   */
  ra8_err_t (*radio_up)(void* ctx);

  /**
   * @brief Stop the radio and release its resources.
   * @param[in,out] ctx Backend context.
   * @return ::k_ra8_ok when the radio is stopped.
   */
  ra8_err_t (*radio_down)(void* ctx);

  /**
   * @brief Ask the station to associate with a network.
   * @details Returns once the request is accepted; association completes later
   *          and is observed through `service`.
   * @param[in,out] ctx Backend context.
   * @param[in] ssid Target SSID, NUL-terminated; never null.
   * @param[in] psk Passphrase, NUL-terminated, or null for an open network.
   * @return ::k_ra8_ok when the association request was accepted.
   */
  ra8_err_t (*join)(void* ctx, const char* ssid, const char* psk);

  /**
   * @brief Disassociate the station from its network.
   * @param[in,out] ctx Backend context.
   * @return ::k_ra8_ok when the disassociation was accepted.
   */
  ra8_err_t (*leave)(void* ctx);

  /**
   * @brief Service the link once and report the association state.
   * @details Drains whatever the radio has to say and reports whether the
   *          station is associated right now.
   * @param[in,out] ctx Backend context.
   * @param[out] out_link Association state after the cycle; never null.
   * @return ::k_ra8_ok when the cycle ran.
   */
  ra8_err_t (*service)(void* ctx, ra8_wifi_link_t* out_link);

  /**
   * @brief Read the station's own MAC address.
   * @param[in,out] ctx Backend context.
   * @param[out] out Address to fill; never null.
   * @return ::k_ra8_ok when @p out holds the station address.
   */
  ra8_err_t (*get_mac)(void* ctx, ra8_wifi_mac_t* out);

  /**
   * @brief Read what the radio knows about the associated AP.
   * @param[in,out] ctx Backend context.
   * @param[out] out Record to fill; never null.
   * @return ::k_ra8_ok when @p out describes the association.
   */
  ra8_err_t (*get_ap)(void* ctx, ra8_wifi_ap_t* out);

  /**
   * @brief Idle for @p ms milliseconds, pacing a bounded wait.
   * @details The facade counts poll attempts, not milliseconds, so without this
   *          row its association wait would run as fast as the backend could
   *          answer -- and a link that answers in tens of milliseconds spends a
   *          two-hundred-attempt budget in a couple of seconds, far less than an
   *          802.11 association takes. This is the only seam through which the
   *          facade reaches a clock; a backend implements it with whatever delay
   *          its transport already owns, and a host test makes it instantaneous.
   *          It cannot fail, so it returns nothing.
   * @param[in,out] ctx Backend context.
   * @param[in] ms Milliseconds to idle for.
   * @return Nothing.
   */
  void (*idle)(void* ctx, uint16_t ms);
};

#ifdef __cplusplus
}
#endif
