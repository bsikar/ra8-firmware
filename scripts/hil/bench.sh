#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# bench.sh -- who is allowed to touch the EK-RA8D2 right now.
#
# The bench is one physical assembly: the RA8D2, the ESP32-C6 soldered to
# Pmod1, the J-Link OB (which is also the console), the hub ports those hang
# off, and the Tapo plug that powers all of it. Nothing about it is divisible,
# so there is exactly ONE lock and it covers the lot. The record carries a
# `resource` field from day one so `bench/board` and `bench/instruments` can
# split later without a data migration, but building sub-locks now would model
# a concurrency that does not exist.
#
# THE LOCK IS A KERNEL FLOCK ON THE BENCH HOST. That is the whole design:
# a holder is a live process, and when it dies -- killed, session-limited,
# ssh dropped, host rebooted -- the kernel drops the lock at once. There is no
# TTL to expire and no heartbeat to miss on the correctness path, so a stale
# lock is not a state that exists. This file is only the client; every lock
# operation happens in scripts/hil/lib/bench_host.sh, on the bench host, using
# the bench host's clock.
#
# Verbs:
#   status                  who has it (0 free / 1 held / 3 UNKNOWN)
#   run [opts] -- <cmd>     hold the bench for exactly as long as <cmd> runs
#   acquire [opts]          take a DETACHED hold (needs --for; see below)
#   release                 give back a hold this actor owns
#   selftest [--ssh-death]  prove the mechanism, including the load-bearing
#                           claim that killing the ssh CLIENT drops the lock
#   help
#
# Exit codes are the contract and there are exactly three, matching
# scripts/ci/monitor.sh:
#   0  free / acquired / OK
#   1  held / denied
#   3  UNKNOWN -- no verdict could be established (host unreachable, state
#      directory missing, record unreadable). NEVER report it as pass or fail.
#
# `run` is the exception, and deliberately: it exits with its PAYLOAD's status,
# because a wrapper that swallowed the exit code of what it wrapped would be
# useless. 1 and 3 from `run` mean the lock, only when the payload never ran --
# which it says on stderr.
#
# WHAT IS PROVEN, AND WHAT IS NOT
# -------------------------------
# The no-stale-lock property depends on one claim that ssh does NOT provide in
# general: that killing the client reaps the remote payload. It holds here only
# because the payload blocks on its stdin, which IS the ssh channel. That is
# measured, not assumed -- `bench.sh selftest --ssh-death` SIGKILLs a real ssh
# client and asserts the flock drops. Measured on the bench: released in 0.29 s,
# with the remote holder process gone, while this end of the pipe was still
# deliberately open.
#
# The residual case is a client that vanishes WITHOUT its socket closing -- a
# yanked power cable, not a killed process. No FIN reaches the bench host, so
# the release waits on sshd noticing. ServerAliveInterval below bounds it from
# this side, but only sshd's ClientAliveInterval bounds it from the other; with
# the default of 0 that falls back to TCP keepalive, i.e. hours. The bench
# host's sshd therefore sets ClientAliveInterval (provisioned by the hil_bench
# ansible role) and `bench.sh doctor` reports it when it is missing, because an
# unbounded release there would be exactly the stale lock this design exists to
# rule out.
#
# Like monitor.sh this runs `set -uo pipefail` and deliberately NOT `set -e`:
# with 1 meaning "denied", an unrelated abort exiting 1 would silently become a
# verdict.
set -uo pipefail

readonly RA8_BENCH_EXIT_FREE=0
readonly RA8_BENCH_EXIT_HELD=1
readonly RA8_BENCH_EXIT_UNKNOWN=3

_bench_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly RA8_BENCH_HOST_SRC="$_bench_dir/lib/bench_host.sh"

# PI_HOST and rig_is_local_pi come from the gitignored .env via rig_env.sh, so
# no maintainer-specific host lands in the tree.
# shellcheck source=scripts/hil/lib/rig_env.sh
source "$_bench_dir/lib/rig_env.sh"

# Overridable ONLY so the selftest can exercise the machinery against a
# throwaway directory. Production always uses the canonical path, because two
# actors flocking two different inodes is the one silent-corruption path here.
RA8_BENCH_DIR="${RA8_BENCH_DIR:-/var/lib/ra8-bench}"

