/**
 * @file ra8_log.c
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
 * in later by overriding the `internal_ra8_log_*` functions with
 * non-weak definitions elsewhere in the project.
 *
 * @note Format: `"[TAG] level: message"` followed by a newline. The
 *       `*_val` variants append `" = <decimal>"`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_log.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_scb.h"

/* =============================================================================
 * Optional byte sink -- redirect log output off ITM at run time.
 * =============================================================================
 */

/**
 * @var s_byte_sink
 * @brief Active log byte sink, or nullptr to use the default ITM port.
 * @warning Set only through ::ra8_log_set_byte_sink.
 */
static ra8_log_byte_sink_fn_t s_byte_sink = nullptr;

/**
 * @var s_byte_sink_ctx
 * @brief Opaque cookie passed back to ::s_byte_sink on every byte.
 * @warning Set only through ::ra8_log_set_byte_sink.
 */
static void* s_byte_sink_ctx = nullptr;

void ra8_log_set_byte_sink(ra8_log_byte_sink_fn_t fn, void* ctx)
{
  s_byte_sink     = fn;
  s_byte_sink_ctx = ctx;
}

/* =============================================================================
 * ITM stimulus port 0 -- CoreSight ITM (Cortex-M85 has one).
 * =============================================================================
 */

typedef enum : uintptr_t {
  k_ra8_itm_stim_base = 0xE0000000UL, /**< ITM stimulus port 0 base.   */
  k_ra8_itm_tcr_addr  = 0xE0000E80UL, /**< ITM Trace Control Register. */
  k_ra8_itm_tenr_addr = 0xE0000E00UL, /**< ITM Trace Enable  Register. */
} ra8_itm_addr_t;

/**
 * @brief Get the ITM stimulus-port-0 register.
 *
 * @details Trivial address-cast helper. Always inlined.
 *
 * @return Volatile pointer to the 32-bit stimulus FIFO.
 * @retval (volatile uint32_t*)k_ra8_itm_stim_base
 *
 * @pre None.
 * @pre Underlying MMIO is mapped (always true on Cortex-M85).
 * @post No state modified.
 * @post Returned pointer remains valid for the program lifetime.
 *
 * @note Trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_HW_REGISTER_ACCESS static inline volatile uint32_t* internal_itm_stim0(void)
{
  return (volatile uint32_t*)k_ra8_itm_stim_base;
}

/**
 * @brief Get the ITM Trace Control Register.
 *
 * @details Trivial address-cast helper for the ITM TCR.
 *
 * @return Volatile pointer to TCR.
 * @retval (volatile uint32_t*)k_ra8_itm_tcr_addr
 *
 * @pre None.
 * @pre Underlying MMIO is mapped.
 * @post No state modified.
 * @post Returned pointer remains valid for the program lifetime.
 *
 * @note Trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_HW_REGISTER_ACCESS static inline volatile uint32_t* internal_itm_tcr(void)
{
  return (volatile uint32_t*)k_ra8_itm_tcr_addr;
}

/**
 * @brief Get the ITM Trace Enable Register.
 *
 * @details Trivial address-cast helper for the ITM TENR.
 *
 * @return Volatile pointer to TENR.
 * @retval (volatile uint32_t*)k_ra8_itm_tenr_addr
 *
 * @pre None.
 * @pre Underlying MMIO is mapped.
 * @post No state modified.
 * @post Returned pointer remains valid for the program lifetime.
 *
 * @note Trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_HW_REGISTER_ACCESS static inline volatile uint32_t* internal_itm_tenr(void)
{
  return (volatile uint32_t*)k_ra8_itm_tenr_addr;
}

/**
 * @brief Check whether ITM port 0 is enabled and ready to accept bytes.
 *
 * @details
 * Reads TCR, TENR, and the STIM0 FIFO register directly. On the
 * target these are the real Cortex-M85 ITM registers. On the
 * `RA8_OFF_TARGET` host test build the same virtual addresses are
 * backed by anonymous RAM via `ra8_fake_mmap.c`.
 *
 * @return `true` if enabled and not full, `false` otherwise.
 * @retval true   ITMENA, port-0 enable, and FIFO-ready bits all set.
 * @retval false  Any of the above is clear.
 *
 * @pre None -- pure read of architectural registers.
 * @pre On the fake, `ra8_fake_mmap.c` has mapped the SCS region.
 * @post No state is modified.
 * @post Result reflects the registers at the moment of the call.
 *
 * @note Thread-safe (read-only). Always inlined.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static inline bool internal_itm_ready(void)
{
  /* A redirected byte sink needs no ITM/debugger -- it is always ready. */
  if (s_byte_sink != nullptr) {
    return true;
  }
