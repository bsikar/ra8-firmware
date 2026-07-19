/**
 * @file examples/_unsupported/ptp_time_transmitter/main.c
 * @brief IEEE 1588-2019 PTP controller smoke test for EK-RA8D2
 *        (1 Hz Sync + Announce over Ethernet, MAC 02:00:00:00:00:01)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip up via ``ra8_cgc_init()``, routes the on-board PEF7071
 * PHY pins via ``ra8_board_ethernet_init()`` (RGMII per EK-RA8D2 v1 UM
 * Table 26 p 33), opens the NIC at MAC ``02:00:00:00:00:01``
 * (locally-administered unicast) and waits for PHY link-up. With the
 * link up, ``ra8_ptp_open`` arms the PTP block in controller role on domain
 * 0 with a 1 Hz Sync interval, ``ra8_ptp_set_time`` seeds the counter
 * from a fixed Unix epoch (RTC integration is out of scope for this
 * smoke test), and the main loop emits one Sync + one Announce every
 * second (IEEE 1588-2019 sec 13.6 / 13.5).
 *
 * SCI8 (PD_02 / PD_03) at 115200 8N1 prints the running PTP time once
 * per loop iteration so an attached terminal can see the clock advance.
 *
 * The full sixteen-pin RGMII map (TXC/TX_CTL/TXD0..3, RXC/RX_CTL/
 * RXD0..3, MDC/MDIO, plus PHY reset / interrupt / power) is owned by
 * ``ra8_board_ethernet_init()``. See ``libs/ra8_board_ek_ra8d2`` for the
 * authoritative pin list.
 *
 * @par Architectural ring
 * [Ring 6 / APP] {World: S} -- application-layer code that runs in
 * the Secure world.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-29
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_eth.h"
#include "ra8_isr.h"
#include "ra8_ptp.h"
#include "ra8_time.h"

/** @brief MAC-address byte indices. */
typedef enum : uint8_t {
  k_mac_byte_0 = 0U, /**< MAC byte 0. */
  k_mac_byte_1 = 1U, /**< MAC byte 1. */
  k_mac_byte_2 = 2U, /**< MAC byte 2. */
  k_mac_byte_3 = 3U, /**< MAC byte 3. */
  k_mac_byte_4 = 4U, /**< MAC byte 4. */
  k_mac_byte_5 = 5U, /**< MAC byte 5. */
} mac_byte_idx_t;

/**
 * @enum ptp_time_transmitter_config_t
 * @brief Numeric configuration constants for the demo.
 *
 * @details
 * Every literal used by the bring-up path lives here so the magic-
 * number lint never sees a bare integer. The console is the on-board
 * J-Link OB CDC bridge (SCI8 / PD_02 / PD_03), owned by the BSP.
 */
typedef enum : uint32_t {
  k_ptp_time_transmitter_baud         = 115200U, /**< J-Link OB CDC baud.           */
  k_ptp_time_transmitter_link_poll_ms = 100U,    /**< PHY BMSR poll period.         */
  k_ptp_time_transmitter_sync_ms      = 1000U,   /**< 1 Hz Sync / Announce cadence. */
} ptp_time_transmitter_config_t;

/**
 * @enum ptp_time_transmitter_epoch_t
 * @brief Initial PTP wall-clock seed.
 *
 * @details
 * Real firmware would read the on-chip RTC; this smoke test hard-codes
 * a recent Unix epoch second (``2026-01-01T00:00:00Z`` =
 * ``1767225600``) as a reproducible starting point. The Sync messages
 * carry monotonic timestamps after this seed.
 */
typedef enum : uint64_t {
  k_ptp_time_transmitter_seed_seconds = 1767225600ULL, /**< Ptp time transmitter seed seconds. */
} ptp_time_transmitter_epoch_t;

/**
 * @enum ptp_time_transmitter_log_layout_t
 * @brief Decimal-formatting limits for the PTP-time log line.
 */
