#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# scripts/ci/devcontainer_image.sh -- THE definition of "the local ra8-ci image
# is the one this checkout describes", and the only thing that builds it.
#
# ===========================================================================
# WHY THIS FILE EXISTS
# ===========================================================================
# `just ci` boots a locally-built image tagged ra8-ci:latest. scripts/ci.sh
# used to build it once, when `image inspect` reported it absent, and reuse it
# from then on forever -- so a change to .devcontainer/Dockerfile refreshed the
# image on machines that had never built one, and on no other machine at all.
#
# The shared verification box was in exactly that state: the image it booted
# was built on 2026-07-20 and lacked cmake-format, cmake-lint, yamllint,
# actionlint, hadolint, gcc-14 and g++-14, so toolchain-parity, lint-cmake,
# lint-yaml and lint-devcontainer FAILED inside the container and PASSED
# natively on the same box, on the same commit (#521). That is the most
# expensive shape a failure can take: `just ci` is what CLAUDE.md tells every
# agent to run before a push, and four reds that have nothing to do with the
# change under test are indistinguishable from real ones until each is re-run
# by hand. The honest response costs time; the dishonest one ("that gate is
# always red here") is how a real regression gets waved through.
#
# A forced rebuild refreshed it, but nothing made that happen and nothing
# noticed that it had not. A build input nobody owns is not a build input.
#
# ===========================================================================
# HOW IT CANNOT GO STALE AGAIN
# ===========================================================================
# The image is a pure function of the tightly allowlisted root build context,
# so the image
# RECORDS which context built it, in an OCI label, and a cached image whose
# label disagrees with the working tree is not reused -- it is rebuilt, loudly,
# saying which digest it carried and which the tree wants.
#
# Staleness is therefore detected from the tree the gates are about to run
# against, on every `just ci`, on every machine. It needs no timer, no converge
# and no human, and it works identically on the Mac -- which no Ansible run
# will ever reach -- as on the fleet.
#
# It is deliberately NOT an mtime comparison. Every file in a fresh clone or a
# `just workspace::new` worktree carries today's mtime, so "the image is older than the
# Dockerfile" is true in a brand-new workspace whose image is perfectly
# current. A staleness check that cries wolf on an ordinary day teaches people
# to ignore it, which is the defect this file is written against, one level up.
#
# The dangerous direction is closed by construction rather than by care: an
# absent, misspelt or unreadable label yields an EMPTY digest, which never
# equals a sha256, so a broken read presents as a rebuild -- visible in
# seconds -- and can never present as a false "current".
#
# One consequence worth knowing before it surprises someone: there is a single
# tag, so two agents on one box whose branches carry DIFFERENT .devcontainer
# contexts will each rebuild it out from under the other. That is correct --
# every run gets the image its own tree describes -- and it is rare, because
# only a change to an allowlisted devcontainer input can cause it. Tagging
# per digest instead
# would avoid the churn at the price of an unbounded pile of images and a
# reaper to own it, which is a worse trade on a box that has already lost an
# image to a garbage collector (#484).
#
# ===========================================================================
# WHAT THE DIGEST COVERS
# ===========================================================================
# Every allowlisted root-context input, by path, normalized expected mode, and
# content: the Dockerfile, shell configuration, pyproject, uv.lock, and the
# authenticated uv bootstrap manifest/code. The exact .dockerignore policy is
# validated before hashing, so relaxing Docker's context cannot create an
# unhashed image input. Symlinks and special files fail closed.
#
# ===========================================================================
# USAGE
# ===========================================================================
#   devcontainer_image.sh digest              the working tree's context digest
#   devcontainer_image.sh state               current | stale | absent
#   devcontainer_image.sh ensure              build unless the cache is current
#   devcontainer_image.sh ensure --rebuild    build regardless
#   devcontainer_image.sh --selftest          prove the digest reacts, and that
#                                             the label round-trips
#
# Environment:
#   RA8_CI_IMAGE            image tag to manage      (default ra8-ci:latest)
#   RA8_CONTAINER_RUNTIME   runtime, may carry args  (default: the first of
#                           podman / docker / nerdctl on PATH). scripts/ci.sh
#                           exports the runtime it already resolved, so the two
#                           can never pick different ones.
#   RA8_IMAGE_LOCK_DIR      explicit pre-provisioned managed lock directory.
#                           When unset, discover the canonical dev-box authority
#                           if it exists; otherwise use a private directory below
#                           the caller's XDG cache home (or $HOME/.cache).

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

  ra8_bound_entry="${RA8_SELFTEST_BOUND_ENTRY-}"
  unset -v RA8_SELFTEST_BOUND_ENTRY
  if [[ -n "$ra8_bound_entry" ]]; then
    [[ "${BASH_SOURCE[0]}" =~ ^/proc/self/fd/[0-9]+$ &&
      "$ra8_bound_entry" == /*/devcontainer_image.sh &&
      -f "$ra8_bound_entry" && ! -L "$ra8_bound_entry" &&
      "${BASH_SOURCE[0]}" -ef "$ra8_bound_entry" ]] || {
      printf 'ERROR: descriptor-bound selftest entry authority is unsafe\n' >&2
      exit 1
    }
    SCRIPT_DIR="$(cd -P "$(dirname "$ra8_bound_entry")" && pwd)"
    [[ "$ra8_bound_entry" == "$SCRIPT_DIR/devcontainer_image.sh" ]] || {
      printf 'ERROR: descriptor-bound selftest entry path is not canonical\n' >&2
      exit 1
    }
  else
    SCRIPT_DIR="$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  fi
  unset -v ra8_bound_entry
  REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
  CONTEXT_DIR="$REPO_ROOT"
  DOCKERFILE="$REPO_ROOT/.devcontainer/Dockerfile"

  IMAGE_TAG="${RA8_CI_IMAGE:-ra8-ci:latest}"
  CANONICAL_IMAGE_LOCK_DIR="/var/cache/ra8-devcontainer-image-lock"
  IMAGE_LOCK_FILE=""
  IMAGE_LOCK_GROUP_GID=""
  IMAGE_LOCK_IDENTITY=""
  IMAGE_LOCK_MANAGED=0
  SELFTEST_HELPER_RAW_SHA256="1d84ebe964ea5085a4145db610cb1d37ad61c02ea6d1e3220f8befdb2de1fcd7"
  SELFTEST_BOUND_EXIT_RAW_SHA256="79a1a39638b961ecb2daededdd363f04a7f765991b860a629cfbdc589df8de4d"
  SELFTEST_CASES_RAW_SHA256="82f83d5717186669852c92e6af4ee426b35527053787586f9130029755d9cb96"
  SELFTEST_SIGNAL_RAW_SHA256="37890007bdfe343848b8d42f2f58367e6018b0503b3117df6011ad599754ea21"
  export SELFTEST_SUPERVISOR_RAW_SHA256="27b73473be5078f5e7f894aba36a6b3bd91fbce2338510b3fc5a0a1cb8b55396"
  export SELFTEST_SUPERVISOR_CASES_RAW_SHA256="897a5be60eec486f9f9615fead84db22f8526dba189df305f561bc1c7b5e49e7"
  export SELFTEST_SUPERVISOR_PROCESS_RAW_SHA256="0d6735a43532e39ebcda7d876ba8223062656a2fe944c00b611a6c85f3dd730c"
  export IMAGE_LOCK_RECEIPTS_RAW_SHA256="854cfd1163d3d49eda0b05d8a14c5b32385b4b96e7d08de20b03da5cbd1ee727"
  export IMAGE_LOCK_SELFTEST_RAW_SHA256="560f0d73cc37317d38ef7cd1d3b32a82a78dacde3baa4769a7bc0ef1d42189c6"

  # The OCI label the digest is stored in. Namespaced so it cannot collide with a
  # label the Ubuntu base image sets.
  LABEL_KEY="org.ra8.devcontainer-context"

  # Print a fatal message and exit non-zero. Provisioning problems must be loud:
  # a silent fallback here is what put a 2026-07-20 image under a 2026-07-28 tree.
  die() {
    echo "ERROR: $*" >&2
    exit 1
  }

  # The container runtime as one argv line per word, for `mapfile`. Mirrors
  # scripts/ci.sh exactly, including the "sudo podman" case the verification box
  # needs: rootless podman cannot BUILD the devcontainer inside an unprivileged
  # LXC, because apt's setgroups(2) is denied in the nested user namespace.
  runtime_cmd() {
    local -a cmd
    read -r -a cmd <<<"${RA8_CONTAINER_RUNTIME:-}"
    if [[ "${#cmd[@]}" -eq 0 ]]; then
      local candidate
      for candidate in podman docker nerdctl; do
        if command -v "$candidate" >/dev/null 2>&1; then
          if "$candidate" info >/dev/null 2>&1 || [[ "$candidate" == "docker" && "$(uname -s)" == "Darwin" && -x "$(command -v colima 2>/dev/null)" ]]; then
            cmd=("$candidate")
            break
          fi
        fi
      done
      if [[ "${#cmd[@]}" -eq 0 ]]; then
        for candidate in podman docker nerdctl; do
          if command -v "$candidate" >/dev/null 2>&1; then
            cmd=("$candidate")
            break
          fi
        done
      fi
    fi
    [[ "${#cmd[@]}" -eq 0 ]] || printf '%s\n' "${cmd[@]}"
  }

  # Resolve the runtime into RUNTIME, or die naming what is missing.
  require_runtime() {
    RUNTIME=()
    local _line
    while IFS= read -r _line; do
      [[ -n "$_line" ]] && RUNTIME+=("$_line")
    done < <(runtime_cmd)
    [[ "${#RUNTIME[@]}" -gt 0 ]] ||
      die "no container runtime on PATH (looked for podman, docker, nerdctl)."
  }

  # sha256 of stdin, as bare hex. Linux ships sha256sum and macOS ships shasum;
  # every machine that runs these gates has one of the two.
  sha256_stdin() {
    if command -v sha256sum >/dev/null 2>&1; then
      sha256sum | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then
      shasum -a 256 | cut -d' ' -f1
    else
      die "neither sha256sum nor shasum is on PATH; the build context cannot be digested."
    fi
  }

  expected_dockerignore() {
    cat <<'EOF'
# The devcontainer builds from the repository root so its Python lock and uv
# bootstrap are real Docker inputs. Keep the sent context narrowly allowlisted.
*
!.dockerignore
!pyproject.toml
!uv.lock
!.devcontainer/
!.devcontainer/**
!scripts/
!scripts/dev/
!scripts/dev/bootstrap_uv.py
!scripts/dev/bootstrap_uv_exec.py
!scripts/dev/managed_python_env.py
!scripts/dev/managed_python_env_checks.py
!scripts/dev/uv_release.json
EOF
  }

  file_mode() {
    if stat -c '%a' "$1" >/dev/null 2>&1; then
      stat -c '%a' "$1"
    else
      stat -f '%Lp' "$1"
    fi
  }

  file_link_count() {
    if stat -c '%h' "$1" >/dev/null 2>&1; then
      stat -c '%h' "$1"
    else
      stat -f '%l' "$1"
    fi
  }

  file_owner_id() {
    if stat -c '%u' "$1" >/dev/null 2>&1; then
      stat -c '%u' "$1"
    else
      stat -f '%u' "$1"
    fi
  }

  file_group_id() {
    if stat -c '%g' "$1" >/dev/null 2>&1; then
      stat -c '%g' "$1"
    else
      stat -f '%g' "$1"
    fi
  }

  select_selftest_group_id() {
    local scratch="$1" candidate="${2:-1}" owner group
    local probe="$scratch/nonroot-group-probe"
    [[ "$candidate" =~ ^[0-9]+$ && "$candidate" != "0" ]] || return 1
    : >"$probe" || return 1
    if ! chown "0:$candidate" "$probe" 2>/dev/null; then
      rm -f "$probe"
      return 1
    fi
    owner="$(file_owner_id "$probe")"
    group="$(file_group_id "$probe")"
    rm -f "$probe"
    [[ "$owner" == "0" && "$group" == "$candidate" ]] || return 1
    printf '%s\n' "$candidate"
  }

  file_identity() {
    if stat -c '%d:%i' "$1" >/dev/null 2>&1; then
      stat -c '%d:%i' "$1"
    else
      stat -f '%d:%i' "$1"
    fi
  }

  file_special_mode() {
    local path="$1" raw value
    if stat -c '%a' "$path" >/dev/null 2>&1; then
      stat -c '%a' "$path"
      return
    fi
    raw="$(stat -f '%p' "$path")" || return 1
    [[ "$raw" =~ ^[0-7]+$ ]] || return 1
    value=$((8#$raw & 4095))
    printf '%o\n' "$value"
  }

  fd_identity() {
    local fd_path="/proc/self/fd/$1"
    [[ -e "$fd_path" ]] || fd_path="/dev/fd/$1"
    if stat -Lc '%d:%i' "$fd_path" >/dev/null 2>&1; then
      stat -Lc '%d:%i' "$fd_path"
    else
      stat -f '%d:%i' "$fd_path"
    fi
  }

  fd_size() {
    local fd_path="/proc/self/fd/$1"
    [[ -e "$fd_path" ]] || fd_path="/dev/fd/$1"
    if stat -Lc '%s' "$fd_path" >/dev/null 2>&1; then
      stat -Lc '%s' "$fd_path"
    else
      stat -f '%z' "$fd_path"
    fi
  }

  validate_managed_image_lock_group_marker() {
    local marker="$1" mode owner group links identity opened current size marker_gid extra
    [[ -e "$marker" || -L "$marker" ]] || die "managed image lock group marker is missing"
    [[ ! -L "$marker" && -f "$marker" ]] ||
      die "managed image lock group marker must be a non-symlink regular file"
    links="$(file_link_count "$marker")"
    mode="$(file_mode "$marker")"
    owner="$(file_owner_id "$marker")"
    group="$(file_group_id "$marker")"
    [[ "$links" == "1" && "$owner" == "0" && "$group" == "0" && "$mode" == "444" ]] ||
      die "managed image lock group marker must be root:root mode 0444 with one link"
    identity="$(file_identity "$marker")"
    exec 7<"$marker" || die "cannot open the managed image lock group marker"
    opened="$(fd_identity 7)"
    current="$(file_identity "$marker")"
    [[ "$opened" == "$identity" && "$current" == "$opened" ]] ||
      die "managed image lock group marker changed while opening it"
    size="$(fd_size 7)"
    [[ "$size" =~ ^[0-9]+$ ]] || die "managed image lock group marker has no exact size"
    IFS= read -r marker_gid <&7 || die "managed image lock group marker lacks its newline"
    ((size == ${#marker_gid} + 1)) ||
      die "managed image lock group marker must contain exactly one numeric gid line"
    if IFS= read -r -n 1 extra <&7; then
      printf -v extra '%q' "$extra"
      die "managed image lock group marker has extra content: $extra"
    fi
    exec 7<&-
    [[ "$marker_gid" =~ ^[0-9]+$ && "$marker_gid" != "0" ]] ||
      die "managed image lock group marker must contain one non-root numeric gid"
    IMAGE_LOCK_GROUP_GID="$marker_gid"
  }

  validate_managed_image_lock_dir() {
    local lock_dir="$1" mode owner group marker
    [[ "$lock_dir" == /* ]] || die "managed image lock directory must be absolute"
    [[ -e "$lock_dir" || -L "$lock_dir" ]] ||
      die "managed image lock directory is missing: $lock_dir"
    [[ ! -L "$lock_dir" && -d "$lock_dir" && -x "$lock_dir" ]] ||
      die "managed image lock path must be a searchable non-symlink directory: $lock_dir"
    mode="$(file_mode "$lock_dir")"
    owner="$(file_owner_id "$lock_dir")"
    [[ "$owner" == "0" && "$mode" == "750" ]] ||
      die "managed image lock directory must be uid 0 mode 0750: $lock_dir ($owner:$mode)"
    marker="$lock_dir/devcontainer-image.gid"
    validate_managed_image_lock_group_marker "$marker"
    group="$(file_group_id "$lock_dir")"
    [[ "$group" == "$IMAGE_LOCK_GROUP_GID" ]] ||
      die "managed image lock directory gid does not match its marker: $group"
  }

  validate_image_lock_file() {
    local lock_file="$1" managed="$2" mode links owner group expected_owner expected_mode
    [[ -e "$lock_file" || -L "$lock_file" ]] ||
      die "image lock is missing: $lock_file"
    [[ ! -L "$lock_file" && -f "$lock_file" ]] ||
      die "image lock must be a non-symlink regular file: $lock_file"
    links="$(file_link_count "$lock_file")"
    [[ "$links" == "1" ]] ||
      die "image lock must have exactly one link: $lock_file ($links)"
    mode="$(file_mode "$lock_file")"
    owner="$(file_owner_id "$lock_file")"
    if [[ "$managed" == "1" ]]; then
      expected_owner=0
      expected_mode=660
      group="$(file_group_id "$lock_file")"
      [[ "$group" == "$IMAGE_LOCK_GROUP_GID" ]] ||
        die "managed image lock gid does not match its marker: $group"
    else
      expected_owner="$(id -u)"
      expected_mode=600
    fi
    [[ "$owner" == "$expected_owner" && "$mode" == "$expected_mode" ]] ||
      die "image lock must be uid $expected_owner mode 0$expected_mode: $lock_file ($owner:$mode)"
    [[ -w "$lock_file" ]] || die "image lock is not writable: $lock_file"
  }

  private_image_lock_dir() {
    if [[ -n "${XDG_CACHE_HOME:-}" ]]; then
      printf '%s/ra8-firmware/image-lock\n' "$XDG_CACHE_HOME"
    else
      [[ -n "${HOME:-}" ]] || die "HOME is required for the private image lock"
      printf '%s/.cache/ra8-firmware/image-lock\n' "$HOME"
    fi
  }

  # Resolve once; canonical state wins without profile environment. The optional
  # path lets the selftest exercise discovery under a temporary root authority.
  resolve_image_lock() {
    local canonical_dir="${1:-$CANONICAL_IMAGE_LOCK_DIR}" lock_dir mode owner
    IMAGE_LOCK_MANAGED=0
    if [[ -n "${RA8_IMAGE_LOCK_DIR:-}" ]]; then
      lock_dir="$RA8_IMAGE_LOCK_DIR"
      IMAGE_LOCK_MANAGED=1
    elif [[ -e "$canonical_dir" || -L "$canonical_dir" ||
      -e "$canonical_dir/devcontainer-image.lock" ||
      -L "$canonical_dir/devcontainer-image.lock" ||
      -e "$canonical_dir/devcontainer-image.gid" ||
      -L "$canonical_dir/devcontainer-image.gid" ]]; then
      lock_dir="$canonical_dir"
      IMAGE_LOCK_MANAGED=1
    else
      lock_dir="$(private_image_lock_dir)"
      [[ "$lock_dir" == /* ]] || die "private image lock directory must be absolute"
      if [[ -e "$lock_dir" || -L "$lock_dir" ]]; then
        [[ ! -L "$lock_dir" && -d "$lock_dir" ]] ||
          die "private image lock path must be a non-symlink directory: $lock_dir"
      else
        (umask 077 && mkdir -p -- "$lock_dir") ||
          die "cannot create private image lock directory: $lock_dir"
      fi
      chmod 0700 "$lock_dir" || die "cannot make image lock directory private: $lock_dir"
      mode="$(file_mode "$lock_dir")"
      owner="$(file_owner_id "$lock_dir")"
      [[ "$owner" == "$(id -u)" && "$mode" == "700" ]] ||
        die "private image lock directory must be caller-owned mode 0700: $lock_dir ($owner:$mode)"
    fi

    if [[ "$IMAGE_LOCK_MANAGED" == "1" ]]; then
      validate_managed_image_lock_dir "$lock_dir"
    fi
    IMAGE_LOCK_FILE="$lock_dir/devcontainer-image.lock"
    if [[ "$IMAGE_LOCK_MANAGED" == "0" && ! -e "$IMAGE_LOCK_FILE" &&
      ! -L "$IMAGE_LOCK_FILE" ]]; then
      (umask 077 && : >>"$IMAGE_LOCK_FILE") ||
        die "cannot create private image lock: $IMAGE_LOCK_FILE"
    fi
    validate_image_lock_file "$IMAGE_LOCK_FILE" "$IMAGE_LOCK_MANAGED"
    IMAGE_LOCK_IDENTITY="$(file_identity "$IMAGE_LOCK_FILE")"
  }

  managed_image_lock_preflight() {
    [[ "$IMAGE_LOCK_MANAGED" == "1" ]] || return
    command -v flock >/dev/null 2>&1 ||
      die "flock is required for the managed image lock"
  }

  validate_opened_image_lock() {
    local fd="$1" opened current
    validate_image_lock_file "$IMAGE_LOCK_FILE" "$IMAGE_LOCK_MANAGED"
    opened="$(fd_identity "$fd")"
    current="$(file_identity "$IMAGE_LOCK_FILE")"
    [[ "$opened" == "$IMAGE_LOCK_IDENTITY" && "$current" == "$opened" ]] ||
      die "image lock changed between validation and open: $IMAGE_LOCK_FILE"
  }

  canonical_root_context_inputs() {
    cat <<'EOF'
644 .dockerignore
644 pyproject.toml
644 uv.lock
755 scripts/dev/bootstrap_uv.py
644 scripts/dev/bootstrap_uv_exec.py
755 scripts/dev/managed_python_env.py
755 scripts/dev/managed_python_env_checks.py
644 scripts/dev/uv_release.json
EOF
  }

  validate_context_policy() {
    local dir="$1" actual expected special file mode
    actual="$(cat "$dir/.dockerignore")"
    expected="$(expected_dockerignore)"
    [[ "$actual" == "$expected" ]] ||
      die ".dockerignore differs from the canonical hashed context allowlist"
    special="$(find "$dir/.devcontainer" -mindepth 1 ! -type f ! -type d -print -quit)"
    [[ -z "$special" ]] || die "special or symlinked devcontainer input is forbidden: $special"
    while read -r expected_mode file; do
      [[ -f "$dir/$file" && ! -L "$dir/$file" ]] ||
        die "required regular build-context input is missing: $file"
      mode="$(file_mode "$dir/$file")"
      [[ "$mode" == "$expected_mode" ]] ||
        die "root context input must have mode $expected_mode: $file ($mode)"
    done < <(canonical_root_context_inputs)
    while IFS= read -r -d '' file; do
      mode="$(file_mode "$file")"
      [[ "$mode" == "644" ]] || die "devcontainer input must have mode 0644: $file ($mode)"
    done < <(find "$dir/.devcontainer" -type f -print0)
  }

  # Digest exactly the allowlisted root-context inputs Docker receives. A raw
  # repository-root walk would include hundreds of megabytes that .dockerignore
  # excludes and would rebuild the image for unrelated firmware edits.
  #
  # Args: $1 repository-shaped context directory (default: this checkout).
  context_digest() {
    local dir="${1:-$CONTEXT_DIR}" file manifest
    local -a files=()
    while read -r _mode file; do
      files+=("$file")
    done < <(canonical_root_context_inputs)
    [[ -f "$dir/.devcontainer/Dockerfile" ]] ||
      die "no devcontainer Dockerfile under build context $dir."
    validate_context_policy "$dir"
    while IFS= read -r -d '' file; do
      files+=("${file#"$dir/"}")
    done < <(find "$dir/.devcontainer" -type f -print0 | LC_ALL=C sort -z)
    manifest="$(
      printf '%s\n' "${files[@]}" | LC_ALL=C sort -u | while IFS= read -r file; do
        [[ -f "$dir/$file" ]] || die "required build-context input is missing: $file"
        printf '%s  %s  %s\n' \
          "$(sha256_stdin <"$dir/$file")" "$(file_mode "$dir/$file")" "$file"
      done
    )"
    [[ -n "$manifest" ]] || die "the devcontainer build context is empty."
    printf '%s\n' "$manifest" | sha256_stdin
  }

  # The digest recorded in an image, or nothing when the image is absent or
  # predates this labelling. Both label locations are consulted: docker reports
  # .Config.Labels and podman reports both. An image built before #521 reports
  # neither, which is a stale image rather than an error.
  #
  # Args: $1 image reference (default: the managed tag)
  image_digest() {
    local image="${1:-$IMAGE_TAG}"
    require_runtime
    "${RUNTIME[@]}" image inspect "$image" >/dev/null 2>&1 || return 0
    local value template
    for template in '{{ index .Config.Labels "'"$LABEL_KEY"'" }}' \
      '{{ index .Labels "'"$LABEL_KEY"'" }}'; do
      value="$("${RUNTIME[@]}" image inspect --format "$template" "$image" 2>/dev/null || true)"
      # A Go template prints "<no value>" for a missing key on some runtimes and
      # an empty string on others. Neither is a digest, and only a digest counts.
      if [[ "$value" =~ ^[0-9a-f]{64}$ ]]; then
        printf '%s\n' "$value"
        return 0
      fi
    done
  }

  # current | stale | absent, for the managed image against the working tree.
  #
  # The tree's digest is taken into a variable FIRST, deliberately. Compared
  # inline inside [[ ]], a context_digest that died -- no checkout, no
  # .devcontainer -- would contribute an empty string that an unlabelled image's
  # equally empty digest would MATCH, reporting "current" for a comparison that
  # never happened. As an assignment, `set -e` takes the run down instead, which
  # is the only answer a broken comparison is allowed to give.
  image_state() {
    require_runtime
    local want have
    want="$(context_digest)"
    if ! "${RUNTIME[@]}" image inspect "$IMAGE_TAG" >/dev/null 2>&1; then
      printf 'absent\n'
      return 0
    fi
    have="$(image_digest)"
    if [[ -n "$want" && "$have" == "$want" ]]; then
      printf 'current\n'
    else
      printf 'stale\n'
    fi
  }

  # Build the image and stamp the context digest onto it. The label is applied by
  # the build that produced it, so the image and its digest cannot be set apart.
  build_image() {
    local want="$1"
    require_runtime
    echo "==> building $IMAGE_TAG from the allowlisted repository context (runtime: ${RUNTIME[*]})"
    echo "    context digest $want"
    "${RUNTIME[@]}" build \
      --label "$LABEL_KEY=$want" \
      -t "$IMAGE_TAG" \
      -f "$DOCKERFILE" \
      "$CONTEXT_DIR"
  }

  # Say why a build is about to happen, in terms the operator can act on.
  announce() {
    local state="$1" want="$2" have="$3"
    if [[ "$state" == "absent" ]]; then
      echo "==> $IMAGE_TAG is not present on this machine; building it."
      return
    fi
    echo "==> cached $IMAGE_TAG was built from a DIFFERENT locked root context."
    echo "    image: ${have:-(no context label -- built before #521 recorded one)}"
    echo "    tree:  $want"
    echo "    Rebuilding. A cached image that predates the Dockerfile is how four"
    echo "    gates came to fail in the container and pass natively on one box (#521)."
  }

  # Build under an exclusive lock where one is available.
  #
  # Not tuning. The verification box is shared: several agents run `just ci` at
  # once, and the first run after a Dockerfile change would otherwise start one
  # full apt-heavy build PER agent, simultaneously. Load on that box has already
  # been measured turning gates red on its own, so a stampede here would
  # manufacture exactly the false failures this file exists to remove. Waiters
  # re-check under the lock, so all but one find a current image and build
  # nothing.
  #
  # flock(1) is Linux-only (macOS ships no equivalent), so the Mac path builds
  # unlocked -- which is correct there, where one developer runs one suite.
  build_locked() {
    local want="$1" state="$2" have="$3" force="$4"
    if ! command -v flock >/dev/null 2>&1; then
      [[ "$IMAGE_LOCK_MANAGED" == "0" ]] ||
        die "flock is required for the managed image lock"
      if [[ "$force" == "1" ]]; then
        echo "==> --rebuild / REBUILD=1: rebuilding $IMAGE_TAG unconditionally."
      else
        announce "$state" "$want" "$have"
      fi
      build_image "$want"
      return
    fi
    (
      exec 9<"$IMAGE_LOCK_FILE" || die "cannot open existing image lock: $IMAGE_LOCK_FILE"
      validate_opened_image_lock 9
      if ! flock -n 9; then
        echo "==> another run on this machine is building $IMAGE_TAG; waiting for it."
        flock 9
      fi
      validate_opened_image_lock 9
      if [[ "$force" == "1" ]]; then
        echo "==> --rebuild / REBUILD=1: rebuilding $IMAGE_TAG unconditionally under the image lock."
      elif [[ "$(image_state)" == "current" ]]; then
        echo "==> $IMAGE_TAG was rebuilt by that run and is current; not building it again."
        exit 0
      else
        announce "$state" "$want" "$(image_digest)"
      fi
      build_image "$want"
    )
  }

  cmd_ensure() {
    local force="${1:-}"
    local want state have
    resolve_image_lock
    managed_image_lock_preflight
    want="$(context_digest)"
    if [[ "$force" == "--rebuild" ]]; then
      build_locked "$want" forced "" 1
      return
    fi
    state="$(image_state)"
    if [[ "$state" == "current" ]]; then
      echo "==> reusing cached $IMAGE_TAG (context digest $want; --rebuild to refresh)"
      return
    fi
    have="$(image_digest)"
    build_locked "$want" "$state" "$have" 0
  }

  # Build a throwaway one-instruction image carrying `$2` as the context label
  # (or no label at all when `$2` is empty), and print what image_digest reads
  # back out of it.
  descendant_startup_selftest() {
    local tmp="$1" marker="$1/descendant-startup.marker"
    local startup="$1/descendant-startup.sh" raw_function control protected
    printf 'printf "startup\\n" >> %q\n' "$marker" >"$startup"

    BASH_ENV="$startup" /bin/bash -c ':'
    [[ -s "$marker" ]] || die "selftest: descendant BASH_ENV control did not fire"
    rm -f "$marker"

    BASH_ENV="$startup" /bin/bash -p \
      "$SCRIPT_DIR/devcontainer_image.sh" --selftest-descendant >/dev/null
    [[ ! -e "$marker" ]] || die "selftest: privileged entry leaked BASH_ENV to descendant Bash"

    raw_function='BASH_FUNC_probe%%=() { printf imported; }'
    control="$(/usr/bin/env "$raw_function" /bin/bash -c \
      'if declare -F probe >/dev/null; then printf "child=1\n"; else printf "child=0\n"; fi')"
    [[ "$control" == child=1 ]] || die "selftest: raw function control did not import"
    protected="$(/usr/bin/env "$raw_function" /bin/bash -p \
      "$SCRIPT_DIR/devcontainer_image.sh" --selftest-descendant)"
    [[ "$protected" == child=0 ]] ||
      die "selftest: privileged entry leaked a raw function to descendant Bash"
  }

  selftest_private_image_lock() {
    local tmp="$1" private contender
    mkdir "$tmp/home"
    HOME="$tmp/home" XDG_CACHE_HOME='' RA8_IMAGE_LOCK_DIR='' \
      resolve_image_lock "$tmp/absent-canonical"
    private="$IMAGE_LOCK_FILE"
    [[ "$private" == "$tmp/home/.cache/ra8-firmware/image-lock/devcontainer-image.lock" ]] ||
      die "selftest: unmanaged image lock is not private to the caller"
    [[ "$(file_mode "${private%/*}")" == "700" && "$(file_mode "$private")" == "600" ]] ||
      die "selftest: unmanaged image lock permissions are not private"
    command -v flock >/dev/null 2>&1 || {
      echo "selftest: private lock metadata OK; contention skipped without flock"
      return
    }
    exec 8<"$private"
    flock -n 8 || die "selftest: could not take the private image lock"
    contender=0
    (
      exec 8>&-
      exec 9<"$private" && flock -n 9
    ) || contender=$?
    [[ "$contender" -ne 0 ]] || die "selftest: a contending image lock passed"
    flock -u 8
    (
      exec 8>&-
      exec 9<"$private" && flock -n 9
    ) ||
      die "selftest: image lock stayed held after its owner released it"
    exec 8>&-
  }

  source_approved_selftest_helper() {
    local helper="$1" expected_digest="$2" resolved_dir digest identity source_path
    local main_path="$SCRIPT_DIR/devcontainer_image.sh" main_owner main_group caller_uid
    resolved_dir="$(cd -P "$(dirname "$helper")" && pwd)" ||
      die "devcontainer image selftest helper directory cannot be resolved"
    main_owner="$(file_owner_id "$main_path")"
    main_group="$(file_group_id "$main_path")"
    caller_uid="$(id -u)"
    [[ "${BASH_SOURCE[0]}" -ef "$main_path" &&
      -f "$main_path" && ! -L "$main_path" && "$(file_link_count "$main_path")" == "1" &&
      "$(file_mode "$main_path")" == "755" &&
      ("$main_owner" == "0" || "$main_owner" == "$caller_uid") &&
      ("$caller_uid" != "0" || ("$main_owner" == "0" && "$main_group" == "0")) ]] ||
      die "devcontainer image selftest entry-point ownership is unsafe"
    [[ "$resolved_dir" == "$SCRIPT_DIR" &&
      ("$helper" == "$SCRIPT_DIR/devcontainer_image_selftest.bash" ||
      "$helper" == "$SCRIPT_DIR/devcontainer_image_bound_exit_selftest.bash" ||
      "$helper" == "$SCRIPT_DIR/devcontainer_image_selftest_cases.bash" ||
      "$helper" == "$SCRIPT_DIR/devcontainer_image_signal_selftest.bash" ||
      "$helper" == "$SCRIPT_DIR/devcontainer_image_lock_receipts.bash" ||
      "$helper" == "$SCRIPT_DIR/devcontainer_image_lock_selftest.bash") &&
      -f "$helper" && ! -L "$helper" && "$(file_link_count "$helper")" == "1" &&
      "$(file_mode "$helper")" == "644" &&
      "$(file_owner_id "$helper")" == "$main_owner" &&
      "$(file_group_id "$helper")" == "$main_group" ]] ||
      die "devcontainer image selftest helper metadata is unsafe"
    identity="$(file_identity "$helper")"
    exec 7<"$helper" || die "devcontainer image selftest helper cannot be opened for hashing"
    exec 8<"$helper" || {
      exec 7<&-
      die "devcontainer image selftest helper cannot be opened for sourcing"
    }
    [[ "$(fd_identity 7)" == "$identity" && "$(fd_identity 8)" == "$identity" ]] || {
      exec 7<&- 8<&-
      die "devcontainer image selftest helper changed while opening"
    }
    digest="$(sha256_stdin <&7)"
    exec 7<&-
    [[ "$digest" == "$expected_digest" ]] ||
      die "devcontainer image selftest helper digest is not approved"
    source_path="/proc/self/fd/8"
    [[ -e "$source_path" ]] || source_path="/dev/fd/8"
    export DEVCONTAINER_SELFTEST_SOURCE_DIR="$SCRIPT_DIR"
    export DEVCONTAINER_SELFTEST_PARENT="$SCRIPT_DIR/devcontainer_image.sh"
    export DEVCONTAINER_SELFTEST_SCRIPT_DIR="$SCRIPT_DIR"
    export DEVCONTAINER_SELFTEST_REPO_ROOT="$REPO_ROOT"
    export DEVCONTAINER_SELFTEST_LABEL_KEY="$LABEL_KEY"
    # The helper is read from the already identity-checked descriptor above;
    # the path is necessarily dynamic across Linux and macOS fd namespaces.
    # shellcheck source=/dev/null
    source "$source_path"
    exec 8<&-
  }

  load_devcontainer_selftest() {
    source_approved_selftest_helper "$SCRIPT_DIR/devcontainer_image_selftest.bash" \
      "$SELFTEST_HELPER_RAW_SHA256"
    source_approved_selftest_helper "$SCRIPT_DIR/devcontainer_image_bound_exit_selftest.bash" \
      "$SELFTEST_BOUND_EXIT_RAW_SHA256"
    source_approved_selftest_helper "$SCRIPT_DIR/devcontainer_image_selftest_cases.bash" \
      "$SELFTEST_CASES_RAW_SHA256"
    source_approved_selftest_helper "$SCRIPT_DIR/devcontainer_image_signal_selftest.bash" \
      "$SELFTEST_SIGNAL_RAW_SHA256"
    declare -F begin_selftest_tmp selftest_root_signal_child \
      selftest_allocation_signal_child selftest_allocation_checkpoint_child \
      selftest_case_signal_child selftest_signal_cleanup selftest_temp_cleanup \
      selftest_runtime_labels run_bound_exit_supervisor selftest_bound_exit_regressions \
      selftest_bound_exit_supervisor_failures selftest_nonroot_cleanup_retry_supervisor \
      selftest_descriptor_bound_entry >/dev/null ||
      die "devcontainer image selftest helper is incomplete"
  }

  usage() {
    cat <<'EOF'
scripts/ci/devcontainer_image.sh -- keep ra8-ci:latest matching the locked root context

  digest              print the working tree's build-context digest
  state               current | stale | absent
  ensure              build the image unless the cached one is current
  ensure --rebuild    build it regardless
  --selftest          prove the digest reacts and the label round-trips

Environment: RA8_CI_IMAGE and RA8_CONTAINER_RUNTIME select the image/runtime.
RA8_IMAGE_LOCK_DIR explicitly selects a managed lock. When unset, the canonical
/var/cache/ra8-devcontainer-image-lock is discovered before private fallback.
EOF
  }

  # Declared before require_runtime resolves it so `set -u` remains safe.
  RUNTIME=()

  load_selftest_for_command() {
    case "$1" in
      --selftest | --selftest-root-signal-child | \
        --selftest-allocation-signal-child | \
        --selftest-allocation-checkpoint-child | \
        --selftest-case-signal-child | \
        --selftest-image-lock-worker | \
        --selftest-image-lock-signal-controller)
        load_devcontainer_selftest
        ;;
    esac
  }

  dispatch_selftest_child() {
    case "$1" in
      --selftest-root-signal-child)
        [[ "$#" == "11" ]] || die "selftest: root signal child arguments are incomplete"
        selftest_root_signal_child "${2:-}" "${3:-}" "${4:-}" "${5:-}" \
          "${6:-}" "${7:-}" "${8:-}" "${9:-}" "${10:-}" "${11:-}"
        ;;
      --selftest-allocation-signal-child)
        [[ "$#" == "9" ]] ||
          die "selftest: allocation kill child requires bound parent and nested-root metadata"
        selftest_allocation_signal_child "${2:-}" "${3:-}" "${4:-}" "${5:-}" "${6:-}" \
          "${7:-}" "${8:-}" "${9:-}"
        ;;
      --selftest-allocation-checkpoint-child)
        [[ "$#" == "8" ]] ||
          die "selftest: allocation checkpoint child requires a phase and bound parent"
        selftest_allocation_checkpoint_child "${2:-}" "${3:-}" "${4:-}" "${5:-}" \
          "${6:-}" "${7:-}" "${8:-}"
        ;;
      --selftest-case-signal-child)
        [[ "$#" == "10" ]] ||
          die "selftest: case signal child requires a signal and bound receipt paths"
        load_image_lock_selftest
        selftest_case_signal_child "${2:-}" "${3:-}" "${4:-}" "${5:-}" \
          "${6:-}" "${7:-}" "${8:-}" "${9:-}" "${10:-}"
        ;;
    esac
  }

  main() {
    load_selftest_for_command "${1:-}"
    case "${1:-}" in
      digest) context_digest ;;
      state) image_state ;;
      ensure) cmd_ensure "${2:-}" ;;
      --selftest)
        SELFTEST_COMMAND_COMPLETE=0
        cmd_selftest
        [[ "$SELFTEST_COMMAND_COMPLETE" == "1" ]] || die "selftest command returned before completion"
        ;;
      --selftest-root-signal-child | --selftest-allocation-signal-child | \
        --selftest-allocation-checkpoint-child | --selftest-case-signal-child)
        dispatch_selftest_child "$@"
        ;;
      --selftest-descendant)
        if /bin/bash -c 'declare -F probe >/dev/null'; then
          printf 'child=1\n'
        else
          printf 'child=0\n'
        fi
        ;;
      --selftest-image-lock-worker | --selftest-image-lock-signal-controller)
        load_image_lock_selftest
        dispatch_image_lock_selftest "$@"
        ;;
      -h | --help) usage ;;
      "") usage ;;
      *) die "unknown command '$1'. Try --help." ;;
    esac
    if [[ "${1:-}" == "--selftest" ]]; then
      SELFTEST_MAIN_COMPLETE=1
    fi
  }

  SELFTEST_MAIN_COMPLETE=0
  main "$@"
  if [[ "${1:-}" == "--selftest" ]]; then
    [[ "$SELFTEST_MAIN_COMPLETE" == "1" ]] || die "selftest main returned before completion"
  fi
else
  [[ "$-" == *p* ]]
fi
