/**
 * @file examples/ek_ra8d2/hw_pending/eth_l3switch_forward_demo/main.c
 * @brief L3 Ethernet switch / frame-forwarding config demo (ra8_eth_mfwd + ra8_layer3_switch)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Configures the RA8D2 Ethernet frame-forwarding path that no other example
 * referenced (recon gap #135). Two drivers, honestly separated:
 *
 *   1. ``ra8_eth_mfwd`` -- the **real** Message Forwarding engine that sits
 *      between the GMAC ports and the CPU Agent (GWCA) making per-frame
 *      port-to-port / port-to-host decisions (HUM Ch 31-32 "Ethernet"). This
 *      app programs the per-port forwarding-destination masks
 *      (``ra8_eth_mfwd_set_forwarding_masks``, permissive 0x7F on every port),
 *      routes port-0 ingress into a GWCA RX queue
 *      (``ra8_eth_mfwd_route_queue``), and reads the MFWD status
 *      (``ra8_eth_mfwd_get_status``).
 *   2. ``ra8_layer3_switch`` -- the FSP-shaped L3-switch **facade**. The RA8D2
 *      silicon carries no L3 switch block (the driver header says so), so the
 *      facade's route-table ops are documented placeholders that return
 *      ``k_ra8_err_not_supported``. The app exercises the working lifecycle
 *      (``open`` / ``status_get`` / ``close``) and calls ``route_add`` once to
 *      log -- honestly -- that the L3 route table is a not-supported placeholder
 *      on this part. The real forwarding config is the MFWD path above.
 *
 * The fixed verdict ``"l3sw: forward PASS"`` prints only when every real
 * operation (MFWD program + status, L3 open / status / close) returned
 * ``k_ra8_ok``. The ``route_add`` placeholder code is logged but never fails the
 * verdict.
 *
 * hw_pending: ``tools/ra8_emulator`` has no Ethernet / MFWD peripheral model, and
 * the EK-RA8D2 Ethernet wire is marginal (#21), so this is compile-gated and
 * bench-only -- matching the driver-gap example wave (#182-188). Proving a
 * frame is actually forwarded to the right egress port needs a multi-port
 * topology (two links + a traffic source, bench wiring #89).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_eth_mfwd.h"
#include "ra8_isr.h"
#include "ra8_layer3_switch.h"
#include "ra8_mstp.h"
#include "ra8_time.h"

/** @brief Demo tunables (illustrative route). */
typedef enum : uint32_t {
  k_l3_baud       = 115200U,     /**< SCI8 console baud.           */
  k_l3_period_ms  = 1000U,       /**< Delay between cycles.        */
  k_l3_route_ip   = 0xC0A80100U, /**< 192.168.1.0 (network order). */
  k_l3_route_mask = 0xFFFFFF00U, /**< /24 subnet mask.             */
} l3_const_t;

/** @brief L3-switch facade MTU. */
typedef enum : uint16_t {
  k_l3_mtu_bytes = 1500U, /**< Per-port MTU handed to the facade. */
} l3_mtu_t;

/** @brief Formatting, indices, and small fields. */
typedef enum : uint8_t {
  k_l3_radix        = 10U,   /**< Decimal serialiser radix.                */
  k_l3_dec_u32_max  = 10U,   /**< Max decimal digits for a uint32_t.       */
  k_l3_mask_entries = 3U,    /**< Forwarding-mask array size (p0/p1/host). */
  k_l3_fwd_mask_all = 0x7FU, /**< 7-bit "all destinations" forward mask.   */
  k_l3_src_port     = 0U,    /**< GMAC ingress port routed to a queue.     */
  k_l3_rx_queue     = 0U,    /**< GWCA RX queue index for port-to-host.    */
  k_l3_egress_port  = 1U,    /**< Illustrative route egress port.          */
  k_l3_port_count   = 2U,    /**< Physical port count for the facade.      */
  k_l3_promisc_off  = 0U,    /**< Facade promiscuous mode off.             */
} l3_fmt_t;

/* Console line fragments (kept short so each write is one shift-register
 * fill; the periodic log is the only output path). */
static const uint8_t k_l3_fwd_prefix[]   = "l3sw: fwd_sts=0x";
static const uint8_t k_l3_open_prefix[]  = "l3sw: l3_open=";
static const uint8_t k_l3_promisc_sep[]  = " promisc=";
static const uint8_t k_l3_route_prefix[] = "l3sw: route_add=";
static const uint8_t k_l3_route_sfx[]    = " (placeholder)\r\n";
static const uint8_t k_l3_crlf[]         = "\r\n";
static const uint8_t k_l3_verdict_pass[] = "l3sw: forward PASS\r\n";
static const uint8_t k_l3_verdict_fail[] = "l3sw: forward FAIL\r\n";

/**
 * @brief Park forever after a fatal init error.
 *
 * @pre Called only after an unrecoverable bring-up failure.
 * @post CPU is parked; only a debugger or reset wakes it.
 * @since 0.1.0
 */
