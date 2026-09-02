#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# bench_contention.sh -- prove the bench lock (#497) under REAL contention from
# genuinely independent machines.
#
# WHY THIS IS NOT THE SELFTEST
# ----------------------------
# `bench.sh selftest` proves the MECHANISM: that a flock is a flock, and that
# killing an ssh client drops it. It runs entirely on one machine, against a
# throwaway state directory, and touches no hardware. Every claim it makes is
# about the lock talking to itself.
#
# This runs several real machines at the real bench, each doing a real flash
# through the real guard, all starting at the same instant, and then asks a
# question the lock cannot answer about itself: was the BOARD ever driven by
# two machines at once? The answer comes from `lib/bench_witness.py` sampling
# /proc on the bench host -- which process had the J-Link, and which machine
# started it, attributed from the kernel's own view of each ssh session.
#
# The distinction matters because an actor that silently skipped its work is
# indistinguishable, in the lock's journal, from one that waited politely and
# then did it. Both write one `acquire` and one `release`. Only the witness can
# tell them apart, and telling them apart is the whole point.
#
# PHASES
#   exclusion         N actors flash N different apps, simultaneously, guarded.
#   negative-control  the same actors touch the bench at once with the guard
#                     BYPASSED, and the witness must SEE the collision. Without
#                     this, "no overlap observed" could just mean "the witness
#                     observes nothing". Read-only J-Link and console reads
#                     only -- no concurrent programming is ever attempted.
#   death             one actor is SIGKILLed mid-hold; a queued waiter must get
#                     in, and must then really use the board.
#   fairness          repeated rounds; who waits how long, and was anybody
#                     starved.
#
# ROSTER
# ------
# Actors come from RA8_BENCH_ACTORS in the gitignored .env, because which
# machines exist is maintainer-specific and does not belong in the tree -- the
# same rule PI_HOST already follows. One actor per line:
#
#   <name> | <PI_HOST as THAT machine reaches the bench> | <transport words>
#
# The transport is a command that runs a bash script fed on ITS STDIN, given as
# space-separated words (no quoting, no embedded spaces in a word):
#
#   dev     | bench-user@bench-host | /usr/bin/ssh -o BatchMode=yes dev /bin/bash -p -s
#   nas     | bench-host            | /usr/bin/ssh -o BatchMode=yes nas /bin/bash -p -s
#   wsl     | bench@10.0.0.10       | /usr/bin/ssh -o BatchMode=yes -J jump user@host wsl -e /bin/bash -p -s
#   local   | bench-user@bench-host | /bin/bash -p -s
#
# Feeding the payload on stdin rather than on a command line is what makes one
# transport shape cover ssh, ssh-into-WSL and this machine at once: no layer in
# between ever re-parses the script, so nothing has to be quoted for a shell
# that is not bash.
#
# Exit 0 when every claim in the selected phases held, 1 when one did not,
# 2 on a usage or configuration error, 3 when no verdict could be established
# (bench unreachable, witness never started) -- the same three codes the rest
# of the bench interface uses, with the same meaning for 3.
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

  set -uo pipefail

  _bc_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  _bc_root="$(cd "$_bc_dir/../.." && pwd)"

  # shellcheck source=scripts/hil/lib/rig_env.sh
  source "$_bc_dir/lib/rig_env.sh"

  readonly BC_EXIT_OK=0
  # shellcheck disable=SC2034  # the verdict's exit code IS this value; declared here so all four codes are stated together, as the rest of the bench interface does.
  readonly BC_EXIT_FAIL=1
  readonly BC_EXIT_USAGE=2
  readonly BC_EXIT_UNKNOWN=3

  # Where an actor's staged copy of the tree lives on its own machine. A fixed
  # path (not a mktemp) so a re-run reuses it and a human can go and look.
  # Deliberately single-quoted: this is expanded on the ACTOR, by the actor's own
  # shell, so that every machine stages under its own home directory. Expanding it
  # here would bake the driver's home path into every payload.
  # shellcheck disable=SC2016  # expansion is meant to happen on the far side.
  readonly BC_STAGE='$HOME/.ra8-bench-contention'

  # The witness's home ON THE BENCH HOST.
  readonly BC_WITNESS_DIR=/tmp/ra8-bench-witness

  # Sampling period. A flash keeps JLinkExe alive for order-of-seconds, so 50 ms
  # brackets every end instant to a twentieth of a second and every start instant
  # is exact regardless (read from /proc, not sampled).
  readonly BC_WITNESS_MS=50

  # How far ahead of now the launch barrier is set. Every actor spins until this
  # absolute epoch, so "simultaneous" means the same instant on every machine
  # rather than "whenever my ssh finished connecting".
  readonly BC_BARRIER_LEAD_S=25

  # Clock skew above which "simultaneous" stops meaning anything and the run is
  # refused rather than reported.
  readonly BC_MAX_SKEW_S=2

  BC_APPS=(blink blink_hal crc_demo clock_check)
  BC_PHASES=(exclusion negative-control death fairness)
  BC_ROUNDS=3
  BC_VICTIM=""
  BC_WAIT=10m
  BC_STAGE_ENABLED=1
  BC_OUT=""
  BC_FAILURES=0

  bc_say() { printf '[contention] %s\n' "$*" >&2; }
  bc_step() { printf '\n[contention] == %s ==\n' "$*" >&2; }

  # ---------------------------------------------------------------------------
  # Roster
  # ---------------------------------------------------------------------------

  BC_NAMES=()
  BC_PIHOSTS=()
  BC_TRANSPORTS=()

  # Parse RA8_BENCH_ACTORS into the three parallel arrays. Parallel arrays and
  # not one associative array because bash 3.2 (the macOS system bash) has none.
  bc_load_roster() {
    local line name pi transport
    if [ -z "${RA8_BENCH_ACTORS:-}" ]; then
      bc_say "RA8_BENCH_ACTORS is not set in .env -- see .env.example for the format."
      return "$BC_EXIT_USAGE"
    fi
    while IFS= read -r line; do
      line="${line%%#*}"
      case "$line" in *[![:space:]]*) ;; *) continue ;; esac
      name="$(printf '%s' "${line%%|*}" | tr -d '[:space:]')"
      line="${line#*|}"
      pi="$(printf '%s' "${line%%|*}" | tr -d '[:space:]')"
      transport="${line#*|}"
      if [ -z "$name" ] || [ -z "$pi" ] || [ -z "$transport" ]; then
        bc_say "malformed RA8_BENCH_ACTORS line (want 'name | pi_host | transport words')"
        return "$BC_EXIT_USAGE"
      fi
      BC_NAMES+=("$name")
      BC_PIHOSTS+=("$pi")
      BC_TRANSPORTS+=("$transport")
    done <<EOF
