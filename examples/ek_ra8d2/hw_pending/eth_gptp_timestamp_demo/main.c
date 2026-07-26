/**
 * @file examples/ek_ra8d2/hw_pending/eth_gptp_timestamp_demo/main.c
 * @brief gPTP / IEEE 1588 hardware-timestamp clock demo (ra8_eth_gptp + ra8_ptp)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Exercises the Ethernet hardware-timestamp path that no other example
 * references (recon gap #133). Two layered drivers are driven end to end:
 *
 *   1. ``ra8_eth_gptp`` -- the GPTP block that owns the IEEE 1588 free-running
 *      hardware timestamp counter shared by the GMAC ports (HUM Ch 31-32
 *      "Ethernet" / gPTP timer). This app brings up the timer and reads its
 *      status word.
 *   2. ``ra8_ptp`` -- the SYNFP / STCA layer that turns the raw counter into an
 *      IEEE 1588-2019 ordinary/boundary clock: role select, absolute-time
 *      get/set (the timestamp capture), the disciplined-clock servo (step
 *      + rate), and the Sync / Announce generators.
 *
 * Per cycle the app:
 *   - loads an absolute PTP time (``ra8_ptp_set_time``) then reads it back
 *     twice (``ra8_ptp_get_time``) -- the hardware-timestamp capture path;
 *   - applies one servo step + rate correction (``ra8_ptp_adjust_time`` /
 *     ``ra8_ptp_adjust_rate``) and reads the offset from the reference clock;
 *   - queues one Sync + one Announce (``ra8_ptp_send_sync`` /
 *     ``ra8_ptp_send_announce``);
 *   - reads the GPTP status word (``ra8_eth_gptp_get_status``).
 *
 * The fixed verdict ``"gptp: clock PASS"`` prints only when every driver call
 * in the cycle returned ``k_ra8_ok``. The captured seconds.nanoseconds pair is
 * logged verbatim so a bench operator can watch the counter advance.
 *
 * Bring-up sequence (CGC -> MSTP -> TIME -> peripheral):
 *   1. CGC + SysTick + UART console (SCI8 on PD_02 / PD_03 -> J-Link OB).
 *   2. ``ra8_eth_gptp_init`` + ``ra8_ptp_open`` (role ``k_ra8_ptp_role_controller``).
 *   3. Loop once per ``k_gptp_period_ms``.
 *
 * hw_pending: ``tools/ra8_emulator`` has no Ethernet / GPTP peripheral model
 * (the GWCA/ETHA/PHY bring-up is shimmed to no-ops there), and the EK-RA8D2
 * Ethernet wire is marginal (#21), so this is a compile-gated, bench-only
 * example. The value it adds is driving the real driver API + a clean ARM
 * cross-build, matching the driver-gap example wave (#182-188).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_eth_gptp.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_ptp.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_gptp_baud      = 115200U,    /**< SCI8 console baud.                   */
  k_gptp_period_ms = 1000U,      /**< Delay between clock cycles.          */
  k_gptp_base_sec  = 1000000000U /**< Absolute epoch seconds loaded/cycle. */
} gptp_const_t;

/** @brief Formatting + iteration constants. */
typedef enum : uint8_t {
  k_gptp_radix       = 10U, /**< Decimal serialiser radix.             */
  k_gptp_dec_u32_max = 10U, /**< Max decimal digits for a uint32_t.    */
  k_gptp_dec_u64_max = 20U, /**< Max decimal digits for a uint64_t.    */
  k_gptp_minus       = '-', /**< ASCII minus for signed serialisation. */
} gptp_fmt_t;

/**
 * @brief Locally-administered demo MAC octets loaded into ``PTP_MAC0/1``.
 *
 * @details
 * The 0x02 top octet marks a locally-administered unicast address so the
 * demo never collides with a real vendor OUI on the bench segment.
 */
typedef enum : uint8_t {
  k_gptp_mac_b0 = 0x02U, /**< Byte 0: locally-administered unicast. */
  k_gptp_mac_b1 = 0x00U, /**< Byte 1.                               */
  k_gptp_mac_b2 = 0x5EU, /**< Byte 2.                               */
  k_gptp_mac_b3 = 0x00U, /**< Byte 3.                               */
  k_gptp_mac_b4 = 0x53U, /**< Byte 4.                               */
  k_gptp_mac_b5 = 0x01U, /**< Byte 5.                               */
} gptp_mac_t;

