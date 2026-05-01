/**
 * @file main.c
 * @brief ThreadX + lwIP TCP echo demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip up the same way ``uart_hello`` does (CGC -> SCI8 @
 * 115200 8N1) and then hands control to ThreadX. The ThreadX
 * application-define hook configures lwIP on top of our hardware
 * Ethernet driver shim (``port/lwip/netif/ra_etha_netif.c``):
 *
 *   1. ``tcpip_init()`` to start the lwIP core thread.
 *   2. Build a static ``struct netif`` and add it via ``netif_add`` with
 *      ::ra_etha_netif_init as the init callback. The netif is bound to
 *      ETHA port 0.
 *   3. Set static IP 192.168.1.50 / 255.255.255.0 (no gateway).
 *   4. ``netif_set_default`` + ``netif_set_up``.
 *   5. The worker thread opens a BSD-style socket on port 7 (the IANA
 *      echo protocol) and loops accept -> recv -> send -> close.
 *
 * Each accepted byte is logged to SCI8 as ``"echoed N bytes\r\n"`` so
 * the J-Link OB CDC port gives visible feedback. This is the lwIP
 * counterpart of ``examples/threadx_netx_tcp_echo`` -- the protocol
 * stack is now lwIP rather than NetX Duo, but the hardware path
 * (CGC + RMII pins + SCI8) is identical.
 *
 * @note No TX/RX hooks are wired up to the real ETHA descriptor
 *       rings here -- the demo proves the ThreadX/lwIP integration
 *       and the ra_etha_netif glue compile and link together. A
 *       follow-up wave attaches ::ra_etha_netif_attach_tx and the
 *       RX-bottom-half pump.
 *
 * @author Brighton Sikarskie
 * @date 2026-05-01
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_etha.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

#ifndef RA_SIMULATOR_MODE
#include "lwip/err.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "ra_etha_netif.h"
#include "tx_api.h"
#endif

/**
 * @enum demo_config_t
 * @brief Compile-time settings for the lwIP TCP-echo demo.
 *
 * @details
 * Every numeric used during bring-up appears here so the magic-
 * number lint never sees a bare integer. The SCI8 channel matches
 * the on-board J-Link OB CDC bridge pins (PD_02 / PD_03).
 */
typedef enum : uint32_t {
  k_demo_baud           = 115200U,
  k_demo_sci_channel    = 8U,
  k_demo_thread_stack   = 8192U,
  k_demo_app_thread_pri = 8U,
  k_demo_echo_port      = 7U,
  k_demo_listen_backlog = 1U,
  k_demo_rx_buf_bytes   = 512U,
  k_demo_log_buf_bytes  = 80U,
} demo_config_t;

/**
 * @enum demo_decimal_t
 * @brief Constants used by the tiny base-10 byte-to-ASCII helper.
 */
typedef enum : uint8_t {
  k_demo_decimal_base = 10U,
} demo_decimal_t;

/** @brief Pinout for the on-board J-Link OB CDC channel (SCI8 / PD02 + PD03). */
static const ra_port_pin_t k_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief RMII signal pinout for the EK-RA8D2 v1 J64 connector. */
static const ra_port_pin_t k_demo_pin_eth_ref_clk =
  (ra_port_pin_t)(((uint16_t)k_ra_port_7 << 8) | (uint16_t)k_ra_pin_0);
static const ra_port_pin_t k_demo_pin_eth_mdc =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_1);
static const ra_port_pin_t k_demo_pin_eth_mdio =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_demo_pin_eth_txd0 =
  (ra_port_pin_t)(((uint16_t)k_ra_port_7 << 8) | (uint16_t)k_ra_pin_1);
