/**
 * @file ra8_err.h
 * @brief Error Code Definitions for ra8-firmware
 * @ingroup grp_core
 *
 * @details
 * Single source of truth for all error codes returned across the firmware.
 * Every public API that can fail returns `ra8_err_t`; every caller checks it
 * via the macros in `ra8_check.h` (`RA8_RETURN_ON_ERROR`, `RA8_ERROR_CHECK`,
 * etc.). There are no other error channels (no errno, no exceptions, no
 * longjmp, no global error flags) -- this is NASA Power of 10 Rule 7
 * ("check all return values") enforced at the type level.
 *
 * ## Design
 *
 * - **Single enum**: one `ra8_err_codes_t` with `uint16_t` underlying type
 *   holds every possible error, organised by category (0x1xx generic,
 *   0x2xx hardware, 0x3xx RTOS, 0x4xx communication, 0x5xx validation).
 * - **0 is success**: `k_ra8_ok == 0` so the idiom `if (err)` treats any
 *   non-zero value as a failure without requiring `!= k_ra8_ok`.
 * - **Stable numeric values**: values are explicit (`= 0x101` etc.) so
 *   remote logging, telemetry and debugger views can decode codes without
 *   needing the source.
 * - **C23 typed enum**: `typedef enum : uint16_t { ... }` pins the
 *   underlying type at 2 bytes, keeping struct layouts stable across
 *   compilers and host/target builds.
 *
 * ## Usage pattern
 *
 * @code{.c}
 * ra8_err_t ra8_sci_init(const ra8_sci_cfg_t* cfg)
 * {
 *     RA8_CHECK_NULL_PTR(cfg, "SCI", "cfg must not be nullptr");
 *     if (cfg->channel >= k_ra8_sci_channel_count) {
 *         return k_ra8_err_gpio_invalid_port;  // reusing a generic "out-of-range" code
 *     }
 *     if (g_initialized[cfg->channel]) {
 *         return k_ra8_err_exists;
 *     }
 *     // ... programme the peripheral ...
 *     g_initialized[cfg->channel] = true;
 *     return k_ra8_ok;
 * }
 * @endcode
 *
 * ## Category layout
 *
 * | Range          | Category               | Typical source                              |
 * |----------------|------------------------|---------------------------------------------|
 * | 0x000          | Success                | Normal return                               |
 * | 0x101 -- 0x1FF | Generic                | Argument/state/lifecycle                    |
 * | 0x201 -- 0x2FF | Hardware               | Peripheral init, GPIO, sensor range, faults |
 * | 0x301 -- 0x3FF | RTOS (reserved)        | Future RTOS (not used in v0.x bare-metal)   |
 * | 0x401 -- 0x4FF | Communication          | Bus-level errors, framing, CRC              |
 * | 0x501 -- 0x5FF | Validation             | Assertion and pre/post-condition failures   |
 *
 * The RTOS category is preserved (identical values to the rx72n project) so
 * that any shared utility code can be moved between projects without
 * renumbering. The bare-metal firmware does not currently return any
 * `k_ra8_err_rtos_*` code.
 *
 * ## NASA Power of 10 Compliance
 *
 * - **Rule 5**: error codes ARE the assertion vocabulary. Every check in
 *   `ra8_check.h` returns one of the values below.
 * - **Rule 7**: every API that can fail returns `ra8_err_t`. `[[nodiscard]]`
 *   is applied to the important ones so ignoring the return is a compile
 *   error, not a warning.
 * - **Rule 8**: uses a C23 typed enum (`: uint16_t`), not `#define`s.
 * - **Rule 10**: compiles clean with `-Wall -Wextra -Werror`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* =============================================================================
 * Error Code Enum
 * =============================================================================
 */

/**
 * @enum ra8_err_codes_t
 * @brief Canonical error code list for ra8-firmware
 *
 * @details
 * Every public API that can fail returns one of these values cast to
 * `ra8_err_t`. Values are grouped by category and use stable hexadecimal
 * numbering so logs, telemetry, and debugger watch expressions decode
 * without needing source.
 *
 * @invariant `k_ra8_ok == 0` (success is the only zero value).
 * @invariant All error values are within `uint16_t` range.
 * @invariant No two distinct error names share a value (except documented
 *            aliases such as `k_ra8_err_threadx`).
 *
 * @note This enum is pinned to `uint16_t`. Do NOT rely on implicit
 *       conversion to `int` in public API signatures; always use the
 *       typedef `ra8_err_t`.
 */
