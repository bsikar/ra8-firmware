#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
# Descriptor-bound supervisor launch and exit-path regressions for devcontainer_image.sh.
# This file is sourced from that fixed, mode-checked entry point after the lifecycle helper.

[[ "${BASH_SOURCE[0]}" != "$0" ]] || {
  printf "error: devcontainer bound-exit selftest helper is source-only\n" >&2
  exit 1
}
[[ "$-" == *p* ]] || {
  printf 'error: devcontainer bound-exit selftest helper requires privileged Bash mode\n' >&2
  return 1
}
SELFTEST_BOUND_EXIT_PARENT="${DEVCONTAINER_SELFTEST_PARENT:-missing}"
SELFTEST_BOUND_EXIT_PARENT_DIR="$(cd -P "$(dirname "$SELFTEST_BOUND_EXIT_PARENT")" 2>/dev/null && pwd)" || return 1
[[ -n "${DEVCONTAINER_SELFTEST_SOURCE_DIR:-}" &&
  "$DEVCONTAINER_SELFTEST_SOURCE_DIR" == "$SELFTEST_BOUND_EXIT_PARENT_DIR" &&
  "${DEVCONTAINER_SELFTEST_PARENT:-}" == "$SELFTEST_BOUND_EXIT_PARENT_DIR/devcontainer_image.sh" &&
  "${BASH_SOURCE[1]:-missing}" -ef "$SELFTEST_BOUND_EXIT_PARENT" ]] || {
  printf 'error: devcontainer bound-exit selftest helper has an unauthorized parent\n' >&2
  return 1
}
unset -v DEVCONTAINER_SELFTEST_PARENT DEVCONTAINER_SELFTEST_SOURCE_DIR
unset -v SELFTEST_BOUND_EXIT_PARENT SELFTEST_BOUND_EXIT_PARENT_DIR
unset -v DEVCONTAINER_SELFTEST_SCRIPT_DIR DEVCONTAINER_SELFTEST_REPO_ROOT
unset -v DEVCONTAINER_SELFTEST_LABEL_KEY

SCRIPT_DIR="${SCRIPT_DIR:?}"
SELFTEST_IMAGE_ENTRY="${SELFTEST_IMAGE_ENTRY-}"
SELFTEST_TMP_IDENTITY="${SELFTEST_TMP_IDENTITY-}"
SELFTEST_SUPERVISOR_RAW_SHA256="${SELFTEST_SUPERVISOR_RAW_SHA256:?}"

run_bound_exit_supervisor() {
  local helper="$SCRIPT_DIR/devcontainer_image_selftest_supervisor.py"
  local cases="$SCRIPT_DIR/devcontainer_image_selftest_supervisor_cases.py"
  local process="$SCRIPT_DIR/devcontainer_image_selftest_process.py"
  local resolved_dir identity cases_identity process_identity digest program status
  local main_owner main_group caller_uid
  resolved_dir="$(cd -P "$(dirname "$helper")" && pwd)" || return 1
  main_owner="$(file_owner_id "$SELFTEST_IMAGE_ENTRY")"
  main_group="$(file_group_id "$SELFTEST_IMAGE_ENTRY")"
  caller_uid="$(id -u)"
  [[ "$resolved_dir" == "$SCRIPT_DIR" && -f "$helper" && ! -L "$helper" &&
    "$(file_link_count "$helper")" == "1" && "$(file_mode "$helper")" == "644" &&
    "$(file_owner_id "$helper")" == "$main_owner" &&
    "$(file_group_id "$helper")" == "$main_group" &&
    ("$main_owner" == "0" || "$main_owner" == "$caller_uid") &&
    ("$caller_uid" != "0" || ("$main_owner" == "0" && "$main_group" == "0")) ]] || return 1
  identity="$(file_identity "$helper")"
  exec 6<"$helper" || return 1
  [[ "$(fd_identity 6)" == "$identity" ]] || {
    exec 6<&-
    return 1
  }
  digest="$(sha256_stdin <&6)"
  [[ "$digest" == "$SELFTEST_SUPERVISOR_RAW_SHA256" ]] || {
    exec 6<&-
    return 1
  }
  cases_identity="$(file_identity "$cases")"
  open_supervisor_cases_authority "$cases" "$cases_identity" "$main_owner" "$main_group" || {
    exec 6<&-
    return 1
  }
  process_identity="$(file_identity "$process")"
  open_supervisor_process_authority "$process" "$process_identity" "$main_owner" "$main_group" || {
    exec 8<&- 6<&-
    return 1
  }
  program="/proc/self/fd/6"
  [[ -e "$program" ]] || program="/dev/fd/6"
  RA8_SELFTEST_BOUND_ENTRY="$1" \
    /usr/bin/python3 -B -I -S "$program" --process-fd 9 --cases-fd 8 "$@" || status=$?
  verify_supervisor_process_authority \
    "$process" "$process_identity" "$main_owner" "$main_group" || status=1
  verify_supervisor_cases_authority "$cases" "$cases_identity" "$main_owner" "$main_group" || status=1
  exec 9<&- 8<&-
  exec 6<&-
  return "${status:-0}"
}

