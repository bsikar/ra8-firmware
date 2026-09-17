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

  # The release archives name the host the same way for both toolchains.
  _ra8_lang_arch() {
    case "$(uname -m)" in
      x86_64) printf 'x86_64\n' ;;
      aarch64 | arm64) printf 'aarch64\n' ;;
      *) return 1 ;;
    esac
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
fi
