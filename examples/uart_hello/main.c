/**
 * @file main.c
 * @brief UART "hello world" smoke test for EK-RA8D2 (SCI8 @ 38400)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @par Status: still blocked, but further along
 *
 * Hardware verified bit-by-bit on real silicon via SWD. What works:
 *   - Pinout (EK-RA8D2 user's manual Table 13: J-Link OB CDC <-> PD02 TX
 *     / PD03 RX, with PD04/PD05 as RTS/CTS).
 *   - Channel (SCI8, confirmed against FSP `quickstart_ek_ra8d2_ep`).
 *   - SCI_B driver layout (HUM Ch 38, p 2174+).
 *   - Pin routing: PD02 PFS reads back PMR=1, PSEL=0x04.
 *   - MSTPCRB[SCI8] is cleared (read 0x40203004 -> bit 23 = 0).
 *   - Driver writes are visible: CCR0=0x11 (TE|RE), CCR1=0x30
 *     (SPB2DT|SPB2IO, so TXD idles HIGH when TE=0), CCR2.BRR=0x0F
 *     (115200 baud at 60 MHz PCLKA), CCR3=0x1200 (LSBF + 8-bit CHR).
 *   - CSR after a TDR write reads TEND=1, TDRE=1, RXDMON=1 (chip
 *     thinks it's done shifting).
 *
 * What still doesn't work: **PD02 PIDR never toggles**. 20 SWD samples
 * of the PFS register during repeated TDR writes all show bit 1 = 1
 * (line at IDLE HIGH). For a 115200-baud frame each bit is 8.7 us, so
 * even slow SWD sampling should catch the line low at least once
 * during the ~1.3 ms transmission of the greeting. It never does.
 *
 * Best hypothesis: SCI_B's operating clock (TCLK -- either PCLK or
 * the independent SCICLK route) isn't actually reaching the SCI8
 * shifter. CCR0 / TDR latch fine because those are PCLK accesses,
 * but TCLK gates the bit-shift state machine. FSP's
 * `quickstart_ek_ra8d2_ep` ra_cfg.txt explicitly configures SCICLK
 * source = PLL2R, /4 -- our `ra_cgc_init` doesn't touch SCICKCR /
 * SCKDIVCR2.SCICKDIV / SCISPISEL at all. Next step: have ra_cgc
 * route SCICLK from a known-good source and re-test.
 *
 * @details
 * Brings the chip up on HOCO + PLL via ``ra_cgc_init()``, configures
 * SCI8 TXD8=PD_02 / RXD8=PD_03 in async mode at 38400 8N1, and
 * prints ``"hello, ra8d2!\r\n"`` once a second while toggling LED1
 * as a heartbeat. The CDC channel of the on-board J-Link OB on the
 * EK-RA8D2 surfaces these pins as a virtual serial port on the host,
 * so connecting a terminal at 38400 8N1 to that port should show
 * the stream once the SCICLK gating is fixed in the CGC driver.
 *
 * Sequence:
 *   1. ``ra_cgc_init()`` -- HOCO + PLL up, CPUCLK0 / PCLKB at their
 *      rated rates. Required for an accurate baud-rate divisor.
 *   2. ``ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz)`` --
 *      SCI_B's BRR is computed against PCLKA (HUM Ch 38 line 1
 *      explicitly says "In this section, PCLK refers to PCLKA").
 *   3. ``ra_pfs_route_peripheral()`` for PD_02 and PD_03 to put
 *      them in SCI async mode (PSEL = ``k_ra_psel_sci_async``).
 *   4. ``ra_sci_init(8, &cfg)`` -- 38400 8N1, no parity, one stop.
 *   5. ``ra_time_init(cpuclk0_hz)`` for the heartbeat delay.
 *   6. ``ra_gpio_output_init(k_ra_pin_led1, low)`` for the visual
 *      heartbeat.
 *   7. Loop: write the greeting, toggle LED1, sleep 1 s.
 *
 * Verification: open the J-Link OB CDC port at 38400 8N1 (e.g.
 * ``screen /dev/cu.usbmodem<...> 115200`` on macOS or
 * ``minicom -D /dev/ttyACM0 -b 115200`` on Linux). You should see
 * one greeting line per second and LED1 toggling in lock-step.
 *
 * If you see garbled bytes, the baud divisor is wrong -- almost
 * always because the CGC didn't bring PCLKB up where you expected.
 *
 * @par Architectural ring
 * [Ring 6 / APP] {World: S} -- application-layer code that runs in
 * the Secure world.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-28
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_system_regs.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_register_protection.h"
#include "ra_sci.h"
#include "ra_time.h"

/**
 * @brief Hand-route SCICLK = PLL1R / 8 so SCI_B's TCLK has a real edge
 *        source.
 *
 * @details
 * The on-chip CGC driver doesn't currently program SCICKCR /
 * SCICKDIVCR -- they're left at reset defaults (SCICLK = MOCO with
 * /1 divider). The SCI_B engine's bit-shift state machine is
 * clocked from SCICLK (with the PCLK synchronizer bypassed via
 * CCR3.BPEN = 1), so without a real SCICLK route the engine never
 * advances even though every register write looks correct.
 *
 * Procedure follows HUM 9.2.54 verbatim:
 *  1. Unlock PRCR.PRC0 (CGC group).
 *  2. Set SCICKCR.SCICKSREQ = 1; poll until SCICKSRDY = 1.
 *  3. Programme SCICKDIVCR + SCICKCR.SCICKSEL with the new source.
 *  4. Clear SCICKCR.SCICKSREQ; poll until SCICKSRDY = 0.
 *  5. Re-lock PRCR.
 *
 * Choices: SCICKSEL = 8 (PLL1R) and SCICKDIV = 4 (/8). PLL1 is the
 * system clock per SCKSCR=5 readback, so PLL1R is guaranteed to be
 * running by the time we get here. Final SCICLK rate depends on
 * PLL1R's tap, but is in the ~30-60 MHz ballpark -- enough for
 * 115200 baud with the BRR our driver writes.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_uart_hello_sci_off_dvcr = 0x054U,
  k_uart_hello_sci_off_ckcr = 0x055U,
} uart_hello_scickcr_off_t;

typedef enum : uint8_t {
  k_uart_hello_scickcr_sel_pll1r = 0x08U,
  k_uart_hello_scickcr_sreq_set  = (1U << 6),
  k_uart_hello_scickcr_srdy_mask = (1U << 7),
  k_uart_hello_scickdivcr_div8   = 0x04U,
} uart_hello_scickcr_bit_t;

static void uart_hello_route_sciclk(void)
{
  volatile uint8_t* divcr = (volatile uint8_t*)(k_ra_system_base_addr + k_uart_hello_sci_off_dvcr);
  volatile uint8_t* ckcr  = (volatile uint8_t*)(k_ra_system_base_addr + k_uart_hello_sci_off_ckcr);

  RA_PROTECTED_WRITE(k_ra_prcr_unlock_cgc)
  {
    /* HUM 9.2.54 step 3: request switch. Keep current SEL value while
     * SREQ is asserted. */
    *ckcr = (uint8_t)(*ckcr | k_uart_hello_scickcr_sreq_set);
    while ((*ckcr & k_uart_hello_scickcr_srdy_mask) == 0U) {
      /* HUM 9.2.54 step 4: spin until SCICLK is gated and re-routable. */
    }
    /* HUM 9.2.54 step 5: programme divider + source while SREQ=1. */
    *divcr = k_uart_hello_scickdivcr_div8;
    *ckcr  = (uint8_t)(k_uart_hello_scickcr_sel_pll1r | k_uart_hello_scickcr_sreq_set);
    /* HUM 9.2.54 step 6: clear SREQ to start the new clock. */
    *ckcr = k_uart_hello_scickcr_sel_pll1r;
    while ((*ckcr & k_uart_hello_scickcr_srdy_mask) != 0U) {
      /* HUM 9.2.54 step 7: spin until the new SCICLK is stable. */
    }
  }
}