write_supervisor_stall_fixture() {
  local destination="$1"
  [[ ! -e "$destination" && ! -L "$destination" ]] || return 1
  (umask 077 && printf '%s\n' \
    '#!/bin/bash' \
    "[[ \"\$1\" == \"--selftest\" ]] || exit 64" \
    "trap '' HUP INT TERM" \
    "exec -a \"\$0\" /bin/sleep 30" >"$destination") || return 1
  chmod 700 "$destination"
}

bound_exit_regression_error() {
  printf 'ERROR: selftest: bound-exit %s %s\n' "$1" "$2" >&2
  return 1
}

selftest_bound_exit_regressions() {
  local tmp="$1" kind receipt supervisor_bound outer_receipt status_receipt outer bound status
  local nested_receipt nested_root nested_suffix payload_status
  for kind in allocation direct-child worker controller; do
    receipt="$tmp/bound-exit-$kind.receipt"
    supervisor_bound="$tmp/bound-exit-$kind.supervisor"
    outer_receipt="$tmp/bound-exit-$kind.outer"
    status_receipt="$tmp/bound-exit-$kind.status"
    nested_receipt="$tmp/bound-exit-$kind.root"
    : >"$receipt"
    (umask 077 && : >"$supervisor_bound")
    [[ ! -e "$outer_receipt" && ! -L "$outer_receipt" &&
      ! -e "$status_receipt" && ! -L "$status_receipt" &&
      ! -e "$nested_receipt" && ! -L "$nested_receipt" ]] || return 1
    if RA8_SELFTEST_INJECT_BOUND_EXIT="$kind" RA8_SELFTEST_BOUND_EXIT_RECEIPT="$receipt" \
      RA8_SELFTEST_NESTED_PARENT="$tmp" \
      RA8_SELFTEST_NESTED_PARENT_IDENTITY="$SELFTEST_TMP_IDENTITY" \
      RA8_SELFTEST_NESTED_ROOT_RECEIPT="$nested_receipt" \
      run_bound_exit_supervisor "$SELFTEST_IMAGE_ENTRY" "$supervisor_bound" \
      "$outer_receipt" "$status_receipt" normal "$SELFTEST_TMP_IDENTITY"; then
      status=0
    else
      status=$?
    fi
    if [[ "$status" != "1" ]]; then
      payload_status=missing
      [[ ! -f "$status_receipt" || -L "$status_receipt" ]] ||
        IFS= read -r payload_status <"$status_receipt" || payload_status=malformed
      bound_exit_regression_error "$kind" \
        "returned status $status (payload $payload_status)" || return 1
    fi
    IFS= read -r nested_root <"$nested_receipt" ||
      bound_exit_regression_error "$kind" "did not publish its nested root" || return 1
    nested_suffix="${nested_root#"$tmp/ra8-devcontainer-image-selftest."}"
    [[ "$nested_root" == "$tmp/ra8-devcontainer-image-selftest.$nested_suffix" &&
      "$nested_suffix" =~ ^[0-9a-f]{32}$ && ! -e "$nested_root" && ! -L "$nested_root" ]] ||
      bound_exit_regression_error "$kind" "left an unsafe nested root" || return 1
    IFS= read -r outer <"$outer_receipt" ||
      bound_exit_regression_error "$kind" "did not publish its supervisor" || return 1
    IFS= read -r bound <"$receipt" ||
      bound_exit_regression_error "$kind" "did not publish its injected process" || return 1
    [[ "$outer" =~ ^[0-9]+$ && "$bound" =~ ^[0-9]+$ ]] ||
      bound_exit_regression_error "$kind" "published a malformed PID" || return 1
    bounded_process_absent "$outer" && bounded_process_absent "$bound" ||
      bound_exit_regression_error "$kind" "left a live process" || return 1
    bounded_group_gone "$outer" ||
      bound_exit_regression_error "$kind" "left its supervisor group" || return 1
    bounded_group_gone "$bound" ||
      bound_exit_regression_error "$kind" "left its injected process group" || return 1
    IFS= read -r bound <"$supervisor_bound" ||
      bound_exit_regression_error "$kind" "did not publish the descriptor-bound PID" || return 1
    [[ "$bound" == "$outer" ]] ||
      bound_exit_regression_error "$kind" "published inconsistent supervisor PIDs" || return 1
  done
}

