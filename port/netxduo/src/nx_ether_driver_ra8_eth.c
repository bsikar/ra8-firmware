/**
 * @file port/netxduo/src/nx_ether_driver_ra8_eth.c
 * @brief NetX Duo network driver bridging onto the RA8 ``ra8_eth`` frame API
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Implements ``nx_ether_driver_ra8_eth`` -- the single entry point
 * passed to ``nx_ip_create()`` -- by dispatching on
 * ``driver_req->nx_ip_driver_command`` into the matching
 * ``ra8_eth_*`` polled frame primitive.
 *
 * The shim is intentionally thin: every NetX Duo I/O request maps
 * 1:1 onto one ``ra8_eth_open`` / ``ra8_eth_close`` / ``ra8_eth_write``
 * / ``ra8_eth_read`` / ``ra8_eth_link_status`` call. The driver holds
 * three pieces of state of its own:
 *
 *   - ``s_ra8_eth_open`` -- whether ``ra8_eth_open`` has been called.
 *   - ``s_link_up``     -- whether ``NX_LINK_ENABLE`` was the most
 *                          recent enable / disable command.
 *   - ``s_attached_ip`` -- the NX_IP pointer captured on
 *                          ``NX_LINK_INTERFACE_ATTACH``, used by the
 *                          deferred-RX path so it can hand the
 *                          received packet to the right stack.
 *
 * NetX serialises driver invocations on the IP thread, so no
 * additional locking is required around these statics.
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "nx_ether_driver_ra8_eth.h"

#include <stdint.h>
#include <string.h>

#include "nx_api.h"
#include "nx_arp.h"
#include "nx_ip.h"
#include "ra8_err.h"
#include "ra8_eth.h"
#include "tx_api.h"

#ifndef NX_DISABLE_IPV4
#include "nx_rarp.h"
#endif

/**
 * @enum nx_ra8_eth_constants_t
 * @brief Compile-time constants for the NetX Duo <-> ``ra8_eth`` bridge.
 *
 * @details
 * The staging buffer covers the largest untagged 802.3 frame we ever
 * push to ``ra8_eth_write`` (1514 bytes payload, no FCS). The minimum
 * frame length matches ``ra8_eth``'s own ``k_ra8_eth_min_frame``.
 */
typedef enum : uint16_t {
  k_nx_ra8_eth_max_frame      = 1514U, /**< Largest 802.3 frame we forward. */
  k_nx_ra8_eth_min_frame      = 60U,   /**< Smallest 802.3 frame.           */
  k_nx_ra8_eth_mac_len        = 6U,    /**< Length of an Ethernet MAC.      */
  k_nx_ra8_eth_phys_addr_len  = 6U,    /**< Reported to NX_LINK_FACTORY_*.  */
  k_nx_ra8_eth_mtu            = 128U,  /**< Capped MTU. The EK-RA8D2 ESWM
                                       *   silently corrupts any single
                                       *   transmitted frame as it approaches
                                       *   ~512 B (a per-FRAME egress defect --
                                       *   confirmed: splitting into multiple
                                       *   descriptors does NOT help, only
                                       *   separate IP packets do). Cap the
                                       *   payload so every frame stays small;
                                       *   IP fragments larger packets and TCP
                                       *   clamps its MSS. RX is unaffected.
                                       *   Silicon-level limitation, not a
                                       *   driver bug -- see issue #21
                                       *   (accepted; needs a logic analyzer to
                                       *   pursue further). */
  k_nx_ra8_eth_phys_msw_shift = 8U,    /**< Bytes-per-byte shift for MAC msw. */
} nx_ra8_eth_constants_t;

/**
 * @enum nx_ra8_eth_mac_byte_idx_t
 * @brief Indices into the local 6-byte MAC array.
 */
typedef enum : uint8_t {
  k_nx_ra8_eth_mac_byte_0 = 0U, /**< Nx RA8 Ethernet MAC byte 0. */
  k_nx_ra8_eth_mac_byte_1 = 1U, /**< Nx RA8 Ethernet MAC byte 1. */
  k_nx_ra8_eth_mac_byte_2 = 2U, /**< Nx RA8 Ethernet MAC byte 2. */
  k_nx_ra8_eth_mac_byte_3 = 3U, /**< Nx RA8 Ethernet MAC byte 3. */
  k_nx_ra8_eth_mac_byte_4 = 4U, /**< Nx RA8 Ethernet MAC byte 4. */
  k_nx_ra8_eth_mac_byte_5 = 5U, /**< Nx RA8 Ethernet MAC byte 5. */
} nx_ra8_eth_mac_byte_idx_t;

/**
 * @enum nx_ra8_eth_mac_word_shift_t
 * @brief Bit shifts that pack the 6 MAC bytes into NetX's msw/lsw words.
 *
 * @details
 * NetX reports the local MAC as two 32-bit words: the MSW holds
 * octets 0-1 (with octet 0 in bits 8-15), the LSW holds octets 2-5
 * (with octet 2 in bits 24-31). These named shifts replace the
 * magic numbers the casts would otherwise contain.
 */
typedef enum : uint8_t {
  k_nx_ra8_eth_msw_shift_byte0 = 8U,  /**< Nx RA8 Ethernet msw shift byte0. */
  k_nx_ra8_eth_lsw_shift_byte2 = 24U, /**< Nx RA8 Ethernet lsw shift byte2. */
  k_nx_ra8_eth_lsw_shift_byte3 = 16U, /**< Nx RA8 Ethernet lsw shift byte3. */
  k_nx_ra8_eth_lsw_shift_byte4 = 8U,  /**< Nx RA8 Ethernet lsw shift byte4. */
  k_nx_ra8_eth_lsw_shift_byte5 = 0U,  /**< Nx RA8 Ethernet lsw shift byte5. */
} nx_ra8_eth_mac_word_shift_t;