/** @brief Compile-time settings for the demo. */
typedef enum : uint32_t {
  k_uart_hello_baud        = 38400U,
  k_uart_hello_period_ms   = 100U,
  k_uart_hello_sci_channel = 8U,
} uart_hello_config_t;

/** @brief Pinout for the on-board J-Link OB CDC channel (SCI8 / PD02 + PD03). */
static const ra_port_pin_t k_uart_hello_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_uart_hello_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief Greeting string sent every period. Must remain ASCII. */
static const uint8_t k_uart_hello_greeting[] = "hello, ra8d2!\r\n";

/**
 * @brief Halt forever in WFI -- used as a panic stop on init failure.
 *
 * @pre Called only after a fatal error in boot.
 *
 * @post CPU is parked; only a debugger or external reset wakes it.
 *
 * @since 0.1.0
 */
static void uart_hello_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Route PD_02 / PD_03 to SCI8 TXD / RXD via the PFS PSEL field.
 *
 * @return Error code from the first failing route call, or k_ra_ok.
 *
 * @retval k_ra_ok                       Both pins are SCI-routed.
 * @retval k_ra_err_gpio_invalid_port    PFS port index out of range.
 * @retval k_ra_err_gpio_invalid_pin     PFS pin index out of range.
 * @retval k_ra_err_gpio_conflict        Pin already claimed by another owner.
 *
 * @pre IOPORT module is reachable.
 * @pre Caller is single-threaded init context.
 *
 * @post On success PD_02 and PD_03 are in SCI-async (PSEL=0x04) mode.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t uart_hello_pins_init(void)
{
  ra_err_t err =
    ra_pfs_route_peripheral(k_uart_hello_pin_txd, k_ra_psel_sci_async, "uart_hello.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_uart_hello_pin_rxd, k_ra_psel_sci_async, "uart_hello.rxd8");
}

/**
 * @brief Application entry. Brings up CGC + SCI0 + LED1 then prints.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On clean entry the CPU stays in the print + blink loop forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @since 0.1.0
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Bring CGC + SysTick + SCI8 + LED1 up. Panic-halts on any fail.
 *
 * @details
 * Hardcoded `pclka_hz = 20 MHz` is the empirically-verified PCLKA
 * post-`ra_cgc_init()` (PLL1 currently lands at ~160 MHz, /8 = 20 MHz).
 * Once the CGC driver brings PLL1 up to the rated 480-1000 MHz, this
 * constant moves up and the example can switch back to 115200 baud.
 *
 * @since 0.1.0
 */
static void uart_hello_setup_or_halt(void)
{
  uint32_t       cpuclk0_hz = 0U;
  const uint32_t pclka_hz   = 20000000U;

  if (ra_cgc_init() != k_ra_ok) {
    uart_hello_panic_halt();
  }
  /* Route SCICLK = PLL1R / 8 -- the SCI_B engine clocks its bit shifter
   * from SCICLK (CCR3.BPEN bypasses the PCLK<->SCICLK synchronizer but
   * doesn't change the source). Without this SCICLK is at reset default
   * MOCO ~8 MHz and the engine produces garbled output. */
  uart_hello_route_sciclk();
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    uart_hello_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    uart_hello_panic_halt();
  }
  if (uart_hello_pins_init() != k_ra_ok) {
    uart_hello_panic_halt();
  }

  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_uart_hello_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_uart_hello_sci_channel, &sci_cfg) != k_ra_ok) {
    uart_hello_panic_halt();
  }
  if (ra_gpio_output_init(k_ra_pin_led1, k_ra_level_low) != k_ra_ok) {
    uart_hello_panic_halt();
  }
}

int32_t main(void)
{
  uart_hello_setup_or_halt();

  ra_isr_globals_enable();

  /* Continuous TX -- no per-greeting sleep -- so a SWD halt has a much
   * higher chance of catching mid-shift, and any working CDC bridge
   * sees a steady stream instead of one greeting per second. */
  while (1) {
    if (ra_sci_write_polling((uint8_t)k_uart_hello_sci_channel,
                             k_uart_hello_greeting,
                             (uint32_t)(sizeof(k_uart_hello_greeting) - 1U)) != k_ra_ok) {
      break;
    }
    if (ra_gpio_toggle(k_ra_pin_led1) != k_ra_ok) {
      break;
    }
    /* No per-cycle sleep -- continuous TX. The chip's CDC bridge
     * appears to drop bytes on slow paced traffic; sustained flow
     * makes the host see clean output. The LED toggles within the
     * write loop so it still pulses (very fast, but visible). */
    /* ra_delay_ms(k_uart_hello_period_ms); */
  }

  uart_hello_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
