#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# Install the exactly versioned Ansible Galaxy collections into this checkout.

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

  export BASH_ENV=/dev/null ENV=/dev/null PYTHONNOUSERSITE=1
  unset PYTHONHOME PYTHONPATH RA8_TOOL_VENV
  PATH=/usr/bin:/bin
  export PATH

  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
  REQUIREMENTS="$ROOT/infra/ansible/requirements.yml"
  COLLECTIONS_ROOT="$ROOT/.ansible/collections"
  PYTHON="$ROOT/.venv/bin/python3"
  ANSIBLE_GALAXY="$ROOT/.venv/bin/ansible-galaxy"
  SELFTEST_ROOT=""

  cleanup_selftest() {
    [[ -z "$SELFTEST_ROOT" ]] || rm -rf -- "$SELFTEST_ROOT"
  }

  validate_collection_target() {
    local root="$1" target="$2" collections_parent
    collections_parent="$root/.ansible"
    if [[ "$target" != "$root/.ansible/collections" ]]; then
      echo "ERROR: collections root escaped its repository-owned path" >&2
      return 1
    fi
    if [[ -L "$collections_parent" ]]; then
      echo "ERROR: refusing symlinked collections parent: $collections_parent" >&2
      return 1
    fi
    if [[ -L "$target" ]]; then
      echo "ERROR: refusing symlinked collections root: $target" >&2
      return 1
    fi
  }

  check_prerequisites() {
    local owner_root="$1" target="$2" requirements="$3" python="$4" galaxy="$5"
    [[ -x "$galaxy" ]] || {
      echo "ERROR: locked ansible-galaxy is absent; run 'just setup-python' first" >&2
      return 1
    }
    [[ -x "$python" ]] || {
      echo "ERROR: locked Python environment is absent; run 'just setup-python' first" >&2
      return 1
    }
    [[ -f "$requirements" ]] || {
      echo "ERROR: Ansible collection lock is absent: $requirements" >&2
      return 1
    }
    validate_collection_target "$owner_root" "$target"
  }

  verify_root() {
    local candidate="$1" galaxy="$2" python="$3" checker_root="$4"
    ANSIBLE_COLLECTIONS_PATH="$candidate" \
      "$galaxy" collection list --format json |
      "$python" "$checker_root/scripts/checks/check_ansible_collections.py" \
        --stdin --root "$candidate"
  }

  setup_collections() {
    local owner_root="$1" target="$2" requirements="$3" python="$4" galaxy="$5"
    local checker_root="$6" parent staging backup
    check_prerequisites "$owner_root" "$target" "$requirements" "$python" "$galaxy"
    if [[ -d "$target" ]] &&
      verify_root "$target" "$galaxy" "$python" "$checker_root" >/dev/null 2>&1; then
      return 0
    fi

    parent="$(dirname "$target")"
    mkdir -p "$parent"
    staging="$(mktemp -d "${target}.staging.XXXXXX")"
    if ! ANSIBLE_COLLECTIONS_PATH="$staging" \
      "$galaxy" collection install \
      --requirements-file "$requirements" --collections-path "$staging"; then
      rm -rf "$staging"
      return 1
    fi
    if ! verify_root "$staging" "$galaxy" "$python" "$checker_root"; then
      rm -rf "$staging"
      return 1
    fi

    backup="${target}.backup.$$"
    [[ ! -e "$backup" ]] || {
      echo "ERROR: safe replacement backup already exists: $backup" >&2
      rm -rf "$staging"
      return 1
    }
    if [[ -e "$target" ]]; then
      mv "$target" "$backup"
    fi
    if ! mv "$staging" "$target"; then
      [[ ! -e "$backup" ]] || mv "$backup" "$target"
      return 1
    fi
    rm -rf "$backup"
    verify_root "$target" "$galaxy" "$python" "$checker_root"
  }

  write_fake_galaxy() {
    local fake="$1"
    cat >"$fake" <<'FAKE'
#!/usr/bin/env bash
set -euo pipefail
if [[ "$1 $2" == "collection install" ]]; then
  printf 'install
' >>"$FAKE_STATE"
  [[ "${FAKE_MODE:-exact}" != "fail" ]] || exit 5
  destination=""
  while [[ "$#" -gt 0 ]]; do
    if [[ "$1" == "--collections-path" ]]; then
      destination="$2"
      break
    fi
    shift
  done
  [[ -n "$destination" ]]
  printf 'exact
' >"$destination/.fake-inventory"
  exit 0
fi
	if [[ "$1 $2" == "collection list" ]]; then
		root="$ANSIBLE_COLLECTIONS_PATH"
		mode="$(cat "$root/.fake-inventory" 2>/dev/null || printf stale)"
		if [[ "$mode" == exact ]]; then
			[[ -n "${FAKE_INVENTORY_JSON:-}" ]]
			printf '{"%s/ansible_collections":%s}\n' "$root" "$FAKE_INVENTORY_JSON"
  else
    printf '{"%s/ansible_collections":{"stale.extra":{"version":"1.0.0"}}}
' "$root"
  fi
  exit 0
fi
exit 2
FAKE
    chmod 0755 "$fake"
  }

  die_selftest() {
    echo "setup_ansible.sh --selftest: $*" >&2
    return 1
  }

  selftest_public_boundary() {
    local test_root="$1" outside linked_parent
    outside="$test_root/outside-important"
    mkdir -p "$outside"
    printf 'preserve\n' >"$outside/sentinel"
    RA8_ANSIBLE_REQUIREMENTS="$outside/requirements.yml" \
      RA8_COLLECTIONS_ROOT="$outside" \
      RA8_ANSIBLE_PYTHON="$outside/python3" \
      RA8_ANSIBLE_GALAXY="$outside/ansible-galaxy" \
      /bin/bash -p "$ROOT/scripts/dev/setup_ansible.sh" --selftest-boundary
    [[ "$(cat "$outside/sentinel")" == preserve ]] ||
      die_selftest "a hostile public override changed an outside directory"
    linked_parent="$test_root/linked-parent"
    mkdir -p "$linked_parent"
    ln -s "$linked_parent" "$test_root/.ansible"
    if /bin/bash -p "$ROOT/scripts/dev/setup_ansible.sh" --selftest-target \
      "$test_root" "$test_root/.ansible/collections" >/dev/null 2>&1; then
      die_selftest "a symlinked collections parent passed target validation"
    fi
    rm "$test_root/.ansible"
  }

  selftest() {
    local test_root test_repo collections_root fake state old_mode fake_inventory
    local python
    SELFTEST_ROOT="$(mktemp -d)"
    test_root="$SELFTEST_ROOT"
    trap cleanup_selftest EXIT
    fake="$test_root/ansible-galaxy"
    state="$test_root/install-calls"
    write_fake_galaxy "$fake"
    selftest_public_boundary "$test_root"
    test_repo="$test_root/repo"
    collections_root="$test_repo/.ansible/collections"
    python="$(command -v python3)"
    fake_inventory="$(
      "$python" - "$ROOT" <<'PY'
import json
import sys

sys.path.insert(0, sys.argv[1] + "/scripts/checks")
from check_ansible_collections import expected_versions

print(json.dumps({name: {"version": version} for name, version in expected_versions().items()}))
PY
    )"
    mkdir -p "$collections_root"
    printf 'stale