/**
 * @enum nx_ra8_eth_byte_mask_t
 * @brief Low-octet mask used when extracting bytes from packed words.
 */
typedef enum : uint8_t {
  k_nx_ra8_eth_byte_mask = 0xFFU, /**< Keep the least-significant octet. */
} nx_ra8_eth_byte_mask_t;

/**
 * @enum nx_ra8_eth_hdr_offset_t
 * @brief Byte offsets of the fields in the 14-byte Ethernet header that
 *        ``priv_handle_send`` stages into ``s_tx_staging[]``.
 *
 * @details
 * The destination MAC occupies octets 0-5, the source MAC octets 6-11,
 * and the EtherType octets 12-13. Indices that fall outside the ignored
 * small-integer set are named here.
 */
typedef enum : uint8_t {
  k_nx_ra8_eth_hdr_src_mac_byte1 = 7U,  /**< Source MAC octet 1.   */
  k_nx_ra8_eth_hdr_src_mac_byte3 = 9U,  /**< Source MAC octet 3.   */
  k_nx_ra8_eth_hdr_src_mac_byte4 = 10U, /**< Source MAC octet 4.   */
  k_nx_ra8_eth_hdr_src_mac_byte5 = 11U, /**< Source MAC octet 5.   */
  k_nx_ra8_eth_hdr_ethertype_hi  = 12U, /**< EtherType high octet. */
  k_nx_ra8_eth_hdr_ethertype_lo  = 13U, /**< EtherType low octet.  */
} nx_ra8_eth_hdr_offset_t;

/**
 * @var s_tx_staging
 * @brief Contiguous TX staging buffer.
 *
 * @details
 * NetX packets may be chained across multiple ``NX_PACKET`` blocks;
 * ``ra8_eth_write`` wants a single linear buffer. We copy each
 * outgoing chain into ``s_tx_staging`` before handing it to the HAL.
 * The buffer is sized to the largest untagged frame we ever forward
 * (1514 bytes) and is statically allocated (NASA Power-of-10 Rule 3).
 *
 * @note Only safe to access from the NetX IP thread.
 *
 * @since 0.1.0
 */
static uint8_t s_tx_staging[k_nx_ra8_eth_max_frame];

/**
 * @var s_rx_staging
 * @brief Contiguous RX staging buffer.
 *
 * @details
 * ``ra8_eth_read`` returns frames into a flat buffer; we hold the
 * scratch space here and then copy into a freshly-allocated
 * ``NX_PACKET`` for the stack to consume.
 *
 * @since 0.1.0
 */
static uint8_t s_rx_staging[k_nx_ra8_eth_max_frame];

/**
 * @var s_ra8_eth_open
 * @brief Latched flag: has ``ra8_eth_open`` been called for this interface?
 *
 * @details
 * Driven by ``NX_LINK_INITIALIZE`` (true) and ``NX_LINK_UNINITIALIZE``
 * (false). The TX / RX command paths refuse to operate when this flag
 * is clear -- mirrors how NetX's reference Ethernet driver gates IO.
 *
 * @since 0.1.0
 */
static UCHAR s_ra8_eth_open;

/**
 * @var s_link_up
 * @brief Latched flag: has ``NX_LINK_ENABLE`` been the most recent transition?
 *
 * @details
 * NetX uses ``NX_LINK_ENABLE`` / ``NX_LINK_DISABLE`` as a software
 * gate independent of PHY link state; we honour it by refusing to
 * transmit (and by skipping the RX drain) when the flag is clear.
 *
 * @since 0.1.0
 */
static UCHAR s_link_up;

/**
 * @var s_local_mac
 * @brief Cached local MAC address captured at INITIALIZE time.
 *
 * @details
 * NetX exposes the MAC through the interface's
 * ``nx_interface_physical_address_msw`` / ``..._lsw`` fields. We pack
 * them back into a 6-byte array so the packed form can be handed to
 * ``ra8_eth_open``.
 *
 * @since 0.1.0
 */
static uint8_t s_local_mac[k_nx_ra8_eth_mac_len];

/**
 * @var s_local_mac_user_set
 * @brief Flag indicating whether the app supplied a MAC via the pre-init seam.
 *
 * @details Set to 1 once ``nx_ether_driver_ra8_eth_set_mac`` runs. The
 * NX_LINK_INITIALIZE handler prefers ``s_local_mac`` over the (empty)
 * interface struct when this flag is 1.
 *
 * @note Module-private; not exported.
 * @since 0.1.0
 */
static uint8_t s_local_mac_user_set;

/* nx_ether_driver_ra8_eth_set_mac -- see header for full description. */
void nx_ether_driver_ra8_eth_set_mac(const uint8_t mac[6])
{
  if (mac == nullptr) {
    return;
  }
  for (uint8_t i = 0U; i < (uint8_t)k_nx_ra8_eth_mac_len; ++i) {
    s_local_mac[i] = mac[i];
  }
  s_local_mac_user_set = 1U;
}

/**
 * @struct nx_ether_diag_t
 * @brief Bench-readable diagnostic counters for the NetX link driver.
 *
 * @details Module-internal counters incremented at every dispatch path
 * into the driver. Read from JLink memprobe / TaskOutput to confirm
 * whether NetX is actually exercising the link driver.
 *
 * @invariant Each counter is mutated only from the NetX IP thread.
 *
 * @code
 *   // JLink: mem32 &g_nx_ether_diag 12
 * @endcode
 *
 * @see g_nx_ether_diag
 * @since 0.1.0
 */
