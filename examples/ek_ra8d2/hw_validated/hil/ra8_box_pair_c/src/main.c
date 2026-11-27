/**
 * @file examples/ek_ra8d2/hw_validated/hil/ra8_box_pair_c/src/main.c
 * @brief Paired C client verifying `ra8_box` Zig C ABI vectors on EK-RA8D2.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up the EK-RA8D2 board (clocks, millisecond SysTick timebase, SCI8
 * UART console at 115200 8N1, and LED1), executes every shared error and
 * layout vector from `tests/zig_abi_pairs/ra8_box_pair/vectors.json` via
 * `ra8_box_pair_run_all()`, and emits the deterministic `HIL_EXPECT` banner
 * (`RA8_BOX_PAIR_C PASS <passed>/<total>`) over SCI8.
 *
 * @par Architectural ring
 * [Ring 6 / APP] {World: S} -- application-layer code that runs in
 * the Secure world.
 *
 * @author Brighton Sikarskie
 * @date 2026-11-24
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_box_pair_runner.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/** @brief Compile-time console and heartbeat settings. */
typedef enum : uint32_t {
  k_pair_baud         = 115200U, /**< SCI8 console baud rate.            */
  k_pair_period_ms    = 1000U,   /**< Pass banner / LED blink period ms. */
  k_pair_banner_bytes = 96U,     /**< Bounded stack banner buffer size.  */
  k_pair_dec_base     = 10U,     /**< Decimal formatting radix.          */
} ra8_box_pair_c_config_t;

/** @brief Observable pass latch for debugger / memprobe inspection. */
volatile uint32_t g_ra8_box_pair_c_pass = 0U;

/** @brief Halt forever in WFI on fatal setup failure or vector mismatch. */
RA8_INTERNAL static void internal_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Append a string literal or identifier cleanly into a bounded buffer. */
RA8_INTERNAL static size_t
internal_append_str(uint8_t* dst, size_t cap, size_t pos, const char* src)
{
  if ((dst == nullptr) || (src == nullptr)) {
    return pos;
  }

  for (size_t i = 0U; (src[i] != '\0') && (pos < cap); ++i) {
    dst[pos] = (uint8_t)src[i];
    ++pos;
  }
  return pos;
}

/** @brief Append a decimal `uint16_t` without linking printf formatting. */
RA8_INTERNAL static size_t internal_append_u16(uint8_t* dst, size_t cap, size_t pos, uint16_t value)
{
  if (dst == nullptr) {
    return pos;
  }

  char     digits[6] = {};
  size_t   count     = 0U;
  uint16_t rem       = value;

  do {
    digits[count] = (char)('0' + (rem % (uint16_t)k_pair_dec_base));
    rem           = (uint16_t)(rem / (uint16_t)k_pair_dec_base);
    ++count;
  } while ((rem != 0U) && (count < sizeof(digits)));

  while ((count > 0U) && (pos < cap)) {
    --count;
    dst[pos] = (uint8_t)digits[count];
    ++pos;
  }
  return pos;
}

/** @brief Format and write the deterministic PASS or FAIL verdict banner. */
RA8_INTERNAL static void internal_emit_banner(bool ok, const ra8_box_pair_result_t* res)
{
  if (res == nullptr) {
    return;
  }

  uint8_t buf[k_pair_banner_bytes] = {};
  size_t  len                      = 0U;

  if (ok) {
    len = internal_append_str(buf, sizeof(buf), len, "RA8_BOX_PAIR_C PASS ");
    len = internal_append_u16(buf, sizeof(buf), len, res->passed);
    len = internal_append_str(buf, sizeof(buf), len, "/");
    len = internal_append_u16(buf, sizeof(buf), len, res->total);
    len = internal_append_str(buf, sizeof(buf), len, "\r\n");
  } else {
    const char* id = (res->failed_id != nullptr) ? res->failed_id : "unknown";
    len            = internal_append_str(buf, sizeof(buf), len, "RA8_BOX_PAIR_C FAIL ");
    len            = internal_append_u16(buf, sizeof(buf), len, res->failed);
    len            = internal_append_str(buf, sizeof(buf), len, "/");
    len            = internal_append_u16(buf, sizeof(buf), len, res->total);
    len            = internal_append_str(buf, sizeof(buf), len, " ");
    len            = internal_append_str(buf, sizeof(buf), len, id);
    len            = internal_append_str(buf, sizeof(buf), len, "\r\n");
  }

  (void)ra8_board_uart_console_write(buf, len);
}

/** @brief Bring up CGC, timebase, SCI8 console, and LED1; halt on failure. */
RA8_INTERNAL static void internal_setup_or_halt(void)
{
  const ra8_board_bringup_cfg_t cfg = {
    .console_baud = (uint32_t)k_pair_baud,
    .leds_mask    = (uint32_t)k_ra8_board_bringup_led1,
  };
  ra8_board_bringup_out_t out = {};

  if (ra8_board_bringup(&cfg, &out) != k_ra8_ok) {
    internal_panic_halt();
  }
}

/**
 * @brief Application entry. Runs shared `ra8_box` ABI vectors and emits verdict.
 *
 * @pre Reset_Handler has copied `.data` and zeroed `.bss`.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On full pass, sets `g_ra8_box_pair_c_pass = 1`, prints the PASS banner
 *       periodically, and toggles LED1.
 * @post On any vector failure or HAL error, emits FAIL banner and halts in WFI.
 *
 * @since 0.1.0
 */
void main(void)
{
  internal_setup_or_halt();
  ra8_isr_globals_enable();

  ra8_box_pair_result_t result = {};
  const bool            ok     = ra8_box_pair_run_all(&result);

  if (!ok) {
    g_ra8_box_pair_c_pass = 0U;
    internal_emit_banner(false, &result);
    internal_panic_halt();
  }

  g_ra8_box_pair_c_pass = 1U;
  while (1) {
    internal_emit_banner(true, &result);
    if (ra8_board_led_toggle(k_ra8_board_led1) != k_ra8_ok) {
      break;
    }
    ra8_delay_ms((uint32_t)k_pair_period_ms);
  }

  internal_panic_halt();
}
