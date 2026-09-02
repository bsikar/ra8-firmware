#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
# Test-only process supervision for devcontainer_image.sh. This file is sourced
# from that fixed, mode-checked entry point and is not a public command.

[[ "${BASH_SOURCE[0]}" != "$0" ]] || {
  printf 'error: devcontainer image lock selftest helper is source-only\n' >&2
  exit 1
}
[[ "$-" == *p* ]] || {
  printf 'error: devcontainer image lock selftest helper requires privileged Bash mode\n' >&2
  return 1
}
SELFTEST_LOCK_HELPER_PARENT="${DEVCONTAINER_SELFTEST_PARENT:-missing}"
SELFTEST_LOCK_HELPER_PARENT_DIR="$(cd -P "$(dirname "$SELFTEST_LOCK_HELPER_PARENT")" 2>/dev/null && pwd)" || return 1
[[ -n "${DEVCONTAINER_SELFTEST_SOURCE_DIR:-}" &&
  "$DEVCONTAINER_SELFTEST_SOURCE_DIR" == "$SELFTEST_LOCK_HELPER_PARENT_DIR" &&
  "${DEVCONTAINER_SELFTEST_PARENT:-}" == "$SELFTEST_LOCK_HELPER_PARENT_DIR/devcontainer_image.sh" &&
  "${BASH_SOURCE[1]:-missing}" -ef "$SELFTEST_LOCK_HELPER_PARENT" ]] || {
  printf 'error: devcontainer image lock selftest helper has an unauthorized parent\n' >&2
  return 1
}
SELFTEST_HELPER_DIR="${DEVCONTAINER_SELFTEST_SCRIPT_DIR:?}"
unset -v DEVCONTAINER_SELFTEST_PARENT DEVCONTAINER_SELFTEST_SOURCE_DIR
unset -v DEVCONTAINER_SELFTEST_SCRIPT_DIR DEVCONTAINER_SELFTEST_REPO_ROOT
unset -v DEVCONTAINER_SELFTEST_LABEL_KEY SELFTEST_LOCK_HELPER_PARENT
unset -v SELFTEST_LOCK_HELPER_PARENT_DIR

SELFTEST_DEADLINE_STEPS="${SELFTEST_DEADLINE_STEPS:?}"
SELFTEST_CONTROLLER_CLEANUP_STEPS=1600
SELFTEST_REAP_STATUS=255
SELFTEST_WORKER_PID=""
SELFTEST_WORKER_PGID=""
SELFTEST_WORKER_SHARED_GROUP=0
SELFTEST_WORKER_DESCENDANT_PID=""
SELFTEST_WORKER_REAPED=0
SELFTEST_CONTROLLER_REAPED=0
SELFTEST_PARENT_LOCK_OPEN=0
SELFTEST_CASE_DIR=""
SELFTEST_MANAGED_DIR=""
SELFTEST_CLEANUP_RECEIPT=""
SELFTEST_FORCE_CLEANUP_RECEIPT=""
SELFTEST_SUITE_RECEIPTS=""
SELFTEST_SUITE_COUNT=0
SELFTEST_DISPATCH_COMPLETE=0
: "$SELFTEST_DISPATCH_COMPLETE"
SELFTEST_FILE_RECEIPTS_VERIFIED=0
SELFTEST_RECEIPT_DIR=""
# The separately authenticated receipt helper owns reads of these shared values.
: "$SELFTEST_CLEANUP_RECEIPT" "$SELFTEST_FORCE_CLEANUP_RECEIPT"
: "$SELFTEST_FILE_RECEIPTS_VERIFIED" "$SELFTEST_RECEIPT_DIR"
SELFTEST_IMAGE_ENTRY="$SELFTEST_HELPER_DIR/devcontainer_image.sh"
SELFTEST_BOUND_DIRECT_CHILD=""
declare SELFTEST_BOUND_CONTROLLER
declare SELFTEST_BOUND_CONTROLLER_PGID

create_managed_test_lock() {
  local directory="$1" gid="${SELFTEST_GROUP_GID:?}"
  mkdir -m 0750 "$directory"
  chgrp "$gid" "$directory"
  printf '%s\n' "$gid" >"$directory/devcontainer-image.gid"
  chown 0:0 "$directory/devcontainer-image.gid"
  chmod 0444 "$directory/devcontainer-image.gid"
  : >"$directory/devcontainer-image.lock"
  chgrp "$gid" "$directory/devcontainer-image.lock"
  chmod 0660 "$directory/devcontainer-image.lock"
}

