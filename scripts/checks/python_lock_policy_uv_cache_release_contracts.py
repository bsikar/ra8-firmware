# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Reviewed release/runtime fixtures for the uv cache policy checker."""

from __future__ import annotations


def _provisioner_action_expected_bodies() -> dict[str, str]:
    """Return reviewed provisioner check/apply action bodies."""
    return {
        "uv_bootstrap": r"""
  uv_bootstrap() {
    /usr/bin/python3 -B -I -S "${ROOT}/scripts/dev/bootstrap_uv.py" \
      --cache-root "${UV_CACHE_ROOT}" "$@"
  }
""",
        "uv_bootstrap_apply": r"""
  uv_bootstrap_apply() {
    as_root /usr/bin/python3 -B -I -S "${ROOT}/scripts/dev/bootstrap_uv.py" \
      --cache-root "${UV_CACHE_ROOT}" "$@"
  }
""",
        "uv_bootstrap_apply_run": r"""
  uv_bootstrap_apply_run() {
    local project_environment="$1"
    shift
    if [ -n "${project_environment}" ]; then
      as_root env UV_PROJECT_ENVIRONMENT="${project_environment}" UV_PYTHON_DOWNLOADS=never \
        /usr/bin/python3 -B -I -S "${ROOT}/scripts/dev/bootstrap_uv.py" \
        --cache-root "${UV_CACHE_ROOT}" --run "$@"
    else
      as_root env UV_PYTHON_DOWNLOADS=never \
        /usr/bin/python3 -B -I -S "${ROOT}/scripts/dev/bootstrap_uv.py" \
        --cache-root "${UV_CACHE_ROOT}" --run "$@"
    fi
  }
""",
        "uv_cache_modes_current": r"""
  uv_cache_modes_current() {
    uv_bootstrap --check-cache-modes >/dev/null 2>&1
  }
""",
        "uv_cache_check": r"""
  uv_cache_check() {
    local status=0
    if uv_bootstrap --verify-cache >/dev/null; then
      return 0
    else
      status=$?
    fi
    if [ "${status}" -eq 2 ]; then
      printf '  ...  %-11s %s -> %s\n' uv-cache restricted 'shared (planned)'
      return 2
    fi
    echo "error: authenticated uv cache check failed" >&2
    return "${status}"
  }
""",
    }


PYTHON_BYTECODE_NEGATIVE_CONTROL_CONTRACT = r"""
  assert_python_bytecode_negative_control() {
    local tmp="$1" helper
    RA8_BYTECODE_STATUS=0 /usr/bin/python3 -I -S "${tmp}/scripts/dev/bootstrap_uv.py"
    helper="$(find "${tmp}/scripts/dev/__pycache__" -type f \
      -name 'residue_helper*.pyc' -print -quit 2>/dev/null || true)"
    if [ -z "${helper}" ]; then
      echo "error: bytecode-residue negative control did not create helper pyc" >&2
      return 1
    fi
  }
"""


