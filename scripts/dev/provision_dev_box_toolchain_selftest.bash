#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
# Release-temp adversarial cases for the authenticated dev-box provisioner.

[[ "${BASH_SOURCE[0]}" != "$0" ]] || {
  printf "error: provisioner release selftest helper is source-only\n" >&2
  exit 1
}
[[ "$-" == *p* ]] || {
  printf 'error: provisioner release selftest helper requires privileged Bash mode\n' >&2
  return 1
}
PROVISION_RELEASE_PARENT_DIR="$(cd -P "$(dirname "${BASH_SOURCE[1]:-missing}")" 2>/dev/null && pwd)" || return 1
[[ -n "${PROVISION_RELEASE_SOURCE_DIR:-}" &&
  "$PROVISION_RELEASE_SOURCE_DIR" == "$PROVISION_RELEASE_PARENT_DIR" &&
  "${PROVISION_RELEASE_PARENT:-}" == "$PROVISION_RELEASE_PARENT_DIR/provision_dev_box_toolchain.sh" &&
  "${BASH_SOURCE[1]##*/}" == "provision_dev_box_toolchain.sh" ]] || {
  printf 'error: provisioner release selftest helper has an unauthorized parent\n' >&2
  return 1
}
unset -v PROVISION_RELEASE_PARENT PROVISION_RELEASE_SOURCE_DIR
unset -v PROVISION_RELEASE_PARENT_DIR

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

python_selftest_tmp_identity() {
  stat -Lc '%d:%i' -- "$1"
}

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

python_selftest_tmp_is_safe() {
  local parent="$1" parent_identity="$2" target="$3" identity="$4"
  python_selftest_suite_is_safe "$parent" "$parent_identity" &&
    [ "$target" = "$parent/work" ] && [ ! -L "$target" ] && [ -d "$target" ] &&
    [ "$(python_selftest_tmp_identity "$target")" = "$identity" ] &&
    [ "$(stat -Lc '%u:%a' -- "$target")" = "$(id -u):700" ]
}

python_selftest_tmp_cleanup() {
  local parent="$1" parent_identity="$2" target="$3" identity="$4"
  python_selftest_tmp_is_safe "$parent" "$parent_identity" "$target" "$identity" || return 1
  rm -rf -- "$target" || return 1
  [ ! -e "$target" ] && [ ! -L "$target" ]
}

python_selftest_suite_cleanup() {
  local target="$1" identity="$2"
  python_selftest_suite_is_safe "$target" "$identity" || return 1
  rm -rf -- "$target" || return 1
  [ ! -e "$target" ] && [ ! -L "$target" ]
}

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

python_selftest_allocation_signal() {
  local status="$1"
  trap - EXIT HUP INT TERM
  python_selftest_pending_cleanup "$suite" "$suite_identity" || exit 1
  exit "$status"
}

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

