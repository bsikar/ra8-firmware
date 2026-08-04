/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/uart_irq_echo/main.c
 * @brief Interrupt-driven SCI8 UART echo for EK-RA8D2 (RXI / TXI / TEI)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Companion to ``uart_hello`` (which proves the polled SCI path): this app
 * proves the **interrupt-driven** SCI path end to end -- a receive byte raises
 * RXI, the registered ISR drives ``ra8_sci_dispatch_rxi``, the echo is queued
 * with ``ra8_sci_write`` (which arms CCR0.TIE), and each TXI then pushes the next
 * byte from ``ra8_sci_dispatch_txi`` until the frame drains. Nothing in the main
 * loop touches the data path, so any byte echoed is proof the real NVIC -> ISR
 * chain ran.
 *
 * It brings the chip up exactly like ``uart_hello`` (CGC -> PLL, SCI8 TXD8 =
 * PD02 / RXD8 = PD03 at 115200 8N1), then:
 *   1. ``ra8_isr_init`` + ``ra8_isr_register`` for SCI8 RXI / TXI / TEI so each
 *      event links through the ICU IELSR table and enables its NVIC line.
 *   2. Prints a one-line banner with ``ra8_sci_write_polling`` (so the polled
 *      path is exercised too, and the host sees a prompt).
 *   3. Attaches an RX byte callback and arms a 1-byte interrupt receive with
 *      ``ra8_sci_read``; the callback re-arms the next receive and echoes the
 *      byte back over the interrupt-driven transmitter.
 *   4. Idles in ``ra8_delay_ms`` -- every echoed byte happens in handler context.
 *
 * On the EK-RA8D2 the SCI8 pins surface as the J-Link OB VCOM port; typing into
 * a terminal at 115200 8N1 echoes each character back and toggles LED1. Under
 * tools/ra8_emulator, ``--input <str>`` feeds the RX queue and the echo appears as
 * captured ``[uart]`` output.
 *
 * @note Unlike the other demos, this one drives SCI8 directly rather than the
 *       BSP ``ra8_board_uart_console`` API: the interrupt-driven SCI8 RX/TX path
 *       is the subject under test, so it must own the channel's ISRs and
 *       registers itself. (The BSP is still linked -- for LED1.)
 *
 * @author Brighton Sikarskie
 * @date 2026-06-08
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_elc.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_isr.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_sci.h"
#include "ra8_time.h"

/** @brief Compile-time settings for the echo demo. */
typedef enum : uint32_t {
  k_uart_irq_baud     = 115200U, /**< UART IRQ baud.                     */
  k_uart_irq_loop_ms  = 100U,    /**< UART IRQ loop ms.                  */
  k_uart_irq_sci_chan = 8U,      /**< Console channel: SCI8 (PD02/PD03). */
  k_uart_irq_isr_prio = 4U,      /**< NVIC priority for the SCI events.  */
} uart_irq_config_t;

/** @brief SCI8 ELC event numbers (FSP ra8d2 bsp_elc: RXI/TXI/TEI). */
typedef enum : uint16_t {
  k_uart_irq_event_rxi = 0x122U, /**< SCI8 RXI receive-data-full event.   */
  k_uart_irq_event_txi = 0x123U, /**< SCI8 TXI transmit-data-empty event. */
  k_uart_irq_event_tei = 0x124U, /**< SCI8 TEI transmit-end event.        */
} uart_irq_event_t;

/** @brief Pinout for the on-board J-Link OB CDC channel (SCI8 / PD02 + PD03). */
static const ra8_port_pin_t k_uart_irq_pin_txd = (ra8_port_pin_t)k_ra8_board_uart_console_pin_txd;
static const ra8_port_pin_t k_uart_irq_pin_rxd = (ra8_port_pin_t)k_ra8_board_uart_console_pin_rxd;

/** @brief Banner printed once over the polled path so the host sees a prompt. */
static const uint8_t k_uart_irq_banner[] = "uart_irq_echo ready\r\n";

