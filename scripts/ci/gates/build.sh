#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/ci/gates/build.sh -- Things that have to compile, link or generate cleanly.
#
# SOURCED, NEVER EXECUTED. scripts/ci.sh sources every file in this directory
# and is the only entry point; RA8_GATE_REGISTRY -- the single list of what
# gates exist -- stays there too. These files hold gate BODIES only, so there
# is still exactly one home for a gate's definition and exactly one command
# for a workflow to call (bash scripts/ci.sh --gate <name>). Adding a second
# registry here would recreate the drift the single-definition rule exists to
# prevent.
#
# Gates in this file: tools-build, build-cross, docs, sbom, roadmap-stats

# --- tools-build ----------------------------------------------------------
# #335/#309: COMPILES AND LINKS every first-party CMake host tool -- media_dl,
# rabook_viewer, rabook_imagepack, mkbookimg, mkfontimg, exfat_mkimage,
# cache_bench and ra8_emulator. They were
# linted (#296 widened
# clang-tidy to tools/) and NONE was ever built by a job, so a change that
# parsed and linted cleanly could break the build or the link with nothing
# going red -- and media_dl could not build on Linux at all, which is the only
# kind of runner this project has. tools/glyph_bench and tools/reader_vmem are
# Make-only and are built under both compilers by the cache-bench gate;
# tools/epub_compile, tools/mcp and tools/vela are Python and are covered by
# lint-py-shell.
#
# The gate also holds them to NASA Power of 10 Rule 10. media_dl used to
# compile ten hand-written first-party firmware sources under a blanket `-w`,
# so the single build in the tree that compiles them for a 64-bit host could
# report nothing; check_tool_warning_flags.py reads the compile database and
# fails if that suppression -- or a missing -Werror -- ever comes back.
#
# clang-18 is one of two pinned arms. It is pinned as the cache-bench and mcdc
# gates pin it: the tools and the firmware sources they pull in use C23 typed
# enums and `nullptr`, which the Debian gcc-12 a developer box may default to
# rejects outright.
#
# gcc-14 is the SECOND arm (#356). clang-18 and gcc-14 catch different warning
# families, and a gate that holds the bar with one compiler holds only that
# compiler's bar: gcc-14's -Wformat-truncation caught a silent PATH_MAX
# path-join truncation in tools/media_dl that clang-18 did not flag. Both arms
# build, link and test under -Wall -Wextra -Werror; neither may degrade to a
# warning-only run, and check_tool_warning_flags.py --require-compilers makes a
# silently-dropped arm a hard failure rather than a vacuous pass.
#
# rabook_viewer's Cocoa window layer is macOS-only by design. Off the APPLE path
# CMake compiles ra8_viewer_view_stub.c in its place, so the portable reader
# core still builds, links, and renders here -- the whole tool is gated on
# Linux rather than skipped for the sake of its window backend.
#
# Every path below is $PWD -- the TREE UNDER TEST -- and never $REPO_ROOT
# (#546). $REPO_ROOT is the host checkout the runner was invoked from, which is
# not the tree the suite is gating: run_suite_on_snapshot cds into a clean
# snapshot of HEAD and every other gate reads it from there. Reaching back to
# $REPO_ROOT had two consequences, and the second one is what made it visible:
#
#   * the gate did not gate HEAD at all. In suite mode it configured, compiled
#     and tested the WORKING TREE's tools/ -- whatever was dirty in it -- and
#     left its build output there, while the snapshot beside it went unbuilt.
#   * on the containerised path it could not run. The host repo is bind-mounted
#     READ-ONLY at /workspace, so `cmake -B /workspace/build/tools-build/...`
#     fails at configure time with `CMake Error: Unable to (re)create the
#     private pkgRedirects directory`. That is what took win-ci -- the fleet's
#     second verification host, where `ci-gate-container` is the normal path --
#     out of ever reporting a full green, and it reproduces identically on the
#     dev box through `make ci-gate-container GATE=tools-build`.
#
# check_gate_bodies.py now rejects $REPO_ROOT in any gate body, so a gate
# cannot silently start measuring a different tree again.
# media_dl: build, link, and run its own CTest suite under compiler $1.
_tb_media_dl() (
  set -e
  local cc="$1" root="$2" jobs="$3"
  shift 3
  echo "tools-build[$cc]: media_dl"
  CC="$cc" cmake -S "$PWD/tools/media_dl" -B "$root/media_dl" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "$@"
  cmake --build "$root/media_dl" -j "$jobs"
  test -x "$root/media_dl/media_dl"
  ctest --test-dir "$root/media_dl" --output-on-failure
)