selftest_image_lock_shape_rejections() {
  local tmp="$1" managed="$2" directory
  if (RA8_IMAGE_LOCK_DIR="$tmp/missing" resolve_image_lock >/dev/null 2>&1); then
    die "selftest: missing managed image lock directory passed"
  fi
  : >"$tmp/not-a-lock-dir"
  if (RA8_IMAGE_LOCK_DIR="$tmp/not-a-lock-dir" resolve_image_lock >/dev/null 2>&1); then
    die "selftest: non-directory managed image lock path passed"
  fi
  ln -s "$managed" "$tmp/linked-lock-dir"
  if (RA8_IMAGE_LOCK_DIR="$tmp/linked-lock-dir" resolve_image_lock >/dev/null 2>&1); then
    die "selftest: symlinked managed image lock directory passed"
  fi
  mkdir -m 0750 "$tmp/missing-lock"
  if (RA8_IMAGE_LOCK_DIR="$tmp/missing-lock" resolve_image_lock >/dev/null 2>&1); then
    die "selftest: missing managed image lock file passed"
  fi
  mkdir -m 0750 "$tmp/missing-marker"
  chgrp "$SELFTEST_GROUP_GID" "$tmp/missing-marker"
  : >"$tmp/missing-marker/devcontainer-image.lock"
  chgrp "$SELFTEST_GROUP_GID" "$tmp/missing-marker/devcontainer-image.lock"
  chmod 0660 "$tmp/missing-marker/devcontainer-image.lock"
  if (RA8_IMAGE_LOCK_DIR="$tmp/missing-marker" resolve_image_lock >/dev/null 2>&1); then
    die "selftest: missing managed image lock group marker passed"
  fi
  mkdir -m 0750 "$tmp/linked-lock"
  ln -s "$managed/devcontainer-image.lock" "$tmp/linked-lock/devcontainer-image.lock"
  if (RA8_IMAGE_LOCK_DIR="$tmp/linked-lock" resolve_image_lock >/dev/null 2>&1); then
    die "selftest: symlinked managed image lock file passed"
  fi
  mkdir -m 0750 "$tmp/nonregular-lock"
  mkfifo "$tmp/nonregular-lock/devcontainer-image.lock"
  if (RA8_IMAGE_LOCK_DIR="$tmp/nonregular-lock" resolve_image_lock >/dev/null 2>&1); then
    die "selftest: non-regular managed image lock file passed"
  fi
  directory="$tmp/multilink-lock"
  create_managed_test_lock "$directory"
  ln "$directory/devcontainer-image.lock" "$directory/second-lock-link"
  if (RA8_IMAGE_LOCK_DIR="$directory" resolve_image_lock >/dev/null 2>&1); then
    die "selftest: multiply-linked managed image lock file passed"
  fi
  directory="$tmp/linked-marker"
  create_managed_test_lock "$directory"
  rm "$directory/devcontainer-image.gid"
  ln -s "$managed/devcontainer-image.gid" "$directory/devcontainer-image.gid"
  if (RA8_IMAGE_LOCK_DIR="$directory" resolve_image_lock >/dev/null 2>&1); then
    die "selftest: symlinked managed image lock group marker passed"
  fi
  directory="$tmp/multilink-marker"
  create_managed_test_lock "$directory"
  ln "$directory/devcontainer-image.gid" "$directory/second-gid-link"
  if (RA8_IMAGE_LOCK_DIR="$directory" resolve_image_lock >/dev/null 2>&1); then
    die "selftest: multiply-linked managed image lock group marker passed"
  fi
}

selftest_image_lock_metadata_rejections() {
  local tmp="$1" directory
  directory="$tmp/replaceable-lock-dir"
  create_managed_test_lock "$directory"
  chmod 0770 "$directory"
  if (RA8_IMAGE_LOCK_DIR="$directory" resolve_image_lock >/dev/null 2>&1); then
    die "selftest: group-replaceable managed image lock directory passed"
  fi
  directory="$tmp/wrong-lock-mode"
  create_managed_test_lock "$directory"
  chmod 0666 "$directory/devcontainer-image.lock"
  if (RA8_IMAGE_LOCK_DIR="$directory" resolve_image_lock >/dev/null 2>&1); then
    die "selftest: world-writable managed image lock passed"
  fi
  directory="$tmp/wrong-directory-owner"
  create_managed_test_lock "$directory"
  chown 1 "$directory"
  if (RA8_IMAGE_LOCK_DIR="$directory" resolve_image_lock >/dev/null 2>&1); then
    die "selftest: non-root managed image lock directory passed"
  fi
  directory="$tmp/wrong-lock-owner"
  create_managed_test_lock "$directory"
  chown 1 "$directory/devcontainer-image.lock"
  if (RA8_IMAGE_LOCK_DIR="$directory" resolve_image_lock >/dev/null 2>&1); then
    die "selftest: non-root managed image lock passed"
  fi
  selftest_image_lock_group_metadata_rejections "$tmp"
}