static const ra_port_pin_t k_demo_pin_eth_txd1 =
  (ra_port_pin_t)(((uint16_t)k_ra_port_7 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_demo_pin_eth_tx_en =
  (ra_port_pin_t)(((uint16_t)k_ra_port_7 << 8) | (uint16_t)k_ra_pin_3);
static const ra_port_pin_t k_demo_pin_eth_rxd0 =
  (ra_port_pin_t)(((uint16_t)k_ra_port_7 << 8) | (uint16_t)k_ra_pin_4);
static const ra_port_pin_t k_demo_pin_eth_rxd1 =
  (ra_port_pin_t)(((uint16_t)k_ra_port_7 << 8) | (uint16_t)k_ra_pin_5);
static const ra_port_pin_t k_demo_pin_eth_rx_dv =
  (ra_port_pin_t)(((uint16_t)k_ra_port_7 << 8) | (uint16_t)k_ra_pin_6);
static const ra_port_pin_t k_demo_pin_eth_rx_er =
  (ra_port_pin_t)(((uint16_t)k_ra_port_7 << 8) | (uint16_t)k_ra_pin_7);
static const ra_port_pin_t k_demo_pin_eth_crs_dv =
  (ra_port_pin_t)(((uint16_t)k_ra_port_7 << 8) | (uint16_t)k_ra_pin_8);

/** @brief Locally-administered unicast MAC for this board. */
static const uint8_t k_demo_mac[6] = {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U};

/** @brief IPv4 address: 192.168.1.50 / 255.255.255.0. */
static const uint8_t k_demo_ip[4]   = {192U, 168U, 1U, 50U};
static const uint8_t k_demo_mask[4] = {255U, 255U, 255U, 0U};
static const uint8_t k_demo_gw[4]   = {192U, 168U, 1U, 1U};

#ifndef RA_SIMULATOR_MODE
/* lwIP / ThreadX state. ThreadX requires statically-allocated control
 * blocks (NASA Power of 10 Rule 3 -- no dynamic memory). */
static struct netif s_netif;

/* ThreadX worker thread. */
static TX_THREAD s_demo_thread;
static UCHAR     s_demo_stack[k_demo_thread_stack];
#endif /* !RA_SIMULATOR_MODE */

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
 * @brief Route PD_02 / PD_03 to SCI8 TXD / RXD.
 *
 * @retval k_ra_ok                       Both pins routed.
 * @retval k_ra_err_gpio_invalid_port    PFS port out of range.
 * @retval k_ra_err_gpio_invalid_pin     PFS pin out of range.
 * @retval k_ra_err_gpio_conflict        Pin already claimed.
 *
 * @pre IOPORT module reachable.
 * @post On success PD_02 / PD_03 are in SCI-async mode.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t demo_sci_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_demo_pin_txd, k_ra_psel_sci_async, "lwiptcp.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_demo_pin_rxd, k_ra_psel_sci_async, "lwiptcp.rxd8");
}

/**
 * @brief Route the eleven RMII pins to the on-chip Ethernet MAC.
 *
 * @retval k_ra_ok                      Pins routed.
 * @retval k_ra_err_gpio_invalid_port   PSEL target out of range.
 * @retval k_ra_err_gpio_invalid_pin    Pin index out of range.
 * @retval k_ra_err_gpio_conflict       Pin owned by another module.
 *
 * @pre IOPORT module is reachable.
 * @post On success the RMII pins are PSEL=0x11 (Ethernet RMII).
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t demo_eth_pins_init(void)
{
  const ra_port_pin_t pins[] = {
    k_demo_pin_eth_ref_clk,
    k_demo_pin_eth_mdc,
    k_demo_pin_eth_mdio,
    k_demo_pin_eth_txd0,
    k_demo_pin_eth_txd1,
    k_demo_pin_eth_tx_en,
    k_demo_pin_eth_rxd0,
    k_demo_pin_eth_rxd1,
    k_demo_pin_eth_rx_dv,
    k_demo_pin_eth_rx_er,
    k_demo_pin_eth_crs_dv,
  };
  for (uint8_t i = 0U; i < (uint8_t)(sizeof(pins) / sizeof(pins[0])); i++) {
    const ra_err_t err = ra_pfs_route_peripheral(pins[i], k_ra_psel_ether_rmii, "lwiptcp.eth");
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}

/**
 * @brief Bring CGC + SCI8 + RMII pins up. Panic-halts on any failure.
 *
 * @pre Reset_Handler / SystemInit complete.
 * @post On success SCI8 is sending at 115200 8N1 and RMII pins are routed.
 *
 * @since 0.1.0
 */
static void demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    demo_panic_halt();
  }
  if (demo_sci_pins_init() != k_ra_ok) {
    demo_panic_halt();
  }
  if (demo_eth_pins_init() != k_ra_ok) {
    demo_panic_halt();
  }

  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_demo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_demo_sci_channel, &sci_cfg) != k_ra_ok) {
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
  uint32_t len = (uint32_t)strlen(s);
  (void)ra_sci_write_polling((uint8_t)k_demo_sci_channel, (const uint8_t*)s, len);
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

#ifndef RA_SIMULATOR_MODE
/**
 * @brief Log "[lwip] echoed N bytes\r\n" to SCI8.
 *
 * @param[in] n Number of bytes echoed (0..65535 typical).
 *
 * @since 0.1.0
 */
static void demo_log_echo(uint32_t n)
{
  char              buf[k_demo_log_buf_bytes];
  char*             p          = buf;
  static const char k_prefix[] = "[lwip] echoed ";
  for (uint32_t i = 0U; i < (uint32_t)(sizeof(k_prefix) - 1U); i++) {
    *p = k_prefix[i];
    p++;
  }
  uint32_t v = n;
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
  } /* dig/di scoped to else-branch */
  static const char k_suffix[] = " bytes\r\n";
  for (uint32_t i = 0U; i < (uint32_t)(sizeof(k_suffix) - 1U); i++) {
    *p = k_suffix[i];
    p++;
  }
  *p = '\0';
  demo_print(buf);
  /* demo_append_byte is shared with sibling examples; keep it
   * referenced so -Wunused-function never trips on it. */
  (void)demo_append_byte;
}

