/**
 * @file main.c
 * @brief `ra8_fmt` command-line entry: parse the verb + options, then dispatch.
 *
 * @details
 * Three verbs over the ::ra8_fmt_desc_t registry:
 *
 * ```
 *   ra8_fmt convert --format jof --in page.jpg --out page.jof
 *   ra8_fmt inspect page.jof [--verbose]
 *   ra8_fmt verify  --format jof --in page.jpg [--out recon.ppm]
 * ```
 *
 * `inspect` auto-detects the format from the container magic, so `--format` is
 * only required where the input is a source file rather than a container
 * (`convert`, `verify`).
 *
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 *
 *

 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_arena.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fmt.h"
#include "ra8_log.h"

enum : uint32_t { k_arena_bytes = 4U * 1024U * 1024U };
static uint8_t*    s_arena_buf;
static ra8_arena_t s_arena;

RA8_NASA_RULE_3_OK
static ra8_err_t init_arena(void)
{
  s_arena_buf = (uint8_t*)malloc(k_arena_bytes);
  if (s_arena_buf == nullptr)
    return k_ra8_err_no_mem;
  return ra8_arena_init(&s_arena, s_arena_buf, k_arena_bytes);
}

/**
 * @enum ra8_fmt_exit_t
 * @brief Process exit codes.
 * @details Distinguishing usage errors from verdict failures lets a script tell
 *          "I invoked it wrong" from "the file is bad".
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_fmt_exit_ok    = 0U, /**< Verb succeeded / container valid. */
  k_fmt_exit_usage = 2U, /**< Bad command line.                 */
  k_fmt_exit_fail  = 1U, /**< Verb ran and reported a failure.  */
} ra8_fmt_exit_t;

/**
 * @enum ra8_fmt_verb_t
 * @brief The verb selected on the command line.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_fmt_verb_none    = 0U, /**< No recognised verb given. */
  k_fmt_verb_convert = 1U, /**< Source file -> container. */
  k_fmt_verb_inspect = 2U, /**< Dump container structure. */
  k_fmt_verb_verify  = 3U, /**< Round-trip comparison.    */
} ra8_fmt_verb_t;

/**
 * @brief ra8_log byte sink -> stderr (host-safe; avoids the ITM MMIO write).
 *
 * @details
 * Registered with ::ra8_log_set_byte_sink so the shared ra8_log backend emits
 * to stderr on the host instead of the on-target ITM stimulus port, whose MMIO
 * write would fault under this command-line tool. One byte per call, matching
 * the byte-sink contract.
 *
 * @param[in] ctx  Unused sink context (the sink keeps no state).
 * @param[in] byte The log byte to emit.
 *
 * @pre ra8_log has been pointed at this sink via ::ra8_log_set_byte_sink.
 * @pre stderr is open.
 * @post @p byte has been written to stderr.
 * @post No other state is mutated.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL
static void fmt_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)fputc((int)byte, stderr);
}

/**
 * @brief Print the usage banner, listing every registered format.
 * @details Driven off the registry so a newly added format documents itself.
 * @param[in] out Stream to write the banner to (non-NULL).
 * @pre @p out is an open stream.
 * @pre The registry is statically initialised.
 * @post The banner and one line per format were written.
 * @post No state other than the stream is mutated.
 * @note Not thread-safe (writes one stream).
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_usage(FILE* out)
{
  (void)fprintf(out,
                "usage:\n"
                "  ra8_fmt convert --format <fmt> --in <file> --out <file>\n"
                "  ra8_fmt inspect <container> [--verbose]\n"
                "  ra8_fmt verify  --format <fmt> --in <file> [--out <dump.ppm>]\n"
                "\nformats:\n");
  size_t                n    = 0U;
  const ra8_fmt_desc_t* regs = ra8_fmt_registry(&n);
  for (size_t i = 0U; i < n; ++i) { /* bounded by the registry size */
    (void)fprintf(out,
                  "  %-8s %-9s %s%s%s%s\n",
                  regs[i].name,
                  regs[i].ext,
                  (regs[i].convert != nullptr) ? "[convert] " : "",
                  (regs[i].inspect != nullptr) ? "[inspect] " : "",
                  (regs[i].verify != nullptr) ? "[verify] " : "",
                  regs[i].summary);
  }
}