# rabook_viewer: build, link, and exercise the headless render. Linking is not
# evidence the reader still decodes anything, so the committed CBZ fixture is
# driven through the headless path and the pixels are checked -- a zero exit
# with no image would be a vacuous pass.
_tb_rabook_viewer() (
  set -e
  local cc="$1" root="$2" jobs="$3"
  shift 3
  echo "tools-build[$cc]: rabook_viewer"
  CC="$cc" cmake -S "$PWD/tools/rabook_viewer" -B "$root/rabook_viewer" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "$@"
  cmake --build "$root/rabook_viewer" -j "$jobs"
  test -x "$root/rabook_viewer/rabook_viewer"

  local ppm="$root/viewer_page0.ppm"
  "$root/rabook_viewer/rabook_viewer" \
    "$PWD/tools/rabook_viewer/fixtures/sample.cbz" --headless --dump-ppm "$ppm"
  if [[ "$(head -c 2 "$ppm" 2>/dev/null)" != "P6" ]]; then
    echo "ERROR: rabook_viewer --headless did not write a P6 PPM." >&2
    echo "       A zero exit with no image would be a vacuous pass." >&2
    return 1
  fi
  echo "tools-build: headless render wrote $(wc -c <"$ppm") bytes of P6"

  # #298: the viewer meets attacker-supplied archives first, so its untrusted-
  # allocation guards (page-buffer cap, JOF atlas/band bounds) are exercised
  # here. The corpus refuses a lying/oversized/overflowing header cleanly and
  # still decodes a valid atlas -- a regression would otherwise need a human at
  # a window to notice.
  echo "tools-build: rabook_viewer malformed-input security corpus"
  bash "$PWD/tools/rabook_viewer/tests/run_corpus.sh" \
    "$root/rabook_viewer/rabook_viewer" "$root/ra8_viewer_corpus"
)

# The remaining first-party CMake tools no job built. #335 asked for these to
# be enumerated rather than fixing media_dl alone. They have no test binary of
# their own, so building and linking them IS the check: each pulls a different
# slice of the firmware (rabook_imagepack the JOF/JPEG stack, mkbookimg, mkfontimg
# and exfat_mkimage the whole FAT/exFAT driver) host-side.
_tb_other_tools() (
  set -e
  local cc="$1" root="$2" jobs="$3" tool
  shift 3
  for tool in rabook_imagepack mkbookimg mkfontimg exfat_mkimage cache_bench ra8_emulator; do
    echo "tools-build[$cc]: $tool"
    CC="$cc" cmake -S "$PWD/tools/$tool" -B "$root/$tool" \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "$@"
    cmake --build "$root/$tool" -j "$jobs"
    test -x "$root/$tool/$tool"
  done
)

_tb_build_compiler() (
  set -e
  local cc="$1" root="$2" jobs="$3"
  local cmake_args=()
  if [[ "$cc" == gcc-14 ]]; then
    # The gcc arm is also the tool-wide UBSan arm, matching gate_ubsan's
    # halt-on-first-undefined-behaviour contract for the library host suite.
    cmake_args+=(
      -DCMAKE_C_FLAGS=-fsanitize=undefined\ -fno-sanitize-recover=undefined
      -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=undefined
    )
  fi
  _tb_media_dl "$cc" "$root" "$jobs" "${cmake_args[@]}"
  _tb_rabook_viewer "$cc" "$root" "$jobs" "${cmake_args[@]}"
  _tb_other_tools "$cc" "$root" "$jobs" "${cmake_args[@]}"
)

gate_tools_build() (
  set -e
  require_cmd clang-18 "the tools-build gate pins clang-18 to match CI"
  require_cmd gcc-14 "the tools-build gate's second warning arm pins gcc-14 (#356)"
  require_cmd cmake
  require_cmd ctest
  # gcc-14 is enforced to its pin the same way the other gates enforce theirs
  # (#333/#447): the wrong gcc silently changes which warnings the arm holds.
  require_tool_versions gcc-14

  # Prove the flag detector fires and stays quiet BEFORE trusting its verdict.
  # A checker asserted in neither direction reports success forever. The
  # selftest now also asserts the second-arm coverage guard fires both ways.
  python3 scripts/checks/check_tool_warning_flags.py --selftest

  local base="$PWD/build/tools-build"
  local jobs
  jobs="$(ra8_max_jobs)"
  rm -rf "$base"

  # Build, link and test every tool under BOTH pinned compilers. Each compiler
  # gets its own build tree so the two compile databases never overwrite each
  # other; both are handed to check_tool_warning_flags.py below.
  local cc
  export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
  for cc in clang-18 gcc-14; do
    _tb_build_compiler "$cc" "$base/$cc" "$jobs"
  done

  local dbs=()
  local tool
  for cc in clang-18 gcc-14; do
    for tool in media_dl rabook_viewer rabook_imagepack mkbookimg mkfontimg \
      exfat_mkimage cache_bench ra8_emulator; do
      dbs+=("$base/$cc/$tool/compile_commands.json")
    done
  done

  # --- the warning bar actually reached every first-party TU, under BOTH arms.
  # --require-compilers makes a silently-dropped gcc (or clang) arm a hard
  # failure instead of a vacuous pass (#356, #348/#355 class).
  python3 scripts/checks/check_tool_warning_flags.py \
    --require-compilers clang,gcc \
    --require-all-cmake-tools \
    "${dbs[@]}"

  echo "PASS: host tools build, link, test and hold the warning bar under clang-18 and gcc-14."
)

