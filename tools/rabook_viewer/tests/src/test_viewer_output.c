/**
 * @file test_viewer_output.c
 * @brief Exact-byte, bounded-RAM, and broken-pipe viewer output tests.
 * @details Exercises every typed diagnostic writer over caller-owned RAM,
 * proves a full sink reports its accepted prefix, and verifies the production
 * raw-descriptor adapter maps a closed pipe to a canonical error.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_posix.h"
#include "ra8_io_stream_ram.h"
#include "ra8_viewer_output_internal.h"

/** @brief Fixed capture budgets for output tests. */
typedef enum : uint32_t {
  k_test_capture_bytes = 1024U, /**< Complete diagnostic golden capture. */
  k_test_short_bytes   = 7U,    /**< Exact capacity of `usage: `.        */
} viewer_output_test_budget_t;

/** @brief Complete output capture backing. */
static uint8_t s_capture[k_test_capture_bytes];
/** @brief Intentionally undersized output capture backing. */
static uint8_t s_short[k_test_short_bytes];

/**
 * @brief Emit one instance of every typed diagnostic into a stream.
 * @details Uses representative paths, sizes, indices, and hexadecimal errors
 * so every fragment kind participates in the exact-byte golden.
 * @param[in,out] output Bound test stream.
 * @return First stream error or success.
 * @retval k_ra8_ok Every diagnostic was written completely.
 * @retval other The first stream write failure.
 * @pre @p output is bound and empty.
 * @pre The capture has room for the complete golden sequence.
 * @post Success appends the exact expected diagnostic sequence.
 * @post Failure leaves the accepted prefix in the sink.
 * @note Test-only and single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_emit_golden(ra8_io_stream_t* output)
{
  ra8_err_t error = priv_viewer_output_usage(output, "viewer");
  if (error == k_ra8_ok) {
    error = priv_viewer_output_capacity(output, "cache", UINT64_C(1234567890123), 17U);
  }
  if (error == k_ra8_ok) {
    error = priv_viewer_output_index_error(output, "render page ", 7U, (ra8_err_t)0x1A);
  }
  if (error == k_ra8_ok) {
    error = priv_viewer_output_error(output, "write tile ppm", (ra8_err_t)0x2B);
  }
  if (error == k_ra8_ok) {
    error = priv_viewer_output_open_error(output, "book.jof", (ra8_err_t)0x3C);
  }
  if (error == k_ra8_ok) {
    error = priv_viewer_output_opened(output, "book.jof", 4U);
  }
  if (error == k_ra8_ok) {
    error = priv_viewer_output_wrote(output, "out.ppm");
  }
  if (error == k_ra8_ok) {
    error = priv_viewer_output_tile(output, 9U, 800U, 600U, "tile.ppm");
  }
  if (error == k_ra8_ok) {
    error = priv_viewer_output_text(output, "dump ppm failed\n");
  }
  return error;
}

/**
 * @brief Compare all diagnostic bytes with their exact golden spelling.
 * @details Binds the production message assembly to a caller-owned RAM sink
 * and compares both its used extent and every captured byte.
 * @return Whether the complete capture matches.
 * @retval true Every typed message matched byte-for-byte.
 * @retval false Binding, emission, length, or content differed.
 * @pre Static capture storage is exclusively owned by this test.
 * @pre ::k_test_capture_bytes exceeds the golden extent.
 * @post Capture storage contains the emitted diagnostic prefix.
 * @post No external descriptor is opened or modified.
 * @note Test-only and single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_exact_capture(void)
{
  static const char expected[]    = "usage: viewer <file.jof> [--headless]\n"
                                    "         [--dump-ppm PATH [--page N | --dump-tile N]]\n"
                                    "  window: resizable, fit-to-width, continuous scroll "
                                    "(wheel/trackpad/PageUp-Dn/Home/End)\n"
                                    "cache workspace requires 1234567890123 bytes, supplied 17 bytes\n"
                                    "render page 7 failed: 0x1a\n"
                                    "write tile ppm failed: 0x2b\n"
                                    "open 'book.jof' failed: 0x3c\n"
                                    "opened 'book.jof': 4 page(s)\n"
                                    "wrote out.ppm\n"
                                    "wrote tile 9 (800x600) -> tile.ppm\n"
                                    "dump ppm failed\n";
  ra8_io_stream_t   output        = {};
  ra8_io_stream_ram_state_t state = {};
  uint32_t                  used  = 0U;
  return (ra8_io_stream_ram_init(&output, &state, s_capture, sizeof(s_capture)) == k_ra8_ok) &&
         (internal_emit_golden(&output) == k_ra8_ok) &&
         (ra8_io_stream_ram_used(&state, &used) == k_ra8_ok) && (used == (sizeof(expected) - 1U)) &&
         (memcmp(s_capture, expected, sizeof(expected) - 1U) == 0);
}

/**
 * @brief Prove RAM exhaustion reports the exact accepted prefix.
 * @details Sizes the sink to the first usage fragment and verifies the next
 * fragment fails without losing the prefix already accepted.
 * @return Whether the short capture contract held.
 * @retval true The sink filled exactly and returned no-memory.
 * @retval false Binding, status, count, or prefix differed.
 * @pre Static short storage is exclusively owned by this test.
 * @pre ::k_test_short_bytes equals the length of `usage: `.
 * @post The short backing contains exactly `usage: `.
 * @post The RAM sink reports its full bounded capacity as used.
 * @note Test-only and single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_short_capture(void)
{
  static const char         expected[] = "usage: ";
  ra8_io_stream_t           output     = {};
  ra8_io_stream_ram_state_t state      = {};
  uint32_t                  used       = 0U;
  return (ra8_io_stream_ram_init(&output, &state, s_short, sizeof(s_short)) == k_ra8_ok) &&
         (priv_viewer_output_usage(&output, "viewer") == k_ra8_err_no_mem) &&
         (ra8_io_stream_ram_used(&state, &used) == k_ra8_ok) && (used == sizeof(s_short)) &&
         (memcmp(s_short, expected, sizeof(s_short)) == 0);
}

/**
 * @brief Prove a closed POSIX pipe is returned as a stream communication error.
 * @details Ignores SIGPIPE, closes the pipe reader, and writes through the same
 * raw-descriptor adapter used by the production viewer composition.
 * @return Whether the raw-descriptor adapter surfaced EPIPE.
 * @retval true The write returned ::k_ra8_err_comm_error.
 * @retval false Signal setup, pipe setup, binding, or error mapping differed.
 * @pre The process may temporarily set SIGPIPE to ignored.
 * @pre The host provides POSIX pipe and descriptor semantics.
 * @post Every descriptor opened here is closed.
 * @post A failed write does not terminate the test process.
 * @note Test-only and single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_broken_pipe(void)
{
  struct sigaction action         = {.sa_handler = SIG_IGN};
  int              descriptors[2] = {-1, -1};
  if ((sigemptyset(&action.sa_mask) != 0) || (sigaction(SIGPIPE, &action, nullptr) != 0) ||
      (pipe(descriptors) != 0)) {
    return false;
  }
  (void)close(descriptors[0]);
  ra8_io_stream_t             output = {};
  ra8_io_stream_posix_state_t state  = {};
  const bool bound = ra8_io_stream_posix_init(&output, &state, descriptors[1]) == k_ra8_ok;
  const bool mapped =
    bound && (priv_viewer_output_wrote(&output, "closed") == k_ra8_err_comm_error);
  (void)close(descriptors[1]);
  return mapped;
}

/** @brief Test entry point. */
int main(void)
{
  return (internal_exact_capture() && internal_short_capture() && internal_broken_pipe()) ? 0 : 1;
}
