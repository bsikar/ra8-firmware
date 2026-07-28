#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/ci/monitor.sh -- ONE shared GitHub Actions watcher for the whole
# agent fleet, replacing per-agent `gh run watch`.
#
# The problem:
#   Each agent ran a blocking `gh run watch` against all three workflows
#   (firmware, coverage, emulator-smoke). Six agents therefore meant ~18
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
# One thing `status` prints that is NOT a verdict: the stalled-queue hint.
# A dead runner pool is indistinguishable from slow CI through this interface --
# both read "queued", both exit UNKNOWN -- and in #484 that cost 16 hours,
# because the runner image had been garbage-collected off the k3s node and
# nothing here could say so. When every run for the newest sha has been sitting
# unstarted for longer than RA8_CI_STALL_MIN minutes, a `warning:` line is
# recorded alongside the verdict pointing at the recovery runbook. It changes
# no exit code: "the queue is not moving" is evidence, not a result.
#
# Usage:
#   bash scripts/ci/monitor.sh daemon            # the single shared poller
#   bash scripts/ci/monitor.sh status [--sha X]  # read cached status (0 quota)
#   bash scripts/ci/monitor.sh wait  --sha X [--timeout 1800]
#   bash scripts/ci/monitor.sh quota             # remaining REST quota
#   bash scripts/ci/monitor.sh install-service   # run the daemon under systemd
#   bash scripts/ci/monitor.sh runner-status     # read outcomes off the runner
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

# The checkout this script lives in, so `runner-status` can ask the fleet
# declaration where a runner box actually is rather than trusting an ssh alias.
RA8_CI_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

RA8_CI_REPO="${RA8_CI_REPO:-bsikar/ra8-firmware}"
RA8_CI_STATE_DIR="${RA8_CI_STATE_DIR:-/var/tmp/ra8-ci-monitor}"
RA8_CI_STATE="$RA8_CI_STATE_DIR/status.json"
# Sparse by design. Jobs here run for many minutes; sub-minute resolution buys
# nothing and is what made 18 watchers expensive.
RA8_CI_INTERVAL="${RA8_CI_INTERVAL:-120}"
# Stop polling well above zero so interactive `gh` use by a human still works.
RA8_CI_QUOTA_RESERVE="${RA8_CI_QUOTA_RESERVE:-500}"
RA8_CI_BRANCH="${RA8_CI_BRANCH:-dev}"
# Minutes an entirely-unstarted queue may sit before the stall hint fires. The
# gates themselves run for tens of minutes, but PICKUP is prompt when the pool
# is healthy -- 15 minutes with nothing started is already abnormal.
RA8_CI_STALL_MIN="${RA8_CI_STALL_MIN:-15}"

mkdir -p "$RA8_CI_STATE_DIR" 2>/dev/null || true

log() { printf '[ci-monitor] %s\n' "$*" >&2; }

# A non-numeric override would make the hint's jq invocation fail and the hint
# would then simply never appear -- a silently disabled warning, which is the
# failure mode this whole file exists to avoid. Say so and fall back.
if [[ ! "$RA8_CI_STALL_MIN" =~ ^[0-9]+$ ]]; then
  log "warning: RA8_CI_STALL_MIN='$RA8_CI_STALL_MIN' is not an integer -- using 15"
  RA8_CI_STALL_MIN=15
fi

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

