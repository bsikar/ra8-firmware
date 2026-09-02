#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
# Signal-controller cases for the authenticated devcontainer image selftest.

[[ "${BASH_SOURCE[0]}" != "$0" ]] || {
  printf "error: devcontainer signal selftest helper is source-only\n" >&2
  exit 1
}
[[ "$-" == *p* ]] || {
  printf 'error: devcontainer signal selftest helper requires privileged Bash mode\n' >&2
  return 1
}
SELFTEST_SIGNAL_PARENT="${DEVCONTAINER_SELFTEST_PARENT:-missing}"
SELFTEST_SIGNAL_PARENT_DIR="$(cd -P "$(dirname "$SELFTEST_SIGNAL_PARENT")" 2>/dev/null && pwd)" || return 1
[[ -n "${DEVCONTAINER_SELFTEST_SOURCE_DIR:-}" &&
  "$DEVCONTAINER_SELFTEST_SOURCE_DIR" == "$SELFTEST_SIGNAL_PARENT_DIR" &&
  "${DEVCONTAINER_SELFTEST_PARENT:-}" == "$SELFTEST_SIGNAL_PARENT_DIR/devcontainer_image.sh" &&
  "${BASH_SOURCE[1]:-missing}" -ef "$SELFTEST_SIGNAL_PARENT" ]] || {
  printf 'error: devcontainer signal selftest helper has an unauthorized parent\n' >&2
  return 1
}
unset -v DEVCONTAINER_SELFTEST_PARENT DEVCONTAINER_SELFTEST_SOURCE_DIR
unset -v SELFTEST_SIGNAL_PARENT SELFTEST_SIGNAL_PARENT_DIR

SCRIPT_DIR="${SCRIPT_DIR:?}"
SELFTEST_IMAGE_ENTRY="${SELFTEST_IMAGE_ENTRY-}"
SELFTEST_BOUND_CONTROLLER="${SELFTEST_BOUND_CONTROLLER-}"
SELFTEST_BOUND_CONTROLLER_PGID="${SELFTEST_BOUND_CONTROLLER_PGID-}"
SELFTEST_BOUND_CONTROLLER_PRE_RELEASE="${SELFTEST_BOUND_CONTROLLER_PRE_RELEASE-}"
SELFTEST_REAP_STATUS="${SELFTEST_REAP_STATUS-}"
SELFTEST_FORCE_CLEANUP_RECEIPT="${SELFTEST_FORCE_CLEANUP_RECEIPT-}"
SELFTEST_TMP_DIR="${SELFTEST_TMP_DIR-}"
SELFTEST_TMP_IDENTITY="${SELFTEST_TMP_IDENTITY-}"
SELFTEST_DEADLINE_STEPS="${SELFTEST_DEADLINE_STEPS:?}"
declare SELFTEST_CONTROLLER_CLEANUP_STEPS
declare SELFTEST_SUITE_ANCHOR
declare SELFTEST_SUITE_ANCHOR_IDENTITY
declare SELFTEST_SUITE_ANCHOR_OWNER_UID

selftest_case_trap_composition() {
  local tmp="$1" receipt="$1/case-trap-cleanup-path" path status
  if (
    reset_selftest_tmp_state
    begin_selftest_tmp
    printf '%s\n' "$SELFTEST_TMP_DIR" >"$receipt"
    # The sourced trap installer invokes this dynamically scoped test callback.
    # shellcheck disable=SC2329  # the sourced trap installer invokes this callback by its fixed name.
    cleanup_image_lock_case() { return 0; }
    install_image_lock_case_traps
    exit 23
  ); then
    status=0
  else
    status=$?
  fi
  [[ "$status" == "23" ]] || die "selftest: case EXIT cleanup returned $status, expected 23"
  IFS= read -r path <"$receipt" || die "selftest: case EXIT cleanup path is missing"
  assert_selftest_tmp_absent "$path" "case EXIT path"
  selftest_one_case_signal_composition "$tmp" HUP 129
  selftest_one_case_signal_composition "$tmp" INT 130
  selftest_one_case_signal_composition "$tmp" TERM 143
}

