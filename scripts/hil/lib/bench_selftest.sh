#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# bench_selftest.sh -- proof that the bench lock does what it claims.
#
# `source` this (do not execute it); scripts/hil/bench.sh pulls it in for the
# `selftest` verb only, so the common path does not carry it.
#
# It runs against a THROWAWAY state directory on the bench host, so it touches
# no instrument, no board and nothing anyone else's hold protects -- only the
# flock machinery itself.

# Sourced again here rather than assumed: this file is meaningless without the
# client machinery, and saying so lets a reader (and shellcheck) follow the
# dependency. Re-sourcing is idempotent -- bench_client.sh defines functions and
# `:=` defaults only.
# shellcheck source=scripts/hil/lib/bench_client.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/bench_client.sh"

# ---------------------------------------------------------------------------

# The mechanics, against a throwaway state directory on the bench host, so no
# instrument and no board is touched. --ssh-death additionally proves the one
# claim the whole "no stale lock" property rests on.
# Cases 1+2: a fresh bench is FREE, and `run` acquires, runs and releases.
_bench_st_basic() {
  local failures=0 rc
  cmd_status >/dev/null 2>&1
  rc=$?
  if [ "$rc" -eq "$RA8_BENCH_EXIT_FREE" ]; then
    printf '  ok: an unheld bench reports FREE (exit 0)\n'
  else
    printf '  FAIL: unheld bench reported exit %s, want 0\n' "$rc"
    failures=$((failures + 1))
  fi
  cmd_run --intent "selftest: mutual exclusion" --for 60s -- \
    bash -c 'true' >/dev/null 2>&1
  rc=$?
  if [ "$rc" -eq 0 ]; then
    printf '  ok: run acquires, runs its payload and releases\n'
  else
    printf '  FAIL: run exited %s on a trivial payload\n' "$rc"
    failures=$((failures + 1))
  fi
  return "$failures"
}

# Cases 3+4: while an incumbent holds, status says HELD and a second acquirer is
# DENIED with its payload never run; when the incumbent's channel closes the
# bench is FREE again. Exclusion that only reports itself is not exclusion.
_bench_st_exclusion() {
  local failures=0 rc tmp lock_id holder
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/ra8-bench-st.XXXXXX")"
  mkfifo "$tmp/hold.in"
  : >"$tmp/hold.out"
  lock_id="$(bench_new_lock_id)"
  bench_start_holder "$lock_id" agent selftest "selftest: incumbent" 60 0 false \
    "$tmp/hold.in" "$tmp/hold.out"
  holder="$RA8_BENCH_HOLDER_PID"
  exec 9>"$tmp/hold.in"
  bench_await_ack "$lock_id" "$tmp/hold.out" "$holder" 30
  rc=$?
  if [ "$rc" -ne 0 ]; then
    printf '  FAIL: the incumbent hold never acquired (rc %s)\n' "$rc"
    failures=$((failures + 1))
  else
    cmd_status >/dev/null 2>&1
    rc=$?
    if [ "$rc" -eq "$RA8_BENCH_EXIT_HELD" ]; then
      printf '  ok: a held bench reports HELD (exit 1)\n'
    else
      printf '  FAIL: held bench reported exit %s, want 1\n' "$rc"
      failures=$((failures + 1))
    fi
    cmd_run --intent "selftest: second acquirer" --for 60s -- bash -c 'true' >/dev/null 2>&1
    rc=$?
    if [ "$rc" -eq "$RA8_BENCH_EXIT_HELD" ]; then
      printf '  ok: a second acquirer is DENIED (exit 1) and its payload never runs\n'
    else
      printf '  FAIL: second acquirer exited %s, want 1\n' "$rc"
      failures=$((failures + 1))
    fi
  fi
  exec 9>&-
  wait "$holder" 2>/dev/null
  rm -rf "$tmp"

  cmd_status >/dev/null 2>&1
  rc=$?
  if [ "$rc" -eq "$RA8_BENCH_EXIT_FREE" ]; then
    printf '  ok: closing the liveness channel released the bench\n'
  else
    printf '  FAIL: bench still %s after the holder went away\n' "$rc"
    failures=$((failures + 1))
  fi
  return "$failures"
}

