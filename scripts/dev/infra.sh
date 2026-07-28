#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/dev/infra.sh -- the front door to infra/.
#
# WHY THIS EXISTS
# ---------------
# Every other real capability in this tree is one `make` verb: `make ci`,
# `make hil`, `make ws-new`, `make ci-status`. Infrastructure was the exception
# -- bare `ansible-playbook -i inventory/hosts.ini playbooks/<something>.yml`
# invocations known only to whoever wrote the role. That is the same tribal
# knowledge the roles themselves were written to abolish, one level up.
#
# So this holds the logic and mk/infra.mk is a thin wrapper, exactly as
# mk/workspace.mk wraps agent_workspace.sh.
#
# THE ONE REGISTRY
# ----------------
# infra/fleet.yml is the single list of what machines exist. This script does
# NOT keep a second one: every host-shaped subcommand hands the name straight
# to scripts/dev/fleet.py, which reads that declaration and derives the
# playbook, the inventory group, the transport and every role variable from it.
#
# A class list lived here once. It was a second place to answer "what can be
# provisioned", and adding a machine meant editing it as well as the roles --
# which is exactly the shape of duplication the roles were written to abolish,
# one level up.
#
# Commands:
#   list                 what machines are declared, and how they are sized
#   doctor               can THIS machine drive infra at all?
#   check <host>         dry run: what would change, changing nothing
#   apply <host>         converge that host to the declaration
#   remove <host>        tear down (classes whose roles implement it)
#   scale <host> <n>     live capacity change; shrinking DRAINS, never kills
#   status               what is deployed across the estate, right now
#
# `list`, `status` and `doctor` are strictly READ-ONLY and safe to run at any
# time, including while CI jobs and agents are working. The rest are not.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FLEET="${ROOT}/scripts/dev/fleet.py"

die() {
  echo "error: $*" >&2
  exit 1
}

fleet() {
  python3 "${FLEET}" "$@"
}

host_names() {
  fleet list | awk 'NR > 1 && NF > 1 && $1 !~ /^(make|docs)/ {print $1}'
}

cmd_list() {
  fleet list
}

# --- doctor -----------------------------------------------------------------

# probe_ssh <host> <label> [required]
#
# `required` defaults to yes. An OPTIONAL host that is unreachable is reported
# and returns 0, because not every control node needs to reach every machine --
# the Proxmox host and the NAS are not needed to provision the dev box. That
# distinction is a parameter rather than a `|| true` at the call site on
# purpose: masking the exit status swallows a genuine failure inside the
# function body along with the one case we meant to tolerate.
probe_ssh() {
  local host="$1" label="$2" required="${3:-yes}"
  # -n is not optional: without it ssh reads its own stdin, and when this script
  # is itself being fed on stdin (`bash -s < infra.sh`, which is how a remote
  # invocation works) the first probe swallows the rest of the script and
  # everything after it silently never runs. Measured, not theorised.
  if ssh -n -o ConnectTimeout=6 -o BatchMode=yes "${host}" 'echo ok' >/dev/null 2>&1; then
    printf '  ok    %-18s %s\n' "${label}" "reachable"
    return 0
  fi
  if [ "${required}" = yes ]; then
    printf '  MISS  %-18s %s\n' "${label}" "NOT reachable over ssh"
    return 1
  fi
  printf '  note  %-18s %s\n' "${label}" "not reachable (optional from here)"
  return 0
}

doctor_tools() {
  local missing=0 tool
  echo "control-node tooling:"
  for tool in ansible ansible-playbook ssh; do
    if command -v "${tool}" >/dev/null 2>&1; then
      printf '  ok    %-18s %s\n' "${tool}" "$(command -v "${tool}")"
    else
      printf '  MISS  %-18s %s\n' "${tool}" "not on PATH"
      missing=1
    fi
  done
  return "${missing}"
}

doctor_declaration() {
  if fleet validate; then
    return 0
  fi
  printf '  MISS  %-18s %s\n' declaration "infra/fleet.yml does not validate"
  return 1
}

cmd_doctor() {
  local rc=0
  doctor_tools || rc=1
  echo "declaration:"
  doctor_declaration || rc=1
  echo "hosts (from THIS machine, each over its own declared transport):"
  # Delegated, and derived: `win-ci` is not an ssh alias but a jump through the
  # bench Pi into a Windows box and then into a WSL distro, so only fleet.py
  # knows how to reach it. A machine added to the declaration is probed from
  # the next run with nothing here edited.
  fleet reach || rc=1
  probe_ssh pve "proxmox host" no
  echo
  if [ "${rc}" -ne 0 ]; then
    cat <<'EOF'
NOT ready to drive infra from this machine.

The estate is split in a way that bites: the Mac reaches every host over ssh
but has no ansible, and the dev box has ansible but cannot resolve the cluster
hosts. Whichever machine you drive from needs BOTH -- install ansible where the
ssh access already is (`pipx install ansible-core`, or `brew install ansible`),
then `make infra-setup` to write the git-ignored inventory.
EOF
  else
    echo "ready: ansible present, inventory written, hosts reachable."
  fi
  return "${rc}"
}

# --- check / apply / remove / scale ------------------------------------------
#
# All four are fleet.py verbs. The ansible invocation, the inventory, the extra
# vars and (for the Windows host) the copy-into-the-distro transport are all
# derived from the declaration, so there is nothing left for this script to
# assemble.

require_playbook_env() {
  command -v ansible-playbook >/dev/null 2>&1 ||
    die "ansible-playbook is not on PATH. Run 'make infra-doctor' for the fix."
}

