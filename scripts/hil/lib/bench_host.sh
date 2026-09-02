#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# bench_host.sh -- the BENCH-HOST half of the bench mutual-exclusion lock.
#
# This file runs ON the machine the EK-RA8D2 is physically attached to (the
# bench Pi). It is the only place a lock is ever taken, a record is ever
# written, or a timestamp is ever read: correctness is a kernel `flock`, and
# policy is decided against ONE machine's clock, so no client clock can skew
# either. `scripts/hil/bench.sh` is the client half and shells out to this.
#
# Executed, never sourced -- the mirror image of the other files in lib/,
# and deliberately so: the client ships this file BY VALUE over ssh (base64 on
# the command line) and runs it out of /tmp, exactly the way rig_env.sh ships
# tty_resolve.sh into a remote heredoc. The bench host therefore needs no
# checkout, no PATH setup and nothing in sync with the client -- which matters
# because the Pi's copy of the tree is whatever a suite last left there.
#
# WHY A FLOCK AND NOT A LEASE
# ---------------------------
# A holder is a live process holding an open fd with a kernel lock on it. When
# it dies -- killed, session-limited, ssh dropped, host rebooted -- the kernel
# drops the lock instantly. There is no TTL to expire and no heartbeat to miss,
# so "stale lock" is not a state this design can be in. TTLs survive only as
# advisory budget (`max_hold_s`), and a budget overrun is reported and reaped,
# never trusted for correctness.
#
# Verbs (each is `bench_host.sh <verb> [args]`):
#   provision            create + permission the state directory (idempotent)
#   probe                machine-readable snapshot: who holds it, and since when
#   hold <mode> <wait_s> <fields_b64>
#                        TAKE the lock and hold it for as long as this process
#                        lives. mode=wrapped blocks on stdin and releases at
#                        EOF (the ssh channel is the liveness signal);
#                        mode=detached blocks until SIGTERM.
#   release <lock_id> [force]
#                        signal the holding process to let go
#   journal [n]          tail the append-only audit log
#   doctor               assert the state directory is the one everyone shares
#   reap                 SIGTERM agent/ci holders past their declared budget
#
# Exit codes -- the same three the whole bench interface uses:
#   0  free / acquired / OK
#   1  held / denied
#   3  UNKNOWN: no verdict could be established. NEVER collapse this into 0.
#
# No `set -e`, deliberately, for the reason scripts/ci/monitor.sh gives: with 1
# meaning "denied", an unrelated abort exiting 1 would silently become a
# verdict. Every command below either handles its own failure or feeds an
# explicit exit-code decision.
set -uo pipefail

# Removing our own file while bash still holds the fd is safe on Linux (and the
# bench host is Linux by definition -- it is where the J-Link is plugged in),
# and it keeps /tmp from filling with one copy of this script per acquire.
rm -f -- "$0" 2>/dev/null || true

readonly BH_EXIT_OK=0
readonly BH_EXIT_HELD=1
readonly BH_EXIT_UNKNOWN=3

# /var/lib and not /tmp or /run: the record must not depend on whether this
# host's /tmp is a tmpfs. A reboot is handled explicitly instead, by boot_id.
BH_DIR="${RA8_BENCH_DIR:-/var/lib/ra8-bench}"
BH_LOCK="$BH_DIR/board.lock"
BH_REC="$BH_DIR/holder.json"
BH_JNL="$BH_DIR/journal.ndjson"
BH_BROKER_PID=""
BH_BROKER_INPUT=""
BH_BROKER_OUTPUT=""

# The record's field order. The reader below is paired with the writer above
# it -- one field per line, in this order -- so neither jq nor python is a
# dependency of taking the bench.
BH_FIELDS="resource lock_id holder_class holder_name pid pid_start_ticks boot_id origin
  intent git_ref acquired_at acquired_epoch max_hold_s hold_kind
  last_activity break_glass"

bh_log() { printf 'bench-host: %s\n' "$*" >&2; }

# ---------------------------------------------------------------------------
# JSON, by hand
# ---------------------------------------------------------------------------

# bh_esc <string> -- escape for a JSON string literal, and flatten newlines.
# An intent with a newline in it would break the one-field-per-line contract
# the reader depends on, so it is folded to a space rather than escaped.
bh_esc() {
  printf '%s' "$1" | tr '\n\r\t' '   ' | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'
}

# bh_unesc -- the inverse, for values read back out of the record.
bh_unesc() { sed -e 's/\\"/"/g' -e 's/\\\\/\\/g'; }

# bh_json_get <file> <key> -- read one field out of a record this script wrote.
# Deliberately not a general JSON parser: it reads the broker's fixed one-field-
# per-line holder record and never evaluates its contents.
bh_json_get() {
  local f="$1" k="$2" v
  [ -f "$f" ] || return 0
  v="$(sed -n "s/^[[:space:]]*\"${k}\"[[:space:]]*:[[:space:]]*//p" "$f" 2>/dev/null | head -1)"
  v="${v%,}"
  v="${v#\"}"
  v="${v%\"}"
  printf '%s' "$v" | bh_unesc
}

