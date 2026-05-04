/***********************************************************************************************************************
 * @file    port/lwip/netif/ra_etha_netif.c
 * @brief   lwIP `struct netif` glue for the on-chip RA8D2 Ethernet Agent (ETHA).
 *
 * @details
 * Implements ::ra_etha_netif_init / ::ra_etha_netif_attach_tx /
 * ::ra_etha_netif_input from `ra_etha_netif.h`. The strategy is the same
 * one used in nearly every embedded lwIP port:
 *
 *   - lwIP's `netif_add()` calls ::ra_etha_netif_init exactly once. We
 *     populate the static netif fields (name, MTU, flags, output, link-
 *     output) and stash the per-port hook table index in netif->state.
 *   - When the upper stack has a frame to send, lwIP calls
 *     `netif->linkoutput(netif, pbuf)`. We coalesce the pbuf chain into
 *     a stack-allocated scratch buffer (or, if the integrator wants
 *     zero-copy, the registered TX hook can re-walk the chain itself --
 *     here we copy because ra_etha's descriptor-ring API is opaque to
 *     this port).
 *   - When the ETHA RX path has a fresh frame, the integrator calls
 *     ::ra_etha_netif_input. We pull a pbuf from `PBUF_POOL`, copy the
 *     bytes in, and post to `tcpip_input()`.
 *
 * @par Thread-safety
 *   - ::ra_etha_netif_init / ::ra_etha_netif_attach_tx run during system
 *     bring-up, single-threaded.
 *   - The internal `internal_linkoutput` runs on tcpip_thread and is
 *     serialised by lwIP's core lock (LWIP_TCPIP_CORE_LOCKING=1 in lwipopts.h).
 *   - ::ra_etha_netif_input is called from a thread (not from hard-IRQ);
 *     `tcpip_input()` is the safe entry point.
 *
 * @par NASA Power of 10 Compliance
 * Rule 3 deviation: pbufs are pulled from lwIP's compile-time `PBUF_POOL`,
 * not malloc. Rule 9 deviation: function pointer (::ra_etha_netif_tx_fn)
 * is used to invert the dependency on the descriptor-ring strategy --
 * documented in CLAUDE.md as a SOLID/DIP exemption.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * @license   SPDX-License-Identifier: MIT
 **********************************************************************************************************************/

#include <stdint.h>
#include <string.h>

#include "lwip/opt.h"
#include "lwip/def.h"
#include "lwip/err.h"
#include "lwip/mem.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/snmp.h"
#include "netif/etharp.h"

#include "ra_err.h"
#include "ra_etha.h"
#include "ra_log.h"

#include "ra_etha_netif.h"

/* ------------------------------------------------------------------------- */
/* Frame-size limits                                                         */
/* ------------------------------------------------------------------------- */

/**
 * @enum ra_etha_netif_limits_t
 * @brief Static limits used by the netif port.
 */
typedef enum : uint16_t {
    k_ra_etha_netif_mtu          = 1500U, /**< IEEE 802.3 standard MTU.       */
    k_ra_etha_netif_max_frame    = 1518U, /**< DA+SA+TYPE+payload, no FCS.    */
    k_ra_etha_netif_min_frame    = 14U,   /**< 6+6+2 = bare Ethernet header.  */
    k_ra_etha_netif_hwaddr_len   = 6U,    /**< 48-bit MAC.                    */
    k_ra_etha_netif_tx_scratch   = 1600U, /**< Coalescing buffer for TX.      */
} ra_etha_netif_limits_t;

/**
 * @def k_ra_etha_netif_max_ports
 * @brief Number of ETHA ports the netif port supports (matches HUM Ch 32).
 */
#define RA_ETHA_NETIF_MAX_PORTS (2U)

/* Tag passed to ra_log_*. */
static const char* const s_tag = "lwip-eth";

/* ------------------------------------------------------------------------- */
/* Per-port hook table                                                       */
/* ------------------------------------------------------------------------- */

/**
 * @struct ra_etha_netif_slot_t
 * @brief Per-port netif binding (TX hook + back-pointer).
 *
 * @details
 * Indexed by ::ra_etha_port_t. We do not allocate one per netif because
 * each port can only host a single netif on this chip.
 */
typedef struct {
    struct netif*           netif; /**< Bound netif (nullptr if free).        */
    ra_etha_netif_tx_hook_t tx;    /**< TX-to-MAC hook (fn may be nullptr).   */
} ra_etha_netif_slot_t;

