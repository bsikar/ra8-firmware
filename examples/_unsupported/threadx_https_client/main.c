/**
 * @file examples/_unsupported/threadx_https_client/main.c
 * @brief ThreadX + NetX Duo + Mbed TLS HTTPS client demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip up the same way ``threadx_netx_tcp_echo`` does (CGC
 * -> SCI8 @ 115200 8N1, RMII pins routed) and then hands control to
 * ThreadX. The single worker thread:
 *
 *   1. ``nx_system_initialize()`` + packet pool / IP / ARP / TCP /
 *      ICMP enable on the static address 192.168.1.42 / 24.
 *   2. Initialises ``ra8_rsip`` for its TRNG entropy source. The
 *      RSIP-E50D AES / SHA-256 engines are not functional on this
 *      silicon, so all handshake crypto runs in Mbed TLS software.
 *   3. Seeds Mbed TLS's CTR_DRBG from ``ra8_rsip_trng_read``.
 *   4. Opens a NetX TCP socket to ``www.example.com:443`` (static IP
 *      ``93.184.216.34`` so the demo runs without DNS).
 *   5. Wires Mbed TLS's BIO callbacks to ``nx_tcp_socket_send`` /
 *      ``nx_tcp_socket_receive`` -- both block on a ThreadX timer.
 *   6. Runs the TLS handshake. On success, computes SHA-256 of the
 *      peer's leaf certificate DER and compares against the
 *      compile-time pin ``k_demo_cert_pin_sha256``. A mismatch is a
 *      fatal error -- we will not send the HTTP request.
 *   7. Sends ``GET / HTTP/1.1\r\nHost: www.example.com\r\n\r\n``.
 *   8. Reads the response, prefixes ``[https]`` to every line, and
 *      dumps the first ``k_demo_dump_bytes`` of the body to SCI8.
 *
 * The demo is deliberately single-shot: after the response is
 * dumped (or any step fails), the worker thread parks. Re-flashing
 * is the easiest way to re-run the demo.
 *
 * @par Test recipe (lab):
 *
 *   1. Plug an Ethernet cable from EK-RA8D2 J64 to the workstation.
 *      Bridge / NAT the workstation interface to the public Internet
 *      so the board can reach 93.184.216.34:443 -- a typical setup
 *      is ``sudo ip link set ... main br0`` on Linux or sharing the
 *      Wi-Fi adapter on macOS.
 *   2. Open a serial terminal on the J-Link OB CDC port at 115200
 *      8N1, e.g. ``picocom -b 115200 /dev/cu.usbmodem0001234567891``.
 *   3. ``make threadx_https_client && make -C examples/threadx_https_client flash``.
 *   4. Reset the board. Within ~3 seconds you should see:
 *      ``[https] handshake OK, cert pin matches, dumping body``
 *      followed by the first 1 KiB of www.example.com.
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
#include "ra8_rsip.h"
#include "ra8_time.h"

#ifndef RA8_OFF_TARGET
/* Mbed TLS 4.x relocated the legacy crypto primitive headers under
 * `mbedtls/private/`. The TLS / X.509 layer headers stay at the
 * public top level. */
#include "mbedtls/net_sockets.h"
#include "mbedtls/platform.h"
#include "mbedtls/private/ctr_drbg.h"
#include "mbedtls/private/entropy.h"
#include "mbedtls/private/sha256.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "nx_api.h"
#include "nx_ether_driver_ra8_eth.h"
#include "psa/crypto.h"
#include "tx_api.h"
#endif

/**
 * @enum demo_config_t
 * @brief Compile-time settings for the HTTPS client demo.
 *
 * @details
 * Mirrors the ``threadx_netx_tcp_echo`` sizing for NetX Duo;
 * the additional Mbed TLS state is sized inside the per-thread
 * stack and the byte pool below.
 */
