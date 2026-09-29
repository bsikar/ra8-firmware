#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/ci/lib/macos_sdk_link.sh -- what the forced-SDK leg actually found.
#
# gate_macos_host_build ends with an INFORMATIONAL leg that deliberately may
# fail:
#
#     if (cd apps/host/image_pyramid && zig build -Dmacos-libsystem=sdk); then
#       printf 'informational: the SDK stub linked cleanly on this runner image\n'
#     else
#       printf 'informational: the SDK stub did NOT link here -- expected ... (#899)\n'
#     fi
#
# That leg is the ONE channel through which a real arm64 Mac reports the state
# of Apple's own libSystem stub back to this repository. Every other step in the
# gate builds against the pinned bundled stub, which is the workaround; this is
# the only step that still touches the thing #899 is about.
#
# And it was a boolean. `zig build -Dmacos-libsystem=sdk` exits non-zero for
# many reasons, and the else-branch above reports every one of them as the
# expected #899 failure:
#
#   - the SDK stub omits arm64-macos, so the link ends in an
#     `error: undefined symbol: _abort` wall. This is the finding. Expected.
#   - the stub cannot be loaded or resolved at all (moved SDK, unreadable
#     .tbd, `library not found for -lSystem`). Also an SDK finding, but a
#     different morning from the one above, so it is not folded in with it.
#   - `-Dmacos-libsystem` no longer exists, because the option was renamed or
#     dropped in tools/zig_build. zig answers `error: invalid option:
#     -Dmacos-libsystem` and exits non-zero, the else-branch calls that an
#     affected SDK, and the leg is DEAD: it will report the #899 shape every
#     night forever while measuring nothing at all. That is the same
#     green-gate-that-never-asked-its-question defect this lane keeps finding,
#     one layer out.
#   - anything else: a compile error in the app, a full disk, a broken
#     toolchain, a cache permission problem. None of these say anything about
#     Apple's SDK, and laundering them into "expected on an affected SDK"
#     hides a real regression behind a known bug.
#
# So the leg classifies its own outcome. The two SDK findings stay
# informational, because failing is the point of the leg. A dead option and an
# unrelated failure REFUSE: not because the SDK is bad, but because the gate
# can no longer tell whether it is, and a gate that cannot ask its question
# must not answer it.
#
# Classification reads the build OUTPUT, not just the status, so it needs the
# log. ra8_macos_sdk_link_run captures both and is what the gate calls.
#
# Normally sourced. Direct execution is reserved for ``--selftest`` (which the
# toolchain-parity gate runs) and ``--explain`` (the state table, quoted in
# docs/MACOS_HOST_BUILDS.md). The whole guarded block is idempotent, so any
# number of scripts can source it.
#
# bash 3.2 only: this file sits on the macOS gate path, where /bin/bash is
# 3.2.57. No associative arrays, no mapfile, no ${v^^}. check_macos_gate_bash32
# enforces it.

