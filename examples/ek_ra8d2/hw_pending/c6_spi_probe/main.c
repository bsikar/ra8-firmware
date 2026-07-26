/**
 * @file examples/ek_ra8d2/hw_pending/c6_spi_probe/main.c
 * @brief First raw SPI link probe against the ESP32-C6 wireless co-processor
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Stand-alone EK-RA8D2 application that opens the physical esp-hosted SPI
 * full-duplex link to the ESP32-C6 soldered to Pmod1 (J26) and proves it
 * end-to-end: it clocks the fixed-size transport buffer, hand-decodes the
 * esp-hosted payload header out of the received bytes, and prints a
 * PASS/FAIL verdict on the board console.
 *
 * The app also resolves the two facts that are **not** recorded anywhere in
 * the tree:
 *
 *   1. Which MCU pin the board's Pmod1 mode mux has actually wired to the
 *      C6's chip-select. EK-RA8D2 UM Table 18 p 26 makes J26-1 either P804
 *      (SPI position) or P800 (UART position), and UM Table 3 p 16 adds
 *      SW4-3, which frees those pins from the Octo-SPI flash only when it
 *      is ON. The chip-select hunt asserts each candidate and watches for
 *      the C6 to answer, so the log states the mux position rather than
 *      assuming it.
 *   2. Which Pmod1 side-band pin carries DATA_READY and which carries
 *      HANDSHAKE. The C6 is built with
 *      ``CONFIG_ESP_SPI_DEASSERT_HS_ON_CS=y``, so it drops HANDSHAKE on the
 *      chip-select falling edge and re-raises it once the next transaction
 *      is queued, while DATA_READY stays high only while its transmit queue
 *      holds a frame. A pin that tracks the chip-select is HANDSHAKE; a pin
 *      that goes low once the queue drains is DATA_READY; a pin that never
 *      moves is not wired to the C6.
 *
 * Flow:
 *   1. ``ra8_cgc_init`` -- bring CPUCLK0 / PCLKA up, start SysTick.
 *   2. Board UART console on SCI8 at 115200 8N1.
 *   3. Pmod1 side-band pins as plain GPIO inputs; report their levels.
 *   4. Muxed-net wire test, then the chip-select hunt.
 *   5. PFS-route Pmod1 SCI2 Simple-SPI and own the chip-select as a GPIO.
 *   6. Sweep SPI modes starting at the C6's configured mode 3, running a
 *      few full-size transactions per mode until one decodes.
 *   7. Print the resolved side-band map and the PASS/FAIL verdict.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "c6_probe.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_spi.h"
#include "ra8_time.h"

/**
 * @var s_c6_stats
 * @brief Accumulated probe evidence for the final verdict and pin map.
 * @details Deliberately not reset between SPI modes: the side-band map is
 * built from the whole run, including the chip-select hunt that precedes
 * any clocking.
 * @note Single-threaded; touched only from the probe flow.
 * @warning Do not clear mid-run or the map loses its evidence.
 * @since 0.1.0
 */
static c6_probe_stats_t s_c6_stats;

/**
 * @var s_c6_pclka_hz
 * @brief Cached PCLKA rate, the SCI baud-clock source.
 * @details Read once during bring-up and handed to every SPI mode opened.
 * @note Written once during setup.
 * @warning Zero until ::c6_probe_clocks_or_halt has run.
 * @since 0.1.0
 */
static uint32_t s_c6_pclka_hz;

/**
 * @var k_c6_mode_order
 * @brief SPI modes tried, most likely first.
 * @details The C6 image in ``coprocessor/esp32c6/`` is built with
 * ``CONFIG_ESP_SPI_MODE=3``, so mode 3 leads; the rest are a bring-up
 * fallback reached only if mode 3 decodes nothing.
 * @note Read-only.
 * @warning Reordering this changes which transaction drains the C6's
 *          queued boot event, which only a power cycle restores.
 * @since 0.1.0
 */
static const ra8_spi_mode_t k_c6_mode_order[k_c6_probe_mode_count] = {
  k_ra8_spi_mode_3,
  k_ra8_spi_mode_0,
  k_ra8_spi_mode_1,
  k_ra8_spi_mode_2,
};

