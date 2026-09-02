#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/ci/fleet_capacity.sh -- change how many CI runners a host is running,
# without ever killing a job.
#
# WHY A DRAIN AND NOT A STOP
# --------------------------
# `docker stop` on a busy runner CANCELS the job it is running. That is not a
# grace-period problem that a longer timeout fixes -- it is what the runner is
# built to do, through three deliberate hops:
#
#   1. Our runner image sets RUNNER_MANUALLY_TRAP_SIG=1 (the official
#      actions-runner image's contract), so run.sh takes the runWithManualTrap
#      path and its `trap 'kill -INT -$PID' INT TERM` forwards a SIGTERM to the
#      listener's process group as SIGINT.
#   2. Runner.Listener treats that as ShutdownReason.UserCancelled, cancels its
#      message loop, and in the finally block calls JobDispatcher.ShutdownAsync().
#   3. ShutdownAsync() calls EnsureDispatchFinished(currentDispatch,
#      cancelRunningJob: true) -- which cancels the worker's token and waits for
#      the worker to die. The job ends Cancelled.
#
# So a longer `--time` buys nothing: the cancel is immediate and deliberate, and
# the extra seconds are only spent waiting for a job that has already been told
# to stop. This is the shape of the failure this fleet has already seen once --
# WSL's idle timeout reaped the VM out from under three live jobs and GitHub
# kept reporting the runners busy with orphaned work until it timed out.
#
# THE MECHANISM THIS USES INSTEAD
# -------------------------------
# Never signal a busy runner. Poll each instance, and stop it in the moment it
# is idle -- a stopped container cannot be handed another job, so the host
# converges downward one instance at a time as its jobs finish naturally.
#
# `docker top <container>` lists Runner.Worker exactly while a job is running
# (Runner.Listener spawns one worker process per job), so "is this instance
# busy" is answered locally, with no GitHub API call and no credential on the
# host. That matters: this script runs unattended from a systemd timer on a
# machine that deliberately holds no PAT.
#
# If the deadline passes with instances still busy, this reports what it could
# not park and exits non-zero. It does NOT force them. A scale-down that kills
# jobs is worse than no scale-down at all.
#
# TWO KINDS OF HOST, ONE DEFINITION OF DRAIN
# ------------------------------------------
#   docker  long-lived runner containers (the NAS, the Windows/WSL2 box)
#   k8s     an ARC scale set, where lowering maxRunners is already safe: ARC
#           runners are ephemeral (one job, then exit) and the controller only
#           deletes runners that hold no job, so the drain is the controller's
#           and this only has to move the number.
#
# Configuration arrives as flags, or as the RA8_FLEET_* environment the
# fleet_capacity role writes to /etc/ra8-fleet/capacity.env for the timer.
# Flags win over the environment, which wins over the defaults below.
#
# Usage:
#   fleet_capacity.sh [options] status
#   fleet_capacity.sh [options] scale <n>
#
# scripts/dev/fleet.py pipes THIS FILE to the host over ssh for every
# control-node capacity command, so an operator run is always the version in
# the checkout; the copy the role installs exists for the unattended timer.

set -euo pipefail

RA8_FLEET_KIND="${RA8_FLEET_KIND:-docker}"
RA8_FLEET_DOCKER="${RA8_FLEET_DOCKER:-docker}"
RA8_FLEET_CONTAINERS="${RA8_FLEET_CONTAINERS:-}"

# Name prefix every runner container on a host shares, so `drain-all` can find
# what is really there rather than what the declaration predicts.
RA8_FLEET_PREFIX="${RA8_FLEET_PREFIX:-ra8-ci-runner}"
# `sudo` is not optional here: k3s writes its kubeconfig root-only, so the
# deploy user's plain `k3s kubectl` fails with "permission denied" on
# /etc/rancher/k3s/k3s.yaml -- which reads as "no scale set" rather than as the
# privilege problem it is.
RA8_FLEET_KUBECTL="${RA8_FLEET_KUBECTL:-sudo k3s kubectl}"
RA8_FLEET_NAMESPACE="${RA8_FLEET_NAMESPACE:-arc-runners}"
RA8_FLEET_SCALESET="${RA8_FLEET_SCALESET:-ra8-ci}"