# Budgets. Advisory, never load-bearing: they decide when a hold is REPORTED as
# overrun and when the reaper acts, and nothing else.
#   900s  -- worst single app is a 240 s verify plus flash plus the rfp-cli
#            retry path, with headroom.
#   7200s -- the 90-minute CI cap for the whole 151-app suite, plus slack.
#   8h    -- the hard cap on any detached hold, human or otherwise.
readonly RA8_BENCH_DEFAULT_HOLD_S=900
readonly RA8_BENCH_MAX_DETACHED_S=28800

# ---------------------------------------------------------------------------
# The client-side machinery: transport to the bench host, identity, and the
# holder lifecycle. Shared with the sourced guard HIL scripts call, so there is
# exactly one implementation of "how a hold is taken".
# ---------------------------------------------------------------------------
# shellcheck source=scripts/hil/lib/bench_client.sh
source "$_bench_dir/lib/bench_client.sh"

# ---------------------------------------------------------------------------
# status
# ---------------------------------------------------------------------------

cmd_status() {
  local probe rc
  probe="$(bench_host probe 2>&1)"
  rc=$?
  # rc 0 = free, 1 = held, 3 = anything we could not establish. An ssh failure
  # lands here too, and lands as UNKNOWN, which is the point.
  if [ "$rc" -ne 0 ] && [ "$rc" -ne 1 ]; then
    printf 'bench:    UNKNOWN\n'
    printf 'reason:   could not reach the bench host or read its state\n'
    printf '%s\n' "$probe" | sed 's/^/          /'
    return "$RA8_BENCH_EXIT_UNKNOWN"
  fi
  local state
  state="$(bench_field "$probe" state UNKNOWN)"

  if [ "$state" = "UNKNOWN" ]; then
    printf 'bench:    UNKNOWN\n'
    printf 'reason:   %s\n' "$(bench_field "$probe" reason 'no verdict could be established')"
    return "$RA8_BENCH_EXIT_UNKNOWN"
  fi

  if [ "$state" = "HELD" ]; then
    local age left budget
    budget="$(bench_field "$probe" f_max_hold_s 0)"
    age=$(($(bench_field "$probe" now_epoch 0) - $(bench_field "$probe" f_acquired_epoch 0)))
    left=$((budget - age))
    printf 'bench:    HELD\n'
    printf 'holder:   %s (%s)\n' "$(bench_field "$probe" f_holder_name '?')" \
      "$(bench_field "$probe" f_holder_class '?')"
    printf 'intent:   %s\n' "$(bench_field "$probe" f_intent '<none stated>')"
    printf 'kind:     %s\n' \
      "$(bench_hold_kind_text "$(bench_field "$probe" f_hold_kind wrapped)")"
    printf 'since:    %s (%s ago)\n' "$(bench_field "$probe" f_acquired_at '?')" \
      "$(bench_human_age "$age")"
    if [ "$left" -ge 0 ]; then
      printf 'budget:   %s (%s left)\n' "$(bench_human_age "$budget")" \
        "$(bench_human_age "$left")"
    else
      printf 'budget:   %s (OVERRUN by %s)\n' "$(bench_human_age "$budget")" \
        "$(bench_human_age $((-left)))"
    fi
    printf 'origin:   %s\n' "$(bench_field "$probe" f_origin '?')"
    printf 'activity: %s\n' "$(bench_field "$probe" f_last_activity '?')"
    if [ "$(bench_field "$probe" f_break_glass false)" = "true" ]; then
      printf 'note:     taken with --break-glass\n'
    fi
    bench_print_health "$probe"
    return "$RA8_BENCH_EXIT_HELD"
  fi

  printf 'bench:    FREE\n'
  local last_who
  last_who="$(bench_field "$probe" last_release_who)"
  if [ -n "$last_who" ]; then
    printf 'last:     %s released %s ("%s")\n' "$last_who" \
      "$(bench_field "$probe" last_release_at '?')" \
      "$(bench_field "$probe" last_release_note)"
  fi
  if [ "$(bench_field "$probe" rec_stale_boot 0)" = "1" ]; then
    printf 'note:     a record from before the last reboot was ignored (boot_id changed)\n'
  fi
  bench_print_health "$probe"
  return "$RA8_BENCH_EXIT_FREE"
}

# A hold that dies with its command is a different promise from one that
# survives the laptop closing, and the difference is the whole reason detached
# holds are capped. Say which is in force rather than making people infer it.
bench_hold_kind_text() {
  case "$1" in
    wrapped) printf 'wrapped -- bound to the holding process; dies with it' ;;
    detached) printf 'DETACHED -- survives its command; bounded only by the budget' ;;
    *) printf '%s' "$1" ;;
  esac
}

