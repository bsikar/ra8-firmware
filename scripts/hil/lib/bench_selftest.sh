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

  # 1. A fresh directory reports FREE, exit 0.
  local rc
  cmd_status >/dev/null 2>&1
  rc=$?
  if [ "$rc" -eq "$RA8_BENCH_EXIT_FREE" ]; then
    printf '  ok: an unheld bench reports FREE (exit 0)\n'
  else
    printf '  FAIL: unheld bench reported exit %s, want 0\n' "$rc"
    failures=$((failures + 1))
  fi

  # 2. A wrapped hold really excludes: status must say HELD (exit 1) while a
  #    payload runs, and FREE again the moment it stops.
  cmd_run --intent "selftest: mutual exclusion" --for 60s -- \
    bash -c 'true' >/dev/null 2>&1
  rc=$?
  if [ "$rc" -eq 0 ]; then
    printf '  ok: run acquires, runs its payload and releases\n'
  else
    printf '  FAIL: run exited %s on a trivial payload\n' "$rc"
    failures=$((failures + 1))
  fi

  # 3. A second acquirer is DENIED while the first holds -- exit 1, not 0.
  local tmp lock_id holder
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

  # 4. Releasing on holder death, with no cooperation from the holder at all.
  cmd_status >/dev/null 2>&1
  rc=$?
  if [ "$rc" -eq "$RA8_BENCH_EXIT_FREE" ]; then
    printf '  ok: closing the liveness channel released the bench\n'
  else
    printf '  FAIL: bench still %s after the holder went away\n' "$rc"
    failures=$((failures + 1))
  fi

  # 5. THE GUARD. Everything above proves the CLI; this proves the thing every
  #    HIL script actually calls -- that `ra8_bench_require` holds for the life
  #    of the calling script, no-ops when nested, and lets go when the script
  #    dies WITHOUT running a trap. A guard that quietly released early, or
  #    quietly held forever, would pass every test above.
  bench_selftest_guard || failures=$((failures + 1))

  # 6. The two halves of the human-preempt handshake are written down in two
  #    files -- the client refuses without the phrase, the bench host enforces
  #    it -- so they can drift, and if they did the client would refuse a
  #    preempt the host would have allowed, or worse.
  local host_phrase client_phrase
  host_phrase="$(sed -n 's/^BH_CONFIRM_PHRASE="\(.*\)"$/\1/p' "$RA8_BENCH_HOST_SRC")"
  client_phrase="$(sed -n 's/^RA8_BENCH_CONFIRM_PHRASE="\(.*\)"$/\1/p' \
    "$(dirname "$RA8_BENCH_HOST_SRC")/bench_human.sh")"
  if [ -n "$host_phrase" ] && [ "$host_phrase" = "$client_phrase" ]; then
    printf '  ok: the CONFIRM phrase agrees between client and bench host\n'
  else
    printf '  FAIL: CONFIRM phrase drift -- host=%s client=%s\n' \
      "${host_phrase:-<unset>}" "${client_phrase:-<unset>}"
    failures=$((failures + 1))
  fi

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
bench_selftest_guard() {
  local lib script pid rc waited=0 probe
  lib="$(dirname "$RA8_BENCH_HOST_SRC")"
  script="$(mktemp "${TMPDIR:-/tmp}/ra8-bench-guardtest.XXXXXX")"
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
  waited=0
  while [ "$waited" -lt 60 ]; do
    probe="$(bench_host probe 2>/dev/null)"
    [ "$(bench_field "$probe" flock_held 1)" = "0" ] && break
    sleep 1
    waited=$((waited + 1))
  done
  rm -f "$script" "$script.out" "$script.err"
  probe="$(bench_host probe 2>/dev/null)"
  if [ "$(bench_field "$probe" flock_held 1)" = "0" ]; then
    printf '  ok: SIGKILL of a guarded script released the bench in <=%ss (no trap ran)\n' \
      "$((waited + 1))"
    return 0
  fi
  printf '  FAIL: the bench stayed held %ss after a guarded script was SIGKILLed.\n' "$waited"
  printf '        The fd-close release is not working; only the EXIT trap is.\n'
  rc=1
  return "$rc"
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
  local tmp lock_id holder rc waited=0 probe
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/ra8-bench-sd.XXXXXX")"
  mkfifo "$tmp/hold.in"
  : >"$tmp/hold.out"
  lock_id="$(bench_new_lock_id)"
  bench_start_holder "$lock_id" agent selftest "selftest: ssh-death" 300 0 false \
    "$tmp/hold.in" "$tmp/hold.out"
  holder="$RA8_BENCH_HOLDER_PID"
  exec 9>"$tmp/hold.in"
  bench_await_ack "$lock_id" "$tmp/hold.out" "$holder" 30
  rc=$?
  if [ "$rc" -ne 0 ]; then
    printf '  FAIL(ssh-death): could not establish the hold to kill (rc %s)\n' "$rc"
    exec 9>&-
    rm -rf "$tmp"
    return 1
  fi
  # Assert the PRECONDITION before drawing any conclusion from the postcondition:
  # a test that starts from an unheld lock proves nothing about releasing one.
  probe="$(bench_host probe 2>/dev/null)"
  if [ "$(bench_field "$probe" flock_held 0)" != "1" ] ||
    [ "$(bench_field "$probe" f_lock_id)" != "$lock_id" ]; then
    printf '  FAIL(ssh-death): the hold was not actually in force before the kill\n'
    exec 9>&-
    rm -rf "$tmp"
    return 1
  fi

  # kill -9 the ssh CLIENT. The remote payload is untouched and has no idea.
  kill -9 "$holder" 2>/dev/null
  wait "$holder" 2>/dev/null
  # Keep our end of the fifo OPEN, so the only thing that can release the lock
  # is the remote payload noticing its channel died. Closing it here would
  # prove nothing.
  #
  # The poll asks about the FLOCK, not about the record. A missing record would
  # also make a lock_id comparison differ, and that is not the same claim: the
  # question is whether the kernel lock is gone, so that is what is measured.
  while [ "$waited" -lt 120 ]; do
    probe="$(bench_host probe 2>/dev/null)"
    if [ "$(bench_field "$probe" flock_held 1)" = "0" ]; then
      break
    fi
    sleep 2
    waited=$((waited + 2))
  done
  exec 9>&-
  rm -rf "$tmp"
  probe="$(bench_host probe 2>/dev/null)"
  if [ "$(bench_field "$probe" flock_held 1)" = "0" ]; then
    printf '  ok: SIGKILL of the ssh client released the flock within %ss\n' \
      "$((waited + 2))"
    return 0
  fi
  printf '  FAIL(ssh-death): the flock was still held %ss after the ssh client died.\n' "$waited"
  printf '        The no-stale-lock property does NOT hold on this transport, so the\n'
  printf '        design has degraded to a TTL lease. Do NOT paper over this.\n'
  return 1
}
