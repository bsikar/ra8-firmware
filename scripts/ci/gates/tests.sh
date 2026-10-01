#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/ci/gates/tests.sh -- Host test execution gates.
#
# SOURCED, NEVER EXECUTED. scripts/ci.sh sources every file in this directory
# and is the only entry point; RA8_GATE_REGISTRY -- the single list of what
# gates exist -- stays there too. These files hold gate BODIES only, so there
# is still exactly one home for a gate's definition and exactly one command
# for a workflow to call (`just quality::local::gate <name>`). Adding a second
# registry here would recreate the drift the single-definition rule exists to
# prevent.
#
# Gates in this file: unit-tests, ubsan, artefact-freshness,
# cache-bench

# --- unit-tests -----------------------------------------------------------
gate_unit_tests() (
  set -e
  # build_tests.sh defaults to `${CMAKE:-cmake}` and run_tests.sh drives ctest.
  # Without a guard an absent toolchain surfaces as a bare "command not found"
  # deep in a build log; name the missing dependency at the gate boundary
  # instead. A gate must never be able to report "nothing to run" as success.
  require_cmd cmake "apt-get install -y cmake"
  require_cmd ctest "ships with cmake; check the cmake install"
  require_cmd zig "the Zig ABI fixture is compiled by the unit-tests gate"
  require_tool_versions zig
  bash tests/build_tests.sh
  bash tests/run_tests.sh
)

# --- test-go --------------------------------------------------------------
gate_test_go() (
  set -e
  require_cmd go "the test-go gate needs the pinned Go toolchain (.devcontainer/Dockerfile GO_VERSION)"
  python3 scripts/checks/check_go.py --test --coverage --require
)

# --- test-zig -------------------------------------------------------------
gate_test_zig() (
  set -e
  require_cmd zig "the test-zig gate needs the Zig toolchain"
  require_tool_versions zig
  require_cmd clang-18 "reg_gen's generated C23 header contract uses the pinned host compiler"
  python3 scripts/checks/check_zig.py --selftest-test
  python3 scripts/checks/check_zig.py --require --test
)

# --- test-rust ------------------------------------------------------------
gate_test_rust() (
  set -e
  require_cmd rustc "the test-rust gate needs the pinned Rust toolchain"
  require_cmd cargo "the test-rust gate needs Cargo"
  require_cmd zig "the Rust ABI consumer links the Zig fixture"
  require_tool_versions rustc cargo zig
  python3 scripts/checks/check_rust.py --selftest-test
  python3 scripts/checks/check_rust.py --require --test
)

# --- ubsan ----------------------------------------------------------------
# The whole host suite rebuilt under -fsanitize=undefined in its own tree with
# UBSAN_OPTIONS=halt_on_error=1, so any undefined behaviour is a hard test
# failure. Pin the compiler like the coverage gate does: the ambient `gcc`
# changes with the runner image, and -Wconversion findings are
# compiler-version-specific.
#
# gcc-14, not gcc-13 (#489): gcc-13 was never a provisioned pin anywhere, only
# an assumption that Ubuntu 24.04's apt `gcc` metapackage happens to default
# to it -- true on the ra8-ci runner image, but the shared dev box runs Debian
# 12 (bookworm), which defaults to gcc-12 and has no gcc-13 package at all
# (backports included). gcc-14 is what this tree actually provisions
# everywhere that runs this gate: the runner image installs it by an explicit
# native toolchain pin in the Dockerfile and dev-box Ansible role, the dev box has it built from
# source at /usr/local/bin/gcc-14 (docs/TOOLCHAIN.md, "CONVERGED"), and every
# other host-compiler probe in this tree already prefers it first
# (scripts/report/tree_coverage.sh, scripts/emu/eil_all.sh, scripts/emu/smoke.sh,
# scripts/emu/matrix.sh all run `ra8_select_host_compiler gcc-14 gcc-13 ...`).
# Pinning ubsan to the one compiler every environment actually guarantees
# turns "gate fails loudly on a missing tool" into "gate does not need the
# missing tool", which is the stronger fix per CLAUDE.md's gate-honesty rule.
gate_ubsan() (
  set -e
  require_cmd gcc-14 "the UBSan gate pins gcc-14 to match the provisioned toolchain"
  CC=gcc-14 CXX=g++-14 /bin/bash -p scripts/dev/run_just.sh quality::local::test 1
)

