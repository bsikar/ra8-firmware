#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
# Receipt and completion proofs for the authenticated image-lock selftest.

[[ "${BASH_SOURCE[0]}" != "$0" ]] || {
  printf 'error: devcontainer image lock receipt helper is source-only\n' >&2
  exit 1
}
[[ "$-" == *p* ]] || {
  printf 'error: devcontainer image lock receipt helper requires privileged Bash mode\n' >&2
  return 1
}
SELFTEST_LOCK_RECEIPT_PARENT="${DEVCONTAINER_SELFTEST_PARENT:-missing}"
SELFTEST_LOCK_RECEIPT_PARENT_DIR="$(cd -P "$(dirname "$SELFTEST_LOCK_RECEIPT_PARENT")" 2>/dev/null && pwd)" || return 1
[[ -n "${DEVCONTAINER_SELFTEST_SOURCE_DIR:-}" &&
  "$DEVCONTAINER_SELFTEST_SOURCE_DIR" == "$SELFTEST_LOCK_RECEIPT_PARENT_DIR" &&
  "${DEVCONTAINER_SELFTEST_PARENT:-}" == "$SELFTEST_LOCK_RECEIPT_PARENT_DIR/devcontainer_image.sh" &&
  "${BASH_SOURCE[1]:-missing}" -ef "$SELFTEST_LOCK_RECEIPT_PARENT" ]] || {
  printf 'error: devcontainer image lock receipt helper has an unauthorized parent\n' >&2
  return 1
}
unset -v DEVCONTAINER_SELFTEST_PARENT DEVCONTAINER_SELFTEST_SOURCE_DIR
unset -v DEVCONTAINER_SELFTEST_SCRIPT_DIR DEVCONTAINER_SELFTEST_REPO_ROOT
unset -v DEVCONTAINER_SELFTEST_LABEL_KEY SELFTEST_LOCK_RECEIPT_PARENT
unset -v SELFTEST_LOCK_RECEIPT_PARENT_DIR

# The lock helper loaded next establishes these values before any receipt call.
SELFTEST_CASE_DIR="${SELFTEST_CASE_DIR-}"
SELFTEST_CLEANUP_RECEIPT="${SELFTEST_CLEANUP_RECEIPT-}"
SELFTEST_FILE_RECEIPTS_VERIFIED="${SELFTEST_FILE_RECEIPTS_VERIFIED-}"
SELFTEST_FORCE_CLEANUP_RECEIPT="${SELFTEST_FORCE_CLEANUP_RECEIPT-}"
SELFTEST_PARENT_LOCK_OPEN="${SELFTEST_PARENT_LOCK_OPEN-}"
SELFTEST_RECEIPT_DIR="${SELFTEST_RECEIPT_DIR-}"

expected_image_lock_suite_receipts() {
  printf '%s' \
    'scenario:early-exit:complete;' \
    'scenario:pre-ready-hang:complete;' \
    'scenario:forced-build-contention:complete;' \
    'scenario:post-ready-hang:complete;' \
    'scenario:signal-ready-timeout:complete;' \
    'scenario:signal-cleanup:complete;'
}

scenario_receipt_value() {
  printf 'scenario:%s:complete' "$1"
}

validate_scenario_receipt_directory() {
  [[ -n "$SELFTEST_RECEIPT_DIR" && -d "$SELFTEST_RECEIPT_DIR" &&
    ! -L "$SELFTEST_RECEIPT_DIR" ]] || return 1
  [[ "$(file_mode "$SELFTEST_RECEIPT_DIR")" == "700" ]] || return 1
  [[ "$(file_owner_id "$SELFTEST_RECEIPT_DIR")" == "$(id -u)" ]]
}

write_scenario_receipt() {
  local name="$1" target temporary receipt
  [[ "$name" =~ ^[a-z][a-z0-9-]*$ ]] || return 1
  validate_scenario_receipt_directory || return 1
  target="$SELFTEST_RECEIPT_DIR/$name.receipt"
  [[ ! -e "$target" && ! -L "$target" ]] || return 1
  receipt="$(scenario_receipt_value "$name")"
  temporary="$target.pending"
  [[ ! -e "$temporary" && ! -L "$temporary" ]] || return 1
  (umask 077 && printf '%s\n' "$receipt" >"$temporary") || return 1
  mv -- "$temporary" "$target" || return 1
  [[ -f "$target" && ! -L "$target" && "$(file_link_count "$target")" == "1" ]]
}