PYTHON_SELFTEST_TMP_CONTRACTS = {
    "python_selftest_tmp_identity": r"""
  python_selftest_tmp_identity() {
    stat -Lc '%d:%i' -- "$1"
  }
""",
    "python_selftest_suite_is_safe": r"""
  python_selftest_suite_is_safe() {
    local target="$1" identity="$2" suffix root
    root="$(cd -P /tmp && pwd)" || return 1
    suffix="${target#"$root/ra8-python-bytecode-selftest."}"
    [ "$root" = /tmp ] && [ ! -L "$root" ] && [ -d "$root" ] &&
      [ "$(stat -Lc '%u:%a' -- "$root")" = "0:1777" ] &&
      [ "$target" = "$root/ra8-python-bytecode-selftest.$suffix" ] &&
      [[ "$suffix" =~ ^[0-9a-f]{32}$ ]] && [ ! -L "$target" ] &&
      [ -d "$target" ] && [ "$(python_selftest_tmp_identity "$target")" = "$identity" ] &&
      [ "$(stat -Lc '%u:%a' -- "$target")" = "$(id -u):700" ]
  }
""",
    "python_selftest_tmp_is_safe": r"""
  python_selftest_tmp_is_safe() {
    local parent="$1" parent_identity="$2" target="$3" identity="$4"
    python_selftest_suite_is_safe "$parent" "$parent_identity" &&
      [ "$target" = "$parent/work" ] && [ ! -L "$target" ] && [ -d "$target" ] &&
      [ "$(python_selftest_tmp_identity "$target")" = "$identity" ] &&
      [ "$(stat -Lc '%u:%a' -- "$target")" = "$(id -u):700" ]
  }
""",
    "python_selftest_tmp_cleanup": r"""
  python_selftest_tmp_cleanup() {
    local parent="$1" parent_identity="$2" target="$3" identity="$4"
    python_selftest_tmp_is_safe "$parent" "$parent_identity" "$target" "$identity" || return 1
    rm -rf -- "$target" || return 1
    [ ! -e "$target" ] && [ ! -L "$target" ]
  }
""",
    "python_selftest_suite_cleanup": r"""
  python_selftest_suite_cleanup() {
    local target="$1" identity="$2"
    python_selftest_suite_is_safe "$target" "$identity" || return 1
    rm -rf -- "$target" || return 1
    [ ! -e "$target" ] && [ ! -L "$target" ]
  }
""",
    "python_selftest_pending_cleanup": r"""
  python_selftest_pending_cleanup() {
    local target="$1" identity="$2" suffix root
    if [ -n "$identity" ]; then
      python_selftest_suite_cleanup "$target" "$identity"
      return
    fi
    root="$(cd -P /tmp && pwd)" || return 1
    suffix="${target#"$root/ra8-python-bytecode-selftest."}"
    [ "$root" = /tmp ] && [ "$(stat -Lc '%u:%a' -- "$root")" = "0:1777" ] &&
      [ "$target" = "$root/ra8-python-bytecode-selftest.$suffix" ] &&
      [[ "$suffix" =~ ^[0-9a-f]{32}$ ]] || return 1
    if [ -e "$target" ] || [ -L "$target" ]; then
      [ ! -L "$target" ] && [ -d "$target" ] &&
        [ "$(stat -Lc '%u:%a' -- "$target")" = "$(id -u):700" ] || return 1
      rmdir -- "$target" || return 1
    fi
    [ ! -e "$target" ] && [ ! -L "$target" ]
  }
""",
    "python_selftest_suite_candidate": r"""
  python_selftest_suite_candidate() {
    local destination="$1" suffix candidate attempt
    for ((attempt = 0; attempt < 20; ++attempt)); do
      suffix="$(od -An -N16 -tx1 /dev/urandom | tr -d '[:space:]')" || return 1
      [[ "$suffix" =~ ^[0-9a-f]{32}$ ]] || return 1
      candidate="/tmp/ra8-python-bytecode-selftest.$suffix"
      if [ ! -e "$candidate" ] && [ ! -L "$candidate" ]; then
        printf -v "$destination" '%s' "$candidate"
        return 0
      fi
    done
    return 1
  }
""",
    "python_selftest_allocation_signal": r"""
  python_selftest_allocation_signal() {
    local status="$1"
    trap - EXIT HUP INT TERM
    python_selftest_pending_cleanup "$suite" "$suite_identity" || exit 1
    exit "$status"
  }
""",
    "python_selftest_allocation_signal_child": r"""
  python_selftest_allocation_signal_child() {
    local phase="$1" suite="$2" suite_identity="" root suffix
    [[ "${BASH_SUBSHELL:-0}" == "0" &&
      ("$phase" == "precreate" || "$phase" == "created") ]] || return 1
    root="$(cd -P /tmp && pwd)" || return 1
    suffix="${suite#"$root/ra8-python-bytecode-selftest."}"
    [[ "$root" == /* && ! -L "$root" && "$(stat -Lc '%u:%a' -- "$root")" == "0:1777" &&
      "$suite" == "$root/ra8-python-bytecode-selftest.$suffix" &&
      "$suffix" =~ ^[0-9a-f]{32}$ && ! -e "$suite" && ! -L "$suite" ]] || return 1
    trap 'python_selftest_pending_cleanup "${suite}" "${suite_identity}"' EXIT
    trap 'python_selftest_allocation_signal 143' TERM
    [[ "$phase" != "precreate" ]] || kill -TERM "$$"
    (umask 077 && mkdir -m 0700 -- "$suite") || return 1
    [[ "$phase" != "created" ]] || kill -TERM "$$"
    return 99
  }
""",
    "python_selftest_allocation_signal_case": r"""
  python_selftest_allocation_signal_case() {
    local phase="$1" suite="" suite_identity="" status
    python_selftest_suite_candidate suite || return 1
    if /bin/bash -p -- "$0" --selftest-python-allocation-signal "$phase" "$suite"; then
      status=0
    else
      status=$?
    fi
    [[ "$status" == "143" ]] || return 1
    [[ ! -e "$suite" && ! -L "$suite" ]]
  }
""",
    "python_selftest_replacement_refusal": r"""
  python_selftest_replacement_refusal() {
    local parent="$1" parent_identity="$2" target="$3" old_identity="$4" saved
    saved="$parent/original"
    python_selftest_tmp_is_safe "$parent" "$parent_identity" "$target" "$old_identity" || return 1
    mv -- "$target" "$saved"
    mkdir -m 0700 -- "$target"
    if python_selftest_tmp_cleanup "$parent" "$parent_identity" "$target" "$old_identity"; then
      echo "error: replaced bytecode selftest root passed cleanup binding" >&2
      return 1
    fi
    rmdir -- "$target"
    mv -- "$saved" "$target"
    python_selftest_tmp_is_safe "$parent" "$parent_identity" "$target" "$old_identity"
  }
""",
}