#ifndef RA8_OFF_TARGET
  /* Hardening: if DEMCR.TRCENA is clear the ITM block is powered down
   * and any read of its registers (TCR, TENR, STIM) will bus-fault.
   * Pre-check TRCENA via the shared ra8_scb primitive (DEMCR is always
   * accessible) before touching ITM.
   *
   * This was the root cause of escalation-to-LOCKUP observed during
   * USB bring-up: the original USB fault entered the fault handler,
   * which called this function, which read ITM_TCR with TRCENA=0 ->
   * second fault -> LOCKUP at PC=0xEFFFFFFE, hiding the real PC. */
  if (!ra8_scb_trace_enabled()) {
    return false;
  }
  /* Additional belt-and-braces: never poke ITM from a fault context.
   * IPSR != 0 means we are inside an exception handler. Even if
   * TRCENA happens to be on, dropping log lines is preferable to a
   * second fault that masks the original PC. */
  /* cppcheck-suppress unreadVariable -- the inline asm mrs writes IPSR into the variable; cppcheck cannot see through the asm. */
  /* cppcheck-suppress knownConditionTrueFalse -- cppcheck assumes the asm-written IPSR value stays 0. */
  volatile uint32_t ipsr = 0U;
  __asm__ volatile("mrs %0, ipsr" : "=r"(ipsr));
  if (ipsr != 0U) {
    return false;
  }
#endif
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
 *
 * @details Polls the FIFO-ready bit up to `k_ra8_itm_poll_limit` times,
 *          then either writes the byte or gives up silently.
 *
 * @param[in] byte Character to emit.
 *
 * @pre None -- caller does not need to pre-check ready state.
 * @pre Underlying MMIO is mapped.
 * @post Either the byte has been pushed into the FIFO or the poll
 *       budget has been exhausted (drop).
 * @post No other state modified.
 *
 * @note Always inlined. Thread-safety is a backend concern.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_itm_putc(uint8_t byte)
{
  if (s_byte_sink != nullptr) {
    s_byte_sink(s_byte_sink_ctx, byte);
    return;
  }
  /* A real build should pre-check `internal_itm_ready()` once during
   * init and skip the call entirely when the debugger is absent. For
   * now we poll with a small bound so a disconnected debugger never
   * stalls the firmware. */
  enum : uint32_t {
    k_ra8_itm_poll_limit = 1000U, /**< RA8 itm poll limit. */
  };
  for (uint32_t i = 0U; i < k_ra8_itm_poll_limit; i++) {
    if (*internal_itm_stim0() != 0U) {
      *(volatile uint8_t*)k_ra8_itm_stim_base = byte;
      return;
    }
  }
}

