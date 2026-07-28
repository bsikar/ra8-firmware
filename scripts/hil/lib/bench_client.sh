#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# bench_client.sh -- the CLIENT-side machinery of the bench lock: how a
# workstation, an agent session or a CI runner reaches the bench host, names
# itself, and binds a hold to a live process.
#
# `source` this (do not execute it). It is shared by scripts/hil/bench.sh (the
# human/CLI face) and by the sourced guard that HIL scripts call, so there is
# one implementation of "how a hold is taken" and not two that can disagree.
# Nothing here decides anything: every lock operation and every timestamp
# happens on the bench host, in lib/bench_host.sh, against the bench host's
# clock.
#
# The caller must already have sourced lib/rig_env.sh (for PI_HOST and
# rig_is_local_pi) and defined RA8_BENCH_DIR and the three exit-code constants.
#
# Portability: bash 3.2 (the macOS system bash) -- no name-refs, no mapfile.

# ---------------------------------------------------------------------------
# The contract, declared here rather than assumed
# ---------------------------------------------------------------------------
#
# Stated in one place the way lib/hil_conf.sh and lib/rig_env.sh state theirs,
# so a consumer that forgot to set one of these gets a working default instead
# of an unbound-variable abort under `set -u`. `:=` never clobbers a value the
# sourcing script already set (including a `readonly` one), so bench.sh stays
# the authority on all five.
: "${RA8_BENCH_EXIT_FREE:=0}"
: "${RA8_BENCH_EXIT_HELD:=1}"
: "${RA8_BENCH_EXIT_UNKNOWN:=3}"
: "${RA8_BENCH_DIR:=/var/lib/ra8-bench}"
_bench_lib_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${RA8_BENCH_HOST_SRC:=$_bench_lib_dir/bench_host.sh}"

# PI_HOST and rig_is_local_pi come from rig_env.sh. Pulled in HERE rather than
# left to the caller: several scripts source the guard before their own
# rig_env.sh line, and a guard whose correctness depends on the order somebody
# happened to write two `source` statements in is not a guard. Idempotent --
# skipped when the caller already brought it in.
if ! declare -f rig_is_local_pi >/dev/null 2>&1; then
  # shellcheck source=scripts/hil/lib/rig_env.sh
  source "$_bench_lib_dir/rig_env.sh"
fi

bench_say() { printf 'bench: %s\n' "$*" >&2; }

# ---------------------------------------------------------------------------
# Argument helpers
# ---------------------------------------------------------------------------

# bench_duration <15m|2h|900s|900> -- seconds, or empty when unparseable.
bench_duration() {
  local d="${1:-}" n unit
  [ -n "$d" ] || return 0
  n="${d%[smhSMH]}"
  unit="${d#"$n"}"
  case "$n" in
    '' | *[!0-9]*) return 0 ;;
    *) ;;
  esac
  case "$unit" in
    h | H) printf '%s' $((n * 3600)) ;;
    m | M) printf '%s' $((n * 60)) ;;
    s | S | '') printf '%s' "$n" ;;
    *) return 0 ;;
  esac
}

