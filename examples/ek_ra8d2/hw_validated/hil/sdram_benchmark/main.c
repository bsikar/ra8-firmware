/**
 * @file examples/ek_ra8d2/hw_validated/hil/sdram_benchmark/main.c
 * @brief External SDRAM bring-up + 64 KB write/read benchmark on EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Standalone EVM-tier app that exercises the external SDRAM
 * controller driver (``libs/ra8_hal/ra8_sdramc.h``). The EK-RA8D2
 * carries a 64 MB ISSI IS42S32160F SDRAM (16M x 32) wired to the
 * BUS pins with the controller-mapped window starting at
 * ``k_ra8_sdram_base_addr`` (0x68000000). The flow:
 *
 *   1. ``ra8_cgc_init`` -- clocks up.
 *   2. ``ra8_mstp_init`` + console pins (PD02 / PD03 SCI8).
 *   3. ``ra8_sdramc_init`` -- runs the documented SDRAM
 *      bring-up sequence (PALL -> MRS -> AREF enable).
 *   4. Write a 64 KB block of incrementing 32-bit words into the
 *      SDRAM window starting at ``k_ra8_sdram_base_addr``. Time
 *      the write and read phases via ``ra8_time_ms`` and emit
 *      ``"sdram: w=NN MB/s r=NN MB/s\r\n"`` over SCI8 once a
 *      second. On a clean read-back it also emits the success-only
 *      verdict line ``"sdram: bench OK\r\n"`` that the HIL scrape keys
 *      on (never printed on a ``"sdram: FAIL idx="`` path). LED1
 *      toggles per cycle; LED2 latches on read-back mismatch.
 *
 * Bare EK-RA8D2 only -- the SDRAM is on-board.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_sdramc.h"
#include "ra8_sdramc_regs.h"
#include "ra8_time.h"

/** @brief App-wide tunables. */
typedef enum : uint32_t {
  k_sdram_demo_baud        = 115200U, /**< SDRAM demo baud.                */
  k_sdram_demo_period_ms   = 1000U,   /**< SDRAM demo period ms.           */
  k_sdram_demo_block_bytes = 65536U,  /**< 64 KB window per pass.          */
  k_sdram_demo_block_words = 16384U,  /**< 65536 / sizeof(uint32_t).       */
  k_sdram_demo_us_per_ms   = 1000U,   /**< Conversion factor used in mbps. */
} sdram_demo_const_t;

/** @brief Single-byte ASCII conversion constants. */
typedef enum : uint8_t {
  k_sdram_demo_ascii_zero   = '0',   /**< SDRAM demo ascii zero.            */
  k_sdram_demo_ascii_a      = 'a',   /**< SDRAM demo ascii a.               */
  k_sdram_demo_dec_base     = 10U,   /**< SDRAM demo dec base.              */
  k_sdram_demo_uint_dec_max = 10U,   /**< Max digits in a uint32 base-10.   */
  k_sdram_demo_print_buf    = 64U,   /**< Max bytes in one throughput line. */
  k_sdram_demo_fail_buf     = 96U,   /**< Max bytes in the FAIL diag line.  */
  k_sdram_demo_hex_digits   = 8U,    /**< Hex digits in a uint32.           */
  k_sdram_demo_nyb_bits     = 4U,    /**< Bits per hex nibble.              */
  k_sdram_demo_nyb_mask     = 0x0FU, /**< Mask for one nibble.              */
} sdram_demo_byte_t;

/**
 * @struct sdram_demo_diag_t
 * @brief First-mismatch diagnostic captured by ::sdram_demo_read_check.
 *
 * @details Populated only when a read-back pass finds at least one
 * corrupt word, so a HIL operator can see the corruption signature
 * (aliasing, stuck bits, row-boundary) without a debugger.
 */
typedef struct {
  uint32_t count;     /**< Total mismatched words in the pass.    */
  uint32_t first_idx; /**< Word index of the first mismatch.      */
  uint32_t expected;  /**< Value that should have been read back. */
  uint32_t got;       /**< Value actually read back at first_idx. */
} sdram_demo_diag_t;

