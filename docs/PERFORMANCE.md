# Host-side Performance Benchmarks

`tests/bench/` is a host microbenchmark suite. `just quality::local::bench` builds every bench
binary and runs it; each binary prints CSV to stdout. The list it runs is
`RA8_BENCH_TARGETS` in `tests/bench/CMakeLists.txt`, which is the registry: the
recipe reads it, and refuses to run when a `tests/bench/src/bench_*.c` is not in
it, so a harness cannot be added and then silently skipped.

`test_ra8_bench_output`, the deterministic CSV golden, is not a benchmark and is
not run by that recipe. It is an ordinary ctest case, picked up by the
`ra8_add_test()` auto-glob in `tests/cmake/unit_tests.cmake`, so it runs in the
normal `just quality::local::test` flow with `RA8_BENCH=OFF`; the bench build
reuses that target rather than declaring its own (#1415).

It has no third-party benchmark dependency on purpose -- no Google Benchmark, no
JSON writer, no plotting. A bench that needs a package installed before it will
build is a bench nobody runs on a fresh checkout, and the whole value of this
suite is that it runs anywhere the host unit tests run, including CI, with
nothing extra present.

The numbers are **host x86_64** numbers, not RA8D2 numbers. They bound the
algorithmic cost of a routine and catch a regression in the off-target path.
They say nothing about the same code on a Cortex-M85, where the CRC, JPEG and
graphics paths all have on-die accelerators underneath them.

## Reading a result

Each binary writes a one-line CSV header followed by one row per measurement:

```text
name,iterations,ns_per_op,MB_per_s
```

| Column       | Definition                                                          |
|--------------|---------------------------------------------------------------------|
| `name`       | Bench label as printed by the binary; never contains a comma.        |
| `iterations` | Inner-loop iterations the harness chose to reach its minimum wall time. |
| `ns_per_op`  | Mean wall-clock nanoseconds per iteration (total elapsed / iterations). |
| `MB_per_s`   | Throughput where 1 MB = 10^6 bytes (decimal).                        |

The harness pre-warms its working buffers outside the timed region, auto-scales
`iterations` until the measurement runs long enough that the work dominates the
clock, and reads `clock_gettime(CLOCK_MONOTONIC)`.

There is no variance, median or percentile column, because at this granularity
the spread is OS scheduler noise rather than microarchitectural jitter. Read a
row as an order of magnitude: a result that moves by a factor of two on a
different machine is the machine, and a result that moves by an order of
magnitude on the same machine is a regression worth chasing. If a bench ever
becomes precise enough that percentiles would mean something, that is the point
to vendor a real benchmark library for it -- not before.

## How the suite is wired

The harness is `tests/bench/inc/ra8_bench.h`: single-header, no allocation, no init.
Include it and call `RA8_BENCH_TIME(...)` from `main()`.

Bench binaries link the same object library as the host unit tests, so they pick
up the same fake MMIO. What a bench measures is therefore exactly the code the
tests exercise; there is deliberately no parallel "perf-only" build variant that
could drift away from the tested one.

Bench translation units are never instrumented: the bench configure path forces
coverage and MC/DC off regardless of what the caller asked for, because an
instrumented timing run measures the instrumentation.
