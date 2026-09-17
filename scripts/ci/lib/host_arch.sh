#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/ci/lib/host_arch.sh -- what hardware is this, really.
#
# `uname -m` answers "what architecture is this PROCESS", not "what
# architecture is this MACHINE". On Apple silicon the two differ whenever the
# shell was launched from a translated parent: `arch -x86_64 zsh`, a terminal
# or IDE shipped as an x86_64 binary, or an x86_64 Homebrew whose `just` and
# `bash` are themselves x86_64. Rosetta 2 translates the whole process tree, so
# every `uname -m` inside it prints x86_64 on a machine that is arm64.
#
# Why this matters here (#899): gate_macos_host_build refuses anything that is
# not Darwin/arm64, because a translated toolchain links the x86_64 path and
# cannot observe the missing arm64-macos slice in the SDK's libSystem stub at
# all. Refusing is right. Refusing with "this host is Darwin/x86_64 -- run it
# on an arm64 macOS runner" is wrong, and wrong on the exact machine the gate
# needs: the owner is told to go and find hardware they are already sitting at.
#
# Darwin publishes both facts, so the diagnosis does not have to guess:
#
#   sysctl -n sysctl.proc_translated   1 when THIS process is translated
#   sysctl -n hw.optional.arm64        1 on Apple silicon, translated or not
#
# proc_translated is the primary signal; hw.optional.arm64 is the fallback for
# a host that does not answer the first (and is what distinguishes an Intel Mac,
# which answers neither, from a translated arm64 one). Nothing here changes a
# verdict on Linux, and on a native arm64 Mac no sysctl is consulted at all.
#
# Normally sourced. Direct execution is reserved for ``--selftest``, which the
# toolchain-parity gate runs; the whole guarded block is idempotent so any
# number of scripts can source it.

