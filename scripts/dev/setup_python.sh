#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# Create and exactly synchronize the repository-local Python environment from
# pyproject.toml and uv.lock. uv itself is pinned and checksum-verified by the
# adjacent standard-library bootstrap; this script never invokes system pip.

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

  # Preserve the caller's path only as data for --print-path. No command is
  # resolved through it before the script establishes its fixed startup path.
  RA8_SETUP_INHERITED_PATH="${PATH-}"
  readonly RA8_SETUP_INHERITED_PATH
  PATH="/usr/bin:/bin:/usr/sbin:/sbin"
  export PATH

  set -euo pipefail

  SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
  REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd -P)"
  VENV_DIR="$REPO_ROOT/.venv"
  PYPROJECT="$REPO_ROOT/pyproject.toml"
  LOCKFILE="$REPO_ROOT/uv.lock"
  UV_BOOTSTRAP="$SCRIPT_DIR/bootstrap_uv.py"
  MANAGED_ENV_AUTHORITY="$SCRIPT_DIR/managed_python_env.py"

  die() {
    echo "ERROR: $*" >&2
    exit 1
  }

  require_python() {
    local python_cmd="$1"
    command -v "$python_cmd" >/dev/null 2>&1 ||
      die "python3 is required to bootstrap uv (install Python 3.11 or newer)"
    local version
    version="$($python_cmd -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')" ||
      die "cannot query Python version from $python_cmd"
    [[ "$version" =~ ^3\.(11|12|13|14)$ ]] ||
      die "Python 3.11 through 3.14 is required (found $version)"
  }

  resolved_path() {
    local repo_venv="${1:-$VENV_DIR}" inherited_path="${2-${RA8_SETUP_INHERITED_PATH}}"
    local selected_venv="$repo_venv" selected_bin entry joined="" managed=0
    local -a inherited_entries=()

    if [[ -n "${RA8_TOOL_VENV:-}" ]]; then
      selected_venv="${RA8_TOOL_VENV%/}"
      managed=1
      selected_bin="$(
        /usr/bin/python3 -I "$MANAGED_ENV_AUTHORITY" verify \
          --env "$selected_venv" --pyproject "$PYPROJECT" --lock "$LOCKFILE" \
          --group ci --print-bin
      )" || return 1
    elif [[ ! -x "$selected_venv/bin/python3" ]]; then
      printf '%s\n' "$inherited_path"
      return 0
    fi

    [[ -n "${selected_bin:-}" ]] || selected_bin="$selected_venv/bin"
    IFS=: read -r -a inherited_entries <<<"$inherited_path"
    for entry in "${inherited_entries[@]}"; do
      [[ -n "$entry" ]] || entry=.
      [[ "$entry" == "$selected_bin" ]] && continue
      if [[ "$managed" -eq 1 ]]; then
        case "$entry" in
          "$repo_venv" | "$repo_venv"/*) continue ;;
        esac
      fi
      joined="${joined:+$joined:}$entry"
    done
    printf '%s%s%s\n' "$selected_bin" "${joined:+:}" "$joined"
  }

  uv_sync() {
    local python_cmd="$1" target="$2"
    UV_PROJECT_ENVIRONMENT="$target" UV_PYTHON_DOWNLOADS=never \
      "$python_cmd" "$UV_BOOTSTRAP" --ensure-and-run --no-config sync \
      --locked --all-groups --no-install-project --python "$python_cmd" || return 1
  }

  verify_environment() {
    local bootstrap_python="$1" target_python="$2"
    UV_PYTHON_DOWNLOADS=never \
      "$bootstrap_python" "$UV_BOOTSTRAP" --run --no-config lock --check || return 1
    UV_PYTHON_DOWNLOADS=never \
      "$bootstrap_python" "$UV_BOOTSTRAP" --run --no-config pip check \
      --python "$target_python" || return 1
    "$target_python" -c \
      'import PIL, clang.cindex, dotenv, hvac, kasa, kubernetes, serial, usb.core, yaml' || return 1
    PATH="$VENV_DIR/bin:$PATH" "$target_python" scripts/checks/check_tool_versions.py \
      ruff cmake-format cmake-lint yamllint gcovr || return 1
    PATH="$VENV_DIR/bin:$PATH" command -v vela >/dev/null ||
      die "locked Vela executable is missing from $VENV_DIR"
    PATH="$VENV_DIR/bin:$PATH" command -v ansible-playbook >/dev/null ||
      die "locked Ansible executable is missing from $VENV_DIR"
  }

  cmd_setup() {
    local host_python="${PYTHON:-python3}"
    require_python "$host_python"
    [[ -f "$PYPROJECT" ]] || die "missing Python project metadata: $PYPROJECT"
    [[ -f "$LOCKFILE" ]] || die "missing committed Python lock: $LOCKFILE"
    [[ -f "$UV_BOOTSTRAP" ]] || die "missing pinned uv bootstrap: $UV_BOOTSTRAP"
    echo "==> synchronizing locked Python dependencies in $VENV_DIR"
    (cd "$REPO_ROOT" && uv_sync "$host_python" "$VENV_DIR")
    [[ -x "$VENV_DIR/bin/python3" ]] || die "uv did not create $VENV_DIR/bin/python3"
    (cd "$REPO_ROOT" && verify_environment "$host_python" "$VENV_DIR/bin/python3")
    echo "==> Python environment ready: $VENV_DIR/bin/python3"
  }

  resolved_path_selftest() {
    local tmp="$1" inherited="/test/inherited/bin:/usr/bin" selected failure
    local RA8_TOOL_VENV
    mkdir -p "$tmp/valid/bin" "$tmp/directory-only/bin" "$tmp/foreign/bin" "$tmp/managed/bin"
    printf '#!/usr/bin/env bash\nexit 0\n' >"$tmp/valid/bin/python3"
    printf '#!/usr/bin/env bash\nexit 0\n' >"$tmp/managed/bin/python3"
    chmod +x "$tmp/valid/bin/python3" "$tmp/managed/bin/python3"
    ln -s "$tmp/no-such-foreign-python3" "$tmp/foreign/bin/python3"

    unset RA8_TOOL_VENV
    selected="$(resolved_path "$tmp/valid" "$inherited")"
    [[ "$selected" == "$tmp/valid/bin:$inherited" ]] ||
      die "selftest: an executable repository venv was not selected"
    selected="$(resolved_path "$tmp/directory-only" "$inherited")"
    [[ "$selected" == "$inherited" ]] ||
      die "selftest: a directory without an executable python3 changed PATH"
    selected="$(resolved_path "$tmp/foreign" "$inherited")"
    [[ "$selected" == "$inherited" ]] ||
      die "selftest: an unresolved foreign python3 symlink changed PATH"

    RA8_TOOL_VENV="$tmp/managed"
    if failure="$(resolved_path "$tmp/valid" "$tmp/valid/bin:$inherited" 2>&1)"; then
      die "selftest: an arbitrary executable managed venv passed: $failure"
    fi
    case "$failure" in
      *"managed environment"* | *"managed-environment"*) ;;
      *) die "selftest: an arbitrary managed venv gave no useful error: $failure" ;;
    esac
    RA8_TOOL_VENV="$tmp/missing"
    if failure="$(resolved_path "$tmp/valid" "$tmp/valid/bin:$inherited" 2>&1)"; then
      die "selftest: a missing managed venv did not fail closed: $failure"
    fi
    case "$failure" in
      *"managed environment"* | *"managed-environment"*) ;;
      *) die "selftest: a missing managed venv gave no useful error: $failure" ;;
    esac
  }

  startup_path_selftest() {
    local tmp="$1" hostile_bin="$1/hostile-bin" managed="$1/opt/ra8-python-tools"
    local marker="$1/hostile.marker" startup="$1/startup.sh" inherited failure wrapper
    mkdir -p "$hostile_bin" "$managed/bin"
    inherited="$hostile_bin:$VENV_DIR/bin:/caller/bin:/usr/bin"

    for wrapper in dirname python3; do
      printf '#!/bin/sh\nprintf "invoked\\n" >> %q\nexit 97\n' "$marker" >"$hostile_bin/$wrapper"
      chmod +x "$hostile_bin/$wrapper"
    done
    printf '#!/bin/sh\nexit 97\n' >"$managed/bin/python3"
    chmod +x "$managed/bin/python3"
    printf 'printf "startup\\n" >> %q\n' "$marker" >"$startup"

    if failure="$(
      RA8_TOOL_VENV="$managed" BASH_ENV="$startup" PATH="$inherited" \
        /bin/bash -p "$SCRIPT_DIR/setup_python.sh" --print-path 2>&1
    )"; then
      die "selftest: hostile startup supplied an unauthenticated managed venv: $failure"
    fi
    [[ ! -e "$marker" ]] || die "selftest: hostile startup command executed"
  }

  startup_builtin_selftest() {
    local tmp="$1" marker="$1/exported-builtin.marker" child="$1/child.sh" output
    printf '%s\n' \
      "builtin() { printf \"invoked\\\\n\" >>\"\$RA8_BUILTIN_MARKER\"; return 0; }" \
      'export -f builtin' \
      "source \"\$RA8_SETUP_SCRIPT\" --print-path" >"$child"
    if output="$(
      RA8_BUILTIN_MARKER="$marker" RA8_SETUP_SCRIPT="$SCRIPT_DIR/setup_python.sh" \
        BASH_ENV=/dev/null PATH=/usr/bin:/bin /bin/bash "$child"
    )"; then
      die "selftest: unprivileged source bypassed the privileged-mode guard"
    fi
    [[ ! -e "$marker" ]] || die "selftest: exported builtin function bypassed the startup guard"
    [[ -z "$output" ]] || die "selftest: refused unprivileged source emitted output"
  }

  descendant_startup_selftest() {
    local tmp="$1" marker="$1/descendant-startup.marker"
    local startup="$1/descendant-startup.sh" raw_function control protected
    printf 'printf "startup\\n" >> %q\n' "$marker" >"$startup"

    BASH_ENV="$startup" /bin/bash -c ':'
    [[ -s "$marker" ]] || die "selftest: descendant BASH_ENV control did not fire"
    rm -f "$marker"

    BASH_ENV="$startup" /bin/bash -p \
      "$SCRIPT_DIR/setup_python.sh" --selftest-descendant >/dev/null
    [[ ! -e "$marker" ]] || die "selftest: privileged entry leaked BASH_ENV to descendant Bash"

    raw_function='BASH_FUNC_probe%%=() { printf imported; }'
    control="$(/usr/bin/env "$raw_function" /bin/bash -c \
      'if declare -F probe >/dev/null; then printf "child=1\n"; else printf "child=0\n"; fi')"
    [[ "$control" == child=1 ]] || die "selftest: raw function control did not import"
    protected="$(/usr/bin/env "$raw_function" /bin/bash -p \
      "$SCRIPT_DIR/setup_python.sh" --selftest-descendant)"
    [[ "$protected" == child=0 ]] ||
      die "selftest: privileged entry leaked a raw function to descendant Bash"
  }

  uv_execution_selftest() {
    local tmp="$1" fake_bootstrap="$1/bootstrap_uv.py" log="$1/uv.log"
    local target_python="$1/target-python" saved_venv="$VENV_DIR" failure
    local -a calls=()
    printf '%s\n' \
      'import os' \
      'import sys' \
      'from pathlib import Path' \
      'args = sys.argv[1:]' \
      'line = "|".join((os.environ.get("UV_PROJECT_ENVIRONMENT", ""),' \
      '                 os.environ.get("UV_PYTHON_DOWNLOADS", ""), *args))' \
      'with Path(os.environ["RA8_SETUP_TEST_LOG"]).open("a", encoding="ascii") as stream:' \
      '    stream.write(line + "\n")' \
      'if os.environ.get("RA8_SETUP_FAIL_MATCH", "") in args:' \
      '    raise SystemExit(41)' \
      >"$fake_bootstrap"
    printf '#!/bin/sh\nexit 0\n' >"$target_python"
    chmod +x "$target_python"
    VENV_DIR="$tmp/venv"
    mkdir -p "$VENV_DIR/bin"
    for tool in vela ansible-playbook; do
      printf '#!/bin/sh\nexit 0\n' >"$VENV_DIR/bin/$tool"
      chmod +x "$VENV_DIR/bin/$tool"
    done

    UV_BOOTSTRAP="$fake_bootstrap"
    RA8_SETUP_TEST_LOG="$log" uv_sync /usr/bin/python3 "$VENV_DIR"
    RA8_SETUP_TEST_LOG="$log" verify_environment /usr/bin/python3 "$target_python"
    mapfile -t calls <"$log"
    [[ "${#calls[@]}" -eq 3 ]] ||
      die "selftest: production uv calls were not all observed"
    [[ "${calls[0]}" == "$VENV_DIR|never|--ensure-and-run|--no-config|sync|--locked|--all-groups|--no-install-project|--python|/usr/bin/python3" ]] ||
      die "selftest: uv synchronization argv drifted: ${calls[0]}"
    [[ "${calls[1]}" == "|never|--run|--no-config|lock|--check" ]] ||
      die "selftest: uv lock verification argv drifted: ${calls[1]}"
    [[ "${calls[2]}" == "|never|--run|--no-config|pip|check|--python|$target_python" ]] ||
      die "selftest: uv package verification argv drifted: ${calls[2]}"

    if RA8_SETUP_TEST_LOG="$log" RA8_SETUP_FAIL_MATCH=sync \
      uv_sync /usr/bin/python3 "$VENV_DIR"; then
      die "selftest: uv synchronization failure was masked"
    fi
    if RA8_SETUP_TEST_LOG="$log" RA8_SETUP_FAIL_MATCH=lock \
      verify_environment /usr/bin/python3 "$target_python"; then
      die "selftest: uv lock-check failure was masked"
    fi
    if RA8_SETUP_TEST_LOG="$log" RA8_SETUP_FAIL_MATCH=pip \
      verify_environment /usr/bin/python3 "$target_python"; then
      die "selftest: uv package-check failure was masked"
    fi
    VENV_DIR="$saved_venv"
  }

  cmd_selftest() {
    local tmp body unsafe_flag user_flag
    tmp="$(mktemp -d "${TMPDIR:-/tmp}/ra8-python-setup.XXXXXXXX")"
    trap 'rm -rf "$tmp"' RETURN
    uv_execution_selftest "$tmp"

    local fake_python="$tmp/python3"
    # shellcheck disable=SC2016  # generated fixtures expand these variables when executed.
    printf '%s\n' '#!/usr/bin/env bash' \
      '[[ "$1" == "-c" ]] && { printf "%s\n" "$RA8_FAKE_PYTHON_VERSION"; exit 0; }' \
      'printf "Python %s\n" "$RA8_FAKE_PYTHON_VERSION"' >"$fake_python"
    chmod +x "$fake_python"
    RA8_FAKE_PYTHON_VERSION=3.11 require_python "$fake_python"
    RA8_FAKE_PYTHON_VERSION=3.14 require_python "$fake_python"
    if (RA8_FAKE_PYTHON_VERSION=3.10 require_python "$fake_python") >/dev/null 2>&1; then
      die "selftest: Python 3.10 passed the project lower bound"
    fi
    if (RA8_FAKE_PYTHON_VERSION=3.15 require_python "$fake_python") >/dev/null 2>&1; then
      die "selftest: Python 3.15 passed the project upper bound"
    fi

    body="$(declare -f cmd_setup uv_sync verify_environment)"
    unsafe_flag="--break-system"'-packages'
    user_flag="--us"'er'
    case " $body " in
      *"$unsafe_flag"* | *" $user_flag "* | *" -m pip "*)
        die "selftest: setup contains a system-Python escape or pip installer"
        ;;
    esac
    [[ -s "$PYPROJECT" && -s "$LOCKFILE" ]] ||
      die "selftest: project metadata or lock is empty"
    UV_BOOTSTRAP="$SCRIPT_DIR/bootstrap_uv.py"
    python3 "$UV_BOOTSTRAP" --selftest
    /usr/bin/python3 -I "$MANAGED_ENV_AUTHORITY" --selftest
    resolved_path_selftest "$tmp"
    startup_path_selftest "$tmp"
    startup_builtin_selftest "$tmp"
    descendant_startup_selftest "$tmp"
    echo "setup_python.sh --selftest: PASS (locked uv, managed isolation, fail-closed)"
  }

  case "${1:-setup}" in
    setup) cmd_setup ;;
    --print-path) resolved_path ;;
    --selftest) cmd_selftest ;;
    --selftest-descendant)
      if /bin/bash -c 'declare -F probe >/dev/null'; then
        printf 'child=1\n'
      else
        printf 'child=0\n'
      fi
      ;;
    -h | --help)
      echo "usage: scripts/dev/setup_python.sh [setup|--print-path|--selftest]"
      ;;
    *) die "unknown command '$1' (try --help)" ;;
  esac
else
  [[ "$-" == *p* ]]
fi