python_bytecode_invocation_selftest() {
  local suite="$1" suite_identity="$2" tmp="$3" identity="$4"
  local status expected_status case_name
  # shellcheck disable=SC2329  # python_bytecode_invocation_selftest calls this fixed stub indirectly.
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

release_selftest_loader_fixture() {
  local directory="$1"
  mkdir -m 0700 -- "$directory"
  printf ':\n' >"$directory/provision_dev_box_toolchain.sh"
  chmod 0755 "$directory/provision_dev_box_toolchain.sh"
  printf 'RA8_RELEASE_LOADER_MARKER=loaded\n' \
    >"$directory/provision_dev_box_toolchain_selftest.bash"
  chmod 0644 "$directory/provision_dev_box_toolchain_selftest.bash"
}

release_selftest_loader_case() (
  local root="$1" kind="$2" directory="$1/$2" fixture_dir="$1/$2"
  local expected_status="refused" status=0
  local main="$directory/provision_dev_box_toolchain.sh"
  local helper="$directory/provision_dev_box_toolchain_selftest.bash" digest
  if [[ "$kind" == "parent-symlink" ]]; then
    fixture_dir="$directory.real"
  fi
  release_selftest_loader_fixture "$fixture_dir" || return 1
  if [[ "$kind" == "parent-symlink" ]]; then
    ln -s "${fixture_dir##*/}" "$directory" || return 1
  fi
  digest="$(sha256sum "$helper")" || return 1
  digest="${digest%% *}"
  case "$kind" in
    safe) expected_status=0 ;;
    parent-symlink) : ;;
    main-symlink) mv "$main" "$main.saved" && ln -s "$main.saved" "$main" ;;
    main-hardlink) ln "$main" "$main.extra" ;;
    main-mode) chmod 0775 "$main" ;;
    helper-symlink) mv "$helper" "$helper.saved" && ln -s "$helper.saved" "$helper" ;;
    helper-hardlink) ln "$helper" "$helper.extra" ;;
    helper-mode) chmod 0664 "$helper" ;;
    digest) digest="0000000000000000000000000000000000000000000000000000000000000000" ;;
    inplace) printf 'RA8_RELEASE_LOADER_MARKER=forged\n' >>"$helper" ;;
    path-replace)
      expected_status=0
      # shellcheck disable=SC2329  # source_release_selftest_helper_from invokes this callback.
      release_selftest_open_checkpoint() {
        mv "$helper" "$helper.saved"
        printf 'RA8_RELEASE_LOADER_MARKER=forged\n' >"$helper"
        chmod 0644 "$helper"
      }
      ;;
    return-42)
      expected_status=42
      printf 'return 42\n' >"$helper"
      digest="$(sha256sum "$helper")" || return 1
      digest="${digest%% *}"
      ;;
    main-owner) chown 65534 "$main" ;;
    main-group) chgrp 1 "$main" ;;
    helper-owner) chown 65534 "$helper" ;;
    helper-group) chgrp 1 "$helper" ;;
    *) return 1 ;;
  esac
  unset RA8_RELEASE_LOADER_MARKER
  source_release_selftest_helper_from "$main" "$helper" "$directory" "$digest" || status=$?
  if [[ "$expected_status" == "0" ]]; then
    [[ "$status" == "0" && "$RA8_RELEASE_LOADER_MARKER" == "loaded" ]]
  elif [[ "$expected_status" == "42" ]]; then
    [[ "$status" == "42" && -z "${RA8_RELEASE_LOADER_MARKER:-}" ]]
  else
    [[ "$status" != "0" && -z "${RA8_RELEASE_LOADER_MARKER:-}" ]]
  fi
)

release_selftest_loader_refusals() {
  local root kind
  release_tmp_begin || return 1
  root="$RELEASE_TMP_DIR/loader"
  mkdir -m 0700 -- "$root"
  for kind in safe parent-symlink main-symlink main-hardlink main-mode \
    helper-symlink helper-hardlink helper-mode digest inplace path-replace return-42; do
    release_selftest_loader_case "$root" "$kind" || return 1
  done
  if [[ "$(id -u)" == "0" ]]; then
    for kind in main-owner main-group helper-owner helper-group; do
      release_selftest_loader_case "$root" "$kind" || return 1
    done
  fi
  release_tmp_cleanup_owned || return 1
  trap - EXIT HUP INT TERM
}

uv_cache_contract_selftest() {
  local scenario=current output status=0 output_file tmp
  local -a calls=()
  release_selftest_loader_refusals || return 1
  release_tmp_contract_selftest || return 1
  release_tmp_begin || return 1
  tmp="$RELEASE_TMP_DIR"
  output_file="$tmp/uv-cache-output"
  : >"$output_file"
  python_no_bytecode_residue_selftest || return 1

  # shellcheck disable=SC2329  # uv_cache_check_scenario_selftest calls this fixed stub indirectly.
  uv_bootstrap() {
    calls+=("$1")
    case "${scenario}" in
      current) return 0 ;;
      drift) return 2 ;;
      invalid) return 1 ;;
      *) return 99 ;;
    esac
  }

  uv_cache_check_scenario_selftest 0 "" current-path || return 1
  scenario=drift
  uv_cache_check_scenario_selftest \
    2 "  ...  uv-cache    restricted -> shared (planned)" drift-path || return 1
  scenario=invalid
  uv_cache_check_scenario_selftest \
    1 "error: authenticated uv cache check failed" invalid-path || return 1
  if [ -n "$(uv_cache_apply_report 1)" ] ||
    [ "$(uv_cache_apply_report 0)" != "  ...  uv-cache    restricted -> shared" ]; then
    echo "error: uv cache apply-report selftest failed" >&2
    return 1
  fi
  release_tmp_cleanup_owned || return 1
  trap - EXIT HUP INT TERM
  echo "provision_dev_box_toolchain.sh --selftest-uv-cache-contract: PASS"
}

# Download one immutable release asset and verify it before any parser or
# privileged install sees its bytes.
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