if [ -z "${_RA8_HOST_ARCH_SH:-}" ]; then
  _RA8_HOST_ARCH_SH=1

  K_RA8_HOST_ARCH_TRANSLATED_KEY="sysctl.proc_translated"
  K_RA8_HOST_ARCH_APPLE_SILICON_KEY="hw.optional.arm64"

  # _ra8_host_arch_uname -- platform probe through fixed absolute paths, so a
  # hostile or merely odd PATH cannot redirect it. Same shape as
  # host_tool_path.sh's probe, and the seam the selftest replaces.
  _ra8_host_arch_uname() {
    if [ -x /usr/bin/uname ]; then
      /usr/bin/uname "$@"
    elif [ -x /bin/uname ]; then
      /bin/uname "$@"
    else
      echo "host_arch.sh: no fixed-path uname found" >&2
      return 1
    fi
  }

  # _ra8_host_arch_sysctl <key> -- one Darwin sysctl value, or non-zero when
  # the key does not exist on this host (an Intel Mac has no hw.optional.arm64)
  # or there is no sysctl to ask. Absence is an answer, not an error, so
  # nothing is printed on failure.
  _ra8_host_arch_sysctl() {
    local key="$1" bin
    for bin in /usr/sbin/sysctl /sbin/sysctl; do
      if [ -x "${bin}" ]; then
        "${bin}" -n "${key}" 2>/dev/null
        return $?
      fi
    done
    return 1
  }

  _ra8_host_arch_sysctl_is_one() {
    local value
    value="$(_ra8_host_arch_sysctl "$1")" || return 1
    [ "${value}" = "1" ]
  }

  # ra8_host_translated [os] [machine] -- success when this process is running
  # under Rosetta 2 on Apple silicon. os/machine default to uname; explicit
  # values exist so the selftest can prove every matrix row.
  ra8_host_translated() {
    local os_name="${1:-}" machine="${2:-}"
    [ -n "${os_name}" ] || os_name="$(_ra8_host_arch_uname -s)" || return 1
    [ -n "${machine}" ] || machine="$(_ra8_host_arch_uname -m)" || return 1
    [ "${os_name}" = "Darwin" ] || return 1
    [ "${machine}" != "arm64" ] || return 1
    _ra8_host_arch_sysctl_is_one "${K_RA8_HOST_ARCH_TRANSLATED_KEY}" && return 0
    _ra8_host_arch_sysctl_is_one "${K_RA8_HOST_ARCH_APPLE_SILICON_KEY}"
  }

  # ra8_host_hardware_arch [os] [machine] -- the MACHINE's architecture, which
  # is `uname -m` everywhere except a translated process on Apple silicon.
  # Fails closed to what uname said: an unreadable sysctl never promotes a host
  # to arm64.
  ra8_host_hardware_arch() {
    local os_name="${1:-}" machine="${2:-}"
    [ -n "${os_name}" ] || os_name="$(_ra8_host_arch_uname -s)" || return 1
    [ -n "${machine}" ] || machine="$(_ra8_host_arch_uname -m)" || return 1
    if ra8_host_translated "${os_name}" "${machine}"; then
      printf 'arm64\n'
    else
      printf '%s\n' "${machine}"
    fi
  }

  # ra8_host_arch_summary [os] [machine] -- one line naming both facts, so a
  # refusal reads as a diagnosis instead of a contradiction of what the owner
  # knows about their own Mac.
  ra8_host_arch_summary() {
    local os_name="${1:-}" machine="${2:-}" hardware
    [ -n "${os_name}" ] || os_name="$(_ra8_host_arch_uname -s)" || return 1
    [ -n "${machine}" ] || machine="$(_ra8_host_arch_uname -m)" || return 1
    hardware="$(ra8_host_hardware_arch "${os_name}" "${machine}")" || return 1
    if [ "${hardware}" = "${machine}" ]; then
      printf '%s/%s\n' "${os_name}" "${machine}"
    else
      printf '%s/%s hardware, %s process (Rosetta 2 translation)\n' \
        "${os_name}" "${hardware}" "${machine}"
    fi
  }

  # ra8_host_translation_advice -- how to get back to a native shell. Printed
  # by the caller that refused, on the stream it refused to.
  ra8_host_translation_advice() {
    printf 'Rosetta 2 translates the whole process tree, so a native arm64 toolchain\n'
    printf 'cannot be reached from here. Re-run from a native shell, e.g.\n'
    printf '    arch -arm64 /bin/zsh -lc "just quality::local::gate macos-host-build"\n'
    printf 'or open a terminal that is not itself an x86_64 binary (check with\n'
    printf '`arch` and `sysctl -n sysctl.proc_translated`).\n'
  }

  # --- selftest -------------------------------------------------------------

  # The seams are replaced wholesale for the matrix below. Every sysctl key the
  # code asks for is appended to the file named by _RA8_HOST_ARCH_KEYLOG, so a
  # case can assert that a native arm64 Mac is decided WITHOUT a sysctl call,
  # and that the translation probe really asks. A file rather than a variable
  # because each probe runs inside a command substitution, and a subshell
  # cannot write a variable back to its parent.
  _ra8_host_arch_stub_seams() {
    _RA8_HOST_ARCH_STUB_OS="$1"
    _RA8_HOST_ARCH_STUB_MACHINE="$2"
    _RA8_HOST_ARCH_STUB_TRANSLATED="$3"
    _RA8_HOST_ARCH_STUB_SILICON="$4"
    : >"${_RA8_HOST_ARCH_KEYLOG}"
    _ra8_host_arch_uname() {
      case "$1" in
        -s) printf '%s\n' "${_RA8_HOST_ARCH_STUB_OS}" ;;
        -m) printf '%s\n' "${_RA8_HOST_ARCH_STUB_MACHINE}" ;;
        *) return 1 ;;
      esac
    }
    _ra8_host_arch_sysctl() {
      local key="$1"
      printf '%s\n' "${key}" >>"${_RA8_HOST_ARCH_KEYLOG}"
      case "${key}" in
        "${K_RA8_HOST_ARCH_TRANSLATED_KEY}")
          [ "${_RA8_HOST_ARCH_STUB_TRANSLATED}" = "absent" ] && return 1
          printf '%s\n' "${_RA8_HOST_ARCH_STUB_TRANSLATED}"
          ;;
        "${K_RA8_HOST_ARCH_APPLE_SILICON_KEY}")
          [ "${_RA8_HOST_ARCH_STUB_SILICON}" = "absent" ] && return 1
          printf '%s\n' "${_RA8_HOST_ARCH_STUB_SILICON}"
          ;;
        *) return 1 ;;
      esac
    }
  }

  ra8_host_arch_selftest() {
    local fails=0 keys got scratch
    local _RA8_HOST_ARCH_KEYLOG
    scratch="$(mktemp -d "${TMPDIR:-/tmp}/ra8-host-arch.XXXXXXXX")"
    # shellcheck disable=SC2064 -- expand the path now, while it is still set.
    trap "rm -rf '${scratch}'" RETURN
    _RA8_HOST_ARCH_KEYLOG="${scratch}/sysctl-keys"

    check() {
      if [ "$1" = "yes" ]; then
        printf 'ok   %s\n' "$2"
      else
        printf 'FAIL %s\n' "$2"
        fails=$((fails + 1))
      fi
    }
    eq() { if [ "$1" = "$2" ]; then printf 'yes\n'; else printf 'no\n'; fi; }
    # `yn` answers "did this succeed", `nn` answers "did this fail". Two
    # helpers rather than a leading `!` argument, which is not a command.
    yn() { if "$@" >/dev/null 2>&1; then printf 'yes\n'; else printf 'no\n'; fi; }
    nn() { if "$@" >/dev/null 2>&1; then printf 'no\n'; else printf 'yes\n'; fi; }

    # Linux: untouched, and no sysctl is consulted.
    _ra8_host_arch_stub_seams Linux x86_64 absent absent
    check "$(eq "$(ra8_host_hardware_arch)" x86_64)" "Linux x86_64 reports x86_64"
    check "$(nn ra8_host_translated)" "Linux is never translated"
    check "$(eq "$(cat "${_RA8_HOST_ARCH_KEYLOG}")" "")" "Linux consults no sysctl"
    _ra8_host_arch_stub_seams Linux aarch64 absent absent
    check "$(eq "$(ra8_host_hardware_arch)" aarch64)" "Linux aarch64 reports aarch64"
    check "$(eq "$(ra8_host_arch_summary)" Linux/aarch64)" "Linux summary is os/arch"

    # A native arm64 Mac: decided from uname alone.
    _ra8_host_arch_stub_seams Darwin arm64 0 1
    check "$(eq "$(ra8_host_hardware_arch)" arm64)" "native arm64 Mac reports arm64"
    check "$(nn ra8_host_translated)" "native arm64 Mac is not translated"
    check "$(eq "$(cat "${_RA8_HOST_ARCH_KEYLOG}")" "")" "native arm64 Mac consults no sysctl"
    check "$(eq "$(ra8_host_arch_summary)" Darwin/arm64)" "native arm64 summary is Darwin/arm64"

    # The case this slice exists for: an arm64 Mac seen through Rosetta.
    _ra8_host_arch_stub_seams Darwin x86_64 1 1
    check "$(yn ra8_host_translated)" "a translated shell on Apple silicon is detected"
    check "$(eq "$(ra8_host_hardware_arch)" arm64)" "translated shell still reports arm64 hardware"
    got="$(ra8_host_arch_summary)"
    check "$(case "${got}" in *Rosetta*) echo yes ;; *) echo no ;; esac)" \
      "translated summary names Rosetta: ${got}"
    check "$(case "${got}" in *Darwin/arm64*) echo yes ;; *) echo no ;; esac)" \
      "translated summary names the real hardware"
    keys="$(cat "${_RA8_HOST_ARCH_KEYLOG}")"
    check "$(case "${keys}" in *sysctl.proc_translated*) echo yes ;; *) echo no ;; esac)" \
      "the translation probe asks sysctl.proc_translated"

    # A genuine Intel Mac answers neither key: refuse, and say x86_64.
    _ra8_host_arch_stub_seams Darwin x86_64 0 absent
    check "$(nn ra8_host_translated)" "an Intel Mac is not translated"
    check "$(eq "$(ra8_host_hardware_arch)" x86_64)" "an Intel Mac reports x86_64"
    check "$(eq "$(ra8_host_arch_summary)" Darwin/x86_64)" "an Intel Mac summary is Darwin/x86_64"

    # proc_translated unreadable, hw.optional.arm64 says Apple silicon: the
    # fallback carries it.
    _ra8_host_arch_stub_seams Darwin x86_64 absent 1
    check "$(yn ra8_host_translated)" "hw.optional.arm64 is the fallback signal"
    check "$(eq "$(ra8_host_hardware_arch)" arm64)" "fallback still reports arm64 hardware"

    # No sysctl at all: fail closed to uname, never promote to arm64.
    _ra8_host_arch_stub_seams Darwin x86_64 absent absent
    check "$(nn ra8_host_translated)" "no sysctl answers means not translated"
    check "$(eq "$(ra8_host_hardware_arch)" x86_64)" "no sysctl answers keeps uname's arch"

    # Explicit arguments win over the seams, which is how the gate passes the
    # values it already read.
    _ra8_host_arch_stub_seams Darwin arm64 0 1
    check "$(eq "$(ra8_host_hardware_arch Linux x86_64)" x86_64)" \
      "explicit os/arch arguments are used as given"

    # The advice is actionable, not a shrug.
    got="$(ra8_host_translation_advice)"
    check "$(case "${got}" in *'arch -arm64'*) echo yes ;; *) echo no ;; esac)" \
      "the advice names arch -arm64"
    check "$(case "${got}" in *'quality::local::gate macos-host-build'*) echo yes ;; *) echo no ;; esac)" \
      "the advice names the gate to re-run"

    # Live, through the real seams on whatever host this is: the summary agrees
    # with uname, and a non-Darwin host is never translated.
    unset -f _ra8_host_arch_uname _ra8_host_arch_sysctl
    _RA8_HOST_ARCH_SH=""
    # shellcheck source=scripts/ci/lib/host_arch.sh
    . "${BASH_SOURCE[0]}"
    got="$(_ra8_host_arch_uname -m)"
    check "$(eq "$(ra8_host_hardware_arch)" "$(ra8_host_hardware_arch "$(_ra8_host_arch_uname -s)" "${got}")")" \
      "the live probe agrees with itself"
    if [ "$(_ra8_host_arch_uname -s)" != "Darwin" ]; then
      check "$(nn ra8_host_translated)" "this non-Darwin host reports no translation"
      check "$(eq "$(ra8_host_hardware_arch)" "${got}")" "this host's arch is uname's arch"
    fi

    if [ "${fails}" -ne 0 ]; then
      printf 'host_arch.sh --selftest: %s case(s) FAILED\n' "${fails}" >&2
      return 1
    fi
    printf 'host_arch.sh --selftest: PASS (Linux, native arm64, Rosetta, Intel, fallback, live)\n'
  }
fi

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  case "${1:-}" in
    --selftest) ra8_host_arch_selftest ;;
    *)
      echo "Usage: scripts/ci/lib/host_arch.sh --selftest" >&2
      exit 2
      ;;
  esac
fi