/**
 * @var g_uart_irq_echoes
 * @brief Count of bytes echoed from interrupt context.
 *
 * @details
 * Incremented only inside ``uart_irq_rx_cb`` (reached via the real RXI ->
 * ra8_isr_dispatch -> driver path). A non-zero value is direct evidence the
 * interrupt-driven receive ran; read externally by J-Link (HIL) and by
 * tools/ra8_emulator (emulator).
 *
 * @note Read externally only; the firmware main loop never reads it back.
 * @since 0.1.0
 */
volatile uint32_t g_uart_irq_echoes = 0U;

/** @brief One-byte RX landing pad re-armed by every receive callback. */
static uint8_t s_uart_irq_rx_byte;

/**
 * @brief Halt forever in WFI -- panic stop on any init failure.
 *
 * @pre Called only after a fatal error in boot.
 * @post CPU is parked; only a debugger or external reset wakes it.
 * @since 0.1.0
 */
static void uart_irq_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/* ---- Interrupt service routines (registered via ra8_isr_register) ---------- */

/**
 * @brief SCI8 RXI ISR: drive the driver's receive state machine.
 *
 * @param[in] ctx Unused registration context.
 * @return Nothing.
 * @pre Registered for the SCI8 RXI event.
 * @post The driver has consumed one RDR byte and the RX callback has run.
 * @since 0.1.0
 */
static void uart_irq_rxi_isr(void* ctx)
{
  (void)ctx;
  ra8_sci_dispatch_rxi((uint8_t)k_uart_irq_sci_chan);
}

/**
 * @brief SCI8 TXI ISR: push the next byte of an in-flight interrupt TX.
 *
 * @param[in] ctx Unused registration context.
 * @return Nothing.
 * @pre Registered for the SCI8 TXI event.
 * @post One queued TX byte has been written to TDR (or TIE cleared at end).
 * @since 0.1.0
 */
static void uart_irq_txi_isr(void* ctx)
{
  (void)ctx;
  ra8_sci_dispatch_txi((uint8_t)k_uart_irq_sci_chan);
}

/**
 * @brief SCI8 TEI ISR: end-of-transmission housekeeping.
 *
 * @param[in] ctx Unused registration context.
 * @return Nothing.
 * @pre Registered for the SCI8 TEI event.
 * @post The driver's error/end path has run for the channel.
 * @since 0.1.0
 */
static void uart_irq_tei_isr(void* ctx)
{
  (void)ctx;
  ra8_sci_dispatch_eri((uint8_t)k_uart_irq_sci_chan);
}

/**
 * @brief RX byte callback: echo the byte and re-arm the next receive.
 *
 * @details
 * Runs in RXI handler context (driver invokes it per received byte). Echoes
 * the byte over the interrupt-driven transmitter (``ra8_sci_write`` arms TIE,
 * so the echo flows out through ``uart_irq_txi_isr``) and re-arms a fresh
 * 1-byte interrupt receive so the stream keeps draining.
 *
 * @param[in] ctx  Unused callback context.
 * @param[in] byte The received data byte.
 * @return Nothing.
 * @since 0.1.0
 */
static void uart_irq_rx_cb(void* ctx, uint8_t byte)
{
  (void)ctx;
  g_uart_irq_echoes += 1U;
  (void)ra8_board_led_toggle(k_ra8_board_led1);
  s_uart_irq_rx_byte = byte;
  (void)ra8_sci_write((uint8_t)k_uart_irq_sci_chan, &s_uart_irq_rx_byte, 1U);
  (void)ra8_sci_read((uint8_t)k_uart_irq_sci_chan, &s_uart_irq_rx_byte, 1U);
}

/* ---- Bring-up ------------------------------------------------------------- */