typedef struct {
  uint32_t dispatch_total;        /**< total dispatches into the driver.          */
  uint32_t init_total;            /**< NX_LINK_INITIALIZE dispatches.             */
  uint32_t enable_total;          /**< NX_LINK_ENABLE dispatches.                 */
  uint32_t send_total;            /**< NX_LINK_PACKET_SEND/_BROADCAST dispatches. */
  uint32_t rx_total;              /**< NX_LINK_DEFERRED_PROCESSING dispatches.    */
  uint32_t status_total;          /**< NX_LINK_GET_STATUS dispatches.             */
  uint32_t last_cmd;              /**< Most recent driver command code.           */
  uint32_t init_last_status;      /**< Last NetX status priv_handle_init set.     */
  uint32_t send_last_status;      /**< Last NetX status priv_handle_send set.     */
  uint32_t enable_last_link_up;   /**< 1 = last set_link_state was UP.            */
  uint32_t init_ra8_eth_open_err; /**< Last ra8_eth_open return code.             */
  uint32_t init_iface_null_hits;  /**< Times INITIALIZE saw a NULL interface.     */
} nx_ether_diag_t;

/**
 * @var g_nx_ether_diag
 * @brief Diagnostic counters readable via JLink.
 *
 * @details Symbol export is module-private (no header declaration); the
 * symbol lives in .bss so JLink memprobe can address it via `nm`.
 *
 * @note Not safe to mutate outside the NetX IP thread.
 * @warning Not part of the public ABI -- bench/debug only.
 * @since 0.1.0
 */
volatile nx_ether_diag_t g_nx_ether_diag = {};

/**
 * @enum nx_ra8_eth_worker_t
 * @brief Sizing constants for the RX-poll worker thread.
 *
 * @details Without an IRQ from the GWCA RX path, NetX is never
 * notified that frames have arrived. A dedicated ThreadX worker
 * polls ra8_eth_read at a short cadence and pushes packets into
 * NetX via _nx_ip_packet_deferred_receive. 2 ms cadence balances
 * latency against CPU load on the M85; tighter polls saturate the
 * NetX packet pool, looser ones add visible RTT to ICMP echo.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_nx_ra8_eth_worker_stack_bytes  = 4096U, /**< Nx RA8 Ethernet worker stack bytes.  */
  k_nx_ra8_eth_worker_priority     = 4U,    /**< Nx RA8 Ethernet worker priority.     */
  k_nx_ra8_eth_worker_period_ticks = 1U,    /**< Nx RA8 Ethernet worker period ticks. */
} nx_ra8_eth_worker_t;

static TX_THREAD s_rx_thread;
alignas(8) static uint8_t s_rx_thread_stack[k_nx_ra8_eth_worker_stack_bytes];
static NX_IP*        s_rx_ip          = NX_NULL;
static NX_INTERFACE* s_rx_iface       = NX_NULL;
static uint8_t       s_rx_thread_made = 0U;

/**
 * @brief RX-poll worker entry.
 *
 * @details Drains the GWCA RX descriptor ring forever, pushing each
 * frame into NetX via ``_nx_ip_packet_deferred_receive``. Sleeps one
 * ThreadX tick between drains so it does not starve other NetX work.
 *
 * @param[in] arg Unused.
 *
 * @pre s_rx_ip and s_rx_iface have been populated.
 * @pre ra8_eth_open has returned k_ra8_ok.
 * @post Loops forever; never returns.
 * @post g_nx_ether_diag.rx_total reflects every successful push to NetX.
 *
 * @note Runs at priority ``k_nx_ra8_eth_worker_priority``.
 * @since 0.1.0
 */
static void priv_rx_worker_entry(ULONG arg);

/**
 * @brief Spawn the RX-poll worker thread once, idempotent.
 *
 * @details The driver has no RX IRQ; this worker is the only path
 * from the GWCA descriptor ring into NetX. Called from INITIALIZE.
 *
 * @param[in] ip    NetX IP control block the worker pushes frames into.
 * @param[in] iface NetX interface struct the worker tags frames with.
 *
 * @pre `ip` and `iface` are non-NULL.
 * @pre ra8_eth_open succeeded (s_ra8_eth_open == 1U).
 * @post On first call, s_rx_thread is running.
 * @post On later calls, no-op.
 *
 * @note Not thread-safe; the dispatch funnel serialises callers.
 * @since 0.1.0
 */
static void priv_spawn_rx_worker(NX_IP* ip, NX_INTERFACE* iface)
{
  if (s_rx_thread_made != 0U) {
    return;
  }
  s_rx_ip    = ip;
  s_rx_iface = iface;
  UINT t     = tx_thread_create(&s_rx_thread,
                                (CHAR*)"ra8_eth_rx",
                                priv_rx_worker_entry,
                                0,
                                (VOID*)s_rx_thread_stack,
                                (ULONG)k_nx_ra8_eth_worker_stack_bytes,
                                (UINT)k_nx_ra8_eth_worker_priority,
                                (UINT)k_nx_ra8_eth_worker_priority,
                                (ULONG)TX_NO_TIME_SLICE,
                                (UINT)TX_AUTO_START);
  if (t == TX_SUCCESS) {
    s_rx_thread_made = 1U;
  }
}

/**
 * @enum nx_ra8_eth_ethertype_t
 * @brief Ethernet II EtherType constants in host byte order.
 *
 * @details NetX dispatch keys; matched against the 16-bit ethertype
 * field at frame offset 12.
 */
