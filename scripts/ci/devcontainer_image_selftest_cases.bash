#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
# Adversarial cases for devcontainer_image.sh; sourced by its approved loader.

[[ "${BASH_SOURCE[0]}" != "$0" && "$-" == *p* ]] || {
  printf "error: devcontainer image selftest cases are source-only\n" >&2
  exit 1
}
SELFTEST_CASES_PARENT="${DEVCONTAINER_SELFTEST_PARENT:-missing}"
SELFTEST_CASES_PARENT_DIR="$(cd -P "$(dirname "$SELFTEST_CASES_PARENT")" 2>/dev/null && pwd)" || return 1
[[ -n "${DEVCONTAINER_SELFTEST_SOURCE_DIR:-}" &&
  "$DEVCONTAINER_SELFTEST_SOURCE_DIR" == "$SELFTEST_CASES_PARENT_DIR" &&
  "${DEVCONTAINER_SELFTEST_PARENT:-}" == "$SELFTEST_CASES_PARENT_DIR/devcontainer_image.sh" &&
  "${BASH_SOURCE[1]:-missing}" -ef "$SELFTEST_CASES_PARENT" ]] || return 1
unset -v DEVCONTAINER_SELFTEST_PARENT DEVCONTAINER_SELFTEST_SOURCE_DIR
unset -v SELFTEST_CASES_PARENT SELFTEST_CASES_PARENT_DIR

SCRIPT_DIR="${DEVCONTAINER_SELFTEST_SCRIPT_DIR:?}"
REPO_ROOT="${DEVCONTAINER_SELFTEST_REPO_ROOT:?}"
LABEL_KEY="${DEVCONTAINER_SELFTEST_LABEL_KEY:?}"
unset -v DEVCONTAINER_SELFTEST_SCRIPT_DIR DEVCONTAINER_SELFTEST_REPO_ROOT
unset -v DEVCONTAINER_SELFTEST_LABEL_KEY
SELFTEST_BOUND_EXIT_NESTED="${SELFTEST_BOUND_EXIT_NESTED:?}"

# These values are populated by the approved image-lock helper before the
# cases that consume them. Preserve any inherited values while making that
# cross-helper contract explicit to standalone static analysis.
IMAGE_LOCK_FILE="${IMAGE_LOCK_FILE-}"
IMAGE_LOCK_IDENTITY="${IMAGE_LOCK_IDENTITY-}"
IMAGE_LOCK_MANAGED="${IMAGE_LOCK_MANAGED-}"
declare -a RUNTIME
declare SELFTEST_IMAGE_NAMESPACE
declare SELFTEST_DISPATCH_COMPLETE
declare IMAGE_LOCK_RECEIPTS_RAW_SHA256
declare IMAGE_LOCK_SELFTEST_RAW_SHA256
declare SELFTEST_SUITE_ANCHOR
declare SELFTEST_SUITE_ANCHOR_IDENTITY
declare SELFTEST_SUITE_ANCHOR_OWNER_UID
declare SELFTEST_SUITE_ROOT
declare SELFTEST_SUITE_ROOT_IDENTITY
SELFTEST_BOUND_ALLOCATION_CHILD=""
SELFTEST_BOUND_ALLOCATION_PATH=""
SELFTEST_BOUND_ALLOCATION_IDENTITY=""

selftest_allocation_failure_path() {
  local tmp="$1" receipt="$1/selftest-allocation-failure-path" path status
  if (
    reset_selftest_tmp_state
    configure_selftest_allocation_checkpoint fail "$receipt"
    begin_selftest_tmp
  ); then status=0; else status=$?; fi
  [[ "$status" == "23" ]] ||
    die "selftest: allocation failure cleanup returned $status, expected 23"
  IFS= read -r path <"$receipt" || die "selftest: allocation failure path is missing"
  assert_selftest_tmp_absent "$path" "allocation failure path"
}

selftest_allocation_nonempty_refusal() {
  local tmp="$1" receipt="$1/selftest-allocation-nonempty-path" path status
  if (
    reset_selftest_tmp_state
    configure_selftest_allocation_checkpoint nonempty "$receipt"
    begin_selftest_tmp
  ); then status=0; else status=$?; fi
  [[ "$status" == "1" ]] ||
    die "selftest: nonempty pending cleanup returned $status, expected 1"
  IFS= read -r path <"$receipt" || die "selftest: nonempty pending path is missing"
  [[ -f "$path/sentinel" ]] || die "selftest: pending cleanup removed replacement content"
  rm "$path/sentinel"
  rmdir "$path"
}

selftest_one_allocation_signal() {
  local tmp="$1" phase="$2" receipt="$1/selftest-allocation-$2-path" path status
  if /bin/bash -p -- "$SCRIPT_DIR/devcontainer_image.sh" \
    --selftest-allocation-checkpoint-child "$phase" "$receipt" \
    "$tmp" "$SELFTEST_TMP_IDENTITY" "$SELFTEST_SUITE_ANCHOR" \
    "$SELFTEST_SUITE_ANCHOR_IDENTITY" "$SELFTEST_SUITE_ANCHOR_OWNER_UID"; then
    status=0
  else
    status=$?
  fi
  [[ "$status" == "143" ]] ||
    die "selftest: allocation $phase signal returned $status, expected 143"
  IFS= read -r path <"$receipt" || die "selftest: allocation $phase path is missing"
  assert_selftest_tmp_absent "$path" "allocation $phase signal path"
}

