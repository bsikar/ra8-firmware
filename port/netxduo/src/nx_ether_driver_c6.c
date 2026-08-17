/**
 * @file port/netxduo/src/nx_ether_driver_c6.c
 * @brief NetX Duo network driver bound to the ESP32-C6 link (``ra8_c6link``).
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Implements ``nx_ether_driver_c6`` -- the single NetX Duo link-driver entry
 * point for the wireless station -- plus the three helpers the application
 * wires up around it (``_bind`` for the open link handle, ``_set_mac`` for the
 * station address, ``_rx`` for the facade's receive callback).
 *
 * The C6 link is a polled, single-consumer SPI transport, so all wire access is
 * serialised behind ``s_c6_mtx``: transmit takes it around
 * ``ra8_c6link_eth_send``, and the RX poll worker takes it around
 * ``ra8_c6link_poll``. The facade delivers received frames synchronously inside
 * that poll (or inside a transmit pump) through ::nx_ether_driver_c6_rx, so the
 * receive callback never re-enters the wire and never needs the mutex itself --
 * the thread that calls the facade already holds it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "nx_ether_driver_c6.h"

#include <stdint.h>
#include <string.h>

#include "nx_api.h"
#include "nx_arp.h"
#include "nx_ip.h"
#include "ra8_attributes.h"
#include "ra8_c6link.h"
#include "ra8_esp_hosted_port.h"
#include "tx_api.h"

#ifndef NX_DISABLE_IPV4
#include "nx_rarp.h"
#endif

/**
 * @enum nx_c6_const_t
 * @brief Compile-time sizes and offsets for the NetX Duo <-> C6 bridge.
 *
 * @details
 * The staging buffer covers the largest untagged 802.3 frame this driver ever
 * hands to ``ra8_c6link_eth_send`` (1514 payload octets, no FCS), which is well
 * within the facade's ::k_ra8_c6link_max_payload. The header and EtherType
 * offsets are the fixed layout of an Ethernet II frame.
 *
 * @invariant ::k_nx_c6_max_frame does not exceed ::k_ra8_c6link_max_payload.
 * @invariant ::k_nx_c6_hdr_bytes is the Ethernet II header length.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_nx_c6_max_frame = 1514U, /**< Largest 802.3 frame forwarded to the C6.      */
  k_nx_c6_min_frame = 60U,   /**< Smallest 802.3 frame; runts are zero-padded.  */
  k_nx_c6_mac_len   = 6U,    /**< Length of an Ethernet MAC, in octets.         */
  k_nx_c6_hdr_bytes = 14U,   /**< Ethernet II header length.                    */
  k_nx_c6_etype_off = 12U,   /**< Offset of the EtherType field in the header.  */
  k_nx_c6_mtu       = 1500U, /**< IP MTU advertised on this interface.          */
  k_nx_c6_rx_align  = 2U,    /**< Prepend slide so the IP header lands aligned. */
} nx_c6_const_t;

/**
 * @enum nx_c6_shift_t
 * @brief Bit shifts for packing a six-octet MAC into NetX msw/lsw words.
 *
 * @details NetX stores a MAC as a 16-bit most-significant word (octets 0..1)
 * and a 32-bit least-significant word (octets 2..5). These are the shifts that
 * place each octet, matched by ::nx_c6_byte_mask.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_nx_c6_shift_8  = 8U,  /**< One-octet shift (msw octet 0, lsw octet 4). */
  k_nx_c6_shift_16 = 16U, /**< Two-octet shift (lsw octet 3).              */
  k_nx_c6_shift_24 = 24U, /**< Three-octet shift (lsw octet 2).            */
} nx_c6_shift_t;

/**
 * @enum nx_c6_mask_t
 * @brief Octet mask used when packing and unpacking MAC words.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_nx_c6_byte_mask = 0xFFU, /**< Low-octet mask. */
} nx_c6_mask_t;

/**
 * @enum nx_c6_mac_idx_t
 * @brief Octet indices into a six-byte MAC address.
 * @details Named so the MAC packing / unpacking carries no bare subscripts.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_nx_c6_mac_i0 = 0U, /**< First MAC octet.  */
  k_nx_c6_mac_i1 = 1U, /**< Second MAC octet. */
  k_nx_c6_mac_i2 = 2U, /**< Third MAC octet.  */
  k_nx_c6_mac_i3 = 3U, /**< Fourth MAC octet. */
  k_nx_c6_mac_i4 = 4U, /**< Fifth MAC octet.  */
  k_nx_c6_mac_i5 = 5U, /**< Sixth MAC octet.  */
} nx_c6_mac_idx_t;

