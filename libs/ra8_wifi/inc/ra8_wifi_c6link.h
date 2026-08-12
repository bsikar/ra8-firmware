/**
 * @file ra8_wifi_c6link.h
 * @brief The ESP32-C6 backend for the ::ra8_wifi facade, over ``ra8_c6link``.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The first ::ra8_wifi_backend_t. It maps each facade operation onto the
 * ``ra8_c6link`` station calls and owns the event callback that turns the
 * co-processor's asynchronous connect/disconnect announcements into the simple
 * "associated / not associated" reading the facade asks for. Everything the
 * facade is designed to hide -- the RPC ids, the transaction pump, the
 * interface index, the ``wifi_mode_t`` -- stays on this side of the seam.
 *
 * @par What an application wires
 * Allocate one ::ra8_wifi_c6link_t and one ::ra8_c6link_t at file scope, fill an
 * ::ra8_wifi_c6link_cfg_t, and call ::ra8_wifi_c6link_setup to populate the
 * backend half of an ::ra8_wifi_cfg_t. The application still owns two things
 * this backend cannot: the transport's hardware bring-up (clocks and pins, done
 * once before ::ra8_wifi_init) and the IP provider (::ra8_wifi_cfg::ip_bind).
 *
 * @par Example:
 * @code
 * static ra8_c6link_t       s_link;
 * static ra8_wifi_c6link_t  s_c6;
 * static uint8_t            s_arena[k_ra8_c6link_arena_min];
 *
 * ra8_wifi_c6link_cfg_t bcfg = {
 *   .link = &s_link, .arena = s_arena,
 *   .arena_bytes = (uint32_t)sizeof s_arena, .rx_cb = nx_ether_driver_c6_rx,
 * };
 * ra8_wifi_cfg_t cfg = {};
 * (void)ra8_wifi_c6link_setup(&s_c6, &bcfg, &cfg);
 * cfg.ip_bind = my_dhcp;
 * cfg.ip_ctx  = &s_link;
 * (void)ra8_wifi_init(&s_wifi, &cfg);
 * @endcode
 *
 * @see ra8_wifi.h
 * @see ra8_c6link.h
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

#include "ra8_c6link.h"
#include "ra8_err.h"
#include "ra8_wifi.h"

/**
 * @struct ra8_wifi_c6link_cfg
 * @brief What ::ra8_wifi_c6link_setup needs to wire this backend to a link.
 *
 * @details
 * The link handle and its decode arena are caller-allocated because the whole
 * ra8_c6link stack is heap-free; this backend opens the link inside
 * ::ra8_wifi_backend::open. The transport seam is supplied already bound -- by
 * ::ra8_esp_hosted_c6link_bind on hardware, or by a co-processor model under
 * test -- so this backend stays independent of any one transport. The receive
 * callback is the IP stack's L2 frame sink and may be null before the stack
 * exists.
 *
 * @invariant `link` points at zero-initialised storage this backend may open.
 * @invariant `transport` has every row filled, exactly as ::ra8_c6link_open
 *            requires.
 * @invariant `arena_bytes` is at least ::k_ra8_c6link_arena_min.
 *
 * @par Example:
 * @code
 * ra8_wifi_c6link_cfg_t bcfg = { .link = &s_link, .arena = s_arena,
 *                                .arena_bytes = (uint32_t)sizeof s_arena };
 * (void)ra8_esp_hosted_c6link_bind(&bcfg.transport);
 * @endcode
 *
 * @see ra8_wifi_c6link_setup
 * @since 0.1.0
 */
typedef struct ra8_wifi_c6link_cfg {
  ra8_c6link_t*          link;        /**< Caller-allocated link, opened by the backend. */
  ra8_c6link_transport_t transport;   /**< Bound hardware (or model) seam.               */
  uint8_t*               arena;       /**< Protobuf decode arena for the link.           */
  uint32_t               arena_bytes; /**< Bytes available at `arena`.                   */
  ra8_c6link_rx_cb_t     rx_cb;       /**< IP-stack L2 receive sink, or null.            */
} ra8_wifi_c6link_cfg_t;

