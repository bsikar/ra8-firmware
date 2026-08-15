/**
 * @file ra8_fmt_portable_main.c
 * @brief Raw-descriptor CLI composition for JOF and RBKC inspection.
 * @details Parses the inspect subset, resolves container magic, checks exact
 * workspace requirements, and binds explicit host-edge adapters.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_fmt_portable_main.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "ra8_attributes.h"
#include "ra8_fmt_host_fd.h"
#include "ra8_fmt_stream.h"

/** @brief Exit values and explicit composition workspace budgets. */
typedef enum : uint32_t {
  k_cli_exit_ok       = 0U,         /**< Successful command.               */
  k_cli_exit_fail     = 1U,         /**< Command ran and failed.           */
  k_cli_exit_usage    = 2U,         /**< Invalid command line.             */
  k_cli_input_cap     = 268435456U, /**< Maximum accepted input (256 MiB). */
  k_cli_decimal_radix = 10U,        /**< Status-code formatting radix.     */
  k_cli_decimal_chars = 20U,        /**< Maximum unsigned decimal digits.  */
} cli_const_t;

/** @brief Supported inspect-container classifications. */
typedef enum : uint8_t {
  k_cli_format_none   = 0U, /**< Unknown or unsupported container. */
  k_cli_format_jof    = 1U, /**< JOF1 band-tile atlas.             */
  k_cli_format_rabook = 2U, /**< RBKC chunked RABOOK1 container.   */
} cli_format_t;

