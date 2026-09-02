/**
 * @file test_ra8_unity_output.c
 * @brief Exact failure-output contracts for the minimal Unity compatibility
 * layer
 * @details Pins normal formatted failure bytes and exit status, then verifies
 * the bounded formatter at its last-fitting, capacity, and capacity-plus-one
 * lengths, including the explicit truncation marker.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ra8_attributes.h"
#include "unity_minimal.h"

/** @brief Unity-output fixture dimensions. */
typedef enum : size_t {
  k_unity_output_source_cap = k_unity_minimal_detail_cap + 2U, /**< Cap-plus-one fixture. */
} unity_output_limit_t;

/**
 * @brief Raise one deterministic formatted failure in a child process.
 * @details Invokes the compatibility macro under a fixed preprocessor source
 * location so the parent can compare the complete diagnostic bytes.
 * @pre The caller has redirected raw descriptor 2 to the fixture pipe.
 * @pre The call runs in a child process that may terminate independently.
 * @post The process exits with status one after writing the exact diagnostic.
 * @post Control never returns to the caller.
 * @note The fixed `#line` directive is part of the golden contract.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_raise_failure(void)
{
#line 700 "unity_output_fixture.c"
  TEST_FAIL_FMT("%s", "boom");
#line 33 "test_ra8_unity_output.c"
}

/**
 * @brief Read one pipe to EOF without losing interrupted or positive-short
 * reads.
 * @details Accumulates bounded reads until EOF, retrying only `EINTR` and
 * rejecting a destination that fills before the pipe closes.
 * @param[in] descriptor Borrowed readable pipe descriptor.
 * @param[out] bytes Destination span.
 * @param[in] capacity Writable destination capacity.
 * @param[out] length Complete observed byte count.
 * @return Whether EOF arrived before the destination filled or an error
 * occurred.
 * @retval true EOF arrived and @p length reports every accepted byte.
 * @retval false The buffer filled or a non-interrupt error occurred.
 * @pre @p descriptor is a readable, open pipe descriptor.
 * @pre @p bytes and @p length point to writable storage for the stated bounds.
 * @post @p length never exceeds @p capacity.
 * @post The borrowed descriptor remains open for the caller to close.
 * @note Partial reads are normal and do not change the result class.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_read_pipe(int descriptor, uint8_t* bytes, size_t capacity, size_t* length)
{
  *length = 0U;
  while (*length < capacity) {
    const ssize_t got = read(descriptor, &bytes[*length], capacity - *length);
    if (got > 0) {
      *length += (size_t)got;
    } else if (got == 0) {
      return true;
    } else if (errno != EINTR) {
      return false;
    }
  }
  return false;
}

/**
 * @brief Verify the compatibility macro's exact diagnostic and failure exit.
 * @details Runs one formatted failure in a child, captures descriptor 2 through
 * a pipe, and compares both the complete line and wait status.
 * @return Whether the pipe bytes and child status match the golden.
 * @retval true The child exited one and every diagnostic byte matched.
 * @retval false Process, pipe, status, length, or byte comparison failed.
 * @pre The host provides POSIX pipes, process creation, duplication, and wait.
 * @pre ::internal_raise_failure retains its fixed source-location contract.
 * @post Both pipe descriptors are closed in the parent and child paths.
 * @post The child has been reaped before this function returns.
 * @note The diagnostic uses no C-runtime stream object.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_test_failure_exit(void)
{
  static const char k_expected[] = "[FAIL] unity_output_fixture.c:700 boom\n";
  int               descriptors[2];
  if (pipe(descriptors) != 0) {
    return false;
  }
  const pid_t child = fork();
  if (child == 0) {
    (void)close(descriptors[0]);
    if (dup2(descriptors[1], STDERR_FILENO) < 0) {
      _exit(2);
    }
    (void)close(descriptors[1]);
    internal_raise_failure();
    _exit(3);
  }
  (void)close(descriptors[1]);
  if (child < 0) {
    (void)close(descriptors[0]);
    return false;
  }
  uint8_t    observed[sizeof(k_expected)] = {};
  size_t     length                       = 0U;
  const bool read_ok = internal_read_pipe(descriptors[0], observed, sizeof(observed), &length);
  (void)close(descriptors[0]);
  int status = 0;
  if ((waitpid(child, &status, 0) != child) || !WIFEXITED(status) || (WEXITSTATUS(status) != 1)) {
    return false;
  }
  return read_ok && (length == (sizeof(k_expected) - 1U)) &&
         (memcmp(observed, k_expected, sizeof(k_expected) - 1U) == 0);
}

/**
 * @brief Verify last-fitting, capacity, and capacity-plus-one formatter
 * goldens.
 * @details Exercises the finalizer with required lengths 255, 256, and 257,
 * comparing all 256 bytes so both marker placement and termination are pinned.
 * @return Whether all three bounded byte sequences match exactly.
 * @retval true The fitting bytes and both overflow marker goldens matched.
 * @retval false Any required length, marker offset, or complete span differed.
 * @pre ::k_unity_minimal_detail_cap exceeds the fixed marker size.
 * @pre The local source array holds the capacity-plus-one fixture and terminator.
 * @post The fitting fixture remains unmarked.
 * @post Both overflow fixtures end with the exact `...[truncated]` marker.
 * @note No descriptor or process-global state is used by this boundary test.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_test_truncation_boundaries(void)
{
  static const char k_marker[] = "...[truncated]";
  char              source[k_unity_output_source_cap];
  char              observed[k_unity_minimal_detail_cap];
  char              expected[k_unity_minimal_detail_cap];
  (void)memset(source, 'a', sizeof(source));
  source[sizeof(source) - 1U] = '\0';

  (void)memcpy(observed, source, sizeof(observed));
  observed[sizeof(observed) - 1U] = '\0';
  int required                    = (int)k_unity_minimal_detail_cap - 1;
  internal_test_mark_truncated(observed, required);
  (void)memset(expected, 'a', sizeof(expected));
  expected[sizeof(expected) - 1U] = '\0';
  if ((required != ((int)k_unity_minimal_detail_cap - 1)) ||
      (memcmp(observed, expected, sizeof(expected)) != 0)) {
    return false;
  }

  observed[sizeof(observed) - 1U] = '\0';
  required                        = (int)k_unity_minimal_detail_cap;
  internal_test_mark_truncated(observed, required);
  const size_t marker_offset = sizeof(expected) - sizeof(k_marker);
  (void)memcpy(&expected[marker_offset], k_marker, sizeof(k_marker));
  if ((required != (int)k_unity_minimal_detail_cap) ||
      (memcmp(observed, expected, sizeof(expected)) != 0)) {
    return false;
  }

  (void)memcpy(observed, source, sizeof(observed));
  observed[sizeof(observed) - 1U] = '\0';
  required                        = (int)k_unity_minimal_detail_cap + 1;
  internal_test_mark_truncated(observed, required);
  return (required == ((int)k_unity_minimal_detail_cap + 1)) &&
         (memcmp(observed, expected, sizeof(expected)) == 0);
}

/** @brief Execute the exact Unity output compatibility tests. */
int main(void)
{
  return (internal_test_failure_exit() && internal_test_truncation_boundaries()) ? 0 : 1;
}
