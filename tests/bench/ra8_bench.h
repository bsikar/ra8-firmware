/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_bench.h
 * @brief Minimal hand-written microbenchmark harness for the
 *        ra8-firmware host-side performance suite.
 *
 * @details
 * Google Benchmark is not vendored under libs/third_party/, so this
 * header provides a tiny stand-in that is good enough for the kind
 * of measurements we care about right now: wall-clock time per
 * iteration over a fixed work item, plus throughput in bytes/sec
 * when the caller declares an item size.
 *
 * Each benchmark file contains exactly one `int main(void)` that
 * drives one or more `RA8_BENCH_TIME(...)` blocks. The harness
 * adaptively chooses an iteration count so each measurement runs
 * for at least `k_ra8_bench_min_seconds` of wall time, then prints a
 * single results row per benchmark to stdout in this format:
 *
 *   <benchmark name>,<iterations>,<ns_per_op>,<MB_per_s>
 *
 * The CSV header is printed once per executable. This makes it
 * trivial to grep / pipe into a spreadsheet later when we have EVM
 * hardware measurements to compare against.
 *
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <time.h>

/**
 * @enum ra8_bench_limits_t
 * @brief Bench harness tuning knobs.
 */
typedef enum : uint32_t {
  k_ra8_bench_min_iters    = 3U,          /**< Floor on iteration count.           */
  k_ra8_bench_max_iters    = 10000000U,   /**< Ceiling on iteration count.         */
  k_ra8_bench_calibrate_n  = 1U,          /**< Calibration runs before timing.     */
  k_ra8_bench_min_us       = 100000U,     /**< Min wall time per measurement (us). */
  k_ra8_bench_ns_per_us    = 1000U,       /**< RA8 bench ns per us.                */
  k_ra8_bench_us_per_s     = 1000000U,    /**< RA8 bench us per s.                 */
  k_ra8_bench_ns_per_s     = 1000000000U, /**< RA8 bench ns per s.                 */
  k_ra8_bench_bytes_per_mb = 1000000U,    /**< RA8 bench bytes per mb.             */
} ra8_bench_limits_t;

/**
 * @brief Read a monotonic wall clock in nanoseconds.
 * @return Current monotonic timestamp in ns.
 */
static inline uint64_t ra8_bench_now_ns(void)
{
  struct timespec ts = {0, 0};
  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((uint64_t)ts.tv_sec * (uint64_t)k_ra8_bench_ns_per_s) + (uint64_t)ts.tv_nsec;
}

/**
 * @brief One-time CSV header. Call from main() before any benches.
 */
static inline void ra8_bench_print_header(const char* exe_name)
{
  (void)fprintf(stdout, "# ra8_bench: %s\n", exe_name);
  (void)fprintf(stdout, "name,iterations,ns_per_op,MB_per_s\n");
  (void)fflush(stdout);
}

/**
 * @brief Print a single results row.
 *
 * @param[in] name      Benchmark name (no commas).
 * @param[in] iters     Number of iterations performed.
 * @param[in] elapsed_ns Total elapsed nanoseconds across all iters.
 * @param[in] bytes_per_iter Bytes processed per iteration (0 if N/A).
 */
static inline void
ra8_bench_report(const char* name, uint64_t iters, uint64_t elapsed_ns, uint64_t bytes_per_iter)
{
  double ns_per_op = 0.0;
  if (iters > 0U) {
    ns_per_op = (double)elapsed_ns / (double)iters;
  }
  double mb_per_s = 0.0;
  if ((bytes_per_iter > 0U) && (elapsed_ns > 0U)) {
    double total_bytes = (double)iters * (double)bytes_per_iter;
    double seconds     = (double)elapsed_ns / (double)k_ra8_bench_ns_per_s;
    mb_per_s           = (total_bytes / seconds) / (double)k_ra8_bench_bytes_per_mb;
  }
  (void)
    fprintf(stdout, "%s,%llu,%.2f,%.2f\n", name, (unsigned long long)iters, ns_per_op, mb_per_s);
  (void)fflush(stdout);
}

/**
 * @brief Run `body` repeatedly, auto-scale iteration count, and
 *        print one row to stdout.
 *
 * @details
 * Doubles the iteration count until total elapsed time exceeds
 * `k_ra8_bench_min_us`, then reports the final measurement.
 *
 * @param[in] name_str   Benchmark name as a string literal.
 * @param[in] bytes_iter Bytes processed per iteration (0 disables MB/s).
 * @param[in] body       Brace-enclosed statement(s) to time.
 */
#define RA8_BENCH_TIME(name_str, bytes_iter, body)                                                 \
  do {                                                                                             \
    uint64_t _iters = (uint64_t)k_ra8_bench_min_iters;                                             \
    uint64_t _ns    = 0U;                                                                          \
    while (_iters <= (uint64_t)k_ra8_bench_max_iters) {                                            \
      uint64_t _t0 = ra8_bench_now_ns();                                                           \
      for (uint64_t _i = 0U; _i < _iters; _i++) {                                                  \
        body                                                                                       \
      }                                                                                            \
      uint64_t _t1 = ra8_bench_now_ns();                                                           \
      _ns          = _t1 - _t0;                                                                    \
      uint64_t _us = _ns / (uint64_t)k_ra8_bench_ns_per_us;                                        \
      if (_us >= (uint64_t)k_ra8_bench_min_us) {                                                   \
        break;                                                                                     \
      }                                                                                            \
      _iters *= 2U;                                                                                \
    }                                                                                              \
    ra8_bench_report((name_str), _iters, _ns, (uint64_t)(bytes_iter));                             \
  } while (0)