/** @brief Disciplined-clock servo test corrections applied per cycle. */
typedef enum : int32_t {
  k_gptp_servo_step_ns  = 1000, /**< +1 us one-shot offset step. */
  k_gptp_servo_rate_ppb = 1000, /**< +1 ppm rate correction.     */
} gptp_servo_t;

/* Console line fragments (kept short so each write is one shift-register
 * fill; the periodic log is the only output path). */
static const uint8_t k_gptp_time_prefix[]  = "gptp: t=";
static const uint8_t k_gptp_time_dot[]     = ".";
static const uint8_t k_gptp_off_prefix[]   = "gptp: offset_ns=";
static const uint8_t k_gptp_sts_prefix[]   = "gptp: sts=0x";
static const uint8_t k_gptp_crlf[]         = "\r\n";
static const uint8_t k_gptp_verdict_pass[] = "gptp: clock PASS\r\n";
static const uint8_t k_gptp_verdict_fail[] = "gptp: clock FAIL\r\n";

/**
 * @brief Park forever after a fatal init error.
 *
 * @pre Called only after an unrecoverable bring-up failure.
 * @post CPU is parked; only a debugger or reset wakes it.
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
 * @pre ``ra8_board_uart_console_init`` has succeeded.
 * @pre ``data`` points at ``len`` readable bytes.
 * @post ``len`` bytes have been queued to the console UART.
 * @since 0.1.0
 */
static void gptp_write(const uint8_t* data, uint32_t len)
{
  (void)ra8_board_uart_console_write(data, (size_t)len);
}

/**
 * @brief Serialise an unsigned 64-bit value into decimal ASCII.
 *
 * @param[out] buf Destination, at least ``k_gptp_dec_u64_max`` bytes.
 * @param[in]  val Value to serialise.
 *
 * @return Number of digits written (1..20).
 *
 * @pre ``buf`` is non-NULL and sized for the widest uint64_t.
 * @pre No trailing NUL is required by the caller.
 * @post ``buf`` holds the most-significant digit first.
 * @post The return value is in ``[1, k_gptp_dec_u64_max]``.
 * @since 0.1.0
 */
static uint32_t gptp_u64_to_dec(uint8_t* buf, uint64_t val)
{
  if (val == 0U) {
    buf[0] = (uint8_t)'0';
    return 1U;
  }
  uint8_t  tmp[k_gptp_dec_u64_max];
  uint32_t n = 0U;
  uint64_t v = val;
  while (v != 0U) {
    tmp[n] = (uint8_t)('0' + (uint8_t)(v % (uint64_t)k_gptp_radix));
    v      = v / (uint64_t)k_gptp_radix;
    n++;
  }
  for (uint32_t i = 0U; i < n; i++) {
    buf[i] = tmp[n - 1U - i];
  }
  return n;
}

/**
 * @brief Log one unsigned 64-bit value as decimal ASCII.
 *
 * @param[in] val Value to print.
 *
 * @pre The console has been initialised.
 * @pre ``val`` fits in a uint64_t (always true).
 * @post The decimal digits of ``val`` have been queued to the console.
 * @since 0.1.0
 */
static void gptp_write_u64(uint64_t val)
{
  uint8_t        buf[k_gptp_dec_u64_max];
  const uint32_t n = gptp_u64_to_dec(buf, val);
  gptp_write(buf, n);
}

/**
 * @brief Log one signed 32-bit value (leading minus for negatives).
 *
 * @param[in] val Value to print (offset-nanoseconds range on this app).
 *
 * @pre The console has been initialised.
 * @pre ``val`` fits in an int32_t (always true).
 * @post For negatives a '-' precedes the magnitude digits.
 * @post The decimal representation of ``val`` has been queued.
 * @since 0.1.0
 */