# How long to keep waiting for busy instances to finish before giving up and
# reporting. The longest job in this tree's CI is the ~30-minute emulator
# smoke, so 90 minutes covers a job that started just before the window opened
# plus a retry, without waiting forever on a wedged runner.
RA8_FLEET_DEADLINE="${RA8_FLEET_DEADLINE:-5400}"

# How often to re-ask whether an instance has gone idle.
#
# This is the ONE number that decides how fast a drain converges, and it is
# short for a measured reason. A runner's idle gap -- between logging one job
# complete and being handed the next -- is a few seconds on a saturated queue,
# so a coarse poll walks straight past it and the instance is busy again by the
# time the drain looks. Measured at 15s on the Windows host: instance 1's job
# finished and a new one had started before the next check, costing a whole
# extra job cycle.
#
# It does not CLOSE that gap -- nothing token-free can, since only GitHub can
# stop assigning work -- it narrows it, so the drain usually catches the first
# gap instead of the third. The drain stays correct either way: it never
# signals a busy runner, it just takes longer. Each poll is two `docker
# inspect`-class calls per instance, so this is cheap to run for the hour the
# deadline allows.
RA8_FLEET_POLL="${RA8_FLEET_POLL:-3}"

# Only ever applied to an instance already proven idle, so this is headroom for
# the listener's own clean exit -- not the drain mechanism. See the header.
RA8_FLEET_STOP_GRACE="${RA8_FLEET_STOP_GRACE:-120}"

# Quiet hours, as declared in infra/fleet.yml and written to
# /etc/ra8-fleet/capacity.env by the fleet_capacity role. Empty days means the
# host declared no window, and `window` is then a no-op that says so.
RA8_FLEET_FULL_INSTANCES="${RA8_FLEET_FULL_INSTANCES:-0}"
RA8_FLEET_QUIET_INSTANCES="${RA8_FLEET_QUIET_INSTANCES:-0}"
RA8_FLEET_QUIET_START="${RA8_FLEET_QUIET_START:-}"
RA8_FLEET_QUIET_END="${RA8_FLEET_QUIET_END:-}"
RA8_FLEET_QUIET_DAYS="${RA8_FLEET_QUIET_DAYS:-}"

# The low-priority dev slice this host lends its idle capacity to, if any.
# Empty on a host that lends none, and every dev-slice path below is then a
# no-op rather than a failure.
RA8_FLEET_DEV_SLICE="${RA8_FLEET_DEV_SLICE:-}"