/**
 * @brief Map the first argument to a ::ra8_fmt_verb_t.
 * @details Exact string match against the three verb spellings; anything else
 *          (including a flag given where a verb is expected) yields
 *          ::k_fmt_verb_none so the caller falls through to printing usage.
 * @param[in] arg Argument to match (non-NULL).
 * @return The matching verb, or ::k_fmt_verb_none.
 * @retval k_fmt_verb_none @p arg is not a recognised verb.
 * @pre @p arg is a NUL-terminated string.
 * @pre The caller handles ::k_fmt_verb_none by printing usage.
 * @post No state is mutated.
 * @post The result depends only on @p arg.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_fmt_verb_t priv_parse_verb(const char* arg)
{
  if (strcmp(arg, "convert") == 0) {
    return k_fmt_verb_convert;
  }
  if (strcmp(arg, "inspect") == 0) {
    return k_fmt_verb_inspect;
  }
  if (strcmp(arg, "verify") == 0) {
    return k_fmt_verb_verify;
  }
  return k_fmt_verb_none;
}

/**
 * @brief Parse the option arguments following the verb.
 * @details Accepts `--format`, `--in`, `--out` and `--verbose`; the first bare
 *          argument is taken as the input path so `inspect <file>` works.
 * @param[in]     argc Argument count.
 * @param[in]     argv Argument vector.
 * @param[in,out] opts Options to populate (non-NULL).
 * @param[out]    fmt  Receives the `--format` selector, or NULL when absent.
 * @return `true` when every argument was recognised, else `false`.
 * @retval false An unknown flag or a flag missing its value was found.
 * @pre @p opts and @p fmt are writable.
 * @pre @p argv holds @p argc entries.
 * @post On success `opts->in_path` is set when an input was supplied.
 * @post On failure the caller prints usage and exits.
 * @note Not thread-safe (writes @p opts).
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_parse_opts(int argc, char** argv, ra8_fmt_opts_t* opts, const char** fmt)
{
  for (int i = 2; i < argc; ++i) { /* bounded by argc */
    if ((strcmp(argv[i], "--verbose") == 0) || (strcmp(argv[i], "-v") == 0)) {
      opts->verbose = true;
    } else if ((strcmp(argv[i], "--format") == 0) && ((i + 1) < argc)) {
      ++i;
      *fmt = argv[i];
    } else if ((strcmp(argv[i], "--in") == 0) && ((i + 1) < argc)) {
      ++i;
      opts->in_path = argv[i];
    } else if ((strcmp(argv[i], "--out") == 0) && ((i + 1) < argc)) {
      ++i;
      opts->out_path = argv[i];
    } else if ((argv[i][0] != '-') && (opts->in_path == nullptr)) {
      opts->in_path = argv[i];
    } else {
      (void)fprintf(stderr, "ra8_fmt: unrecognised argument '%s'\n", argv[i]);
      return false;
    }
  }
  return true;
}

/**
 * @brief Resolve the descriptor for a verb, by name or by sniffing the input.
 * @param[in] arena Scratch arena for temporary allocations.
 * @param[in] verb Selected verb.
 * @param[in] name `--format` selector, or NULL.
 * @param[in] src  Slurped input (used for sniffing on `inspect`).
 * @return The resolved descriptor, or NULL when it could not be determined.
 * @retval NULL No `--format` given and the magic matched no known format.
 * @pre @p src holds the slurped input file.
 * @pre @p verb is a recognised verb.
 * @post No state is mutated.
 * @post A non-NULL result points into the static registry.
 * @note Thread-safe (pure over immutable data).
 * @since 0.1.0
 */
RA8_INTERNAL
static const ra8_fmt_desc_t*
priv_resolve(ra8_fmt_verb_t verb, const char* name, const ra8_fmt_blob_t* src)
{
  if (name != nullptr) {
    return ra8_fmt_find(name);
  }
  return (verb == k_fmt_verb_inspect) ? ra8_fmt_sniff(src) : nullptr;
}

