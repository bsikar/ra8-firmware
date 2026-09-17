#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Run a developer command against the CURRENT working tree in the pinned
# devcontainer. Unlike scripts/ci.sh's suite transport, this mount is writable:
# formatters and focused test builds are expected to update the checkout.
# Image ownership still belongs exclusively to devcontainer_image.sh.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd -P)"
IMAGE_TAG="${RA8_CI_IMAGE:-ra8-ci:latest}"

# shellcheck source=scripts/ci/lib/parallelism.sh
. "$SCRIPT_DIR/lib/parallelism.sh"
# shellcheck source=scripts/ci/lib/container.sh
. "$SCRIPT_DIR/lib/container.sh"

usage() {
  cat <<'EOF'
scripts/ci/devcontainer_run.sh -- run a command in the pinned devcontainer

  devcontainer_run.sh [--rebuild] -- <command> [args...]
  devcontainer_run.sh --selftest

The repository is mounted read-write at /workspace. A TTY is allocated only
when both stdin and stdout are terminals; stdin remains open otherwise.

Environment: RA8_CI_IMAGE, RA8_CONTAINER_RUNTIME, RA8_CI_CONTAINER_ARGS.
EOF
}

die() {
  echo "ERROR: $*" >&2
  exit 1
}

# Print the runtime flags one per line. Kept pure so the selftest can prove the
# interactive and redirected directions without trying to manufacture a PTY.
tty_run_args() {
  local stdin_tty="$1" stdout_tty="$2"
  printf '%s\n' -i
  if [[ "$stdin_tty" == "1" && "$stdout_tty" == "1" ]]; then
    printf '%s\n' -t
  fi
}

assert_lines() {
  local label="$1" expected="$2"
  shift 2
  local got
  got="$("$@")"
  [[ "$got" == "$expected" ]] ||
    die "selftest: $label (expected '$expected', got '$got')"
}

cmd_selftest() {
  assert_lines "interactive input/output allocates stdin and a TTY" $'-i\n-t' \
    tty_run_args 1 1
  assert_lines "redirected output keeps stdin but allocates no TTY" '-i' \
    tty_run_args 1 0
  assert_lines "redirected input keeps stdin but allocates no TTY" '-i' \
    tty_run_args 0 1
  echo "devcontainer_run.sh --selftest: PASS (both TTY directions)"
}

main() {
  local rebuild=0
  case "${1:-}" in
    --selftest)
      cmd_selftest
      return
      ;;
    --rebuild)
      rebuild=1
      shift
      ;;
    -h | --help)
      usage
      return
      ;;
  esac
  [[ "${1:-}" == "--" ]] && shift
  [[ "$#" -gt 0 ]] || die "no command supplied. Try --help."

  local runtime=() extra=() worktree=() tty=() nofile=() line stdin_tty=0 stdout_tty=0
  while IFS= read -r line; do [[ -n "$line" ]] && runtime+=("$line"); done < <(ci_runtime_argv)
  [[ "${#runtime[@]}" -gt 0 ]] ||
    die "no container runtime on PATH (looked for podman, docker, nerdctl)."
  ci_require_runtime "${runtime[@]}"

  RA8_TOOLS_CACHE_DIR="${RA8_TOOLS_CACHE_DIR:-/var/cache/ra8-tools}" \
    ci_ensure_image "$rebuild" "$IMAGE_TAG" "$REPO_ROOT" "${runtime[@]}"

  while IFS= read -r line; do [[ -n "$line" ]] && worktree+=("$line"); done < <(ci_worktree_run_args "$REPO_ROOT")
  while IFS= read -r line; do [[ -n "$line" ]] && extra+=("$line"); done < <(ci_extra_run_args)
  ra8_nofile_validate_target "$RA8_NOFILE_TARGET"
  while IFS= read -r line; do [[ -n "$line" ]] && nofile+=("$line"); done < <(
    ra8_nofile_container_run_args "$IMAGE_TAG" "${runtime[@]}"
  )
  [[ -t 0 ]] && stdin_tty=1
  [[ -t 1 ]] && stdout_tty=1
  while IFS= read -r line; do [[ -n "$line" ]] && tty+=("$line"); done < <(tty_run_args "$stdin_tty" "$stdout_tty")

  echo "==> running in writable $IMAGE_TAG (runtime=${runtime[*]}; tree=$REPO_ROOT)"
  exec "${runtime[@]}" run --rm \
    "${tty[@]}" \
    -e RA8_MAX_JOBS="${RA8_MAX_JOBS:-$(ra8_max_jobs)}" \
    -e CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-${RA8_MAX_JOBS:-$(ra8_max_jobs)}}" \
    -v "$REPO_ROOT:/workspace:rw" \
    ${extra[@]+"${extra[@]}"} \
    ${worktree[@]+"${worktree[@]}"} \
    "${nofile[@]}" \
    -w /workspace \
    "$IMAGE_TAG" \
    "$@"
}

main "$@"