/**
 * @brief Emit a NUL-terminated string over ITM port 0.
 *
 * @details Walks `s` and forwards each byte through `internal_itm_putc`.
 *
 * @param[in] s String to emit (must be non-NULL).
 *
 * @pre `s` is non-NULL and NUL-terminated.
 * @pre Backend FIFO is reachable -- otherwise bytes are dropped.
 * @post All bytes up to (excluding) the NUL have been pushed (or dropped).
 * @post No internal state modified.
 *
 * @note Always inlined. Not thread-safe across multiple writers.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_itm_puts(const char* s)
{
  while (*s != '\0') {
    internal_itm_putc((uint8_t)*s++);
  }
}

/**
 * @brief Emit an unsigned decimal integer over ITM port 0.
 *
 * @details Hand-rolled itoa for `uint32_t`. Avoids `snprintf` (which
 *          would drag in vararg + nano stdio).
 *
 * @param[in] value Value to emit.
 *
 * @return None.
 * @retval None
 *
 * @pre Underlying MMIO is reachable -- otherwise digits are dropped.
 * @pre Caller has already emitted any prefix it wants.
 * @post Decimal digits of `value` (MSB-first) have been pushed.
 * @post No state modified.
 *
 * @note Always inlined. Bounded loop, NASA Rule 2 compliant.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_u32_max_digits = 10U, /**< Max decimal digits in a uint32_t. */
  k_ra8_decimal_base   = 10U, /**< Decimal radix.                    */
} ra8_log_u32_t;

/* Itm put u32 -- see implementation for details. */
RA8_INTERNAL static inline void internal_itm_put_u32(uint32_t value)
{
  char    buf[k_ra8_u32_max_digits + 1U] = {};
  uint8_t i                              = 0U;
  if (value == 0U) {
    internal_itm_putc('0');
    return;
  }
  /* mcdc-deactivated: digit-buffer bound; uint32_t max is 10 digits (k_ra8_u32_max_digits == 10), so the loop always terminates by `value == 0` first; the bound is a defensive watchdog. */
  while (value != 0U && i < k_ra8_u32_max_digits) {
    buf[i++] = (char)('0' + (char)(value % (uint32_t)k_ra8_decimal_base));
    value /= (uint32_t)k_ra8_decimal_base;
  }
  /* Digits were pushed LSB-first; emit MSB-first. */
  while (i > 0U) {
    i--;
    internal_itm_putc((uint8_t)buf[i]);
  }
}

/**
 * @brief Emit a signed decimal integer over ITM port 0.
 *
 * @details Emits a leading `-` for negative values then forwards to
 *          `internal_itm_put_u32`. Negation goes through `int64_t` to
 *          avoid signed-overflow UB at `INT32_MIN`.
 *
 * @param[in] value Value to emit.
 *
 * @pre Underlying MMIO is reachable -- otherwise dropped.
 * @pre Caller has already emitted any prefix it wants.
 * @post Sign + decimal digits have been pushed.
 * @post No state modified.
 *
 * @note Always inlined.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_itm_put_i32(int32_t value)
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

/**
 * @brief Default log backend init -- no-op for the ITM sink.
 *
 * @details ITM is enabled by the debugger when it attaches; the
 *          firmware does not need to programme any control registers.
 *
 * @pre Called exactly once during early boot.
 * @pre Build either has a debugger attached or is the off-target host.
 * @post Subsequent log calls follow the ITM emit path.
 * @post No state modified -- the function is intentionally empty.
 *
 * @note Thread-safety irrelevant -- one-shot init only.
 *
 * @since 0.1.0
 */
void ra8_log_init(void)
{
  /* ITM is set up by the debugger when it attaches. Nothing to do here
   * in the default backend. A UART-backed override would configure its
   * SCI channel in its own ra8_log_init(). */
}

