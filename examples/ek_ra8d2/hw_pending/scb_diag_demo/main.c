/**
 * @file examples/ek_ra8d2/hw_pending/scb_diag_demo/main.c
 * @brief Cortex-M85 SCB HAL demo: VTOR query + fault-status dump over UART.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Exercises the ra8_scb System Control Block driver (issue #583) end to end on
 * a bare EK-RA8D2, printing what the three raw-poke call sites used to read for
 * themselves so the HAL primitive can be diffed against them on the bench:
 *
 *   1. ``ra8_scb_get_vtor`` -- the current vector-table base (the primitive the
 *      DFU copy-to-run launcher writes to relocate the table). Logged as hex.
 *   2. ``ra8_scb_trace_enable`` / ``ra8_scb_trace_enabled`` -- power the ITM /
 *      DWT trace block up via DEMCR.TRCENA (the bit the log transport
 *      pre-checks) and confirm it reads back set.
 *   3. ``ra8_scb_read_fault_status`` -- one snapshot of CFSR / HFSR / DFSR /
 *      MMFAR / BFAR / AFSR + the Secure SFSR / SFAR pair (the set the exception
 *      decoder reads). Every register is logged as hex.
 *
 * The demo only QUERIES VTOR -- it never relocates the live table, which would
 * unseat the running app -- and it injects no fault, so on a clean boot every
 * fault-status register reads zero. The fixed verdict ``"scb: probe PASS"``
 * prints only when the snapshot read returned ``k_ra8_ok`` and CFSR + HFSR are
 * both zero (no latched fault); that verdict is the line the HIL scrape keys
 * on. Bare EK-RA8D2, no expansion board.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_boot_entry.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_scb.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_scb_demo_baud      = 115200U, /**< SCI8 console baud.          */
  k_scb_demo_period_ms = 1000U,   /**< Delay between probe cycles. */
} scb_demo_const_t;

/** @brief Hex-formatting constants. */
typedef enum : uint8_t {
  k_scb_demo_hex_digits  = 8U,   /**< Hex digits in a uint32_t. */
  k_scb_demo_nibble_bits = 4U,   /**< Bits per hex nibble.      */
  k_scb_demo_nibble_mask = 0xFU, /**< Low-nibble mask.          */
} scb_demo_fmt_t;

/* Console line fragments (each write is one shift-register fill). */
static const uint8_t k_scb_demo_vtor_prefix[]  = "scb: vtor=";
static const uint8_t k_scb_demo_trace_prefix[] = "scb: trace=";
static const uint8_t k_scb_demo_cfsr_prefix[]  = "scb: cfsr=";
static const uint8_t k_scb_demo_hfsr_sep[]     = " hfsr=";
static const uint8_t k_scb_demo_dfsr_prefix[]  = "scb: dfsr=";
static const uint8_t k_scb_demo_afsr_sep[]     = " afsr=";
static const uint8_t k_scb_demo_mmfar_prefix[] = "scb: mmfar=";
static const uint8_t k_scb_demo_bfar_sep[]     = " bfar=";
static const uint8_t k_scb_demo_sfsr_prefix[]  = "scb: sfsr=";
static const uint8_t k_scb_demo_sfar_sep[]     = " sfar=";
static const uint8_t k_scb_demo_crlf[]         = "\r\n";
static const uint8_t k_scb_demo_verdict_pass[] = "scb: probe PASS\r\n";
static const uint8_t k_scb_demo_verdict_fail[] = "scb: probe FAIL\r\n";

/**
 * @brief Park forever after a fatal init error.
 *
 * @pre Called only after an unrecoverable bring-up failure.
 * @post CPU is parked; only a debugger or reset wakes it.
 * @since 0.1.0
 */
static void scb_demo_panic_halt(void)
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
static void scb_demo_write(const uint8_t* data, uint32_t len)
{
  (void)ra8_board_uart_console_write(data, (size_t)len);
}

/**
 * @brief Log one 32-bit value as ``0x`` + eight fixed hex digits.
 *
 * @param[in] val Value to print (a register value or an address).
 *
 * @pre The console has been initialised.
 * @pre ``val`` fits in a uint32_t (always true).
 * @post ``"0x"`` followed by eight MSB-first hex digits has been queued.
 * @post No other console state changed.
 * @since 0.1.0
 */
static void scb_demo_write_hex32(uint32_t val)
{
  static const uint8_t k_hex[] = "0123456789ABCDEF";
  uint8_t              buf[2U + k_scb_demo_hex_digits];
  buf[0] = (uint8_t)'0';
  buf[1] = (uint8_t)'x';
  for (uint32_t i = 0U; i < (uint32_t)k_scb_demo_hex_digits; i++) {
    const uint32_t shift =
      ((uint32_t)k_scb_demo_hex_digits - 1U - i) * (uint32_t)k_scb_demo_nibble_bits;
    const uint32_t nibble = (val >> shift) & (uint32_t)k_scb_demo_nibble_mask;
    buf[2U + i]           = k_hex[nibble];
  }
  scb_demo_write(buf, (uint32_t)sizeof(buf));
}