if [ -z "${_RA8_MACOS_SDK_LINK_SH:-}" ]; then
  _RA8_MACOS_SDK_LINK_SH=1

  # The build option the informational leg drives. Named once: the "option is
  # gone" state below exists precisely because this string can drift away from
  # tools/zig_build, and the selftest asserts the graph still declares it.
  K_RA8_MACOS_SDK_LINK_OPTION="macos-libsystem"
  K_RA8_MACOS_SDK_LINK_VALUE="sdk"

  # ra8_macos_sdk_link_classify <log-file> <exit-status> -- name the outcome.
  #
  # Order matters. `invalid option` is checked before the symbol wall because a
  # rejected option means nothing downstream ran, so any other text in the log
  # is stale. The unrelated-failure state is the fallback on purpose: a shape
  # nobody has seen yet must land somewhere that refuses, never in one of the
  # expected buckets.
  ra8_macos_sdk_link_classify() {
    _rmsl_log="$1"
    _rmsl_status="$2"

    if [ ! -f "${_rmsl_log}" ]; then
      echo "log_unreadable"
      return 0
    fi

    if [ "${_rmsl_status}" -eq 0 ]; then
      echo "linked"
      return 0
    fi

    # The option itself was refused: `error: invalid option: -Dmacos-libsystem`.
    # Measured against zig 0.14.1, which also prints the help-menu hint line.
    if grep -q "invalid option: -D${K_RA8_MACOS_SDK_LINK_OPTION}" "${_rmsl_log}"; then
      echo "option_gone"
      return 0
    fi
    # Older/newer wordings of the same thing, kept separate from the exact
    # match above so a phrasing change degrades to the right state instead of
    # falling through to unrelated_failure.
    if grep -qE "(no option named|unrecognized option|unknown option).*${K_RA8_MACOS_SDK_LINK_OPTION}" \
      "${_rmsl_log}"; then
      echo "option_gone"
      return 0
    fi

    # The #899 finding: the stub was read, it does not declare arm64-macos, so
    # the libc symbols never resolve.
    if grep -q "undefined symbol: _" "${_rmsl_log}"; then
      echo "symbols_unresolved"
      return 0
    fi

    # The stub could not be used at all. A different remedy from the above: the
    # SDK is missing or broken rather than merely incomplete.
    if grep -qE "library not found for -lSystem|unable to (find|load|open).*libSystem|no such file or directory.*libSystem" \
      "${_rmsl_log}"; then
      echo "stub_unusable"
      return 0
    fi

    echo "unrelated_failure"
  }

  # ra8_macos_sdk_link_informational <state> -- 0 when the state is a legitimate
  # outcome of the leg, 1 when the gate must refuse.
  ra8_macos_sdk_link_informational() {
    case "$1" in
      linked | symbols_unresolved | stub_unusable) return 0 ;;
      *) return 1 ;;
    esac
  }

  # ra8_macos_sdk_link_reason <state> -- one line, what was observed.
  ra8_macos_sdk_link_reason() {
    case "$1" in
      linked)
        printf 'the active SDK'\''s libSystem stub linked cleanly on this runner image\n'
        ;;
      symbols_unresolved)
        printf 'the link ended in an undefined-symbol wall, so the SDK stub does not declare arm64-macos\n'
        ;;
      stub_unusable)
        printf 'the SDK libSystem stub could not be resolved at all, so the link never got as far as symbols\n'
        ;;
      option_gone)
        printf 'zig refused -D%s, so this leg built nothing and measured nothing\n' \
          "${K_RA8_MACOS_SDK_LINK_OPTION}"
        ;;
      log_unreadable)
        printf 'the build output was not captured, so the outcome cannot be classified\n'
        ;;
      *)
        printf 'the build failed in a shape that says nothing about the macOS SDK\n'
        ;;
    esac
  }

  # ra8_macos_sdk_link_advice <state> -- what to do about it.
  ra8_macos_sdk_link_advice() {
    case "$1" in
      linked)
        printf 'note: news, not a problem. If arm64 Macs generally link the SDK stub again,\n'
        printf 'note: the #899 workaround (the pinned aarch64-macos query) can be revisited.\n'
        ;;
      symbols_unresolved)
        printf 'note: this is the #899 finding itself, on this runner image. The gate verdict\n'
        printf 'note: comes from the pinned-target legs above, which is why this one is only\n'
        printf 'note: informational.\n'
        ;;
      stub_unusable)
        printf 'note: distinct from the undefined-symbol finding: the stub was not readable,\n'
        printf 'note: not merely incomplete. Run scripts/ci/lib/macos_sdk.sh --report on the\n'
        printf 'note: runner to see which SDK state that is.\n'
        ;;
      option_gone)
        printf 'error: tools/zig_build no longer declares -D%s, so the only leg that\n' \
          "${K_RA8_MACOS_SDK_LINK_OPTION}"
        printf 'error: still touches Apple'\''s own libSystem stub is dead. Either restore the\n'
        printf 'error: option or point this leg at whatever replaced it, and update\n'
        printf 'error: K_RA8_MACOS_SDK_LINK_OPTION in scripts/ci/lib/macos_sdk_link.sh.\n'
        ;;
      log_unreadable)
        printf 'error: ra8_macos_sdk_link_run could not read back the build log it wrote.\n'
        printf 'error: check TMPDIR is writable on this runner.\n'
        ;;
      *)
        printf 'error: the failure above is not an SDK finding, so it is not excused as one.\n'
        printf 'error: read the build output and fix it, or if it IS a new SDK shape, add it\n'
        printf 'error: to ra8_macos_sdk_link_classify with its own state.\n'
        ;;
    esac
  }

  # ra8_macos_sdk_link_report <state> -- the log line, prefixed the way the rest
  # of this leg has always been prefixed so existing log readers still parse it.
  ra8_macos_sdk_link_report() {
    _rmsl_state="$1"
    if ra8_macos_sdk_link_informational "${_rmsl_state}"; then
      printf 'informational: [%s] %s\n' "${_rmsl_state}" \
        "$(ra8_macos_sdk_link_reason "${_rmsl_state}")"
      ra8_macos_sdk_link_advice "${_rmsl_state}"
    else
      printf 'error: [%s] %s\n' "${_rmsl_state}" \
        "$(ra8_macos_sdk_link_reason "${_rmsl_state}")" >&2
      ra8_macos_sdk_link_advice "${_rmsl_state}" >&2
    fi
  }

  # ra8_macos_sdk_link_run <root-dir> -- drive the leg, classify it, report it.
  # Returns 0 for an informational outcome and 1 when the gate must refuse.
  # The build output goes to the console as it always did AND into a log, so a
  # human reading a nightly sees the same thing they used to plus the verdict.
  ra8_macos_sdk_link_run() {
    _rmsl_root="$1"
    _rmsl_logdir="$(mktemp -d "${TMPDIR:-/tmp}/ra8-macos-sdk-link.XXXXXXXX")" || return 1
    _rmsl_logfile="${_rmsl_logdir}/build.log"
    _rmsl_rc=0
    (cd "${_rmsl_root}" && zig build "-D${K_RA8_MACOS_SDK_LINK_OPTION}=${K_RA8_MACOS_SDK_LINK_VALUE}") \
      >"${_rmsl_logfile}" 2>&1 || _rmsl_rc=$?
    cat "${_rmsl_logfile}"
    _rmsl_state="$(ra8_macos_sdk_link_classify "${_rmsl_logfile}" "${_rmsl_rc}")"
    ra8_macos_sdk_link_report "${_rmsl_state}"
    rm -rf "${_rmsl_logdir}"
    ra8_macos_sdk_link_informational "${_rmsl_state}"
  }

  # --- explain ---------------------------------------------------------------

  ra8_macos_sdk_link_explain() {
    printf 'scripts/ci/lib/macos_sdk_link.sh -- states of the forced-SDK informational leg\n\n'
    for _rmsl_s in linked symbols_unresolved stub_unusable option_gone unrelated_failure log_unreadable; do
      if ra8_macos_sdk_link_informational "${_rmsl_s}"; then
        _rmsl_verdict="informational"
      else
        _rmsl_verdict="REFUSES"
      fi
      printf '%-20s %-14s %s\n' "${_rmsl_s}" "${_rmsl_verdict}" \
        "$(ra8_macos_sdk_link_reason "${_rmsl_s}")"
    done
  }

  # --- selftest --------------------------------------------------------------

  _ra8_macos_sdk_link_case() {
    _rmsl_case_name="$1"
    _rmsl_case_want="$2"
    _rmsl_case_status="$3"
    _rmsl_case_log="$4"
    _rmsl_case_got="$(ra8_macos_sdk_link_classify "${_rmsl_case_log}" "${_rmsl_case_status}")"
    if [ "${_rmsl_case_got}" = "${_rmsl_case_want}" ]; then
      printf '  [ok] %s -> %s\n' "${_rmsl_case_name}" "${_rmsl_case_got}"
      return 0
    fi
    printf '  [FAIL] %s -> %s (wanted %s)\n' \
      "${_rmsl_case_name}" "${_rmsl_case_got}" "${_rmsl_case_want}"
    return 1
  }

  ra8_macos_sdk_link_selftest() {
    _rmsl_fails=0
    _rmsl_scratch="$(mktemp -d "${TMPDIR:-/tmp}/ra8-macos-sdk-link-selftest.XXXXXXXX")" || return 1

    printf 'macos_sdk_link.sh --selftest\n'

    # --- the clean case ------------------------------------------------------
    printf 'zig build\nBuild Summary: 3/3 steps succeeded\n' >"${_rmsl_scratch}/clean.log"
    _ra8_macos_sdk_link_case "exit 0 is linked" linked 0 "${_rmsl_scratch}/clean.log" ||
      _rmsl_fails=$((_rmsl_fails + 1))
    # Status wins over text: a log mentioning the wall but exiting zero is a
    # clean link (e.g. the phrase appearing in a passing test name).
    printf 'test: reports undefined symbol: _abort ... OK\n' >"${_rmsl_scratch}/noise.log"
    _ra8_macos_sdk_link_case "exit 0 beats wall-shaped text" linked 0 "${_rmsl_scratch}/noise.log" ||
      _rmsl_fails=$((_rmsl_fails + 1))

    # --- the #899 finding ----------------------------------------------------
    printf 'error: undefined symbol: _abort\n    note: referenced by ...\n' \
      >"${_rmsl_scratch}/wall.log"
    _ra8_macos_sdk_link_case "undefined symbol is the finding" symbols_unresolved 1 \
      "${_rmsl_scratch}/wall.log" || _rmsl_fails=$((_rmsl_fails + 1))

    # --- the stub could not be used -----------------------------------------
    printf 'error: library not found for -lSystem\n' >"${_rmsl_scratch}/nolib.log"
    _ra8_macos_sdk_link_case "missing -lSystem is stub_unusable" stub_unusable 1 \
      "${_rmsl_scratch}/nolib.log" || _rmsl_fails=$((_rmsl_fails + 1))
    printf 'error: unable to load libSystem.tbd: FileNotFound\n' >"${_rmsl_scratch}/noload.log"
    _ra8_macos_sdk_link_case "unloadable stub is stub_unusable" stub_unusable 1 \
      "${_rmsl_scratch}/noload.log" || _rmsl_fails=$((_rmsl_fails + 1))

    # --- the leg is dead -----------------------------------------------------
    # Exact text measured against zig 0.14.1.
    printf 'error: invalid option: -Dmacos-libsystem\nerror:   access the help menu with '\''zig build -h'\''\n' \
      >"${_rmsl_scratch}/badopt.log"
    _ra8_macos_sdk_link_case "invalid option is option_gone" option_gone 1 \
      "${_rmsl_scratch}/badopt.log" || _rmsl_fails=$((_rmsl_fails + 1))
    printf 'error: no option named macos-libsystem\n' >"${_rmsl_scratch}/badopt2.log"
    _ra8_macos_sdk_link_case "alternate wording is option_gone" option_gone 1 \
      "${_rmsl_scratch}/badopt2.log" || _rmsl_fails=$((_rmsl_fails + 1))
    # A rejected option means nothing downstream ran, so stale wall text in the
    # same log must not win. This is the ordering assertion.
    printf 'error: invalid option: -Dmacos-libsystem\nerror: undefined symbol: _abort\n' \
      >"${_rmsl_scratch}/both.log"
    _ra8_macos_sdk_link_case "option_gone outranks the wall" option_gone 1 \
      "${_rmsl_scratch}/both.log" || _rmsl_fails=$((_rmsl_fails + 1))

    # --- the fallback refuses ------------------------------------------------
    printf 'src/main.zig:12:5: error: expected type '\''u32'\'', found '\''bool'\''\n' \
      >"${_rmsl_scratch}/compile.log"
    _ra8_macos_sdk_link_case "a compile error is not an SDK finding" unrelated_failure 1 \
      "${_rmsl_scratch}/compile.log" || _rmsl_fails=$((_rmsl_fails + 1))
    printf 'error: AccessDenied writing to .zig-cache\n' >"${_rmsl_scratch}/cache.log"
    _ra8_macos_sdk_link_case "a cache failure is not an SDK finding" unrelated_failure 1 \
      "${_rmsl_scratch}/cache.log" || _rmsl_fails=$((_rmsl_fails + 1))
    printf '' >"${_rmsl_scratch}/empty.log"
    _ra8_macos_sdk_link_case "a silent failure is not an SDK finding" unrelated_failure 1 \
      "${_rmsl_scratch}/empty.log" || _rmsl_fails=$((_rmsl_fails + 1))
    _ra8_macos_sdk_link_case "a missing log is log_unreadable" log_unreadable 1 \
      "${_rmsl_scratch}/absent.log" || _rmsl_fails=$((_rmsl_fails + 1))

    # --- the verdict split, both directions ----------------------------------
    for _rmsl_s in linked symbols_unresolved stub_unusable; do
      if ra8_macos_sdk_link_informational "${_rmsl_s}"; then
        printf '  [ok] %s is informational\n' "${_rmsl_s}"
      else
        printf '  [FAIL] %s should be informational\n' "${_rmsl_s}"
        _rmsl_fails=$((_rmsl_fails + 1))
      fi
    done
    for _rmsl_s in option_gone unrelated_failure log_unreadable; do
      if ra8_macos_sdk_link_informational "${_rmsl_s}"; then
        printf '  [FAIL] %s should refuse\n' "${_rmsl_s}"
        _rmsl_fails=$((_rmsl_fails + 1))
      else
        printf '  [ok] %s refuses\n' "${_rmsl_s}"
      fi
    done

    # --- every state says something -----------------------------------------
    for _rmsl_s in linked symbols_unresolved stub_unusable option_gone unrelated_failure log_unreadable; do
      if [ -n "$(ra8_macos_sdk_link_reason "${_rmsl_s}")" ] &&
        [ -n "$(ra8_macos_sdk_link_advice "${_rmsl_s}" 2>&1)" ]; then
        printf '  [ok] %s has a reason and advice\n' "${_rmsl_s}"
      else
        printf '  [FAIL] %s is missing a reason or advice\n' "${_rmsl_s}"
        _rmsl_fails=$((_rmsl_fails + 1))
      fi
    done

    # --- the option this leg drives still exists -----------------------------
    # Without this, option_gone is a state that could only ever be reached by a
    # real nightly. The graph is the source of truth, so read it.
    _rmsl_graph="tools/zig_build/build.zig"
    if [ -f "${_rmsl_graph}" ]; then
      if grep -q "\"${K_RA8_MACOS_SDK_LINK_OPTION}\"" "${_rmsl_graph}"; then
        printf '  [ok] %s still declares "%s"\n' "${_rmsl_graph}" "${K_RA8_MACOS_SDK_LINK_OPTION}"
      else
        printf '  [FAIL] %s does not declare "%s" -- this leg measures nothing\n' \
          "${_rmsl_graph}" "${K_RA8_MACOS_SDK_LINK_OPTION}"
        _rmsl_fails=$((_rmsl_fails + 1))
      fi
      if grep -q "${K_RA8_MACOS_SDK_LINK_VALUE}" "${_rmsl_graph}"; then
        printf '  [ok] %s still accepts the "%s" value\n' \
          "${_rmsl_graph}" "${K_RA8_MACOS_SDK_LINK_VALUE}"
      else
        printf '  [FAIL] %s no longer mentions "%s"\n' \
          "${_rmsl_graph}" "${K_RA8_MACOS_SDK_LINK_VALUE}"
        _rmsl_fails=$((_rmsl_fails + 1))
      fi
    else
      printf '  [FAIL] %s not found (run from the repository root)\n' "${_rmsl_graph}"
      _rmsl_fails=$((_rmsl_fails + 1))
    fi

    # --- the gate really calls this -----------------------------------------
    # A classifier nothing invokes is decoration.
    _rmsl_gate="scripts/ci/gates/manual.sh"
    if [ -f "${_rmsl_gate}" ] && grep -q "ra8_macos_sdk_link_run" "${_rmsl_gate}"; then
      printf '  [ok] %s calls ra8_macos_sdk_link_run\n' "${_rmsl_gate}"
    else
      printf '  [FAIL] %s does not call ra8_macos_sdk_link_run\n' "${_rmsl_gate}"
      _rmsl_fails=$((_rmsl_fails + 1))
    fi

    rm -rf "${_rmsl_scratch}"

    if [ "${_rmsl_fails}" -ne 0 ]; then
      printf 'macos_sdk_link.sh --selftest: %s case(s) FAILED\n' "${_rmsl_fails}" >&2
      return 1
    fi
    printf 'macos_sdk_link.sh --selftest: PASS (clean, wall, unusable stub, dead option, ordering, fallback, verdict split, texts, option live, gate wired)\n'
    return 0
  }
fi

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  case "${1:-}" in
    --selftest) ra8_macos_sdk_link_selftest ;;
    --explain) ra8_macos_sdk_link_explain ;;
    *)
      echo "Usage: scripts/ci/lib/macos_sdk_link.sh --selftest | --explain" >&2
      exit 2
      ;;
  esac
fi
