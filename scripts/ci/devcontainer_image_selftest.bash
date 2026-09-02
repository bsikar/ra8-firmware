#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
# Test-only cleanup and adversarial lifecycle checks for devcontainer_image.sh.
# This file is sourced from that fixed, mode-checked entry point.

[[ "${BASH_SOURCE[0]}" != "$0" ]] || {
  printf "error: devcontainer image selftest helper is source-only\n" >&2
  exit 1
}
[[ "$-" == *p* ]] || {
  printf 'error: devcontainer image selftest helper requires privileged Bash mode\n' >&2
  return 1
}
SELFTEST_HELPER_PARENT="${DEVCONTAINER_SELFTEST_PARENT:-missing}"
SELFTEST_HELPER_PARENT_DIR="$(cd -P "$(dirname "$SELFTEST_HELPER_PARENT")" 2>/dev/null && pwd)" || return 1
[[ -n "${DEVCONTAINER_SELFTEST_SOURCE_DIR:-}" &&
  "$DEVCONTAINER_SELFTEST_SOURCE_DIR" == "$SELFTEST_HELPER_PARENT_DIR" &&
  "${DEVCONTAINER_SELFTEST_PARENT:-}" == "$SELFTEST_HELPER_PARENT_DIR/devcontainer_image.sh" &&
  "${BASH_SOURCE[1]:-missing}" -ef "$SELFTEST_HELPER_PARENT" ]] || {
  printf 'error: devcontainer image selftest helper has an unauthorized parent\n' >&2
  return 1
}
unset -v DEVCONTAINER_SELFTEST_PARENT DEVCONTAINER_SELFTEST_SOURCE_DIR
unset -v SELFTEST_HELPER_PARENT SELFTEST_HELPER_PARENT_DIR

unset -v DEVCONTAINER_SELFTEST_SCRIPT_DIR DEVCONTAINER_SELFTEST_REPO_ROOT
unset -v DEVCONTAINER_SELFTEST_LABEL_KEY

# The approved parent owns paths and pins; this lifecycle helper owns the
# bounded polling deadline consumed by the later source-only helpers.
SCRIPT_DIR="${SCRIPT_DIR:?}"
SELFTEST_SUPERVISOR_RAW_SHA256="${SELFTEST_SUPERVISOR_RAW_SHA256:?}"
SELFTEST_SUPERVISOR_CASES_RAW_SHA256="${SELFTEST_SUPERVISOR_CASES_RAW_SHA256:?}"
SELFTEST_SUPERVISOR_PROCESS_RAW_SHA256="${SELFTEST_SUPERVISOR_PROCESS_RAW_SHA256:?}"
SELFTEST_DEADLINE_STEPS=200
: "$SELFTEST_DEADLINE_STEPS"
SELFTEST_IMAGE_ENTRY="${SELFTEST_IMAGE_ENTRY-}"
SELFTEST_WORKER_PID="${SELFTEST_WORKER_PID-}"
SELFTEST_WORKER_PGID="${SELFTEST_WORKER_PGID-}"

export SELFTEST_COMMAND_COMPLETE=0
export SELFTEST_MAIN_COMPLETE=0
SELFTEST_TMP_DIR=""
SELFTEST_TMP_IDENTITY=""
SELFTEST_TMP_OWNER_SHELL_ID=""
SELFTEST_TMP_OWNER_UID=""
SELFTEST_TMP_ROOT=""
SELFTEST_TMP_ROOT_IDENTITY=""
SELFTEST_IMAGE_NAMESPACE=""
SELFTEST_IMAGE_TAGS=()
SELFTEST_TMP_ALLOCATION_PENDING=0
SELFTEST_ALLOCATION_CHECKPOINT_MODE=""
SELFTEST_ALLOCATION_CHECKPOINT_RECEIPT=""
NESTED_SELFTEST_PARENT=""
NESTED_SELFTEST_PARENT_IDENTITY=""
NESTED_SELFTEST_DIR=""
NESTED_SELFTEST_IDENTITY=""
SELFTEST_SUITE_ROOT=""
SELFTEST_SUITE_ROOT_IDENTITY=""
SELFTEST_SUITE_ROOT_OWNER_UID=""
SELFTEST_SUITE_ANCHOR=""
SELFTEST_SUITE_ANCHOR_IDENTITY=""
SELFTEST_SUITE_ANCHOR_OWNER_UID=""
SELFTEST_BOUND_EXIT_NESTED=0
SELFTEST_BOUND_EXIT_NESTED_RECEIPT=""
SELFTEST_SPAWN_CRITICAL=0
SELFTEST_PENDING_SIGNAL_STATUS=0
SELFTEST_SPAWN_HANDLER=""
SELFTEST_SPAWN_PROBE_STATUS=0
SELFTEST_BOUND_PROBE_CHILD=""

reset_selftest_tmp_state() {
  SELFTEST_TMP_DIR=""
  SELFTEST_TMP_IDENTITY=""
  SELFTEST_TMP_OWNER_SHELL_ID=""
  SELFTEST_TMP_OWNER_UID=""
  SELFTEST_TMP_ROOT=""
  SELFTEST_TMP_ROOT_IDENTITY=""
  SELFTEST_IMAGE_NAMESPACE=""
  SELFTEST_IMAGE_TAGS=()
  SELFTEST_TMP_ALLOCATION_PENDING=0
  SELFTEST_ALLOCATION_CHECKPOINT_MODE=""
  SELFTEST_ALLOCATION_CHECKPOINT_RECEIPT=""
}

