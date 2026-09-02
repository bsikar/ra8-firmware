#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# Provision a fresh macOS checkout to build and run ra8_emulator. Everything
# installed by this script stays under the invoking user's home directory.

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

  case "${1:-}" in
    "") ;;
    -h | --help)
      cat <<'EOF'
Usage: just apps::emulator::setup

Installs the macOS dependencies needed to build firmware and run ra8_emulator:
Xcode Command Line Tools, Homebrew packages, Arm GNU Toolchain 13.3.rel1, and
the pinned Unicorn library.  Installs remain in the invoking user's home
directory except for the standard Xcode/Homebrew installers.
EOF
      exit 0
      ;;
    *)
      echo "ERROR: unknown argument '$1' (try --help)." >&2
      exit 2
      ;;
  esac

  if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "ERROR: scripts/emu/setup_macos.sh only supports macOS." >&2
    exit 2
  fi

  root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
  dockerfile="${root}/.devcontainer/Dockerfile"
  prefix="${RA8_UNICORN_PREFIX:-${HOME}/.local/ra8-firmware/unicorn}"
  homebrew_installer_commit="b9990527570f7e07d5393f37447b8293ec0a78de"
  homebrew_installer_sha256="12479a24be3f5307eecac7cde670fad7118640f031229e964f544b1367b52a41"

  # The Dockerfile is the repository's non-Python tool pin authority. macOS uses
  # its Darwin-specific Arm archives while deriving the release and hashes from
  # that same source instead of maintaining a second set of literals here.
  dockerfile_arg() {
    local name="$1" value
    if ! value="$(awk -v name="${name}" '
    function inspect_instruction(    body, line, lower, target) {
      line = logical
      sub(/^[[:blank:]]*/, "", line)
      lower = tolower(line)
      if (substr(lower, 1, 3) != "arg" ||
          substr(line, 4, 1) !~ /[[:blank:]]/) {
        return
      }
      body = substr(line, 5)
      sub(/^[[:blank:]]+/, "", body)
      target = "(^|[[:blank:]])" name "([=[:blank:]]|$)"
      if (body !~ target) {
        return
      }
      count++
      if (continued || index(body, name "=") != 1) {
        invalid = 1
        return
      }
      value = substr(body, length(name) + 2)
    }
    {
      physical = $0
      if (physical ~ /^[[:blank:]]*(#.*)?$/) {
        next
      }
      has_continuation = physical ~ /\\[[:blank:]]*$/
      if (has_continuation) {
        sub(/\\[[:blank:]]*$/, "", physical)
        logical = logical physical
        continued = 1
        next
      }
      logical = logical physical
      inspect_instruction()
      logical = ""
      continued = 0
    }
    END {
      if (logical != "") {
        invalid = 1
        inspect_instruction()
      }
      if (count != 1 || invalid) exit 2
      printf "%s", value
    }
  ' "${dockerfile}")"; then
      echo "ERROR: expected one canonical ARG ${name}=... in ${dockerfile}." >&2
      return 1
    fi
    printf '%s' "${value}"
  }

  require_arm_hash_pins() {
    local name value
    for name in ARM_GCC_SHA256_X86_64 ARM_GCC_SHA256_AARCH64 \
      ARM_GCC_SHA256_DARWIN_X86_64 ARM_GCC_SHA256_DARWIN_ARM64; do
      value="$(dockerfile_arg "${name}")" || return 1
      if [[ ! "${value}" =~ ^[0-9a-f]{64}$ ]]; then
        echo "ERROR: ${name} must be a 64-hex sha256 in ${dockerfile}." >&2
        return 1
      fi
    done
  }

  canonical_cleanup_target() {
    if [[ "$#" -ne 2 ]]; then
      echo "ERROR: cleanup validation requires one target and one allowed root." >&2
      return 1
    fi
    local target="$1" allowed_root="$2"
    local root_name root_parent root_physical target_name target_parent canonical
    if [[ "$target" != /* ]] || [[ "$allowed_root" != /* ]] || [[ "$allowed_root" == "/" ]]; then
      echo "ERROR: refusing relative target or unsafe cleanup root." >&2
      return 1
    fi
    root_name="$(basename -- "$allowed_root")"
    target_name="$(basename -- "$target")"
    if [[ -z "$root_name" ]] || [[ "$root_name" == "." ]] || [[ "$root_name" == ".." ]] ||
      [[ -z "$target_name" ]] || [[ "$target_name" == "." ]] || [[ "$target_name" == ".." ]]; then
      echo "ERROR: refusing unsafe cleanup path component." >&2
      return 1
    fi
    root_parent="$(cd "$(dirname -- "$allowed_root")" && pwd -P)" || return 1
    root_physical="${root_parent}/${root_name}"
    if [[ -e "$allowed_root" ]] || [[ -L "$allowed_root" ]]; then
      if [[ ! -d "$allowed_root" ]] || [[ -L "$allowed_root" ]] ||
        [[ "$(cd "$allowed_root" && pwd -P)" != "$root_physical" ]]; then
        echo "ERROR: cleanup root is not a physical directory: $allowed_root" >&2
        return 1
      fi
    fi
    if [[ -d "$(dirname -- "$target")" ]]; then
      target_parent="$(cd "$(dirname -- "$target")" && pwd -P)" || return 1
    elif [[ "$(dirname -- "$target")" == "$root_physical" ]] && [[ ! -e "$allowed_root" ]]; then
      target_parent="$root_physical"
    else
      echo "ERROR: cleanup target parent cannot be canonicalized: $target" >&2
      return 1
    fi
    canonical="${target_parent}/${target_name}"
    if [[ "$target_parent" != "$root_physical" ]] || [[ -L "$canonical" ]] ||
      { [[ -e "$canonical" ]] && [[ ! -d "$canonical" ]]; }; then
      echo "ERROR: cleanup target escapes its allowed root: $target" >&2
      return 1
    fi
    printf '%s\n' "$canonical"
  }

  safe_remove_tree() {
    if [[ "$#" -ne 2 ]]; then
      echo "ERROR: safe removal requires one target and one allowed root." >&2
      return 1
    fi
    local target="$1" allowed_root="$2" canonical
    canonical="$(canonical_cleanup_target "$target" "$allowed_root")" || return 1
    if [[ "$canonical" != "$target" ]]; then
      echo "ERROR: cleanup target changed after validation: $target" >&2
      return 1
    fi
    command rm -rf -- "$canonical"
  }

  arm_release="$(dockerfile_arg ARM_GCC_RELEASE)" || exit 1
  readonly arm_release
  if [[ ! "${arm_release}" =~ ^[0-9]+\.[0-9]+\.rel[1-9][0-9]*$ ]]; then
    echo "ERROR: ARM_GCC_RELEASE must match N.N.relN in ${dockerfile}." >&2
    exit 1
  fi
  readonly arm_version="${arm_release%.rel*}"
  if [[ -z "${HOME:-}" ]] || [[ "$HOME" != /* ]] || [[ "$HOME" == "/" ]]; then
    echo "ERROR: refusing unsafe HOME for Arm toolchain installation." >&2
    exit 1
  fi
  home_root="$(cd "$HOME" && pwd -P)" || exit 1
  readonly home_root
  if [[ -z "$home_root" ]]; then
    echo "ERROR: HOME cannot be physically canonicalized." >&2
    exit 1
  fi
  readonly arm_root="${home_root}/opt"
  arm_prefix="$(canonical_cleanup_target "${arm_root}/arm-gnu-toolchain-${arm_version}" "$arm_root")" || exit 1
  readonly arm_prefix
  if [[ -z "$arm_prefix" ]]; then
    echo "ERROR: Arm toolchain prefix cannot be safely canonicalized." >&2
    exit 1
  fi
  require_arm_hash_pins

  arm_toolchain_asset() {
    case "$1" in
      arm64)
        printf '%s\t%s\n' "darwin-arm64" "ARM_GCC_SHA256_DARWIN_ARM64"
        ;;
      x86_64)
        printf '%s\t%s\n' "darwin-x86_64" "ARM_GCC_SHA256_DARWIN_X86_64"
        ;;
      *)
        echo "ERROR: unsupported macOS architecture $1 for Arm GNU Toolchain." >&2
        return 1
        ;;
    esac
  }
  install_homebrew() (
    local installer
    work_candidate="$(mktemp -d)" || return 1
    readonly work_candidate
    if [[ -z "$work_candidate" ]]; then
      echo "ERROR: Homebrew work directory creation failed." >&2
      return 1
    fi
    work_root="$(cd "$(dirname -- "$work_candidate")" && pwd -P)" || return 1
    readonly work_root
    if [[ -z "$work_root" ]]; then
      echo "ERROR: Homebrew work root cannot be physically canonicalized." >&2
      return 1
    fi
    work="$(canonical_cleanup_target "$work_candidate" "$work_root")" || return 1
    readonly work
    [[ -n "$work" ]] || return 1
    trap 'safe_remove_tree "${work}" "${work_root}"' EXIT
    installer="${work}/install.sh"
    curl --proto '=https' --proto-redir '=https' --tlsv1.2 -fsSL -o "${installer}" \
      "https://raw.githubusercontent.com/Homebrew/install/${homebrew_installer_commit}/install.sh"
    printf '%s  %s\n' "${homebrew_installer_sha256}" "${installer}" | shasum -a 256 -c -
    /bin/bash -p "${installer}"
  )

  if ! xcode-select -p >/dev/null 2>&1; then
    echo "[emu-setup] requesting Xcode Command Line Tools (the full Xcode app is not required) ..." >&2
    if ! xcode-select --install >/dev/null 2>&1; then
      echo "[emu-setup] Command Line Tools request was already active or could not be opened" >&2
    fi
    echo "ERROR: finish the Command Line Tools installer, then rerun: just apps::emulator::setup" >&2
    exit 1
  fi
  brew="$(command -v brew 2>/dev/null || true)"
  if [[ -z "$brew" ]]; then
    for candidate in /opt/homebrew/bin/brew /usr/local/bin/brew; do
      if [[ -x "$candidate" ]]; then
        brew="$candidate"
        break
      fi
    done
  fi
  if [[ -z "$brew" ]]; then
    echo "[emu-setup] Homebrew is required; launching its pinned, verified installer ..."
    install_homebrew
    brew="$(command -v brew 2>/dev/null || true)"
    if [[ -z "$brew" ]]; then
      for candidate in /opt/homebrew/bin/brew /usr/local/bin/brew; do
        if [[ -x "$candidate" ]]; then
          brew="$candidate"
          break
        fi
      done
    fi
  fi
  if [[ -z "$brew" ]]; then
    echo "ERROR: Homebrew installation did not provide brew; restart the shell, then rerun just apps::emulator::setup." >&2
    exit 1
  fi
  eval "$("$brew" shellenv)"

  brew_missing=()
  command -v cmake >/dev/null 2>&1 || brew_missing+=(cmake)
  capstone_prefix="$("$brew" --prefix capstone 2>/dev/null || true)"
  [[ -f "$capstone_prefix/include/capstone/capstone.h" ]] || brew_missing+=(capstone)
  if [[ "${#brew_missing[@]}" -gt 0 ]]; then
    echo "[emu-setup] installing Homebrew dependencies: ${brew_missing[*]} ..."
    "$brew" install "${brew_missing[@]}"
  else
    echo "[emu-setup] Homebrew dependencies already installed."
  fi

  arm_gcc="$arm_prefix/bin/arm-none-eabi-gcc"
  if [[ ! -x "$arm_gcc" ]] ||
    [[ "$("$arm_gcc" -dumpfullversion 2>/dev/null || true)" != "${arm_version}".* ]]; then
    IFS=$'\t' read -r arm_asset_arch arm_sha_arg < <(arm_toolchain_asset "$(uname -m)")
    arm_sha256="$(dockerfile_arg "${arm_sha_arg}")"
    readonly arm_asset_arch arm_sha_arg arm_sha256
    if [[ ! "${arm_sha256}" =~ ^[0-9a-f]{64}$ ]]; then
      echo "ERROR: invalid Darwin Arm GNU Toolchain sha256 pin in ${dockerfile}." >&2
      exit 1
    fi
    arm_url="https://developer.arm.com/-/media/Files/downloads/gnu/${arm_release}/binrel/arm-gnu-toolchain-${arm_release}-${arm_asset_arch}-arm-none-eabi.tar.xz"
    arm_work_candidate="$(mktemp -d)" || exit 1
    readonly arm_work_candidate
    if [[ -z "$arm_work_candidate" ]]; then
      echo "ERROR: Arm work directory creation failed." >&2
      exit 1
    fi
    arm_work_root="$(cd "$(dirname -- "$arm_work_candidate")" && pwd -P)" || exit 1
    readonly arm_work_root
    if [[ -z "$arm_work_root" ]]; then
      echo "ERROR: Arm work root cannot be physically canonicalized." >&2
      exit 1
    fi
    arm_work="$(canonical_cleanup_target "$arm_work_candidate" "$arm_work_root")" || exit 1
    readonly arm_work
    [[ -n "$arm_work" ]] || exit 1
    trap 'safe_remove_tree "$arm_work" "$arm_work_root"' EXIT
    echo "[emu-setup] installing Arm GNU Toolchain $arm_release to $arm_prefix ..."
    curl --proto '=https' --proto-redir '=https' --tlsv1.2 -fsSL \
      "$arm_url" -o "$arm_work/arm-gcc.tar.xz"
    echo "$arm_sha256  $arm_work/arm-gcc.tar.xz" | shasum -a 256 -c -
    mkdir -p "$arm_root"
    safe_remove_tree "$arm_prefix" "$arm_root"
    mkdir -p "$arm_prefix"
    tar -xf "$arm_work/arm-gcc.tar.xz" -C "$arm_prefix" --strip-components=1
  fi
  echo "[emu-setup] using $($arm_gcc -dumpfullversion) Arm GNU Toolchain"

  echo "[emu-setup] installing pinned Unicorn to $prefix ..."
  RA8_UNICORN_PREFIX="$prefix" /bin/bash -p "$root/scripts/ci/install_unicorn.sh"

  echo "[emu-setup] configuring ra8_emulator ..."
  emu_root="$(cd "$root/tools/ra8_emulator" && pwd -P)" || exit 1
  readonly emu_root
  if [[ -z "$emu_root" ]]; then
    echo "ERROR: emulator root cannot be physically canonicalized." >&2
    exit 1
  fi
  emu_build="$(canonical_cleanup_target "${emu_root}/build" "$emu_root")" || exit 1
  readonly emu_build
  [[ -n "$emu_build" ]] || exit 1
  # A pre-setup configure may have cached Homebrew's incompatible Unicorn bottle.
  # Reset only this host-tool build so CMake resolves the freshly pinned library.
  safe_remove_tree "$emu_build" "$emu_root"
  cmake -S "$root/tools/ra8_emulator" -B "$emu_build"
  cmake --build "$emu_build" --parallel

  echo "[emu-setup] ready: just apps::emulator::run blink"
else
  [[ "$-" == *p* ]]
fi
