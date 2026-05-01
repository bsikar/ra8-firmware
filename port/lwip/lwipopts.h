/***********************************************************************************************************************
 * @file    port/lwip/lwipopts.h
 * @brief   Project-wide lwIP build configuration for the RA8D2 firmware.
 *
 * @details
 * This header tunes the vendored lwIP (libs/third_party/lwip,
 * tag STABLE-2_2_0_RELEASE) for an embedded build that runs on top of
 * Eclipse ThreadX. It is consumed by every lwIP translation unit through
 * lwIP's `lwip/opt.h` -> `lwipopts.h` indirection.
 *
 * Defaults selected here:
 *   - NO_SYS = 0           : we have an OS (ThreadX), so use the
 *                            netconn / socket APIs.
 *   - MEM_LIBC_MALLOC = 0  : never call libc malloc; lwIP's pool
 *                            allocator (memp) is used everywhere.
 *   - MEMP_MEM_MALLOC = 0  : MEMP_NUM_* defines below cap every pool.
 *   - LWIP_NETCONN = 1     : sequential API enabled (ThreadX threads use it).
 *   - LWIP_SOCKET = 1      : BSD-ish socket layer enabled on top of netconn.
 *   - LWIP_IPV4 = 1, LWIP_IPV6 = 0 : v4-only first cut, v6 to come later.
 *   - TCP_MSS = 1460       : standard Ethernet MSS for v4 / 1500-byte MTU.
 *
 * @par NASA Power of 10 Compliance:
 * Rule 3 (no dynamic allocation after init) is intentionally relaxed for
 * the vendored lwIP stack. lwIP allocates exclusively from compile-time
 * sized memp pools (see MEMP_NUM_* below); no runtime malloc is called.
 * The pool sizes here are conservative defaults sufficient for a
 * single-netif TCP echo workload. This deviation is documented in
 * docs/STYLE_GUIDE.md under the "Vendored third-party libraries" section
 * and follows the same pattern as the NetX Duo port.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * @license   SPDX-License-Identifier: MIT
 **********************************************************************************************************************/

#pragma once

/* ------------------------------------------------------------------------ */
/* errno provisioning                                                        */
/*                                                                           */
/* arm-none-eabi-newlib's <errno.h> exposes errno itself but only a small    */
/* subset of the *VALUES* (ENOMEM yes, EHOSTUNREACH no). lwIP's `api/err.c`  */
/* references the full POSIX set. Have lwIP supply them itself so the port  */
/* compiles cleanly under -Wall -Wextra -Werror without linking to a host   */
/* errno table.                                                              */
/* ------------------------------------------------------------------------ */
#define LWIP_PROVIDE_ERRNO              (1)

/* ------------------------------------------------------------------------ */
/* OS / threading model                                                      */
/* ------------------------------------------------------------------------ */
#define NO_SYS                          (0)
#define LWIP_TIMERS                     (1)
#define LWIP_TCPIP_CORE_LOCKING         (1)
#define SYS_LIGHTWEIGHT_PROT            (1)

/* ------------------------------------------------------------------------ */
/* Memory model                                                              */
/*                                                                           */
/* MEM_LIBC_MALLOC=0 forbids any path through libc's malloc/free.            */
/* MEMP_MEM_MALLOC=0 forces every memp allocation to come from a static pool.*/
/* The static pools below are the only memory lwIP ever uses on this build.  */
/* ------------------------------------------------------------------------ */
#define MEM_LIBC_MALLOC                 (0)
#define MEMP_MEM_MALLOC                 (0)
#define MEM_ALIGNMENT                   (4)
#define MEM_SIZE                        (16 * 1024)

/* Conservative pool sizes for a small embedded TCP/IP workload. */
#define MEMP_NUM_PBUF                   (16)
#define MEMP_NUM_RAW_PCB                (4)
#define MEMP_NUM_UDP_PCB                (6)
#define MEMP_NUM_TCP_PCB                (8)
#define MEMP_NUM_TCP_PCB_LISTEN         (4)
#define MEMP_NUM_TCP_SEG                (32)
#define MEMP_NUM_REASSDATA              (4)
#define MEMP_NUM_FRAG_PBUF              (8)
#define MEMP_NUM_ARP_QUEUE              (4)
#define MEMP_NUM_IGMP_GROUP             (4)
#define MEMP_NUM_NETBUF                 (8)
#define MEMP_NUM_NETCONN                (8)
#define MEMP_NUM_TCPIP_MSG_API          (8)
#define MEMP_NUM_TCPIP_MSG_INPKT        (16)
#define MEMP_NUM_SYS_TIMEOUT            (8)