selftest_case_signal_child() {
  local signal="$1" parent="$2" parent_identity="$3" receipt="$4"
  local lock_receipt="$5" proof="$6" anchor="$7" anchor_identity="$8"
  local anchor_owner_uid="$9"
  [[ "${BASH_SUBSHELL:-0}" == "0" &&
    ("$signal" == "HUP" || "$signal" == "INT" || "$signal" == "TERM") &&
    "$receipt" == "$parent/case-$signal-root-cleanup" &&
    "$lock_receipt" == "$parent/case-$signal-lock-cleanup" &&
    "$proof" == "$parent/case-$signal-lock-proof" &&
    ! -e "$receipt" && ! -L "$receipt" &&
    ! -e "$lock_receipt" && ! -L "$lock_receipt" &&
    ! -e "$proof" && ! -L "$proof" ]] ||
    die "selftest: case signal child arguments are unsafe"
  if ! configure_selftest_suite_authority "$parent" "$parent_identity" \
    "$anchor" "$anchor_identity" "$anchor_owner_uid"; then
    die "selftest: case signal parent is unsafe"
  fi
  reset_selftest_tmp_state
  begin_selftest_tmp || die "selftest: case signal root allocation failed"
  printf '%s\n' "$SELFTEST_TMP_DIR" >"$receipt"
  # shellcheck disable=SC2329  # install_image_lock_case_traps invokes this callback by its fixed name.
  cleanup_image_lock_case() { : >"$lock_receipt"; }
  # shellcheck disable=SC2329  # install_image_lock_case_traps invokes this callback by its fixed name.
  require_cleanup_receipt() { [[ -f "$lock_receipt" ]]; }
  # shellcheck disable=SC2329  # install_image_lock_case_traps invokes this callback by its fixed name.
  write_worker_cleanup_proof_file() { : >"$proof"; }
  install_image_lock_case_traps
  kill -"$signal" "$$"
  die "selftest: case $signal trap returned"
}

selftest_one_case_signal_composition() {
  local tmp="$1" signal="$2" expected="$3" receipt path status
  local lock_receipt="$1/case-$2-lock-cleanup" proof="$1/case-$2-lock-proof"
  receipt="$1/case-$2-root-cleanup"
  [[ ! -e "$receipt" && ! -L "$receipt" &&
    ! -e "$lock_receipt" && ! -L "$lock_receipt" &&
    ! -e "$proof" && ! -L "$proof" ]] ||
    die "selftest: case $signal receipt paths were precreated"
  if /bin/bash -p -- "$SCRIPT_DIR/devcontainer_image.sh" \
    --selftest-case-signal-child "$signal" "$tmp" "$SELFTEST_TMP_IDENTITY" \
    "$receipt" "$lock_receipt" "$proof" "$SELFTEST_SUITE_ANCHOR" \
    "$SELFTEST_SUITE_ANCHOR_IDENTITY" "$SELFTEST_SUITE_ANCHOR_OWNER_UID"; then
    status=0
  else
    status=$?
  fi
  [[ "$status" == "$expected" && -f "$lock_receipt" && -f "$proof" ]] ||
    die "selftest: case $signal trap composition failed"
  IFS= read -r path <"$receipt" || die "selftest: case $signal root path is missing"
  assert_selftest_tmp_absent "$path" "case $signal root path"
}

start_signal_controller() {
  local destination="$1" managed="$2" case_dir="$3" readiness_mode="${4:-}"
  local launcher_mode="${5:-}" controller_pid pending ready="$3/controller-launcher.ready"
  local ack="$3/controller-launcher.ack"
  local pre_ready="$3/controller-pre-isolation.ready" pre_release="$3/controller-pre-isolation.release"
  local launcher='import os, signal, sys, time
entry, ready, ack, mode = sys.argv[1:5]; args = sys.argv[5:]
if os.getpid() != os.getpgrp():
    raise SystemExit(125)
for value in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM): signal.signal(value, signal.SIG_DFL)
fd = os.open(ready, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600); value = "malformed" if mode in ("malformed-ready", "ignore-timeout") else str(os.getpid())
os.write(fd, (value + "\n").encode("ascii")); os.close(fd)
if mode == "ignore-timeout":
    time.sleep(60); raise SystemExit(126)
