/**
 * @file port/netxduo/inc/nx_ether_driver_netif.h
 * @brief NetX Duo network driver over the RA8 ``ra8_io_netif`` seam (RA8FW-318).
 *
 * [Ring 4 / PORT] {World: NS}
 *
 * One NetX Duo link driver that binds either Ethernet transport at bind time
 * through the ``ra8_io_netif`` seam (PR #3907). Frame I/O goes through the
 * seam only: ``ra8_io_netif_send`` for TX, ``ra8_io_netif_poll`` for RX.
 * What the seam does not carry stays per-backend, supplied at bind time in
 * ::nx_netif_backend_ops_t: backend bring-up at INITIALIZE, teardown at
 * UNINITIALIZE, the link-presence query, and the MTU the backend safely
 * carries. The driver core names no backend symbol, so one core links
 * against either backend; each backend ships its own ops provider.
 *
 * Replaces ``nx_ether_driver_ra8_eth`` / ``nx_ether_driver_c6`` once proven;
 * those two stay in tree until then.
 */
#ifndef NX_ETHER_DRIVER_NETIF_H
#define NX_ETHER_DRIVER_NETIF_H

#include <stdbool.h>
#include <stdint.h>

#include "nx_api.h"
#include "ra8_err.h"
#include "ra8_io_netif.h"

/**
 * @struct nx_netif_backend_ops_t
 * @brief Per-backend operations the unified driver needs beyond the seam.
 *
 * @details The seam carries frames; a NetX driver also needs backend
 *          bring-up, teardown, a link-presence query, and the MTU the
 *          backend can safely carry. Bind-time function pointers keep the
 *          driver core free of every backend symbol.
 */
typedef struct nx_netif_backend_ops_t {
  /**
   * @brief Bring the backend up at ``NX_LINK_INITIALIZE``.
   *
   * @details Runs with the driver-resolved 6-byte local MAC so the
   *          backend can programme it (the net_pal backend feeds it to
   *          ``ra8_net_pal_init``). The c6link backend needs no
   *          bring-up -- the app opened the link before binding -- so
   *          its provider supplies a no-op.
   *
   * @param[in] mac The local MAC; never NULL on the INITIALIZE path.
   * @return ``k_ra8_ok`` on success; any other value fails INITIALIZE.
   */
  ra8_err_t (*open)(const uint8_t* mac);

  /**
   * @brief Tear the backend down at ``NX_LINK_UNINITIALIZE``.
   *
   * May be ``NULL`` when the backend owns nothing to release.
   */
  void (*close)(void);

  /**
   * @brief Query link presence for ``NX_LINK_GET_STATUS``.
   *
   * May be ``NULL``: the driver then answers from its own
   * ``NX_LINK_ENABLE`` mirror (the c6link backend's behavior).
   *
   * @return ``true`` when the backend reports a live link.
   */
  bool (*link_up)(void);

  /** @brief MTU the backend safely carries, payload octets. */
  uint16_t mtu;
} nx_netif_backend_ops_t;

/**
 * @brief Bind the unified driver to a seam handle and backend ops.
 *
 * @details Must run before ``nx_ip_create`` fires ``NX_LINK_INITIALIZE``.
 *          ``netif`` and ``ops`` are caller-owned and must outlive the
 *          driver (the seam's established contract). ``ops->open`` fires
 *          on INITIALIZE, not here, so the MAC the driver resolves is
 *          available to the backend. Rebinding while the driver is open
 *          is rejected.
 *
 * @param[in] netif Bound ``ra8_io_netif_t`` (a seam backend binder ran on it).
 * @param[in] ops  Backend operations; must be non-NULL.
 *
 * @retval k_ra8_ok Bound.
 * @retval k_ra8_err_null_ptr ``netif`` or ``ops`` was NULL.
 */
[[nodiscard]] ra8_err_t nx_ether_driver_netif_bind(const ra8_io_netif_t* netif,
                                                   const nx_netif_backend_ops_t* ops);

/**
 * @brief Give the driver an app-supplied local MAC before ``nx_ip_create()``.
 *
 * @details Parity with ``nx_ether_driver_ra8_eth_set_mac()``: when this has
 *          run, ``NX_LINK_INITIALIZE`` programmes the stored MAC instead of
 *          unpacking the interface struct, stamps it back into the
 *          interface, and hands it to ``ops->open``. A ``NULL`` pointer is
 *          ignored. Call it before ``nx_ip_create()`` fires
 *          ``NX_LINK_INITIALIZE``.
 *
 * @param[in] mac The 6-byte local MAC; ``NULL`` is silently ignored.
 */
void nx_ether_driver_netif_set_mac(const uint8_t mac[6]);

/**
 * @brief The single entry point passed to ``nx_ip_create()``.
 *
 * @details Dispatches on ``driver_req->nx_ip_driver_command`` exactly as the
 *          two transport-specific drivers do; see RA8FW-318 for the map.
 */
void nx_ether_driver_netif(NX_IP_DRIVER* driver_req);

/**
 * @brief net_pal backend ops (ESWM): MTU 128 (the ESWM per-frame egress
 *        defect cap), open = ``ra8_net_pal_init``, close =
 *        ``ra8_net_pal_deinit``, link_up = ``ra8_net_pal_link_status``.
 */
const nx_netif_backend_ops_t* nx_ether_driver_netif_net_pal_ops(void);

/**
 * @brief c6link backend ops: MTU 1514, open = no-op (the app opened the
 *        link before binding), close = NULL, link_up = NULL (the driver
 *        answers GET_STATUS from its ENABLE mirror).
 */
const nx_netif_backend_ops_t* nx_ether_driver_netif_c6_ops(void);

#endif /* NX_ETHER_DRIVER_NETIF_H */
