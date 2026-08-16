/**
 * @file examples/ek_ra8d2/hw_pending/eth_gptp_timestamp_demo/main.c
 * @brief GPTP hardware-timer demo -- proves the HUM Ch 35 counter really runs
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Exercises the only PTP-related hardware the RA8D2 actually has: the
 * Ethernet Generic PTP Timer of HUM Ch 35 p 1925-1964. That block is a
 * **timer** -- it has no Sync / Announce generator, no domain, no
 * clockIdentity and no BMCA -- so this app makes no claim to be an IEEE 1588
 * or 802.1AS node. It brings the counter up and then measures it.
 *
 * Bring-up (CGC -> ESWCLK -> MSTP -> TIME -> peripheral):
 *   1. CGC + SysTick + UART console (SCI8 on PD_02 / PD_03 -> J-Link OB).
 *   2. ``ra8_cgc_eswclk_init`` powers the ESWM domain and starts ESWCLK,
 *      which is the GPTP ``clk`` input.
 *   3. ``ra8_eth_gptp_init`` derives ``PTPTIVCt`` from the live ESWCLK
 *      frequency, then the app loads a known epoch into the 78-bit offset
 *      and enables timer 0.
 *
 * Per cycle the app:
 *   - reads the read-only ``PTPIPV`` IP-version word and requires it to be
 *     nonzero. That is the presence probe: a reserved aperture reads back
 *     zeroes or whatever was last written, while ``PTPIPV`` has a nonzero
 *     reset value (HUM 35.3.1.1 p 1927);
 *   - samples the 78-bit GPTP time, waits ``k_gptp_window_ms`` measured on
 *     SysTick, samples it again, and requires the counter to have advanced
 *     by that interval to within ``k_gptp_tolerance_pct``.
 *
 * The verdict ``"gptp: clock PASS"`` therefore means the counter advanced one
 * second per second against an independent time base -- not merely that a
 * driver call returned ``k_ra8_ok``.
 *
 * hw_pending: this stays under hw_pending because the full IEEE-1588 / 802.1AS
 * claim (a bounded sync offset against the bench's gPTP peer) can only be made
 * on the bench. Its counter-advance verdict, however, is now gated in the
 * emulator: ``tools/ra8_emulator`` models the HUM Ch 35 GPTP timer
 * (``board_periph_gptp.c``), whose free-running counter really advances one
 * second per second against the emulator's SysTick, so ``gptp: clock PASS`` is
 * asserted on every CI run through the ``emulator-smoke`` gate as well as on the
 * bench.
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
#include "ra8_eth_gptp.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_gptp_baud      = 115200U,    /**< SCI8 console baud.                    */
  k_gptp_period_ms = 1000U,      /**< Idle time between measurement cycles. */
  k_gptp_window_ms = 500U,       /**< Measurement window, timed on SysTick. */
  k_gptp_base_sec  = 1000000000U /**< Epoch seconds loaded into the offset. */
} gptp_const_t;

/**
 * @brief Largest seconds value that can be flattened to nanoseconds.
 *
 * @details
 * ``UINT64_MAX / 1e9``: above this, ``sec * 1e9`` wraps.
 */
typedef enum : uint64_t {
  k_gptp_sec_flatten_max = 18446744073ULL, /**< floor(2^64 - 1 / 1e9). */
} gptp_flatten_limit_t;

/** @brief Formatting and arithmetic constants. */
typedef enum : uint32_t {
  k_gptp_radix       = 10U,        /**< Decimal serialiser radix.          */
  k_gptp_dec_u64_max = 20U,        /**< Max decimal digits for a uint64_t. */
  k_gptp_ns_per_ms   = 1000000U,   /**< Nanoseconds in one millisecond.    */
  k_gptp_ns_per_sec  = 1000000000U /**< Nanoseconds in one second.         */
} gptp_fmt_t;

