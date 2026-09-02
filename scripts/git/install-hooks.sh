#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# Install stable launchers under the shared Git common directory.
#
# Each installed hook executes scripts/git/<hook> from the invoking worktree's
# immutable HEAD. Candidate staged or unstaged hook bytes therefore cannot run
# before pre-commit captures the exact index. Linked worktrees share the
# launcher directory but resolve their own current HEAD at invocation time.
#
#     ./scripts/git/install-hooks.sh        (or: just hooks)
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

  unset BASH_ENV ENV PYTHONHOME PYTHONPATH
  unalias git python3 bash 2>/dev/null || true
  unset -f git python3 bash 2>/dev/null || true
  hash -r
  PATH=/usr/bin:/bin:/usr/sbin:/sbin
  export PATH
  TRUSTED_GIT=/usr/bin/git
  [[ "${RA8_TRUSTED_GIT:-$TRUSTED_GIT}" == "$TRUSTED_GIT" ]] || {
    echo "install-hooks.sh: refusing non-authority Git executable" >&2
    exit 1
  }
  [[ -f "$TRUSTED_GIT" && ! -L "$TRUSTED_GIT" && -x "$TRUSTED_GIT" ]] || {
    echo "install-hooks.sh: trusted /usr/bin/git is unavailable or unsafe" >&2
    exit 1
  }
  while IFS='=' read -r name _; do
    if [[ "$name" =~ ^GIT_[A-Za-z0-9_]+$ ]]; then
      unset "$name"
    fi
  done < <(env)
  export RA8_TRUSTED_GIT="$TRUSTED_GIT"
  export GIT_ATTR_NOSYSTEM=1
  export GIT_CONFIG_GLOBAL=/dev/null
  export GIT_CONFIG_NOSYSTEM=1
  export GIT_CONFIG_SYSTEM=/dev/null
  export GIT_PAGER=cat
  export GIT_TERMINAL_PROMPT=0

  MARKER="RA8-MANAGED-HOOK-LAUNCHER-V1"
  HOOK_NAMES="commit-msg post-checkout post-commit post-merge pre-commit pre-push"
  scratch=""
  staging=""
  backup=""
  lock=""
  managed=""
  current=""
  current_present=0
  hooks_path_value=""
  hooks_path_present=0
  original_managed=0
  transaction_started=0
  installed=0
  lock_owned=0
  termination_pending=0

  die() {
    echo "install-hooks.sh: $*" >&2
    exit 1
  }

  read_hooks_path() {
    local count=0 row status=""
    local -a rows=()
    hooks_path_value=""
    hooks_path_present=0
    while IFS= read -r -d '' row; do
      count=$((count + 1))
      rows+=("$row")
    done < <(
      if "$TRUSTED_GIT" -C "$root" config --local --null --get core.hooksPath \
        2>/dev/null; then
        status=0
      else
        status="$?"
      fi
      printf 'RA8_HOOKS_PATH_STATUS=%s\0' "$status"
    )
    [[ "$count" -ge 1 && "${rows[$((count - 1))]}" == RA8_HOOKS_PATH_STATUS=* ]] || return 2
    status="${rows[$((count - 1))]#RA8_HOOKS_PATH_STATUS=}"
    case "$status:$count" in
      0:2)
        hooks_path_value="${rows[0]}"
        hooks_path_present=1
        ;;
      1:1)
        hooks_path_value=""
        ;;
      [2-9]:1 | [1-9][0-9]*:1) return "$status" ;;
      *) return 2 ;;
    esac
  }

  restore_hooks_config() {
    local restore_failed=0
    if [[ "$current_present" == "1" ]]; then
      "$TRUSTED_GIT" -C "$root" config --local core.hooksPath "$current" \
        2>/dev/null || restore_failed=1
    else
      "$TRUSTED_GIT" -C "$root" config --local --unset-all core.hooksPath \
        2>/dev/null || true
    fi
    if ! read_hooks_path; then
      restore_failed=1
    elif [[ "$hooks_path_present" != "$current_present" ]]; then
      restore_failed=1
    elif [[ "$current_present" == "1" && "$hooks_path_value" != "$current" ]]; then
      restore_failed=1
    fi
    return "$restore_failed"
  }

  restore_hook_state() {
    local restore_failed=0
    if [[ "$installed" != "1" && "$transaction_started" == "1" ]]; then
      # Restore configuration and directory state from facts recorded before
      # the first mutation. No assignment after a move is load-bearing.
      restore_hooks_config || restore_failed=1
      if [[ "$original_managed" == "1" ]]; then
        if [[ -n "$backup" && -d "$backup" && ! -L "$backup" ]]; then
          if [[ -e "$managed" || -L "$managed" ]]; then
            rm -rf -- "$managed" || restore_failed=1
          fi
          if [[ ! -e "$managed" && ! -L "$managed" ]]; then
            if mv -- "$backup" "$managed"; then
              backup=""
            else
              restore_failed=1
            fi
          fi
        elif [[ ! -d "$managed" || -L "$managed" ]]; then
          restore_failed=1
        fi
      elif [[ -e "$managed" || -L "$managed" ]]; then
        rm -rf -- "$managed" || restore_failed=1
      fi
    fi
    return "$restore_failed"
  }

  remove_transaction_artifacts() {
    local restore_failed=0
    if [[ -n "$scratch" && -f "$scratch" ]] && ! rm -f -- "$scratch"; then
      restore_failed=1
    fi
    if [[ -n "$staging" && -d "$staging" ]] && ! rm -rf -- "$staging"; then
      restore_failed=1
    fi
    if [[ -n "$backup" && -d "$backup" ]]; then
      if [[ "$installed" == "1" ]]; then
        if rm -rf -- "$backup"; then
          backup=""
        else
          restore_failed=1
        fi
      else
        printf 'install-hooks.sh: recovery backup retained at %s\n' "$backup" >&2
        restore_failed=1
      fi
    fi
    if [[ "$lock_owned" == "1" ]]; then
      if [[ -d "$lock" && ! -L "$lock" ]] && rmdir -- "$lock"; then
        lock_owned=0
      else
        restore_failed=1
      fi
    fi
    return "$restore_failed"
  }

  cleanup() {
    local status="$?" restore_failed=0
    trap - EXIT
    # A second termination signal must not interrupt rollback after the first
    # one entered this handler. SIGKILL remains outside any shell guarantee.
    trap '' HUP INT QUIT TERM
    if ! restore_hook_state; then
      restore_failed=1
    fi
    if ! remove_transaction_artifacts; then
      restore_failed=1
    fi
    if [[ "$restore_failed" == "1" ]]; then
      status=1
    fi
    exit "$status"
  }

  # Release tarballs and vendored copies have no Git metadata.
  [[ "$#" -eq 0 ]] || die "this installer accepts no arguments"
  root="$("$TRUSTED_GIT" rev-parse --show-toplevel 2>/dev/null)" || exit 0
  [[ -n "$root" ]] || exit 0
  root="$(cd "$root" && pwd -P)"
  common="$("$TRUSTED_GIT" -C "$root" rev-parse --path-format=absolute --git-common-dir)"
  common="$(cd "$common" && pwd -P)"
  managed="$common/ra8-hooks"
  read_hooks_path || die "cannot read local core.hooksPath"
  current="$hooks_path_value"
  current_present="$hooks_path_present"
  lock="$common/ra8-hooks.install.lock"
  trap cleanup EXIT
  trap 'termination_pending=1' HUP INT QUIT TERM
  if mkdir -- "$lock" 2>/dev/null; then
    lock_owned=1
  else
    die "another hook installer owns $lock"
  fi
  trap 'exit 3' HUP INT QUIT TERM
  [[ "$termination_pending" == "0" ]] || exit 3

  case "${current_present}:${current}" in
    0: | 1: | 1:scripts/git | "1:${managed}") ;;
    *) die "refusing to replace unmanaged core.hooksPath: $current" ;;
  esac
  if [[ -e "$managed" && (! -d "$managed" || -L "$managed") ]]; then
    die "managed hook path exists but is not a regular directory: $managed"
  fi
  if [[ -d "$managed" ]]; then
    for entry in "$managed"/* "$managed"/.[!.]* "$managed"/..?*; do
      [[ -e "$entry" || -L "$entry" ]] || continue
      name="${entry##*/}"
      case " $HOOK_NAMES " in
        *" $name "*) ;;
        *) die "refusing unknown or interrupted file in managed hook directory: $name" ;;
      esac
      [[ -f "$entry" && ! -L "$entry" ]] || die "managed hook is not a regular file: $name"
      grep -q "^# $MARKER$" "$entry" || die "refusing unmanaged hook file: $entry"
    done
    original_managed=1
    backup="$common/ra8-hooks.backup.$$.$RANDOM"
    [[ ! -e "$backup" ]] || die "backup collision: $backup"
  fi

  head="$("$TRUSTED_GIT" -C "$root" rev-parse --verify HEAD)" || die "repository has no committed HEAD"
  entry="$("$TRUSTED_GIT" -C "$root" ls-tree "$head" -- scripts/git/hook-launcher)"
  IFS=$' \t' read -r mode type blob _path <<<"$entry"
  [[ "$mode" == "100755" && "$type" == "blob" && -n "$blob" ]] ||
    die "HEAD does not own an executable scripts/git/hook-launcher"
  scratch="$(mktemp "$common/ra8-hook-launcher.XXXXXXXX")"
  "$TRUSTED_GIT" -C "$root" cat-file blob "$blob" >"$scratch"
  actual="$("$TRUSTED_GIT" -C "$root" hash-object --no-filters "$scratch")"
  [[ "$actual" == "$blob" ]] || die "materialized launcher differs from its HEAD blob"
  grep -q "^# $MARKER$" "$scratch" || die "committed launcher lacks its ownership marker"
  chmod 500 "$scratch"
  staging="$(mktemp -d "$common/ra8-hooks.stage.XXXXXXXX")"
  chmod 700 "$staging"
  for name in $HOOK_NAMES; do
    cp "$scratch" "$staging/$name"
    chmod 500 "$staging/$name"
  done
  transaction_started=1
  if [[ "$original_managed" == "1" ]]; then
    mv -- "$managed" "$backup"
  fi
  mv -- "$staging" "$managed"
  staging=""
  "$TRUSTED_GIT" -C "$root" config --local core.hooksPath "$managed"
  installed=1
  if [[ -n "$backup" && -d "$backup" ]]; then
    rm -rf -- "$backup"
    backup=""
  fi
  echo "[hooks] core.hooksPath -> $managed (immutable-HEAD launchers active)"
else
  [[ "$-" == *p* ]]
fi