/**
 * @brief Bring the lwIP netif up at 192.168.1.50 / 24.
 *
 * @return ::err_t
 * @retval ERR_OK  netif added, set default, set up.
 * @retval !ERR_OK netif_add failed (returns its error verbatim).
 *
 * @pre ``tcpip_init()`` has been called.
 * @post On success ``netif_default == &s_netif`` and the netif is up.
 *
 * @since 0.1.0
 */
static err_t demo_netif_bring_up(void)
{
  ip4_addr_t ip;
  ip4_addr_t mask;
  ip4_addr_t gw;
  IP4_ADDR(&ip, k_demo_ip[0], k_demo_ip[1], k_demo_ip[2], k_demo_ip[3]);
  IP4_ADDR(&mask, k_demo_mask[0], k_demo_mask[1], k_demo_mask[2], k_demo_mask[3]);
  IP4_ADDR(&gw, k_demo_gw[0], k_demo_gw[1], k_demo_gw[2], k_demo_gw[3]);

  /* Populate the MAC before netif_add so ::ra_etha_netif_init can
   * publish it through MIB2 if desired. */
  s_netif.hwaddr_len = 6U;
  for (uint8_t i = 0U; i < 6U; i++) {
    s_netif.hwaddr[i] = k_demo_mac[i];
  }

  /* netif->state encodes the ETHA port index for ra_etha_netif_init. */
  struct netif* added = netif_add(&s_netif,
                                  &ip,
                                  &mask,
                                  &gw,
                                  (void*)(uintptr_t)k_ra_etha_port_0,
                                  ra_etha_netif_init,
                                  tcpip_input);
  if (added == NULL) {
    return ERR_IF;
  }
  netif_set_default(&s_netif);
  netif_set_up(&s_netif);
  return ERR_OK;
}

/**
 * @brief Echo loop: socket -> bind -> listen -> accept -> recv/send -> close.
 *
 * @pre ``demo_netif_bring_up`` returned ERR_OK.
 * @post Loop runs forever; never returns.
 *
 * @since 0.1.0
 */