/**
 * @brief Route PD02 / PD03 to SCI8 TXD / RXD via the PFS PSEL field.
 *
 * @return Error code from the first failing route call, or k_ra8_ok.
 * @retval k_ra8_ok Both pins are SCI-routed.
 * @pre IOPORT module is reachable; single-threaded init context.
 * @post On success PD02 and PD03 are in SCI-async (PSEL=0x04) mode.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t uart_irq_pins_init(void)
{
  ra8_err_t err =
    ra8_pfs_route_peripheral(k_uart_irq_pin_txd, k_ra8_psel_sci_async, "uart_irq.txd8");
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_pfs_route_peripheral(k_uart_irq_pin_rxd, k_ra8_psel_sci_async, "uart_irq.rxd8");
}

/**
 * @brief Register the SCI8 RXI / TXI / TEI events with the NVIC.
 *
 * @return k_ra8_ok on success, else the first failing registration error.
 * @pre ra8_isr_init has run; interrupts not yet globally enabled.
 * @post Each SCI8 event links through IELSR and its NVIC line is enabled.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t uart_irq_register_isrs(void)
{
  ra8_err_t err = ra8_isr_register((ra8_elc_event_t)k_uart_irq_event_rxi,
                                   uart_irq_rxi_isr,
                                   nullptr,
                                   (uint8_t)k_uart_irq_isr_prio,
                                   nullptr);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_isr_register((ra8_elc_event_t)k_uart_irq_event_txi,
                         uart_irq_txi_isr,
                         nullptr,
                         (uint8_t)k_uart_irq_isr_prio,
                         nullptr);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_isr_register((ra8_elc_event_t)k_uart_irq_event_tei,
                          uart_irq_tei_isr,
                          nullptr,
                          (uint8_t)k_uart_irq_isr_prio,
                          nullptr);
}

/**
 * @brief Bring CGC + SysTick + SCI8 + LED1 + the SCI ISRs up, or panic-halt.
 *
 * @return Nothing (halts forever on any failure).
 * @since 0.1.0
 */
static void uart_irq_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    uart_irq_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    uart_irq_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &pclka_hz) != k_ra8_ok) {
    uart_irq_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    uart_irq_panic_halt();
  }
  if (uart_irq_pins_init() != k_ra8_ok) {
    uart_irq_panic_halt();
  }

  const ra8_sci_cfg_t sci_cfg = {
    .baud      = k_uart_irq_baud,
    .data_bits = k_ra8_sci_data_8,
    .parity    = k_ra8_sci_parity_none,
    .stop_bits = k_ra8_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra8_sci_init((uint8_t)k_uart_irq_sci_chan, &sci_cfg) != k_ra8_ok) {
    uart_irq_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    uart_irq_panic_halt();
  }
  if (ra8_isr_init() != k_ra8_ok) {
    uart_irq_panic_halt();
  }
  if (uart_irq_register_isrs() != k_ra8_ok) {
    uart_irq_panic_halt();
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up SCI8 + ISRs, prints, then echoes via IRQ.
 *
 * @return Never returns.
 * @pre Reset_Handler has copied .data and zeroed .bss; SystemInit set VTOR/FPU.
 * @post On clean entry the CPU idles while the SCI ISRs echo received bytes.
 * @post On any HAL init failure the function halts in WFI.
 * @since 0.1.0
 */
int32_t main(void)
{
  uart_irq_setup_or_halt();

  ra8_isr_globals_enable();

  /* Polled path: announce readiness (also exercises ra8_sci_write_polling). */
  (void)ra8_sci_write_polling((uint8_t)k_uart_irq_sci_chan,
                              k_uart_irq_banner,
                              (uint32_t)(sizeof(k_uart_irq_banner) - 1U));

  /* Attach the echo callback and arm the first interrupt-driven receive. */
  (void)ra8_sci_attach_rx_handler((uint8_t)k_uart_irq_sci_chan, uart_irq_rx_cb, nullptr);
  (void)ra8_sci_read((uint8_t)k_uart_irq_sci_chan, &s_uart_irq_rx_byte, 1U);

  while (1) {
    /* All echo work happens in the SCI ISRs; the loop only idles. */
    ra8_delay_ms((uint32_t)k_uart_irq_loop_ms);
  }

  uart_irq_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