typedef enum : uint16_t {
  k_nx_ra8_eth_ethertype_ipv4 = 0x0800U, /**< IPv4 frame. */
  k_nx_ra8_eth_ethertype_arp  = 0x0806U, /**< ARP frame.  */
  k_nx_ra8_eth_ethertype_rarp = 0x8035U, /**< RARP frame. */
  k_nx_ra8_eth_ethertype_ipv6 = 0x86DDU, /**< IPv6 frame. */
} nx_ra8_eth_ethertype_t;

typedef enum : uint16_t {
  k_nx_ra8_eth_hdr_bytes = 14U, /**< Nx RA8 Ethernet hdr bytes. */
  k_nx_ra8_eth_etype_off = 12U, /**< Nx RA8 Ethernet etype off. */
} nx_ra8_eth_hdr_t;

/**
 * @brief Dispatch a received frame into NetX by ethertype.
 *
 * @details Mirrors the NetX reference driver's RX path: read the
 * ethertype, strip the 14-byte ethernet header, then hand the IP
 * payload to the matching `*_packet_deferred_receive`. Calling
 * `_nx_ip_packet_deferred_receive` directly routes everything to the
 * IP path and ARP frames get silently dropped, so the chip never
 * answers ``who-has`` requests.
 *
 * @param[in,out] pkt NX_PACKET containing the raw ethernet frame.
 *
 * @pre `pkt` is non-NULL and `pkt->nx_packet_length >= 14`.
 * @pre s_rx_ip and s_rx_iface have been populated.
 * @post Header is stripped and the packet is owned by NetX.
 * @post Unknown ethertypes release the packet rather than leaking.
 *
 * @note Not thread-safe; only the RX worker calls this.
 * @since 0.1.0
 */
static void priv_dispatch_to_netx(NX_PACKET* pkt)
{
  if (pkt->nx_packet_length < (ULONG)k_nx_ra8_eth_hdr_bytes) {
    (void)nx_packet_release(pkt);
    return;
  }
  const UCHAR*   p           = (const UCHAR*)pkt->nx_packet_prepend_ptr;
  const uint16_t et          = (uint16_t)(((uint16_t)p[k_nx_ra8_eth_etype_off] << 8) |
                                          (uint16_t)p[(uint16_t)k_nx_ra8_eth_etype_off + 1U]);
  pkt->nx_packet_prepend_ptr = pkt->nx_packet_prepend_ptr + (ULONG)k_nx_ra8_eth_hdr_bytes;
  pkt->nx_packet_length      = pkt->nx_packet_length - (ULONG)k_nx_ra8_eth_hdr_bytes;
  if (et == (uint16_t)k_nx_ra8_eth_ethertype_ipv4) {
    _nx_ip_packet_deferred_receive(s_rx_ip, pkt);
    return;
  }
#ifndef NX_DISABLE_IPV4
  if (et == (uint16_t)k_nx_ra8_eth_ethertype_arp) {
    _nx_arp_packet_deferred_receive(s_rx_ip, pkt);
    return;
  }
  if (et == (uint16_t)k_nx_ra8_eth_ethertype_rarp) {
    _nx_rarp_packet_deferred_receive(s_rx_ip, pkt);
    return;
  }
#endif
  (void)nx_packet_release(pkt);
}

/**
 * @brief Drain the GWCA RX ring and push every frame into NetX.
 *
 * @details One pass of the inner loop -- factored out so the worker
 * entry stays simple and the function-size gate stays happy.
 *
 * @pre s_rx_ip != NX_NULL.
 * @pre s_ra8_eth_open == 1U.
 * @post Pending RX frames have been dispatched into NetX (drops on alloc fail).
 * @post g_nx_ether_diag.rx_total reflects successful pushes.
 *
 * @note Not thread-safe; only the RX worker calls this.
 * @since 0.1.0
 */
static void priv_rx_drain(void)
{
  NX_PACKET_POOL* pool = s_rx_ip->nx_ip_default_packet_pool;
  if (pool == NX_NULL) {
    return;
  }
  while (1) {
    uint32_t  got = 0U;
    ra8_err_t err = ra8_eth_read(s_rx_staging, (uint32_t)sizeof(s_rx_staging), &got);
    if (err != k_ra8_ok) {
      break;
    }
    if (got == 0U) {
      break;
    }
    NX_PACKET* pkt = NX_NULL;
    UINT       a   = nx_packet_allocate(pool, &pkt, NX_RECEIVE_PACKET, NX_NO_WAIT);
    if (a != NX_SUCCESS) {
      continue;
    }
    if (pkt == NX_NULL) {
      continue;
    }
    /* Slide prepend by 2 bytes so the IP header lands 4-byte aligned
     * after the driver strips the 14-byte ethernet header. NetX's IP
     * receive path word-accesses the IP header and silently drops
     * misaligned frames on Cortex-M parts. */
    pkt->nx_packet_prepend_ptr = pkt->nx_packet_prepend_ptr + 2U;
    pkt->nx_packet_append_ptr  = pkt->nx_packet_prepend_ptr;
    UINT app = nx_packet_data_append(pkt, (VOID*)s_rx_staging, (ULONG)got, pool, (ULONG)NX_NO_WAIT);
    if (app != NX_SUCCESS) {
      (void)nx_packet_release(pkt);
      continue;
    }
    pkt->nx_packet_ip_interface = s_rx_iface;
    priv_dispatch_to_netx(pkt);
    g_nx_ether_diag.rx_total += 1U;
  }
}