/** @brief Accuracy band the measured advance must fall inside. */
typedef enum : uint8_t {
  k_gptp_tolerance_pct = 10U,  /**< Allowed drift vs SysTick, in percent. */
  k_gptp_pct_full      = 100U, /**< Denominator for the percentage above. */
} gptp_band_t;

/* Console line fragments (kept short so each write is one shift-register
 * fill; the periodic log is the only output path). */
static const uint8_t k_gptp_ipv_prefix[]   = "gptp: ipv=";
static const uint8_t k_gptp_time_prefix[]  = "gptp: t=";
static const uint8_t k_gptp_time_dot[]     = ".";
static const uint8_t k_gptp_adv_prefix[]   = "gptp: adv_ns=";
static const uint8_t k_gptp_sys_prefix[]   = " sys_ms=";
static const uint8_t k_gptp_crlf[]         = "\r\n";
static const uint8_t k_gptp_verdict_pass[] = "gptp: clock PASS\r\n";
static const uint8_t k_gptp_verdict_fail[] = "gptp: clock FAIL\r\n";

/**
 * @brief Park forever after a fatal init error.
 *
 * @details
 * ``wfi`` in a bare loop rather than a spin so the part idles cold while a
 * bench operator attaches a debugger.
 *
 * @pre Called only after an unrecoverable bring-up failure.
 * @pre The caller has already reported the cause it can report.
 * @post CPU is parked; only a debugger or reset wakes it.
 * @post No further console output is produced.
 *
 * @note Never returns; safe from any context.
 * @since 0.1.0
 */
static void gptp_panic_halt(void)
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
 * @details
 * The console status is discarded deliberately: this app's verdict is about
 * the GPTP counter, and a UART hiccup must not be reported as a clock fault.
 *
 * @pre ``ra8_board_uart_console_init`` has succeeded.
 * @pre ``data`` points at ``len`` readable bytes.
 * @post ``len`` bytes have been queued to the console UART.
 * @post No driver state was mutated.
 *
 * @note Not thread-safe; single-threaded demo loop.
 * @since 0.1.0
 */
static void gptp_write(const uint8_t* data, uint32_t len)
{
  (void)ra8_board_uart_console_write(data, (size_t)len);
}

/**
 * @brief Log one unsigned 64-bit value as decimal ASCII.
 *
 * @param[in] val Value to print.
 *
 * @details
 * Digits are generated least-significant first into a scratch buffer and
 * reversed, so no division-by-power-of-ten table is needed. Both buffers are
 * ``k_gptp_dec_u64_max`` bytes, the exact width of ``UINT64_MAX`` at
 * ``k_gptp_radix``, which is what bounds the generate loop.
 *
 * @pre The console has been initialised.
 * @pre ``val`` fits in a uint64_t (always true).
 * @post The decimal digits of ``val`` have been queued to the console.
 * @post Neither scratch buffer outlives the call.
 *
 * @note Not thread-safe; single-threaded demo loop.
 * @since 0.1.0
 */
static void gptp_write_u64(uint64_t val)
{
  uint8_t  buf[k_gptp_dec_u64_max];
  uint8_t  tmp[k_gptp_dec_u64_max];
  uint32_t n = 0U;
  uint64_t v = val;
  if (v == 0U) {
    buf[0] = (uint8_t)'0';
    gptp_write(buf, 1U);
    return;
  }
  while (v != 0U) {
    tmp[n] = (uint8_t)('0' + (uint8_t)(v % (uint64_t)k_gptp_radix));
    v      = v / (uint64_t)k_gptp_radix;
    n++;
  }
  for (uint32_t i = 0U; i < n; i++) {
    buf[i] = tmp[n - 1U - i];
  }
  gptp_write(buf, n);
}