log() { printf '%s %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$*"; }

die() {
  printf 'fleet-capacity: error: %s\n' "$*" >&2
  exit 1
}

usage() {
  cat <<'EOF'
usage: fleet_capacity.sh [options] <command>

commands:
  status        report every instance: state, busy/idle, and how long its job
                has been running
  scale <n>     converge to n ACTIVE instances; shrinking DRAINS, never kills
  drain-all     park every runner container ON THIS HOST, whatever it is
                called -- what a re-provision runs first, because a converge
                recreates containers and would cancel their jobs
  window        converge to what this host should be RIGHT NOW -- the
                quiet-hours instance count inside a declared window, and its
                declared capacity at every other time, INCLUDING on a host
                that declares no window at all. What the systemd timer runs,
                so a live `scale` is temporary on every host rather than only
                on the ones with a window.

options:
  --kind docker|k8s      host shape                     (RA8_FLEET_KIND)
  --sudo                 invoke docker through sudo     (RA8_FLEET_DOCKER)
  --container NAME       one instance container; repeat, in instance order
                                                        (RA8_FLEET_CONTAINERS)
  --prefix NAME          name prefix drain-all discovers by
                                                        (RA8_FLEET_PREFIX)
  --namespace NS         ARC runner namespace           (RA8_FLEET_NAMESPACE)
  --scale-set NAME       ARC scale set name             (RA8_FLEET_SCALESET)
  --deadline SECONDS     give up draining after this    (RA8_FLEET_DEADLINE)
  --poll SECONDS         busy-check interval            (RA8_FLEET_POLL)
  --stop-grace SECONDS   docker stop -t for an IDLE instance
  --full-instances N     capacity outside the window    (RA8_FLEET_FULL_INSTANCES)
  --quiet-instances N    capacity inside it             (RA8_FLEET_QUIET_INSTANCES)
  --quiet-start HH:MM    window start                   (RA8_FLEET_QUIET_START)
  --quiet-end HH:MM      window end                     (RA8_FLEET_QUIET_END)
  --quiet-days Mon,Tue   days the window applies to     (RA8_FLEET_QUIET_DAYS)
  --dev-slice UNIT       low-priority dev slice to freeze while this host's
                         runner target is 0             (RA8_FLEET_DEV_SLICE)
EOF
}

# --- the dev slice ----------------------------------------------------------
#
# A host may lend its idle capacity to agent verification work in a
# low-priority cgroup (the dev_slice role). Standing the runners down has to
# reach that work too, or it buys the owner nothing: quiet hours would park
# three idle containers while a gate suite in the slice went on using the
# machine, which is the entire thing the window exists to prevent.
#
# THE RULE, in one line: the slice is frozen exactly while this host's runner
# target is ZERO -- by the quiet-hours timer, or by a deliberate
# `just infra::scale HOST=x N=0` ("I want to play a game for an hour"). Any
# non-zero target means the machine is working and dev work may share it at its
# weight. One rule covers both paths, in one place.
#
# FREEZE, NOT STOP, and for the same reason the runner drain never signals a
# busy instance: work in flight must not be destroyed. cgroup v2's freezer
# suspends every process in the slice where it stands -- including the
# containerised gate run, whose scope is a child of the slice -- and thawing
# resumes them exactly there. An agent's suite pauses for the evening instead
# of dying at 18:00.
#
# The cost, stated rather than hidden: a run that is frozen for hours resumes
# with its wall-clock budgets already spent, so a gate with a time budget can
# fail on the way out. That is why `ra8-dev` REFUSES to start new work while
# the slice is frozen -- a paused run is the caller's informed choice, a run
# that silently began five minutes before a window is not.

dev_slice_freezer_state() {
  systemctl show -p FreezerState --value "${RA8_FLEET_DEV_SLICE}" 2>/dev/null || true
}

dev_slice_active() {
  [ "$(systemctl is-active "${RA8_FLEET_DEV_SLICE}" 2>/dev/null || true)" = active ]
}

# Freeze or thaw the dev slice to match a runner target. Never fatal: a host
# whose slice is absent or already gone is a host with nothing to suspend, and
# failing the window over that would leave the RUNNERS at the wrong capacity
# over a dev-work detail.
dev_slice_follow() {
  local target="$1" state
  [ -n "${RA8_FLEET_DEV_SLICE}" ] || return 0
  if ! command -v systemctl >/dev/null 2>&1; then
    log "dev-slice ${RA8_FLEET_DEV_SLICE}: no systemctl on this host; cannot follow"
    return 0
  fi
  if ! dev_slice_active; then
    log "dev-slice ${RA8_FLEET_DEV_SLICE}: not active; nothing to suspend"
    return 0
  fi
  state="$(dev_slice_freezer_state)"
  if [ "${target}" -eq 0 ]; then
    case "${state}" in
      frozen | freezing) log "dev-slice ${RA8_FLEET_DEV_SLICE}: already frozen" ;;
      *)
        log "dev-slice ${RA8_FLEET_DEV_SLICE}: freezing -- runner target is 0"
        systemctl freeze "${RA8_FLEET_DEV_SLICE}" ||
          log "dev-slice ${RA8_FLEET_DEV_SLICE}: freeze FAILED; dev work is still running"
        ;;
    esac
    return 0
  fi
  case "${state}" in
    running | "") log "dev-slice ${RA8_FLEET_DEV_SLICE}: running" ;;
    *)
      log "dev-slice ${RA8_FLEET_DEV_SLICE}: thawing -- runner target is ${target}"
      systemctl thaw "${RA8_FLEET_DEV_SLICE}" ||
        log "dev-slice ${RA8_FLEET_DEV_SLICE}: thaw FAILED; dev work is still suspended"
      ;;
  esac
}