/**
 * @brief RX-poll worker entry.
 *
 * @details Loops forever, draining the GWCA RX ring on every tick.
 *
 * @param[in] arg Unused.
 *
 * @pre Driver state has been initialised.
 * @pre s_rx_ip / s_rx_iface populated by priv_spawn_rx_worker.
 * @post Drains pending RX into NetX whenever the driver is open.
 * @post Never returns.
 *
 * @note Owns no shared state; safe to run alongside the IP thread.
 * @since 0.1.0
 */
static void priv_rx_worker_entry(ULONG arg)
{
  (void)arg;
  while (1) {
    if (s_rx_ip != NX_NULL) {
      if (s_ra8_eth_open != 0U) {
        priv_rx_drain();
      }
    }
    tx_thread_sleep((ULONG)k_nx_ra8_eth_worker_period_ticks);
  }
}

/* Pull the 6-byte MAC out of the NetX interface descriptor -- see implementation for details. */
static void priv_unpack_mac(const NX_INTERFACE* iface, uint8_t* mac)
{
  ULONG msw = iface->nx_interface_physical_address_msw;
  ULONG lsw = iface->nx_interface_physical_address_lsw;
  mac[k_nx_ra8_eth_mac_byte_0] =
    (uint8_t)((msw >> (ULONG)k_nx_ra8_eth_msw_shift_byte0) & (ULONG)k_nx_ra8_eth_byte_mask);
  mac[k_nx_ra8_eth_mac_byte_1] = (uint8_t)(msw & (ULONG)k_nx_ra8_eth_byte_mask);
  mac[k_nx_ra8_eth_mac_byte_2] =
    (uint8_t)((lsw >> (ULONG)k_nx_ra8_eth_lsw_shift_byte2) & (ULONG)k_nx_ra8_eth_byte_mask);
  mac[k_nx_ra8_eth_mac_byte_3] =
    (uint8_t)((lsw >> (ULONG)k_nx_ra8_eth_lsw_shift_byte3) & (ULONG)k_nx_ra8_eth_byte_mask);
  mac[k_nx_ra8_eth_mac_byte_4] =
    (uint8_t)((lsw >> (ULONG)k_nx_ra8_eth_lsw_shift_byte4) & (ULONG)k_nx_ra8_eth_byte_mask);
  mac[k_nx_ra8_eth_mac_byte_5] = (uint8_t)(lsw & (ULONG)k_nx_ra8_eth_byte_mask);
}

/* Linearise a (possibly-chained) NX_PACKET into the staging buffer -- see implementation for details. */
static UCHAR
priv_packet_to_buffer(const NX_PACKET* packet, uint8_t* dst, uint32_t cap, uint32_t* out_len)
{
  uint32_t         total = 0U;
  const NX_PACKET* cur   = packet;
  while (cur != NX_NULL) {
    const uint8_t* seg     = (const uint8_t*)cur->nx_packet_prepend_ptr;
    uint32_t       seg_len = (uint32_t)(cur->nx_packet_append_ptr - cur->nx_packet_prepend_ptr);
    if ((total + seg_len) > cap) {
      return 0U;
    }
    /* Copy payload bytes into the linear staging buffer. */
    (void)memcpy((void*)(dst + total), (const void*)seg, (size_t)seg_len);
    total += seg_len;
#ifndef NX_DISABLE_PACKET_CHAIN
    cur = cur->nx_packet_next;
#else
    cur = NX_NULL;
#endif
  }
  *out_len = total;
  return 1U;
}

/* Handle ``NX_LINK_INITIALIZE``: capture MAC + open the NIC -- see implementation for details. */
static void priv_handle_init(NX_IP_DRIVER* req)
{
  NX_INTERFACE* iface = req->nx_ip_driver_interface;
  if (iface == NX_NULL) {
    g_nx_ether_diag.init_iface_null_hits += 1U;
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
    return;
  }
  /* Prefer the app-supplied MAC from nx_ether_driver_ra8_eth_set_mac.
   * If the app did not call it, fall back to the interface struct
   * (typically zero during INITIALIZE -- nx_ip_create fires this
   * callback BEFORE the app can call
   * nx_ip_interface_physical_address_set). */
  if (s_local_mac_user_set == 0U) {
    priv_unpack_mac(iface, s_local_mac);
  }
  /* Push the MAC into the interface struct so NetX subsystems that
   * read it later (ARP, sender-MAC stamping) see the right value. */
  {
    ULONG msw =
      ((ULONG)s_local_mac[k_nx_ra8_eth_mac_byte_0] << (ULONG)k_nx_ra8_eth_msw_shift_byte0) |
      (ULONG)s_local_mac[k_nx_ra8_eth_mac_byte_1];
    ULONG lsw =
      ((ULONG)s_local_mac[k_nx_ra8_eth_mac_byte_2] << (ULONG)k_nx_ra8_eth_lsw_shift_byte2) |
      ((ULONG)s_local_mac[k_nx_ra8_eth_mac_byte_3] << (ULONG)k_nx_ra8_eth_lsw_shift_byte3) |
      ((ULONG)s_local_mac[k_nx_ra8_eth_mac_byte_4] << (ULONG)k_nx_ra8_eth_msw_shift_byte0) |
      (ULONG)s_local_mac[k_nx_ra8_eth_mac_byte_5];
    iface->nx_interface_physical_address_msw = msw;
    iface->nx_interface_physical_address_lsw = lsw;
  }

  /* EK-RA8D2 wires its RJ45 to ETHA1 / RMAC1 -- channel 0 is unused
   * on this board. Bench-confirmed: with channel=0 the GWCA goes
   * to OPERATION and ra8_eth_open reports k_ra8_ok but the on-silicon
   * MRGFCE/MTGFCE counters on RMAC1 stay zero because frames go to
   * the unwired ETHA0/RMAC0 port instead. Issue #1 trail. */
  ra8_eth_cfg_t cfg = {
    .mac_address        = {0U, 0U, 0U, 0U, 0U, 0U},
    .channel            = 1U,
    .num_tx_descriptors = 0U,
    .num_rx_descriptors = 0U,
    .buffer_size        = 0U,
  };
  (void)memcpy((void*)cfg.mac_address, (const void*)s_local_mac, (size_t)k_nx_ra8_eth_mac_len);

  ra8_err_t err                         = ra8_eth_open(&cfg);
  g_nx_ether_diag.init_ra8_eth_open_err = (uint32_t)err;
  if (err != k_ra8_ok) {
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
    return;
  }
  s_ra8_eth_open = 1U;
  /* Advertise NetX's framing capabilities for this interface. */
  iface->nx_interface_ip_mtu_size            = (ULONG)k_nx_ra8_eth_mtu;
  iface->nx_interface_address_mapping_needed = NX_TRUE;

  priv_spawn_rx_worker(req->nx_ip_driver_ptr, iface);
  req->nx_ip_driver_status = NX_SUCCESS;
}

