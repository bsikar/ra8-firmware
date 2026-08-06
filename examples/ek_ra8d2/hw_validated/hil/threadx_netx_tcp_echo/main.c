/**
 * @file examples/ek_ra8d2/hw_validated/hil/threadx_netx_tcp_echo/main.c
 * @brief ThreadX + NetX Duo TCP echo demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip up the same way ``uart_hello`` does (CGC -> SCI8 @
 * 115200 8N1) and then hands control to ThreadX. The ThreadX
 * application-define hook configures NetX Duo on top of our hardware
 * Ethernet driver shim (``nx_ether_driver_ra8_eth``):
 *
 *   1. ``nx_system_initialize()``.
 *   2. ``nx_packet_pool_create`` against a static byte pool.
 *   3. ``nx_ip_create`` at static IP 192.168.1.42 / 255.255.255.0,
 *      passing ``nx_ether_driver_ra8_eth`` as the link driver.
 *   4. ``nx_ip_address_change_notify`` is omitted -- we never DHCP.
 *   5. ``nx_arp_enable`` + ``nx_tcp_enable`` + ``nx_icmp_enable``.
 *   6. ``nx_tcp_socket_create`` + ``nx_tcp_server_socket_listen`` on
 *      port 7 (the IANA echo protocol).
 *
 * The single worker thread loops:
 *   - ``nx_tcp_server_socket_accept`` (blocking).
 *   - ``nx_tcp_socket_receive`` -> ``nx_tcp_socket_send`` echo.
 *   - On peer close, ``nx_tcp_socket_disconnect`` +
 *     ``nx_tcp_server_socket_unaccept`` +
 *     ``nx_tcp_server_socket_relisten`` and back to accept.
 *
 * Each accepted byte is logged to SCI8 as
 * ``"echoed N bytes from a.b.c.d\r\n"`` so the J-Link OB CDC port
 * gives visible feedback. NetX Duo is now the only TCP/IP stack
 * in the tree -- the hand-rolled ``ra8_net`` adapter and its
 * companion ``ethernet_tcp_echo`` / ``ethernet_udp_echo`` /
 * ``ethernet_http_responder`` apps were retired in #7.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-29
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_eth.h"
#include "ra8_isr.h"
#include "ra8_time.h"

#ifndef RA8_OFF_TARGET
#include "nx_api.h"
#include "nx_ether_driver_ra8_eth.h"
#include "tx_api.h"
#endif

/**
 * @enum demo_config_t
 * @brief Compile-time settings for the NetX Duo TCP-echo demo.
 *
 * @details
 * Every numeric used during bring-up appears here so the magic-
 * number lint never sees a bare integer. The SCI8 channel matches
 * the on-board J-Link OB CDC bridge pins (PD_02 / PD_03). The pool
 * sizing follows the standard NetX Duo demo recipe (16 packets at
 * 1568 bytes each; pool overhead allowed for) which fits easily in
 * the 2 MiB SRAM budget.
 *
 * @note ``k_demo_pool_bytes`` used to be pinned here with a warning not to
 * grow it, because growing it to 98304 moved ``s_tx_pool_storage`` to
 * 0x2201CA60 and from that address the Ethernet TX path corrupted bits 4 and
 * 5 of every byte at a frame offset congruent to 13 (mod 16) -- silently,
 * under a valid FCS, so even the 60-byte ARP reply went out with the wrong
 * sender IP and the board simply vanished off the wire (#499). That was never
 * an Ethernet defect. `SRAMWTSC.WTEN` was never programmed, so every SRAM read
 * ran with no wait state at ICLK = 250 MHz, which HUM Ch 58.3.7 p 3540 puts
 * outside guaranteed operation; the GWCA's DMA reads of this pool were simply
 * where it showed. `ra8_cgc_init` now programs it, and the 98304-byte layout
 * was re-tested on the wire at 3/3 byte-exact after the fix (3/3 corrupt
 * before it, same source, same board). The pool size is back to being an
 * ordinary sizing choice.
 */