/**
 * @brief Log one ``gptp: t=`` line -- seconds, a dot, then nanoseconds.
 *
 * @details
 * Prints the two fields verbatim rather than converting to a single
 * nanosecond count, so a bench operator sees exactly what the registers held.
 *
 * @param[in] sec  Seconds field of a GPTP sample.
 * @param[in] nsec Nanoseconds field of the same sample.
 *
 * @pre The console has been initialised.
 * @pre ``nsec`` is below one second.
 * @post One timestamp line has been queued.
 * @post No driver state was mutated.
 *
 * @note Not thread-safe; single-threaded demo loop.
 * @since 0.1.0
 */
static void gptp_log_time(uint64_t sec, uint32_t nsec)
{
  gptp_write(k_gptp_time_prefix, (uint32_t)(sizeof(k_gptp_time_prefix) - 1U));
  gptp_write_u64(sec);
  gptp_write(k_gptp_time_dot, (uint32_t)(sizeof(k_gptp_time_dot) - 1U));
  gptp_write_u64((uint64_t)nsec);
  gptp_write(k_gptp_crlf, (uint32_t)(sizeof(k_gptp_crlf) - 1U));
}

/**
 * @brief Flatten a GPTP sample to nanoseconds.
 *
 * @details
 * The seconds field is 48 bits wide, so ``sec * 1e9`` would wrap a uint64_t
 * above ``sec ~= 1.8e10``. That is far beyond any epoch this demo loads, but
 * it is a value read straight off a hardware register, so an implausible
 * reading is clamped to ``UINT64_MAX`` rather than silently wrapped into a
 * small number that could look like a healthy advance.
 *
 * @param[in] sec  Seconds field.
 * @param[in] nsec Nanoseconds field.
 *
 * @return The sample as a nanosecond count, or ``UINT64_MAX`` if ``sec`` is
 *         too large to flatten.
 *
 * @pre ``nsec`` is below one second.
 * @pre ``sec`` came from ``ra8_eth_gptp_get_time`` (48-bit range).
 * @post The result is monotonic in ``sec`` and ``nsec``.
 * @post No hardware was touched.
 *
 * @note Pure and re-entrant.
 * @since 0.1.0
 */
static uint64_t gptp_flatten_ns(uint64_t sec, uint32_t nsec)
{
  if (sec > (uint64_t)k_gptp_sec_flatten_max) {
    return UINT64_MAX;
  }
  return (sec * (uint64_t)k_gptp_ns_per_sec) + (uint64_t)nsec;
}

/**
 * @brief Read the IP-version word and require it to be nonzero.
 *
 * @details
 * A reserved aperture either reads 0 or echoes the last write; ``PTPIPV``
 * is read-only with a nonzero reset value (HUM 35.3.1.1 p 1927), so a
 * nonzero read is evidence the GPTP block is really mapped here.
 *
 * @return True iff the register read succeeded and returned nonzero.
 * @retval true  ``PTPIPV`` read back a nonzero version word.
 * @retval false The read failed, or the aperture returned 0.
 *
 * @pre ``ra8_eth_gptp_init`` has succeeded.
 * @pre The console has been initialised.
 * @post One ``gptp: ipv=`` line has been queued.
 * @post No GPTP state was mutated.
 *
 * @note Not thread-safe; single-threaded demo loop.
 * @since 0.1.0
 */
static bool gptp_probe_presence(void)
{
  uint32_t        version = 0U;
  const ra8_err_t err     = ra8_eth_gptp_ip_version(&version);
  gptp_write(k_gptp_ipv_prefix, (uint32_t)(sizeof(k_gptp_ipv_prefix) - 1U));
  gptp_write_u64((uint64_t)version);
  gptp_write(k_gptp_crlf, (uint32_t)(sizeof(k_gptp_crlf) - 1U));
  if (err != k_ra8_ok) {
    return false;
  }
  return version != 0U;
}

