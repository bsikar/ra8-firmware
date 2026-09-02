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
 * single results row per benchmark to its caller-supplied sink in this format:
 *
 *   <benchmark name>,<iterations>,<ns_per_op>,<MB_per_s>
 *
 * The CSV header is printed once per executable. This makes it
 * trivial to grep / pipe into a spreadsheet later when we have EVM
 * hardware measurements to compare against.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>
#include <time.h>

#include "ra8_test_output.h"

/**
 * @enum ra8_bench_limits_t
 * @brief Bench harness tuning knobs.
 */
typedef enum : uint32_t {
  k_ra8_bench_min_iters   = 3U,          /**< Floor on iteration count.           */
  k_ra8_bench_max_iters   = 10000000U,   /**< Ceiling on iteration count.         */
  k_ra8_bench_calibrate_n = 1U,          /**< Calibration runs before timing.     */
  k_ra8_bench_min_us      = 100000U,     /**< Min wall time per measurement (us). */
  k_ra8_bench_ns_per_us   = 1000U,       /**< RA8 bench ns per us.                */
  k_ra8_bench_ns_per_s    = 1000000000U, /**< RA8 bench ns per s.                 */
  k_ra8_bench_fixed_scale = 100U,        /**< Two-decimal fixed-point scale.      */
  k_ra8_bench_mbps_scale  = 100000U,     /**< Bytes/ns to hundredths of MB/s.     */
} ra8_bench_limits_t;

/**
 * @brief Read a monotonic wall clock in nanoseconds.
 * @details Converts one host monotonic `timespec` into the benchmark harness's
 * unsigned nanosecond epoch.
 * @return Current monotonic timestamp in ns.
 * @retval 0 The host returned its monotonic epoch or the clock read failed.
 * @retval >0 The elapsed monotonic nanoseconds at the observation point.
 * @pre The host provides `CLOCK_MONOTONIC`.
 * @pre The converted timestamp fits in `uint64_t` for the test lifetime.
 * @post The real-time clock is neither read nor changed.
 * @post No process-global benchmark state is modified.
 * @note Host clock-read failure leaves the zero-initialized timestamp result.
 * @since 0.1.0
 */
RA8_INTERNAL static inline uint64_t internal_bench_now_ns(void)
{
  struct timespec ts = {0, 0};
  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((uint64_t)ts.tv_sec * (uint64_t)k_ra8_bench_ns_per_s) + (uint64_t)ts.tv_nsec;
}

/**
 * @brief Write the one-time CSV header before any benches.
 * @details Composes the executable banner and fixed column names through the
 * same first-error-latching sink used for result rows.
 * @param[in,out] output Caller-owned result sink.
 * @param[in] exe_name Benchmark executable name.
 * @return Success or the sink's first failure.
 * @retval k_ra8_test_output_ok Both complete header lines were accepted.
 * @retval k_ra8_test_output_error The destination rejected a header fragment.
 * @pre @p output is initialized and @p exe_name is NUL-terminated.
 * @pre The caller keeps the sink context alive for the complete composition.
 * @post Success appends the exact two-line CSV header.
 * @post No process-global output state is used.
 * @note The caller controls whether and how often the header is emitted.
 * @since 0.1.0
 */
RA8_INTERNAL static inline ra8_test_output_status_t
internal_bench_print_header(ra8_test_output_t* output, const char* exe_name)
{
  ra8_test_output_status_t status = internal_test_output_text(output, "# ra8_bench: ");
  if (status == k_ra8_test_output_ok) {
    status = internal_test_output_text(output, exe_name);
  }
  if (status == k_ra8_test_output_ok) {
    status = internal_test_output_text(output, "\nname,iterations,ns_per_op,MB_per_s\n");
  }
  return status;
}

/**
 * @brief Round one nonnegative ratio to a caller-selected integer scale.
 * @details Computes through a widened floating intermediate, rounds to nearest
 * integer, and saturates magnitudes outside the unsigned result range.
 * @param[in] numerator Ratio numerator.
 * @param[in] denominator Nonzero ratio denominator.
 * @param[in] scale Fixed-point scale multiplier.
 * @return Rounded scaled ratio, or zero for a zero denominator.
 * @retval 0 The denominator was zero or the rounded ratio was zero.
 * @retval UINT64_MAX The scaled ratio exceeded the representable result range.
 * @pre @p scale is nonzero.
 * @pre Inputs represent a nonnegative benchmark quantity.
 * @post The result is rounded to nearest rather than truncated.
 * @post No formatting stream or allocation is used.
 * @note Benchmark magnitudes keep the long-double intermediate within uint64_t.
 * @since 0.1.0
 */