dev_slice_status() {
  [ -n "${RA8_FLEET_DEV_SLICE}" ] || return 0
  local state detail
  if ! dev_slice_active; then
    printf '  %-20s %-9s %-7s %s\n' "${RA8_FLEET_DEV_SLICE}" "inactive" "-" \
      "dev slice not started on this host"
    return 0
  fi
  state="$(dev_slice_freezer_state)"
  case "${state}" in
    frozen | freezing) detail="dev work SUSPENDED (runner target 0)" ;;
    *) detail="dev work runs at its weight, below CI" ;;
  esac
  printf '  %-20s %-9s %-7s %s\n' "${RA8_FLEET_DEV_SLICE}" "active" \
    "${state:-running}" "${detail}"
}

# --- docker kind ------------------------------------------------------------

# Deliberately unquoted: RA8_FLEET_DOCKER may be "sudo docker" (the NAS, where
# the deploy user is not in the docker group), which has to word-split.
dk() {
  command $RA8_FLEET_DOCKER "$@"
}

container_state() {
  local name="$1" state
  state="$(dk inspect -f '{{.State.Status}}' "${name}" 2>/dev/null || true)"
  printf '%s' "${state:-absent}"
}

# 0 when a job is running in this container. Runner.Listener spawns exactly one
# Runner.Worker process per job, so the process either exists or the runner is
# idle. `pid` must stay in the -o list: docker rejects a ps format without it
# ("Couldn't find PID field in ps output"), so `-o comm` alone silently becomes
# an error the caller would read as "not busy".
#
# `docker top` output is read WHOLE and only then filtered. Neither reader may
# exit early: a busy runner's container holds a hundred build processes, docker
# writes that list in several chunks, and a filter that stops at the first match
# closes the pipe under it -- SIGPIPE, a 141 through `pipefail`, and under
# `set -e` the script dies mid-sweep. It survived every test against an idle
# container, whose whole list fits in one write.
container_busy() {
  local name="$1" out
  [ "$(container_state "${name}")" = running ] || return 1
  out="$(dk top "${name}" -o pid,comm 2>/dev/null || true)"
  printf '%s\n' "${out}" | awk '$2 == "Runner.Worker" {found = 1} END {exit !found}'
}

# How long this instance's current job has been running, for the status table.
# Empty when idle.
busy_for() {
  local name="$1" out
  out="$(dk top "${name}" -o pid,comm,etime 2>/dev/null || true)"
  printf '%s\n' "${out}" | awk '$2 == "Runner.Worker" && !seen {print $3; seen = 1}'
}

# The race this closes: a job can be assigned in the moment between the busy
# check passing and the stop landing. It is small (one round trip) and cannot
# be eliminated without a GitHub credential on the host, so it is DETECTED and
# reported rather than assumed away.
#
# Bounded by --since rather than --tail: a runner that has been up for weeks has
# a log far longer than any tail worth reading, and the only lines that can
# answer "did this stop cancel a job" are the ones written after the stop began.
assert_not_interrupted() {
  local name="$1" since="$2" window last
  window="$(dk logs --since "${since}" "${name}" 2>&1 |
    grep -Eo '(Running job: .*|Job .* completed with result: [A-Za-z]+)$' || true)"
  case "${window}" in
    *"completed with result: Canceled"*)
      log "INTERRUPTED ${name}: a job was CANCELLED by this stop"
      printf '%s\n' "${window}" | sed 's/^/            /'
      return 1
      ;;
  esac
  # The verdict is the LAST boundary in the window, not whether a "Running
  # job:" line appears in it at all. A clean stop of a runner that finished a
  # job seconds earlier has BOTH lines in its window, and an any-occurrence
  # test calls that an interruption -- measured on the Windows host, where the
  # runner logged "Job Pre-commit gate suite completed with result: Succeeded"
  # at 07:17:19 and exited cleanly at 07:17:32, and the first cut of this check
  # reported it INTERRUPTED. A detector that cries wolf on a correct drain is
  # worse than none: the next real one gets ignored.
  #
  # A TRAILING "Running job:" is the real signal -- work taken and never
  # logged finishing.
  last="$(printf '%s' "${window}" | tail -1)"
  case "${last}" in
    "Running job: "*)
      log "INTERRUPTED ${name}: a job started during the stop and never finished"
      printf '%s\n' "${window}" | sed 's/^/            /'
      return 1
      ;;
    *)
      log "verified  ${name}: nothing cancelled; last boundary: ${last:-none in window}"
      ;;
  esac
}