def python_bytecode_invocation_contract() -> str:
    """Return the reviewed bytecode invocation selftest body."""
    return r"""
  python_bytecode_invocation_selftest() {
    local suite="$1" suite_identity="$2" tmp="$3" identity="$4"
    local status expected_status case_name
    as_root() { "$@"; }
    for case_name in bootstrap apply run-empty run-project; do
      for expected_status in 0 37; do
        python_selftest_tmp_is_safe "$suite" "$suite_identity" "$tmp" "$identity" || return 1
        rm -rf -- "${tmp}/scripts/dev/__pycache__"
        status=0
        case "${case_name}" in
        bootstrap)
          set +e
          RA8_BYTECODE_STATUS="${expected_status}" ROOT="${tmp}" \
            UV_CACHE_ROOT="${tmp}/cache" uv_bootstrap --probe
          status=$?
          set -e
          ;;
        apply)
          set +e
          RA8_BYTECODE_STATUS="${expected_status}" ROOT="${tmp}" \
            UV_CACHE_ROOT="${tmp}/cache" uv_bootstrap_apply --probe
          status=$?
          set -e
          ;;
        run-empty)
          set +e
          RA8_BYTECODE_STATUS="${expected_status}" ROOT="${tmp}" \
            UV_CACHE_ROOT="${tmp}/cache" \
            uv_bootstrap_apply_run "" --probe
          status=$?
          set -e
          ;;
        run-project)
          set +e
          RA8_BYTECODE_STATUS="${expected_status}" ROOT="${tmp}" \
            UV_CACHE_ROOT="${tmp}/cache" \
            uv_bootstrap_apply_run "${tmp}/project-env" --probe
          status=$?
          set -e
          ;;
        esac
        if [ "${status}" -ne "${expected_status}" ] ||
          find "${tmp}/scripts/dev" \( -type d -name __pycache__ -o \
            -type f \( -name '*.pyc' -o -name '*.pyo' \) \) -print -quit | grep -q .; then
          echo "error: ${case_name}/${expected_status} bytecode-residue selftest failed" >&2
          return 1
        fi
      done
    done
  }
"""


PYTHON_NO_BYTECODE_RESIDUE_CONTRACT = r"""
  python_no_bytecode_residue_selftest() {
    (
      local suite="" suite_identity="" tmp identity status expected_status case_name
      python_selftest_allocation_signal_case precreate || return 1
      python_selftest_allocation_signal_case created || return 1
      python_selftest_suite_candidate suite || return 1
      trap 'python_selftest_pending_cleanup "${suite}" "${suite_identity}"' EXIT
      (umask 077 && mkdir -m 0700 -- "$suite") || return 1
      suite_identity="$(python_selftest_tmp_identity "$suite")"
      python_selftest_suite_is_safe "$suite" "$suite_identity" || return 1
      tmp="$suite/work"
      mkdir -m 0700 -- "$tmp"
      identity="$(python_selftest_tmp_identity "$tmp")"
      python_selftest_tmp_is_safe "$suite" "$suite_identity" "$tmp" "$identity" || return 1
      python_selftest_replacement_refusal "$suite" "$suite_identity" "$tmp" "$identity"
      mkdir -p "${tmp}/scripts/dev"
      printf 'VALUE = 1\n' >"${tmp}/scripts/dev/residue_helper.py"
      cat >"${tmp}/scripts/dev/bootstrap_uv.py" <<'PY'
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import residue_helper

raise SystemExit(int(os.environ.get("RA8_BYTECODE_STATUS", "0")))
PY
      python_bytecode_invocation_selftest "$suite" "$suite_identity" "$tmp" "$identity" ||
        return 1
      assert_python_bytecode_negative_control "${tmp}"
      python_selftest_tmp_cleanup "$suite" "$suite_identity" "$tmp" "$identity" || return 1
      python_selftest_suite_cleanup "$suite" "$suite_identity" || return 1
      trap - EXIT
    )
  }
"""