/**
 * @brief Decide whether a measured advance matches the SysTick window.
 *
 * @param[in] advance_ns Nanoseconds the GPTP counter moved.
 * @param[in] elapsed_ms Milliseconds SysTick measured over the same window.
 *
 * @details
 * The band is a percentage of the SysTick-measured interval, so it scales
 * with the window and needs no absolute tolerance constant. ``band_ns`` can
 * never exceed ``expected_ns``, so the lower comparison cannot underflow.
 *
 * @return True iff the two agree to within ``k_gptp_tolerance_pct``.
 * @retval true  The advance is inside the band.
 * @retval false The advance is outside the band, or no time elapsed.
 *
 * @pre ``elapsed_ms`` is nonzero (the caller waited a real interval).
 * @pre Both arguments describe the same measurement window.
 * @post No hardware was touched.
 * @post The result is false whenever the counter did not move.
 *
 * @note Pure and re-entrant.
 * @since 0.1.0
 */
static bool gptp_advance_ok(uint64_t advance_ns, uint32_t elapsed_ms)
{
  if (elapsed_ms == 0U) {
    return false;
  }
  const uint64_t expected_ns = (uint64_t)elapsed_ms * (uint64_t)k_gptp_ns_per_ms;
  const uint64_t band_ns =
    (expected_ns * (uint64_t)k_gptp_tolerance_pct) / (uint64_t)k_gptp_pct_full;
  if (advance_ns > (expected_ns + band_ns)) {
    return false;
  }
  return advance_ns >= (expected_ns - band_ns);
}

/**
 * @brief Sample the counter twice around a SysTick-timed window.
 *
 * @return True iff both samples read back and the advance matched SysTick.
 * @retval true  The counter advanced by the measured interval.
 * @retval false A read failed, or the counter did not track SysTick.
 *
 * @pre ``gptp_arm`` enabled timer unit 0.
 * @pre The console has been initialised.
 * @post Two timestamp lines plus one ``gptp: adv_ns=`` line were queued.
 * @post No GPTP configuration was changed.
 *
 * @note Not thread-safe; single-threaded demo loop.
 * @since 0.1.0
 */
static bool gptp_measure(void)
{
  uint64_t sec0  = 0U;
  uint32_t nsec0 = 0U;
  uint64_t sec1  = 0U;
  uint32_t nsec1 = 0U;
  bool     ok    = true;

  const uint32_t ms0 = ra8_time_ms();
  if (ra8_eth_gptp_get_time(k_ra8_gptp_timer_0, &sec0, &nsec0) != k_ra8_ok) {
    ok = false;
  }
  ra8_delay_ms((uint32_t)k_gptp_window_ms);
  if (ra8_eth_gptp_get_time(k_ra8_gptp_timer_0, &sec1, &nsec1) != k_ra8_ok) {
    ok = false;
  }
  const uint32_t elapsed_ms = ra8_time_ms() - ms0;

  gptp_log_time(sec0, nsec0);
  gptp_log_time(sec1, nsec1);

  const uint64_t ns0        = gptp_flatten_ns(sec0, nsec0);
  const uint64_t ns1        = gptp_flatten_ns(sec1, nsec1);
  const uint64_t advance_ns = (ns1 > ns0) ? (ns1 - ns0) : 0ULL;

  gptp_write(k_gptp_adv_prefix, (uint32_t)(sizeof(k_gptp_adv_prefix) - 1U));
  gptp_write_u64(advance_ns);
  gptp_write(k_gptp_sys_prefix, (uint32_t)(sizeof(k_gptp_sys_prefix) - 1U));
  gptp_write_u64((uint64_t)elapsed_ms);
  gptp_write(k_gptp_crlf, (uint32_t)(sizeof(k_gptp_crlf) - 1U));

  if (!gptp_advance_ok(advance_ns, elapsed_ms)) {
    ok = false;
  }
  return ok;
}

/**
 * @brief Run one cycle: presence probe plus one advance measurement.
 *
 * @details
 * Both halves run unconditionally so one log block always appears, even when
 * the first check has already failed -- a bench operator needs the numbers
 * from the failing cycle, not just the verdict.
 *
 * @return True iff both checks passed.
 * @retval true  Presence probe and advance measurement both held.
 * @retval false At least one of them did not.
 *
 * @pre ``gptp_arm`` has succeeded.
 * @pre The console has been initialised.
 * @post One log block has been queued.
 * @post No driver was closed or reconfigured by the cycle.
 *
 * @note Not thread-safe; single-threaded demo loop.
 * @since 0.1.0
 */