/**
 * @brief Common tagged-line emit helper.
 *
 * @details Writes `[tag] level: msg\r\n`. Bails out early if the ITM
 *          backend is not ready.
 *
 * @param[in] level String like `"ERROR"` or `"INFO"`. Must be non-NULL.
 * @param[in] tag   Component tag. Must be non-NULL.
 * @param[in] msg   Free-form message. Must be non-NULL.
 *
 * @pre All three string parameters are non-NULL and NUL-terminated.
 * @pre `ra8_log_init()` has run (otherwise dropped).
 * @post One log line is emitted (or dropped).
 * @post No internal state modified.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_emit_line(const char* level, const char* tag, const char* msg)
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
 *
 * @details Writes `[tag] level: msg=<decimal>\r\n`.
 *
 * @param[in] level String like `"ERROR"`. Must be non-NULL.
 * @param[in] tag   Component tag. Must be non-NULL.
 * @param[in] msg   Free-form message. Must be non-NULL.
 * @param[in] value Unsigned numeric companion value.
 *
 * @pre All string parameters are non-NULL and NUL-terminated.
 * @pre `ra8_log_init()` has run (otherwise dropped).
 * @post One log line is emitted (or dropped).
 * @post No internal state modified.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void
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
 *
 * @details Writes `[tag] level: msg=<signed-decimal>\r\n`.
 *
 * @param[in] level String like `"DEBUG"`. Must be non-NULL.
 * @param[in] tag   Component tag. Must be non-NULL.
 * @param[in] msg   Free-form message. Must be non-NULL.
 * @param[in] value Signed numeric companion value.
 *
 * @pre All string parameters are non-NULL and NUL-terminated.
 * @pre `ra8_log_init()` has run (otherwise dropped).
 * @post One log line is emitted (or dropped).
 * @post No internal state modified.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_emit_line_i(const char* level, const char* tag, const char* msg, int32_t value)
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

[[gnu::weak]] void internal_ra8_log_error(const char* tag, const char* message)
{
  internal_emit_line("ERROR", tag, message);
}

[[gnu::weak]] void internal_ra8_log_warn(const char* tag, const char* message)
{
  internal_emit_line("WARN", tag, message);
}

[[gnu::weak]] void internal_ra8_log_info(const char* tag, const char* message)
{
  internal_emit_line("INFO", tag, message);
}

[[gnu::weak]] void internal_ra8_log_debug(const char* tag, const char* message)
{
  internal_emit_line("DEBUG", tag, message);
}

/* ---- string + value log ------------------------------------------------- */

/**
 * @brief ERROR-level log emitter that also reports a uint32 value.
 * @details Weak default backend. Forwards to ::internal_emit_line_u with the
 *          fixed level string "ERROR".
 * @param[in] tag     NUL-terminated module tag (e.g. "ra8_log").
 * @param[in] message NUL-terminated short event description.
 * @param[in] value   uint32 datum appended as "=NNN" to the line.
 * @pre tag and message are non-NULL, NUL-terminated.
 * @pre ra8_log_init() has run (otherwise the line is silently dropped).
 * @post One line is emitted to the log backend (or dropped if backend down).
 * @post No internal logger state is modified.
 * @note Thread-safety inherited from the active backend (ITM by default).
 * @since 0.1.0
 */
[[gnu::weak]] void internal_ra8_log_error_val(const char* tag, const char* message, uint32_t value)
{
  internal_emit_line_u("ERROR", tag, message, value);
}

/**
 * @brief WARN-level log emitter that also reports a uint32 value.
 * @details Weak default backend. Forwards to ::internal_emit_line_u with the
 *          fixed level string "WARN".
 * @param[in] tag     NUL-terminated module tag.
 * @param[in] message NUL-terminated short event description.
 * @param[in] value   uint32 datum appended as "=NNN" to the line.
 * @pre tag and message are non-NULL, NUL-terminated.
 * @pre ra8_log_init() has run.
 * @post One line is emitted (or dropped).
 * @post No internal logger state is modified.
 * @note Thread-safety inherited from the active backend.
 * @since 0.1.0
 */
[[gnu::weak]] void internal_ra8_log_warn_val(const char* tag, const char* message, uint32_t value)
{
  internal_emit_line_u("WARN", tag, message, value);
}

/**
 * @brief INFO-level log emitter that also reports a uint32 value.
 * @details Weak default backend. Forwards to ::internal_emit_line_u with the
 *          fixed level string "INFO".
 * @param[in] tag     NUL-terminated module tag.
 * @param[in] message NUL-terminated short event description.
 * @param[in] value   uint32 datum appended as "=NNN" to the line.
 * @pre tag and message are non-NULL, NUL-terminated.
 * @pre ra8_log_init() has run.
 * @post One line is emitted (or dropped).
 * @post No internal logger state is modified.
 * @note Thread-safety inherited from the active backend.
 * @since 0.1.0
 */