$RA8_BENCH_ACTORS
EOF
    if [ "${#BC_NAMES[@]}" -lt 2 ]; then
      bc_say "a contention test needs at least 2 actors; the roster has ${#BC_NAMES[@]}."
      return "$BC_EXIT_USAGE"
    fi
    return 0
  }

  # bc_on <index> -- run the script on stdin on actor <index>'s machine.
  # The transport is deliberately UNQUOTED: it is a word list by contract.
  bc_on() {
    local i="$1"
    ${BC_TRANSPORTS[$i]}
  }

  # ---------------------------------------------------------------------------
  # Staging: give every actor the real scripts, the real guard, and a real image
  # ---------------------------------------------------------------------------
  #
  # Not a stripped-down imitation: scripts/hil and scripts/checks go over
  # verbatim, so each actor runs the same flash.sh, the same lib/bench_lock.sh
  # and the same pre-flash guard this repo ships. Only the .env is per-actor,
  # because each machine reaches the bench by a different route.

  bc_stage_tarball() {
    local tarball="$1" app rel
    local -a paths=(scripts/hil scripts/checks)
    for app in "${BC_APPS[@]}"; do
      rel="$(bc_app_dir "$app")" || return 1
      paths+=("${rel#"$_bc_root"/}")
    done
    # COPYFILE_DISABLE keeps the macOS tar from packing an AppleDouble `._foo`
    # beside every file, and --no-xattrs keeps it from writing the
    # LIBARCHIVE.xattr.* headers that GNU tar then warns about once per file on
    # the far side. Both are noise, and a hundred lines of noise per actor is how
    # a real staging failure goes unread.
    local -a xflag=()
    tar --no-xattrs -cf /dev/null -T /dev/null 2>/dev/null && xflag=(--no-xattrs)
    COPYFILE_DISABLE=1 tar "${xflag[@]}" -czf "$tarball" -C "$_bc_root" "${paths[@]}" 2>/dev/null
  }

  # The app directory, as an absolute path. One definition, used by staging and
  # by the pre-strip below.
  bc_app_dir() {
    local d
    d="$(find "$_bc_root/examples" -type d -name "$1" -print 2>/dev/null | head -1)"
    [ -n "$d" ] || {
      bc_say "app '$1' not found -- build it first: just apps::build $1"
      return 1
    }
    printf '%s' "$d"
  }

  # Pre-strip the OFS sections HERE, once, so an actor without an ARM toolchain
  # still programs exactly the bytes flash.sh would have produced. flash.sh's own
  # objcopy step is a no-op on an already-stripped image and falls back to a copy
  # when the toolchain is absent, so both paths converge on the same bytes.
  bc_prestrip() {
    local app dir
    command -v arm-none-eabi-objcopy >/dev/null 2>&1 || {
      bc_say "arm-none-eabi-objcopy not found -- cannot pre-strip images for actors."
      return 1
    }
    for app in "${BC_APPS[@]}"; do
      dir="$(bc_app_dir "$app")" || return 1
      [ -f "$dir/build/$app.hex" ] || {
        bc_say "missing $dir/build/$app.hex -- run: just apps::build $app"
        return 1
      }
      if [ -f "$dir/build/$app.elf" ]; then
        arm-none-eabi-objcopy --remove-section='.option_setting*' -O ihex \
          "$dir/build/$app.elf" "$dir/build/$app.hex" || return 1
      fi
    done
    return 0
  }

  bc_stage_actor() {
    local i="$1" tarball="$2"
    local name="${BC_NAMES[$i]}" pi="${BC_PIHOSTS[$i]}"
    {
      printf 'set -e\n'
      printf 'rm -rf %s; mkdir -p %s; cd %s\n' "$BC_STAGE" "$BC_STAGE" "$BC_STAGE"
      printf "base64 -d <<'BC_TARBALL_EOF' | tar xzf -\n"
      base64 <"$tarball" | tr -d '\r'
      printf 'BC_TARBALL_EOF\n'
      printf 'printf "PI_HOST=%%s\\nJLINK_SN=%%s\\nJLINK_DEVICE=%%s\\n" %s %s %s > .env\n' \
        "$pi" "${JLINK_SN}" "${JLINK_DEVICE}"
      # shellcheck disable=SC2016  # runs on the ACTOR: its hostname, its file count.
      printf 'echo "STAGED $(hostname) $(ls scripts/hil/lib | wc -l) lib files"\n'
    } | bc_on "$i" 2>&1 | sed "s/^/[stage:$name] /" >&2
  }

  # ---------------------------------------------------------------------------
  # Clock skew: "simultaneous" has to mean something
  # ---------------------------------------------------------------------------

  # Sub-second wall clock on this machine. `date +%s.%N` is a GNU extension and
  # the driver may be a Mac, so python3 -- which is already a hard dependency
  # here, since the verdict is written in it.
  bc_now() { python3 -c 'import time; print(repr(time.time()))'; }

  # Compare one remote clock to this one WITHOUT mistaking latency for skew.
  #
  # The naive form -- read the bench clock, then read the actor's, then subtract
  # -- charges the whole round trip to the actor, and reported the WSL box (which
  # is reached through a jump host and then through wsl.exe) as 3 s adrift when
  # it was in fact exact. So the remote reading is BRACKETED: take the local time
  # before and after, and a remote clock landing anywhere inside that bracket is
  # skew-free to within the round trip, which is all the resolution there is.
  # Only the distance OUTSIDE the bracket is real skew.
  #
  # Emits "<label> rtt=<s> skew=<s>" and returns non-zero past the limit.
  bc_clock_delta() {
    local label="$1" reading="$2" t0="$3" t1="$4" out="$5"
    case "$reading" in
      '' | *[!0-9]*)
        bc_say "$label did not answer with a clock"
        return "$BC_EXIT_UNKNOWN"
        ;;
      *) ;;
    esac
    python3 - "$label" "$reading" "$t0" "$t1" "$BC_MAX_SKEW_S" >>"$out" <<'PY'