/**
 * @brief Append one literal through an injected sink.
 * @details Measures the NUL-terminated text and delegates one exact byte span.
 * @param[in] sink Bound output sink.
 * @param[in] text NUL-terminated literal or diagnostic string.
 * @return Canonical sink status.
 * @retval k_ra8_ok Complete text was accepted.
 * @retval other Injected sink rejected the span.
 * @pre @p sink, its callback, and @p text are non-null.
 * @pre @p text is NUL-terminated.
 * @post Exactly the bytes before NUL were offered once.
 * @post Sink binding and source text are unchanged.
 * @note Thread safety inherits the injected sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_text(const ra8_fmt_sink_t* sink, const char* text)
{
  return sink->write(sink->ctx, (const uint8_t*)text, strlen(text));
}

/**
 * @brief Append an unsigned decimal through an injected sink.
 * @details Converts through fixed local buffers and delegates one digit span.
 * @param[in] sink Bound output sink.
 * @param[in] value Unsigned value to render in base ten.
 * @return Canonical sink status.
 * @retval k_ra8_ok Complete decimal spelling was accepted.
 * @retval other Injected sink rejected the span.
 * @pre @p sink and its callback are non-null.
 * @pre Fixed digit capacity covers every `uint64_t` value.
 * @post Exactly one non-empty decimal spelling was offered.
 * @post No global or filesystem state changed.
 * @note Thread safety inherits the injected sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_u64(const ra8_fmt_sink_t* sink, uint64_t value)
{
  char   reverse[k_cli_decimal_chars];
  size_t count = 0U;
  do {
    reverse[count++] = (char)('0' + (char)(value % k_cli_decimal_radix));
    value /= k_cli_decimal_radix;
  } while (value != 0U);
  char text[k_cli_decimal_chars];
  for (size_t i = 0U; i < count; ++i) {
    text[i] = reverse[count - i - 1U];
  }
  return sink->write(sink->ctx, (const uint8_t*)text, count);
}

/**
 * @brief Emit one error line with an integer status.
 * @details Writes prefix, unsigned status spelling, and closing delimiter
 * fail-fast.
 * @param[in] sink Bound diagnostic sink.
 * @param[in] prefix NUL-terminated prefix including opening delimiter.
 * @param[in] status Canonical status to report.
 * @pre @p sink, its callback, and @p prefix are non-null.
 * @pre @p prefix is NUL-terminated.
 * @post Complete line was attempted until the first sink error.
 * @post No command or source state changed.
 * @note Diagnostic sink failures are intentionally not propagated.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_error_status(const ra8_fmt_sink_t* sink, const char* prefix, ra8_err_t status)
{
  ra8_err_t rc = internal_text(sink, prefix);
  if (rc == k_ra8_ok) {
    rc = internal_u64(sink, (uint64_t)status);
  }
  if (rc == k_ra8_ok) {
    (void)internal_text(sink, ")\n");
  }
}

/**
 * @brief Parse the established inspect command arguments.
 * @details Recognizes bounded options while preserving positional input
 * behavior.
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @param[in,out] input Receives input path when present.
 * @param[in,out] format Receives explicit format when present.
 * @param[in,out] verbose Receives verbose-option state.
 * @return Whether every inspect argument was recognized and complete.
 * @retval true Output selections reflect the complete argument sequence.
 * @retval false Unknown, duplicate positional, or missing-value syntax
 * occurred.
 * @pre @p argv contains @p argc NUL-terminated strings.
 * @pre Output pointers are non-null and initialized by the caller.
 * @post Success consumes every argument from index two onward.
 * @post Parser performs no I/O and retains only pointers into @p argv.
 * @note Parsing is deterministic and locale-independent.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool
internal_parse(int argc, char** argv, const char** input, const char** format, bool* verbose)
{
  for (int i = 2; i < argc; ++i) {
    if ((strcmp(argv[i], "--verbose") == 0) || (strcmp(argv[i], "-v") == 0)) {
      *verbose = true;
    } else if ((strcmp(argv[i], "--format") == 0) && ((i + 1) < argc)) {
      *format = argv[++i];
    } else if ((strcmp(argv[i], "--in") == 0) && ((i + 1) < argc)) {
      *input = argv[++i];
    } else if ((strcmp(argv[i], "--out") == 0) && ((i + 1) < argc)) {
      ++i;
    } else if ((argv[i][0] != '-') && (*input == nullptr)) {
      *input = argv[i];
    } else {
      return false;
    }
  }
  return true;
}

/**
 * @brief Resolve an explicit name or four-byte container magic.
 * @details Reads one bounded prefix only when no explicit format was supplied.
 * @param[in] source Bound portable source.
 * @param[in] explicit_name Optional CLI format selector.
 * @param[out] format Receives supported classification or none.
 * @return Canonical source status.
 * @retval k_ra8_ok Classification was produced, including an unknown source.
 * @retval other Injected positioned read failed.
 * @pre @p source, its callback, and @p format are non-null.
 * @pre Source size describes the same backing used by its callback.
 * @post Success initializes @p format exactly once.
 * @post Source position and binding are unchanged.
 * @note Classification is prefix evidence; full parser performs validation.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_format(const ra8_fmt_source_t* source, const char* explicit_name, cli_format_t* format)
{
  *format = k_cli_format_none;
  if (explicit_name != nullptr) {
    if (strcmp(explicit_name, "jof") == 0) {
      *format = k_cli_format_jof;
    } else if (strcmp(explicit_name, "rabook") == 0) {
      *format = k_cli_format_rabook;
    }
    return k_ra8_ok;
  }
  uint8_t         magic[4U];
  size_t          got = 0U;
  const ra8_err_t rc  = source->read_at(source->ctx, 0U, magic, sizeof(magic), &got);
  if (rc != k_ra8_ok) {
    return rc;
  }
  if ((got == sizeof(magic)) && (memcmp(magic, "JOF1", sizeof(magic)) == 0)) { /* MAGIC-OK */
    *format = k_cli_format_jof;
  } else if ((got == sizeof(magic)) && (memcmp(magic, "RBKC", sizeof(magic)) == 0)) { /* MAGIC-OK */
    *format = k_cli_format_rabook;
  }
  return k_ra8_ok;
}

