#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
# shellcheck disable=SC2154  # FIRMWARE_DIR / BUILD_DIR / RC_INFRA and the print_* helpers come from scripts/checks/clang_tidy.sh, the only thing that sources this file
#
# scripts/checks/tidy/collect.sh -- What gets linted, and which pass owns it.
#
# SOURCED, NEVER EXECUTED. scripts/checks/clang_tidy.sh sources every file in
# this directory and is the only entry point. These fragments hold helper
# BODIES only: the option parsing, the pass orchestration and the selftest
# entry stay in the driver, so there is still exactly one way to run this.
#
# Functions here: source_path_is_live, collect_source_files,
# source_requires_firmware_headers, route_bucket

# ---------------------------------------------------------------------------
# Collect first-party source files (exclude vendor paths)
#
# Scope: EVERY first-party C-family file in the repository -- C, C++ and
# Objective-C, translation units and headers alike, under libs/, apps/,
# tests/, tools/, examples/, and port/. CLAUDE.md ("Scope: these
# standards apply to EVERY first-party file in the repository") makes the
# host tools, the host test suite and the firmware subject to exactly the
# same rules; a file being "just an emulator", "just a test" or "just an
# example" is not a reason to relax them.
#
# The ONLY exemptions are vendored SOUP under libs/third_party/ and
# apps/shared_libs/third_party/, plus generated tables (libs/ra8_fonts/ and
# tools/vela/generated/), matching the CLAUDE.md exemption list. Build trees
# and CMake-fetched deps are excluded because they are not source.
#
# Scope is derived from `git ls-files`, NOT from a directory glob. An earlier
# revision globbed `examples/*/*/main.c` to find app directories, which
# matched only the apps sitting exactly three levels deep while the tree's
# actual layout is examples/<tier>/.../<app>/src/main.c, up to six deep. That is the
# #296 / #332 / #358 / #359 / #360 defect: a scan list that silently stopped
# matching the tree. Deriving from git means a new directory at any depth is
# picked up the day it lands.
#
# HOW A FILE GETS A COMPILE COMMAND (#369, #370)
# ----------------------------------------------
# clang-tidy cannot analyse a translation unit it cannot parse, and it cannot
# parse one without the flags it is really compiled with. There are three
# sources of those flags here, and `route_bucket` below picks between them:
#
#   host       the unit-test build's compile_commands.json -- describes the
#              enrolled host-buildable C and C++ in libs/, apps/, tests/,
#              tools/, and hosted ports.
#   firmware   a CROSS-COMPILE compile_commands.json built by
#              scripts/builders/build_cross_compile_db.py, covering every
#              cross-compiled TU in examples/ and port/, firmware products
#              under apps/, board boot code, and the handful of libs/ TUs that
#              include ThreadX / NetX / USBX vendor headers.
#   fixed      a hand-assembled command, for the host dev tools whose own
#              per-tool build files never feed any compile database.
#
# The firmware bucket is what #369 was about. Pointing clang-tidy at the
# firmware against the HOST database was measured rather than assumed: it
# surfaced 135 findings across 96 files, and EVERY ONE was a
# clang-diagnostic-error rather than an actionable style finding. The answer
# was never a wider glob -- it was a database that describes how those TUs
# actually compile.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# The exact paths a generator owns, printed one per line.
#
# Read from scripts/checks/lint_coverage_rules.py, which is the ONE registry
# that classifies a path as `generated-source`; format_code.sh and the
# annotation walker already read the same classification. Linting generated
# output reports style on bytes nobody edits and that the generator will
# overwrite -- 79 findings on the two protobuf-c codec files alone, none of
# them actionable.
#
# It is deliberately a registry of EXACT paths and not a `*.pb-c.*` wildcard:
# a suffix rule silently exempts the next generated-looking file, which is the
# defect the registry was created to prevent. A new generated file has to be
# classified by hand, and until it is, it is linted as hand-authored C.
# ---------------------------------------------------------------------------
generated_source_paths() {
  python3 - <<'PY_GENERATED'
import sys

sys.path.insert(0, "scripts/checks")
from lint_coverage_rules import PATH_CLASS  # noqa: E402 -- path set above

for rel, cls in sorted(PATH_CLASS.items()):
    if cls == "generated-source":
        print(rel)
PY_GENERATED
}

source_path_is_live() {
  [[ -f "$1" ]]
}