# --- artefact-freshness ---------------------------------------------------
# #380: committed generated docs (docs/DRIVER_STATUS.md) must equal what their
# generator produces from the current tree. --selftest runs first, in both
# directions, so a checker that stopped comparing cannot pass as clean.
#
# The media-download codec (#715) rides here too, but by DIGEST rather than by
# regeneration: scripts/gen/gen_ra8_media_proto.sh --check needs the exact pinned
# protobuf-c 1.5.2 / libprotoc 35.1 pair, which neither the dev box nor this image has,
# and a guaranteed-red command is worse than none. check_proto_codec_pairing.py instead
# re-derives the SHA-256 of the schema and both generated files from the tree and
# compares them with .github/proto-codec-pairing.txt, which the regenerate script
# rewrites in the same command. That catches the two drift shapes the issue names -- a
# hand edit to a generated file, and a schema change with a forgotten regenerate --
# with no generator installed. It does NOT claim the committed C is what protoc-c would
# emit today; only a regenerate proves that, and #715 stays open for wiring the pinned
# generator into the image so it can run here.
#
# The SOUP consumer counts (#624) ride here for the same reason the artefacts do:
# they are a DERIVED number stated in prose, and nothing recomputed them. The
# threadx record claimed 45 example apps against a tree holding 47, and
# sbom_registry.py restated the same 45. check_soup_consumer_census.py re-derives
# every stated count from each app's own USES clause and fails on disagreement,
# and fails again when a checked number appears nowhere in the prose -- so the
# marker being checked cannot drift away from the sentence a reader actually sees.
#
# check_usbx_class_claims.py (#626) is the same defect one layer down: not a count
# but a CAPABILITY list. docs/SOUP/usbx.md advertised CDC-ACM, HID and MSC as
# "device + host" while cmake/usbx.cmake globs nothing at all out of
# common/usbx_host_classes/, and it omitted the DFU device class five HIL apps
# depend on. The checker resolves each file(GLOB) against the vendored sources and
# applies that variable's own list(FILTER EXCLUDE REGEX) lines the way CMake does,
# so the single-TU INQUIRY override does not read as dropping the whole MSC class,
# then compares the surviving set with the record's marker. A glob left matching
# nothing, or filtered empty, is a finding too: a class must not stay claimed by a
# pattern that quietly stopped resolving.
#
# check_bench_claims.py (#710) closes the same class in prose rather than in a
# build recipe. coprocessor/esp32c6/build.sh asserted the recipe had been "built,
# flashed and booted on the bench" in a comment written BEFORE the media component
# existed: true when written, silently widened to cover code added 19 days later,
# and nothing in the tree could tell. A claim that names no evidence cannot go
# stale, because there is nothing to check it against. So the checker asks one
# mechanical question of every completed bench/silicon verification claim in
# first-party sources -- does its own comment or prose block name a locator (a
# date, a commit, an issue, a file path, or a backticked app/symbol/artifact) --
# and leaves negated and forward-looking statements alone, since those are the
# honest shape. "On the bench" on its own is not a claim: this tree uses it
# constantly as a location (the bench host, the bench Pi).
#
# gen_ra8_media_proto.sh --selftest runs here too, and it is the odd one out: it
# exercises a --check this gate cannot itself run. That is the reason for it. The
# byte-exact comparison, the post-processing, the version pin and the missing-generator
# exit are all code no machine we build on has ever executed, so without the selftest
# they would first run on the day someone installs the pinned pair -- the worst moment
# to discover the compare was wrong. It needs no generator: it stubs one inside a
# throwaway git repo and asserts both directions.
gate_artefact_freshness() (
  set -e
  require_cmd python3 "the artefact-freshness gate regenerates docs via python generators"
  python3 scripts/checks/check_generated_artefacts.py --selftest
  python3 scripts/checks/check_generated_artefacts.py
  python3 scripts/checks/check_proto_codec_pairing.py --selftest
  python3 scripts/checks/check_proto_codec_pairing.py
  bash scripts/gen/gen_ra8_media_proto.sh --selftest
  python3 scripts/checks/check_soup_consumer_census.py --selftest
  python3 scripts/checks/check_soup_consumer_census.py
  python3 scripts/checks/check_usbx_class_claims.py --selftest
  python3 scripts/checks/check_usbx_class_claims.py
  python3 scripts/checks/check_bench_claims.py --selftest
  python3 scripts/checks/check_bench_claims.py
)

# --- cache-bench ----------------------------------------------------------
# Builds and runs cache_bench (the SLRU decision record), reader_vmem
# (drives the real ra8_vmem with a reader workload and emits a
# cache_bench-consumable trace) and glyph_bench (sweeps the real glyph atlas),
# re-confirming SLRU on the captured reader trace on every push. Both pinned
# host compilers are required: the rest of tools-build already proves clang-18
# and gcc-14 independently, and a benchmark is not a reason to accept a weaker
# warning/compiler bar. Each arm starts from clean outputs so Just cannot reuse
# the first compiler's binaries for the second arm.
#
# Despite the name this is NOT a wall-clock gate, so it belongs in the local
# suite and is stable under a loaded shared box (#326/#328). Every non-zero exit
# in cache_bench / reader_vmem / glyph_bench comes from a DETERMINISTIC failure
# -- an allocation or trace-build error, a get/put/verify data-integrity
# mismatch, or the SLRU policy losing on the fixed captured trace -- none of
# which depend on how busy the machine is. The wall_ns / MiB-s figures the tools
# print are informational only and gate nothing, so a load average of 156 (#328)
# changes the numbers on screen but never the PASS/FAIL verdict.
gate_cache_bench() (
  set -e
  require_cmd just "the cache benchmark builds through authoritative tool recipes"
  require_cmd clang-18 "the cache-bench gate pins clang-18 to match CI"
  require_cmd gcc-14 "the cache-bench gate pins gcc-14 as its second warning arm"
  require_tool_versions gcc-14
  local cc
  for cc in clang-18 gcc-14; do
    /bin/bash -p scripts/dev/run_just.sh tools::clean
    CC="$cc" /bin/bash -p scripts/dev/run_just.sh tools::bench_cache
  done
)
