/**
 * @file main.c
 * @brief External SDRAM bring-up + 64 KB write/read benchmark on EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Standalone EVM-tier app that exercises the external SDRAM
 * controller driver (``libs/ra_hal/ra_sdramc.h``). The EK-RA8D2
 * carries a 64 MB Winbond W9825G6KH SDRAM wired to the BUS pins
 * with the controller-mapped window starting at
 * ``k_ra_sdram_base_addr`` (0x68000000). The flow:
 *
 *   1. ``ra_cgc_init`` -- clocks up.
 *   2. ``ra_mstp_init`` + console pins (PD02 / PD03 SCI8).
 *   3. ``ra_sdramc_init`` -- runs the documented SDRAM
 *      bring-up sequence (PALL -> MRS -> AREF enable).
 *   4. Write a 64 KB block of incrementing 32-bit words into the
 *      SDRAM window starting at ``k_ra_sdram_base_addr``. Time
 *      the write and read phases via ``ra_time_ms`` and emit
 *      ``"sdram: w=NN MB/s r=NN MB/s\r\n"`` over SCI8 once a
 *      second. LED1 toggles per cycle; LED2 latches on read-back
 *      mismatch.
 *
 * Bare EK-RA8D2 only -- the SDRAM is on-board.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_sdramc_regs.h"
#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_mstp.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_sdramc.h"
#include "ra_time.h"

/** @brief App-wide tunables. */
typedef enum : uint32_t {
  k_sdram_demo_baud        = 115200U,
  k_sdram_demo_period_ms   = 1000U,
  k_sdram_demo_block_bytes = 65536U, /**< 64 KB window per pass. */
  k_sdram_demo_block_words = 16384U, /**< 65536 / sizeof(uint32_t). */
  k_sdram_demo_sci_channel = 8U,
  k_sdram_demo_us_per_ms   = 1000U, /**< Conversion factor used in mbps. */
} sdram_demo_const_t;

/** @brief Single-byte ASCII conversion constants. */
typedef enum : uint8_t {
  k_sdram_demo_ascii_zero   = '0',
  k_sdram_demo_dec_base     = 10U,
  k_sdram_demo_uint_dec_max = 10U, /**< Max digits in a uint32 base-10. */
  k_sdram_demo_print_buf    = 64U, /**< Max bytes in one print line.    */
} sdram_demo_byte_t;

/** @brief Pattern seed -- recognisable hex word for SWD inspection. */
typedef enum : uint32_t {
  k_sdram_demo_pattern_seed = 0xC0FFEE00UL,
} sdram_demo_seed_t;

/** @brief Pinout for SCI8 console. */
static const ra_port_pin_t k_sdram_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_sdram_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief Park forever after a fatal init failure.
 *
 * @pre Called only after a fatal error in boot.
 * @post CPU is parked; only a debugger or external reset wakes it.
 * @since 0.1.0
 */
static void sdram_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC + SysTick + console + SDRAM up. Panic-halts on fail.
 *
 * @since 0.1.0
 */
static void sdram_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    sdram_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    sdram_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    sdram_demo_panic_halt();
  }
  if (ra_mstp_init() != k_ra_ok) {
    sdram_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    sdram_demo_panic_halt();
  }

  if (ra_pfs_route_peripheral(k_sdram_demo_pin_txd, k_ra_psel_sci_async, "sdram_benchmark.txd8") !=
      k_ra_ok) {
    sdram_demo_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_sdram_demo_pin_rxd, k_ra_psel_sci_async, "sdram_benchmark.rxd8") !=
      k_ra_ok) {
    sdram_demo_panic_halt();
  }

  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_sdram_demo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_sdram_demo_sci_channel, &sci_cfg) != k_ra_ok) {
    sdram_demo_panic_halt();
  }
  if (ra_sdramc_init() != k_ra_ok) {
    sdram_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    sdram_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    sdram_demo_panic_halt();
  }
}

/**
 * @brief Compute throughput in MB/s given a byte count and millisecond span.
 *
 * @details
 * Returns ``(bytes / 1e6) / (ms / 1000) = bytes / (ms * 1000)``. The
 * function clamps ``ms`` to 1 to avoid divide-by-zero when the
 * benchmark loop is too fast to register on the millisecond tick.
 *
 * @param[in] bytes Total bytes transferred.
 * @param[in] ms    Elapsed time in milliseconds.
 * @return MB/s as an integer (truncated).
 *
 * @pre None.
 * @post Return value never overflows uint32 for ``bytes <= 4 GB``
 *       and ``ms >= 1``.
 * @since 0.1.0
 */
static uint32_t sdram_demo_mbps(uint32_t bytes, uint32_t ms)
{
  const uint32_t ms_clamped = (ms == 0U) ? 1U : ms;
  return bytes / (ms_clamped * (uint32_t)k_sdram_demo_us_per_ms);
}

/**
 * @brief Write ``base + i`` into every word of the SDRAM block.
 *
 * @param[in] base Seed value mixed into the pattern.
 * @return Elapsed milliseconds.
 *
 * @pre ``ra_sdramc_init`` succeeded so the SDRAM window is live.
 * @post Every word in the 64 KB block holds ``base + i``.
 * @since 0.1.0
 */
