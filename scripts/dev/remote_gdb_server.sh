#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# Hold the shared bench while forwarding a J-Link GDB server from the rig.
# IDEs run this as a background process and call `stop` after debugging. An
# optional app selector flashes through the guarded HIL path after taking the
# lock and before opening the server, with no race between flash and attach.
# The lock and authenticated local state are bound to this process.

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
  export PATH=/usr/bin:/bin:/usr/sbin:/sbin
  unset -v PYTHONHOME PYTHONPATH

  script_source="${BASH_SOURCE[0]}"
  script_dir="${script_source%/*}"
  [[ "$script_dir" != "$script_source" ]] || script_dir="."
  ROOT="$(cd "$script_dir/../.." && pwd -P)"
  cd "$ROOT"
  GUARD="$ROOT/scripts/dev/remote_gdb_guard.py"
  PROCESS_GUARD="$ROOT/scripts/dev/remote_gdb_process.py"
  ARGS_GUARD="$ROOT/scripts/dev/remote_gdb_args.py"
  REMOTE_GUARD="$ROOT/scripts/dev/remote_gdb_remote.py"
  GUARD_SELFTEST="$ROOT/scripts/dev/remote_gdb_guard_selftest.py"
  PYTHON="/usr/bin/python3"
  [[ -x "$PYTHON" && -f "$GUARD" && -f "$PROCESS_GUARD" &&
    -f "$ARGS_GUARD" && -f "$REMOTE_GUARD" && -f "$GUARD_SELFTEST" ]] || {
    echo "remote_gdb_server: fixed Python guard authority is unavailable" >&2
    exit 2
  }

  ACTION="${1:-run}"
  PORT="${2:-2331}"
  APP_SELECTOR="${3:-}"
  case "$ACTION" in
    --selftest)
      "$PYTHON" -I "$GUARD_SELFTEST" \
        --helper "$GUARD" --args-helper "$ARGS_GUARD" \
        --remote-helper "$REMOTE_GUARD" --root "$ROOT"
      exit $?
      ;;
    --boundary-selftest)
      if /bin/bash -c \
        'declare -F ra8_remote_gdb_probe >/dev/null || exit 1; ra8_remote_gdb_probe; exit 0'; then
        exit 1
      fi
      [[ "$PWD" == "$ROOT" ]]
      exit $?
      ;;
    stop)
      "$PYTHON" -I "$ARGS_GUARD" validate-port --port "$PORT"
      "$PYTHON" -I "$GUARD" stop --root "$ROOT" --port "$PORT"
      exit $?
      ;;
    run) ;;
    *)
      echo "usage: $0 <run|stop> [local-port] [app-to-flash]" >&2
      exit 2
      ;;
  esac

  # shellcheck source=scripts/hil/lib/rig_env.sh
  source "$ROOT/scripts/hil/lib/rig_env.sh"
  rig_require PI_HOST JLINK_SN JLINK_DEVICE PI_REPO
  "$PYTHON" -I "$ARGS_GUARD" validate \
    --host "$PI_HOST" --serial "$JLINK_SN" --device "$JLINK_DEVICE" --port "$PORT"

  APP_ID=""
  if [[ -n "$APP_SELECTOR" ]]; then
    APP_ID="$(
      "$PYTHON" -I "$ARGS_GUARD" canonical-app \
        --root "$ROOT" --selector "$APP_SELECTOR"
    )"
  fi
  REMOTE_START_COMMAND="$(
    "$PYTHON" -I "$ARGS_GUARD" remote-command \
      --device "$JLINK_DEVICE" --serial "$JLINK_SN" --port "$PORT"
  )"

  ssh_pid=""
  broker_pid=""
  cleanup() {
    if [[ -n "$ssh_pid" ]] && kill -0 "$ssh_pid" 2>/dev/null; then
      kill -TERM "$ssh_pid" 2>/dev/null || true
      wait "$ssh_pid" 2>/dev/null || true
    fi
    "$PYTHON" -I "$GUARD" release \
      --root "$ROOT" --port "$PORT" >/dev/null 2>&1 || true
    if [[ -n "$broker_pid" ]]; then
      wait "$broker_pid" 2>/dev/null || true
    fi
  }
  trap cleanup EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM HUP

  "$PYTHON" -I "$GUARD" broker \
    --root "$ROOT" --port "$PORT" \
    --parent-pid "$$" --app-arg "$APP_SELECTOR" &
  broker_pid=$!
  "$PYTHON" -I "$GUARD" await \
    --root "$ROOT" --port "$PORT" \
    --parent-pid "$$" --broker-pid "$broker_pid"

  # shellcheck source=scripts/hil/lib/bench_lock.sh
  source "$ROOT/scripts/hil/lib/bench_lock.sh"
  ra8_bench_require "remote IDE J-Link GDB session on port $PORT" 2h || exit $?

  if [[ -n "$APP_ID" ]]; then
    echo "remote_gdb_server: guarded flash of $APP_ID before attach" >&2
    /bin/bash -p "$ROOT/scripts/hil/flash.sh" "$APP_ID"
  fi

  echo "remote_gdb_server: opening guarded tunnel on 127.0.0.1:$PORT" >&2
  /usr/bin/ssh -o BatchMode=yes -o ExitOnForwardFailure=yes \
    -o ServerAliveInterval=10 -o ServerAliveCountMax=3 \
    -L "127.0.0.1:${PORT}:127.0.0.1:${PORT}" \
    -- "$PI_HOST" "$REMOTE_START_COMMAND" <"$REMOTE_GUARD" &
  ssh_pid=$!
  wait "$ssh_pid"
else
  [[ "$-" == *p* ]]
fi
