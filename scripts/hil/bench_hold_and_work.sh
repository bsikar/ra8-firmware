#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# bench_hold_and_work.sh -- take the bench through the guard and then really
# use it, continuously, for a stated number of seconds.
#
# This exists for scripts/hil/bench_contention.sh. A contention test needs an
# actor that OCCUPIES the hardware for a controlled span, not one that sleeps
# while holding a lock: a sleeping holder proves only that the bookkeeping is
# consistent with itself, and would look identical whether the board were
# protected or not.
#
# So the work is a real J-Link session -- connect, halt, read the reset vector,
# hold the probe, read it again, release the core -- for the whole duration.
# The J-Link Commander `sleep` runs INSIDE the session, so a single JLinkExe
# process owns the probe end to end and appears in the bench witness as one
# unbroken interval, rather than as a string of short ones with gaps a
# competitor could slip through unseen.
#
# It is deliberately READ-ONLY. Nothing here programs MRAM, erases anything or
# touches an option byte; the contention experiment gets its programming
# evidence from real `flash.sh` runs, and this is the payload for the phases
# where a long, safe, unmistakable occupancy is what is wanted.
#
# Usage:
#   /bin/bash -p scripts/hil/bench_hold_and_work.sh [seconds]     # default 10
#
# Exit codes:
#   0  held the bench and did the work
#   1  DENIED -- somebody else holds the bench (and RA8_BENCH_WAIT_S ran out)
#   2  usage / Pi unreachable
#   3  UNKNOWN -- could not establish a hold; the bench was NOT touched
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

  _hil_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

  # shellcheck source=scripts/hil/lib/rig_env.sh
  source "$_hil_dir/lib/rig_env.sh"
  rig_require PI_HOST JLINK_SN

  SECONDS_TO_HOLD="${1:-10}"
  case "$SECONDS_TO_HOLD" in
    '' | *[!0-9]*)
      echo "usage: $0 [seconds]" >&2
      exit 2
      ;;
    *) ;;
  esac

  # ---- bench mutual exclusion --------------------------------------------------
  # The hold lives exactly as long as this script does. See scripts/hil/bench.sh.
  # shellcheck source=scripts/hil/lib/bench_lock.sh
  source "$_hil_dir/lib/bench_lock.sh"
  ra8_bench_require "read-only J-Link occupancy for ${SECONDS_TO_HOLD}s (contention test)" \
    "$((SECONDS_TO_HOLD + 300))s" || exit $?

  printf '[hold-and-work] occupying the J-Link for %ss from %s\n' \
    "$SECONDS_TO_HOLD" "$(date +%s.%N)" >&2

  jlink_script() {
    cat <<EOF
device ${JLINK_DEVICE}
si SWD
speed 1000
connect
halt
mem32 0x02000000, 4
sleep $((SECONDS_TO_HOLD * 1000))
mem32 0x02000000, 4
go
q
EOF
  }

  run_session() {
    local script="$1" log="$2"
    JLinkExe -nogui 1 -SelectEmuBySN "${JLINK_SN}" -commanderscript "$script" >"$log" 2>&1 || true
    grep -q "Connecting to J-Link via USB\.\.\.O\.K\." "$log"
  }

  if rig_is_local_pi; then
    SCRIPT="$(mktemp /tmp/ra8-hold-work.XXXXXX.jlink)"
    LOG="$(mktemp /tmp/ra8-hold-work.XXXXXX.log)"
    trap 'rm -f "$SCRIPT" "$LOG"' EXIT
    jlink_script >"$SCRIPT"
    run_session "$SCRIPT" "$LOG" || {
      echo "[hold-and-work] J-Link did not connect" >&2
      exit 1
    }
  else
    # shellcheck disable=SC2087  # client-side substitution of JLINK_DEVICE/JLINK_SN and the
    # hold duration is intentional: the bench host has no .env and no checkout.
    ssh -o BatchMode=yes -o ConnectTimeout=8 "$PI_HOST" /bin/bash -p <<REMOTE || exit 2
set -euo pipefail
S=\$(mktemp /tmp/ra8-hold-work.XXXXXX.jlink)
L=\$(mktemp /tmp/ra8-hold-work.XXXXXX.log)
trap 'rm -f "\$S" "\$L"' EXIT
cat > "\$S" <<'JLINK'
$(jlink_script)
JLINK
JLinkExe -nogui 1 -SelectEmuBySN '${JLINK_SN}' -commanderscript "\$S" > "\$L" 2>&1 || true
grep -q 'Connecting to J-Link via USB\.\.\.O\.K\.' "\$L" || { cat "\$L" >&2; exit 1; }
echo "[hold-and-work] J-Link session complete at \$(date +%s.%N)"
REMOTE
  fi

  printf '[hold-and-work] done at %s\n' "$(date +%s.%N)" >&2
else
  [[ "$-" == *p* ]]
fi