# ---------------------------------------------------------------------------
# State directory
# ---------------------------------------------------------------------------

# Create or validate state at exactly ONE canonical path. Production ownership
# comes from the hil_bench Ansible role; a writable sandbox can still be made by
# the selftest. Two actors flocking different inodes is the silent-corruption
# path, so an unavailable canonical directory fails closed instead of falling
# back elsewhere.
bh_provision() {
  if [ ! -d "$BH_DIR" ]; then
    mkdir -p "$BH_DIR" 2>/dev/null || {
      bh_log "FATAL -- cannot create $BH_DIR; reapply the hil_bench Ansible role"
      return "$BH_EXIT_UNKNOWN"
    }
    # 1777: several accounts (star, a CI runner, a human) share this box and
    # any of them may be the next acquirer. The sticky bit keeps one actor
    # from unlinking another's files.
    if ! chmod 1777 "$BH_DIR" 2>/dev/null; then
      bh_log "FATAL -- cannot set shared sticky permissions on $BH_DIR"
      return "$BH_EXIT_UNKNOWN"
    fi
  fi
  local f
  for f in "$BH_LOCK" "$BH_JNL"; do
    if [ ! -e "$f" ]; then
      if ! : >"$f" 2>/dev/null || ! chmod 666 "$f" 2>/dev/null; then
        bh_log "FATAL -- cannot create shared bench state file $f"
        return "$BH_EXIT_UNKNOWN"
      fi
    fi
  done
  [ -w "$BH_LOCK" ] || {
    bh_log "FATAL -- $BH_LOCK is not writable by $(id -un)"
    return "$BH_EXIT_UNKNOWN"
  }
  [ -w "$BH_JNL" ] || {
    bh_log "FATAL -- $BH_JNL is not writable by $(id -un)"
    return "$BH_EXIT_UNKNOWN"
  }
  return "$BH_EXIT_OK"
}

# ---------------------------------------------------------------------------
# Journal
# ---------------------------------------------------------------------------

# bh_journal_add <event> <lock_id> <class> <name> <note>
# Append-only, one JSON object per line. Never rewritten, never truncated:
# a force-take is exactly the event nobody will admit to afterwards.
bh_journal_add() {
  if ! printf '{"at":"%s","event":"%s","lock_id":"%s","holder_class":"%s","holder_name":"%s","note":"%s"}\n' \
    "$(date -Iseconds)" "$(bh_esc "$1")" "$(bh_esc "$2")" "$(bh_esc "$3")" \
    "$(bh_esc "$4")" "$(bh_esc "$5")" >>"$BH_JNL" 2>/dev/null; then
    bh_log "WARNING -- could not append the $1 event to $BH_JNL"
  fi
}

# ---------------------------------------------------------------------------
# Liveness
# ---------------------------------------------------------------------------

# bh_flock_held -- 0 when somebody holds the lock, 1 when it is free.
# Non-destructive: it takes the lock on a SEPARATE fd and drops it again inside
# the subshell, so asking the question never changes the answer.
bh_flock_held() {
  [ -e "$BH_LOCK" ] || return 1
  if (flock -n 8) 8>>"$BH_LOCK" 2>/dev/null; then
    return 1
  fi
  return 0
}

