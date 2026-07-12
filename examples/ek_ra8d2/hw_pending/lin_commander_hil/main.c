/**
 * @file examples/ek_ra8d2/hw_pending/lin_commander_hil/main.c
 * @brief LIN commander frame-driver HIL demo for EK-RA8D2 (SCI2 on Pmod1)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Drives a Local Interconnect Network (LIN) commander on SCI2, whose TXD2 /
 * RXD2 signals are broken out on the EK-RA8D2 Pmod1 header (J26, P801 =
 * TXD2, P802 = RXD2). Once a period it emits one LIN frame -- a header
 * (break + SYNC 0x55 + protected identifier) followed by a published data
 * response (payload bytes + enhanced checksum) -- rotating through a small
 * set of frame ids, and reports progress on the J-Link OB console (SCI8 @
 * 115200 8N1) while blinking LED1.
 *
 * Sequence:
 *   1. ``ra8_cgc_init()`` -- clock tree up (CPUCLK0 = 1 GHz, SCICLK routed).
 *   2. ``ra8_time_init()`` for the heartbeat delay.
 *   3. ``ra8_board_uart_console_init(115200)`` -- SCI8 status console.
 *   4. ``ra8_board_led_init(LED1)`` for the visual heartbeat.
 *   5. Route Pmod1 TXD2 / RXD2 to SCI async via ``ra8_pfs_route_peripheral``.
 *   6. ``ra8_sci_lin_init(SCI2, commander)`` -- Simple-LIN break-field timer.
 *   7. Loop: send a header + response for a rotating id, print, blink, delay.
 *
 * @par Hardware not yet validated (hw_pending)
 * LIN is a physical automotive bus: the SCI's TXD/RXD are logic-level UART,
 * so a real LIN segment needs an external LIN transceiver (e.g. TJA1021 /
 * MCP2003) between P801/P802 and the single-wire bus, plus at least one
 * responder node to answer subscribe frames. Without that partner this app
 * only exercises the COMMANDER transmit path: a logic analyzer on P801
 * (TXD2) can confirm the break / SYNC / PID / data / checksum framing at the
 * UART level, but end-to-end bus behaviour (the responder detection path in
 * ``ra8_sci_lin_wait_break`` / ``ra8_sci_lin_read_response``) is proven only by
 * the host unit tests, not on silicon. Promote to hw_validated only after a
 * transceiver + partner run. Pmod1 shares pins with the Octo-SPI bus; set
 * the board's Pmod1/OSPI mux (SW4) to the Pmod1 side before use.
 *
 * @author Brighton Sikarskie
 * @date 2026-07-09
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_isr.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_sci_lin.h"
#include "ra8_time.h"

/** @brief Compile-time settings for the LIN commander demo. */
typedef enum : uint32_t {
  k_lin_hil_console_baud = 115200U,   /**< SCI8 status-console baud.        */
  k_lin_hil_lin_baud     = 19200U,    /**< SCI2 LIN bus baud (<= 20 kbps).  */
  k_lin_hil_pclk_hz      = 60000000U, /**< PCLKB used for the LIN BRR calc. */
  k_lin_hil_period_ms    = 500U,      /**< Delay between LIN frames.        */
} lin_hil_config_t;

/** @brief LIN channel + frame constants. */
typedef enum : uint8_t {
  k_lin_hil_channel  = 2U, /**< SCI2 (Pmod1 TXD2/RXD2) is the LIN bus. */
  k_lin_hil_id_count = 4U, /**< Number of rotating demo frame ids.     */
  k_lin_hil_data_len = 2U, /**< Response payload length.               */
} lin_hil_frame_t;

/**
 * @brief Break-field length programmed into XCR2.BFLW.
 *
 * @details The dominant break time is ``(BFLW + 1) x (TCLK / 16)`` (div16
 * timer clock). This illustrative value yields a break comfortably longer
 * than the 13-bit-time LIN minimum at the demo baud; tune it to the actual
 * SCICLK when validating on the bench.
 */
typedef enum : uint16_t {
  k_lin_hil_break_len = 0x00A0U, /**< XCR2.BFLW break-field length. */
} lin_hil_break_t;

/** @brief The four rotating demo frame ids (arbitrary 6-bit LIN identifiers). */
typedef enum : uint8_t {
  k_lin_hil_id_0 = 0x01U, /**< Demo frame id 0. */
  k_lin_hil_id_1 = 0x02U, /**< Demo frame id 1. */
  k_lin_hil_id_2 = 0x11U, /**< Demo frame id 2. */
  k_lin_hil_id_3 = 0x3CU, /**< Demo frame id 3. */
} lin_hil_id_t;

/** @brief The two-byte demo response payload bytes. */
typedef enum : uint8_t {
  k_lin_hil_pay_0 = 0xA5U, /**< Response payload byte 0. */
  k_lin_hil_pay_1 = 0x5AU, /**< Response payload byte 1. */
} lin_hil_payload_byte_t;

/** @brief Rotating demo frame ids (arbitrary 6-bit LIN identifiers). */
static const uint8_t k_lin_hil_ids[k_lin_hil_id_count] = {
  (uint8_t)k_lin_hil_id_0,
  (uint8_t)k_lin_hil_id_1,
  (uint8_t)k_lin_hil_id_2,
  (uint8_t)k_lin_hil_id_3,
};

/** @brief Two-byte response payload published for every frame. */
static const uint8_t k_lin_hil_payload[k_lin_hil_data_len] = {
  (uint8_t)k_lin_hil_pay_0,
  (uint8_t)k_lin_hil_pay_1,
};

