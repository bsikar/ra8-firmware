#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# scripts/dev/agent_workspace.sh -- the ONE way a concurrent agent gets an isolated
# checkout on a shared verification box.
#
# The problem this replaces:
#   Several agents shared one working tree at ~/ra8-firmware. Each improvised
#   its own isolation, leaving stray checkouts (~/ra8-296, ~/ra8-base,
#   ~/ra8-fmt-work, ~/wt-land296, /home/bsikar/wt-296) scattered across home.
#   Two agents had their checkouts clobbered mid-run, corrupting a baseline
#   measurement and an EIL run. A .gcda left behind by a build of a DIFFERENT
#   branch made gcovr fail with no_working_dir_found -- a bogus coverage
#   failure that reads exactly like a real one, worked around by remembering to
#   `rm -rf tests/build build/tidy` before every coverage run.
#
# The shape of the fix -- three properties, all STRUCTURAL rather than
# remembered:
#
#   1. ISOLATION. One git worktree per agent under $RA8_WS_ROOT. Worktrees
#      share ~/ra8-firmware's object store, so a workspace costs a checkout
#      rather than a full clone, and a branch committed in one workspace is
#      immediately visible to all the others.
#
#   2. EPHEMERAL BUILD OUTPUT. Gates run via `just ci`, which builds inside a
#      `--rm` container against a fresh snapshot of committed HEAD. Build
#      output therefore never lands in the workspace at all and cannot outlive
#      the run that produced it. This is what makes the stale-.gcda class
#      IMPOSSIBLE instead of prevented-by-habit: a coverage run cannot find
#      another branch's .gcda because the tree it builds in did not exist a
#      moment earlier. Do not add a "clean first" step -- if one is ever
#      needed again, the isolation has been broken and that is the bug.
#
#   3. SELF-HEALING LIFECYCLE. `reap` deletes stale workspaces on its own. It
#      runs from a systemd timer AND on every `create`, so the box heals even
#      if the timer is disabled or was never installed. Nothing here depends on
#      a human or an agent remembering to clean up.
#
# The reaper REFUSES to delete anything holding work that exists nowhere else.
# That rule is not theoretical: an audit of this box found nine trees whose
# commits were absent from GitHub while a naive `git log --not --remotes` check
# called them all pushed, because local clone-chains had made stale
# remote-tracking refs look authoritative. See is_reapable() below.
#
# Usage:
#   /bin/bash -p scripts/dev/agent_workspace.sh create <name> [<ref>] [options]
#   /bin/bash -p scripts/dev/agent_workspace.sh release <name>         # give it back
#   /bin/bash -p scripts/dev/agent_workspace.sh forget <name>          # stale metadata only
#   /bin/bash -p scripts/dev/agent_workspace.sh drop-branch <work-name> [<landed-ref>]
#   /bin/bash -p scripts/dev/agent_workspace.sh list                   # what exists
#   /bin/bash -p scripts/dev/agent_workspace.sh reap [--force]         # sweep stale ones
#   /bin/bash -p scripts/dev/agent_workspace.sh doctor                 # environment check
#   /bin/bash -p scripts/dev/agent_workspace.sh install-timer          # automatic reaping
#
# Equivalent recipes: `just workspace::new`, `just workspace::free`,
# `just workspace::list`, and `just workspace::reap`.

