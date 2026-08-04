/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/spi_loopback/main.c
 * @brief SPI_B internal-loopback HIL test for the EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Standalone EVM-tier app that exercises the SPI_B controller driver
 * (``libs/ra8_hal/ra8_spi.h``) via the silicon's internal-loopback
 * mode -- no external CIPO/COPI jumper required. The flow:
 *
 *   1. ``ra8_cgc_init`` -- bring CPUCLK0 / PCLKA up.
 *   2. ``ra8_mstp_init`` + ``ra8_board_uart_console_init`` for the
 *      SCI8 J-Link console (PD02 / PD03).
 *   3. ``ra8_spi_init(0, .loopback=true, ...)`` at 1 MHz mode-0
 *      MSB-first.  The HAL programmes SPCR2.SPLP2=1 BEFORE asserting
 *      SPCR.SPE so the write is honored (HUM Ch 43.2.4 p 2889 --
 *      SPCR2 writes require SPE=0).  SPLP2 is the non-inverting
 *      loopback variant (rx = tx); the inverting SPLP would flip
 *      every bit.  No external CIPO/COPI wiring needed -- the
 *      silicon ties COPI back to CIPO internally.
 *   4. Walk a 16-byte test pattern (``0xA0..0xAF``) through
 *   5. ``ra8_spi_xfer8`` and compare RX vs TX byte-for-byte.
 *      LED1 toggles on each successful round-trip; LED2 latches
 *      ON if any byte mismatches.
 *
 * SCI8 prints ``"spi: pass\r\n"`` once a second.
 *
 * Bare EK-RA8D2 only -- no shields or external loopback wiring.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_spi.h"
#include "ra8_time.h"

/** @brief App-wide tunables. */
typedef enum : uint32_t {
  k_spi_demo_baud        = 115200U,  /**< SPI demo baud.        */
  k_spi_demo_period_ms   = 1000U,    /**< SPI demo period ms.   */
  k_spi_demo_spi_baud_hz = 1000000U, /**< SPI demo SPI baud Hz. */
  k_spi_demo_spi_channel = 0U,       /**< SPI demo SPI channel. */
} spi_demo_const_t;

/** @brief Test pattern width and seed. */
typedef enum : uint8_t {
  k_spi_demo_pattern_len  = 16U,   /**< SPI demo pattern length. */
  k_spi_demo_pattern_base = 0xA0U, /**< SPI demo pattern base.   */
} spi_demo_byte_t;

static const uint8_t k_spi_demo_msg_pass[] = "spi: pass\r\n";
static const uint8_t k_spi_demo_msg_fail[] = "spi: FAIL\r\n";

/** @brief Park forever after a fatal init failure.
 *
 * @pre Called only after a fatal error in boot.
 * @post CPU is parked; only a debugger or external reset wakes it.
 * @since 0.1.0
 */
static void spi_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC, MSTP and SysTick up. Returns PCLKA Hz via ``out_pclka``.
 *
 * @pre Reset_Handler has finished C-runtime init.
 * @post CGC, MSTP and SysTick are live; ``*out_pclka`` holds PCLKA in Hz.
 * @since 0.1.0
 */
static void spi_demo_clocks_or_halt(uint32_t* out_pclka)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    spi_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    spi_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, out_pclka) != k_ra8_ok) {
    spi_demo_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    spi_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    spi_demo_panic_halt();
  }
}

/**
 * @brief Bring CGC + SysTick + console + SPI_B up. Panic-halts on fail.
 *
 * @details
 * Passes ``cfg.loopback = true`` to ``ra8_spi_init`` so the HAL
 * programmes SPCR2.SPLP2=1 while SPE is still 0 (HUM Ch 43.2.4
 * p 2889).  The silicon then ties COPI to CIPO internally; no
 * external loopback jumper required.
 *
 * @since 0.1.0
 */
static void spi_demo_setup_or_halt(void)
{
  uint32_t pclka_hz = 0U;
  spi_demo_clocks_or_halt(&pclka_hz);
  if (ra8_board_uart_console_init((uint32_t)k_spi_demo_baud) != k_ra8_ok) {
    spi_demo_panic_halt();
  }

  const ra8_spi_cfg_t spi_cfg = {
    .baud_hz   = k_spi_demo_spi_baud_hz,
    .pclka_hz  = pclka_hz,
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = false,
    .loopback  = true,
  };
  if (ra8_spi_init((uint8_t)k_spi_demo_spi_channel, &spi_cfg) != k_ra8_ok) {
    spi_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    spi_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    spi_demo_panic_halt();
  }
}

/**
 * @brief Walk the test pattern once and return whether RX matches TX.
 *
 * @return ``true`` when every byte read back matches the byte sent.
 *
 * @pre ``ra8_spi_init`` succeeded and SPLP is set.
 * @post No external bus traffic occurred (loopback is internal).
 *
 * @since 0.1.0
 */
static bool spi_demo_round_trip_ok(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_spi_demo_pattern_len; i++) {
    const uint8_t tx = (uint8_t)((uint8_t)k_spi_demo_pattern_base + i);
    uint8_t       rx = 0U;
    if (ra8_spi_xfer8((uint8_t)k_spi_demo_spi_channel, tx, &rx) != k_ra8_ok) {
      return false;
    }
    if (rx != tx) {
      return false;
    }
  }
  return true;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up SPI_B + loops a self-test pattern.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On clean entry the CPU stays in the loopback + blink loop.
 * @post On any HAL hard error LED2 latches ON.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  spi_demo_setup_or_halt();
  ra8_isr_globals_enable();

  while (1) {
    const bool     ok = spi_demo_round_trip_ok();
    const uint8_t* msg;
    uint32_t       msg_len;
    if (ok) {
      msg     = k_spi_demo_msg_pass;
      msg_len = (uint32_t)(sizeof(k_spi_demo_msg_pass) - 1U);
    } else {
      msg     = k_spi_demo_msg_fail;
      msg_len = (uint32_t)(sizeof(k_spi_demo_msg_fail) - 1U);
    }
    if (!ok) {
      (void)ra8_board_led_on(k_ra8_board_led2);
    }
    (void)ra8_board_uart_console_write(msg, (size_t)msg_len);
    if (ra8_board_led_toggle(k_ra8_board_led1) != k_ra8_ok) {
      break;
    }
    ra8_delay_ms(k_spi_demo_period_ms);
  }

  spi_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
