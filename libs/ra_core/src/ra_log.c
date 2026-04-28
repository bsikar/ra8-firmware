/**
 * @file ra_log.c
 * @brief Default log sink implementation
 *
 * @details
 * Minimal log backend for the bare-metal bring-up phase. Every log
 * function is a weak symbol that writes a single line to the ARM
 * Cortex-M85 ITM (Instrumented Trace Macrocell) stimulus port 0. With
 * an attached J-Link the output shows up in SWV Console (e2 studio),
 * `JLinkRTTViewer`, or `JLinkSWOViewer` without any extra wiring.
 *
 * Why ITM:
 *  - Zero hardware setup beyond a single register write during init.
 *  - Works before any peripheral clock is up (it uses the CoreSight
 *    debug clock, not the CPU peripheral bus).
 *  - Non-blocking: if the debugger is not draining the FIFO the write
 *    is silently dropped -- fine for a fire-and-forget logger.
 *
 * A UART-backed log sink for boards without a J-Link can be plugged
 * in later by overriding the `internal_ra_log_*` functions with
 * non-weak definitions elsewhere in the project.
 *
 * @note Format: `"[TAG] level: message"` followed by a newline. The
 *       `*_val` variants append `" = <decimal>"`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_log.h"

#include <stdint.h>

/* =============================================================================
 * ITM stimulus port 0 -- CoreSight ITM (Cortex-M85 has one).
 * =============================================================================
 */

typedef enum : uintptr_t {
  k_ra_itm_stim_base = 0xE0000000UL, /**< ITM stimulus port 0 base. */
  k_ra_itm_tcr_addr  = 0xE0000E80UL, /**< ITM Trace Control Register. */
  k_ra_itm_tenr_addr = 0xE0000E00UL, /**< ITM Trace Enable  Register. */
} ra_itm_addr_t;

/**
 * @brief Get the ITM stimulus-port-0 register.
 * @return Volatile pointer to the 32-bit stimulus FIFO.
 */
static inline volatile uint32_t* internal_itm_stim0(void)
{
  return (volatile uint32_t*)k_ra_itm_stim_base;
}

/**
 * @brief Get the ITM Trace Control Register.
 * @return Volatile pointer to TCR.
 */
static inline volatile uint32_t* internal_itm_tcr(void)
{
  return (volatile uint32_t*)k_ra_itm_tcr_addr;
}

/**
 * @brief Get the ITM Trace Enable Register.
 * @return Volatile pointer to TENR.
 */
static inline volatile uint32_t* internal_itm_tenr(void)
{
  return (volatile uint32_t*)k_ra_itm_tenr_addr;
}

/**
 * @brief Check whether ITM port 0 is enabled and ready to accept bytes.
 * @return `true` if enabled and not full, `false` otherwise.
 *
 * @details
 * Reads TCR, TENR, and the STIM0 FIFO register directly. On the
 * target these are the real Cortex-M85 ITM registers. On the
 * `RA_SIMULATOR_MODE` host test build the same virtual addresses are
 * backed by anonymous RAM via `ra_sim_mmap.c`, so the same logic
 * produces a well-defined answer: the log backend stays a no-op until
 * a test pre-seeds the three registers, at which point it will walk
 * the full emit path.
 */
static inline bool internal_itm_ready(void)
{
  const uint32_t tcr  = *internal_itm_tcr();
  const uint32_t tenr = *internal_itm_tenr();
  /* TCR bit 0 = ITMENA. TENR bit 0 = stimulus port 0 enabled. */
  if ((tcr & 1U) == 0U) {
    return false;
  }
  if ((tenr & 1U) == 0U) {
    return false;
  }
  return *internal_itm_stim0() != 0U;
}

/**
 * @brief Blocking-but-bounded write of a single byte to ITM port 0.
 * @param[in] byte Character to emit.
 */
static inline void internal_itm_putc(uint8_t byte)
{
  /* A real build should pre-check `internal_itm_ready()` once during
   * init and skip the call entirely when the debugger is absent. For
   * now we poll with a small bound so a disconnected debugger never
   * stalls the firmware. */
  enum : uint32_t {
    k_ra_itm_poll_limit = 1000U,
  };
  for (uint32_t i = 0U; i < k_ra_itm_poll_limit; i++) {
    if (*internal_itm_stim0() != 0U) {
      *(volatile uint8_t*)k_ra_itm_stim_base = byte;
      return;
    }
  }
}

/**
 * @brief Emit a NUL-terminated string over ITM port 0.
 * @param[in] s String to emit (must be non-NULL).
 */
