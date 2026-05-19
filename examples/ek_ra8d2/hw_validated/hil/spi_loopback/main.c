/**
 * @file examples/ek_ra8d2/spi_loopback/main.c
 * @brief SPI_B internal-loopback smoke test for the EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Standalone EVM-tier app that exercises the SPI_B controller driver
 * (``libs/ra_hal/ra_spi.h``) via the silicon's internal-loopback
 * mode -- no external CIPO/COPI jumper required. The flow:
 *
 *   1. ``ra_cgc_init`` -- bring CPUCLK0 / PCLKA up.
 *   2. ``ra_mstp_init`` + ``ra_pfs_route_peripheral`` for SCI8
 *      console pins (PD02 / PD03).
 *   3. ``ra_spi_init(0, ...)`` at 1 MHz mode-0 MSB-first.
 *   4. Direct register stamp ``SPCR2.SPLP = 1`` so COPI is fed
 *      back into CIPO inside the chip; no need to route or wire
 *      RSPI pins (HUM Ch 43.2.5 p 2889 -- ``k_ra_spcr2_mask_splp``).
 *      The HAL doesn't expose a "loopback" knob today; documenting
 *      the raw write keeps the demo single-file like canfd_loopback.
 *   5. Walk a 16-byte test pattern (``0xA0..0xAF``) through
 *      ``ra_spi_xfer8`` and compare RX vs TX byte-for-byte.
 *      LED1 toggles on each successful round-trip; LED2 latches
 *      ON if any byte mismatches.
 *
 * SCI8 prints ``"spi: pass\r\n"`` once a second.
 *
 * Bare EK-RA8D2 only -- no shields or external loopback wiring.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_spi_regs.h"
#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_mstp.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_spi.h"
#include "ra_time.h"

/** @brief App-wide tunables. */
typedef enum : uint32_t {
  k_spi_demo_baud        = 115200U,
  k_spi_demo_period_ms   = 1000U,
  k_spi_demo_spi_baud_hz = 1000000U,
  k_spi_demo_sci_channel = 8U,
  k_spi_demo_spi_channel = 0U,
} spi_demo_const_t;

/** @brief Test pattern width and seed. */
typedef enum : uint8_t {
  k_spi_demo_pattern_len  = 16U,
  k_spi_demo_pattern_base = 0xA0U,
} spi_demo_byte_t;

/** @brief Pinout for SCI8 console. */
static const ra_port_pin_t k_spi_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_spi_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

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
 * @brief Bring CGC + SysTick + console + SPI_B up. Panic-halts on fail.
 *
 * @details
 * Stamps SPCR2.SPLP = 1 directly after ``ra_spi_init`` so the
 * silicon ties COPI to CIPO internally and no external loopback
 * jumper is needed. The HAL surface does not yet expose this knob;
 * the raw write is documented above per the canfd_loopback pattern.
 *
 * @since 0.1.0
 */
static void spi_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    spi_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    spi_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    spi_demo_panic_halt();
  }
  if (ra_mstp_init() != k_ra_ok) {
    spi_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    spi_demo_panic_halt();
  }

  if (ra_pfs_route_peripheral(k_spi_demo_pin_txd, k_ra_psel_sci_async, "spi_loopback.txd8") !=
      k_ra_ok) {
    spi_demo_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_spi_demo_pin_rxd, k_ra_psel_sci_async, "spi_loopback.rxd8") !=
      k_ra_ok) {
    spi_demo_panic_halt();
  }

  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_spi_demo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_spi_demo_sci_channel, &sci_cfg) != k_ra_ok) {
    spi_demo_panic_halt();
  }

  const ra_spi_cfg_t spi_cfg = {
    .baud_hz   = k_spi_demo_spi_baud_hz,
    .pclka_hz  = pclka_hz,
    .mode      = k_ra_spi_mode_0,
    .lsb_first = false,
  };
  if (ra_spi_init((uint8_t)k_spi_demo_spi_channel, &spi_cfg) != k_ra_ok) {
    spi_demo_panic_halt();
  }
  /* Internal-loopback: COPI -> CIPO, no external pins required. */
  ra_spi((uint8_t)k_spi_demo_spi_channel)->SPCR2 |= (uint32_t)k_ra_spcr2_mask_splp;

  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    spi_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    spi_demo_panic_halt();
  }
}

/**
 * @brief Walk the test pattern once and return whether RX matches TX.
 *
 * @return ``true`` when every byte read back matches the byte sent.
 *
 * @pre ``ra_spi_init`` succeeded and SPLP is set.
 * @post No external bus traffic occurred (loopback is internal).
 *
 * @since 0.1.0
 */
static bool spi_demo_round_trip_ok(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_spi_demo_pattern_len; i++) {
    const uint8_t tx = (uint8_t)((uint8_t)k_spi_demo_pattern_base + i);
    uint8_t       rx = 0U;
    if (ra_spi_xfer8((uint8_t)k_spi_demo_spi_channel, tx, &rx) != k_ra_ok) {
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
  ra_isr_globals_enable();

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
      (void)ra_board_led_on(k_ra_board_led2);
    }
    if (ra_sci_write_polling((uint8_t)k_spi_demo_sci_channel, msg, msg_len) != k_ra_ok) {
      break;
    }
    if (ra_board_led_toggle(k_ra_board_led1) != k_ra_ok) {
      break;
    }
    ra_delay_ms(k_spi_demo_period_ms);
  }

  spi_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
