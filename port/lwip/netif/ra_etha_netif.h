/***********************************************************************************************************************
 * @file    port/lwip/netif/ra_etha_netif.h
 * @brief   lwIP `struct netif` glue for the on-chip RA8D2 Ethernet Agent (ETHA).
 *
 * @details
 * Bridges lwIP's `struct netif` to ::ra_etha (libs/ra_hal/inc/ra_etha.h) and,
 * by extension, the per-port MAC/PHY (::ra_rmac). Exposes:
 *
 *   - ::ra_etha_netif_init  -- lwIP `netif_init_fn` to pass to `netif_add()`.
 *                              Wires up `linkoutput`, `output` (etharp), MTU,
 *                              flags, and the two-character interface name.
 *   - ::ra_etha_netif_attach_tx -- registers the function the netif calls
 *                              when it has a fully-formed Ethernet frame ready
 *                              to push onto the wire. Decouples the netif
 *                              from any specific TX descriptor strategy --
 *                              the integrator (firmware app) wires the call-
 *                              back into whichever DMA / descriptor ring it
 *                              is using on top of ra_etha.
 *   - ::ra_etha_netif_input -- called from the RX ISR / RX bottom-half once a
 *                              frame has been DMA-ed off the wire; copies the
 *                              frame into a pbuf chain pulled from lwIP's
 *                              static `PBUF_POOL` and hands it to
 *                              `tcpip_input()`.
 *
 * @par NASA Power of 10 Compliance
 * Rule 3 (no dynamic allocation) is intentionally relaxed for the vendored
 * lwIP stack; the `PBUF_POOL` referenced by ::ra_etha_netif_input is
 * compile-time sized via `PBUF_POOL_SIZE` / `PBUF_POOL_BUFSIZE` in
 * `port/lwip/lwipopts.h` -- no malloc is called.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * @license   SPDX-License-Identifier: MIT
 **********************************************************************************************************************/

#pragma once

#include <stdint.h>

#include "lwip/err.h"
#include "lwip/netif.h"

#include "ra_etha.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @typedef ra_etha_netif_tx_fn
 * @brief   Callback invoked by the netif to push a fully-formed Ethernet frame
 *          out of the MAC.
 *
 * @details
 * The netif's `linkoutput` walks the pbuf chain, copies the bytes into the
 * supplied scratch buffer (or directly into a DMA-mapped descriptor buffer
 * the integrator owns), and then calls the registered ::ra_etha_netif_tx_fn
 * once with the contiguous frame. The callback returns ::k_ra_ok on
 * successful enqueue; any non-OK return is mapped to lwIP's `ERR_IF`.
 *
 * @param[in] ctx     Opaque cookie supplied at attach time.
 * @param[in] port    ETHA port the netif is bound to.
 * @param[in] frame   Pointer to the Ethernet frame (DA + SA + EtherType + payload).
 * @param[in] length  Frame length in bytes (excluding FCS, which the MAC adds).
 *
 * @return ::ra_err_t
 * @retval k_ra_ok               Frame accepted by the MAC / DMA ring.
 * @retval k_ra_err_busy         TX ring full, retry later (mapped to ERR_WOULDBLOCK).
 * @retval k_ra_err_invalid_arg  frame is nullptr or length out of bounds.
 *
 * @note Re-entrancy: lwIP serialises calls into linkoutput at the netif
 *       level; the callback need not be IRQ-safe but must be safe to call
 *       from the tcpip_thread context.
 *
 * @see ra_etha_netif_attach_tx
 * @since 0.1.0
 */
typedef ra_err_t (*ra_etha_netif_tx_fn)(void*          ctx,
                                        ra_etha_port_t port,
                                        const uint8_t* frame,
                                        uint16_t       length);

/**
 * @struct ra_etha_netif_tx_hook_t
 * @brief Registration record bound to a netif via ::ra_etha_netif_attach_tx.
 */
typedef struct {
    ra_etha_netif_tx_fn fn;  /**< Callback (nullptr -> linkoutput drops). */
    void*               ctx; /**< Opaque cookie passed back to fn.        */
} ra_etha_netif_tx_hook_t;

