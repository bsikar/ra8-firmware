/**
 * @file ra8_fmt_portable_verify.c
 * @brief Raw-fd composition root for bounded two-spool JOF verification.
 * @details Opens two immutable source contexts, plans exact phase-overlaid
 * workspace, creates anonymous sibling spools, and optionally binds a durable
 * PPM transaction. All owned descriptors are closed on every path.
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
#include "ra8_fmt_host_spool_internal.h"
#include "ra8_fmt_portable_main_internal.h"
#include "ra8_fmt_stream.h"

/** @brief CLI and workspace-layout constants. */
typedef enum : uint32_t {
  k_verify_cli_ok      = 0U,         /**< Successful exact verdict.        */
  k_verify_cli_fail    = 1U,         /**< Verification or host failure.    */
  k_verify_cli_input   = 268435456U, /**< Maximum encoded input (256 MiB). */
  k_verify_cli_align   = 16U,        /**< Arena slice alignment.           */
  k_verify_cli_digits  = 20U,        /**< Digits in uint64_t.              */
  k_verify_cli_decimal = 10U,        /**< Decimal formatting radix.        */
} verify_cli_const_t;

/** @brief Parsed legacy-compatible JOF verify selections. */
typedef struct {
  const char* input;  /**< Encoded source path. */
  const char* output; /**< Optional PPM path.   */
  const char* format; /**< Explicit format.     */
} verify_cli_args_t;

/** @brief Exact phase-overlaid byte offsets in the shared composition arena. */
typedef struct {
  size_t producer; /**< Maximum producer work bytes.    */
  size_t webp;     /**< WebP arena offset.              */
  size_t scratch;  /**< Comparison scratch offset.      */
  size_t row;      /**< Reference-row offset.           */
  size_t total;    /**< Exact maximum phase high-water. */
} verify_layout_t;