import sys
label, reading, t0, t1, limit = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5])
# The remote answered with whole seconds, so widen the bracket by one on each
# side: a truthful clock inside the window can still print a second either way.
lo, hi = t0 - 1.0, t1 + 1.0
skew = 0.0 if lo <= reading <= hi else (reading - hi if reading > hi else reading - lo)
print(f"{label} rtt={t1 - t0:.2f}s skew={skew:+.2f}s {'OK' if abs(skew) <= limit else 'OVER'}")
PY
    grep -q "^$label .* OVER\$" "$out" && return "$BC_EXIT_UNKNOWN"
    return 0
  }

  bc_check_skew() {
    local i name reading t0 t1 out="$1"
    : >"$out"
    t0="$(bc_now)"
    reading="$(ssh -o BatchMode=yes -o ConnectTimeout=8 "$PI_HOST" 'date +%s' 2>/dev/null | tr -dc '0-9')"
    t1="$(bc_now)"
    bc_clock_delta "bench-host" "$reading" "$t0" "$t1" "$out" || {
      bc_say "the bench host clock is adrift from this one -- every timestamp comparison"
      bc_say "in the verdict rests on the two agreeing. Refusing to run."
      sed 's/^/[contention]   /' "$out" >&2
      return "$BC_EXIT_UNKNOWN"
    }
    for i in "${!BC_NAMES[@]}"; do
      name="${BC_NAMES[$i]}"
      t0="$(bc_now)"
      reading="$(printf 'date +%%s\n' | bc_on "$i" 2>/dev/null | tr -dc '0-9')"
      t1="$(bc_now)"
      bc_clock_delta "$name" "$reading" "$t0" "$t1" "$out" || {
        bc_say "actor $name is more than ${BC_MAX_SKEW_S}s off this clock;"
        bc_say "a simultaneous launch cannot be established across that."
        sed 's/^/[contention]   /' "$out" >&2
        return "$BC_EXIT_UNKNOWN"
      }
    done
    sed 's/^/[contention]   /' "$out" >&2
    return 0
  }

  # ---------------------------------------------------------------------------
  # The witness
  # ---------------------------------------------------------------------------

  bc_witness_start() {
    local tag="$1" b64
    b64="$(base64 <"$_bc_dir/lib/bench_witness.py" | tr -d '\n')" || return "$BC_EXIT_UNKNOWN"
    ssh -o BatchMode=yes -o ConnectTimeout=8 "$PI_HOST" "
    mkdir -p $BC_WITNESS_DIR
    printf %s '$b64' | base64 -d > $BC_WITNESS_DIR/bench_witness.py
    python3 $BC_WITNESS_DIR/bench_witness.py --selftest || exit 1
    rm -f $BC_WITNESS_DIR/$tag.ndjson $BC_WITNESS_DIR/$tag.ndjson.stop
    setsid nohup python3 $BC_WITNESS_DIR/bench_witness.py run \
      --out $BC_WITNESS_DIR/$tag.ndjson --interval-ms $BC_WITNESS_MS \
      --max-seconds 5400 >$BC_WITNESS_DIR/$tag.log 2>&1 </dev/null &
    sleep 1
    test -s $BC_WITNESS_DIR/$tag.ndjson
  " >&2 || {
      bc_say "the witness did not start on the bench host -- refusing to run blind."
      return "$BC_EXIT_UNKNOWN"
    }
    return 0
  }

  bc_witness_stop() {
    local tag="$1" dest="$2"
    ssh -o BatchMode=yes -o ConnectTimeout=8 "$PI_HOST" \
      "touch $BC_WITNESS_DIR/$tag.ndjson.stop; sleep 1; cat $BC_WITNESS_DIR/$tag.ndjson" \
      >"$dest" 2>/dev/null
    [ -s "$dest" ]
  }

  # The journal, from the line count taken before the phase started, so the
  # analyser never sees somebody else's history as part of this run.
  bc_journal_mark() {
    ssh -o BatchMode=yes -o ConnectTimeout=8 "$PI_HOST" \
      'wc -l < /var/lib/ra8-bench/journal.ndjson 2>/dev/null || echo 0' 2>/dev/null | tr -dc '0-9'
  }

  bc_journal_since() {
    local from="$1" dest="$2"
    ssh -o BatchMode=yes -o ConnectTimeout=8 "$PI_HOST" \
      "tail -n +$((from + 1)) /var/lib/ra8-bench/journal.ndjson" >"$dest" 2>/dev/null
  }

  # ---------------------------------------------------------------------------
  # The actor payload
  # ---------------------------------------------------------------------------

  # bc_payload <name> <barrier_epoch> <wait_s> <body...>
  #
  # Everything an actor runs, on stdin, on its own machine. It reports its own
  # timeline on stdout -- which the verdict treats as a CLAIM to be checked
  # against the witness, never as evidence.
  bc_payload() {
    local name="$1" barrier="$2" wait_s="$3"
    shift 3
    cat <<EOF
set -uo pipefail
cd $BC_STAGE || exit 3
export RA8_BENCH_ACTOR='$name'
export RA8_BENCH_WAIT_S='$wait_s'
export RA8_BENCH_CLASS=agent
$(bc_stamp_fn)
echo "ACTOR $name host=\$(hostname) pid=\$\$ stage=\$PWD"
# Spin, do not sleep, over the last stretch: every machine crosses the same
# absolute instant rather than each starting when its ssh happened to connect.
while [ "\$(date +%s)" -lt $barrier ]; do
  sleep 0.05 2>/dev/null || sleep 1
done
echo "BARRIER-CROSSED $name at \$(bc_stamp)"
echo "WORK-BEGIN $name \$(bc_stamp)"
$*
rc=\$?
echo "WORK-END $name \$(bc_stamp) rc=\$rc"
exit \$rc
EOF
  }

  # The sub-second stamp function, as TEXT, for pasting into an actor payload.
  #
  # `date +%s.%N` is a GNU extension: on the macOS actor it prints a literal "N"
  # where the nanoseconds should be. Every instant an actor reports is compared
  # against millisecond-resolution witness data, so a second-resolution stamp on
  # one machine would quietly become the resolution of the whole comparison.
  bc_stamp_fn() {
    cat <<'STAMP'
bc_stamp() {
  python3 -c 'import time; print(repr(time.time()))' 2>/dev/null || date +%s.%N
}
STAMP
  }

  # ---------------------------------------------------------------------------
  # Phase: exclusion
  # ---------------------------------------------------------------------------

  bc_phase_exclusion() {
    local out="$1" i name app barrier mark pids=() rc=0
    bc_step "phase: exclusion -- ${#BC_NAMES[@]} machines flash ${#BC_NAMES[@]} apps at the same instant"
    bc_witness_start exclusion || return "$BC_EXIT_UNKNOWN"
    mark="$(bc_journal_mark)"
    barrier=$(($(date +%s) + BC_BARRIER_LEAD_S))
    bc_say "barrier at epoch $barrier (in ${BC_BARRIER_LEAD_S}s)"

    # Per PHASE, not per run: the phases below assign different apps to different
    # actors (death excludes its victim), and one shared file meant the last phase
    # to run silently rewrote the record the earlier ones are judged against.
    : >"$out/assignment-exclusion.txt"
    for i in "${!BC_NAMES[@]}"; do
      name="${BC_NAMES[$i]}"
      app="${BC_APPS[$((i % ${#BC_APPS[@]}))]}"
      bc_say "  $name -> flash $app"
      printf '%s %s\n' "$name" "$app" >>"$out/assignment-exclusion.txt"
      bc_payload "$name" "$barrier" "$(bc_seconds "$BC_WAIT")" \
        "/bin/bash -p scripts/hil/flash.sh $app" |
        bc_on "$i" >"$out/actor-$name.log" 2>&1 &
      pids+=("$!")
    done
    for i in "${!pids[@]}"; do
      wait "${pids[$i]}" || rc=1
    done
    bc_say "all actors finished (some non-zero exits are expected only if a wait budget ran out)"
    bc_witness_stop exclusion "$out/witness-exclusion.ndjson" ||
      {
        bc_say "no witness output"
        return "$BC_EXIT_UNKNOWN"
      }
    bc_journal_since "$mark" "$out/journal-exclusion.ndjson"
    return 0
  }

  # ---------------------------------------------------------------------------
  # Phase: negative control
  # ---------------------------------------------------------------------------
  #
  # The claim "the witness saw no two machines on the board at once" is worthless
  # unless the witness CAN see that. So the same actors are pointed at the bench
  # with the guard bypassed and required to collide.
  #
  # What they do is chosen to be harmless: a read-only J-Link session (connect,
  # halt, read a word, go) and a console read. NO concurrent programming is
  # attempted -- two interleaved writes to MRAM is the one collision that could
  # leave the board needing recovery, and proving the witness works does not
  # require risking it.

  bc_phase_negative_control() {
    local out="$1" i name barrier mark pids=()
    bc_step "phase: negative-control -- same machines, guard BYPASSED, must collide"
    bc_witness_start negctl || return "$BC_EXIT_UNKNOWN"
    mark="$(bc_journal_mark)"
    barrier=$(($(date +%s) + BC_BARRIER_LEAD_S))

    for i in "${!BC_NAMES[@]}"; do
      name="${BC_NAMES[$i]}"
      bc_say "  $name -> unguarded read-only J-Link + console read"
      bc_payload "$name" "$barrier" 0 \
        "RA8_BENCH_NEGATIVE_CONTROL=1 /bin/bash -p scripts/hil/bench_unguarded_probe.sh 15" |
        bc_on "$i" >"$out/negctl-$name.log" 2>&1 &
      pids+=("$!")
    done
    for i in "${!pids[@]}"; do wait "${pids[$i]}" || true; done
    bc_witness_stop negctl "$out/witness-negctl.ndjson" ||
      {
        bc_say "no witness output"
        return "$BC_EXIT_UNKNOWN"
      }
    bc_journal_since "$mark" "$out/journal-negctl.ndjson"
    return 0
  }

  # ---------------------------------------------------------------------------
  # Phase: death
  # ---------------------------------------------------------------------------
  #
  # Actor 0 takes the bench and starts real work. The others queue behind it with
  # a real wait budget. Part way in, actor 0's bench-hold ssh CLIENT is SIGKILLed
  # on its own machine -- no trap runs, no release is sent, the process simply
  # stops existing. What must then happen is that a WAITER gets in and does real
  # work of its own; the existing selftest proves the flock drops, and this
  # proves somebody is there to take it.

  bc_phase_death() {
    local out="$1" i name barrier mark victim vidx pids=() app vpid
    vidx="$(bc_victim_index)"
    victim="${BC_NAMES[$vidx]}"
    bc_step "phase: death -- SIGKILL $victim's bench-hold ssh client, a waiter must get in"
    bc_witness_start death || return "$BC_EXIT_UNKNOWN"
    mark="$(bc_journal_mark)"
    barrier=$(($(date +%s) + BC_BARRIER_LEAD_S))

    # The victim holds for a good while doing real work, so there is a hold to
    # kill in the middle of rather than a race to catch.
    bc_payload "$victim" "$barrier" "$(bc_seconds "$BC_WAIT")" \
      "/bin/bash -p scripts/hil/bench_hold_and_work.sh 90" |
      bc_on "$vidx" >"$out/death-$victim.log" 2>&1 &
    pids+=("$!")

    : >"$out/assignment-death.txt"
    for i in "${!BC_NAMES[@]}"; do
      [ "$i" -eq "$vidx" ] && continue
      name="${BC_NAMES[$i]}"
      app="${BC_APPS[$((i % ${#BC_APPS[@]}))]}"
      printf '%s %s\n' "$name" "$app" >>"$out/assignment-death.txt"
      bc_payload "$name" "$((barrier + 5))" "$(bc_seconds "$BC_WAIT")" \
        "/bin/bash -p scripts/hil/flash.sh $app" |
        bc_on "$i" >"$out/death-$name.log" 2>&1 &
      pids+=("$!")
    done

    # Wait for the victim to be the recorded holder, then let it get properly
    # into its work before killing it.
    bc_wait_for_holder "$victim" 120 || bc_say "WARNING: $victim never appeared as the holder"
    sleep 15
    vpid="$(sed -n 's/.*ACTOR '"$victim"' .*pid=\([0-9][0-9]*\).*/\1/p' "$out/death-$victim.log" | head -1)"
    if [ -z "$vpid" ]; then
      bc_say "could not read $victim's payload pid -- refusing to kill unrelated processes"
      return "$BC_EXIT_UNKNOWN"
    fi
    bc_say "SIGKILLing the bench-hold ssh client under pid $vpid on $victim"
    bc_kill_hold "$vidx" "$vpid" >"$out/death-kill.log" 2>&1

    for i in "${!pids[@]}"; do wait "${pids[$i]}" || true; done
    bc_witness_stop death "$out/witness-death.ndjson" ||
      {
        bc_say "no witness output"
        return "$BC_EXIT_UNKNOWN"
      }
    bc_journal_since "$mark" "$out/journal-death.ndjson"
    cp "$out/death-kill.log" "$out/kill-marker.txt" 2>/dev/null
    return 0
  }

  # Which actor gets killed. Default to the first REMOTE one: a remote machine
  # dying is the case that matters, and the driver's own host is routinely shared
  # with other work whose holds must not be caught in the blast.
  bc_victim_index() {
    local i
    if [ -n "${BC_VICTIM:-}" ]; then
      for i in "${!BC_NAMES[@]}"; do
        [ "${BC_NAMES[$i]}" = "$BC_VICTIM" ] && {
          printf '%s' "$i"
          return 0
        }
      done
    fi
    for i in "${!BC_NAMES[@]}"; do
      case "${BC_TRANSPORTS[$i]}" in
        /bin/bash*) ;;
        *)
          printf '%s' "$i"
          return 0
          ;;
      esac
    done
    printf '0'
  }

  # SIGKILL only the bench-hold ssh client that BELONGS to the victim's payload.
  #
  # `pkill -f ra8-bench-host` was the obvious form and is wrong: the bench is
  # shared, several agents may be holding it from the same machine at the same
  # moment, and a blanket kill would drop somebody else's hold mid-flash -- the
  # exact accident this whole subsystem exists to prevent. So each candidate's
  # ancestry is walked back to the victim's payload pid, and anything not
  # descended from it is left alone. `ps -o ppid=` rather than /proc, because the
  # victim may be a Mac.
  bc_kill_hold() {
    local idx="$1" root="$2"
    {
      bc_stamp_fn
      cat <<EOF
root=$root
mine=""
for p in \$(pgrep -f ra8-bench-host 2>/dev/null); do
  cur=\$p
  for _ in 1 2 3 4 5 6 7 8; do
    [ -z "\$cur" ] && break
    [ "\$cur" = "1" ] && break
    if [ "\$cur" = "\$root" ]; then mine="\$mine \$p"; break; fi
    cur=\$(ps -o ppid= -p "\$cur" 2>/dev/null | tr -d ' ')
  done
done
echo "candidates: \$(pgrep -f ra8-bench-host 2>/dev/null | tr '\n' ' ')"
echo "descended from \$root: \$mine"
if [ -z "\$mine" ]; then echo "NOTHING-TO-KILL"; exit 1; fi
echo "KILLING \$mine at \$(bc_stamp)"
for p in \$mine; do kill -9 "\$p" 2>/dev/null || true; done
echo "KILLED at \$(bc_stamp)"
EOF
    } | bc_on "$idx"
  }

  # Block until <name> is the holder the bench host reports, or the deadline
  # passes. Read from the bench host, never from the actor's own claim.
  bc_wait_for_holder() {
    local want="$1" limit="$2" waited=0 who
    while [ "$waited" -lt "$limit" ]; do
      who="$(ssh -o BatchMode=yes -o ConnectTimeout=8 "$PI_HOST" \
        "sed -n 's/.*\"holder_name\": \"//p' /var/lib/ra8-bench/holder.json 2>/dev/null | head -1" 2>/dev/null)"
      case "$who" in *"$want"*) return 0 ;; esac
      sleep 2
      waited=$((waited + 2))
    done
    return 1
  }

  # ---------------------------------------------------------------------------
  # Phase: fairness
  # ---------------------------------------------------------------------------
  #
  # flock(2) makes no ordering promise. Linux wakes blocked waiters and lets them
  # race, so a queue is not a queue and starvation is possible in principle. This
  # measures it rather than assuming either way: every actor asks for the bench
  # repeatedly, and the verdict reports each one's wait distribution and whether
  # any request never got in.

  bc_phase_fairness() {
    local out="$1" i name barrier mark pids=() round
    bc_step "phase: fairness -- $BC_ROUNDS rounds, every actor competing every round"
    bc_witness_start fairness || return "$BC_EXIT_UNKNOWN"
    mark="$(bc_journal_mark)"
    barrier=$(($(date +%s) + BC_BARRIER_LEAD_S))

    for i in "${!BC_NAMES[@]}"; do
      name="${BC_NAMES[$i]}"
      round="/bin/bash -p scripts/hil/bench_hold_and_work.sh 8"
      bc_payload "$name" "$barrier" "$(bc_seconds "$BC_WAIT")" \
        "for r in \$(seq 1 $BC_ROUNDS); do echo \"ROUND \$r $name request \$(date +%s.%N)\"; $round; echo \"ROUND \$r $name done \$(date +%s.%N)\"; done" |
        bc_on "$i" >"$out/fairness-$name.log" 2>&1 &
      pids+=("$!")
    done
    for i in "${!pids[@]}"; do wait "${pids[$i]}" || true; done
    bc_witness_stop fairness "$out/witness-fairness.ndjson" ||
      {
        bc_say "no witness output"
        return "$BC_EXIT_UNKNOWN"
      }
    bc_journal_since "$mark" "$out/journal-fairness.ndjson"
    return 0
  }

  # ---------------------------------------------------------------------------
  # Driver
  # ---------------------------------------------------------------------------

  bc_seconds() {
    local n="${1%[smhSMH]}" unit="${1##*[0-9]}"
    case "$unit" in
      h | H) printf '%s' $((n * 3600)) ;;
      m | M) printf '%s' $((n * 60)) ;;
      *) printf '%s' "$n" ;;
    esac
  }

  bc_usage() {
    sed -n '5,60p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    printf '\nusage: bench_contention.sh [--phase %s|all] [--rounds N]\n' \
      "$(
        IFS='|'
        printf '%s' "${BC_PHASES[*]}"
      )"
    printf '                           [--wait 10m] [--out DIR] [--no-stage]\n'
  }

  bc_parse() {
    local phases=""
    while [ $# -gt 0 ]; do
      case "$1" in
        --phase)
          phases="${2:-}"
          shift 2
          ;;
        --rounds)
          BC_ROUNDS="${2:-3}"
          shift 2
          ;;
        --wait)
          BC_WAIT="${2:-10m}"
          shift 2
          ;;
        --out)
          BC_OUT="${2:-}"
          shift 2
          ;;
        --no-stage)
          BC_STAGE_ENABLED=0
          shift
          ;;
        --victim)
          BC_VICTIM="${2:-}"
          shift 2
          ;;
        -h | --help)
          bc_usage
          exit "$BC_EXIT_OK"
          ;;
        *)
          bc_say "unknown option '$1'"
          return "$BC_EXIT_USAGE"
          ;;
      esac
    done
    if [ -n "$phases" ] && [ "$phases" != all ]; then
      IFS=',' read -r -a BC_PHASES <<<"$phases"
    fi
    return 0
  }

  # Build the payload tarball once and push it to every actor. Split out of main()
  # only because it is a complete step in its own right -- prepare the images,
  # pack them, hand them out -- and main() reads as a list of steps when it is not
  # also carrying this one inline.
  bc_stage_all() {
    [ "$BC_STAGE_ENABLED" -eq 1 ] || return 0
    bc_step "staging the real scripts and images onto every actor"
    bc_prestrip || return "$BC_EXIT_USAGE"
    local tarball i
    tarball="$(mktemp "${TMPDIR:-/tmp}/ra8-bc-stage.XXXXXX.tgz")" || return "$BC_EXIT_UNKNOWN"
    bc_stage_tarball "$tarball" || {
      rm -f "$tarball"
      return "$BC_EXIT_USAGE"
    }
    for i in "${!BC_NAMES[@]}"; do
      bc_stage_actor "$i" "$tarball" || {
        bc_say "staging failed for ${BC_NAMES[$i]}"
        rm -f "$tarball"
        return "$BC_EXIT_UNKNOWN"
      }
    done
    rm -f "$tarball"
    return 0
  }

  main() {
    bc_parse "$@" || {
      bc_usage
      return "$BC_EXIT_USAGE"
    }
    rig_require PI_HOST JLINK_SN
    bc_load_roster || return "$BC_EXIT_USAGE"

    [ -n "$BC_OUT" ] || BC_OUT="/tmp/ra8-bench-contention/$(date +%Y%m%dT%H%M%S)"
    mkdir -p "$BC_OUT" || return "$BC_EXIT_UNKNOWN"
    bc_say "artifacts: $BC_OUT"
    bc_say "actors: ${BC_NAMES[*]}"

    bc_step "clock skew"
    bc_check_skew "$BC_OUT/skew.txt" || return "$BC_EXIT_UNKNOWN"

    bc_stage_all || return $?

    local phase rc
    for phase in "${BC_PHASES[@]}"; do
      case "$phase" in
        stage)
          bc_say "stage-only run: every actor now has the tree and the images."
          return "$BC_EXIT_OK"
          ;;
        exclusion) bc_phase_exclusion "$BC_OUT" ;;
        negative-control) bc_phase_negative_control "$BC_OUT" ;;
        death) bc_phase_death "$BC_OUT" ;;
        fairness) bc_phase_fairness "$BC_OUT" ;;
        *)
          bc_say "unknown phase '$phase'"
          return "$BC_EXIT_USAGE"
          ;;
      esac
      rc=$?
      [ "$rc" -eq 0 ] || return "$rc"
    done

    bc_step "verdict"
    printf '%s\n' "${BC_NAMES[@]}" >"$BC_OUT/roster.txt"
    python3 "$_bc_dir/lib/bench_contention_verdict.py" --dir "$BC_OUT" \
      --rounds "$BC_ROUNDS" \
      --phases "$(
        IFS=,
        printf '%s' "${BC_PHASES[*]}"
      )"
    BC_FAILURES=$?
    bc_say "artifacts kept in $BC_OUT"
    return "$BC_FAILURES"
  }

  main "$@"
else
  [[ "$-" == *p* ]]
fi