static void gptp_write_i32(int32_t val)
{
  if (val < 0) {
    const uint8_t minus = (uint8_t)k_gptp_minus;
    gptp_write(&minus, 1U);
    gptp_write_u64((uint64_t)(-(int64_t)val));
  } else {
    gptp_write_u64((uint64_t)val);
  }
}

/**
 * @brief Capture the PTP wall clock: set, then read it back twice.
 *
 * @details
 * ``ra8_ptp_set_time`` loads an absolute epoch, then two back-to-back
 * ``ra8_ptp_get_time`` calls read the hardware-timestamp counter. Both
 * captured ``seconds.nanoseconds`` pairs are logged so the bench operator
 * can watch the free-running counter advance between reads.
 *
 * @return True iff the set and both reads returned ``k_ra8_ok``.
 *
 * @pre ``ra8_ptp_open`` has succeeded.
 * @pre The console has been initialised.
 * @post Two timestamp lines have been queued to the console.
 * @post No driver state other than the loaded time was mutated.
 * @since 0.1.0
 */
static bool gptp_probe_clock(void)
{
  bool ok = true;
  if (ra8_ptp_set_time((uint64_t)k_gptp_base_sec, 0U) != k_ra8_ok) {
    ok = false;
  }
  for (uint32_t i = 0U; i < 2U; i++) {
    uint64_t        sec  = 0U;
    uint32_t        nsec = 0U;
    const ra8_err_t err  = ra8_ptp_get_time(&sec, &nsec);
    if (err != k_ra8_ok) {
      ok = false;
    }
    gptp_write(k_gptp_time_prefix, (uint32_t)(sizeof(k_gptp_time_prefix) - 1U));
    gptp_write_u64(sec);
    gptp_write(k_gptp_time_dot, (uint32_t)(sizeof(k_gptp_time_dot) - 1U));
    gptp_write_u64((uint64_t)nsec);
    gptp_write(k_gptp_crlf, (uint32_t)(sizeof(k_gptp_crlf) - 1U));
  }
  return ok;
}

/**
 * @brief Run one disciplined-clock servo step + rate correction, log offset.
 *
 * @return True iff the step, rate, and offset read returned ``k_ra8_ok``.
 *
 * @pre ``ra8_ptp_open`` has succeeded.
 * @pre The console has been initialised.
 * @post One ``gptp: offset_ns=`` line has been queued.
 * @post The local clock has absorbed one step + one rate correction.
 * @since 0.1.0
 */
static bool gptp_run_servo(void)
{
  bool ok = true;
  if (ra8_ptp_adjust_time((int32_t)k_gptp_servo_step_ns) != k_ra8_ok) {
    ok = false;
  }
  if (ra8_ptp_adjust_rate((int32_t)k_gptp_servo_rate_ppb) != k_ra8_ok) {
    ok = false;
  }
  int32_t         offset_ns = 0;
  const ra8_err_t off_err   = ra8_ptp_get_offset(&offset_ns);
  if (off_err != k_ra8_ok) {
    ok = false;
  }
  gptp_write(k_gptp_off_prefix, (uint32_t)(sizeof(k_gptp_off_prefix) - 1U));
  gptp_write_i32(offset_ns);
  gptp_write(k_gptp_crlf, (uint32_t)(sizeof(k_gptp_crlf) - 1U));
  return ok;
}

/**
 * @brief Queue one Sync and one Announce as the transmitting clock.
 *
 * @return True iff both generators returned ``k_ra8_ok``.
 *
 * @pre ``ra8_ptp_set_role`` selected ``k_ra8_ptp_role_controller``.
 * @pre ``ra8_ptp_open`` has succeeded.
 * @post One Sync and one Announce have been queued for transmission.
 * @post No other PTP state was mutated.
 * @since 0.1.0
 */
static bool gptp_run_controller(void)
{
  bool ok = true;
  if (ra8_ptp_send_sync() != k_ra8_ok) {
    ok = false;
  }
  if (ra8_ptp_send_announce() != k_ra8_ok) {
    ok = false;
  }
  return ok;
}