typedef enum : uint32_t {
  k_ptp_time_transmitter_log_buf_bytes = 24U, /**< Per-field decimal buffer.   */
  k_ptp_time_transmitter_dec_radix     = 10U, /**< Base-10 decimal.            */
  k_ptp_time_transmitter_dec_max       = 20U, /**< 20 digits fit any uint64_t. */
} ptp_time_transmitter_log_layout_t;

/**
 * @brief Local MAC address (locally-administered, unicast).
 *
 * @details Bit 1 of the first octet is set (locally administered) and
 * bit 0 is clear (unicast). Same value the ethernet_tcp_echo demo uses.
 */
static const uint8_t k_ptp_time_transmitter_mac[] = {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U};

/**
 * @brief Park the CPU forever in WFI on fatal init failure.
 *
 * @pre Called only after a fatal error in boot.
 * @post CPU is parked; only a debugger or external reset wakes it.
 *
 * @since 0.1.0
 */
static void ptp_time_transmitter_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Send a NUL-terminated ASCII line over SCI8 (best-effort).
 *
 * @param[in] s ASCII string (NUL-terminated). May be ``nullptr``.
 *
 * @pre ra8_board_uart_console_init() succeeded for the console channel.
 * @post Bytes have been polled out of the console TXD (or silently
 *       discarded on backpressure -- this is logging only).
 *
 * @since 0.1.0
 */
static void ptp_time_transmitter_log(const char* s)
{
  if (s == nullptr) {
    return;
  }
  uint32_t len = 0U;
  while (s[len] != '\0') {
    len++;
  }
  (void)ra8_board_uart_console_write((const uint8_t*)s, (size_t)len);
}

/**
 * @brief Convert a uint64_t to ASCII decimal in caller-supplied buffer.
 *
 * @param[in]  value Value to format.
 * @param[out] buf   Destination buffer (>= 21 bytes incl. NUL).
 * @param[in]  cap   Capacity of buf in bytes.
 *
 * @pre buf != nullptr.
 * @pre cap >= 21.
 * @post buf holds a NUL-terminated decimal representation of value.
 *
 * @since 0.1.0
 */
static void ptp_time_transmitter_u64_to_ascii(uint64_t value, char* buf, uint32_t cap)
{
  if (buf == nullptr || cap == 0U) {
    return;
  }
  char     tmp[k_ptp_time_transmitter_dec_max + 1U];
  uint32_t n = 0U;
  if (value == 0U) {
    tmp[n] = '0';
    n++;
  } else {
    while (value > 0U && n < k_ptp_time_transmitter_dec_max) {
      tmp[n] = (char)('0' + (char)(value % k_ptp_time_transmitter_dec_radix));
      value /= k_ptp_time_transmitter_dec_radix;
      n++;
    }
  }
  uint32_t out = 0U;
  while (n > 0U && out < (cap - 1U)) {
    n--;
    buf[out] = tmp[n];
    out++;
  }
  buf[out] = '\0';
}

/**
 * @brief Convert a uint32_t to ASCII decimal in caller-supplied buffer.
 *
 * @param[in]  value Value to format.
 * @param[out] buf   Destination buffer (>= 11 bytes incl. NUL).
 * @param[in]  cap   Capacity of buf in bytes.
 *
 * @pre buf != nullptr.
 * @pre cap >= 11.
 * @post buf holds a NUL-terminated decimal representation of value.
 *
 * @since 0.1.0
 */
static void ptp_time_transmitter_u32_to_ascii(uint32_t value, char* buf, uint32_t cap)
{
  ptp_time_transmitter_u64_to_ascii((uint64_t)value, buf, cap);
}

/**
 * @brief Block until the PHY reports link-up via BMSR.
 *
 * @details Polls ``ra8_eth_link_status`` every 100 ms until ``link_up == 1``.
 * Logs once on first success. Mirrors the helper in the
 * ``ethernet_tcp_echo`` example so the two demos behave the same way at
 * power-on.
 *
 * @pre ra8_eth_open succeeded.
 * @post On return, the PHY has reported link-up at least once.
 *
 * @since 0.1.0
 */
