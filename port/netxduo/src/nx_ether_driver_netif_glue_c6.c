/**
 * @file port/netxduo/src/nx_ether_driver_netif_glue_c6.c
 * @brief c6link backend ops for the unified NetX Duo driver.
 *
 * [Ring 4 / PORT] {World: NS}
 *
 * Supplies ::nx_ether_driver_netif_c6_ops, the ::nx_netif_backend_ops_t the
 * unified driver binds when an app wants the ESP32-C6 link. The app opens
 * the ``ra8_c6link`` and runs a seam backend binder
 * (``ra8_io_netif_bind_c6link``) before ``nx_ip_create`` -- exactly the
 * ``nx_ether_driver_c6_bind`` contract -- so backend bring-up is a no-op
 * and teardown/link queries are left NULL: the driver then keeps its
 * ``NX_LINK_ENABLE`` mirror as the GET_STATUS answer, matching what
 * ``nx_ether_driver_c6`` does today.
 *
 * Owns a per-backend translation unit on purpose: an app that never binds
 * the c6 backend never compiles (or links) this file.
 */
#include "nx_ether_driver_netif.h"

/**
 * @brief No-op bring-up: the app opened the c6 link before binding.
 *
 * @param[in] mac Unused; the MAC flows to the stack through the iface
 *                stamp, not through the transport.
 * @return Always ``k_ra8_ok``.
 */
static ra8_err_t internal_c6_open(const uint8_t* mac)
{
  (void)mac;
  return k_ra8_ok;
}

const nx_netif_backend_ops_t* nx_ether_driver_netif_c6_ops(void)
{
  static const nx_netif_backend_ops_t ops = {
    .open    = internal_c6_open,
    .close   = nullptr,
    .link_up = nullptr,
    .mtu     = 1514U,
  };
  return &ops;
}
