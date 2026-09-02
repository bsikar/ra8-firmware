#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/dev/infra.sh -- the front door to infra/.
#
# WHY THIS EXISTS
# ---------------
# Every other real capability in this tree is one Just recipe: `just ci`,
# `just hil`, `just workspace::new`, `just quality::local::gate ci-status-contract`. Infrastructure was the exception
# -- bare `ansible-playbook -i inventory/hosts.ini playbooks/<something>.yml`
# invocations known only to whoever wrote the role. That is the same tribal
# knowledge the roles themselves were written to abolish, one level up.
#
# This script holds the orchestration logic, while the namespaced Just recipes
# provide the operator-facing entry points.
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
# NO HOST NAME IS SPELLED HERE EITHER
# ----------------------------------
# Not even in a probe. Every ssh command this script runs is asked for with
# `fleet.py ssh-target <host>`, which builds it from the declaration's address,
# user and jump. Spelling `ssh truenas` would have quietly re-introduced #526:
# those aliases lived in one laptop's ~/.ssh/config, so `just infra::status` from
# the dev box reported the entire estate unreachable when in fact every machine
# answered on its address.
#
# Commands:
#   list                 what machines are declared, and how they are sized
#   show <host>          inspect one host declaration and its derived values
#   doctor               can THIS machine drive infra at all?
#   ssh-config           name every declared machine in your ~/.ssh/config
#   ssh-config-preview   print the fragment without installing it
#   check <host>         dry run: what would change, changing nothing
#   apply <host>         converge that host to the declaration
#   register-runner      first-register a declared Docker runner host
#   register-hil         first-register the declared native HIL listener
#   remove <host>        tear down (classes whose roles implement it)
#   scale <host> <n>     live capacity change; shrinking DRAINS, never kills
#   status               what is deployed across the estate, right now
#
# `list`, `status` and `doctor` are strictly READ-ONLY and safe to run at any
# time, including while CI jobs and agents are working. The rest are not.

set -euo pipefail

[[ "${RA8_INFRA_SANITIZED:-}" == v1 ]] || {
  echo "error: enter infrastructure only through a just infra:: recipe" >&2
  exit 1
}
[[ -z "${BASH_ENV:-}" && -z "${ENV:-}" && -z "${PYTHONPATH:-}" && -z "${PYTHONHOME:-}" ]] || {
  echo "error: infrastructure startup environment was not sanitized" >&2
  exit 1
}

