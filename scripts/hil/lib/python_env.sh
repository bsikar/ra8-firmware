#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# Select the locked Python environment that owns HIL-only third-party imports.

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
    if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
      printf 'error: sourced privileged entry refuses inherited Bash functions\n' >&2
      unset -v ra8_startup_env_done_count
      unset -v ra8_startup_env_name ra8_startup_env_row ra8_startup_env_unset
      unset -v RA8_STARTUP_ENV_DONE RA8_STARTUP_ENV_SCRUBBED
      unset -f _ra8_startup_refuse
      return 2
    fi
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

  ra8_hil_python() {
    local repo_root="$1"
    local dev_box_python="${2:-/opt/ra8-python-tools/bin/python3}"
    local bench_python="${3:-/opt/ra8-hil-python/bin/python3}"
    local candidate
    local -a candidates=("$repo_root/.venv/bin/python3")

    if [[ -n "${RA8_TOOL_VENV:-}" ]]; then
      if [[ "$RA8_TOOL_VENV" != /* || "$RA8_TOOL_VENV" == "/" ]]; then
        echo "ra8_hil_python: RA8_TOOL_VENV must be a non-root absolute path" >&2
        return 2
      fi
      candidates+=("$RA8_TOOL_VENV/bin/python3")
    fi
    candidates+=("$dev_box_python" "$bench_python")

    for candidate in "${candidates[@]}"; do
      if [[ -x "$candidate" && ! -L "$candidate" ]]; then
        printf '%s\n' "$candidate"
        return 0
      fi
    done
    echo "ra8_hil_python: no locked HIL-capable Python environment is available" >&2
    return 1
  }

  _ra8_hil_python_selftest() {
    local test_root repo_root tool_root dev_python bench_python selected
    test_root="$(mktemp -d)"
    trap 'rm -rf "$test_root"' RETURN
    repo_root="$test_root/repo"
    tool_root="$test_root/tool"
    dev_python="$test_root/dev/bin/python3"
    bench_python="$test_root/bench/bin/python3"
    mkdir -p "$repo_root/.venv/bin" "$tool_root/bin" \
      "$(dirname "$dev_python")" "$(dirname "$bench_python")"
    : >"$repo_root/.venv/bin/python3"
    : >"$tool_root/bin/python3"
    : >"$dev_python"
    : >"$bench_python"
    chmod 0755 "$repo_root/.venv/bin/python3" "$tool_root/bin/python3" \
      "$dev_python" "$bench_python"

    selected="$(RA8_TOOL_VENV="$tool_root" \
      ra8_hil_python "$repo_root" "$dev_python" "$bench_python")"
    [[ "$selected" == "$repo_root/.venv/bin/python3" ]] || return 1

    rm "$repo_root/.venv/bin/python3"
    selected="$(RA8_TOOL_VENV="$tool_root" \
      ra8_hil_python "$repo_root" "$dev_python" "$bench_python")"
    [[ "$selected" == "$tool_root/bin/python3" ]] || return 1

    selected="$(RA8_TOOL_VENV='' \
      ra8_hil_python "$repo_root" "$dev_python" "$bench_python")"
    [[ "$selected" == "$dev_python" ]] || return 1

    rm "$dev_python"
    selected="$(RA8_TOOL_VENV='' \
      ra8_hil_python "$repo_root" "$dev_python" "$bench_python")"
    [[ "$selected" == "$bench_python" ]] || return 1

    rm "$bench_python"
    if RA8_TOOL_VENV='' ra8_hil_python \
      "$repo_root" "$dev_python" "$bench_python" >/dev/null 2>&1; then
      return 1
    fi
    if RA8_TOOL_VENV=relative ra8_hil_python \
      "$repo_root" "$dev_python" "$bench_python" >/dev/null 2>&1; then
      return 1
    fi
    echo "python_env.sh --selftest: PASS"
  }

  if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    [[ "${1:-}" == "--selftest" && "$#" -eq 1 ]] || {
      echo "usage: python_env.sh --selftest" >&2
      exit 2
    }
    _ra8_hil_python_selftest
  fi
else
  [[ "$-" == *p* ]]
fi
