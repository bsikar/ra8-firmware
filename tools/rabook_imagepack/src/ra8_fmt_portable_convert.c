/**
 * @file ra8_fmt_portable_convert.c
 * @brief Raw-descriptor composition root for caller-workspace JOF conversion.
 * @details Parses the JOF-convert subset, derives exact producer requirements,
 * binds slices of an explicit caller arena, and publishes through a durable
 * same-directory transaction. Over-budget inputs fail before stage creation.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "ra8_attributes.h"
#include "ra8_fmt_host_fd_internal.h"
#include "ra8_fmt_portable_main_internal.h"
#include "ra8_fmt_stream.h"

/** @brief CLI status, input, and decimal formatting bounds. */
typedef enum : uint32_t {
  k_convert_cli_ok      = 0U,         /**< Successful conversion.          */
  k_convert_cli_fail    = 1U,         /**< Command execution failed.       */
  k_convert_cli_usage   = 2U,         /**< Invalid command line.           */
  k_convert_cli_input   = 268435456U, /**< Maximum source file (256 MiB).  */
  k_convert_cli_decimal = 20U,        /**< Digits in uint64_t.             */
  k_convert_cli_radix   = 10U,        /**< Diagnostic number radix.        */
  k_convert_cli_align   = 16U,        /**< Producer-arena slice alignment. */
} convert_cli_const_t;

/** @brief Parsed arguments accepted by the portable convert composition. */
typedef struct {
  const char* input;  /**< Source image path.        */
  const char* output; /**< Destination JOF path.     */
  const char* format; /**< Explicit format selector. */
} convert_cli_args_t;