park_instance() {
  local name="$1" since
  # Unix seconds: the one --since form docker parses without ambiguity, and the
  # one that cannot be misread across the container's and the host's idea of a
  # timezone.
  since="$(date +%s)"
  log "parking   ${name}: idle, stopping with -t ${RA8_FLEET_STOP_GRACE}"
  dk stop -t "${RA8_FLEET_STOP_GRACE}" "${name}" >/dev/null
  assert_not_interrupted "${name}" "${since}"
}

unpark_instance() {
  local name="$1" state
  state="$(container_state "${name}")"
  case "${state}" in
    running) log "active    ${name}: already running" ;;
    absent) die "${name} does not exist on this host. Deploy it first: just infra::apply <host>" ;;
    *)
      log "resuming  ${name}: was ${state}"
      dk start "${name}" >/dev/null
      ;;
  esac
}

docker_status() {
  local name state busy elapsed
  for name in "${CONTAINERS[@]}"; do
    state="$(container_state "${name}")"
    if container_busy "${name}"; then
      busy=busy
      elapsed="job running for $(busy_for "${name}")"
    else
      busy=idle
      elapsed="ready to park"
    fi
    [ "${state}" = running ] || elapsed="parked"
    printf '  %-20s %-9s %-7s %s\n' "${name}" "${state}" "${busy}" "${elapsed}"
  done
}

# Shrink by attrition: sweep the instances that must go, stop whichever are
# idle right now, and come back for the rest. An instance stopped in an earlier
# sweep cannot be handed a new job, so the host only ever converges downward.
docker_drain_to() {
  local want="$1" deadline pending=() still=() name
  local total="${#CONTAINERS[@]}"
  pending=("${CONTAINERS[@]:want}")
  [ "${#pending[@]}" -eq 0 ] && return 0
  deadline=$(($(date +%s) + RA8_FLEET_DEADLINE))
  log "draining  ${#pending[@]} of ${total} instance(s): ${pending[*]}"
  while [ "${#pending[@]}" -gt 0 ]; do
    still=()
    for name in "${pending[@]}"; do
      case "$(container_state "${name}")" in
        running) ;;
        absent) log "skipping  ${name}: not deployed on this host" && continue ;;
        *)
          log "parked    ${name}: already stopped"
          continue
          ;;
      esac
      if container_busy "${name}"; then
        log "waiting   ${name}: busy, job running for $(busy_for "${name}")"
        still+=("${name}")
      else
        park_instance "${name}"
      fi
    done
    pending=(${still[@]+"${still[@]}"})
    [ "${#pending[@]}" -eq 0 ] && break
    if [ "$(date +%s)" -ge "${deadline}" ]; then
      log "DEADLINE  ${RA8_FLEET_DEADLINE}s elapsed; still busy: ${pending[*]}"
      log "NOT converged. These were left RUNNING on purpose: forcing them"
      log "would cancel the jobs they hold, which is the failure this avoids."
      return 1
    fi
    sleep "${RA8_FLEET_POLL}"
  done
  log "drained   every instance above the target is parked"
}

# Every runner container ACTUALLY on this host, whatever the declaration says.
#
# Load-bearing for the pre-provision drain. Crossing the 1 <-> N instance
# boundary renames the containers (`ra8-ci-runner` becomes `ra8-ci-runner-1`,
# ...), so a drain that walked the DECLARED names would find nothing to stop on
# exactly the change that most needs draining -- and the converge would then
# recreate, and cancel, whatever the host was really running.
discover_containers() {
  dk ps -a --filter "name=^${RA8_FLEET_PREFIX}" --format '{{.Names}}' | sort
}