static void demo_echo_loop(void)
{
  while (1) {
    int listen_fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
      demo_print("[lwip] socket() failed\r\n");
      (void)tx_thread_sleep(100U);
      continue;
    }

    struct sockaddr_in addr = {};
    addr.sin_family         = AF_INET;
    addr.sin_addr.s_addr    = htonl(INADDR_ANY);
    addr.sin_port           = htons((uint16_t)k_demo_echo_port);

    if (lwip_bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
      demo_print("[lwip] bind() failed\r\n");
      (void)lwip_close(listen_fd);
      (void)tx_thread_sleep(100U);
      continue;
    }
    if (lwip_listen(listen_fd, (int)k_demo_listen_backlog) != 0) {
      demo_print("[lwip] listen() failed\r\n");
      (void)lwip_close(listen_fd);
      (void)tx_thread_sleep(100U);
      continue;
    }

    while (1) {
      struct sockaddr_in peer      = {};
      socklen_t          peer_len  = (socklen_t)sizeof(peer);
      int                client_fd = lwip_accept(listen_fd, (struct sockaddr*)&peer, &peer_len);
      if (client_fd < 0) {
        demo_print("[lwip] accept() failed; relistening\r\n");
        break;
      }

      uint8_t rx[k_demo_rx_buf_bytes];
      while (1) {
        int n = lwip_recv(client_fd, rx, (size_t)sizeof(rx), 0);
        if (n <= 0) {
          break;
        }
        int sent_total = 0;
        while (sent_total < n) {
          int s = lwip_send(client_fd, &rx[sent_total], (size_t)(n - sent_total), 0);
          if (s <= 0) {
            sent_total = -1;
            break;
          }
          sent_total += s;
        }
        if (sent_total < 0) {
          break;
        }
        demo_log_echo((uint32_t)n);
      }
      (void)lwip_close(client_fd);
    }

    (void)lwip_close(listen_fd);
  }
}

/**
 * @brief ThreadX worker entry: bring lwIP up, then run the echo loop.
 *
 * @param[in] thread_input Unused.
 *
 * @pre ``tcpip_init`` has run.
 * @post Echo loop runs forever.
 *
 * @since 0.1.0
 */
static void demo_thread_entry(ULONG thread_input)
{
  (void)thread_input;
  demo_print("[lwip] bringing lwIP up...\r\n");
  if (demo_netif_bring_up() != ERR_OK) {
    demo_print("[lwip] netif bring-up failed\r\n");
    return;
  }
  demo_print("[lwip] listening on TCP/7 at 192.168.1.50\r\n");
  demo_echo_loop();
}

/**
 * @brief ThreadX system-define hook: build worker thread + lwIP core.
 *
 * @param[in] first_unused_memory Pointer to free RAM provided by the
 *   ThreadX port; unused -- we statically allocate.
 *
 * @pre ``tx_kernel_enter()`` has been called.
 * @post tcpip_thread is created and one worker thread runs the echo loop.
 *
 * @since 0.1.0
 */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  /* Spin up the lwIP tcpip_thread once. NULL completion-callback is
   * fine; the call returns when the lwIP core is initialised. */
  tcpip_init(NULL, NULL);

  (void)tx_thread_create(&s_demo_thread,
                         "lwip_echo",
                         demo_thread_entry,
                         0U,
                         s_demo_stack,
                         (ULONG)sizeof(s_demo_stack),
                         (UINT)k_demo_app_thread_pri,
                         (UINT)k_demo_app_thread_pri,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}
#endif /* !RA_SIMULATOR_MODE */

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
  ra_isr_globals_enable();
  demo_print("[lwip] booting ThreadX + lwIP...\r\n");

#ifndef RA_SIMULATOR_MODE
  /* Hands control over to ThreadX permanently. */
  tx_kernel_enter();
#endif

  /* Should never return. */
  demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
