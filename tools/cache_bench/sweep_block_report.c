/**
 * @file sweep_block_report.c
 * @brief Reporting for the #208 sweep: machine-parseable rows, per-leg
 *        summary tables, and the measured knee / crossover verdict.
 *
 * @details
 * Consumes the finished ::cbs_row_t array the sweep core produced. The
 * machine-parseable `sweep-block key=value ...` rows are printed as each leg
 * finishes; the human summary (one sequential and one hot-path markdown
 * table per backend, then the crossover paragraph naming the measured knee
 * against the 64 KiB `.rabook` chunk default from #204) is printed once the
 * sweep completes. Pure readers: nothing here mutates row data.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @since 0.1.0
 */
#include <stdio.h>
#include <string.h>

#include "sweep_block_internal.h"

/** @brief 100.0 as a double, for percentage maths. */
static const double s_cbs_pct_f = 100.0;
/** @brief Bytes per MiB as a double, for throughput maths. */
static const double s_cbs_mib_f = 1048576.0;
/** @brief Nanoseconds per second as a double, for throughput maths. */
static const double s_cbs_ns_per_s_f = 1000000000.0;
/** @brief Nanoseconds per microsecond as a double, for latency maths. */
static const double s_cbs_ns_per_us_f = 1000.0;

/** @brief Implementation of `cbs_priv_print_row()` -- one key=value line. */
void cbs_priv_print_row(const cbs_row_t* r)
{
  if ((r == nullptr) || (r->reads == 0U)) {
    return;
  }
  const uint64_t wall = (r->wall_ns == 0U) ? 1U : r->wall_ns;
  const double   mib_s =
    ((double)r->reads * (double)k_cbs_req_bytes / s_cbs_mib_f) / ((double)wall / s_cbs_ns_per_s_f);
  (void)printf("sweep-block backend=%s leg=%s block=%u frames=%u reads=%llu hits=%llu "
               "misses=%llu evictions=%llu backend_calls=%llu backend_bytes=%llu "
               "src_bytes=%llu backing_bytes=%llu wall_us=%llu mib_s=%.1f ns_per_read=%.1f\n",
               r->backend,
               r->leg,
               r->block_bytes,
               r->frames,
               (unsigned long long)r->reads,
               (unsigned long long)r->hits,
               (unsigned long long)r->misses,
               (unsigned long long)r->evictions,
               (unsigned long long)r->be_calls,
               (unsigned long long)r->be_bytes,
               (unsigned long long)r->src_bytes,
               (unsigned long long)r->backing_bytes,
               (unsigned long long)(wall / (uint64_t)k_cbs_ns_per_us),
               mib_s,
               (double)wall / (double)r->reads);
}

/**
 * @brief Format a block size as a short label ("512B", "64K") into @p out.
 *
 * @details Writes a KiB label with `snprintf` for sizes at or above one KiB and
 *          a byte label below that, truncating safely to @p cap, so table rows
 *          stay narrow and aligned.
 *
 * @param[in]  block Block size in bytes to label.
 * @param[out] out   Destination buffer (NUL-terminated by `snprintf`).
 * @param[in]  cap   Capacity of @p out in bytes.
 *
 * @pre @p out has @p cap writable bytes when @p cap > 0.
 * @pre Called on the single benchmark thread.
 * @post On @p cap > 0, @p out holds a NUL-terminated label.
 * @post No state other than @p out is modified.
 *
 * @note Not thread-safe: writes the caller's buffer (no shared state).
 * @since 0.1.0
 */
static void cbs_block_label(uint32_t block, char* out, size_t cap)
{
  if ((out == nullptr) || (cap == 0U)) {
    return;
  }
  if (block >= (uint32_t)k_cbs_kib) {
    (void)snprintf(out, cap, "%uK", block / (uint32_t)k_cbs_kib);
  } else {
    (void)snprintf(out, cap, "%uB", block);
  }
}

/** @brief Find the row for (backend, leg, block), or NULL. */
static const cbs_row_t*
cbs_find_row(const cbs_row_t* rows, uint32_t n, const char* be, const char* leg, uint32_t block)
{
  if ((rows == nullptr) || (be == nullptr) || (leg == nullptr)) {
    return nullptr;
  }
  for (uint32_t i = 0U; i < n; ++i) {
    if ((rows[i].block_bytes == block) && (strcmp(rows[i].backend, be) == 0) &&
        (strcmp(rows[i].leg, leg) == 0)) {
      return &rows[i];
    }
  }
  return nullptr;
}

