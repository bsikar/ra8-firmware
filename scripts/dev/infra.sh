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
# RA8_INFRA_CLASSES below is the single list of what can be provisioned. Every
# subcommand reads it, so a class cannot exist for `apply` and be invisible to
# `list`, and `make infra-list` cannot drift from what actually runs.
#
# Commands:
#   list                 what host classes exist and what serves each
#   doctor               can THIS machine drive infra at all?
#   check <class>        dry run: what would change, changing nothing
#   apply <class>        provision for real
#   remove <class>       tear down (only classes that implement it)
#   status               what is deployed across the estate, right now
#
# `status` and `doctor` are strictly READ-ONLY and safe to run at any time,
# including while CI jobs and agents are working. `apply` and `remove` are not.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ANSIBLE_DIR="${ROOT}/infra/ansible"
INVENTORY="${ANSIBLE_DIR}/inventory/hosts.ini"

# One row per provisionable host class:
#   <class>|<playbook>|<inventory group>|<roles>|<removable>|<what it is>
# `removable` is yes only where the role genuinely implements a teardown path;
# claiming one that does not exist is worse than admitting there is none.
RA8_INFRA_CLASSES=(
  "dev|dev-box.yml|dev_boxes|dev_box|no|the shared box agents run gates on"
  "k3s|k3s-node.yml|ci_runners|k3s_node,openbao|no|the k3s cluster + the OpenBao vault"
  "ci-runner|ci-runner.yml|ci_runners|ci_runner|no|the ARC autoscaling runner pool on k3s"
  "ci-runner-docker|ci-runner-docker.yml|ci_runners_docker|ci_runner_docker|yes|a plain runner in Docker (NAS, gaming PC)"
  "bench|hil-bench.yml|hil_bench|hil_bench,c6_toolchain,ad2_tools|no|the HIL bench Pi, ESP32-C6 and AD2"
)

die() {
  echo "error: $*" >&2
  exit 1
}

# Look one class up in the registry; empty output means "no such class".
class_row() {
  local want="$1" row
  for row in "${RA8_INFRA_CLASSES[@]}"; do
    [ "${row%%|*}" = "${want}" ] && {
      printf '%s' "${row}"
      return 0
    }
  done
  return 1
}

class_names() {
  local row
  for row in "${RA8_INFRA_CLASSES[@]}"; do printf '%s ' "${row%%|*}"; done
}

cmd_list() {
  printf '%-18s %-22s %-18s %s\n' CLASS PLAYBOOK "INVENTORY GROUP" DESCRIPTION
  local row name play group roles rm desc
  for row in "${RA8_INFRA_CLASSES[@]}"; do
    IFS='|' read -r name play group roles rm desc <<<"${row}"
    printf '%-18s %-22s %-18s %s\n' "${name}" "${play}" "${group}" "${desc}"
    printf '%-18s   roles: %s%s\n' '' "${roles}" \
      "$([ "${rm}" = yes ] && echo '   [supports infra-remove]')"
  done
  echo
  echo "make infra-check HOST=<class>   dry run (changes nothing)"
  echo "make infra-apply HOST=<class>   provision for real"
  echo "make infra-status              what is deployed right now (read-only)"
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

doctor_inventory() {
  if [ -f "${INVENTORY}" ]; then
    printf '  ok    %-18s %s\n' inventory "${INVENTORY#"${ROOT}"/}"
    return 0
  fi
  printf '  MISS  %-18s %s\n' inventory \
    "${INVENTORY#"${ROOT}"/} absent -- run 'make infra-setup'"
  return 1
}

cmd_doctor() {
  local rc=0
  doctor_tools || rc=1
  echo "inventory:"
  doctor_inventory || rc=1
  echo "hosts (ssh, from THIS machine):"
  probe_ssh dev "dev box" || rc=1
  probe_ssh k3s-pve "k3s node" || rc=1
  probe_ssh star "bench Pi" || rc=1
  probe_ssh pve "proxmox host" no
  probe_ssh truenas "NAS runner" no
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

# --- check / apply / remove -------------------------------------------------

require_playbook_env() {
  command -v ansible-playbook >/dev/null 2>&1 ||
    die "ansible-playbook is not on PATH. Run 'make infra-doctor' for the fix."
  [ -f "${INVENTORY}" ] ||
    die "no inventory at ${INVENTORY#"${ROOT}"/}. Run 'make infra-setup' first."
}

run_playbook() {
  local class="$1"
  shift
  local row play
  row="$(class_row "${class}")" ||
    die "unknown host class '${class}'. Known: $(class_names)"
  play="$(printf '%s' "${row}" | cut -d'|' -f2)"
  require_playbook_env
  echo "==> ansible-playbook ${play} $*"
  (cd "${ANSIBLE_DIR}" && ansible-playbook -i "${INVENTORY}" "playbooks/${play}" "$@")
}

cmd_check() {
  # --check --diff: ansible reports what it WOULD change and changes nothing.
  # Worth knowing: tasks gated on `creates:` or on a probe of something not yet
  # installed report "skipped" in check mode rather than the work they would
  # really do, so a clean check run is evidence of reachability and syntax, not
  # a complete change list.
  run_playbook "$1" --check --diff
}

cmd_apply() { run_playbook "$1"; }

cmd_remove() {
  local class="$1" row removable
  row="$(class_row "${class}")" ||
    die "unknown host class '${class}'. Known: $(class_names)"
  removable="$(printf '%s' "${row}" | cut -d'|' -f5)"
  [ "${removable}" = yes ] || die "host class '${class}' implements no teardown path.
Only classes marked [supports infra-remove] in 'make infra-list' can be removed
this way, because only their roles own both halves of the lifecycle. Removing
the others means undoing them by hand, which is exactly the drift the roles
exist to prevent -- add a removal path to the role instead."
  run_playbook "${class}" -e "ci_runner_docker_state=absent"
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
  echo "toolchain parity on the dev box:"
  status_host dev "parity" \
    'cd ~/ra8-firmware && PATH=$HOME/.local/bin:$PATH python3 scripts/checks/check_tool_versions.py --all 2>&1 | tail -1' || true
}

usage() {
  cat <<EOF
usage: infra.sh <command> [args]

  list                what host classes exist and what serves each
  doctor              can THIS machine drive infra at all?
  check <class>       dry run -- report what would change, change nothing
  apply <class>       provision for real
  remove <class>      tear down (only classes that implement it)
  status              what is deployed across the estate, right now

Host classes: $(class_names)
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
      [ $# -ge 1 ] || die "check needs a host class: $(class_names)"
      cmd_check "$1"
      ;;
    apply)
      [ $# -ge 1 ] || die "apply needs a host class: $(class_names)"
      cmd_apply "$1"
      ;;
    remove)
      [ $# -ge 1 ] || die "remove needs a host class: $(class_names)"
      cmd_remove "$1"
      ;;
    -h | --help | help | "") usage ;;
    *) die "unknown command '${cmd}'. Run 'infra.sh help'." ;;
  esac
}

main "$@"