configure_selftest_allocation_checkpoint() {
  SELFTEST_ALLOCATION_CHECKPOINT_MODE="$1"
  SELFTEST_ALLOCATION_CHECKPOINT_RECEIPT="$2"
}

selftest_tmp_allocation_checkpoint() {
  local phase="$1" mode="$SELFTEST_ALLOCATION_CHECKPOINT_MODE"
  local receipt="$SELFTEST_ALLOCATION_CHECKPOINT_RECEIPT"
  case "$mode" in
    "") return 0 ;;
    fail)
      [[ "$phase" == "created" ]] || return 0
      printf '%s\n' "$SELFTEST_TMP_DIR" >"$receipt"
      return 23
      ;;
    nonempty)
      [[ "$phase" == "created" ]] || return 0
      printf '%s\n' "$SELFTEST_TMP_DIR" >"$receipt"
      printf 'keep\n' >"$SELFTEST_TMP_DIR/sentinel"
      return 23
      ;;
    signal)
      [[ "$phase" == "created" ]] || return 0
      printf '%s\n' "$SELFTEST_TMP_DIR" >"$receipt"
      [[ "${SELFTEST_FRESH_SIGNAL_CHILD:-0}" == "1" && "${BASH_SUBSHELL:-0}" == "0" ]] ||
        return 1
      kill -TERM "$$"
      ;;
    pre-signal)
      [[ "$phase" == "precreate" ]] || return 0
      printf '%s\n' "$SELFTEST_TMP_DIR" >"$receipt"
      [[ "${SELFTEST_FRESH_SIGNAL_CHILD:-0}" == "1" && "${BASH_SUBSHELL:-0}" == "0" ]] ||
        return 1
      kill -TERM "$$"
      ;;
    *) return 1 ;;
  esac
}

set_selftest_runtime() {
  RUNTIME=("$1")
}

set_selftest_image_tracking() {
  SELFTEST_IMAGE_NAMESPACE="$1"
  shift
  SELFTEST_IMAGE_TAGS=("$@")
}

set_selftest_shell_identity() {
  local destination="$1" value="$$:${BASH_SUBSHELL:-0}"
  [[ "$value" =~ ^[0-9]+:[0-9]+$ ]] || return 1
  printf -v "$destination" '%s' "$value"
}

live_child_jobspec() {
  local destination="$1" candidate="$2" job child _state
  [[ "$candidate" =~ ^[0-9]+$ ]] || return 1
  while read -r job child _state; do
    if [[ "$child" == "$candidate" && "$job" =~ ^\[([0-9]+)\][+-]?$ ]]; then
      printf -v "$destination" '%%%s' "${BASH_REMATCH[1]}"
      return 0
    fi
  done < <(jobs -r -l)
  return 1
}

shell_owns_live_child() {
  local job_spec
  live_child_jobspec job_spec "$1" || return 1
  [[ -n "$job_spec" ]]
}

signal_owned_live_child() {
  local signal="$1" child="$2"
  shell_owns_live_child "$child" || return 1
  builtin kill -"$signal" "$child"
}

defer_selftest_spawn_signal() {
  local status="$1"
  [[ "$SELFTEST_SPAWN_CRITICAL" == "1" &&
    ("$status" == "129" || "$status" == "130" || "$status" == "143") ]] || return 1
  [[ "$SELFTEST_PENDING_SIGNAL_STATUS" == "0" ]] &&
    SELFTEST_PENDING_SIGNAL_STATUS="$status"
}

begin_selftest_spawn_critical() {
  local handler="$1"
  [[ "$SELFTEST_SPAWN_CRITICAL" == "0" && "$SELFTEST_PENDING_SIGNAL_STATUS" == "0" &&
    -z "$SELFTEST_SPAWN_HANDLER" ]] || return 1
  [[ "$handler" =~ ^[a-z_][a-z0-9_]*$ ]] && declare -F "$handler" >/dev/null ||
    return 1
  SELFTEST_SPAWN_HANDLER="$handler"
  SELFTEST_SPAWN_CRITICAL=1
  trap 'defer_selftest_spawn_signal 129' HUP
  trap 'defer_selftest_spawn_signal 130' INT
  trap 'defer_selftest_spawn_signal 143' TERM
}

finish_selftest_spawn_critical() {
  local destination="$1" handler="$SELFTEST_SPAWN_HANDLER" deferred
  [[ "$SELFTEST_SPAWN_CRITICAL" == "1" ]] || return 1
  deferred="$SELFTEST_PENDING_SIGNAL_STATUS"
  # shellcheck disable=SC2064  # the approved handler name is intentionally bound before the child exists.
  trap "$handler \$?" EXIT
  # shellcheck disable=SC2064  # the approved handler name is intentionally bound before the child exists.
  trap "$handler 129" HUP
  # shellcheck disable=SC2064  # the approved handler name is intentionally bound before the child exists.
  trap "$handler 130" INT
  # shellcheck disable=SC2064  # the approved handler name is intentionally bound before the child exists.
  trap "$handler 143" TERM
  SELFTEST_SPAWN_CRITICAL=0
  SELFTEST_PENDING_SIGNAL_STATUS=0
  SELFTEST_SPAWN_HANDLER=""
  printf -v "$destination" '%s' "$deferred"
  [[ "$deferred" == "0" ]] || "$handler" "$deferred"
}