# ---------------------------------------------------------------------------
# probe -- the machine-readable snapshot the client renders
# ---------------------------------------------------------------------------
#
# Emitted as `key=value`, one per line, values newline-free. The client splits
# on the FIRST `=` only, so a value may contain any number more.
bh_probe() {
  if [ ! -d "$BH_DIR" ]; then
    printf 'state=UNKNOWN\n'
    printf 'reason=state directory %s does not exist -- run: bench.sh doctor --provision\n' "$BH_DIR"
    return "$BH_EXIT_UNKNOWN"
  fi
  local boot now_epoch now_iso held=0 rec=0 stale_boot=0
  boot="$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)"
  now_epoch="$(date +%s)"
  now_iso="$(date -Iseconds)"
  bh_flock_held && held=1
  [ -f "$BH_REC" ] && rec=1

  printf 'boot_id=%s\n' "$boot"
  printf 'now_epoch=%s\n' "$now_epoch"
  printf 'now_iso=%s\n' "$now_iso"
  printf 'flock_held=%s\n' "$held"
  printf 'rec_present=%s\n' "$rec"

  if [ "$rec" -eq 1 ]; then
    local k v rec_boot
    rec_boot="$(bh_json_get "$BH_REC" boot_id)"
    # A record from before the last reboot describes holders that are all dead
    # by definition -- their ssh sessions, JLinkExe processes and flocks went
    # with the kernel. Invalid on sight, so a reboot can never leave a lease
    # that outlives its holder.
    [ -n "$rec_boot" ] && [ "$rec_boot" != "$boot" ] && stale_boot=1
    for k in $BH_FIELDS; do
      v="$(bh_json_get "$BH_REC" "$k")"
      printf 'f_%s=%s\n' "$k" "$v"
    done
  fi
  printf 'rec_stale_boot=%s\n' "$stale_boot"

  # The last release, so a FREE bench can still say who used it last and why.
  local last
  last="$(grep '"event":"release"' "$BH_JNL" 2>/dev/null | tail -1)"
  if [ -n "$last" ]; then
    printf 'last_release_at=%s\n' "$(printf '%s' "$last" | sed -n 's/.*"at":"\([^"]*\)".*/\1/p')"
    printf 'last_release_who=%s\n' "$(printf '%s' "$last" | sed -n 's/.*"holder_name":"\([^"]*\)".*/\1/p')"
    printf 'last_release_note=%s\n' "$(printf '%s' "$last" | sed -n 's/.*"note":"\([^"]*\)".*/\1/p')"
  fi

  if [ "$held" -eq 1 ] && [ "$stale_boot" -eq 0 ]; then
    printf 'state=HELD\n'
    return "$BH_EXIT_HELD"
  fi
  printf 'state=FREE\n'
  return "$BH_EXIT_OK"
}

# ---------------------------------------------------------------------------
# hold -- the only path that takes the lock
# ---------------------------------------------------------------------------

# Release runs from an EXIT trap, so it runs on a normal return, on SIGTERM,
# and on the ssh channel closing -- but it is NOT what makes the release safe.
# The kernel already dropped the flock when this process died; this only tidies
# the record and writes the audit line.
bh_release_self() {
  local rec_id
  bh_broker_close
  rec_id="$(bh_json_get "$BH_REC" lock_id)"
  if [ "$rec_id" = "$BH_LOCK_ID" ]; then
    rm -f "$BH_REC" 2>/dev/null || true
  fi
  bh_journal_add release "$BH_LOCK_ID" "$BH_CLASS" "$BH_NAME" "$BH_INTENT"
}

bh_close_fd() {
  local descriptor="$1"
  case "$descriptor" in '' | *[!0-9]*) return 0 ;; *) ;; esac
  eval "exec ${descriptor}>&-" 2>/dev/null || true
}

bh_broker_close() {
  bh_close_fd "$BH_BROKER_INPUT"
  bh_close_fd "$BH_BROKER_OUTPUT"
  if [ -n "$BH_BROKER_PID" ]; then
    wait "$BH_BROKER_PID" 2>/dev/null || true
  fi
  BH_BROKER_PID=""
  BH_BROKER_INPUT=""
  BH_BROKER_OUTPUT=""
}

# bh_decode_fields <base64> -- the client-supplied half of the record.
# Shipped base64-encoded so an intent string can contain anything at all
# without a quoting accident becoming a shell injection over ssh.
bh_decode_fields() {
  local line k v
  BH_LOCK_ID=""
  BH_CLASS="agent"
  BH_NAME="unknown"
  BH_INTENT=""
  BH_MAX_HOLD_S="900"
  BH_QUIESCE_S="120"
  BH_BREAK_GLASS="false"
  BH_CONFIRM=""
  while IFS= read -r line; do
    [ -n "$line" ] || continue
    k="${line%%=*}"
    v="${line#*=}"
    case "$k" in
      resource) ;;
      lock_id) BH_LOCK_ID="$v" ;;
      holder_class) BH_CLASS="$v" ;;
      holder_name) BH_NAME="$v" ;;
      origin) ;;
      intent) BH_INTENT="$v" ;;
      git_ref) ;;
      max_hold_s) BH_MAX_HOLD_S="$v" ;;
      quiesce_s) BH_QUIESCE_S="$v" ;;
      hold_kind) ;;
      break_glass) BH_BREAK_GLASS="$v" ;;
      confirm) BH_CONFIRM="$v" ;;
      *) ;;
    esac
  done <<EOF
$(printf '%s' "$1" | base64 -d 2>/dev/null)
EOF
  [ -n "$BH_LOCK_ID" ] || return 1
  case "$BH_MAX_HOLD_S" in
    '' | *[!0-9]*) return 1 ;;
    *) ;;
  esac
  return 0
}

# PRECEDENCE, enforced in code and not in a convention: human > agent, and
# human > ci. CI never preempts anything, and an agent never preempts a human.
#
# The one case that needs a person's explicit acknowledgement is preempting a
# HUMAN holder, because a human holder may be standing at the bench right now
# with a probe on a test point, and nothing on this host can see that. So it
# takes a phrase that cannot be typed by accident, and cannot be produced by a
# script that did not mean it.
BH_CONFIRM_PHRASE="someone-may-be-at-the-bench"