def _release_tmp_root_contract() -> str:
    """Return one reviewed release-tool temporary-directory contract part."""
    return r"""
  release_tmp_reset() {
    RELEASE_TMP_DIR=""
    RELEASE_TMP_IDENTITY=""
    RELEASE_TMP_OWNER_UID=""
    RELEASE_TMP_ROOT=""
    RELEASE_TMP_ROOT_IDENTITY=""
    RELEASE_TMP_ALLOCATION_PENDING=0
  }

  release_tmp_identity() {
    stat -Lc '%d:%i' -- "$1"
  }

  release_tmp_root_is_safe() {
    local root="$1" identity="$2" canonical
    canonical="$(cd -P /tmp && pwd)" || return 1
    [[ "$root" == "$canonical" && "$root" == /* && ! -L "$root" && -d "$root" &&
      "$(release_tmp_identity "$root")" == "$identity" &&
      "$(stat -Lc '%u:%a' -- "$root")" == "0:1777" ]]
  }

  release_tmp_is_safe() {
    local suffix
    suffix="${RELEASE_TMP_DIR#"$RELEASE_TMP_ROOT/ra8-tool-install."}"
    release_tmp_root_is_safe "$RELEASE_TMP_ROOT" "$RELEASE_TMP_ROOT_IDENTITY" &&
      [[ "$RELEASE_TMP_DIR" == "$RELEASE_TMP_ROOT/ra8-tool-install.$suffix" &&
        "$suffix" =~ ^[0-9a-f]{32}$ && "$RELEASE_TMP_OWNER_UID" =~ ^[0-9]+$ &&
        ! -L "$RELEASE_TMP_DIR" && -d "$RELEASE_TMP_DIR" &&
        "$(release_tmp_identity "$RELEASE_TMP_DIR")" == "$RELEASE_TMP_IDENTITY" &&
        "$(stat -Lc '%u:%a' -- "$RELEASE_TMP_DIR")" == "$RELEASE_TMP_OWNER_UID:700" ]]
  }
"""


def _release_tmp_cleanup_contract() -> str:
    """Return one reviewed release-tool temporary-directory contract part."""
    return r"""
  release_tmp_pending_cleanup() {
    local suffix
    suffix="${RELEASE_TMP_DIR#"$RELEASE_TMP_ROOT/ra8-tool-install."}"
    [[ "$RELEASE_TMP_ALLOCATION_PENDING" == "1" && -z "$RELEASE_TMP_IDENTITY" &&
      "$RELEASE_TMP_OWNER_UID" =~ ^[0-9]+$ ]] &&
      release_tmp_root_is_safe "$RELEASE_TMP_ROOT" "$RELEASE_TMP_ROOT_IDENTITY" &&
      [[ "$RELEASE_TMP_DIR" == "$RELEASE_TMP_ROOT/ra8-tool-install.$suffix" &&
        "$suffix" =~ ^[0-9a-f]{32}$ ]] || return 1
    if [[ -e "$RELEASE_TMP_DIR" || -L "$RELEASE_TMP_DIR" ]]; then
      [[ ! -L "$RELEASE_TMP_DIR" && -d "$RELEASE_TMP_DIR" &&
        "$(stat -Lc '%u:%a' -- "$RELEASE_TMP_DIR")" == "$RELEASE_TMP_OWNER_UID:700" ]] ||
        return 1
      rmdir -- "$RELEASE_TMP_DIR" || return 1
    fi
    [[ ! -e "$RELEASE_TMP_DIR" && ! -L "$RELEASE_TMP_DIR" ]] || return 1
    release_tmp_reset
  }

  release_tmp_cleanup_owned() {
    if [[ "$RELEASE_TMP_ALLOCATION_PENDING" == "1" ]]; then
      release_tmp_pending_cleanup
      return
    fi
    release_tmp_is_safe || return 1
    rm -rf -- "$RELEASE_TMP_DIR" || return 1
    [[ ! -e "$RELEASE_TMP_DIR" && ! -L "$RELEASE_TMP_DIR" ]] || return 1
    release_tmp_reset
  }

  release_tmp_exit() {
    local status=$?
    trap - EXIT HUP INT TERM
    release_tmp_cleanup_owned || exit 1
    exit "$status"
  }

  release_tmp_signal() {
    local status="$1"
    trap - EXIT HUP INT TERM
    release_tmp_cleanup_owned || exit 1
    exit "$status"
  }

  install_release_tmp_traps() {
    trap release_tmp_exit EXIT
    trap 'release_tmp_signal 129' HUP
    trap 'release_tmp_signal 130' INT
    trap 'release_tmp_signal 143' TERM
  }
"""