require_scenario_receipt() {
  local name="$1" target expected value size
  target="$SELFTEST_RECEIPT_DIR/$name.receipt"
  expected="$(scenario_receipt_value "$name")"
  [[ -f "$target" && ! -L "$target" && "$(file_link_count "$target")" == "1" ]] || return 1
  IFS= read -r value <"$target" || return 1
  size="$(wc -c <"$target")" || return 1
  [[ "$value" == "$expected" && "$size" == "$((${#expected} + 1))" ]]
}

verify_scenario_receipt_files() {
  local name actual="" seen=";" count=0
  SELFTEST_FILE_RECEIPTS_VERIFIED=0
  [[ "$#" == "6" ]] || return 1
  validate_scenario_receipt_directory || return 1
  for name in "$@"; do
    [[ "$seen" != *";$name;"* ]] || return 1
    seen+="$name;"
    require_scenario_receipt "$name" || return 1
  done
  while IFS= read -r name; do
    [[ -n "$name" ]] || continue
    actual+="$name;"
    ((count += 1))
  done < <(find "$SELFTEST_RECEIPT_DIR" -mindepth 1 -maxdepth 1 -printf '%f\n' | LC_ALL=C sort)
  [[ "$count" == "6" ]] || return 1
  [[ "$actual" == 'early-exit.receipt;forced-build-contention.receipt;post-ready-hang.receipt;pre-ready-hang.receipt;signal-cleanup.receipt;signal-ready-timeout.receipt;' ]] || return 1
  SELFTEST_FILE_RECEIPTS_VERIFIED=1
  [[ "$SELFTEST_FILE_RECEIPTS_VERIFIED" == "1" ]]
}

expected_cleanup_receipt() {
  printf 'cleanup:%s:complete' "$SELFTEST_CASE_DIR"
}

expected_force_cleanup_receipt() {
  printf 'force-cleanup:%s:complete' "$1"
}

parent_lock_fd_is_closed() {
  [[ "$SELFTEST_PARENT_LOCK_OPEN" == "0" ]] || return 1
  if (: <&8) 2>/dev/null; then
    return 1
  fi
}

require_cleanup_receipt() {
  [[ "$SELFTEST_CLEANUP_RECEIPT" == "$(expected_cleanup_receipt)" ]]
}

require_force_cleanup_receipt() {
  local case_dir="$1"
  [[ "$SELFTEST_FORCE_CLEANUP_RECEIPT" == "$(expected_force_cleanup_receipt "$case_dir")" ]]
}

write_worker_cleanup_proof_file() {
  local target="$SELFTEST_CASE_DIR/worker-cleanup.proof" temporary
  require_cleanup_receipt || return 1
  temporary="$target.pending"
  [[ ! -e "$temporary" && ! -L "$temporary" ]] || return 1
  printf '%s\n' "$SELFTEST_CLEANUP_RECEIPT" >"$temporary" || return 1
  mv -- "$temporary" "$target"
}

require_worker_cleanup_proof_file() {
  local case_dir="$1" target="$1/worker-cleanup.proof" expected value size
  expected="cleanup:$case_dir:complete"
  [[ -f "$target" && ! -L "$target" ]] || return 1
  IFS= read -r value <"$target" || return 1
  size="$(wc -c <"$target")" || return 1
  [[ "$value" == "$expected" && "$size" == "$((${#expected} + 1))" ]]
}

write_controller_cleanup_receipt_file() {
  local case_dir="$1" target="$1/controller-cleanup.receipt" temporary receipt
  receipt="controller-cleanup:$case_dir:complete"
  temporary="$target.pending"
  [[ ! -e "$temporary" && ! -L "$temporary" ]] || return 1
  printf '%s\n' "$receipt" >"$temporary" || return 1
  mv -- "$temporary" "$target"
}

require_controller_cleanup_receipt_file() {
  local case_dir="$1" target="$1/controller-cleanup.receipt" expected value size
  expected="controller-cleanup:$case_dir:complete"
  [[ -f "$target" && ! -L "$target" ]] || return 1
  IFS= read -r value <"$target" || return 1
  size="$(wc -c <"$target")" || return 1
  [[ "$value" == "$expected" && "$size" == "$((${#expected} + 1))" ]]
}
