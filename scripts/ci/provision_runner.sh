#!/usr/bin/env bash
#
# scripts/ci/provision_runner.sh -- bring a bare-metal self-hosted CI runner
# up to the pinned host toolchain, then prove it with the same check the
# `toolchain-parity` gate runs.
#
# WHY THIS EXISTS
# ---------------
# The CI workflows run every job directly on the self-hosted runner host (no
# `container:` key), so -- unlike `make ci`, which runs inside the pinned
# .devcontainer image -- the runner relies on whatever toolchain is installed
# on the host. When that host drifts from the Dockerfile pins, a gate can pass
# on a hand-provisioned dev box and fail on the runner: a missing g++-14 sank
# the coverage gate for hours, with shellcheck / shfmt / actionlint / hadolint
# absent behind it. This script reprovisions a runner to the pins so that
# divergence cannot happen, and is safe to re-run (idempotent).
#
# The pinned versions are read FROM .devcontainer/Dockerfile so this script and
# the container never disagree: bump a version there and re-run this, no edit
# here. The acceptance test at the end is check_tool_versions.py --all -- the
# exact assertion the `toolchain-parity` gate makes -- so a run that cannot
# reach parity fails loudly rather than reporting a false success.
#
# Usage:
#   sudo bash scripts/ci/provision_runner.sh          # install + verify
#   bash scripts/ci/provision_runner.sh --check-only  # verify only, no install
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DOCKERFILE="${ROOT}/.devcontainer/Dockerfile"
BIN_DIR="/usr/local/bin"

# Read `ARG <name>=<value>` from the Dockerfile. Fails loudly (non-empty guard
# by the caller) if the pin is absent, so a renamed ARG cannot silently skip a
# tool.
dockerfile_arg() {
  local name="$1" value
  value="$(sed -n "s/^ARG ${name}=\(.*\)$/\1/p" "${DOCKERFILE}" | head -1)"
  printf '%s' "${value}"
}

# sudo only when not already root, so the script works both as `sudo bash ...`
# and from an already-root provisioning context.
as_root() {
  if [ "$(id -u)" -eq 0 ]; then
    "$@"
  else
    sudo "$@"
  fi
}

install_shellcheck() {
  local version="$1"
  local url="https://github.com/koalaman/shellcheck/releases/download/v${version}/shellcheck-v${version}.linux.x86_64.tar.xz"
  local tmp
  tmp="$(mktemp -d)"
  curl -fsSL "${url}" | tar -xJ -C "${tmp}"
  as_root install -m 0755 "${tmp}/shellcheck-v${version}/shellcheck" "${BIN_DIR}/shellcheck"
  rm -rf "${tmp}"
}

install_shfmt() {
  local version="$1"
  as_root curl -fsSL -o "${BIN_DIR}/shfmt" \
    "https://github.com/mvdan/sh/releases/download/v${version}/shfmt_v${version}_linux_amd64"
  as_root chmod 0755 "${BIN_DIR}/shfmt"
}

install_actionlint() {
  local version="$1"
  local tmp
  tmp="$(mktemp -d)"
  curl -fsSL "https://github.com/rhysd/actionlint/releases/download/v${version}/actionlint_${version}_linux_amd64.tar.gz" |
    tar -xz -C "${tmp}" actionlint
  as_root install -m 0755 "${tmp}/actionlint" "${BIN_DIR}/actionlint"
  rm -rf "${tmp}"
}

install_hadolint() {
  local version="$1"
  as_root curl -fsSL -o "${BIN_DIR}/hadolint" \
    "https://github.com/hadolint/hadolint/releases/download/v${version}/hadolint-Linux-x86_64"
  as_root chmod 0755 "${BIN_DIR}/hadolint"
}

# Install a GitHub-release binary tool only when the wanted version is not
# already the one on PATH. $3 is a shell snippet that echoes the installed
# version (empty if absent), so the compare stays tool-specific.
ensure_release_tool() {
  local name="$1" want="$2" have="$3" installer="$4"
  if [ "${have}" = "${want}" ]; then
    printf '  ok   %-11s %s (pinned)\n' "${name}" "${want}"
    return 0
  fi
  printf '  ...  %-11s %s -> %s\n' "${name}" "${have:-absent}" "${want}"
  "${installer}" "${want}"
}

main() {
  local check_only=0
  [ "${1:-}" = "--check-only" ] && check_only=1

  [ -f "${DOCKERFILE}" ] || {
    echo "error: ${DOCKERFILE} not found -- run from a full checkout" >&2
    exit 1
  }

  local shellcheck_v shfmt_v actionlint_v hadolint_v ruff_v yamllint_v cmakelang_v
  shellcheck_v="$(dockerfile_arg SHELLCHECK_VERSION)"
  shfmt_v="$(dockerfile_arg SHFMT_VERSION)"
  actionlint_v="$(dockerfile_arg ACTIONLINT_VERSION)"
  hadolint_v="$(dockerfile_arg HADOLINT_VERSION)"
  ruff_v="$(dockerfile_arg RUFF_VERSION)"
  yamllint_v="$(dockerfile_arg YAMLLINT_VERSION)"
  cmakelang_v="$(dockerfile_arg CMAKELANG_VERSION)"

  for pair in "SHELLCHECK_VERSION=${shellcheck_v}" "SHFMT_VERSION=${shfmt_v}" \
    "ACTIONLINT_VERSION=${actionlint_v}" "HADOLINT_VERSION=${hadolint_v}" \
    "RUFF_VERSION=${ruff_v}" "YAMLLINT_VERSION=${yamllint_v}" \
    "CMAKELANG_VERSION=${cmakelang_v}"; do
    [ -n "${pair#*=}" ] || {
      echo "error: could not read ${pair%%=*} from the Dockerfile" >&2
      exit 1
    }
  done

  if [ "${check_only}" -eq 0 ]; then
    echo "provisioning runner toolchain from ${DOCKERFILE#"${ROOT}"/} pins:"

    # apt-managed compiler pins. gcc-14 and g++-14 MUST be installed as a pair:
    # the coverage build enable_language(CXX) needs the C++ half, and a lone
    # gcc-14 is exactly what broke it.
    as_root apt-get update -qq
    as_root apt-get install -y --no-install-recommends \
      gcc-14 g++-14 clang-format-22 clang-tools-18 >/dev/null

    # GitHub-release binaries, pinned to the Dockerfile versions.
    ensure_release_tool shellcheck "${shellcheck_v}" \
      "$(shellcheck --version 2>/dev/null | sed -n 's/^version: //p')" install_shellcheck
    ensure_release_tool shfmt "${shfmt_v}" \
      "$(shfmt --version 2>/dev/null | sed 's/^v//')" install_shfmt
    ensure_release_tool actionlint "${actionlint_v}" \
      "$(actionlint --version 2>/dev/null | head -1)" install_actionlint
    ensure_release_tool hadolint "${hadolint_v}" \
      "$(hadolint --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')" install_hadolint

    # pip-managed pins. --break-system-packages matches the Dockerfile on a
    # PEP-668 base image; the pins keep these exact.
    python3 -m pip install --quiet --break-system-packages \
      "ruff==${ruff_v}" "yamllint==${yamllint_v}" "cmakelang==${cmakelang_v}"
  fi

  echo "verifying parity (the check the toolchain-parity gate runs):"
  python3 "${ROOT}/scripts/checks/check_tool_versions.py" --all
}

main "$@"