typedef enum : uint32_t {
  k_demo_baud           = 115200U,     /**< Demo baud.                          */
  k_demo_thread_stack   = 8192U,       /**< Demo thread stack.                  */
  k_demo_ip_thread_pri  = 1U,          /**< Demo IP thread priority.            */
  k_demo_app_thread_pri = 8U,          /**< Demo app thread priority.           */
  k_demo_packet_size    = 1568U,       /**< Demo packet size.                   */
  k_demo_packet_count   = 16U,         /**< Demo packet count.                  */
  k_demo_pool_bytes     = 32768U,      /**< Packet pool (16 x 1568 + overhead). */
  k_demo_ip_stack       = 4096U,       /**< Demo IP stack.                      */
  k_demo_arp_cache      = 1024U,       /**< Demo arp cache.                     */
  k_demo_echo_port      = 7U,          /**< Demo echo port.                     */
  k_demo_recv_window    = 4096U,       /**< Demo recv window.                   */
  k_demo_socket_ttl     = 64U,         /**< Demo socket ttl.                    */
  k_demo_recv_timeout   = 0xFFFFFFFFU, /**< TX_WAIT_FOREVER on this port.       */
  k_demo_log_buf_bytes  = 80U,         /**< Demo log buffer bytes.              */
} demo_config_t;

/**
 * @enum demo_ip_octet_t
 * @brief Octet indices into a packed IPv4 address.
 */
typedef enum : uint8_t {
  k_demo_ip_oct_a = 0U, /**< Demo IP oct a. */
  k_demo_ip_oct_b = 1U, /**< Demo IP oct b. */
  k_demo_ip_oct_c = 2U, /**< Demo IP oct c. */
  k_demo_ip_oct_d = 3U, /**< Demo IP oct d. */
} demo_ip_octet_t;

/**
 * @enum demo_ip_shift_t
 * @brief Bit shifts that pack a 4-octet IPv4 into NetX's ULONG.
 */
typedef enum : uint8_t {
  k_demo_ip_shift_a = 24U, /**< Demo IP shift a. */
  k_demo_ip_shift_b = 16U, /**< Demo IP shift b. */
  k_demo_ip_shift_c = 8U,  /**< Demo IP shift c. */
  k_demo_ip_shift_d = 0U,  /**< Demo IP shift d. */
} demo_ip_shift_t;

/**
 * @enum demo_mac_word_shift_t
 * @brief Shifts that pack the 6-byte MAC into msw/lsw ULONG pairs.
 */
typedef enum : uint8_t {
  k_demo_mac_msw_shift_b0 = 8U,  /**< Demo MAC msw shift b0. */
  k_demo_mac_msw_shift_b1 = 0U,  /**< Demo MAC msw shift b1. */
  k_demo_mac_lsw_shift_b2 = 24U, /**< Demo MAC lsw shift b2. */
  k_demo_mac_lsw_shift_b3 = 16U, /**< Demo MAC lsw shift b3. */
  k_demo_mac_lsw_shift_b4 = 8U,  /**< Demo MAC lsw shift b4. */
  k_demo_mac_lsw_shift_b5 = 0U,  /**< Demo MAC lsw shift b5. */
} demo_mac_word_shift_t;

/**
 * @enum demo_decimal_t
 * @brief Constants used by the tiny base-10 byte-to-ASCII helper.
 */
typedef enum : uint8_t {
  k_demo_decimal_base = 10U, /**< Base used by ``demo_append_byte``. */
} demo_decimal_t;

/** @brief Demo IPv4 address + netmask octets and helper masks. */
typedef enum : uint16_t {
  k_demo_ipaddr_0   = 192U,  /**< 192.168.1.42                         */
  k_demo_ipaddr_1   = 168U,  /**< Demo ipaddr 1.                       */
  k_demo_ipaddr_2   = 1U,    /**< Demo ipaddr 2.                       */
  k_demo_ipaddr_3   = 42U,   /**< Demo ipaddr 3.                       */
  k_demo_netmask_b  = 255U,  /**< 255.255.255.0 (first three octets).  */
  k_demo_octet_mask = 0xFFU, /**< Mask one octet out of a packed IPv4. */
} demo_ipv4_t;