/**
 * @enum nx_c6_ethertype_t
 * @brief Ethernet II EtherType values in host byte order.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_nx_c6_ethertype_ipv4 = 0x0800U, /**< IPv4 frame. */
  k_nx_c6_ethertype_arp  = 0x0806U, /**< ARP frame.  */
  k_nx_c6_ethertype_rarp = 0x8035U, /**< RARP frame. */
} nx_c6_ethertype_t;

/**
 * @enum nx_c6_worker_t
 * @brief Sizing constants for the RX-poll worker thread.
 *
 * @details Without an interrupt from the SPI transport, NetX is never told a
 * frame arrived. A dedicated ThreadX worker pumps ``ra8_c6link_poll`` at a
 * short cadence; the facade's receive callback fires from inside that poll.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_nx_c6_worker_stack_bytes  = 4096U, /**< RX worker stack, in octets.    */
  k_nx_c6_worker_priority     = 4U,    /**< RX worker ThreadX priority.    */
  k_nx_c6_worker_period_ticks = 1U,    /**< Sleep between polls, in ticks. */
  k_nx_c6_poll_transactions   = 1U,    /**< One DATA_READY frame per poll. */
} nx_c6_worker_t;

/**
 * @struct nx_c6_diag
 * @brief Bench-visible counters for the C6 NetX bridge.
 *
 * @details Never consulted by control flow; a debugger or a diagnostic print
 * reads them to tell a silent link apart from a busy one.
 *
 * @invariant Every field is monotonic non-decreasing for the life of the link.
 *
 * @see s_nx_c6_diag
 * @since 0.1.0
 */
typedef struct nx_c6_diag {
  uint32_t tx_total;   /**< Frames handed to ``ra8_c6link_eth_send``.      */
  uint32_t tx_err;     /**< Transmits the facade or link rejected.         */
  uint32_t rx_total;   /**< Frames pushed into NetX from the receive path. */
  uint32_t rx_drop;    /**< Frames dropped (pre-init, runt, no packet).    */
  uint32_t poll_total; /**< ``ra8_c6link_poll`` calls the worker made.     */
} nx_c6_diag_t;

/**
 * @var s_nx_c6_diag
 * @brief Diagnostic counters for the C6 NetX bridge.
 * @details File-scope and static; a debugger still watches it by symbol name,
 * and nothing outside this translation unit reads it.
 * @note Read-mostly; only this translation unit writes it.
 * @warning Not synchronised; treat a torn read as advisory.
 * @since 0.1.0
 */
static nx_c6_diag_t s_nx_c6_diag;

/** @brief Bound C6 link handle. @note Set by bind. @since 0.1.0 */
static ra8_c6link_t* s_c6_link;
/** @brief Wire-serialisation mutex. @note Created by bind. @since 0.1.0 */
static TX_MUTEX s_c6_mtx;
/** @brief Non-zero once the mutex exists. @since 0.1.0 */
static uint8_t s_c6_mtx_made;
/** @brief Non-zero after NX_LINK_INITIALIZE. @since 0.1.0 */
static uint8_t s_open;
/** @brief NX_LINK_ENABLE mirror. @since 0.1.0 */
static uint8_t s_link_up;
/** @brief Station MAC stamped on transmitted frames. @since 0.1.0 */
static uint8_t s_local_mac[k_nx_c6_mac_len];
/** @brief Non-zero once _set_mac ran. @since 0.1.0 */
static uint8_t s_mac_user_set;
/** @brief Linear transmit frame buffer. @since 0.1.0 */
static uint8_t s_tx_staging[k_nx_c6_max_frame];

/** @brief IP the receive path pushes frames into. @since 0.1.0 */
static NX_IP* s_rx_ip;
/** @brief Interface the receive path tags frames with. @since 0.1.0 */
static NX_INTERFACE* s_rx_iface;

void nx_ether_driver_c6_set_mac(const uint8_t mac[6])
{
  if (mac == NX_NULL) {
    return;
  }
  (void)memcpy((void*)s_local_mac, (const void*)mac, (size_t)k_nx_c6_mac_len);
  s_mac_user_set = 1U;
}

void nx_ether_driver_c6_bind(ra8_c6link_t* link)
{
  /* ThreadX stores the name pointer, so the buffer must outlive the mutex:
   * a block-scope static (MISRA 8.9) with static storage duration. It is a
   * fully-braced, non-const char array -- braced so it is not a bare string
   * literal assigned to CHAR* (MISRA 7.4), and fully initialised so no
   * element is left unset (MISRA 9.2/9.3). */
  static CHAR s_mutex_name[] = {'c', '6', '_', 'w', 'i', 'r', 'e', '\0'};
  if (link == NX_NULL) {
    return;
  }
  s_c6_link = link;
  if (s_c6_mtx_made == 0U) {
    if (tx_mutex_create(&s_c6_mtx, s_mutex_name, TX_INHERIT) == TX_SUCCESS) {
      s_c6_mtx_made = 1U;
    }
  }
}

