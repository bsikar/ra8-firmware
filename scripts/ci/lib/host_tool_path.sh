#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/ci/lib/host_tool_path.sh -- deterministic trusted host-tool PATH.
#
# Sourced by boundary scripts (scripts/hil/dev.sh) and executed directly for
# `export PATH := ...` evaluation (just/hil.just) and `--selftest`. The whole
# guarded block is idempotent so any number of scripts can source it.
#
# Why this exists: the HIL boundary must not inherit the caller's PATH, so it
# builds PATH from a closed list of known locations instead. A hand-written
# literal drifted: it listed the Intel macOS / Linux prefixes but not the
# Apple Silicon Homebrew prefix, so `just hil::run` failed on Apple Silicon
# with `cmake: command not found` while the same cmake resolved fine in a
# login shell. One helper owns the list, the platform order, and the
# required-tool check, so the just module and the HIL scripts cannot drift
# apart again.
#
# Policy, highest precedence first:
#   1. <repo>/.venv/bin, always first (may not exist yet; kept for
#      determinism so every platform shares one shape);
#   2. Darwin arm64: /opt/homebrew/bin, /opt/homebrew/sbin, then
#      /usr/local/bin, /usr/local/sbin;
#   3. Darwin x86_64: /usr/local/bin, /usr/local/sbin, then the Homebrew pair;
#   4. Linux: /usr/local/bin, /usr/local/sbin (no Homebrew prefix);
#   5. every platform ends with /usr/bin, /usr/sbin, /bin, /sbin.
#
# Never reads $PATH, never consults `brew --prefix`, shell startup files, or
# shellenv. `uname` itself is resolved through fixed absolute paths so a
# hostile PATH cannot redirect platform detection.