/** @brief Banner + per-frame markers (must remain 7-bit ASCII). */
static const uint8_t k_lin_hil_banner[]    = "lin_commander_hil: SCI2 LIN commander up\r\n";
static const uint8_t k_lin_hil_frame_msg[] = "lin_commander_hil: frame sent\r\n";
static const uint8_t k_lin_hil_fault_msg[] = "lin_commander_hil: LIN TX error\r\n";

/**
 * @brief Halt forever in WFI -- used as a panic stop on init failure.
 *
 * @pre Called only after a fatal error in boot.
 * @post CPU is parked; only a debugger or external reset wakes it.
 * @since 0.1.0
 */
static void lin_hil_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring up CGC + SysTick + console + LED, route Pmod1, init LIN.
 *
 * @details
 * The LIN commander lives on SCI2; its TXD2 (P801) and RXD2 (P802) are
 * routed to SCI async before ``ra8_sci_lin_init`` programs the Simple-LIN
 * break-field timer. Any failure panic-halts so the fault is visible on a
 * debugger.
 *
 * @pre Reset_Handler has run; single-threaded boot context.
 * @post On return, SCI2 is a live LIN commander and the console is up.
 * @since 0.1.0
 */
static void lin_hil_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    lin_hil_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    lin_hil_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    lin_hil_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_lin_hil_console_baud) != k_ra8_ok) {
    lin_hil_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    lin_hil_panic_halt();
  }

  /* Route the Pmod1 UART pins to SCI async so the LIN signals reach P801/P802. */
  if (ra8_pfs_route_peripheral((ra8_port_pin_t)k_ra8_board_pmod1_uart_txd,
                               k_ra8_psel_sci_async,
                               "lin.txd2") != k_ra8_ok) {
    lin_hil_panic_halt();
  }
  if (ra8_pfs_route_peripheral((ra8_port_pin_t)k_ra8_board_pmod1_uart_rxd,
                               k_ra8_psel_sci_async,
                               "lin.rxd2") != k_ra8_ok) {
    lin_hil_panic_halt();
  }

  const ra8_sci_lin_cfg_t lin_cfg = {
    .uart =
      {
        .baud      = (uint32_t)k_lin_hil_lin_baud,
        .data_bits = k_ra8_sci_data_8,
        .parity    = k_ra8_sci_parity_none,
        .stop_bits = k_ra8_sci_stop_1,
        .pclk_hz   = (uint32_t)k_lin_hil_pclk_hz,
      },
    .role            = k_ra8_sci_lin_role_commander,
    .timer_clk       = k_ra8_sci_lin_clk_div16,
    .break_field_len = (uint16_t)k_lin_hil_break_len,
  };
  if (ra8_sci_lin_init((uint8_t)k_lin_hil_channel, &lin_cfg) != k_ra8_ok) {
    lin_hil_panic_halt();
  }
}

/**
 * @brief Send one LIN frame: header + published data response.
 *
 * @details
 * Emits the break + SYNC + PID header for ``id`` and then publishes the
 * demo payload with the enhanced (PID-folded) checksum, exercising the
 * commander transmit path end-to-end.
 *
 * @param[in] id 6-bit LIN frame identifier to drive.
 *
 * @return ``ra8_err_t`` -- ``k_ra8_ok`` or the first driver error.
 * @retval k_ra8_ok Header + response clocked out.
 * @retval other   Propagated from the LIN driver.
 *
 * @pre ``lin_hil_setup_or_halt`` has configured SCI2 as a LIN commander.
 * @pre ``id`` is a 6-bit LIN identifier.
 * @post On success one complete LIN frame has been transmitted.
 * @post On failure no further bytes are pushed.
 * @since 0.1.0
 */
static ra8_err_t lin_hil_send_frame(uint8_t id)
{
  const ra8_err_t herr = ra8_sci_lin_send_header((uint8_t)k_lin_hil_channel, id);
  if (herr != k_ra8_ok) {
    return herr;
  }
  const uint8_t pid = ra8_sci_lin_pid(id);
  return ra8_sci_lin_send_response((uint8_t)k_lin_hil_channel,
                                   k_ra8_sci_lin_checksum_enhanced,
                                   pid,
                                   k_lin_hil_payload,
                                   (uint8_t)k_lin_hil_data_len);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up the LIN commander then drives frames.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On clean entry the CPU stays in the LIN-frame + blink loop forever.
 * @post On any HAL init failure the function halts in WFI.
 * @since 0.1.0
 */
int32_t main(void)
{
  lin_hil_setup_or_halt();
  ra8_isr_globals_enable();
  (void)ra8_board_uart_console_write(k_lin_hil_banner, (size_t)(sizeof(k_lin_hil_banner) - 1U));

  uint8_t idx = 0U;
  while (1) {
    const uint8_t   id  = k_lin_hil_ids[idx];
    const ra8_err_t err = lin_hil_send_frame(id);
    if (err == k_ra8_ok) {
      (void)ra8_board_uart_console_write(k_lin_hil_frame_msg,
                                         (size_t)(sizeof(k_lin_hil_frame_msg) - 1U));
    } else {
      (void)ra8_board_uart_console_write(k_lin_hil_fault_msg,
                                         (size_t)(sizeof(k_lin_hil_fault_msg) - 1U));
    }

    if (ra8_board_led_toggle(k_ra8_board_led1) != k_ra8_ok) {
      break;
    }
    idx = (uint8_t)((idx + 1U) % (uint8_t)k_lin_hil_id_count);
    ra8_delay_ms((uint32_t)k_lin_hil_period_ms);
  }

  lin_hil_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
