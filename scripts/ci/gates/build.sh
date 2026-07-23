# shellcheck shell=bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
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
# ra8_viewer, ra8_fmt, mkbookimg, mkfontimg. They were linted (#296 widened
# clang-tidy to tools/) and NONE was ever built by a job, so a change that
# parsed and linted cleanly could break the build or the link with nothing
# going red -- and media_dl could not build on Linux at all, which is the only
# kind of runner this project has. tools/cache_bench, tools/glyph_bench and
# tools/reader_vmem are already built by the cache-bench gate; tools/board_sim
# by the board-sim gates; tools/epub_compile, tools/mcp and tools/vela are
# Python and are covered by lint-py-shell.
#
# The gate also holds them to NASA Power of 10 Rule 10. media_dl used to
# compile ten hand-written first-party firmware sources under a blanket `-w`,
# so the single build in the tree that compiles them for a 64-bit host could
# report nothing; check_tool_warning_flags.py reads the compile database and
# fails if that suppression -- or a missing -Werror -- ever comes back.
#
# clang-18 is pinned as the cache-bench and mcdc gates pin it: the tools and
# the firmware sources they pull in use C23 typed enums and `nullptr`, which
# the Debian gcc-12 a developer box may default to rejects outright.
#
# ra8_viewer's Cocoa window layer is macOS-only by design. Off the APPLE path
# CMake compiles ra8_viewer_view_stub.c in its place, so the portable reader
# core still builds, links, and renders here -- the whole tool is gated on
# Linux rather than skipped for the sake of its window backend.
# media_dl: build, link, and run its own CTest suite.
_tb_media_dl() (
  set -e
  local root="$1" jobs="$2"
  echo "tools-build: media_dl"
  # shellcheck disable=SC2154  # REPO_ROOT comes from scripts/ci.sh, the only thing that sources this file; shellcheck reports the variable once per file.
  CC=clang-18 cmake -S "$REPO_ROOT/tools/media_dl" -B "$root/media_dl" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  cmake --build "$root/media_dl" -j "$jobs"
  test -x "$root/media_dl/media_dl"
  ctest --test-dir "$root/media_dl" --output-on-failure
)

# ra8_viewer: build, link, and exercise the headless render. Linking is not
# evidence the reader still decodes anything, so the committed CBZ fixture is
# driven through the headless path and the pixels are checked -- a zero exit
# with no image would be a vacuous pass.
_tb_ra8_viewer() (
  set -e
  local root="$1" jobs="$2"
  echo "tools-build: ra8_viewer"
  CC=clang-18 cmake -S "$REPO_ROOT/tools/ra8_viewer" -B "$root/ra8_viewer" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  cmake --build "$root/ra8_viewer" -j "$jobs"
  test -x "$root/ra8_viewer/ra8_viewer"

  local ppm="$root/viewer_page0.ppm"
  "$root/ra8_viewer/ra8_viewer" \
    "$REPO_ROOT/tools/ra8_viewer/fixtures/sample.cbz" --headless --dump-ppm "$ppm"
  if [[ "$(head -c 2 "$ppm" 2>/dev/null)" != "P6" ]]; then
    echo "ERROR: ra8_viewer --headless did not write a P6 PPM." >&2
    echo "       A zero exit with no image would be a vacuous pass." >&2
    return 1
  fi
  echo "tools-build: headless render wrote $(wc -c <"$ppm") bytes of P6"

  # #298: the viewer meets attacker-supplied archives first, so its untrusted-
  # allocation guards (page-buffer cap, JOF atlas/band bounds) are exercised
  # here. The corpus refuses a lying/oversized/overflowing header cleanly and
  # still decodes a valid atlas -- a regression would otherwise need a human at
  # a window to notice.
  echo "tools-build: ra8_viewer malformed-input security corpus"
  bash "$REPO_ROOT/tools/ra8_viewer/tests/run_corpus.sh" \
    "$root/ra8_viewer/ra8_viewer" "$root/ra8_viewer_corpus"
)

# The remaining first-party CMake tools no job built. #335 asked for these to
# be enumerated rather than fixing media_dl alone. They have no test binary of
# their own, so building and linking them IS the check: each pulls a different
# slice of the firmware (ra8_fmt the JOF/JPEG stack, mkbookimg and mkfontimg
# the whole FAT/exFAT driver) host-side.
_tb_other_tools() (
  set -e
  local root="$1" jobs="$2" tool
  for tool in ra8_fmt mkbookimg mkfontimg; do
    echo "tools-build: $tool"
    CC=clang-18 cmake -S "$REPO_ROOT/tools/$tool" -B "$root/$tool" \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    cmake --build "$root/$tool" -j "$jobs"
    test -x "$root/$tool/$tool"
  done
)

gate_tools_build() (
  set -e
  require_cmd clang-18 "the tools-build gate pins clang-18 to match CI"
  require_cmd cmake
  require_cmd ctest

  # Prove the flag detector fires and stays quiet BEFORE trusting its verdict.
  # A checker asserted in neither direction reports success forever.
  python3 scripts/checks/check_tool_warning_flags.py --selftest

  local root="$REPO_ROOT/build/tools-build"
  local jobs
  jobs="$(ra8_max_jobs)"
  rm -rf "$root"

  _tb_media_dl "$root" "$jobs"
  _tb_ra8_viewer "$root" "$jobs"
  _tb_other_tools "$root" "$jobs"

  # --- the warning bar actually reached every first-party TU --------------
  python3 scripts/checks/check_tool_warning_flags.py \
    "$root/media_dl/compile_commands.json" \
    "$root/ra8_viewer/compile_commands.json" \
    "$root/ra8_fmt/compile_commands.json" \
    "$root/mkbookimg/compile_commands.json" \
    "$root/mkfontimg/compile_commands.json"

  echo "PASS: host tools build, link, test and hold the warning bar."
)

# --- build-cross ----------------------------------------------------------
# #178: RA8_STRICT_TOOLCHAIN=1 promotes toolchain-ra8d2.cmake's version
# mismatch warning to a hard error, so a runner with a skewed arm-gcc fails
# loudly instead of silently shipping version-divergent miniz codegen.
gate_build_cross() (
  set -e
  use_pinned_arm_toolchain
  require_cmd arm-none-eabi-gcc
  RA8_STRICT_TOOLCHAIN=1 bash scripts/builders/all_examples.sh
)

# --- docs -----------------------------------------------------------------
# --gate builds the single top-level Doxyfile with the project-pinned doxygen
# (downloaded + sha256-verified by provision_doxygen.sh on first use, cached
# under build/tools/ after) and writes the warning log. Using the same pinned
# version as docs-publish keeps this gate and the published site in lockstep.
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
  python3 scripts/checks/check_doc_diagrams.py --html build/docs-gate/html
)

# --- sbom -----------------------------------------------------------------
# Supply-chain provenance gate. Fails when the committed CycloneDX SBOM
# (docs/sbom/ra8-firmware.cdx.json) is stale or the vendored libs/third_party/
# tree drifted from the registry -- an uncatalogued SOUP directory, or a
# version macro that disagrees with the recorded version.
gate_sbom() {
  python3 scripts/gen/gen_sbom.py --check
}

# --- roadmap-stats --------------------------------------------------------
gate_roadmap_stats() {
  if [[ -f docs/ROADMAP.md ]]; then
    python3 scripts/report/roadmap_stats.py --check
  else
    echo "no docs/ROADMAP.md -- skipping roadmap_stats"
  fi
}