/**
 * @brief Append one NUL-terminated text fragment.
 * @details Measures the fixed spelling and delegates one exact sink write.
 * @param[in] sink Bound output sink.
 * @param[in] text NUL-terminated spelling.
 * @return Sink status.
 * @retval k_ra8_ok The complete spelling was appended.
 * @retval other Injected sink failure.
 * @pre @p sink and its callback are valid.
 * @pre @p text is NUL-terminated.
 * @post Success appends exactly strlen(@p text) bytes.
 * @post No input byte changes.
 * @note Thread safety inherits the sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_text(const ra8_fmt_sink_t* sink, const char* text)
{
  return sink->write(sink->ctx, (const uint8_t*)text, strlen(text));
}

/**
 * @brief Append one uint64_t in decimal.
 * @details Uses fixed reverse-digit storage and emits no terminator.
 * @param[in] sink Bound output sink.
 * @param[in] value Value to spell.
 * @return Sink status.
 * @retval k_ra8_ok The complete decimal was appended.
 * @retval other Injected sink failure.
 * @pre @p sink and its callback are valid.
 * @pre Fixed digit storage spans ::k_verify_cli_digits bytes.
 * @post Success appends the canonical unsigned decimal.
 * @post No global or input state changes.
 * @note Thread safety inherits the sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_u64(const ra8_fmt_sink_t* sink, uint64_t value)
{
  char   reverse[k_verify_cli_digits];
  size_t count = 0U;
  do {
    reverse[count++] = (char)('0' + (char)(value % k_verify_cli_decimal));
    value /= k_verify_cli_decimal;
  } while (value != 0U);
  char text[k_verify_cli_digits];
  for (size_t i = 0U; i < count; ++i) {
    text[i] = reverse[count - i - 1U];
  }
  return sink->write(sink->ctx, (const uint8_t*)text, count);
}

/**
 * @brief Append one numeric field and suffix while status succeeds.
 * @details Preserves the first sink error across the chained report operation.
 * @param[in] sink Bound output sink.
 * @param[in] value Numeric field.
 * @param[in] suffix NUL-terminated suffix.
 * @param[in,out] status Current and resulting report status.
 * @pre Every pointer argument is non-null.
 * @pre @p status contains the prior append result.
 * @post Existing failure skips every append.
 * @post Success appends both field and suffix.
 * @note Thread safety inherits the sink.
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
 * @brief Emit one canonical status diagnostic.
 * @details Appends a fixed prefix, decimal status, close parenthesis, and newline.
 * @param[in] sink Bound diagnostic sink.
 * @param[in] prefix NUL-terminated diagnostic prefix.
 * @param[in] status Status value to report.
 * @pre @p sink and @p prefix are valid.
 * @pre The prefix leaves the numeric parenthesis open.
 * @post Best effort emits one complete diagnostic line.
 * @post No caller input changes.
 * @note Sink failures are intentionally not recursive.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_status(const ra8_fmt_sink_t* sink, const char* prefix, ra8_err_t status)
{
  ra8_err_t rc = internal_text(sink, prefix);
  internal_field(sink, status, ")\n", &rc);
}

/**
 * @brief Parse only the legacy JOF verify option spellings.
 * @details Accepts explicit format, input, output, verbosity, and one positional input.
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @param[out] args Receives paths and format.
 * @return Whether every option was recognized and complete.
 * @retval true Every token was accepted.
 * @retval false An unknown or incomplete option was found.
 * @pre @p argv spans @p argc pointers.
 * @pre @p args is zero-initialized and writable.
 * @post Success retains only pointers into @p argv.
 * @post Failure performs no I/O or ownership transfer.
 * @note Parsing is deterministic and performs no I/O.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_parse(int argc, char** argv, verify_cli_args_t* args)
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
 * @brief Align one size to the composition slice boundary.
 * @details Checks addition before rounding up to ::k_verify_cli_align.
 * @param[in] value Unaligned byte count.
 * @param[out] out Receives the aligned count.
 * @return Whether alignment is representable.
 * @retval true @p out contains the aligned count.
 * @retval false Rounding would overflow size_t.
 * @pre @p out is writable.
 * @pre ::k_verify_cli_align is a power of two.
 * @post Success initializes @p out.
 * @post Failure performs no allocation or I/O.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_align(size_t value, size_t* out)
{
  const size_t mask = (size_t)k_verify_cli_align - 1U;
  if (value > (SIZE_MAX - mask)) {
    return false;
  }
  *out = (value + mask) & ~mask;
  return true;
}

/**
 * @brief Add one aligned arena slice without size_t wrapping.
 * @details Aligns the incoming offset before checked slice addition.
 * @param[in] offset Current phase offset.
 * @param[in] bytes Slice bytes.
 * @param[out] next Receives the next unaligned phase offset.
 * @return Whether alignment and addition were representable.
 * @retval true @p next contains the exact slice end.
 * @retval false Alignment or addition overflowed.
 * @pre @p next is writable.
 * @pre @p offset describes the current phase arena.
 * @post Success initializes @p next.
 * @post Failure performs no allocation or I/O.
 * @note Pure apart from caller output.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_add(size_t offset, size_t bytes, size_t* next)
{
  size_t aligned = 0U;
  if (!internal_align(offset, &aligned) || (bytes > (SIZE_MAX - aligned))) {
    return false;
  }
  *next = aligned + bytes;
  return true;
}

/**
 * @brief Compute exact maximum high-water across producer and compare phases.
 * @details Overlays mutually exclusive phases while aligning every simultaneous slice.
 * @param[in] need Exact engine requirements.
 * @param[out] layout Receives offsets and maximum phase high-water.
 * @return Whether every offset is representable.
 * @retval true Every exact slice and high-water fits size_t.
 * @retval false One alignment or addition overflowed.
 * @pre @p need and @p layout are non-null.
 * @pre Requirement fields came from the bounded planner.
 * @post Success initializes all layout fields.
 * @post Failure performs no workspace write.
 * @note Producer and comparison phases intentionally overlap byte zero.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_layout(const ra8_fmt_jof_verify_requirements_t* need, verify_layout_t* layout)
{
  *layout             = (verify_layout_t){};
  layout->producer    = (need->reference_work_bytes > need->banded_work_bytes)
                          ? need->reference_work_bytes
                          : need->banded_work_bytes;
  size_t producer_end = 0U;
  if (!internal_align(layout->producer, &layout->webp) ||
      !internal_add(layout->webp, need->webp_work_bytes, &producer_end)) {
    return false;
  }
  size_t scratch_end = 0U;
  size_t compare_end = 0U;
  if (!internal_align(need->band_tile_bytes, &layout->scratch) ||
      !internal_add(layout->scratch, need->scratch_bytes, &scratch_end) ||
      !internal_align(scratch_end, &layout->row) ||
      !internal_add(layout->row, need->row_bytes, &compare_end)) {
    return false;
  }
  layout->total = (producer_end > compare_end) ? producer_end : compare_end;
  return true;
}

/**
 * @brief Report exact required and supplied shared-workspace evidence.
 * @details Emits the high-water plus every contributing phase component.
 * @param[in] errors Standard-error sink.
 * @param[in] need Exact verifier requirements.
 * @param[in] layout Computed exact offsets.
 * @param[in] supplied Caller workspace capacity.
 * @pre Every pointer argument is valid.
 * @pre @p layout corresponds to @p need.
 * @post Best effort emits one bounded diagnostic line.
 * @post Workspace and requirements remain unchanged.
 * @note Sink failures are intentionally ignored after first failure.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_capacity(const ra8_fmt_sink_t*                    errors,
                              const ra8_fmt_jof_verify_requirements_t* need,
                              const verify_layout_t*                   layout,
                              size_t                                   supplied)
{
  ra8_err_t rc = internal_text(errors, "ra8_fmt: JOF verify workspace too small: required ");
  internal_field(errors, layout->total, " supplied ", &rc);
  internal_field(errors, supplied, " (producer ", &rc);
  internal_field(errors, layout->producer, ", webp ", &rc);
  internal_field(errors, need->webp_work_bytes, ", band ", &rc);
  internal_field(errors, need->band_tile_bytes, ", scratch ", &rc);
  internal_field(errors, need->scratch_bytes, ", row ", &rc);
  internal_field(errors, need->row_bytes, ")\n", &rc);
}

/** @copydoc ra8_fmt_sink_write_fn */
RA8_INTERNAL
static ra8_err_t internal_failed_append(void* ctx, const uint8_t* bytes, size_t len)
{
  (void)ctx;
  (void)bytes;
  (void)len;
  return k_ra8_fail;
}

