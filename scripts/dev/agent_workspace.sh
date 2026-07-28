#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
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
#   2. EPHEMERAL BUILD OUTPUT. Gates run via `make ci`, which builds inside a
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
#   bash scripts/dev/agent_workspace.sh create <name> [<ref>]  # isolated workspace
#   bash scripts/dev/agent_workspace.sh release <name>         # give it back
#   bash scripts/dev/agent_workspace.sh list                   # what exists
#   bash scripts/dev/agent_workspace.sh reap [--force]         # sweep stale ones
#   bash scripts/dev/agent_workspace.sh doctor                 # environment check
#   bash scripts/dev/agent_workspace.sh install-timer          # make reaping automatic
#
# Equivalent make targets: make ws-new NAME=x / ws-free NAME=x / ws-list / ws-reap

set -euo pipefail

RA8_WS_ROOT="${RA8_WS_ROOT:-$HOME/ra8-ws}"
RA8_WS_UPSTREAM="${RA8_WS_UPSTREAM:-$HOME/ra8-firmware}"
# A workspace idle longer than this is reapable. Deliberately longer than a
# working session and shorter than a weekend.
RA8_WS_TTL_HOURS="${RA8_WS_TTL_HOURS:-24}"
# Above this disk usage the reaper switches to the aggressive TTL below, so the
# box responds to actually being full rather than only to the clock.
RA8_WS_DISK_PCT="${RA8_WS_DISK_PCT:-80}"
RA8_WS_TTL_HOURS_FULL="${RA8_WS_TTL_HOURS_FULL:-4}"

log() { printf '%s\n' "$*" >&2; }
die() {
  log "error: $*"
  exit 1
}

# This script DELETES directories, and every one of its safety checks is built
# on a Linux-only primitive: /proc/<pid>/cwd for liveness, `find -printf` for
# idle age, `df --output=pcent` for disk pressure. On a platform without them
# each check degrades to "no evidence of use" -- which is the fail-OPEN
# direction, i.e. it would make a busy workspace look reapable. Refuse instead.
# The shared verification box is Linux; there is no reason to run this on the
# Mac, where `make ci` is the containerised path against a full clone anyway.
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
  if [[ -n "$(git -C "$ws" status --porcelain 2>/dev/null | grep -v '^?? ' || true)" ]]; then
    echo "uncommitted-changes"
    return 1
  fi
  if [[ -n "$(git -C "$ws" ls-files --others --exclude-standard 2>/dev/null || true)" ]]; then
    echo "untracked-files"
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
cmd_create() {
  require_linux
  local name="${1:-}" ref="${2:-origin/dev}"
  [[ -z "$name" ]] && die "usage: agent_workspace.sh create <name> [<ref>]"
  [[ -d "$RA8_WS_UPSTREAM" ]] || die "upstream repo not found at $RA8_WS_UPSTREAM"
  mkdir -p "$RA8_WS_ROOT"
  local ws="$RA8_WS_ROOT/$name"
  [[ -e "$ws" ]] && die "workspace already exists: $ws"

  # Self-healing: reap stale siblings on the creation path, so the box stays
  # tidy even where the timer is not installed. Never fatal -- a reap problem
  # must not block getting a workspace.
  cmd_reap --quiet || true

  # Refresh the stable copy the systemd timer executes, so it tracks whatever
  # version of this script agents are actually running. Without this the
  # installed copy is a one-time snapshot that silently drifts from the tree,
  # and a reaper fix landed in git would never reach the timer that matters.
  # Cheap, idempotent, and on the creation path so it self-heals.
  local stable="$HOME/.local/bin/ra8-agent-workspace"
  local self_now
  self_now="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
  if [[ "$self_now" != "$stable" ]] && ! cmp -s "$self_now" "$stable" 2>/dev/null; then
    mkdir -p "$HOME/.local/bin"
    install -m 0755 "$self_now" "$stable" 2>/dev/null || true
  fi

  git -C "$RA8_WS_UPSTREAM" fetch --quiet origin 2>/dev/null || log "note: fetch failed, using cached refs"
  # Prune first: a previously deleted directory can leave a stale registration
  # that makes `worktree add` refuse the path.
  git -C "$RA8_WS_UPSTREAM" worktree prune
  git -C "$RA8_WS_UPSTREAM" worktree add --detach "$ws" "$ref" >/dev/null
  # Metadata lives OUTSIDE the checkout. Writing it into the workspace left an
  # untracked file there, which made is_reapable() report "untracked-files"
  # forever -- every workspace would have been permanently unreapable, i.e. the
  # self-healing property silently defeated by its own bookkeeping. It also
  # kept `git status` dirty for the agent working in the tree.
  mkdir -p "$RA8_WS_ROOT/.meta"
  printf 'created=%s\nby=%s\nref=%s\npath=%s\n' \
    "$(date -Iseconds)" "${USER:-unknown}" "$ref" "$ws" >"$RA8_WS_ROOT/.meta/$name"

  cat <<EOF
workspace ready: $ws
  ref:    $ref ($(git -C "$ws" rev-parse --short HEAD))
  gates:  cd $ws && make ci        # runs in an ephemeral container
  one:    cd $ws && make ci-gate GATE=<name>
  done:   bash scripts/dev/agent_workspace.sh release $name

This is a linked git worktree. scripts/ci.sh detects that and bind-mounts the
main repo's git directory into the container as well, so the containerised
suite works here unmodified.

Build output stays inside the container, so there is nothing to clean here and
no need to remove tests/build or build/tidy before a coverage run.
EOF
}