selftest_image_lock_group_metadata_rejections() {
  local tmp="$1" directory

  directory="$tmp/wrong-directory-group"
  create_managed_test_lock "$directory"
  chgrp 0 "$directory"
  if (RA8_IMAGE_LOCK_DIR="$directory" resolve_image_lock >/dev/null 2>&1); then
    die "selftest: wrong managed image lock directory group passed"
  fi
  directory="$tmp/wrong-lock-group"
  create_managed_test_lock "$directory"
  chgrp 0 "$directory/devcontainer-image.lock"
  if (RA8_IMAGE_LOCK_DIR="$directory" resolve_image_lock >/dev/null 2>&1); then
    die "selftest: wrong managed image lock file group passed"
  fi
  directory="$tmp/wrong-marker-owner"
  create_managed_test_lock "$directory"
  chown 1 "$directory/devcontainer-image.gid"
  if (RA8_IMAGE_LOCK_DIR="$directory" resolve_image_lock >/dev/null 2>&1); then
    die "selftest: non-root managed image lock group marker passed"
  fi
  directory="$tmp/wrong-marker-mode"
  create_managed_test_lock "$directory"
  chmod 0644 "$directory/devcontainer-image.gid"
  if (RA8_IMAGE_LOCK_DIR="$directory" resolve_image_lock >/dev/null 2>&1); then
    die "selftest: writable managed image lock group marker passed"
  fi
  directory="$tmp/wrong-marker-content"
  create_managed_test_lock "$directory"
  printf '0\n' >"$directory/devcontainer-image.gid"
  chmod 0444 "$directory/devcontainer-image.gid"
  if (RA8_IMAGE_LOCK_DIR="$directory" resolve_image_lock >/dev/null 2>&1); then
    die "selftest: root group in managed image lock group marker passed"
  fi
  directory="$tmp/binary-marker-content"
  create_managed_test_lock "$directory"
  chmod 0644 "$directory/devcontainer-image.gid"
  printf '%s\n\0' "$SELFTEST_GROUP_GID" >"$directory/devcontainer-image.gid"
  chmod 0444 "$directory/devcontainer-image.gid"
  if (RA8_IMAGE_LOCK_DIR="$directory" resolve_image_lock >/dev/null 2>&1); then
    die "selftest: binary managed image lock group marker passed"
  fi
}

process_is_terminal() {
  local pid="$1" state
  if state="$(ps -o stat= -p "$pid" 2>/dev/null)"; then
    [[ "$state" == *Z* ]]
    return
  fi
  ! kill -0 "$pid" 2>/dev/null
}

bounded_process_terminal() {
  local pid="$1" steps="${2:-$SELFTEST_DEADLINE_STEPS}" attempt
  for ((attempt = 0; attempt < steps; ++attempt)); do
    process_is_terminal "$pid" && return 0
    sleep 0.01
  done
  return 1
}

bounded_child_reap() {
  local pid="$1" status
  bounded_process_terminal "$pid" || return 1
  if wait "$pid"; then
    status=0
  else
    status=$?
  fi
  SELFTEST_REAP_STATUS="$status"
}

reap_worker() {
  bounded_child_reap "$SELFTEST_WORKER_PID" || return 1
  SELFTEST_WORKER_REAPED=1
}

reap_controller() {
  bounded_child_reap "$1" || return 1
  SELFTEST_CONTROLLER_REAPED=1
}

bounded_group_empty() {
  local pgid="$1" steps="${2:-$SELFTEST_DEADLINE_STEPS}" attempt live
  for ((attempt = 0; attempt < steps; ++attempt)); do
    if ! live="$(image_lock_ps_group_snapshot | awk -v group="$pgid" \
      '$1 == group && $2 !~ /Z/ { count += 1 } END { print count + 0 }')"; then
      sleep 0.01
      continue
    fi
    [[ "$live" == "0" ]] && return 0
    sleep 0.01
  done
  return 1
}

bounded_group_gone() {
  local pgid="$1" attempt members
  for ((attempt = 0; attempt < SELFTEST_DEADLINE_STEPS; ++attempt)); do
    if ! members="$(image_lock_ps_group_snapshot | awk -v group="$pgid" \
      '$1 == group { count += 1 } END { print count + 0 }')"; then
      sleep 0.01
      continue
    fi
    [[ "$members" == "0" ]] && return 0
    sleep 0.01
  done
  return 1
}

image_lock_ps_group_snapshot() {
  [[ "${SELFTEST_FORCE_PS_FAILURE:-0}" == "0" ]] || return 71
  ps -eo pgid=,stat=
}

bounded_process_absent() {
  local pid="$1" attempt
  for ((attempt = 0; attempt < SELFTEST_DEADLINE_STEPS; ++attempt)); do
    if ! ps -o stat= -p "$pid" >/dev/null 2>&1 && ! kill -0 "$pid" 2>/dev/null; then
      return 0
    fi
    sleep 0.01
  done
  return 1
}

wait_for_status_file() {
  local path="$1" pid="$2" attempt
  for ((attempt = 0; attempt < SELFTEST_DEADLINE_STEPS; ++attempt)); do
    [[ -s "$path" ]] && return 0
    if process_is_terminal "$pid"; then
      [[ -s "$path" ]] && return 0
      return 1
    fi
    sleep 0.01
  done
  return 1
}

worker_group_is_safe() {
  local own_pgid
  own_pgid="$(ps -o pgid= -p "$$" | tr -d ' ')"
  [[ "$SELFTEST_WORKER_PID" =~ ^[0-9]+$ && "$SELFTEST_WORKER_PGID" =~ ^[0-9]+$ ]]
  ((SELFTEST_WORKER_PGID > 1))
  if [[ "$SELFTEST_WORKER_SHARED_GROUP" == "1" ]]; then
    [[ "$SELFTEST_WORKER_PGID" == "$own_pgid" &&
      "$SELFTEST_WORKER_PGID" != "$SELFTEST_WORKER_PID" ]]
  else
    [[ "$SELFTEST_WORKER_PGID" != "$own_pgid" ]]
  fi
}

worker_group_signal_is_authorized() {
  worker_group_is_safe && [[ "$SELFTEST_WORKER_PGID" == "$SELFTEST_WORKER_PID" ]] &&
    shell_owns_live_child "$SELFTEST_WORKER_PID"
}

