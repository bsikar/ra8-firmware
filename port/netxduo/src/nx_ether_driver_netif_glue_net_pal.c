/**
 * @file port/netxduo/src/nx_ether_driver_netif_glue_net_pal.c
 * @brief net_pal (ESWM) backend ops for the unified NetX Duo driver.
 *
 * [Ring 4 / PORT] {World: NS}
 *
 * Supplies ::nx_ether_driver_netif_net_pal_ops, the
 * ::nx_netif_backend_ops_t the unified driver binds when an app wants the
 * on-chip ESWM MAC. Bring-up is ``ra8_net_pal_init`` with the
 * driver-resolved MAC; teardown is ``ra8_net_pal_deinit``; link presence
 * is ``ra8_net_pal_link_status``. The MTU stays at the ESWM per-frame
 * egress-defect cap the ``nx_ether_driver_ra8_eth`` driver documents
 * (payloads approaching ~512 B corrupt on silicon; IP fragments and TCP
 * clamps MSS, RX unaffected).
 *
 * Owns a per-backend translation unit on purpose: an app that never binds
 * the net_pal backend never compiles (or links) this file, so
 * ``ra8_net_pal`` stays out of every other image.
 */
#include "nx_ether_driver_netif.h"

#include <string.h>

#include "ra8_net_pal.h"

/**
 * @brief Bring the ESWM transport up with the driver-resolved MAC.
 *
 * @details Runs at ``NX_LINK_INITIALIZE``. Zero MACs are passed through
 *          as-is: ``ra8_net_pal_init`` treats a NULL descriptor as "keep
 *          whatever the ESWM block already has", and a driver-resolved
 *          zero MAC means the app had none either.
 *
 * @param[in] mac The 6-byte local MAC; never NULL on the INITIALIZE path.
 * @return The ``ra8_net_pal_init`` verdict.
 */
static ra8_err_t internal_net_pal_open(const uint8_t* mac)
{
  ra8_net_pal_mac_t desc = {};
  (void)memcpy((void*)desc.bytes, (const void*)mac, (size_t)k_ra8_net_pal_mac_addr_len);
  return ra8_net_pal_init(&desc);
}

/**
 * @brief Release the ESWM transport at ``NX_LINK_UNINITIALIZE``.
 */
static void internal_net_pal_close(void)
{
  (void)ra8_net_pal_deinit();
}

/**
 * @brief Report ESWM link presence for ``NX_LINK_GET_STATUS``.
 *
 * @return ``true`` when the PAL reports ``k_ra8_net_pal_link_up``.
 */
static bool internal_net_pal_link_up(void)
{
  ra8_net_pal_link_state_t state = k_ra8_net_pal_link_down;
  if (ra8_net_pal_link_status(&state) != k_ra8_ok) {
    return false;
  }
  return state == k_ra8_net_pal_link_up;
}

const nx_netif_backend_ops_t* nx_ether_driver_netif_net_pal_ops(void)
{
  static const nx_netif_backend_ops_t ops = {
    .open    = internal_net_pal_open,
    .close   = internal_net_pal_close,
    .link_up = internal_net_pal_link_up,
    .mtu     = 128U, /** ESWM egress-defect cap; see nx_ether_driver_ra8_eth.c. */
  };
  return &ops;
}