docker_drain_all() {
  local found=()
  # First, and deliberately before the drain: a converge is about to recreate
  # every container, and dev work has no business competing with the jobs the
  # drain is waiting to finish. Freezing here also makes them finish sooner.
  dev_slice_follow 0
  while IFS= read -r line; do
    [[ -n "$line" ]] && found+=("$line")
  done < <(discover_containers)
  if [ "${#found[@]}" -eq 0 ]; then
    log "nothing    no ${RA8_FLEET_PREFIX}* container on this host"
    return 0
  fi
  log "found     ${#found[@]} runner container(s): ${found[*]}"
  CONTAINERS=("${found[@]}")
  docker_drain_to 0
}

docker_scale() {
  local want="$1" i
  [ "${want}" -ge 0 ] 2>/dev/null || die "scale needs a non-negative integer, got '${want}'"
  [ "${want}" -le "${#CONTAINERS[@]}" ] ||
    die "this host declares ${#CONTAINERS[@]} instance(s); cannot scale to ${want} without re-deploying it (just infra::apply <host>)"
  # Before the runners move, either way. Standing down: the owner asked for the
  # machine, so give it to them now rather than after a drain that may take
  # several job cycles -- and a frozen dev slice makes those cycles shorter.
  # Coming back: thawing is instant and costs a suspended run nothing.
  dev_slice_follow "${want}"
  for ((i = 0; i < want; i++)); do
    unpark_instance "${CONTAINERS[i]}"
  done
  docker_drain_to "${want}"
}

# --- k8s kind ---------------------------------------------------------------

kc() {
  command $RA8_FLEET_KUBECTL "$@"
}

k8s_status() {
  kc get autoscalingrunnersets -n "${RA8_FLEET_NAMESPACE}" "${RA8_FLEET_SCALESET}" \
    -o custom-columns='NAME:.metadata.name,MIN:.spec.minRunners,MAX:.spec.maxRunners,CURRENT:.status.currentRunners' ||
    die "no scale set ${RA8_FLEET_SCALESET} in namespace ${RA8_FLEET_NAMESPACE}"
}

# ARC runners are ephemeral -- one job, then the pod exits -- and the controller
# only deletes runners that hold no job, so lowering the ceiling never
# interrupts work. The drain here is the controller's, and this moves the
# number it drains toward. Note the helm release still holds the DECLARED
# maximum, so `just infra::apply HOST=k3s-pve` restores it; that is intentional
# for a quiet-hours window, which is temporary by definition.
k8s_scale() {
  local want="$1"
  [ "${want}" -ge 0 ] 2>/dev/null || die "scale needs a non-negative integer, got '${want}'"
  log "patching  ${RA8_FLEET_SCALESET} maxRunners -> ${want}"
  kc patch autoscalingrunnerset -n "${RA8_FLEET_NAMESPACE}" "${RA8_FLEET_SCALESET}" \
    --type=merge -p "{\"spec\":{\"maxRunners\":${want}}}" >/dev/null
  log "patched   ARC will retire idle runners down to the new ceiling; running"
  log "          jobs finish first because its runners are ephemeral."
  k8s_status
}

# --- quiet hours ------------------------------------------------------------
#
# ONE timer that runs often and asks "what should this host be right now",
# rather than a pair of timers firing at the window's edges.
#
# Edge-triggered timers are wrong for a machine that is switched off, asleep,
# or mid-upgrade at the moment one would have fired: the transition is simply
# missed, and the host sits at the wrong capacity until the next edge -- which
# on a Friday-evening window means all weekend. This form is level-triggered
# and idempotent, so a host that was off at 18:00 goes quiet at 18:10 instead,
# and a host already at its target does nothing at all. It also removes the
# only genuinely fiddly case, a window that crosses midnight, from the timer
# and puts it here where it can be reasoned about once.

