#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# bench_exit_traps_selftest.sh -- bench-free regression tests for the EXIT
# handler machinery in lib/bench_lock.sh.
#
# Usage:
#   /bin/bash -p scripts/hil/lib/bench_exit_traps_selftest.sh --selftest
#
# The bench lock must work under stock macOS Bash 3.2, where expanding an
# empty "${array[@]}" under `set -u` is a fatal unbound-variable error (fixed
# in Bash 4.4). These tests run that machinery with zero, one, and many
# handlers, through real EXIT traps and a cross-process inherited lock id,
# without touching the network: bench_lock_id_now and bench_host are stubbed
# wherever the nested-hold path would otherwise reach the bench host.
#
# Every case runs with nounset active, so a regression fails loudly here
# rather than surfacing later as `bench_lock.sh: _RA8_BENCH_EXIT_HANDLERS[@]:
# unbound variable` halfway into `just hil::run`.

if [[ "$-" == *p* ]]; then
  unset -v BASH_ENV ENV
  declare -a ra8_startup_env_unset=()
  _ra8_startup_refuse() {
    printf 'error: privileged startup %s\n' "$1" >&2
    exit 1
  }
  ra8_startup_env_done_count=0
  while IFS= read -r -d '' ra8_startup_env_row; do
    ra8_startup_env_name="${ra8_startup_env_row%%=*}"
    case "$ra8_startup_env_name" in
      RA8_STARTUP_ENV_DONE)
        ra8_startup_env_done_count=$((ra8_startup_env_done_count + 1))
        ;;
      BASH_FUNC_*%% | BASH_FUNC_*'()') ra8_startup_env_unset+=(-u "$ra8_startup_env_name") ;;
    esac
  done < <(
    /usr/bin/env -u RA8_STARTUP_ENV_DONE -0 &&
      /usr/bin/printf 'RA8_STARTUP_ENV_DONE=1\0'
  )
  ((ra8_startup_env_done_count == 1)) && [[ "$ra8_startup_env_name" == RA8_STARTUP_ENV_DONE ]] || _ra8_startup_refuse 'environment enumeration was incomplete'
  if ((${#ra8_startup_env_unset[@]})); then
    [[ -z "${RA8_STARTUP_ENV_SCRUBBED-}" ]] || _ra8_startup_refuse 'scrub did not converge'
    ra8_startup_reentry="$0"
    [[ "$ra8_startup_reentry" == */* ]] || _ra8_startup_refuse 'requires a script path'
    if [[ "$ra8_startup_reentry" != /* ]]; then
      ra8_startup_reentry="$PWD/$ra8_startup_reentry"
    fi
    ra8_startup_check="$ra8_startup_reentry"
    while [[ "$ra8_startup_check" != "/" ]]; do
      [[ ! -L "$ra8_startup_check" ]] || _ra8_startup_refuse 'refuses a symlinked path'
      ra8_startup_parent="${ra8_startup_check%/*}"
      [[ -n "$ra8_startup_parent" ]] || ra8_startup_parent="/"
      [[ "$ra8_startup_parent" != "$ra8_startup_check" ]] ||
        _ra8_startup_refuse 'cannot validate its script path'
      ra8_startup_check="$ra8_startup_parent"
    done
    [[ -f "$ra8_startup_reentry" ]] || _ra8_startup_refuse 'refuses a non-regular path'
    if ! exec /usr/bin/env "${ra8_startup_env_unset[@]}" -u BASH_ENV -u ENV \
      -u RA8_STARTUP_ENV_DONE RA8_STARTUP_ENV_SCRUBBED=1 \
      /bin/bash -p -- "$ra8_startup_reentry" "$@"; then
      _ra8_startup_refuse 'could not enter sanitized process'
    fi
  fi
  unset -v ra8_startup_check ra8_startup_env_done_count
  unset -v ra8_startup_env_name ra8_startup_env_row
  unset -v ra8_startup_env_unset ra8_startup_parent ra8_startup_reentry
  unset -v RA8_STARTUP_ENV_DONE
  unset -v RA8_STARTUP_ENV_SCRUBBED
  unset -f _ra8_startup_refuse

  set -euo pipefail

  _EXIT_ST_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  # shellcheck source=scripts/hil/lib/bench_lock.sh
  source "$_EXIT_ST_LIB_DIR/bench_lock.sh"

  _EXIT_ST_TMP=""
  _EXIT_ST_FAILURES=0

  # Marker recorders, one per case file. Top-level so the dispatcher resolves
  # them in any subshell; registered by name, exactly like production.
  rec_one() { printf 'one\n' >>"$_EXIT_ST_TMP/one"; }
  rec_a() { printf 'a\n' >>"$_EXIT_ST_TMP/many"; }
  rec_b() { printf 'b\n' >>"$_EXIT_ST_TMP/many"; }
  rec_c() { printf 'c\n' >>"$_EXIT_ST_TMP/many"; }
  rec_composed() { printf 'guard\n' >>"$_EXIT_ST_TMP/composed"; }
  rec_clean() { printf 'clean\n' >>"$_EXIT_ST_TMP/clean"; }
  rec_failing() { printf 'failing\n' >>"$_EXIT_ST_TMP/failing"; }

  # Fresh handler state for one case. White-box by design: the regression is
  # precisely about this collection's empty expansion under nounset.
  _exit_st_reset() {
    _RA8_BENCH_EXIT_HANDLERS=()
    _RA8_BENCH_EXIT_DISPATCH_INSTALLED=0
    _RA8_BENCH_ORIGINAL_EXIT_TRAP=""
    trap - EXIT
  }

  _exit_st_cleanup_tmp() {
    rm -rf "$_EXIT_ST_TMP"
    _EXIT_ST_TMP=""
  }

  run_case() {
    local label="$1"
    shift
    if "$@"; then
      printf '  ok: %s\n' "$label"
    else
      printf '  FAIL: %s\n' "$label" >&2
      _EXIT_ST_FAILURES=$((_EXIT_ST_FAILURES + 1))
    fi
  }

  # Zero handlers: dispatch is a silent no-op, not an unbound-variable error.
  t_zero_handlers() (
    set -euo pipefail
    _exit_st_reset
    _ra8_bench_run_exit_traps 2>"$_EXIT_ST_TMP/zero.err"
    rc=$?
    [ "$rc" -eq 0 ] || {
      printf 'zero-handler dispatch exited %s\n' "$rc" >&2
      return 1
    }
    [ ! -s "$_EXIT_ST_TMP/zero.err" ] || {
      cat "$_EXIT_ST_TMP/zero.err" >&2
      return 1
    }
  )

  # First registration onto the empty collection must not fail either.
  t_first_add() (
    set -euo pipefail
    _exit_st_reset
    _ra8_bench_add_exit_trap true 2>"$_EXIT_ST_TMP/first.err"
    [ "${#_RA8_BENCH_EXIT_HANDLERS[@]}" -eq 1 ] || return 1
    [ ! -s "$_EXIT_ST_TMP/first.err" ] || return 1
  )

  # One handler runs exactly once through the dispatcher.
  t_one_handler() (
    set -euo pipefail
    _exit_st_reset
    : >"$_EXIT_ST_TMP/one"
    _ra8_bench_add_exit_trap rec_one
    _ra8_bench_run_exit_traps 2>"$_EXIT_ST_TMP/one.err"
    [ "$(cat "$_EXIT_ST_TMP/one")" = "one" ] || return 1
    [ ! -s "$_EXIT_ST_TMP/one.err" ] || return 1
  )

  # Several handlers run newest-first; none is dropped or doubled.
  t_many_handlers() (
    set -euo pipefail
    _exit_st_reset
    : >"$_EXIT_ST_TMP/many"
    _ra8_bench_add_exit_trap rec_a
    _ra8_bench_add_exit_trap rec_b
    _ra8_bench_add_exit_trap rec_c
    _ra8_bench_run_exit_traps 2>"$_EXIT_ST_TMP/many.err"
    [ "$(cat "$_EXIT_ST_TMP/many")" = "$(printf 'c\nb\na')" ] || return 1
    [ ! -s "$_EXIT_ST_TMP/many.err" ] || return 1
  )

  # A caller EXIT trap installed before the guard still runs, after ours.
  t_caller_trap_composed() (
    set -euo pipefail
    _exit_st_reset
    : >"$_EXIT_ST_TMP/composed"
    trap 'printf "caller\n" >>"$_EXIT_ST_TMP/composed"' EXIT
    _ra8_bench_add_exit_trap rec_composed
    exit 0
  )

  # Normal exit: cleanup runs once, exit status stays the script's own. The
  # trap comes from the production registration path, never installed twice.
  t_clean_exit() (
    set -euo pipefail
    : >"$_EXIT_ST_TMP/clean"
    (
      _exit_st_reset
      _ra8_bench_add_exit_trap rec_clean
      exit 0
    )
    rc=$?
    [ "$rc" -eq 0 ] || return 1
    [ "$(grep -c . "$_EXIT_ST_TMP/clean")" -eq 1 ] || return 1
  )

  # Failing exit: cleanup still runs once, and the failure status -- not the
  # handler's -- is what the caller sees.
  t_failing_exit() (
    set -euo pipefail
    : >"$_EXIT_ST_TMP/failing"
    (
      _exit_st_reset
      _ra8_bench_add_exit_trap rec_failing
      exit 5
    )
    rc=$?
    [ "$rc" -eq 5 ] || return 1
    [ "$(grep -c . "$_EXIT_ST_TMP/failing")" -eq 1 ] || return 1
  )

  # Releasing with nothing held is a silent no-op, twice in a row.
  t_release_idempotent() {
    ra8_bench_release_local || return 1
    ra8_bench_release_local || return 1
  }

  # Nested hold: a second require inside our own lock short-circuits -- no new
  # handler, no new holder -- while still touching liveness. Network stubs keep
  # this bench-free; everything stays in a subshell so the stubs cannot leak.
  t_nested_hold() (
    set -euo pipefail
    bench_lock_id_now() { printf 'st-nested'; }
    bench_host() {
      printf 'touch\n' >>"$_EXIT_ST_TMP/nested-host"
      return 0
    }
    RA8_BENCH_LOCK_ID="st-nested"
    unset RA8_BENCH_HOLDER_PID
    _exit_st_reset
    _ra8_bench_add_exit_trap true
    before="${#_RA8_BENCH_EXIT_HANDLERS[@]}"
    ra8_bench_require "selftest: nested" >/dev/null 2>"$_EXIT_ST_TMP/nested.err"
    rc=$?
    [ "$rc" -eq 0 ] || {
      cat "$_EXIT_ST_TMP/nested.err" >&2
      return 1
    }
    [ "${#_RA8_BENCH_EXIT_HANDLERS[@]}" -eq "$before" ] || return 1
    [ -z "${RA8_BENCH_HOLDER_PID:-}" ] || return 1
    touches=0
    if [ -f "$_EXIT_ST_TMP/nested-host" ]; then
      touches="$(awk 'END { print NR+0 }' "$_EXIT_ST_TMP/nested-host")"
    fi
    [ "$touches" -eq 1 ] || return 1
  )

  # Inherited lock id across processes: a child that inherits RA8_BENCH_LOCK_ID
  # through the environment takes the same short-circuit without ssh. The
  # env-prefix form exports for that one command only, never the harness.
  t_inherited_lock_id() (
    set -euo pipefail
    RA8_BENCH_LIBDIR="$_EXIT_ST_LIB_DIR" \
      RA8_BENCH_LOCK_ID="st-inherited" \
      /bin/bash -p -c '
        set -euo pipefail
        # Absolute test-time path, exported by the parent case above.
        source "$RA8_BENCH_LIBDIR/bench_lock.sh"
        bench_lock_id_now() { printf "%s" "$RA8_BENCH_LOCK_ID"; }
        bench_host() { return 0; }
        unset RA8_BENCH_HOLDER_PID
        _RA8_BENCH_EXIT_HANDLERS=()
        _RA8_BENCH_EXIT_DISPATCH_INSTALLED=0
        _RA8_BENCH_ORIGINAL_EXIT_TRAP=""
        _ra8_bench_add_exit_trap true
        before=${#_RA8_BENCH_EXIT_HANDLERS[@]}
        ra8_bench_require "selftest: inherited" >/dev/null 2>&1 || exit 1
        [ "${#_RA8_BENCH_EXIT_HANDLERS[@]}" -eq "$before" ] || exit 1
        [ -z "${RA8_BENCH_HOLDER_PID:-}" ] || exit 1
      ' 2>"$_EXIT_ST_TMP/inherited.err"
    rc=$?
    [ "$rc" -eq 0 ] || {
      cat "$_EXIT_ST_TMP/inherited.err" >&2
      return 1
    }
  )

  cmd_selftest() {
    _EXIT_ST_TMP="$(mktemp -d "${TMPDIR:-/tmp}/ra8-bench-exit-traps.XXXXXXXX")" || return 1
    # The harness itself consumes the production registration path, so the
    # first-add direction is exercised outside any subshell as well.
    _ra8_bench_add_exit_trap _exit_st_cleanup_tmp
    run_case "zero handlers dispatch silently" t_zero_handlers
    run_case "first registration onto empty set" t_first_add
    run_case "one handler runs once" t_one_handler
    run_case "many handlers run newest-first" t_many_handlers
    t_caller_trap_composed
    if [ "$(cat "$_EXIT_ST_TMP/composed")" = "$(printf 'guard\ncaller')" ]; then
      printf '  ok: caller EXIT trap is composed, not replaced\n'
    else
      printf '  FAIL: caller EXIT trap is composed, not replaced\n' >&2
      _EXIT_ST_FAILURES=$((_EXIT_ST_FAILURES + 1))
    fi
    run_case "cleanup runs once on clean exit" t_clean_exit
    run_case "cleanup runs once, status preserved, on failure" t_failing_exit
    run_case "release without a hold is a silent no-op" t_release_idempotent
    run_case "nested require short-circuits with liveness touch" t_nested_hold
    run_case "inherited RA8_BENCH_LOCK_ID short-circuits cross-process" t_inherited_lock_id
    if [ "$_EXIT_ST_FAILURES" -eq 0 ]; then
      printf 'bench_exit_traps_selftest: PASS (3.2-safe dispatch, order, traps, nested/inherited holds)\n'
      return 0
    fi
    printf 'bench_exit_traps_selftest: %s failure(s)\n' "$_EXIT_ST_FAILURES" >&2
    return 1
  }

  case "${1:-}" in
    --selftest) cmd_selftest ;;
    *)
      echo "Usage: /bin/bash -p scripts/hil/lib/bench_exit_traps_selftest.sh --selftest" >&2
      exit 2
      ;;
  esac
else
  [[ "$-" == *p* ]]
fi
