/**
 * @file bench_ra8_crc.c
 * @brief Microbenchmark: CRC-32 (IEEE 802.3) over 1 KiB / 16 KiB / 1 MiB.
 *
 * @details
 * Drives `ra8_crc_compute()` via the host-side simulator MMIO mock
 * exactly as the unit-test suite does, then reports per-iteration
 * latency and effective throughput. The simulator's CRC accelerator
 * model is a portable C reference implementation, so the resulting
 * numbers reflect the host's general-purpose ALU rather than the
 * RA8D2's hardware CRC peripheral; the benchmark is still useful for
 * regression-tracking the simulator path and for sanity-checking the
 * driver-call overhead.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_bench.h"
#include "ra8_crc.h"
#include "ra8_err.h"

/**
 * @enum crc_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_crc_i_ff = 0xFFU,
} crc_uint8_const_t;

/**
 * @enum crc_uint32_const_t
 * @brief Named uint32_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint32_t {
  k_crc_crc_deadbeef = 0xDEADBEEFU,
} crc_uint32_const_t;

/**
 * @enum bench_crc_sizes_t
 * @brief Buffer sizes exercised by this benchmark.
 */
typedef enum : uint32_t {
  k_bench_crc_1k  = 1024U,         /**< Bench CRC 1k.  */
  k_bench_crc_16k = 16U * 1024U,   /**< Bench CRC 16k. */
  k_bench_crc_1m  = 1024U * 1024U, /**< Bench CRC 1m.  */
} bench_crc_sizes_t;

/** @brief Static input buffer sized to the largest case. */
static uint8_t s_buf[(uint32_t)k_bench_crc_1m];

/**
 * @brief Fill the working buffer with a deterministic pattern.
 */
static void fill_buf(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_bench_crc_1m; i++) {
    s_buf[i] = (uint8_t)(i & k_crc_i_ff);
  }
}

/**
 * @brief Run a single CRC-32 throughput pass.
 */
/* The nesting this reports is entirely inside RA8_BENCH_TIME's expansion --
 * the harness wraps every measured body in do{ while{ for{ ... } } } to pick an
 * iteration count that reaches a minimum wall time. The body below is flat
 * straight-line code and contributes no nesting of its own.
 *
 * It is not reducible: an adaptive iteration count needs a calibration loop
 * around the measured loop, and the body must stay lexically inline -- routing
 * it through a callback would add an indirect call per iteration and inflate
 * exactly the number the benchmark exists to report. clang-tidy offers no
 * per-aspect suppression, so this also masks the statement count for this
 * function; keep it short.
 */
// NOLINTNEXTLINE(readability-function-size)
static void run_one(const char* name, uint32_t len)
{
  uint32_t crc = 0U;
  RA8_BENCH_TIME(name, len, {
    ra8_crc_reset();
    (void)ra8_crc_compute(s_buf, len, &crc);
  });
  /* Touch crc so the optimizer cannot elide the call. */
  if (crc == k_crc_crc_deadbeef) {
    (void)fprintf(stderr, "unreachable\n");
  }
}

/**
 * @brief Bench entry.
 */
int main(void)
{
  ra8_bench_print_header("bench_ra8_crc");
  fill_buf();
  if (ra8_crc_init(k_ra8_crc_poly_32_ieee802_3) != k_ra8_ok) {
    (void)fprintf(stderr, "ra8_crc_init failed\n");
    return 1;
  }
  run_one("crc32_1KiB", (uint32_t)k_bench_crc_1k);
  run_one("crc32_16KiB", (uint32_t)k_bench_crc_16k);
  run_one("crc32_1MiB", (uint32_t)k_bench_crc_1m);
  return 0;
}