/**
 * @struct ra8_wifi_c6link
 * @brief Caller-allocated backend context for the ESP32-C6 radio.
 *
 * @details
 * Holds the link this backend drives plus the two flags its event callback
 * latches from the co-processor's station announcements. It is the `void* ctx`
 * every ::ra8_wifi_backend_t row receives. Zero-initialise it and hand it to
 * ::ra8_wifi_c6link_setup; treat every field as private thereafter.
 *
 * @invariant `connected` and `disconnected` reflect the newest station events
 *            since the last ::ra8_wifi_backend::join.
 * @invariant `link` is the same handle across the backend's whole lifetime.
 *
 * @par Example:
 * @code
 * static ra8_wifi_c6link_t s_c6;
 * @endcode
 *
 * @see ra8_wifi_c6link_setup
 * @since 0.1.0
 */
typedef struct ra8_wifi_c6link {
  ra8_c6link_t*          link;         /**< The link this backend opens and drives.   */
  ra8_c6link_transport_t transport;    /**< Bound seam passed to ::ra8_c6link_open.   */
  uint8_t*               arena;        /**< Decode arena passed to ::ra8_c6link_open. */
  uint32_t               arena_bytes;  /**< Bytes available at `arena`.               */
  ra8_c6link_rx_cb_t     rx_cb;        /**< IP-stack L2 receive sink, or null.        */
  volatile bool          connected;    /**< A station-connected event has arrived.    */
  volatile bool          disconnected; /**< A station-disconnected event has arrived. */
  uint16_t               reason;       /**< Last 802.11 disconnect reason code.       */
} ra8_wifi_c6link_t;

/**
 * @brief Wire this backend into the backend half of an ::ra8_wifi_cfg_t.
 *
 * @details
 * Copies @p cfg into @p self, clears the event latches, and fills @p out_wcfg 's
 * `backend` and `backend_ctx`. It leaves `ip_bind` and `ip_ctx` untouched so the
 * application can set its IP provider on the same structure. No hardware is
 * touched; the link is opened later, from ::ra8_wifi_init.
 *
 * @param[out] self Backend context to populate; must be non-null.
 * @param[in] cfg Backend configuration; must be non-null with a link and an
 *                arena of at least ::k_ra8_c6link_arena_min bytes.
 * @param[out] out_wcfg Facade configuration whose backend half is filled; must
 *                      be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok @p out_wcfg selects this backend with @p self as context.
 * @retval k_ra8_err_null_ptr @p self, @p cfg, @p out_wcfg, or `cfg->link` was
 *         null.
 * @retval k_ra8_err_invalid_size `cfg->arena_bytes` is below
 *         ::k_ra8_c6link_arena_min.
 *
 * @pre The transport's hardware bring-up has run or will run before
 *      ::ra8_wifi_init.
 * @pre `cfg->link` is zero-initialised storage.
 * @post On success @p out_wcfg->backend is ::k_ra8_wifi_backend_c6link.
 * @post On failure @p out_wcfg is not modified.
 *
 * @note Not thread-safe; call once during bring-up.
 *
 * @par Example:
 * @code
 * (void)ra8_wifi_c6link_setup(&s_c6, &bcfg, &cfg);
 * @endcode
 *
 * @see ra8_wifi_init
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 5: preconditions on every pointer plus a size floor, two
 *   postconditions.
 */
[[nodiscard]] ra8_err_t ra8_wifi_c6link_setup(ra8_wifi_c6link_t*           self,
                                              const ra8_wifi_c6link_cfg_t* cfg,
                                              ra8_wifi_cfg_t*              out_wcfg);

/**
 * @var k_ra8_wifi_backend_c6link
 * @brief The ESP32-C6 ::ra8_wifi_backend_t, exported for ::ra8_wifi_cfg::backend.
 *
 * @details
 * A single `const` table shared by every handle that runs on the co-processor.
 * Applications take its address through ::ra8_wifi_c6link_setup rather than
 * naming it directly, but it is exported so a test or a bespoke wiring can
 * reference it.
 *
 * @note Immutable; every row points at a translation-unit-local function.
 * @warning Do not copy or mutate it; pass its address.
 *
 * @see ra8_wifi_c6link_setup
 * @since 0.1.0
 */
extern const ra8_wifi_backend_t k_ra8_wifi_backend_c6link;

#ifdef __cplusplus
}
#endif
