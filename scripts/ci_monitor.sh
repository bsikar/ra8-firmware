#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/ci_monitor.sh -- ONE shared GitHub Actions watcher for the whole
# agent fleet, replacing per-agent `gh run watch`.
#
# The problem:
#   Each agent ran a blocking `gh run watch` against all three workflows
#   (firmware, coverage, board-sim-smoke). Six agents therefore meant ~18
#   independent pollers plus issue queries, and the 5000/hour REST quota was
#   exhausted TWICE in one day. The consequences were not cosmetic: two agents'
#   watchers were killed mid-run, and one had to finish its task reporting
#   "4 jobs unconfirmed" because it could no longer query -- a state
#   indistinguishable from a broken build until a human checks by hand.
#
# The design:
#   ONE long-lived poller (`daemon`) asks GitHub for recent runs and writes the
#   result to a local status file. Every agent reads that FILE (`status`,
#   `wait`), which costs zero quota no matter how many agents there are. Poll
#   cost is ONE request per interval for ALL workflows, because
#   /actions/runs returns every workflow in a single response -- so the fleet's
#   entire CI-status traffic is ~30 requests/hour at the default interval,
#   against a 5000/hour budget.
#
#   Note git itself never consumed quota here: the remote is SSH, so pushes and
#   fetches are free. All of the waste was status polling.
#
# Three rules this encodes, each from an observed failure:
#   1. Quota exhaustion reports UNKNOWN -- never pass, never fail. Conflating
#      "I cannot see the result" with "the result is bad" is what sends agents
#      chasing ghosts. Every path that cannot establish a verdict exits 3.
#   2. Back off BEFORE exhaustion, not after, so the fleet degrades gracefully
#      and a reserve is left for the interactive work a human is doing.
#   3. Agents should not BLOCK on CI at all. `make ci` runs a superset of the
#      workflows locally, so GitHub is confirmation, not discovery. `wait`
#      exists for the rare case that genuinely needs it and is bounded.
#
# Exit codes are the contract, and there are exactly three:
#   0  PASS      every run for the sha completed successfully
#   1  FAIL      at least one run for the sha failed / timed out / was cancelled
#   3  UNKNOWN   no verdict could be established -- quota exhausted, daemon
#                down, status file stale, jq missing, sha not seen yet. NEVER
#                collapse this into 0 or 1.
#
# Usage:
#   bash scripts/ci_monitor.sh daemon            # the single shared poller
#   bash scripts/ci_monitor.sh status [--sha X]  # read cached status (0 quota)
#   bash scripts/ci_monitor.sh wait  --sha X [--timeout 1800]
#   bash scripts/ci_monitor.sh quota             # remaining REST quota
#   bash scripts/ci_monitor.sh install-service   # run the daemon under systemd
#   bash scripts/ci_monitor.sh runner-status     # read outcomes off the runner
#                                                # box; zero quota, works when
#                                                # the budget is exhausted

# No `set -e`: every command below either has its failure handled explicitly or
# feeds an exit-code decision, and an unexpected abort would exit 1 -- which
# this contract reads as FAIL. Silence is UNKNOWN here, never failure.
set -uo pipefail

# Exit codes, by name, so no call site has to remember which integer is which.
readonly RA8_CI_EXIT_PASS=0
readonly RA8_CI_EXIT_FAIL=1
readonly RA8_CI_EXIT_UNKNOWN=3

RA8_CI_REPO="${RA8_CI_REPO:-bsikar/ra8-firmware}"
RA8_CI_STATE_DIR="${RA8_CI_STATE_DIR:-/var/tmp/ra8-ci-monitor}"
RA8_CI_STATE="$RA8_CI_STATE_DIR/status.json"
# Sparse by design. Jobs here run for many minutes; sub-minute resolution buys
# nothing and is what made 18 watchers expensive.
RA8_CI_INTERVAL="${RA8_CI_INTERVAL:-120}"
# Stop polling well above zero so interactive `gh` use by a human still works.
RA8_CI_QUOTA_RESERVE="${RA8_CI_QUOTA_RESERVE:-500}"
RA8_CI_BRANCH="${RA8_CI_BRANCH:-dev}"

mkdir -p "$RA8_CI_STATE_DIR" 2>/dev/null || true

log() { printf '[ci-monitor] %s\n' "$*" >&2; }