day_is_listed() {
  case ",${RA8_FLEET_QUIET_DAYS}," in
    *",$1,"*) return 0 ;;
    *) return 1 ;;
  esac
}

# 0 when the wall clock is inside the declared window. A window whose end is
# not after its start wraps past midnight, and then "inside" means either
# after the start on a listed day, or before the end on the day AFTER one.
in_quiet_window() {
  local now today yesterday
  now="$(LC_ALL=C date +%H:%M)"
  today="$(LC_ALL=C date +%a)"
  yesterday="$(LC_ALL=C date -d yesterday +%a)"
  if [[ "${RA8_FLEET_QUIET_START}" < "${RA8_FLEET_QUIET_END}" ]]; then
    day_is_listed "${today}" || return 1
    [[ "${now}" > "${RA8_FLEET_QUIET_START}" || "${now}" == "${RA8_FLEET_QUIET_START}" ]] &&
      [[ "${now}" < "${RA8_FLEET_QUIET_END}" ]]
    return
  fi
  if day_is_listed "${today}" &&
    [[ "${now}" > "${RA8_FLEET_QUIET_START}" || "${now}" == "${RA8_FLEET_QUIET_START}" ]]; then
    return 0
  fi
  day_is_listed "${yesterday}" && [[ "${now}" < "${RA8_FLEET_QUIET_END}" ]]
}

# What should this host be RIGHT NOW -- and then make it that.
#
# A host with no quiet-hours window still has an answer, and it is its declared
# instance count. This used to return "nothing to do" there, which made the
# level-triggered timer level-triggered only on hosts that declared a window.
# The consequence was measured: `just infra::scale HOST=truenas N=1` drained
# ra8-ci-runner-2 during a bench session, `restart: unless-stopped` deliberately
# does not undo an explicit stop, truenas declares no window and therefore had
# no timer -- so the NAS served CI at half its declared capacity for hours with
# nothing anywhere that would ever notice or correct it. win-ci, which declares
# a window, would have healed the same fault in ten minutes.
#
# So a live scale-down is TEMPORARY by construction now, on every host. To stand
# a host down durably, change `instances:` in infra/fleet.yml (or give it a
# quiet_hours block) and re-converge -- change the declaration, not the machine.
# That is the whole point of a declarative fleet, and it is the difference
# between a capacity decision a human can find later and one they cannot.
cmd_window() {
  local target
  if [ -z "${RA8_FLEET_QUIET_DAYS}" ]; then
    target="${RA8_FLEET_FULL_INSTANCES}"
    # Zero here would mean "converge this host to no CI at all", which no host
    # declares outside a window: it is what an unset RA8_FLEET_FULL_INSTANCES
    # looks like. Draining a whole host on a missing environment variable is
    # not a thing to do quietly.
    [ "${target}" -gt 0 ] 2>/dev/null ||
      die "no window declared and RA8_FLEET_FULL_INSTANCES is '${target}';" \
        "refusing to drain this host to zero capacity on an unset value"
    log "no window: target ${target} (this host's declared capacity)"
  elif in_quiet_window; then
    target="${RA8_FLEET_QUIET_INSTANCES}"
    log "inside    ${RA8_FLEET_QUIET_DAYS} ${RA8_FLEET_QUIET_START}-${RA8_FLEET_QUIET_END}: target ${target}"
  else
    target="${RA8_FLEET_FULL_INSTANCES}"
    log "outside   ${RA8_FLEET_QUIET_DAYS} ${RA8_FLEET_QUIET_START}-${RA8_FLEET_QUIET_END}: target ${target}"
  fi
  case "${RA8_FLEET_KIND}" in
    docker)
      require_docker_config
      docker_scale "${target}"
      ;;
    k8s) k8s_scale "${target}" ;;
    *) die "unknown --kind '${RA8_FLEET_KIND}' (docker|k8s)" ;;
  esac
}

# --- entry point ------------------------------------------------------------