selftest_spawn_probe_signal() {
  local status="$1" child_status
  trap - EXIT HUP INT TERM
  if [[ -n "$SELFTEST_BOUND_PROBE_CHILD" ]]; then
    signal_owned_live_child KILL "$SELFTEST_BOUND_PROBE_CHILD" || exit 1
    if wait "$SELFTEST_BOUND_PROBE_CHILD"; then child_status=0; else child_status=$?; fi
    [[ "$child_status" == "137" ]] || exit 1
    SELFTEST_BOUND_PROBE_CHILD=""
  fi
  restore_selftest_root_traps
  SELFTEST_SPAWN_PROBE_STATUS="$status"
  [[ "$status" != "1" ]] || exit 1
}

selftest_spawn_critical_directions() {
  local phase child pending
  for phase in pre-bind post-bind; do
    begin_selftest_spawn_critical selftest_spawn_probe_signal || return 1
    [[ "$phase" != "pre-bind" ]] || kill -TERM "$$"
    (
      trap '' HUP INT TERM
      while :; do sleep 1; done
    ) &
    child=$!
    SELFTEST_BOUND_PROBE_CHILD="$child"
    [[ "$phase" != "post-bind" ]] || kill -TERM "$$"
    SELFTEST_SPAWN_PROBE_STATUS=0
    finish_selftest_spawn_critical pending || return 1
    restore_selftest_root_traps
    [[ "$pending" == "143" && "$SELFTEST_SPAWN_PROBE_STATUS" == "143" ]] || return 1
    if shell_owns_live_child "$child"; then return 1; fi
  done
  return 0
}

selftest_root_metadata_directions() {
  local tmp="$1" probe="$1/root-metadata-probe" saved identity
  [[ "$(id -u)" == "0" ]] || return 0
  mkdir -m 1777 "$probe"
  printf 'keep\n' >"$probe/sentinel"
  identity="$(file_identity "$probe")"
  selftest_root_metadata_is_safe "$probe" "$identity" "$probe" || return 1
  chmod 0755 "$probe"
  selftest_root_metadata_is_safe "$probe" "$identity" "$probe" && return 1
  chmod 1777 "$probe"
  chown 1:1 "$probe"
  selftest_root_metadata_is_safe "$probe" "$identity" "$probe" && return 1
  chown 0:0 "$probe"
  saved="$probe.saved"
  mv "$probe" "$saved"
  ln -s "$saved" "$probe"
  selftest_root_metadata_is_safe "$probe" "$identity" "$probe" && return 1
  rm "$probe"
  mv "$saved" "$probe"
  [[ -f "$probe/sentinel" ]] || return 1
  rm "$probe/sentinel"
  rmdir "$probe"
}

assert_selftest_tmp_absent() {
  local path="$1" label="$2"
  [[ ! -e "$path" && ! -L "$path" ]] ||
    die "selftest: $label left its temporary directory"
}

selftest_controller_launcher_refusals() {
  local tmp="$1" mode launcher_mode case_dir managed controller
  for mode in malformed-ready ignore-timeout ps-failure-pre-bind; do
    case_dir="$tmp/controller-launcher-$mode"
    managed="$case_dir/managed"
    mkdir "$case_dir"
    create_managed_test_lock "$managed"
    launcher_mode="$mode"
    if [[ "$mode" == "ps-failure-pre-bind" ]]; then
      launcher_mode="ignore-timeout"
      SELFTEST_FORCE_CONTROLLER_PS_FAILURE=1
      export SELFTEST_FORCE_CONTROLLER_PS_FAILURE
    fi
    if start_signal_controller controller "$managed" "$case_dir" "" "$launcher_mode"; then
      die "selftest: $mode launcher receipt passed"
    fi
    unset SELFTEST_FORCE_CONTROLLER_PS_FAILURE
    [[ -z "$SELFTEST_BOUND_CONTROLLER" ]] || die "selftest: $mode launcher remained bound"
  done
}

selftest_controller_persisted_ps_failure() {
  local tmp="$1" controller case_dir="$1/controller-persisted-ps-failure"
  local managed="$case_dir/managed"
  mkdir "$case_dir"
  create_managed_test_lock "$managed"
  start_signal_controller controller "$managed" "$case_dir" || return 1
  wait_for_status_file "$case_dir/controller-ready.status" "$controller" || return 1
  SELFTEST_FORCE_CONTROLLER_PS_FAILURE=1
  export SELFTEST_FORCE_CONTROLLER_PS_FAILURE
  cleanup_bound_controller_launcher || return 1
  unset SELFTEST_FORCE_CONTROLLER_PS_FAILURE
  SELFTEST_CASE_DIR="$case_dir"
  SELFTEST_MANAGED_DIR="$managed"
  : "$SELFTEST_CASE_DIR" "$SELFTEST_MANAGED_DIR"
  read_worker_group_from_case "$case_dir" || return 1
  bounded_process_absent "$SELFTEST_WORKER_PID" &&
    bounded_group_gone "$SELFTEST_WORKER_PGID" && fresh_lock_probe
}

selftest_no_running_jobs() {
  [[ -z "$(jobs -r -p)" ]]
}

selftest_inject_bound_exit() {
  local kind="$1" child="$2" receipt="${RA8_SELFTEST_BOUND_EXIT_RECEIPT:-}"
  [[ "${RA8_SELFTEST_INJECT_BOUND_EXIT:-}" == "$kind" ]] || return 0
  [[ -n "$receipt" && -f "$receipt" && ! -L "$receipt" ]] || return 1
  printf '%s\n' "$child" >"$receipt" || return 1
  printf '%s\n' "${RA8_SELFTEST_BOUND_EXIT_UNSET:?injected bound-process exit}"
}