static void l3_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Write a byte span to the SCI8 console, discarding the status.
 *
 * @param[in] data Non-NULL byte span to transmit.
 * @param[in] len  Byte count (0 is a no-op).
 *
 * @pre ``ra8_board_uart_console_init`` has succeeded.
 * @pre ``data`` points at ``len`` readable bytes.
 * @post ``len`` bytes have been queued to the console UART.
 * @since 0.1.0
 */
static void l3_write(const uint8_t* data, uint32_t len)
{
  (void)ra8_board_uart_console_write(data, (size_t)len);
}

/**
 * @brief Serialise an unsigned 32-bit value into decimal ASCII.
 *
 * @param[out] buf Destination, at least ``k_l3_dec_u32_max`` bytes.
 * @param[in]  val Value to serialise.
 *
 * @return Number of digits written (1..10).
 *
 * @pre ``buf`` is non-NULL and sized for the widest uint32_t.
 * @pre No trailing NUL is required by the caller.
 * @post ``buf`` holds the most-significant digit first.
 * @post The return value is in ``[1, k_l3_dec_u32_max]``.
 * @since 0.1.0
 */
static uint32_t l3_u32_to_dec(uint8_t* buf, uint32_t val)
{
  if (val == 0U) {
    buf[0] = (uint8_t)'0';
    return 1U;
  }
  uint8_t  tmp[k_l3_dec_u32_max];
  uint32_t n = 0U;
  uint32_t v = val;
  while (v != 0U) {
    tmp[n] = (uint8_t)('0' + (uint8_t)(v % (uint32_t)k_l3_radix));
    v      = v / (uint32_t)k_l3_radix;
    n++;
  }
  for (uint32_t i = 0U; i < n; i++) {
    buf[i] = tmp[n - 1U - i];
  }
  return n;
}

/**
 * @brief Log one unsigned 32-bit value as decimal ASCII.
 *
 * @param[in] val Value to print.
 *
 * @pre The console has been initialised.
 * @pre ``val`` fits in a uint32_t (always true).
 * @post The decimal digits of ``val`` have been queued to the console.
 * @since 0.1.0
 */
static void l3_write_u32(uint32_t val)
{
  uint8_t        buf[k_l3_dec_u32_max];
  const uint32_t n = l3_u32_to_dec(buf, val);
  l3_write(buf, n);
}

/**
 * @brief Program the real MFWD forwarding path and read its status.
 *
 * @details
 * Sets the permissive per-port forwarding masks (0x7F -> all destinations on
 * ports 0, 1 and the host/GWCA port), routes GMAC port-0 ingress into GWCA RX
 * queue 0, and logs the MFWD status word.
 *
 * @return True iff the mask, route, and status calls returned ``k_ra8_ok``.
 *
 * @pre ``ra8_eth_mfwd_init`` has succeeded.
 * @pre The console has been initialised.
 * @post One ``l3sw: fwd_sts=0x`` line has been queued.
 * @post FWPBFC0/1/2 masks and the port-0 queue route are programmed.
 * @since 0.1.0
 */
static bool l3_program_forwarding(void)
{
  bool          ok                       = true;
  const uint8_t masks[k_l3_mask_entries] = {
    (uint8_t)k_l3_fwd_mask_all,
    (uint8_t)k_l3_fwd_mask_all,
    (uint8_t)k_l3_fwd_mask_all,
  };
  if (ra8_eth_mfwd_set_forwarding_masks(masks) != k_ra8_ok) {
    ok = false;
  }
  if (ra8_eth_mfwd_route_queue((uint8_t)k_l3_src_port, (uint8_t)k_l3_rx_queue) != k_ra8_ok) {
    ok = false;
  }
  uint32_t        sts = 0U;
  const ra8_err_t err = ra8_eth_mfwd_get_status(&sts);
  if (err != k_ra8_ok) {
    ok = false;
  }
  l3_write(k_l3_fwd_prefix, (uint32_t)(sizeof(k_l3_fwd_prefix) - 1U));
  l3_write_u32(sts);
  l3_write(k_l3_crlf, (uint32_t)(sizeof(k_l3_crlf) - 1U));
  return ok;
}

/**
 * @brief Exercise the FSP-shaped L3-switch facade lifecycle.
 *
 * @details
 * Opens the facade (2 ports, 1500-byte MTU, non-promiscuous), reads back the
 * open / promiscuous flags, and closes it. Between open and close it calls
 * ``ra8_layer3_switch_route_add`` once and logs the returned code: on this
 * silicon the L3 route table is a documented placeholder that returns
 * ``k_ra8_err_not_supported``, so that code is logged (never failing the
 * verdict) to make the placeholder explicit.
 *
 * @return True iff open, status_get and close returned ``k_ra8_ok``.
 *
 * @pre The console has been initialised.
 * @pre No prior facade open is still outstanding.
 * @post The facade has been opened, queried, and closed exactly once.
 * @post One ``l3_open=`` line and one ``route_add=`` placeholder line queued.
 * @since 0.1.0
 */