if [[ "${1:-}" == --selftest-boundary ]]; then
  if (($# != 1)); then
    echo "error: infrastructure boundary selftest takes no arguments" >&2
    exit 1
  fi
  exit 0
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export ANSIBLE_COLLECTIONS_PATH="$ROOT/.ansible/collections"
FLEET="${ROOT}/scripts/dev/fleet.py"
MANAGED_VENV="$ROOT/.venv"
MANAGED_BIN="$MANAGED_VENV/bin"
MANAGED_PATH="$MANAGED_BIN:/usr/local/bin:/usr/bin:/bin"
[[ -x "$MANAGED_BIN/python3" && -x "$MANAGED_BIN/ansible-playbook" ]] || {
  echo "error: locked Python/Ansible environment is absent; run 'just setup'" >&2
  exit 1
}
[[ ! -L "$MANAGED_VENV" && "$(readlink -f "$MANAGED_BIN/python3")" == "$(readlink -f /usr/bin/python3)" ]] || {
  echo "error: repository Python authority does not use the fixed system interpreter" >&2
  exit 1
}

verify_managed_python_environment() {
  (
    cd "$ROOT"
    UV_PROJECT_ENVIRONMENT="$MANAGED_VENV" UV_PYTHON_DOWNLOADS=never \
      UV_CACHE_DIR="$ROOT/.tools/uv" \
      /usr/bin/python3 -I -S "$ROOT/scripts/dev/bootstrap_uv.py" \
      --run --no-config sync --locked --all-groups --no-install-project \
      --python /usr/bin/python3 --check
  ) >/dev/null
}

if ! verify_managed_python_environment; then
  echo "error: repository .venv does not exactly match pyproject.toml and uv.lock" >&2
  exit 1
fi
export PATH="$MANAGED_PATH"
PYTHON="$MANAGED_BIN/python3"

# Filled in by ssh_argv(); an array because an ssh command with a ProxyJump is
# several words and `eval`-ing a string here would be a shell injection seam
# for no gain.
RA8_SSH_ARGV=()

die() {
  echo "error: $*" >&2
  exit 1
}

fleet() {
  "$PYTHON" -I "${FLEET}" "$@"
}

host_names() {
  fleet list | awk 'NR > 1 && NF > 1 && $1 !~ /^(just|docs)/ {print $1}'
}

cmd_list() {
  fleet list
}

cmd_show() {
  [ $# -eq 1 ] || die "show needs exactly one host: $(host_names | tr '\n' ' ')"
  fleet show "$1"
}

cmd_ssh_config() {
  fleet ssh-config --install
}

cmd_ssh_config_preview() {
  fleet ssh-config
}

# ssh_argv <host>
#
# The ssh command that reaches a DECLARED host from this machine, as words.
# Splitting on whitespace is safe by construction: the fleet-declaration gate
# rejects an address, user or jump containing any.
ssh_argv() {
  local out
  out="$(fleet ssh-target "$1" 2>/dev/null)" || return 1
  # The fleet-declaration gate rejects whitespace inside every argv field.
  read -r -a RA8_SSH_ARGV <<<"$out"
}

# --- doctor -----------------------------------------------------------------

# probe_ssh <host> <label> [required]
#
# `required` defaults to yes. An OPTIONAL host that is unreachable is reported
# and returns 0, because not every control node needs to reach every machine --
# the Proxmox host is not needed to provision the dev box. That distinction is
# a parameter rather than a `|| true` at the call site on purpose: masking the
# exit status swallows a genuine failure inside the function body along with
# the one case we meant to tolerate.
#
# This one takes a raw ssh destination rather than a fleet host, and the only
# caller left is the Proxmox hypervisor -- deliberately not in the declaration,
# because it is not a machine CI runs on (docs/CI_FLEET.md section 8). Every
# DECLARED host is probed by `fleet reach`, over its declared transport.
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
  for tool in "$MANAGED_BIN/ansible" "$MANAGED_BIN/ansible-playbook" ssh; do
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

A control node needs two things, and neither is a hand-copied ~/.ssh/config any
more -- every address is declared in infra/fleet.yml and every command here is
built from it (#526):

  1. ansible        `just setup-ansible` (uv-locked Python plus exact Galaxy collections)
  2. a key each declared host accepts for its declared login user

Then `just infra::setup` writes the git-ignored inventory, and
`just infra::ssh_config` names the machines in your ~/.ssh/config so `ssh
truenas` works too. A host still reported MISS above is one your key is not
authorised on, or one that is genuinely down -- not one you cannot resolve.
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
  [[ -x "$MANAGED_BIN/ansible-playbook" ]] ||
    die "locked ansible-playbook is absent. Run 'just setup' for the fix."
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

cmd_register_runner() {
  [[ $# -eq 2 ]] || die "register-runner needs a host and typed vars file"
  require_playbook_env
  fleet register-runner "$@"
}

cmd_register_hil() {
  [[ $# -eq 1 ]] || die "register-hil needs one typed vars file"
  require_playbook_env
  fleet register-hil "$@"
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
  if ! ssh_argv "${host}"; then
    printf '  %-12s UNDECLARED   (no such host in infra/fleet.yml)\n' "${label}"
    return 1
  fi
  # </dev/null for the same reason probe_ssh passes -n: never let a probe eat
  # this script's own stdin, which is how a remote `bash -s < infra.sh` feeds it.
  if ! out="$("${RA8_SSH_ARGV[@]}" "${probe}" </dev/null 2>&1)"; then
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

# shellcheck disable=SC2016  # remote command literals must expand on their target hosts.
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
  # Probe the context Ansible last staged and the venv it owns. The base
  # ~/ra8-firmware checkout is intentionally not updated because linked agent
  # worktrees depend on it; reading pins there compared a stale tree against
  # user-local tools and reported a convincing but unrelated result.
  # infra.sh runs without `set -e`, so this mask has exactly one effect: it is
  # the last command in cmd_status, and it keeps `infra.sh status` -- a report,
  # not a verdict -- exiting 0 when the dev box is unreachable or the parity
  # probe itself fails. The earlier probes are masked the same way for
  # symmetry, but only this one changes the command's exit status.
  status_host dev "parity" \
    'cd /var/lib/ra8-ci/build-context &&
      PATH=/opt/ra8-python-tools/bin:/usr/local/bin:/usr/bin:/bin \
        python3 scripts/checks/check_tool_versions.py --all 2>&1 | tail -1' || true # last command in cmd_status: a report must exit 0 even when the probe cannot run
}

usage() {
  cat <<EOF
usage: infra.sh <command> [args]

  list                what machines are declared, and how they are sized
  show <host>         inspect one host declaration and its derived values
  doctor              can THIS machine drive infra at all?
  ssh-config          name every declared machine in your ~/.ssh/config
  ssh-config-preview  print the generated fragment without installing it
  check <host>        dry run -- report what would change, change nothing
  apply <host>        converge that machine to infra/fleet.yml
  register-runner     first-register a declared Docker runner host
  register-hil        first-register the declared native HIL listener
  remove <host>       tear down (classes whose roles implement it)
  scale <host> <n>    live capacity change; shrinking DRAINS, never kills
  status              what is deployed across the estate, right now

HOST is a machine declared in infra/fleet.yml: $(host_names | tr '\n' ' ')
EOF
}

main() {
  local cmd="${1:-}"
  (($# == 0)) || shift
  case "${cmd}" in
    list) cmd_list ;;
    show) cmd_show "$@" ;;
    doctor) cmd_doctor ;;
    ssh-config) cmd_ssh_config ;;
    ssh-config-preview) cmd_ssh_config_preview ;;
    status) cmd_status ;;
    check)
      [ $# -ge 1 ] || die "check needs a host: $(host_names | tr '\n' ' ')"
      cmd_check "$@"
      ;;
    apply)
      [ $# -ge 1 ] || die "apply needs a host: $(host_names | tr '\n' ' ')"
      cmd_apply "$@"
      ;;
    register-runner) cmd_register_runner "$@" ;;
    register-hil) cmd_register_hil "$@" ;;
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