/**
 * @brief Delivered-payload throughput of a row in MiB/s.
 *
 * @details Computes `reads * k_cbs_req_bytes` delivered bytes over the row's
 *          wall time, clamping a zero wall time to 1 ns so the division is
 *          always defined. Used by the summary tables and the knee search.
 *
 * @param[in] r Finished row (NULL or zero-reads yields 0.0).
 *
 * @return double Throughput in MiB/s, or 0.0 when @p r is empty.
 * @retval 0.0   @p r is NULL or recorded no reads.
 * @retval other The delivered-payload throughput in MiB/s.
 *
 * @pre @p r is NULL, or a finished row with final counters.
 * @pre Called on the single benchmark thread.
 * @post @p r is not modified (pure read).
 * @post No global state is touched.
 *
 * @note Thread-safe over a quiescent row (pure read).
 * @since 0.1.0
 */
static double cbs_row_mibs(const cbs_row_t* r)
{
  if ((r == nullptr) || (r->reads == 0U)) {
    return 0.0;
  }
  const uint64_t wall = (r->wall_ns == 0U) ? 1U : r->wall_ns;
  return ((double)r->reads * (double)k_cbs_req_bytes / s_cbs_mib_f) /
         ((double)wall / s_cbs_ns_per_s_f);
}

/** @brief Implementation of `cbs_priv_print_seq_table()` -- Ch 23.2 miss-cost split. */
void cbs_priv_print_seq_table(const cbs_row_t* rows,
                              uint32_t         nrows,
                              const char*      be,
                              const uint32_t*  blocks,
                              uint32_t         nblocks)
{
  (void)printf("\n### `%s` -- sequential whole-object scan (leg a)\n\n", be);
  (void)printf("| block | frames | MiB/s | ns/read | est us/miss | backend calls | src MiB | "
               "backing MiB |\n");
  (void)printf("|------:|-------:|------:|--------:|------------:|--------------:|--------:|"
               "------------:|\n");
  for (uint32_t i = 0U; i < nblocks; ++i) {
    const cbs_row_t* s = cbs_find_row(rows, nrows, be, "seq", blocks[i]);
    const cbs_row_t* h = cbs_find_row(rows, nrows, be, "hot", blocks[i]);
    if ((s == nullptr) || (h == nullptr) || (s->reads == 0U) || (h->reads == 0U)) {
      continue;
    }
    const double hit_ns  = (double)h->wall_ns / (double)h->reads;
    double       miss_ns = (double)s->wall_ns - (hit_ns * (double)s->reads);
    miss_ns              = (s->misses == 0U) ? 0.0 : (miss_ns / (double)s->misses);
    if (miss_ns < 0.0) {
      miss_ns = 0.0;
    }
    char label[16] = {};
    cbs_block_label(blocks[i], label, sizeof(label));
    (void)printf("| %5s | %6u | %5.0f | %7.1f | %11.1f | %13llu | %7.2f | %11.2f |\n",
                 label,
                 s->frames,
                 cbs_row_mibs(s),
                 (double)s->wall_ns / (double)s->reads,
                 miss_ns / s_cbs_ns_per_us_f,
                 (unsigned long long)s->be_calls,
                 (double)s->src_bytes / s_cbs_mib_f,
                 (double)s->backing_bytes / s_cbs_mib_f);
  }
}

/** @brief Implementation of `cbs_priv_print_hot_table()` -- pure hit-path table. */
void cbs_priv_print_hot_table(const cbs_row_t* rows,
                              uint32_t         nrows,
                              const char*      be,
                              const uint32_t*  blocks,
                              uint32_t         nblocks)
{
  (void)printf("\n### `%s` -- same-block re-read (leg b, pure hit path)\n\n", be);
  (void)printf("| block | ns/read | hits | misses |\n");
  (void)printf("|------:|--------:|-----:|-------:|\n");
  for (uint32_t i = 0U; i < nblocks; ++i) {
    const cbs_row_t* h = cbs_find_row(rows, nrows, be, "hot", blocks[i]);
    if ((h == nullptr) || (h->reads == 0U)) {
      continue;
    }
    char label[16] = {};
    cbs_block_label(blocks[i], label, sizeof(label));
    (void)printf("| %5s | %7.1f | %llu | %6llu |\n",
                 label,
                 (double)h->wall_ns / (double)h->reads,
                 (unsigned long long)h->hits,
                 (unsigned long long)h->misses);
  }
}

/**
 * @struct cbs_knee_t
 * @brief The computed crossover of the chunked backend's sequential sweep.
 * @details Filled by ::cbs_find_knee; `knee_block` is the smallest size whose
 *          throughput reaches ::k_cbs_knee_pct percent of `peak_mibs`.
 * @invariant Both `knee_block` and `peak_block` always name swept sizes.
 * @since 0.1.0
 */
typedef struct {
  double   peak_mibs;  /**< Best sequential throughput seen.   */
  uint32_t peak_block; /**< Size achieving the peak.           */
  double   knee_mibs;  /**< Throughput at the knee.            */
  uint32_t knee_block; /**< Smallest size within the knee bar. */
} cbs_knee_t;