typedef enum : uint32_t {
  k_demo_baud           = 115200U, /**< Demo baud.                                    */
  k_demo_thread_stack   = 16384U,  /**< Demo thread stack.                            */
  k_demo_ip_thread_pri  = 1U,      /**< Demo IP thread priority.                      */
  k_demo_app_thread_pri = 8U,      /**< Demo app thread priority.                     */
  k_demo_packet_size    = 1568U,   /**< Demo packet size.                             */
  k_demo_packet_count   = 16U,     /**< Demo packet count.                            */
  k_demo_pool_bytes     = 32768U,  /**< Demo pool bytes.                              */
  k_demo_byte_pool_size = 65536U,  /**< Demo byte pool size.                          */
  k_demo_ip_stack       = 4096U,   /**< Demo IP stack.                                */
  k_demo_arp_cache      = 1024U,   /**< Demo arp cache.                               */
  k_demo_https_port     = 443U,    /**< Demo HTTPS port.                              */
  k_demo_recv_window    = 4096U,   /**< Demo recv window.                             */
  k_demo_socket_ttl     = 64U,     /**< Demo socket ttl.                              */
  k_demo_recv_timeout   = 200U,    /**< Ticks; ~2 s at TX_TIMER_TICKS_PER_SECOND=100. */
  k_demo_handshake_max  = 3000U,   /**< Max ticks waiting for handshake.              */
  k_demo_log_buf_bytes  = 80U,     /**< Demo log buffer bytes.                        */
  k_demo_dump_bytes     = 1024U,   /**< First 1 KiB of body to SCI8.                  */
  k_demo_request_buf    = 256U,    /**< Demo request buffer.                          */
  k_demo_response_buf   = 4096U,   /**< Demo response buffer.                         */
  k_demo_drbg_seed_len  = 32U,     /**< Bytes pulled from RSIP TRNG.                  */
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

/** @brief Demo IPv4 addresses (board, gateway, host) as octet enums. */
typedef enum : uint16_t {
  k_demo_ipaddr_0  = 192U, /**< 192.168.1.42 board.                 */
  k_demo_ipaddr_1  = 168U, /**< Demo ipaddr 1.                      */
  k_demo_ipaddr_2  = 1U,   /**< Demo ipaddr 2.                      */
  k_demo_ipaddr_3  = 42U,  /**< Demo ipaddr 3.                      */
  k_demo_netmask_b = 255U, /**< 255.255.255.0 (first three octets). */
  k_demo_hostip_0  = 93U,  /**< 93.184.216.34 (www.example.com).    */
  k_demo_hostip_1  = 184U, /**< Demo hostip 1.                      */
  k_demo_hostip_2  = 216U, /**< Demo hostip 2.                      */
  k_demo_hostip_3  = 34U,  /**< Demo hostip 3.                      */
} demo_ipv4_t;

/** @brief ASCII control bytes used by the HTTP line scanner. */
typedef enum : uint8_t {
  k_ascii_cr = 0x0DU, /**< Carriage return. */
  k_ascii_lf = 0x0AU, /**< Line feed.       */
} http_ascii_t;

/** @brief MAC-address byte indices. */
typedef enum : uint8_t {
  k_demo_mac_idx_0 = 0U, /**< Demo MAC index 0. */
  k_demo_mac_idx_1 = 1U, /**< Demo MAC index 1. */
  k_demo_mac_idx_2 = 2U, /**< Demo MAC index 2. */
  k_demo_mac_idx_3 = 3U, /**< Demo MAC index 3. */
  k_demo_mac_idx_4 = 4U, /**< Demo MAC index 4. */
  k_demo_mac_idx_5 = 5U, /**< Demo MAC index 5. */
} demo_mac_idx_t;

static const uint8_t k_demo_mac[6] = {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U};

/** @brief IPv4 address: 192.168.1.42 / 255.255.255.0. */
static const uint8_t k_demo_ip[4] = {k_demo_ipaddr_0,
                                     k_demo_ipaddr_1,
                                     k_demo_ipaddr_2,
                                     k_demo_ipaddr_3};

/** @brief Subnet mask: 255.255.255.0. */
static const uint8_t k_demo_mask[4] = {k_demo_netmask_b, k_demo_netmask_b, k_demo_netmask_b, 0U};

/** @brief Default gateway: 192.168.1.1. */
static const uint8_t k_demo_gw[4] = {k_demo_ipaddr_0, k_demo_ipaddr_1, k_demo_ipaddr_2, 1U};

/**
 * @brief Static IPv4 for ``www.example.com`` (legacy edge-of-net IP).
 *
 * @details
 * Pinned to 93.184.216.34 -- the historic ``www.example.com`` IP.
 * If the address rotates, override at compile time with
 * ``-DDEMO_HOST_IPV4_OVERRIDE=0x5DB8D822UL`` etc., or replace this
 * static lookup with a NetX DNS resolver in a future sweep.
 */
static const uint8_t k_demo_host_ip[4] = {k_demo_hostip_0,
                                          k_demo_hostip_1,
                                          k_demo_hostip_2,
                                          k_demo_hostip_3};

/** @brief HTTP/1.1 ``Host`` header literal -- matches the IP pin above. */
static const char k_demo_host_name[] = "www.example.com";

/**
 * @brief SHA-256 of the expected leaf certificate DER, compile-time pinned.
 *
 * @details
 * Update this constant whenever ``www.example.com`` rotates its
 * leaf certificate. Mismatch is treated as a fatal handshake
 * failure -- we will not send the HTTP request. To capture a fresh
 * pin from a development host:
 *
 *   ``echo | openssl s_client -connect www.example.com:443 \
 *     -servername www.example.com 2>/dev/null \
 *     | openssl x509 -outform der | openssl dgst -sha256``
 *
 * The 32-byte digest goes here byte-for-byte. The placeholder below
 * is all-zero and will deliberately fail the pin check until the
 * value is filled in -- callers can disable pinning during bring-up
 * by defining ``DEMO_DISABLE_CERT_PIN``.
 */
static const uint8_t k_demo_cert_pin_sha256[32] = {
  0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
  0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
  0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};

#ifndef RA8_OFF_TARGET
/* NetX Duo state. ThreadX requires statically-allocated control
 * blocks (NASA Power of 10 Rule 3 -- no dynamic memory). */
static NX_PACKET_POOL s_packet_pool;
static NX_IP          s_ip;
static NX_TCP_SOCKET  s_tls_socket;
static UCHAR          s_pool_memory[k_demo_pool_bytes];
static ULONG          s_ip_stack[k_demo_ip_stack / sizeof(ULONG)];
static ULONG          s_arp_cache[k_demo_arp_cache / sizeof(ULONG)];

/* ThreadX worker thread + a byte pool that backs Mbed TLS's
 * mbedtls_calloc / mbedtls_free hooks. */
static TX_THREAD    s_demo_thread;
static UCHAR        s_demo_stack[k_demo_thread_stack];
static TX_BYTE_POOL s_byte_pool;
static UCHAR        s_byte_pool_memory[k_demo_byte_pool_size];

/* Mbed TLS state. Placed at file scope so the stack frame of the
 * worker thread stays well below k_demo_thread_stack. */
static mbedtls_ssl_context      s_ssl;
static mbedtls_ssl_config       s_ssl_cfg;
static mbedtls_ctr_drbg_context s_drbg;
static mbedtls_entropy_context  s_entropy;

static UCHAR s_request_buf[k_demo_request_buf];
static UCHAR s_response_buf[k_demo_response_buf];
#endif /* !RA8_OFF_TARGET */

/**
 * @brief Halt forever in WFI. Called on any fatal error.
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
 * @brief Bring CGC + the J-Link OB VCOM console + on-board Ethernet + RSIP up.
 *
 * @pre Reset_Handler / SystemInit complete.
 * @post On success the BSP console is sending at 115200 8N1, the
 *       on-board RGMII PHY pins + ETHA0 / RMAC0 are initialized, and
 *       the RSIP engine has finished BIST.
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

  /* Bring up the RSIP engine for its TRNG entropy source (used to
   * seed Mbed TLS's CTR_DRBG). The BIST is a one-shot ~ms operation so
   * we run it here at boot, not per request. */
  const ra8_rsip_config_t rsip_cfg = {.run_bist = true};
  if (ra8_rsip_init(&rsip_cfg) != k_ra8_ok) {
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
 * @brief Write an arbitrary byte buffer to SCI8 (no NUL required).
 *
 * @param[in] buf Bytes to emit.
 * @param[in] len Length in bytes.
 *
 * @since 0.1.0
 */
static void demo_write_bytes(const uint8_t* buf, uint32_t len)
{
  if (buf == nullptr || len == 0U) {
    return;
  }
  (void)ra8_board_uart_console_write(buf, (size_t)len);
}

#ifndef RA8_OFF_TARGET
/**
 * @brief Pack a 4-octet IPv4 array into NetX's host-order ULONG.
 *
 * @param[in] octets IPv4 address (4 bytes).
 *
 * @return Host-order packed address suitable for NetX.
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
 * @brief Bring NetX Duo up: pool, IP, ARP, TCP, ICMP. No listen socket.
 *
 * @return ``NX_SUCCESS`` on success, propagating NetX error codes.
 *
 * @pre ``nx_system_initialize`` has run.
 * @post On success the IP stack is ready for outbound TCP.
 *
 * @since 0.1.0
 */
static UINT demo_netx_bring_up(void)
{
  UINT s = nx_packet_pool_create(&s_packet_pool,
                                 (CHAR*)"ra8_eth_pool",
                                 (ULONG)k_demo_packet_size,
                                 s_pool_memory,
                                 (ULONG)sizeof(s_pool_memory));
  if (s != NX_SUCCESS) {
    return s;
  }

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

  ULONG msw = 0U;
  ULONG lsw = 0U;
  demo_pack_mac(&msw, &lsw);
  (void)nx_ip_interface_physical_address_set(&s_ip, 0U, msw, lsw, NX_TRUE);

  /* Default gateway so packets to www.example.com leave the LAN. */
  (void)nx_ip_gateway_address_set(&s_ip, demo_pack_ip(k_demo_gw));

  s = nx_arp_enable(&s_ip, (VOID*)s_arp_cache, (ULONG)sizeof(s_arp_cache));
  if (s != NX_SUCCESS) {
    return s;
  }
  s = nx_tcp_enable(&s_ip);
  if (s != NX_SUCCESS) {
    return s;
  }
  (void)nx_icmp_enable(&s_ip);
  return NX_SUCCESS;
}

/**
 * @brief Open a NetX Duo TCP socket and connect to ``www.example.com:443``.
 *
 * @return ``NX_SUCCESS`` on success.
 *
 * @pre ``demo_netx_bring_up`` has returned ``NX_SUCCESS``.
 * @post On success ``s_tls_socket`` is connected and ready for I/O.
 *
 * @since 0.1.0
 */
static UINT demo_tcp_connect(void)
{
  UINT s = nx_tcp_socket_create(&s_ip,
                                &s_tls_socket,
                                (CHAR*)"https443",
                                NX_IP_NORMAL,
                                NX_FRAGMENT_OKAY,
                                (UINT)k_demo_socket_ttl,
                                (ULONG)k_demo_recv_window,
                                NX_NULL,
                                NX_NULL);
  if (s != NX_SUCCESS) {
    return s;
  }
  s = nx_tcp_client_socket_bind(&s_tls_socket, NX_ANY_PORT, NX_WAIT_FOREVER);
  if (s != NX_SUCCESS) {
    return s;
  }
  ULONG host_ip = demo_pack_ip(k_demo_host_ip);
  s             = nx_tcp_client_socket_connect(&s_tls_socket,
                                   host_ip,
                                   (UINT)k_demo_https_port,
                                   (ULONG)k_demo_handshake_max);
  return s;
}

/**
 * @brief Mbed TLS BIO send callback bound to ``nx_tcp_socket_send``.
 *
 * @param[in] ctx Opaque context (unused -- we use the file-scope socket).
 * @param[in] buf Bytes Mbed TLS wants to send.
 * @param[in] len Buffer length.
 *
 * @return Bytes written, or a negative ``MBEDTLS_ERR_*`` on failure.
 *
 * @pre Socket is connected.
 *
 * @since 0.1.0
 */
static int demo_bio_send(void* ctx, const unsigned char* buf, size_t len)
{
  (void)ctx;
  if (buf == nullptr || len == 0U) {
    return 0;
  }
  NX_PACKET* pkt = NX_NULL;
  UINT       s   = nx_packet_allocate(&s_packet_pool, &pkt, NX_TCP_PACKET, NX_WAIT_FOREVER);
  if (s != NX_SUCCESS || pkt == NX_NULL) {
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  }
  s = nx_packet_data_append(pkt, (VOID*)buf, (ULONG)len, &s_packet_pool, NX_WAIT_FOREVER);
  if (s != NX_SUCCESS) {
    (void)nx_packet_release(pkt);
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  }
  s = nx_tcp_socket_send(&s_tls_socket, pkt, (ULONG)k_demo_recv_timeout);
  if (s != NX_SUCCESS) {
    (void)nx_packet_release(pkt);
    if (s == NX_NO_PACKET || s == NX_WINDOW_OVERFLOW) {
      return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return MBEDTLS_ERR_NET_SEND_FAILED;
  }
  return (int)len;
}

/**
 * @brief Mbed TLS BIO recv callback bound to ``nx_tcp_socket_receive``.
 *
 * @param[in]  ctx Opaque context (unused).
 * @param[out] buf Output buffer.
 * @param[in]  len Maximum bytes to read.
 *
 * @return Bytes copied, ``MBEDTLS_ERR_SSL_WANT_READ`` if the socket
 *         has nothing yet, or a negative ``MBEDTLS_ERR_*`` on error.
 *
 * @pre Socket is connected.
 *
 * @since 0.1.0
 */
static int demo_bio_recv(void* ctx, unsigned char* buf, size_t len)
{
  (void)ctx;
  if (buf == nullptr || len == 0U) {
    return 0;
  }
  NX_PACKET* pkt = NX_NULL;
  UINT       s   = nx_tcp_socket_receive(&s_tls_socket, &pkt, (ULONG)k_demo_recv_timeout);
  if (s == NX_NO_PACKET) {
    return MBEDTLS_ERR_SSL_WANT_READ;
  }
  if (s != NX_SUCCESS || pkt == NX_NULL) {
    return MBEDTLS_ERR_NET_RECV_FAILED;
  }
  ULONG copied = 0U;
  (void)nx_packet_data_retrieve(pkt, (VOID*)buf, &copied);
  (void)nx_packet_release(pkt);
  if (copied > (ULONG)len) {
    /* Should not happen with a 1568-byte pool packet, but cap. */
    copied = (ULONG)len;
  }
  return (int)copied;
}

/**
 * @brief Pull entropy from the RSIP TRNG into Mbed TLS.
 *
 * @details
 * Mbed TLS supplies the f_rng signature ``int f_rng(void* ctx,
 * unsigned char* buf, size_t len)``; we bind it to
 * ``ra8_rsip_trng_read`` so seeding the CTR_DRBG never depends on
 * software entropy collectors.
 *
 * @param[in]  ctx  Unused.
 * @param[out] buf  Output buffer.
 * @param[in]  len  Number of bytes requested.
 * @param[out] olen Receives the number of bytes actually written (always
 *                  @p len on success).
 *
 * @return ``0`` on success.
 *
 * @since 0.1.0
 */
static int demo_entropy_source(void* ctx, unsigned char* buf, size_t len, size_t* olen)
{
  (void)ctx;
  if (buf == nullptr || olen == nullptr) {
    return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
  }
  ra8_err_t err = ra8_rsip_trng_read((uint8_t*)buf, (uint32_t)len);
  if (err != k_ra8_ok) {
    return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
  }
  *olen = len;
  return 0;
}

/**
 * @brief PSA external RNG hook -- feeds RSIP TRNG straight into PSA.
 *
 * @details
 * Mbed TLS 4.x removed @ref MBEDTLS_ENTROPY_C / @ref MBEDTLS_CTR_DRBG_C
 * from the standard build path. With @ref MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
 * the SSL layer pulls random bytes from this callback (via
 * @ref psa_generate_random) instead of running an in-tree DRBG.
 *
 * @param[in,out] context     PSA's per-process external RNG context (unused).
 * @param[out]    output      Output buffer.
 * @param[in]     output_size Bytes requested.
 * @param[out]    output_length Bytes actually produced.
 *
 * @retval PSA_SUCCESS on success.
 * @retval PSA_ERROR_HARDWARE_FAILURE if the RSIP TRNG returned an error.
 *
 * @pre RSIP has been initialized by ``ra8_rsip_init()``.
 * @post @p output contains @p output_size bytes pulled from RSIP TRNG.
 *
 * @note Not thread-safe; callers must serialise across threads.
 *
 * @since 0.1.0
 */
psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t* context,
                                             uint8_t*                               output,
                                             size_t                                 output_size,
                                             size_t*                                output_length)
{
  (void)context;
  if (output == nullptr || output_length == nullptr) {
    return PSA_ERROR_INVALID_ARGUMENT;
  }
  ra8_err_t err = ra8_rsip_trng_read((uint8_t*)output, (uint32_t)output_size);
  if (err != k_ra8_ok) {
    return PSA_ERROR_HARDWARE_FAILURE;
  }
  *output_length = output_size;
  return PSA_SUCCESS;
}

/**
 * @brief Verify the peer's leaf certificate matches our compile-time pin.
 *
 * @details
 * Hashes the peer's leaf DER with ``ra8_rsip_sha256`` and compares
 * against ``k_demo_cert_pin_sha256``. Mismatch is fatal -- the
 * caller must abort the handshake without sending the HTTP request.
 *
 * @retval 0  Pin matches.
 * @retval -1 Pin mismatch (or no peer cert available).
 *
 * @pre Mbed TLS handshake has reached at least
 *      ``MBEDTLS_SSL_SERVER_CERTIFICATE``.
 *
 * @since 0.1.0
 */
static int demo_verify_cert_pin(void)
{
#if defined(DEMO_DISABLE_CERT_PIN)
  /* Bring-up override -- skip the pin check when the user has not
   * yet captured a fresh fingerprint. */
  return 0;
#else
  const mbedtls_x509_crt* peer = mbedtls_ssl_get_peer_cert(&s_ssl);
  if (peer == nullptr) {
    return -1;
  }
  uint8_t got[32];
  if (ra8_rsip_sha256(peer->raw.p, (uint32_t)peer->raw.len, got) != k_ra8_ok) {
    return -1;
  }
  if (memcmp(got, k_demo_cert_pin_sha256, sizeof(got)) != 0) {
    return -1;
  }
  return 0;
#endif
}

/**
 * @brief Issue the HTTP GET and dump the first ``k_demo_dump_bytes`` to SCI8.
 *
 * @return ``0`` on success.
 *
 * @pre TLS handshake is complete and the cert pin verified.
 * @post Up to ``k_demo_dump_bytes`` of body have been emitted.
 *
 * @since 0.1.0
 */
static int demo_http_get(void)
{
  int n = snprintf(
    (char*)s_request_buf,
    sizeof(s_request_buf),
    "GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nUser-Agent: ra8d2-https-client/0.1\r\n\r\n",
    k_demo_host_name);
  if (n <= 0 || (size_t)n >= sizeof(s_request_buf)) {
    return -1;
  }
  size_t written = 0U;
  while (written < (size_t)n) {
    int rc = mbedtls_ssl_write(&s_ssl, s_request_buf + written, (size_t)n - written);
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
      continue;
    }
    if (rc < 0) {
      return rc;
    }
    written += (size_t)rc;
  }

  /* Read until we've dumped k_demo_dump_bytes of body or the peer
   * closes. */
  uint32_t body_dumped    = 0U;
  uint8_t  saw_header_end = 0U;
  uint8_t  match          = 0U; /* tracks "\r\n\r\n" run. */
  while (body_dumped < (uint32_t)k_demo_dump_bytes) {
    int rc = mbedtls_ssl_read(&s_ssl, s_response_buf, sizeof(s_response_buf));
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
      continue;
    }
    if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == 0) {
      break;
    }
    if (rc < 0) {
      return rc;
    }
    int start = 0;
    if (saw_header_end == 0U) {
      /* Walk byte-by-byte looking for CRLFCRLF. */
      for (int i = 0; i < rc; i++) {
        uint8_t c = s_response_buf[i];
        if ((match == 0U && c == k_ascii_cr) || (match == 2U && c == k_ascii_cr)) {
          match++;
        } else if ((match == 1U && c == k_ascii_lf) || (match == 3U && c == k_ascii_lf)) {
          match++;
        } else {
          match = 0U;
        }
        if (match == 4U) {
          saw_header_end = 1U;
          start          = i + 1;
          break;
        }
      }
      if (saw_header_end == 0U) {
        continue;
      }
    }
    int avail = rc - start;
    if (avail <= 0) {
      continue;
    }
    uint32_t take = (uint32_t)avail;
    if (body_dumped + take > (uint32_t)k_demo_dump_bytes) {
      take = (uint32_t)k_demo_dump_bytes - body_dumped;
    }
    demo_write_bytes(s_response_buf + start, take);
    body_dumped += take;
  }
  return 0;
}