/**
 * @brief Invoke the verb's entry point on the descriptor, or report absence.
 * @details Selects the descriptor's convert / inspect / verify function pointer
 *          for @p verb and calls it; a NULL slot means this format does not
 *          implement the verb, which is reported to `opts->report` and returned
 *          as ::k_ra8_err_not_supported rather than dereferenced.
 * @param[in] arena Scratch arena for temporary allocations.
 * @param[in] verb Selected verb.
 * @param[in] desc Resolved format descriptor (non-NULL).
 * @param[in] src  Slurped input.
 * @param[in] opts Parsed options.
 * @return The verb's result, or ::k_ra8_err_not_supported when unimplemented.
 * @retval k_ra8_err_not_supported This format does not implement this verb.
 * @pre @p desc points into the registry.
 * @pre @p src holds the slurped input file.
 * @post The verb's own reporting was written to `opts->report`.
 * @post No state other than the report stream is mutated here.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_dispatch(ra8_arena_t*          arena,
                               ra8_fmt_verb_t        verb,
                               const ra8_fmt_desc_t* desc,
                               const ra8_fmt_blob_t* src,
                               const ra8_fmt_opts_t* opts)
{
  ra8_fmt_convert_fn fn = nullptr;
  if (verb == k_fmt_verb_convert) {
    fn = desc->convert;
  } else if (verb == k_fmt_verb_inspect) {
    fn = desc->inspect;
  } else {
    fn = desc->verify;
  }
  if (fn == nullptr) {
    (void)fprintf(opts->report, "ra8_fmt: format '%s' does not support this verb\n", desc->name);
    return k_ra8_err_not_supported;
  }
  return fn(arena, src, opts);
}

int main(int argc, char** argv)
{
  ra8_log_set_byte_sink(fmt_log_sink, nullptr);
  if (init_arena() != k_ra8_ok) {
    (void)fprintf(stderr, "ra8_fmt: failed to initialize arena\n");
    return (int)k_fmt_exit_fail;
  }

  if (argc < 2) {
    priv_usage(stderr);
    return (int)k_fmt_exit_usage;
  }
  const ra8_fmt_verb_t verb = priv_parse_verb(argv[1]);
  if (verb == k_fmt_verb_none) {
    priv_usage(stderr);
    return (int)k_fmt_exit_usage;
  }
  ra8_fmt_opts_t opts = {.in_path  = nullptr,
                         .out_path = nullptr,
                         .verbose  = false,
                         .report   = stdout};
  const char*    fmt  = nullptr;
  if (!priv_parse_opts(argc, argv, &opts, &fmt)) {
    priv_usage(stderr);
    return (int)k_fmt_exit_usage;
  }
  if (opts.in_path == nullptr) {
    (void)fprintf(stderr, "ra8_fmt: no input file given\n");
    priv_usage(stderr);
    return (int)k_fmt_exit_usage;
  }
  ra8_fmt_blob_t  src    = {};
  const ra8_err_t src_rc = ra8_fmt_slurp(&s_arena, opts.in_path, &src);
  if (src_rc != k_ra8_ok) {
    (void)fprintf(stderr, "ra8_fmt: cannot read '%s' (rc=%d)\n", opts.in_path, (int)src_rc);
    return (int)k_fmt_exit_fail;
  }
  const ra8_fmt_desc_t* desc = priv_resolve(verb, fmt, &src);
  if (desc == nullptr) {
    (void)fprintf(stderr, "ra8_fmt: unknown or undetectable format; pass --format\n");
    ra8_fmt_blob_free(&src);
    priv_usage(stderr);
    return (int)k_fmt_exit_usage;
  }
  const ra8_err_t rc = priv_dispatch(&s_arena, verb, desc, &src, &opts);
  ra8_fmt_blob_free(&src);
  return (rc == k_ra8_ok) ? (int)k_fmt_exit_ok : (int)k_fmt_exit_fail;
}