# Evidence channel, never a verdict: it changes no exit code, exactly like
# monitor.sh's stalled-queue `warning:` line.
bench_print_health() {
  local probe="$1" evidence
  evidence="$(bench_field "$probe" activity_evidence)"
  if [ "$(bench_field "$probe" health OK)" = "UNLOCKED-ACTIVITY" ]; then
    printf 'health:   UNLOCKED-ACTIVITY -- hardware is being touched with no lock held.\n'
    printf '          %s\n' "$evidence"
    printf '          Someone is touching the board without taking the bench first.\n'
    return "$RA8_BENCH_EXIT_FREE"
  fi
  printf 'health:   OK\n'
  if [ -n "$evidence" ]; then
    printf 'evidence: %s\n' "$evidence"
  fi
}

# ---------------------------------------------------------------------------
# run
# ---------------------------------------------------------------------------

cmd_run() {
  local intent="" budget_s="$RA8_BENCH_DEFAULT_HOLD_S" wait_s=0 cls="" name=""
  local glass=false confirm="${CONFIRM:-}"
  while [ $# -gt 0 ]; do
    case "$1" in
      --intent)
        intent="${2:-}"
        shift 2
        ;;
      --for)
        budget_s="$(bench_duration "${2:-}")"
        shift 2
        ;;
      --wait)
        wait_s="$(bench_duration "${2:-}")"
        shift 2
        ;;
      --as)
        cls="${2:-}"
        shift 2
        ;;
      --name)
        name="${2:-}"
        shift 2
        ;;
      --break-glass)
        glass=true
        shift
        ;;
      --confirm)
        confirm="${2:-}"
        shift 2
        ;;
      --)
        shift
        break
        ;;
      *)
        bench_say "unknown option '$1' (did you forget the -- before the command?)"
        return "$RA8_BENCH_EXIT_UNKNOWN"
        ;;
    esac
  done
  [ $# -gt 0 ] || {
    bench_say "usage: bench.sh run --intent \"...\" -- <command> [args]"
    return "$RA8_BENCH_EXIT_UNKNOWN"
  }
  [ -n "$intent" ] || {
    bench_say "--intent is required. A lock you cannot read at a glance is a lock"
    bench_say "people force-take blindly."
    return "$RA8_BENCH_EXIT_UNKNOWN"
  }
  [ -n "$budget_s" ] || {
    bench_say "--for: could not parse a duration (want 900s / 15m / 2h)"
    return "$RA8_BENCH_EXIT_UNKNOWN"
  }
  [ -n "$wait_s" ] || wait_s=0
  [ -n "$cls" ] || cls="$(bench_default_class)"
  [ -n "$name" ] || name="$(bench_default_name "$cls")"

  local tmp lock_id holder rc
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/ra8-bench.XXXXXX")" || return "$RA8_BENCH_EXIT_UNKNOWN"
  mkfifo "$tmp/hold.in" || {
    rm -rf "$tmp"
    return "$RA8_BENCH_EXIT_UNKNOWN"
  }
  : >"$tmp/hold.out"
  lock_id="$(bench_new_lock_id)"

  bench_start_holder "$lock_id" "$cls" "$name" "$intent" "$budget_s" \
    "$wait_s" "$glass" "$tmp/hold.in" "$tmp/hold.out" "$confirm"
  holder="$RA8_BENCH_HOLDER_PID"
  if [ -z "$holder" ]; then
    bench_say "could not start a holder (PI_HOST unset, or the host half is unreadable)"
    rm -rf "$tmp"
    return "$RA8_BENCH_EXIT_UNKNOWN"
  fi
  # Opening the write end rendezvouses with the holder's read end. From here
  # on, this shell dying closes the pipe, which is the release.
  exec 8>"$tmp/hold.in"

  bench_await_ack "$lock_id" "$tmp/hold.out" "$holder" $((wait_s + 30))
  rc=$?
  if [ "$rc" -eq "$RA8_BENCH_EXIT_HELD" ]; then
    bench_report_denial "$tmp/hold.out"
    exec 8>&-
    wait "$holder" 2>/dev/null
    rm -rf "$tmp"
    return "$RA8_BENCH_EXIT_HELD"
  fi
  if [ "$rc" -ne 0 ]; then
    bench_say "UNKNOWN -- could not establish a hold. Refusing to touch the bench."
    sed 's/^/          /' "$tmp/hold.out" >&2 2>/dev/null
    exec 8>&-
    wait "$holder" 2>/dev/null
    rm -rf "$tmp"
    return "$RA8_BENCH_EXIT_UNKNOWN"
  fi

  bench_say "holding ($lock_id, $name [$cls]) -- $intent"
  RA8_BENCH_LOCK_ID="$lock_id" RA8_BENCH_HOLDER_PID="$holder" \
    RA8_BENCH_INTENT="$intent" bench_supervise "$holder" "$@"
  rc=$?

  exec 8>&-
  wait "$holder" 2>/dev/null
  rm -rf "$tmp"
  return "$rc"
}