/** @brief Pattern seed -- recognisable hex word for SWD inspection. */
typedef enum : uint32_t {
  k_sdram_demo_pattern_seed = 0xC0FFEE00UL, /**< SDRAM demo pattern seed. */
} sdram_demo_seed_t;

/** @brief Verdict line emitted only when a read-back pass matched every
 *         word (never on a "sdram: FAIL idx=" path); the HIL scrape keys
 *         on this success-only string. */
static const uint8_t k_sdram_demo_pass_msg[] = "sdram: bench OK\r\n";

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

  if (ra8_cgc_init() != k_ra8_ok) {
    sdram_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    sdram_demo_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    sdram_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    sdram_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_sdram_demo_baud) != k_ra8_ok) {
    sdram_demo_panic_halt();
  }
  if (ra8_sdramc_init() != k_ra8_ok) {
    sdram_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    sdram_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
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
 * @pre ``ra8_sdramc_init`` succeeded so the SDRAM window is live.
 * @post Every word in the 64 KB block holds ``base + i``.
 * @since 0.1.0
 */
static uint32_t sdram_demo_write_pass(uint32_t base)
{
  volatile uint32_t* mem      = (volatile uint32_t*)k_ra8_sdram_base_addr;
  const uint32_t     start_ms = ra8_time_ms();
  for (uint32_t i = 0U; i < (uint32_t)k_sdram_demo_block_words; i++) {
    mem[i] = base + i;
  }
  return ra8_time_ms() - start_ms;
}

/**
 * @brief Read every word back and check it matches ``base + i``.
 *
 * @param[in]  base    Seed used by the matching write pass.
 * @param[out] elapsed Receives the elapsed milliseconds.
 * @param[out] diag    Receives the first-mismatch diagnostic.
 * @return ``true`` if every word matches the expected pattern.
 *
 * @pre ``elapsed`` and ``diag`` are non-NULL.
 * @post On success ``*elapsed`` holds the read-pass duration.
 * @post ``diag->count`` holds the total mismatch count.
 * @since 0.1.0
 */
static bool sdram_demo_read_check(uint32_t base, uint32_t* elapsed, sdram_demo_diag_t* diag)
{
  volatile uint32_t* mem      = (volatile uint32_t*)k_ra8_sdram_base_addr;
  const uint32_t     start_ms = ra8_time_ms();
  diag->count                 = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_sdram_demo_block_words; i++) {
    const uint32_t got = mem[i];
    if (got != base + i) {
      if (diag->count == 0U) {
        diag->first_idx = i;
        diag->expected  = base + i;
        diag->got       = got;
      }
      diag->count++;
    }
  }
  *elapsed = ra8_time_ms() - start_ms;
  return diag->count == 0U;
}

/**
 * @brief Render @p v as exactly 8 lowercase hex digits into @p buf.
 *
 * @param[out] buf Destination (must hold >= k_sdram_demo_hex_digits).
 * @param[in]  v   Value to render.
 * @return Number of digits written (always k_sdram_demo_hex_digits).
 *
 * @pre ``buf`` is non-NULL with >= 8 bytes capacity.
 * @post No null terminator is written.
 * @since 0.1.0
 */