/**
 * @brief Run the TLS bring-up: DRBG, config, handshake, pin, GET, dump.
 *
 * @return ``0`` on success.
 *
 * @pre ``demo_tcp_connect`` returned ``NX_SUCCESS``.
 * @post Worker thread parks after dumping the body.
 *
 * @since 0.1.0
 */
static int demo_tls_session(void)
{
  mbedtls_ssl_init(&s_ssl);
  mbedtls_ssl_config_init(&s_ssl_cfg);

  /* Mbed TLS 4.x removed MBEDTLS_ENTROPY_C and MBEDTLS_CTR_DRBG_C from
   * the standard build path; the SSL layer pulls random bytes from PSA
   * crypto (psa_generate_random), which the firmware wires to the RSIP
   * TRNG via the mbedtls_psa_external_get_random() implementation
   * above. The legacy mbedtls_entropy_* / mbedtls_ctr_drbg_* setup
   * dance is therefore no longer needed. We still need PSA itself
   * online; demo_thread_entry calls psa_crypto_init() before this. */
  (void)demo_entropy_source; /* Retained for reference; unused on 4.x. */
  (void)s_drbg;
  (void)s_entropy;

  if (mbedtls_ssl_config_defaults(&s_ssl_cfg,
                                  MBEDTLS_SSL_IS_CLIENT,
                                  MBEDTLS_SSL_TRANSPORT_STREAM,
                                  MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
    return -1;
  }
  /* No CA bundle is provisioned in this demo -- we authenticate the
   * peer by SHA-256 cert-pin instead. ``OPTIONAL`` lets the
   * handshake complete even with an empty trust store; the pin
   * check below is what gates the GET. */
  mbedtls_ssl_conf_authmode(&s_ssl_cfg, MBEDTLS_SSL_VERIFY_OPTIONAL);
  /* Mbed TLS 4.x removed mbedtls_ssl_conf_rng() -- the SSL layer
   * pulls random bytes from PSA crypto's psa_generate_random() now,
   * which is wired up by demo_psa_init() below via the
   * MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG hook
   * (mbedtls_psa_external_get_random). */

  if (mbedtls_ssl_setup(&s_ssl, &s_ssl_cfg) != 0) {
    return -1;
  }
  if (mbedtls_ssl_set_hostname(&s_ssl, k_demo_host_name) != 0) {
    return -1;
  }
  mbedtls_ssl_set_bio(&s_ssl, nullptr, demo_bio_send, demo_bio_recv, nullptr);

  /* Drive the handshake to completion. */
  int rc = 0;
  while ((rc = mbedtls_ssl_handshake(&s_ssl)) != 0) {
    if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
      return rc;
    }
  }
  if (demo_verify_cert_pin() != 0) {
    demo_print("[https] cert pin MISMATCH -- aborting\r\n");
    return -1;
  }
  demo_print("[https] handshake OK, cert pin matches, dumping body\r\n");
  return demo_http_get();
}