open_supervisor_cases_authority() {
  local cases="$1" identity="$2" owner="$3" group="$4" digest
  [[ -f "$cases" && ! -L "$cases" && "$(file_link_count "$cases")" == "1" &&
  "$(file_mode "$cases")" == "644" && "$(file_owner_id "$cases")" == "$owner" &&
  "$(file_group_id "$cases")" == "$group" ]] || return 1
  exec 7<"$cases" || return 1
  [[ "$(fd_identity 7)" == "$identity" ]] || {
    exec 7<&-
    return 1
  }
  digest="$(sha256_stdin <&7)"
  exec 7<&-
  [[ "$digest" == "$SELFTEST_SUPERVISOR_CASES_RAW_SHA256" ]] || return 1
  exec 8<"$cases" || return 1
  [[ "$(fd_identity 8)" == "$identity" ]] || {
    exec 8<&-
    return 1
  }
}

verify_supervisor_cases_authority() {
  local cases="$1" identity="$2" owner="$3" group="$4" digest
  [[ -f "$cases" && ! -L "$cases" && "$(file_link_count "$cases")" == "1" &&
  "$(file_mode "$cases")" == "644" && "$(file_owner_id "$cases")" == "$owner" &&
  "$(file_group_id "$cases")" == "$group" &&
  "$(file_identity "$cases")" == "$identity" && "$(fd_identity 8)" == "$identity" ]] ||
    return 1
  exec 9<"$cases" || return 1
  digest="$(sha256_stdin <&9)"
  exec 9<&-
  [[ "$digest" == "$SELFTEST_SUPERVISOR_CASES_RAW_SHA256" ]]
}

open_supervisor_process_authority() {
  local process="$1" identity="$2" owner="$3" group="$4" digest
  [[ -f "$process" && ! -L "$process" && "$(file_link_count "$process")" == "1" &&
  "$(file_mode "$process")" == "644" && "$(file_owner_id "$process")" == "$owner" &&
  "$(file_group_id "$process")" == "$group" ]] || return 1
  exec 10<"$process" || return 1
  [[ "$(fd_identity 10)" == "$identity" ]] || {
    exec 10<&-
    return 1
  }
  digest="$(sha256_stdin <&10)"
  exec 10<&-
  [[ "$digest" == "$SELFTEST_SUPERVISOR_PROCESS_RAW_SHA256" ]] || return 1
  exec 9<"$process" || return 1
  [[ "$(fd_identity 9)" == "$identity" ]] || {
    exec 9<&-
    return 1
  }
}

verify_supervisor_process_authority() {
  local process="$1" identity="$2" owner="$3" group="$4" digest
  [[ -f "$process" && ! -L "$process" && "$(file_link_count "$process")" == "1" &&
  "$(file_mode "$process")" == "644" && "$(file_owner_id "$process")" == "$owner" &&
  "$(file_group_id "$process")" == "$group" &&
  "$(file_identity "$process")" == "$identity" && "$(fd_identity 9)" == "$identity" ]] ||
    return 1
  exec 10<"$process" || return 1
  digest="$(sha256_stdin <&10)"
  exec 10<&-
  [[ "$digest" == "$SELFTEST_SUPERVISOR_PROCESS_RAW_SHA256" ]]
}

selftest_group_selection() {
  local tmp="$1" selected rejected
  selected="$(select_selftest_group_id "$tmp")" || return 1
  [[ "$selected" == "1" ]] || return 1
  for rejected in 0 not-a-gid 4294967296; do
    select_selftest_group_id "$tmp" "$rejected" >/dev/null 2>&1 && return 1
  done
  return 0
}