# Single-quote a value for safe interpolation into a remote command string.
bench_q() { printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"; }

# base64 with no line wrapping, on both a Mac client and a Linux one.
bench_b64() { base64 <"$1" | tr -d '\n'; }

# ---------------------------------------------------------------------------
# Transport: run the bench-host half, there
# ---------------------------------------------------------------------------
#
# The host half is shipped BY VALUE (base64 on the command line) rather than
# invoked from a checkout, exactly the way rig_env.sh ships tty_resolve.sh into
# a remote heredoc. The bench Pi's copy of this tree is whatever a suite last
# left there; the lock must not depend on it.
bench_host_cmd() {
  local b64 out arg
  b64="$(bench_b64 "$RA8_BENCH_HOST_SRC")" || return 1
  out="f=\$(mktemp /tmp/ra8-bench-host.XXXXXX); printf %s $(bench_q "$b64") | base64 -d >\"\$f\"; "
  out="${out}RA8_BENCH_DIR=$(bench_q "$RA8_BENCH_DIR") exec bash \"\$f\""
  for arg in "$@"; do
    out="$out $(bench_q "$arg")"
  done
  printf '%s' "$out"
}

# ssh options that make a dead client a PROMPT release rather than an eventual
# one: without ServerAlive the client survives a partition until TCP gives up,
# and the bench stays locked by a machine that cannot reach it. -T because the
# holder's stdin must be a plain pipe -- a pty would keep the remote side alive
# past the client's death, which is precisely what must not happen.
RA8_BENCH_SSH_OPTS=(-T -o BatchMode=yes -o ConnectTimeout=8
  -o ServerAliveInterval=15 -o ServerAliveCountMax=4)

# bench_host <verb> [args...] -- run the host half and return its exit code.
# stdin is /dev/null: this form is for the verbs that answer a question, never
# for `hold`, whose stdin IS the liveness channel.
bench_host() {
  local cmd
  cmd="$(bench_host_cmd "$@")" || {
    bench_say "cannot read $RA8_BENCH_HOST_SRC"
    return "$RA8_BENCH_EXIT_UNKNOWN"
  }
  if rig_is_local_pi; then
    local t rc
    t="$(mktemp "${TMPDIR:-/tmp}/ra8-bench-host.XXXXXX")" || return "$RA8_BENCH_EXIT_UNKNOWN"
    cp "$RA8_BENCH_HOST_SRC" "$t" || return "$RA8_BENCH_EXIT_UNKNOWN"
    RA8_BENCH_DIR="$RA8_BENCH_DIR" bash "$t" "$@" </dev/null
    rc=$?
    rm -f "$t" 2>/dev/null || true
    return "$rc"
  fi
  [ -n "${PI_HOST:-}" ] || {
    bench_say "PI_HOST is not set -- copy .env.example to .env and fill it in"
    return "$RA8_BENCH_EXIT_UNKNOWN"
  }
  # shellcheck disable=SC2029  # $cmd is composed HERE on purpose: it carries this file's own text, base64'd, so the bench host needs no checkout.
  ssh "${RA8_BENCH_SSH_OPTS[@]}" "$PI_HOST" "$cmd" </dev/null
}

# ---------------------------------------------------------------------------
# Identity
# ---------------------------------------------------------------------------

# Default to `agent`, never to `human`. The precedence rules give a human the
# power to preempt, so guessing "human" would hand an unattended agent the
# strongest class in the system. Guessing the WEAKER class is the fail-safe
# direction; --as says otherwise explicitly.
bench_default_class() {
  if [ -n "${RA8_BENCH_CLASS:-}" ]; then
    printf '%s' "$RA8_BENCH_CLASS"
  elif [ "${GITHUB_ACTIONS:-}" = "true" ]; then
    printf 'ci'
  else
    printf 'agent'
  fi
}

bench_default_name() {
  local cls="$1"
  if [ -n "${RA8_BENCH_ACTOR:-}" ]; then
    printf '%s' "$RA8_BENCH_ACTOR"
    return 0
  fi
  case "$cls" in
    human) printf '%s' "${USER:-$(id -un 2>/dev/null)}" ;;
    ci) printf 'ci' ;;
    *) printf 'agent:%s' "$(hostname -s 2>/dev/null || echo unknown)/$$" ;;
  esac
}

bench_origin() {
  if [ -n "${GITHUB_RUN_ID:-}" ]; then
    printf '%s/%s/actions/runs/%s' "${GITHUB_SERVER_URL:-https://github.com}" \
      "${GITHUB_REPOSITORY:-}" "$GITHUB_RUN_ID"
  elif [ -n "${SSH_CONNECTION:-}" ]; then
    printf '%s' "$SSH_CONNECTION"
  else
    printf '%s@%s' "${USER:-unknown}" "$(hostname -s 2>/dev/null || echo unknown)"
  fi
}

bench_git_ref() {
  git -C "$_bench_lib_dir" rev-parse --short HEAD 2>/dev/null || printf 'unknown'
}

bench_new_lock_id() {
  od -An -N8 -tx1 /dev/urandom 2>/dev/null | tr -d ' \n' || printf '%s%s' "$$" "$(date +%s)"
}

# The client's half of the record, as key=value lines, base64'd so an intent
# string can contain anything at all without a quoting accident becoming a
# shell injection over ssh. Every TIMESTAMP is added by the host, from the
# host's clock -- no client time is ever sent, so clock skew cannot reach a
# decision.
bench_fields_b64() {
  {
    printf 'resource=bench\n'
    printf 'lock_id=%s\n' "$1"
    printf 'holder_class=%s\n' "$2"
    printf 'holder_name=%s\n' "$3"
    printf 'intent=%s\n' "$4"
    printf 'max_hold_s=%s\n' "$5"
    printf 'hold_kind=%s\n' "$6"
    printf 'break_glass=%s\n' "$7"
    printf 'confirm=%s\n' "${8:-}"
    printf 'origin=%s\n' "$(bench_origin)"
    printf 'git_ref=%s\n' "$(bench_git_ref)"
  } | base64 | tr -d '\n'
}

# ---------------------------------------------------------------------------

# bench_field <probe-text> <key> [default]
# `probe` emits one key=value per line with newline-free values, so a field is
# a `sed` away. Reading the fields this side (rather than eval-ing the whole
# document into variables) keeps a malformed or hostile probe line from being
# able to define anything in this shell.
bench_field() {
  local v
  v="$(printf '%s\n' "$1" | sed -n "s/^$2=//p" | head -1)"
  [ -n "$v" ] || v="${3:-}"
  printf '%s' "$v"
}