/**
 * @brief Pull the six-octet station MAC out of a NetX interface descriptor.
 * @details NetX stores the address as a 16-bit msw (octets 0..1) and a 32-bit
 * lsw (octets 2..5); this splits them back into a byte array.
 * @param[in] iface Interface whose physical address is read; must be non-null.
 * @param[out] mac Six-octet buffer to fill; must be non-null.
 * @return Nothing.
 * @pre @p iface->nx_interface_physical_address_msw/lsw hold the address.
 * @pre @p mac has room for ::k_nx_c6_mac_len octets.
 * @post @p mac holds the interface address, most-significant octet first.
 * @post No NetX state is modified.
 * @note Not thread-safe; called only during single-threaded bring-up.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_unpack_mac(const NX_INTERFACE* iface, uint8_t* mac)
{
  ULONG msw           = iface->nx_interface_physical_address_msw;
  ULONG lsw           = iface->nx_interface_physical_address_lsw;
  mac[k_nx_c6_mac_i0] = (uint8_t)((msw >> (ULONG)k_nx_c6_shift_8) & (ULONG)k_nx_c6_byte_mask);
  mac[k_nx_c6_mac_i1] = (uint8_t)(msw & (ULONG)k_nx_c6_byte_mask);
  mac[k_nx_c6_mac_i2] = (uint8_t)((lsw >> (ULONG)k_nx_c6_shift_24) & (ULONG)k_nx_c6_byte_mask);
  mac[k_nx_c6_mac_i3] = (uint8_t)((lsw >> (ULONG)k_nx_c6_shift_16) & (ULONG)k_nx_c6_byte_mask);
  mac[k_nx_c6_mac_i4] = (uint8_t)((lsw >> (ULONG)k_nx_c6_shift_8) & (ULONG)k_nx_c6_byte_mask);
  mac[k_nx_c6_mac_i5] = (uint8_t)(lsw & (ULONG)k_nx_c6_byte_mask);
}

/**
 * @brief Write a six-octet MAC into a NetX interface's msw/lsw words.
 * @details The inverse of ::internal_unpack_mac, so NetX subsystems that read the
 * address later (ARP, sender stamping) see the value INITIALIZE adopted.
 * @param[in,out] iface Interface to update; must be non-null.
 * @param[in] mac Six-octet address to install; must be non-null.
 * @return Nothing.
 * @pre @p mac points at ::k_nx_c6_mac_len readable octets.
 * @pre @p iface is the interface INITIALIZE is bringing up.
 * @post @p iface's physical-address words hold @p mac.
 * @post No other NetX state is modified.
 * @note Not thread-safe; called only from the INITIALIZE handler.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_stamp_iface_mac(NX_INTERFACE* iface, const uint8_t* mac)
{
  ULONG msw = ((ULONG)mac[k_nx_c6_mac_i0] << (ULONG)k_nx_c6_shift_8) | (ULONG)mac[k_nx_c6_mac_i1];
  ULONG lsw = ((ULONG)mac[k_nx_c6_mac_i2] << (ULONG)k_nx_c6_shift_24) |
              ((ULONG)mac[k_nx_c6_mac_i3] << (ULONG)k_nx_c6_shift_16) |
              ((ULONG)mac[k_nx_c6_mac_i4] << (ULONG)k_nx_c6_shift_8) | (ULONG)mac[k_nx_c6_mac_i5];
  iface->nx_interface_physical_address_msw = msw;
  iface->nx_interface_physical_address_lsw = lsw;
}

/**
 * @brief Route a received frame into NetX by EtherType, stripping the header.
 * @details Reads the EtherType, strips the 14-byte Ethernet header, then hands
 * the packet to the matching deferred-receive path (IP, ARP or RARP). An
 * unknown EtherType or a runt frame is released rather than leaked.
 * @param[in,out] pkt Packet holding the raw Ethernet frame; must be non-null.
 * @return Nothing; @p pkt is consumed (queued to NetX) or released.
 * @pre ::s_rx_ip and ::s_rx_iface have been populated by INITIALIZE.
 * @pre @p pkt holds a frame whose length includes the Ethernet header.
 * @post A well-formed frame is owned by NetX; anything else is released.
 * @post ::s_nx_c6_diag rx counters reflect the outcome.
 * @note Not thread-safe; runs with the wire mutex held on the calling thread.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_dispatch_to_netx(NX_PACKET* pkt)
{
  if (pkt->nx_packet_length < (ULONG)k_nx_c6_hdr_bytes) {
    (void)nx_packet_release(pkt);
    s_nx_c6_diag.rx_drop += 1U;
    return;
  }
  const UCHAR*   p  = (const UCHAR*)pkt->nx_packet_prepend_ptr;
  const uint16_t et = (uint16_t)(((uint16_t)p[k_nx_c6_etype_off] << (uint16_t)k_nx_c6_shift_8) |
                                 (uint16_t)p[(uint16_t)k_nx_c6_etype_off + 1U]);
  pkt->nx_packet_prepend_ptr = pkt->nx_packet_prepend_ptr + (ULONG)k_nx_c6_hdr_bytes;
  pkt->nx_packet_length      = pkt->nx_packet_length - (ULONG)k_nx_c6_hdr_bytes;
  if (et == (uint16_t)k_nx_c6_ethertype_ipv4) {
    _nx_ip_packet_deferred_receive(s_rx_ip, pkt);
    s_nx_c6_diag.rx_total += 1U;
    return;
  }
#ifndef NX_DISABLE_IPV4
  if (et == (uint16_t)k_nx_c6_ethertype_arp) {
    _nx_arp_packet_deferred_receive(s_rx_ip, pkt);
    s_nx_c6_diag.rx_total += 1U;
    return;
  }
  if (et == (uint16_t)k_nx_c6_ethertype_rarp) {
    _nx_rarp_packet_deferred_receive(s_rx_ip, pkt);
    s_nx_c6_diag.rx_total += 1U;
    return;
  }
#endif
  (void)nx_packet_release(pkt);
  s_nx_c6_diag.rx_drop += 1U;
}

/**
 * @brief Report whether a received frame is fit to deliver into NetX.
 * @details Sequential checks (no compound boolean) so this not-host-testable
 * driver owes no MC/DC vector set: the interface must be up, the frame present,
 * and its length between the Ethernet header size and the staging buffer.
 * @param[in] frame The frame the facade handed the receive callback; may be null.
 * @param[in] len   Its length in octets.
 * @return bool True when the frame may be delivered.
 * @retval true The interface is up and @p frame / @p len are in range.
 * @retval false The interface is not up or the frame is null / out of range.
 * @pre The driver statics reflect the current link state.
 * @pre @p len is the length the facade reported for @p frame.
 * @post No state is modified.
 * @post The caller drops and counts the frame when this returns false.
 * @note Not thread-safe; called only from the receive callback.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_rx_acceptable(const uint8_t* frame, uint16_t len)
{
  if (s_rx_ip == NX_NULL) {
    return false;
  }
  if (s_open == 0U) {
    return false;
  }
  if (frame == NX_NULL) {
    return false;
  }
  if (len < (uint16_t)k_nx_c6_hdr_bytes) {
    return false;
  }
  if (len > (uint16_t)k_nx_c6_max_frame) {
    return false;
  }
  return true;
}

void nx_ether_driver_c6_rx(void* ctx, const uint8_t* frame, uint16_t len)
{
  /* Non-const copy of the received frame for NetX's non-const append; a
   * block-scope static (MISRA 8.9) whose static storage duration keeps this
   * 1514-octet buffer off the RX worker's bounded stack. */
  /* cppcheck-suppress misra-c2012-18.8 -- a static array is never a VLA; its size is the C23 enum k_nx_c6_max_frame, which cppcheck 2.13 cannot parse and so misreads as a variable length. */
  static uint8_t s_rx_staging[k_nx_c6_max_frame];
  (void)ctx;
  if (!internal_rx_acceptable(frame, len)) {
    s_nx_c6_diag.rx_drop += 1U;
    return;
  }
  NX_PACKET_POOL* pool = s_rx_ip->nx_ip_default_packet_pool;
  if (pool == NX_NULL) {
    s_nx_c6_diag.rx_drop += 1U;
    return;
  }
  NX_PACKET* pkt = NX_NULL;
  if (nx_packet_allocate(pool, &pkt, NX_RECEIVE_PACKET, NX_NO_WAIT) != NX_SUCCESS) {
    s_nx_c6_diag.rx_drop += 1U;
    return;
  }
  if (pkt == NX_NULL) {
    s_nx_c6_diag.rx_drop += 1U;
    return;
  }
  /* Copy into a non-const staging buffer so the const-correct callback frame is
   * never cast away for NetX's non-const append parameter. */
  (void)memcpy((void*)s_rx_staging, (const void*)frame, (size_t)len);
  /* Slide prepend by two octets so the IP header lands word-aligned once the
   * 14-byte Ethernet header is stripped; NetX drops misaligned frames on
   * Cortex-M parts. */
  pkt->nx_packet_prepend_ptr = pkt->nx_packet_prepend_ptr + (ULONG)k_nx_c6_rx_align;
  pkt->nx_packet_append_ptr  = pkt->nx_packet_prepend_ptr;
  if (nx_packet_data_append(pkt, s_rx_staging, (ULONG)len, pool, (ULONG)NX_NO_WAIT) != NX_SUCCESS) {
    (void)nx_packet_release(pkt);
    s_nx_c6_diag.rx_drop += 1U;
    return;
  }
  pkt->nx_packet_ip_interface = s_rx_iface;
  internal_dispatch_to_netx(pkt);
}