# Run the payload, and FENCE it: if the holder dies while the payload is still
# going, the bench is no longer ours and the payload must stop touching it.
# stdin is passed through on fd 3 so the payload is not silently detached from
# the terminal the way a plain background job would be.
bench_supervise() {
  local holder="$1"
  shift
  local pay rc
  exec 3<&0
  "$@" <&3 &
  pay=$!
  while :; do
    if ! kill -0 "$pay" 2>/dev/null; then
      wait "$pay"
      rc=$?
      break
    fi
    if ! kill -0 "$holder" 2>/dev/null; then
      bench_say "the bench hold died while the payload was running -- stopping it."
      pkill -TERM -P "$pay" 2>/dev/null || true
      kill -TERM "$pay" 2>/dev/null || true
      sleep 5
      pkill -KILL -P "$pay" 2>/dev/null || true
      kill -KILL "$pay" 2>/dev/null || true
      wait "$pay" 2>/dev/null
      rc="$RA8_BENCH_EXIT_UNKNOWN"
      break
    fi
    sleep 1
  done
  return "$rc"
}

# ---------------------------------------------------------------------------
# acquire / release -- the detached pair
# ---------------------------------------------------------------------------

cmd_acquire() {
  local intent="" budget_s="" wait_s=0 cls="" name="" glass=false confirm=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --intent)
        intent="${2:-}"
        shift 2
        ;;
      --for)
        budget_s="$(bench_duration "${2:-}")"
        shift 2
        ;;
      --wait)
        wait_s="$(bench_duration "${2:-}")"
        shift 2
        ;;
      --as)
        cls="${2:-}"
        shift 2
        ;;
      --name)
        name="${2:-}"
        shift 2
        ;;
      --run-url)
        RA8_BENCH_RUN_URL="${2:-}"
        export RA8_BENCH_RUN_URL
        shift 2
        ;;
      --break-glass)
        glass=true
        shift
        ;;
      --confirm)
        confirm="${2:-}"
        shift 2
        ;;
      *)
        bench_say "unknown option '$1'"
        return "$RA8_BENCH_EXIT_UNKNOWN"
        ;;
    esac
  done
  [ -n "$intent" ] || {
    bench_say "--intent is required."
    return "$RA8_BENCH_EXIT_UNKNOWN"
  }
  # A detached hold is the ONE shape that outlives the process that took it, so
  # it is the one shape a TTL is unavoidable for -- and therefore the one shape
  # that may not have a default. A default is what gets forgotten.
  [ -n "$budget_s" ] || {
    bench_say "--for is MANDATORY for a detached hold: it survives the command"
    bench_say "that took it, so nothing but the budget bounds it. Cap is 8h."
    bench_say "If you want a hold that dies with your command, use \`bench.sh run\`."
    return "$RA8_BENCH_EXIT_UNKNOWN"
  }
  if [ "$budget_s" -gt "$RA8_BENCH_MAX_DETACHED_S" ]; then
    bench_say "--for $budget_s s exceeds the 8h cap on a detached hold."
    return "$RA8_BENCH_EXIT_UNKNOWN"
  fi
  [ -n "$wait_s" ] || wait_s=0
  [ -n "$cls" ] || cls="$(bench_default_class)"
  [ -n "$name" ] || name="$(bench_default_name "$cls")"

  local lock_id fields cmd rc
  lock_id="$(bench_new_lock_id)"
  fields="$(bench_fields_b64 "$lock_id" "$cls" "$name" "$intent" "$budget_s" detached \
    "$glass" "$confirm")"
  cmd="$(bench_host_cmd hold detached "$wait_s" "$fields")" || return "$RA8_BENCH_EXIT_UNKNOWN"

  if rig_is_local_pi; then
    local t
    t="$(mktemp "${TMPDIR:-/tmp}/ra8-bench-host.XXXXXX")" || return "$RA8_BENCH_EXIT_UNKNOWN"
    cp "$RA8_BENCH_HOST_SRC" "$t" || return "$RA8_BENCH_EXIT_UNKNOWN"
    RA8_BENCH_DIR="$RA8_BENCH_DIR" setsid bash "$t" hold detached "$wait_s" "$fields" \
      </dev/null >/dev/null 2>&1 &
  else
    # shellcheck disable=SC2029  # $cmd is composed here on purpose (see bench_host_cmd) and quoted for the remote shell by bench_q.
    ssh "${RA8_BENCH_SSH_OPTS[@]}" "$PI_HOST" \
      "setsid nohup bash -c $(bench_q "$cmd") </dev/null >/dev/null 2>&1 &" \
      </dev/null >/dev/null 2>&1
  fi

  # Confirm by OBSERVING the lock, not by trusting that we launched something.
  local waited=0 limit=$((wait_s + 20)) seen
  while [ "$waited" -lt "$limit" ]; do
    seen="$(bench_lock_id_now)"
    if [ "$seen" = "$lock_id" ]; then
      printf '%s\n' "$lock_id"
      bench_say "acquired ($name [$cls], detached, budget $(bench_human_age "$budget_s")) -- $intent"
      bench_say "release it with: make bench-free"
      return "$RA8_BENCH_EXIT_FREE"
    fi
    sleep 1
    waited=$((waited + 1))
  done
  rc=0
  cmd_status >/dev/null 2>&1 || rc=$?
  if [ "$rc" -eq "$RA8_BENCH_EXIT_HELD" ]; then
    bench_say "DENIED -- somebody else holds the bench."
    cmd_status >&2
    return "$RA8_BENCH_EXIT_HELD"
  fi
  bench_say "UNKNOWN -- the hold did not appear within ${limit}s."
  return "$RA8_BENCH_EXIT_UNKNOWN"
}