static uint8_t sdram_demo_uint_to_hex(uint8_t* buf, uint32_t v)
{
  for (uint8_t i = 0U; i < (uint8_t)k_sdram_demo_hex_digits; i++) {
    const uint8_t shift =
      (uint8_t)(((uint8_t)k_sdram_demo_hex_digits - 1U - i) * (uint8_t)k_sdram_demo_nyb_bits);
    const uint8_t nyb = (uint8_t)((v >> shift) & (uint32_t)k_sdram_demo_nyb_mask);
    buf[i]            = (nyb < (uint8_t)k_sdram_demo_dec_base)
                          ? (uint8_t)((uint8_t)k_sdram_demo_ascii_zero + nyb)
                          : (uint8_t)((uint8_t)k_sdram_demo_ascii_a +
                           (uint8_t)(nyb - (uint8_t)k_sdram_demo_dec_base));
  }
  return (uint8_t)k_sdram_demo_hex_digits;
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

/**
 * @brief Format the read-back FAIL diagnostic line into @p out.
 *
 * @details Emits ``sdram: FAIL idx=<dec> exp=<hex> got=<hex> n=<dec>``
 * so a HIL log shows the corruption signature. The literal ``sdram:
 * FAIL`` prefix is what the HIL negative-match regex keys on.
 *
 * @param[out] out  Destination buffer (>= k_sdram_demo_fail_buf bytes).
 * @param[in]  diag First-mismatch diagnostic from sdram_demo_read_check.
 * @return Number of bytes written (no NUL terminator).
 *
 * @pre ``out`` is non-NULL with >= k_sdram_demo_fail_buf capacity.
 * @pre ``diag->count`` is non-zero (a mismatch was found).
 * @post No bytes beyond the returned length are touched.
 * @since 0.1.0
 */
static uint32_t sdram_demo_format_fail(uint8_t* out, const sdram_demo_diag_t* diag)
{
  uint32_t      off       = 0U;
  const uint8_t prefix[]  = "sdram: FAIL idx=";
  const uint8_t exp_tag[] = " exp=";
  const uint8_t got_tag[] = " got=";
  const uint8_t n_tag[]   = " n=";
  const uint8_t suffix[]  = "\r\n";
  for (uint32_t i = 0U; i < sizeof(prefix) - 1U; i++) {
    out[off++] = prefix[i];
  }
  off += sdram_demo_uint_to_dec(&out[off], diag->first_idx);
  for (uint32_t i = 0U; i < sizeof(exp_tag) - 1U; i++) {
    out[off++] = exp_tag[i];
  }
  off += sdram_demo_uint_to_hex(&out[off], diag->expected);
  for (uint32_t i = 0U; i < sizeof(got_tag) - 1U; i++) {
    out[off++] = got_tag[i];
  }
  off += sdram_demo_uint_to_hex(&out[off], diag->got);
  for (uint32_t i = 0U; i < sizeof(n_tag) - 1U; i++) {
    out[off++] = n_tag[i];
  }
  off += sdram_demo_uint_to_dec(&out[off], diag->count);
  for (uint32_t i = 0U; i < sizeof(suffix) - 1U; i++) {
    out[off++] = suffix[i];
  }
  return off;
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
int32_t main(void)
{
  sdram_demo_setup_or_halt();
  ra8_isr_globals_enable();

  uint32_t base = (uint32_t)k_sdram_demo_pattern_seed;
  while (1) {
    const uint32_t    w_ms   = sdram_demo_write_pass(base);
    uint32_t          r_ms   = 0U;
    sdram_demo_diag_t diag   = {};
    const bool        match  = sdram_demo_read_check(base, &r_ms, &diag);
    const uint32_t    w_mbps = sdram_demo_mbps((uint32_t)k_sdram_demo_block_bytes, w_ms);
    const uint32_t    r_mbps = sdram_demo_mbps((uint32_t)k_sdram_demo_block_bytes, r_ms);

    uint8_t        out[k_sdram_demo_print_buf] = {};
    const uint32_t off                         = sdram_demo_format_line(out, w_mbps, r_mbps);
    if (!match) {
      (void)ra8_board_led_on(k_ra8_board_led2);
      /* Emit a FAIL diagnostic so the HIL negative regex catches a
       * read-back mismatch and a log reader sees the corruption
       * signature (first index, expected vs got, total count). */
      uint8_t        fail[k_sdram_demo_fail_buf] = {};
      const uint32_t fail_off                    = sdram_demo_format_fail(fail, &diag);
      (void)ra8_board_uart_console_write(fail, (size_t)fail_off);
    }
    if (ra8_board_uart_console_write(out, (size_t)off) != k_ra8_ok) {
      break;
    }
    /* Success-only verdict for the HIL scrape: a clean read-back pass. */
    if (match) {
      (void)ra8_board_uart_console_write(k_sdram_demo_pass_msg,
                                         (size_t)(sizeof(k_sdram_demo_pass_msg) - 1U));
    }
    if (ra8_board_led_toggle(k_ra8_board_led1) != k_ra8_ok) {
      break;
    }
    base += 1U;
    ra8_delay_ms(k_sdram_demo_period_ms);
  }

  sdram_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