static inline void internal_itm_puts(const char* s)
{
  while (*s != '\0') {
    internal_itm_putc((uint8_t)*s++);
  }
}

/**
 * @brief Emit an unsigned decimal integer over ITM port 0.
 * @param[in] value Value to emit.
 */
typedef enum : uint8_t {
  k_ra_u32_max_digits = 10U, /**< Max decimal digits in a uint32_t. */
  k_ra_decimal_base   = 10U, /**< Decimal radix.                    */
} ra_log_u32_t;

static inline void internal_itm_put_u32(uint32_t value)
{
  char    buf[k_ra_u32_max_digits + 1U] = {};
  uint8_t i                             = 0U;
  if (value == 0U) {
    internal_itm_putc('0');
    return;
  }
  while (value != 0U && i < k_ra_u32_max_digits) {
    buf[i++] = (char)('0' + (char)(value % (uint32_t)k_ra_decimal_base));
    value /= (uint32_t)k_ra_decimal_base;
  }
  /* Digits were pushed LSB-first; emit MSB-first. */
  while (i > 0U) {
    i--;
    internal_itm_putc((uint8_t)buf[i]);
  }
}

/**
 * @brief Emit a signed decimal integer over ITM port 0.
 * @param[in] value Value to emit.
 */
static inline void internal_itm_put_i32(int32_t value)
{
  if (value < 0) {
    internal_itm_putc((uint8_t)'-');
    /* Casting INT32_MIN negated is UB; subtract through uint32_t. */
    internal_itm_put_u32((uint32_t)(-(int64_t)value));
    return;
  }
  internal_itm_put_u32((uint32_t)value);
}

/* =============================================================================
 * Public API (weak so downstream can override)
 * =============================================================================
 */

void ra_log_init(void)
{
  /* ITM is set up by the debugger when it attaches. Nothing to do here
   * in the default backend. A UART-backed override would configure its
   * SCI channel in its own ra_log_init(). */
}

/**
 * @brief Common tagged-line emit helper.
 * @param[in] level String like `"ERROR"` or `"INFO"`.
 * @param[in] tag   Component tag.
 * @param[in] msg   Free-form message.
 */
static void internal_emit_line(const char* level, const char* tag, const char* msg)
{
  if (!internal_itm_ready()) {
    return;
  }
  internal_itm_putc((uint8_t)'[');
  internal_itm_puts(tag);
  internal_itm_putc((uint8_t)']');
  internal_itm_putc((uint8_t)' ');
  internal_itm_puts(level);
  internal_itm_putc((uint8_t)':');
  internal_itm_putc((uint8_t)' ');
  internal_itm_puts(msg);
  internal_itm_putc((uint8_t)'\r');
  internal_itm_putc((uint8_t)'\n');
}

/**
 * @brief Common tagged-line-with-value emit helper (unsigned).
 */
static void
internal_emit_line_u(const char* level, const char* tag, const char* msg, uint32_t value)
{
  if (!internal_itm_ready()) {
    return;
  }
  internal_itm_putc((uint8_t)'[');
  internal_itm_puts(tag);
  internal_itm_putc((uint8_t)']');
  internal_itm_putc((uint8_t)' ');
  internal_itm_puts(level);
  internal_itm_putc((uint8_t)':');
  internal_itm_putc((uint8_t)' ');
  internal_itm_puts(msg);
  internal_itm_putc((uint8_t)'=');
  internal_itm_put_u32(value);
  internal_itm_putc((uint8_t)'\r');
  internal_itm_putc((uint8_t)'\n');
}

/**
 * @brief Common tagged-line-with-value emit helper (signed).
 */
static void internal_emit_line_i(const char* level, const char* tag, const char* msg, int32_t value)
{
  if (!internal_itm_ready()) {
    return;
  }
  internal_itm_putc((uint8_t)'[');
  internal_itm_puts(tag);
  internal_itm_putc((uint8_t)']');
  internal_itm_putc((uint8_t)' ');
  internal_itm_puts(level);
  internal_itm_putc((uint8_t)':');
  internal_itm_putc((uint8_t)' ');
  internal_itm_puts(msg);
  internal_itm_putc((uint8_t)'=');
  internal_itm_put_i32(value);
  internal_itm_putc((uint8_t)'\r');
  internal_itm_putc((uint8_t)'\n');
}

/* ---- plain string log -------------------------------------------------- */

__attribute__((weak)) void internal_ra_log_error(const char* tag, const char* message)
{
  internal_emit_line("ERROR", tag, message);
}