/**
 * @brief Read + log the GPTP block status word.
 *
 * @return True iff ``ra8_eth_gptp_get_status`` returned ``k_ra8_ok``.
 *
 * @pre ``ra8_eth_gptp_init`` has succeeded.
 * @pre The console has been initialised.
 * @post One ``gptp: sts=0x`` line has been queued.
 * @post No GPTP register state was mutated (status read is passive).
 * @since 0.1.0
 */
static bool gptp_log_status(void)
{
  bool            ok   = true;
  uint32_t        mask = 0U;
  const ra8_err_t err  = ra8_eth_gptp_get_status(&mask);
  if (err != k_ra8_ok) {
    ok = false;
  }
  gptp_write(k_gptp_sts_prefix, (uint32_t)(sizeof(k_gptp_sts_prefix) - 1U));
  gptp_write_u64((uint64_t)mask);
  gptp_write(k_gptp_crlf, (uint32_t)(sizeof(k_gptp_crlf) - 1U));
  return ok;
}

/**
 * @brief Run one clock cycle: capture + servo + transmit + status.
 *
 * @return True iff every driver call in the cycle returned ``k_ra8_ok``.
 *
 * @pre ``gptp_arm`` initialised the GPTP timer and opened the PTP clock.
 * @pre The console has been initialised.
 * @post One log block (two timestamps + offset + status) has been queued.
 * @post No driver was closed or reset by the cycle.
 * @since 0.1.0
 */
static bool gptp_run_cycle(void)
{
  bool ok = true;
  if (!gptp_probe_clock()) {
    ok = false;
  }
  if (!gptp_run_servo()) {
    ok = false;
  }
  if (!gptp_run_controller()) {
    ok = false;
  }
  if (!gptp_log_status()) {
    ok = false;
  }
  return ok;
}

/**
 * @brief Core bring-up: CGC -> MSTP -> TIME -> console + LED.
 *
 * @pre Reset defaults are in force (single-threaded boot context).
 * @pre No peripheral has been claimed yet.
 * @post On return every core clock + console + LED is live.
 * @post Any failure parks the CPU via ``gptp_panic_halt``.
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
}

/**
 * @brief Bring up the GPTP timer and open the IEEE 1588 clock in the
 *        ``k_ra8_ptp_role_controller`` role.
 *
 * @details
 * ``ra8_eth_gptp_init`` ungates and starts the shared hardware-timestamp
 * counter; ``ra8_ptp_open`` layers the SYNFP / STCA IEEE 1588 clock on top
 * (domain 0, 1 s Sync interval, the demo MAC, default clockClass), and
 * ``ra8_ptp_set_role`` selects ``k_ra8_ptp_role_controller`` so the Sync / Announce
 * generators are legal.
 *
 * @return ``ra8_err_t`` error code from the first failing step.
 * @retval k_ra8_ok GPTP timer up and the PTP clock is the transmitting clock.
 * @retval Other   Forwarded from ``ra8_eth_gptp_init`` / ``ra8_ptp_open`` /
 *                 ``ra8_ptp_set_role``.
 *
 * @pre ``gptp_setup_or_halt`` has ungated MSTP.
 * @pre IRQs are masked or this is single-threaded init.
 * @post On ``k_ra8_ok`` the timestamp counter runs; role ``k_ra8_ptp_role_controller``.
 * @post On failure no partial PTP state is relied upon by the caller.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t gptp_arm(void)
{
  const ra8_err_t gptp_err = ra8_eth_gptp_init();
  if (gptp_err != k_ra8_ok) {
    return gptp_err;
  }
  const ra8_ptp_cfg_t cfg = {
    .domain        = (uint8_t)k_ra8_ptp_domain_default,
    .sync_interval = k_ra8_ptp_sync_int_1,
    .mac_addr =
      {k_gptp_mac_b0, k_gptp_mac_b1, k_gptp_mac_b2, k_gptp_mac_b3, k_gptp_mac_b4, k_gptp_mac_b5},
    .clock_class = k_ra8_ptp_clock_class_default,
  };
  const ra8_err_t open_err = ra8_ptp_open(&cfg);
  if (open_err != k_ra8_ok) {
    return open_err;
  }
  return ra8_ptp_set_role(k_ra8_ptp_role_controller);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
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
  return 0;
}
#pragma GCC diagnostic pop