# Read the status file with python3, NOT jq.
#
# python3 is a hard dependency of this whole CI system -- every gate driver in
# scripts/checks is a python script -- so it is guaranteed present anywhere a
# gate runs. jq is not: it is in neither .devcontainer/Dockerfile nor the
# runner image built FROM it, and the ci-status-contract gate consequently
# failed on every dev head from the moment it landed, with a require_cmd hint
# that wrongly claimed "the CI runners ship it".
#
# Adding jq to the image is the obvious fix and is also being done, but it only
# takes effect after a rebuild and redeploy -- the image-vs-Dockerfile lag
# tracked in #513. Depending on the interpreter that is already guaranteed
# removes the failure mode instead of provisioning around it, and makes
# `make ci-status` work on a bare machine with no jq at all.
#
# SKIPPED IS NOT SUCCESS -- the `verdict` mode below depends on it. Every
# self-hosted job carries the fork guard `if: github.event_name !=
# 'pull_request' || head.repo.full_name == github.repository`; when that is
# false the jobs skip and the run's conclusion is `skipped`. Scoring that as
# PASS meant a run in which NO GATE EXECUTED reported PASS (#530) -- the exact
# shape this file is scrupulous about everywhere else. An all-skipped run is
# UNKNOWN: no verdict could be established, because nothing ran.
#
# A PARTIALLY skipped run still passes. A conditional job that legitimately
# does not apply is normal, and failing on it would cry wolf; the skipped names
# are printed as evidence by `skipped-count` and `lines-sha` instead.
#
# One reader, one place: every field and every rendered view comes from here.
_status_read() {
  python3 - "$RA8_CI_STATE" "$@" <<'PY'
import json
import sys

path, mode = sys.argv[1], sys.argv[2]
arg = sys.argv[3] if len(sys.argv) > 3 else ""
with open(path) as fh:
    doc = json.load(fh)
runs = doc.get("runs") or []


def matching(sha):
    return [r for r in runs if str(r.get("sha") or "").startswith(sha)]


def line(run, with_sha=False):
    tail = "  " + str(run.get("sha") or "")[:9] if with_sha else ""
    return "  %s: %s/%s%s" % (
        run.get("name"),
        run.get("status"),
        run.get("conclusion") or "-",
        tail,
    )


if mode == "field":
    print(doc.get(arg) or "")
elif mode == "count":
    print(len(matching(arg)))
elif mode == "verdict":
    # A sha's verdict comes from that sha's runs and nothing else, and an
    # all-skipped set is UNKNOWN. See the SKIPPED IS NOT SUCCESS note above
    # this function.
    got = matching(arg)
    unfinished = {"failure", "timed_out", "cancelled"}
    if any(r.get("conclusion") in unfinished for r in got):
        print("FAIL")
    elif any(r.get("status") != "completed" for r in got):
        print("UNKNOWN")
    elif got and all(r.get("conclusion") == "skipped" for r in got):
        print("UNKNOWN")
    else:
        print("PASS")
elif mode == "skipped-count":
    print(sum(1 for r in matching(arg) if r.get("conclusion") == "skipped"))
elif mode == "lines-sha":
    for r in matching(arg):
        print(line(r))
elif mode == "lines-head":
    for r in runs[:6]:
        print(line(r, with_sha=True))
else:
    sys.exit("unknown mode: " + mode)
PY
}

# rate_limit does not itself count against the REST quota, so this is safe to
# call every cycle.
quota_remaining() {
  gh api rate_limit --jq '.resources.core.remaining' 2>/dev/null || echo "unknown"
}

# Assert the exit-code contract, in both directions, against a synthetic state
# file. This exists because `status` has twice handed an agent a WRONG verdict:
# once read as FAIL, once as PASS, both times because the branch-level
# "overall:" header was taken for a per-sha answer. A tool nobody has watched
# give a wrong answer on purpose is a tool nobody has tested.
_ci_selftest_case() {
  local want="$1" label="$2"
  shift 2
  local out rc
  out="$(bash "$RA8_CI_SELF" "$@" 2>&1)"
  rc=$?
  if [[ "$rc" != "$want" ]]; then
    echo "FAIL  $label -- expected exit $want, got $rc"
    printf '%s\n' "$out" | sed 's/^/        /'
    return 1
  fi
  echo "ok    $label (exit $rc)"
  return 0
}

# Assert that `status --sha <sha>` EXPLAINS itself, not just that it exits
# right. The exit code alone cannot distinguish "UNKNOWN because nothing ran"
# from "UNKNOWN because the daemon is dead", and a reader who cannot tell
# those apart is back where #530 left them.
_ci_selftest_says() {
  local sha="$1" pattern="$2" label="$3" hits
  hits="$(bash "$0" status --sha "$sha" 2>&1 | grep -cE "$pattern" || true)"
  if [[ "$hits" == "0" ]]; then
    echo "FAIL  $label -- no line matched /$pattern/"
    return 1
  fi
  echo "ok    $label"
  return 0
}

