/**
 * @file examples/ek_ra8d2/hw_pending/tls_client/main.c
 * @brief TLS client over ``ra8_tls`` + ``ra8_net_pal`` (NetX Duo transport)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Minimal TLS-client demo for issue #261. It brings the chip up the same
 * way ``threadx_netx_tcp_echo`` does (CGC -> SCI console -> RMII pins ->
 * RSIP), hands control to ThreadX, and on a single worker thread:
 *
 *   1. ``psa_crypto_init()`` (Mbed TLS 4.x sources randomness from PSA; the
 *      external-RNG hook below feeds it from the RSIP TRNG).
 *   2. Brings NetX Duo up as a TCP client at 192.168.1.42/24, gateway
 *      192.168.1.1.
 *   3. Opens a TCP socket to a test TLS endpoint (default
 *      192.168.1.1:4433) and connects.
 *   4. Opens an ``ra8_tls`` session whose BIO callbacks are bound to the
 *      NetX socket, then drives ``ra8_tls_handshake`` to completion.
 *   5. Sends one application record and reads one back.
 *   6. Prints the negotiated cipher suite and the peer-verification result
 *      through ``ra8_tls_get_cipher_suite`` / ``ra8_tls_get_verify_result``.
 *
 * The TCP MSS is clamped with ``ra8_tls_mss_clamp`` so every segment fits
 * inside the #21-pinned 128-byte MTU (128 - 20 IPv4 - 20 TCP == 88).
 *
 * ## Why hw_pending (not emulator-gated)
 *
 * ra8_emulator runs the real firmware ELF, so a genuine handshake here needs
 * (a) a crypto-complete TLS *server* on the wire and (b) a working entropy
 * source under emulation. Today ra8_emulator's peer (``board_net``) is a
 * plaintext TCP stack with no TLS and no RSIP/TRNG model, so it cannot
 * complete an Mbed TLS handshake. Bringing up a board_net TLS-server
 * endpoint (host-side mbedtls + a reverse TCP role + a TRNG model) is the
 * follow-up that promotes this app to ``hw_validated/hil/``; see README.md.
 * Until then the transport + facade glue is proven by the host unit test
 * ``tests/test_ra8_tls_net.c`` (which drives the same ``ra8_tls`` API over
 * the ``ra8_net_pal`` loopback ring), and this app runs against a real
 * ``openssl s_server`` on the bench.
 *
 * @author Brighton Sikarskie
 * @date 2026-07-15
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_boot_entry.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_rsip.h"
#include "ra8_time.h"
#include "ra8_tls.h"

#ifndef RA8_OFF_TARGET
#include "mbedtls/net_sockets.h"
#include "mbedtls/platform.h"
#include "mbedtls/ssl.h"
#include "nx_api.h"
#include "nx_ether_driver_ra8_eth.h"
#include "psa/crypto.h"
#include "tx_api.h"
#endif

/**
 * @enum demo_config_t
 * @brief Compile-time settings for the TLS-client demo.
 *
 * @details
 * Mirrors the ``threadx_https_client`` sizing for NetX Duo + Mbed TLS; the
 * ra8_tls session pool + one SSL context fit inside the ThreadX byte pool.
 */