deadline = time.monotonic() + 1.0
while time.monotonic() < deadline:
    if os.path.isfile(ack): os.execv("/bin/bash", ["/bin/bash", "-p", "--", entry, *args])
    time.sleep(0.01)
raise SystemExit(124)'
  [[ ! -e "$ready" && ! -L "$ready" && ! -e "$ack" && ! -L "$ack" ]] || return 1
  begin_selftest_spawn_critical controller_bound_spawn_signal || return 1
  SELFTEST_BOUND_CONTROLLER_PGID=""
  (
    trap '' HUP INT TERM
    if [[ "$launcher_mode" == "pre-isolation-signal" ]]; then
      (umask 077 && printf 'ready\n' >"$pre_ready") || exit 127
      while [[ ! -e "$pre_release" ]]; do sleep 0.01; done
    fi
    exec /usr/bin/setsid /usr/bin/python3 -B -I -S -c "$launcher" \
      "$SELFTEST_IMAGE_ENTRY" "$ready" "$ack" "$launcher_mode" \
      --selftest-image-lock-signal-controller "$managed" "$case_dir" "$readiness_mode"
  ) &
  controller_pid=$!
  SELFTEST_BOUND_CONTROLLER="$controller_pid"
  SELFTEST_BOUND_CONTROLLER_PRE_RELEASE=""
  [[ "$launcher_mode" != "pre-isolation-signal" ]] ||
    SELFTEST_BOUND_CONTROLLER_PRE_RELEASE="$pre_release"
  finish_selftest_spawn_critical pending || return 1
  [[ "$pending" == "0" ]] || return 1
  if [[ "$launcher_mode" == "pre-isolation-signal" ]]; then
    wait_for_status_file "$pre_ready" "$controller_pid" || controller_bound_spawn_signal 1
    signal_owned_live_child TERM "$controller_pid" || controller_bound_spawn_signal 1
    : >"$pre_release" || controller_bound_spawn_signal 1
  fi
  if ! wait_for_controller_launcher "$controller_pid" "$ready"; then
    cleanup_bound_controller_launcher || return 1
    return 1
  fi
  SELFTEST_BOUND_CONTROLLER_PGID="$controller_pid"
  selftest_inject_bound_exit controller "$controller_pid"
  if ! (umask 077 && printf '%s\n' "$controller_pid" >"$ack"); then
    cleanup_bound_controller_launcher || return 1
    return 1
  fi
  printf -v "$destination" '%s' "$controller_pid"
}

wait_for_controller_launcher() {
  local controller="$1" ready="$2" attempt value
  for ((attempt = 0; attempt < SELFTEST_DEADLINE_STEPS; ++attempt)); do
    if controller_group_signal_is_authorized "$controller" && [[ -f "$ready" && ! -L "$ready" ]]; then
      IFS= read -r value <"$ready" || return 1
      [[ "$value" == "$controller" && "$(file_link_count "$ready")" == "1" ]] && return 0
    fi
    process_is_terminal "$controller" && return 1
    sleep 0.01
  done
  return 1
}

signal_owned_controller_group() {
  local signal="$1" controller="$2"
  controller_group_signal_is_authorized "$controller" || return 1
  builtin kill -"$signal" -- "-$controller"
}

controller_bound_spawn_signal() {
  local status="$1"
  trap - EXIT HUP INT TERM
  cleanup_bound_controller_launcher || exit 1
  selftest_root_signal "$status"
}

