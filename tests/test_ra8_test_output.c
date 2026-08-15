/**
 * @file test_ra8_test_output.c
 * @brief Focused contract tests for the bounded host-test output sink
 * @details Exercises injected positive-short retries, zero-progress and error
 * latching, typed formatting, exact raw-pipe writes, and broken-pipe errors.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "ra8_attributes.h"
#include "support/ra8_test_output.h"

/** @brief Focused sink-test dimensions and values. */
typedef enum : size_t {
  k_test_output_capacity    = 256U,       /**< RAM sink capacity.            */
  k_test_output_short_chunk = 2U,         /**< Forced positive-short count.  */
  k_test_output_short_calls = 3U,         /**< Calls required for six bytes. */
  k_test_output_hex_value   = 0x123ABCDU, /**< Padded uppercase hex fixture. */
  k_test_output_hex_width   = 8U,         /**< Hex fixture width.            */
  k_test_output_fixed_value = 1234U,      /**< Fixed fixture: 12.34.         */
} test_output_limit_t;

/** @brief Caller-owned injected RAM sink state. */
typedef struct {
  uint8_t bytes[k_test_output_capacity]; /**< Accepted output bytes.        */
  size_t  length;                        /**< Accepted byte count.          */
  size_t  max_chunk;                     /**< Positive-short limit.         */
  size_t  calls;                         /**< Callback invocation count.    */
  size_t  fail_call;                     /**< Call that reports EIO.        */
  bool    zero_progress;                 /**< Report success with no bytes. */
} test_ram_sink_t;