if [ -z "${_RA8_HOST_TOOL_PATH_SH:-}" ]; then
  _RA8_HOST_TOOL_PATH_SH=1

  # _ra8_host_tool_uname -- platform probe without trusting PATH.
  _ra8_host_tool_uname() {
    if [ -x /usr/bin/uname ]; then
      /usr/bin/uname "$@"
    elif [ -x /bin/uname ]; then
      /bin/uname "$@"
    else
      echo "host_tool_path.sh: no fixed-path uname found" >&2
      return 1
    fi
  }

  # _ra8_host_tool_repo_root -- repository root derived from this file's own
  # path, using shell builtins only. The previous `dirname` call resolved
  # through the caller's PATH: without it the derivation silently degraded to
  # a `//.venv/bin` entry instead of failing closed. A bare filename (no
  # directory component, e.g. resolved through a PATH lookup) cannot anchor a
  # repository root, so it fails closed rather than deriving from the CWD.
  _ra8_host_tool_repo_root() {
    local src_dir="${BASH_SOURCE[0]%/*}"
    if [ -z "$src_dir" ] || [ "$src_dir" = "${BASH_SOURCE[0]}" ]; then
      echo "host_tool_path.sh: cannot derive repository root from '${BASH_SOURCE[0]}'" >&2
      return 1
    fi
    cd "$src_dir/../../.." && pwd -P
  }

  # ra8_trusted_host_path [repo_root] [os] [arch] -- print the deterministic
  # trusted host-tool PATH. os/arch default to `uname -s`/`uname -m`; explicit
  # values exist only so the selftest can prove every platform matrix row.
  ra8_trusted_host_path() {
    local repo_root os_name arch_name venv_bin
    repo_root="${1:-}"
    os_name="${2:-}"
    arch_name="${3:-}"
    if [ -z "${repo_root}" ]; then
      repo_root="$(_ra8_host_tool_repo_root)" || return 1
    fi
    if [ -z "${os_name}" ]; then
      os_name="$(_ra8_host_tool_uname -s)" || return 1
    fi
    if [ -z "${arch_name}" ]; then
      arch_name="$(_ra8_host_tool_uname -m)" || return 1
    fi
    venv_bin="${repo_root}/.venv/bin"
    case "${os_name}" in
      Darwin)
        case "${arch_name}" in
          arm64)
            printf '%s\n' "${venv_bin}:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/bin:/usr/sbin:/bin:/sbin"
            ;;
          x86_64)
            printf '%s\n' "${venv_bin}:/usr/local/bin:/usr/local/sbin:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/bin:/usr/sbin:/bin:/sbin"
            ;;
          *)
            echo "host_tool_path.sh: unsupported Darwin architecture '${arch_name}'" >&2
            return 1
            ;;
        esac
        ;;
      Linux)
        printf '%s\n' "${venv_bin}:/usr/local/bin:/usr/local/sbin:/usr/bin:/usr/sbin:/bin:/sbin"
        ;;
      *)
        echo "host_tool_path.sh: unsupported operating system '${os_name}'" >&2
        return 1
        ;;
    esac
  }

  # ra8_use_trusted_host_path [repo_root] [os] [arch] -- install the trusted
  # PATH into the current shell, discarding the inherited caller PATH.
  ra8_use_trusted_host_path() {
    local trusted
    trusted="$(ra8_trusted_host_path "$@")" || return 1
    PATH="${trusted}"
    export PATH
  }

  # ra8_require_host_tools tool... -- fail closed with an actionable message
  # when a tool the local build path needs is missing from the trusted PATH.
  ra8_require_host_tools() {
    local tool hint failure
    failure=0
    if [ "$#" -eq 0 ]; then
      echo "host_tool_path.sh: ra8_require_host_tools needs at least one tool name" >&2
      return 2
    fi
    for tool in "$@"; do
      case "${tool}" in
        '' | */*)
          echo "error: invalid required host tool name '${tool}'" >&2
          return 2
          ;;
      esac
      if command -v "${tool}" >/dev/null 2>&1; then
        continue
      fi
      case "${tool}" in
        cmake)
          hint="install CMake under a trusted prefix (/opt/homebrew/bin on Apple Silicon macOS, /usr/local/bin on Intel macOS or Linux)"
          ;;
        python3)
          hint="run 'just setup-python' to create the repository .venv"
          ;;
        *)
          hint="install '${tool}' under a trusted prefix"
          ;;
      esac
      echo "error: required host tool '${tool}' was not found in the trusted tool PATH; ${hint}" >&2
      failure=1
    done
    return "${failure}"
  }

  # _ra8_host_tool_path_assert_contains -- one readable selftest assertion.
  _ra8_host_tool_path_assert_contains() {
    local path_value="$1" wanted="$2" label="$3"
    case ":${path_value}:" in
      *":${wanted}:"*) return 0 ;;
    esac
    echo "host_tool_path.sh --selftest: ${label}: missing '${wanted}' in '${path_value}'" >&2
    return 1
  }

  _ra8_host_tool_path_platform_selftest() {
    local fake_root="$1" got first
    got="$(ra8_trusted_host_path "${fake_root}" Darwin arm64)"
    first="${got%%:*}"
    [ "${first}" = "${fake_root}/.venv/bin" ] || {
      echo "host_tool_path.sh --selftest: Darwin arm64 prefers '${first}', not .venv/bin" >&2
      return 1
    }
    _ra8_host_tool_path_assert_contains "${got}" /opt/homebrew/bin "Darwin arm64"

    got="$(ra8_trusted_host_path "${fake_root}" Darwin x86_64)"
    first="${got%%:*}"
    [ "${first}" = "${fake_root}/.venv/bin" ] || {
      echo "host_tool_path.sh --selftest: Darwin x86_64 prefers '${first}', not .venv/bin" >&2
      return 1
    }
    _ra8_host_tool_path_assert_contains "${got}" /usr/local/bin "Darwin x86_64"

    got="$(ra8_trusted_host_path "${fake_root}" Linux x86_64)"
    _ra8_host_tool_path_assert_contains "${got}" /usr/local/bin "Linux"
    _ra8_host_tool_path_assert_contains "${got}" /usr/bin "Linux"
    case ":${got}:" in
      *":/opt/homebrew/bin:"*)
        echo "host_tool_path.sh --selftest: Linux must not carry a Homebrew prefix" >&2
        return 1
        ;;
    esac

    PATH="/evil/caller/bin:/usr/bin" got="$(ra8_trusted_host_path "${fake_root}" Darwin arm64)"
    case ":${got}:" in
      *":/evil/caller/bin:"*)
        echo "host_tool_path.sh --selftest: caller PATH leaked into '${got}'" >&2
        return 1
        ;;
    esac
  }

  _ra8_host_tool_path_bootstrap_selftest() {
    local scratch="$1" fake_root="$2" got _helper_abs
    # Bootstrap independence: --print-path must resolve fully with no
    # PATH-resolved tooling at all. A previous revision shelled out to
    # `dirname` here and silently degraded to a `//.venv/bin` entry when the
    # caller's PATH lacked it; now only builtins and fixed paths remain.
    /bin/mkdir -p "${scratch}/empty-path"
    case "${BASH_SOURCE[0]}" in
      /*) _helper_abs="${BASH_SOURCE[0]}" ;;
      *) _helper_abs="$PWD/${BASH_SOURCE[0]}" ;;
    esac
    if ! got="$(PATH="${scratch}/empty-path" /bin/bash -p "$_helper_abs" --print-path 2>"${scratch}/bootstrap.err")"; then
      cat "${scratch}/bootstrap.err" >&2
      echo "host_tool_path.sh --selftest: --print-path needs PATH-resolved tools" >&2
      return 1
    fi
    case "${got}" in
      */.venv/bin:*)
        case "${got}" in
          //*)
            echo "host_tool_path.sh --selftest: malformed root in '${got}'" >&2
            return 1
            ;;
        esac
        ;;
      *)
        echo "host_tool_path.sh --selftest: no .venv/bin first in '${got}'" >&2
        return 1
        ;;
    esac
    case ":${got}:" in
      *":${scratch}/empty-path:"* | "${scratch}/empty-path:"*)
        echo "host_tool_path.sh --selftest: bootstrap PATH leaked into '${got}'" >&2
        return 1
        ;;
    esac
    if got="$(ra8_trusted_host_path "${fake_root}" FreeBSD arm64 2>&1)"; then
      echo "host_tool_path.sh --selftest: unsupported OS did not fail closed" >&2
      return 1
    fi
    case "${got}" in
      *"unsupported operating system"*) ;;
      *)
        echo "host_tool_path.sh --selftest: unsupported OS gave no useful error" >&2
        return 1
        ;;
    esac
  }

  _ra8_host_tool_path_require_selftest() {
    local scratch="$1" fake_root="$2" failure
    printf '#!/bin/sh\nexit 0\n' >"${scratch}/fake-bin/cmake"
    /bin/chmod +x "${scratch}/fake-bin/cmake"
    PATH="${scratch}/fake-bin:$(ra8_trusted_host_path "${fake_root}" Linux x86_64)" \
      ra8_require_host_tools cmake
    if failure="$(PATH="${scratch}/empty" ra8_require_host_tools cmake 2>&1)"; then
      echo "host_tool_path.sh --selftest: missing cmake did not fail" >&2
      return 1
    fi
    case "${failure}" in
      *"error: required host tool 'cmake' was not found in the trusted tool PATH"*) ;;
      *)
        echo "host_tool_path.sh --selftest: missing cmake gave no useful error: ${failure}" >&2
        return 1
        ;;
    esac
  }

  host_tool_path_selftest() {
    set -euo pipefail
    local scratch fake_root
    scratch="$(/usr/bin/mktemp -d "${TMPDIR:-/tmp}/ra8-host-tool-path.XXXXXXXX")"
    trap '/bin/rm -rf "${scratch}"' RETURN
    fake_root="${scratch}/repo"
    /bin/mkdir -p "${fake_root}/.venv/bin" "${scratch}/fake-bin" "${scratch}/empty"

    _ra8_host_tool_path_platform_selftest "${fake_root}"
    _ra8_host_tool_path_bootstrap_selftest "${scratch}" "${fake_root}"
    _ra8_host_tool_path_require_selftest "${scratch}" "${fake_root}"

    echo "host_tool_path.sh --selftest: PASS (platform matrix, venv precedence, isolation, require both ways)"
  }
fi

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  case "${1:-}" in
    --print-path) ra8_trusted_host_path "${2:-}" "${3:-}" "${4:-}" ;;
    --require)
      shift
      ra8_require_host_tools "$@"
      ;;
    --selftest) host_tool_path_selftest ;;
    *)
      echo "Usage: scripts/ci/lib/host_tool_path.sh {--print-path|--require|--selftest}" >&2
      exit 2
      ;;
  esac
fi