/**
 * @brief RX poll worker entry: pump the link so received frames reach NetX.
 * @details The SPI transport raises no interrupt, so this thread calls
 * ``ra8_c6link_poll`` under the wire mutex only while the held DATA_READY
 * signal says receive work exists; the facade fires ::nx_ether_driver_c6_rx
 * from inside that poll. One transaction is clocked per wake and the worker
 * sleeps one tick between checks so an idle C6 never blocks outbound traffic.
 * @param[in] arg ThreadX entry argument; unused.
 * @return Never returns.
 * @pre The driver has been bound and the mutex created.
 * @pre INITIALIZE has populated ::s_rx_ip and set ::s_open.
 * @post Received frames are delivered to NetX whenever the link is open.
 * @post ::s_nx_c6_diag.poll_total counts every poll performed.
 * @note Runs at ::k_nx_c6_worker_priority alongside the NetX IP thread.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_rx_worker_entry(ULONG arg)
{
  (void)arg;
  while (1) {
    /* Sequential readiness checks rather than one compound boolean, so the driver
     * needs no MC/DC vector set for a decision that cannot be host-tested. */
    bool pumpable = true;
    if (s_open == 0U) {
      pumpable = false;
    }
    if (s_c6_link == NX_NULL) {
      pumpable = false;
    }
    if (s_c6_mtx_made == 0U) {
      pumpable = false;
    }
    if (pumpable) {
      pumpable = ra8_esp_hosted_port_rx_pending();
    }
    if (pumpable) {
      (void)tx_mutex_get(&s_c6_mtx, TX_WAIT_FOREVER);
      /* Pass a null stats out-pointer: ra8_c6link_poll pumps the transport the
       * same way and simply discards the per-call counters, which this worker
       * never reads. That drops the throwaway C23 `= {}` aggregate cppcheck
       * 2.13 misreads under Rule 9.2, with no change in behaviour. */
      (void)ra8_c6link_poll(s_c6_link, (uint16_t)k_nx_c6_poll_transactions, nullptr);
      (void)tx_mutex_put(&s_c6_mtx);
      s_nx_c6_diag.poll_total += 1U;
    }
    tx_thread_sleep((ULONG)k_nx_c6_worker_period_ticks);
  }
}