typedef enum : uint32_t {
  k_demo_baud           = 115200U, /**< Demo baud.                     */
  k_demo_thread_stack   = 16384U,  /**< Demo thread stack.             */
  k_demo_ip_thread_pri  = 1U,      /**< Demo IP thread priority.       */
  k_demo_app_thread_pri = 8U,      /**< Demo app thread priority.      */
  k_demo_packet_size    = 1568U,   /**< Demo packet size.              */
  k_demo_pool_bytes     = 32768U,  /**< Demo pool bytes.               */
  k_demo_byte_pool_size = 65536U,  /**< Demo byte pool size.           */
  k_demo_ip_stack       = 4096U,   /**< Demo IP stack.                 */
  k_demo_arp_cache      = 1024U,   /**< Demo arp cache.                */
  k_demo_tls_port       = 4433U,   /**< Default TLS endpoint port.     */
  k_demo_recv_window    = 4096U,   /**< Demo recv window.              */
  k_demo_socket_ttl     = 64U,     /**< Demo socket ttl.               */
  k_demo_recv_timeout   = 200U,    /**< Ticks; ~2 s at 100 Hz.         */
  k_demo_connect_ticks  = 3000U,   /**< Max ticks waiting for connect. */
  k_demo_handshake_max  = 20000U,  /**< Bounded handshake poll count.  */
  k_demo_io_max         = 4000U,   /**< Bounded send/recv poll count.  */
  k_demo_line_buf       = 160U,    /**< Result line buffer bytes.      */
  k_demo_record_cap     = 64U,     /**< Inbound record buffer bytes.   */
  k_demo_mtu            = 128U,    /**< #21 pinned link MTU.           */
  k_demo_verify_unset =
    0xFFFFFFFFU, /**< Fail-closed verify sentinel: nonzero until the getter fills it. */
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
 * @enum demo_ipv4_t
 * @brief Demo IPv4 octets (board, gateway/endpoint, netmask).
 */
typedef enum : uint16_t {
  k_demo_ipaddr_0  = 192U, /**< 192.168.1.42 board.                 */
  k_demo_ipaddr_1  = 168U, /**< Demo ipaddr 1.                      */
  k_demo_ipaddr_2  = 1U,   /**< Demo ipaddr 2.                      */
  k_demo_ipaddr_3  = 42U,  /**< Demo ipaddr 3.                      */
  k_demo_netmask_b = 255U, /**< 255.255.255.0 (first three octets). */
  k_demo_gw_3      = 1U,   /**< 192.168.1.1 gateway + TLS endpoint. */
} demo_ipv4_t;

/**
 * @enum demo_mac_idx_t
 * @brief MAC-address byte indices.
 */
typedef enum : uint8_t {
  k_demo_mac_idx_0 = 0U, /**< Demo MAC index 0. */
  k_demo_mac_idx_1 = 1U, /**< Demo MAC index 1. */
  k_demo_mac_idx_2 = 2U, /**< Demo MAC index 2. */
  k_demo_mac_idx_3 = 3U, /**< Demo MAC index 3. */
  k_demo_mac_idx_4 = 4U, /**< Demo MAC index 4. */
  k_demo_mac_idx_5 = 5U, /**< Demo MAC index 5. */
} demo_mac_idx_t;

/** @brief Locally-administered unicast MAC for this board. */
static const uint8_t k_demo_mac[6] = {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x03U};

/** @brief IPv4 address: 192.168.1.42. */
static const uint8_t k_demo_ip[4] = {(uint8_t)k_demo_ipaddr_0,
                                     (uint8_t)k_demo_ipaddr_1,
                                     (uint8_t)k_demo_ipaddr_2,
                                     (uint8_t)k_demo_ipaddr_3};

/** @brief Subnet mask: 255.255.255.0. */
static const uint8_t k_demo_mask[4] = {(uint8_t)k_demo_netmask_b,
                                       (uint8_t)k_demo_netmask_b,
                                       (uint8_t)k_demo_netmask_b,
                                       0U};

/** @brief Default gateway + TLS endpoint: 192.168.1.1. */
static const uint8_t k_demo_gw[4] = {(uint8_t)k_demo_ipaddr_0,
                                     (uint8_t)k_demo_ipaddr_1,
                                     (uint8_t)k_demo_ipaddr_2,
                                     (uint8_t)k_demo_gw_3};

/** @brief SNI hostname advertised in the ClientHello. */
static const char k_demo_server_name[] = "endpoint.test";

/** @brief The one application record this demo sends after the handshake. */
static const uint8_t k_demo_record[] = "PING over ra8_tls\n";

#ifndef RA8_OFF_TARGET
/* NetX Duo state. ThreadX requires statically-allocated control blocks
 * (NASA Power of 10 Rule 3 -- no dynamic memory). */
static NX_PACKET_POOL s_packet_pool;
static NX_IP          s_ip;
static NX_TCP_SOCKET  s_tls_socket;
static UCHAR          s_pool_memory[k_demo_pool_bytes];
static ULONG          s_ip_stack[k_demo_ip_stack / sizeof(ULONG)];
static ULONG          s_arp_cache[k_demo_arp_cache / sizeof(ULONG)];

/* ThreadX worker thread + a byte pool that backs Mbed TLS's calloc/free. */
static TX_THREAD    s_demo_thread;
static UCHAR        s_demo_stack[k_demo_thread_stack];
static TX_BYTE_POOL s_byte_pool;
static UCHAR        s_byte_pool_memory[k_demo_byte_pool_size];
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
 * @post On success the BSP console is sending at 115200 8N1, ETHA0 / RMAC0
 *       are initialized, and the RSIP engine has finished BIST.
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
  const ra8_rsip_config_t rsip_cfg = {.run_bist = true};
  if (ra8_rsip_init(&rsip_cfg) != k_ra8_ok) {
    demo_panic_halt();
  }
}