def _release_tmp_allocation_contract() -> str:
    """Return one reviewed release-tool temporary-directory contract part."""
    return r"""
  release_tmp_allocation_checkpoint() {
    local phase="$1"
    [[ -n "$RELEASE_TMP_CHECKPOINT_MODE" ]] || return 0
    [[ "$RELEASE_TMP_CHECKPOINT_MODE" == "precreate" ||
      "$RELEASE_TMP_CHECKPOINT_MODE" == "created" ]] || return 1
    [[ "$RELEASE_TMP_CHECKPOINT_MODE" == "$phase" ]] || return 0
    [[ "${BASH_SUBSHELL:-0}" == "0" ]] || return 1
    printf '%s\n' "$RELEASE_TMP_DIR"
    kill -TERM "$$"
    return 1
  }

  release_tmp_begin() {
    local suffix candidate attempt
    [[ -z "$RELEASE_TMP_DIR" && -z "$RELEASE_TMP_IDENTITY" &&
      -z "$RELEASE_TMP_OWNER_UID" && -z "$RELEASE_TMP_ROOT" &&
      -z "$RELEASE_TMP_ROOT_IDENTITY" && "$RELEASE_TMP_ALLOCATION_PENDING" == "0" ]] ||
      return 1
    RELEASE_TMP_ROOT="$(cd -P /tmp && pwd)" || return 1
    RELEASE_TMP_ROOT_IDENTITY="$(release_tmp_identity "$RELEASE_TMP_ROOT")" || return 1
    RELEASE_TMP_OWNER_UID="$(id -u)" || return 1
    release_tmp_root_is_safe "$RELEASE_TMP_ROOT" "$RELEASE_TMP_ROOT_IDENTITY" || return 1
    for ((attempt = 0; attempt < 20; ++attempt)); do
      suffix="$(od -An -N16 -tx1 /dev/urandom | tr -d '[:space:]')" || return 1
      [[ "$suffix" =~ ^[0-9a-f]{32}$ ]] || return 1
      candidate="$RELEASE_TMP_ROOT/ra8-tool-install.$suffix"
      if [[ ! -e "$candidate" && ! -L "$candidate" ]]; then
        RELEASE_TMP_DIR="$candidate"
        break
      fi
    done
    [[ -n "$RELEASE_TMP_DIR" ]] || return 1
    RELEASE_TMP_ALLOCATION_PENDING=1
    install_release_tmp_traps
    release_tmp_allocation_checkpoint precreate
    (umask 077 && mkdir -m 0700 -- "$RELEASE_TMP_DIR") || return 1
    release_tmp_allocation_checkpoint created
    RELEASE_TMP_IDENTITY="$(release_tmp_identity "$RELEASE_TMP_DIR")" || return 1
    release_tmp_is_safe || return 1
    RELEASE_TMP_ALLOCATION_PENDING=0
  }
"""


def _release_tmp_signal_contract() -> str:
    """Return one reviewed release-tool temporary-directory contract part."""
    return r"""
  release_tmp_path_is_absent() {
    local target="$1" root suffix identity
    root="$(cd -P /tmp && pwd)" || return 1
    identity="$(release_tmp_identity "$root")" || return 1
    suffix="${target#"$root/ra8-tool-install."}"
    release_tmp_root_is_safe "$root" "$identity" &&
      [[ "$target" == "$root/ra8-tool-install.$suffix" &&
        "$suffix" =~ ^[0-9a-f]{32}$ && ! -e "$target" && ! -L "$target" ]]
  }

  release_tmp_signal_case() {
    local phase="$1" output status
    if output="$(/bin/bash -p -- "$0" --selftest-release-tmp-signal "$phase")"; then
      status=0
    else
      status=$?
    fi
    [[ "$status" == "143" && "$output" != *$'\n'* ]] || return 1
    release_tmp_path_is_absent "$output"
  }

  release_tmp_signal_child() {
    local phase="$1"
    [[ "${BASH_SUBSHELL:-0}" == "0" &&
      ("$phase" == "precreate" || "$phase" == "created") ]] || return 1
    RELEASE_TMP_CHECKPOINT_MODE="$phase"
    : "$RELEASE_TMP_CHECKPOINT_MODE"
    release_tmp_begin
    return 99
  }
"""