cleanup_selftest_images() {
  local tag cleanup_failed=0
  ((${#SELFTEST_IMAGE_TAGS[@]} == 0)) && return 0
  ((${#RUNTIME[@]} > 0)) || return 1
  [[ "$SELFTEST_IMAGE_NAMESPACE" =~ ^ra8-ci-selftest:([a-z0-9]{10}|[0-9a-f]{32})$ ]] || return 1
  for tag in "${SELFTEST_IMAGE_TAGS[@]}"; do
    [[ "$tag" == "${SELFTEST_IMAGE_NAMESPACE}-labelled" ||
      "$tag" == "${SELFTEST_IMAGE_NAMESPACE}-bare" ]] || return 1
    "${RUNTIME[@]}" rmi -f "$tag" >/dev/null 2>&1 || true
    temporary_image_absent "$tag" || cleanup_failed=1
  done
  ((cleanup_failed == 0)) || return 1
  SELFTEST_IMAGE_TAGS=()
}

temporary_image_absent() {
  local tag="$1"
  if "${RUNTIME[@]}" image inspect "$tag" >/dev/null 2>&1; then
    return 1
  fi
  "${RUNTIME[@]}" info >/dev/null 2>&1 || return 1
  ! "${RUNTIME[@]}" image inspect "$tag" >/dev/null 2>&1
}

selftest_root_metadata_is_safe() {
  local root="$1" identity="$2" expected_root="$3"
  [[ -n "$root" && -n "$identity" && "$root" == "$expected_root" &&
    ! -L "$root" && -d "$root" && "$(file_identity "$root")" == "$identity" ]] || return 1
  if [[ -n "$SELFTEST_SUITE_ROOT" && "$root" == "$SELFTEST_SUITE_ROOT" ]]; then
    [[ "$identity" == "$SELFTEST_SUITE_ROOT_IDENTITY" &&
      "$(file_owner_id "$root")" == "$SELFTEST_SUITE_ROOT_OWNER_UID" &&
      "$(file_mode "$root")" == "700" ]]
  else
    [[ "$(file_owner_id "$root")" == "0" && "$(file_special_mode "$root")" == "1777" ]]
  fi
}

selftest_suite_anchor_is_safe() {
  local anchor="$SELFTEST_SUITE_ANCHOR" canonical suffix
  canonical="$(cd -P /tmp && pwd)" || return 1
  suffix="${anchor#"$canonical/ra8-devcontainer-image-selftest."}"
  [[ -n "$anchor" &&
    "$anchor" == "$canonical/ra8-devcontainer-image-selftest.$suffix" &&
    "$suffix" =~ ^[0-9a-f]{32}$ && ! -L "$canonical" && -d "$canonical" &&
    "$(file_owner_id "$canonical")" == "0" &&
    "$(file_special_mode "$canonical")" == "1777" && ! -L "$anchor" && -d "$anchor" &&
    "$(file_identity "$anchor")" == "$SELFTEST_SUITE_ANCHOR_IDENTITY" &&
    "$(file_owner_id "$anchor")" == "$SELFTEST_SUITE_ANCHOR_OWNER_UID" &&
    "$SELFTEST_SUITE_ANCHOR_OWNER_UID" == "$(id -u)" &&
    "$(file_mode "$anchor")" == "700" ]]
}

selftest_suite_path_is_safe() {
  local suite="$1" identity="$2" suffix
  selftest_suite_anchor_is_safe || return 1
  [[ -n "$suite" && ! -L "$suite" && -d "$suite" &&
    "$(file_identity "$suite")" == "$identity" &&
    "$(file_owner_id "$suite")" == "$SELFTEST_SUITE_ANCHOR_OWNER_UID" &&
    "$(file_mode "$suite")" == "700" ]] || return 1
  [[ "$suite" == "$SELFTEST_SUITE_ANCHOR" ]] &&
    [[ "$identity" == "$SELFTEST_SUITE_ANCHOR_IDENTITY" ]] && return 0
  suffix="${suite#"$SELFTEST_SUITE_ANCHOR/ra8-devcontainer-image-selftest."}"
  [[ "$suite" == "$SELFTEST_SUITE_ANCHOR/ra8-devcontainer-image-selftest.$suffix" &&
    "$suffix" =~ ^[0-9a-f]{32}$ ]]
}

selftest_suite_root_is_safe() {
  [[ "$SELFTEST_SUITE_ROOT_OWNER_UID" == "$SELFTEST_SUITE_ANCHOR_OWNER_UID" ]] &&
    selftest_suite_path_is_safe "$SELFTEST_SUITE_ROOT" "$SELFTEST_SUITE_ROOT_IDENTITY"
}

configure_selftest_suite_authority() {
  SELFTEST_SUITE_ROOT="$1"
  SELFTEST_SUITE_ROOT_IDENTITY="$2"
  SELFTEST_SUITE_ROOT_OWNER_UID="$(file_owner_id "$1")" || return 1
  SELFTEST_SUITE_ANCHOR="$3"
  SELFTEST_SUITE_ANCHOR_IDENTITY="$4"
  SELFTEST_SUITE_ANCHOR_OWNER_UID="$5"
  selftest_suite_root_is_safe
}

configure_bound_exit_nested_suite() {
  local kind="${RA8_SELFTEST_INJECT_BOUND_EXIT:-}"
  local parent="${RA8_SELFTEST_NESTED_PARENT:-}"
  local identity="${RA8_SELFTEST_NESTED_PARENT_IDENTITY:-}"
  local receipt="${RA8_SELFTEST_NESTED_ROOT_RECEIPT:-}"
  if [[ -z "$kind" ]]; then
    [[ -z "$parent" && -z "$identity" && -z "$receipt" ]]
    return
  fi
  [[ "$kind" == "allocation" || "$kind" == "direct-child" ||
    "$kind" == "worker" || "$kind" == "controller" ]] || return 1
  configure_selftest_suite_authority "$parent" "$identity" "$parent" "$identity" \
    "$(id -u)" || return 1
  SELFTEST_BOUND_EXIT_NESTED=1
  SELFTEST_BOUND_EXIT_NESTED_RECEIPT="$receipt"
  [[ "$receipt" == "$parent/bound-exit-$kind.root" && ! -e "$receipt" && ! -L "$receipt" ]] ||
    return 1
  unset RA8_SELFTEST_NESTED_PARENT RA8_SELFTEST_NESTED_PARENT_IDENTITY \
    RA8_SELFTEST_NESTED_ROOT_RECEIPT
}

publish_bound_exit_nested_root() {
  local suffix
  [[ "$SELFTEST_BOUND_EXIT_NESTED" == "1" &&
    "$SELFTEST_TMP_ROOT" == "$SELFTEST_SUITE_ROOT" &&
    "$SELFTEST_TMP_ROOT_IDENTITY" == "$SELFTEST_SUITE_ROOT_IDENTITY" ]] || return 1
  suffix="${SELFTEST_TMP_DIR#"$SELFTEST_SUITE_ROOT/ra8-devcontainer-image-selftest."}"
  [[ "$SELFTEST_TMP_DIR" == "$SELFTEST_SUITE_ROOT/ra8-devcontainer-image-selftest.$suffix" &&
    "$suffix" =~ ^[0-9a-f]{32}$ && ! -L "$SELFTEST_TMP_DIR" && -d "$SELFTEST_TMP_DIR" &&
    "$(file_identity "$SELFTEST_TMP_DIR")" == "$SELFTEST_TMP_IDENTITY" &&
    "$(file_owner_id "$SELFTEST_TMP_DIR")" == "$(id -u)" &&
    "$(file_mode "$SELFTEST_TMP_DIR")" == "700" ]] || return 1
  (umask 077 && set -C && printf '%s\n' "$SELFTEST_TMP_DIR" >"$SELFTEST_BOUND_EXIT_NESTED_RECEIPT")
}

establish_selftest_suite_root() {
  [[ -z "$SELFTEST_SUITE_ROOT" && -n "$SELFTEST_TMP_DIR" &&
    "$SELFTEST_TMP_ROOT" == "$(cd -P /tmp && pwd)" ]] || return 1
  SELFTEST_SUITE_ROOT="$SELFTEST_TMP_DIR"
  SELFTEST_SUITE_ROOT_IDENTITY="$SELFTEST_TMP_IDENTITY"
  SELFTEST_SUITE_ROOT_OWNER_UID="$SELFTEST_TMP_OWNER_UID"
  SELFTEST_SUITE_ANCHOR="$SELFTEST_TMP_DIR"
  SELFTEST_SUITE_ANCHOR_IDENTITY="$SELFTEST_TMP_IDENTITY"
  SELFTEST_SUITE_ANCHOR_OWNER_UID="$SELFTEST_TMP_OWNER_UID"
  selftest_suite_root_is_safe
}

clear_selftest_suite_root() {
  local suite="$SELFTEST_SUITE_ROOT"
  [[ -n "$suite" && ! -e "$suite" && ! -L "$suite" ]] || return 1
  SELFTEST_SUITE_ROOT=""
  SELFTEST_SUITE_ROOT_IDENTITY=""
  SELFTEST_SUITE_ROOT_OWNER_UID=""
  SELFTEST_SUITE_ANCHOR=""
  SELFTEST_SUITE_ANCHOR_IDENTITY=""
  SELFTEST_SUITE_ANCHOR_OWNER_UID=""
}

nested_selftest_metadata_is_safe() {
  local parent="$1" parent_identity="$2" target="$3" target_identity="$4"
  local nested_suffix
  nested_suffix="${target#"$parent/ra8-devcontainer-image-selftest."}"
  selftest_suite_path_is_safe "$parent" "$parent_identity" &&
    [[ "$target" == "$parent/ra8-devcontainer-image-selftest.$nested_suffix" &&
      "$nested_suffix" =~ ^[[:alnum:]]{10}$ && ! -L "$target" && -d "$target" &&
      "$(file_identity "$target")" == "$target_identity" &&
      "$(file_owner_id "$target")" == "$(id -u)" && "$(file_mode "$target")" == "700" ]]
}

remove_nested_selftest_tmp() {
  nested_selftest_metadata_is_safe "$NESTED_SELFTEST_PARENT" \
    "$NESTED_SELFTEST_PARENT_IDENTITY" "$NESTED_SELFTEST_DIR" \
    "$NESTED_SELFTEST_IDENTITY" || return 1
  rm -rf -- "$NESTED_SELFTEST_DIR" || return 1
  [[ ! -e "$NESTED_SELFTEST_DIR" && ! -L "$NESTED_SELFTEST_DIR" ]] || return 1
  NESTED_SELFTEST_PARENT=""
  NESTED_SELFTEST_PARENT_IDENTITY=""
  NESTED_SELFTEST_DIR=""
  NESTED_SELFTEST_IDENTITY=""
}

configure_nested_selftest_tmp() {
  local parent="$1" parent_identity="$2" target="$3" target_identity="$4"
  nested_selftest_metadata_is_safe "$parent" "$parent_identity" "$target" \
    "$target_identity" || return 1
  NESTED_SELFTEST_PARENT="$parent"
  NESTED_SELFTEST_PARENT_IDENTITY="$parent_identity"
  NESTED_SELFTEST_DIR="$target"
  NESTED_SELFTEST_IDENTITY="$target_identity"
  SELFTEST_IMAGE_NAMESPACE="ra8-ci-selftest:$(printf '%s' "${target##*.}" | tr '[:upper:]' '[:lower:]')"
}

nested_selftest_signal() {
  local status="$1" cleanup_failed=0
  trap - EXIT HUP INT TERM
  cleanup_selftest_images || cleanup_failed=1
  remove_nested_selftest_tmp || cleanup_failed=1
  ((cleanup_failed == 0)) || exit 1
  exit "$status"
}

install_nested_selftest_traps() {
  trap 'nested_selftest_signal 129' HUP
  trap 'nested_selftest_signal 130' INT
  trap 'nested_selftest_signal 143' TERM
}

cleanup_pending_selftest_tmp() {
  local status="$1" current_shell="$2" target="$SELFTEST_TMP_DIR"
  local owner_shell="$SELFTEST_TMP_OWNER_SHELL_ID" owner_uid="$SELFTEST_TMP_OWNER_UID"
  local root="$SELFTEST_TMP_ROOT" root_identity="$SELFTEST_TMP_ROOT_IDENTITY" suffix
  [[ "$current_shell" == "$owner_shell" && "$owner_uid" =~ ^[0-9]+$ &&
    "${#SELFTEST_IMAGE_TAGS[@]}" == "0" ]] || return 1
  selftest_root_metadata_is_safe "$root" "$root_identity" "$root" || return 1
  if [[ -n "$target" ]]; then
    suffix="${target#"$root/ra8-devcontainer-image-selftest."}"
    [[ "$target" == "$root/ra8-devcontainer-image-selftest.$suffix" &&
      "$suffix" =~ ^[0-9a-f]{32}$ ]] || return 1
    if [[ -e "$target" || -L "$target" ]]; then
      [[ ! -L "$target" && -d "$target" &&
        "$(file_owner_id "$target")" == "$owner_uid" &&
        "$(file_mode "$target")" == "700" ]] || return 1
      rmdir -- "$target" 2>/dev/null || return 1
    fi
    [[ ! -e "$target" && ! -L "$target" ]] || return 1
  fi
  reset_selftest_tmp_state
  return "$status"
}

cleanup_selftest_tmp() {
  local status=$? target="$SELFTEST_TMP_DIR" expected="$SELFTEST_TMP_IDENTITY"
  local owner_shell="$SELFTEST_TMP_OWNER_SHELL_ID" owner_uid="$SELFTEST_TMP_OWNER_UID"
  local root="$SELFTEST_TMP_ROOT" root_identity="$SELFTEST_TMP_ROOT_IDENTITY"
  local suffix current_shell cleanup_failed=0
  set_selftest_shell_identity current_shell
  [[ -z "$target" || "$current_shell" == "$owner_shell" ]] || return "$status"
  if [[ "$SELFTEST_TMP_ALLOCATION_PENDING" == "1" ]]; then
    cleanup_pending_selftest_tmp "$status" "$current_shell"
    return $?
  fi
  if [[ -z "$target" ]]; then
    if [[ -z "$expected" && -z "$owner_shell" && -z "$owner_uid" && -z "$root" &&
      -z "$root_identity" && -z "$SELFTEST_IMAGE_NAMESPACE" &&
      "${#SELFTEST_IMAGE_TAGS[@]}" == "0" ]]; then
      return "$status"
    fi
    echo "ERROR: selftest temporary-directory state is incomplete" >&2
    return 1
  fi
  suffix="${target#"$root/ra8-devcontainer-image-selftest."}"
  if ! selftest_root_metadata_is_safe "$root" "$root_identity" "$root" ||
    [[ -z "$root" || "$target" != "$root/ra8-devcontainer-image-selftest.$suffix" ||
      ! "$suffix" =~ ^[0-9a-f]{32}$ || -z "$expected" || -z "$owner_shell" ||
      ! "$owner_uid" =~ ^[0-9]+$ ||
      -L "$target" || ! -d "$target" || "$(file_identity "$target")" != "$expected" ||
      "$(file_owner_id "$target")" != "$owner_uid" || "$(file_mode "$target")" != "700" ]]; then
    echo "ERROR: selftest temporary directory changed before cleanup: $target" >&2
    return 1
  fi
  cleanup_selftest_images || cleanup_failed=1
  ((cleanup_failed == 0)) || return 1
  rm -rf -- "$target" || return 1
  [[ ! -e "$target" && ! -L "$target" ]] || return 1
  SELFTEST_TMP_DIR=""
  SELFTEST_TMP_IDENTITY=""
  SELFTEST_TMP_OWNER_SHELL_ID=""
  SELFTEST_TMP_OWNER_UID=""
  SELFTEST_TMP_ROOT=""
  SELFTEST_TMP_ROOT_IDENTITY=""
  SELFTEST_IMAGE_NAMESPACE=""
  return "$status"
}

selftest_root_signal() {
  local status="$1"
  trap - EXIT HUP INT TERM
  cleanup_selftest_tmp || exit 1
  exit "$status"
}

selftest_root_signal_child() {
  local receipt="$1" fake_runtime="$2" signal="$3" parent="$4" parent_identity="$5"
  local target="$6" target_identity="$7" anchor="$8" anchor_identity="$9"
  local anchor_owner_uid="${10}" fake_state tag
  [[ ("$signal" == "HUP" || "$signal" == "INT" || "$signal" == "TERM") &&
    "$receipt" == "$parent/selftest-cleanup-$signal" &&
    -f "$receipt" && ! -L "$receipt" ]] ||
    die "selftest: signal cleanup receipt path is unsafe"
  fake_state="$parent/fake-image-state.$signal"
  [[ "$fake_runtime" == "$parent/fake-image-runtime" &&
    -f "$fake_runtime" && ! -L "$fake_runtime" && -x "$fake_runtime" &&
    ! -e "$fake_state" && ! -L "$fake_state" ]] ||
    die "selftest: signal cleanup fake runtime is unsafe"
  configure_selftest_suite_authority "$parent" "$parent_identity" \
    "$anchor" "$anchor_identity" "$anchor_owner_uid" ||
    die "selftest: signal child suite authority is unsafe"
  configure_nested_selftest_tmp "$parent" "$parent_identity" "$target" \
    "$target_identity" || die "selftest: signal child private root is unsafe"
  install_nested_selftest_traps
  RUNTIME=("$fake_runtime")
  tag="${SELFTEST_IMAGE_NAMESPACE}-labelled"
  SELFTEST_IMAGE_TAGS+=("$tag")
  RA8_SELFTEST_FAKE_IMAGE_STATE="$fake_state"
  export RA8_SELFTEST_FAKE_IMAGE_STATE
  printf '%s\n' "$tag" >"$fake_state"
  [[ "${BASH_SUBSHELL:-0}" == "0" ]] || die "selftest: signal child is not a fresh process"
  kill -"$signal" "$$"
  die "selftest: $signal trap returned"
}

selftest_allocation_checkpoint_child() {
  local phase="$1" receipt="$2" parent="$3" parent_identity="$4"
  local anchor="$5" anchor_identity="$6" anchor_owner_uid="$7" mode
  [[ "${BASH_SUBSHELL:-0}" == "0" &&
    ("$phase" == "precreate" || "$phase" == "created") &&
    "$receipt" == "$parent/selftest-allocation-$phase-path" &&
    ! -e "$receipt" && ! -L "$receipt" ]] ||
    die "selftest: allocation signal child arguments are unsafe"
  if ! configure_selftest_suite_authority "$parent" "$parent_identity" \
    "$anchor" "$anchor_identity" "$anchor_owner_uid"; then
    die "selftest: allocation signal parent is unsafe"
  fi
  reset_selftest_tmp_state
  SELFTEST_FRESH_SIGNAL_CHILD=1
  mode=signal
  [[ "$phase" != "precreate" ]] || mode=pre-signal
  configure_selftest_allocation_checkpoint "$mode" "$receipt"
  begin_selftest_tmp
  die "selftest: allocation $phase signal trap returned"
}

install_selftest_root_traps() {
  trap cleanup_selftest_tmp EXIT
  trap 'selftest_root_signal 129' HUP
  trap 'selftest_root_signal 130' INT
  trap 'selftest_root_signal 143' TERM
}

restore_selftest_root_traps() {
  local current_shell
  set_selftest_shell_identity current_shell
  if [[ -n "$SELFTEST_TMP_DIR" && "$current_shell" == "$SELFTEST_TMP_OWNER_SHELL_ID" ]]; then
    install_selftest_root_traps
  else
    trap - EXIT HUP INT TERM
  fi
}

begin_selftest_tmp() {
  local suffix candidate attempt created=0
  [[ -z "$SELFTEST_TMP_DIR" && -z "$SELFTEST_TMP_IDENTITY" &&
    -z "$SELFTEST_TMP_OWNER_SHELL_ID" && -z "$SELFTEST_TMP_OWNER_UID" &&
    -z "$SELFTEST_TMP_ROOT" && -z "$SELFTEST_TMP_ROOT_IDENTITY" &&
    -z "$SELFTEST_IMAGE_NAMESPACE" && "${#SELFTEST_IMAGE_TAGS[@]}" == "0" &&
    "$SELFTEST_TMP_ALLOCATION_PENDING" == "0" ]] || return 1
  if [[ -n "$SELFTEST_SUITE_ROOT" ]]; then
    selftest_suite_root_is_safe || return 1
    SELFTEST_TMP_ROOT="$SELFTEST_SUITE_ROOT"
    SELFTEST_TMP_ROOT_IDENTITY="$SELFTEST_SUITE_ROOT_IDENTITY"
  else
    SELFTEST_TMP_ROOT="$(cd -P /tmp && pwd)" || return 1
    [[ "$SELFTEST_TMP_ROOT" == /* && ! -L "$SELFTEST_TMP_ROOT" &&
      -d "$SELFTEST_TMP_ROOT" && "$(file_owner_id "$SELFTEST_TMP_ROOT")" == "0" &&
      "$(file_special_mode "$SELFTEST_TMP_ROOT")" == "1777" ]] || return 1
    SELFTEST_TMP_ROOT_IDENTITY="$(file_identity "$SELFTEST_TMP_ROOT")" || return 1
  fi
  set_selftest_shell_identity SELFTEST_TMP_OWNER_SHELL_ID
  SELFTEST_TMP_OWNER_UID="$(id -u)"
  SELFTEST_TMP_ALLOCATION_PENDING=1
  install_selftest_root_traps
  for ((attempt = 0; attempt < 20; ++attempt)); do
    suffix="$(od -An -N16 -tx1 /dev/urandom | tr -d '[:space:]')" || return 1
    [[ "$suffix" =~ ^[0-9a-f]{32}$ ]] || return 1
    candidate="$SELFTEST_TMP_ROOT/ra8-devcontainer-image-selftest.$suffix"
    [[ ! -e "$candidate" && ! -L "$candidate" ]] || continue
    SELFTEST_TMP_DIR="$candidate"
    selftest_tmp_allocation_checkpoint precreate || return $?
    if (umask 077 && mkdir -m 0700 -- "$candidate"); then
      created=1
      break
    fi
    SELFTEST_TMP_DIR=""
  done
  [[ "$created" == "1" ]] || return 1
  selftest_tmp_allocation_checkpoint created || return $?
  SELFTEST_TMP_IDENTITY="$(file_identity "$SELFTEST_TMP_DIR")"
  SELFTEST_IMAGE_NAMESPACE="ra8-ci-selftest:$(printf '%s' "${SELFTEST_TMP_DIR##*.}" | tr '[:upper:]' '[:lower:]')"
  SELFTEST_TMP_ALLOCATION_PENDING=0
  [[ "$SELFTEST_TMP_DIR" == "$SELFTEST_TMP_ROOT"/ra8-devcontainer-image-selftest.* &&
    ! -L "$SELFTEST_TMP_DIR" && -d "$SELFTEST_TMP_DIR" &&
    "$(file_owner_id "$SELFTEST_TMP_DIR")" == "$SELFTEST_TMP_OWNER_UID" &&
    "$(file_mode "$SELFTEST_TMP_DIR")" == "700" ]]
}

finish_selftest_tmp() {
  local cleanup_status restore_status
  if cleanup_selftest_tmp; then
    cleanup_status=0
  else
    cleanup_status=$?
    return "$cleanup_status"
  fi
  if restore_selftest_root_traps; then restore_status=0; else restore_status=$?; fi
  return "$restore_status"
}