need_gh() {
  command -v gh >/dev/null 2>&1 || {
    log "error: gh CLI not on PATH"
    exit "$RA8_CI_EXIT_UNKNOWN"
  }
}

# jq is a hard dependency of every verdict-producing path, so its absence is
# reported as UNKNOWN and never worked around. An earlier version fell back to
# re-emitting the raw JSON, which made the caller read a whole document as a
# sha and let `status` exit 0 -- a missing tool silently reporting PASS. That
# is the exact failure mode CLAUDE.md bans for gates, and it applies here for
# the same reason.
need_jq() {
  command -v jq >/dev/null 2>&1 || {
    log "error: jq not on PATH -- cannot parse CI status; reporting UNKNOWN"
    log "       install it: sudo apt-get install -y jq"
    exit "$RA8_CI_EXIT_UNKNOWN"
  }
}

# rate_limit does not itself count against the REST quota, so this is safe to
# call every cycle.
quota_remaining() {
  gh api rate_limit --jq '.resources.core.remaining' 2>/dev/null || echo "unknown"
}

cmd_quota() {
  need_gh
  local r reset
  r="$(quota_remaining)"
  reset="$(gh api rate_limit --jq '.resources.core.reset' 2>/dev/null || echo 0)"
  echo "core quota remaining: $r"
  if [[ "$reset" != "0" && "$reset" != "" ]]; then
    echo "resets at: $(date -d "@$reset" 2>/dev/null || date -r "$reset" 2>/dev/null || echo "$reset")"
  fi
  echo "reserve threshold: $RA8_CI_QUOTA_RESERVE (poller backs off below this)"
}

write_state() {
  local payload="$1"
  local tmp="$RA8_CI_STATE.tmp.$$"
  printf '%s\n' "$payload" >"$tmp" 2>/dev/null && mv -f "$tmp" "$RA8_CI_STATE" 2>/dev/null
  # World-writable on purpose: several agents run as different accounts on the
  # shared box and any of them may be the one that restarts the daemon.
  chmod 666 "$RA8_CI_STATE" 2>/dev/null || true
}

poll_once() {
  local remaining
  remaining="$(quota_remaining)"

  if [[ "$remaining" == "unknown" ]]; then
    write_state "$(printf '{"polled_at":"%s","quota":"unknown","overall":"UNKNOWN","reason":"cannot reach GitHub API","runs":[]}' "$(date -Iseconds)")"
    log "quota unknown -- reporting UNKNOWN"
    return 1
  fi
  if [[ "$remaining" -lt "$RA8_CI_QUOTA_RESERVE" ]]; then
    write_state "$(printf '{"polled_at":"%s","quota":%s,"overall":"UNKNOWN","reason":"quota below reserve (%s) -- backing off, NOT a failure","runs":[]}' \
      "$(date -Iseconds)" "$remaining" "$RA8_CI_QUOTA_RESERVE")"
    log "quota $remaining < reserve $RA8_CI_QUOTA_RESERVE -- backing off, state=UNKNOWN"
    return 1
  fi

  # ONE request covers every workflow.
  local runs
  runs="$(gh api "/repos/$RA8_CI_REPO/actions/runs?branch=$RA8_CI_BRANCH&per_page=30" \
    --jq '[.workflow_runs[] | {name:.name, sha:.head_sha, status:.status, conclusion:.conclusion, url:.html_url, updated:.updated_at}]' \
    2>/dev/null)"
  if [[ -z "$runs" ]]; then
    write_state "$(printf '{"polled_at":"%s","quota":%s,"overall":"UNKNOWN","reason":"query failed","runs":[]}' \
      "$(date -Iseconds)" "$remaining")"
    return 1
  fi

  # Overall verdict covers only the newest sha seen, so a stale older red does
  # not mask a current green (or vice versa).
  local head_sha overall
  head_sha="$(printf '%s' "$runs" | jq -r 'if length>0 then .[0].sha else "" end')"
  overall="$(printf '%s' "$runs" | jq -r --arg s "$head_sha" '
      [ .[] | select(.sha==$s) ] as $cur
      | if ($cur|length)==0 then "UNKNOWN"
        elif any($cur[]; .conclusion=="failure" or .conclusion=="timed_out" or .conclusion=="cancelled") then "FAIL"
        elif all($cur[]; .status=="completed") and all($cur[]; .conclusion=="success" or .conclusion=="skipped") then "PASS"
        else "RUNNING" end')"

  write_state "$(printf '{"polled_at":"%s","quota":%s,"branch":"%s","head_sha":"%s","overall":"%s","runs":%s}' \
    "$(date -Iseconds)" "$remaining" "$RA8_CI_BRANCH" "$head_sha" "$overall" "$runs")"
  log "polled: $overall (sha ${head_sha:0:9}, quota $remaining)"
  return 0
}