/**
 * @brief ThreadX worker entry: bring NetX up, run TLS session, dump body.
 *
 * @param[in] thread_input Unused.
 *
 * @pre ``nx_system_initialize`` has run.
 * @post Worker parks after dumping or on the first failure.
 *
 * @since 0.1.0
 */
static void demo_thread_entry(ULONG thread_input)
{
  (void)thread_input;
  /* Bring PSA crypto online before any TLS / X.509 work. The library
   * lazy-initialises inside the SSL layer, but doing it explicitly
   * here surfaces failures earlier and binds the RSIP-TRNG external
   * RNG hook before the handshake's first random call. */
  if (psa_crypto_init() != PSA_SUCCESS) {
    demo_print("[https] psa_crypto_init failed\r\n");
    return;
  }
  demo_print("[https] bringing NetX Duo up...\r\n");
  if (demo_netx_bring_up() != NX_SUCCESS) {
    demo_print("[https] NetX bring-up failed\r\n");
    return;
  }
  demo_print("[https] connecting to www.example.com:443\r\n");
  if (demo_tcp_connect() != NX_SUCCESS) {
    demo_print("[https] TCP connect failed\r\n");
    return;
  }
  if (demo_tls_session() != 0) {
    demo_print("[https] TLS session failed\r\n");
  }
  demo_print("\r\n[https] done\r\n");
}

