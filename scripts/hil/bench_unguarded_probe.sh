#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# bench_unguarded_probe.sh -- the NEGATIVE CONTROL for the bench-lock
# contention experiment. It touches the bench WITHOUT taking the lock, on
# purpose, and it is the only file in this tree allowed to.
#
# WHY THIS HAS TO EXIST
# ---------------------
# The headline claim of scripts/hil/bench_contention.sh is "the witness never
# saw two machines on the board at once". That claim is worth nothing until the
# witness has been shown capable of seeing exactly that. A detector that
# quietly stopped matching reports an empty timeline, and an empty timeline
# reads as a clean run -- which is this repository's most frequent tooling
# failure, not a hypothetical one.
#
# So the same actors are pointed at the same bench at the same instant with no
# lock at all, and the run is REQUIRED to collide. If it does not, the
# experiment is inconclusive and says so, instead of reporting a pass it did
# not earn.
#
# It also settles a question that turned out not to be rhetorical. Two
# concurrent JLinkExe sessions against one probe were measured on this bench:
# both connected, both read memory, both exited 0. The J-Link OB enforces
# nothing. There is no hardware interlock underneath the software lock, so the
# software lock is the only thing there is.
#
# WHAT IT IS ALLOWED TO DO
# ------------------------
# Read, and only read: connect, halt, read the reset vector, release the core,
# and read the console. It never programs MRAM, never erases, never writes an
# option byte and never cuts power. Two interleaved WRITES are the collision
# that could leave the board needing recovery, and proving the witness works
# does not require staging one.
#
# It refuses to run unless RA8_BENCH_NEGATIVE_CONTROL=1 is set, so it cannot be
# reached by a typo, a tab-completion or a copied command line.
#
# Exit codes:
#   0  the unguarded probe ran
#   2  refused (opt-in not set) or the rig is not configured
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

  if [ "${RA8_BENCH_NEGATIVE_CONTROL:-0}" != "1" ]; then
    cat >&2 <<'REFUSE'
bench_unguarded_probe.sh: REFUSED.

This script touches the bench WITHOUT taking the bench lock. It exists only as
the negative control for scripts/hil/bench_contention.sh -- to prove the bench
witness can actually see two machines on the board at once, so that "no overlap
observed" is a finding rather than an artefact.

If you want to use the bench, use a guarded entry point:
    just hil::probe / just hil::flash <app> / just hil::hold "why"

To run the negative control deliberately:
    RA8_BENCH_NEGATIVE_CONTROL=1 /bin/bash -p scripts/hil/bench_unguarded_probe.sh
REFUSE
    exit 2
  fi

  _hil_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

  # shellcheck source=scripts/hil/lib/rig_env.sh
  source "$_hil_dir/lib/rig_env.sh"
  rig_require PI_HOST JLINK_SN

  HOLD_S="${1:-12}"

  # ---- the one interlock this script does keep ---------------------------------
  #
  # It must collide with the OTHER unguarded actors of the same negative control
  # -- that is its entire purpose -- and it must never collide with somebody who
  # took the bench properly. Those are compatible, because during the negative
  # control nobody holds the lock: a lock in force therefore means a REAL actor
  # is on the board, quite possibly mid-flash, and an unguarded read landing in
  # the middle of that is how you prove the lock works by corrupting a flash.
  #
  # So: refuse when the bench is held, and refuse when the answer cannot be
  # established. Fail closed, the same way ra8_bench_require does.
  /bin/bash -p "$_hil_dir/bench.sh" status >/dev/null 2>&1
  case $? in
    0) ;;
    1)
      printf '[negative-control] REFUSED -- the bench is HELD right now.\n' >&2
      printf '[negative-control] The negative control may collide only with the other\n' >&2
      printf '[negative-control] unguarded actors of its own run, never with somebody who\n' >&2
      printf '[negative-control] took the lock properly and may be programming MRAM.\n' >&2
      /bin/bash -p "$_hil_dir/bench.sh" status >&2 2>/dev/null
      exit 2
      ;;
    *)
      printf '[negative-control] REFUSED -- could not establish whether the bench is free.\n' >&2
      printf '[negative-control] A lock you cannot read is not a free bench.\n' >&2
      exit 2
      ;;
  esac

  printf '[negative-control] UNGUARDED bench touch starting at %s -- no lock is held\n' \
    "$(date +%s.%N)" >&2

  # Read-only J-Link occupancy plus a console read, both for the same span, so
  # the witness has two independent kinds of collision to notice: two JLinkExe
  # processes with different ssh peers, and two readers on one /dev/ttyACM.
  # shellcheck disable=SC2087  # client-side substitution of JLINK_DEVICE/JLINK_SN and the
  # hold duration is intentional: the bench host has no .env and no checkout.
  ssh -o BatchMode=yes -o ConnectTimeout=8 "$PI_HOST" /bin/bash -p <<REMOTE
set -uo pipefail
$RA8_TTY_RESOLVER_SRC
S=\$(mktemp /tmp/ra8-negctl.XXXXXX.jlink)
L=\$(mktemp /tmp/ra8-negctl.XXXXXX.log)
trap 'rm -f "\$S" "\$L"' EXIT
cat > "\$S" <<'JLINK'
device ${JLINK_DEVICE}
si SWD
speed 1000
connect
halt
mem32 0x02000000, 4
sleep $((HOLD_S * 1000))
mem32 0x02000000, 4
go
q
JLINK
# The console reader is the SECOND collision axis: two readers on one VCOM each
# get half the bytes, which is one of the concrete failures #497 lists. Resolve
# it loudly -- an unresolvable console silently skipped the reader once already,
# and the run then reported an evidence channel it had never exercised.
TTY="\$(ra8_tty_resolve console 2>&1)" || {
  echo "[negative-control] FATAL -- cannot resolve the board console: \$TTY" >&2
  exit 3
}
[ -e "\$TTY" ] || { echo "[negative-control] FATAL -- \$TTY does not exist" >&2; exit 3; }
echo "[negative-control] holding console \$TTY for $((HOLD_S + 2))s"
timeout $((HOLD_S + 2)) cat "\$TTY" > /dev/null 2>&1 &
CAT_PID=\$!
JLinkExe -nogui 1 -SelectEmuBySN '${JLINK_SN}' -commanderscript "\$S" > "\$L" 2>&1 || true
wait "\$CAT_PID" 2>/dev/null || true
grep -q 'Connecting to J-Link via USB\.\.\.O\.K\.' "\$L" &&
  echo "[negative-control] J-Link connected (unguarded) at \$(date +%s.%N)" ||
  echo "[negative-control] J-Link did NOT connect -- see \$L"
REMOTE

  printf '[negative-control] UNGUARDED bench touch finished at %s\n' "$(date +%s.%N)" >&2
else
  [[ "$-" == *p* ]]
fi
