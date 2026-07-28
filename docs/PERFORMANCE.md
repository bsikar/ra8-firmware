# Host-side Performance Benchmarks

This document describes the `tests/bench/` host-side microbenchmark
suite for `ra8-firmware`. The benchmarks are intentionally simple
(no Google Benchmark dependency, no JSON output) so they remain
trivial to run on any developer workstation and on CI.

The numbers measured here are **host x86_64** numbers, not RA8D2
numbers. They establish a regression baseline for the off-target path
and bound the algorithmic cost of each routine; once we have an
EVM-resident HIL runner we will collect a parallel set of on-target
measurements to compare against.

## How to run

From the repository root:

```sh
make bench
```

That target:

1. Configures `tests/build-bench/` with `-DRA8_BENCH=ON` and
   `-DCMAKE_BUILD_TYPE=Release`.
2. Builds the `ra8_bench_all` aggregate target (every binary under
   `tests/bench/`).
3. Runs each bench binary in turn and forwards its stdout to the
   terminal.

Each bench binary writes a one-line CSV header followed by one row
per measurement:

```text
name,iterations,ns_per_op,MB_per_s
crc32_1KiB,131072,712.45,1437.36
```

## Bench inventory

| Binary                  | What it measures                                                        | Bytes/iter |
|-------------------------|-------------------------------------------------------------------------|------------|
| `bench_ra8_crc`          | `ra8_crc_compute()` over 1 KiB / 16 KiB / 1 MiB buffers, CRC-32 IEEE     | buffer len |
| `bench_ra8_jpeg_sw`      | `ra8_jpeg_sw_decode()` of a 64x64 q=75 baseline JPEG fixture             | JPEG bytes |
| `bench_ra8_gfx_text`     | `ra8_gfx_text_out()` rendering the pangram in the bundled 8x16 font     | FB bytes   |

Each bench:

- Pre-warms its working buffers outside the timed region.
- Auto-scales the iteration count so each measurement runs for at
  least `k_ra8_bench_min_us` (currently 100 ms) of wall time, then
  reports a single CSV row.
- Reads the monotonic clock with `clock_gettime(CLOCK_MONOTONIC)`.

## What the columns mean

| Column        | Definition                                                            |
|---------------|-----------------------------------------------------------------------|
| `name`        | Bench label as printed by the binary; never contains a comma.         |
| `iterations`  | Number of inner-loop iterations the harness chose to hit `min_us`.    |
| `ns_per_op`   | Mean wall-clock nanoseconds per iteration (total elapsed / iters).    |
| `MB_per_s`    | Throughput in megabytes/sec where 1 MB = 10^6 bytes (decimal).        |

The harness does **not** report variance, percentiles, or median; the
benchmarks are coarse-grained and the per-iteration loop is long
enough that variance is dominated by OS scheduler noise rather than
microarchitectural jitter. If a benchmark grows precise enough to
warrant percentiles, switch it over to a vendored Google Benchmark
build at that point.

## Expected ranges on a typical x86_64 host

These are order-of-magnitude expectations from a 2020-era laptop-class
x86_64 Linux/macOS host. Numbers within ~2x of these on a fresh CI
runner should not be considered a regression; numbers an order of
magnitude off probably are.

| Bench                            | Ballpark `ns_per_op` | Ballpark `MB_per_s` |
|----------------------------------|----------------------|---------------------|
| `crc32_1KiB`                     | ~500 - 3000          | ~300 - 2000         |
| `crc32_16KiB`                    | ~8000 - 40000        | ~400 - 2000         |
| `crc32_1MiB`                     | ~500000 - 3000000    | ~300 - 2000         |
| `jpeg_decode_64x64_q75`          | ~50000 - 500000      | varies w/ payload   |
| `gfx_text_pangram_8x16_rgb565`   | ~10000 - 200000      | varies w/ FB size   |

CRC throughput is dominated by the fake's reference C
implementation of the CRC peripheral, not by the host CPU's
hardware CRC32 instruction. JPEG decode throughput depends on the
encoded byte-stream size, which itself depends on the quality factor
chosen at encode time.

## Host x86_64 reference numbers (2026-05-02)

Captured by running `make bench` inside the project devcontainer
(Ubuntu 24.04, gcc 13, Release build) on an Apple Silicon host. These
numbers are **indicative only**; on-target Cortex-M85 numbers are
pending hardware (see "EVM measurements" below). Variance across
re-runs on the same host is roughly +/- 10% due to OS scheduler noise.

| Binary             | Bench label                    | iterations | ns_per_op | MB_per_s  |
|--------------------|--------------------------------|-----------:|----------:|----------:|
| `bench_ra8_crc`     | `crc32_1KiB`                   |  1 572 864 |     88.47 | 11 575.11 |
| `bench_ra8_crc`     | `crc32_16KiB`                  |     98 304 |   1346.37 | 12 169.04 |
| `bench_ra8_crc`     | `crc32_1MiB`                   |      1 536 |  90111.46 | 11 636.43 |
| `bench_ra8_jpeg_sw` | `jpeg_decode_64x64_q75`        |      6 144 |  22001.60 |     44.22 |
| `bench_ra8_gfx_text`| `gfx_text_pangram_8x16_rgb565` |     12 288 |   8836.26 |  3 708.36 |

JPEG fixture: encoded 64x64 baseline JPEG @ q=75 -> 973 bytes.

## EVM measurements

To be filled in once the EVM HIL workflow lands. The plan is:

1. Cross-compile each bench TU as a standalone EVM app under
   `examples/ek_ra8d2/bench_<name>/` with the same `RA8_BENCH_TIME`
   harness (the harness header is portable C, no host-only deps).
2. Stream CSV results out over the J-Link RTT channel.
3. Diff against the host CSV to highlight where the production HW path
   beats the off-target path (CRC, JPEG, GFX all benefit from on-die
   accelerators on the RA8D2).

## Implementation notes

- The bench harness lives in `tests/bench/ra8_bench.h`. It is a
  single-header, no-allocation, no-init utility -- include it and
  call `RA8_BENCH_TIME(...)` inside `main()`.
- The bench binaries link against the existing `ra8_core_hal` OBJECT
  library so they pick up the same fake MMIO mocks as the unit
  tests. This guarantees that what the bench measures is exactly the
  same code the unit tests exercise; no parallel "perf-only" build
  variant.
- Bench TUs are **not** compiled with coverage / MC-DC instrumentation
  -- the `RA8_BENCH=ON` configure path forces `RA8_COVERAGE=OFF` and
  `RA8_MCDC=OFF` regardless of the caller's environment.