/** @brief MAC-address byte indices. */
typedef enum : uint8_t {
  k_demo_mac_idx_0 = 0U, /**< Demo MAC index 0. */
  k_demo_mac_idx_1 = 1U, /**< Demo MAC index 1. */
  k_demo_mac_idx_2 = 2U, /**< Demo MAC index 2. */
  k_demo_mac_idx_3 = 3U, /**< Demo MAC index 3. */
  k_demo_mac_idx_4 = 4U, /**< Demo MAC index 4. */
  k_demo_mac_idx_5 = 5U, /**< Demo MAC index 5. */
} demo_mac_idx_t;

static const uint8_t k_demo_mac[6] = {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U};

/** @brief IPv4 address: 192.168.1.42 / 255.255.255.0. */
static const uint8_t k_demo_ip[4] = {k_demo_ipaddr_0,
                                     k_demo_ipaddr_1,
                                     k_demo_ipaddr_2,
                                     k_demo_ipaddr_3};

/** @brief Subnet mask: 255.255.255.0. */
static const uint8_t k_demo_mask[4] = {k_demo_netmask_b, k_demo_netmask_b, k_demo_netmask_b, 0U};

#ifndef RA8_OFF_TARGET
/* NetX Duo state. ThreadX requires statically-allocated control
 * blocks (NASA Power of 10 Rule 3 -- no dynamic memory). */
static NX_PACKET_POOL s_packet_pool;
static NX_IP          s_ip;
static NX_TCP_SOCKET  s_echo_socket;
static UCHAR          s_pool_memory[k_demo_pool_bytes];
static ULONG          s_ip_stack[k_demo_ip_stack / sizeof(ULONG)];
static ULONG          s_arp_cache[k_demo_arp_cache / sizeof(ULONG)];

/* ThreadX worker thread. */
static TX_THREAD s_demo_thread;
static UCHAR     s_demo_stack[k_demo_thread_stack];
#endif /* !RA8_OFF_TARGET */

/**
 * @brief Halt forever in WFI.
 *
 * @pre Called only after a fatal error.
 * @post CPU is parked.
 *
 * @since 0.1.0
 */
static void demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC + the J-Link OB VCOM console + on-board Ethernet up.
 *
 * @pre Reset_Handler / SystemInit complete.
 * @post On success the BSP console is sending at 115200 8N1 and the
 *       on-board RGMII PHY pins + ETHA0 / RMAC0 are initialized.
 *
 * @since 0.1.0
 */
static void demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_demo_baud) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_board_ethernet_init() != k_ra8_ok) {
    demo_panic_halt();
  }
}

/**
 * @brief Convenience wrapper to write a NUL-terminated string to SCI8.
 *
 * @param[in] s NUL-terminated ASCII string. Must not be NULL.
 *
 * @pre s != NULL.
 * @post Bytes are queued in the SCI8 TX FIFO (best-effort).
 *
 * @since 0.1.0
 */
static void demo_print(const char* s)
{
  if (s == (const char*)0) {
    return;
  }
  size_t len = strlen(s);
  (void)ra8_board_uart_console_write((const uint8_t*)s, len);
}

/**
 * @brief Append a single decimal byte to the staging buffer.
 *
 * @param[in,out] buf Buffer pointer (advanced past the appended digits).
 * @param[in]     v   Byte value (0..255).
 *
 * @return Updated buffer pointer.
 *
 * @pre buf != NULL with at least 4 bytes of headroom.
 *
 * @since 0.1.0
 */
static char* demo_append_byte(char* buf, uint8_t v)
{
  /* Tiny base-10 conversion; max 3 digits + NUL. */
  char    tmp[4] = {};
  uint8_t i      = 0U;
  uint8_t value  = v;
  if (value == 0U) {
    *buf = '0';
    return buf + 1;
  }
  while (value > 0U && i < (uint8_t)sizeof(tmp)) {
    tmp[i] = (char)('0' + (char)(value % k_demo_decimal_base));
    value /= k_demo_decimal_base;
    i++;
  }
  while (i > 0U) {
    i--;
    *buf = tmp[i];
    buf++;
  }
  return buf;
}

#ifndef RA8_OFF_TARGET
/**
 * @brief Pack a 4-octet IPv4 array into NetX's host-order ULONG.
 *
 * @param[in] octets IPv4 address (4 bytes).
 *
 * @return Host-order packed address suitable for ``nx_ip_interface_address_set``.
 *
 * @since 0.1.0
 */