/**
 * @brief Write a NUL-terminated string to the console.
 *
 * @param[in] s NUL-terminated ASCII string. Must not be NULL.
 *
 * @pre s != NULL.
 * @post Bytes are queued in the console TX FIFO (best-effort).
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

#ifndef RA8_OFF_TARGET
/**
 * @brief Pack a 4-octet IPv4 array into NetX's host-order ULONG.
 *
 * @param[in] octets IPv4 address (4 bytes). Must not be NULL.
 *
 * @return Host-order packed address suitable for NetX.
 *
 * @pre octets != NULL and holds 4 bytes.
 * @post No state is modified.
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
 * @param[out] msw Most-significant word (octets 0-1). Must not be NULL.
 * @param[out] lsw Least-significant word (octets 2-5). Must not be NULL.
 *
 * @pre msw != NULL and lsw != NULL.
 * @post *msw / *lsw hold the packed MAC halves.
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
 * @brief PSA external RNG hook -- feeds the RSIP TRNG straight into PSA.
 *
 * @details
 * With ``MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG`` the SSL layer pulls random
 * bytes from this callback (via ``psa_generate_random``) rather than an
 * in-tree DRBG. Bound to ``ra8_rsip_trng_read``.
 *
 * @param[in,out] context       PSA per-process external RNG context (unused).
 * @param[out]    output        Output buffer.
 * @param[in]     output_size   Bytes requested.
 * @param[out]    output_length Bytes actually produced.
 *
 * @retval PSA_SUCCESS on success.
 * @retval PSA_ERROR_INVALID_ARGUMENT on a NULL output pointer.
 * @retval PSA_ERROR_HARDWARE_FAILURE if the RSIP TRNG returned an error.
 *
 * @pre RSIP has been initialized by ``ra8_rsip_init()``.
 * @post @p output holds @p output_size bytes on success.
 *
 * @note Not thread-safe; callers must serialise across threads.
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
  if (ra8_rsip_trng_read((uint8_t*)output, (uint32_t)output_size) != k_ra8_ok) {
    return PSA_ERROR_HARDWARE_FAILURE;
  }
  *output_length = output_size;
  return PSA_SUCCESS;
}

/**
 * @brief ``mbedtls_calloc`` hook backed by the ThreadX byte pool.
 *
 * @param[in] n    Element count.
 * @param[in] size Element size in bytes.
 *
 * @return Zeroed memory, or NULL on failure / zero request.
 *
 * @pre ``s_byte_pool`` has been created.
 * @post Returned block (if any) is zeroed.
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
 * @pre ``s_byte_pool`` has been created.
 * @post The block is returned to the pool.
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
 * @brief ra8_tls BIO send callback bound to ``nx_tcp_socket_send``.
 *
 * @param[in] ctx Opaque context (unused -- the file-scope socket is used).
 * @param[in] buf Ciphertext bytes the TLS layer wants to transmit.
 * @param[in] len Buffer length in bytes.
 *
 * @return Bytes written, or a negative ``MBEDTLS_ERR_*`` code.
 *
 * @pre Socket ``s_tls_socket`` is connected.
 * @post On success the bytes are queued for TX.
 *
 * @since 0.1.0
 */
static int tls_bio_send(void* ctx, const uint8_t* buf, size_t len)
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
  s =
    nx_packet_data_append(pkt, (VOID*)(uintptr_t)buf, (ULONG)len, &s_packet_pool, NX_WAIT_FOREVER);
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
 * @brief ra8_tls BIO recv callback bound to ``nx_tcp_socket_receive``.
 *
 * @param[in]  ctx Opaque context (unused).
 * @param[out] buf Output buffer.
 * @param[in]  len Maximum bytes to read.
 *
 * @return Bytes copied, ``MBEDTLS_ERR_SSL_WANT_READ`` when nothing is
 *         ready yet, or a negative ``MBEDTLS_ERR_*`` on error.
 *
 * @pre Socket ``s_tls_socket`` is connected.
 * @post On success @p buf holds the returned bytes.
 *
 * @since 0.1.0
 */
static int tls_bio_recv(void* ctx, uint8_t* buf, size_t len)
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
    copied = (ULONG)len;
  }
  return (int)copied;
}

