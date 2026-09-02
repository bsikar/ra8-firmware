/**
 * @file src/cache_bench_report.c
 * @brief #147 benchmark front end: swept-capacity report, mode dispatch, main.
 *
 * @details
 * Drives the replay engine in src/cache_bench.c across the RAM-budget axis and
 * prints the markdown report the #147 decision record quotes: one hit-rate
 * matrix per workload (policies x cache sizes), then a cross-workload summary
 * carrying the worst per-eviction scan depth (a WCET proxy) and each policy's
 * per-frame metadata cost. It also owns the command line -- the optional
 * `<name>=<path>` captured traces, `--output=`, and the `--sweep-block`
 * composition root that hands the fixed cache backing and the scratch seam to
 * ::cb_sweep_block -- plus the process entry point. Every engine call goes
 * through the public inc/cache_bench.h surface, which is why the contract test
 * links the engine without this driver.
 *
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#include <string.h>

#include "cache_bench.h"
#include "cache_bench_host.h"
#include "cache_bench_io.h"
#include "ra8_attributes.h"
#include "sweep_block.h"
#include "trace.h"

/**
 * @enum cb_bench_size_t
 * @brief Swept cache capacities (in frames) used on the RAM-budget axis.
 * @details These are the seven capacity points that the benchmark sweeps over.
 *          Each is a power of two chosen to cover the expected SRAM/SDRAM
 *          budget range for the RA8D2 page cache (#147 decision record).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cb_size_64   = 64U,   /**< Smallest evaluated capacity (frames).  */
  k_cb_size_128  = 128U,  /**< 128-frame sweep point.                 */
  k_cb_size_256  = 256U,  /**< Mid-budget representative sweep point. */
  k_cb_size_512  = 512U,  /**< 512-frame sweep point.                 */
  k_cb_size_1024 = 1024U, /**< 1 K-frame sweep point.                 */
  k_cb_size_2048 = 2048U, /**< Largest evaluated capacity (frames).   */
} cb_bench_size_t;

/** @brief Representative mid-budget capacity used in the summary table. */
typedef enum : uint32_t {
  k_cb_mid_cap = 256U, /**< Mid-point capacity (frames) for the summary view. */
} cb_mid_cap_t;

/** @brief Full scale used when computing a hit-rate percentage (integer form). */
typedef enum : uint32_t {
  k_cb_pct_scale = 100U, /**< Divisor to convert a ratio to a percentage. */
} cb_pct_scale_t;

/** @brief Floating-point 100.0 scale factor for hit-rate percentage output. */
static const double s_cb_pct_scale_f = 100.0; /**< double 100.0 for pct maths. */

/** @brief Swept cache capacities (frames) -- the RAM-budget axis. */
static const uint32_t s_cb_sizes[] = {
  32U,
  (uint32_t)k_cb_size_64,
  (uint32_t)k_cb_size_128,
  (uint32_t)k_cb_size_256,
  (uint32_t)k_cb_size_512,
  (uint32_t)k_cb_size_1024,
  (uint32_t)k_cb_size_2048,
};

