#
# cmake/ccache.cmake -- wire ccache in as a compiler launcher when it is
# available, for BOTH the host test builds and the arm-none-eabi cross builds.
#
# Why: on the shared Linux verification box several agents concurrently build
# the same source tree on 12 cores, each recompiling everything from scratch
# with no object reuse between them (208 concurrent compiler processes and `ld`
# at 600% CPU were measured). A shared cache turns the second and subsequent
# builds of the same sources into cache hits regardless of which agent, which
# workspace, or which container produced the first one.
#
# CMAKE_<LANG>_COMPILER_LAUNCHER is used rather than a PATH shim (/usr/lib/ccache)
# on purpose: the launcher travels in the CMake cache, so it applies identically
# inside the ephemeral CI container, where PATH is the image's and any host shim
# directory does not exist.
#
# This file is included from THREE places, because no single listfile is on the
# path of every build:
#   - CMakeLists.txt              the root project (board_sim, host tools)
#   - tests/CMakeLists.txt        the host unit tests, included AFTER the
#                                 coverage options it must inspect
#   - cmake/toolchain-ra8d2.cmake for every cross build -- `make <app>`
#     configures examples/<app>/ standalone and never processes the root
#     listfile at all, which is exactly how build_all_examples.sh and the CI
#     build-cross job build all ~200 apps
# Including it from the root alone left the largest and most repetitive compile
# workload in the project with no cache whatsoever.
#
# Sharing hits across differing build directories requires two ccache settings
# that belong with the cache rather than here (see the box's ccache.conf):
#   base_dir = /      -- rewrite absolute paths in the command line to relative
#   hash_dir = false  -- do not hash the working directory into the result
# Without them, every ephemeral snapshot directory (a fresh mktemp per `make ci`
# run) would look like a different compilation and never hit.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Deliberately NOT include_guard()ed. The file is idempotent -- find_program
# caches, and the launcher is set to the same value under FORCE -- and a guard
# would make the coverage test below a one-shot: the toolchain file includes
# this before project() runs, i.e. before RA8_COVERAGE can exist, so a guarded
# second include from a listfile that DOES set it would be skipped and an
# instrumented build would silently keep the launcher.

# Coverage builds deliberately bypass ccache.
#
# gcov instrumentation writes the .gcno next to the object file and records
# absolute source and object paths inside it; a cached object replayed into a
# different build directory carries the ORIGINAL paths, so gcovr resolves
# nothing and reports no_working_dir_found. That failure is indistinguishable
# from a real coverage regression and has already cost this project a full
# diagnostic cycle from a stale .gcda -- reintroducing the same class through
# the cache would be a poor trade for build time on a gate that runs once per
# push. MC/DC (clang source-based profiles) is excluded for the same reason.
#
# scripts/clang_tidy.sh and tests/build_tests.sh already pass -DRA8_COVERAGE=OFF
# for the same underlying reason, so the only builds this opts out are the two
# that genuinely want instrumentation.
if(RA8_COVERAGE OR RA8_MCDC)
    message(STATUS "ccache: DISABLED for this build (coverage/MC-DC instrumentation)")
    return()
endif()

find_program(RA8_CCACHE_PROGRAM ccache)
if(NOT RA8_CCACHE_PROGRAM)
    message(STATUS "ccache: not found, building without a compiler cache")
    return()
endif()

foreach(_lang C CXX ASM)
    set(CMAKE_${_lang}_COMPILER_LAUNCHER "${RA8_CCACHE_PROGRAM}"
        CACHE STRING "ccache launcher for ${_lang}" FORCE)
endforeach()

message(STATUS "ccache: ENABLED via ${RA8_CCACHE_PROGRAM}")