/* Handle ``NX_LINK_UNINITIALIZE``: close the NIC -- see implementation for details. */
static void priv_handle_uninit(NX_IP_DRIVER* req)
{
  if (s_ra8_eth_open != 0U) {
    (void)ra8_eth_close();
    s_ra8_eth_open = 0U;
  }
  s_link_up                = 0U;
  req->nx_ip_driver_status = NX_SUCCESS;
}

/**
 * @brief Pick the ethernet ethertype for a NetX driver SEND command.
 *
 * @details NetX hands us the L3 payload (ARP body, IP packet) without
 * an ethernet header; the driver builds the header. The command kind
 * implicitly tells us which ethertype goes on the wire.
 *
 * @param[in] cmd NetX driver command code.
 * @return ethertype in host byte order.
 * @retval 0x0806  cmd was NX_LINK_ARP_SEND or NX_LINK_ARP_RESPONSE_SEND.
 * @retval 0x8035  cmd was NX_LINK_RARP_SEND.
 * @retval 0x0800  default (IPv4 -- PACKET_SEND, PACKET_BROADCAST).
 *
 * @pre Module state is consistent.
 * @pre Caller validated `cmd` came from NetX's dispatch.
 * @post Returns 0x0806/0x8035/0x0800.
 * @post No side effects.
 * @note Not thread-safe; pure.
 * @since 0.1.0
 */
static uint16_t priv_ethertype_for_cmd(UINT cmd)
{
  if (cmd == NX_LINK_ARP_SEND) {
    return (uint16_t)k_nx_ra8_eth_ethertype_arp;
  }
  if (cmd == NX_LINK_ARP_RESPONSE_SEND) {
    return (uint16_t)k_nx_ra8_eth_ethertype_arp;
  }
  if (cmd == NX_LINK_RARP_SEND) {
    return (uint16_t)k_nx_ra8_eth_ethertype_rarp;
  }
  return (uint16_t)k_nx_ra8_eth_ethertype_ipv4;
}

/**
 * @brief Handle NX_LINK_PACKET_SEND / ARP_SEND / ARP_RESPONSE_SEND / RARP_SEND.
 *
 * @details Constructs the 14-byte ethernet header from
 * `req->nx_ip_driver_physical_address_msw/lsw` (destination MAC NetX
 * resolved via ARP for IP, or the broadcast address for ARP-request
 * etc.) plus `s_local_mac` and an ethertype derived from
 * `req->nx_ip_driver_command`. The packet's prepend_ptr-to-append_ptr
 * range is then linearised after the header and fed to `ra8_eth_write`.
 *
 * @param[in,out] req NetX driver request.
 *
 * @pre `s_ra8_eth_open != 0U` and `s_link_up != 0U`.
 * @pre `req->nx_ip_driver_packet` and its chain are valid.
 * @post `nx_packet_transmit_release` is always called on the packet.
 * @post `req->nx_ip_driver_status` is NX_SUCCESS / NX_NOT_SUCCESSFUL.
 *
 * @note Not thread-safe; only the NetX IP thread calls this.
 * @since 0.1.0
 */