cmd_release() {
  require_linux
  local name="${1:-}"
  [[ -z "$name" ]] && die "usage: agent_workspace.sh release <name>"
  local ws="$RA8_WS_ROOT/$name"
  [[ -d "$ws" ]] || die "no such workspace: $ws"
  if tree_is_busy "$ws"; then
    die "workspace is in use by a live process: $ws"
  fi
  local status
  status="$(git -C "$ws" status --porcelain 2>/dev/null || true)"
  if [[ -n "$status" ]]; then
    log "refusing to release: $ws has uncommitted work"
    log "commit it, or force with: git -C $RA8_WS_UPSTREAM worktree remove --force $ws"
    exit 1
  fi
  git -C "$RA8_WS_UPSTREAM" worktree remove "$ws"
  git -C "$RA8_WS_UPSTREAM" worktree prune
  rm -f "$RA8_WS_ROOT/.meta/$name"
  echo "released $ws"
}

cmd_list() {
  require_linux
  mkdir -p "$RA8_WS_ROOT"
  printf '%-22s %-10s %-8s %s\n' WORKSPACE IDLE INUSE HEAD
  local ws
  for ws in "$RA8_WS_ROOT"/*; do
    [[ -d "$ws" ]] || continue
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

# Sweep regenerable build output out of checkouts that are NOT managed
# workspaces.
#
# The managed lifecycle covers $RA8_WS_ROOT, but agents improvised their own
# checkouts long before it existed (~/ra8-296, ~/wt-*), and that is where the
# disk actually went: tens of GiB of build output against ~0.5 GiB of source
# per tree. Those trees cannot simply be deleted -- some hold work, and some
# have an agent inside them right now -- but their build directories are
# regenerable BY DEFINITION, which makes them safe to reclaim on a timer.
#
# This is what makes the cleanup automatic for legacy layouts too, rather than
# only for trees created the new way. Three independent safety rules:
#   1. Only directory NAMES known to be build output are ever considered.
#   2. The parent must look like a checkout of this project (CMakeLists.txt),
#      so a stray path can never be interpreted as a build directory.
#   3. A tree with a live process inside it is skipped entirely.
sweep_build_output() {
  local ttl_hours="$1" quiet="$2"
  local roots="${RA8_WS_SWEEP_ROOTS:-$HOME}"
  local reclaimed=0 root tree bd sz

  for root in $roots; do
    [[ -d "$root" ]] || continue
    for tree in "$root"/*; do
      [[ -d "$tree" ]] || continue
      # Rule 2: only inside something that looks like a checkout of this repo.
      [[ -f "$tree/CMakeLists.txt" ]] || continue
      # Rule 3: never touch a tree someone is working in.
      if tree_is_busy "$tree"; then
        [[ "$quiet" == "1" ]] || echo "skip   $(basename "$tree") (busy: live process inside)"
        continue
      fi
      for bd in "$tree/build" "$tree/tests/build" "$tree/tests/build-cov" \
        "$tree/tests/build-ubsan"; do
        [[ -d "$bd" ]] || continue
        # Age check on the directory itself: -mmin takes minutes.
        if [[ -n "$(find "$bd" -maxdepth 0 -mmin "+$((ttl_hours * 60))" 2>/dev/null)" ]]; then
          sz="$(du -sm "$bd" 2>/dev/null | cut -f1)"
          rm -rf "$bd" && reclaimed=$((reclaimed + ${sz:-0}))
          [[ "$quiet" == "1" ]] || echo "swept  ${bd#"$HOME"/} (${sz:-?} MB)"
        fi
      done
    done
  done
  [[ "$quiet" == "1" ]] || echo "build-output sweep reclaimed ${reclaimed} MB"
}

cmd_reap() {
  require_linux
  local quiet=0 force=0 arg
  for arg in "$@"; do
    case "$arg" in
      --quiet) quiet=1 ;;
      --force) force=1 ;;
    esac
  done
  mkdir -p "$RA8_WS_ROOT"
  local ttl
  ttl="$(current_ttl)"
  [[ "$force" == "1" ]] && ttl=0

  local ws verdict reaped=0
  for ws in "$RA8_WS_ROOT"/*; do
    [[ -d "$ws" ]] || continue
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
      git -C "$RA8_WS_UPSTREAM" worktree remove --force "$ws" 2>/dev/null || rm -rf "$ws"
      rm -f "$RA8_WS_ROOT/.meta/$(basename "$ws")"
      [[ "$quiet" == "1" ]] || echo "reaped $(basename "$ws") ($verdict)"
      reaped=$((reaped + 1))
    else
      [[ "$quiet" == "1" ]] || echo "kept   $(basename "$ws") ($verdict)"
    fi
  done
  git -C "$RA8_WS_UPSTREAM" worktree prune 2>/dev/null || true

  # Reclaim build output from unmanaged checkouts as part of the same sweep.
  # Deliberately a LONGER ttl than the workspace ttl: a checkout an agent is
  # iterating in should keep its object files across a short pause, and the
  # only cost of waiting is disk that the threshold rule already reacts to.
  sweep_build_output "${RA8_WS_BUILD_TTL_HOURS:-6}" "$quiet"

  # Container garbage is part of the same lifecycle. Folding it in here is what
  # stops `podman system prune` from becoming another thing someone has to
  # remember. Images are kept: rebuilding the devcontainer is expensive and it
  # is shared by every workspace.
  if command -v podman >/dev/null 2>&1; then
    podman container prune -f >/dev/null 2>&1 || true
    podman volume prune -f >/dev/null 2>&1 || true
    if [[ "$force" == "1" ]]; then
      podman image prune -f >/dev/null 2>&1 || true
    fi
  fi
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
  # reports a runtime that is not the one `make ci` will actually use.
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
      printf '  %-22s %s\n' "runtime usable" "NO -- '${rt_cmd[*]} info' fails; make ci cannot run"
    fi
  else
    printf '  %-22s %s\n' "container runtime" "MISSING -- make ci cannot run"
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
    printf '  %-22s %s\n' "gate container args" "${RA8_CI_CONTAINER_ARGS:-NONE -- make ci would run OUTSIDE the slice}"
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
  mkdir -p "$unitdir"
  cat >"$unitdir/ra8-workspace-reap.service" <<EOF
[Unit]
Description=Reap stale RA8 agent workspaces and container garbage

[Service]
Type=oneshot
Environment=RA8_WS_ROOT=$RA8_WS_ROOT
Environment=RA8_WS_UPSTREAM=$RA8_WS_UPSTREAM
Environment=RA8_WS_TTL_HOURS=$RA8_WS_TTL_HOURS
Environment=RA8_WS_DISK_PCT=$RA8_WS_DISK_PCT
Environment=RA8_WS_TTL_HOURS_FULL=$RA8_WS_TTL_HOURS_FULL
ExecStart=/usr/bin/env bash $stable reap
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
    shift
    cmd_create "$@"
    ;;
  install-timer)
    shift
    cmd_install_timer "$@"
    ;;
  release)
    shift
    cmd_release "$@"
    ;;
  list)
    shift
    cmd_list "$@"
    ;;
  reap)
    shift
    cmd_reap "$@"
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