# The lock_id currently recorded on the bench host, or nothing.
#
# The probe is captured before it is filtered, deliberately: under `pipefail` a
# pipeline whose FIRST stage exits 1 (which `probe` does whenever the bench is
# held -- that is its verdict, not a failure) reports 1 no matter what the
# filter found. Piping it straight into `grep` therefore reported "not my lock"
# for every successful acquire.
bench_lock_id_now() {
  local probe
  probe="$(bench_host probe 2>/dev/null)"
  bench_field "$probe" f_lock_id
}

cmd_release() {
  local want="${RA8_BENCH_LOCK_ID:-}" force=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --lock-id)
        want="${2:-}"
        shift 2
        ;;
      --force)
        force=force
        shift
        ;;
      *)
        bench_say "unknown option '$1'"
        return "$RA8_BENCH_EXIT_UNKNOWN"
        ;;
    esac
  done
  if [ -z "$want" ]; then
    want="$(bench_lock_id_now)"
    [ -n "$want" ] || {
      bench_say "the bench is not held (nothing to release)."
      return "$RA8_BENCH_EXIT_FREE"
    }
    # Releasing a hold this actor did not take is a preempt, not a release.
    # Phase 1's `bench take` is the verb for that; it demands a reason and,
    # against a human, an explicit acknowledgement.
    if [ -z "$force" ]; then
      local mine
      local probe
      probe="$(bench_host probe 2>/dev/null)"
      mine="$(bench_field "$probe" f_holder_name)"
      bench_say "the bench is held by ${mine:-someone else} and RA8_BENCH_LOCK_ID is unset."
      bench_say "If it is yours, pass --lock-id. To preempt, use --force (journaled)."
      return "$RA8_BENCH_EXIT_HELD"
    fi
  fi
  bench_host release "$want" "$force"
}

# ---------------------------------------------------------------------------
# help / dispatch
# ---------------------------------------------------------------------------