cmd_daemon() {
  need_gh
  need_jq
  log "starting shared poller: repo=$RA8_CI_REPO branch=$RA8_CI_BRANCH interval=${RA8_CI_INTERVAL}s"
  log "state file: $RA8_CI_STATE"
  local backoff="$RA8_CI_INTERVAL"
  while true; do
    if poll_once; then
      backoff="$RA8_CI_INTERVAL"
    else
      # Exponential backoff on quota pressure / errors, capped at 15 minutes.
      backoff=$((backoff * 2))
      [[ "$backoff" -gt 900 ]] && backoff=900
      log "backing off to ${backoff}s"
    fi
    sleep "$backoff"
  done
}

cmd_status() {
  need_jq
  local want_sha=""
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --sha)
        want_sha="${2:-}"
        shift 2
        ;;
      *) shift ;;
    esac
  done
  if [[ ! -f "$RA8_CI_STATE" ]]; then
    echo "overall: UNKNOWN"
    echo "reason:  no status file at $RA8_CI_STATE -- is the daemon running?"
    echo "         start it with: bash scripts/ci_monitor.sh install-service"
    exit "$RA8_CI_EXIT_UNKNOWN"
  fi
  local age_s
  age_s=$(($(date +%s) - $(stat -c %Y "$RA8_CI_STATE" 2>/dev/null || stat -f %m "$RA8_CI_STATE")))
  local overall reason polled
  overall="$(jq -r '.overall' "$RA8_CI_STATE")"
  reason="$(jq -r '.reason // ""' "$RA8_CI_STATE")"
  polled="$(jq -r '.polled_at' "$RA8_CI_STATE")"
  # A stale file is UNKNOWN, not a pass. The daemon may have died.
  if [[ "$age_s" -gt $((RA8_CI_INTERVAL * 5)) ]]; then
    echo "overall: UNKNOWN"
    echo "reason:  status file is ${age_s}s stale (daemon down?) -- last said $overall"
    exit "$RA8_CI_EXIT_UNKNOWN"
  fi
  echo "overall: $overall   (polled $polled, ${age_s}s ago)"
  [[ -n "$reason" ]] && echo "reason:  $reason"
  if [[ -n "$want_sha" ]]; then
    echo "runs for $want_sha:"
    jq -r --arg s "$want_sha" '.runs[] | select(.sha|startswith($s)) | "  \(.name): \(.status)/\(.conclusion // "-")"' "$RA8_CI_STATE"
    # A sha the poller has not seen has no verdict -- do not inherit the
    # branch-level one, which belongs to a different commit.
    local seen
    seen="$(jq -r --arg s "$want_sha" '[.runs[] | select(.sha|startswith($s))] | length' "$RA8_CI_STATE")"
    if [[ "$seen" == "0" ]]; then
      echo "reason:  no run recorded for $want_sha yet"
      exit "$RA8_CI_EXIT_UNKNOWN"
    fi
  else
    jq -r '.runs[:6][] | "  \(.name): \(.status)/\(.conclusion // "-")  \(.sha[0:9])"' "$RA8_CI_STATE"
  fi
  case "$overall" in
    PASS) exit "$RA8_CI_EXIT_PASS" ;;
    FAIL) exit "$RA8_CI_EXIT_FAIL" ;;
    *) exit "$RA8_CI_EXIT_UNKNOWN" ;;
  esac
}