typedef enum : uint16_t {
  /* =========================================================================
   * Success
   * =========================================================================
   */

  /**
   * @brief Success -- operation completed with all postconditions satisfied.
   * @details The only non-error value. Callers should always compare against
   *          `k_ra8_ok` rather than the numeric 0 for clarity.
   */
  k_ra8_ok = 0,

  /* =========================================================================
   * Generic errors (0x100 -- 0x1FF)
   * General-purpose failures not tied to a specific peripheral or bus
   * =========================================================================
   */

  /**
   * @brief Generic unspecified failure.
   * @details Last-resort code when nothing more specific fits. Prefer a
   *          purpose-built code when one exists.
   * @par Value: 0x101
   */
  k_ra8_fail = 0x101,

  /**
   * @brief Static buffer exhausted (no dynamic memory on this project).
   * @details Returned when a statically-sized pool / ring / queue has no
   *          space left. In a zero-alloc system this almost always means
   *          the buffer was sized too small for the worst-case load.
   * @par Value: 0x102
   */
  k_ra8_err_no_mem = 0x102,

  /**
   * @brief Invalid function argument.
   * @details Parameter out of range, NULL where non-NULL required, or
   *          logically inconsistent inputs.
   * @par Value: 0x103
   * @see k_ra8_err_null_ptr           More specific: pointer was NULL
   * @see k_ra8_err_range_check_failed More specific: numeric out of range
   */
  k_ra8_err_invalid_arg = 0x103,

  /**
   * @brief Module in wrong state for requested operation.
   * @details E.g. calling `_start()` before `_init()` succeeded, or
   *          `_init()` twice in a row.
   * @par Value: 0x104
   * @see k_ra8_err_not_initialized More specific: init() never ran
   */
  k_ra8_err_invalid_state = 0x104,

  /**
   * @brief Invalid size parameter (too large, too small, or misaligned).
   * @par Value: 0x105
   */
  k_ra8_err_invalid_size = 0x105,

  /**
   * @brief Requested item not found (lookup / search missed).
   * @par Value: 0x106
   */
  k_ra8_err_not_found = 0x106,

  /**
   * @brief Requested feature not compiled in, not wired, or not supported
   *        by this MCU variant.
   * @par Value: 0x107
   */
  k_ra8_err_not_supported = 0x107,

  /**
   * @brief Operation exceeded its time budget.
   * @details Generic timeout. For hardware-specific waits (e.g. PLL lock,
   *          peripheral busy) prefer `k_ra8_err_hw_timeout`.
   * @par Value: 0x108
   */
  k_ra8_err_timeout = 0x108,

  /**
   * @brief Resource busy -- blocking operation cannot proceed.
   * @par Value: 0x109
   * @see k_ra8_err_would_block Non-blocking flavour
   */
  k_ra8_err_busy = 0x109,

  /**
   * @brief No application data available (e.g. only a control frame arrived).
   * @details Not a failure condition -- the caller should continue polling.
   * @par Value: 0x10A
   */
  k_ra8_err_no_data = 0x10A,

  /**
   * @brief Non-blocking operation would have blocked.
   * @details Returned by non-blocking APIs when the underlying resource
   *          is not immediately available.
   * @par Value: 0x10B
   */
  k_ra8_err_would_block = 0x10B,

  /**
   * @brief Item already exists -- cannot create again.
   * @par Value: 0x10C
   */
  k_ra8_err_exists = 0x10C,

  /**
   * @brief Container empty -- nothing to retrieve.
   * @par Value: 0x10D
   */
  k_ra8_err_empty = 0x10D,

  /**
   * @brief Operation cancelled before completion.
   * @details The operation had no side effects (atomic cancellation).
   * @par Value: 0x10E
   */
  k_ra8_err_cancelled = 0x10E,

  /**
   * @brief Module not initialized -- `_init()` not yet called successfully.
   * @par Value: 0x10F
   */
  k_ra8_err_not_initialized = 0x10F,

  /**
   * @brief Emergency stop active -- operation forbidden until cleared.
   * @details Safety-critical: do not bypass. Once set, requires explicit
   *          reset via the safety supervisor.
   * @par Value: 0x110
   * @warning This is a safety latch. Never return this code from a fast
   *          path without also driving the hardware into a safe state.
   */
  k_ra8_err_estop = 0x110,

  /**
   * @brief Container still holds members -- the operation requires it empty.
   * @details The exact inverse of `k_ra8_err_empty`: that one reports "nothing
   *          to retrieve", this one reports "something is still in there".
   *          Returned by `ra8_fs_rmdir()` for a directory that still has
   *          entries, the POSIX `ENOTEMPTY` condition. Distinct from
   *          `k_ra8_err_invalid_arg` on purpose: a caller that wants to remove
   *          a tree must be able to tell "you named the wrong thing" from
   *          "empty it first and retry".
   * @par Value: 0x111
   */
  k_ra8_err_not_empty = 0x111,

  /**
   * @brief Operation refused because the target is protected against it.
   * @details The POSIX `EACCES` condition, returned when a mutating request is
   *          denied by a permission the target itself carries rather than by a
   *          bad argument or a broken device. Its first use is the FAT/exFAT
   *          read-only attribute: `ra8_fs_open()` for writing,
   *          `ra8_fs_write_file()`, `ra8_fs_unlink()` and `ra8_fs_rename()`
   *          return it rather than overwrite or delete a file a host marked
   *          read-only. The bit is checked at OPEN time, so `ra8_fs_write()` on
   *          a handle opened before the file became read-only is unaffected.
   *          Distinct from `k_ra8_err_invalid_arg`, which says the
   *          request was malformed: here the request is well-formed and the
   *          answer is "not allowed".
   * @par Value: 0x112
   * @see k_ra8_err_invalid_arg  Malformed request, not a protected target.
   */
  k_ra8_err_access_denied = 0x112,

  /* =========================================================================
   * Hardware errors (0x200 -- 0x2FF)
   * Peripheral, GPIO, and hardware-interface errors
   * =========================================================================
   */

  /**
   * @brief Hardware peripheral failed to initialise.
   * @details Clock gating failed, register verify failed, or the block
   *          never reached ready state.
   * @par Value: 0x201
   */
  k_ra8_err_hw_init_failed = 0x201,

  /**
   * @brief Hardware peripheral exists but not ready yet.
   * @details Examples: PLL not locked, ADC not calibrated, LVD still
   *          below threshold.
   * @par Value: 0x202
   */
  k_ra8_err_hw_not_ready = 0x202,

  /**
   * @brief Hardware timed out waiting for a flag or handshake.
   * @par Value: 0x203
   */
  k_ra8_err_hw_timeout = 0x203,

  /**
   * @brief Generic hardware fault detected (error flag set, fault interrupt).
   * @par Value: 0x204
   */
  k_ra8_err_hw_error = 0x204,

  /**
   * @brief GPIO pin already owned by another peripheral or driver.
   * @details Raised by `ra8_pin_validator` when two modules both try to
   *          claim the same pin.
   * @par Value: 0x205
   */
  k_ra8_err_gpio_conflict = 0x205,

  /**
   * @brief GPIO port index out of range for this MCU.
   * @details Valid RA8D2 ports are 0..14 (not all pins bonded out on BGA 289).
   * @par Value: 0x206
   */
  k_ra8_err_gpio_invalid_port = 0x206,

  /**
   * @brief GPIO pin index out of range within its port (valid: 0..15).
   * @par Value: 0x207
   */
  k_ra8_err_gpio_invalid_pin = 0x207,

  /**
   * @brief Sensor or peripheral output out of valid range.
   * @par Value: 0x208
   */
  k_ra8_err_out_of_range = 0x208,

  /**
   * @brief Peripheral register block is not mapped in this MCU variant.
   * @details Raised when driver code is built for a chip that does not
   *          actually have the addressed peripheral.
   * @par Value: 0x209
   */
  k_ra8_err_hw_unmapped = 0x209,

  /* =========================================================================
   * RTOS errors (0x300 -- 0x3FF)
   * Reserved for the future RTOS. Unused in the bare-metal v0.x build.
   * =========================================================================
   */

  /**
   * @brief Generic RTOS error (reserved for future use).
   * @par Value: 0x301
   */
  k_ra8_err_rtos_error = 0x301,

  /**
   * @brief Thread create failed (reserved for future use).
   * @par Value: 0x302
   */
  k_ra8_err_rtos_thread_create = 0x302,

  /**
   * @brief Semaphore API error (reserved for future use).
   * @par Value: 0x303
   */
  k_ra8_err_rtos_semaphore = 0x303,

  /**
   * @brief Mutex API error (reserved for future use).
   * @par Value: 0x304
   */
  k_ra8_err_rtos_mutex = 0x304,

  /**
   * @brief Queue / message API error (reserved for future use).
   * @par Value: 0x305
   */
  k_ra8_err_rtos_queue = 0x305,

  /**
   * @brief Timer API error (reserved for future use).
   * @par Value: 0x306
   */
  k_ra8_err_rtos_timer = 0x306,

  /* =========================================================================
   * Communication errors (0x400 -- 0x4FF)
   * Bus-level and protocol-level failures
   * =========================================================================
   */

  /**
   * @brief Generic communication error (use a more specific code when possible).
   * @par Value: 0x401
   */
  k_ra8_err_comm_error = 0x401,

  /**
   * @brief SPI transfer failed (bus fault, mode fault, overrun, ...).
   * @par Value: 0x402
   */
  k_ra8_err_spi_error = 0x402,

  /**
   * @brief UART / SCI error (framing, parity, overrun, break).
   * @par Value: 0x403
   */
  k_ra8_err_uart_error = 0x403,

  /**
   * @brief I2C / IIC error (arbitration lost, NACK, timeout, bus error).
   * @par Value: 0x404
   */
  k_ra8_err_i2c_error = 0x404,

  /**
   * @brief CRC mismatch detected on received data.
   * @par Value: 0x405
   */
  k_ra8_err_crc_mismatch = 0x405,

  /**
   * @brief Protocol-level error (e.g. unexpected opcode, bad sequence number).
   * @par Value: 0x406
   */
  k_ra8_err_protocol_error = 0x406,

  /**
   * @brief Peer responded with NACK (negative acknowledgement).
   * @par Value: 0x407
   */
  k_ra8_err_nack = 0x407,

  /**
   * @brief Conflict with concurrent access detected.
   * @par Value: 0x408
   */
  k_ra8_err_conflict = 0x408,

  /**
   * @brief Retry budget exhausted -- operation still failing after all attempts.
   * @par Value: 0x409
   */
  k_ra8_err_retry_limit = 0x409,

  /* =========================================================================
   * Validation errors (0x500 -- 0x5FF)
   * Raised by ra8_check.h helpers and explicit pre/post-condition checks
   * =========================================================================
   */

  /**
   * @brief Validation rule failed (caller-supplied invariant not satisfied).
   * @par Value: 0x501
   */
  k_ra8_err_validation_failed = 0x501,

  /**
   * @brief Stored / transmitted checksum does not match computed value.
   * @par Value: 0x502
   */
  k_ra8_err_checksum_mismatch = 0x502,

  /**
   * @brief Value outside range enforced by `RA8_CHECK_RANGE` / `RA8_CHECK_RANGE_TAG`.
   * @par Value: 0x503
   */
  k_ra8_err_range_check_failed = 0x503,

  /**
   * @brief Pointer was NULL where a valid pointer was required.
   * @details This is the value returned by `RA8_CHECK_NULL_PTR`.
   * @par Value: 0x504
   */
  k_ra8_err_null_ptr = 0x504,

  /**
   * @brief Decompression output cap breached (`ra8_decomp_limits_t`).
   * @details A decode unit (one archive member or one wrapped stream) either
   *          declared or actually produced more bytes than the policy's
   *          `max_output_bytes`. The decoder stops fail-closed; nothing past
   *          the cap is written.
   * @par Value: 0x505
   * @see ra8_decomp_limits.h The unified decompression-limits policy.
   */
  k_ra8_err_decomp_output_cap = 0x505,

  /**
   * @brief Compression ratio bound breached (`ra8_decomp_limits_t`).
   * @details A decode unit's output exceeded `input * max_ratio +
   *          ratio_grace_bytes` -- the decompression-bomb signature. The
   *          decoder stops fail-closed at the breach point.
   * @par Value: 0x506
   * @see ra8_decomp_limits.h The unified decompression-limits policy.
   */
  k_ra8_err_decomp_ratio = 0x506,

  /**
   * @brief Archive entry-count cap breached (`ra8_decomp_limits_t`).
   * @details An archive enumerated more members than the policy's
   *          `max_entries` -- the many-tiny-entries resource-exhaustion
   *          shape. The whole archive is rejected fail-closed.
   * @par Value: 0x507
   * @see ra8_decomp_limits.h The unified decompression-limits policy.
   */
  k_ra8_err_decomp_entries = 0x507,

  /**
   * @brief Container nesting-depth cap breached (`ra8_decomp_limits_t`).
   * @details Decoder plumbing was asked to stack more layers (e.g. a
   *          compressed stream inside a compressed stream) than the policy's
   *          `max_depth` -- the recursive-bomb shape. Rejected fail-closed
   *          before any inner decode starts.
   * @par Value: 0x508
   * @see ra8_decomp_limits.h The unified decompression-limits policy.
   */
  k_ra8_err_decomp_depth = 0x508,

  /**
   * @brief Decode-loop iteration budget exhausted (`ra8_decomp_limits_t`).
   * @details A decode loop charged more iterations than the policy's
   *          `max_iterations` without finishing -- the stuck-stream /
   *          no-progress shape (NASA P10 Rule 2 backstop). The decoder
   *          stops fail-closed.
   * @par Value: 0x509
   * @see ra8_decomp_limits.h The unified decompression-limits policy.
   */
  k_ra8_err_decomp_iterations = 0x509,
} ra8_err_codes_t;

