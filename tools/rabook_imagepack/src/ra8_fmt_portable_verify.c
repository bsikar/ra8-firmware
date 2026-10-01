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

#include "ra8_arena.h"
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

/** @brief Slot capacity of each overlaid workspace phase. */
typedef enum : uint32_t {
  k_verify_producer_slots = 2U, /**< Producer work and optional WebP arena. */
  k_verify_compare_slots  = 3U, /**< Band tile, stored scratch, and row.    */
} verify_slot_count_t;

/** @brief Parsed legacy-compatible JOF verify selections. */
typedef struct {
  const char* input;  /**< Encoded source path. */
  const char* output; /**< Optional PPM path.   */
  const char* format; /**< Explicit format.     */
} verify_cli_args_t;

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
 * @brief Declare one workspace span as an arena slot, or as an absent span.
 * @details A requirement of zero bytes is not carved at all: the matching
 * workspace pointer is published as null with a zero cap, which is how this
 * struct has always spelled an absent WebP arena. Every present span takes
 * the same ::k_verify_cli_align boundary the hand-written offset chain used.
 * @param[in,out] slots Slot array being filled, with room for one more.
 * @param[in] count Slots already declared.
 * @param[in] bytes Requirement for this span, possibly zero.
 * @param[out] out_ptr Receives the carved span, or null when @p bytes is zero.
 * @return The new slot count.
 * @retval count @p bytes was zero and no slot was added.
 * @pre @p slots has capacity for @p count + 1 entries.
 * @pre @p out_ptr is writable.
 * @post A zero requirement writes null through @p out_ptr immediately.
 * @post A non-zero requirement leaves @p out_ptr untouched until the carve.
 * @note Pure apart from caller output.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t
internal_slot(ra8_arena_slot_t* slots, uint32_t count, uint32_t bytes, void** out_ptr)
{
  if (bytes == 0U) {
    *out_ptr = nullptr;
    return count;
  }
  slots[count] = (ra8_arena_slot_t){
    .bytes   = bytes,
    .align   = (uint32_t)k_verify_cli_align,
    .out_ptr = out_ptr,
  };
  return count + 1U;
}

