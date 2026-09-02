#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/ci/lib/container.sh -- HOST MODE for scripts/ci.sh: pick a container
# runtime, make sure the toolchain image exists, and re-enter that same script
# inside it.
#
# SOURCED, NEVER EXECUTED. Split out of ci.sh because that file is THE gate
# registry, and choosing a runtime and assembling a `docker run` command line is
# transport, not a definition of what a gate checks. Keeping both in one file
# made the registry harder to read and left it with no room to grow under the
# 1000-line cap.
#
# Everything here takes its inputs as ARGUMENTS rather than reading ci.sh's
# globals. That is not style: a sourced file that reads variables it never
# assigns is a file whose contract is invisible, and shellcheck says so (SC2154).
# The one thing it does reach back for is ci.sh's own helper functions
# (export_tools_cache, run_suite_on_snapshot, ra8_tools_cache_host_dir), which
# are the fallback paths a missing runtime lands on.
#
# ci_host_mode_exec ends in `exec`, so control never returns to the caller. An
# `exec` inside a sourced function replaces the same process it would have from
# ci.sh itself, so moving this out changed nothing about how the container is
# entered.

_RA8_CONTAINER_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck source=scripts/ci/lib/nofile.sh
. "$_RA8_CONTAINER_LIB_DIR/nofile.sh"
unset _RA8_CONTAINER_LIB_DIR

# The container runtime's command words, one per line, or nothing when there is
# none.
#
# podman is preferred (daemonless, and rootless where the kernel allows it, so a
# gate run cannot outlive the user's session); docker via colima is the macOS
# path.
#
# RA8_CONTAINER_RUNTIME may carry ARGUMENTS, e.g. "sudo podman". That is not
# hypothetical: on a verification box that is itself an unprivileged LXC
# container, rootless podman runs containers fine but cannot BUILD the
# devcontainer -- apt drops privileges to the _apt user and its setgroups(2)
# call is denied inside the nested user namespace. Running podman as root inside
# that LXC sidesteps the nested namespace, and root there is still unprivileged
# on the Proxmox host, so the security boundary is unchanged.
ci_runtime_argv() {
  local candidate words=()
  read -r -a words <<<"${RA8_CONTAINER_RUNTIME:-}"
  if [[ "${#words[@]}" -eq 0 ]]; then
    for candidate in podman docker nerdctl; do
      if command -v "$candidate" >/dev/null 2>&1; then
        if "$candidate" info >/dev/null 2>&1 || [[ "$candidate" == "docker" && "$(uname -s)" == "Darwin" && -x "$(command -v colima 2>/dev/null)" ]]; then
          words=("$candidate")
          break
        fi
      fi
    done
    if [[ "${#words[@]}" -eq 0 ]]; then
      for candidate in podman docker nerdctl; do
        if command -v "$candidate" >/dev/null 2>&1; then
          words=("$candidate")
          break
        fi
      done
    fi
  fi
  [[ "${#words[@]}" -gt 0 ]] && printf '%s\n' "${words[@]}"
  return 0
}

# What to do when no container runtime exists. Never returns.
#
# On Linux the native path is faithful -- the runner is Linux too -- so fall
# back rather than refusing to run. On macOS it is not: the format gate needs
# clang-format-22 and the host tests cannot mmap MAP_FIXED below 4 GiB, so a
# macOS "pass" would be a lie. Refuse there.
ci_no_runtime() {
  local fast="$1"
  if [[ "$(uname -s)" == "Linux" ]]; then
    echo "==> no container runtime found; running gates NATIVELY on this Linux host."
    echo "    (Linux native == the CI environment. Install podman for isolation:"
    echo "     sudo apt-get install -y podman uidmap)"
    export_tools_cache
    run_suite_on_snapshot "$fast"
    exit $?
  fi
  echo "error: no container runtime on PATH (looked for podman, docker, nerdctl)." >&2
  echo "  macOS cannot run these gates natively: the format gate pins" >&2
  echo "  clang-format-22, and the host tests need MAP_FIXED below 4 GiB," >&2
  echo "  which macOS arm64 refuses. Install a runtime:" >&2
  echo "    brew install colima docker && colima start" >&2
  exit 1
}