static void ptp_time_transmitter_wait_link(void)
{
  while (1) {
    ra8_eth_link_t  st  = {};
    const ra8_err_t err = ra8_eth_link_status(&st);
    if (err == k_ra8_ok && st.link_up != 0U) {
      ptp_time_transmitter_log("ra8d2: link up\r\n");
      return;
    }
    ra8_delay_ms(k_ptp_time_transmitter_link_poll_ms);
  }
}

/**
 * @brief Bring up clocks, time, GPIO. Panic-halts on any failure.
 *
 * @param[out] cpuclk0_hz Receives CPUCLK0 rate.
 *
 * @pre cpuclk0_hz non-null.
 * @post CGC + SysTick + LED1 GPIO are usable.
 *
 * @since 0.1.0
 */
static void ptp_time_transmitter_clocks_or_halt(uint32_t* cpuclk0_hz)
{
  if (ra8_cgc_init() != k_ra8_ok) {
    ptp_time_transmitter_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, cpuclk0_hz) != k_ra8_ok) {
    ptp_time_transmitter_panic_halt();
  }
  if (ra8_time_init(*cpuclk0_hz) != k_ra8_ok) {
    ptp_time_transmitter_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    ptp_time_transmitter_panic_halt();
  }
}

/**
 * @brief Bring the J-Link OB CDC console up at 115200 8N1 via the BSP.
 *
 * @pre Clocks are already initialized.
 * @post The console (SCI8 / PD_02 / PD_03) is ready to print log lines.
 *
 * @since 0.1.0
 */
static void ptp_time_transmitter_console_or_halt(void)
{
  if (ra8_board_uart_console_init((uint32_t)k_ptp_time_transmitter_baud) != k_ra8_ok) {
    ptp_time_transmitter_panic_halt();
  }
}

/**
 * @brief Bring up the Ethernet NIC and wait for PHY link.
 *
 * @pre Clocks + SCI8 are already initialized.
 * @post NIC TX/RX rings are running and the PHY reports BMSR.LINK = 1.
 *
 * @since 0.1.0
 */
static void ptp_time_transmitter_eth_or_halt(void)
{
  /* RGMII pinmux per EK-RA8D2 v1 UM Table 26 p 33. */
  if (ra8_board_ethernet_init() != k_ra8_ok) {
    ptp_time_transmitter_panic_halt();
  }
  const ra8_eth_cfg_t eth_cfg = {
    .mac_address        = {k_ptp_time_transmitter_mac[k_mac_byte_0],
                           k_ptp_time_transmitter_mac[k_mac_byte_1],
                           k_ptp_time_transmitter_mac[k_mac_byte_2],
                           k_ptp_time_transmitter_mac[k_mac_byte_3],
                           k_ptp_time_transmitter_mac[k_mac_byte_4],
                           k_ptp_time_transmitter_mac[k_mac_byte_5]},
    .channel            = 0U,
    .num_tx_descriptors = 0U,
    .num_rx_descriptors = 0U,
    .buffer_size        = 0U,
  };
  if (ra8_eth_open(&eth_cfg) != k_ra8_ok) {
    ptp_time_transmitter_panic_halt();
  }
  ptp_time_transmitter_wait_link();
}

/**
 * @brief Bring up the PTP block, switch to controller role, seed wall clock.
 *
 * @pre Ethernet NIC is open and the PHY is link-up.
 * @post PTP block is in controller role on domain 0 with the wall clock
 *       seeded from k_ptp_time_transmitter_seed_seconds.
 *
 * @since 0.1.0
 */