cmd_wait() {
  need_jq
  local want_sha="" timeout=1800 waited=0
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --sha)
        want_sha="${2:-}"
        shift 2
        ;;
      --timeout)
        timeout="${2:-1800}"
        shift 2
        ;;
      *) shift ;;
    esac
  done
  # Polls the LOCAL file, never GitHub -- any number of agents can wait
  # concurrently at zero quota cost.
  while [[ "$waited" -lt "$timeout" ]]; do
    if [[ -f "$RA8_CI_STATE" ]]; then
      local overall sha
      overall="$(jq -r '.overall' "$RA8_CI_STATE" 2>/dev/null)"
      sha="$(jq -r '.head_sha // ""' "$RA8_CI_STATE" 2>/dev/null)"
      if [[ -z "$want_sha" || "$sha" == "$want_sha"* ]]; then
        case "$overall" in
          PASS)
            echo "PASS ($sha)"
            exit "$RA8_CI_EXIT_PASS"
            ;;
          FAIL)
            echo "FAIL ($sha)"
            exit "$RA8_CI_EXIT_FAIL"
            ;;
        esac
      fi
    fi
    sleep 15
    waited=$((waited + 15))
  done
  echo "UNKNOWN: timed out after ${timeout}s waiting for ${want_sha:-head}"
  echo "NOTE: a timeout is NOT a failure. Check: bash scripts/ci_monitor.sh status"
  exit "$RA8_CI_EXIT_UNKNOWN"
}

cmd_install_service() {
  local unitdir="$HOME/.config/systemd/user"
  local self
  self="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"

  # Run the daemon from a STABLE copy for the same reason the reaper does (see
  # agent_workspace.sh cmd_install_timer): install-service is naturally invoked
  # from whichever tree the agent had open, often a workspace that the reaper
  # is entitled to delete. Pointing ExecStart into a reapable directory means
  # the monitor dies the moment its workspace ages out -- and a dead monitor
  # writes no status file, which every reader correctly reports as UNKNOWN.
  # The fleet would then silently lose CI visibility with no obvious cause.
  local stable_dir="$HOME/.local/bin"
  local stable="$stable_dir/ra8-ci-monitor"
  mkdir -p "$stable_dir"
  if [[ "$self" != "$stable" ]]; then
    install -m 0755 "$self" "$stable"
  fi
  self="$stable"

  mkdir -p "$unitdir"
  cat >"$unitdir/ra8-ci-monitor.service" <<EOF
[Unit]
Description=Shared GitHub Actions status poller for the RA8 agent fleet
After=network-online.target

[Service]
Type=simple
Environment=RA8_CI_REPO=$RA8_CI_REPO
Environment=RA8_CI_BRANCH=$RA8_CI_BRANCH
Environment=RA8_CI_INTERVAL=$RA8_CI_INTERVAL
Environment=RA8_CI_QUOTA_RESERVE=$RA8_CI_QUOTA_RESERVE
Environment=RA8_CI_STATE_DIR=$RA8_CI_STATE_DIR
ExecStart=/usr/bin/env bash $self daemon
Restart=always
RestartSec=30

[Install]
WantedBy=default.target
EOF
  systemctl --user daemon-reload
  systemctl --user enable ra8-ci-monitor.service
  # RESTART, not `enable --now`. `--now` starts a stopped unit and does nothing
  # at all to a running one, so re-running install-service after fixing this
  # script left the OLD code running out of $stable with no indication -- the
  # exact silent drift the stable-copy comment above exists to prevent. The
  # daemon is a poller; dropping one cycle to restart it costs nothing.
  systemctl --user restart ra8-ci-monitor.service
  echo "installed and (re)started ra8-ci-monitor.service"
  systemctl --user status ra8-ci-monitor.service --no-pager 2>/dev/null | head -6
}

