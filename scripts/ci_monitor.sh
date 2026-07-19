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
#      chasing ghosts.
#   2. Back off BEFORE exhaustion, not after, so the fleet degrades gracefully
#      and a reserve is left for the interactive work a human is doing.
#   3. Agents should not BLOCK on CI at all. `make ci` runs a superset of the
#      workflows locally, so GitHub is confirmation, not discovery. `wait`
#      exists for the rare case that genuinely needs it and is bounded.
#
# Usage:
#   bash scripts/ci_monitor.sh daemon            # the single shared poller
#   bash scripts/ci_monitor.sh status [--sha X]  # read cached status (0 quota)
#   bash scripts/ci_monitor.sh wait  --sha X [--timeout 1800]
#   bash scripts/ci_monitor.sh quota             # remaining REST quota
#   bash scripts/ci_monitor.sh install-service   # run the daemon under systemd

set -uo pipefail

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
    exit 1
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
  head_sha="$(printf '%s' "$runs" | gh_jq 'if length>0 then .[0].sha else "" end')"
  overall="$(printf '%s' "$runs" | gh_jq --arg s "$head_sha" '
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

# jq is not guaranteed present; gh bundles one for --jq but not for stdin. Use
# jq when available and fall back to gh's embedded filter via a temp round-trip.
gh_jq() {
  if command -v jq >/dev/null 2>&1; then
    jq -r "$@"
  else
    python3 -c 'import sys,json;d=json.load(sys.stdin);print(json.dumps(d))' 2>/dev/null
  fi
}

cmd_daemon() {
  need_gh
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
    exit 3
  fi
  local age_s
  age_s=$(($(date +%s) - $(stat -c %Y "$RA8_CI_STATE" 2>/dev/null || stat -f %m "$RA8_CI_STATE")))
  if command -v jq >/dev/null 2>&1; then
    local overall reason polled
    overall="$(jq -r '.overall' "$RA8_CI_STATE")"
    reason="$(jq -r '.reason // ""' "$RA8_CI_STATE")"
    polled="$(jq -r '.polled_at' "$RA8_CI_STATE")"
    # A stale file is UNKNOWN, not a pass. The daemon may have died.
    if [[ "$age_s" -gt $((RA8_CI_INTERVAL * 5)) ]]; then
      echo "overall: UNKNOWN"
      echo "reason:  status file is ${age_s}s stale (daemon down?) -- last said $overall"
      exit 3
    fi
    echo "overall: $overall   (polled $polled, ${age_s}s ago)"
    [[ -n "$reason" ]] && echo "reason:  $reason"
    if [[ -n "$want_sha" ]]; then
      echo "runs for $want_sha:"
      jq -r --arg s "$want_sha" '.runs[] | select(.sha|startswith($s)) | "  \(.name): \(.status)/\(.conclusion // "-")"' "$RA8_CI_STATE"
    else
      jq -r '.runs[:6][] | "  \(.name): \(.status)/\(.conclusion // "-")  \(.sha[0:9])"' "$RA8_CI_STATE"
    fi
    case "$overall" in
      PASS) exit 0 ;;
      FAIL) exit 1 ;;
      *) exit 3 ;;
    esac
  else
    cat "$RA8_CI_STATE"
  fi
}

cmd_wait() {
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
    if [[ -f "$RA8_CI_STATE" ]] && command -v jq >/dev/null 2>&1; then
      local overall sha
      overall="$(jq -r '.overall' "$RA8_CI_STATE" 2>/dev/null)"
      sha="$(jq -r '.head_sha // ""' "$RA8_CI_STATE" 2>/dev/null)"
      if [[ -z "$want_sha" || "$sha" == "$want_sha"* ]]; then
        case "$overall" in
          PASS)
            echo "PASS ($sha)"
            exit 0
            ;;
          FAIL)
            echo "FAIL ($sha)"
            exit 1
            ;;
        esac
      fi
    fi
    sleep 15
    waited=$((waited + 15))
  done
  echo "UNKNOWN: timed out after ${timeout}s waiting for ${want_sha:-head}"
  echo "NOTE: a timeout is NOT a failure. Check: bash scripts/ci_monitor.sh status"
  exit 3
}

cmd_install_service() {
  local unitdir="$HOME/.config/systemd/user"
  local self
  self="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
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
  systemctl --user enable --now ra8-ci-monitor.service
  echo "installed ra8-ci-monitor.service"
  sleep 3
  systemctl --user status ra8-ci-monitor.service --no-pager 2>/dev/null | head -8
}

case "${1:-}" in
  daemon)
    shift
    cmd_daemon "$@"
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
