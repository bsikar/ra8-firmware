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

/**
 * @struct ra_err_name_entry_t
 * @brief Single (code, name) pair in the ra_err_to_str() lookup table.
 *
 * @details
 * The error codes are sparse (0, 0x101..0x10F, 0x201..0x209, 0x301..0x306,
 * 0x401..0x409, 0x501..0x504), so a directly indexed array would waste
 * ~1.2 KB of flash. A linear-scan {code, name} table keeps the table size
 * proportional to the number of codes (~44 entries).
 *
 * @invariant Every entry's `name` is a NUL-terminated string literal in
 *            .rodata (no dynamic strings).
 *
 * @since 0.1.0
 */
typedef struct {
  ra_err_t    code; /**< Error code value from `ra_err_t`. */
  const char* name; /**< Short ASCII name for the code (no spaces). */
} ra_err_name_entry_t;

/**
 * @var s_ra_err_names
 * @brief Lookup table backing ra_err_to_str().
 *
 * @details
 * One row per `k_ra_err_*` value defined in `ra_err.h`. The table lives
 * in `.rodata` (static const) and is scanned linearly by ra_err_to_str().
 * Adding a new error code is a one-line addition here -- no switch arm
 * to update.
 *
 * @note The table order is informational only; ra_err_to_str() does
 *       linear search so any order works. Keeping it in the same order
 *       as `ra_err_t` makes diff review easier.
 *
 * @warning When a new code is added to `ra_err_t`, this table MUST be
 *          extended in the same commit. The unit test
 *          `tests/test_ra_log.c` walks every code so a missing entry
 *          fails CI.
 *
 * @since 0.1.0
 */
static const ra_err_name_entry_t s_ra_err_names[] = {
  {k_ra_ok, "ok"},
  {k_ra_fail, "fail"},
  {k_ra_err_no_mem, "no_mem"},
  {k_ra_err_invalid_arg, "invalid_arg"},
  {k_ra_err_invalid_state, "invalid_state"},
  {k_ra_err_invalid_size, "invalid_size"},
  {k_ra_err_not_found, "not_found"},
  {k_ra_err_not_supported, "not_supported"},
  {k_ra_err_timeout, "timeout"},
  {k_ra_err_busy, "busy"},
  {k_ra_err_no_data, "no_data"},
  {k_ra_err_would_block, "would_block"},
  {k_ra_err_exists, "exists"},
  {k_ra_err_empty, "empty"},
  {k_ra_err_cancelled, "cancelled"},
  {k_ra_err_not_initialized, "not_initialized"},
  {k_ra_err_estop, "estop"},
  {k_ra_err_hw_init_failed, "hw_init_failed"},
  {k_ra_err_hw_not_ready, "hw_not_ready"},
  {k_ra_err_hw_timeout, "hw_timeout"},
  {k_ra_err_hw_error, "hw_error"},
  {k_ra_err_gpio_conflict, "gpio_conflict"},
  {k_ra_err_gpio_invalid_port, "gpio_invalid_port"},
  {k_ra_err_gpio_invalid_pin, "gpio_invalid_pin"},
  {k_ra_err_out_of_range, "out_of_range"},
  {k_ra_err_hw_unmapped, "hw_unmapped"},
  {k_ra_err_rtos_error, "rtos_error"},
  {k_ra_err_rtos_thread_create, "rtos_thread_create"},
  {k_ra_err_rtos_semaphore, "rtos_semaphore"},
  {k_ra_err_rtos_mutex, "rtos_mutex"},
  {k_ra_err_rtos_queue, "rtos_queue"},
  {k_ra_err_rtos_timer, "rtos_timer"},
  {k_ra_err_comm_error, "comm_error"},
  {k_ra_err_spi_error, "spi_error"},
  {k_ra_err_uart_error, "uart_error"},
  {k_ra_err_i2c_error, "i2c_error"},
  {k_ra_err_crc_mismatch, "crc_mismatch"},
  {k_ra_err_protocol_error, "protocol_error"},
  {k_ra_err_nack, "nack"},
  {k_ra_err_conflict, "conflict"},
  {k_ra_err_retry_limit, "retry_limit"},
  {k_ra_err_validation_failed, "validation_failed"},
  {k_ra_err_checksum_mismatch, "checksum_mismatch"},
  {k_ra_err_range_check_failed, "range_check_failed"},
  {k_ra_err_null_ptr, "null_ptr"},
};

/**
 * @var k_ra_err_names_count
 * @brief Number of entries in s_ra_err_names.
 *
 * @details Computed at compile time from `sizeof` so the table and
 *          loop bound stay in sync.
 *
 * @since 0.1.0
 */
static const uint32_t k_ra_err_names_count =
  (uint32_t)(sizeof(s_ra_err_names) / sizeof(s_ra_err_names[0]));

const char* ra_err_to_str(ra_err_t err)
{
  for (uint32_t i = 0; i < k_ra_err_names_count; i++) {
    if (s_ra_err_names[i].code == err) {
      return s_ra_err_names[i].name;
    }
  }
  return "unknown";
}
