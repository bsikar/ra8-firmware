#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/ci/lib/macos_sdk.sh -- is there an active macOS SDK, really.
#
# `command -v xcrun` is not a question about the SDK. macOS ships /usr/bin/xcrun
# as a stub that exists on every install, whether or not any developer tools are
# behind it. With no Command Line Tools and no Xcode selected, the stub is still
# on PATH and still executable; it fails only when RUN, with
#
#     xcrun: error: invalid active developer path (/Library/Developer/CommandLineTools),
#            missing xcrun at: /Library/Developer/CommandLineTools/usr/bin/xcrun
#
# Why this matters here (#899): gate_macos_host_build used `require_cmd xcrun`
# as its SDK precondition. That check passes on a Mac with no SDK at all, and
# then the gate goes GREEN having measured nothing it claims to measure. The
# build graph probes the SDK with `xcrun --show-sdk-path`; when the probe comes
# back empty it reports `sdk_not_probed` and pins the bundled libSystem stub,
# which is the right thing for an ordinary local build and is exactly wrong as
# a gate verdict: the gate exists to say whether the NATIVE SDK link path works
# on this machine, and on a machine with no SDK that question was not asked.
# Every root then builds against the bundled stub, every step exits zero, and
# the forced-SDK informational leg -- the one channel through which a real Mac
# reports the state of Apple's stub back to this repo -- records nothing.
#
# So the precondition has to RUN the probe and read its answer. The states below
# are kept apart because they need different fixes: no tools installed at all,
# an `xcode-select` pointing at an install that has moved, an unaccepted Xcode
# licence, a path that no longer exists on disk, and an SDK whose libSystem stub
# is missing are five different mornings.
#
# The graph's own forgiving behaviour is deliberately left alone. A developer
# with no Command Line Tools can still build the host apps here: they are
# libc-only and the bundled stub serves them. It is only the gate that must
# refuse, and refuse with the remedy.
#
# Normally sourced. Direct execution is reserved for ``--selftest`` (which the
# toolchain-parity gate runs) and ``--report`` (the human diagnosis, quoted in
# docs/MACOS_HOST_BUILDS.md); the whole guarded block is idempotent so any
# number of scripts can source it.