/**
 * @brief Write the per-trace table header: title line, column heads, rule row.
 * @details Emits the "### name (...)" title, the "| policy |" column heading
 *          with one column per swept size in ::s_cb_sizes, and the markdown
 *          table rule row beneath it.
 * @param[in] tr Trace being reported (name, access count, footprint).
 * @param[in,out] sink Report destination.
 * @param[in] nsz Number of entries in ::s_cb_sizes to head one column each.
 * @pre @p tr is non-NULL.
 * @pre @p sink is non-NULL and accepts further writes.
 * @post The complete three-line markdown header is written to @p sink.
 * @post @p sink is left ready for one row per policy.
 * @return Zero after the complete header was accepted, otherwise one.
 * @retval 0 Every header fragment was accepted by @p sink.
 * @retval 1 A sink write failed.
 * @note Not thread-safe: writes @p sink. Benchmark thread only.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_report_trace_header(const cb_trace_t* tr, cb_sink_t* sink, uint32_t nsz)
{
  if (cb_sink_format(sink,
                     "\n### %s  (%llu accesses, footprint %u pages)\n\n",
                     tr->name,
                     (unsigned long long)tr->n,
                     tr->footprint) != k_cb_io_ok ||
      cb_sink_format(sink, "| policy |") != k_cb_io_ok) {
    return 1;
  }
  for (uint32_t s = 0U; s < nsz; ++s) {
    if (cb_sink_format(sink, " %u |", s_cb_sizes[s]) != k_cb_io_ok) {
      return 1;
    }
  }
  if (cb_sink_format(sink, "\n|--------|") != k_cb_io_ok) {
    return 1;
  }
  for (uint32_t s = 0U; s < nsz; ++s) {
    if (cb_sink_format(sink, "------|") != k_cb_io_ok) {
      return 1;
    }
  }
  return cb_sink_format(sink, "\n") != k_cb_io_ok ? 1 : 0;
}

/**
 * @brief Write one policy's hit-rate row across every swept cache size.
 * @details Replays @p tr once per entry in ::s_cb_sizes under @p policy,
 *          converting each result to a hit-rate percentage cell (0.0 when
 *          no accesses ran), and terminates the row with a newline.
 * @param[in] policy Policy under test for this row.
 * @param[in] tr Trace to replay (name, key stream, footprint).
 * @param[in,out] workspace Reusable exact replay workspace.
 * @param[in,out] sink Report destination.
 * @param[in] nsz Number of entries in ::s_cb_sizes to replay and print.
 * @pre @p policy and @p tr are non-NULL with valid operations.
 * @pre @p sink is non-NULL and accepts further writes.
 * @post One complete markdown row for @p policy is written to @p sink.
 * @post @p tr and @p policy are left unmodified (replays are self-contained).
 * @return Zero after the complete row was accepted, otherwise one.
 * @retval 0 Every cell was replayed and accepted by @p sink.
 * @retval 1 A replay or sink write failed.
 * @note Not thread-safe: writes @p sink and runs replays. Benchmark thread only.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_report_trace_row(const cache_policy_t* policy,
                                     const cb_trace_t*     tr,
                                     cb_workspace_t*       workspace,
                                     cb_sink_t*            sink,
                                     uint32_t              nsz)
{
  if (cb_sink_format(sink, "| %-14s |", policy->name) != k_cb_io_ok) {
    return 1;
  }
  for (uint32_t s = 0U; s < nsz; ++s) {
    cb_result_t r = {};
    if (cb_replay(policy, tr, s_cb_sizes[s], workspace, &r) != 0) {
      return 1;
    }
    const double hit =
      (r.accesses == 0U) ? 0.0 : (s_cb_pct_scale_f * (double)r.hits / (double)r.accesses);
    if (cb_sink_format(sink, " %5.1f |", hit) != k_cb_io_ok) {
      return 1;
    }
  }
  return cb_sink_format(sink, "\n") != k_cb_io_ok ? 1 : 0;
}

/**
 * @brief Print the per-trace hit-rate matrix (policies x cache sizes).
 * @details Emits a markdown section for @p tr: a header naming the workload,
 *          then one row per registered policy giving its hit-rate percentage at
 *          each swept capacity in ::s_cb_sizes. Each cell is produced by a full
 *          ::cb_replay of the trace at that size (0.0 when no accesses ran).
 * @param[in] tr Trace to report (name, key stream, footprint).
 * @param[in,out] workspace Reusable exact replay workspace.
 * @param[in,out] sink Report destination.
 * @pre @p tr is non-NULL with valid reset and next operations.
 * @pre ::g_cb_policies / ::g_cb_policy_count are initialized.
 * @post One markdown table for @p tr is written to @p sink.
 * @post @p tr and every policy are left unmodified (replays are self-contained).
 * @return Zero after complete publication, otherwise one.
 * @retval 0 Every table fragment was accepted by @p sink.
 * @retval 1 A replay or sink operation failed.
 * @note Not thread-safe: writes @p sink and runs replays. Benchmark thread only.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_report_trace(const cb_trace_t* tr, cb_workspace_t* workspace, cb_sink_t* sink)
{
  const uint32_t nsz = (uint32_t)(sizeof(s_cb_sizes) / sizeof(s_cb_sizes[0]));
  if (internal_report_trace_header(tr, sink, nsz) != 0) {
    return 1;
  }
  for (uint32_t p = 0U; p < g_cb_policy_count; ++p) {
    if (internal_report_trace_row(g_cb_policies[p], tr, workspace, sink, nsz) != 0) {
      return 1;
    }
  }
  return 0;
}

/**
 * @brief Print the cross-workload summary (WCET + metadata + mean hit rate).
 * @details For each policy, replays every trace at the fixed mid-budget
 *          capacity ::k_cb_mid_cap, then prints the mean hit rate across
 *          workloads, the worst per-eviction scan depth seen (a WCET proxy),
 *          and the policy's per-frame metadata cost.
 * @param[in] traces Array of @p ntr workloads to average over.
 * @param[in] ntr    Number of traces in @p traces (> 0).
 * @param[in,out] workspace Reusable exact replay workspace.
 * @param[in,out] sink Report destination.
 * @pre @p traces is non-NULL with @p ntr valid entries.
 * @pre ::g_cb_policies / ::g_cb_policy_count are initialized.
 * @post One markdown summary table is written to @p sink.
 * @post No trace or policy state is mutated by the reporting.
 * @return Zero after complete publication, otherwise one.
 * @retval 0 Every summary row was replayed and accepted by @p sink.
 * @retval 1 A replay or sink operation failed.
 * @note Not thread-safe: writes @p sink and runs replays. Benchmark thread only.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_report_summary(const cb_trace_t* traces,
                                   uint32_t          ntr,
                                   cb_workspace_t*   workspace,
                                   cb_sink_t*        sink)
{
  const uint32_t mid_cap = (uint32_t)k_cb_mid_cap;
  if (cb_sink_format(sink, "\n## Summary at %u frames (mean over all workloads)\n\n", mid_cap) !=
        k_cb_io_ok ||
      cb_sink_format(sink, "| policy | mean hit %% | worst scan/evict | meta bytes/frame |\n") !=
        k_cb_io_ok ||
      cb_sink_format(sink, "|--------|-----------:|-----------------:|-----------------:|\n") !=
        k_cb_io_ok) {
    return 1;
  }
  for (uint32_t p = 0U; p < g_cb_policy_count; ++p) {
    double   sum_hit = 0.0;
    uint32_t worst   = 0U;
    for (uint32_t t = 0U; t < ntr; ++t) {
      cb_result_t r = {};
      if (cb_replay(g_cb_policies[p], &traces[t], mid_cap, workspace, &r) != 0) {
        return 1;
      }
      sum_hit +=
        (r.accesses == 0U) ? 0.0 : (s_cb_pct_scale_f * (double)r.hits / (double)r.accesses);
      if (r.worst_scan > worst) {
        worst = r.worst_scan;
      }
    }
    if (cb_sink_format(sink,
                       "| %-14s | %10.2f | %16u | %16zu |\n",
                       g_cb_policies[p]->name,
                       sum_hit / (double)ntr,
                       worst,
                       g_cb_policies[p]->meta_bytes) != k_cb_io_ok) {
      return 1;
    }
  }
  return 0;
}

/** @brief Capacity of the extra captured-trace table filled from argv. */
typedef enum : uint8_t {
  k_cb_max_loaded          = 8U, /**< Most `<name>=<path>` traces accepted per run. */
  k_cb_output_prefix_bytes = 9U, /**< Bytes in the literal `--output=` prefix.      */
} cb_loaded_cap_t;