collect_source_files() {
  cd "$FIRMWARE_DIR" || return 1
  local generated
  generated="$(generated_source_paths)"
  git ls-files --cached --others --exclude-standard |
    grep -E '\.(c|h|cpp|cc|cxx|hpp|hh|hxx|m)$' |
    grep -E '^(libs|tests|tools|apps|examples|port)/' |
    # Vendored SOUP and generated tables -- the CLAUDE.md exemption list.
    grep -Ev '^(libs/third_party/|apps/shared_libs/third_party/|libs/ra8_fonts/|tools/vela/generated/)' |
    # Build trees and CMake-fetched deps are not source.
    grep -Ev '(^|/)(build|build-[^/]*|_deps)/' |
    # Generator-owned bytes, by exact path (see generated_source_paths).
    grep -vxF -e "$generated" |
    while IFS= read -r f; do
      # A dirty migration can leave a staged rename/delete in the index while
      # its replacement is still untracked. Analyse the live working tree,
      # never a cached pathname whose file no longer exists.
      source_path_is_live "$f" || continue
      # Objective-C needs the macOS SDK's AppKit / CoreGraphics headers to
      # parse at all, so it can only be linted on a macOS host. Claim it only
      # where it can really be checked: `--list-files` feeds
      # check_lint_coverage.py, and claiming coverage this platform cannot
      # deliver would make the coverage matrix report a lint that never ran.
      case "$f" in
        *.m)
          [[ "$(uname -s)" == "Darwin" ]] || continue
          ;;
      esac
      printf '%s/%s\n' "$FIRMWARE_DIR" "$f"
    done || true
}

# ---------------------------------------------------------------------------
# Which pass owns a file -- the single place that decision is made.
#
# Prints one of: firmware | ra8p1 | tools | cxx | objc | host
#
# Kept as one function (rather than inlined into the loop that fills the pass
# lists) so `--selftest` can interrogate the routing directly. A router that
# quietly sent every file to one bucket would still "lint everything" and
# report success while analysing most files with the wrong flags.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Is this header a textual include-FRAGMENT rather than a translation unit?
#
# A header that declares a file-scope `static` function has no standalone TU by
# construction: `static` gives internal linkage to the INCLUDING translation
# unit, so the declaration only means anything textually. This tree uses the
# construct deliberately -- `*_contracts_internal.h` / `*_fixture.h` carry the
# forward declarations of a large .c file`s file-local helpers, which is how
# several suites got back under the file-size cap.
#
# Analysing such a header as its own TU parses it with no includer context, and
# every finding that comes back is either a clang-diagnostic-error or a naming
# finding derived from a type that never got declared -- 181 findings across 16
# headers, not one of them actionable, and not one of them in the committed
# baseline. Skipping the DIRECT analysis does not unlint them: .clang-tidy sets
# HeaderFilterRegex to every first-party .h, so their contents are still
# reported when the .c that includes them is analysed, with the right flags.
#
# `static inline` is deliberately NOT this class: a header full of inline
# accessors (the HAL register banks) is self-contained and parses standalone,
# so it stays in the normal passes. The test is a non-inline `static` FUNCTION
# declaration, not merely the word `static`.
# ---------------------------------------------------------------------------
# Headers that are include-FRAGMENTS, listed as mode|EXACT-repo-relative-path.
#
# A naming convention (`*_internal.h`, `*_fixture.h`) would be the wrong rule
# here: most headers with those names ARE self-contained -- libs/ra8_hal/src is
# full of them -- and a suffix wildcard would drop the whole private-header
# surface out of the analysis while reporting a smaller, cleaner run. The
# registry is therefore the one ownership manifest for the HeaderFilterRegex.
# `static-decl` rows are independently derived by the content rule below;
# `includer-context` rows document the exceptional parse contract that content
# alone cannot see. The selftest proves the registry, content-derived census,
# and shipped regex name exactly the same 28 authored headers.
TIDY_HEADER_FRAGMENT_RECORDS=(
  # Non-inline static declarations are contracts for their including TU, not
  # standalone translation units. The content census must rediscover every
  # one of these rows and reject any new unregistered fragment.
  "static-decl|apps/board/stand_alone/ereader/tests/inc/test_ra8_manga_stream.h"
  "static-decl|apps/shared_libs/epub/src/epub_xml_opf_internal.h"
  "static-decl|apps/shared_libs/epub/src/epub_xml_toc_internal.h"
  "static-decl|apps/shared_libs/mdl/tests/inc/test_mdl_fetch_fixture.h"
  "static-decl|apps/shared_libs/unarch/tests/inc/unarch_tar_fixture.h"
  "static-decl|apps/shared_libs/xml/src/xml_reader_internal.h"
  "static-decl|libs/if_ra8_vfs/src/fw_if_fs_ra8_vfs_contracts_internal.h"
  "static-decl|libs/ra8_sdmmc_spi/src/ra8_sdmmc_spi_core_contracts_internal.h"
  "static-decl|libs/ra8_sdmmc_spi/src/ra8_sdmmc_spi_io_contracts_internal.h"
  "static-decl|port/posix/src/fw_if_fs_posix_contracts_internal.h"
  "static-decl|port/posix/src/fw_if_fs_posix_stream_contracts_internal.h"
  "static-decl|tests/storage/inc/test_ra8_fs_format_fixture.h"
  "static-decl|tests/storage/inc/test_ra8_fs_check_util.h"
  "static-decl|tests/support/inc/fs_sparse_backend_test_util.h"
  "static-decl|tests/support/inc/ra8_gpio_test_contracts.h"
  "static-decl|tests/support/inc/ra8_jpeg_sw_decode_cov_fixture.h"
  "static-decl|tests/support/inc/ra8_keyboard_test_contracts.h"
  "static-decl|tests/support/inc/ra8_mpu_test_internal.h"
  "static-decl|tests/support/inc/ra8_power_profile_test_contracts.h"
  "static-decl|tests/support/inc/ra8_tls_net_test_contracts.h"
  "static-decl|tests/support/inc/ra8_tls_test_contracts.h"
  "static-decl|tests/support/inc/ra8_touch_cal_test_contracts.h"
  "static-decl|tests/usb/inc/test_ra8_usb_hmsc_enum_fixture.h"

  # Fixture constants and storage for the cache-store suite. It deliberately
  # inherits <stdint.h> and the ra8_cache_store types from its includer, so
  # standalone it cannot name uint8_t: 17 findings, all parse errors.
  "includer-context|tests/support/inc/ra8_cache_store_fixture.h"
  # Test-only fixture helpers consume the target-scoped repository-root
  # definition supplied by ra8_add_test; the owning test TUs analyse them.
  "includer-context|apps/shared_libs/reflow/tests/inc/reflow_v1_test_util.h"
  # This OS-shape header is included from upstream nimble_npl.h after upstream
  # defines ble_npl_event_fn. It deliberately cannot be parsed before that
  # contract, matching every vendored NimBLE OS port.
  "includer-context|port/nimble/inc/nimble/nimble_npl_os.h"
  # The PSA configuration is consumed as a preprocessor configuration unit by
  # TF-PSA-Crypto. The split platform body and umbrella are analysed through
  # those vendor includers, with the controlling configuration state present.
  "includer-context|port/mbedtls/inc/tf_psa_crypto_config.h"
  "includer-context|port/mbedtls/inc/tf_psa_crypto_config_platform.h"
)