[[gnu::weak]] void internal_ra8_log_info_val(const char* tag, const char* message, uint32_t value)
{
  internal_emit_line_u("INFO", tag, message, value);
}

/**
 * @brief DEBUG-level log emitter that also reports a signed int32 value.
 * @details Weak default backend. Forwards to ::internal_emit_line_i with the
 *          fixed level string "DEBUG".
 * @param[in] tag     NUL-terminated module tag.
 * @param[in] message NUL-terminated short event description.
 * @param[in] value   int32 datum appended as "=NNN" (sign preserved).
 * @pre tag and message are non-NULL, NUL-terminated.
 * @pre ra8_log_init() has run.
 * @post One line is emitted (or dropped).
 * @post No internal logger state is modified.
 * @note Thread-safety inherited from the active backend.
 * @since 0.1.0
 */
[[gnu::weak]] void internal_ra8_log_debug_val(const char* tag, const char* message, int32_t value)
{
  internal_emit_line_i("DEBUG", tag, message, value);
}

/* =============================================================================
 * ra8_err_to_str (lives here because the log backend is the only user)
 * =============================================================================
 */

#include "ra8_err.h"

/**
 * @struct ra8_err_name_entry_t
 * @brief Single (code, name) pair in the ra8_err_to_str() lookup table.
 *
 * @details
 * The error codes are sparse (0, 0x101..0x10F, 0x201..0x209, 0x301..0x306,
 * 0x401..0x409, 0x501..0x509), so a directly indexed array would waste
 * ~1.2 KB of flash. A linear-scan {code, name} table keeps the table size
 * proportional to the number of codes (~44 entries).
 *
 * @invariant Every entry's `name` is a NUL-terminated string literal in
 *            .rodata (no dynamic strings).
 *
 * @since 0.1.0
 */
typedef struct {
  ra8_err_t   code; /**< Error code value from `ra8_err_t`.         */
  const char* name; /**< Short ASCII name for the code (no spaces). */
} ra8_err_name_entry_t;

/**
 * @var s_ra8_err_names
 * @brief Lookup table backing ra8_err_to_str().
 *
 * @details
 * One row per `k_ra8_err_*` value defined in `ra8_err.h`. The table lives
 * in `.rodata` (static const) and is scanned linearly by ra8_err_to_str().
 * Adding a new error code is a one-line addition here -- no switch arm
 * to update.
 *
 * @note The table order is informational only; ra8_err_to_str() does
 *       linear search so any order works. Keeping it in the same order
 *       as `ra8_err_t` makes diff review easier.
 *
 * @warning When a new code is added to `ra8_err_t`, this table MUST be
 *          extended in the same commit. The unit test
 *          `tests/test_ra8_log.c` walks every code so a missing entry
 *          fails CI.
 *
 * @since 0.1.0
 */