static ULONG demo_pack_ip(const uint8_t* octets)
{
  return (((ULONG)octets[k_demo_ip_oct_a]) << (ULONG)k_demo_ip_shift_a) |
         (((ULONG)octets[k_demo_ip_oct_b]) << (ULONG)k_demo_ip_shift_b) |
         (((ULONG)octets[k_demo_ip_oct_c]) << (ULONG)k_demo_ip_shift_c) |
         (((ULONG)octets[k_demo_ip_oct_d]) << (ULONG)k_demo_ip_shift_d);
}

/**
 * @brief Pack the local MAC into the msw/lsw fields NetX expects.
 *
 * @param[out] msw Most-significant word (octets 0-1).
 * @param[out] lsw Least-significant word (octets 2-5).
 *
 * @since 0.1.0
 */
static void demo_pack_mac(ULONG* msw, ULONG* lsw)
{
  *msw = (((ULONG)k_demo_mac[k_demo_mac_idx_0]) << (ULONG)k_demo_mac_msw_shift_b0) |
         (((ULONG)k_demo_mac[k_demo_mac_idx_1]) << (ULONG)k_demo_mac_msw_shift_b1);
  *lsw = (((ULONG)k_demo_mac[k_demo_mac_idx_2]) << (ULONG)k_demo_mac_lsw_shift_b2) |
         (((ULONG)k_demo_mac[k_demo_mac_idx_3]) << (ULONG)k_demo_mac_lsw_shift_b3) |
         (((ULONG)k_demo_mac[k_demo_mac_idx_4]) << (ULONG)k_demo_mac_lsw_shift_b4) |
         (((ULONG)k_demo_mac[k_demo_mac_idx_5]) << (ULONG)k_demo_mac_lsw_shift_b5);
}

/**
 * @brief Log "echoed N bytes from a.b.c.d" to SCI8.
 *
 * @param[in] n        Number of bytes echoed.
 * @param[in] peer_ip  Peer's IPv4 address as a host-order ULONG.
 *
 * @since 0.1.0
 */
static void demo_log_echo(ULONG n, ULONG peer_ip)
{
  char  buf[k_demo_log_buf_bytes];
  char* p = buf;
  /* "echoed " literal. */
  static const char k_prefix[] = "[netx] echoed ";
  for (uint32_t i = 0U; i < (uint32_t)(sizeof(k_prefix) - 1U); i++) {
    *p = k_prefix[i];
    p++;
  }
  /* Decimal n (clipped to 5 digits which fits k_demo_packet_size). */
  uint32_t v = (uint32_t)n;
  if (v == 0U) {
    *p = '0';
    p++;
  } else {
    char    dig[6];
    uint8_t di = 0U;
    while (v > 0U && di < (uint8_t)sizeof(dig)) {
      dig[di] = (char)('0' + (char)(v % (uint32_t)k_demo_decimal_base));
      v /= (uint32_t)k_demo_decimal_base;
      di++;
    }
    while (di > 0U) {
      di--;
      *p = dig[di];
      p++;
    }
  }
  static const char k_mid[] = " bytes from ";
  for (uint32_t i = 0U; i < (uint32_t)(sizeof(k_mid) - 1U); i++) {
    *p = k_mid[i];
    p++;
  }
  uint8_t a = (uint8_t)((peer_ip >> (ULONG)k_demo_ip_shift_a) & k_demo_octet_mask);
  uint8_t b = (uint8_t)((peer_ip >> (ULONG)k_demo_ip_shift_b) & k_demo_octet_mask);
  uint8_t c = (uint8_t)((peer_ip >> (ULONG)k_demo_ip_shift_c) & k_demo_octet_mask);
  uint8_t d = (uint8_t)((peer_ip >> (ULONG)k_demo_ip_shift_d) & k_demo_octet_mask);
  p         = demo_append_byte(p, a);
  *p        = '.';
  p++;
  p  = demo_append_byte(p, b);
  *p = '.';
  p++;
  p  = demo_append_byte(p, c);
  *p = '.';
  p++;
  p  = demo_append_byte(p, d);
  *p = '\r';
  p++;
  *p = '\n';
  p++;
  *p = '\0';
  demo_print(buf);
}

