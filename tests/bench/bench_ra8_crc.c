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
 * @enum bench_crc_sizes_t
 * @brief Buffer sizes exercised by this benchmark.
 */
typedef enum : uint32_t {
  k_bench_crc_1k  = 1024U,
  k_bench_crc_16k = 16U * 1024U,
  k_bench_crc_1m  = 1024U * 1024U,
} bench_crc_sizes_t;

/** @brief Static input buffer sized to the largest case. */
static uint8_t s_buf[(uint32_t)k_bench_crc_1m];

/**
 * @brief Fill the working buffer with a deterministic pattern.
 */
static void fill_buf(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_bench_crc_1m; i++) {
    s_buf[i] = (uint8_t)(i & 0xFFU);
  }
}

/**
 * @brief Run a single CRC-32 throughput pass.
 */
static void run_one(const char* name, uint32_t len)
{
  uint32_t crc = 0U;
  RA8_BENCH_TIME(name, len, {
    ra8_crc_reset();
    (void)ra8_crc_compute(s_buf, len, &crc);
  });
  /* Touch crc so the optimizer cannot elide the call. */
  if (crc == 0xDEADBEEFU) {
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