static void ptp_time_transmitter_ptp_or_halt(void)
{
  const ra8_ptp_cfg_t ptp_cfg = {
    .domain        = k_ra8_ptp_domain_default,
    .sync_interval = k_ra8_ptp_sync_int_1,
    .mac_addr      = {k_ptp_time_transmitter_mac[k_mac_byte_0],
                      k_ptp_time_transmitter_mac[k_mac_byte_1],
                      k_ptp_time_transmitter_mac[k_mac_byte_2],
                      k_ptp_time_transmitter_mac[k_mac_byte_3],
                      k_ptp_time_transmitter_mac[k_mac_byte_4],
                      k_ptp_time_transmitter_mac[k_mac_byte_5]},
    .clock_class   = k_ra8_ptp_clock_class_default,
  };
  if (ra8_ptp_open(&ptp_cfg) != k_ra8_ok) {
    ptp_time_transmitter_panic_halt();
  }
  if (ra8_ptp_set_role(k_ra8_ptp_role_controller) != k_ra8_ok) {
    ptp_time_transmitter_panic_halt();
  }
  if (ra8_ptp_set_time(k_ptp_time_transmitter_seed_seconds, 0U) != k_ra8_ok) {
    ptp_time_transmitter_panic_halt();
  }
}

/**
 * @brief Bring CGC + SysTick + GPIO + SCI8 + Ethernet + PTP up.
 *        Panic-halts on any failure.
 *
 * @details
 * Mirrors the ``ethernet_tcp_echo`` setup for everything up to the NIC
 * open, then layers ``ra8_ptp_open`` on top in controller role with a 1 Hz
 * Sync interval and the locally-administered MAC. ``ra8_ptp_set_time``
 * seeds the wall clock from a fixed Unix epoch second so a downstream
 * peripheral sees a sensible timestamp on the very first Sync.
 *
 * @since 0.1.0
 */
static void ptp_time_transmitter_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  ptp_time_transmitter_clocks_or_halt(&cpuclk0_hz);
  ptp_time_transmitter_console_or_halt();
  ptp_time_transmitter_eth_or_halt();
  ptp_time_transmitter_ptp_or_halt();
}

/**
 * @brief Emit one Sync + one Announce, then log the running PTP time.
 *
 * @return Error code from the first failing PTP call, or k_ra8_ok.
 *
 * @retval k_ra8_ok                  Sync, Announce, and time-read all OK.
 * @retval k_ra8_err_invalid_state   PTP block not open / not controller.
 *
 * @pre ``ra8_ptp_open`` returned k_ra8_ok and role is controller.
 * @post One Sync + one Announce queued on the egress path.
 * @post One log line emitted to SCI8.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t ptp_time_transmitter_step(void)
{
  ra8_err_t err = ra8_ptp_send_sync();
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_ptp_send_announce();
  if (err != k_ra8_ok) {
    return err;
  }

  uint64_t sec  = 0U;
  uint32_t nsec = 0U;
  err           = ra8_ptp_get_time(&sec, &nsec);
  if (err != k_ra8_ok) {
    return err;
  }

  char sec_buf[k_ptp_time_transmitter_log_buf_bytes]  = {};
  char nsec_buf[k_ptp_time_transmitter_log_buf_bytes] = {};
  ptp_time_transmitter_u64_to_ascii(sec, sec_buf, k_ptp_time_transmitter_log_buf_bytes);
  ptp_time_transmitter_u32_to_ascii(nsec, nsec_buf, k_ptp_time_transmitter_log_buf_bytes);
  ptp_time_transmitter_log("ptp: ");
  ptp_time_transmitter_log(sec_buf);
  ptp_time_transmitter_log(".");
  ptp_time_transmitter_log(nsec_buf);
  ptp_time_transmitter_log(" s\r\n");
  (void)ra8_board_led_toggle(k_ra8_board_led1);
  return k_ra8_ok;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up CGC + Ethernet + PTP controller,
 *        then emits 1 Hz Sync/Announce forever.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On clean entry the CPU stays in the Sync/Announce loop forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  ptp_time_transmitter_setup_or_halt();
  ra8_isr_globals_enable();
  ptp_time_transmitter_log("ra8d2: PTP controller ready (1 Hz Sync, domain 0)\r\n");

  while (1) {
    if (ptp_time_transmitter_step() != k_ra8_ok) {
      break;
    }
    ra8_delay_ms(k_ptp_time_transmitter_sync_ms);
  }

  ptp_time_transmitter_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