/**
 * @brief Inject a bounded RAM write with configurable short/error behavior.
 * @details Applies the fixture's positive-short, zero-progress, and fail-call
 * controls before appending accepted bytes to fixed caller-owned storage.
 * @param[in,out] context ::test_ram_sink_t under test.
 * @param[in] bytes Source bytes.
 * @param[in] length Requested byte count.
 * @return Accepted count or injected error.
 * @retval k_ra8_test_output_ok In `status` after accepting progress.
 * @retval k_ra8_test_output_error In `status` on the configured failing call.
 * @pre @p context and @p bytes are valid for a nonzero request.
 * @pre The sink's current length does not exceed its fixed capacity.
 * @post Accepted bytes are appended within the fixed capacity.
 * @post Configured error and zero-progress calls do not mutate RAM bytes.
 * @note A configured maximum chunk deliberately exercises positive short writes.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_test_output_result_t
internal_ram_write(void* context, const uint8_t* bytes, size_t length)
{
  test_ram_sink_t* ram = (test_ram_sink_t*)context;
  ram->calls++;
  if ((ram->fail_call != 0U) && (ram->calls == ram->fail_call)) {
    return (ra8_test_output_result_t){.status = k_ra8_test_output_error, .os_error = EIO};
  }
  if (ram->zero_progress) {
    return (ra8_test_output_result_t){.status = k_ra8_test_output_ok};
  }
  size_t accepted = length;
  if ((ram->max_chunk != 0U) && (accepted > ram->max_chunk)) {
    accepted = ram->max_chunk;
  }
  const size_t available = sizeof(ram->bytes) - ram->length;
  if (accepted > available) {
    accepted = available;
  }
  if (accepted == 0U) {
    return (ra8_test_output_result_t){.status = k_ra8_test_output_short};
  }
  (void)memcpy(&ram->bytes[ram->length], bytes, accepted);
  ram->length += accepted;
  return (ra8_test_output_result_t){.status = k_ra8_test_output_ok, .accepted = accepted};
}

/**
 * @brief Verify positive-short retries plus zero-progress and error latches.
 * @details Drives one successful multi-call write and two independent failure
 * modes through fresh caller-owned handles.
 * @return Whether every injected contract result matched its golden.
 * @retval true All positive-short and failure-latch checks passed.
 * @retval false One callback count, byte span, status, or error differed.
 * @pre ::internal_ram_write implements the injected fixture callback.
 * @pre The fixed payload fits ::test_ram_sink_t storage.
 * @post Each scenario uses freshly initialized caller-owned state.
 * @post No descriptor or process-global output state is changed.
 * @note The error scenario pins propagation of the injected `EIO` value.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_test_injected(void)
{
  static const uint8_t k_payload[] = "abcdef";
  test_ram_sink_t      ram         = {.max_chunk = k_test_output_short_chunk};
  ra8_test_output_t    output      = {};
  if (!internal_test_output_init(&output, internal_ram_write, &ram) ||
      (internal_test_output_write_all(&output, k_payload, sizeof(k_payload) - 1U) !=
       k_ra8_test_output_ok) ||
      (ram.calls != k_test_output_short_calls) || (ram.length != (sizeof(k_payload) - 1U)) ||
      (memcmp(ram.bytes, k_payload, sizeof(k_payload) - 1U) != 0)) {
    return false;
  }
  ram = (test_ram_sink_t){.zero_progress = true};
  if (!internal_test_output_init(&output, internal_ram_write, &ram) ||
      (internal_test_output_text(&output, "x") != k_ra8_test_output_short)) {
    return false;
  }
  ram = (test_ram_sink_t){.fail_call = 1U};
  return internal_test_output_init(&output, internal_ram_write, &ram) &&
         (internal_test_output_text(&output, "x") == k_ra8_test_output_error) &&
         (output.os_error == EIO);
}

/**
 * @brief Verify signed, unsigned, padded-hex, and fixed-two formatting.
 * @details Composes every typed representation into one RAM span and compares
 * its complete bytes with a fixed human-readable golden.
 * @return Whether the typed renderer output matched exactly.
 * @retval true The complete output status, length, and bytes matched.
 * @retval false Initialization or any rendered field differed.
 * @pre The golden capacity fits within ::test_ram_sink_t storage.
 * @pre ::internal_ram_write is bound to fresh caller-owned state.
 * @post The sink contains only the fragments emitted by this scenario.
 * @post No raw descriptor is opened or modified.
 * @note INT64_MIN pins the signed renderer's overflow-safe magnitude path.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_test_formatting(void)
{
  static const char k_expected[] = "0|-9223372036854775808|0123ABCD|12.34";
  test_ram_sink_t   ram          = {};
  ra8_test_output_t output       = {};
  if (!internal_test_output_init(&output, internal_ram_write, &ram)) {
    return false;
  }
  (void)internal_test_output_u64(&output, 0U);
  (void)internal_test_output_text(&output, "|");
  (void)internal_test_output_i64(&output, INT64_MIN);
  (void)internal_test_output_text(&output, "|");
  (void)internal_test_output_hex64(&output,
                                   (uint64_t)k_test_output_hex_value,
                                   (uint8_t)k_test_output_hex_width,
                                   true);
  (void)internal_test_output_text(&output, "|");
  (void)internal_test_output_fixed2(&output, (uint64_t)k_test_output_fixed_value);
  return (output.status == k_ra8_test_output_ok) && (ram.length == (sizeof(k_expected) - 1U)) &&
         (memcmp(ram.bytes, k_expected, sizeof(k_expected) - 1U) == 0);
}

/**
 * @brief Verify exact raw-pipe output and observable EPIPE propagation.
 * @details Writes and reads one live pipe, then ignores SIGPIPE temporarily so
 * a second pipe exposes the raw adapter's latched `EPIPE` contract.
 * @return Whether both descriptor scenarios matched their expected results.
 * @retval true The live-pipe bytes and broken-pipe error matched.
 * @retval false Pipe setup, transfer, comparison, or error propagation failed.
 * @pre The host provides POSIX `pipe`, `read`, `write`, and signal handling.
 * @pre The process may temporarily replace and restore its SIGPIPE disposition.
 * @post Every descriptor opened by the test is closed.
 * @post The prior SIGPIPE disposition is restored before return.
 * @note No process-global output sink is installed.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_test_descriptor(void)
{
  static const uint8_t k_payload[] = "pipe";
  int                  descriptors[2];
  if (pipe(descriptors) != 0) {
    return false;
  }
  ra8_test_output_t    output = {};
  ra8_test_output_fd_t state  = {};
  bool    ok = internal_test_output_fd_init(&output, &state, descriptors[1]) &&
               (internal_test_output_write_all(&output, k_payload, sizeof(k_payload) - 1U) ==
                k_ra8_test_output_ok);
  uint8_t observed[sizeof(k_payload)] = {};
  ok =
    ok &&
    (read(descriptors[0], observed, sizeof(k_payload) - 1U) == (ssize_t)(sizeof(k_payload) - 1U)) &&
    (memcmp(observed, k_payload, sizeof(k_payload) - 1U) == 0);
  (void)close(descriptors[0]);
  (void)close(descriptors[1]);

  if (!ok || (pipe(descriptors) != 0)) {
    return false;
  }
  void (*previous)(int) = signal(SIGPIPE, SIG_IGN);
  (void)close(descriptors[0]);
  output = (ra8_test_output_t){};
  state  = (ra8_test_output_fd_t){};
  ok     = internal_test_output_fd_init(&output, &state, descriptors[1]) &&
           (internal_test_output_text(&output, "broken") == k_ra8_test_output_error) &&
           (output.os_error == EPIPE);
  (void)close(descriptors[1]);
  (void)signal(SIGPIPE, previous);
  return ok;
}

/** @brief Execute the focused test-output contract suite. */
int main(void)
{
  return (internal_test_injected() && internal_test_formatting() && internal_test_descriptor()) ? 0
                                                                                                : 1;
}