cmd_selftest() {
  local d fails=0
  d="$(mktemp -d)"
  export RA8_CI_STATE_DIR="$d"
  export RA8_CI_SELF="$0"
  cat >"$d/status.json" <<JSON
{"overall":"PASS","reason":"","polled_at":"$(date -u +%Y-%m-%dT%H:%M:%SZ)","runs":[
 {"name":"firmware","status":"completed","conclusion":"success","sha":"aaaaaaaaa1"},
 {"name":"firmware","status":"completed","conclusion":"failure","sha":"bbbbbbbbb2"},
 {"name":"docs","status":"in_progress","conclusion":null,"sha":"ccccccccc3"},
 {"name":"firmware","status":"completed","conclusion":"skipped","sha":"ddddddddd4"},
 {"name":"docs","status":"completed","conclusion":"skipped","sha":"ddddddddd4"},
 {"name":"firmware","status":"completed","conclusion":"success","sha":"eeeeeeeee5"},
 {"name":"hil","status":"completed","conclusion":"skipped","sha":"eeeeeeeee5"}]}
JSON

  _ci_selftest_case 0 "healthy branch head is PASS" status || fails=$((fails + 1))
  _ci_selftest_case 1 "failing sha is FAIL even when the branch head passed" \
    status --sha bbbbbbbbb2 || fails=$((fails + 1))
  _ci_selftest_case 3 "a still-running sha is UNKNOWN, not PASS" status --sha ccccccccc3 || fails=$((fails + 1))
  _ci_selftest_case 3 "an unrecorded sha is UNKNOWN, not PASS" status --sha deadbeef9 || fails=$((fails + 1))

  # #530: skipped is not success. These are the two directions of that rule --
  # all-skipped must NOT pass, and a partial skip must NOT start failing, or
  # the fix would cry wolf on every legitimately-conditional job.
  _ci_selftest_case 3 "an ALL-SKIPPED sha is UNKNOWN, not PASS (no gate executed)" \
    status --sha ddddddddd4 || fails=$((fails + 1))
  _ci_selftest_case 0 "a partially skipped sha still PASSes" \
    status --sha eeeeeeeee5 || fails=$((fails + 1))

  _ci_selftest_says ddddddddd4 'no gate executed' \
    "an all-skipped sha states that no gate executed" || fails=$((fails + 1))
  _ci_selftest_says eeeeeeeee5 '^note:.*skipped' \
    "a partially skipped sha names the jobs that did not execute" || fails=$((fails + 1))

  # The precise trap that burned two agents: an `until` loop grepping for a
  # settled verdict must not match anything while the sha has no runs.
  local hits
  hits="$(bash "$0" status --sha deadbeef9 2>&1 | grep -cE '^overall: (PASS|FAIL)' || true)"
  if [[ "$hits" != "0" ]]; then
    echo "FAIL  unrecorded sha still emits a settled-looking 'overall:' line ($hits)"
    fails=$((fails + 1))
  else
    echo "ok    unrecorded sha emits no settled-looking verdict line"
  fi

  # A stale file means the daemon may be dead; that is UNKNOWN, never a pass.
  touch -t 200001010000 "$d/status.json"
  _ci_selftest_case 3 "a stale status file is UNKNOWN, not PASS" status || fails=$((fails + 1))

  rm -rf "$d"
  if [[ "$fails" -gt 0 ]]; then
    echo "selftest: $fails case(s) FAILED"
    exit 1
  fi
  echo "selftest: all cases passed"
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

# Stall hint for the newest sha, emitted as a JSON string so poll_once can
# splice it straight into the state document. `""` means "nothing to say", and
# every path that cannot decide returns exactly that -- a hint is worth having
# only while it stays quiet in the normal case.
#
# "Stalled" is: every run for the sha is in a pre-start status, and the oldest
# was created more than RA8_CI_STALL_MIN minutes ago. GitHub spells pre-start
# as `queued`, plus `requested` / `waiting` / `pending` for the approval and
# concurrency gates; all of them mean no runner has picked the job up. One run
# already `in_progress` proves the pool is alive, so the hint stays silent.
stall_hint() {
  local runs="$1" head_sha="$2" hint
  hint="$(printf '%s' "$runs" | jq -c --arg s "$head_sha" --argjson lim "$RA8_CI_STALL_MIN" '
      [ .[] | select(.sha == $s) ] as $cur
      | if ($cur | length) == 0 then ""
        elif any($cur[]; .created == null) then ""
        elif any($cur[]; .status != "queued" and .status != "requested"
                         and .status != "waiting" and .status != "pending") then ""
        else
          ([ $cur[] | .created | fromdateiso8601 ] | min) as $oldest
          | (((now - $oldest) / 60) | floor) as $age
          | if $age < $lim then ""
            else
              "warning: all \($cur | length) run(s) for \($s[0:9]) queued "
              + "\($age)m (>\($lim)m) -- runner pool likely stalled, nothing is "
              + "picking jobs up. Recovery: issue #484 and the \"Emergency "
              + "recovery\" section of infra/images/README.md."
            end
        end' 2>/dev/null)"
  # jq failing (malformed timestamp, ancient state document) must not corrupt
  # the state file with an empty splice, so normalise to the JSON empty string.
  [[ -z "$hint" ]] && hint='""'
  printf '%s' "$hint"
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

  # ONE request covers every workflow. `created` is carried because it is the
  # enqueue time -- `updated` moves for reasons unrelated to being picked up,
  # so it cannot measure how long a job has been waiting for a runner.
  local runs
  runs="$(gh api "/repos/$RA8_CI_REPO/actions/runs?branch=$RA8_CI_BRANCH&per_page=30" \
    --jq '[.workflow_runs[] | {name:.name, sha:.head_sha, status:.status, conclusion:.conclusion, url:.html_url, created:.created_at, updated:.updated_at}]' \
    2>/dev/null)"
  if [[ -z "$runs" ]]; then
    write_state "$(printf '{"polled_at":"%s","quota":%s,"overall":"UNKNOWN","reason":"query failed","runs":[]}' \
      "$(date -Iseconds)" "$remaining")"
    return 1
  fi

  # Overall verdict covers only the newest sha seen, so a stale older red does
  # not mask a current green (or vice versa).
  #
  # The all-skipped arm is the #530 fix and must stay AHEAD of the success
  # arm. `all(... =="success" or =="skipped")` alone scored a run in which
  # every job skipped -- the fork-guard case, where no gate executed at all --
  # as PASS. Nothing ran, so there is no verdict: that is UNKNOWN. A partially
  # skipped run still passes; a conditional job that does not apply is normal
  # and failing on it would cry wolf.
  local head_sha overall
  head_sha="$(printf '%s' "$runs" | jq -r 'if length>0 then .[0].sha else "" end')"
  overall="$(printf '%s' "$runs" | jq -r --arg s "$head_sha" '
      [ .[] | select(.sha==$s) ] as $cur
      | if ($cur|length)==0 then "UNKNOWN"
        elif any($cur[]; .conclusion=="failure" or .conclusion=="timed_out" or .conclusion=="cancelled") then "FAIL"
        elif all($cur[]; .status=="completed") and all($cur[]; .conclusion=="skipped") then "UNKNOWN"
        elif all($cur[]; .status=="completed") and all($cur[]; .conclusion=="success" or .conclusion=="skipped") then "PASS"
        else "RUNNING" end')"

  # Evidence, not verdict: `overall` above is untouched by the hint.
  local warning
  warning="$(stall_hint "$runs" "$head_sha")"

  write_state "$(printf '{"polled_at":"%s","quota":%s,"branch":"%s","head_sha":"%s","overall":"%s","warning":%s,"runs":%s}' \
    "$(date -Iseconds)" "$remaining" "$RA8_CI_BRANCH" "$head_sha" "$overall" "$warning" "$runs")"
  log "polled: $overall (sha ${head_sha:0:9}, quota $remaining)"
  if [[ "$warning" != '""' ]]; then
    log "$(printf '%s' "$warning" | jq -r . 2>/dev/null)"
  fi
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

# Report the verdict for ONE sha, derived from that sha's runs and nothing
# else, and exit. The single "overall:" line printed here is the only one the
# caller sees in this mode.
#
# That exclusivity is not cosmetic. Emitting the branch-level header first has
# twice been misread as the per-sha answer -- once as a false FAIL, once as a
# false PASS by an `until` loop grepping '^overall: (PASS|FAIL)', which matched
# the header on the very first poll and exited before the run existed. Deciding
# on the branch "$overall" was also wrong outright: an older sha whose runs
# failed would inherit a PASS from a healthy branch head.
_status_for_sha() {
  local want_sha="$1" polled="$2" age_s="$3" warning="$4"
  local seen sha_verdict
  seen="$(_status_read count "$want_sha")"
  if [[ "$seen" == "0" ]]; then
    echo "overall: UNKNOWN   for $want_sha (polled $polled, ${age_s}s ago)"
    echo "reason:  no run recorded for $want_sha yet -- UNKNOWN is not a pass and not a failure"
    echo "         the daemon tracks pushes to dev/main, so a PR-event run on a"
    echo "         feature branch is never recorded here; use 'gh pr checks <n>'"
    [[ -n "$warning" ]] && echo "$warning"
    exit "$RA8_CI_EXIT_UNKNOWN"
  fi
  sha_verdict="$(_status_read verdict "$want_sha")"
  echo "overall: $sha_verdict   for $want_sha (polled $polled, ${age_s}s ago)"
  local skipped
  skipped="$(_status_read skipped-count "$want_sha")"
  if [[ "$skipped" != "0" ]]; then
    if [[ "$skipped" == "$seen" ]]; then
      echo "reason:  all $seen run(s) SKIPPED -- no gate executed, so there is no verdict."
      echo "         Usually the fork guard on the self-hosted jobs; re-run on a"
      echo "         branch in this repository. UNKNOWN is not a pass."
    else
      echo "note:    $skipped of $seen run(s) skipped and did not execute (listed below)"
    fi
  fi
  _status_read lines-sha "$want_sha"
  [[ -n "$warning" ]] && echo "$warning"
  case "$sha_verdict" in
    PASS) exit "$RA8_CI_EXIT_PASS" ;;
    FAIL) exit "$RA8_CI_EXIT_FAIL" ;;
    *) exit "$RA8_CI_EXIT_UNKNOWN" ;;
  esac
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
    echo "         start it with: bash scripts/ci/monitor.sh install-service"
    exit "$RA8_CI_EXIT_UNKNOWN"
  fi
  local age_s
  age_s=$(($(date +%s) - $(stat -c %Y "$RA8_CI_STATE" 2>/dev/null || stat -f %m "$RA8_CI_STATE")))
  local overall reason polled
  overall="$(_status_read field overall)"
  reason="$(_status_read field reason)"
  polled="$(_status_read field polled_at)"
  # A stale file is UNKNOWN, not a pass. The daemon may have died.
  if [[ "$age_s" -gt $((RA8_CI_INTERVAL * 5)) ]]; then
    echo "overall: UNKNOWN"
    echo "reason:  status file is ${age_s}s stale (daemon down?) -- last said $overall"
    exit "$RA8_CI_EXIT_UNKNOWN"
  fi
  local warning
  warning="$(_status_read field warning)"

  if [[ -n "$want_sha" ]]; then
    _status_for_sha "$want_sha" "$polled" "$age_s" "$warning"
  fi

  echo "overall: $overall   (polled $polled, ${age_s}s ago)"
  [[ -n "$reason" ]] && echo "reason:  $reason"
  # Printed next to the verdict, never instead of it -- see the header note.
  [[ -n "$warning" ]] && echo "$warning"
  _status_read lines-head
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
  echo "NOTE: a timeout is NOT a failure. Check: bash scripts/ci/monitor.sh status"
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
Environment=RA8_CI_STALL_MIN=$RA8_CI_STALL_MIN
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
# This needs a LONG-LIVED runner: an ARC pod is ephemeral (one pod per job) and
# its _diag tree dies with it, so the ra8-ci scale set on the k3s node can never
# serve this. The defaults therefore point at the truenas container runner
# (infra/ansible/roles/ci_runner_docker), whose _work / _diag live on a dataset
# outside the container and survive it. That is also a strict improvement on
# where these defaults used to point -- /home/ubuntu/actions-runner*/ on the
# k3s node, the bare-metal `k3s-runner-*` pool, which by the end only ever
# served docs-publish / fuzz-nightly / osv-scan and so could not show a
# `firmware` result at all. That pool is retired.
#
# The log format is version-specific and was established empirically against
# runner 2.335.1 (an earlier attempt failed because the widely-cited
# "Job <name> completed with result: X" pattern is NOT what this version emits).
# The fields actually present, all in Worker_<UTC>.log:
#
#   result   [JobRunner] Job result after all job steps finish: Succeeded
#   job name "jobDisplayName": "ra8_emulator boot smoke"
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
# Pull the most recent job records straight off the runner's own worker logs.
# One record per line, "result|workflow|job|sha". Costs zero GitHub API quota,
# which is the whole point of this command.