# Case 6: the human-preempt handshake is written down in TWO files -- the client
# refuses without the phrase, the bench host enforces it -- so they can drift,
# and a drift would mean the client refusing a preempt the host would have
# allowed, or the reverse.
_bench_st_confirm_phrase() {
  local host_phrase client_phrase
  host_phrase="$(sed -n 's/^BH_CONFIRM_PHRASE="\(.*\)"$/\1/p' "$RA8_BENCH_HOST_SRC")"
  client_phrase="$(sed -n 's/^RA8_BENCH_CONFIRM_PHRASE="\(.*\)"$/\1/p' \
    "$(dirname "$RA8_BENCH_HOST_SRC")/bench_human.sh")"
  if [ -n "$host_phrase" ] && [ "$host_phrase" = "$client_phrase" ]; then
    printf '  ok: the CONFIRM phrase agrees between client and bench host\n'
    return 0
  fi
  printf '  FAIL: CONFIRM phrase drift -- host=%s client=%s\n' \
    "${host_phrase:-<unset>}" "${client_phrase:-<unset>}"
  return 1
}

cmd_selftest() {
  local want_ssh_death=0
  while [ $# -gt 0 ]; do
    case "$1" in
      --ssh-death)
        want_ssh_death=1
        shift
        ;;
      *) shift ;;
    esac
  done
  local sandbox failures=0
  sandbox="/tmp/ra8-bench-selftest.$$"
  # Redirect every lock operation below at a directory nobody else uses. This
  # is the ONLY place RA8_BENCH_DIR is ever moved off its canonical path, and
  # it is why the selftest can prove exclusion without excluding anyone.
  # shellcheck disable=SC2034  # consumed by lib/bench_client.sh and lib/bench_host.sh, not here.
  RA8_BENCH_DIR="$sandbox"
  printf 'bench selftest: state directory %s (throwaway)\n' "$sandbox"

  bench_host provision >/dev/null 2>&1 || {
    printf '  FAIL: could not provision %s\n' "$sandbox"
    return "$RA8_BENCH_EXIT_UNKNOWN"
  }

  _bench_st_basic || failures=$((failures + $?))
  _bench_st_exclusion || failures=$((failures + $?))
  # THE GUARD. Everything above proves the CLI; this proves the thing every HIL
  # script actually calls -- that `ra8_bench_require` holds for the life of the
  # calling script, no-ops when nested, and lets go when the script dies WITHOUT
  # running a trap. A guard that quietly released early, or quietly held
  # forever, would pass every case above.
  bench_selftest_guard || failures=$((failures + 1))
  bench_selftest_fence || failures=$((failures + 1))
  _bench_st_confirm_phrase || failures=$((failures + 1))

  if [ "$want_ssh_death" -eq 1 ]; then
    bench_selftest_ssh_death || failures=$((failures + 1))
  fi

  bench_host release - force >/dev/null 2>&1
  rm -rf "$sandbox" 2>/dev/null || bench_host provision >/dev/null 2>&1
  if [ "$failures" -eq 0 ]; then
    printf 'bench selftest: PASS\n'
    return "$RA8_BENCH_EXIT_FREE"
  fi
  printf 'bench selftest: FAIL (%s)\n' "$failures"
  return "$RA8_BENCH_EXIT_HELD"
}

# The sourced guard, exercised the way a HIL script uses it -- as a real
# child process that takes the lock, checks it is held, and is then killed
# WITHOUT a chance to run any trap. `kill -9` is the case that matters: if the
# only thing releasing the bench were the EXIT trap, every hard-killed HIL run
# would leak the bench, and the fd-close mechanism this design relies on would
# be doing nothing while looking like it worked.
# A stand-in for a real HIL script: source the guard, take the bench, prove a
# nested call is a no-op, then sit still so the caller can kill it.
_bench_st_write_guard_script() {
  local script="$1" lib
  lib="$(dirname "$RA8_BENCH_HOST_SRC")"
  cat >"$script" <<GUARDTEST
#!/usr/bin/env bash
set -uo pipefail
source "$lib/bench_lock.sh"
ra8_bench_require "selftest: sourced guard" 120s || exit \$?
# Nested call: a script that calls another guarded script must NOT deadlock
# against the hold it already owns.
ra8_bench_require "selftest: nested" || exit \$?
printf 'GUARD-HELD %s\n' "\$RA8_BENCH_LOCK_ID"
# Hold still until killed.
while :; do sleep 1; done
GUARDTEST
}