/**
 * @typedef ra8_err_t
 * @brief Canonical error-return type used by every ra8-firmware API.
 *
 * @details
 * Always returns a value from `ra8_err_codes_t`. Use the helpers in
 * `ra8_check.h` to propagate and check.
 *
 * @note Defined as a distinct typedef rather than using the enum name
 *       directly so the error-code set can be extended without touching
 *       every function signature in the tree.
 */
typedef ra8_err_codes_t ra8_err_t;

/* =============================================================================
 * Helpers
 * =============================================================================
 */

/**
 * @brief Test whether a code represents failure.
 * @param[in] err Code to test.
 * @return `true` if `err != k_ra8_ok`, `false` otherwise.
 * @note Inline, zero overhead. Provided so call sites can read as
 *       `if (ra8_err_is_error(err))` instead of `if (err != k_ra8_ok)`.
 *
 * @details See implementation for details.
 * @retval 0 Success or default value.
 * @pre Module has been initialized.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @since 0.1.0
 */
static inline bool ra8_err_is_error(ra8_err_t err)
{
  return err != k_ra8_ok;
}

/**
 * @brief Human-readable string for an error code.
 *
 * @details
 * Returns a pointer to a static literal describing the code. Useful for
 * logging and UART dumps. The returned pointer is valid for the lifetime
 * of the program and must NOT be freed.
 *
 * @param[in] err Code to look up.
 * @return Pointer to a static C string. Never NULL; an unknown code
 *         returns the literal `"unknown"`.
 * @retval "ok"            Returned for `k_ra8_ok`.
 * @retval "<name>"        Returned for any known `k_ra8_err_*` code.
 * @retval "unknown"       Returned for any code not in the lookup table.
 *
 * @pre None -- pure lookup; safe to call before init.
 * @pre Module-level lookup table `s_ra8_err_names` is fully populated.
 * @post No internal state is modified.
 * @post Returned pointer remains valid for the program lifetime.
 *
 * @note Implementation lives in `ra8_err.c`. Declared `[[nodiscard]]`-free
 *       intentionally: it is common to pass the result straight into a
 *       variadic log macro. Thread-safe -- pure read of `.rodata`.
 *
 * @since 0.1.0
 */
const char* ra8_err_to_str(ra8_err_t err);

#ifdef __cplusplus
}
#endif