/**
 * @brief Park forever after a fatal initialisation failure.
 *
 * @pre Called only once a bring-up step has failed unrecoverably.
 * @post The CPU is parked; only a debugger or a reset wakes it.
 *
 * @note Never returns; not callable from interrupt context.
 * @since 0.1.0
 */
static void c6_probe_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC, MSTP and SysTick up, caching PCLKA.
 *
 * @pre Reset_Handler has copied ``.data`` and zeroed ``.bss``.
 * @pre Interrupts are masked or this is a single-threaded init context.
 * @post ::s_c6_pclka_hz is non-zero and SysTick is running.
 * @post The function panic-halts instead of returning on any failure.
 *
 * @note Never returns on error.
 * @since 0.1.0
 */
static void c6_probe_clocks_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    c6_probe_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    c6_probe_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &s_c6_pclka_hz) != k_ra8_ok) {
    c6_probe_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    c6_probe_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    c6_probe_panic_halt();
  }
}

/**
 * @brief Bring clocks, console, LEDs and side-band inputs up.
 *
 * @details
 * Deliberately stops short of routing the Pmod1 SPI pins: the wire test and
 * the chip-select hunt run next and need those nets unclaimed so they can
 * drive and release them.
 *
 * @pre Reset_Handler has initialised ``.data`` and ``.bss``.
 * @pre The console SCI is not otherwise claimed.
 * @post The console prints and the side-band pins are inputs.
 * @post The function panic-halts instead of returning on any failure.
 *
 * @note Never returns on error.
 * @since 0.1.0
 */
static void c6_probe_setup_or_halt(void)
{
  c6_probe_clocks_or_halt();
  if (ra8_board_uart_console_init((uint32_t)k_c6_probe_uart_baud) != k_ra8_ok) {
    c6_probe_panic_halt();
  }
  if (c6_probe_sideband_init() != k_ra8_ok) {
    c6_probe_puts("c6_probe: side-band pin init failed\r\n");
    c6_probe_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    c6_probe_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    c6_probe_panic_halt();
  }
}

/**
 * @brief Print the resolved DATA_READY / HANDSHAKE side-band map.
 *
 * @param[in] st     Accumulated evidence.
 * @param[in] hs_idx Resolved handshake index, or ::k_c6_sb_count.
 * @param[in] dr_idx Resolved data-ready index, or ::k_c6_sb_count.
 *
 * @pre The console is up and ``st`` is non-NULL.
 * @pre Both indices are valid or ::k_c6_sb_count.
 * @post One evidence line per pin plus one map line were printed.
 * @post Nothing is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void c6_probe_print_map(const c6_probe_stats_t* st, uint8_t hs_idx, uint8_t dr_idx)
{
  if (st == nullptr) {
    return;
  }
  for (uint8_t i = 0U; i < (uint8_t)k_c6_sb_count; i++) {
    c6_probe_puts("c6_probe: evidence ");
    c6_probe_puts(c6_probe_sideband_name(i));
    c6_probe_puts(" hs_vote=");
    c6_probe_put_u32(st->hs_vote[i]);
    c6_probe_puts(" dr_vote=");
    c6_probe_put_u32(st->dr_vote[i]);
    c6_probe_puts(" high=");
    c6_probe_put_u32((uint32_t)st->ever_high[i]);
    c6_probe_puts(" low=");
    c6_probe_put_u32((uint32_t)st->ever_low[i]);
    c6_probe_puts("\r\n");
  }
  c6_probe_puts("c6_probe: map HANDSHAKE=");
  c6_probe_puts(c6_probe_sideband_name(hs_idx));
  c6_probe_puts(" DATA_READY=");
  c6_probe_puts(c6_probe_sideband_name(dr_idx));
  c6_probe_puts("\r\n");
}

/**
 * @brief Print the single-line PASS / FAIL verdict.
 *
 * @param[in] st        Accumulated evidence.
 * @param[in] mode      Mode that decoded, or the last mode tried.
 * @param[in] link_live Whether any recognisable frame was decoded.
 *
 * @pre The console is up and ``st`` is non-NULL.
 * @pre ``mode`` is one of ::ra8_spi_mode_t.
 * @post Exactly one verdict line beginning ``c6_probe: PASS`` or
 *       ``c6_probe: FAIL`` was printed.
 * @post Nothing is modified.
 *
 * @note The verdict line is what a HIL scrape would match on.
 * @since 0.1.0
 */