typedef enum : size_t {
  k_cb_composition_workspace_bytes = 131072U, /**< Maximum exact metadata budget. */
} cb_composition_limit_t;

alignas(max_align_t) static uint8_t s_cb_composition_workspace[k_cb_composition_workspace_bytes];
typedef struct {
  uint64_t before;                              /**< Detect setup underflow. */
  alignas(max_align_t) uint8_t bytes[1048576U]; /**< Semantic cache budget.  */
  uint64_t after;                               /**< Detect setup overflow.  */
} cb_sweep_backing_t;

typedef enum : uint64_t {
  k_cb_sweep_canary_before = 0x0CACEB00C0FFEE11ULL, /**< Leading guard value.  */
  k_cb_sweep_canary_after  = 0xA11CE55E0BADC0DEULL, /**< Trailing guard value. */
} cb_sweep_canary_t;

/**
 * @brief Load the extra captured traces named on the command line.
 * @details Each argv of the form `<name>=<path>` (e.g. `hw-reader=t.trace`)
 *          is split at the first `=` and loaded via ::cb_trace_load;
 *          arguments without `=` are ignored (they are mode flags). Traces
 *          that fail to load (n == 0) are dropped silently, exactly as before.
 * @param[in]  argc   Argument count from main.
 * @param[in]  argv   Argument vector from main (read, never modified).
 * @param[out] loaded Receives up to ::k_cb_max_loaded loaded traces.
 * @param[out] sources Receives the corresponding open host bindings.
 * @return uint32_t Number of traces actually loaded (0 .. ::k_cb_max_loaded).
 * @retval 0     No argv held a loadable `<name>=<path>` pair.
 * @retval other The count of successfully loaded traces.
 * @pre @p loaded has capacity ::k_cb_max_loaded.
 * @pre Every `argv[a]` is NUL-terminated.
 * @post Entries `loaded[0..return)` all have `n > 0`.
 * @post No argument string is modified.
 * @note Not thread-safe (the host source bindings are caller-owned).
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t
internal_load_argv_traces(int argc, char** argv, cb_trace_t* loaded, cb_host_source_t* sources)
{
  uint32_t nloaded = 0U;
  for (int a = 1; (a < argc) && (nloaded < (uint32_t)k_cb_max_loaded); ++a) {
    const size_t name_length = strcspn(argv[a], "=");
    if ((argv[a][name_length] != '=') ||
        (strncmp(argv[a], "--output=", (size_t)k_cb_output_prefix_bytes) == 0)) {
      continue;
    }
    cb_source_t source = {};
    if ((cb_host_source_open(&argv[a][name_length + 1U], &sources[nloaded], &source) ==
         k_cb_io_ok) &&
        (cb_trace_bind(&source, argv[a], name_length, &loaded[nloaded]) == k_cb_io_ok)) {
      nloaded++;
    } else if (sources[nloaded].fd >= 0) {
      (void)cb_host_source_close(&sources[nloaded]);
    } else {
      /* The open bound no descriptor, so there is nothing to close here. */
    }
  }
  return nloaded;
}