static uint32_t sdram_demo_write_pass(uint32_t base)
{
  volatile uint32_t* mem      = (volatile uint32_t*)k_ra_sdram_base_addr;
  const uint32_t     start_ms = ra_time_ms();
  for (uint32_t i = 0U; i < (uint32_t)k_sdram_demo_block_words; i++) {
    mem[i] = base + i;
  }
  return ra_time_ms() - start_ms;
}

/**
 * @brief Read every word back and check it matches ``base + i``.
 *
 * @param[in]  base    Seed used by the matching write pass.
 * @param[out] elapsed Receives the elapsed milliseconds.
 * @return ``true`` if every word matches the expected pattern.
 *
 * @pre ``elapsed`` is non-NULL.
 * @post On success ``*elapsed`` holds the read-pass duration.
 * @since 0.1.0
 */
static bool sdram_demo_read_check(uint32_t base, uint32_t* elapsed)
{
  volatile uint32_t* mem      = (volatile uint32_t*)k_ra_sdram_base_addr;
  const uint32_t     start_ms = ra_time_ms();
  bool               ok       = true;
  for (uint32_t i = 0U; i < (uint32_t)k_sdram_demo_block_words; i++) {
    if (mem[i] != base + i) {
      ok = false;
    }
  }
  *elapsed = ra_time_ms() - start_ms;
  return ok;
}

/**
 * @brief Write a decimal uint32 into ``buf``; returns bytes written.
 *
 * @param[out] buf Destination buffer (must hold at least 10 chars).
 * @param[in]  v   Value to render.
 * @return Number of ASCII digits written.
 *
 * @pre ``buf`` is non-NULL with >= 10 bytes capacity.
 * @post No null terminator is written.
 * @since 0.1.0
 */
static uint8_t sdram_demo_uint_to_dec(uint8_t* buf, uint32_t v)
{
  uint8_t  tmp[k_sdram_demo_uint_dec_max];
  uint8_t  n = 0U;
  uint32_t r = v;
  if (r == 0U) {
    buf[0] = (uint8_t)k_sdram_demo_ascii_zero;
    return 1U;
  }
  while ((r > 0U) && (n < (uint8_t)k_sdram_demo_uint_dec_max)) {
    tmp[n] =
      (uint8_t)((uint8_t)k_sdram_demo_ascii_zero + (uint8_t)(r % (uint32_t)k_sdram_demo_dec_base));
    r = r / (uint32_t)k_sdram_demo_dec_base;
    n++;
  }
  for (uint8_t i = 0U; i < n; i++) {
    buf[i] = tmp[(uint8_t)(n - 1U - i)];
  }
  return n;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Bench-tests SDRAM and prints MB/s.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On clean entry the CPU stays in the bench + blink loop.
 * @post On read-back mismatch LED2 latches ON.
 *
 * @since 0.1.0
 */
/**
 * @brief Format the per-cycle MB/s line into @p out, return its length.
 *
 * @param[out] out    Destination buffer (>= k_sdram_demo_print_buf bytes).
 * @param[in]  w_mbps Write throughput in MB/s.
 * @param[in]  r_mbps Read throughput in MB/s.
 * @return Number of bytes written (no NUL terminator).
 *
 * @pre ``out`` is non-NULL with >= k_sdram_demo_print_buf capacity.
 * @post No bytes beyond the returned length are touched.
 *
 * @since 0.1.0
 */
static uint32_t sdram_demo_format_line(uint8_t* out, uint32_t w_mbps, uint32_t r_mbps)
{
  uint32_t      off      = 0U;
  const uint8_t prefix[] = "sdram: w=";
  const uint8_t mid[]    = " MB/s r=";
  const uint8_t suffix[] = " MB/s\r\n";
  for (uint32_t i = 0U; i < sizeof(prefix) - 1U; i++) {
    out[off++] = prefix[i];
  }
  off += sdram_demo_uint_to_dec(&out[off], w_mbps);
  for (uint32_t i = 0U; i < sizeof(mid) - 1U; i++) {
    out[off++] = mid[i];
  }
  off += sdram_demo_uint_to_dec(&out[off], r_mbps);
  for (uint32_t i = 0U; i < sizeof(suffix) - 1U; i++) {
    out[off++] = suffix[i];
  }
  return off;
}

int32_t main(void)
{
  sdram_demo_setup_or_halt();
  ra_isr_globals_enable();

  uint32_t base = (uint32_t)k_sdram_demo_pattern_seed;
  while (1) {
    const uint32_t w_ms   = sdram_demo_write_pass(base);
    uint32_t       r_ms   = 0U;
    const bool     match  = sdram_demo_read_check(base, &r_ms);
    const uint32_t w_mbps = sdram_demo_mbps((uint32_t)k_sdram_demo_block_bytes, w_ms);
    const uint32_t r_mbps = sdram_demo_mbps((uint32_t)k_sdram_demo_block_bytes, r_ms);

    uint8_t        out[k_sdram_demo_print_buf] = {};
    const uint32_t off                         = sdram_demo_format_line(out, w_mbps, r_mbps);
    if (!match) {
      (void)ra_board_led_on(k_ra_board_led2);
    }
    if (ra_sci_write_polling((uint8_t)k_sdram_demo_sci_channel, out, off) != k_ra_ok) {
      break;
    }
    if (ra_board_led_toggle(k_ra_board_led1) != k_ra_ok) {
      break;
    }
    base += 1U;
    ra_delay_ms(k_sdram_demo_period_ms);
  }

  sdram_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
