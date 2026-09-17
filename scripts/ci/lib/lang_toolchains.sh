#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/ci/lib/lang_toolchains.sh -- resolve the pinned Zig and Rust
# toolchains for every gate that needs them.
#
# SOURCED, NEVER EXECUTED. scripts/ci.sh calls use_pinned_lang_toolchains at
# the top of run_one_gate, right after use_pinned_tool_path, so every gate
# resolves the SAME zig and cargo regardless of how the shell was entered.
#
# Why: the Zig migration made zig -- and, through the Rust ABI fixtures, cargo,
# rustc, rustfmt and clippy-driver -- hard dependencies of ordinary gates.
# unit-tests, ubsan, scan-build, mcdc, coverage, format and the whole
# pre-commit suite all configure a tree whose cmake/zig_abi_contract.cmake
# requires zig on PATH. .devcontainer/Dockerfile carries both pins, but the
# deployed ARC runner image was built before those layers existed, so on that
# image every one of those gates died with a provisioning error instead of a
# verdict. Rebuilding the image is the durable fix and is an Ansible converge
# (infra/images/README.md); this makes the gates work on the image the fleet is
# actually running, the same way scripts/builders/provision_doxygen.sh makes
# the docs gate work without a system doxygen.
#
# The pins are READ FROM .devcontainer/Dockerfile rather than copied here, so
# that file stays the single source of truth the toolchain-parity gate checks
# against and this provisioner cannot drift from the image it stands in for.
#
# Resolution order per toolchain, cheapest first:
#   1. a PATH binary already reporting the pinned version (the rebuilt image);
#   2. a previously provisioned copy under the pinned-tool cache;
#   3. download of the official release, sha256-verified against the pin.
#
# Step 3 is LINUX ONLY, and deliberately so. The pins live in
# .devcontainer/Dockerfile, which describes a Linux image, so the only release
# archives this file can name AND sha256-verify are the Linux ones. On a macOS
# host (the arm64 runner #899 adds, and any developer Mac) the host binary must
# already be the pin; the provisioner says that in one line and stops. It used
# to fall through to the Linux URL, download ~50 MB of unrunnable ELF on every
# gate, pass the sha check because the pin genuinely is that archive's, and
# then report "provisioned zig at <path> is not 0.14.1" -- a version
# complaint about a binary whose real problem was its operating system.
#
# Failure is deliberately NON-FATAL: the caller's require_cmd emits the real
# diagnostic. A provisioner that exited the gate would replace a precise
# "required tool 'zig' is not on PATH" with a curl error.
#
# The whole guarded block is idempotent so any number of scripts can source it.
if [ -z "${_RA8_LANG_TOOLCHAINS_SH:-}" ]; then
  _RA8_LANG_TOOLCHAINS_SH=1

  # Repository root, derived the way the sibling libs derive it.
  _ra8_lang_repo_root() {
    if [ -n "${REPO_ROOT:-}" ]; then
      printf '%s\n' "${REPO_ROOT}"
      return 0
    fi
    (cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
  }

  _ra8_lang_log() { echo "lang_toolchains.sh: $*" >&2; }

  # Read one `ARG NAME=value` pin out of .devcontainer/Dockerfile. Empty when
  # the pin is absent, which the callers treat as "cannot provision".
  _ra8_lang_pin() {
    local name="$1" dockerfile
    dockerfile="$(_ra8_lang_repo_root)/.devcontainer/Dockerfile"
    [ -r "${dockerfile}" ] || return 0
    sed -n "s/^ARG ${name}=\([^[:space:]]*\).*/\1/p" "${dockerfile}" | head -n 1
  }

  # Where a provisioned toolchain lands. RA8_TOOLS_CACHE (exported by
  # scripts/ci.sh export_tools_cache) is a host directory that survives the
  # ephemeral suite snapshot; without it the per-build build/tools/ is used and
  # the download repeats, exactly as provision_doxygen.sh degrades.
  _ra8_lang_tools_dir() {
    if [ -n "${RA8_TOOLS_CACHE:-}" ]; then
      printf '%s/%s-%s\n' "${RA8_TOOLS_CACHE}" "$(uname -s)" "$(uname -m)"
    else
      printf '%s/build/tools\n' "$(_ra8_lang_repo_root)"
    fi
  }

  _ra8_lang_sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
      sha256sum "$1" | awk '{print $1}'
    else
      shasum -a 256 "$1" | awk '{print $1}'
    fi
  }

  # Prepend a directory to PATH exactly once, so repeated sourcing or repeated
  # gate dispatch never grows PATH.
  _ra8_lang_prepend_path() {
    local dir="$1"
    [ -n "${dir}" ] && [ -d "${dir}" ] || return 1
    case ":${PATH}:" in
      *":${dir}:"*) ;;
      *) PATH="${dir}:${PATH}" ;;
    esac
    export PATH
    return 0
  }

  # Download + verify + extract one pinned tarball. Extraction happens in a
  # scratch directory and is renamed into place, so a concurrent gate never
  # observes a half-unpacked toolchain.
  _ra8_lang_fetch() {
    local url="$1" want_sha="$2" dest="$3" strip="$4"
    local tools_dir tmp archive actual
    tools_dir="$(dirname "${dest}")"
    mkdir -p "${tools_dir}" 2>/dev/null || return 1
    tmp="$(mktemp -d "${tools_dir}/lang-dl.XXXXXX")" || return 1
    archive="${tmp}/archive.tar.xz"

    if ! curl --proto '=https' --proto-redir '=https' --tlsv1.2 \
      -fL --retry 3 --retry-delay 2 -o "${archive}" "${url}" >&2; then
      _ra8_lang_log "ERROR: download failed: ${url}"
      rm -rf "${tmp}"
      return 1
    fi

    actual="$(_ra8_lang_sha256_of "${archive}")"
    if [ "${actual}" != "${want_sha}" ]; then
      _ra8_lang_log "ERROR: sha256 mismatch for ${url}"
      _ra8_lang_log "  expected ${want_sha}"
      _ra8_lang_log "  actual   ${actual}"
      rm -rf "${tmp}"
      return 1
    fi

    mkdir -p "${tmp}/unpack"
    if ! tar -xf "${archive}" --strip-components="${strip}" -C "${tmp}/unpack"; then
      _ra8_lang_log "ERROR: could not unpack ${url}"
      rm -rf "${tmp}"
      return 1
    fi

    rm -rf "${dest}"
    if ! mv "${tmp}/unpack" "${dest}"; then
      _ra8_lang_log "ERROR: could not install into ${dest}"
      rm -rf "${tmp}"
      return 1
    fi
    rm -rf "${tmp}"
    return 0
  }

  # `uname` behind one seam so the selftest can prove every host row without
  # needing that host, exactly as host_tool_path.sh does.
  _ra8_lang_uname() {
    uname "$@"
  }

  # The release archives name the host the same way for both toolchains.
  _ra8_lang_arch() {
    case "$(_ra8_lang_uname -m)" in
      x86_64) printf 'x86_64\n' ;;
      aarch64 | arm64) printf 'aarch64\n' ;;
      *) return 1 ;;
    esac
  }

  # Whether this host is one the pins can describe. Linux is; everything else
  # is refused by name rather than sent down a Linux download path.
  _ra8_lang_can_provision() {
    [ "$(_ra8_lang_uname -s)" = "Linux" ]
  }

  # One shared refusal, so zig and rust say the same thing the same way.
  _ra8_lang_refuse_foreign_host() {
    local tool="$1" want="$2"
    _ra8_lang_log \
      "cannot provision ${tool} ${want} on $(_ra8_lang_uname -s)/$(_ra8_lang_uname -m):"
    _ra8_lang_log \
      "  .devcontainer/Dockerfile pins the Linux release archives only."
    _ra8_lang_log \
      "  Install ${tool} ${want} on the host and put it on PATH; the caller's"
    _ra8_lang_log \
      "  require_cmd reports the missing tool."
    return 1
  }

  # use_pinned_zig -- put the pinned zig on PATH, provisioning it when the
  # image does not carry it.
  use_pinned_zig() {
    local want arch sha url dest tools_dir
    want="$(_ra8_lang_pin ZIG_VERSION)"
    [ -n "${want}" ] || return 1

    if command -v zig >/dev/null 2>&1 &&
      [ "$(zig version 2>/dev/null)" = "${want}" ]; then
      return 0
    fi

    _ra8_lang_can_provision || _ra8_lang_refuse_foreign_host zig "${want}" || return 1

    arch="$(_ra8_lang_arch)" || return 1
    tools_dir="$(_ra8_lang_tools_dir)"
    dest="${tools_dir}/zig-${want}"

    if [ ! -x "${dest}/zig" ] || [ "$("${dest}/zig" version 2>/dev/null)" != "${want}" ]; then
      case "${arch}" in
        x86_64) sha="$(_ra8_lang_pin ZIG_SHA256_X86_64)" ;;
        aarch64) sha="$(_ra8_lang_pin ZIG_SHA256_AARCH64)" ;;
      esac
      [ -n "${sha}" ] || return 1
      url="https://ziglang.org/download/${want}/zig-${arch}-linux-${want}.tar.xz"
      _ra8_lang_log "provisioning zig ${want} (${arch}); the runner image does not carry it"
      _ra8_lang_fetch "${url}" "${sha}" "${dest}" 1 || return 1
    fi

    if [ "$("${dest}/zig" version 2>/dev/null)" != "${want}" ]; then
      _ra8_lang_log "ERROR: provisioned zig at ${dest} is not ${want}"
      return 1
    fi
    _ra8_lang_prepend_path "${dest}"
  }

  # use_pinned_rust -- the same contract for the Rust toolchain the ABI
  # fixtures build against. The official distribution ships rustc, cargo,
  # rustfmt and clippy as one installer; --disable-ldconfig keeps it entirely
  # inside the cache directory, and rustc's $ORIGIN-relative rpath means no
  # LD_LIBRARY_PATH is needed.
  use_pinned_rust() {
    local want arch sha url unpack dest tools_dir
    want="$(_ra8_lang_pin RUST_VERSION)"
    [ -n "${want}" ] || return 1

    if command -v rustc >/dev/null 2>&1 && command -v cargo >/dev/null 2>&1 &&
      [ "$(rustc --version 2>/dev/null | awk '{print $2}')" = "${want}" ]; then
      return 0
    fi

    _ra8_lang_can_provision || _ra8_lang_refuse_foreign_host rust "${want}" || return 1

    arch="$(_ra8_lang_arch)" || return 1
    tools_dir="$(_ra8_lang_tools_dir)"
    dest="${tools_dir}/rust-${want}"

    if [ ! -x "${dest}/bin/rustc" ]; then
      case "${arch}" in
        x86_64) sha="$(_ra8_lang_pin RUST_SHA256_X86_64)" ;;
        aarch64) sha="$(_ra8_lang_pin RUST_SHA256_AARCH64)" ;;
      esac
      [ -n "${sha}" ] || return 1
      url="https://static.rust-lang.org/dist/rust-${want}-${arch}-unknown-linux-gnu.tar.xz"
      unpack="${tools_dir}/rust-${want}-dist"
      _ra8_lang_log "provisioning rust ${want} (${arch}); the runner image does not carry it"
      _ra8_lang_fetch "${url}" "${sha}" "${unpack}" 1 || return 1
      if ! "${unpack}/install.sh" --prefix="${dest}" --disable-ldconfig \
        --components="rustc,cargo,rust-std-${arch}-unknown-linux-gnu,rustfmt-preview,clippy-preview" >&2; then
        _ra8_lang_log "ERROR: the rust ${want} installer failed"
        rm -rf "${unpack}" "${dest}"
        return 1
      fi
      rm -rf "${unpack}"
    fi

    if [ "$("${dest}/bin/rustc" --version 2>/dev/null | awk '{print $2}')" != "${want}" ]; then
      _ra8_lang_log "ERROR: provisioned rustc at ${dest} is not ${want}"
      return 1
    fi
    _ra8_lang_prepend_path "${dest}/bin"
  }

  # The single entry point scripts/ci.sh calls. Both toolchains are attempted
  # and neither failure aborts: a gate that needs neither must still run on a
  # box that cannot reach either download host.
  use_pinned_lang_toolchains() {
    local zig_rc rust_rc

    # `|| true` would leave the callee running OUTSIDE errexit, so a failure
    # part-way through its body would be swallowed instead of returning. Disable
    # errexit around the CALL and read the status back, which is what
    # scripts/checks/check_errexit_masking.py asks for.
    set +e
    use_pinned_zig
    zig_rc=$?
    use_pinned_rust
    rust_rc=$?
    set -e

    if [ "${zig_rc}" -ne 0 ]; then
      _ra8_lang_log "note: no pinned zig; gates needing it will fail with their own message"
    fi
    if [ "${rust_rc}" -ne 0 ]; then
      _ra8_lang_log "note: no pinned rust; gates needing it will fail with their own message"
    fi
    return 0
  }

  # ==========================================================================
  # SELFTEST -- proves BOTH directions of the host-OS rule.
  #
  # The rule is easy to regress and impossible to observe from the Linux boxes
  # that run CI: a macOS host must never enter the download path, and a macOS
  # host that already carries the pin must still succeed. Every external
  # dependency is stubbed (uname, the Dockerfile pins, the cache directory,
  # the fetch itself), so the whole host matrix runs on any host.
  # ==========================================================================

  _RA8_LANG_SELFTEST_PIN_ZIG="9.9.9"
  _RA8_LANG_SELFTEST_PIN_RUST="9.9.9"
  _RA8_LANG_SELFTEST_FETCH_LOG=""
  _RA8_LANG_SELFTEST_OS="Linux"
  _RA8_LANG_SELFTEST_MACHINE="x86_64"

  # Replace the seams. Called only from lang_toolchains_selftest below.
  _ra8_lang_selftest_install_stubs() {
    local scratch="$1"
    _RA8_LANG_SELFTEST_FETCH_LOG="${scratch}/fetch.log"
    : >"${_RA8_LANG_SELFTEST_FETCH_LOG}"

    _ra8_lang_uname() {
      case "${1:-}" in
        -s) printf '%s\n' "${_RA8_LANG_SELFTEST_OS}" ;;
        -m) printf '%s\n' "${_RA8_LANG_SELFTEST_MACHINE}" ;;
        *) return 1 ;;
      esac
    }

    _ra8_lang_pin() {
      case "$1" in
        ZIG_VERSION) printf '%s\n' "${_RA8_LANG_SELFTEST_PIN_ZIG}" ;;
        RUST_VERSION) printf '%s\n' "${_RA8_LANG_SELFTEST_PIN_RUST}" ;;
        ZIG_SHA256_* | RUST_SHA256_*) printf 'deadbeef\n' ;;
        *) return 0 ;;
      esac
    }

    _ra8_lang_tools_dir() {
      printf '%s/tools\n' "${scratch}"
    }

    # Records the URL instead of downloading, and fails the way an unreachable
    # download does, so nothing can proceed on a stub.
    _ra8_lang_fetch() {
      printf '%s\n' "$1" >>"${_RA8_LANG_SELFTEST_FETCH_LOG}"
      return 1
    }
  }

  _ra8_lang_selftest_fetch_count() {
    wc -l <"${_RA8_LANG_SELFTEST_FETCH_LOG}" | tr -d ' '
  }

  _ra8_lang_selftest_fail() {
    echo "lang_toolchains.sh --selftest: $*" >&2
    return 1
  }

  # Direction 1: a Linux host still reaches the pinned Linux archives.
  _ra8_lang_selftest_linux_still_downloads() {
    local scratch="$1" out
    _RA8_LANG_SELFTEST_OS="Linux"
    _RA8_LANG_SELFTEST_MACHINE="x86_64"
    : >"${_RA8_LANG_SELFTEST_FETCH_LOG}"
    if out="$(PATH="${scratch}/empty" use_pinned_zig 2>&1)"; then
      _ra8_lang_selftest_fail "a failed zig download reported success"
      return 1
    fi
    case "$(cat "${_RA8_LANG_SELFTEST_FETCH_LOG}")" in
      *"zig-x86_64-linux-${_RA8_LANG_SELFTEST_PIN_ZIG}.tar.xz"*) ;;
      *)
        _ra8_lang_selftest_fail "Linux x86_64 did not request the pinned zig archive"
        return 1
        ;;
    esac
    case "${out}" in
      *"provisioning zig"*) ;;
      *)
        _ra8_lang_selftest_fail "Linux provisioning said nothing: ${out}"
        return 1
        ;;
    esac

    : >"${_RA8_LANG_SELFTEST_FETCH_LOG}"
    if PATH="${scratch}/empty" use_pinned_rust >/dev/null 2>&1; then
      _ra8_lang_selftest_fail "a failed rust download reported success"
      return 1
    fi
    case "$(cat "${_RA8_LANG_SELFTEST_FETCH_LOG}")" in
      *"rust-${_RA8_LANG_SELFTEST_PIN_RUST}-x86_64-unknown-linux-gnu.tar.xz"*) ;;
      *)
        _ra8_lang_selftest_fail "Linux x86_64 did not request the pinned rust archive"
        return 1
        ;;
    esac

    _RA8_LANG_SELFTEST_MACHINE="aarch64"
    : >"${_RA8_LANG_SELFTEST_FETCH_LOG}"
    if PATH="${scratch}/empty" use_pinned_zig >/dev/null 2>&1; then
      _ra8_lang_selftest_fail "a failed aarch64 zig download reported success"
      return 1
    fi
    case "$(cat "${_RA8_LANG_SELFTEST_FETCH_LOG}")" in
      *"zig-aarch64-linux-${_RA8_LANG_SELFTEST_PIN_ZIG}.tar.xz"*) ;;
      *)
        _ra8_lang_selftest_fail "Linux aarch64 did not request the aarch64 archive"
        return 1
        ;;
    esac
  }

  # Direction 2: a macOS host refuses BEFORE any download, and says why.
  _ra8_lang_selftest_macos_never_downloads() {
    local scratch="$1" out
    _RA8_LANG_SELFTEST_OS="Darwin"
    _RA8_LANG_SELFTEST_MACHINE="arm64"
    : >"${_RA8_LANG_SELFTEST_FETCH_LOG}"

    if out="$(PATH="${scratch}/empty" use_pinned_zig 2>&1)"; then
      _ra8_lang_selftest_fail "Darwin zig provisioning reported success"
      return 1
    fi
    case "${out}" in
      *"cannot provision zig ${_RA8_LANG_SELFTEST_PIN_ZIG} on Darwin/arm64"*) ;;
      *)
        _ra8_lang_selftest_fail "Darwin zig refusal gave no useful error: ${out}"
        return 1
        ;;
    esac

    if out="$(PATH="${scratch}/empty" use_pinned_rust 2>&1)"; then
      _ra8_lang_selftest_fail "Darwin rust provisioning reported success"
      return 1
    fi
    case "${out}" in
      *"cannot provision rust ${_RA8_LANG_SELFTEST_PIN_RUST} on Darwin/arm64"*) ;;
      *)
        _ra8_lang_selftest_fail "Darwin rust refusal gave no useful error: ${out}"
        return 1
        ;;
    esac

    if [ "$(_ra8_lang_selftest_fetch_count)" != "0" ]; then
      _ra8_lang_selftest_fail "Darwin reached the download path"
      return 1
    fi
  }

  # Direction 3: the Mac that IS set up correctly still passes, and the entry
  # point stays non-fatal there. That is the #899 case: the arm64 macOS gate
  # runs after its workflow installs the pin, and this file must not stop it.
  _ra8_lang_selftest_macos_path_hit() {
    local scratch="$1"
    _RA8_LANG_SELFTEST_OS="Darwin"
    _RA8_LANG_SELFTEST_MACHINE="arm64"
    : >"${_RA8_LANG_SELFTEST_FETCH_LOG}"
    if ! PATH="${scratch}/fake-bin:${scratch}/empty" use_pinned_zig >/dev/null 2>&1; then
      _ra8_lang_selftest_fail "a Darwin host carrying the pinned zig was refused"
      return 1
    fi
    if [ "$(_ra8_lang_selftest_fetch_count)" != "0" ]; then
      _ra8_lang_selftest_fail "a satisfied Darwin host still downloaded"
      return 1
    fi
    if ! PATH="${scratch}/empty" use_pinned_lang_toolchains >/dev/null 2>&1; then
      _ra8_lang_selftest_fail "use_pinned_lang_toolchains became fatal on Darwin"
      return 1
    fi
  }

  # The arch mapping both callers share, including its closed default.
  _ra8_lang_selftest_arch_matrix() {
    local row got
    _RA8_LANG_SELFTEST_OS="Linux"
    for row in "x86_64:x86_64" "aarch64:aarch64" "arm64:aarch64"; do
      _RA8_LANG_SELFTEST_MACHINE="${row%%:*}"
      got="$(_ra8_lang_arch)" || got="none"
      if [ "${got}" != "${row##*:}" ]; then
        _ra8_lang_selftest_fail "arch ${row%%:*} mapped to '${got}'"
        return 1
      fi
    done
    _RA8_LANG_SELFTEST_MACHINE="riscv64"
    if _ra8_lang_arch >/dev/null 2>&1; then
      _ra8_lang_selftest_fail "an unknown architecture did not fail closed"
      return 1
    fi
  }

  lang_toolchains_selftest() {
    set -euo pipefail
    local scratch
    scratch="$(mktemp -d "${TMPDIR:-/tmp}/ra8-lang-toolchains.XXXXXXXX")"
    trap 'rm -rf "${scratch}"' RETURN
    mkdir -p "${scratch}/empty" "${scratch}/fake-bin" "${scratch}/tools"
    printf '#!/bin/sh\nprintf "%%s\\n" "%s"\n' "${_RA8_LANG_SELFTEST_PIN_ZIG}" \
      >"${scratch}/fake-bin/zig"
    chmod +x "${scratch}/fake-bin/zig"

    _ra8_lang_selftest_install_stubs "${scratch}"
    _ra8_lang_selftest_arch_matrix
    _ra8_lang_selftest_linux_still_downloads "${scratch}"
    _ra8_lang_selftest_macos_never_downloads "${scratch}"
    _ra8_lang_selftest_macos_path_hit "${scratch}"

    echo "lang_toolchains.sh --selftest: PASS (arch matrix, Linux downloads, macOS refuses, macOS pin honoured)"
  }
fi

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  case "${1:-}" in
    --selftest) lang_toolchains_selftest ;;
    *)
      echo "Usage: scripts/ci/lib/lang_toolchains.sh --selftest" >&2
      echo "       (the provisioner itself is sourced, never executed)" >&2
      exit 2
      ;;
  esac
fi