selftest_allocation_signal_child() {
  local ready="$1" parent="$2" parent_identity="$3" target="$4" target_identity="$5"
  local anchor="$6" anchor_identity="$7" anchor_owner_uid="$8"
  configure_selftest_suite_authority "$parent" "$parent_identity" \
    "$anchor" "$anchor_identity" "$anchor_owner_uid" ||
    die "selftest: allocation kill child suite authority is unsafe"
  configure_nested_selftest_tmp "$parent" "$parent_identity" "$target" \
    "$target_identity" || die "selftest: allocation kill child root is unsafe"
  [[ "$ready" == "$parent/selftest-allocation-kill-ready" && ! -e "$ready" &&
    ! -L "$ready" ]] || die "selftest: allocation kill receipt is unsafe"
  printf 'ready\n' >"$ready"
  while :; do sleep 1; done
}

cleanup_allocation_signal_child() {
  local child="$1" path="$2" identity="$3" status namespace="$SELFTEST_IMAGE_NAMESPACE"
  signal_owned_live_child KILL "$child" ||
    die "selftest: allocation kill child lost direct-child authority"
  SELFTEST_BOUND_ALLOCATION_CHILD=""
  SELFTEST_BOUND_ALLOCATION_PATH=""
  SELFTEST_BOUND_ALLOCATION_IDENTITY=""
  restore_selftest_root_traps
  if wait "$child"; then status=0; else status=$?; fi
  [[ "$status" == "137" ]] ||
    die "selftest: allocation kill returned $status, expected 137"
  ! shell_owns_live_child "$child" ||
    die "selftest: stale allocation child PID retained signal authority"
  nested_selftest_metadata_is_safe "$SELFTEST_TMP_DIR" "$SELFTEST_TMP_IDENTITY" \
    "$path" "$identity" || die "selftest: allocation kill root became unowned"
  configure_nested_selftest_tmp "$SELFTEST_TMP_DIR" "$SELFTEST_TMP_IDENTITY" \
    "$path" "$identity"
  remove_nested_selftest_tmp || die "selftest: parent could not clean allocation kill root"
  SELFTEST_IMAGE_NAMESPACE="$namespace"
  assert_selftest_tmp_absent "$path" "allocation kill path"
}

allocation_bound_spawn_signal() {
  local status="$1"
  trap - EXIT HUP INT TERM
  cleanup_allocation_signal_child "$SELFTEST_BOUND_ALLOCATION_CHILD" \
    "$SELFTEST_BOUND_ALLOCATION_PATH" "$SELFTEST_BOUND_ALLOCATION_IDENTITY"
  selftest_root_signal "$status"
}

selftest_allocation_signal_path() {
  local tmp="$1" receipt="$1/selftest-allocation-kill-path"
  local ready="$1/selftest-allocation-kill-ready" child path identity attempt pending
  local supervisor='import os, signal, sys
for value in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
    signal.signal(value, signal.SIG_DFL)
os.execv("/bin/bash", ["/bin/bash", "-p", "--", sys.argv[1],
    "--selftest-allocation-signal-child", *sys.argv[2:]])'
  path="$(mktemp -d "$tmp/ra8-devcontainer-image-selftest.XXXXXXXXXX")"
  identity="$(file_identity "$path")"
  printf '%s\n' "$path" >"$receipt"
  begin_selftest_spawn_critical allocation_bound_spawn_signal ||
    die "selftest: allocation spawn guard did not start"
  /usr/bin/python3 -B -I -S -c "$supervisor" "$SCRIPT_DIR/devcontainer_image.sh" \
    "$ready" "$tmp" "$SELFTEST_TMP_IDENTITY" "$path" "$identity" \
    "$SELFTEST_SUITE_ANCHOR" "$SELFTEST_SUITE_ANCHOR_IDENTITY" \
    "$SELFTEST_SUITE_ANCHOR_OWNER_UID" &
  child=$!
  SELFTEST_BOUND_ALLOCATION_CHILD="$child"
  SELFTEST_BOUND_ALLOCATION_PATH="$path"
  SELFTEST_BOUND_ALLOCATION_IDENTITY="$identity"
  finish_selftest_spawn_critical pending ||
    die "selftest: allocation child was not bound"
  [[ "$pending" == "0" ]] || allocation_bound_spawn_signal "$pending"
  selftest_inject_bound_exit allocation "$child"
  for ((attempt = 0; attempt < 500; ++attempt)); do
    [[ -s "$ready" ]] && break
    kill -0 "$child" 2>/dev/null || break
    sleep 0.01
  done
  [[ -s "$ready" ]] || allocation_bound_spawn_signal 1
  IFS= read -r path <"$receipt" || die "selftest: allocation kill path is missing"
  cleanup_allocation_signal_child "$child" "$path" "$identity"
}

selftest_temp_root_is_safe() {
  if [[ "$SELFTEST_BOUND_EXIT_NESTED" == "0" ]]; then
    [[ "$SELFTEST_TMP_ROOT" == "$(cd -P /tmp && pwd)" &&
    "$(file_special_mode "$SELFTEST_TMP_ROOT")" == "1777" ]]
    return
  fi
  [[ "$SELFTEST_TMP_ROOT" == "$SELFTEST_SUITE_ROOT" &&
    "$SELFTEST_TMP_ROOT_IDENTITY" == "$SELFTEST_SUITE_ROOT_IDENTITY" ]] &&
    selftest_suite_root_is_safe
}