static void priv_handle_send(NX_IP_DRIVER* req)
{
  NX_PACKET* pkt = req->nx_ip_driver_packet;
  if (pkt == NX_NULL || s_ra8_eth_open == 0U || s_link_up == 0U) {
    if (pkt != NX_NULL) {
      (void)nx_packet_transmit_release(pkt);
    }
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
    return;
  }

  /* Build the 14-byte ethernet header in the staging buffer from the
   * destination MAC NetX puts in nx_ip_driver_physical_address_msw/lsw,
   * our local MAC, and the ethertype derived from the dispatch cmd.
   * NetX hands the link driver the L3 payload only -- the header is
   * the driver's responsibility (per `nx_arp_packet_send` and the
   * reference NetX driver). */
  const ULONG    msw = req->nx_ip_driver_physical_address_msw;
  const ULONG    lsw = req->nx_ip_driver_physical_address_lsw;
  const uint16_t et  = priv_ethertype_for_cmd(req->nx_ip_driver_command);
  s_tx_staging[k_nx_ra8_eth_mac_byte_0] =
    (uint8_t)((msw >> (ULONG)k_nx_ra8_eth_msw_shift_byte0) & (ULONG)k_nx_ra8_eth_byte_mask);
  s_tx_staging[k_nx_ra8_eth_mac_byte_1] = (uint8_t)(msw & (ULONG)k_nx_ra8_eth_byte_mask);
  s_tx_staging[k_nx_ra8_eth_mac_byte_2] =
    (uint8_t)((lsw >> (ULONG)k_nx_ra8_eth_lsw_shift_byte2) & (ULONG)k_nx_ra8_eth_byte_mask);
  s_tx_staging[k_nx_ra8_eth_mac_byte_3] =
    (uint8_t)((lsw >> (ULONG)k_nx_ra8_eth_lsw_shift_byte3) & (ULONG)k_nx_ra8_eth_byte_mask);
  s_tx_staging[k_nx_ra8_eth_mac_byte_4] =
    (uint8_t)((lsw >> (ULONG)k_nx_ra8_eth_lsw_shift_byte4) & (ULONG)k_nx_ra8_eth_byte_mask);
  s_tx_staging[k_nx_ra8_eth_mac_byte_5]        = (uint8_t)(lsw & (ULONG)k_nx_ra8_eth_byte_mask);
  s_tx_staging[6]                              = s_local_mac[k_nx_ra8_eth_mac_byte_0];
  s_tx_staging[k_nx_ra8_eth_hdr_src_mac_byte1] = s_local_mac[k_nx_ra8_eth_mac_byte_1];
  s_tx_staging[8]                              = s_local_mac[k_nx_ra8_eth_mac_byte_2];
  s_tx_staging[k_nx_ra8_eth_hdr_src_mac_byte3] = s_local_mac[k_nx_ra8_eth_mac_byte_3];
  s_tx_staging[k_nx_ra8_eth_hdr_src_mac_byte4] = s_local_mac[k_nx_ra8_eth_mac_byte_4];
  s_tx_staging[k_nx_ra8_eth_hdr_src_mac_byte5] = s_local_mac[k_nx_ra8_eth_mac_byte_5];
  s_tx_staging[k_nx_ra8_eth_hdr_ethertype_hi] =
    (uint8_t)((et >> (uint16_t)k_nx_ra8_eth_msw_shift_byte0) & (uint16_t)k_nx_ra8_eth_byte_mask);
  s_tx_staging[k_nx_ra8_eth_hdr_ethertype_lo] = (uint8_t)(et & (uint16_t)k_nx_ra8_eth_byte_mask);

  uint32_t body_len = 0U;
  if (priv_packet_to_buffer(pkt,
                            s_tx_staging + k_nx_ra8_eth_hdr_bytes,
                            (uint32_t)sizeof(s_tx_staging) - (uint32_t)k_nx_ra8_eth_hdr_bytes,
                            &body_len) == 0U) {
    (void)nx_packet_transmit_release(pkt);
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
    return;
  }
  uint32_t len = body_len + (uint32_t)k_nx_ra8_eth_hdr_bytes;

  /* Pad runt frames up to the 802.3 minimum. */
  if (len < (uint32_t)k_nx_ra8_eth_min_frame) {
    (void)memset((void*)(s_tx_staging + len), 0, (size_t)((uint32_t)k_nx_ra8_eth_min_frame - len));
    len = (uint32_t)k_nx_ra8_eth_min_frame;
  }

  ra8_err_t err = ra8_eth_write(s_tx_staging, len);
  (void)nx_packet_transmit_release(pkt);
  req->nx_ip_driver_status = (err == k_ra8_ok) ? (UINT)NX_SUCCESS : (UINT)NX_NOT_SUCCESSFUL;
}

/**
 * @brief Handle ``NX_LINK_DEFERRED_PROCESSING``: drain RX into NetX.
 *
 * @details
 * Loops calling ``ra8_eth_read`` until the descriptor ring is empty.
 * For each frame we allocate a new NX_PACKET, copy the bytes in,
 * and hand the packet to ``_nx_ip_packet_receive`` (NetX consumes
 * and eventually releases it). Allocation failures are tolerated --
 * the frame is dropped and the rx_err counter (in ``ra8_eth_stats``)
 * captures the loss.
 *
 * @param[in,out] req NetX driver request.
 *
 * @since 0.1.0
 *
 * @pre Module has been initialized.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 */
static void priv_handle_deferred_rx(NX_IP_DRIVER* req)
{
  NX_IP* ip_ptr = req->nx_ip_driver_ptr;
  if (ip_ptr == NX_NULL || s_ra8_eth_open == 0U) {
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
    return;
  }

  /* The default packet pool lives off the IP control block; NetX
   * expects RX-side allocation to come from it. */
  NX_PACKET_POOL* pool = ip_ptr->nx_ip_default_packet_pool;
  if (pool == NX_NULL) {
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
    return;
  }

  /* Drain the descriptor ring. ra8_eth_read returns k_ra8_err_no_data
   * when the ring is empty -- that is the loop exit. */
  while (1) {
    uint32_t  got = 0U;
    ra8_err_t err = ra8_eth_read(s_rx_staging, (uint32_t)sizeof(s_rx_staging), &got);
    if (err != k_ra8_ok || got == 0U) {
      break;
    }
    NX_PACKET* pkt = NX_NULL;
    UINT       a   = nx_packet_allocate(pool, &pkt, NX_RECEIVE_PACKET, NX_NO_WAIT);
    if (a != NX_SUCCESS || pkt == NX_NULL) {
      /* Packet pool exhausted; drop the frame and keep draining. */
      continue;
    }
    /* Append the bytes (this also bumps nx_packet_length). */
    UINT app = nx_packet_data_append(pkt, (VOID*)s_rx_staging, (ULONG)got, pool, (ULONG)NX_NO_WAIT);
    if (app != NX_SUCCESS) {
      (void)nx_packet_release(pkt);
      continue;
    }
    pkt->nx_packet_ip_interface = req->nx_ip_driver_interface;
    priv_dispatch_to_netx(pkt);
  }
  req->nx_ip_driver_status = NX_SUCCESS;
}