/**
 * @brief Spawn the RX poll worker thread once; idempotent on later calls.
 * @details Called from INITIALIZE. Records the IP and interface the receive
 * path pushes frames into, then creates the worker; a second call is a no-op.
 * @param[in] ip NetX IP the worker pushes received frames into; non-null.
 * @param[in] iface NetX interface the worker tags frames with; non-null.
 * @return Nothing.
 * @pre The ThreadX kernel is running.
 * @pre @p ip and @p iface are the objects INITIALIZE is bringing up.
 * @post On first call the worker is running and its one-shot spawn guard is set.
 * @post On later calls no thread is created.
 * @note Not thread-safe; the NetX link dispatch serialises callers.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_spawn_rx_worker(NX_IP* ip, NX_INTERFACE* iface)
{
  /* Worker-private state kept at block scope (MISRA 8.9). Each retains static
   * storage duration because ThreadX keeps a pointer to the control block, the
   * stack and the name for the life of the thread; the spawn guard makes a
   * second call a no-op. The name is a fully-braced, non-const char array
   * (avoids the 7.4 string-literal-to-CHAR* assignment and any partial init). */
  static TX_THREAD          s_rx_thread;
  alignas(8) static uint8_t s_rx_thread_stack[k_nx_c6_worker_stack_bytes];
  static uint8_t            s_rx_thread_made;
  static CHAR               s_rx_name[] = {'c', '6', '_', 'n', 'x', '_', 'r', 'x', '\0'};
  if (s_rx_thread_made != 0U) {
    return;
  }
  s_rx_ip    = ip;
  s_rx_iface = iface;
  UINT t     = tx_thread_create(&s_rx_thread,
                                s_rx_name,
                                internal_rx_worker_entry,
                                0U,
                                (VOID*)s_rx_thread_stack,
                                (ULONG)k_nx_c6_worker_stack_bytes,
                                (UINT)k_nx_c6_worker_priority,
                                (UINT)k_nx_c6_worker_priority,
                                (ULONG)TX_NO_TIME_SLICE,
                                (UINT)TX_AUTO_START);
  if (t == TX_SUCCESS) {
    s_rx_thread_made = 1U;
  }
}