# The real body is reachable only after the protected shebang has taken
# effect. An explicit ordinary-Bash caller may already have run arbitrary
# BASH_ENV code, so it fails through shell grammar instead of attempting an
# in-process repair. Raw exported-function rows are removed before any later
# ordinary Bash descendant can import them.
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

  PATH=/usr/bin:/bin:/usr/sbin:/sbin
  export PATH

  RA8_WS_ROOT="${RA8_WS_ROOT:-$HOME/ra8-ws}"
  RA8_WS_UPSTREAM="${RA8_WS_UPSTREAM:-$HOME/ra8-firmware}"
  # A workspace idle longer than this is reapable. Deliberately longer than a
  # working session and shorter than a weekend.
  RA8_WS_TTL_HOURS="${RA8_WS_TTL_HOURS:-24}"
  # Above this disk usage the reaper switches to the aggressive TTL below, so the
  # box responds to actually being full rather than only to the clock.
  RA8_WS_DISK_PCT="${RA8_WS_DISK_PCT:-80}"
  RA8_WS_TTL_HOURS_FULL="${RA8_WS_TTL_HOURS_FULL:-4}"
  RA8_WS_REMOTE_REFS_FRESH=0

  _RA8_WS_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  # Every local Git operation is a nested writer/read against an explicit -C
  # repository. Install the shared strict child policy per call so direct shell
  # entry cannot inherit hook routing or executable global/system policy.
  # shellcheck source=scripts/dev/git_environment.sh
  . "${_RA8_WS_SCRIPT_DIR}/git_environment.sh"
  git() (
    install_sanitized_git_environment
    "$RA8_TRUSTED_GIT" -c core.attributesFile=/dev/null -c core.fsmonitor=false \
      -c core.hooksPath=/dev/null "$@"
  )

  log() { printf '%s\n' "$*" >&2; }
  die() {
    log "error: $*"
    exit 1
  }

  validate_workspace_name() {
    [[ "${1:-}" =~ ^[A-Za-z0-9][A-Za-z0-9_.-]{0,62}$ ]] ||
      die "workspace name must be 1-63 safe characters"
  }

  validate_metadata_value() {
    local value="${1:-}" label="${2:-value}"
    [[ "$value" != *$'\n'* && "$value" != *$'\r'* ]] ||
      die "$label must be a single line"
    printf '%s' "$value" | LC_ALL=C grep -q '^[ -~]*$' ||
      die "$label must contain only printable ASCII"
  }

  workspace_guard() {
    local script_dir
    script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    printf '%s' "$script_dir/workspace_guard.py"
  }

  ensure_lifecycle_lock() {
    if [[ "${RA8_WS_LOCKED:-}" != "1" ]]; then
      exec "$RA8_TRUSTED_PYTHON" -I "$(workspace_guard)" lock "$RA8_WS_ROOT" "$RA8_WS_UPSTREAM" "$0" -- "$@"
    fi
    [[ "${RA8_WS_LOCK_FD:-}" =~ ^[0-9]+$ ]] || die "workspace lock descriptor is absent"
    local lock_path="$RA8_WS_ROOT/.workspace.lock" fd_path="/proc/$$/fd/$RA8_WS_LOCK_FD"
    [[ -e "$fd_path" && -f "$lock_path" && ! -L "$lock_path" ]] ||
      die "workspace lock descriptor/path binding is unsafe"
    [[ "$(stat -Lc '%d:%i' "$fd_path")" == "$(stat -Lc '%d:%i' "$lock_path")" ]] ||
      die "workspace lock descriptor does not own the canonical lock"
    flock -n "$RA8_WS_LOCK_FD" || die "workspace lock descriptor is not exclusively held"
  }

  guard_metadata() {
    "$RA8_TRUSTED_PYTHON" -I "$(workspace_guard)" metadata "$1" "$RA8_WS_ROOT" "$2" "$3"
  }

  # This script DELETES directories, and every one of its safety checks is built
  # on a Linux-only primitive: /proc/<pid>/cwd for liveness, `find -printf` for
  # idle age, `df --output=pcent` for disk pressure. On a platform without them
  # each check degrades to "no evidence of use" -- which is the fail-OPEN
  # direction, i.e. it would make a busy workspace look reapable. Refuse instead.
  # The shared verification box is Linux; there is no reason to run this on the
  # Mac, where `just ci` is the containerised path against a full clone anyway.
  require_linux() {
    [[ "$(uname -s)" == "Linux" ]] ||
      die "agent workspaces are Linux-only (needs /proc, GNU find, GNU df); this is $(uname -s)"
  }

  # --------------------------------------------------------------------------
  # Liveness. A workspace with a live process in it is never touched, no matter
  # how old it looks -- several agents share this box and a mid-flight gate run
  # must not lose its tree underneath it.
  #
  # The cheap, dependency-free test: every process exposes its working directory
  # as /proc/<pid>/cwd. A build running in a tree always has at least one process
  # cwd'd inside it, so this catches an in-flight gate run that a timestamp check
  # would miss (a long link step can leave mtimes hours old while the build is
  # very much alive). Deleting a build directory out from under a running build
  # is precisely the "clobbered mid-run" failure this infrastructure exists to
  # stop, so this check gates every deletion below.
  #
  # The /proc scan is deliberately over ALL pids rather than only the current
  # user's: the box is shared, and a tree busy under another account is just as
  # busy. lsof adds open files (a process that opened a file in the tree but
  # chdir'd elsewhere) and is used when present.
  # --------------------------------------------------------------------------
  tree_is_busy() {
    local tree="$1" p c
    for p in /proc/[0-9]*; do
      c="$(readlink "$p/cwd" 2>/dev/null)" || continue
      case "$c" in "$tree" | "$tree"/*) return 0 ;; esac
    done
    if command -v lsof >/dev/null 2>&1; then
      lsof +D "$tree" >/dev/null 2>&1 && return 0
    fi
    return 1
  }

  # Hours since the newest non-.git file was touched.
  #
  # Fails CLOSED: if `find` produces nothing usable -- an unreadable tree, a
  # platform without -printf -- this reports 0 (just used) rather than a huge
  # age. The alternative rounds every unreadable workspace up to "stale" and
  # deletes it, which is the one outcome this script exists to prevent.
  ws_idle_hours() {
    local ws="$1" newest now
    newest="$(find "$ws" -xdev -type f -not -path '*/.git/*' -printf '%T@\n' 2>/dev/null |
      sort -rn | head -1 | cut -d. -f1)"
    [[ "$newest" =~ ^[0-9]+$ ]] || {
      echo 0
      return 0
    }
    now="$(date +%s)"
    echo $(((now - newest) / 3600))
  }

  # --------------------------------------------------------------------------
  # Safety. Returns 0 only when losing this directory loses NOTHING.
  #
  # `git log --not --remotes` is NOT sufficient on its own and trusting it is how
  # real work nearly went away here: a clone whose origin is another local clone
  # has remote-tracking refs that make purely local commits look published. The
  # authoritative question is whether the upstream repo's GitHub refs contain
  # this HEAD, so ask that repo directly.
  # --------------------------------------------------------------------------
  is_reapable() {
    local ws="$1" ttl="$2"
    [[ "$RA8_WS_REMOTE_REFS_FRESH" == "1" ]] || {
      echo "remote-refresh-failed"
      return 1
    }
    workspace_is_registered_here "$ws" || {
      echo "foreign-or-unregistered-tree"
      return 1
    }
    if tree_is_busy "$ws"; then
      echo "in-use"
      return 1
    fi
    local idle
    idle="$(ws_idle_hours "$ws")"
    if [[ "$idle" -lt "$ttl" ]]; then
      echo "active-${idle}h-of-${ttl}h"
      return 1
    fi
    if ! git -C "$ws" rev-parse --git-dir >/dev/null 2>&1; then
      echo "not-a-git-tree"
      return 1
    fi
    local status
    if ! status="$(git -C "$ws" status --porcelain=v1 --untracked-files=all --ignore-submodules=none 2>/dev/null)"; then
      echo "status-failed"
      return 1
    fi
    if [[ -n "$status" ]]; then
      echo "dirty-or-untracked"
      return 1
    fi
    local head
    head="$(git -C "$ws" rev-parse HEAD 2>/dev/null || true)"
    [[ -z "$head" ]] && {
      echo "no-head"
      return 1
    }
    # Is this commit reachable from a real remote ref in the upstream repo?
    if ! git -C "$RA8_WS_UPSTREAM" cat-file -e "${head}^{commit}" 2>/dev/null; then
      echo "commit-absent-upstream"
      return 1
    fi
    if [[ -z "$(git -C "$RA8_WS_UPSTREAM" branch -r --contains "$head" 2>/dev/null || true)" ]]; then
      echo "commit-not-on-any-remote-branch"
      return 1
    fi
    echo "reapable-idle-${idle}h"
    return 0
  }

  workspace_is_registered_here() {
    local ws="$1" ws_real root_real upstream_common ws_common
    [[ -d "$ws" && ! -L "$ws" ]] || return 1
    ws_real="$(realpath -e -- "$ws" 2>/dev/null)" || return 1
    root_real="$(realpath -e -- "$RA8_WS_ROOT" 2>/dev/null)" || return 1
    [[ "$ws_real" == "$ws" && "$(dirname "$ws")" == "$RA8_WS_ROOT" ]] || return 1
    [[ "$ws_real" == "$root_real"/* ]] || return 1
    git -C "$RA8_WS_UPSTREAM" worktree list --porcelain 2>/dev/null |
      grep -Fxq "worktree $ws" || return 1
    upstream_common="$(git -C "$RA8_WS_UPSTREAM" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)" ||
      return 1
    ws_common="$(git -C "$ws" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)" ||
      return 1
    [[ "$(realpath -e -- "$upstream_common")" == "$(realpath -e -- "$ws_common")" ]]
  }

  refresh_remote_refs() {
    if run_git_network_with_inherited_transport \
      -c core.attributesFile=/dev/null -c core.fsmonitor=false \
      -c core.hooksPath=/dev/null -C "$RA8_WS_UPSTREAM" \
      fetch --quiet --prune origin; then
      RA8_WS_REMOTE_REFS_FRESH=1
      return 0
    fi
    RA8_WS_REMOTE_REFS_FRESH=0
    log "note: origin refresh failed; every reap candidate will be retained"
    return 1
  }

  refresh_remote_refs_best_effort() {
    # Keep refresh_remote_refs out of Bash's `||` ignore-errors context so a
    # future multi-command edit cannot silently continue after a partial failure.
    set +e
    refresh_remote_refs
    set -e
    return 0
  }

  refresh_remote_refs_unless_fresh() {
    if [[ "$1" != "1" ]]; then
      refresh_remote_refs_best_effort
    fi
  }

  current_ttl() {
    local pct
    pct="$(df --output=pcent "$RA8_WS_ROOT" 2>/dev/null | tail -1 | tr -dc '0-9')"
    [[ -z "$pct" ]] && pct=0
    if [[ "$pct" -ge "$RA8_WS_DISK_PCT" ]]; then
      log "note: disk ${pct}% >= ${RA8_WS_DISK_PCT}% -- aggressive TTL ${RA8_WS_TTL_HOURS_FULL}h"
      echo "$RA8_WS_TTL_HOURS_FULL"
    else
      echo "$RA8_WS_TTL_HOURS"
    fi
  }

  # --------------------------------------------------------------------------
  # Commands
  # --------------------------------------------------------------------------
  validate_create_request() {
    local name="$1" ref="$2" branch="$3" owner="$4"
    validate_workspace_name "$name"
    validate_metadata_value "$RA8_WS_ROOT" "workspace root"
    validate_metadata_value "$RA8_WS_UPSTREAM" "upstream repository"
    validate_metadata_value "$ref" "start ref"
    validate_metadata_value "${USER:-unknown}" "creator"
    [[ "$owner" == "agent" || "$owner" == "work" ]] || die "owner must be agent or work"
    if [[ -n "$branch" ]]; then
      validate_metadata_value "$branch" "branch"
      git check-ref-format --branch "$branch" >/dev/null 2>&1 || die "invalid branch name: $branch"
    fi
    if [[ "$owner" == "work" ]]; then
      [[ "$name" =~ ^work-([a-z0-9][a-z0-9-]{0,62}|[1-9][0-9]{0,9})$ ]] ||
        die "work-owned workspace must use the reserved work-<identifier> name"
      [[ "$branch" == "work/${name#work-}" ]] ||
        die "work-owned workspace branch must be work/<identifier>"
    else
      [[ "$name" != work-* ]] || die "agent-owned workspace may not use the reserved work-* prefix"
      [[ -z "$branch" ]] || die "agent-owned workspace may not claim a work branch"
    fi
    [[ -d "$RA8_WS_UPSTREAM" ]] || die "upstream repo not found at $RA8_WS_UPSTREAM"
  }

  ensure_metadata_slot() {
    local name="$1" meta="$2"
    if [[ -e "$meta" || -L "$meta" ]]; then
      [[ -d "$meta" && ! -L "$meta" ]] || die "canonical metadata directory is unsafe: $meta"
    else
      mkdir -m 0700 "$meta"
    fi
    [[ "$(stat -c '%u' "$meta")" -eq "$(id -u)" ]] || die "metadata directory has foreign ownership"
    [[ "$((8#$(stat -c '%a' "$meta") & 8#022))" -eq 0 ]] ||
      die "metadata directory is group/world writable"
    [[ ! -e "$meta/$name" && ! -L "$meta/$name" ]] ||
      die "canonical metadata already exists for workspace: $name"
  }

  prepare_create_environment() {
    local ws="$1" stable="$HOME/.local/bin/ra8-agent-workspace" self_now guard_now
    # Refresh the stable copy the systemd timer executes, so it tracks whatever
    # version of this script agents are actually running. Without this the
    # installed copy is a one-time snapshot that silently drifts from the tree,
    # and a reaper fix landed in git would never reach the timer that matters.
    # Cheap, idempotent, and on the creation path so it self-heals.
    self_now="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
    if [[ "$self_now" != "$stable" ]] && ! cmp -s "$self_now" "$stable" 2>/dev/null; then
      mkdir -p "$HOME/.local/bin"
      if ! install -m 0755 "$self_now" "$stable" 2>/dev/null; then
        log "note: could not refresh optional timer helper at $stable"
      fi
    fi
    guard_now="$(workspace_guard)"
    if [[ "$guard_now" != "$HOME/.local/bin/workspace_guard.py" ]] &&
      ! cmp -s "$guard_now" "$HOME/.local/bin/workspace_guard.py" 2>/dev/null; then
      install -m 0644 "$guard_now" "$HOME/.local/bin/workspace_guard.py" 2>/dev/null ||
        log "note: could not refresh optional timer lock guard"
    fi

    # Prune first: a previously deleted directory can leave a stale registration
    # that makes `worktree add` refuse the path.
    git -C "$RA8_WS_UPSTREAM" worktree prune
    [[ ! -e "$ws" && ! -L "$ws" ]] || die "workspace already exists: $ws"
  }

  publish_workspace_metadata() {
    local name="$1" ref="$2" base_commit="$3" ws="$4"
    local branch="$5" owner="$6" meta="$7" temporary rc
    temporary="$(mktemp "$meta/.${name}.XXXXXX")" || return
    chmod 0600 "$temporary" || {
      rc=$?
      rm -f -- "$temporary"
      return "$rc"
    }
    printf 'schema=2\nname=%s\ncreated=%s\nby=%s\nref=%s\nbase_commit=%s\npath=%s\nbranch=%s\nowner=%s\n' \
      "$name" "$(date -Iseconds)" "${USER:-unknown}" "$ref" "$base_commit" "$ws" "$branch" \
      "$owner" >"$temporary" || {
      rc=$?
      rm -f -- "$temporary"
      return "$rc"
    }
    mv "$temporary" "$meta/$name" || {
      rc=$?
      rm -f -- "$temporary"
      return "$rc"
    }
  }

  rollback_failed_create() {
    local rc=$?
    trap - EXIT INT TERM HUP
    if [[ "$created" == "1" ]]; then
      if git -C "$RA8_WS_UPSTREAM" worktree remove --force "$ws" >/dev/null 2>&1 &&
        [[ -n "$requested_branch" ]]; then
        git -C "$RA8_WS_UPSTREAM" branch -D "$requested_branch" >/dev/null 2>&1 || true # Worktree is gone; preserve the create error if branch cleanup races.
      fi
    fi
    exit "$rc"
  }

  create_linked_workspace() {
    local name="$1" ref="$2" branch="$3" owner="$4" ws="$5" meta="$6" base_commit="$7"
    local attributes_checker="$RA8_WS_UPSTREAM/scripts/dev/git_environment.py"
    [[ -f "$attributes_checker" ]] || die "Git attribute policy checker is missing"
    "$RA8_TRUSTED_PYTHON" -I "$attributes_checker" --check-attributes "$RA8_WS_UPSTREAM" \
      --commit "$base_commit" >/dev/null ||
      die "Git attribute policy refused workspace creation"
    if [[ -n "$branch" ]]; then
      ! git -C "$RA8_WS_UPSTREAM" show-ref --verify --quiet "refs/heads/$branch" ||
        die "branch already exists: $branch"
    fi
    local requested_branch="$branch" created=0
    trap rollback_failed_create EXIT INT TERM HUP
    if [[ -n "$branch" ]]; then
      GIT_ATTR_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1 \
        GIT_CONFIG_SYSTEM=/dev/null \
        GIT_LFS_SKIP_SMUDGE=1 \
        "$RA8_TRUSTED_GIT" -c core.attributesFile=/dev/null -c core.fsmonitor=false \
        -c core.hooksPath=/dev/null -c filter.lfs.process= \
        -c filter.lfs.required=false -c filter.lfs.smudge=/bin/cat \
        -C "$RA8_WS_UPSTREAM" \
        worktree add -b "$branch" "$ws" "$base_commit" >/dev/null
    else
      GIT_ATTR_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1 \
        GIT_CONFIG_SYSTEM=/dev/null \
        GIT_LFS_SKIP_SMUDGE=1 \
        git -c core.attributesFile=/dev/null -c core.fsmonitor=false \
        -c core.hooksPath=/dev/null -c filter.lfs.process= \
        -c filter.lfs.required=false -c filter.lfs.smudge=/bin/cat \
        -C "$RA8_WS_UPSTREAM" \
        worktree add --detach "$ws" "$base_commit" >/dev/null
      branch="DETACHED"
    fi
    created=1
    publish_workspace_metadata "$name" "$ref" "$base_commit" "$ws" "$branch" "$owner" "$meta"
    created=0
    trap - EXIT INT TERM HUP
  }

  print_workspace_ready() {
    local name="$1" ref="$2" ws="$3"
    printf 'workspace ready: %q\n' "$ws"
    printf '  ref:    %q (%s)\n' "$ref" "$(git -C "$ws" rev-parse --short HEAD)"
    printf '  gates:  cd %q && just ci\n' "$ws"
    printf '  one:    cd %q && just quality::local::gate %q\n' "$ws" '<name>'
    printf '  done:   /bin/bash -p %q release %q\n' "$0" "$name"
  }

  cmd_create() {
    require_linux
    local name="${1:-}" ref="origin/dev" branch="" owner="agent"
    [[ -z "$name" ]] && die "usage: agent_workspace.sh create <name> [<ref>] [--branch NAME] [--owner NAME]"
    shift
    if [[ "${1:-}" != --* && -n "${1:-}" ]]; then
      ref="$1"
      shift
    fi
    while [[ "$#" -gt 0 ]]; do
      case "$1" in
        --branch)
          [[ "$#" -ge 2 ]] || die "--branch requires a value"
          branch="$2"
          shift 2
          ;;
        --owner)
          [[ "$#" -ge 2 ]] || die "--owner requires a value"
          owner="$2"
          shift 2
          ;;
        *) die "unknown create option: $1" ;;
      esac
    done
    validate_create_request "$name" "$ref" "$branch" "$owner"
    local ws="$RA8_WS_ROOT/$name" meta="$RA8_WS_ROOT/.meta" base_commit
    refresh_remote_refs_best_effort
    cmd_reap --quiet --already-refreshed
    ensure_metadata_slot "$name" "$meta"
    prepare_create_environment "$ws"
    base_commit="$(git -C "$RA8_WS_UPSTREAM" rev-parse --verify "${ref}^{commit}" 2>/dev/null)" ||
      die "start ref does not resolve to a commit: $ref"
    create_linked_workspace "$name" "$ref" "$branch" "$owner" "$ws" "$meta" "$base_commit"
    print_workspace_ready "$name" "$ref" "$ws"
  }

  cmd_forget() {
    require_linux
    local name="${1:-}"
    [[ -n "$name" && "$#" -eq 1 ]] || die "usage: agent_workspace.sh forget <name>"
    validate_workspace_name "$name"
    local ws="$RA8_WS_ROOT/$name" meta="$RA8_WS_ROOT/.meta/$name"
    [[ -f "$meta" && ! -L "$meta" ]] || die "no canonical metadata for workspace: $name"
    guard_metadata "$meta" "$name" work || die "metadata is not a valid work-owned claim"
    [[ ! -e "$ws" && ! -L "$ws" ]] || die "workspace still exists; use release after reviewing it"
    if git -C "$RA8_WS_UPSTREAM" worktree list --porcelain | grep -Fxq "worktree $ws"; then
      die "workspace is still registered with git: $ws"
    fi
    rm -f -- "$meta"
    echo "forgot stale canonical metadata for $name"
  }

  cmd_release() {
    require_linux
    local name="${1:-}"
    [[ -z "$name" ]] && die "usage: agent_workspace.sh release <name>"
    validate_workspace_name "$name"
    local ws="$RA8_WS_ROOT/$name"
    workspace_is_registered_here "$ws" || die "workspace is not a registered direct child: $ws"
    local meta="$RA8_WS_ROOT/.meta/$name"
    [[ -f "$meta" && ! -L "$meta" ]] || die "workspace metadata is absent or unsafe: $meta"
    guard_metadata "$meta" "$name" any || die "workspace metadata is not authoritative"
    if tree_is_busy "$ws"; then
      die "workspace is in use by a live process: $ws"
    fi
    local status
    status="$(git -C "$ws" status --porcelain=v1 --untracked-files=all --ignore-submodules=none)" ||
      die "git status failed; retaining workspace"
    if [[ -n "$status" ]]; then
      log "refusing to release: $ws has uncommitted work"
      printf 'commit it, or after human review run: git -C %q worktree remove --force %q\n' \
        "$RA8_WS_UPSTREAM" "$ws" >&2
      exit 1
    fi
    git -C "$RA8_WS_UPSTREAM" worktree remove "$ws"
    git -C "$RA8_WS_UPSTREAM" worktree prune
    rm -f -- "$meta"
    echo "released $ws"
  }

  cmd_drop_branch() {
    require_linux
    local name="${1:-}" landed_ref="${2:-origin/dev}"
    [[ -n "$name" && "$#" -le 2 ]] ||
      die "usage: agent_workspace.sh drop-branch <work-name> [<landed-ref>]"
    validate_workspace_name "$name"
    [[ "$name" == work-* ]] || die "only reserved work-* branches are eligible"
    local branch="work/${name#work-}" branch_tree landed_tree ws="$RA8_WS_ROOT/$name"
    [[ ! -e "$ws" && ! -L "$ws" ]] || die "workspace still exists; release it first"
    git -C "$RA8_WS_UPSTREAM" show-ref --verify --quiet "refs/heads/$branch" ||
      die "local branch does not exist: $branch"
    branch_tree="$(git -C "$RA8_WS_UPSTREAM" rev-parse --verify "$branch^{tree}")" ||
      die "branch tree does not resolve"
    landed_tree="$(git -C "$RA8_WS_UPSTREAM" rev-parse --verify "$landed_ref^{tree}")" ||
      die "landed reference tree does not resolve: $landed_ref"
    [[ "$branch_tree" == "$landed_tree" ]] ||
      die "branch content is not exactly equivalent to $landed_ref; retaining it"
    git -C "$RA8_WS_UPSTREAM" branch -D -- "$branch"
    printf 'deleted content-equivalent local branch %q (matched %q)\n' "$branch" "$landed_ref"
  }

  cmd_list() {
    require_linux
    mkdir -p "$RA8_WS_ROOT"
    printf '%-22s %-10s %-8s %s\n' WORKSPACE IDLE INUSE HEAD
    local ws
    for ws in "$RA8_WS_ROOT"/*; do
      [[ -d "$ws" && ! -L "$ws" ]] || continue
      printf '%-22s %-10s %-8s %s\n' \
        "$(basename "$ws")" \
        "$(ws_idle_hours "$ws")h" \
        "$(tree_is_busy "$ws" && echo yes || echo no)" \
        "$(git -C "$ws" rev-parse --short HEAD 2>/dev/null || echo '-')"
    done
    echo
    echo "registered worktrees:"
    git -C "$RA8_WS_UPSTREAM" worktree list 2>/dev/null | sed 's/^/  /'
  }

  cmd_reap() {
    require_linux
    local quiet=0 force=0 already_refreshed=0 arg
    for arg in "$@"; do
      case "$arg" in
        --quiet) quiet=1 ;;
        --force) force=1 ;;
        --already-refreshed) already_refreshed=1 ;;
        *) die "unknown reap option: $arg" ;;
      esac
    done
    refresh_remote_refs_unless_fresh "$already_refreshed"
    [[ -d "$RA8_WS_ROOT" && ! -L "$RA8_WS_ROOT" ]] || die "workspace root became unsafe"
    local ttl
    ttl="$(current_ttl)"
    [[ "$force" == "1" ]] && ttl=0

    local ws verdict reaped=0
    for ws in "$RA8_WS_ROOT"/*; do
      [[ -d "$ws" && ! -L "$ws" ]] || continue
      # errexit off around the CALL, re-armed INSIDE the substitution. `|| true`
      # here would put is_reapable into bash's inherited ignore-errors state,
      # which propagates into the substitution subshell and cannot be cleared
      # there -- a mid-body failure would then yield a partial verdict that
      # still looks like a decision. Now it yields an empty one, and an empty
      # verdict is not `reapable-*`, so the workspace is kept.
      set +e
      verdict="$(
        set -e
        is_reapable "$ws" "$ttl"
      )"
      set -e
      if [[ "$verdict" == reapable-* ]]; then
        local name meta
        name="$(basename "$ws")"
        meta="$RA8_WS_ROOT/.meta/$name"
        if [[ -e "$meta" || -L "$meta" ]]; then
          if ! guard_metadata "$meta" "$name" any; then
            [[ "$quiet" == "1" ]] || echo "kept   $name (unsafe-metadata)"
            continue
          fi
        fi
        if git -C "$RA8_WS_UPSTREAM" worktree remove --force "$ws"; then
          [[ ! -e "$meta" && ! -L "$meta" ]] || rm -f -- "$meta"
          [[ "$quiet" == "1" ]] || echo "reaped $name ($verdict)"
          reaped=$((reaped + 1))
        else
          [[ "$quiet" == "1" ]] || echo "kept   $name (git-worktree-remove-failed)"
        fi
      else
        [[ "$quiet" == "1" ]] || echo "kept   $(basename "$ws") ($verdict)"
      fi
    done
    git -C "$RA8_WS_UPSTREAM" worktree prune 2>/dev/null || true

    # Legacy HOME scans and global runtime pruning are deliberately absent. This
    # lifecycle removes only registered direct children of its validated root.
    [[ "$quiet" == "1" ]] || echo "reaped $reaped workspace(s); disk now $(df -h "$RA8_WS_ROOT" | tail -1 | awk '{print $4}') free"
  }

  cmd_doctor() {
    require_linux
    echo "=== agent workspace environment ==="
    printf '  %-22s %s\n' "workspace root" "$RA8_WS_ROOT"
    printf '  %-22s %s\n' "upstream repo" "$RA8_WS_UPSTREAM"
    printf '  %-22s %s\n' "ttl (hours)" "$RA8_WS_TTL_HOURS (aggressive $RA8_WS_TTL_HOURS_FULL above ${RA8_WS_DISK_PCT}% disk)"
    # Mirror scripts/ci.sh's selection exactly, including a multi-word
    # RA8_CONTAINER_RUNTIME such as "sudo podman" -- otherwise doctor cheerfully
    # reports a runtime that is not the one `just ci` will actually use.
    local rt_cmd=()
    read -r -a rt_cmd <<<"${RA8_CONTAINER_RUNTIME:-}"
    if [[ "${#rt_cmd[@]}" -eq 0 ]]; then
      local c
      for c in podman docker nerdctl; do
        command -v "$c" >/dev/null 2>&1 && rt_cmd=("$c") && break
      done
    fi
    if [[ "${#rt_cmd[@]}" -gt 0 ]]; then
      printf '  %-22s %s\n' "container runtime" "${rt_cmd[*]} ($("${rt_cmd[@]}" --version 2>/dev/null | head -1))"
      if "${rt_cmd[@]}" info >/dev/null 2>&1; then
        printf '  %-22s %s\n' "runtime usable" "yes"
      else
        printf '  %-22s %s\n' "runtime usable" "NO -- '${rt_cmd[*]} info' fails; just ci cannot run"
      fi
    else
      printf '  %-22s %s\n' "container runtime" "MISSING -- just ci cannot run"
      echo "     Debian/Ubuntu: sudo apt-get install -y podman uidmap"
    fi
    if command -v ccache >/dev/null 2>&1; then
      printf '  %-22s %s\n' "ccache" "$(ccache --version | head -1) dir=${CCACHE_DIR:-$HOME/.cache/ccache}"
    else
      printf '  %-22s %s\n' "ccache" "MISSING -- builds will not share objects between workspaces"
    fi
    printf '  %-22s %s\n' "disk free" "$(df -h "$RA8_WS_ROOT" 2>/dev/null | tail -1 | awk '{print $4" of "$2}')"
    # Ask BOTH managers. install-timer picks the system one as root and the user
    # one otherwise, so a doctor that only knew about user units would report
    # "not installed" on every host where the timer is in fact running.
    local timer="not installed"
    if systemctl is-enabled ra8-workspace-reap.timer >/dev/null 2>&1; then
      timer="$(systemctl is-enabled ra8-workspace-reap.timer 2>/dev/null) (system)"
    elif systemctl --user is-enabled ra8-workspace-reap.timer >/dev/null 2>&1; then
      timer="$(systemctl --user is-enabled ra8-workspace-reap.timer 2>/dev/null) (user)"
    fi
    printf '  %-22s %s\n' "reap timer" "$timer"
    # The dev slice, where this host has one: a workspace on a CI runner host is
    # only safe because gate runs in it are weighted below the runners, so a
    # doctor there must say whether that is actually in force.
    if [[ -n "${RA8_DEV_SLICE:-}" ]]; then
      printf '  %-22s %s\n' "dev slice" \
        "$RA8_DEV_SLICE $(systemctl is-active "$RA8_DEV_SLICE" 2>/dev/null || echo inactive), freezer=$(systemctl show -p FreezerState --value "$RA8_DEV_SLICE" 2>/dev/null || echo '?')"
      printf '  %-22s %s\n' "gate container args" "${RA8_CI_CONTAINER_ARGS:-NONE -- just ci would run OUTSIDE the slice}"
      printf '  %-22s %s\n' "bounded parallelism" "RA8_MAX_JOBS=${RA8_MAX_JOBS:-unset} CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-unset}"
    fi
  }

  # WHICH systemd the reaper is installed into, and why it is decided rather than
  # fixed. As an ordinary user
  # this installs a USER timer: the whole mechanism then lives inside the agent
  # account with no root involvement, which is right on the shared verification
  # box where several agents each own their workspaces.
  #
  # As ROOT it installs a SYSTEM timer instead, and that is not a preference. A
  # user timer belongs to a user MANAGER that exits when the account's last
  # session ends unless lingering is enabled -- and the hosts where this runs as
  # root are headless distros nobody logs into (the WSL2 CI box's only account is
  # root, with `loginctl show-user root -p Linger` reporting `no`). A user timer
  # there is installed, reported enabled, and then silently never fires again
  # after the installing session closes: the disk fills up with no visible cause,
  # which is precisely the class of tooling-that-enforces-nothing this tree keeps
  # finding. `systemctl --user` for root is also the odd path in every other
  # respect; a system unit is what a root-owned, machine-wide sweep should be.
  #
  # Writes the two unit files into $1 for the reaper copy at $2.
  write_reap_units() {
    local unitdir="$1" stable="$2"
    [[ "$stable" =~ ^/[A-Za-z0-9._/@:+,=-]+$ ]] ||
      die "stable reaper path is not systemd-exec-safe: $stable"
    mkdir -p "$unitdir"
    cat >"$unitdir/ra8-workspace-reap.service" <<EOF
[Unit]
Description=Reap stale RA8 agent workspaces and build output

[Service]
Type=oneshot
Environment=RA8_WS_ROOT=$RA8_WS_ROOT
Environment=RA8_WS_UPSTREAM=$RA8_WS_UPSTREAM
Environment=RA8_WS_TTL_HOURS=$RA8_WS_TTL_HOURS
Environment=RA8_WS_DISK_PCT=$RA8_WS_DISK_PCT
Environment=RA8_WS_TTL_HOURS_FULL=$RA8_WS_TTL_HOURS_FULL
ExecStart=$stable reap
EOF
    cat >"$unitdir/ra8-workspace-reap.timer" <<'EOF'
[Unit]
Description=Periodic reap of stale RA8 agent workspaces

[Timer]
# Hourly rather than daily: the reaper is cheap and idle-aware, and the
# disk-threshold rule inside it only helps if it gets a chance to observe a
# full disk before the next agent needs space.
OnCalendar=hourly
OnBootSec=10min
Persistent=true

[Install]
WantedBy=timers.target
EOF
  }

  # Install the systemd timer that makes reaping automatic. Idempotent, so
  # re-running it after a script change is safe.
  cmd_install_timer() {
    require_linux
    local system=0 manager=user unitdir="$HOME/.config/systemd/user"
    local sc=(systemctl --user)
    if [[ "$(id -u)" -eq 0 ]]; then
      system=1
      manager=system
      unitdir="/etc/systemd/system"
      sc=(systemctl)
    fi
    local self
    self="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"

    # Run the timer from a STABLE copy, never from the checkout it was invoked
    # from. install-timer is naturally run out of whichever tree the agent had
    # open -- frequently a workspace under $RA8_WS_ROOT -- and pointing ExecStart
    # there makes the reaper delete its own executable the moment that workspace
    # goes stale. The failure is silent (a systemd oneshot that cannot exec just
    # logs and exits non-zero) and it disables exactly the mechanism that is
    # supposed to keep the disk clear, so the box would fill up again with no
    # visible cause. A copy outside $RA8_WS_ROOT cannot be reaped by definition.
    local stable_dir="$HOME/.local/bin"
    local stable="$stable_dir/ra8-agent-workspace"
    mkdir -p "$stable_dir"
    if [[ "$self" != "$stable" ]]; then
      install -m 0755 "$self" "$stable"
    fi
    install -m 0644 "$(workspace_guard)" "$stable_dir/workspace_guard.py"

    write_reap_units "$unitdir" "$stable"
    "${sc[@]}" daemon-reload
    "${sc[@]}" enable --now ra8-workspace-reap.timer
    if [[ "$system" == "0" ]] &&
      ! loginctl show-user "$(id -un)" -p Linger --value 2>/dev/null | grep -q yes; then
      log "note: lingering is off; enable it so the timer runs without a login:"
      log "      sudo loginctl enable-linger $(id -un)"
    fi
    # Read back rather than trust: a unit that was written and not armed is the
    # silent nothing the comment above exists to prevent.
    if [[ "$("${sc[@]}" is-active ra8-workspace-reap.timer 2>/dev/null)" != active ]]; then
      die "ra8-workspace-reap.timer is not active after installing it ($manager" \
        "manager); stale workspaces would never be swept"
    fi
    echo "installed ra8-workspace-reap.timer ($manager)"
    "${sc[@]}" list-timers ra8-workspace-reap.timer --no-pager 2>/dev/null | head -3
  }

  case "${1:-}" in
    create)
      ensure_lifecycle_lock "$@"
      shift
      cmd_create "$@"
      ;;
    install-timer)
      shift
      cmd_install_timer "$@"
      ;;
    release)
      ensure_lifecycle_lock "$@"
      shift
      cmd_release "$@"
      ;;
    forget)
      ensure_lifecycle_lock "$@"
      shift
      cmd_forget "$@"
      ;;
    list)
      shift
      cmd_list "$@"
      ;;
    reap)
      ensure_lifecycle_lock "$@"
      shift
      cmd_reap "$@"
      ;;
    drop-branch)
      ensure_lifecycle_lock "$@"
      shift
      cmd_drop_branch "$@"
      ;;
    doctor)
      shift
      cmd_doctor "$@"
      ;;
    *)
      sed -n '/^# Usage:/,/^$/p' "$0" | sed 's/^# \{0,1\}//'
      exit 2
      ;;
  esac
else
  [[ "$-" == *p* ]]
fi