if [ -z "${_RA8_MACOS_SDK_SH:-}" ]; then
  _RA8_MACOS_SDK_SH=1

  # The stub the arm64 link path needs to read, relative to the SDK root.
  K_RA8_MACOS_SDK_STUB_REL="usr/lib/libSystem.tbd"

  # _ra8_macos_sdk_uname -- platform probe through fixed absolute paths, so a
  # hostile or merely odd PATH cannot redirect it. Same shape as host_arch.sh.
  _ra8_macos_sdk_uname() {
    if [ -x /usr/bin/uname ]; then
      /usr/bin/uname "$@"
    elif [ -x /bin/uname ]; then
      /bin/uname "$@"
    else
      echo "macos_sdk.sh: no fixed-path uname found" >&2
      return 1
    fi
  }

  # _ra8_macos_sdk_xcrun_bin -- the xcrun the BUILD GRAPH will execute.
  #
  # Deliberately PATH lookup rather than this tree's usual fixed-path probe:
  # tools/zig_build/build.zig spawns the bare name `xcrun`, so PATH is what
  # decides which binary answers, and a precondition that consulted a different
  # one could pass while the graph failed. Falls back to the /usr/bin stub so a
  # stripped PATH still yields the honest diagnosis instead of "absent".
  _ra8_macos_sdk_xcrun_bin() {
    local found
    if found="$(command -v xcrun 2>/dev/null)" && [ -n "${found}" ]; then
      printf '%s\n' "${found}"
      return 0
    fi
    if [ -x /usr/bin/xcrun ]; then
      printf '%s\n' /usr/bin/xcrun
      return 0
    fi
    return 1
  }

  # _ra8_macos_sdk_show_sdk_path <bin> -- run the probe the graph runs.
  #
  # stdout is the SDK path. stderr is appended to _RA8_MACOS_SDK_STDERR_FILE
  # when set, because the error TEXT is what separates "no tools installed"
  # from "licence not accepted"; it is kept rather than discarded for exactly
  # that reason. Exit status is xcrun's own.
  _ra8_macos_sdk_show_sdk_path() {
    local bin="$1"
    if [ -n "${_RA8_MACOS_SDK_STDERR_FILE:-}" ]; then
      "${bin}" --show-sdk-path 2>>"${_RA8_MACOS_SDK_STDERR_FILE}"
    else
      "${bin}" --show-sdk-path 2>/dev/null
    fi
  }

  # _ra8_macos_sdk_classify_failure <stderr-text> -- which failure this was.
  #
  # Matched on Apple's own wording. The licence case is tested first because an
  # unaccepted licence also mentions paths, while the reverse is not true.
  _ra8_macos_sdk_classify_failure() {
    local text="$1"
    case "${text}" in
      *"license"* | *"licence"*) printf 'license_unaccepted\n' ;;
      *"invalid active developer path"* | *"unable to find utility"* | *"active developer directory"*)
        printf 'developer_dir_invalid\n'
        ;;
      *) printf 'xcrun_failed\n' ;;
    esac
  }

  # ra8_macos_sdk_state [os] -- one token naming what this host has.
  #
  # Also sets RA8_MACOS_SDK_PATH, RA8_MACOS_SDK_STUB and RA8_MACOS_SDK_ERROR
  # for the caller, so the diagnosis can quote the path and xcrun's own words
  # without probing twice. Every token is a distinguishable state of the
  # machine with its own remedy; `ok` is the only one this gate can work with.
  ra8_macos_sdk_state() {
    local os_name="${1:-}" bin path scratch status text
    RA8_MACOS_SDK_PATH=""
    RA8_MACOS_SDK_STUB=""
    RA8_MACOS_SDK_ERROR=""

    [ -n "${os_name}" ] || os_name="$(_ra8_macos_sdk_uname -s)" || return 1
    if [ "${os_name}" != "Darwin" ]; then
      printf 'not_macos\n'
      return 0
    fi

    if ! bin="$(_ra8_macos_sdk_xcrun_bin)"; then
      printf 'xcrun_absent\n'
      return 0
    fi

    scratch="$(mktemp "${TMPDIR:-/tmp}/ra8-macos-sdk.XXXXXXXX")" || return 1
    # errexit off around the CALL only, never `|| status=$?`: the callee then
    # runs in a normal errexit context, and the state `||` sets would propagate
    # into nested subshells that `set -e` cannot rescue.
    set +e
    _RA8_MACOS_SDK_STDERR_FILE="${scratch}" path="$(_ra8_macos_sdk_show_sdk_path "${bin}")"
    status=$?
    set -e
    text="$(cat "${scratch}" 2>/dev/null)"
    rm -f "${scratch}"
    RA8_MACOS_SDK_ERROR="${text}"

    if [ "${status}" -ne 0 ]; then
      _ra8_macos_sdk_classify_failure "${text}"
      return 0
    fi
    if [ -z "${path}" ]; then
      printf 'sdk_path_empty\n'
      return 0
    fi

    RA8_MACOS_SDK_PATH="${path}"
    if [ ! -d "${path}" ]; then
      printf 'sdk_path_missing\n'
      return 0
    fi

    RA8_MACOS_SDK_STUB="${path}/${K_RA8_MACOS_SDK_STUB_REL}"
    if [ ! -f "${RA8_MACOS_SDK_STUB}" ]; then
      printf 'stub_missing\n'
      return 0
    fi
    printf 'ok\n'
  }

  # ra8_macos_sdk_stub_path <sdk-path> -- where the libSystem stub lives.
  ra8_macos_sdk_stub_path() {
    printf '%s/%s\n' "$1" "${K_RA8_MACOS_SDK_STUB_REL}"
  }

  # ra8_macos_sdk_usable <state> -- success only when the SDK can be read.
  ra8_macos_sdk_usable() {
    [ "$1" = "ok" ]
  }

  # ra8_macos_sdk_state_reason <state> -- one line, in plain words.
  ra8_macos_sdk_state_reason() {
    case "$1" in
      not_macos) printf 'this host is not macOS, so there is no active SDK to read\n' ;;
      xcrun_absent) printf 'no xcrun was found on PATH or at /usr/bin/xcrun\n' ;;
      developer_dir_invalid)
        printf 'xcrun ran but no developer directory is active: the Command Line Tools are not installed, or xcode-select points at an install that has moved\n'
        ;;
      license_unaccepted) printf 'xcrun refuses until the Xcode licence is accepted\n' ;;
      xcrun_failed) printf 'xcrun --show-sdk-path failed for a reason this probe does not recognise\n' ;;
      sdk_path_empty) printf 'xcrun --show-sdk-path succeeded but printed nothing\n' ;;
      sdk_path_missing) printf 'xcrun named an SDK path that is not a directory on this disk\n' ;;
      stub_missing) printf 'the active SDK has no usr/lib/libSystem.tbd, so there is no stub to read\n' ;;
      ok) printf 'an active SDK is readable and carries a libSystem stub\n' ;;
      *) printf 'unknown SDK state\n' ;;
    esac
  }

  # ra8_macos_sdk_state_advice <state> -- the remedy, printed by whoever
  # refused, on the stream they refused to.
  ra8_macos_sdk_state_advice() {
    case "$1" in
      not_macos)
        printf 'Run the macos-host-build gate on an arm64 macOS host; a Linux checkout\n'
        printf 'cannot exercise the SDK link path at all (#899).\n'
        ;;
      xcrun_absent | developer_dir_invalid)
        printf 'Install the Command Line Tools and point xcode-select at them:\n'
        printf '    xcode-select --install\n'
        printf '    sudo xcode-select --reset      # or --switch /Applications/Xcode.app\n'
        printf 'then check the probe the build graph uses:\n'
        printf '    xcrun --show-sdk-path\n'
        ;;
      license_unaccepted)
        printf 'Accept the licence, then re-run the gate:\n'
        printf '    sudo xcodebuild -license accept\n'
        ;;
      xcrun_failed | sdk_path_empty)
        printf 'Run the probe by hand and read its error, which is the real diagnosis:\n'
        printf '    xcrun --show-sdk-path\n'
        ;;
      sdk_path_missing)
        printf 'The selected SDK has been moved or deleted. Re-point xcode-select and retry:\n'
        printf '    xcode-select -p\n'
        printf '    sudo xcode-select --reset\n'
        ;;
      stub_missing)
        printf 'This SDK cannot answer the #899 question. Reinstall the Command Line Tools,\n'
        printf 'or select an SDK that ships usr/lib/libSystem.tbd:\n'
        printf '    xcode-select --install\n'
        ;;
      *)
        printf 'Run `bash scripts/ci/lib/macos_sdk.sh --report` and read the state it names.\n'
        ;;
    esac
  }

  # ra8_macos_sdk_report [state] -- the diagnosis, on stdout.
  ra8_macos_sdk_report() {
    local state="${1:-}"
    [ -n "${state}" ] || state="$(ra8_macos_sdk_state)" || return 1
    printf 'macOS SDK state: %s\n' "${state}"
    printf '  %s\n' "$(ra8_macos_sdk_state_reason "${state}")"
    [ -n "${RA8_MACOS_SDK_PATH}" ] && printf '  sdk:  %s\n' "${RA8_MACOS_SDK_PATH}"
    [ -n "${RA8_MACOS_SDK_STUB}" ] && printf '  stub: %s\n' "${RA8_MACOS_SDK_STUB}"
    if [ -n "${RA8_MACOS_SDK_ERROR}" ]; then
      printf '  xcrun said: %s\n' "${RA8_MACOS_SDK_ERROR}"
    fi
    return 0
  }

  # ra8_macos_sdk_require -- gate precondition. Prints the diagnosis either
  # way, and on anything but `ok` prints the remedy to stderr and fails.
  #
  # This replaces `require_cmd xcrun`, which passes on a Mac with no SDK and
  # lets the gate report a green verdict on a question it never asked.
  ra8_macos_sdk_require() {
    local state
    state="$(ra8_macos_sdk_state)" || return 1
    ra8_macos_sdk_report "${state}"
    if ra8_macos_sdk_usable "${state}"; then
      return 0
    fi
    printf 'error: macos-host-build needs a readable active SDK: %s.\n' \
      "$(ra8_macos_sdk_state_reason "${state}")" >&2
    printf 'error: without one the graph pins the bundled libSystem stub, every root\n' >&2
    printf 'error: builds, and the gate reports green for a link path it never took (#899).\n' >&2
    ra8_macos_sdk_state_advice "${state}" >&2
    return 1
  }

  # --- selftest -------------------------------------------------------------

  # The three seams are replaced wholesale. The filesystem is NOT stubbed: the
  # sdk_path_missing / stub_missing / ok rows use real directories in a scratch
  # tree, so the on-disk checks are proved rather than simulated.
  _ra8_macos_sdk_stub_seams() {
    _RA8_MACOS_SDK_STUB_OS="$1"
    _RA8_MACOS_SDK_STUB_BIN="$2"
    _RA8_MACOS_SDK_STUB_OUT="$3"
    _RA8_MACOS_SDK_STUB_ERR="$4"
    _RA8_MACOS_SDK_STUB_RC="$5"
    : >"${_RA8_MACOS_SDK_CALLLOG}"
    _ra8_macos_sdk_uname() {
      case "$1" in
        -s) printf '%s\n' "${_RA8_MACOS_SDK_STUB_OS}" ;;
        *) return 1 ;;
      esac
    }
    _ra8_macos_sdk_xcrun_bin() {
      [ "${_RA8_MACOS_SDK_STUB_BIN}" = "absent" ] && return 1
      printf '%s\n' "${_RA8_MACOS_SDK_STUB_BIN}"
    }
    _ra8_macos_sdk_show_sdk_path() {
      printf '%s\n' "$1" >>"${_RA8_MACOS_SDK_CALLLOG}"
      [ -n "${_RA8_MACOS_SDK_STUB_ERR}" ] &&
        printf '%s\n' "${_RA8_MACOS_SDK_STUB_ERR}" >>"${_RA8_MACOS_SDK_STDERR_FILE:-/dev/null}"
      [ -n "${_RA8_MACOS_SDK_STUB_OUT}" ] && printf '%s\n' "${_RA8_MACOS_SDK_STUB_OUT}"
      return "${_RA8_MACOS_SDK_STUB_RC}"
    }
  }

  ra8_macos_sdk_selftest() {
    local fails=0 got scratch sdk
    local _RA8_MACOS_SDK_CALLLOG
    scratch="$(mktemp -d "${TMPDIR:-/tmp}/ra8-macos-sdk-selftest.XXXXXXXX")"
    # shellcheck disable=SC2064 -- expand the path now, while it is still set.
    trap "rm -rf '${scratch}'" RETURN
    _RA8_MACOS_SDK_CALLLOG="${scratch}/xcrun-calls"

    check() {
      if [ "$1" = "yes" ]; then
        printf 'ok   %s\n' "$2"
      else
        printf 'FAIL %s\n' "$2"
        fails=$((fails + 1))
      fi
    }
    eq() { if [ "$1" = "$2" ]; then printf 'yes\n'; else printf 'no\n'; fi; }
    has() { case "$2" in *"$1"*) printf 'yes\n' ;; *) printf 'no\n' ;; esac; }
    # `yn` answers "did this succeed", `nn` answers "did this fail". Two
    # helpers rather than a leading `!` argument, which is not a command.
    yn() { if "$@" >/dev/null 2>&1; then printf 'yes\n'; else printf 'no\n'; fi; }
    nn() { if "$@" >/dev/null 2>&1; then printf 'no\n'; else printf 'yes\n'; fi; }

    # A real SDK: one healthy tree, built on disk.
    sdk="${scratch}/MacOSX.sdk"
    mkdir -p "${sdk}/usr/lib"
    : >"${sdk}/usr/lib/libSystem.tbd"

    # Linux: inert, and the probe is not even attempted.
    _ra8_macos_sdk_stub_seams Linux /usr/bin/xcrun "${sdk}" "" 0
    check "$(eq "$(ra8_macos_sdk_state)" not_macos)" "a Linux host reports not_macos"
    check "$(eq "$(cat "${_RA8_MACOS_SDK_CALLLOG}")" "")" "a Linux host runs no xcrun"
    check "$(nn ra8_macos_sdk_usable "$(ra8_macos_sdk_state)")" "not_macos is not usable"

    # The healthy Mac.
    _ra8_macos_sdk_stub_seams Darwin /usr/bin/xcrun "${sdk}" "" 0
    got="$(ra8_macos_sdk_state)"
    check "$(eq "${got}" ok)" "a readable SDK with a stub reports ok"
    check "$(yn ra8_macos_sdk_usable ok)" "ok is the usable state"
    check "$(eq "$(cat "${_RA8_MACOS_SDK_CALLLOG}")" /usr/bin/xcrun)" \
      "the probe really runs xcrun, once, at the resolved path"
    ra8_macos_sdk_state >/dev/null
    check "$(eq "${RA8_MACOS_SDK_PATH}" "${sdk}")" "the SDK path is published to the caller"
    check "$(eq "${RA8_MACOS_SDK_STUB}" "${sdk}/usr/lib/libSystem.tbd")" \
      "the stub path is published to the caller"
    check "$(yn ra8_macos_sdk_require)" "require passes on a healthy Mac"

    # THE DEFECT THIS SLICE EXISTS FOR: xcrun present, no developer directory.
    # `command -v xcrun` succeeds here, which is why the old precondition let
    # the gate through.
    _ra8_macos_sdk_stub_seams Darwin /usr/bin/xcrun "" \
      "xcrun: error: invalid active developer path (/Library/Developer/CommandLineTools), missing xcrun at: /Library/Developer/CommandLineTools/usr/bin/xcrun" 1
    got="$(ra8_macos_sdk_state)"
    check "$(eq "${got}" developer_dir_invalid)" "a missing developer directory is named, not passed over"
    check "$(nn ra8_macos_sdk_usable "${got}")" "developer_dir_invalid is not usable"
    check "$(nn ra8_macos_sdk_require)" "require REFUSES when xcrun exists but no SDK does"
    got="$(ra8_macos_sdk_state_advice developer_dir_invalid)"
    check "$(has 'xcode-select --install' "${got}")" "the advice names xcode-select --install"
    got="$(ra8_macos_sdk_require 2>&1 >/dev/null)"
    check "$(has '#899' "${got}")" "the refusal says why a green run would be vacuous"
    got="$(ra8_macos_sdk_state; ra8_macos_sdk_report developer_dir_invalid)"
    check "$(has 'invalid active developer path' "${got}")" "the report quotes what xcrun said"

    # An unaccepted licence is its own morning.
    _ra8_macos_sdk_stub_seams Darwin /usr/bin/xcrun "" \
      "You have not agreed to the Xcode license agreements, please run 'sudo xcodebuild -license'." 1
    check "$(eq "$(ra8_macos_sdk_state)" license_unaccepted)" "an unaccepted licence is its own state"
    check "$(has 'xcodebuild -license' "$(ra8_macos_sdk_state_advice license_unaccepted)")" \
      "the licence advice names xcodebuild -license"

    # An unrecognised failure is reported as unrecognised, not guessed at.
    _ra8_macos_sdk_stub_seams Darwin /usr/bin/xcrun "" "xcrun: error: something new" 1
    check "$(eq "$(ra8_macos_sdk_state)" xcrun_failed)" "an unfamiliar xcrun error is not misclassified"

    # No xcrun anywhere.
    _ra8_macos_sdk_stub_seams Darwin absent "" "" 0
    check "$(eq "$(ra8_macos_sdk_state)" xcrun_absent)" "an absent xcrun is named"
    check "$(eq "$(cat "${_RA8_MACOS_SDK_CALLLOG}")" "")" "an absent xcrun is not executed"

    # Success with nothing on stdout.
    _ra8_macos_sdk_stub_seams Darwin /usr/bin/xcrun "" "" 0
    check "$(eq "$(ra8_macos_sdk_state)" sdk_path_empty)" "an empty SDK path is not read as ok"

    # A path that is not there. Real filesystem, not a stub.
    _ra8_macos_sdk_stub_seams Darwin /usr/bin/xcrun "${scratch}/gone.sdk" "" 0
    check "$(eq "$(ra8_macos_sdk_state)" sdk_path_missing)" "a named SDK that is not on disk is caught"

    # An SDK directory with no libSystem stub: the graph would read no tbd and
    # pin the bundled stub, so the gate must not call this a measurement.
    mkdir -p "${scratch}/Stubless.sdk/usr/lib"
    _ra8_macos_sdk_stub_seams Darwin /usr/bin/xcrun "${scratch}/Stubless.sdk" "" 0
    got="$(ra8_macos_sdk_state)"
    check "$(eq "${got}" stub_missing)" "an SDK with no libSystem.tbd is caught"
    check "$(nn ra8_macos_sdk_require)" "require refuses an SDK with no stub"

    # Every state has its own reason and its own advice: no state falls through
    # to the catch-all, and no two share a reason line.
    local state reasons=""
    for state in not_macos xcrun_absent developer_dir_invalid license_unaccepted \
      xcrun_failed sdk_path_empty sdk_path_missing stub_missing ok; do
      got="$(ra8_macos_sdk_state_reason "${state}")"
      check "$(nn test "${got}" = "unknown SDK state")" "${state} has its own reason"
      check "$(eq "$(has "${got}" "${reasons}")" no)" "${state}'s reason is not a repeat"
      reasons="${reasons}${got}