# Zero-quota fallback: read job outcomes off the runner box itself.
#
# When the REST quota is exhausted the daemon correctly reports UNKNOWN, but
# UNKNOWN is not actionable. The runner writes every job's outcome to its own
# _diag/Worker_*.log, which costs no quota to read -- so a fleet that has burned
# its budget can still SEE its results over ssh.
#
# The log format is version-specific and was established empirically against
# runner 2.335.1 (an earlier attempt failed because the widely-cited
# "Job <name> completed with result: X" pattern is NOT what this version emits).
# The fields actually present, all in Worker_<UTC>.log:
#
#   result   [JobRunner] Job result after all job steps finish: Succeeded
#   job name "jobDisplayName": "board_sim boot smoke"
#   commit   "k": "sha",          followed by   "v": "<40 hex>"
#   branch   "k": "ref_name",     followed by   "v": "dev"
#   workflow "k": "workflow_ref",  followed by   "v": "owner/repo/.github/workflows/<f>.yml@refs/heads/<branch>"
#
# The k/v pairs are on SEPARATE lines inside the job message JSON, hence the
# grep -A1 pairing rather than a single regex.
#
# This reports what the runner box observed. A job that never reached a runner
# (queued, or dispatched to a stopped runner) is invisible here BY DESIGN --
# absence of a log is reported as UNKNOWN, never as a pass.
cmd_runner_status() {
  local host="${RA8_CI_RUNNER_HOST:-k3s-pve}"
  local glob="${RA8_CI_RUNNER_GLOB:-/home/ubuntu/actions-runner*/_diag/Worker_*.log}"
  local want_sha="" limit=10
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --sha)
        want_sha="${2:-}"
        shift 2
        ;;
      --limit)
        limit="${2:-10}"
        shift 2
        ;;
      --host)
        host="${2:-}"
        shift 2
        ;;
      *) shift ;;
    esac
  done

  local out
  # shellcheck disable=SC2029  # deliberate: expand $glob/$limit locally.
  out="$(ssh -o ConnectTimeout=10 -o BatchMode=yes "$host" "
    for f in \$(ls -t $glob 2>/dev/null | head -$limit); do
      res=\$(grep -oE 'Job result after all job steps finish: [A-Za-z]+' \"\$f\" 2>/dev/null | tail -1 | awk '{print \$NF}')
      name=\$(grep -oE '\"jobDisplayName\": \"[^\"]+\"' \"\$f\" 2>/dev/null | head -1 | cut -d'\"' -f4)
      sha=\$(grep -A1 '\"k\": \"sha\"' \"\$f\" 2>/dev/null | grep -oE '[0-9a-f]{40}' | head -1)
      wf=\$(grep -A1 '\"k\": \"workflow_ref\"' \"\$f\" 2>/dev/null | grep -oE '\.github/workflows/[^@\"]+' | head -1 | sed 's|.*/||')
      printf '%s|%s|%s|%s\n' \"\${res:-RUNNING}\" \"\${wf:-?}\" \"\${name:-?}\" \"\${sha:-?}\"
    done" 2>/dev/null)"

  if [[ -z "$out" ]]; then
    echo "overall: UNKNOWN"
    echo "  reason: no runner logs readable on '$host' (ssh failed, or no jobs have run)"
    exit "$RA8_CI_EXIT_UNKNOWN"
  fi

  local shown=0 failed=0 running=0
  echo "source: runner logs on $host (zero GitHub API quota)"
  while IFS='|' read -r res wf name sha; do
    [[ -z "$res" ]] && continue
    if [[ -n "$want_sha" && "$sha" != "$want_sha"* ]]; then continue; fi
    printf '  %-10s %-28s %-34s %s\n' "$res" "$wf" "$name" "${sha:0:9}"
    shown=$((shown + 1))
    case "$res" in
      Succeeded) ;;
      RUNNING) running=$((running + 1)) ;;
      *) failed=$((failed + 1)) ;;
    esac
  done <<<"$out"

  echo "note: reports only what REACHED a runner; a queued job is invisible here."
  # No matching record is UNKNOWN, not a pass: the job may simply not have
  # reached this box yet.
  if [[ "$shown" -eq 0 ]]; then
    echo "overall: UNKNOWN (no runner log matches${want_sha:+ sha $want_sha})"
    exit "$RA8_CI_EXIT_UNKNOWN"
  elif [[ "$failed" -gt 0 ]]; then
    echo "overall: FAIL ($failed failed of $shown observed)"
    exit "$RA8_CI_EXIT_FAIL"
  elif [[ "$running" -gt 0 ]]; then
    echo "overall: UNKNOWN ($running still running of $shown observed)"
    exit "$RA8_CI_EXIT_UNKNOWN"
  fi
  echo "overall: PASS ($shown observed, all Succeeded)"
  exit "$RA8_CI_EXIT_PASS"
}

case "${1:-}" in
  daemon)
    shift
    cmd_daemon "$@"
    ;;
  runner-status)
    shift
    cmd_runner_status "$@"
    ;;
  status)
    shift
    cmd_status "$@"
    ;;
  wait)
    shift
    cmd_wait "$@"
    ;;
  quota)
    shift
    cmd_quota "$@"
    ;;
  install-service)
    shift
    cmd_install_service "$@"
    ;;
  *)
    sed -n '/^# Usage:/,/^$/p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