/**
 * @brief Bring NetX Duo up: pool, IP, gateway, ARP, TCP, ICMP.
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
  nx_ether_driver_ra8_eth_set_mac(k_demo_mac);

  s = nx_ip_create(&s_ip,
                   (CHAR*)"ra8_eth_ip",
                   demo_pack_ip(k_demo_ip),
                   demo_pack_ip(k_demo_mask),
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
  /* Fragment oversized datagrams into in-spec frames under the 128-byte MTU. */
  (void)nx_ip_fragment_enable(&s_ip);
  return NX_SUCCESS;
}

/**
 * @brief Open a NetX TCP socket and connect to the TLS endpoint.
 *
 * @return ``NX_SUCCESS`` on success.
 *
 * @pre ``demo_netx_bring_up`` returned ``NX_SUCCESS``.
 * @post On success ``s_tls_socket`` is connected and ready for I/O.
 *
 * @since 0.1.0
 */
static UINT demo_tcp_connect(void)
{
  UINT s = nx_tcp_socket_create(&s_ip,
                                &s_tls_socket,
                                (CHAR*)"tls4433",
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
  return nx_tcp_client_socket_connect(&s_tls_socket,
                                      demo_pack_ip(k_demo_gw),
                                      (UINT)k_demo_tls_port,
                                      (ULONG)k_demo_connect_ticks);
}

/**
 * @brief Drive the ra8_tls handshake to completion (bounded poll loop).
 *
 * @param[in] session Open ra8_tls session handle.
 *
 * @return ``k_ra8_ok`` on a completed handshake, otherwise the facade
 *         error (or ``k_ra8_err_timeout`` if the poll budget is exhausted).
 *
 * @pre ``session`` is open and its BIO is bound.
 * @post On ``k_ra8_ok`` the session is in the application-data state.
 *
 * @since 0.1.0
 */
static ra8_err_t demo_handshake(ra8_tls_session_t session)
{
  for (uint32_t i = 0U; i < (uint32_t)k_demo_handshake_max; ++i) {
    ra8_err_t err = ra8_tls_handshake(session);
    if (err != k_ra8_err_would_block) {
      return err;
    }
  }
  return k_ra8_err_timeout;
}

/**
 * @brief Send one record and read one back over the TLS session.
 *
 * @param[in]  session Open, handshaken ra8_tls session.
 * @param[out] got     Receive buffer for the reply.
 * @param[in]  got_cap Capacity of @p got in bytes.
 *
 * @return ``k_ra8_ok`` if a record was sent and a reply was received.
 *
 * @pre ``session`` handshake completed; ``got`` non-NULL, ``got_cap`` >= 1.
 * @post On success at least one plaintext byte was exchanged each way.
 *
 * @since 0.1.0
 */
static ra8_err_t demo_exchange(ra8_tls_session_t session, uint8_t* got, size_t got_cap)
{
  if (got == nullptr || got_cap == 0U) {
    return k_ra8_err_invalid_arg;
  }
  size_t    sent = 0U;
  ra8_err_t err  = k_ra8_err_would_block;
  for (uint32_t i = 0U; (i < (uint32_t)k_demo_io_max) && (err == k_ra8_err_would_block); ++i) {
    err = ra8_tls_send(session, k_demo_record, sizeof(k_demo_record) - 1U, &sent);
  }
  if (err != k_ra8_ok) {
    return err;
  }
  size_t got_n = 0U;
  err          = k_ra8_err_would_block;
  for (uint32_t i = 0U; (i < (uint32_t)k_demo_io_max) && (err == k_ra8_err_would_block); ++i) {
    err = ra8_tls_recv(session, got, got_cap, &got_n);
  }
  return err;
}

/**
 * @brief Print the negotiated cipher + verification result to the console.
 *
 * @param[in] session Handshaken ra8_tls session.
 *
 * @pre ``session`` handshake completed.
 * @post One ``[tls] cipher=... verify=...`` line has been emitted.
 *
 * @since 0.1.0
 */
static void demo_report(ra8_tls_session_t session)
{
  uint16_t id                              = 0U;
  char     name[k_ra8_tls_cipher_name_cap] = {};
  uint32_t flags                           = (uint32_t)k_demo_verify_unset;
  (void)ra8_tls_get_cipher_suite(session, &id, name, sizeof(name));
  (void)ra8_tls_get_verify_result(session, &flags);

  char line[k_demo_line_buf];
  int  n = snprintf(line,
                    sizeof(line),
                    "[tls] cipher=%s id=0x%04X verify=0x%08lX %s\r\n",
                    name,
                    (unsigned)id,
                    (unsigned long)flags,
                    (flags == 0U) ? "OK" : "UNVERIFIED");
  if (n > 0 && (size_t)n < sizeof(line)) {
    demo_print(line);
  }
}

/**
 * @brief Run the whole TLS session: open, handshake, exchange, report.
 *
 * @return ``k_ra8_ok`` on a completed exchange.
 *
 * @pre ``demo_tcp_connect`` returned ``NX_SUCCESS``.
 * @post The session has been closed and the facade deinitialised.
 *
 * @since 0.1.0
 */
static ra8_err_t demo_run_tls(void)
{
  uint16_t mss = 0U;
  if (ra8_tls_mss_clamp((uint16_t)k_demo_mtu, &mss) != k_ra8_ok) {
    return k_ra8_err_invalid_arg;
  }
  {
    char line[k_demo_line_buf];
    int  n = snprintf(line,
                      sizeof(line),
                      "[tls] MTU=%u -> MSS clamp=%u\r\n",
                      (unsigned)k_demo_mtu,
                      (unsigned)mss);
    if (n > 0 && (size_t)n < sizeof(line)) {
      demo_print(line);
    }
  }

  ra8_err_t err = ra8_tls_global_init();
  if (err != k_ra8_ok) {
    return err;
  }

  ra8_tls_session_cfg_t cfg = {};
  cfg.bio_send              = tls_bio_send;
  cfg.bio_recv              = tls_bio_recv;
  cfg.bio_ctx               = &s_tls_socket;
  cfg.server_name           = k_demo_server_name;
  /* The test endpoint typically presents a self-signed cert; report the
   * verification result rather than aborting the exchange. */
  cfg.verify_mode = k_ra8_tls_verify_optional;

  ra8_tls_session_t session = nullptr;
  err                       = ra8_tls_session_open(&session, &cfg);
  if (err != k_ra8_ok) {
    (void)ra8_tls_global_deinit();
    return err;
  }

  err = demo_handshake(session);
  if (err == k_ra8_ok) {
    uint8_t got[k_demo_record_cap] = {};
    err                            = demo_exchange(session, got, sizeof(got));
    demo_report(session);
  }

  (void)ra8_tls_session_close(session);
  (void)ra8_tls_global_deinit();
  return err;
}

/**
 * @brief ThreadX worker entry: PSA + NetX up, connect, run the TLS session.
 *
 * @param[in] thread_input Unused.
 *
 * @pre ``nx_system_initialize`` has run.
 * @post Worker parks after the exchange or on the first failure.
 *
 * @since 0.1.0
 */
static void demo_thread_entry(ULONG thread_input)
{
  (void)thread_input;
  if (psa_crypto_init() != PSA_SUCCESS) {
    demo_print("[tls] psa_crypto_init failed\r\n");
    return;
  }
  demo_print("[tls] bringing NetX Duo up...\r\n");
  if (demo_netx_bring_up() != NX_SUCCESS) {
    demo_print("[tls] NetX bring-up failed\r\n");
    return;
  }
  demo_print("[tls] connecting to 192.168.1.1:4433\r\n");
  if (demo_tcp_connect() != NX_SUCCESS) {
    demo_print("[tls] TCP connect failed\r\n");
    return;
  }
  if (demo_run_tls() != k_ra8_ok) {
    demo_print("[tls] session failed\r\n");
  }
  demo_print("[tls] done\r\n");
}

/**
 * @brief ThreadX system-define hook: build worker thread + byte pool.
 *
 * @param[in] first_unused_memory Free RAM from the ThreadX port; unused.
 *
 * @pre ``tx_kernel_enter()`` has been called.
 * @post One worker thread is created and Mbed TLS's allocator is wired.
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
                         "tls_worker",
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

/**
 * @brief Application entry. Brings up clocks + UART + RMII + RSIP, then ThreadX.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @post On clean entry the kernel runs the worker thread once.
 *
 * @since 0.1.0
 */
void main(void)
{
  demo_setup_or_halt();
  ra8_isr_globals_enable();
  demo_print("[tls] booting ThreadX + NetX Duo + ra8_tls...\r\n");

#ifndef RA8_OFF_TARGET
  tx_kernel_enter();
#endif

  demo_panic_halt();
}