bh_preempt() {
  local cls name lock_id
  if [ ! -f "$BH_REC" ]; then
    # Held with no record: nobody to check precedence against, and nobody to
    # signal either. Refuse rather than guess.
    bh_log "held by an unidentified process -- refusing to preempt blind"
    return 1
  fi
  cls="$(bh_json_get "$BH_REC" holder_class)"
  name="$(bh_json_get "$BH_REC" holder_name)"
  lock_id="$(bh_json_get "$BH_REC" lock_id)"
  if [ "$BH_CLASS" = "ci" ]; then
    bh_log "refusing: CI never preempts. It waits, or it reports UNKNOWN."
    return 1
  fi
  if [ "$cls" = "human" ]; then
    if [ "$BH_CLASS" != "human" ]; then
      bh_log "refusing: a $BH_CLASS holder may not preempt a human ($name)."
      return 1
    fi
    if [ "$BH_CONFIRM" != "$BH_CONFIRM_PHRASE" ]; then
      bh_log "refusing: the bench is held by $name (human)."
      bh_log "  A human holder can only be preempted by another human with an"
      bh_log "  explicit acknowledgement that someone may physically be at the"
      bh_log "  bench:  CONFIRM=$BH_CONFIRM_PHRASE"
      return 1
    fi
  fi
  bh_journal_add force-take "$lock_id" "$BH_CLASS" "$BH_NAME" \
    "preempted $cls:$name -- $BH_INTENT"
  bh_release "$lock_id" force >/dev/null
  return $?
}

# bh_take_flock <wait_s> <fields_b64> -- 0 when the broker owns the lock.
#
# fd 9 stays open for the life of this process on purpose: THAT open fd, with a
# kernel lock on it, IS the hold. Nothing below ever writes "held" anywhere as
# a claim.
bh_take_flock() {
  local wait_s="$1" fields="$2" retry="${3:-yes}" host_ticks code ack rc
  local broker_pid_name=RA8_LOCK_BROKER_PID
  [ -f "${RA8_BENCH_BROKER_SRC:-}" ] && [ ! -L "$RA8_BENCH_BROKER_SRC" ] || return 1
  host_ticks="$(sed -n 's/^[^)]*) //p' "/proc/$$/stat" 2>/dev/null | awk '{print $20}')"
  case "$host_ticks" in '' | *[!0-9]*) return 1 ;; *) ;; esac
  # A non-newline sentinel prevents command substitution from stripping the
  # reviewed source's trailing newlines before Python receives it as argv[4].
  code="$(cat -- "$RA8_BENCH_BROKER_SRC" && printf '\001')" || return 1
  code="${code%?}"
  coproc RA8_LOCK_BROKER {
    exec /usr/bin/python3 -I -S -c "$code" "$BH_LOCK" "$BH_REC" \
      "$wait_s" "$fields" "$$" "$host_ticks"
  }
  BH_BROKER_PID="${!broker_pid_name}"
  BH_BROKER_OUTPUT="${RA8_LOCK_BROKER[0]}"
  BH_BROKER_INPUT="${RA8_LOCK_BROKER[1]}"
  if IFS= read -r ack <&"$BH_BROKER_OUTPUT"; then
    case "$ack" in
      "ACQUIRED $BH_BROKER_PID "*) return 0 ;;
      *)
        bh_broker_close
        return 1
        ;;
    esac
  fi
  wait "$BH_BROKER_PID" 2>/dev/null
  rc=$?
  BH_BROKER_PID=""
  bh_close_fd "$BH_BROKER_INPUT"
  bh_close_fd "$BH_BROKER_OUTPUT"
  BH_BROKER_INPUT=""
  BH_BROKER_OUTPUT=""
  [ "$rc" -eq 11 ] || return 1
  [ "$BH_BREAK_GLASS" = "true" ] && [ "$retry" = "yes" ] || return 1
  # Break-glass: preempt the incumbent and try again, IN ONE OPERATION. Doing
  # it as a separate `release` then `hold` would leave a window in which a
  # third actor could take the bench between them -- and the whole reason
  # somebody is breaking glass is that the board is already wedged.
  bh_preempt || return 1
  bh_take_flock 15 "$fields" no
}

# bh_hold <mode> <wait_s> <fields_b64>
bh_hold() {
  local mode="$1" wait_s="$2" fields="$3"
  bh_provision || return "$BH_EXIT_UNKNOWN"
  bh_decode_fields "$fields" || {
    bh_log "FATAL -- malformed hold request"
    return "$BH_EXIT_UNKNOWN"
  }

  bh_take_flock "$wait_s" "$fields" || {
    # Denied. Print the incumbent so the caller can name who to go and ask.
    printf 'bench: DENIED\n'
    # Its exit status is the incumbent's state, which the caller already knows;
    # what is wanted here is the RECORD it prints. No `|| true`: this file runs
    # without errexit precisely so a non-zero verdict is a verdict, not an abort.
    bh_probe
    return "$BH_EXIT_HELD"
  }

  # Entitled to the board, but not yet the only one driving it. A leftover tool
  # from a holder whose hold died is still programming; wait it out before
  # acknowledging, so no acquirer is ever told the board is theirs while
  # somebody else's JLinkExe has the core halted.
  bh_await_quiescent || {
    # Drop the broker as we go: its private descriptor is the kernel hold.
    # this is the one path that takes the lock and then declines to use it.
    bh_broker_close
    return "$BH_EXIT_UNKNOWN"
  }

  bh_journal_add acquire "$BH_LOCK_ID" "$BH_CLASS" "$BH_NAME" "$BH_INTENT"
  [ "$BH_BREAK_GLASS" = "true" ] &&
    bh_journal_add break-glass "$BH_LOCK_ID" "$BH_CLASS" "$BH_NAME" "$BH_INTENT"

  # From here on, dying in any way releases the bench.
  trap 'bh_release_self; exit 0' EXIT
  trap 'exit 0' TERM INT HUP

  # The ack the client waits for. Flushed before we block, and carrying the
  # lock_id the client chose, so a client can never mistake somebody else's
  # holder for its own.
  printf 'bench: ACQUIRED %s\n' "$BH_LOCK_ID"

  bh_block "$mode"
}