/**
 * @brief Creates the NetX packet pool and IP instance with our MAC.
 *
 * @return UINT NX_SUCCESS on success, propagated NetX error otherwise.
 * @retval NX_SUCCESS Pool and IP created; MAC pushed to link driver.
 *
 * @pre ``nx_system_initialize`` has run.
 * @pre File-scope ``s_packet_pool`` / ``s_ip`` are reserved.
 * @post On success, IP thread is running and link driver knows the MAC.
 * @post On failure, returned code names the failing API.
 *
 * @note Called once from ``demo_netx_bring_up``.
 * @since 0.1.0
 */
static UINT demo_netx_create_ip(void)
{
  UINT s = nx_packet_pool_create(&s_packet_pool,
                                 (CHAR*)"ra8_eth_pool",
                                 (ULONG)k_demo_packet_size,
                                 s_pool_memory,
                                 (ULONG)sizeof(s_pool_memory));
  if (s != NX_SUCCESS) {
    return s;
  }

  /* Hand the MAC to the link driver BEFORE nx_ip_create so it has it
   * at INITIALIZE time. nx_ip_create runs INITIALIZE synchronously on
   * the spawned IP thread, well before the main thread can call
   * nx_ip_interface_physical_address_set; that ordering was the
   * issue #1 RX-silent symptom on bench. */
  nx_ether_driver_ra8_eth_set_mac(k_demo_mac);

  ULONG ip_addr = demo_pack_ip(k_demo_ip);
  ULONG ip_mask = demo_pack_ip(k_demo_mask);
  s             = nx_ip_create(&s_ip,
                   (CHAR*)"ra8_eth_ip",
                   ip_addr,
                   ip_mask,
                   &s_packet_pool,
                   nx_ether_driver_ra8_eth,
                   (VOID*)s_ip_stack,
                   (ULONG)sizeof(s_ip_stack),
                   (UINT)k_demo_ip_thread_pri);
  if (s != NX_SUCCESS) {
    return s;
  }

  /* Belt-and-suspenders: also push the MAC through the NetX API in
   * case any subsystem reads it from the interface struct later. */
  ULONG msw = 0U;
  ULONG lsw = 0U;
  demo_pack_mac(&msw, &lsw);
  (void)nx_ip_interface_physical_address_set(&s_ip, 0U, msw, lsw, NX_TRUE);
  return NX_SUCCESS;
}

static UINT demo_netx_bring_up(void)
{
  UINT s = demo_netx_create_ip();
  if (s != NX_SUCCESS) {
    return s;
  }

  s = nx_arp_enable(&s_ip, (VOID*)s_arp_cache, (ULONG)sizeof(s_arp_cache));
  if (s != NX_SUCCESS) {
    return s;
  }
  s = nx_tcp_enable(&s_ip);
  if (s != NX_SUCCESS) {
    return s;
  }
  (void)nx_icmp_enable(&s_ip);

  /* Enable IP fragmentation so payloads larger than the capped 128-byte
   * MTU are split into in-spec frames the ESWM egress transmits cleanly
   * (the silicon corrupts single frames over ~512 bytes). Best-effort:
   * if NetX was built with NX_DISABLE_FRAGMENTATION, TCP still works via
   * the MSS clamp; only oversized ICMP/UDP would be affected. */
  (void)nx_ip_fragment_enable(&s_ip);

  s = nx_tcp_socket_create(&s_ip,
                           &s_echo_socket,
                           (CHAR*)"echo7",
                           NX_IP_NORMAL,
                           NX_FRAGMENT_OKAY,
                           (UINT)k_demo_socket_ttl,
                           (ULONG)k_demo_recv_window,
                           NX_NULL,
                           NX_NULL);
  if (s != NX_SUCCESS) {
    return s;
  }

  s = nx_tcp_server_socket_listen(&s_ip,
                                  (UINT)k_demo_echo_port,
                                  &s_echo_socket,
                                  (UINT)k_demo_packet_count,
                                  NX_NULL);
  return s;
}