parse_args() {
  while [ $# -gt 0 ]; do
    case "$1" in
      --kind) RA8_FLEET_KIND="$2" && shift 2 ;;
      # No flag in this parser ever takes a value containing a space, and that
      # is a transport constraint rather than a style choice: for the WSL host
      # this command line is assembled on the control node, parsed by Windows'
      # shell, and only then handed to `wsl -e`. A quoted argument does not
      # survive that intact, so `sudo docker` is a boolean and the container
      # list is repeated rather than joined.
      --sudo)
        RA8_FLEET_DOCKER="sudo docker"
        shift
        ;;
      --container)
        RA8_FLEET_CONTAINERS="${RA8_FLEET_CONTAINERS:+${RA8_FLEET_CONTAINERS} }$2"
        shift 2
        ;;
      --prefix) RA8_FLEET_PREFIX="$2" && shift 2 ;;
      --namespace) RA8_FLEET_NAMESPACE="$2" && shift 2 ;;
      --scale-set) RA8_FLEET_SCALESET="$2" && shift 2 ;;
      --deadline) RA8_FLEET_DEADLINE="$2" && shift 2 ;;
      --poll) RA8_FLEET_POLL="$2" && shift 2 ;;
      --stop-grace) RA8_FLEET_STOP_GRACE="$2" && shift 2 ;;
      --full-instances) RA8_FLEET_FULL_INSTANCES="$2" && shift 2 ;;
      --quiet-instances) RA8_FLEET_QUIET_INSTANCES="$2" && shift 2 ;;
      --quiet-start) RA8_FLEET_QUIET_START="$2" && shift 2 ;;
      --quiet-end) RA8_FLEET_QUIET_END="$2" && shift 2 ;;
      --quiet-days) RA8_FLEET_QUIET_DAYS="$2" && shift 2 ;;
      --dev-slice) RA8_FLEET_DEV_SLICE="$2" && shift 2 ;;
      -h | --help)
        usage
        exit 0
        ;;
      --*) die "unknown option '$1'" ;;
      *) break ;;
    esac
  done
  RA8_FLEET_ARGV=("$@")
}

require_docker_config() {
  command -v "${RA8_FLEET_DOCKER%% *}" >/dev/null 2>&1 ||
    die "'${RA8_FLEET_DOCKER%% *}' is not on PATH on this host; there is nothing to scale"
  [ -n "${RA8_FLEET_CONTAINERS}" ] ||
    die "no instance containers declared (--containers / RA8_FLEET_CONTAINERS)"
  read -r -a CONTAINERS <<<"${RA8_FLEET_CONTAINERS}"
}

cmd_status() {
  case "${RA8_FLEET_KIND}" in
    docker)
      require_docker_config
      printf '  %-20s %-9s %-7s %s\n' INSTANCE STATE BUSY DETAIL
      docker_status
      dev_slice_status
      ;;
    k8s) k8s_status ;;
    *) die "unknown --kind '${RA8_FLEET_KIND}' (docker|k8s)" ;;
  esac
}

cmd_scale() {
  [ $# -ge 1 ] || die "scale needs a target instance count"
  case "${RA8_FLEET_KIND}" in
    docker)
      require_docker_config
      docker_scale "$1"
      ;;
    k8s) k8s_scale "$1" ;;
    *) die "unknown --kind '${RA8_FLEET_KIND}' (docker|k8s)" ;;
  esac
}

cmd_drain_all() {
  [ "${RA8_FLEET_KIND}" = docker ] ||
    die "drain-all is a container-host command; an ARC scale set drains itself"
  command -v "${RA8_FLEET_DOCKER%% *}" >/dev/null 2>&1 ||
    die "'${RA8_FLEET_DOCKER%% *}' is not on PATH on this host; there is nothing to drain"
  docker_drain_all
}

main() {
  parse_args "$@"
  set -- ${RA8_FLEET_ARGV[@]+"${RA8_FLEET_ARGV[@]}"}
  local cmd="${1:-}"
  [ -n "${cmd}" ] || {
    usage
    exit 2
  }
  shift
  case "${cmd}" in
    status) cmd_status ;;
    scale) cmd_scale "$@" ;;
    drain-all) cmd_drain_all ;;
    window) cmd_window ;;
    *) die "unknown command '${cmd}'" ;;
  esac
}

main "$@"