selftest_descriptor_bound_entry() {
  local tmp="$1" program output status wrong_entry
  [[ -d /proc/self/fd ]] || return 0
  wrong_entry="$tmp/devcontainer_image.sh"
  [[ ! -e "$wrong_entry" && ! -L "$wrong_entry" ]] || return 1
  (umask 077 && : >"$wrong_entry") || return 1
  exec 11<"$SCRIPT_DIR/devcontainer_image.sh" || return 1
  program=/proc/self/fd/11
  if output="$(RA8_SELFTEST_BOUND_ENTRY="$wrong_entry" \
    /bin/bash -p -- "$program" --selftest-descendant 2>&1)"; then
    status=0
  else
    status=$?
  fi
  [[ "$status" == "1" &&
    "$output" == "ERROR: descriptor-bound selftest entry authority is unsafe" ]] || {
    exec 11<&-
    return 1
  }
  output="$(RA8_SELFTEST_BOUND_ENTRY="$SCRIPT_DIR/devcontainer_image.sh" \
    /bin/bash -p -- "$program" --selftest-descendant)" || {
    exec 11<&-
    return 1
  }
  exec 11<&-
  [[ "$output" == "child=0" ]]
}

run_bound_exit_supervisor_failure_modes() {
  local tmp="$1" stall_entry="$2" mode launch_mode bound outer status_receipt
  local victim result pid
  for mode in signal-pre-bind receipt-failure malformed-status nonascii-status hardlink-bound; do
    bound="$tmp/supervisor-$mode.bound"
    outer="$tmp/supervisor-$mode.outer"
    status_receipt="$tmp/supervisor-$mode.status"
    if [[ "$mode" == "hardlink-bound" ]]; then
      victim="$tmp/supervisor-$mode.victim"
      (umask 077 && printf 'preserve\n' >"$victim")
      ln "$victim" "$bound"
    else
      (umask 077 && : >"$bound")
    fi
    [[ "$mode" != "receipt-failure" ]] || : >"$outer"
    [[ "$mode" != "malformed-status" ]] || (umask 077 && printf 'malformed\n' >"$status_receipt")
    [[ "$mode" != "nonascii-status" ]] || (umask 077 && printf '\377\n' >"$status_receipt")
    launch_mode="normal"
    [[ "$mode" != "signal-pre-bind" ]] || launch_mode="$mode"
    if run_bound_exit_supervisor "$stall_entry" "$bound" "$outer" \
      "$status_receipt" "$launch_mode" "$SELFTEST_TMP_IDENTITY"; then result=0; else result=$?; fi
    if [[ "$mode" == "signal-pre-bind" ]]; then
      [[ "$result" == "143" ]] || {
        printf 'ERROR: selftest: supervisor mode %s returned %s\n' "$mode" "$result" >&2
        return 1
      }
    else
      [[ "$result" == "125" ]] || {
        printf 'ERROR: selftest: supervisor mode %s returned %s\n' "$mode" "$result" >&2
        return 1
      }
    fi
    if [[ "$mode" == "hardlink-bound" ]]; then
      [[ "$(<"$victim")" == "preserve" && "$(file_link_count "$victim")" == "2" ]] || return 1
      rm "$bound" "$victim"
      continue
    fi
    IFS= read -r pid <"$bound" || return 1
    [[ "$pid" =~ ^[0-9]+$ ]] || return 1
    bounded_process_absent "$pid" && bounded_group_gone "$pid" || return 1
  done
}