# Poll the bench host until the flock reaches <want> (0 free, 1 held), or the
# window runs out. Prints the seconds waited.
_bench_st_await_flock() {
  local want="$1" limit="$2" waited=0 probe
  while [ "$waited" -lt "$limit" ]; do
    probe="$(bench_host probe 2>/dev/null)"
    [ "$(bench_field "$probe" flock_held 9)" = "$want" ] && break
    sleep 1
    waited=$((waited + 1))
  done
  printf '%s' "$waited"
}

bench_selftest_guard() {
  local script pid waited=0 probe
  script="$(mktemp "${TMPDIR:-/tmp}/ra8-bench-guardtest.XXXXXX")"
  _bench_st_write_guard_script "$script"

  RA8_BENCH_DIR="$RA8_BENCH_DIR" bash "$script" >"$script.out" 2>"$script.err" &
  pid=$!
  while [ "$waited" -lt 60 ]; do
    grep -q '^GUARD-HELD ' "$script.out" 2>/dev/null && break
    kill -0 "$pid" 2>/dev/null || break
    sleep 1
    waited=$((waited + 1))
  done
  if ! grep -q '^GUARD-HELD ' "$script.out" 2>/dev/null; then
    printf '  FAIL: ra8_bench_require never acquired\n'
    sed 's/^/        /' "$script.err" 2>/dev/null | head -8
    kill -9 "$pid" 2>/dev/null
    rm -f "$script" "$script.out" "$script.err"
    return 1
  fi
  probe="$(bench_host probe 2>/dev/null)"
  if [ "$(bench_field "$probe" flock_held 0)" != "1" ]; then
    printf '  FAIL: the guard reported a hold that the bench host does not have\n'
    kill -9 "$pid" 2>/dev/null
    rm -f "$script" "$script.out" "$script.err"
    return 1
  fi
  printf '  ok: ra8_bench_require holds, and a nested call is a no-op\n'

  kill -9 "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  waited="$(_bench_st_await_flock 0 60)"
  rm -f "$script" "$script.out" "$script.err"
  probe="$(bench_host probe 2>/dev/null)"
  if [ "$(bench_field "$probe" flock_held 1)" = "0" ]; then
    printf '  ok: SIGKILL of a guarded script released the bench in <=%ss (no trap ran)\n' \
      "$((waited + 1))"
    return 0
  fi
  printf '  FAIL: the bench stayed held %ss after a guarded script was SIGKILLed.\n' "$waited"
  printf '        The fd-close release is not working; only the EXIT trap is.\n'
  return 1
}

# THE FENCE. The case above kills the whole guarded script, so the hold and the
# work die together and nothing has to notice anything. This kills ONLY the
# hold's ssh client and leaves the script running -- which is what a network
# blip, an sshd restart or ServerAlive giving up looks like, and what
# scripts/hil/bench_contention.sh staged deliberately.
#
# Before the fence existed the script carried on driving the board with no lock
# at all: measured on the bench, a J-Link session ran 75 s past its own release
# while three other machines took the lock in turn and programmed MRAM
# underneath it. So the assertion is not "the flock dropped" -- it did, that was
# never the problem -- but that the CALLER STOPS.
_bench_st_write_fence_script() {
  local script="$1" lib
  lib="$(dirname "$RA8_BENCH_HOST_SRC")"
  cat >"$script" <<FENCETEST
#!/usr/bin/env bash
set -uo pipefail
source "$lib/bench_lock.sh"
ra8_bench_require "selftest: fence" 300s || exit \$?
printf 'FENCE-HELD %s\n' "\$RA8_BENCH_LOCK_ID"
# Stand in for a script that is still driving the board. If the fence does not
# fire, this runs to completion and prints the line that must never appear.
i=0
while [ \$i -lt 120 ]; do
  sleep 1
  i=\$((i + 1))
done
printf 'STILL-RUNNING-UNPROTECTED\n'
FENCETEST
}