controller_group_signal_is_authorized() {
  local controller="$1" pgid own_pgid
  [[ "${SELFTEST_FORCE_CONTROLLER_PS_FAILURE:-0}" == "0" ]] || return 71
  [[ "$controller" =~ ^[0-9]+$ ]] && shell_owns_live_child "$controller" || return 1
  pgid="$(ps -o pgid= -p "$controller" 2>/dev/null | tr -d ' ')" || return 1
  own_pgid="$(ps -o pgid= -p "$$" | tr -d ' ')" || return 1
  [[ "$pgid" == "$controller" && "$pgid" != "$own_pgid" && "$pgid" -gt 1 ]]
}

wait_for_controller_group() {
  local controller="$1" attempt
  for ((attempt = 0; attempt < SELFTEST_DEADLINE_STEPS; ++attempt)); do
    controller_group_signal_is_authorized "$controller" && return 0
    process_is_terminal "$controller" && return 1
    sleep 0.01
  done
  return 1
}

read_worker_group() {
  local record="$SELFTEST_CASE_DIR/worker.group" pid pgid
  wait_for_status_file "$record" "$SELFTEST_WORKER_PID" || return 1
  IFS=: read -r pid pgid <"$record"
  [[ "$pid" == "$SELFTEST_WORKER_PID" ]] || return 1
  SELFTEST_WORKER_PGID="$pgid"
  worker_group_is_safe
}

read_worker_group_from_case() {
  local record="$1/worker.group" value size
  [[ -f "$record" && ! -L "$record" && "$(file_link_count "$record")" == "1" ]] || return 1
  IFS= read -r value <"$record" || return 1
  size="$(wc -c <"$record")" || return 1
  [[ "$size" == "$((${#value} + 1))" && "$value" =~ ^[0-9]+:[0-9]+$ ]] || return 1
  IFS=: read -r SELFTEST_WORKER_PID SELFTEST_WORKER_PGID <<<"$value"
  worker_group_is_safe
}

assert_no_surviving_descendants() {
  local descendants="$SELFTEST_CASE_DIR/descendants" pid
  [[ -e "$descendants" ]] || return 0
  while IFS= read -r pid; do
    [[ "$pid" =~ ^[0-9]+$ ]] || return 1
    bounded_process_absent "$pid" || return 1
  done <"$descendants"
}

release_parent_lock() {
  local release_failed=0
  if [[ "$SELFTEST_PARENT_LOCK_OPEN" == "1" ]]; then
    flock -u 8 || release_failed=1
    if exec 8>&-; then
      SELFTEST_PARENT_LOCK_OPEN=0
    else
      release_failed=1
    fi
  fi
  return "$release_failed"
}

fresh_lock_probe() {
  local lock="$SELFTEST_MANAGED_DIR/devcontainer-image.lock"
  if ! exec 7<"$lock"; then
    return 1
  fi
  if ! flock -n 7; then
    if ! exec 7>&-; then
      return 1
    fi
    return 1
  fi
  if ! flock -u 7; then
    if ! exec 7>&-; then
      return 1
    fi
    return 1
  fi
  if ! exec 7>&-; then
    return 1
  fi
}

cleanup_image_lock_case() {
  local cleanup_failed=0 group_signal_authorized=0
  SELFTEST_CLEANUP_RECEIPT=""
  if [[ "$SELFTEST_WORKER_SHARED_GROUP" == "1" ]] &&
    ! process_is_terminal "$SELFTEST_WORKER_PID"; then
    [[ "$SELFTEST_CASE_DIR" == */signal-* || "$SELFTEST_CASE_DIR" == */signal-ready-timeout ]] ||
      cleanup_failed=1
    printf 'stop\n' >"$SELFTEST_CASE_DIR/worker-stop" || cleanup_failed=1
    bounded_process_terminal "$SELFTEST_WORKER_PID" || cleanup_failed=1
  elif worker_group_is_safe 2>/dev/null && ! bounded_group_empty "$SELFTEST_WORKER_PGID"; then
    if worker_group_signal_is_authorized 2>/dev/null; then
      group_signal_authorized=1
      kill -TERM -- "-$SELFTEST_WORKER_PGID" 2>/dev/null || cleanup_failed=1
    else
      cleanup_failed=1
    fi
  fi
  release_parent_lock || cleanup_failed=1
  if [[ "$SELFTEST_WORKER_SHARED_GROUP" != "1" ]] &&
    worker_group_is_safe 2>/dev/null && ! bounded_group_empty "$SELFTEST_WORKER_PGID" 50; then
    if [[ "$group_signal_authorized" == "1" ]] &&
      worker_group_signal_is_authorized 2>/dev/null; then
      kill -KILL -- "-$SELFTEST_WORKER_PGID" 2>/dev/null || cleanup_failed=1
      bounded_group_empty "$SELFTEST_WORKER_PGID" || cleanup_failed=1
    else
      cleanup_failed=1
    fi
  fi
  if [[ "$SELFTEST_WORKER_PID" =~ ^[0-9]+$ && "$SELFTEST_WORKER_REAPED" == "0" ]]; then
    reap_worker || cleanup_failed=1
  fi
  if [[ "$SELFTEST_WORKER_PID" =~ ^[0-9]+$ ]]; then
    bounded_process_absent "$SELFTEST_WORKER_PID" || cleanup_failed=1
  fi
  if [[ "$SELFTEST_WORKER_SHARED_GROUP" != "1" ]] && worker_group_is_safe 2>/dev/null; then
    bounded_group_gone "$SELFTEST_WORKER_PGID" || cleanup_failed=1
  fi
  assert_no_surviving_descendants || cleanup_failed=1
  parent_lock_fd_is_closed || cleanup_failed=1
  fresh_lock_probe || cleanup_failed=1
  if ((cleanup_failed == 0)); then
    SELFTEST_CLEANUP_RECEIPT="$(expected_cleanup_receipt)"
  fi
  return "$cleanup_failed"
}