/**
 * @brief ``mbedtls_calloc`` hook backed by the ThreadX byte pool.
 *
 * @param[in] n    Element count.
 * @param[in] size Element size in bytes.
 *
 * @return Newly allocated zeroed memory, or NULL on failure.
 *
 * @pre ``s_byte_pool`` has been created.
 *
 * @since 0.1.0
 */
static void* demo_calloc(size_t n, size_t size)
{
  size_t total = n * size;
  if (total == 0U) {
    return nullptr;
  }
  VOID* p = NX_NULL;
  if (tx_byte_allocate(&s_byte_pool, &p, (ULONG)total, TX_NO_WAIT) != TX_SUCCESS) {
    return nullptr;
  }
  (void)memset((void*)p, 0, total);
  return p;
}

/**
 * @brief ``mbedtls_free`` hook backed by the ThreadX byte pool.
 *
 * @param[in] p Pointer previously returned by ``demo_calloc`` (NULL OK).
 *
 * @since 0.1.0
 */
static void demo_free(void* p)
{
  if (p == nullptr) {
    return;
  }
  (void)tx_byte_release(p);
}

/**
 * @brief ThreadX system-define hook: build worker thread + byte pool.
 *
 * @param[in] first_unused_memory Pointer to free RAM provided by the
 *   ThreadX port; unused -- we statically allocate.
 *
 * @pre ``tx_kernel_enter()`` has been called.
 * @post One worker thread is created and NetX + Mbed TLS are
 *       initialized inside it.
 *
 * @since 0.1.0
 */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  (void)tx_byte_pool_create(&s_byte_pool,
                            "mbedtls_pool",
                            s_byte_pool_memory,
                            (ULONG)sizeof(s_byte_pool_memory));
  mbedtls_platform_set_calloc_free(demo_calloc, demo_free);

  nx_system_initialize();

  (void)tx_thread_create(&s_demo_thread,
                         "https_worker",
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
 * @brief Application entry. Brings up clocks + UART + RMII pins, then ThreadX.
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
  demo_print("[https] booting ThreadX + NetX Duo + Mbed TLS...\r\n");

#ifndef RA8_OFF_TARGET
  tx_kernel_enter();
#endif

  demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