# --- build-cross ----------------------------------------------------------
# #178: RA8_STRICT_TOOLCHAIN=1 promotes toolchain-ra8d2.cmake's version
# mismatch warning to a hard error, so a runner with a skewed arm-gcc fails
# loudly instead of silently shipping version-divergent miniz codegen.
#
# RA8_BUILD_SHARDS/RA8_BUILD_SHARD (both read by all_examples.sh, and passed
# through from the workflow's matrix rather than as CLI arguments so the
# workflow step stays the bare `--gate build-cross` driver ci-parity requires)
# build only a stride slice of the app list. Unset -- the local suite -- builds
# everything, exactly as before. Whatever the split, the build-cross-union gate
# below proves the shards covered the tree.
gate_build_cross() (
  set -e
  use_pinned_arm_toolchain
  require_cmd arm-none-eabi-gcc
  RA8_STRICT_TOOLCHAIN=1 bash scripts/builders/all_examples.sh
)

# --- build-cross-union ----------------------------------------------------
# Proves the sharded cross-build was WHOLE. Sharding a gate across parallel
# jobs silently weakens it unless something checks the union: a shard that was
# skipped, cancelled, or sliced to nothing emits no error, and the stack-usage
# aggregate downstream would just measure fewer .su files and still clear its
# floor on the shards that did run -- a gate quietly checking less than it
# claims, which is this tree's most-repeated defect.
#
# The checker re-derives the app list itself instead of trusting the manifest
# a shard wrote (a broken discovery would otherwise agree with itself), and
# fails on a missing shard, an unbuilt app, or an app claimed twice. Its
# --selftest runs first and asserts both directions, so a detector that
# stopped matching cannot pass as a clean gate.
#
# Unsharded (the local suite) this is N=1 and still a real check: it proves
# every discovered app reached the build.
gate_build_cross_union() (
  set -e
  python3 scripts/checks/check_build_shard_union.py --selftest
  python3 scripts/checks/check_build_shard_union.py --shards "${RA8_BUILD_SHARDS:-1}"
)

# --- docs -----------------------------------------------------------------
# --gate builds the single top-level Doxyfile with the project-pinned doxygen
# (downloaded + sha256-verified by provision_doxygen.sh on first use) and writes
# the warning log. Using the same pinned version as docs-publish keeps this gate
# and the published site in lockstep.
#
# The pinned binary caches in the persistent tool cache scripts/ci.sh provides
# (RA8_TOOLS_CACHE -> /toolcache in the container, /var/cache/ra8-tools
# natively), not the per-run build/tools/ that each ephemeral snapshot destroys.
# Without that the download would repeat every run and FAIL offline (#326).
gate_docs() (
  set -e
  # graphviz is a hard dependency, not a nice-to-have: build_docs.sh degrades to
  # text-only output when `dot` is absent, and doxygen then warns on every
  # author-written diagram block, which this gate reports as a failure. Without
  # this check that surfaces as a dozen confusing warnings about the .md files
  # rather than the one true cause. Fail on the real reason instead.
  require_cmd dot
  bash scripts/builders/docs.sh --gate
  local log="build/docs-gate/doxygen-warnings.log"
  if [[ ! -f "$log" ]]; then
    echo "FAIL: doxygen warning log not produced at $log" >&2
    return 1
  fi
  # Filter known-benign Doxygen warnings:
  #   - "for \ref command" -- Doxygen treats Markdown links in README.md as
  #     \ref directives; targets outside INPUT "fail" to resolve but render.
  #   - "multiple documentation sections" / "from the argument list of" --
  #     @retval / @param present in both the public header (canonical) and the
  #     .c definition. Cosmetic, no output impact.
  local relevant_warnings
  relevant_warnings="$(grep "warning:" "$log" |
    grep -v "for .ref command" |
    grep -v "multiple documentation sections" |
    grep -v "from the argument list of " |
    grep -v "multiple @param documentation sections" |
    grep -v "has multiple documentation sections" |
    grep -v "tag INCLUDE_PATH:" |
    grep -v "is not a readable file or directory" |
    grep -v "found more than one .mainpage comment block" |
    grep -v "End of list marker found without any preceding list items" |
    grep -v "Invalid list item found" |
    grep -v "Found unknown command" |
    grep -v "explicit link request to" |
    grep -v "argument '.*' of command @param is not found" |
    grep -v "found documented return type for .* that does not return anything" |
    grep -v "Problems running latex" || true)"
  if [[ -n "$relevant_warnings" ]]; then
    echo "Doxygen reported warnings:"
    echo "$relevant_warnings"
    return 1
  fi
  # A clean warning log does NOT mean the diagrams rendered. Doxygen drops an
  # authored diagram silently in several ways (HAVE_DOT=NO, a dot layout that
  # produces an empty SVG, a block doxygen never parsed), and the page still
  # publishes HTTP 200 with its prose intact. This counts what actually reached
  # the generated HTML and compares it against the source. It runs here, inside
  # the docs gate, because this is where the built HTML it inspects exists.
  #
  # --selftest first: the checker HAD one and no gate ran it, so the detector
  # behind the diagram guarantee was itself unverified (#531).
  python3 scripts/checks/check_doc_diagrams.py --selftest
  python3 scripts/checks/check_doc_diagrams.py --html build/docs-gate/html
)