verify_signal_controller_cleanup() {
  local controller="$1" case_dir="$2" managed="$3"
  SELFTEST_CASE_DIR="$case_dir"
  SELFTEST_MANAGED_DIR="$managed"
  SELFTEST_CONTROLLER_REAPED=0
  reap_controller "$controller" || return 1
  [[ "$SELFTEST_CONTROLLER_REAPED" == "1" ]] || return 1
  bounded_process_absent "$controller" || return 1
  read_worker_group_from_case "$case_dir" || return 1
  bounded_process_absent "$SELFTEST_WORKER_PID" || return 1
  bounded_group_gone "$SELFTEST_WORKER_PGID" || return 1
  assert_no_surviving_descendants || return 1
  parent_lock_fd_is_closed || return 1
  fresh_lock_probe || return 1
  write_controller_cleanup_receipt_file "$case_dir" || return 1
  require_controller_cleanup_receipt_file "$case_dir" || return 1
  release_bound_controller_traps
}

force_signal_controller_cleanup() {
  local controller="$1" case_dir="$2" managed="$3" group_authorized=0
  SELFTEST_FORCE_CLEANUP_RECEIPT=""
  SELFTEST_CASE_DIR="$case_dir"
  SELFTEST_MANAGED_DIR="$managed"
  [[ "$SELFTEST_BOUND_CONTROLLER" != "$controller" ||
    "$SELFTEST_BOUND_CONTROLLER_PGID" != "$controller" ]] || group_authorized=1
  if ! process_is_terminal "$controller"; then
    controller_group_signal_is_authorized "$controller" || return 1
    group_authorized=1
    signal_owned_controller_group TERM "$controller" 2>/dev/null || return 1
  fi
  if ! bounded_process_terminal "$controller" "$SELFTEST_CONTROLLER_CLEANUP_STEPS"; then
    [[ "$group_authorized" == "1" ]] || return 1
    controller_group_signal_is_authorized "$controller" || return 1
    kill -KILL -- "-$controller" 2>/dev/null || return 1
  elif [[ "$group_authorized" == "1" ]]; then
    kill -KILL -- "-$controller" 2>/dev/null || bounded_group_gone "$controller" || return 1
  fi
  verify_signal_controller_cleanup "$controller" "$case_dir" "$managed" || return 1
  SELFTEST_FORCE_CLEANUP_RECEIPT="$(expected_force_cleanup_receipt "$case_dir")"
}

image_lock_case_exit() {
  local status=$? cleanup_failed=0
  trap - EXIT HUP INT TERM
  cleanup_image_lock_case || cleanup_failed=1
  cleanup_selftest_tmp || cleanup_failed=1
  ((cleanup_failed == 0)) || status=1
  exit "$status"
}

image_lock_case_signal() {
  local status="$1"
  local cleanup_failed=0
  trap - EXIT HUP INT TERM
  if [[ "${SELFTEST_HANG_CONTROLLER_CLEANUP:-0}" == "1" ]]; then
    trap '' HUP INT TERM
    while :; do sleep 1; done
  fi
  cleanup_image_lock_case || cleanup_failed=1
  require_cleanup_receipt || cleanup_failed=1
  write_worker_cleanup_proof_file || cleanup_failed=1
  cleanup_selftest_tmp || cleanup_failed=1
  ((cleanup_failed == 0)) || exit 1
  exit "$status"
}

install_image_lock_case_traps() {
  trap image_lock_case_exit EXIT
  trap 'image_lock_case_signal 129' HUP
  trap 'image_lock_case_signal 130' INT
  trap 'image_lock_case_signal 143' TERM
}

clear_image_lock_case_traps() {
  restore_selftest_root_traps
}

start_image_lock_worker() {
  local mode="$1" pending ack
  SELFTEST_WORKER_REAPED=0
  SELFTEST_WORKER_PGID=""
  begin_selftest_spawn_critical abort_bound_worker_spawn || return 1
  if [[ "$SELFTEST_WORKER_SHARED_GROUP" == "1" ]]; then
    (
      exec 8>&-
      trap '' HUP INT TERM
      exec /bin/bash -p -- "$SELFTEST_IMAGE_ENTRY" --selftest-image-lock-worker \
        "$mode" "$SELFTEST_MANAGED_DIR" "$SELFTEST_CASE_DIR"
    ) &
  else
    (
      exec 8>&-
      trap '' HUP INT TERM
      exec /usr/bin/setsid /bin/bash -p -- "$SELFTEST_IMAGE_ENTRY" \
        --selftest-image-lock-worker "$mode" "$SELFTEST_MANAGED_DIR" "$SELFTEST_CASE_DIR"
    ) &
  fi
  SELFTEST_WORKER_PID=$!
  finish_selftest_spawn_critical pending || return 1
  selftest_inject_bound_exit worker "$SELFTEST_WORKER_PID"
  if ! read_worker_group; then
    abort_bound_worker_spawn 1
  fi
  install_image_lock_case_traps
  ack="$SELFTEST_CASE_DIR/worker.ack"
  (umask 077 && printf '%s\n' "$SELFTEST_WORKER_PID" >"$ack") || image_lock_case_signal 1
}