# Start the fence test script and wait for it to report a hold. Prints its pid,
# or nothing when it never acquired.
_bench_st_fence_start() {
  local script="$1" pid waited=0
  RA8_BENCH_DIR="$RA8_BENCH_DIR" bash "$script" >"$script.out" 2>"$script.err" &
  pid=$!
  while [ "$waited" -lt 60 ]; do
    grep -q '^FENCE-HELD ' "$script.out" 2>/dev/null && break
    kill -0 "$pid" 2>/dev/null || break
    sleep 1
    waited=$((waited + 1))
  done
  if ! grep -q '^FENCE-HELD ' "$script.out" 2>/dev/null; then
    kill -9 "$pid" 2>/dev/null
    return 1
  fi
  printf '%s' "$pid"
}

# Judge the aftermath: the caller must have stopped, must NOT have reached the
# line it prints when it runs to completion, and must have said why.
_bench_st_fence_verdict() {
  local script="$1" pid="$2" waited="$3"
  if grep -q '^STILL-RUNNING-UNPROTECTED' "$script.out" 2>/dev/null; then
    printf '  FAIL: the caller kept running after its hold died -- the fence did not fire.\n'
    return 1
  fi
  if kill -0 "$pid" 2>/dev/null; then
    kill -9 "$pid" 2>/dev/null
    printf '  FAIL: the caller was still alive %ss after its hold was SIGKILLed.\n' "$waited"
    return 1
  fi
  if ! grep -q 'THE BENCH HOLD DIED' "$script.err" 2>/dev/null; then
    printf '  FAIL: the caller stopped, but the fence never said why.\n'
    printf '        A silent abort is indistinguishable from an unrelated crash.\n'
    return 1
  fi
  printf '  ok: killing ONLY the hold stopped the caller in <=%ss, loudly\n' "$((waited + 1))"
  return 0
}

bench_selftest_fence() {
  local script pid hold waited=0 rc
  script="$(mktemp "${TMPDIR:-/tmp}/ra8-bench-fencetest.XXXXXX")"
  _bench_st_write_fence_script "$script"
  pid="$(_bench_st_fence_start "$script")"
  if [ -z "$pid" ]; then
    printf '  FAIL: the fence test script never acquired\n'
    sed 's/^/        /' "$script.err" 2>/dev/null | head -8
    rm -f "$script" "$script.out" "$script.err"
    return 1
  fi

  # The hold's own ssh client, and ONLY it: a descendant of the test script
  # whose command line carries the shipped host half. Killing by pattern alone
  # would catch every other actor's hold on this machine. A local bench
  # (rig_is_local_pi) has no ssh client at all; there the hold is a plain child
  # process, and killing that is the same experiment.
  hold="$(_bench_st_hold_client "$pid")"
  [ -n "$hold" ] || hold="$(pgrep -P "$pid" 2>/dev/null | head -1)"
  if [ -z "$hold" ]; then
    printf '  FAIL: could not find the hold process to kill\n'
    kill -9 "$pid" 2>/dev/null
    rm -f "$script" "$script.out" "$script.err"
    return 1
  fi

  kill -9 "$hold" 2>/dev/null
  while [ "$waited" -lt 30 ]; do
    kill -0 "$pid" 2>/dev/null || break
    sleep 1
    waited=$((waited + 1))
  done
  wait "$pid" 2>/dev/null
  _bench_st_fence_verdict "$script" "$pid" "$waited"
  rc=$?
  rm -f "$script" "$script.out" "$script.err"
  return "$rc"
}