/**
 * @brief Find the optional report destination in the argument vector.
 * @details Scans arguments after argv[0] and returns the bytes following the
 *          first `--output=` prefix without copying or taking ownership.
 * @param[in] argc Number of entries in @p argv.
 * @param[in] argv Process argument vector.
 * @return Borrowed destination path, or NULL when the option is absent.
 * @retval NULL No `--output=` argument was present.
 * @retval other Pointer into the matching argument.
 * @pre @p argv names at least @p argc readable pointers.
 * @pre Each inspected argument is NUL-terminated.
 * @post @p argv and its strings are not modified.
 * @post Any non-NULL result remains valid while the argument vector lives.
 * @note The empty path from a bare `--output=` is returned for open validation.
 * @since 0.1.0
 */
RA8_INTERNAL
static const char* internal_output_path(int argc, char** argv)
{
  for (int i = 1; i < argc; ++i) {
    if (strncmp(argv[i], "--output=", (size_t)k_cb_output_prefix_bytes) == 0) {
      return &argv[i][k_cb_output_prefix_bytes];
    }
  }
  return nullptr;
}

/**
 * @brief Compose scratch, cache, and workspace bindings and execute block mode.
 * @details Opens one host scratch transaction, guards the fixed cache backing
 *          with canaries, runs ::cb_sweep_block, then closes the scratch seam.
 * @param[in,out] output Sweep report destination.
 * @param[in,out] error Diagnostic destination.
 * @return Zero on a complete sweep, otherwise one.
 * @retval 0 The sweep, canary checks, and scratch close all succeeded.
 * @retval 1 Composition, sweep, canary, or close validation failed.
 * @pre @p output and @p error are bound writable sinks.
 * @pre The benchmark runs on its single composition thread.
 * @post The scratch descriptor is closed on every successful open path.
 * @post The fixed cache backing remains owned by this translation unit.
 * @note The two sink bindings may refer to distinct borrowed descriptors.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_run_sweep(cb_sink_t* output, cb_sink_t* error)
{
  cb_host_scratch_t scratch_binding = {.fd = -1};
  cb_scratch_t      scratch         = {};
  if (cb_host_scratch_open(&scratch_binding, &scratch) != k_cb_io_ok) {
    return 1;
  }
  /** @brief Fixed cache backing owned by this translation unit alone. */
  static cb_sweep_backing_t s_cb_sweep_backing;
  s_cb_sweep_backing.before = (uint64_t)k_cb_sweep_canary_before;
  s_cb_sweep_backing.after  = (uint64_t)k_cb_sweep_canary_after;
  cb_sweep_config_t config  = {.cache_backing      = s_cb_sweep_backing.bytes,
                               .cache_capacity     = sizeof(s_cb_sweep_backing.bytes),
                               .workspace          = s_cb_composition_workspace,
                               .workspace_capacity = sizeof(s_cb_composition_workspace),
                               .scratch            = &scratch,
                               .output             = output,
                               .error              = error};
  int               result  = cb_sweep_block(&config);
  if ((s_cb_sweep_backing.before != (uint64_t)k_cb_sweep_canary_before) ||
      (s_cb_sweep_backing.after != (uint64_t)k_cb_sweep_canary_after) ||
      (cb_host_scratch_close(&scratch_binding) != k_cb_io_ok)) {
    result = 1;
  }
  return result;
}