/**
 * @brief lwIP `netif_init_fn` for the RA8D2 ETHA.
 *
 * @details
 * Pass this as the last argument to `netif_add()`. The function fills in:
 *   - `netif->name        = { 'r', 'a' }`
 *   - `netif->mtu         = 1500`
 *   - `netif->flags       = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
 *                           NETIF_FLAG_ETHERNET  | NETIF_FLAG_LINK_UP`
 *   - `netif->output      = etharp_output`
 *   - `netif->linkoutput  = internal TX trampoline`
 *   - `netif->hwaddr_len  = 6`
 *
 * The caller must populate `netif->hwaddr` (the MAC) and `netif->state`
 * (which must point to a ::ra_etha_port_t coerced through `(void*)(uintptr_t)`)
 * BEFORE calling `netif_add(.., ra_etha_netif_init, ..)`.
 *
 * @param[in,out] netif lwIP netif being initialised.
 *
 * @return ::err_t
 * @retval ERR_OK   netif initialised.
 * @retval ERR_ARG  netif is nullptr or `netif->state` carries an invalid port.
 *
 * @pre `netif->state` is `(void*)(uintptr_t)k_ra_etha_port_0` or `_1`.
 * @pre `netif->hwaddr[0..5]` already populated with the MAC for this port.
 * @post `netif->name`, `netif->mtu`, `netif->flags` set as documented.
 * @post `netif->linkoutput` and `netif->output` installed.
 *
 * @note Not thread-safe; call once per netif during system bring-up before
 *       handing the netif to `netif_set_default()` / `netif_set_up()`.
 *
 * @par Example:
 * @code
 * static struct netif s_netif;
 * static ra_etha_port_t s_port = k_ra_etha_port_0;
 * netif_add(&s_netif,
 *           &ip_addr, &netmask, &gateway,
 *           (void*)(uintptr_t)s_port,
 *           ra_etha_netif_init,
 *           tcpip_input);
 * netif_set_default(&s_netif);
 * netif_set_up(&s_netif);
 * @endcode
 *
 * @see ra_etha_netif_attach_tx
 * @see ra_etha_netif_input
 * @since 0.1.0
 */
err_t ra_etha_netif_init(struct netif* netif);

/**
 * @brief Register the TX-to-MAC callback for a netif.
 *
 * @details
 * Decouples the netif from the descriptor-ring strategy. The integrator
 * calls this once after `netif_add()` returns, supplying a function that
 * pushes a contiguous Ethernet frame into the chosen TX path (DMA descriptor
 * ring, or a polled MAC FIFO write, etc.).
 *
 * @param[in,out] netif Netif previously initialised via ::ra_etha_netif_init.
 * @param[in]     fn    TX callback (nullptr to detach).
 * @param[in]     ctx   Opaque cookie passed to fn on each invocation.
 *
 * @return ::err_t
 * @retval ERR_OK   Hook installed (or cleared).
 * @retval ERR_ARG  netif is nullptr or was not initialised by this port.
 *
 * @pre netif initialised via ::ra_etha_netif_init.
 * @pre Caller owns the lifetime of ctx for as long as fn is registered.
 * @post Subsequent calls into `netif->linkoutput` route through fn.
 * @post Previous hook (if any) is no longer reachable.
 *
 * @note Not thread-safe; install before unmasking the ETHA RX IRQ.
 * @see ra_etha_netif_init
 * @since 0.1.0
 */
err_t ra_etha_netif_attach_tx(struct netif*       netif,
                              ra_etha_netif_tx_fn fn,
                              void*               ctx);

/**
 * @brief Inject an RX frame into lwIP from the ETHA RX path.
 *
 * @details
 * Allocates a pbuf chain from the static `PBUF_POOL`, copies the supplied
 * frame into it, and hands the pbuf to `tcpip_input()` so it is processed
 * on the tcpip_thread. Returns ::k_ra_err_busy when the pool is exhausted;
 * the caller should bump the netif drop counter and free the source buffer
 * normally.
 *
 * Safe to call from the ETHA RX bottom-half (deferred-from-ISR thread).
 * Direct invocation from hard-IRQ context is NOT supported because
 * `tcpip_input()` posts to the lwIP mailbox via `sys_mbox_trypost`, which
 * is itself ISR-safe but the surrounding pbuf pool accessor takes
 * `SYS_ARCH_PROTECT()` which is a TX_MUTEX in this port -- not ISR-callable.
 *
 * @param[in] netif  Netif previously initialised via ::ra_etha_netif_init.
 * @param[in] frame  Pointer to the Ethernet frame bytes.
 * @param[in] length Frame length in bytes (without FCS).
 *
 * @return ::ra_err_t
 * @retval k_ra_ok               Frame copied into a pbuf and queued to tcpip_input.
 * @retval k_ra_err_null_ptr     netif or frame is nullptr.
 * @retval k_ra_err_invalid_arg  length == 0 or length > 1518 (max Ethernet w/o jumbo).
 * @retval k_ra_err_busy         PBUF_POOL exhausted; frame dropped.
 *
 * @pre netif initialised via ::ra_etha_netif_init.
 * @pre Called from a ThreadX thread (NOT from hard-IRQ context).
 * @post On success the frame is owned by lwIP; the caller may free its source
 *       buffer immediately.
 * @post On failure the source buffer is unchanged and must be freed by caller.
 *
 * @note Per-port traffic counters are bumped via ::ra_etha_account_traffic.
 * @see ra_etha_netif_init
 * @since 0.1.0
 */
ra_err_t ra_etha_netif_input(struct netif* netif, const uint8_t* frame, uint16_t length);

#ifdef __cplusplus
}
#endif