# Hold still until the hold ends. This function IS the lease: while it is
# blocked, fd 9 is open and locked; when it returns or dies, the kernel drops
# the lock. Nothing else needs to happen.
bh_block() {
  case "$1" in
    wrapped)
      # THE load-bearing line of the whole design. stdin is the ssh channel;
      # when the client's ssh process dies -- exits, is killed, or loses its
      # socket -- sshd closes this end and `read` returns EOF, so the lock is
      # dropped by a live kernel rather than by a promise anybody made.
      # scripts/hil/bench.sh selftest --ssh-death proves it against a real
      # `kill -9` of the ssh client, because "ssh reaps its remote payload" is
      # exactly the kind of claim that is true right up until it is not.
      while IFS= read -r _line; do :; done
      ;;
    detached)
      # No stdin to watch: a detached hold outlives the command that took it.
      # It is bounded by max_hold_s and the reaper instead, which is why the
      # client refuses to create one without an explicit budget. fd 9 is closed
      # for the sleeper so a stray child can never keep the flock alive after
      # this shell is gone.
      while :; do
        sleep 30 9>&- &
        wait $! || break
      done
      ;;
    *)
      bh_log "FATAL -- unknown hold mode '$1'"
      return "$BH_EXIT_UNKNOWN"
      ;;
  esac
  return "$BH_EXIT_OK"
}

# ---------------------------------------------------------------------------
# release / take
# ---------------------------------------------------------------------------

# bh_release <lock_id|-> [force]
# Signals the holding PROCESS; it does not touch the lock file, because the
# lock is not a file state to be edited. `-` means "whatever holds it now",
# which only the force path uses.
bh_release() {
  local want="$1" force="${2:-}" pid rec_id rec_boot boot cls name
  [ -d "$BH_DIR" ] || return "$BH_EXIT_UNKNOWN"
  if ! bh_flock_held; then
    printf 'bench: already free\n'
    [ -f "$BH_REC" ] && rm -f "$BH_REC" 2>/dev/null
    return "$BH_EXIT_OK"
  fi
  [ -f "$BH_REC" ] || {
    bh_log "the bench is held but no record was written -- cannot identify the holder"
    return "$BH_EXIT_UNKNOWN"
  }
  rec_id="$(bh_json_get "$BH_REC" lock_id)"
  rec_boot="$(bh_json_get "$BH_REC" boot_id)"
  pid="$(bh_json_get "$BH_REC" pid)"
  cls="$(bh_json_get "$BH_REC" holder_class)"
  name="$(bh_json_get "$BH_REC" holder_name)"
  boot="$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)"

  if [ "$rec_boot" != "$boot" ]; then
    bh_log "record predates the current boot -- nothing to release"
    rm -f "$BH_REC" 2>/dev/null || true
    return "$BH_EXIT_OK"
  fi
  if [ "$want" != "-" ] && [ "$want" != "$rec_id" ]; then
    bh_log "refusing: lock_id $want is not the current holder ($rec_id, $cls:$name)"
    return "$BH_EXIT_HELD"
  fi
  case "$pid" in
    '' | *[!0-9]*)
      bh_log "record carries no usable pid"
      return "$BH_EXIT_UNKNOWN"
      ;;
    *) ;;
  esac

  [ "$force" = "force" ] &&
    bh_journal_add force-take "$rec_id" "$cls" "$name" "forced release"
  bh_signal_holder "$pid"
}

# SIGTERM first with a 10s grace window, so the holder's own EXIT trap gets to
# write its journal line; SIGKILL only if it will not go. The verdict is read
# off the FLOCK each time round, not off the signal's exit status -- the
# question is whether the lock is gone, and only the kernel can answer that.
bh_signal_holder() {
  local pid="$1" i=0
  kill -TERM "$pid" 2>/dev/null || true
  while [ "$i" -lt 20 ]; do
    bh_flock_held || {
      printf 'bench: released\n'
      return "$BH_EXIT_OK"
    }
    sleep 0.5
    i=$((i + 1))
  done
  kill -KILL "$pid" 2>/dev/null || true
  i=0
  while [ "$i" -lt 10 ]; do
    bh_flock_held || {
      printf 'bench: released (SIGKILL)\n'
      rm -f "$BH_REC" 2>/dev/null || true
      return "$BH_EXIT_OK"
    }
    sleep 0.5
    i=$((i + 1))
  done
  bh_log "holder pid $pid will not release the lock"
  return "$BH_EXIT_UNKNOWN"
}