RA8_INTERNAL static inline uint64_t
internal_bench_scaled_ratio(uint64_t numerator, uint64_t denominator, uint64_t scale)
{
  if (denominator == 0U) {
    return 0U;
  }
  const long double scaled =
    ((long double)numerator * (long double)scale) / (long double)denominator;
  if (scaled >= (long double)UINT64_MAX) {
    return UINT64_MAX;
  }
  return (uint64_t)(scaled + 0.5L);
}

/**
 * @brief Print a single results row.
 * @details Computes rounded hundredths for latency and throughput, then
 * composes the exact comma-separated fields through the injected sink.
 *
 * @param[in] name      Benchmark name (no commas).
 * @param[in] iters     Number of iterations performed.
 * @param[in] elapsed_ns Total elapsed nanoseconds across all iters.
 * @param[in] bytes_per_iter Bytes processed per iteration (0 if N/A).
 * @param[in,out] output Caller-owned CSV sink.
 * @return Success or the sink's first failure.
 * @retval k_ra8_test_output_ok The complete CSV row was accepted.
 * @retval k_ra8_test_output_error The destination rejected a row fragment.
 * @pre @p output is initialized and @p name is NUL-terminated.
 * @pre The caller keeps the sink context alive for the complete row.
 * @post Success appends one exact CSV row with two decimal places.
 * @post No process-global output state is used.
 * @note A zero byte count reports throughput as `0.00`.
 * @since 0.1.0
 */
RA8_INTERNAL static inline ra8_test_output_status_t internal_bench_report(ra8_test_output_t* output,
                                                                          const char*        name,
                                                                          uint64_t           iters,
                                                                          uint64_t elapsed_ns,
                                                                          uint64_t bytes_per_iter)
{
  const uint64_t ns_per_op =
    internal_bench_scaled_ratio(elapsed_ns, iters, (uint64_t)k_ra8_bench_fixed_scale);
  uint64_t mb_per_s = 0U;
  if ((bytes_per_iter > 0U) && (elapsed_ns > 0U) && (iters <= (UINT64_MAX / bytes_per_iter))) {
    mb_per_s = internal_bench_scaled_ratio(iters * bytes_per_iter,
                                           elapsed_ns,
                                           (uint64_t)k_ra8_bench_mbps_scale);
  }
  ra8_test_output_status_t status = internal_test_output_text(output, name);
  if (status == k_ra8_test_output_ok) {
    status = internal_test_output_text(output, ",");
  }
  if (status == k_ra8_test_output_ok) {
    status = internal_test_output_u64(output, iters);
  }
  if (status == k_ra8_test_output_ok) {
    status = internal_test_output_text(output, ",");
  }
  if (status == k_ra8_test_output_ok) {
    status = internal_test_output_fixed2(output, ns_per_op);
  }
  if (status == k_ra8_test_output_ok) {
    status = internal_test_output_text(output, ",");
  }
  if (status == k_ra8_test_output_ok) {
    status = internal_test_output_fixed2(output, mb_per_s);
  }
  if (status == k_ra8_test_output_ok) {
    status = internal_test_output_text(output, "\n");
  }
  return status;
}

/**
 * @brief Run `body` repeatedly, auto-scale iteration count, and
 *        write one row to the explicit result sink.
 *
 * @details
 * Doubles the iteration count until total elapsed time exceeds
 * `k_ra8_bench_min_us`, then reports the final measurement.
 *
 * @param[in] name_str   Benchmark name as a string literal.
 * @param[in] bytes_iter Bytes processed per iteration (0 disables MB/s).
 * @param[in] body       Brace-enclosed statement(s) to time.
 * @param[in,out] output Caller-owned result sink.
 */
#define RA8_BENCH_TIME(output, name_str, bytes_iter, body)                                         \
  do {                                                                                             \
    uint64_t _iters = (uint64_t)k_ra8_bench_min_iters;                                             \
    uint64_t _ns    = 0U;                                                                          \
    while (_iters <= (uint64_t)k_ra8_bench_max_iters) {                                            \
      uint64_t _t0 = internal_bench_now_ns();                                                      \
      for (uint64_t _i = 0U; _i < _iters; _i++) {                                                  \
        body                                                                                       \
      }                                                                                            \
      uint64_t _t1 = internal_bench_now_ns();                                                      \
      _ns          = _t1 - _t0;                                                                    \
      uint64_t _us = _ns / (uint64_t)k_ra8_bench_ns_per_us;                                        \
      if (_us >= (uint64_t)k_ra8_bench_min_us) {                                                   \
        break;                                                                                     \
      }                                                                                            \
      _iters *= 2U;                                                                                \
    }                                                                                              \
    (void)internal_bench_report((output), (name_str), _iters, _ns, (uint64_t)(bytes_iter));        \
  } while (0)