/* Handle ``NX_LINK_GET_STATUS``: report the PHY link bit -- see implementation for details. */
static void priv_handle_get_status(NX_IP_DRIVER* req)
{
  if (req->nx_ip_driver_return_ptr == NX_NULL) {
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
    return;
  }
  ra8_eth_link_t link = {.link_up = 0U, .speed_mbps = 0U, .full_duplex = 0U, .bmsr = 0U};
  if (s_ra8_eth_open == 0U) {
    *(req->nx_ip_driver_return_ptr) = (ULONG)NX_FALSE;
    req->nx_ip_driver_status        = NX_SUCCESS;
    return;
  }
  ra8_err_t err = ra8_eth_link_status(&link);
  if (err != k_ra8_ok) {
    *(req->nx_ip_driver_return_ptr) = (ULONG)NX_FALSE;
    req->nx_ip_driver_status        = NX_NOT_SUCCESSFUL;
    return;
  }
  *(req->nx_ip_driver_return_ptr) = (link.link_up != 0U) ? (ULONG)NX_TRUE : (ULONG)NX_FALSE;
  req->nx_ip_driver_status        = NX_SUCCESS;
}

/**
 * @brief Apply an ENABLE/DISABLE state change to the link mirror and NetX iface.
 *
 * @details
 * Sets the driver's ``s_link_up`` cache to ``link_up``, marks the
 * request successful, and -- when NetX provided an interface pointer
 * -- toggles ``nx_interface_link_up`` to keep NetX's IP layer in sync
 * with the driver's view.
 *
 * @param[in,out] driver_req NetX driver request; non-NULL.
 * @param[in]     link_up    1U to bring the link up, 0U to bring it down.
 *
 * @pre ``driver_req != NULL``.
 * @pre ``link_up`` is either ``0U`` (disable) or ``1U`` (enable).
 * @post ``s_link_up == link_up`` and request status is ``NX_SUCCESS``.
 * @post If ``driver_req->nx_ip_driver_interface != NULL``, the NetX
 *       interface link state mirrors ``link_up``.
 *
 * @note Serialised on the NetX IP thread; not safe to call from ISRs.
 *
 * @since 0.1.0
 */
static void priv_set_link_state(NX_IP_DRIVER* driver_req, UCHAR link_up)
{
  s_link_up                       = link_up;
  driver_req->nx_ip_driver_status = NX_SUCCESS;
  if (driver_req->nx_ip_driver_interface != NX_NULL) {
    driver_req->nx_ip_driver_interface->nx_interface_link_up = (link_up != 0U) ? NX_TRUE : NX_FALSE;
  }
}

/* Nx ether driver ra eth -- see implementation for details. */
void nx_ether_driver_ra8_eth(NX_IP_DRIVER* driver_req)
{
  /* NetX never invokes the driver with NULL but mirror the FileX
   * pattern and the upstream nx_link reference driver. */
  if (driver_req == NX_NULL) {
    return;
  }

  g_nx_ether_diag.dispatch_total += 1U;
  g_nx_ether_diag.last_cmd = (uint32_t)driver_req->nx_ip_driver_command;

  switch (driver_req->nx_ip_driver_command) {
    case NX_LINK_INITIALIZE:
      g_nx_ether_diag.init_total += 1U;
      priv_handle_init(driver_req);
      g_nx_ether_diag.init_last_status = (uint32_t)driver_req->nx_ip_driver_status;
      break;
    case NX_LINK_ENABLE:
      g_nx_ether_diag.enable_total += 1U;
      g_nx_ether_diag.enable_last_link_up = 1U;
      priv_set_link_state(driver_req, 1U);
      break;
    case NX_LINK_DISABLE:
      g_nx_ether_diag.enable_last_link_up = 0U;
      priv_set_link_state(driver_req, 0U);
      break;
    case NX_LINK_PACKET_SEND:
    case NX_LINK_PACKET_BROADCAST:
    case NX_LINK_ARP_SEND:
    case NX_LINK_ARP_RESPONSE_SEND:
    case NX_LINK_RARP_SEND:
      g_nx_ether_diag.send_total += 1U;
      priv_handle_send(driver_req);
      g_nx_ether_diag.send_last_status = (uint32_t)driver_req->nx_ip_driver_status;
      break;
    case NX_LINK_DEFERRED_PROCESSING:
      g_nx_ether_diag.rx_total += 1U;
      priv_handle_deferred_rx(driver_req);
      break;
    case NX_LINK_GET_STATUS:
      g_nx_ether_diag.status_total += 1U;
      priv_handle_get_status(driver_req);
      break;
    case NX_LINK_UNINITIALIZE:
      priv_handle_uninit(driver_req);
      break;
    case NX_LINK_MULTICAST_JOIN:
    case NX_LINK_MULTICAST_LEAVE:
    case NX_LINK_INTERFACE_ATTACH:
    case NX_LINK_INTERFACE_DETACH:
    case NX_LINK_SET_PHYSICAL_ADDRESS:
      driver_req->nx_ip_driver_status = NX_SUCCESS;
      break;
    default:
      driver_req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
      break;
  }
}