# ---------------------------------------------------------------------------
# journal / doctor / reap
# ---------------------------------------------------------------------------

bh_journal_tail() {
  local n="${1:-25}"
  [ -f "$BH_JNL" ] || {
    printf 'bench: no journal at %s\n' "$BH_JNL"
    return "$BH_EXIT_UNKNOWN"
  }
  tail -n "$n" "$BH_JNL"
  return "$BH_EXIT_OK"
}

bh_doctor() {
  local rc=0
  printf 'dir=%s\n' "$BH_DIR"
  if [ -d "$BH_DIR" ]; then
    printf 'dir_present=1\n'
    printf 'dir_mode=%s\n' "$(stat -c '%a %U:%G' "$BH_DIR" 2>/dev/null)"
  else
    printf 'dir_present=0\n'
    rc=3
  fi
  if [ -e "$BH_LOCK" ]; then
    printf 'lock_present=1\n'
    printf 'lock_mode=%s\n' "$(stat -c '%a %U:%G' "$BH_LOCK" 2>/dev/null)"
    printf 'lock_inode=%s\n' "$(stat -c '%i' "$BH_LOCK" 2>/dev/null)"
    if [ -w "$BH_LOCK" ]; then
      printf 'lock_writable=1\n'
    else
      printf 'lock_writable=0\n'
      rc=3
    fi
  else
    printf 'lock_present=0\n'
    rc=3
  fi
  printf 'journal_present=%s\n' "$([ -f "$BH_JNL" ] && echo 1 || echo 0)"
  printf 'journal_lines=%s\n' "$(wc -l <"$BH_JNL" 2>/dev/null || echo 0)"
  printf 'boot_id=%s\n' "$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)"
  printf 'now_iso=%s\n' "$(date -Iseconds)"
  printf 'flock_bin=%s\n' "$(command -v flock 2>/dev/null)"
  [ -n "$(command -v flock 2>/dev/null)" ] || rc=3
  # `is-active` already PRINTS its verdict and exits non-zero for anything but
  # active, so an `|| echo inactive` fallback appended a SECOND line to the
  # substitution and the field came out as two lines.
  printf 'reaper_timer=%s\n' \
    "$(systemctl is-active ra8-bench-reap.timer 2>/dev/null | head -1)"
  return "$rc"
}

# The reaper. An OVERRUN hold is one that is alive but past the budget it
# declared. Agent and CI holders are SIGTERMed; a HUMAN holder is only ever
# flagged, because the human may be standing at the bench with a probe on a
# test point and nothing here can see that.
bh_reap() {
  [ -d "$BH_DIR" ] || return "$BH_EXIT_UNKNOWN"
  bh_flock_held || {
    # Not held: clear a record left behind by a holder that was killed hard.
    [ -f "$BH_REC" ] && {
      bh_journal_add reap-record "$(bh_json_get "$BH_REC" lock_id)" \
        "$(bh_json_get "$BH_REC" holder_class)" \
        "$(bh_json_get "$BH_REC" holder_name)" "record outlived a dead holder"
      rm -f "$BH_REC" 2>/dev/null || true
    }
    printf 'reap: bench free\n'
    return "$BH_EXIT_OK"
  }
  [ -f "$BH_REC" ] || {
    printf 'reap: held, no record -- nothing to judge\n'
    return "$BH_EXIT_OK"
  }
  local cls name lock_id started budget pid now over
  cls="$(bh_json_get "$BH_REC" holder_class)"
  name="$(bh_json_get "$BH_REC" holder_name)"
  lock_id="$(bh_json_get "$BH_REC" lock_id)"
  started="$(bh_json_get "$BH_REC" acquired_epoch)"
  budget="$(bh_json_get "$BH_REC" max_hold_s)"
  pid="$(bh_json_get "$BH_REC" pid)"
  now="$(date +%s)"
  case "$started$budget" in
    '' | *[!0-9]*)
      printf 'reap: record has no usable budget\n'
      return "$BH_EXIT_OK"
      ;;
    *) ;;
  esac
  over=$((now - started - budget))
  if [ "$over" -le 0 ]; then
    printf 'reap: %s:%s within budget (%ss left)\n' "$cls" "$name" "$((-over))"
    return "$BH_EXIT_OK"
  fi
  if [ "$cls" = "human" ]; then
    bh_journal_add overrun "$lock_id" "$cls" "$name" "human hold ${over}s past budget -- flagged, not reaped"
    printf 'reap: OVERRUN %s:%s by %ss -- human holder, flagged only\n' "$cls" "$name" "$over"
    return "$BH_EXIT_OK"
  fi
  bh_journal_add overrun "$lock_id" "$cls" "$name" "${over}s past budget -- reaping"
  printf 'reap: OVERRUN %s:%s by %ss -- reaping\n' "$cls" "$name" "$over"
  bh_release "$lock_id" force
  return $?
}