header_fragment_mode() {
  local rel="$1" record
  for record in ${TIDY_HEADER_FRAGMENT_RECORDS[@]+"${TIDY_HEADER_FRAGMENT_RECORDS[@]}"}; do
    if [[ "${record#*|}" == "$rel" ]]; then
      printf '%s\n' "${record%%|*}"
      return 0
    fi
  done
  return 1
}

header_has_noninline_static_decl() {
  python3 "$FIRMWARE_DIR/scripts/checks/tidy/static_decl_scan.py" "$1"
}

header_static_decl_state() {
  local scan_rc
  if header_has_noninline_static_decl "$1"; then
    printf '%s\n' yes
    return 0
  else
    scan_rc=$?
  fi
  if [[ "$scan_rc" -eq 1 ]]; then
    printf '%s\n' no
    return 0
  fi
  print_error "static declaration classifier failed for $1"
  return "$RC_INFRA"
}

header_is_include_fragment() {
  local f="$1"
  [[ -f "$f" ]] || return 1
  local rel state
  rel="${f#"$FIRMWARE_DIR"/}"
  header_fragment_mode "$rel" >/dev/null && return 0
  if ! state="$(header_static_decl_state "$f")"; then
    return "$RC_INFRA"
  fi
  [[ "$state" == yes ]]
}

# ---------------------------------------------------------------------------
# The language half of the routing decision, kept apart from the path half so
# neither grows past the 60-line function cap.
#
# Prints a bucket and returns 0 when the file`s LANGUAGE (or its being a
# textual include fragment) settles the question; returns 1 when the answer
# depends on where the file lives.
# ---------------------------------------------------------------------------
route_bucket_by_language() {
  local f="$1"
  case "$f" in
    # A header that is only ever textually included cannot be its own TU.
    # Checked before every path rule: this is a property of the FILE, and the
    # class spans tests/, libs/ and port/ alike.
    *.h | *.hpp | *.hh | *.hxx)
      if header_is_include_fragment "$f"; then
        echo included && return 0
      else
        local fragment_rc=$?
        if [[ "$fragment_rc" -ne 1 ]]; then
          return "$fragment_rc"
        fi
      fi
      ;;
  esac
  case "$f" in
    *.m) echo objc && return 0 ;;
    # C++, but cross-compiled: the first-party Ethos-U55 kernel is pulled into
    # the TFLite-micro object library by cmake/tflite_micro.cmake and exists
    # only in an RA8P1 build, so the CROSS database is the one that knows how
    # it compiles. Checked before the generic C++ rule below.
    */ra8_ethosu_kernel.cc) echo firmware && return 0 ;;
    *.cpp | *.cc | *.cxx | *.hpp | *.hh | *.hxx) echo cxx && return 0 ;;
  esac
  return 1
}