cmd_help() {
  cat <<'USAGE'
bench.sh -- exclusive access to the EK-RA8D2 bench.

  bench.sh status
      Who holds the bench. Exit 0 free, 1 held, 3 UNKNOWN.

  bench.sh run --intent "<why>" [--for 15m] [--wait 20m] [--as human|agent|ci]
               [--break-glass] -- <command> [args...]
      Hold the bench for exactly as long as <command> runs, then release.
      This is the normal shape: the hold is bound to a live process, so
      nothing can leave it stale. Exits with <command>'s status.

  bench.sh acquire --intent "<why>" --for <dur> [--wait <dur>] [--as ...]
      Take a DETACHED hold, which survives the command that took it. --for is
      mandatory and capped at 8h precisely because nothing else bounds it.
      Prints the lock id.

  bench.sh release [--lock-id <id>] [--force]
      Give back a hold. --force preempts somebody else's and is journaled.

  bench.sh hold --why "<what you are doing>" [--for 2h] [--detached]
      A shell that holds the bench. Close it and the bench comes back, so a
      dead laptop cannot leave it held. --detached survives the command and
      therefore REQUIRES --for; `status` shows which kind is in force.

  bench.sh free                 give back the hold you own
  bench.sh extend --for 30m     push your own budget out
  bench.sh take --reason "..."  preempt. Humans only; preempting a HUMAN
                                additionally needs CONFIRM=<phrase>.
  bench.sh log [n]              tail the append-only journal
  bench.sh doctor               state dir, perms, inode, clock, sshd bound
  bench.sh reap                 SIGTERM agent/CI holds past their budget

  bench.sh selftest [--ssh-death]
      Prove the machinery against a throwaway state directory. --ssh-death
      additionally kills a real ssh client and asserts the lock drops.

Environment:
  PI_HOST            the bench host (from the gitignored .env)
  RA8_BENCH_CLASS    default holder class (human|agent|ci); default agent
  RA8_BENCH_ACTOR    default holder name
  RA8_BENCH_LOCK_ID  set by `run` for its payload; `release` uses it
USAGE
  return "$RA8_BENCH_EXIT_FREE"
}

# ---------------------------------------------------------------------------
# journal / doctor / reap -- thin pass-throughs to the bench host
# ---------------------------------------------------------------------------

cmd_log() { bench_host journal "${1:-25}"; }

# The failure this exists for is two actors flocking two DIFFERENT inodes,
# which looks exactly like a working lock right up to the moment it is not.
# So doctor reports the inode, the mode and the owner rather than asserting
# they are fine, and it also reports the one sshd setting that bounds a
# vanished client's release (see the header).
cmd_doctor() {
  local out rc sshd
  out="$(bench_host doctor 2>&1)"
  rc=$?
  printf '%s\n' "$out"
  if ! rig_is_local_pi && [ -n "${PI_HOST:-}" ]; then
    sshd="$(ssh "${RA8_BENCH_SSH_OPTS[@]}" "$PI_HOST" \
      'sudo -n sshd -T 2>/dev/null | sed -n "s/^clientaliveinterval //p"' </dev/null 2>/dev/null)"
    printf 'sshd_clientaliveinterval=%s\n' "${sshd:-unknown}"
    if [ "${sshd:-0}" = "0" ]; then
      printf 'FINDING: sshd ClientAliveInterval is 0 on the bench host. A client that\n'
      printf '         vanishes WITHOUT closing its socket (power cut, not a kill) then\n'
      printf '         leaves the hold in place until TCP keepalive gives up -- hours.\n'
      printf '         Fix: the hil_bench ansible role sets ClientAliveInterval.\n'
      rc=3
    fi
  fi
  return "$rc"
}

cmd_reap() { bench_host reap; }

bench_main() {
  local verb="${1:-status}"
  shift 2>/dev/null || true
  case "$verb" in
    status) cmd_status "$@" ;;
    run) cmd_run "$@" ;;
    acquire) cmd_acquire "$@" ;;
    release) cmd_release "$@" ;;
    log) cmd_log "$@" ;;
    doctor) cmd_doctor "$@" ;;
    reap) cmd_reap "$@" ;;
    hold | free | extend | take)
      # The human verbs, sourced on demand: `status` and `run` are what runs a
      # thousand times a day and neither needs them.
      # shellcheck source=scripts/hil/lib/bench_human.sh
      source "$_bench_dir/lib/bench_human.sh"
      "cmd_$verb" "$@"
      ;;
    selftest)
      # Sourced on demand: the common path is `status` and `run`, and neither
      # of those should pay to parse 200 lines of proof.
      # shellcheck source=scripts/hil/lib/bench_selftest.sh
      source "$_bench_dir/lib/bench_selftest.sh"
      cmd_selftest "$@"
      ;;
    help | --help | -h) cmd_help ;;
    *)
      bench_say "unknown verb '$verb'"
      cmd_help
      return "$RA8_BENCH_EXIT_UNKNOWN"
      ;;
  esac
}

bench_main "$@"