static bool gptp_run_cycle(void)
{
  bool ok = true;
  if (!gptp_probe_presence()) {
    ok = false;
  }
  if (!gptp_measure()) {
    ok = false;
  }
  return ok;
}

/**
 * @brief Core bring-up: CGC -> ESWCLK -> MSTP -> TIME -> console + LED.
 *
 * @details
 * ESWCLK comes last because ``ra8_cgc_eswclk_init`` powers the ESWM domain
 * and needs HOCO and the PLL already stable.
 *
 * @pre Reset defaults are in force (single-threaded boot context).
 * @pre No peripheral has been claimed yet.
 * @post On return every core clock, ESWCLK, console and LED is live.
 * @post Any failure parks the CPU via ``gptp_panic_halt``.
 *
 * @note Boot-time only; not re-entrant.
 * @since 0.1.0
 */
static void gptp_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    gptp_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    gptp_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    gptp_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    gptp_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_gptp_baud) != k_ra8_ok) {
    gptp_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    gptp_panic_halt();
  }
  if (ra8_cgc_eswclk_init() != k_ra8_ok) {
    gptp_panic_halt();
  }
}

/**
 * @brief Programme the GPTP timer from the live ESWCLK and start it.
 *
 * @details
 * Reads the ESWCLK frequency published by ``ra8_cgc_eswclk_init``, hands it
 * to ``ra8_eth_gptp_init`` (which derives ``PTPTIVCt``), loads
 * ``k_gptp_base_sec`` into the 78-bit offset and enables the timer unit.
 *
 * @return ``ra8_err_t`` error code from the first failing step.
 * @retval k_ra8_ok The counter is programmed and running.
 * @retval Other    Forwarded from the failing driver call.
 *
 * @pre ``gptp_setup_or_halt`` has started ESWCLK and ungated MSTP.
 * @pre IRQs are masked or this is single-threaded init.
 * @post On ``k_ra8_ok`` timer unit 0 is counting.
 * @post On failure no partial GPTP state is relied upon by the caller.
 *
 * @note Boot-time only; not re-entrant.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t gptp_arm(void)
{
  uint32_t        eswclk_hz = 0U;
  const ra8_err_t hz_err    = ra8_cgc_eswclk_hz(&eswclk_hz);
  if (hz_err != k_ra8_ok) {
    return hz_err;
  }
  const ra8_eth_gptp_cfg_t cfg      = {.clk_hz = eswclk_hz};
  const ra8_err_t          init_err = ra8_eth_gptp_init(&cfg);
  if (init_err != k_ra8_ok) {
    return init_err;
  }
  const ra8_err_t off_err =
    ra8_eth_gptp_set_offset(k_ra8_gptp_timer_0, (uint64_t)k_gptp_base_sec, 0U);
  if (off_err != k_ra8_ok) {
    return off_err;
  }
  return ra8_eth_gptp_timer_enable(k_ra8_gptp_timer_0);
}

void main(void)
{
  gptp_setup_or_halt();
  ra8_isr_globals_enable();

  if (gptp_arm() != k_ra8_ok) {
    gptp_panic_halt();
  }

  while (1) {
    const bool healthy = gptp_run_cycle();
    if (healthy) {
      gptp_write(k_gptp_verdict_pass, (uint32_t)(sizeof(k_gptp_verdict_pass) - 1U));
    } else {
      gptp_write(k_gptp_verdict_fail, (uint32_t)(sizeof(k_gptp_verdict_fail) - 1U));
    }
    if (ra8_board_led_toggle(k_ra8_board_led1) != k_ra8_ok) {
      break;
    }
    ra8_delay_ms((uint32_t)k_gptp_period_ms);
  }
  gptp_panic_halt();
}