#define PBUF_POOL_SIZE                  (16)
#define PBUF_POOL_BUFSIZE               (1536U)

/* ------------------------------------------------------------------------ */
/* IP stack feature flags                                                    */
/* ------------------------------------------------------------------------ */
#define LWIP_IPV4                       (1)
#define LWIP_IPV6                       (0)
#define LWIP_ARP                        (1)
#define LWIP_ICMP                       (1)
#define LWIP_DHCP                       (1)
#define LWIP_AUTOIP                     (0)
#define LWIP_DNS                        (1)
#define LWIP_IGMP                       (0)

/* ------------------------------------------------------------------------ */
/* Transport                                                                 */
/* ------------------------------------------------------------------------ */
#define LWIP_RAW                        (1)
#define LWIP_UDP                        (1)
#define LWIP_TCP                        (1)
#define TCP_MSS                         (1460)
#define TCP_WND                         (4 * TCP_MSS)
#define TCP_SND_BUF                     (4 * TCP_MSS)
#define TCP_SND_QUEUELEN                ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define TCP_LISTEN_BACKLOG              (1)

/* ------------------------------------------------------------------------ */
/* Sequential API + sockets                                                  */
/* ------------------------------------------------------------------------ */
#define LWIP_NETCONN                    (1)
#define LWIP_SOCKET                     (1)
#define LWIP_COMPAT_SOCKETS             (0)
#define LWIP_POSIX_SOCKETS_IO_NAMES     (0)
#define LWIP_SO_RCVTIMEO                (1)
#define LWIP_SO_SNDTIMEO                (1)
#define LWIP_TCP_KEEPALIVE              (1)

/* ------------------------------------------------------------------------ */
/* Statistics / debug                                                        */
/* ------------------------------------------------------------------------ */
#define LWIP_STATS                      (0)
#define LWIP_STATS_DISPLAY              (0)

/* No SLIP, no PPP, no 6LoWPAN. */
#define PPP_SUPPORT                     (0)

/* ------------------------------------------------------------------------ */
/* Checksum                                                                  */
/*                                                                           */
/* The Ethernet MAC on RA8D2 supports IPv4 / TCP / UDP HW checksum offload   */
/* but the netif driver lands in step 3, so for now use the stack's          */
/* software checksum implementation. Flip these to 0 when the netif driver   */
/* opts in to HW offload via NETIF_FLAG_*_CHKSUM_*.                          */
/* ------------------------------------------------------------------------ */
#define CHECKSUM_GEN_IP                 (1)
#define CHECKSUM_GEN_UDP                (1)
#define CHECKSUM_GEN_TCP                (1)
#define CHECKSUM_CHECK_IP               (1)
#define CHECKSUM_CHECK_UDP              (1)
#define CHECKSUM_CHECK_TCP              (1)

/* ------------------------------------------------------------------------ */
/* Stack / priority for sys_thread_new()                                     */
/*                                                                           */
/* The TCPIP thread is created via sys_thread_new() in tcpip_init(); use a   */
/* generous stack (12 KB) and a mid ThreadX priority. Apps may override.     */
/* ------------------------------------------------------------------------ */
#define TCPIP_THREAD_STACKSIZE          (12 * 1024)
#define TCPIP_THREAD_PRIO               (12)
#define TCPIP_MBOX_SIZE                 (16)
#define DEFAULT_THREAD_STACKSIZE        (4 * 1024)
#define DEFAULT_THREAD_PRIO             (14)
#define DEFAULT_RAW_RECVMBOX_SIZE       (8)
#define DEFAULT_UDP_RECVMBOX_SIZE       (8)
#define DEFAULT_TCP_RECVMBOX_SIZE       (16)
#define DEFAULT_ACCEPTMBOX_SIZE         (8)