def _release_tmp_refusal_contract() -> str:
    """Return one reviewed release-tool temporary-directory contract part."""
    return r"""
  release_tmp_replacement_refusal() {
    local kind="$1" original="$RELEASE_TMP_DIR" saved refused preserved=0
    local identity="$RELEASE_TMP_IDENTITY" owner="$RELEASE_TMP_OWNER_UID"
    local root="$RELEASE_TMP_ROOT" root_identity="$RELEASE_TMP_ROOT_IDENTITY"
    saved="$original.saved"
    release_tmp_is_safe && [[ ! -e "$saved" && ! -L "$saved" ]] || return 1
    mv -- "$original" "$saved" || return 1
    case "$kind" in
    directory) mkdir -m 0700 -- "$original" || return 1 ;;
    symlink) ln -s -- "$saved" "$original" || return 1 ;;
    *) return 1 ;;
    esac
    if release_tmp_cleanup_owned; then refused=0; else refused=1; fi
    case "$kind" in
    directory)
      [[ ! -L "$original" && -d "$original" ]] && preserved=1
      rmdir -- "$original" || return 1
      ;;
    symlink)
      [[ -L "$original" && "$(readlink -- "$original")" == "$saved" ]] && preserved=1
      rm -- "$original" || return 1
      ;;
    esac
    mv -- "$saved" "$original" || return 1
    RELEASE_TMP_DIR="$original"
    RELEASE_TMP_IDENTITY="$identity"
    RELEASE_TMP_OWNER_UID="$owner"
    RELEASE_TMP_ROOT="$root"
    RELEASE_TMP_ROOT_IDENTITY="$root_identity"
    RELEASE_TMP_ALLOCATION_PENDING=0
    : "$RELEASE_TMP_ALLOCATION_PENDING"
    release_tmp_is_safe && [[ "$refused" == "1" && "$preserved" == "1" ]]
  }

  release_tmp_wrong_owner_refusal() {
    local path refused
    [[ "$(id -u)" == "0" ]] || return 0
    release_tmp_begin || return 1
    path="$RELEASE_TMP_DIR"
    chown 65534 "$path" || return 1
    if release_tmp_cleanup_owned; then refused=0; else refused=1; fi
    chown 0 "$path" || return 1
    release_tmp_is_safe || return 1
    release_tmp_cleanup_owned || return 1
    trap - EXIT HUP INT TERM
    release_tmp_path_is_absent "$path" && [[ "$refused" == "1" ]]
  }
"""


def _release_tmp_selftest_contract() -> str:
    """Return one reviewed release-tool temporary-directory contract part."""
    return r"""
  release_tmp_contract_selftest() {
    local tmp original_identity original_path refused
    release_tmp_wrong_owner_refusal || return 1
    release_tmp_signal_case precreate || return 1
    release_tmp_signal_case created || return 1
    TMPDIR="/definitely-not-a-release-temp-parent"
    export TMPDIR
    release_tmp_begin || return 1
    original_path="$RELEASE_TMP_DIR"
    original_identity="$RELEASE_TMP_IDENTITY"
    [[ "$original_path" != "$TMPDIR/"* ]] || return 1
    release_tmp_replacement_refusal directory || return 1
    release_tmp_replacement_refusal symlink || return 1

    RELEASE_TMP_IDENTITY="0:0"
    if release_tmp_cleanup_owned; then refused=0; else refused=1; fi
    RELEASE_TMP_IDENTITY="$original_identity"
    [[ "$refused" == "1" ]] || return 1

    RELEASE_TMP_DIR="$original_path/forged"
    if release_tmp_cleanup_owned; then refused=0; else refused=1; fi
    RELEASE_TMP_DIR="$original_path"
    [[ "$refused" == "1" ]] || return 1

    chmod 0755 "$original_path"
    if release_tmp_cleanup_owned; then refused=0; else refused=1; fi
    chmod 0700 "$original_path"
    [[ "$refused" == "1" ]] || return 1

    unset TMPDIR
    tmp="$original_path"
    release_tmp_cleanup_owned || return 1
    trap - EXIT HUP INT TERM
    release_tmp_path_is_absent "$tmp"
  }
"""