__attribute__((weak)) void internal_ra_log_warn(const char* tag, const char* message)
{
  internal_emit_line("WARN", tag, message);
}

__attribute__((weak)) void internal_ra_log_info(const char* tag, const char* message)
{
  internal_emit_line("INFO", tag, message);
}

__attribute__((weak)) void internal_ra_log_debug(const char* tag, const char* message)
{
  internal_emit_line("DEBUG", tag, message);
}

/* ---- string + value log ------------------------------------------------- */

__attribute__((weak)) void
internal_ra_log_error_val(const char* tag, const char* message, uint32_t value)
{
  internal_emit_line_u("ERROR", tag, message, value);
}

__attribute__((weak)) void
internal_ra_log_warn_val(const char* tag, const char* message, uint32_t value)
{
  internal_emit_line_u("WARN", tag, message, value);
}

__attribute__((weak)) void
internal_ra_log_info_val(const char* tag, const char* message, uint32_t value)
{
  internal_emit_line_u("INFO", tag, message, value);
}

__attribute__((weak)) void
internal_ra_log_debug_val(const char* tag, const char* message, int32_t value)
{
  internal_emit_line_i("DEBUG", tag, message, value);
}

/* =============================================================================
 * ra_err_to_str (lives here because the log backend is the only user)
 * =============================================================================
 */

#include "ra_err.h"

/* NOLINTNEXTLINE(readability-function-size) -- lookup table by design. */
const char* ra_err_to_str(ra_err_t err)
{
  switch (err) {
    case k_ra_ok:
      return "ok";
    case k_ra_fail:
      return "fail";
    case k_ra_err_no_mem:
      return "no_mem";
    case k_ra_err_invalid_arg:
      return "invalid_arg";
    case k_ra_err_invalid_state:
      return "invalid_state";
    case k_ra_err_invalid_size:
      return "invalid_size";
    case k_ra_err_not_found:
      return "not_found";
    case k_ra_err_not_supported:
      return "not_supported";
    case k_ra_err_timeout:
      return "timeout";
    case k_ra_err_busy:
      return "busy";
    case k_ra_err_no_data:
      return "no_data";
    case k_ra_err_would_block:
      return "would_block";
    case k_ra_err_exists:
      return "exists";
    case k_ra_err_empty:
      return "empty";
    case k_ra_err_cancelled:
      return "cancelled";
    case k_ra_err_not_initialized:
      return "not_initialized";
    case k_ra_err_estop:
      return "estop";
    case k_ra_err_hw_init_failed:
      return "hw_init_failed";
    case k_ra_err_hw_not_ready:
      return "hw_not_ready";
    case k_ra_err_hw_timeout:
      return "hw_timeout";
    case k_ra_err_hw_error:
      return "hw_error";
    case k_ra_err_gpio_conflict:
      return "gpio_conflict";
    case k_ra_err_gpio_invalid_port:
      return "gpio_invalid_port";
    case k_ra_err_gpio_invalid_pin:
      return "gpio_invalid_pin";
    case k_ra_err_out_of_range:
      return "out_of_range";
    case k_ra_err_hw_unmapped:
      return "hw_unmapped";
    case k_ra_err_rtos_error:
      return "rtos_error";
    case k_ra_err_rtos_thread_create:
      return "rtos_thread_create";
    case k_ra_err_rtos_semaphore:
      return "rtos_semaphore";
    case k_ra_err_rtos_mutex:
      return "rtos_mutex";
    case k_ra_err_rtos_queue:
      return "rtos_queue";
    case k_ra_err_rtos_timer:
      return "rtos_timer";
    case k_ra_err_comm_error:
      return "comm_error";
    case k_ra_err_spi_error:
      return "spi_error";
    case k_ra_err_uart_error:
      return "uart_error";
    case k_ra_err_i2c_error:
      return "i2c_error";
    case k_ra_err_crc_mismatch:
      return "crc_mismatch";
    case k_ra_err_protocol_error:
      return "protocol_error";
    case k_ra_err_nack:
      return "nack";
    case k_ra_err_conflict:
      return "conflict";
    case k_ra_err_retry_limit:
      return "retry_limit";
    case k_ra_err_validation_failed:
      return "validation_failed";
    case k_ra_err_checksum_mismatch:
      return "checksum_mismatch";
    case k_ra_err_range_check_failed:
      return "range_check_failed";
    case k_ra_err_null_ptr:
      return "null_ptr";
    default:
      return "unknown";
  }
}