cleanup_bound_controller_launcher() {
  local controller="$SELFTEST_BOUND_CONTROLLER" child_status group_bound=0 attempt
  [[ "$controller" =~ ^[0-9]+$ ]] || return 1
  if [[ -n "$SELFTEST_BOUND_CONTROLLER_PRE_RELEASE" ]]; then
    : >"$SELFTEST_BOUND_CONTROLLER_PRE_RELEASE" || return 1
  fi
  [[ "$SELFTEST_BOUND_CONTROLLER_PGID" != "$controller" ]] || group_bound=1
  for ((attempt = 0; attempt < SELFTEST_DEADLINE_STEPS; ++attempt)); do
    process_is_terminal "$controller" && break
    if controller_group_signal_is_authorized "$controller"; then
      group_bound=1
      SELFTEST_BOUND_CONTROLLER_PGID="$controller"
      break
    fi
    sleep 0.01
  done
  if [[ "$group_bound" == "1" ]]; then
    if ! process_is_terminal "$controller"; then
      shell_owns_live_child "$controller" || return 1
    fi
    kill -KILL -- "-$controller" 2>/dev/null || bounded_group_gone "$controller" || return 1
  elif ! process_is_terminal "$controller"; then
    signal_owned_live_child KILL "$controller" || return 1
  fi
  if wait "$controller"; then child_status=0; else child_status=$?; fi
  if [[ "$child_status" == "127" ]]; then
    bounded_process_absent "$controller" || return 1
  else
    [[ "$child_status" == "0" || "$child_status" == "124" || "$child_status" == "125" ||
      "$child_status" == "126" || "$child_status" == "137" ||
      "$child_status" == "143" ]] || return 1
  fi
  bounded_process_absent "$controller" || return 1
  [[ "$group_bound" == "0" ]] || bounded_group_gone "$controller" || return 1
  release_bound_controller_traps
}

release_bound_controller_traps() {
  if [[ -z "$SELFTEST_BOUND_CONTROLLER" ]]; then
    restore_selftest_root_traps
    return 0
  fi
  shell_owns_live_child "$SELFTEST_BOUND_CONTROLLER" && return 1
  SELFTEST_BOUND_CONTROLLER=""
  SELFTEST_BOUND_CONTROLLER_PGID=""
  SELFTEST_BOUND_CONTROLLER_PRE_RELEASE=""
  restore_selftest_root_traps
}

selftest_pre_isolation_exit_refusal() {
  local tmp="$1" ready="$1/pre-isolation-exit.ready" child pending
  [[ ! -e "$ready" && ! -L "$ready" ]] || return 1
  begin_selftest_spawn_critical controller_bound_spawn_signal || return 1
  (
    trap '' HUP INT TERM
    (umask 077 && printf 'ready\n' >"$ready")
  ) &
  child=$!
  SELFTEST_BOUND_CONTROLLER="$child"
  SELFTEST_BOUND_CONTROLLER_PGID=""
  SELFTEST_BOUND_CONTROLLER_PRE_RELEASE=""
  finish_selftest_spawn_critical pending || return 1
  [[ "$pending" == "0" ]] || return 1
  wait_for_status_file "$ready" "$child" || controller_bound_spawn_signal 1
  bounded_process_terminal "$child" 50 || controller_bound_spawn_signal 1
  ! signal_owned_live_child TERM "$child" || controller_bound_spawn_signal 1
  cleanup_bound_controller_launcher || return 1
  [[ -z "$SELFTEST_BOUND_CONTROLLER" ]]
}

selftest_signal_ready_timeout() {
  local tmp="$1" controller case_dir="$1/signal-ready-timeout"
  local managed="$case_dir/managed"
  mkdir "$case_dir"
  create_managed_test_lock "$managed"
  start_signal_controller controller "$managed" "$case_dir" delay-controller-ready ||
    die "selftest: delayed signal controller did not start"
  if wait_for_status_file "$case_dir/controller-ready.status" "$controller"; then
    die "selftest: delayed signal controller unexpectedly became ready"
  fi
  SELFTEST_FORCE_CLEANUP_RECEIPT=""
  force_signal_controller_cleanup "$controller" "$case_dir" "$managed" ||
    die "selftest: unready signal controller cleanup failed"
  require_force_cleanup_receipt "$case_dir" ||
    die "selftest: unready signal controller cleanup receipt missing"
  release_bound_controller_traps || die "selftest: unready controller binding was not released"
  write_scenario_receipt signal-ready-timeout
}