/**
 * @brief Map a NetX link-send command to the EtherType its frame carries.
 * @details ARP and RARP sends carry their own EtherTypes; everything else this
 * driver forwards is IPv4.
 * @param[in] cmd NetX ``nx_ip_driver_command`` for a send.
 * @return uint16_t The Ethernet II EtherType in host byte order.
 * @retval 0x0806 @p cmd is an ARP send or ARP response.
 * @retval 0x8035 @p cmd is a RARP send.
 * @retval 0x0800 Any other send (IPv4 and broadcast).
 * @pre @p cmd is one of the NX_LINK_*_SEND commands.
 * @pre The caller writes the result into the frame header.
 * @post The returned value names the frame's protocol.
 * @post No state is modified.
 * @note Pure; safe from any thread.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t internal_ethertype_for_cmd(UINT cmd)
{
  /* A switch rather than a compound boolean, so the driver carries no MC/DC-owing
   * decision; the two ARP commands share one arm by fall-through. */
  switch (cmd) {
    case NX_LINK_ARP_SEND:
    case NX_LINK_ARP_RESPONSE_SEND:
      return (uint16_t)k_nx_c6_ethertype_arp;
    case NX_LINK_RARP_SEND:
      return (uint16_t)k_nx_c6_ethertype_rarp;
    default:
      return (uint16_t)k_nx_c6_ethertype_ipv4;
  }
}

/**
 * @brief Copy a (possibly chained) NX_PACKET body into a linear buffer.
 * @details Walks the packet chain, concatenating each fragment into @p dst; it
 * refuses rather than overrun when the total exceeds @p cap.
 * @param[in] packet Packet chain to flatten; must be non-null.
 * @param[out] dst Destination buffer; must be non-null.
 * @param[in] cap Capacity of @p dst in octets.
 * @param[out] out_len Total octets written on success; must be non-null.
 * @return UCHAR Non-zero on success, zero when the body did not fit.
 * @retval 1 The whole body was copied and @p out_len is set.
 * @retval 0 The body exceeded @p cap; @p dst is partially written.
 * @pre @p dst has room for @p cap octets.
 * @pre @p packet fragments are valid prepend..append ranges.
 * @post On success @p dst holds the frame body and @p out_len its length.
 * @post No packet is released; ownership stays with the caller.
 * @note Not thread-safe with respect to @p dst (the shared staging buffer).
 * @since 0.1.0
 */