/**
 * @var s_slots
 * @brief Static per-port slot table.
 *
 * @details
 * One slot per ETHA port. Sized at compile time -- no malloc.
 */
static ra_etha_netif_slot_t s_slots[RA_ETHA_NETIF_MAX_PORTS];

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

/* Recover the ::ra_etha_port_t a netif was created for -- see implementation for details. */
static uint32_t internal_port_of(const struct netif* netif) {
    if (netif == NULL) {
        return RA_ETHA_NETIF_MAX_PORTS;
    }
    const uintptr_t raw = (uintptr_t)netif->state;
    if (raw >= RA_ETHA_NETIF_MAX_PORTS) {
        return RA_ETHA_NETIF_MAX_PORTS;
    }
    return (uint32_t)raw;
}

/**
 * @brief lwIP `linkoutput` trampoline.
 *
 * @details
 * Coalesces the pbuf chain into a contiguous scratch buffer and forwards
 * to the registered ::ra_etha_netif_tx_fn. Called from tcpip_thread under
 * lwIP's core lock.
 *
 * @param[in] netif Bound netif.
 * @param[in] p     pbuf chain to transmit.
 *
 * @return ::err_t
 * @retval ERR_OK         Frame handed to the MAC.
 * @retval ERR_IF         No TX hook attached, or the hook returned non-OK.
 * @retval ERR_VAL        Frame too large for the scratch buffer.
 * @retval ERR_ARG        netif/pbuf invalid.
 *
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static err_t internal_linkoutput(struct netif* netif, struct pbuf* p) {
    if (netif == NULL || p == NULL) {
        return ERR_ARG;
    }
    const uint32_t port_idx = internal_port_of(netif);
    if (port_idx >= RA_ETHA_NETIF_MAX_PORTS) {
        return ERR_ARG;
    }

    if (p->tot_len > (u16_t)k_ra_etha_netif_tx_scratch) {
        ra_log_warn_val(s_tag, "tx frame too large", (uint32_t)p->tot_len);
        return ERR_VAL;
    }

    /* Coalesce the pbuf chain. The scratch buffer is on the tcpip_thread
     * stack which the port sizes at >=12 KiB (port/threadx tx_user.h),
     * comfortably above the 1600-byte coalescing buffer used here. */
    uint8_t scratch[k_ra_etha_netif_tx_scratch];
    const u16_t copied = pbuf_copy_partial(p, scratch, p->tot_len, 0U);
    if (copied != p->tot_len) {
        return ERR_VAL;
    }

    const ra_etha_netif_slot_t* slot = &s_slots[port_idx];
    if (slot->tx.fn == NULL) {
        /* No hook attached -- silently drop and account as an error so the
         * upper stack notices via netif stats. */
        MIB2_STATS_NETIF_INC(netif, ifoutdiscards);
        return ERR_IF;
    }

    const ra_err_t rc = slot->tx.fn(slot->tx.ctx,
                                    (ra_etha_port_t)port_idx,
                                    scratch,
                                    (uint16_t)copied);
    if (rc != k_ra_ok) {
        MIB2_STATS_NETIF_INC(netif, ifoutdiscards);
        (void)ra_etha_account_traffic((ra_etha_port_t)port_idx,
                                      0U,
                                      1U,
                                      0U,
                                      0U,
                                      0U);
        return (rc == k_ra_err_busy) ? ERR_WOULDBLOCK : ERR_IF;
    }

    MIB2_STATS_NETIF_ADD(netif, ifoutoctets, copied);
    if (((scratch[0]) & 0x01U) != 0U) {
        /* Multicast / broadcast destination MAC. */
        MIB2_STATS_NETIF_INC(netif, ifoutnucastpkts);
    } else {
        MIB2_STATS_NETIF_INC(netif, ifoutucastpkts);
    }
    (void)ra_etha_account_traffic((ra_etha_port_t)port_idx,
                                  1U,
                                  0U,
                                  0U,
                                  0U,
                                  0U);
    return ERR_OK;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