def provisioner_release_tmp_contract() -> str:
    """Return the reviewed release-tool temporary-directory lifecycle."""
    return (
        f"{_release_tmp_root_contract()}\n\n"
        f"{_release_tmp_cleanup_contract()}\n\n"
        f"{_release_tmp_allocation_contract()}\n\n"
        f"{_release_tmp_signal_contract()}\n\n"
        f"{_release_tmp_refusal_contract()}\n\n"
        f"{_release_tmp_selftest_contract()}"
    )


def _provisioner_report_expected_bodies() -> dict[str, str]:
    """Return reviewed reporting and contract-test bodies."""
    return {
        "uv_cache_apply_report": r"""
  uv_cache_apply_report() {
    local modes_current="$1"
    if [ "${modes_current}" -eq 0 ]; then
      printf '  ...  %-11s %s -> %s\n' uv-cache restricted shared
    fi
  }
""",
        "uv_cache_check_scenario_selftest": r"""
  uv_cache_check_scenario_selftest() {
    local expected_status="$1" expected_output="$2" message="$3"
    status=0
    calls=()
    if uv_cache_check >"${output_file}" 2>&1; then
      status=0
    else
      status=$?
    fi
    output="$(<"${output_file}")"
    if [ "${status}" -ne "${expected_status}" ] ||
      [ "${output}" != "${expected_output}" ] ||
      [ "${calls[*]}" != "--verify-cache" ]; then
      echo "error: uv cache check ${message} selftest failed" >&2
      return 1
    fi
  }
""",
        "assert_python_bytecode_negative_control": PYTHON_BYTECODE_NEGATIVE_CONTROL_CONTRACT,
        **PYTHON_SELFTEST_TMP_CONTRACTS,
        "python_bytecode_invocation_selftest": python_bytecode_invocation_contract(),
        "python_no_bytecode_residue_selftest": PYTHON_NO_BYTECODE_RESIDUE_CONTRACT,
    }


def provisioner_expected_bodies() -> dict[str, str]:
    """Return every reviewed provisioner semantic body."""
    return {
        **_provisioner_action_expected_bodies(),
        **_provisioner_report_expected_bodies(),
    }


def provisioner_dispatch() -> tuple[str, ...]:
    """Return the exact production selftest dispatch sequence."""
    return (
        'if [ "${1:-}" = "--selftest-release-tmp-signal" ]; then',
        '[[ "$#" == "2" ]] || exit 1',
        "load_release_selftest_helper || exit 1",
        'release_tmp_signal_child "$2"',
        'elif [ "${1:-}" = "--selftest-python-allocation-signal" ]; then',
        '[[ "$#" == "3" ]] || exit 1',
        "load_release_selftest_helper || exit 1",
        'python_selftest_allocation_signal_child "$2" "$3"',
        'elif [ "${1:-}" = "--selftest-uv-cache-contract" ]; then',
        "load_release_selftest_helper || exit 1",
        "uv_cache_contract_selftest",
        "else",
        'main "$@"',
        "fi",
    )


def provisioner_mutations() -> tuple[tuple[str, str], ...]:
    """Return load-bearing mutations for the provisioner contract."""
    return _provisioner_action_mutations() + _python_selftest_mutations() + _release_tmp_mutations()


def _provisioner_action_mutations() -> tuple[tuple[str, str], ...]:
    """Return release-action and reporting mutations."""
    return (
        (
            "uv_bootstrap() {\n    /usr/bin/python3 -B -I -S",
            "uv_bootstrap() {\n    /usr/bin/python3 -I -S",
        ),
        (
            "uv_bootstrap_apply() {\n    as_root /usr/bin/python3 -B -I -S",
            "uv_bootstrap_apply() {\n    as_root /usr/bin/python3 -I -S",
        ),
        (
            'if [ -n "${project_environment}" ]; then\n'
            '      as_root env UV_PROJECT_ENVIRONMENT="${project_environment}" '
            "UV_PYTHON_DOWNLOADS=never \\\n"
            "        /usr/bin/python3 -B -I -S",
            'if [ -n "${project_environment}" ]; then\n'
            '      as_root env UV_PROJECT_ENVIRONMENT="${project_environment}" '
            "UV_PYTHON_DOWNLOADS=never \\\n"
            "        /usr/bin/python3 -I -S",
        ),
        (
            "    else\n      as_root env UV_PYTHON_DOWNLOADS=never \\\n"
            "        /usr/bin/python3 -B -I -S",
            "    else\n      as_root env UV_PYTHON_DOWNLOADS=never \\\n"
            "        /usr/bin/python3 -I -S",
        ),
        ("uv_bootstrap --verify-cache >/dev/null", "uv_bootstrap --ensure >/dev/null"),
        ("uv_bootstrap_apply --ensure >/dev/null", "uv_bootstrap --ensure >/dev/null"),
        (
            'uv_bootstrap_apply_run "" --no-config pip check',
            "uv_bootstrap_apply --no-config pip check",
        ),
        ('if [ "${modes_current}" -eq 0 ]; then', 'if [ "${modes_current}" -eq 1 ]; then'),
        ('uv_cache_apply_report "${cache_modes_current}"', ": # report removed"),
        ("if uv_cache_check; then", "if uv_bootstrap --ensure; then"),
        ("uv_cache_contract_selftest\n", ": # selftest removed\n"),
    )