/**
 * @brief Report commit failure for an unavailable optional output.
 * @details Models an output transaction that could not be opened securely.
 * @param[in] ctx Unused null context.
 * @return Constant failure status.
 * @retval k_ra8_fail No output transaction exists.
 * @pre @p ctx is null.
 * @pre No stage descriptor is owned.
 * @post No filesystem object is created or changed.
 * @post The modeled transaction remains failed.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_failed_commit(void* ctx)
{
  (void)ctx;
  return k_ra8_fail;
}

/**
 * @brief Abort an output transaction that never began.
 * @details Supplies a complete transaction vtable after secure open failure.
 * @param[in] ctx Unused null context.
 * @pre @p ctx is null.
 * @pre No stage descriptor is owned.
 * @post No filesystem object is created or changed.
 * @post Repeated calls remain harmless.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_failed_abort(void* ctx)
{
  (void)ctx;
}

static const ra8_fmt_transaction_ops_t s_failed_transaction_ops = {
  .append = internal_failed_append,
  .commit = internal_failed_commit,
  .abort  = internal_failed_abort,
};

/**
 * @brief Bind phase-overlaid producer and comparison arena views.
 * @details Maps validated exact offsets into the caller-owned composition root.
 * @param[in,out] root Shared composition workspace.
 * @param[in] need Exact verifier requirements.
 * @param[in] layout Validated exact offsets.
 * @param[out] out Receives every engine arena view.
 * @pre Every pointer argument is non-null.
 * @pre @p layout total fits @p root storage.
 * @post All spans lie within layout total bytes.
 * @post No workspace byte is initialized or allocated.
 * @note Spans overlap only across non-concurrent phases.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_bind(ra8_fmt_cli_workspace_t*                 root,
                          const ra8_fmt_jof_verify_requirements_t* need,
                          const verify_layout_t*                   layout,
                          ra8_fmt_jof_verify_workspace_t*          out)
{
  *out = (ra8_fmt_jof_verify_workspace_t){
    .work          = root->bytes,
    .work_cap      = (uint32_t)layout->producer,
    .webp_work     = (need->webp_work_bytes == 0U) ? nullptr : &root->bytes[layout->webp],
    .webp_work_cap = need->webp_work_bytes,
    .band_tile     = root->bytes,
    .band_tile_cap = need->band_tile_bytes,
    .scratch       = &root->bytes[layout->scratch],
    .scratch_cap   = need->scratch_bytes,
    .row           = &root->bytes[layout->row],
    .row_cap       = need->row_bytes,
  };
}

/**
 * @brief Close all verifier-owned source and spool descriptors.
 * @details Performs idempotent cleanup in scratch-then-source order.
 * @param[in,out] ref Reference source state.
 * @param[in,out] got Subject source state.
 * @param[in,out] ref_spool Reference scratch state, optionally null.
 * @param[in,out] got_spool Subject scratch state, optionally null.
 * @pre Non-null states were initialized closed or successfully opened.
 * @pre No callback is executing through the states.
 * @post Every owned descriptor is closed.
 * @post Repeated cleanup leaves all states closed.
 * @note Sequential composition-root cleanup only.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_cleanup(ra8_fmt_host_source_t* ref,
                             ra8_fmt_host_source_t* got,
                             ra8_fmt_host_spool_t*  ref_spool,
                             ra8_fmt_host_spool_t*  got_spool)
{
  ra8_fmt_host_spool_close(ref_spool);
  ra8_fmt_host_spool_close(got_spool);
  ra8_fmt_host_source_close(ref);
  ra8_fmt_host_source_close(got);
}

/**
 * @brief Run the fully bound portable verifier engine.
 * @details Adapts host-source owners to portable source views without new ownership.
 * @param[in] ref First source context.
 * @param[in] got Second source context.
 * @param[in] need Exact requirements.
 * @param[in,out] work Phase-overlaid arena views.
 * @param[in,out] ref_spool Reference scratch binding.
 * @param[in,out] got_spool Subject scratch binding.
 * @param[in,out] dump Optional PPM transaction.
 * @param[in] dump_name Optional PPM spelling.
 * @param[in] report Standard-output report sink.
 * @return Engine status.
 * @retval k_ra8_ok The complete comparison was exact.
 * @retval other Producer, decoder, stability, or comparison status.
 * @pre All required source, spool, workspace, and report bindings are valid.
 * @pre Optional dump and name are either both present or both absent.
 * @post Engine-owned transaction state is committed or aborted.
 * @post Host descriptor ownership remains with the caller.
 * @note Thread safety inherits independent bound contexts.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_run(const ra8_fmt_host_source_t*             ref,
                              const ra8_fmt_host_source_t*             got,
                              const ra8_fmt_jof_verify_requirements_t* need,
                              ra8_fmt_jof_verify_workspace_t*          work,
                              ra8_fmt_spool_t*                         ref_spool,
                              ra8_fmt_spool_t*                         got_spool,
                              ra8_fmt_transaction_t*                   dump,
                              const char*                              dump_name,
                              const ra8_fmt_sink_t*                    report)
{
  return ra8_fmt_jof_verify_stream(&ref->source,
                                   &got->source,
                                   need,
                                   work,
                                   ref_spool,
                                   got_spool,
                                   dump,
                                   dump_name,
                                   report);
}

/**
 * @brief Bind host spools and optional output, run, and close every owner.
 * @details Creates anonymous sibling spools and a durable optional transaction.
 * @param[in] args Valid portable verify arguments.
 * @param[in,out] workspace Shared composition arena.
 * @param[in,out] ref_source Open reference source, always closed here.
 * @param[in,out] got_source Open subject source, always closed here.
 * @param[in] need Exact verifier requirements.
 * @param[in] layout Validated arena layout.
 * @param[in] errors Standard-error sink.
 * @param[in] report Standard-output sink.
 * @return Portable CLI status.
 * @retval 0 Verification completed exactly.
 * @retval 1 Spool, output, producer, decoder, or comparison failed.
 * @pre Every required pointer and sink binding is valid.
 * @pre Both source owners are open independent descriptors.
 * @post Every source, spool, and still-active transaction is closed.
 * @post Output is published only after complete comparison and stability checks.
 * @note Single-threaded composition root; engine contexts remain injectable.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_execute(const verify_cli_args_t*                 args,
                            ra8_fmt_cli_workspace_t*                 workspace,
                            ra8_fmt_host_source_t*                   ref_source,
                            ra8_fmt_host_source_t*                   got_source,
                            const ra8_fmt_jof_verify_requirements_t* need,
                            const verify_layout_t*                   layout,
                            const ra8_fmt_sink_t*                    errors,
                            const ra8_fmt_sink_t*                    report)
{
  ra8_fmt_host_spool_t ref_host_spool = {.fd = -1};
  ra8_fmt_host_spool_t got_host_spool = {.fd = -1};
  ra8_fmt_spool_t      ref_spool      = {};
  ra8_fmt_spool_t      got_spool      = {};
  ra8_err_t            rc = ra8_fmt_host_spool_open(args->input, &ref_host_spool, &ref_spool);
  if (rc == k_ra8_ok) {
    rc = ra8_fmt_host_spool_open(args->input, &got_host_spool, &got_spool);
  }
  if (rc != k_ra8_ok) {
    internal_status(errors, "ra8_fmt: cannot create verify spool (rc=", rc);
    internal_cleanup(ref_source, got_source, &ref_host_spool, &got_host_spool);
    return (int)k_verify_cli_fail;
  }
  ra8_fmt_host_transaction_t host_dump = {.parent_fd = -1, .stage_fd = -1};
  ra8_fmt_transaction_t      dump      = {};
  ra8_fmt_transaction_t*     dump_ptr  = nullptr;
  if (args->output != nullptr) {
    rc = ra8_fmt_host_transaction_begin(args->output, &host_dump, &dump);
    if (rc != k_ra8_ok) {
      dump = (ra8_fmt_transaction_t){.ops = &s_failed_transaction_ops, .ctx = nullptr};
    }
    dump_ptr = &dump;
  }
  ra8_fmt_jof_verify_workspace_t work;
  internal_bind(workspace, need, layout, &work);
  rc = internal_run(ref_source,
                    got_source,
                    need,
                    &work,
                    &ref_spool,
                    &got_spool,
                    dump_ptr,
                    args->output,
                    report);
  if (host_dump.active) {
    dump.ops->abort(dump.ctx);
  }
  internal_cleanup(ref_source, got_source, &ref_host_spool, &got_host_spool);
  return (rc == k_ra8_ok) ? (int)k_verify_cli_ok : (int)k_verify_cli_fail;
}

int ra8_fmt_try_portable_verify(int                      argc,
                                char**                   argv,
                                ra8_fmt_cli_workspace_t* workspace,
                                bool*                    handled)
{
  if ((handled == nullptr) || (workspace == nullptr)) {
    return (int)k_verify_cli_fail;
  }
  *handled = false;
  if ((argc < 2) || (strcmp(argv[1], "verify") != 0)) {
    return (int)k_verify_cli_ok;
  }
  verify_cli_args_t args = {};
  if (!internal_parse(argc, argv, &args) || (args.format == nullptr) ||
      (strcmp(args.format, "jof") != 0) || (args.input == nullptr)) {
    return (int)k_verify_cli_ok;
  }
  *handled                            = true;
  ra8_fmt_host_fd_sink_t error_state  = {.fd = STDERR_FILENO};
  ra8_fmt_host_fd_sink_t report_state = {.fd = STDOUT_FILENO};
  const ra8_fmt_sink_t   errors       = ra8_fmt_host_fd_sink(&error_state);
  const ra8_fmt_sink_t   report       = ra8_fmt_host_fd_sink(&report_state);
  ra8_fmt_host_source_t  ref_source   = {.fd = -1};
  ra8_fmt_host_source_t  got_source   = {.fd = -1};
  ra8_err_t              rc = ra8_fmt_host_source_open(args.input, k_verify_cli_input, &ref_source);
  if (rc == k_ra8_ok) {
    rc = ra8_fmt_host_source_open(args.input, k_verify_cli_input, &got_source);
  }
  if ((rc == k_ra8_ok) && (!ra8_fmt_host_sources_same(&ref_source, &got_source) ||
                           (ra8_fmt_host_source_unchanged(&ref_source) != k_ra8_ok) ||
                           (ra8_fmt_host_source_unchanged(&got_source) != k_ra8_ok))) {
    rc = k_ra8_err_validation_failed;
  }
  if (rc != k_ra8_ok) {
    internal_status(&errors, "ra8_fmt: cannot open verify input (rc=", rc);
    internal_cleanup(&ref_source, &got_source, nullptr, nullptr);
    return (int)k_verify_cli_fail;
  }
  ra8_fmt_jof_verify_requirements_t need = {};
  rc                     = ra8_fmt_jof_verify_requirements(&ref_source.source, &need);
  verify_layout_t layout = {};
  const bool      sized  = (rc == k_ra8_ok) && internal_layout(&need, &layout);
  if ((rc == k_ra8_ok) && (!sized || (layout.total > sizeof(workspace->bytes)))) {
    internal_capacity(&errors, &need, &layout, sizeof(workspace->bytes));
    rc = k_ra8_err_invalid_size;
  }
  if (rc != k_ra8_ok) {
    internal_status(&report, "verify: cannot read source dimensions (rc=", rc);
    internal_cleanup(&ref_source, &got_source, nullptr, nullptr);
    return (int)k_verify_cli_fail;
  }
  return internal_execute(&args,
                          workspace,
                          &ref_source,
                          &got_source,
                          &need,
                          &layout,
                          &errors,
                          &report);
}