/* Ra etha netif init -- see implementation for details. */
err_t ra_etha_netif_init(struct netif* netif) {
    if (netif == NULL) {
        return ERR_ARG;
    }
    const uint32_t port_idx = internal_port_of(netif);
    if (port_idx >= RA_ETHA_NETIF_MAX_PORTS) {
        ra_log_error(s_tag, "ra_etha_netif_init: invalid port in netif->state");
        return ERR_ARG;
    }

    netif->name[0] = 'r';
    netif->name[1] = 'a';

#if LWIP_NETIF_HOSTNAME
    netif->hostname = "ra8d2";
#endif

    netif->mtu        = (u16_t)k_ra_etha_netif_mtu;
    netif->hwaddr_len = (u8_t)k_ra_etha_netif_hwaddr_len;
    netif->flags      = (u8_t)(NETIF_FLAG_BROADCAST |
                               NETIF_FLAG_ETHARP    |
                               NETIF_FLAG_ETHERNET  |
                               NETIF_FLAG_LINK_UP);

#if LWIP_IPV4
    netif->output = etharp_output;
#endif
    netif->linkoutput = internal_linkoutput;

    /* MIB2: speed unknown until the PHY auto-negotiates; report 100 Mb/s
     * as a sane default. The integrator may override after PHY link-up. */
    MIB2_INIT_NETIF(netif, snmp_ifType_ethernet_csmacd, 100000000U);

    s_slots[port_idx].netif  = netif;
    s_slots[port_idx].tx.fn  = NULL;
    s_slots[port_idx].tx.ctx = NULL;

    ra_log_info_val(s_tag, "netif init", port_idx);
    return ERR_OK;
}

/* Ra etha netif attach tx -- see implementation for details. */
err_t ra_etha_netif_attach_tx(struct netif*       netif,
                              ra_etha_netif_tx_fn fn,
                              void*               ctx) {
    if (netif == NULL) {
        return ERR_ARG;
    }
    const uint32_t port_idx = internal_port_of(netif);
    if (port_idx >= RA_ETHA_NETIF_MAX_PORTS) {
        return ERR_ARG;
    }
    if (s_slots[port_idx].netif != netif) {
        return ERR_ARG;
    }
    s_slots[port_idx].tx.fn  = fn;
    s_slots[port_idx].tx.ctx = ctx;
    return ERR_OK;
}

/* Ra etha netif input -- see implementation for details. */
ra_err_t ra_etha_netif_input(struct netif* netif, const uint8_t* frame, uint16_t length) {
    if (netif == NULL || frame == NULL) {
        return k_ra_err_null_ptr;
    }
    if (length < (uint16_t)k_ra_etha_netif_min_frame ||
        length > (uint16_t)k_ra_etha_netif_max_frame) {
        return k_ra_err_invalid_arg;
    }
    const uint32_t port_idx = internal_port_of(netif);
    if (port_idx >= RA_ETHA_NETIF_MAX_PORTS) {
        return k_ra_err_invalid_arg;
    }

    struct pbuf* p = pbuf_alloc(PBUF_RAW, (u16_t)length, PBUF_POOL);
    if (p == NULL) {
        ra_log_warn(s_tag, "pbuf pool exhausted -- dropping rx frame");
        MIB2_STATS_NETIF_INC(netif, ifindiscards);
        (void)ra_etha_account_traffic((ra_etha_port_t)port_idx,
                                      0U,
                                      0U,
                                      0U,
                                      0U,
                                      1U);
        return k_ra_err_busy;
    }

    /* Walk the pbuf chain; PBUF_POOL returns one or more linked segments. */
    u16_t       offset    = 0U;
    struct pbuf* segment  = p;
    while (segment != NULL && offset < length) {
        const u16_t chunk = (u16_t)((length - offset) < segment->len
                                        ? (length - offset)
                                        : segment->len);
        (void)memcpy(segment->payload, &frame[offset], chunk);
        offset  = (u16_t)(offset + chunk);
        segment = segment->next;
    }

    MIB2_STATS_NETIF_ADD(netif, ifinoctets, length);
    if (((frame[0]) & 0x01U) != 0U) {
        MIB2_STATS_NETIF_INC(netif, ifinnucastpkts);
    } else {
        MIB2_STATS_NETIF_INC(netif, ifinucastpkts);
    }

    /* tcpip_input posts the pbuf to tcpip_thread. On failure we own the
     * pbuf and must free it. */
    if (tcpip_input(p, netif) != ERR_OK) {
        (void)pbuf_free(p);
        MIB2_STATS_NETIF_INC(netif, ifindiscards);
        (void)ra_etha_account_traffic((ra_etha_port_t)port_idx,
                                      0U,
                                      0U,
                                      0U,
                                      1U,
                                      0U);
        return k_ra_err_busy;
    }

    (void)ra_etha_account_traffic((ra_etha_port_t)port_idx,
                                  0U,
                                  0U,
                                  1U,
                                  0U,
                                  0U);
    return k_ra_ok;
}