# --- sbom -----------------------------------------------------------------
# Supply-chain provenance gate. Fails when the committed CycloneDX SBOM
# (docs/sbom/ra8-firmware.cdx.json) is stale or the vendored libs/third_party/
# tree drifted from the registry -- an uncatalogued SOUP directory, or a
# version macro that disagrees with the recorded version.
# The --check pass is only worth its status because the SHA-256 digests it
# compares are RE-DERIVED from libs/third_party/ on every run. They used to be
# hand-transcribed literals in sbom_registry.py -- present on 4 of 23
# components, absent from the one that had actually drifted -- so --check
# compared a constant with itself and appending a line to a vendored source
# still printed "SBOM matches the tree" with status 0 (#538). --selftest runs
# FIRST and proves the digest fires on a mutated byte and stays quiet on an
# unchanged tree, so a detector that stopped detecting cannot pass as clean.
gate_sbom() (
  set -e
  python3 scripts/gen/gen_sbom.py --selftest
  python3 scripts/gen/gen_sbom.py --check
)

# --- soup-upstream --------------------------------------------------------
# The other half of the provenance claim (#548). The sbom gate above re-derives
# a digest over each vendored tree, which proves only that the tree has not
# changed since the SBOM was regenerated -- a tree that was already wrong at
# vendor-in hashes faithfully and reports clean forever. This gate compares
# every vendored file against the blob SHA-1 its UPSTREAM project publishes for
# the pinned revision, recorded in docs/sbom/upstream/*.manifest by a real
# fetch. Two hashes from two projects, so nothing is compared with itself.
#
# Offline by construction: the manifests are committed, so a push does not
# depend on twenty upstream hosts being reachable. The networked half is the
# weekly soup-upstream-refresh gate, which re-fetches and catches what this one
# structurally cannot -- a tag that moved under a pin.
#
# --selftest FIRST, and it drives run_check() itself against a scratch git
# repository: a mutated blob, a lost file, an undeclared patch, a collapsed
# scan. This claim was asserted in three places and checked by nothing, so
# every tree passed it -- including one that had drifted.
gate_soup_upstream() (
  set -e
  python3 scripts/checks/check_soup_upstream.py --selftest
  python3 scripts/checks/check_soup_upstream.py
)

# --- roadmap-stats --------------------------------------------------------
gate_roadmap_stats() (
  set -e
  # A MISSING ROADMAP.md is a failure, not a skip. This gate used to `echo
  # "no docs/ROADMAP.md -- skipping"` and return 0, so a `git mv` of that one
  # file would have turned the gate green forever while checking nothing --
  # the same shape as every other finding under the gate-honesty epic (#190).
  if [[ ! -f docs/ROADMAP.md ]]; then
    echo "ERROR: docs/ROADMAP.md is missing; the roadmap-stats gate has" >&2
    echo "       nothing to check and must not report success." >&2
    echo "       Restore the file, or delete this gate and its registry row." >&2
    return 1
  fi
  python3 scripts/report/roadmap_stats.py --check
)