/**
 * @brief Read + log the SCB state and return whether the boot is fault-clean.
 *
 * @details Enables the trace block, queries VTOR, and dumps the fault-status
 *          snapshot -- each value as hex -- then reports whether the snapshot
 *          read succeeded and no fault is latched (CFSR + HFSR both zero).
 *
 * @return Whether the probe is clean.
 * @retval true  Snapshot read returned ``k_ra8_ok`` and CFSR + HFSR are zero.
 * @retval false The read failed or a fault is latched in CFSR / HFSR.
 *
 * @pre The console has been initialised.
 * @pre The SCB PPB window is accessible (always on Cortex-M85).
 * @post One log block (vtor + trace + six fault-status lines) has been queued.
 * @post No SCB register other than DEMCR.TRCENA has been modified.
 * @since 0.1.0
 */
static bool scb_demo_probe_and_report(void)
{
  ra8_scb_trace_enable();

  scb_demo_write(k_scb_demo_vtor_prefix, (uint32_t)(sizeof(k_scb_demo_vtor_prefix) - 1U));
  scb_demo_write_hex32((uint32_t)ra8_scb_get_vtor());
  scb_demo_write(k_scb_demo_crlf, (uint32_t)(sizeof(k_scb_demo_crlf) - 1U));

  scb_demo_write(k_scb_demo_trace_prefix, (uint32_t)(sizeof(k_scb_demo_trace_prefix) - 1U));
  scb_demo_write_hex32(ra8_scb_trace_enabled() ? 1U : 0U);
  scb_demo_write(k_scb_demo_crlf, (uint32_t)(sizeof(k_scb_demo_crlf) - 1U));

  ra8_scb_fault_status_t fs  = {};
  const ra8_err_t        err = ra8_scb_read_fault_status(&fs);

  scb_demo_write(k_scb_demo_cfsr_prefix, (uint32_t)(sizeof(k_scb_demo_cfsr_prefix) - 1U));
  scb_demo_write_hex32(fs.cfsr);
  scb_demo_write(k_scb_demo_hfsr_sep, (uint32_t)(sizeof(k_scb_demo_hfsr_sep) - 1U));
  scb_demo_write_hex32(fs.hfsr);
  scb_demo_write(k_scb_demo_crlf, (uint32_t)(sizeof(k_scb_demo_crlf) - 1U));

  scb_demo_write(k_scb_demo_dfsr_prefix, (uint32_t)(sizeof(k_scb_demo_dfsr_prefix) - 1U));
  scb_demo_write_hex32(fs.dfsr);
  scb_demo_write(k_scb_demo_afsr_sep, (uint32_t)(sizeof(k_scb_demo_afsr_sep) - 1U));
  scb_demo_write_hex32(fs.afsr);
  scb_demo_write(k_scb_demo_crlf, (uint32_t)(sizeof(k_scb_demo_crlf) - 1U));

  scb_demo_write(k_scb_demo_mmfar_prefix, (uint32_t)(sizeof(k_scb_demo_mmfar_prefix) - 1U));
  scb_demo_write_hex32(fs.mmfar);
  scb_demo_write(k_scb_demo_bfar_sep, (uint32_t)(sizeof(k_scb_demo_bfar_sep) - 1U));
  scb_demo_write_hex32(fs.bfar);
  scb_demo_write(k_scb_demo_crlf, (uint32_t)(sizeof(k_scb_demo_crlf) - 1U));

  scb_demo_write(k_scb_demo_sfsr_prefix, (uint32_t)(sizeof(k_scb_demo_sfsr_prefix) - 1U));
  scb_demo_write_hex32(fs.sfsr);
  scb_demo_write(k_scb_demo_sfar_sep, (uint32_t)(sizeof(k_scb_demo_sfar_sep) - 1U));
  scb_demo_write_hex32(fs.sfar);
  scb_demo_write(k_scb_demo_crlf, (uint32_t)(sizeof(k_scb_demo_crlf) - 1U));

  return (err == k_ra8_ok) && (fs.cfsr == 0U) && (fs.hfsr == 0U);
}

/**
 * @brief Core bring-up: CGC -> MSTP -> TIME -> console + LED.
 *
 * @pre Reset defaults are in force (single-threaded boot context).
 * @pre No peripheral has been claimed yet.
 * @post On return every core clock + console + LED is live.
 * @post Any failure parks the CPU via ``scb_demo_panic_halt``.
 * @since 0.1.0
 */
static void scb_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    scb_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    scb_demo_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    scb_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    scb_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_scb_demo_baud) != k_ra8_ok) {
    scb_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    scb_demo_panic_halt();
  }
}

void main(void)
{
  scb_demo_setup_or_halt();
  ra8_isr_globals_enable();

  while (1) {
    const bool clean = scb_demo_probe_and_report();
    if (clean) {
      scb_demo_write(k_scb_demo_verdict_pass, (uint32_t)(sizeof(k_scb_demo_verdict_pass) - 1U));
    } else {
      scb_demo_write(k_scb_demo_verdict_fail, (uint32_t)(sizeof(k_scb_demo_verdict_fail) - 1U));
    }
    if (ra8_board_led_toggle(k_ra8_board_led1) != k_ra8_ok) {
      break;
    }
    ra8_delay_ms((uint32_t)k_scb_demo_period_ms);
  }
  scb_demo_panic_halt();
}
