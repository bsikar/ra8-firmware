/**
 * @file examples/ek_ra8d2/hw_validated/hil/watchdog_demo/main.c
 * @brief IWDT watchdog + reset-cause demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Two-stage demo:
 *
 *   1. On boot, snapshot the reset cause via ``ra8_reset_init`` +
 *      ``ra8_reset_get_cause`` and log it over SCI8 -- a power-on
 *      reset reads ``power_on``, while a deliberate IWDT trip from
 *      the previous run reads ``iwdt``.
 *   2. Refresh the IWDT counter for ``k_wdt_demo_alive_seconds``
 *      seconds (LED1 toggles each refresh as a heartbeat), then
 *      stop refreshing and let the IWDT underflow trip a hardware
 *      reset. On the next boot the cause flips to ``iwdt`` and the
 *      cycle repeats.
 *
 * The IWDT period itself is configured by the OFS0 option-setting
 * register at flash-write time (the chip cannot be reconfigured at
 * runtime); ``examples/ek_ra8d2/uart_hello/linker_script.ld`` -- the
 * shared template -- sets a multi-second window so the
 * "stop refreshing" stage takes a visible amount of time before the
 * reset fires.
 *
 * Note: in the fake (host-side test) ``ra8_reset_software_reset``
 * returns; on real silicon it never returns. The app's ``while`` loop
 * is the IWDT-stop stage, terminated only by the chip resetting.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_iwdt.h"
#include "ra8_reset.h"
#include "ra8_time.h"

/** @brief Time-unit conversion. */
typedef enum : uint32_t {
  k_ms_per_sec = 1000U, /**< Milliseconds per second. */
} wdt_demo_time_unit_t;

typedef enum : uint32_t {
  k_wdt_demo_baud          = 115200U, /**< Wdt demo baud.          */
  k_wdt_demo_refresh_ms    = 100U,    /**< Wdt demo refresh ms.    */
  k_wdt_demo_alive_seconds = 30U,     /**< Wdt demo alive seconds. */
} wdt_demo_const_t;

/** @brief Banner emitted when the cause is ``power_on``. */
static const uint8_t s_wdt_demo_msg_pwr[] = "wdt: boot reason=power_on\r\n";
/** @brief Banner emitted when the cause is ``iwdt`` (we tripped). */
static const uint8_t s_wdt_demo_msg_wdt[] = "wdt: boot reason=iwdt\r\n";
/** @brief Banner emitted for any other cause. */
static const uint8_t s_wdt_demo_msg_other[] = "wdt: boot reason=other\r\n";
/** @brief Logged just before the demo stops feeding the IWDT. */
static const uint8_t s_wdt_demo_msg_stop[] = "wdt: stopping refresh, expect reset\r\n";

/**
 * @brief Park the processor after an unrecoverable watchdog demo failure.
 *
 * @details Repeats wait-for-interrupt forever when setup fails before the
 *          intentional watchdog-reset phase can begin.
 *
 * @return None.
 *
 * @pre The caller has determined the reset demonstration cannot continue.
 * @pre Any desired console diagnostic has already completed.
 * @post The function never returns to its caller.
 * @post No later watchdog refresh or intentional timeout is initiated.
 *
 * @note This halt is distinct from deliberately stopping IWDT refresh in main.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_wdt_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Pick the right banner string for ``cause``.
 *
 * @par MC/DC:
 * Compound decision: ``cause == power_on || cause == iwdt`` (default).
 * Two atomic conditions x N+1 = 3 vectors -- power_on (this returns
 * pwr msg), iwdt (returns wdt msg), other cause (returns other msg).
 *
 * @param[in]  cause   Decoded reset cause used to select the banner.
 * @param[out] out_len Receives the selected banner length in bytes.
 *
 * @return Pointer to the immutable banner matching @p cause.
 * @retval s_wdt_demo_msg_pwr Power-on reset was decoded.
 * @retval s_wdt_demo_msg_wdt IWDT reset was decoded.
 * @retval s_wdt_demo_msg_other Any other reset cause was decoded.
 *
 * @pre @p out_len points to writable caller-owned storage.
 * @pre The file-scope banner arrays remain alive for the caller's write.
 * @post @p out_len matches the returned banner excluding its terminator.
 * @post No reset or watchdog state is modified.
 *
 * @note Returned storage is immutable and must not be released.
 * @since 0.1.0
 */
RA8_INTERNAL static const uint8_t* internal_wdt_demo_banner_for(ra8_reset_cause_t cause,
                                                                uint32_t*         out_len)
{
  if (cause == k_ra8_reset_cause_power_on) {
    *out_len = (uint32_t)(sizeof(s_wdt_demo_msg_pwr) - 1U);
    return s_wdt_demo_msg_pwr;
  }
  if (cause == k_ra8_reset_cause_iwdt) {
    *out_len = (uint32_t)(sizeof(s_wdt_demo_msg_wdt) - 1U);
    return s_wdt_demo_msg_wdt;
  }
  *out_len = (uint32_t)(sizeof(s_wdt_demo_msg_other) - 1U);
  return s_wdt_demo_msg_other;
}

/**
 * @brief Initialize clocks, reset-cause decoding, console, and status LED.
 *
 * @details Starts the system time base, initializes reset-cause support, opens
 *          the board UART, and claims LED1 before the watchdog demonstration
 *          begins. Any failed dependency enters the permanent panic halt.
 *
 * @return None.
 *
 * @pre Reset-time initialization configured the core and C runtime.
 * @pre The board console and LED1 are available to this image.
 * @post On success reset-cause, timing, console, and LED services are ready.
 * @post On failure the function never returns to its caller.
 *
 * @note Single-shot boot helper; it does not start the IWDT itself.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_wdt_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_wdt_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_wdt_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_wdt_demo_panic_halt();
  }
  if (ra8_reset_init() != k_ra8_ok) {
    internal_wdt_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_wdt_demo_baud) != k_ra8_ok) {
    internal_wdt_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    internal_wdt_demo_panic_halt();
  }
  if (ra8_iwdt_init() != k_ra8_ok) {
    internal_wdt_demo_panic_halt();
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  internal_wdt_demo_setup_or_halt();
  ra8_isr_globals_enable();

  ra8_reset_cause_t cause = k_ra8_reset_cause_unknown;
  (void)ra8_reset_get_cause(&cause);
  uint32_t       msg_len = 0U;
  const uint8_t* msg     = internal_wdt_demo_banner_for(cause, &msg_len);
  (void)ra8_board_uart_console_write(msg, (size_t)msg_len);

  /* Stage 1: refresh for ``alive_seconds`` seconds. */
  const uint32_t refreshes_per_sec = k_ms_per_sec / (uint32_t)k_wdt_demo_refresh_ms;
  const uint32_t total_refreshes   = (uint32_t)k_wdt_demo_alive_seconds * refreshes_per_sec;
  for (uint32_t i = 0U; i < total_refreshes; ++i) {
    ra8_iwdt_refresh_deferred();
    (void)ra8_board_led_toggle(k_ra8_board_led1);
    ra8_delay_ms((uint32_t)k_wdt_demo_refresh_ms);
  }

  /* Stage 2: stop refreshing and let the IWDT underflow reset us. */
  (void)ra8_board_uart_console_write(s_wdt_demo_msg_stop,
                                     (size_t)(sizeof(s_wdt_demo_msg_stop) - 1U));
  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