cmd_check() {
  # --check --diff: ansible reports what it WOULD change and changes nothing.
  # Worth knowing: tasks gated on `creates:` or on a probe of something not yet
  # installed report "skipped" in check mode rather than the work they would
  # really do, so a clean check run is evidence of reachability and syntax, not
  # a complete change list.
  require_playbook_env
  fleet check "$@"
}

cmd_apply() {
  require_playbook_env
  fleet apply "$@"
}

cmd_remove() {
  require_playbook_env
  fleet remove "$@"
}

cmd_scale() {
  [ $# -ge 2 ] || die "scale needs a host and a target instance count"
  fleet scale "$1" "$2"
}

# --- status -----------------------------------------------------------------
#
# Read-only, and deliberately cheap on GitHub API quota: one REST call for the
# runner list, everything else over ssh. `gh run watch`-style polling is what
# exhausted the shared 5000/hour budget twice in one day.

status_host() {
  local host="$1" label="$2" probe="$3" out
  # -n for the same reason as probe_ssh: never let a probe eat this script's
  # own stdin.
  if ! out="$(ssh -n -o ConnectTimeout=8 -o BatchMode=yes "${host}" "${probe}" 2>&1)"; then
    printf '  %-12s UNREACHABLE  (%s)\n' "${label}" "${out%%$'\n'*}"
    return 1
  fi
  printf '  %-12s %s\n' "${label}" "${out}"
}

status_runners() {
  echo "GitHub runner fleet (one REST call):"
  command -v gh >/dev/null 2>&1 || {
    echo "  gh not on PATH -- skipping (this is the only quota-costing probe)"
    return 0
  }
  gh api repos/bsikar/ra8-firmware/actions/runners --paginate \
    --jq '.runners[] | "  \(.name)  \(.status)  busy=\(.busy)  [\([.labels[].name] | join(","))]"' \
    2>/dev/null || echo "  could not read the runner list (quota, or no token scope)"
}

# shellcheck disable=SC2016
# The single quotes below are deliberate and load-bearing: every one of these
# strings is a command sent to ANOTHER machine over ssh, so `$2`, `$HOME` and
# `$(...)` must survive this shell untouched and expand on the remote host.
# Double-quoting them would interpolate the control node's values into a probe
# meant to report the target's -- e.g. printing the Mac's load average and
# labelling it the dev box's.
cmd_status() {
  echo "=== estate status  ($(date -u '+%Y-%m-%dT%H:%M:%SZ')) ==="
  echo
  echo "hosts:"
  status_host dev "dev box" \
    'printf "load%s  " "$(cut -d" " -f1-3 /proc/loadavg)"; nproc | tr -d "\n"; echo " cpus"' || true
  status_host k3s-pve "k3s node" \
    'sudo k3s kubectl get nodes --no-headers 2>/dev/null | awk "{print \$2}" | tr "\n" " "; uptime | sed "s/.*load/load/"' || true
  status_host star "bench Pi" \
    'ls /dev/serial/by-id/ 2>/dev/null | wc -l | tr -d "\n"; echo " serial device(s) by-id"' || true
  echo
  echo "CI runner pool (ARC pods on k3s):"
  status_host k3s-pve "scale set" \
    'sudo k3s kubectl get autoscalingrunnersets -n arc-runners --no-headers 2>/dev/null | awk "{print \$1\"  min=\"\$2\" max=\"\$3\" current=\"\$4}"' || true
  echo
  echo "OpenBao vault:"
  status_host k3s-pve "seal state" \
    'sudo k3s kubectl exec -n openbao openbao-0 -- bao status -format=json 2>/dev/null | python3 -c "import json,sys;d=json.load(sys.stdin);print(\"initialized=%s sealed=%s\"%(d[\"initialized\"],d[\"sealed\"]))" 2>/dev/null || echo "could not read status"' || true
  echo
  status_runners
  echo
  # Per-instance, from each host itself: which instances are up, which are
  # parked, and how long the busy ones have been on their current job. The
  # REST call above cannot tell a parked instance from a dead one.
  echo "declared capacity, per host:"
  fleet status || true
  echo
  echo "toolchain parity on the dev box:"
  status_host dev "parity" \
    'cd ~/ra8-firmware && PATH=$HOME/.local/bin:$PATH python3 scripts/checks/check_tool_versions.py --all 2>&1 | tail -1' || true
}

usage() {
  cat <<EOF
usage: infra.sh <command> [args]

  list                what machines are declared, and how they are sized
  doctor              can THIS machine drive infra at all?
  check <host>        dry run -- report what would change, change nothing
  apply <host>        converge that machine to infra/fleet.yml
  remove <host>       tear down (classes whose roles implement it)
  scale <host> <n>    live capacity change; shrinking DRAINS, never kills
  status              what is deployed across the estate, right now

HOST is a machine declared in infra/fleet.yml: $(host_names | tr '\n' ' ')
EOF
}

main() {
  local cmd="${1:-}"
  shift || true
  case "${cmd}" in
    list) cmd_list ;;
    doctor) cmd_doctor ;;
    status) cmd_status ;;
    check)
      [ $# -ge 1 ] || die "check needs a host: $(host_names | tr '\n' ' ')"
      cmd_check "$@"
      ;;
    apply)
      [ $# -ge 1 ] || die "apply needs a host: $(host_names | tr '\n' ' ')"
      cmd_apply "$@"
      ;;
    remove)
      [ $# -ge 1 ] || die "remove needs a host: $(host_names | tr '\n' ' ')"
      cmd_remove "$@"
      ;;
    scale) cmd_scale "$@" ;;
    -h | --help | help | "") usage ;;
    *) die "unknown command '${cmd}'. Run 'infra.sh help'." ;;
  esac
}

main "$@"