# Fail unless the chosen runtime is present AND usable, starting colima first
# where that is what "usable" means. Never returns on failure.
ci_require_runtime() {
  local runtime=("$@") name
  name="$(basename "${runtime[${#runtime[@]} - 1]}")"
  if ! command -v "${runtime[0]}" >/dev/null 2>&1; then
    echo "error: container runtime '${runtime[0]}' is not on PATH." >&2
    exit 1
  fi
  # On macOS the project uses colima (no Docker Desktop license). Auto-start it,
  # matching scripts/ci/devcontainer_run.sh. podman on macOS uses its own VM instead.
  if [[ "$(uname -s)" == "Darwin" && "$name" == "docker" ]]; then
    if ! command -v colima >/dev/null 2>&1; then
      echo "error: colima not on PATH. Install: brew install colima" >&2
      exit 1
    fi
    if ! colima status >/dev/null 2>&1; then
      echo "==> starting colima VM (4 CPU, 6 GiB)"
      colima start --cpu 4 --memory 6
    fi
  fi
  if "${runtime[@]}" info >/dev/null 2>&1; then
    return 0
  fi
  echo "error: '${runtime[*]}' is installed but not usable." >&2
  if [[ "$name" == "docker" ]]; then
    echo "  docker daemon not reachable (try: colima start)" >&2
  else
    echo "  check '${runtime[*]} info' output for the underlying failure." >&2
  fi
  exit 1
}

# Bring the devcontainer image up to date with this checkout's build context.
#
# The tiny .devcontainer/ directory is what is shipped to the daemon -- never
# the multi-GB repo, with its datasheets and build trees.
#
# The decision is NOT "is an image present". It was, and that is why the shared
# verification box booted a 2026-07-20 image under a 2026-07-28 tree and
# reported four gates red that pass natively on the same box, on the same
# commit (#521). devcontainer_image.sh compares the build-context digest
# recorded on the image against the working tree's, so an image built from a
# different Dockerfile is rebuilt rather than reused. That is one definition,
# shared with the dev_box Ansible role, so a converge and a `just ci` cannot
# disagree about what "current" means.
ci_ensure_image() {
  local rebuild="$1" image="$2" repo="$3"
  shift 3
  local runtime=("$@")
  local args=(ensure)
  [[ "$rebuild" == "1" ]] && args+=(--rebuild)
  RA8_CONTAINER_RUNTIME="${runtime[*]}" \
    RA8_CI_IMAGE="$image" \
    RA8_TOOLS_CACHE_DIR="${RA8_TOOLS_CACHE_DIR:-/var/cache/ra8-tools}" \
    /bin/bash -p "$repo/scripts/ci/devcontainer_image.sh" "${args[@]}"
}

# Linked git worktrees: mount the main repo's git dir too (#334).
#
# In a linked worktree -- an agent workspace from `just workspace::new`, or a
# .claude/worktrees/* tree -- `.git` is a FILE holding "gitdir: <path>" that
# points into the MAIN repo's git directory. That directory is outside the
# workspace bind mount, so the in-container `git archive HEAD` saw no repository
# at all and the suite died before its first gate with "fatal: not a git
# repository" followed by "tar: This does not look like a tar archive".
#
# That made the isolation pattern agents are told to use (one workspace each, so
# concurrent runs stop clobbering one another) incompatible with the only
# toolchain-correct way to run the gates, and pushed agents onto native runs
# where clang-tidy / gcovr / shellcheck / shfmt all differ from CI (#333).
#
# Mounting the common git dir at the SAME absolute path it has on the host is
# what makes the pointer resolve identically inside and outside. It stays
# read-only like the worktree itself: `git archive` only reads.
ci_worktree_run_args() {
  local repo="$1" common
  common="$(cd "$repo" &&
    cd "$(git rev-parse --git-common-dir 2>/dev/null || echo .git)" 2>/dev/null &&
    pwd || true)"
  case "${common:-}" in
    # An ordinary checkout: the git dir is already inside the mount.
    "" | "$repo"/*) return 0 ;;
  esac
  echo "==> linked worktree detected; also mounting $common (read-only)" >&2
  printf '%s\n' -v "$common:$common:ro" -e "RA8_CI_GIT_COMMON_DIR=$common"
}

# Persistent compiler cache for the containerised path.
#
# Without this the cache is pointless here: HOME is /tmp inside the container
# and --rm takes it away, so every run would start cold and cmake/ccache.cmake
# would be wiring in a launcher whose cache never survives. A host directory
# mounted read-write is what lets a second `just ci` -- by any agent, from any
# workspace -- hit objects the first one compiled. It cannot carry stale state
# forward: a changed input is simply a different cache key.
#
# CCACHE_BASEDIR + CCACHE_NOHASHDIR are load-bearing, not tuning. Each run
# builds in a fresh mktemp snapshot, so without path normalisation every
# compilation hashes differently and the hit rate is flat zero.
#
# Absent or unwritable cache dir: run without it rather than fail. A cache is an
# optimisation, and a missing one must never turn a gate red.
ci_ccache_run_args() {
  local dir="${RA8_CCACHE_DIR:-/var/cache/ccache-ra8}"
  [[ -d "$dir" && -w "$dir" ]] || return 0
  echo "==> compiler cache: $dir -> /ccache" >&2
  printf '%s\n' \
    -v "$dir:/ccache" \
    -e CCACHE_DIR=/ccache \
    -e CCACHE_BASEDIR=/ \
    -e CCACHE_NOHASHDIR=1 \
    -e "CCACHE_SLOPPINESS=include_file_mtime,include_file_ctime,locale,time_macros" \
    -e "CCACHE_MAXSIZE=${RA8_CCACHE_MAXSIZE:-20G}"
}

# Persistent PINNED-TOOL cache for the containerised path (#326).
#
# The docs gate builds with a version-pinned doxygen that
# scripts/builders/provision_doxygen.sh downloads + sha256-verifies on first
# use. Every gate run builds in a fresh mktemp snapshot whose build/tools/ is
# destroyed on exit (trap rm -rf), so without a persistent mount that download
# repeats every run and FAILS outright with no network. This mount is to pinned
# tools what the ccache mount is to compiled objects: one host directory, reused
# across runs and shared across agents. Best-effort create; absent or
# unwritable, run without it rather than fail.
ci_toolcache_run_args() {
  local dir
  dir="$(ra8_tools_cache_host_dir)"
  if ! mkdir -p "$dir" 2>/dev/null; then
    return 0
  fi
  [[ -d "$dir" && -w "$dir" ]] || return 0
  echo "==> pinned-tool cache: $dir -> /toolcache" >&2
  printf '%s\n' -v "$dir:/toolcache" -e RA8_TOOLS_CACHE=/toolcache
}

# Extra container-run flags, for a host that is ALSO something else. Empty by
# default, so this changes nothing where it is not set.
#
# One measured case: win-ci is a CI RUNNER host first and a verification host
# second, so a `just ci` there must be weighted BELOW its runner containers
# rather than compete with them. It has to be a run-command flag --
# `--cgroup-parent=<slice>` -- because the container's cgroup is created by the
# docker DAEMON, so it inherits nothing from the slice this shell is in. The
# dev_slice Ansible role exports it in /etc/profile.d there; nothing else does.
ci_extra_run_args() {
  local words=()
  read -r -a words <<<"${RA8_CI_CONTAINER_ARGS:-}"
  [[ "${#words[@]}" -eq 0 ]] && return 0
  echo "==> extra container args: ${words[*]} (RA8_CI_CONTAINER_ARGS)" >&2
  printf '%s\n' "${words[@]}"
}

# HOST MODE, end to end. Never returns.
#
# The host repo is bind-mounted READ-ONLY: the in-container step materialises
# a clean copy of committed HEAD into a throwaway dir and builds there, so the
# host source tree and its macOS CMake caches are never touched. A gate that
# writes under the mount instead of the tree it is standing in therefore fails
# outright, which is exactly what tools-build did (#546). It runs as root so
# that throwaway tree (and its fresh build dirs) is writable.
#
# --rm is load-bearing, not hygiene: the snapshot tree and every build dir the
# gates create live INSIDE the container, so the container exiting is what
# reclaims them. Nothing a gate writes can survive to poison the next run.
# --init is equally load-bearing for signal tests: the gate process must not be
# PID 1, which otherwise leaves killed orphan grandchildren as zombies and
# makes an empty process-group assertion impossible inside the container.
#
# CMAKE_BUILD_PARALLEL_LEVEL defaults to 4 (the CI runner's value, so this
# reproduces its timing) but is overridable for a beefier verification box.
# RA8_MAX_JOBS is forwarded when set so an operator can cap every in-container
# parallel width (#328) below the CMAKE_BUILD_PARALLEL_LEVEL budget; unset on
# the host, `-e RA8_MAX_JOBS` passes nothing and ra8_max_jobs falls through.
ci_host_mode_exec() {
  local fast="$1" gate="$2" rebuild="$3" image="$4" repo="$5"
  local runtime=() extra=() worktree=() ccache=() toolcache=() nofile=()
  local _line
  while IFS= read -r _line; do [[ -n "$_line" ]] && runtime+=("$_line"); done < <(ci_runtime_argv)
  [[ "${#runtime[@]}" -eq 0 ]] && ci_no_runtime "$fast"
  ci_require_runtime "${runtime[@]}"
  ci_ensure_image "$rebuild" "$image" "$repo" "${runtime[@]}"

  while IFS= read -r _line; do [[ -n "$_line" ]] && worktree+=("$_line"); done < <(ci_worktree_run_args "$repo")
  while IFS= read -r _line; do [[ -n "$_line" ]] && ccache+=("$_line"); done < <(ci_ccache_run_args)
  while IFS= read -r _line; do [[ -n "$_line" ]] && toolcache+=("$_line"); done < <(ci_toolcache_run_args)
  while IFS= read -r _line; do [[ -n "$_line" ]] && extra+=("$_line"); done < <(ci_extra_run_args)
  ra8_nofile_validate_target "$RA8_NOFILE_TARGET"
  while IFS= read -r _line; do [[ -n "$_line" ]] && nofile+=("$_line"); done < <(
    ra8_nofile_container_run_args "$image" "${runtime[@]}"
  )

  echo "==> running ${gate:-CI gates} in container (runtime=${runtime[*]} fast=$fast)"
  # Host mode deliberately runs as root with an ephemeral private HOME, so the
  # image's developer-user Git config does not apply.  Bind the one read-only
  # mount as safe through Git's fixed process environment; otherwise Git
  # rejects the host checkout before the snapshot can be materialised.
  exec "${runtime[@]}" run --rm \
    --init \
    -u 0:0 \
    --tmpfs /home/ra8-ci:rw,mode=0700 \
    -e RA8_CI_INNER=1 \
    -e RA8_CI_FAST="$fast" \
    -e RA8_CI_GATE="$gate" \
    -e HOME=/home/ra8-ci \
    -e GIT_CONFIG_COUNT=1 \
    -e GIT_CONFIG_KEY_0=safe.directory \
    -e GIT_CONFIG_VALUE_0=/workspace \
    -e RA8_MAX_JOBS \
    -e CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-${RA8_MAX_JOBS:-}}" \
    -v "$repo":/workspace:ro \
    ${extra[@]+"${extra[@]}"} \
    ${worktree[@]+"${worktree[@]}"} \
    ${ccache[@]+"${ccache[@]}"} \
    ${toolcache[@]+"${toolcache[@]}"} \
    "${nofile[@]}" \
    -w /workspace \
    "$image" \
    /bin/bash -p scripts/ci.sh
}