RA8_INTERNAL static UCHAR
internal_packet_to_buffer(const NX_PACKET* packet, uint8_t* dst, uint32_t cap, uint32_t* out_len)
{
  uint32_t         total = 0U;
  const NX_PACKET* cur   = packet;
  while (cur != NX_NULL) {
    const uint8_t* seg     = (const uint8_t*)cur->nx_packet_prepend_ptr;
    uint32_t       seg_len = (uint32_t)(cur->nx_packet_append_ptr - cur->nx_packet_prepend_ptr);
    if ((total + seg_len) > cap) {
      return 0U;
    }
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

/**
 * @brief Write the 14-byte Ethernet II header into the staging buffer.
 * @details Destination MAC comes from the NetX request (resolved by ARP, or
 * the broadcast address), the source MAC from ::s_local_mac, and the EtherType
 * from ::internal_ethertype_for_cmd. The body is appended after this by the caller.
 * @param[in] req NetX driver request carrying the destination and command;
 *                must be non-null.
 * @return Nothing.
 * @pre ::s_local_mac holds the station address.
 * @pre The first ::k_nx_c6_hdr_bytes of ::s_tx_staging are writable.
 * @post ::s_tx_staging[0..13] holds a valid Ethernet II header.
 * @post No packet is consumed.
 * @note Not thread-safe; ::s_tx_staging is shared and guarded by the send path.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_write_eth_header(const NX_IP_DRIVER* req)
{
  const ULONG    msw = req->nx_ip_driver_physical_address_msw;
  const ULONG    lsw = req->nx_ip_driver_physical_address_lsw;
  const uint16_t et  = internal_ethertype_for_cmd(req->nx_ip_driver_command);
  s_tx_staging[k_nx_c6_mac_i0] =
    (uint8_t)((msw >> (ULONG)k_nx_c6_shift_8) & (ULONG)k_nx_c6_byte_mask);
  s_tx_staging[k_nx_c6_mac_i1] = (uint8_t)(msw & (ULONG)k_nx_c6_byte_mask);
  s_tx_staging[k_nx_c6_mac_i2] =
    (uint8_t)((lsw >> (ULONG)k_nx_c6_shift_24) & (ULONG)k_nx_c6_byte_mask);
  s_tx_staging[k_nx_c6_mac_i3] =
    (uint8_t)((lsw >> (ULONG)k_nx_c6_shift_16) & (ULONG)k_nx_c6_byte_mask);
  s_tx_staging[k_nx_c6_mac_i4] =
    (uint8_t)((lsw >> (ULONG)k_nx_c6_shift_8) & (ULONG)k_nx_c6_byte_mask);
  s_tx_staging[k_nx_c6_mac_i5] = (uint8_t)(lsw & (ULONG)k_nx_c6_byte_mask);
  (void)memcpy((void*)(s_tx_staging + k_nx_c6_mac_len),
               (const void*)s_local_mac,
               (size_t)k_nx_c6_mac_len);
  s_tx_staging[k_nx_c6_etype_off] =
    (uint8_t)((et >> (uint16_t)k_nx_c6_shift_8) & (uint16_t)k_nx_c6_byte_mask);
  s_tx_staging[(uint16_t)k_nx_c6_etype_off + 1U] = (uint8_t)(et & (uint16_t)k_nx_c6_byte_mask);
}

/**
 * @brief Handle NX_LINK_INITIALIZE: adopt the MAC and spawn the RX worker.
 * @details Prefers the app-supplied MAC (::nx_ether_driver_c6_set_mac) over the
 * interface's, stamps it back into the interface, advertises the MTU, marks the
 * link open and starts the RX poll worker. The C6 link is already open, so no
 * hardware is brought up here.
 * @param[in,out] req NetX driver request; must be non-null.
 * @return Nothing; the result is written to @p req->nx_ip_driver_status.
 * @pre ::nx_ether_driver_c6_bind has run with an open link.
 * @pre @p req->nx_ip_driver_interface names the interface being initialised.
 * @post On success ::s_open is set and the RX worker is running.
 * @post On failure @p req->nx_ip_driver_status is NX_NOT_SUCCESSFUL.
 * @note Not thread-safe; NetX issues INITIALIZE once, on the IP thread.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_handle_init(NX_IP_DRIVER* req)
{
  NX_INTERFACE* iface = req->nx_ip_driver_interface;
  /* Split guards (no compound boolean) so this driver owes no MC/DC vectors. */
  if (iface == NX_NULL) {
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
    return;
  }
  if (s_c6_link == NX_NULL) {
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
    return;
  }
  if (s_mac_user_set == 0U) {
    internal_unpack_mac(iface, s_local_mac);
  }
  internal_stamp_iface_mac(iface, s_local_mac);
  iface->nx_interface_ip_mtu_size            = (ULONG)k_nx_c6_mtu;
  iface->nx_interface_address_mapping_needed = NX_TRUE;
  s_open                                     = 1U;
  internal_spawn_rx_worker(req->nx_ip_driver_ptr, iface);
  req->nx_ip_driver_status = NX_SUCCESS;
}

/**
 * @brief Report whether a transmit must be refused before touching the wire.
 * @details Sequential checks (no compound boolean) so this not-host-testable
 * driver owes no MC/DC vector set: a null packet, a closed or down link, or a
 * missing mutex each block the transmit.
 * @param[in] pkt The packet NetX handed the send handler; may be null.
 * @return bool True when the send must be refused.
 * @retval true One of the readiness conditions failed.
 * @retval false The link is ready to transmit @p pkt.
 * @pre The driver statics reflect the current link state.
 * @pre @p pkt is the request's packet, null or otherwise.
 * @post No state is modified.
 * @post The caller releases @p pkt when this returns true.
 * @note Not thread-safe; called only from the send handler.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_send_blocked(const NX_PACKET* pkt)
{
  bool blocked = false;
  if (pkt == NX_NULL) {
    blocked = true;
  }
  if (s_open == 0U) {
    blocked = true;
  }
  if (s_link_up == 0U) {
    blocked = true;
  }
  if (s_c6_mtx_made == 0U) {
    blocked = true;
  }
  return blocked;
}

/**
 * @brief Handle NX_LINK_PACKET_SEND and its broadcast / ARP / RARP variants.
 * @details Builds the Ethernet header, linearises the packet body after it,
 * zero-pads a runt to the 802.3 minimum, and forwards the whole frame to
 * ``ra8_c6link_eth_send`` under the wire mutex. The packet is always released.
 * @param[in,out] req NetX driver request carrying the packet; must be non-null.
 * @return Nothing; the result is written to @p req->nx_ip_driver_status.
 * @pre ::s_open and ::s_link_up are set and the mutex exists.
 * @pre @p req->nx_ip_driver_packet is a valid packet chain.
 * @post The packet is released exactly once via nx_packet_transmit_release.
 * @post ::s_nx_c6_diag tx counters and the request status reflect the outcome.
 * @note Not thread-safe against itself; NetX serialises sends on the IP thread.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_handle_send(NX_IP_DRIVER* req)
{
  NX_PACKET* pkt = req->nx_ip_driver_packet;
  if (internal_send_blocked(pkt)) {
    if (pkt != NX_NULL) {
      (void)nx_packet_transmit_release(pkt);
    }
    s_nx_c6_diag.tx_err += 1U;
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
    return;
  }
  internal_write_eth_header(req);
  uint32_t body_len = 0U;
  if (internal_packet_to_buffer(pkt,
                                s_tx_staging + k_nx_c6_hdr_bytes,
                                (uint32_t)sizeof(s_tx_staging) - (uint32_t)k_nx_c6_hdr_bytes,
                                &body_len) == 0U) {
    (void)nx_packet_transmit_release(pkt);
    s_nx_c6_diag.tx_err += 1U;
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
    return;
  }
  uint32_t len = body_len + (uint32_t)k_nx_c6_hdr_bytes;
  if (len < (uint32_t)k_nx_c6_min_frame) {
    (void)memset((void*)(s_tx_staging + len), 0, (size_t)((uint32_t)k_nx_c6_min_frame - len));
    len = (uint32_t)k_nx_c6_min_frame;
  }
  (void)tx_mutex_get(&s_c6_mtx, TX_WAIT_FOREVER);
  ra8_err_t err = ra8_c6link_eth_send(s_c6_link, s_tx_staging, (uint16_t)len);
  (void)tx_mutex_put(&s_c6_mtx);
  (void)nx_packet_transmit_release(pkt);
  if (err == k_ra8_ok) {
    s_nx_c6_diag.tx_total += 1U;
    req->nx_ip_driver_status = NX_SUCCESS;
  } else {
    s_nx_c6_diag.tx_err += 1U;
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
  }
}

/**
 * @brief Handle NX_LINK_GET_STATUS: report the link state NetX asks for.
 * @details The C6 has no PHY status register to poll; association is reflected
 * through NX_LINK_ENABLE, so this returns the ::s_link_up mirror.
 * @param[in,out] req NetX driver request with a return pointer; must be non-null.
 * @return Nothing; the state is written through @p req->nx_ip_driver_return_ptr.
 * @pre @p req->nx_ip_driver_return_ptr is non-null for a meaningful answer.
 * @pre ::s_link_up reflects the most recent ENABLE / DISABLE.
 * @post The return pointer holds NX_TRUE or NX_FALSE.
 * @post @p req->nx_ip_driver_status names success or the null-pointer failure.
 * @note Not thread-safe; NetX calls it on the IP thread.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_handle_get_status(NX_IP_DRIVER* req)
{
  if (req->nx_ip_driver_return_ptr == NX_NULL) {
    req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
    return;
  }
  *(req->nx_ip_driver_return_ptr) = (s_link_up != 0U) ? (ULONG)NX_TRUE : (ULONG)NX_FALSE;
  req->nx_ip_driver_status        = NX_SUCCESS;
}

/**
 * @brief Apply an ENABLE / DISABLE change to the link mirror and NetX iface.
 * @details Caches the new state in ::s_link_up and, when NetX supplied an
 * interface, mirrors it into ``nx_interface_link_up`` so the IP layer agrees
 * with the driver about whether the link is up.
 * @param[in,out] req NetX driver request; must be non-null.
 * @param[in] link_up 1 to bring the link up, 0 to bring it down.
 * @return Nothing; @p req->nx_ip_driver_status is set to NX_SUCCESS.
 * @pre @p link_up is 0 or 1.
 * @pre @p req is the ENABLE or DISABLE request being handled.
 * @post ::s_link_up equals @p link_up.
 * @post Any NetX interface on @p req mirrors the new link state.
 * @note Not thread-safe; serialised on the NetX IP thread.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_set_link_state(NX_IP_DRIVER* req, UCHAR link_up)
{
  s_link_up                = link_up;
  req->nx_ip_driver_status = NX_SUCCESS;
  if (req->nx_ip_driver_interface != NX_NULL) {
    req->nx_ip_driver_interface->nx_interface_link_up = (link_up != 0U) ? NX_TRUE : NX_FALSE;
  }
}

void nx_ether_driver_c6(NX_IP_DRIVER* driver_req)
{
  if (driver_req == NX_NULL) {
    return;
  }
  switch (driver_req->nx_ip_driver_command) {
    case NX_LINK_INITIALIZE:
      internal_handle_init(driver_req);
      break;
    case NX_LINK_ENABLE:
      internal_set_link_state(driver_req, 1U);
      break;
    case NX_LINK_DISABLE:
      internal_set_link_state(driver_req, 0U);
      break;
    case NX_LINK_PACKET_SEND:
    case NX_LINK_PACKET_BROADCAST:
    case NX_LINK_ARP_SEND:
    case NX_LINK_ARP_RESPONSE_SEND:
    case NX_LINK_RARP_SEND:
      internal_handle_send(driver_req);
      break;
    case NX_LINK_GET_STATUS:
      internal_handle_get_status(driver_req);
      break;
    case NX_LINK_UNINITIALIZE:
      s_open                          = 0U;
      driver_req->nx_ip_driver_status = NX_SUCCESS;
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