# The ssh argv that reaches a runner box.
#
# A fleet host is dialled through infra/fleet.yml -- its declared address, user
# and jump -- NOT by the bare name. `truenas` is an ~/.ssh/config alias on
# exactly one laptop, so this command (the one CLAUDE.md sends you to when the
# GitHub quota is gone) died on "Could not resolve hostname truenas" from the
# dev box while the machine answered fine on its address (#526). Anything that
# is not a declared host is passed through as a raw ssh destination, so
# --host user@1.2.3.4 still works for a box outside the fleet.
_runner_ssh_argv() {
  local host="$1" argv
  if argv="$(python3 "${RA8_CI_ROOT}/scripts/dev/fleet.py" ssh-target "$host" 2>/dev/null)"; then
    # Word splitting is deliberate and safe: the fleet-declaration gate rejects
    # an address, user or jump containing whitespace.
    # shellcheck disable=SC2206
    RA8_RUNNER_SSH=(${argv})
    return 0
  fi
  RA8_RUNNER_SSH=(ssh -o ConnectTimeout=10 -o BatchMode=yes "$host")
}

_runner_status_fetch() {
  local host="$1" glob="$2" limit="$3"
  _runner_ssh_argv "$host"
  # shellcheck disable=SC2029  # deliberate: expand $glob/$limit locally.
  "${RA8_RUNNER_SSH[@]}" "
    for f in \$(ls -t $glob 2>/dev/null | head -$limit); do
      res=\$(grep -oE 'Job result after all job steps finish: [A-Za-z]+' \"\$f\" 2>/dev/null | tail -1 | awk '{print \$NF}')
      name=\$(grep -oE '\"jobDisplayName\": \"[^\"]+\"' \"\$f\" 2>/dev/null | head -1 | cut -d'\"' -f4)
      sha=\$(grep -A1 '\"k\": \"sha\"' \"\$f\" 2>/dev/null | grep -oE '[0-9a-f]{40}' | head -1)
      wf=\$(grep -A1 '\"k\": \"workflow_ref\"' \"\$f\" 2>/dev/null | grep -oE '\.github/workflows/[^@\"]+' | head -1 | sed 's|.*/||')
      printf '%s|%s|%s|%s\n' \"\${res:-RUNNING}\" \"\${wf:-?}\" \"\${name:-?}\" \"\${sha:-?}\"
    done" 2>/dev/null
}

# Print the matching records and exit with the overall verdict.
#
# No matching record is UNKNOWN, not a pass: the job may simply not have
# reached this box yet. That distinction is the reason this command exists,
# so it is the one thing the reporting half must never blur.
_runner_status_report() {
  local want_sha="$1" host="$2" out="$3"
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

cmd_runner_status() {
  local host="${RA8_CI_RUNNER_HOST:-truenas}"
  local glob="${RA8_CI_RUNNER_GLOB:-/mnt/stripe/ci-runner/home/_diag/Worker_*.log}"
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
  out="$(_runner_status_fetch "$host" "$glob" "$limit")"

  if [[ -z "$out" ]]; then
    echo "overall: UNKNOWN"
    echo "  reason: no runner logs readable on '$host' (ssh failed, or no jobs have run)"
    exit "$RA8_CI_EXIT_UNKNOWN"
  fi

  _runner_status_report "$want_sha" "$host" "$out"
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
  selftest)
    shift
    cmd_selftest "$@"
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
