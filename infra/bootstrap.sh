#!/usr/bin/env bash
#
# infra/bootstrap.sh -- one-command setup for a fresh clone.
#
# Walks you from "git clone" to a deployable rig: checks prerequisites, writes
# your (git-ignored) inventory, and stores your GitHub token locally so nothing
# secret ever touches the repo. At the end it prints -- or runs -- the deploy.
#
#   bash infra/bootstrap.sh            # or:  make infra-setup
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ANSIBLE_DIR="${ROOT}/infra/ansible"
PRIVATE_DIR="${ANSIBLE_DIR}/private"
INVENTORY="${ANSIBLE_DIR}/inventory/hosts.ini"
SECRETS="${PRIVATE_DIR}/secrets.yml"

say() { printf '\n== %s ==\n' "$1"; }

say "ra8-firmware rig bootstrap"

# 1. Prerequisites -----------------------------------------------------------
missing=()
for cmd in ansible ansible-playbook ansible-galaxy ssh; do
  command -v "${cmd}" >/dev/null 2>&1 || missing+=("${cmd}")
done
if [ "${#missing[@]}" -gt 0 ]; then
  echo "  Missing: ${missing[*]}"
  echo "  Install Ansible first, e.g.:  pipx install ansible-core"
  echo "  (macOS: brew install ansible;  Debian/Ubuntu: apt install ansible)"
  exit 1
fi
echo "  ansible found; installing the required collections..."
ansible-galaxy collection install kubernetes.core community.hashi_vault >/dev/null 2>&1 || true

mkdir -p "${PRIVATE_DIR}"

# 2. Inventory (git-ignored, GENERATED) --------------------------------------
#
# Nothing is asked here any more, and nothing is copied from an example. The
# machines are declared once in infra/fleet.yml and the inventory is derived
# from it -- so adding a machine is adding a block there, and this file cannot
# describe a fleet the declaration does not.
say "Inventory"
python3 "${ROOT}/scripts/dev/fleet.py" inventory
echo "  generated from infra/fleet.yml; edit that file, not ${INVENTORY#"${ROOT}"/}"

# 3. SSH aliases (generated, outside the repo) -------------------------------
#
# The declaration carries every machine's real address, so nothing here NEEDS
# an alias -- but the docs, the runbooks and everyone's fingers say `ssh dev`
# and `ssh truenas`, and those names used to exist on exactly one laptop. This
# generates them from the same declaration, so a fresh control node has them
# too (#526).
say "SSH aliases"
python3 "${ROOT}/scripts/dev/fleet.py" ssh-config --install

# 4. Secrets (git-ignored, never committed) ----------------------------------
say "Secrets"
if [ -f "${SECRETS}" ]; then
  echo "  ${SECRETS#"${ROOT}"/} already exists -- leaving it."
else
  echo "  ARC needs a GitHub token to register runners."
  echo "  Create a fine-grained PAT with 'Administration: read/write' on the repo:"
  echo "    https://github.com/settings/personal-access-tokens"
  read -r -s -p "  Paste PAT (input hidden): " pat
  echo
  if [ -z "${pat}" ]; then
    echo "  No PAT entered -- skipping. Add it later to ${SECRETS#"${ROOT}"/}"
  else
    (
      umask 077
      printf 'ci_runner_github_pat: "%s"\n' "${pat}" >"${SECRETS}"
    )
    unset pat
    echo "  wrote ${SECRETS#"${ROOT}"/} (git-ignored, 0600)"
    echo "  (Prefer OpenBao? Put the token there and set ci_runner_github_pat"
    echo "   via a vault lookup in group_vars/all.yml -- see all.example.yml.)"
  fi
fi

# 5. Deploy ------------------------------------------------------------------
say "Ready"
deploy_cmd=(ansible-playbook -i inventory/hosts.ini playbooks/ci-runner.yml)
echo "  Deploy the CI runner pool with:"
echo "    cd infra/ansible && ${deploy_cmd[*]}"
echo
read -r -p "  Run it now? [y/N] " go
case "${go}" in
  [yY]*)
    cd "${ANSIBLE_DIR}"
    exec "${deploy_cmd[@]}"
    ;;
  *)
    echo "  Skipped. Run the command above when you are ready."
    ;;
esac