abort_bound_worker_spawn() {
  local status="$1" child_status
  trap - EXIT HUP INT TERM
  signal_owned_live_child KILL "$SELFTEST_WORKER_PID" 2>/dev/null || exit 1
  if wait "$SELFTEST_WORKER_PID"; then child_status=0; else child_status=$?; fi
  [[ "$child_status" == "137" ]] || exit 1
  SELFTEST_WORKER_REAPED=1
  [[ ! -e "$SELFTEST_CASE_DIR/descendants" ]] || exit 1
  image_lock_case_signal "$status"
}

record_worker_group() {
  local case_dir="$1" mode="$2" pid="$$" pgid tmp
  [[ "${BASH_SUBSHELL:-0}" == "0" ]] || die "selftest worker is not a fresh process"
  pgid="$(ps -o pgid= -p "$pid" | tr -d ' ')"
  if [[ "$mode" == "signal-controller" ]]; then
    [[ "$pgid" == "$PPID" && "$pgid" != "$pid" ]] ||
      die "selftest shared worker did not remain in its controller group"
  else
    [[ "$pgid" == "$pid" ]] || die "selftest isolated worker is not its group leader"
  fi
  tmp="$case_dir/worker.group.tmp.$pid"
  printf '%s:%s\n' "$pid" "$pgid" >"$tmp"
  mv "$tmp" "$case_dir/worker.group"
}

wait_for_worker_ack() {
  local case_dir="$1" pid="$2" ack="$1/worker.ack" attempt value
  for ((attempt = 0; attempt < SELFTEST_DEADLINE_STEPS; ++attempt)); do
    if [[ -f "$ack" && ! -L "$ack" && "$(file_link_count "$ack")" == "1" ]]; then
      IFS= read -r value <"$ack" || return 1
      [[ "$value" == "$pid" ]] && return 0
    fi
    sleep 0.01
  done
  return 1
}

start_ignoring_descendant() {
  local case_dir="$1" mode="${2:-ignore-stop}" child
  (
    trap '' HUP INT TERM
    if [[ "$mode" == "stop-aware" ]]; then
      while [[ ! -e "$case_dir/worker-stop" ]]; do sleep 0.01; done
    else
      while :; do sleep 1; done
    fi
  ) &
  child=$!
  SELFTEST_WORKER_DESCENDANT_PID="$child"
  printf '%s\n' "$child" >>"$case_dir/descendants"
}

write_fake_image_runtime() {
  local target="$1"
  cat >"$target" <<'EOF'
#!/bin/bash
set -euo pipefail
printf 'entered\n' >"${RA8_SELFTEST_CASE_DIR:?}/build-entered.status"
if [[ "${RA8_SELFTEST_MODE:?}" == "post-ready-build-hang" ]]; then
  (
    trap '' HUP INT TERM
    while :; do sleep 1; done
  ) &
  printf '%s\n' "$!" >>"$RA8_SELFTEST_CASE_DIR/descendants"
  while :; do sleep 1; done
fi
printf 'done\n' >"$RA8_SELFTEST_CASE_DIR/done.status"
EOF
  chmod 0700 "$target"
}

image_lock_selftest_worker() {
  local mode="$1" managed="$2" case_dir="$3" fake_runtime
  record_worker_group "$case_dir" "$mode"
  wait_for_worker_ack "$case_dir" "$$" || exit 124
  case "$mode" in
    pre-ready-hang | signal-controller | post-ready-build-hang) trap '' HUP INT TERM ;;
    *) trap - HUP INT TERM ;;
  esac
  case "$mode" in
    early-exit) exit 23 ;;
    pre-ready-hang)
      start_ignoring_descendant "$case_dir"
      while :; do sleep 1; done
      ;;
    signal-controller)
      printf 'ready\n' >"$case_dir/ready.status"
      start_ignoring_descendant "$case_dir" stop-aware
      while [[ ! -e "$case_dir/worker-stop" ]]; do sleep 0.01; done
      wait "$SELFTEST_WORKER_DESCENDANT_PID"
      ;;
    normal | post-ready-build-hang)
      printf 'ready\n' >"$case_dir/ready.status"
      fake_runtime="$case_dir/fake-runtime"
      write_fake_image_runtime "$fake_runtime"
      export RA8_SELFTEST_MODE="$mode" RA8_SELFTEST_CASE_DIR="$case_dir"
      RA8_CONTAINER_RUNTIME="$fake_runtime" RA8_IMAGE_LOCK_DIR="$managed" \
        cmd_ensure --rebuild >/dev/null
      ;;
    *) die "selftest worker received an unknown mode" ;;
  esac
}