bench_human_age() {
  local secs="$1"
  case "$secs" in
    '' | *[!0-9-]*) printf 'unknown' && return 0 ;;
    *) ;;
  esac
  if [ "$secs" -lt 60 ]; then
    printf '%ss' "$secs"
  elif [ "$secs" -lt 3600 ]; then
    printf '%sm' $((secs / 60))
  else
    printf '%sh%sm' $((secs / 3600)) $(((secs % 3600) / 60))
  fi
}

# ---------------------------------------------------------------------------
# Holding
# ---------------------------------------------------------------------------

# bench_start_holder <lock_id> <class> <name> <intent> <budget_s> <wait_s>
#                    <break_glass> <fifo> <outfile> [confirm]
#
# Starts the bench-host holder with its stdin connected to <fifo> and its
# stdout to <outfile>, and sets RA8_BENCH_HOLDER_PID. The CALLER then opens the
# write end of the fifo and keeps it open for exactly as long as it wants the
# bench: closing it -- or dying, which closes it too -- is the release.
#
# The pid is returned in a GLOBAL rather than on stdout, and that is not a
# style choice. A `$(...)` would run the background job in a subshell, so
# (a) the holder would be reparented the moment the subshell exited and `wait`
# could never reach it, and (b) the blocking open of the fifo happens with the
# command substitution's pipe still held, which deadlocks outright -- the
# parent waits for output that the blocked child is holding the pipe for.
# Redirection ORDER matters for the same reason: stdout and stderr are moved
# off the inherited fds BEFORE stdin blocks on the fifo.
# shellcheck disable=SC2034  # read by bench.sh and lib/bench_selftest.sh -- it is this library's output parameter.
RA8_BENCH_HOLDER_PID=""
bench_start_holder() {
  local lock_id="$1" cls="$2" name="$3" intent="$4" budget="$5" wait_s="$6"
  local glass="$7" fifo="$8" out="$9"
  local confirm="${10:-}"
  local fields cmd
  RA8_BENCH_HOLDER_PID=""
  fields="$(bench_fields_b64 "$lock_id" "$cls" "$name" "$intent" "$budget" wrapped \
    "$glass" "$confirm")"
  if rig_is_local_pi; then
    local t
    t="$(mktemp "${TMPDIR:-/tmp}/ra8-bench-host.XXXXXX")" || return 1
    cp "$RA8_BENCH_HOST_SRC" "$t" || return 1
    RA8_BENCH_DIR="$RA8_BENCH_DIR" bash "$t" hold wrapped "$wait_s" "$fields" \
      >"$out" 2>&1 <"$fifo" &
    RA8_BENCH_HOLDER_PID="$!"
    return 0
  fi
  [ -n "${PI_HOST:-}" ] || return 1
  cmd="$(bench_host_cmd hold wrapped "$wait_s" "$fields")" || return 1
  # shellcheck disable=SC2029  # $cmd is composed here on purpose (see bench_host_cmd); the fifo is what the remote side blocks on.
  ssh "${RA8_BENCH_SSH_OPTS[@]}" "$PI_HOST" "$cmd" >"$out" 2>&1 <"$fifo" &
  # shellcheck disable=SC2034  # this function's output parameter; read by bench.sh and lib/bench_selftest.sh.
  RA8_BENCH_HOLDER_PID="$!"
  return 0
}

# bench_await_ack <lock_id> <outfile> <holder_pid> <deadline_s>
#   0 acquired, 1 denied, 3 could not tell
bench_await_ack() {
  local lock_id="$1" out="$2" pid="$3" limit="$4" waited=0
  while [ "$waited" -lt "$limit" ]; do
    if grep -q "^bench: ACQUIRED $lock_id\$" "$out" 2>/dev/null; then
      return "$RA8_BENCH_EXIT_FREE"
    fi
    if grep -q '^bench: DENIED' "$out" 2>/dev/null; then
      return "$RA8_BENCH_EXIT_HELD"
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
      return "$RA8_BENCH_EXIT_UNKNOWN"
    fi
    sleep 1
    waited=$((waited + 1))
  done
  return "$RA8_BENCH_EXIT_UNKNOWN"
}

# Render the incumbent from a denied holder's output, so a refusal names who to
# go and ask rather than just saying no.
bench_report_denial() {
  local out="$1" who="" what="" since=""
  who="$(sed -n 's/^f_holder_name=//p' "$out" 2>/dev/null | head -1)"
  what="$(sed -n 's/^f_intent=//p' "$out" 2>/dev/null | head -1)"
  since="$(sed -n 's/^f_acquired_at=//p' "$out" 2>/dev/null | head -1)"
  bench_say "DENIED -- the bench is held by ${who:-someone}"
  [ -n "$what" ] && bench_say "  intent: $what"
  [ -n "$since" ] && bench_say "  since:  $since"
  bench_say "  run \`make bench-status\` for the full record."
}