/**
 * @brief Close every captured source, preserving any close failure.
 * @details Attempts all @p count closes even after one fails so no later
 *          borrowed host binding is skipped during teardown.
 * @param[in,out] sources Array of open host source bindings.
 * @param[in] count Number of entries to close.
 * @return Zero when every close succeeds, otherwise one.
 * @retval 0 All descriptors closed successfully.
 * @retval 1 At least one close operation failed.
 * @pre @p sources contains @p count initialized bindings.
 * @pre Each binding is closed at most once by this call.
 * @post Every entry has been passed to ::cb_host_source_close.
 * @post A failure does not prevent later entries from being attempted.
 * @note Not thread-safe with concurrent users of the same descriptors.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_close_sources(cb_host_source_t* sources, uint32_t count)
{
  int result = 0;
  for (uint32_t index = 0U; index < count; ++index) {
    if (cb_host_source_close(&sources[index]) != k_cb_io_ok) {
      result = 1;
    }
  }
  return result;
}

/**
 * @brief Execute the capacity report over synthetic and captured traces.
 * @details Binds the fixed synthetic corpus, opens optional captured sources,
 *          publishes per-trace and summary tables, and closes every source.
 * @param[in] argc Number of entries in @p argv.
 * @param[in,out] argv Writable process argument vector.
 * @param[in,out] output Report destination.
 * @param[in,out] error Diagnostic destination used for close failures.
 * @return Zero on complete publication and teardown, otherwise one.
 * @retval 0 All replays, sink writes, and source closes succeeded.
 * @retval 1 A trace, workspace, sink, or close operation failed.
 * @pre @p argv names @p argc writable, NUL-terminated argument strings.
 * @pre @p output and @p error are bound writable sinks.
 * @post Every successfully opened captured source is closed.
 * @post The fixed composition workspace remains owned by this translation unit.
 * @note This function mutates accepted `<name>=<path>` argument separators.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_run_capacity(int argc, char** argv, cb_sink_t* output, cb_sink_t* error)
{
  cb_trace_t traces[k_cb_synthetic_trace_count] = {};
  cb_traces_synthetic(traces);
  cb_trace_t       loaded[k_cb_max_loaded]  = {};
  cb_host_source_t sources[k_cb_max_loaded] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_cb_max_loaded; ++i) {
    sources[i].fd = -1;
  }
  const uint32_t nloaded   = internal_load_argv_traces(argc, argv, loaded, sources);
  cb_workspace_t workspace = {.data     = s_cb_composition_workspace,
                              .capacity = sizeof(s_cb_composition_workspace)};
  const bool     banner_ok =
    (cb_sink_format(output, "# #147 eviction-policy benchmark\n") == k_cb_io_ok) &&
    (cb_sink_format(output, "\nHit rate (%%) by cache size (frames). Higher is better.\n") ==
     k_cb_io_ok);
  int result = 1;
  if (banner_ok) {
    result = 0;
  }
  for (uint32_t t = 0U; (t < (uint32_t)k_cb_synthetic_trace_count) && (result == 0); ++t) {
    result = internal_report_trace(&traces[t], &workspace, output);
  }
  for (uint32_t t = 0U; (t < nloaded) && (result == 0); ++t) {
    result = internal_report_trace(&loaded[t], &workspace, output);
  }
  if (result == 0) {
    result =
      internal_report_summary(traces, (uint32_t)k_cb_synthetic_trace_count, &workspace, output);
  }
  result |= internal_close_sources(sources, nloaded);
  if (result != 0) {
    (void)cb_sink_format(error,
                         "cache_bench: run failed (workspace required=%zu supplied=%zu)\n",
                         workspace.required,
                         workspace.capacity);
  }
  return result;
}

int main(int argc, char** argv)
{
  cb_sink_t error = {};
  cb_host_standard_sinks(nullptr, &error);
  cb_host_output_t output_binding = {};
  cb_sink_t        output         = {};
  if (cb_host_output_open(internal_output_path(argc, argv), &output_binding, &output) !=
      k_cb_io_ok) {
    (void)cb_sink_format(&error, "cache_bench: output open failed\n");
    return 1;
  }
  const int result = ((argc > 1) && (strcmp(argv[1], "--sweep-block") == 0))
                       ? internal_run_sweep(&output, &error)
                       : internal_run_capacity(argc, argv, &output, &error);
  if ((result == 0) && (cb_host_output_commit(&output_binding) == k_cb_io_ok)) {
    return 0;
  }
  cb_host_output_abort(&output_binding);
  return 1;
}