/**
 * @brief Echo loop: accept -> recv -> send -> disconnect -> relisten.
 *
 * @pre ``demo_netx_bring_up`` returned ``NX_SUCCESS``.
 * @post Loop runs forever; never returns.
 *
 * @since 0.1.0
 */
static void demo_echo_loop(void)
{
  while (1) {
    UINT s = nx_tcp_server_socket_accept(&s_echo_socket, NX_WAIT_FOREVER);
    if (s != NX_SUCCESS) {
      demo_print("[netx] accept failed; relistening\r\n");
      (void)nx_tcp_server_socket_unaccept(&s_echo_socket);
      (void)nx_tcp_server_socket_relisten(&s_ip, (UINT)k_demo_echo_port, &s_echo_socket);
      continue;
    }

    while (1) {
      NX_PACKET* rx_pkt = NX_NULL;
      s                 = nx_tcp_socket_receive(&s_echo_socket, &rx_pkt, NX_WAIT_FOREVER);
      if (s != NX_SUCCESS || rx_pkt == NX_NULL) {
        break;
      }
      ULONG peer_ip   = 0U;
      ULONG peer_port = 0U;
      (void)nx_tcp_socket_peer_info_get(&s_echo_socket, &peer_ip, &peer_port);
      ULONG length = rx_pkt->nx_packet_length;
      /* Echo the packet straight back. */
      UINT ts = nx_tcp_socket_send(&s_echo_socket, rx_pkt, NX_WAIT_FOREVER);
      if (ts != NX_SUCCESS) {
        (void)nx_packet_release(rx_pkt);
      }
      demo_log_echo(length, peer_ip);
    }

    (void)nx_tcp_socket_disconnect(&s_echo_socket, NX_NO_WAIT);
    (void)nx_tcp_server_socket_unaccept(&s_echo_socket);
    (void)nx_tcp_server_socket_relisten(&s_ip, (UINT)k_demo_echo_port, &s_echo_socket);
  }
}

/**
 * @brief ThreadX worker entry: bring NetX up, then run the echo loop.
 *
 * @param[in] thread_input Unused.
 *
 * @pre ``nx_system_initialize`` has run.
 * @post Echo loop runs forever.
 *
 * @since 0.1.0
 */
static void demo_thread_entry(ULONG thread_input)
{
  (void)thread_input;
  demo_print("[netx] bringing NetX Duo up...\r\n");
  UINT s = demo_netx_bring_up();
  if (s != NX_SUCCESS) {
    demo_print("[netx] bring-up failed\r\n");
    return;
  }
  demo_print("[netx] listening on TCP/7 at 192.168.1.42\r\n");
  /* HIL probe banner -- parsed by scripts/hil/eth_tcp.sh after flashing
   * to discover the static IPv4 address the firmware responds at, plus
   * an explicit "ready" mark so the host knows to start probing. */
  demo_print("eth: ip=192.168.1.42 port=7 proto=tcp\r\n");
  demo_print("eth: ready\r\n");
  demo_echo_loop();
}

/**
 * @brief ThreadX system-define hook: build worker thread + NetX core.
 *
 * @param[in] first_unused_memory Pointer to free RAM provided by the
 *   ThreadX port; unused -- we statically allocate.
 *
 * @pre ``tx_kernel_enter()`` has been called.
 * @post One worker thread is created and NetX is initialized.
 *
 * @since 0.1.0
 */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  nx_system_initialize();

  (void)tx_thread_create(&s_demo_thread,
                         "netx_echo",
                         demo_thread_entry,
                         0U,
                         s_demo_stack,
                         (ULONG)sizeof(s_demo_stack),
                         (UINT)k_demo_app_thread_pri,
                         (UINT)k_demo_app_thread_pri,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}
#endif /* !RA8_OFF_TARGET */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up clocks + UART + RMII pins, then enters ThreadX.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @post On clean entry the kernel runs the worker thread once.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  demo_setup_or_halt();
  ra8_isr_globals_enable();
  demo_print("[netx] booting ThreadX + NetX Duo...\r\n");

#ifndef RA8_OFF_TARGET
  /* Hands control over to ThreadX permanently. */
  tx_kernel_enter();
#endif

  /* Should never return. */
  demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