# The hold's ssh client: a descendant of <root> running the shipped host half.
_bench_st_hold_client() {
  local root="$1" p cur
  for p in $(pgrep -f ra8-bench-host 2>/dev/null); do
    cur="$p"
    while [ -n "$cur" ] && [ "$cur" != "1" ]; do
      if [ "$cur" = "$root" ]; then
        printf '%s' "$p"
        return 0
      fi
      cur="$(ps -o ppid= -p "$cur" 2>/dev/null | tr -d ' ')"
    done
  done
  return 0
}

# THE experiment the design stands on.
#
# "A holder that dies drops the lock" is trivially true for a local process.
# For a remote one it rests entirely on ssh reaping its payload when the
# CLIENT dies -- and ssh does not do that in general. It does it here only
# because the payload BLOCKS ON ITS STDIN, which is the ssh channel: when the
# client process dies, its socket closes, sshd closes the channel, and the
# payload's `read` returns EOF.
#
# So this kills the local ssh client with SIGKILL -- no traps, no shutdown, the
# closest thing to a yanked network cable that can be staged deterministically
# -- and asserts the flock is observably free within a bounded time. If this
# ever fails, the honest answer is that the design has degraded to a TTL lease
# and must be reconsidered, NOT that a TTL should be bolted on quietly.
bench_selftest_ssh_death() {
  if rig_is_local_pi; then
    printf '  skip: --ssh-death needs a remote bench host (running ON the bench)\n'
    return 0
  fi
  local tmp lock_id holder waited probe
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/ra8-bench-sd.XXXXXX")"
  mkfifo "$tmp/hold.in"
  : >"$tmp/hold.out"
  lock_id="$(bench_new_lock_id)"
  bench_start_holder "$lock_id" agent selftest "selftest: ssh-death" 300 0 false \
    "$tmp/hold.in" "$tmp/hold.out"
  holder="$RA8_BENCH_HOLDER_PID"
  exec 9>"$tmp/hold.in"
  if ! _bench_st_ssh_death_precondition "$lock_id" "$holder" "$tmp"; then
    exec 9>&-
    rm -rf "$tmp"
    return 1
  fi

  # kill -9 the ssh CLIENT. The remote payload is untouched and has no idea.
  kill -9 "$holder" 2>/dev/null
  wait "$holder" 2>/dev/null
  # Our end of the fifo stays OPEN, so the only thing that can release the lock
  # is the remote payload noticing its channel died. Closing it here would
  # prove nothing.
  #
  # The poll asks about the FLOCK, not about the record. A missing record would
  # also make a lock_id comparison differ, and that is not the same claim: the
  # question is whether the kernel lock is gone, so that is what is measured.
  waited="$(_bench_st_await_flock 0 120)"
  exec 9>&-
  rm -rf "$tmp"
  probe="$(bench_host probe 2>/dev/null)"
  if [ "$(bench_field "$probe" flock_held 1)" = "0" ]; then
    printf '  ok: SIGKILL of the ssh client released the flock within %ss\n' \
      "$((waited + 1))"
    return 0
  fi
  printf '  FAIL(ssh-death): the flock was still held %ss after the ssh client died.\n' "$waited"
  printf '        The no-stale-lock property does NOT hold on this transport, so the\n'
  printf '        design has degraded to a TTL lease. Do NOT paper over this.\n'
  return 1
}

# Assert the PRECONDITION before drawing any conclusion from the postcondition:
# a test that starts from an unheld lock proves nothing at all about releasing
# one, and would pass forever.
_bench_st_ssh_death_precondition() {
  local lock_id="$1" holder="$2" tmp="$3" rc probe
  bench_await_ack "$lock_id" "$tmp/hold.out" "$holder" 30
  rc=$?
  if [ "$rc" -ne 0 ]; then
    printf '  FAIL(ssh-death): could not establish the hold to kill (rc %s)\n' "$rc"
    return 1
  fi
  probe="$(bench_host probe 2>/dev/null)"
  if [ "$(bench_field "$probe" flock_held 0)" != "1" ] ||
    [ "$(bench_field "$probe" f_lock_id)" != "$lock_id" ]; then
    printf '  FAIL(ssh-death): the hold was not actually in force before the kill\n'
    return 1
  fi
  return 0
}
