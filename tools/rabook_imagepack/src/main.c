/**
 * @file main.c
 * @brief Portable command entry for JOF and RBKC content tooling.
 * @details Dispatches the three JOF verbs and strict RBKC inspection through
 * raw descriptors, injected callbacks, and one named caller workspace.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include "ra8_attributes.h"
#include "ra8_fmt_host_fd_internal.h"
#include "ra8_fmt_portable_main_internal.h"
#include "ra8_log.h"

/** @brief Process status returned when no supported command owns the input. */
typedef enum : uint8_t {
  k_main_exit_usage = 2U, /**< Invalid or unsupported command line. */
} main_exit_t;

/**
 * @var s_cli_workspace
 * @brief Existing explicit high-water shared by mutually exclusive CLI verbs.
 * @details JOF conversion uses its first 8 MiB; JOF and RBKC inspection bind
 * disjoint typed views over the same 10,486,016-byte object.
 * @note The command is single-threaded; tests bind independent workspaces.
 * @since 0.1.0
 */
static ra8_fmt_cli_workspace_t s_cli_workspace;

/** @brief Established usage banner and supported-format inventory. */
static const char s_usage[] =
  "usage:\n"
  "  ra8_fmt convert --format <fmt> --in <file> --out <file>\n"
  "  ra8_fmt inspect <container> [--verbose]\n"
  "  ra8_fmt verify  --format <fmt> --in <file> [--out <dump.ppm>]\n"
  "\nformats:\n"
  "  jof      .jof      [convert] [inspect] [verify] band-tile atlas (JOF): "
  "display-native, O(1) random access per tile\n"
  "  rabook   .rabook   [inspect] chunked book container (RBKC): compiled "
  "book, "
  "one unit = one book\n";

/**
 * @brief Offer the complete usage banner to the injected diagnostic sink.
 * @details Sends the immutable banner as one exact byte span and deliberately
 * drops a diagnostic-only sink error.
 * @param[in] errors Bound raw-descriptor diagnostic sink.
 * @pre @p errors is non-null.
 * @pre `errors->write` remains callable for the operation.
 * @post The complete banner was offered exactly once.
 * @post No descriptor ownership or workspace state changed.
 * @note The command exit status, not banner delivery, owns error reporting.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_usage(const ra8_fmt_sink_t* errors)
{
  (void)errors->write(errors->ctx, (const uint8_t*)s_usage, sizeof(s_usage) - 1U);
}

/**
 * @brief Try every portable dispatcher in stable command order.
 * @details Offers the command to convert, verify, and inspect until one path
 * claims ownership of the returned process status.
 * @param[in] argc Process argument count.
 * @param[in] argv Process argument vector.
 * @param[out] handled Receives whether a dispatcher owns the result.
 * @return Process status from the selected dispatcher, or zero when unmatched.
 * @retval 0 A command succeeded or no dispatcher matched.
 * @retval 1 A selected command failed.
 * @pre @p argv spans @p argc argument pointers.
 * @pre @p handled is non-null and writable.
 * @post At most one dispatcher executed a command.
 * @post @p handled identifies whether the return value is authoritative.
 * @note Uses the single named CLI workspace and is not reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_dispatch(int argc, char** argv, bool* handled)
{
  int status = priv_fmt_try_portable_convert(argc,
                                             argv,
                                             s_cli_workspace.bytes,
                                             k_ra8_fmt_cli_convert_arena_bytes,
                                             handled);
  if (!*handled) {
    status = priv_fmt_try_portable_verify(argc, argv, &s_cli_workspace, handled);
  }
  if (!*handled) {
    status = priv_fmt_try_portable_inspect(argc, argv, &s_cli_workspace, handled);
  }
  return status;
}

int main(int argc, char** argv)
{
  ra8_fmt_host_fd_sink_t error_state = {.fd = STDERR_FILENO};
  const ra8_fmt_sink_t   errors      = priv_fmt_host_fd_sink(&error_state);
  ra8_log_set_byte_sink(priv_fmt_host_log_byte, &error_state);
  bool      handled = false;
  const int status  = internal_dispatch(argc, argv, &handled);
  if (handled) {
    return status;
  }
  internal_usage(&errors);
  return (int)k_main_exit_usage;
}