/**
 * @brief Report exact workspace requirements and capacities.
 * @details Emits required/supplied record, tile, and scratch bounds through
 * callbacks.
 * @param[in] sink Bound diagnostic sink.
 * @param[in] need Successfully derived JOF audit requirements.
 * @pre @p sink, its callback, and @p need are non-null.
 * @pre At least one requirement exceeds the compiled composition budget.
 * @post Capacity diagnostic was attempted until the first sink failure.
 * @post Requirements and static workspace are unchanged.
 * @note Diagnostic sink failures are intentionally not propagated.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_workspace_error(const ra8_fmt_sink_t*               sink,
                                     const ra8_jof_audit_requirements_t* need)
{
  ra8_err_t rc = internal_text(sink, "ra8_fmt: JOF inspect workspace too small: records ");
  if (rc == k_ra8_ok) {
    rc = internal_u64(sink, need->record_count);
  }
  if (rc == k_ra8_ok) {
    rc = internal_text(sink, "/65536, tile ");
  }
  if (rc == k_ra8_ok) {
    rc = internal_u64(sink, need->tile_bytes);
  }
  if (rc == k_ra8_ok) {
    rc = internal_text(sink, "/4194304, scratch ");
  }
  if (rc == k_ra8_ok) {
    rc = internal_u64(sink, need->scratch_bytes);
  }
  if (rc == k_ra8_ok) {
    (void)internal_text(sink, "/4718848\n");
  }
}

/**
 * @brief Execute inspection after CLI ownership and source resolution.
 * @details Derives exact needs, checks the named BSS budget, binds caller-owned
 * views, and delegates the complete callback-driven inspection.
 * @param[in] source Open bounded portable source.
 * @param[in] verbose Whether to emit bounded hex dumps and tile table.
 * @param[in,out] workspace Explicit shared composition-root storage.
 * @param[in] output Bound report sink.
 * @param[in] errors Bound diagnostic sink.
 * @return Portable command exit status.
 * @retval k_cli_exit_ok Complete audit and report succeeded.
 * @retval k_cli_exit_fail Parse, capacity, audit, or report failed.
 * @pre Source, sinks, and their callbacks are non-null and remain live.
 * @pre Source size does not exceed the host-open ceiling.
 * @post No dynamic storage or descriptor ownership is acquired here.
 * @post Static workspace contents may change but no source byte is changed.
 * @note Single-threaded through the named composition-root workspace.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_run_jof(const ra8_fmt_source_t*  source,
                            bool                     verbose,
                            ra8_fmt_cli_workspace_t* workspace,
                            const ra8_fmt_sink_t*    output,
                            const ra8_fmt_sink_t*    errors)
{
  ra8_jof_audit_requirements_t need = {};
  ra8_err_t rc = ra8_jof_audit_requirements(source->read_at, source->ctx, source->size, &need);
  if (rc != k_ra8_ok) {
    internal_error_status(errors, "JOF parse FAILED (rc=", rc);
    return (int)k_cli_exit_fail;
  }
  if ((need.record_count > k_ra8_fmt_cli_record_cap) ||
      (need.tile_bytes > k_ra8_fmt_cli_tile_cap) ||
      (need.scratch_bytes > k_ra8_fmt_cli_scratch_cap)) {
    internal_workspace_error(errors, &need);
    return (int)k_cli_exit_fail;
  }
  const size_t records_bytes = sizeof(ra8_jof_audit_record_t) * (size_t)k_ra8_fmt_cli_record_cap;
  ra8_fmt_jof_inspect_workspace_t inspect_workspace = {
    .records     = (ra8_jof_audit_record_t*)workspace->bytes,
    .record_cap  = k_ra8_fmt_cli_record_cap,
    .tile        = &workspace->bytes[records_bytes],
    .tile_cap    = k_ra8_fmt_cli_tile_cap,
    .scratch     = &workspace->bytes[records_bytes + k_ra8_fmt_cli_tile_cap],
    .scratch_cap = k_ra8_fmt_cli_scratch_cap,
  };
  rc = ra8_fmt_jof_inspect_stream(source, verbose, &inspect_workspace, output);
  return (rc == k_ra8_ok) ? (int)k_cli_exit_ok : (int)k_cli_exit_fail;
}

/**
 * @brief Bind the existing shared high-water to strict RBKC workspaces.
 * @details Partitions the named CLI bytes into disjoint table, compressed,
 * inflated, and semantic-validation spans before invoking the stream engine.
 * @param[in] source Open immutable container source.
 * @param[in] verbose Whether to emit the bounded chunk inventory.
 * @param[in,out] workspace Existing named CLI composition storage.
 * @param[in] output Bound report sink.
 * @return Portable process status.
 * @retval k_cli_exit_ok Strict outer and inner validation succeeded.
 * @retval k_cli_exit_fail Validation or reporting failed.
 * @pre Pointer arguments and callbacks are non-null and remain live.
 * @pre Workspace alignment and extent match ::ra8_fmt_cli_workspace_t.
 * @post Workspace contents may change but no source byte changes.
 * @post No descriptor ownership escapes the call.
 * @note The partition does not increase the existing CLI storage high-water.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_run_rabook(const ra8_fmt_source_t*  source,
                               bool                     verbose,
                               ra8_fmt_cli_workspace_t* workspace,
                               const ra8_fmt_sink_t*    output)
{
  const size_t table_bytes = (size_t)k_ra8_fmt_cli_rbkc_table_cap * sizeof(uint64_t);
  ra8_fmt_rabook_inspect_workspace_t inspect_workspace = {
    .table          = (uint64_t*)workspace->bytes,
    .table_cap      = k_ra8_fmt_cli_rbkc_table_cap,
    .compressed     = &workspace->bytes[table_bytes],
    .compressed_cap = k_ra8_fmt_cli_rbkc_compressed_cap,
    .chunk          = &workspace->bytes[table_bytes + k_ra8_fmt_cli_rbkc_compressed_cap],
    .chunk_cap      = k_ra8_fmt_cli_rbkc_chunk_cap,
    .scratch =
      &workspace
         ->bytes[table_bytes + k_ra8_fmt_cli_rbkc_compressed_cap + k_ra8_fmt_cli_rbkc_chunk_cap],
    .scratch_cap = k_ra8_fmt_cli_rbkc_scratch_cap,
  };
  const ra8_err_t rc = ra8_fmt_rabook_inspect_stream(source, verbose, &inspect_workspace, output);
  return (rc == k_ra8_ok) ? (int)k_cli_exit_ok : (int)k_cli_exit_fail;
}

/**
 * @brief Open, classify, and inspect one bounded host source.
 * @details Opens a stable raw-descriptor view, resolves an explicit selector or
 * magic prefix, dispatches the strict engine, and closes the source exactly
 * once.
 * @param[in] input NUL-terminated input path.
 * @param[in] explicit_name Optional explicit format name.
 * @param[in] verbose Whether to emit format-specific detail.
 * @param[in,out] workspace Named CLI composition storage.
 * @param[out] handled Receives whether a supported format owned the result.
 * @param[in] errors Bound diagnostic sink.
 * @return Portable process status.
 * @retval k_cli_exit_ok Inspection succeeded or the format was unknown.
 * @retval k_cli_exit_fail Open, classification, validation, or reporting
 * failed.
 * @pre All required pointer arguments and callbacks are non-null.
 * @pre @p input and any @p explicit_name are NUL-terminated.
 * @post Every successfully opened source descriptor is closed.
 * @post @p handled is false only for an unsupported format classification.
 * @note Unknown formats return to the top-level usage path.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_open(const char*              input,
                         const char*              explicit_name,
                         bool                     verbose,
                         ra8_fmt_cli_workspace_t* workspace,
                         bool*                    handled,
                         const ra8_fmt_sink_t*    errors)
{
  ra8_fmt_host_source_t host_source = {.fd = -1};
  ra8_err_t             rc     = ra8_fmt_host_source_open(input, k_cli_input_cap, &host_source);
  cli_format_t          format = k_cli_format_none;
  if (rc == k_ra8_ok) {
    rc = internal_format(&host_source.source, explicit_name, &format);
  }
  if (rc != k_ra8_ok) {
    internal_error_status(errors, "ra8_fmt: cannot open inspect input (rc=", rc);
    return (int)k_cli_exit_fail;
  }
  if (format == k_cli_format_none) {
    ra8_fmt_host_source_close(&host_source);
    *handled = false;
    return (int)k_cli_exit_ok;
  }
  ra8_fmt_host_fd_sink_t output_state = {.fd = STDOUT_FILENO};
  const ra8_fmt_sink_t   output       = ra8_fmt_host_fd_sink(&output_state);
  const int status = (format == k_cli_format_jof)
                       ? internal_run_jof(&host_source.source, verbose, workspace, &output, errors)
                       : internal_run_rabook(&host_source.source, verbose, workspace, &output);
  ra8_fmt_host_source_close(&host_source);
  return status;
}

int ra8_fmt_try_portable_inspect(int                      argc,
                                 char**                   argv,
                                 ra8_fmt_cli_workspace_t* workspace,
                                 bool*                    handled)
{
  if ((handled == nullptr) || (workspace == nullptr)) {
    return (int)k_cli_exit_fail;
  }
  *handled = false;
  if ((argc < 2) || (strcmp(argv[1], "inspect") != 0)) {
    return (int)k_cli_exit_ok;
  }
  const char*            input       = nullptr;
  const char*            format      = nullptr;
  bool                   verbose     = false;
  ra8_fmt_host_fd_sink_t error_state = {.fd = STDERR_FILENO};
  const ra8_fmt_sink_t   errors      = ra8_fmt_host_fd_sink(&error_state);
  if (!internal_parse(argc, argv, &input, &format, &verbose)) {
    *handled = true;
    (void)internal_text(&errors, "ra8_fmt: invalid inspect arguments\n");
    return (int)k_cli_exit_usage;
  }
  *handled = true;
  if (input == nullptr) {
    (void)internal_text(&errors, "ra8_fmt: no input file given\n");
    return (int)k_cli_exit_usage;
  }
  return internal_open(input, format, verbose, workspace, handled, &errors);
}