static void c6_probe_print_verdict(const c6_probe_stats_t* st, ra8_spi_mode_t mode, bool link_live)
{
  if (st == nullptr) {
    return;
  }
  c6_probe_puts(link_live ? "c6_probe: PASS esp-hosted link up mode="
                          : "c6_probe: FAIL no esp-hosted frame mode=");
  c6_probe_put_u32((uint32_t)mode);
  c6_probe_puts(" sck_hz=");
  c6_probe_put_u32((uint32_t)k_c6_probe_sck_hz);
  c6_probe_puts(" xfers=");
  c6_probe_put_u32(st->attempts);
  c6_probe_puts(" data=");
  c6_probe_put_u32(st->data_frames);
  c6_probe_puts(" idle=");
  c6_probe_put_u32(st->idle_frames);
  c6_probe_puts(" badcsum=");
  c6_probe_put_u32(st->bad_csum_frames);
  c6_probe_puts("\r\n");
}

/**
 * @brief Sweep the SPI modes and report the outcome.
 *
 * @pre Setup completed, the console is up and the SPI pins are routed.
 * @pre ::s_c6_stats already holds the chip-select hunt's evidence.
 * @post A map line and a verdict line were printed exactly once.
 * @post LED2 is lit when the link failed to come up.
 *
 * @note Not thread-safe; runs once from ``main``.
 * @since 0.1.0
 */
static void c6_probe_run(void)
{
  uint8_t        hs_idx    = c6_probe_best(s_c6_stats.hs_vote, (uint8_t)k_c6_sb_count);
  bool           link_live = false;
  ra8_spi_mode_t used      = k_c6_mode_order[0];

  for (uint8_t m = 0U; m < (uint8_t)k_c6_probe_mode_count && !link_live; m++) {
    used      = k_c6_mode_order[m];
    link_live = c6_probe_sweep_mode(used, s_c6_pclka_hz, &s_c6_stats, &hs_idx);
  }

  hs_idx               = c6_probe_best(s_c6_stats.hs_vote, (uint8_t)k_c6_sb_count);
  const uint8_t dr_idx = c6_probe_best(s_c6_stats.dr_vote, hs_idx);
  c6_probe_print_map(&s_c6_stats, hs_idx, dr_idx);
  c6_probe_print_verdict(&s_c6_stats, used, link_live);
  if (!link_live) {
    (void)ra8_board_led_on(k_ra8_board_led2);
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry: probe the ESP32-C6 esp-hosted SPI link.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied ``.data`` and zeroed ``.bss``.
 * @pre SystemInit has set VTOR, the FPU and priority grouping.
 * @post The probe ran exactly once and printed its verdict.
 * @post The CPU stays in a slow heartbeat loop afterwards.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  c6_probe_setup_or_halt();
  ra8_isr_globals_enable();

  c6_probe_puts("\r\nc6_probe: EK-RA8D2 <-> ESP32-C6 esp-hosted SPI probe\r\n");
  c6_probe_puts("c6_probe: pmod1 sci2 simple-spi controller, cs on P804\r\n");
  ra8_delay_ms((uint32_t)k_c6_probe_boot_wait_ms);

  c6_sideband_sample_t idle = {};
  c6_probe_sample_sideband(&idle);
  c6_probe_print_sideband("c6_probe: idle sideband", &idle);

  c6_probe_wire_test();
  (void)c6_probe_cs_hunt(&s_c6_stats);

  if (c6_probe_spi_pins_init() != k_ra8_ok) {
    c6_probe_puts("c6_probe: spi pin routing failed\r\n");
    c6_probe_panic_halt();
  }
  c6_probe_run();

  while (1) {
    (void)ra8_board_led_toggle(k_ra8_board_led1);
    ra8_delay_ms((uint32_t)k_c6_probe_idle_ms);
  }

  c6_probe_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