' >"$collections_root/.fake-inventory"
    printf 'preserve-until-swap
' >"$collections_root/sentinel"

    FAKE_STATE="$state" FAKE_MODE=exact FAKE_INVENTORY_JSON="$fake_inventory" \
      setup_collections "$test_repo" "$collections_root" "$REQUIREMENTS" \
      "$python" "$fake" "$ROOT" >/dev/null
    [[ ! -e "$collections_root/sentinel" && "$(wc -l <"$state")" -eq 1 ]] ||
      die_selftest "stale inventory was not replaced exactly"
    FAKE_STATE="$state" FAKE_MODE=exact FAKE_INVENTORY_JSON="$fake_inventory" \
      setup_collections "$test_repo" "$collections_root" "$REQUIREMENTS" \
      "$python" "$fake" "$ROOT" >/dev/null
    [[ "$(wc -l <"$state")" -eq 1 ]] ||
      die_selftest "exact repeat reinstalled collections"

    printf 'stale
' >"$collections_root/.fake-inventory"
    printf 'rollback
' >"$collections_root/sentinel"
    old_mode="$(cat "$collections_root/.fake-inventory")"
    if FAKE_STATE="$state" FAKE_MODE=fail FAKE_INVENTORY_JSON="$fake_inventory" \
      setup_collections "$test_repo" "$collections_root" "$REQUIREMENTS" \
      "$python" "$fake" "$ROOT" >/dev/null 2>&1; then
      die_selftest "failed staging install passed"
    fi
    [[ -f "$collections_root/sentinel" &&
      "$(cat "$collections_root/.fake-inventory")" == "$old_mode" ]] ||
      die_selftest "failed install did not preserve old state"
    "$python" "$ROOT/scripts/checks/check_ansible_collections.py" --selftest >/dev/null
    echo "setup_ansible.sh --selftest: PASS"
  }

  if [[ "${1:-}" == "--selftest" ]]; then
    selftest
    exit
  fi
  if [[ "${1:-}" == "--selftest-boundary" ]]; then
    [[ "$REQUIREMENTS" == "$ROOT/infra/ansible/requirements.yml" &&
      "$COLLECTIONS_ROOT" == "$ROOT/.ansible/collections" &&
      "$PYTHON" == "$ROOT/.venv/bin/python3" &&
      "$ANSIBLE_GALAXY" == "$ROOT/.venv/bin/ansible-galaxy" &&
      "$PATH" == "/usr/bin:/bin" && -z "${PYTHONHOME:-}" &&
      -z "${PYTHONPATH:-}" && -z "${RA8_TOOL_VENV:-}" ]] || exit 1
    exit 0
  fi
  if [[ "${1:-}" == "--selftest-target" && "$#" -eq 3 ]]; then
    validate_collection_target "$2" "$3"
    exit
  fi
  [[ "$#" -eq 0 ]] || {
    echo "usage: setup_ansible.sh [--selftest]" >&2
    exit 2
  }
  setup_collections "$ROOT" "$COLLECTIONS_ROOT" "$REQUIREMENTS" \
    "$PYTHON" "$ANSIBLE_GALAXY" "$ROOT"
  echo "==> Ansible collections ready: $COLLECTIONS_ROOT"
else
  [[ "$-" == *p* ]]
fi