def _release_tmp_mutations() -> tuple[tuple[str, str], ...]:
    """Return load-bearing privileged temporary-root mutations."""
    return (
        (
            "unset PYTHONHOME PYTHONPATH RA8_TOOL_VENV TMPDIR",
            "unset PYTHONHOME PYTHONPATH RA8_TOOL_VENV",
        ),
        (
            'release_tmp_root_is_safe() {\n    local root="$1" identity="$2" canonical',
            'release_tmp_root_is_safe() {\n    local root="$1" identity="$2" canonical\n'
            "    return 0",
        ),
        (
            '"$(release_tmp_identity "$RELEASE_TMP_DIR")" == "$RELEASE_TMP_IDENTITY"',
            "true",
        ),
        (
            'rmdir -- "$RELEASE_TMP_DIR" || return 1',
            'rm -rf -- "$RELEASE_TMP_DIR" || return 1',
        ),
        ("    release_tmp_is_safe || return 1\n    rm -rf", "    true\n    rm -rf"),
        (
            "    release_tmp_allocation_checkpoint precreate\n"
            '    (umask 077 && mkdir -m 0700 -- "$RELEASE_TMP_DIR") || return 1\n'
            "    release_tmp_allocation_checkpoint created",
            '    mkdir -m 0700 -- "$RELEASE_TMP_DIR"',
        ),
        (
            '    TMPDIR="/definitely-not-a-release-temp-parent"',
            '    TMPDIR="$RELEASE_TMP_ROOT"',
        ),
        (
            "    release_tmp_signal_case precreate || return 1\n"
            "    release_tmp_signal_case created || return 1",
            "    true",
        ),
        (
            "    release_tmp_wrong_owner_refusal || return 1",
            "    true",
        ),
        (
            "    release_tmp_replacement_refusal directory || return 1\n"
            "    release_tmp_replacement_refusal symlink || return 1",
            "    true",
        ),
        (
            '[[ -L "$original" && "$(readlink -- "$original")" == "$saved" ]] && preserved=1',
            "preserved=1",
        ),
        ("    release_tmp_contract_selftest || return 1", "    true"),
    )


def _python_selftest_mutations() -> tuple[tuple[str, str], ...]:
    """Return safe-allocation and residue-proof mutations."""
    return (
        (
            "      python_selftest_suite_candidate suite || return 1\n"
            '      trap \'python_selftest_pending_cleanup "${suite}" '
            '"${suite_identity}"\' EXIT\n'
            '      (umask 077 && mkdir -m 0700 -- "$suite") || return 1',
            '      suite="$(mktemp -d)"',
        ),
        (
            "python_selftest_tmp_identity() {\n    stat -Lc '%d:%i' -- \"$1\"",
            "python_selftest_tmp_identity() {\n    printf '0:0\\n'",
        ),
        (
            "      python_selftest_allocation_signal_case precreate || return 1\n"
            "      python_selftest_allocation_signal_case created || return 1",
            "      true",
        ),
        (
            'python_selftest_suite_is_safe "$target" "$identity" || return 1',
            "true",
        ),
        (
            'python_selftest_replacement_refusal "$suite" "$suite_identity" "$tmp" "$identity"',
            ": # replacement refusal removed",
        ),
        (
            '    mv -- "$target" "$saved"\n    mkdir -m 0700 -- "$target"',
            '    rmdir -- "$target"\n    mkdir -m 0700 -- "$target"',
        ),
        (
            'python_selftest_tmp_cleanup "$suite" "$suite_identity" "$tmp" "$identity" || return 1',
            'rm -rf -- "$tmp"',
        ),
    )