prepare_image_lock_case() {
  local tmp="$1" name="$2"
  SELFTEST_CASE_DIR="$tmp/$name"
  SELFTEST_MANAGED_DIR="$SELFTEST_CASE_DIR/managed"
  mkdir "$SELFTEST_CASE_DIR"
  create_managed_test_lock "$SELFTEST_MANAGED_DIR"
  RA8_IMAGE_LOCK_DIR="$SELFTEST_MANAGED_DIR" resolve_image_lock
  SELFTEST_WORKER_PID=""
  SELFTEST_WORKER_PGID=""
  SELFTEST_WORKER_SHARED_GROUP=0
  SELFTEST_WORKER_REAPED=0
  SELFTEST_CONTROLLER_REAPED=0
  SELFTEST_PARENT_LOCK_OPEN=0
  SELFTEST_CLEANUP_RECEIPT=""
  SELFTEST_FORCE_CLEANUP_RECEIPT=""
}

hold_parent_lock() {
  exec 8<"$SELFTEST_MANAGED_DIR/devcontainer-image.lock"
  flock -n 8 || die "selftest: could not hold the parent image lock"
  SELFTEST_PARENT_LOCK_OPEN=1
}

selftest_direct_child_authority() {
  local child status pending saved_worker_pid="$SELFTEST_WORKER_PID"
  local saved_worker_pgid="$SELFTEST_WORKER_PGID"
  begin_selftest_spawn_critical abort_bound_direct_child_spawn ||
    die "selftest: child authority spawn guard failed"
  (
    trap '' HUP INT TERM
    while :; do sleep 1; done
  ) &
  child=$!
  SELFTEST_BOUND_DIRECT_CHILD="$child"
  finish_selftest_spawn_critical pending ||
    die "selftest: child authority was not bound"
  [[ "$pending" == "0" ]] || die "selftest: child authority retained a pending signal"
  selftest_inject_bound_exit direct-child "$child"
  shell_owns_live_child "$child" || die "selftest: live direct child was not recognized"
  ! shell_owns_live_child "$$" || die "selftest: unrelated shell PID gained signal authority"
  signal_owned_live_child KILL "$child" || die "selftest: bound child could not be signalled"
  SELFTEST_BOUND_DIRECT_CHILD=""
  restore_selftest_root_traps
  if wait "$child"; then status=0; else status=$?; fi
  [[ "$status" == "137" ]] || die "selftest: bound child returned $status, expected 137"
  ! shell_owns_live_child "$child" || die "selftest: stale child PID retained signal authority"
  SELFTEST_WORKER_PID="$$"
  SELFTEST_WORKER_PGID="$$"
  ! worker_group_signal_is_authorized ||
    die "selftest: rebound numeric process group gained signal authority"
  SELFTEST_WORKER_PID="$saved_worker_pid"
  SELFTEST_WORKER_PGID="$saved_worker_pgid"
}

abort_bound_direct_child_spawn() {
  local status="$1" child_status
  trap - EXIT HUP INT TERM
  signal_owned_live_child KILL "$SELFTEST_BOUND_DIRECT_CHILD" || exit 1
  if wait "$SELFTEST_BOUND_DIRECT_CHILD"; then child_status=0; else child_status=$?; fi
  [[ "$child_status" == "137" ]] || exit 1
  SELFTEST_BOUND_DIRECT_CHILD=""
  restore_selftest_root_traps
  selftest_root_signal "$status"
}

selftest_ps_failure_cleanup() {
  local tmp="$1" status=0
  prepare_image_lock_case "$tmp" ps-failure-cleanup
  install_image_lock_case_traps
  start_image_lock_worker pre-ready-hang || die "selftest: ps-failure worker handshake failed"
  SELFTEST_FORCE_PS_FAILURE=1
  export SELFTEST_FORCE_PS_FAILURE
  set +e
  cleanup_image_lock_case
  status=$?
  set -e
  unset SELFTEST_FORCE_PS_FAILURE
  [[ "$status" == "1" && "$SELFTEST_WORKER_REAPED" == "1" ]] ||
    die "selftest: repeated ps failure did not fail closed after reaping the worker"
  bounded_process_absent "$SELFTEST_WORKER_PID" ||
    die "selftest: repeated ps failure left its signal-ignoring worker"
  assert_no_surviving_descendants ||
    die "selftest: repeated ps failure left a signal-ignoring descendant"
  fresh_lock_probe || die "selftest: repeated ps failure left the image lock held"
  clear_image_lock_case_traps
}

selftest_early_exit() {
  local tmp="$1"
  prepare_image_lock_case "$tmp" early-exit
  install_image_lock_case_traps
  start_image_lock_worker early-exit || die "selftest: early-exit handshake failed"
  reap_worker || die "selftest: early-exit child did not finish"
  [[ "$SELFTEST_REAP_STATUS" == "23" ]] || die "selftest: early-exit status drifted"
  cleanup_image_lock_case || die "selftest: early-exit cleanup failed"
  require_cleanup_receipt || die "selftest: early-exit cleanup receipt missing"
  clear_image_lock_case_traps
  write_scenario_receipt early-exit
}

selftest_pre_ready_hang() {
  local tmp="$1"
  prepare_image_lock_case "$tmp" pre-ready-hang
  install_image_lock_case_traps
  start_image_lock_worker pre-ready-hang || die "selftest: pre-ready handshake failed"
  if wait_for_status_file "$SELFTEST_CASE_DIR/ready.status" "$SELFTEST_WORKER_PID"; then
    die "selftest: pre-ready hang unexpectedly became ready"
  fi
  cleanup_image_lock_case || die "selftest: pre-ready cleanup failed"
  require_cleanup_receipt || die "selftest: pre-ready cleanup receipt missing"
  clear_image_lock_case_traps
  write_scenario_receipt pre-ready-hang
}

