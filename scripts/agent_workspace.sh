#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/agent_workspace.sh -- the ONE way a concurrent agent gets an isolated
# checkout on a shared verification box.
#
# The problem this replaces:
#   Several agents shared one working tree at ~/ra8-firmware. Each improvised
#   its own isolation, leaving stray checkouts (~/ra8-296, ~/ra8-base,
#   ~/ra8-fmt-work, ~/ra8-integ, /tmp/ra8-h, 11 registered worktrees, 64 GiB of
#   home). Concurrent gate runs thrashed the box, so a `flock` was bolted on
#   mid-session. A .gcda left behind by a build of a DIFFERENT branch made
#   gcovr fail with no_working_dir_found -- a bogus coverage failure that reads
#   exactly like a real one, worked around by remembering to
#   `rm -rf tests/build build/tidy` before every coverage run.
#
# The shape of the fix -- three properties, all STRUCTURAL rather than
# remembered:
#
#   1. ISOLATION. One git worktree per agent under $RA8_WS_ROOT. Worktrees
#      share ~/ra8-firmware's object store, so a workspace costs a checkout
#      (~500 MiB) rather than a full clone (~5 GiB), and a branch committed in
#      one workspace is immediately visible to all the others.
#
#   2. EPHEMERAL BUILD OUTPUT. Gates run via `make ci`, which builds inside a
#      `--rm` container against a fresh `git archive HEAD` snapshot. Build
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
#   bash scripts/agent_workspace.sh create <name> [<ref>]  # isolated workspace
#   bash scripts/agent_workspace.sh release <name>         # give it back
#   bash scripts/agent_workspace.sh list                   # what exists
#   bash scripts/agent_workspace.sh reap [--force]         # sweep stale ones
#   bash scripts/agent_workspace.sh doctor                 # environment check
#   bash scripts/agent_workspace.sh install-timer          # make reaping automatic
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

# --------------------------------------------------------------------------
# Liveness. A workspace with a live process in it is never touched, no matter
# how old it looks -- four agents share this box and a mid-flight gate run must
# not lose its tree underneath it.
# --------------------------------------------------------------------------
ws_in_use() {
  local ws="$1" p cw
  for p in $(ps -u "$(id -u)" -o pid= 2>/dev/null); do
    cw="$(readlink "/proc/$p/cwd" 2>/dev/null || true)"
    case "$cw" in "$ws" | "$ws"/*) return 0 ;; esac
  done
  if command -v lsof >/dev/null 2>&1; then
    lsof +D "$ws" >/dev/null 2>&1 && return 0
  fi
  return 1
}

ws_idle_hours() {
  local ws="$1" newest now
  newest="$(find "$ws" -xdev -type f -not -path '*/.git/*' -printf '%T@\n' 2>/dev/null |
    sort -rn | head -1 | cut -d. -f1)"
  [[ -z "$newest" ]] && newest=0
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
  if ws_in_use "$ws"; then
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
  done:   bash scripts/agent_workspace.sh release $name

Build output stays inside the container, so there is nothing to clean here and
no need to remove tests/build or build/tidy before a coverage run.
EOF
}

cmd_release() {
  local name="${1:-}"
  [[ -z "$name" ]] && die "usage: agent_workspace.sh release <name>"
  local ws="$RA8_WS_ROOT/$name"
  [[ -d "$ws" ]] || die "no such workspace: $ws"
  if ws_in_use "$ws"; then
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
  mkdir -p "$RA8_WS_ROOT"
  printf '%-22s %-10s %-8s %s\n' WORKSPACE IDLE INUSE HEAD
  local ws
  for ws in "$RA8_WS_ROOT"/*; do
    [[ -d "$ws" ]] || continue
    printf '%-22s %-10s %-8s %s\n' \
      "$(basename "$ws")" \
      "$(ws_idle_hours "$ws")h" \
      "$(ws_in_use "$ws" && echo yes || echo no)" \
      "$(git -C "$ws" rev-parse --short HEAD 2>/dev/null || echo '-')"
  done
  echo
  echo "registered worktrees:"
  git -C "$RA8_WS_UPSTREAM" worktree list 2>/dev/null | sed 's/^/  /'
}

cmd_reap() {
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
    verdict="$(is_reapable "$ws" "$ttl" || true)"
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
  printf '  %-22s %s\n' "disk free" "$(df -h "$RA8_WS_ROOT" 2>/dev/null | tail -1 | awk '{print $4" of "$2}')"
  printf '  %-22s %s\n' "reap timer" "$(systemctl --user is-enabled ra8-workspace-reap.timer 2>/dev/null || echo 'not installed')"
}

# Install the systemd user timer that makes reaping automatic. Idempotent, so
# re-running it after a script change is safe. A user timer (not system) keeps
# the whole mechanism inside the agent account with no root involvement; it
# needs lingering enabled so it still fires when nobody is logged in.
cmd_install_timer() {
  local unitdir="$HOME/.config/systemd/user"
  local self
  self="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
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
ExecStart=/usr/bin/env bash $self reap
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
  systemctl --user daemon-reload
  systemctl --user enable --now ra8-workspace-reap.timer
  if ! loginctl show-user "$(id -un)" -p Linger --value 2>/dev/null | grep -q yes; then
    log "note: lingering is off; enable it so the timer runs without a login:"
    log "      sudo loginctl enable-linger $(id -un)"
  fi
  echo "installed ra8-workspace-reap.timer"
  systemctl --user list-timers ra8-workspace-reap.timer --no-pager 2>/dev/null | head -3
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