# A libs/ TU that includes a ThreadX / NetX / USBX vendor header belongs to the
# firmware pass. The host database has no path to those headers, whereas the
# cross database contains the real compile command and include graph.
source_requires_firmware_headers() {
  grep -qlE '#\s*include\s*[<"](tx_api|nx_api|ux_api)\.h[">]' "$1" 2>/dev/null
}

# The FIRMWARE PRODUCT rule below deserves its own note, because it is the one
# path pattern here that is narrower than the root it sits in. apps/ is the
# products tier and its other inhabitants are host programs, routed to the
# tools bucket at the end of route_bucket() -- so the e-reader cannot be
# matched by `*/apps/*` and has to be named ahead of it. The set of firmware
# products is derived in lint_targets.firmware_app_dirs(); keep the two in
# step, exactly as HOST_PORT_ROOTS is kept in step with
# build_cross_compile_db.py.
route_bucket() {
  local f="$1" language_rc=0
  route_bucket_by_language "$f" || language_rc=$?
  [[ "$language_rc" -eq 0 ]] && return 0
  [[ "$language_rc" -eq 1 ]] || return "$language_rc"
  case "$f" in
    # ...except the HOSTED ports. port/posix/ binds fw_if_fs and
    # ra8_io_stream to the host kernel ABI, declares itself
    # `[Ring 4 / Host Port] {World: Host}`, and is compiled only by
    # tests/cmake/unit_tests.cmake -- so the HOST database is the one that
    # knows how it compiles, and no app cross-compiles it at all. Checked
    # before the generic port/ rule below, and kept in step with
    # HOST_PORT_ROOTS in scripts/builders/build_cross_compile_db.py.
    */port/posix/*) echo host && return 0 ;;
    # Tests nested below a firmware product/example are host translation
    # units registered by tests/cmake/unit_tests.cmake. They need that target's
    # exact definitions and app-local include graph, not an inferred
    # firmware/tool command. Keep this ahead of the product roots, matching
    # is_firmware_source() in build_cross_compile_db.py.
    */apps/board/stand_alone/*/tests/* | */apps/shared_libs/*/tests/* | \
      */examples/*/tests/*)
      echo host && return 0
      ;;
    # Cross-compiled firmware: examples/ and port/ in full.
    */examples/* | */port/*) echo firmware && return 0 ;;
    # A board layer's boot/ directory is the reset path compiled into every
    # image of that board -- firmware by any definition, but it lives under
    # libs/ and so used to fall through to the host bucket, whose database
    # does not contain it at all. clang-tidy then analysed it with default
    # HOSTED flags, which is precisely the wrong-flags failure this router
    # exists to prevent. It surfaced when `main` became `void main(void)`
    # behind `__STDC_HOSTED__ == 0`: a hosted-flags parse cannot see the
    # declaration and reports `use of undeclared identifier 'main'` (#707).
    */libs/ra8_board_*/src/boot/*) echo firmware && return 0 ;;
    # The e-reader image: a cross-compiled application built by ra8_add_app(),
    # registered in RA8_APPS and present in the cross database. Routing it to
    # the host bucket analysed its `void main(void)` as a hosted program
    # (#707). See the products-tier note above route_bucket().
    */apps/board/stand_alone/ereader/*) echo firmware && return 0 ;;
  esac
  if source_requires_firmware_headers "$f"; then
    echo firmware && return 0
  fi
  case "$f" in
    # apps/ rides the tools pass: a product is built by its own CMakeLists
    # exactly as a tool is, so it is absent from the host database and needs
    # the same hand-assembled compile command -- including the MDL_* fixture
    # defines tools_fixture_definitions() supplies. Routing it to the host
    # bucket would analyse every TU as clang-diagnostic-error instead.
    */tools/* | */apps/*) echo tools && return 0 ;;
    */ra8_npu.c | */ra8_npu.h | */ra8_npu_regs.h | \
      */ra8_npu_loader.c | */ra8_npu_loader.h | \
      */ra8_npu_quant.c | */ra8_npu_quant.h | \
      */ra8_ethosu_kernel.h | \
      */libs/ra8_board_ra8p1/*)
      echo ra8p1 && return 0
      ;;
  esac
  echo host
}
