#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
# scripts/dev/provision_dev_box_toolchain.sh -- converge the Debian dev box's
# host-side gate tools to their pins, then run the `toolchain-parity` check.
# OWNERSHIP
# ---------
# The Ansible `dev_box` role is the only supported mutating entry point. It
# asserts a Debian-family host, owns every apt package and source build, then
# invokes this helper for release binaries and the isolated Python tool venv.
# CI runners do NOT call this script: their complete container images are built
# and deployed by the Ansible runner roles from infra/images/runner/Dockerfile.
# Python tool versions come from pyproject.toml and uv.lock; non-Python host
# tools still read their pins from .devcontainer/Dockerfile. The final
# check_tool_versions.py --all is the exact toolchain-parity assertion.
# Supported mutation: just infra::apply dev
# Read-only audit:     /bin/bash -p scripts/dev/provision_dev_box_toolchain.sh --check-only

if [[ "$-" == *p* ]]; then
  unset -v BASH_ENV ENV
  declare -a ra8_startup_env_unset=()
  _ra8_startup_refuse() {
    printf 'error: privileged startup %s\n' "$1" >&2
    exit 1
  }
  ra8_startup_env_done_count=0
  while IFS= read -r -d '' ra8_startup_env_row; do
    ra8_startup_env_name="${ra8_startup_env_row%%=*}"
    case "$ra8_startup_env_name" in
      RA8_STARTUP_ENV_DONE)
        ra8_startup_env_done_count=$((ra8_startup_env_done_count + 1))
        ;;
      BASH_FUNC_*%% | BASH_FUNC_*'()') ra8_startup_env_unset+=(-u "$ra8_startup_env_name") ;;
    esac
  done < <(
    /usr/bin/env -u RA8_STARTUP_ENV_DONE -0 &&
      /usr/bin/printf 'RA8_STARTUP_ENV_DONE=1\0'
  )
  ((ra8_startup_env_done_count == 1)) && [[ "$ra8_startup_env_name" == RA8_STARTUP_ENV_DONE ]] || _ra8_startup_refuse 'environment enumeration was incomplete'
  if ((${#ra8_startup_env_unset[@]})); then
    [[ -z "${RA8_STARTUP_ENV_SCRUBBED-}" ]] || _ra8_startup_refuse 'scrub did not converge'
    ra8_startup_reentry="$0"
    [[ "$ra8_startup_reentry" == */* ]] || _ra8_startup_refuse 'requires a script path'
    if [[ "$ra8_startup_reentry" != /* ]]; then
      ra8_startup_reentry="$PWD/$ra8_startup_reentry"
    fi
    ra8_startup_check="$ra8_startup_reentry"
    while [[ "$ra8_startup_check" != "/" ]]; do
      [[ ! -L "$ra8_startup_check" ]] || _ra8_startup_refuse 'refuses a symlinked path'
      ra8_startup_parent="${ra8_startup_check%/*}"
      [[ -n "$ra8_startup_parent" ]] || ra8_startup_parent="/"
      [[ "$ra8_startup_parent" != "$ra8_startup_check" ]] ||
        _ra8_startup_refuse 'cannot validate its script path'
      ra8_startup_check="$ra8_startup_parent"
    done
    [[ -f "$ra8_startup_reentry" ]] || _ra8_startup_refuse 'refuses a non-regular path'
    if ! exec /usr/bin/env "${ra8_startup_env_unset[@]}" -u BASH_ENV -u ENV \
      -u RA8_STARTUP_ENV_DONE RA8_STARTUP_ENV_SCRUBBED=1 \
      /bin/bash -p -- "$ra8_startup_reentry" "$@"; then
      _ra8_startup_refuse 'could not enter sanitized process'
    fi
  fi
  unset -v ra8_startup_check ra8_startup_env_done_count
  unset -v ra8_startup_env_name ra8_startup_env_row
  unset -v ra8_startup_env_unset ra8_startup_parent ra8_startup_reentry
  unset -v RA8_STARTUP_ENV_DONE
  unset -v RA8_STARTUP_ENV_SCRUBBED
  unset -f _ra8_startup_refuse

  set -euo pipefail

  export BASH_ENV=/dev/null ENV=/dev/null PYTHONNOUSERSITE=1
  unset PYTHONHOME PYTHONPATH RA8_TOOL_VENV TMPDIR
  PATH=/usr/local/bin:/usr/bin:/bin
  export PATH

  if [[ "${1:-}" == "--selftest-boundary" ]]; then
    [[ "$PATH" == "/usr/local/bin:/usr/bin:/bin" && -z "${PYTHONHOME:-}" &&
      -z "${PYTHONPATH:-}" && -z "${RA8_TOOL_VENV:-}" ]] || exit 1
    exit 0
  fi

  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
  DOCKERFILE="${ROOT}/.devcontainer/Dockerfile"
  BIN_DIR="/usr/local/bin"
  UV_CACHE_ROOT="/opt/ra8-uv-cache"
  RELEASE_TMP_DIR=""
  RELEASE_TMP_IDENTITY=""
  RELEASE_TMP_OWNER_UID=""
  RELEASE_TMP_ROOT=""
  RELEASE_TMP_ROOT_IDENTITY=""
  RELEASE_TMP_ALLOCATION_PENDING=0
  RELEASE_TMP_CHECKPOINT_MODE=""
  RELEASE_SELFTEST_RAW_SHA256="ca44963575fff3ac3e12bfbf2adca1413a39d565d60830ffc50f1f900cfa5eae"

  # Read `ARG <name>=<value>` from the Dockerfile. Fails loudly (non-empty guard
  # by the caller) if the pin is absent, so a renamed ARG cannot silently skip a
  # tool.
  dockerfile_arg() {
    local name="$1" value
    value="$(sed -n "s/^ARG ${name}=\(.*\)$/\1/p" "${DOCKERFILE}" | head -1)"
    printf '%s' "${value}"
  }

  # The Ansible task normally enters as root. Keep escalation scoped to individual
  # mutations so `--check-only` remains an entirely unprivileged audit path.
  as_root() {
    if [ "$(id -u)" -eq 0 ]; then
      "$@"
    else
      sudo "$@"
    fi
  }

  # Invoke the audited bootstrap through one fixed interpreter/cache boundary.
  # Keeping this in one function lets the offline contract selftest replace the
  # action without reaching the live cache or network.
  uv_bootstrap() {
    /usr/bin/python3 -B -I -S "${ROOT}/scripts/dev/bootstrap_uv.py" \
      --cache-root "${UV_CACHE_ROOT}" "$@"
  }

  uv_bootstrap_apply() {
    as_root /usr/bin/python3 -B -I -S "${ROOT}/scripts/dev/bootstrap_uv.py" \
      --cache-root "${UV_CACHE_ROOT}" "$@"
  }

  uv_bootstrap_apply_run() {
    local project_environment="$1"
    shift
    if [ -n "${project_environment}" ]; then
      as_root env UV_PROJECT_ENVIRONMENT="${project_environment}" UV_PYTHON_DOWNLOADS=never \
        /usr/bin/python3 -B -I -S "${ROOT}/scripts/dev/bootstrap_uv.py" \
        --cache-root "${UV_CACHE_ROOT}" --run "$@"
    else
      as_root env UV_PYTHON_DOWNLOADS=never \
        /usr/bin/python3 -B -I -S "${ROOT}/scripts/dev/bootstrap_uv.py" \
        --cache-root "${UV_CACHE_ROOT}" --run "$@"
    fi
  }

  uv_cache_modes_current() {
    uv_bootstrap --check-cache-modes >/dev/null 2>&1
  }

  uv_cache_check() {
    local status=0
    if uv_bootstrap --verify-cache >/dev/null; then
      return 0
    else
      status=$?
    fi
    if [ "${status}" -eq 2 ]; then
      printf '  ...  %-11s %s -> %s\n' uv-cache restricted 'shared (planned)'
      return 2
    fi
    echo "error: authenticated uv cache check failed" >&2
    return "${status}"
  }

  uv_cache_apply_report() {
    local modes_current="$1"
    if [ "${modes_current}" -eq 0 ]; then
      printf '  ...  %-11s %s -> %s\n' uv-cache restricted shared
    fi
  }

  download_verified() {
    local url="$1" sha="$2" output="$3"
    [[ "${sha}" =~ ^[0-9a-f]{64}$ ]] || {
      echo "error: invalid sha256 pin for ${url}" >&2
      return 1
    }
    curl --proto '=https' --proto-redir '=https' --tlsv1.2 -fsSL \
      -o "${output}" "${url}"
    printf '%s  %s\n' "${sha}" "${output}" | sha256sum -c - >/dev/null
  }
  release_tmp_reset() {
    RELEASE_TMP_DIR=""
    RELEASE_TMP_IDENTITY=""
    RELEASE_TMP_OWNER_UID=""
    RELEASE_TMP_ROOT=""
    RELEASE_TMP_ROOT_IDENTITY=""
    RELEASE_TMP_ALLOCATION_PENDING=0
  }

  release_tmp_identity() {
    stat -Lc '%d:%i' -- "$1"
  }

  release_tmp_root_is_safe() {
    local root="$1" identity="$2" canonical
    canonical="$(cd -P /tmp && pwd)" || return 1
    [[ "$root" == "$canonical" && "$root" == /* && ! -L "$root" && -d "$root" &&
      "$(release_tmp_identity "$root")" == "$identity" &&
      "$(stat -Lc '%u:%a' -- "$root")" == "0:1777" ]]
  }

  release_tmp_is_safe() {
    local suffix
    suffix="${RELEASE_TMP_DIR#"$RELEASE_TMP_ROOT/ra8-tool-install."}"
    release_tmp_root_is_safe "$RELEASE_TMP_ROOT" "$RELEASE_TMP_ROOT_IDENTITY" &&
      [[ "$RELEASE_TMP_DIR" == "$RELEASE_TMP_ROOT/ra8-tool-install.$suffix" &&
        "$suffix" =~ ^[0-9a-f]{32}$ && "$RELEASE_TMP_OWNER_UID" =~ ^[0-9]+$ &&
        ! -L "$RELEASE_TMP_DIR" && -d "$RELEASE_TMP_DIR" &&
        "$(release_tmp_identity "$RELEASE_TMP_DIR")" == "$RELEASE_TMP_IDENTITY" &&
        "$(stat -Lc '%u:%a' -- "$RELEASE_TMP_DIR")" == "$RELEASE_TMP_OWNER_UID:700" ]]
  }

  release_tmp_pending_cleanup() {
    local suffix
    suffix="${RELEASE_TMP_DIR#"$RELEASE_TMP_ROOT/ra8-tool-install."}"
    [[ "$RELEASE_TMP_ALLOCATION_PENDING" == "1" && -z "$RELEASE_TMP_IDENTITY" &&
      "$RELEASE_TMP_OWNER_UID" =~ ^[0-9]+$ ]] &&
      release_tmp_root_is_safe "$RELEASE_TMP_ROOT" "$RELEASE_TMP_ROOT_IDENTITY" &&
      [[ "$RELEASE_TMP_DIR" == "$RELEASE_TMP_ROOT/ra8-tool-install.$suffix" &&
        "$suffix" =~ ^[0-9a-f]{32}$ ]] || return 1
    if [[ -e "$RELEASE_TMP_DIR" || -L "$RELEASE_TMP_DIR" ]]; then
      [[ ! -L "$RELEASE_TMP_DIR" && -d "$RELEASE_TMP_DIR" &&
        "$(stat -Lc '%u:%a' -- "$RELEASE_TMP_DIR")" == "$RELEASE_TMP_OWNER_UID:700" ]] ||
        return 1
      rmdir -- "$RELEASE_TMP_DIR" || return 1
    fi
    [[ ! -e "$RELEASE_TMP_DIR" && ! -L "$RELEASE_TMP_DIR" ]] || return 1
    release_tmp_reset
  }

  release_tmp_cleanup_owned() {
    if [[ "$RELEASE_TMP_ALLOCATION_PENDING" == "1" ]]; then
      release_tmp_pending_cleanup
      return
    fi
    release_tmp_is_safe || return 1
    rm -rf -- "$RELEASE_TMP_DIR" || return 1
    [[ ! -e "$RELEASE_TMP_DIR" && ! -L "$RELEASE_TMP_DIR" ]] || return 1
    release_tmp_reset
  }

  release_tmp_exit() {
    local status=$?
    trap - EXIT HUP INT TERM
    release_tmp_cleanup_owned || exit 1
    exit "$status"
  }

  release_tmp_signal() {
    local status="$1"
    trap - EXIT HUP INT TERM
    release_tmp_cleanup_owned || exit 1
    exit "$status"
  }

  install_release_tmp_traps() {
    trap release_tmp_exit EXIT
    trap 'release_tmp_signal 129' HUP
    trap 'release_tmp_signal 130' INT
    trap 'release_tmp_signal 143' TERM
  }

  release_tmp_allocation_checkpoint() {
    local phase="$1"
    [[ -n "$RELEASE_TMP_CHECKPOINT_MODE" ]] || return 0
    [[ "$RELEASE_TMP_CHECKPOINT_MODE" == "precreate" ||
      "$RELEASE_TMP_CHECKPOINT_MODE" == "created" ]] || return 1
    [[ "$RELEASE_TMP_CHECKPOINT_MODE" == "$phase" ]] || return 0
    [[ "${BASH_SUBSHELL:-0}" == "0" ]] || return 1
    printf '%s\n' "$RELEASE_TMP_DIR"
    kill -TERM "$$"
    return 1
  }

  release_tmp_begin() {
    local suffix candidate attempt
    [[ -z "$RELEASE_TMP_DIR" && -z "$RELEASE_TMP_IDENTITY" &&
      -z "$RELEASE_TMP_OWNER_UID" && -z "$RELEASE_TMP_ROOT" &&
      -z "$RELEASE_TMP_ROOT_IDENTITY" && "$RELEASE_TMP_ALLOCATION_PENDING" == "0" ]] ||
      return 1
    RELEASE_TMP_ROOT="$(cd -P /tmp && pwd)" || return 1
    RELEASE_TMP_ROOT_IDENTITY="$(release_tmp_identity "$RELEASE_TMP_ROOT")" || return 1
    RELEASE_TMP_OWNER_UID="$(id -u)" || return 1
    release_tmp_root_is_safe "$RELEASE_TMP_ROOT" "$RELEASE_TMP_ROOT_IDENTITY" || return 1
    for ((attempt = 0; attempt < 20; ++attempt)); do
      suffix="$(od -An -N16 -tx1 /dev/urandom | tr -d '[:space:]')" || return 1
      [[ "$suffix" =~ ^[0-9a-f]{32}$ ]] || return 1
      candidate="$RELEASE_TMP_ROOT/ra8-tool-install.$suffix"
      if [[ ! -e "$candidate" && ! -L "$candidate" ]]; then
        RELEASE_TMP_DIR="$candidate"
        break
      fi
    done
    [[ -n "$RELEASE_TMP_DIR" ]] || return 1
    RELEASE_TMP_ALLOCATION_PENDING=1
    install_release_tmp_traps
    release_tmp_allocation_checkpoint precreate
    (umask 077 && mkdir -m 0700 -- "$RELEASE_TMP_DIR") || return 1
    release_tmp_allocation_checkpoint created
    RELEASE_TMP_IDENTITY="$(release_tmp_identity "$RELEASE_TMP_DIR")" || return 1
    release_tmp_is_safe || return 1
    RELEASE_TMP_ALLOCATION_PENDING=0
  }

  install_shellcheck() (
    local version="$1" arch sha
    case "$(uname -m)" in
      x86_64)
        arch=x86_64
        sha="$(dockerfile_arg SHELLCHECK_SHA256_X86_64)"
        ;;
      aarch64)
        arch=aarch64
        sha="$(dockerfile_arg SHELLCHECK_SHA256_AARCH64)"
        ;;
      *)
        echo "error: unsupported shellcheck architecture: $(uname -m)" >&2
        return 1
        ;;
    esac
    local url tmp
    release_tmp_begin
    tmp="$RELEASE_TMP_DIR"
    url="https://github.com/koalaman/shellcheck/releases/download/v${version}/shellcheck-v${version}.linux.${arch}.tar.xz"
    download_verified "${url}" "${sha}" "${tmp}/shellcheck.tar.xz"
    mkdir "${tmp}/extract"
    tar -xJf "${tmp}/shellcheck.tar.xz" -C "${tmp}/extract" --strip-components=1 \
      "shellcheck-v${version}/shellcheck"
    as_root install -m 0755 "${tmp}/extract/shellcheck" "${BIN_DIR}/shellcheck"
  )

  install_shfmt() (
    local version="$1" arch sha tmp
    case "$(uname -m)" in
      x86_64)
        arch=amd64
        sha="$(dockerfile_arg SHFMT_SHA256_AMD64)"
        ;;
      aarch64)
        arch=arm64
        sha="$(dockerfile_arg SHFMT_SHA256_ARM64)"
        ;;
      *)
        echo "error: unsupported shfmt architecture: $(uname -m)" >&2
        return 1
        ;;
    esac
    release_tmp_begin
    tmp="$RELEASE_TMP_DIR"
    download_verified \
      "https://github.com/mvdan/sh/releases/download/v${version}/shfmt_v${version}_linux_${arch}" \
      "${sha}" "${tmp}/shfmt"
    as_root install -m 0755 "${tmp}/shfmt" "${BIN_DIR}/shfmt"
  )

  install_actionlint() (
    local version="$1" arch sha tmp
    case "$(uname -m)" in
      x86_64)
        arch=amd64
        sha="$(dockerfile_arg ACTIONLINT_SHA256_AMD64)"
        ;;
      aarch64)
        arch=arm64
        sha="$(dockerfile_arg ACTIONLINT_SHA256_ARM64)"
        ;;
      *)
        echo "error: unsupported actionlint architecture: $(uname -m)" >&2
        return 1
        ;;
    esac
    release_tmp_begin
    tmp="$RELEASE_TMP_DIR"
    download_verified \
      "https://github.com/rhysd/actionlint/releases/download/v${version}/actionlint_${version}_linux_${arch}.tar.gz" \
      "${sha}" "${tmp}/actionlint.tar.gz"
    mkdir "${tmp}/extract"
    tar -xzf "${tmp}/actionlint.tar.gz" -C "${tmp}/extract" actionlint
    as_root install -m 0755 "${tmp}/extract/actionlint" "${BIN_DIR}/actionlint"
  )

  install_hadolint() (
    local version="$1" arch sha tmp
    case "$(uname -m)" in
      x86_64)
        arch=x86_64
        sha="$(dockerfile_arg HADOLINT_SHA256_X86_64)"
        ;;
      aarch64)
        arch=arm64
        sha="$(dockerfile_arg HADOLINT_SHA256_ARM64)"
        ;;
      *)
        echo "error: unsupported hadolint architecture: $(uname -m)" >&2
        return 1
        ;;
    esac
    release_tmp_begin
    tmp="$RELEASE_TMP_DIR"
    download_verified \
      "https://github.com/hadolint/hadolint/releases/download/v${version}/hadolint-linux-${arch}" \
      "${sha}" "${tmp}/hadolint"
    as_root install -m 0755 "${tmp}/hadolint" "${BIN_DIR}/hadolint"
  )

  install_just() (
    local version="$1" arch sha tmp
    case "$(uname -m)" in
      x86_64)
        arch=x86_64
        sha="$(dockerfile_arg JUST_SHA256_X86_64)"
        ;;
      aarch64)
        arch=aarch64
        sha="$(dockerfile_arg JUST_SHA256_AARCH64)"
        ;;
      *)
        echo "error: unsupported just architecture: $(uname -m)" >&2
        return 1
        ;;
    esac
    release_tmp_begin
    tmp="$RELEASE_TMP_DIR"
    download_verified \
      "https://github.com/casey/just/releases/download/${version}/just-${version}-${arch}-unknown-linux-musl.tar.gz" \
      "${sha}" "${tmp}/just.tar.gz"
    mkdir "${tmp}/extract"
    tar -xzf "${tmp}/just.tar.gz" -C "${tmp}/extract" just
    as_root install -m 0755 "${tmp}/extract/just" "${BIN_DIR}/just"
  )

  # The pinned doxygen release, installed exactly the way .devcontainer/Dockerfile
  # installs it: same URL shape, same sha256 check, same /usr/local/bin
  # destination shadowing apt's package (#522).
  #
  # The sha256 is read here rather than threaded through ensure_release_tool: it
  # belongs to the release, not to the caller, and reading it at the point of use
  # keeps the one-argument installer contract every other tool here follows.
  install_doxygen() (
    local version="$1" sha tmp
    sha="$(dockerfile_arg DOXYGEN_SHA256_LINUX_X64)"
    [ -n "${sha}" ] || {
      echo "error: DOXYGEN_SHA256_LINUX_X64 is not pinned in the Dockerfile" >&2
      return 1
    }
    release_tmp_begin
    tmp="$RELEASE_TMP_DIR"
    download_verified \
      "https://github.com/doxygen/doxygen/releases/download/Release_${version//./_}/doxygen-${version}.linux.bin.tar.gz" \
      "${sha}" "${tmp}/doxygen.tar.gz"
    mkdir "${tmp}/extract"
    tar -xzf "${tmp}/doxygen.tar.gz" -C "${tmp}/extract" \
      "doxygen-${version}/bin/doxygen"
    as_root install -m 0755 "${tmp}/extract/doxygen-${version}/bin/doxygen" \
      "${BIN_DIR}/doxygen"
  )

  require_release_digests() {
    local name value
    for name in SHELLCHECK_SHA256_X86_64 SHELLCHECK_SHA256_AARCH64 \
      SHFMT_SHA256_AMD64 SHFMT_SHA256_ARM64 ACTIONLINT_SHA256_AMD64 \
      ACTIONLINT_SHA256_ARM64 HADOLINT_SHA256_X86_64 HADOLINT_SHA256_ARM64 \
      JUST_SHA256_X86_64 JUST_SHA256_AARCH64 DOXYGEN_SHA256_LINUX_X64; do
      value="$(dockerfile_arg "${name}")"
      [[ "${value}" =~ ^[0-9a-f]{64}$ ]] || {
        echo "error: ${name} is not a sha256 pin in the Dockerfile" >&2
        return 1
      }
    done
  }

  # Install a GitHub-release binary tool only when the wanted version is not
  # already the one on PATH. $3 is a shell snippet that echoes the installed
  # version (empty if absent), so the compare stays tool-specific.
  ensure_release_tool() {
    local name="$1" want="$2" have="$3" installer="$4"
    if [ "${have}" = "${want}" ]; then
      printf '  ok   %-11s %s (pinned)\n' "${name}" "${want}"
      return 0
    fi
    printf '  ...  %-11s %s -> %s\n' "${name}" "${have:-absent}" "${want}"
    "${installer}" "${want}"
  }

  # The GitHub-release binaries and uv-managed Python tools, at the versions
  # read from their native or project authorities. Split out of main() so each
  # 60-line NASA P10 Rule 4 cap the repo enforces on shell as well as C.
  install_pinned_tools() {
    local shellcheck_v="$1" shfmt_v="$2" actionlint_v="$3" hadolint_v="$4"
    local just_v="$5" doxygen_v="$6"
    local just_bin="" just_installed_v=""

    ensure_release_tool shellcheck "${shellcheck_v}" \
      "$(shellcheck --version 2>/dev/null | sed -n 's/^version: //p')" install_shellcheck
    ensure_release_tool shfmt "${shfmt_v}" \
      "$(shfmt --version 2>/dev/null | sed 's/^v//')" install_shfmt
    ensure_release_tool actionlint "${actionlint_v}" \
      "$(actionlint --version 2>/dev/null | head -1)" install_actionlint
    ensure_release_tool hadolint "${hadolint_v}" \
      "$(hadolint --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')" install_hadolint
    just_bin="$(command -v just 2>/dev/null || true)"
    if [[ -n "$just_bin" ]]; then
      just_installed_v="$("$just_bin" --version 2>/dev/null | awk '{print $2}')"
    fi
    ensure_release_tool just "${just_v}" "${just_installed_v}" install_just
    ensure_release_tool doxygen "${doxygen_v}" \
      "$(doxygen --version 2>/dev/null | awk '{print $1}')" install_doxygen
  }

  # Synchronize Python-managed gate tools into the root-owned service
  # environment from the repository lock. The verified uv archive is retained in
  # /opt so every later converge can authenticate the cached executable again.
  install_python_tools() {
    local venv="$1" cache_modes_current=1
    if ! uv_cache_modes_current; then
      cache_modes_current=0
    fi
    uv_bootstrap_apply --ensure >/dev/null
    uv_cache_apply_report "${cache_modes_current}"
    uv_bootstrap_apply_run "${venv}" --directory "${ROOT}" --no-config sync \
      --locked --only-group ci --no-install-project --python /usr/bin/python3
    uv_bootstrap_apply_run "" --directory "${ROOT}" \
      --no-config lock --check
    uv_bootstrap_apply_run "" --no-config pip check \
      --python "${venv}/bin/python3"
    "${venv}/bin/python3" -c 'import PIL, clang.cindex, dotenv, kasa, serial, usb.core, yaml'
    "${venv}/bin/ruff" --version
    "${venv}/bin/cmake-format" --version
    "${venv}/bin/yamllint" --version
    "${venv}/bin/gcovr" --version
    as_root rm -f -- "${venv}/.lock"
    as_root /usr/bin/python3 -I "${ROOT}/scripts/dev/managed_python_env.py" write \
      --env "${venv}" --pyproject "${ROOT}/pyproject.toml" \
      --lock "${ROOT}/uv.lock" --group ci
    as_root /usr/bin/python3 -I "${ROOT}/scripts/dev/managed_python_env.py" verify \
      --env "${venv}" --pyproject "${ROOT}/pyproject.toml" \
      --lock "${ROOT}/uv.lock" --group ci
  }

  release_selftest_files_are_safe() {
    local main="$1" helper="$2" expected_dir="$3"
    local resolved caller_uid main_owner main_group
    resolved="$(cd -P "$(dirname "$main")" && pwd)" || return 1
    caller_uid="$(id -u)" || return 1
    main_owner="$(stat -c '%u' "$main")" || return 1
    main_group="$(stat -c '%g' "$main")" || return 1
    [[ "$resolved" == "$expected_dir" && -f "$main" && ! -L "$main" &&
      "$(stat -c '%h' "$main")" == "1" && "$(stat -c '%a' "$main")" == "755" &&
      ("$main_owner" == "0" || "$main_owner" == "$caller_uid") &&
      ("$caller_uid" != "0" || ("$main_owner" == "0" && "$main_group" == "0")) &&
      -f "$helper" && ! -L "$helper" && "$(stat -c '%h' "$helper")" == "1" &&
      "$(stat -c '%a' "$helper")" == "644" &&
      "$(stat -c '%u' "$helper")" == "$main_owner" &&
      "$(stat -c '%g' "$helper")" == "$main_group" ]]
  }

  release_selftest_open_checkpoint() { :; }

  source_release_selftest_helper_from() {
    local main="$1" helper="$2" expected_dir="$3" expected_digest="$4"
    local identity digest source_path source_status=0
    release_selftest_files_are_safe "$main" "$helper" "$expected_dir" || return 1
    identity="$(stat -c '%d:%i' "$helper")" || return 1
    exec 7<"$helper" || return 1
    exec 8<"$helper" || {
      exec 7<&-
      return 1
    }
    [[ "$(stat -Lc '%d:%i' /proc/self/fd/7)" == "$identity" &&
    "$(stat -Lc '%d:%i' /proc/self/fd/8)" == "$identity" ]] || {
      exec 7<&- 8<&-
      return 1
    }
    digest="$(sha256sum <&7)" || {
      exec 7<&- 8<&-
      return 1
    }
    exec 7<&-
    digest="${digest%% *}"
    [[ "$digest" == "$expected_digest" ]] || {
      exec 8<&-
      return 1
    }
    release_selftest_open_checkpoint || {
      exec 8<&-
      return 1
    }
    source_path="/proc/self/fd/8"
    export PROVISION_RELEASE_SOURCE_DIR="$expected_dir"
    export PROVISION_RELEASE_PARENT="$main"
    # FD 8 is separately identity- and digest-bound above; its /proc path is
    # dynamic and cannot be represented by a static ShellCheck source path.
    # shellcheck source=/dev/null
    source "$source_path" || source_status=$?
    exec 8<&- || return 1
    [[ "$source_status" == "0" ]] || return "$source_status"
  }

  load_release_selftest_helper() {
    local helper="$ROOT/scripts/dev/provision_dev_box_toolchain_selftest.bash"
    local main="$ROOT/scripts/dev/provision_dev_box_toolchain.sh"
    local expected_dir="$ROOT/scripts/dev" expected_digest="$RELEASE_SELFTEST_RAW_SHA256"
    source_release_selftest_helper_from "$main" "$helper" "$expected_dir" "$expected_digest" || return 1
    declare -F release_tmp_contract_selftest release_tmp_signal_child >/dev/null
  }

  main() {
    local check_only=0
    local uv_check_status=0
    [ "${1:-}" = "--check-only" ] && check_only=1

    [ -f "${DOCKERFILE}" ] || {
      echo "error: ${DOCKERFILE} not found -- run from a full checkout" >&2
      exit 1
    }

    local shellcheck_v shfmt_v actionlint_v hadolint_v just_v doxygen_v
    local python_venv
    shellcheck_v="$(dockerfile_arg SHELLCHECK_VERSION)"
    shfmt_v="$(dockerfile_arg SHFMT_VERSION)"
    actionlint_v="$(dockerfile_arg ACTIONLINT_VERSION)"
    hadolint_v="$(dockerfile_arg HADOLINT_VERSION)"
    just_v="$(dockerfile_arg JUST_VERSION)"
    python_venv="$(dockerfile_arg PYTHON_TOOL_VENV)"
    doxygen_v="$(dockerfile_arg DOXYGEN_VERSION)"

    for pair in "SHELLCHECK_VERSION=${shellcheck_v}" "SHFMT_VERSION=${shfmt_v}" \
      "ACTIONLINT_VERSION=${actionlint_v}" "HADOLINT_VERSION=${hadolint_v}" \
      "JUST_VERSION=${just_v}" "PYTHON_TOOL_VENV=${python_venv}" \
      "DOXYGEN_VERSION=${doxygen_v}"; do
      [ -n "${pair#*=}" ] || {
        echo "error: could not read ${pair%%=*} from the Dockerfile" >&2
        exit 1
      }
    done
    require_release_digests

    if [ "${check_only}" -eq 0 ]; then
      echo "provisioning dev-box host tools from ${DOCKERFILE#"${ROOT}"/} pins:"
      install_pinned_tools "${shellcheck_v}" "${shfmt_v}" "${actionlint_v}" \
        "${hadolint_v}" "${just_v}" "${doxygen_v}"
      install_python_tools "${python_venv}"
    else
      if uv_cache_check; then
        :
      else
        uv_check_status=$?
        return "${uv_check_status}"
      fi
    fi

    [ -x "${python_venv}/bin/python3" ] || {
      echo "error: Python tool venv missing at ${python_venv}; run just infra::apply dev" >&2
      exit 1
    }
    /usr/bin/python3 -I "${ROOT}/scripts/dev/managed_python_env.py" verify \
      --env "${python_venv}" --pyproject "${ROOT}/pyproject.toml" \
      --lock "${ROOT}/uv.lock" --group ci
    PATH="${python_venv}/bin:${PATH}"
    export PATH
    python3 -c 'import clang.cindex, yaml'
    echo "verifying parity (the check the toolchain-parity gate runs):"
    python3 "${ROOT}/scripts/checks/check_tool_versions.py" --all
  }

  if [ "${1:-}" = "--selftest-release-tmp-signal" ]; then
    [[ "$#" == "2" ]] || exit 1
    load_release_selftest_helper || exit 1
    release_tmp_signal_child "$2"
  elif [ "${1:-}" = "--selftest-python-allocation-signal" ]; then
    [[ "$#" == "3" ]] || exit 1
    load_release_selftest_helper || exit 1
    python_selftest_allocation_signal_child "$2" "$3"
  elif [ "${1:-}" = "--selftest-uv-cache-contract" ]; then
    load_release_selftest_helper || exit 1
    uv_cache_contract_selftest
  else
    main "$@"
  fi
else
  [[ "$-" == *p* ]]
fi