# bh_extend <lock_id> <extra_seconds> -- push a hold's budget out.
#
# Only the budget moves. The hold itself is unaffected because the hold is not
# a budget -- it is a live process on a flock -- so this changes exactly one
# thing: when the reaper and `status` start calling it an overrun.
bh_extend() {
  local want="$1" extra="$2" cur new
  [ -f "$BH_REC" ] || {
    bh_log "nothing is held"
    return "$BH_EXIT_UNKNOWN"
  }
  [ "$(bh_json_get "$BH_REC" lock_id)" = "$want" ] || {
    bh_log "refusing: $want is not the current holder"
    return "$BH_EXIT_HELD"
  }
  cur="$(bh_json_get "$BH_REC" max_hold_s)"
  case "$cur$extra" in
    '' | *[!0-9]*)
      bh_log "unusable budget"
      return "$BH_EXIT_UNKNOWN"
      ;;
    *) ;;
  esac
  new=$((cur + extra))
  local tmp="$BH_REC.tmp.$$"
  sed "s/^\([[:space:]]*\"max_hold_s\"[[:space:]]*:[[:space:]]*\)[0-9]*,/\1${new},/" \
    "$BH_REC" >"$tmp" 2>/dev/null || return "$BH_EXIT_UNKNOWN"
  chmod 666 "$tmp" 2>/dev/null || true
  mv -f "$tmp" "$BH_REC" 2>/dev/null || {
    rm -f "$tmp" 2>/dev/null
    return "$BH_EXIT_UNKNOWN"
  }
  bh_journal_add extend "$want" "$(bh_json_get "$BH_REC" holder_class)" \
    "$(bh_json_get "$BH_REC" holder_name)" "budget ${cur}s -> ${new}s"
  printf 'bench: budget now %ss\n' "$new"
  return "$BH_EXIT_OK"
}

# bh_touch <lock_id> -- record that the holder just started a bench operation.
# Evidence for the `quiet` classification only; it changes no verdict and
# extends no budget.
bh_touch() {
  [ -f "$BH_REC" ] || return "$BH_EXIT_OK"
  [ "$(bh_json_get "$BH_REC" lock_id)" = "$1" ] || return "$BH_EXIT_OK"
  local now tmp
  now="$(date -Iseconds)"
  tmp="$BH_REC.tmp.$$"
  sed "s/^\([[:space:]]*\"last_activity\"[[:space:]]*:[[:space:]]*\"\)[^\"]*\"/\1${now}\"/" \
    "$BH_REC" >"$tmp" 2>/dev/null || return "$BH_EXIT_OK"
  chmod 666 "$tmp" 2>/dev/null || true
  mv -f "$tmp" "$BH_REC" 2>/dev/null || rm -f "$tmp" 2>/dev/null
  return "$BH_EXIT_OK"
}

# ---------------------------------------------------------------------------
# UNLOCKED-ACTIVITY -- observe, do not trust
# ---------------------------------------------------------------------------
#
# The gate audits committed scripts; it cannot audit somebody typing
# `ssh star JLinkExe ...` by hand, and agents type raw ssh constantly. So the
# status path LOOKS for hardware activity instead of assuming there is none:
# same doctrine as agent_workspace.sh scanning /proc rather than trusting a
# registered pid. This reports; it never kills anything.
bh_activity_scan() {
  local pat proc hits=""
  for pat in JLinkExe JLinkGDBServer rfp-cli openocd esptool esptool.py uhubctl; do
    proc="$(pgrep -a -f "(^|/)$pat" 2>/dev/null | head -3)"
    [ -n "$proc" ] && hits="$hits
$proc"
  done
  # Anyone holding a bench CDC device open: a second console reader splits the
  # byte stream and silently breaks the holder's pattern match.
  local dev
  for dev in /dev/ttyACM*; do
    [ -e "$dev" ] || continue
    proc="$(fuser -v "$dev" 2>&1 | tail -n +2 | head -3)"
    [ -n "$proc" ] && hits="$hits
$dev: $proc"
  done
  printf '%s' "$hits" | tr '\n' ';' | sed 's/^;*//'
}

# ---------------------------------------------------------------------------
# Handover interlock: do not grant a board somebody is still driving
# ---------------------------------------------------------------------------
#
# The flock says who is ENTITLED to the board. It does not say the board is
# idle, and those came apart on the bench in a way that mattered.
#
# A guarded script's hold travels over its own ssh connection, separate from
# the connections it uses to drive the hardware. Kill the hold and the flock
# drops instantly -- correctly, that is the whole design -- while a JLinkExe
# the script started is still running here, halted on the target, because ssh
# does not reap a remote payload that is not reading its stdin. Measured on
# this bench: a J-Link session outlived its own release by 75 s, and three
# other machines took the lock in turn and programmed MRAM underneath it.
#
# lib/bench_lock.sh's fence stops the dead holder's SCRIPT within about a
# second, but it cannot reach across ssh and stop a tool already running here.
# This side can: the acquirer has the flock and nobody else can be entitled to
# the board, so anything still driving it is a leftover, and the honest thing
# is to wait for it rather than hand over a board that is in use.
#
# Only PROGRAMMING and debug tools count. A console reader is a nuisance rather
# than a hazard, and blocking every acquire behind somebody's forgotten `cat`
# would be a cure worse than the disease -- `bh_health` already reports those.
BH_QUIESCE_TOOLS="JLinkExe JLinkGDBServer rfp-cli openocd esptool esptool.py"