static const ra8_err_name_entry_t s_ra8_err_names[] = {
  {k_ra8_ok, "ok"},
  {k_ra8_fail, "fail"},
  {k_ra8_err_no_mem, "no_mem"},
  {k_ra8_err_invalid_arg, "invalid_arg"},
  {k_ra8_err_invalid_state, "invalid_state"},
  {k_ra8_err_invalid_size, "invalid_size"},
  {k_ra8_err_not_found, "not_found"},
  {k_ra8_err_not_supported, "not_supported"},
  {k_ra8_err_timeout, "timeout"},
  {k_ra8_err_busy, "busy"},
  {k_ra8_err_no_data, "no_data"},
  {k_ra8_err_would_block, "would_block"},
  {k_ra8_err_exists, "exists"},
  {k_ra8_err_empty, "empty"},
  {k_ra8_err_cancelled, "cancelled"},
  {k_ra8_err_not_initialized, "not_initialized"},
  {k_ra8_err_estop, "estop"},
  {k_ra8_err_hw_init_failed, "hw_init_failed"},
  {k_ra8_err_hw_not_ready, "hw_not_ready"},
  {k_ra8_err_hw_timeout, "hw_timeout"},
  {k_ra8_err_hw_error, "hw_error"},
  {k_ra8_err_gpio_conflict, "gpio_conflict"},
  {k_ra8_err_gpio_invalid_port, "gpio_invalid_port"},
  {k_ra8_err_gpio_invalid_pin, "gpio_invalid_pin"},
  {k_ra8_err_out_of_range, "out_of_range"},
  {k_ra8_err_hw_unmapped, "hw_unmapped"},
  {k_ra8_err_rtos_error, "rtos_error"},
  {k_ra8_err_rtos_thread_create, "rtos_thread_create"},
  {k_ra8_err_rtos_semaphore, "rtos_semaphore"},
  {k_ra8_err_rtos_mutex, "rtos_mutex"},
  {k_ra8_err_rtos_queue, "rtos_queue"},
  {k_ra8_err_rtos_timer, "rtos_timer"},
  {k_ra8_err_comm_error, "comm_error"},
  {k_ra8_err_spi_error, "spi_error"},
  {k_ra8_err_uart_error, "uart_error"},
  {k_ra8_err_i2c_error, "i2c_error"},
  {k_ra8_err_crc_mismatch, "crc_mismatch"},
  {k_ra8_err_protocol_error, "protocol_error"},
  {k_ra8_err_nack, "nack"},
  {k_ra8_err_conflict, "conflict"},
  {k_ra8_err_retry_limit, "retry_limit"},
  {k_ra8_err_validation_failed, "validation_failed"},
  {k_ra8_err_checksum_mismatch, "checksum_mismatch"},
  {k_ra8_err_range_check_failed, "range_check_failed"},
  {k_ra8_err_null_ptr, "null_ptr"},
  {k_ra8_err_decomp_output_cap, "decomp_output_cap"},
  {k_ra8_err_decomp_ratio, "decomp_ratio"},
  {k_ra8_err_decomp_entries, "decomp_entries"},
  {k_ra8_err_decomp_depth, "decomp_depth"},
  {k_ra8_err_decomp_iterations, "decomp_iterations"},
};

/**
 * @var k_ra8_err_names_count
 * @brief Number of entries in s_ra8_err_names.
 *
 * @details Computed at compile time from `sizeof` so the table and
 *          loop bound stay in sync.
 *
 * @since 0.1.0
 */
static const uint32_t k_ra8_err_names_count =
  (uint32_t)(sizeof(s_ra8_err_names) / sizeof(s_ra8_err_names[0]));

/**
 * @brief Implementation of `ra8_err_to_str()` -- linear-scan lookup.
 *
 * @details Walks `s_ra8_err_names` and returns the matching `name`.
 *          Returns the literal `"unknown"` if no entry matches.
 *
 * @param[in] err Error code to look up.
 *
 * @return Pointer to a static C string. Never NULL.
 * @retval "ok"        Returned for `k_ra8_ok`.
 * @retval "<name>"    Returned for any known `k_ra8_err_*` code.
 * @retval "unknown"   Returned for any code not in the table.
 *
 * @pre None -- pure lookup.
 * @pre Module-level table is populated at compile time.
 * @post No state modified.
 * @post Returned pointer remains valid for the program lifetime.
 *
 * @note Thread-safe (read-only walk of `.rodata`).
 *
 * @since 0.1.0
 */
const char* ra8_err_to_str(ra8_err_t err)
{
  for (uint32_t i = 0; i < k_ra8_err_names_count; i++) {
    if (s_ra8_err_names[i].code == err) {
      return s_ra8_err_names[i].name;
    }
  }
  return "unknown";
}