selftest_forced_build_contention() {
  local tmp="$1" attempt
  prepare_image_lock_case "$tmp" normal-contention
  hold_parent_lock
  if fresh_lock_probe; then
    die "selftest: fresh lock probe accepted a held lock"
  fi
  install_image_lock_case_traps
  start_image_lock_worker normal || die "selftest: normal worker handshake failed"
  wait_for_status_file "$SELFTEST_CASE_DIR/ready.status" "$SELFTEST_WORKER_PID" ||
    die "selftest: normal worker did not become ready"
  for ((attempt = 0; attempt < 20; ++attempt)); do
    [[ ! -e "$SELFTEST_CASE_DIR/build-entered.status" ]] ||
      die "selftest: forced rebuild bypassed the held lock"
    process_is_terminal "$SELFTEST_WORKER_PID" &&
      die "selftest: forced rebuild exited while the lock was held"
    sleep 0.01
  done
  release_parent_lock
  wait_for_status_file "$SELFTEST_CASE_DIR/done.status" "$SELFTEST_WORKER_PID" ||
    die "selftest: forced rebuild did not complete after release"
  reap_worker || die "selftest: normal child did not finish"
  [[ "$SELFTEST_REAP_STATUS" == "0" ]] || die "selftest: normal child failed"
  fresh_lock_probe || die "selftest: fresh lock probe failed after worker completion"
  cleanup_image_lock_case || die "selftest: normal cleanup failed"
  require_cleanup_receipt || die "selftest: normal cleanup receipt missing"
  clear_image_lock_case_traps
  write_scenario_receipt forced-build-contention
}

selftest_post_ready_hang() {
  local tmp="$1"
  prepare_image_lock_case "$tmp" post-ready-hang
  hold_parent_lock
  install_image_lock_case_traps
  start_image_lock_worker post-ready-build-hang || die "selftest: build-hang handshake failed"
  wait_for_status_file "$SELFTEST_CASE_DIR/ready.status" "$SELFTEST_WORKER_PID" ||
    die "selftest: build-hang worker did not become ready"
  release_parent_lock
  wait_for_status_file "$SELFTEST_CASE_DIR/build-entered.status" "$SELFTEST_WORKER_PID" ||
    die "selftest: build-hang worker never entered the build"
  if wait_for_status_file "$SELFTEST_CASE_DIR/done.status" "$SELFTEST_WORKER_PID"; then
    die "selftest: build-hang worker unexpectedly completed"
  fi
  cleanup_image_lock_case || die "selftest: build-hang cleanup failed"
  require_cleanup_receipt || die "selftest: build-hang cleanup receipt missing"
  clear_image_lock_case_traps
  write_scenario_receipt post-ready-hang
}

selftest_signal_controller() {
  local managed="$1" case_dir="$2" readiness_mode="${3:-normal}"
  SELFTEST_MANAGED_DIR="$managed"
  SELFTEST_CASE_DIR="$case_dir"
  RA8_IMAGE_LOCK_DIR="$managed" resolve_image_lock
  hold_parent_lock
  install_image_lock_case_traps
  SELFTEST_WORKER_SHARED_GROUP=1
  start_image_lock_worker signal-controller || die "selftest: signal worker handshake failed"
  wait_for_status_file "$case_dir/ready.status" "$SELFTEST_WORKER_PID" ||
    die "selftest: signal worker did not become ready"
  if [[ "$readiness_mode" == "delay-controller-ready" ]]; then
    while :; do sleep 1 8>&-; done
  fi
  [[ "$readiness_mode" != "hang-controller-cleanup" ]] ||
    SELFTEST_HANG_CONTROLLER_CLEANUP=1
  printf 'controller-ready\n' >"$case_dir/controller-ready.status"
  while :; do sleep 1 8>&-; done
}

run_image_lock_scenario() {
  local name="$1" scenario="$2" tmp="$3"
  "$scenario" "$tmp"
  require_scenario_receipt "$name" ||
    die "selftest: $name scenario did not produce its completion receipt"
  SELFTEST_SUITE_RECEIPTS+="$(scenario_receipt_value "$name");"
  ((SELFTEST_SUITE_COUNT += 1))
}

verify_image_lock_suite_receipts() {
  local expected
  expected="$(expected_image_lock_suite_receipts)"
  [[ "$SELFTEST_SUITE_COUNT" == "6" && "$SELFTEST_SUITE_RECEIPTS" == "$expected" ]] ||
    die "selftest: image lock suite receipt set is incomplete or forged"
}

dispatch_image_lock_selftest() {
  local command="$1"
  shift
  case "$command" in
    suite)
      command -v flock >/dev/null 2>&1 || die "selftest: managed lock requires flock"
      command -v setsid >/dev/null 2>&1 || die "selftest: managed lock requires setsid"
      run_managed_image_lock_suite "$1"
      ;;
    --selftest-image-lock-worker) image_lock_selftest_worker "$@" ;;
    --selftest-image-lock-signal-controller) selftest_signal_controller "$@" ;;
    *) die "unknown image-lock selftest dispatch '$command'" ;;
  esac
}