/**
 * @brief Carve both overlaid verify phases out of the shared CLI block.
 * @details Declares one slot per workspace member and lets the platform arena
 * place them, replacing a hand-written offset chain and the struct that held
 * it. The producer and comparison phases are mutually exclusive, so they are
 * carved as two passes over one arena with ::ra8_arena_reset between them:
 * the overlay at byte zero is now what the reset means rather than an aliased
 * pointer written by hand. Each pass is all-or-none, so a block too small for
 * its last span publishes no pointer at all.
 * @param[in,out] root Shared composition-root storage.
 * @param[in] need Exact verifier requirements.
 * @param[out] out Receives every engine arena view.
 * @return Arena status.
 * @retval k_ra8_ok Every present span was carved and published.
 * @retval k_ra8_err_no_mem One phase did not fit the shared block.
 * @retval other An arena argument was rejected.
 * @pre Every pointer argument is non-null.
 * @post Success publishes spans that are disjoint within each phase.
 * @post Failure leaves every carved pointer in @p out unpublished.
 * @note Not thread-safe; the CLI is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_carve(ra8_fmt_cli_workspace_t*                 root,
                                const ra8_fmt_jof_verify_requirements_t* need,
                                ra8_fmt_jof_verify_workspace_t*          out)
{
  const uint32_t producer_bytes = (need->reference_work_bytes > need->banded_work_bytes)
                                    ? need->reference_work_bytes
                                    : need->banded_work_bytes;
  *out                          = (ra8_fmt_jof_verify_workspace_t){
                             .work_cap      = producer_bytes,
                             .webp_work_cap = need->webp_work_bytes,
                             .band_tile_cap = need->band_tile_bytes,
                             .scratch_cap   = need->scratch_bytes,
                             .row_cap       = need->row_bytes,
  };
  void*            work      = nullptr;
  void*            webp      = nullptr;
  void*            band_tile = nullptr;
  void*            scratch   = nullptr;
  void*            row       = nullptr;
  ra8_arena_slot_t producer[k_verify_producer_slots] = {};
  uint32_t         producer_count = internal_slot(producer, 0U, producer_bytes, &work);
  producer_count = internal_slot(producer, producer_count, need->webp_work_bytes, &webp);
  ra8_arena_slot_t compare[k_verify_compare_slots] = {};
  uint32_t         compare_count = internal_slot(compare, 0U, need->band_tile_bytes, &band_tile);
  compare_count = internal_slot(compare, compare_count, need->scratch_bytes, &scratch);
  compare_count = internal_slot(compare, compare_count, need->row_bytes, &row);

  ra8_arena_t arena = {};
  ra8_err_t   rc    = ra8_arena_init(&arena, root->bytes, (uint32_t)sizeof root->bytes);
  if ((rc == k_ra8_ok) && (producer_count > 0U)) {
    rc = ra8_arena_carve_all(&arena, producer, producer_count);
  }
  if (rc == k_ra8_ok) {
    rc = ra8_arena_reset(&arena);
  }
  if ((rc == k_ra8_ok) && (compare_count > 0U)) {
    rc = ra8_arena_carve_all(&arena, compare, compare_count);
  }
  if (rc != k_ra8_ok) {
    return rc;
  }
  out->work      = (uint8_t*)work;
  out->webp_work = (uint8_t*)webp;
  out->band_tile = (uint8_t*)band_tile;
  out->scratch   = (uint8_t*)scratch;
  out->row       = (uint8_t*)row;
  return k_ra8_ok;
}

/**
 * @brief Report the supplied capacity and every shared-workspace component.
 * @details Emits each phase component the arena was asked to place. The exact
 * high-water is no longer spelled here: the offsets belong to the arena now,
 * and a carve that did not fit published no placement to report.
 * @param[in] errors Standard-error sink.
 * @param[in] need Exact verifier requirements.
 * @param[in] supplied Caller workspace capacity.
 * @pre Every pointer argument is valid.
 * @pre @p need came from the bounded planner.
 * @post Best effort emits one bounded diagnostic line.
 * @post Workspace and requirements remain unchanged.
 * @note Sink failures are intentionally ignored after first failure.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_capacity(const ra8_fmt_sink_t*                    errors,
                              const ra8_fmt_jof_verify_requirements_t* need,
                              size_t                                   supplied)
{
  const uint32_t producer = (need->reference_work_bytes > need->banded_work_bytes)
                              ? need->reference_work_bytes
                              : need->banded_work_bytes;
  ra8_err_t rc = internal_text(errors, "ra8_fmt: JOF verify workspace too small: supplied ");
  internal_field(errors, supplied, " (producer ", &rc);
  internal_field(errors, producer, ", webp ", &rc);
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
  priv_fmt_host_spool_close(ref_spool);
  priv_fmt_host_spool_close(got_spool);
  priv_fmt_host_source_close(ref);
  priv_fmt_host_source_close(got);
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
 * @param[in,out] ref_source Open reference source, always closed here.
 * @param[in,out] got_source Open subject source, always closed here.
 * @param[in] need Exact verifier requirements.
 * @param[in,out] work Phase-overlaid arena views already carved from @p workspace.
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
                            ra8_fmt_host_source_t*                   ref_source,
                            ra8_fmt_host_source_t*                   got_source,
                            const ra8_fmt_jof_verify_requirements_t* need,
                            ra8_fmt_jof_verify_workspace_t*          work,
                            const ra8_fmt_sink_t*                    errors,
                            const ra8_fmt_sink_t*                    report)
{
  ra8_fmt_host_spool_t ref_host_spool = {.fd = -1};
  ra8_fmt_host_spool_t got_host_spool = {.fd = -1};
  ra8_fmt_spool_t      ref_spool      = {};
  ra8_fmt_spool_t      got_spool      = {};
  ra8_err_t            rc = priv_fmt_host_spool_open(args->input, &ref_host_spool, &ref_spool);
  if (rc == k_ra8_ok) {
    rc = priv_fmt_host_spool_open(args->input, &got_host_spool, &got_spool);
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
    rc = priv_fmt_host_transaction_begin(args->output, &host_dump, &dump);
    if (rc != k_ra8_ok) {
      dump = (ra8_fmt_transaction_t){.ops = &s_failed_transaction_ops, .ctx = nullptr};
    }
    dump_ptr = &dump;
  }
  rc = internal_run(ref_source,
                    got_source,
                    need,
                    work,
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

/**
 * @brief Open both verify sources and carve the shared workspace.
 * @details Opens the reference and comparison file descriptors on the same
 * input, confirms they observe the identical unchanged file, derives the JOF
 * verify requirements, then asks the platform arena to place every span.
 * @param[in] args Parsed CLI arguments (input path).
 * @param[in,out] workspace Shared composition-root storage.
 * @param[out] ref_source Opened reference-pass source.
 * @param[out] got_source Opened comparison-pass source.
 * @param[out] need Derived JOF verify requirements.
 * @param[out] work Receives every carved engine arena view.
 * @param[in] errors Sink for open/validation diagnostics.
 * @param[in] report Sink for capacity diagnostics.
 * @return Open/sizing status.
 * @retval k_ra8_ok Both sources are open, identical, unchanged, and sized.
 * @retval other Open, identity, sizing, or carve validation failed
 * (already reported and cleaned up).
 * @pre @p args->input names a readable file.
 * @pre Every output pointer and both sink bindings are valid and independent.
 * @post On failure both sources are closed and no partial state escapes.
 * @post On success both open sources pass to the caller, which must close them.
 * @note Not thread-safe with respect to concurrent mutation of the input.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_open_and_size(const verify_cli_args_t* args,
                                                     ra8_fmt_cli_workspace_t* workspace,
                                                     ra8_fmt_host_source_t*   ref_source,
                                                     ra8_fmt_host_source_t*   got_source,
                                                     ra8_fmt_jof_verify_requirements_t* need,
                                                     ra8_fmt_jof_verify_workspace_t*    work,
                                                     const ra8_fmt_sink_t*              errors,
                                                     const ra8_fmt_sink_t*              report)
{
  ra8_err_t rc = priv_fmt_host_source_open(args->input, k_verify_cli_input, ref_source);
  if (rc == k_ra8_ok) {
    rc = priv_fmt_host_source_open(args->input, k_verify_cli_input, got_source);
  }
  if ((rc == k_ra8_ok) && (!priv_fmt_host_sources_same(ref_source, got_source) ||
                           (priv_fmt_host_source_unchanged(ref_source) != k_ra8_ok) ||
                           (priv_fmt_host_source_unchanged(got_source) != k_ra8_ok))) {
    rc = k_ra8_err_validation_failed;
  }
  if (rc != k_ra8_ok) {
    internal_status(errors, "ra8_fmt: cannot open verify input (rc=", rc);
    internal_cleanup(ref_source, got_source, nullptr, nullptr);
    return rc;
  }
  rc = ra8_fmt_jof_verify_requirements(&ref_source->source, need);
  if (rc == k_ra8_ok) {
    rc = internal_carve(workspace, need, work);
    if (rc != k_ra8_ok) {
      internal_capacity(errors, need, sizeof workspace->bytes);
      rc = k_ra8_err_invalid_size;
    }
  }
  if (rc != k_ra8_ok) {
    internal_status(report, "verify: cannot read source dimensions (rc=", rc);
    internal_cleanup(ref_source, got_source, nullptr, nullptr);
  }
  return rc;
}

RA8_PRIV int priv_fmt_try_portable_verify(int                      argc,
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
  *handled                                       = true;
  ra8_fmt_host_fd_sink_t            error_state  = {.fd = STDERR_FILENO};
  ra8_fmt_host_fd_sink_t            report_state = {.fd = STDOUT_FILENO};
  const ra8_fmt_sink_t              errors       = priv_fmt_host_fd_sink(&error_state);
  const ra8_fmt_sink_t              report       = priv_fmt_host_fd_sink(&report_state);
  ra8_fmt_host_source_t             ref_source   = {.fd = -1};
  ra8_fmt_host_source_t             got_source   = {.fd = -1};
  ra8_fmt_jof_verify_requirements_t need         = {};
  ra8_fmt_jof_verify_workspace_t    work         = {};
  const ra8_err_t                   rc           = internal_open_and_size(&args,
                                                                          workspace,
                                                                          &ref_source,
                                                                          &got_source,
                                                                          &need,
                                                                          &work,
                                                                          &errors,
                                                                          &report);
  if (rc != k_ra8_ok) {
    return (int)k_verify_cli_fail;
  }
  return internal_execute(&args, &ref_source, &got_source, &need, &work, &errors, &report);
}