/**
 * @brief Append one NUL-terminated diagnostic fragment.
 * @details Measures the complete fragment and delegates one exact write.
 * @param[in] sink Bound diagnostic sink.
 * @param[in] text NUL-terminated text fragment.
 * @return Injected sink status.
 * @retval k_ra8_ok Complete text was accepted.
 * @pre @p sink and its callback are non-null.
 * @pre @p text is non-null and NUL-terminated.
 * @post Exactly the bytes before NUL were offered once.
 * @post Neither sink binding nor source text was changed.
 * @note Thread safety inherits the injected sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_text(const ra8_fmt_sink_t* sink, const char* text)
{
  return sink->write(sink->ctx, (const uint8_t*)text, strlen(text));
}

/**
 * @brief Append one unsigned decimal diagnostic field.
 * @details Converts without locale or formatting streams through fixed buffers.
 * @param[in] sink Bound diagnostic sink.
 * @param[in] value Unsigned value to render.
 * @return Injected sink status.
 * @retval k_ra8_ok Every decimal digit was accepted.
 * @pre @p sink and its callback are non-null.
 * @pre The fixed digit capacity covers every uint64_t value.
 * @post One non-empty base-ten spelling was offered.
 * @post No global or filesystem state changed.
 * @note Thread safety inherits the injected sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_u64(const ra8_fmt_sink_t* sink, uint64_t value)
{
  char   reverse[k_convert_cli_decimal];
  size_t count = 0U;
  do {
    reverse[count++] = (char)('0' + (char)(value % k_convert_cli_radix));
    value /= k_convert_cli_radix;
  } while (value != 0U);
  char text[k_convert_cli_decimal];
  for (size_t i = 0U; i < count; ++i) {
    text[i] = reverse[count - i - 1U];
  }
  return sink->write(sink->ctx, (const uint8_t*)text, count);
}

/**
 * @brief Append one numeric field and suffix while status remains successful.
 * @details Centralizes fail-fast report composition without a local formatting macro.
 * @param[in] sink Bound diagnostic sink.
 * @param[in] value Unsigned field value.
 * @param[in] suffix NUL-terminated text following the number.
 * @param[in,out] status Current and resulting sink status.
 * @pre Every pointer argument is non-null.
 * @pre @p suffix is NUL-terminated and `*status` is a canonical status.
 * @post A successful incoming status attempts number then suffix in order.
 * @post A failing incoming or intermediate status prevents later writes.
 * @note Thread safety inherits the injected sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static void
internal_field(const ra8_fmt_sink_t* sink, uint64_t value, const char* suffix, ra8_err_t* status)
{
  if (*status == k_ra8_ok) {
    *status = internal_u64(sink, value);
  }
  if (*status == k_ra8_ok) {
    *status = internal_text(sink, suffix);
  }
}

/**
 * @brief Emit one canonical status diagnostic through fixed local formatting.
 * @details Appends a caller prefix, numeric status, and closing line delimiter.
 * @param[in] errors Bound diagnostic sink.
 * @param[in] prefix NUL-terminated prefix including an opening delimiter.
 * @param[in] status Canonical status value.
 * @pre @p errors and its callback are non-null.
 * @pre @p prefix is non-null and NUL-terminated.
 * @post Components were offered in order until the first sink error.
 * @post Diagnostic failure does not alter command state.
 * @note The no-return diagnostic path intentionally drops sink errors.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_status(const ra8_fmt_sink_t* errors, const char* prefix, ra8_err_t status)
{
  ra8_err_t rc = internal_text(errors, prefix);
  if (rc == k_ra8_ok) {
    rc = internal_u64(errors, (uint64_t)status);
  }
  if (rc == k_ra8_ok) {
    (void)internal_text(errors, ")\n");
  }
}

/**
 * @brief Parse the options accepted by the legacy convert command.
 * @details Recognizes the same bounded convert spellings before selecting JOF.
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @param[out] args Receives input, output, and format selections.
 * @return Whether every argument was recognized and complete.
 * @retval true All arguments were consumed into @p args.
 * @retval false An unknown, duplicate positional, or incomplete option occurred.
 * @pre @p argv holds @p argc NUL-terminated argument pointers.
 * @pre @p args is writable and initially zeroed.
 * @post Success consumes the complete argument vector without I/O.
 * @post Returned path pointers continue to refer into @p argv.
 * @note Parsing is deterministic and locale-independent.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_parse(int argc, char** argv, convert_cli_args_t* args)
{
  for (int i = 2; i < argc; ++i) {
    if ((strcmp(argv[i], "--format") == 0) && ((i + 1) < argc)) {
      args->format = argv[++i];
    } else if ((strcmp(argv[i], "--in") == 0) && ((i + 1) < argc)) {
      args->input = argv[++i];
    } else if ((strcmp(argv[i], "--out") == 0) && ((i + 1) < argc)) {
      args->output = argv[++i];
    } else if ((strcmp(argv[i], "--verbose") == 0) || (strcmp(argv[i], "-v") == 0)) {
      continue;
    } else if ((argv[i][0] != '-') && (args->input == nullptr)) {
      args->input = argv[i];
    } else {
      return false;
    }
  }
  return true;
}

/**
 * @brief Round one byte count up to the composition arena alignment.
 * @details Applies checked power-of-two alignment without wrapping size_t.
 * @param[in] value Unaligned byte count.
 * @param[out] aligned Receives the rounded byte count.
 * @return Whether rounding was representable.
 * @retval true @p aligned contains the exact rounded value.
 * @retval false Adding alignment padding would overflow.
 * @pre @p aligned is writable.
 * @pre ::k_convert_cli_align is a non-zero power of two.
 * @post Success writes a multiple of ::k_convert_cli_align not below @p value.
 * @post Failure leaves @p aligned unspecified and performs no I/O.
 * @note Pure apart from the caller output.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_align(size_t value, size_t* aligned)
{
  const size_t mask = (size_t)k_convert_cli_align - 1U;
  if (value > (SIZE_MAX - mask)) {
    return false;
  }
  *aligned = (value + mask) & ~mask;
  return true;
}

/**
 * @brief Compute the exact shared-arena high-water for one conversion.
 * @details Adds the aligned producer slice and optional WebP slice with overflow checks.
 * @param[in] requirements Per-producer arena requirements.
 * @param[out] webp_offset Aligned offset of optional WebP arena.
 * @param[out] total Exact shared arena high-water.
 * @return Whether the sum is representable.
 * @retval true Both outputs contain exact representable byte counts.
 * @retval false Alignment or addition would overflow size_t.
 * @pre Every pointer argument is non-null.
 * @pre @p requirements was produced by the requirements API.
 * @post Success includes inter-arena alignment padding in @p total.
 * @post Failure performs no I/O or output transaction work.
 * @note Pure apart from the two outputs.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_high_water(const ra8_fmt_jof_convert_requirements_t* requirements,
                                size_t*                                   webp_offset,
                                size_t*                                   total)
{
  if (!internal_align(requirements->work_bytes, webp_offset)) {
    return false;
  }
  if ((size_t)requirements->webp_work_bytes > (SIZE_MAX - *webp_offset)) {
    return false;
  }
  *total = *webp_offset + requirements->webp_work_bytes;
  return true;
}

/**
 * @brief Emit exact required/supplied workspace evidence.
 * @details Reports total high-water and both component arena requirements.
 * @param[in] errors Diagnostic sink.
 * @param[in] requirements Exact producer requirements.
 * @param[in] required Shared-arena high-water including alignment.
 * @param[in] supplied Caller arena capacity.
 * @pre @p errors and its write callback are non-null.
 * @pre @p requirements describes a successfully probed source.
 * @post Complete diagnostic is attempted until the first sink error.
 * @post Requirements and capacity values remain unchanged.
 * @note Diagnostic sink failures are intentionally ignored.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_capacity(const ra8_fmt_sink_t*                     errors,
                              const ra8_fmt_jof_convert_requirements_t* requirements,
                              size_t                                    required,
                              size_t                                    supplied)
{
  ra8_err_t rc = internal_text(errors, "ra8_fmt: JOF convert workspace too small: required ");
  internal_field(errors, required, " supplied ", &rc);
  internal_field(errors, supplied, " (work ", &rc);
  internal_field(errors, requirements->work_bytes, ", webp ", &rc);
  internal_field(errors, requirements->webp_work_bytes, ")\n", &rc);
}

/**
 * @brief Bind exact work slices and execute one already-open conversion.
 * @details Begins the sibling transaction only after sizing, then delegates the full engine.
 * @param[in] source Open host source.
 * @param[in] output Destination path.
 * @param[in] requirements Exact producer requirements.
 * @param[in,out] arena Caller composition workspace.
 * @param[in] webp_offset Aligned WebP arena offset.
 * @param[in] report Standard-output report sink.
 * @return Canonical conversion or host transaction status.
 * @retval k_ra8_ok A complete atlas was durably published and reported.
 * @retval other Transaction creation or the portable engine failed.
 * @pre @p source and @p report remain bound for the complete call.
 * @pre @p arena spans both exact slices at @p webp_offset.
 * @post Any failed conversion preserves the prior destination.
 * @post Every transaction resource acquired here is released.
 * @note Descriptor ownership stays at this host composition edge.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_run(const ra8_fmt_source_t*                   source,
                              const char*                               output,
                              const ra8_fmt_jof_convert_requirements_t* requirements,
                              uint8_t*                                  arena,
                              size_t                                    webp_offset,
                              const ra8_fmt_sink_t*                     report)
{
  ra8_fmt_host_transaction_t host_transaction;
  ra8_fmt_transaction_t      transaction;
  ra8_err_t rc = ra8_fmt_host_transaction_begin(output, &host_transaction, &transaction);
  if (rc != k_ra8_ok) {
    return rc;
  }
  ra8_fmt_jof_convert_workspace_t workspace = {
    .work          = arena,
    .work_cap      = requirements->work_bytes,
    .webp_work     = (requirements->webp_work_bytes == 0U) ? nullptr : &arena[webp_offset],
    .webp_work_cap = requirements->webp_work_bytes,
  };
  rc = ra8_fmt_jof_convert_stream(source, requirements, &workspace, &transaction, report, output);
  return rc;
}

int ra8_fmt_try_portable_convert(int      argc,
                                 char**   argv,
                                 uint8_t* arena,
                                 size_t   arena_cap,
                                 bool*    handled)
{
  if ((handled == nullptr) || (arena == nullptr)) {
    return (int)k_convert_cli_fail;
  }
  *handled = false;
  if ((argc < 2) || (strcmp(argv[1], "convert") != 0)) {
    return (int)k_convert_cli_ok;
  }
  convert_cli_args_t args = {};
  if (!internal_parse(argc, argv, &args) || (args.format == nullptr) ||
      (strcmp(args.format, "jof") != 0)) {
    return (int)k_convert_cli_ok;
  }
  *handled                           = true;
  ra8_fmt_host_fd_sink_t error_state = {.fd = STDERR_FILENO};
  const ra8_fmt_sink_t   errors      = ra8_fmt_host_fd_sink(&error_state);
  if ((args.input == nullptr) || (args.output == nullptr)) {
    (void)internal_text(&errors, "ra8_fmt: JOF convert needs --in and --out\n");
    return (int)k_convert_cli_usage;
  }
  ra8_fmt_host_source_t source = {.fd = -1};
  ra8_err_t             rc     = ra8_fmt_host_source_open(args.input, k_convert_cli_input, &source);
  if (rc != k_ra8_ok) {
    internal_status(&errors, "ra8_fmt: cannot open convert input (rc=", rc);
    return (int)k_convert_cli_fail;
  }
  ra8_fmt_jof_convert_requirements_t requirements = {};
  rc                     = ra8_fmt_jof_convert_requirements(&source.source, &requirements);
  size_t     webp_offset = 0U;
  size_t     high_water  = 0U;
  const bool sized =
    (rc == k_ra8_ok) && internal_high_water(&requirements, &webp_offset, &high_water);
  if ((rc == k_ra8_ok) && (!sized || (high_water > arena_cap))) {
    internal_capacity(&errors, &requirements, sized ? high_water : SIZE_MAX, arena_cap);
    rc = k_ra8_err_invalid_size;
  }
  ra8_fmt_host_fd_sink_t output_state = {.fd = STDOUT_FILENO};
  const ra8_fmt_sink_t   report       = ra8_fmt_host_fd_sink(&output_state);
  if (rc == k_ra8_ok) {
    rc = internal_run(&source.source, args.output, &requirements, arena, webp_offset, &report);
  }
  ra8_fmt_host_source_close(&source);
  if ((rc != k_ra8_ok) && sized && (high_water <= arena_cap)) {
    internal_status(&errors, "ra8_fmt: JOF convert failed (rc=", rc);
  } else if ((rc != k_ra8_ok) && !sized) {
    internal_status(&errors, "ra8_fmt: cannot size JOF convert (rc=", rc);
  }
  return (rc == k_ra8_ok) ? (int)k_convert_cli_ok : (int)k_convert_cli_fail;
}