/**
 * @brief Locate the rbkc-z9 sequential throughput peak and knee.
 *
 * @details Scans the chunked backend's sequential rows for the peak MiB/s, then
 *          walks the sizes ascending and takes the first whose throughput
 *          reaches ::k_cbs_knee_pct percent of that peak -- the smallest block
 *          where per-request overhead has stopped dominating.
 *
 * @param[in]  rows    All finished rows.
 * @param[in]  nrows   Number of rows.
 * @param[in]  blocks  The swept sizes, ascending.
 * @param[in]  nblocks Number of swept sizes.
 * @param[out] out     Receives the peak + knee.
 *
 * @return bool true when rbkc-z9 sequential rows existed, false otherwise.
 * @retval true  @p out is fully populated.
 * @retval false No usable rows; @p out is zeroed.
 *
 * @pre @p blocks is sorted ascending (the first size over the bar wins).
 * @pre @p out is writable.
 * @post On true, `out->knee_mibs >= k_cbs_knee_pct% * out->peak_mibs`.
 * @post No row data is modified.
 *
 * @note Thread-safe over quiescent rows (pure read).
 * @since 0.1.0
 */
static bool cbs_find_knee(const cbs_row_t* rows,
                          uint32_t         nrows,
                          const uint32_t*  blocks,
                          uint32_t         nblocks,
                          cbs_knee_t*      out)
{
  if ((out == nullptr) || (blocks == nullptr)) {
    return false;
  }
  *out = (cbs_knee_t){};
  for (uint32_t i = 0U; i < nblocks; ++i) {
    const cbs_row_t* s = cbs_find_row(rows, nrows, "rbkc-z9", "seq", blocks[i]);
    const double     m = cbs_row_mibs(s);
    if (m > out->peak_mibs) {
      out->peak_mibs  = m;
      out->peak_block = blocks[i];
    }
  }
  if (out->peak_mibs <= 0.0) {
    return false;
  }
  const double bar = out->peak_mibs * ((double)k_cbs_knee_pct / s_cbs_pct_f);
  out->knee_block  = out->peak_block;
  out->knee_mibs   = out->peak_mibs;
  for (uint32_t i = 0U; i < nblocks; ++i) {
    const cbs_row_t* s = cbs_find_row(rows, nrows, "rbkc-z9", "seq", blocks[i]);
    const double     m = cbs_row_mibs(s);
    if (m >= bar) {
      out->knee_block = blocks[i];
      out->knee_mibs  = m;
      break;
    }
  }
  return true;
}

/** @brief Implementation of `cbs_priv_print_crossover()` -- knee verdict prose. */
void cbs_priv_print_crossover(const cbs_row_t* rows,
                              uint32_t         nrows,
                              const uint32_t*  blocks,
                              uint32_t         nblocks)
{
  cbs_knee_t k = {};
  if (!cbs_find_knee(rows, nrows, blocks, nblocks, &k)) {
    (void)printf("\n(no rbkc-z9 sequential rows; crossover not computed)\n");
    return;
  }
  const double knee_pct   = s_cbs_pct_f * k.knee_mibs / k.peak_mibs;
  char         peak_l[16] = {};
  char         knee_l[16] = {};
  char         def_l[16]  = {};
  cbs_block_label(k.peak_block, peak_l, sizeof(peak_l));
  cbs_block_label(k.knee_block, knee_l, sizeof(knee_l));
  cbs_block_label((uint32_t)k_cbs_default_chunk, def_l, sizeof(def_l));
  (void)printf("\n### Measured crossover (rbkc-z9, sequential)\n\n");
  (void)printf("Peak %.0f MiB/s at %s; knee (first size within %u%% of peak) at %s "
               "(%.0f MiB/s, %.1f%% of peak).\n",
               k.peak_mibs,
               peak_l,
               (unsigned)k_cbs_knee_pct,
               knee_l,
               k.knee_mibs,
               knee_pct);
  if (k.knee_block == (uint32_t)k_cbs_default_chunk) {
    (void)printf("The measured knee lands on the current %s `.rabook` chunk default (#204): "
                 "keep it.\n",
                 def_l);
  } else if (k.knee_block < (uint32_t)k_cbs_default_chunk) {
    (void)printf("The knee sits BELOW the current %s default: %s already reaches %.1f%% of "
                 "peak on this backend, at a smaller per-miss inflate latency and less RAM "
                 "per frame.\n",
                 def_l,
                 knee_l,
                 knee_pct);
  } else {
    (void)printf("The knee sits ABOVE the current %s default: sequential throughput is still "
                 "climbing past it; consider a larger chunk if per-miss latency allows.\n",
                 def_l);
  }
  (void)printf("Caveat: these are host numbers -- per-request cost here is only the chunk "
               "lookup + zlib stream setup. SD-over-SPI adds real per-command overhead "
               "(CMD17 loops, #202), which pushes the knee toward larger blocks; the "
               "hardware leg of #208 must re-run this sweep on the bench before shrinking "
               "the chunk size below the default.\n");
}