selftest_bound_exit_controller_trigger() {
  local tmp="$1" controller case_dir="$1/bound-exit-controller-trigger"
  local managed="$case_dir/managed"
  [[ "${RA8_SELFTEST_INJECT_BOUND_EXIT:-}" == "controller" ]] || return 0
  mkdir "$case_dir"
  create_managed_test_lock "$managed"
  start_signal_controller controller "$managed" "$case_dir" ||
    die "selftest: bound-exit controller trigger did not start"
  cleanup_bound_controller_launcher ||
    die "selftest: returned bound-exit controller could not be cleaned"
  die "selftest: bound-exit controller injection returned"
}

selftest_controller_handler_hang() {
  local tmp="$1" controller case_dir="$1/controller-handler-hang"
  local managed="$case_dir/managed"
  mkdir "$case_dir"
  create_managed_test_lock "$managed"
  start_signal_controller controller "$managed" "$case_dir" hang-controller-cleanup ||
    die "selftest: handler-hang controller did not start"
  wait_for_status_file "$case_dir/controller-ready.status" "$controller" ||
    die "selftest: handler-hang controller did not become ready"
  signal_owned_controller_group TERM "$controller" ||
    die "selftest: handler-hang controller lost its process-group authority"
  ! bounded_process_terminal "$controller" 50 ||
    die "selftest: injected controller cleanup hang unexpectedly returned"
  force_signal_controller_cleanup "$controller" "$case_dir" "$managed" ||
    die "selftest: handler-hang group fallback failed"
  [[ "$SELFTEST_REAP_STATUS" == "137" ]] ||
    die "selftest: handler-hang group fallback did not KILL its process group"
  require_force_cleanup_receipt "$case_dir" ||
    die "selftest: handler-hang fallback receipt missing"
  release_bound_controller_traps || die "selftest: handler-hang binding was not released"
}

selftest_signal_cleanup() {
  local tmp="$1" signal expected controller case_dir managed launcher_mode
  for signal in HUP INT TERM; do
    case "$signal" in HUP) expected=129 ;; INT) expected=130 ;; TERM) expected=143 ;; esac
    case_dir="$tmp/signal-$signal"
    managed="$case_dir/managed"
    mkdir "$case_dir"
    create_managed_test_lock "$managed"
    launcher_mode=""
    [[ "$signal" != "HUP" ]] || launcher_mode="pre-isolation-signal"
    start_signal_controller controller "$managed" "$case_dir" "" "$launcher_mode" ||
      die "selftest: $signal controller did not start"
    if ! wait_for_status_file "$case_dir/controller-ready.status" "$controller"; then
      SELFTEST_FORCE_CLEANUP_RECEIPT=""
      force_signal_controller_cleanup "$controller" "$case_dir" "$managed" ||
        die "selftest: $signal unready-controller cleanup failed"
      : "$SELFTEST_FORCE_CLEANUP_RECEIPT"
      require_force_cleanup_receipt "$case_dir" ||
        die "selftest: $signal unready-controller cleanup receipt missing"
      die "selftest: $signal controller did not become ready"
    fi
    signal_owned_controller_group "$signal" "$controller" ||
      die "selftest: $signal controller lost process-group authority"
    bounded_process_terminal "$controller" "$SELFTEST_CONTROLLER_CLEANUP_STEPS" ||
      die "selftest: $signal controller did not terminate within its cleanup bound"
    verify_signal_controller_cleanup "$controller" "$case_dir" "$managed" ||
      die "selftest: $signal controller cleanup verification failed"
    [[ "$SELFTEST_REAP_STATUS" == "$expected" ]] ||
      die "selftest: $signal controller returned $SELFTEST_REAP_STATUS, expected $expected"
    require_worker_cleanup_proof_file "$case_dir" ||
      die "selftest: $signal worker cleanup proof missing"
    require_controller_cleanup_receipt_file "$case_dir" ||
      die "selftest: $signal controller cleanup receipt missing or malformed"
    release_bound_controller_traps ||
      die "selftest: $signal controller binding was not released"
  done
  write_scenario_receipt signal-cleanup
}