selftest_temp_exit_paths() {
  local tmp="$1" receipt="$1/selftest-cleanup-path" path status current_shell
  set_selftest_shell_identity current_shell
  [[ "$current_shell" =~ ^[0-9]+:[0-9]+$ &&
    "$current_shell" == "$$:${BASH_SUBSHELL:-0}" ]] ||
    die "selftest: portable shell process identity disagrees"
  selftest_temp_root_is_safe || die "selftest: allocation parent authority is unsafe"
  mkdir -m 1750 "$tmp/special-mode-probe"
  [[ "$(file_special_mode "$tmp/special-mode-probe")" == "1750" ]] ||
    die "selftest: special-mode helper dropped permission bits"
  rmdir "$tmp/special-mode-probe"
  selftest_root_metadata_directions "$tmp" ||
    die "selftest: root metadata directions failed"
  selftest_spawn_critical_directions ||
    die "selftest: spawn critical signal directions failed"
  selftest_allocation_failure_path "$tmp"
  selftest_allocation_nonempty_refusal "$tmp"
  selftest_one_allocation_signal "$tmp" precreate
  selftest_one_allocation_signal "$tmp" created
  selftest_allocation_signal_path "$tmp"
  if (
    reset_selftest_tmp_state
    begin_selftest_tmp
    printf '%s\n' "$SELFTEST_TMP_DIR" >"$receipt"
    exit 23
  ); then
    status=0
  else
    status=$?
  fi
  [[ "$status" == "23" ]] || die "selftest: failure cleanup returned $status, expected 23"
  IFS= read -r path <"$receipt" || die "selftest: failure cleanup path is missing"
  assert_selftest_tmp_absent "$path" "failure path"
  mkdir "$tmp/hostile-tmp"
  (
    reset_selftest_tmp_state
    TMPDIR="$tmp/hostile-tmp" begin_selftest_tmp
    printf '%s\n' "$SELFTEST_TMP_DIR" >"$receipt"
    finish_selftest_tmp || die "selftest: successful temporary-directory cleanup failed"
  ) || die "selftest: successful temporary-directory cleanup failed"
  IFS= read -r path <"$receipt" || die "selftest: success cleanup path is missing"
  [[ "$path" != "$tmp/hostile-tmp"/* ]] || die "selftest: hostile TMPDIR selected the root"
  assert_selftest_tmp_absent "$path" "success path"
}

selftest_finish_retry() {
  local tmp="$1" receipt="$1/finish-retry-path" state="$1/finish-retry-state"
  local runtime="$1/finish-retry-runtime" path status
  cat >"$runtime" <<'EOF'
#!/bin/bash
set -euo pipefail
case "${1:-}:${2:-}" in
info:) exit 0 ;;
image:inspect)
  [[ -f "${RA8_SELFTEST_RETRY_STATE:?}" ]] ;;
rmi:-f)
  count=0
  [[ ! -f "${RA8_SELFTEST_RETRY_COUNT:?}" ]] || IFS= read -r count <"$RA8_SELFTEST_RETRY_COUNT"
  count=$((count + 1))
  printf '%s\n' "$count" >"$RA8_SELFTEST_RETRY_COUNT"
  [[ "$count" != "1" ]] || exit 65
  rm -f -- "$RA8_SELFTEST_RETRY_STATE"
  ;;
*) exit 64 ;;
esac
EOF
  chmod 0700 "$runtime"
  if (
    reset_selftest_tmp_state
    begin_selftest_tmp
    path="$SELFTEST_TMP_DIR"
    printf '%s\n' "$path" >"$receipt"
    set_selftest_runtime "$runtime"
    set_selftest_image_tracking \
      "$SELFTEST_IMAGE_NAMESPACE" "${SELFTEST_IMAGE_NAMESPACE}-labelled"
    RA8_SELFTEST_RETRY_STATE="$state"
    RA8_SELFTEST_RETRY_COUNT="$tmp/finish-retry-count"
    export RA8_SELFTEST_RETRY_STATE RA8_SELFTEST_RETRY_COUNT
    printf '%s\n' labelled >"$state"
    if finish_selftest_tmp; then status=0; else status=$?; fi
    [[ "$status" == "1" ]] || exit 66
    exit "$status"
  ); then
    status=0
  else
    status=$?
  fi
  [[ "$status" == "1" ]] || die "selftest: finish cleanup retry returned $status, expected 1"
  IFS= read -r path <"$receipt" || die "selftest: finish retry path is missing"
  assert_selftest_tmp_absent "$path" "finish retry path"
  [[ ! -e "$state" ]] || die "selftest: finish retry left image state"
}

selftest_one_root_signal() {
  local tmp="$1" signal="$2" expected="$3" receipt status path tag identity
  local fake_runtime="$tmp/fake-image-runtime"
  local fake_state="$tmp/fake-image-state.$signal"
  local call_log="$tmp/fake-image-calls.signal-$signal"
  receipt="$tmp/selftest-cleanup-$signal"
  : >"$call_log"
  path="$(mktemp -d "$tmp/ra8-devcontainer-image-selftest.XXXXXXXXXX")"
  identity="$(file_identity "$path")"
  printf '%s\n' "$path" >"$receipt"
  RA8_SELFTEST_FAKE_CALL_LOG="$call_log"
  export RA8_SELFTEST_FAKE_CALL_LOG
  if RA8_SELFTEST_FAKE_IMAGE_STATE="$fake_state" \
    /bin/bash -p "$SCRIPT_DIR/devcontainer_image.sh" \
    --selftest-root-signal-child "$receipt" "$fake_runtime" "$signal" \
    "$tmp" "$SELFTEST_TMP_IDENTITY" "$path" "$identity" \
    "$SELFTEST_SUITE_ANCHOR" "$SELFTEST_SUITE_ANCHOR_IDENTITY" \
    "$SELFTEST_SUITE_ANCHOR_OWNER_UID"; then
    status=0
  else
    status=$?
  fi
  [[ "$status" == "$expected" ]] ||
    die "selftest: $signal cleanup returned $status, expected $expected"
  IFS= read -r path <"$receipt" || die "selftest: $signal cleanup path is missing"
  assert_selftest_tmp_absent "$path" "$signal path"
  [[ ! -e "$fake_state" ]] || die "selftest: $signal cleanup left its temporary image"
  tag="ra8-ci-selftest:$(printf '%s' "${path##*.}" | tr '[:upper:]' '[:lower:]')-labelled"
  [[ "$(grep -cFx "rmi -f $tag" "$call_log")" == "1" &&
  "$(grep -c '^rmi ' "$call_log")" == "1" ]] ||
    die "selftest: $signal cleanup did not remove its temporary image"
  unset RA8_SELFTEST_FAKE_CALL_LOG
}

selftest_temp_signal_cleanup() {
  local tmp="$1" signal expected
  cat >"$tmp/fake-image-runtime" <<'EOF'
#!/bin/bash
set -euo pipefail
if [[ -n "${RA8_SELFTEST_FAKE_CALL_LOG:-}" ]]; then
  printf '%s\n' "$*" >>"$RA8_SELFTEST_FAKE_CALL_LOG"
fi
state_tag() {
  [[ -f "${RA8_SELFTEST_FAKE_IMAGE_STATE:?}" ]] || return 1
  IFS= read -r result <"$RA8_SELFTEST_FAKE_IMAGE_STATE"
  printf '%s\n' "$result"
}
case "${1:-}:${2:-}" in
  info:) [[ "${RA8_SELFTEST_FAKE_RUNTIME_BROKEN:-0}" == "0" ]] ;;
  image:inspect) [[ "$(state_tag)" == "${3:?}" ]] ;;
  rmi:-f)
    [[ "$(state_tag)" == "${3:?}" ]] || exit 65
    [[ "${RA8_SELFTEST_FAKE_RMI_NOOP:-0}" == "1" ]] || rm -f -- "$RA8_SELFTEST_FAKE_IMAGE_STATE"
    ;;
  build:*)
    previous=""
    tag=""
    for argument in "$@"; do
      [[ "$previous" == "-t" ]] && tag="$argument"
      previous="$argument"
    done
    [[ -n "$tag" ]] || exit 66
    printf '%s\n' "$tag" >"${RA8_SELFTEST_FAKE_IMAGE_STATE:?}"
    exit "${RA8_SELFTEST_FAKE_BUILD_STATUS:-0}"
    ;;
  *) exit 64 ;;
esac
EOF
  chmod 0700 "$tmp/fake-image-runtime"
  for signal in HUP INT TERM; do
    case "$signal" in HUP) expected=129 ;; INT) expected=130 ;; TERM) expected=143 ;; esac
    selftest_one_root_signal "$tmp" "$signal" "$expected"
  done
}

selftest_cleanup_path_refusal() {
  local mode="$1" valid="$2" identity="$3" root="$4" wrong=""
  case "$mode" in
    empty) SELFTEST_TMP_DIR="" ;;
    relative) SELFTEST_TMP_DIR="relative-selftest-path" ;;
    slash) SELFTEST_TMP_DIR="/" ;;
    root-target) SELFTEST_TMP_DIR="$root" ;;
    prefix)
      wrong="$(mktemp -d "$valid/not-ra8-selftest.XXXXXXXXXX")"
      SELFTEST_TMP_DIR="$wrong"
      SELFTEST_TMP_IDENTITY="$(file_identity "$wrong")"
      ;;
    *) return 2 ;;
  esac
  if cleanup_selftest_tmp >/dev/null 2>&1; then return 1; fi
  [[ -z "$wrong" ]] || rmdir "$wrong"
  SELFTEST_TMP_DIR="$valid"
  SELFTEST_TMP_IDENTITY="$identity"
}

selftest_cleanup_metadata_refusal() {
  local mode="$1" valid="$2" root="$3" root_identity="$4" owner_uid="$5" backup
  case "$mode" in
    mode) chmod 0755 "$valid" ;;
    root-identity) SELFTEST_TMP_ROOT_IDENTITY="0:0" ;;
    root-path) SELFTEST_TMP_ROOT="/" ;;
    owner) SELFTEST_TMP_OWNER_UID="$((owner_uid + 1))" ;;
    symlink | inode)
      backup="$valid.saved"
      mv "$valid" "$backup"
      if [[ "$mode" == "symlink" ]]; then ln -s "$backup" "$valid"; else mkdir -m 0700 "$valid"; fi
      ;;
    *) return 2 ;;
  esac
  if cleanup_selftest_tmp >/dev/null 2>&1; then return 1; fi
  case "$mode" in
    mode) chmod 0700 "$valid" ;;
    root-identity) SELFTEST_TMP_ROOT_IDENTITY="$root_identity" ;;
    root-path) SELFTEST_TMP_ROOT="$root" ;;
    owner) SELFTEST_TMP_OWNER_UID="$owner_uid" ;;
    symlink | inode)
      if [[ -L "$valid" ]]; then rm "$valid"; else rmdir "$valid"; fi
      mv "$backup" "$valid"
      ;;
  esac
}

selftest_cleanup_refusal() {
  local mode="$1" outer="$2" valid identity root victim status=0
  reset_selftest_tmp_state
  begin_selftest_tmp
  valid="$SELFTEST_TMP_DIR"
  identity="$SELFTEST_TMP_IDENTITY"
  root="$SELFTEST_TMP_ROOT"
  if [[ "$mode" == "nested-symlink" ]]; then
    victim="$(mktemp -d "$outer/nested-symlink-victim.XXXXXXXXXX")"
    printf 'keep\n' >"$victim/keep"
    ln -s "$victim" "$valid/nested-link"
    finish_selftest_tmp || die "selftest: nested-symlink cleanup failed closed"
    [[ -f "$victim/keep" ]] || die "selftest: cleanup followed a nested symlink"
    rm "$victim/keep"
    rmdir "$victim"
    return
  fi
  selftest_cleanup_path_refusal "$mode" "$valid" "$identity" "$root" || status=$?
  if [[ "$status" == "1" ]]; then
    die "selftest: cleanup accepted $mode replacement"
  elif [[ "$status" == "2" ]]; then
    selftest_cleanup_metadata_refusal "$mode" "$valid" "$root" \
      "$SELFTEST_TMP_ROOT_IDENTITY" "$SELFTEST_TMP_OWNER_UID" ||
      die "selftest: cleanup accepted $mode replacement"
  fi
  finish_selftest_tmp || die "selftest: cleanup recovery failed"
}

selftest_temp_cleanup() {
  local tmp="$1" mode
  selftest_temp_exit_paths "$tmp"
  selftest_finish_retry "$tmp"
  selftest_temp_signal_cleanup "$tmp"
  for mode in empty relative slash root-target prefix mode root-identity root-path owner symlink inode nested-symlink; do
    (selftest_cleanup_refusal "$mode" "$tmp") || die "selftest: $mode cleanup case failed"
  done
}

selftest_image_failure_cleanup() {
  local tmp="$1" receipt="$1/image-failure-cleanup-path" path state status
  state="$tmp/fake-image-state.failure"
  if (
    reset_selftest_tmp_state
    begin_selftest_tmp
    printf '%s\n' "$SELFTEST_TMP_DIR" >"$receipt"
    set_selftest_runtime "$tmp/fake-image-runtime"
    RA8_CONTAINER_RUNTIME="$tmp/fake-image-runtime"
    RA8_SELFTEST_FAKE_IMAGE_STATE="$state"
    RA8_SELFTEST_FAKE_BUILD_STATUS=23
    export RA8_CONTAINER_RUNTIME RA8_SELFTEST_FAKE_IMAGE_STATE RA8_SELFTEST_FAKE_BUILD_STATUS
    selftest_round_trip ignored "${SELFTEST_IMAGE_NAMESPACE}-labelled" deadbeef
  ); then
    status=0
  else
    status=$?
  fi
  [[ "$status" == "23" ]] || die "selftest: failed image build returned $status, expected 23"
  IFS= read -r path <"$receipt" || die "selftest: image failure cleanup path is missing"
  assert_selftest_tmp_absent "$path" "image failure path"
  [[ ! -e "$state" ]] || die "selftest: failed build left its temporary image"
}

selftest_image_runtime_refusals() {
  local tmp="$1" state mode path identity refused
  state="$tmp/fake-image-state.refusal"
  for mode in no-op-rmi broken-runtime; do
    (
      reset_selftest_tmp_state
      begin_selftest_tmp
      path="$SELFTEST_TMP_DIR"
      identity="$SELFTEST_TMP_IDENTITY"
      set_selftest_runtime "$tmp/fake-image-runtime"
      RA8_SELFTEST_FAKE_IMAGE_STATE="$state"
      export RA8_SELFTEST_FAKE_IMAGE_STATE
      set_selftest_image_tracking "$SELFTEST_IMAGE_NAMESPACE" \
        "${SELFTEST_IMAGE_NAMESPACE}-labelled"
      printf '%s\n' "${SELFTEST_IMAGE_NAMESPACE}-labelled" >"$state"
      if [[ "$mode" == "no-op-rmi" ]]; then
        RA8_SELFTEST_FAKE_RMI_NOOP=1
        export RA8_SELFTEST_FAKE_RMI_NOOP
      else
        rm -f -- "$state"
        RA8_SELFTEST_FAKE_RUNTIME_BROKEN=1
        export RA8_SELFTEST_FAKE_RUNTIME_BROKEN
      fi
      if cleanup_selftest_tmp >/dev/null 2>&1; then refused=0; else refused=1; fi
      [[ "$refused" == "1" && -d "$path" &&
        "$(file_identity "$path")" == "$identity" ]] ||
        die "selftest: $mode did not fail closed before filesystem cleanup"
      unset RA8_SELFTEST_FAKE_RMI_NOOP RA8_SELFTEST_FAKE_RUNTIME_BROKEN
      finish_selftest_tmp || die "selftest: $mode image cleanup recovery failed"
      assert_selftest_tmp_absent "$path" "$mode refusal path"
      [[ ! -e "$state" ]] || die "selftest: $mode recovery left its image state"
    ) || die "selftest: $mode image-cleanup refusal case failed"
  done
}

selftest_image_tag_refusals() {
  local tmp="$1" state mode path identity refused namespace log
  for mode in empty-namespace regex-namespace foreign-tag wrong-suffix metachar-tag; do
    (
      reset_selftest_tmp_state
      begin_selftest_tmp
      path="$SELFTEST_TMP_DIR"
      identity="$SELFTEST_TMP_IDENTITY"
      namespace="$SELFTEST_IMAGE_NAMESPACE"
      state="$tmp/fake-image-state.$mode"
      log="$tmp/fake-image-calls.$mode"
      printf '%s\n' "${namespace}-labelled" >"$state"
      : >"$log"
      set_selftest_runtime "$tmp/fake-image-runtime"
      RA8_SELFTEST_FAKE_IMAGE_STATE="$state"
      RA8_SELFTEST_FAKE_CALL_LOG="$log"
      export RA8_SELFTEST_FAKE_IMAGE_STATE RA8_SELFTEST_FAKE_CALL_LOG
      case "$mode" in
        empty-namespace)
          set_selftest_image_tracking "" "${namespace}-labelled"
          ;;
        regex-namespace) set_selftest_image_tracking ".*" "anything-labelled" ;;
        foreign-tag) set_selftest_image_tracking "$namespace" "ra8-ci:latest" ;;
        wrong-suffix) set_selftest_image_tracking "$namespace" "${namespace}-unknown" ;;
        metachar-tag) set_selftest_image_tracking "$namespace" "${namespace}-labelled/../foreign" ;;
      esac
      if cleanup_selftest_tmp >/dev/null 2>&1; then refused=0; else refused=1; fi
      [[ "$refused" == "1" && -d "$path" &&
        "$(file_identity "$path")" == "$identity" &&
        "$(grep -c '^rmi ' "$log" || true)" == "0" ]] ||
        die "selftest: $mode image tag reached destructive cleanup"
      set_selftest_image_tracking "$namespace" "${namespace}-labelled"
      finish_selftest_tmp || die "selftest: $mode tag cleanup recovery failed"
      assert_selftest_tmp_absent "$path" "$mode tag-refusal path"
      [[ ! -e "$state" ]] || die "selftest: $mode tag recovery left its image state"
    ) || die "selftest: $mode image-tag refusal case failed"
  done
}

selftest_image_cleanup_refusals() {
  selftest_image_runtime_refusals "$1"
  selftest_image_tag_refusals "$1"
}

selftest_round_trip() {
  local destination="$1" tag="$2" label="$3" tmp digest_value
  [[ "$SELFTEST_IMAGE_NAMESPACE" =~ ^ra8-ci-selftest:[0-9a-f]{32}$ &&
    ("$tag" == "${SELFTEST_IMAGE_NAMESPACE}-labelled" ||
    "$tag" == "${SELFTEST_IMAGE_NAMESPACE}-bare") ]] ||
    die "selftest: unsafe temporary image tag: $tag"
  if ! temporary_image_absent "$tag"; then
    die "selftest: temporary image tag already exists: $tag"
  fi
  SELFTEST_IMAGE_TAGS+=("$tag")
  tmp="$(mktemp -d "$SELFTEST_TMP_DIR/round-trip.XXXXXXXXXX")"
  printf 'FROM scratch\nLABEL org.ra8.selftest="1"\n' >"$tmp/Dockerfile"
  local -a label_arg=()
  [[ -n "$label" ]] && label_arg=(--label "$LABEL_KEY=$label")
  local build_status=0
  if "${RUNTIME[@]}" build "${label_arg[@]}" -t "$tag" \
    -f "$tmp/Dockerfile" "$tmp" >/dev/null; then
    :
  else
    build_status=$?
    return "$build_status"
  fi
  digest_value="$(image_digest "$tag")"
  "${RUNTIME[@]}" rmi -f "$tag" >/dev/null 2>&1 ||
    die "selftest: could not remove temporary image: $tag"
  if ! temporary_image_absent "$tag"; then
    die "selftest: temporary image still exists after removal: $tag"
  fi
  SELFTEST_IMAGE_TAGS=("${SELFTEST_IMAGE_TAGS[@]:1}")
  printf -v "$destination" '%s' "$digest_value"
}

# Prove context-digest separation and the runtime image-label round trip.
selftest_content_changes() {
  local tmp="$1" base="$2" file saved changed
  for file in pyproject.toml uv.lock scripts/dev/bootstrap_uv.py scripts/dev/bootstrap_uv_exec.py scripts/dev/managed_python_env.py scripts/dev/managed_python_env_checks.py scripts/dev/uv_release.json .devcontainer/zshrc; do
    saved="$(mktemp "$tmp/saved.XXXXXXXXXX")"
    cp "$tmp/$file" "$saved"
    printf 'edited
' >>"$tmp/$file"
    changed="$(context_digest "$tmp")"
    [[ "$changed" != "$base" ]] ||
      die "selftest: editing $file did not change the digest"
    cp "$saved" "$tmp/$file"
    rm -f "$saved"
    [[ "$(context_digest "$tmp")" == "$base" ]] ||
      die "selftest: restoring $file did not restore the digest"
  done
}

selftest_policy_rejections() {
  local tmp="$1" base="$2" saved
  saved="$(mktemp "$tmp/saved.XXXXXXXXXX")"
  cp "$tmp/.dockerignore" "$saved"
  printf '!new-unhashed-input
' >>"$tmp/.dockerignore"
  (context_digest "$tmp" >/dev/null 2>&1) &&
    die "selftest: relaxed .dockerignore policy passed"
  cp "$saved" "$tmp/.dockerignore"
  rm -f "$saved"

  ln -s zshrc "$tmp/.devcontainer/link"
  (context_digest "$tmp" >/dev/null 2>&1) &&
    die "selftest: symlinked devcontainer input passed"
  rm "$tmp/.devcontainer/link"
  chmod 0600 "$tmp/.devcontainer/zshrc"
  (context_digest "$tmp" >/dev/null 2>&1) &&
    die "selftest: unexpected devcontainer mode passed"
  chmod 0644 "$tmp/.devcontainer/zshrc"
  chmod 0644 "$tmp/scripts/dev/bootstrap_uv.py"
  (context_digest "$tmp" >/dev/null 2>&1) &&
    die "selftest: non-executable bootstrap mode passed"
  chmod 0755 "$tmp/scripts/dev/bootstrap_uv.py"

  printf 'new included file
' >"$tmp/.devcontainer/extra"
  [[ "$(context_digest "$tmp")" != "$base" ]] ||
    die "selftest: adding a .devcontainer file did not change the digest"
  rm -f "$tmp/.devcontainer/extra"
  printf 'ignored firmware source
' >"$tmp/unrelated.c"
  [[ "$(context_digest "$tmp")" == "$base" ]] ||
    die "selftest: an ignored root file changed the digest"
}

selftest_staged_context() {
  local source="$1" base="$2" staged file
  staged="$(mktemp -d "$SELFTEST_TMP_DIR/staged.XXXXXXXXXX")"
  mkdir -p "$staged/scripts/dev"
  cp -R "$source/.devcontainer" "$staged/.devcontainer"
  while read -r _mode file; do
    mkdir -p "$staged/$(dirname "$file")"
    cp "$source/$file" "$staged/$file"
  done < <(canonical_root_context_inputs)
  [[ "$(context_digest "$staged")" == "$base" ]] ||
    die "selftest: byte-identical staged Ansible context changed the digest"
  for file in scripts/dev/uv_release.json scripts/dev/bootstrap_uv_exec.py; do
    rm "$staged/$file"
    (context_digest "$staged" >/dev/null 2>&1) &&
      die "selftest: half-staged nested bootstrap context passed without $file"
    cp "$source/$file" "$staged/$file"
  done
}

selftest_runtime_labels() {
  local base="$1" read_back
  require_runtime
  selftest_round_trip read_back "${SELFTEST_IMAGE_NAMESPACE}-labelled" "$base"
  [[ "$read_back" == "$base" ]] ||
    die "selftest: label round-trip wrote $base and read '${read_back:-<empty>}'"
  selftest_round_trip read_back "${SELFTEST_IMAGE_NAMESPACE}-bare" ""
  [[ -z "$read_back" ]] ||
    die "selftest: an unlabelled image reported a digest ('$read_back')"
  echo "selftest: labelled and unlabelled image directions OK"
}
selftest_managed_discovery_and_open() {
  local tmp="$1" managed explicit_identity marker
  managed="$tmp/managed-lock"
  create_managed_test_lock "$managed"
  RA8_IMAGE_LOCK_DIR='' resolve_image_lock "$managed"
  [[ "$IMAGE_LOCK_MANAGED" == "1" && "$IMAGE_LOCK_FILE" == "$managed/devcontainer-image.lock" ]] ||
    die "selftest: unset environment did not discover the canonical managed lock"
  explicit_identity="$IMAGE_LOCK_IDENTITY"
  RA8_IMAGE_LOCK_DIR="$managed" resolve_image_lock
  [[ "$IMAGE_LOCK_IDENTITY" == "$explicit_identity" ]] ||
    die "selftest: explicit and non-login callers resolved different lock inodes"

  marker="$tmp/unexpected-swap-build"
  mv "$IMAGE_LOCK_FILE" "$managed/original-image.lock"
  : >"$IMAGE_LOCK_FILE"
  chgrp "$SELFTEST_GROUP_GID" "$IMAGE_LOCK_FILE"
  chmod 0660 "$IMAGE_LOCK_FILE"
  if (
    # shellcheck disable=SC2329  # must-not-fire tripwire records a build after replaced-lock refusal.
    build_image() { : >"$marker"; }
    build_locked dead forced "" 1 >/dev/null 2>&1
  ); then
    die "selftest: a replaced image lock passed post-open identity validation"
  fi
  [[ ! -e "$marker" ]] || die "selftest: build ran after image lock replacement"
  RA8_IMAGE_LOCK_DIR="$managed" resolve_image_lock
  rm -f "$IMAGE_LOCK_FILE"
  if (
    # shellcheck disable=SC2329  # must-not-fire tripwire records a build after missing-lock refusal.
    build_image() { : >"$marker"; }
    build_locked dead forced "" 1 >/dev/null 2>&1
  ); then
    die "selftest: a missing resolved image lock was recreated"
  fi
  [[ ! -e "$IMAGE_LOCK_FILE" ]] || die "selftest: no-create image lock open recreated the file"
}

load_image_lock_selftest() {
  local receipts="$SCRIPT_DIR/devcontainer_image_lock_receipts.bash"
  local helper="$SCRIPT_DIR/devcontainer_image_lock_selftest.bash"
  local output status=0
  cd "$REPO_ROOT" || die "selftest: repository root is unavailable"
  if output="$(/bin/bash -p -- "$receipts" 2>&1)"; then
    status=0
  else
    status=$?
  fi
  [[ "$status" == "1" &&
    "$output" == "error: devcontainer image lock receipt helper is source-only" ]] ||
    die "image-lock receipt helper standalone refusal drifted"
  source_approved_selftest_helper "$receipts" "$IMAGE_LOCK_RECEIPTS_RAW_SHA256"
  source_approved_selftest_helper "$helper" "$IMAGE_LOCK_SELFTEST_RAW_SHA256"
  declare -F expected_image_lock_suite_receipts scenario_receipt_value \
    validate_scenario_receipt_directory write_scenario_receipt \
    require_scenario_receipt verify_scenario_receipt_files \
    expected_cleanup_receipt expected_force_cleanup_receipt \
    parent_lock_fd_is_closed require_cleanup_receipt \
    require_force_cleanup_receipt write_worker_cleanup_proof_file \
    require_worker_cleanup_proof_file write_controller_cleanup_receipt_file \
    require_controller_cleanup_receipt_file dispatch_image_lock_selftest >/dev/null ||
    die "image-lock selftest helpers did not load their complete interfaces"
}

# ShellCheck cannot see that the image-lock dispatch helper invokes this suite.
# shellcheck disable=SC2329  # the image-lock dispatch helper invokes this suite by its fixed name.
run_managed_image_lock_suite() {
  local tmp="$1"
  selftest_direct_child_authority
  selftest_pre_isolation_exit_refusal "$tmp" ||
    die "selftest: exited pre-isolation child retained signal authority"
  selftest_ps_failure_cleanup "$tmp"
  selftest_controller_launcher_refusals "$tmp"
  selftest_controller_persisted_ps_failure "$tmp" ||
    die "selftest: persisted controller ps-failure cleanup failed"
  selftest_controller_handler_hang "$tmp"
  SELFTEST_SUITE_RECEIPTS=""
  SELFTEST_SUITE_COUNT=0
  : "$SELFTEST_SUITE_RECEIPTS" "$SELFTEST_SUITE_COUNT"
  SELFTEST_DISPATCH_COMPLETE=0
  run_image_lock_scenario early-exit selftest_early_exit "$tmp"
  run_image_lock_scenario pre-ready-hang selftest_pre_ready_hang "$tmp"
  run_image_lock_scenario forced-build-contention selftest_forced_build_contention "$tmp"
  run_image_lock_scenario post-ready-hang selftest_post_ready_hang "$tmp"
  run_image_lock_scenario signal-ready-timeout selftest_signal_ready_timeout "$tmp"
  run_image_lock_scenario signal-cleanup selftest_signal_cleanup "$tmp"
  verify_image_lock_suite_receipts
  SELFTEST_DISPATCH_COMPLETE=1
  [[ "$SELFTEST_DISPATCH_COMPLETE" == "1" ]]
}

prepare_devcontainer_selftest_context() {
  local tmp="$1"
  mkdir -p "$tmp/.devcontainer" "$tmp/scripts/dev"
  printf 'FROM scratch
' >"$tmp/.devcontainer/Dockerfile"
  printf 'shell config
' >"$tmp/.devcontainer/zshrc"
  expected_dockerignore >"$tmp/.dockerignore"
  printf 'project
' >"$tmp/pyproject.toml"
  printf 'lock
' >"$tmp/uv.lock"
  printf 'bootstrap
' >"$tmp/scripts/dev/bootstrap_uv.py"
  chmod 0755 "$tmp/scripts/dev/bootstrap_uv.py"
  printf 'bootstrap execution helper
' >"$tmp/scripts/dev/bootstrap_uv_exec.py"
  printf 'managed environment authority
' >"$tmp/scripts/dev/managed_python_env.py"
  chmod 0755 "$tmp/scripts/dev/managed_python_env.py"
  printf 'managed environment checks
' >"$tmp/scripts/dev/managed_python_env_checks.py"
  chmod 0755 "$tmp/scripts/dev/managed_python_env_checks.py"
  printf 'manifest
' >"$tmp/scripts/dev/uv_release.json"
}

prepare_devcontainer_selftest_suite() {
  configure_bound_exit_nested_suite || die "selftest: injected parent suite is unsafe"
  begin_selftest_tmp || die "selftest: could not create a private temporary directory"
  [[ "$SELFTEST_BOUND_EXIT_NESTED" == "0" ]] ||
    publish_bound_exit_nested_root || die "selftest: injected nested-root receipt failed"
  if [[ "$SELFTEST_BOUND_EXIT_NESTED" == "0" ]]; then
    establish_selftest_suite_root || die "selftest: could not bind its suite root"
  else
    selftest_suite_root_is_safe || die "selftest: injected parent suite binding drifted"
  fi
  prepare_devcontainer_selftest_context "$SELFTEST_TMP_DIR"
}

cmd_selftest() {
  local tmp base
  prepare_devcontainer_selftest_suite
  tmp="$SELFTEST_TMP_DIR"
  selftest_descriptor_bound_entry "$tmp" ||
    die "selftest: descriptor-bound entry authority regressed"
  base="$(context_digest "$tmp")"
  [[ "$base" =~ ^[0-9a-f]{64}$ ]] ||
    die "selftest: context digest is not a sha256: '$base'"

  selftest_content_changes "$tmp" "$base"
  selftest_policy_rejections "$tmp" "$base"
  selftest_staged_context "$tmp" "$base"
  descendant_startup_selftest "$tmp"
  selftest_private_image_lock "$tmp"
  selftest_temp_cleanup "$tmp"
  selftest_image_failure_cleanup "$tmp"
  selftest_image_cleanup_refusals "$tmp"
  if [[ "$(id -u)" == "0" ]]; then
    selftest_group_selection "$tmp" || die "selftest: managed-lock group directions failed"
    SELFTEST_GROUP_GID="$(select_selftest_group_id "$tmp")" ||
      die "selftest: no usable non-root group for managed-lock attacks"
    load_image_lock_selftest
    selftest_bound_exit_controller_trigger "$tmp"
    selftest_case_trap_composition "$tmp"
    selftest_image_lock_shape_rejections "$tmp" "$tmp/managed-lock"
    selftest_image_lock_metadata_rejections "$tmp"
    selftest_managed_discovery_and_open "$tmp"
    SELFTEST_RECEIPT_DIR="$tmp/image-lock-selftest-receipts"
    mkdir -m 0700 "$SELFTEST_RECEIPT_DIR"
    dispatch_image_lock_selftest suite "$tmp"
    [[ "$SELFTEST_DISPATCH_COMPLETE" == "1" ]] || die "selftest: image lock dispatcher did not complete"
    SELFTEST_FILE_RECEIPTS_VERIFIED=0
    verify_scenario_receipt_files early-exit pre-ready-hang forced-build-contention \
      post-ready-hang signal-ready-timeout signal-cleanup || die "selftest: scenario receipt set is incomplete"
    [[ "$SELFTEST_FILE_RECEIPTS_VERIFIED" == "1" ]] || die "selftest: scenario receipt verifier did not complete"
    selftest_bound_exit_regressions "$tmp" || die "selftest: bound-process EXIT cleanup regressed"
    selftest_bound_exit_supervisor_failures "$tmp" ||
      die "selftest: bound-process supervisor failure cleanup regressed"
    echo "selftest: managed refusal, identity, and force contention OK"
  else
    selftest_nonroot_cleanup_retry_supervisor "$tmp" ||
      die "selftest: non-root supervisor cleanup-retry coverage regressed"
    echo "selftest: root-only managed-lock attacks skipped; private directions OK"
  fi
  echo "selftest: exact root-context inputs react in both directions OK"
  selftest_runtime_labels "$base"
  selftest_no_running_jobs || die "selftest: a supervised process survived the suite"
  finish_selftest_tmp || die "selftest: final temporary-directory cleanup failed"
  clear_selftest_suite_root || die "selftest: suite-root cleanup did not complete"
  export SELFTEST_COMMAND_COMPLETE=1
}