run_bound_exit_supervisor_failure_cases() {
  local tmp="$1" stall_entry="$2"
  run_bound_exit_supervisor --selftest-stat-parser || {
    printf 'ERROR: selftest: supervisor stat-parser case failed\n' >&2
    return 1
  }
  run_bound_exit_supervisor --selftest-parent-death "$stall_entry" "$tmp" \
    "$SELFTEST_TMP_IDENTITY" || {
    printf 'ERROR: selftest: supervisor parent-death case failed\n' >&2
    return 1
  }
  run_bound_exit_supervisor --selftest-watchdog-expiry "$stall_entry" "$tmp" \
    "$SELFTEST_TMP_IDENTITY" || {
    printf 'ERROR: selftest: supervisor watchdog-expiry case failed\n' >&2
    return 1
  }
  run_bound_exit_supervisor --selftest-closed-death-fd "$stall_entry" "$tmp" \
    "$SELFTEST_TMP_IDENTITY" || {
    printf 'ERROR: selftest: supervisor closed-death-fd case failed\n' >&2
    return 1
  }
  run_bound_exit_supervisor --selftest-closed-entry-fd "$stall_entry" "$tmp" \
    "$SELFTEST_TMP_IDENTITY" || {
    printf 'ERROR: selftest: supervisor closed-entry-fd case failed\n' >&2
    return 1
  }
  run_bound_exit_supervisor --selftest-observation-failure "$stall_entry" "$tmp" \
    "$SELFTEST_TMP_IDENTITY" || {
    printf 'ERROR: selftest: supervisor observation-failure case failed\n' >&2
    return 1
  }
  run_bound_exit_supervisor --selftest-cleanup-retry "$stall_entry" "$tmp" \
    "$SELFTEST_TMP_IDENTITY" || {
    printf 'ERROR: selftest: supervisor cleanup-retry case failed\n' >&2
    return 1
  }
  run_bound_exit_supervisor --selftest-hardlink-bound "$stall_entry" "$tmp" \
    "$SELFTEST_TMP_IDENTITY" || {
    printf 'ERROR: selftest: supervisor hardlink-bound case failed\n' >&2
    return 1
  }
  run_bound_exit_supervisor --selftest-missing-entry "$tmp" "$SELFTEST_TMP_IDENTITY" || {
    printf 'ERROR: selftest: supervisor missing-entry case failed\n' >&2
    return 1
  }
  run_bound_exit_supervisor --selftest-entry-binding "$tmp" "$SELFTEST_TMP_IDENTITY" || {
    printf 'ERROR: selftest: supervisor entry-binding case failed\n' >&2
    return 1
  }
  run_bound_exit_supervisor --selftest-controller-isolation "$tmp" \
    "$SELFTEST_TMP_IDENTITY" || {
    printf 'ERROR: selftest: supervisor controller-isolation case failed\n' >&2
    return 1
  }
}

selftest_bound_exit_supervisor_failures() {
  local tmp="$1" stall_entry="$1/supervisor-stall-entry.sh"
  write_supervisor_stall_fixture "$stall_entry" || return 1
  run_bound_exit_supervisor_failure_modes "$tmp" "$stall_entry" || return 1
  run_bound_exit_supervisor_failure_cases "$tmp" "$stall_entry"
}

selftest_nonroot_cleanup_retry_supervisor() {
  local tmp="$1" stall_entry="$1/nonroot-supervisor-stall-entry.sh" status
  local original_entry="$SELFTEST_IMAGE_ENTRY"
  [[ -z "$original_entry" || "$original_entry" == "$SCRIPT_DIR/devcontainer_image.sh" ]] || return 1
  SELFTEST_IMAGE_ENTRY="$SCRIPT_DIR/devcontainer_image.sh"
  write_supervisor_stall_fixture "$stall_entry" || return 1
  if run_bound_exit_supervisor --selftest-cleanup-retry "$stall_entry" "$tmp" \
    "$SELFTEST_TMP_IDENTITY"; then
    status=0
  else
    status=$?
  fi
  SELFTEST_IMAGE_ENTRY="$original_entry"
  [[ "$status" == "0" ]]
}