# Live programming/debug tools, one per line, or empty when the board is idle.
bh_busy_tools() {
  local pat proc hits=""
  for pat in $BH_QUIESCE_TOOLS; do
    proc="$(pgrep -a -f "(^|/)$pat" 2>/dev/null | head -3)"
    [ -n "$proc" ] && hits="$hits
$proc"
  done
  printf '%s' "$hits" | sed '/^$/d'
}

# Wait for the board to go idle, having already taken the flock.
#   0 -- idle (immediately, or after waiting)
#   3 -- still busy at the deadline. FAIL CLOSED: the caller drops the flock.
bh_await_quiescent() {
  local limit="$BH_QUIESCE_S" waited=0 busy
  case "$limit" in '' | *[!0-9]*) limit=120 ;; *) ;; esac
  [ "$BH_DIR" = "/var/lib/ra8-bench" ] || return "$BH_EXIT_OK"
  busy="$(bh_busy_tools)"
  [ -n "$busy" ] || return "$BH_EXIT_OK"
  bh_log "the flock is ours, but the board is still being driven by a leftover:"
  printf '%s\n' "$busy" | sed 's/^/bench-host:   /' >&2
  bh_log "waiting up to ${limit}s for it to finish before taking the board."
  bh_journal_add quiesce-wait "$BH_LOCK_ID" "$BH_CLASS" "$BH_NAME" \
    "acquired the flock but the board was still busy: $(printf '%s' "$busy" | tr '\n' ';')"
  while [ "$waited" -lt "$limit" ]; do
    sleep 2
    waited=$((waited + 2))
    busy="$(bh_busy_tools)"
    if [ -z "$busy" ]; then
      bh_log "board went idle after ${waited}s -- proceeding."
      bh_journal_add quiesce-ok "$BH_LOCK_ID" "$BH_CLASS" "$BH_NAME" \
        "board idle after ${waited}s"
      return "$BH_EXIT_OK"
    fi
  done
  bh_log "STILL BUSY after ${limit}s. NOT handing over a board somebody is driving."
  printf '%s\n' "$busy" | sed 's/^/bench-host:   /' >&2
  bh_log "if that process is wedged, kill it on the bench host, or force with"
  bh_log "RA8_BENCH_BREAK_GLASS=... after checking nobody is at the bench."
  bh_journal_add quiesce-timeout "$BH_LOCK_ID" "$BH_CLASS" "$BH_NAME" \
    "board still busy after ${limit}s -- refused the handover"
  return "$BH_EXIT_UNKNOWN"
}

bh_health() {
  local act
  # Only meaningful against the real bench: a selftest sandbox has its own lock
  # and no relationship at all to what the J-Link is doing, so reporting real
  # hardware activity as "unlocked" there would be a false alarm by
  # construction.
  if [ "$BH_DIR" != "/var/lib/ra8-bench" ]; then
    printf 'health=OK\n'
    return "$BH_EXIT_OK"
  fi
  act="$(bh_activity_scan)"
  if [ -z "$act" ]; then
    printf 'health=OK\n'
    return "$BH_EXIT_OK"
  fi
  if bh_flock_held; then
    printf 'health=OK\n'
    printf 'activity_evidence=%s\n' "$act"
    return "$BH_EXIT_OK"
  fi
  printf 'health=UNLOCKED-ACTIVITY\n'
  printf 'activity_evidence=%s\n' "$act"
  return "$BH_EXIT_OK"
}

# ---------------------------------------------------------------------------
# Dispatch
# ---------------------------------------------------------------------------

bh_main() {
  local verb="${1:-probe}"
  (($# == 0)) || shift
  case "$verb" in
    provision)
      bh_provision
      return $?
      ;;
    probe)
      bh_probe
      local rc=$?
      bh_health
      return "$rc"
      ;;
    hold)
      bh_hold "${1:-wrapped}" "${2:-0}" "${3:-}"
      return $?
      ;;
    release)
      bh_release "${1:--}" "${2:-}"
      return $?
      ;;
    touch)
      bh_touch "${1:-}"
      return $?
      ;;
    extend)
      bh_extend "${1:-}" "${2:-0}"
      return $?
      ;;
    journal)
      bh_journal_tail "${1:-25}"
      return $?
      ;;
    doctor)
      bh_doctor
      return $?
      ;;
    reap)
      bh_reap
      return $?
      ;;
    health)
      bh_health
      return $?
      ;;
    *)
      bh_log "unknown verb '$verb'"
      return "$BH_EXIT_UNKNOWN"
      ;;
  esac
}

bh_main "$@"