"
      check "$(nn test -z "$(ra8_macos_sdk_state_advice "${state}")")" "${state} has advice"
    done
    check "$(eq "$(ra8_macos_sdk_state_reason made-up)" "unknown SDK state")" \
      "an unknown state is reported as unknown"

    # The gate really consumes this, rather than the old presence check: the
    # body must call ra8_macos_sdk_require and must no longer require_cmd xcrun.
    local gate_body
    gate_body="$(awk '/^gate_macos_host_build\(\)/,/^\)$/' scripts/ci/gates/manual.sh 2>/dev/null)"
    check "$(nn test -z "${gate_body}")" "the macos-host-build gate body was located"
    check "$(has 'ra8_macos_sdk_require' "${gate_body}")" "the gate calls ra8_macos_sdk_require"
    check "$(has 'macos_sdk.sh' "${gate_body}")" "the gate sources this library"
    check "$(eq "$(has 'require_cmd xcrun' "${gate_body}")" no)" \
      "the gate no longer leans on xcrun being merely present"

    # Live, through the real seams on whatever host this is.
    unset -f _ra8_macos_sdk_uname _ra8_macos_sdk_xcrun_bin _ra8_macos_sdk_show_sdk_path
    _RA8_MACOS_SDK_SH=""
    # shellcheck source=scripts/ci/lib/macos_sdk.sh
    . "${BASH_SOURCE[0]}"
    got="$(ra8_macos_sdk_state)"
    check "$(nn test -z "${got}")" "the live probe names a state: ${got}"
    if [ "$(_ra8_macos_sdk_uname -s)" != "Darwin" ]; then
      check "$(eq "${got}" not_macos)" "this non-Darwin host reports not_macos"
      check "$(nn ra8_macos_sdk_require)" "require refuses on this non-Darwin host"
    fi

    if [ "${fails}" -ne 0 ]; then
      printf 'macos_sdk.sh --selftest: %s case(s) FAILED\n' "${fails}" >&2
      return 1
    fi
    printf 'macos_sdk.sh --selftest: PASS (healthy, no developer dir, licence, absent, empty, missing, stubless, live)\n'
  }
fi

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  case "${1:-}" in
    --selftest) ra8_macos_sdk_selftest ;;
    --report) ra8_macos_sdk_report ;;
    *)
      echo "Usage: scripts/ci/lib/macos_sdk.sh --selftest | --report" >&2
      exit 2
      ;;
  esac
fi