static bool l3_run_facade(void)
{
  bool                          ok  = true;
  const ra8_layer3_switch_cfg_t cfg = {
    .port_count  = (uint8_t)k_l3_port_count,
    .mtu_bytes   = (uint16_t)k_l3_mtu_bytes,
    .promiscuous = (uint8_t)k_l3_promisc_off,
  };
  if (ra8_layer3_switch_open(&cfg) != k_ra8_ok) {
    ok = false;
  }
  uint8_t         is_open = 0U;
  uint8_t         promisc = 0U;
  const ra8_err_t st_err  = ra8_layer3_switch_status_get(&is_open, &promisc);
  if (st_err != k_ra8_ok) {
    ok = false;
  }
  l3_write(k_l3_open_prefix, (uint32_t)(sizeof(k_l3_open_prefix) - 1U));
  l3_write_u32((uint32_t)is_open);
  l3_write(k_l3_promisc_sep, (uint32_t)(sizeof(k_l3_promisc_sep) - 1U));
  l3_write_u32((uint32_t)promisc);
  l3_write(k_l3_crlf, (uint32_t)(sizeof(k_l3_crlf) - 1U));

  const ra8_layer3_switch_route_t route = {
    .dst_ip      = (uint32_t)k_l3_route_ip,
    .mask        = (uint32_t)k_l3_route_mask,
    .egress_port = (uint8_t)k_l3_egress_port,
  };
  const ra8_err_t route_err = ra8_layer3_switch_route_add(&route);
  l3_write(k_l3_route_prefix, (uint32_t)(sizeof(k_l3_route_prefix) - 1U));
  l3_write_u32((uint32_t)route_err);
  l3_write(k_l3_route_sfx, (uint32_t)(sizeof(k_l3_route_sfx) - 1U));

  if (ra8_layer3_switch_close() != k_ra8_ok) {
    ok = false;
  }
  return ok;
}

/**
 * @brief Run one cycle: MFWD forwarding config + L3-switch facade lifecycle.
 *
 * @return True iff every real operation in the cycle returned ``k_ra8_ok``.
 *
 * @pre ``l3_arm`` initialised the MFWD engine.
 * @pre The console has been initialised.
 * @post One log block (MFWD status + facade open + route placeholder) queued.
 * @post The forwarding masks + queue route are (re)programmed.
 * @since 0.1.0
 */
static bool l3_run_cycle(void)
{
  bool ok = true;
  if (!l3_program_forwarding()) {
    ok = false;
  }
  if (!l3_run_facade()) {
    ok = false;
  }
  return ok;
}

/**
 * @brief Core bring-up: CGC -> MSTP -> TIME -> console + LED.
 *
 * @pre Reset defaults are in force (single-threaded boot context).
 * @pre No peripheral has been claimed yet.
 * @post On return every core clock + console + LED is live.
 * @post Any failure parks the CPU via ``l3_panic_halt``.
 * @since 0.1.0
 */
static void l3_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    l3_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    l3_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    l3_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    l3_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_l3_baud) != k_ra8_ok) {
    l3_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    l3_panic_halt();
  }
}

/**
 * @brief Bring up the MFWD forwarding engine.
 *
 * @details
 * ``ra8_eth_mfwd_init`` ungates the shared Ethernet switch MSTP gate and resets
 * the MFWD registers so the per-port forwarding masks and queue routing can be
 * programmed by the run cycle.
 *
 * @return ``ra8_err_t`` error code from ``ra8_eth_mfwd_init``.
 * @retval k_ra8_ok MFWD engine is up.
 * @retval Other   Forwarded from ``ra8_eth_mfwd_init``.
 *
 * @pre ``l3_setup_or_halt`` has ungated MSTP.
 * @pre IRQs are masked or this is single-threaded init.
 * @post On ``k_ra8_ok`` the MFWD forwarding registers are writable.
 * @post On failure no partial MFWD state is relied upon by the caller.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t l3_arm(void)
{
  return ra8_eth_mfwd_init();
}

void main(void)
{
  l3_setup_or_halt();
  ra8_isr_globals_enable();

  if (l3_arm() != k_ra8_ok) {
    l3_panic_halt();
  }

  while (1) {
    const bool healthy = l3_run_cycle();
    if (healthy) {
      l3_write(k_l3_verdict_pass, (uint32_t)(sizeof(k_l3_verdict_pass) - 1U));
    } else {
      l3_write(k_l3_verdict_fail, (uint32_t)(sizeof(k_l3_verdict_fail) - 1U));
    }
    if (ra8_board_led_toggle(k_ra8_board_led1) != k_ra8_ok) {
      break;
    }
    ra8_delay_ms((uint32_t)k_l3_period_ms);
  }
  l3_panic_halt();
}
