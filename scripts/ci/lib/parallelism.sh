# shellcheck shell=bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/ci/lib/parallelism.sh -- the ONE canonical bounded-parallelism source.
#
# SOURCED, NEVER EXECUTED. Every parallel invocation in a CI gate body, a
# builder, a check or an emulator driver (make -j, cmake --build -j, ctest -j,
# xargs -P, cppcheck -j, clang-tidy fan-out) MUST take its width from
# ra8_max_jobs -- never from a raw `nproc` / `sysctl -n hw.ncpu`.
#
# Why (#328): a raw `nproc` lets ONE gate grab every core. When several gate
# jobs share one box -- the self-hosted dev box runs many runners at once, and
# agents also burst gate suites on it by hand -- they each grab all cores and
# the load average runs to many multiples of the CPU count. The issue measured
# 156 load on 12 vCPU, with 168 concurrent `ld` against an intended ceiling of
# 24: CMAKE_BUILD_PARALLEL_LEVEL was set per job, but individual gates
# self-parallelised past it (make, ctest, xargs, cppcheck and the cross-build
# worker pool all defaulted to `nproc`). An emulator gate then TIMED OUT under
# that contention and read as a code regression rather than the load artefact
# it was. Deriving every site's width from one bounded value keeps total
# concurrency near the CPU budget regardless of how many jobs land at once.
#
# The whole guarded block is idempotent so any number of scripts can source it.
if [ -z "${_RA8_PARALLELISM_SH:-}" ]; then
  _RA8_PARALLELISM_SH=1

  # cpu_count -- the host's raw core count. This is the physical ceiling, NOT
  # the width a single job should parallelise to on a shared box; use
  # ra8_max_jobs for that. Portable across Linux (nproc) and macOS (sysctl),
  # falling back to 4 when neither is available.
  cpu_count() {
    if command -v nproc >/dev/null 2>&1; then
      nproc
    elif command -v sysctl >/dev/null 2>&1; then
      sysctl -n hw.ncpu 2>/dev/null || echo 4
    else
      echo 4
    fi
  }

  # ra8_max_jobs -- the bounded per-job parallelism width every parallel
  # invocation must derive from (#328).
  #
  # Resolution order; the first value that is a positive integer wins:
  #   1. RA8_MAX_JOBS               explicit operator override -- the knob a
  #                                 shared box sets (e.g. RA8_MAX_JOBS=2) to cap
  #                                 what every concurrent job may consume.
  #   2. CMAKE_BUILD_PARALLEL_LEVEL the per-job budget the self-hosted runner
  #                                 already exports (4). Routing every site
  #                                 through here is what finally makes it govern
  #                                 make / ctest / xargs / cppcheck too, not
  #                                 just `cmake --build` -- the exact gap #328
  #                                 identified.
  #   3. cpu_count                  host core count, so an unconfigured
  #                                 single-user box still runs at full speed.
  ra8_max_jobs() {
    local candidate
    for candidate in "${RA8_MAX_JOBS:-}" "${CMAKE_BUILD_PARALLEL_LEVEL:-}"; do
      if [[ "$candidate" =~ ^[1-9][0-9]*$ ]]; then
        printf '%s\n' "$candidate"
        return 0
      fi
    done
    cpu_count
  }
fi
