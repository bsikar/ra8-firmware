#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# Resolve and execute the repository's Just command without assuming that the
# executable which entered a recipe is also discoverable on a child PATH.

# The real body is reachable only after the protected shebang has taken
# effect. An explicit ordinary-Bash caller may already have run arbitrary
# BASH_ENV code, so it fails through shell grammar instead of attempting an
# in-process repair. Raw exported-function rows are removed before any later
# ordinary Bash descendant can import them.
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

  die() {
    echo "run_just.sh: $*" >&2
    return 127
  }

  resolve_override() {
    local requested="$1" candidate
    if [[ "$requested" == */* ]]; then
      candidate="$requested"
    else
      candidate="$(command -v "$requested" 2>/dev/null || true)"
    fi
    if [[ -z "$candidate" || ! -x "$candidate" ]]; then
      die "RA8_JUST does not name an executable: $requested"
      return
    fi
    printf '%s\n' "$candidate"
  }

  resolve_just() {
    local candidate user_candidate
    if [[ -n "${RA8_JUST:-}" ]]; then
      resolve_override "$RA8_JUST"
      return
    fi

    candidate="$(command -v just 2>/dev/null || true)"
    if [[ -n "$candidate" && -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return
    fi

    if [[ -n "${HOME:-}" ]]; then
      user_candidate="$HOME/.local/bin/just"
      if [[ -x "$user_candidate" ]]; then
        printf '%s\n' "$user_candidate"
        return
      fi
    fi

    die "Just is unavailable; set RA8_JUST, add just to PATH, or install \$HOME/.local/bin/just"
  }

  write_fake_just() {
    local path="$1"
    cat >"$path" <<'FAKE_JUST'
#!/bin/bash -p
: "${RA8_JUST_CAPTURE:?set RA8_JUST_CAPTURE}"
printf "%s\0" "$#" "$@" >"$RA8_JUST_CAPTURE"
FAKE_JUST
    chmod +x "$path"
  }

  write_fake_bash() {
    local path="$1" marker="$2"
    cat >"$path" <<FAKE_BASH
#!/bin/bash -p
/usr/bin/printf x >"$marker"
exit 73
FAKE_BASH
    chmod +x "$path"
  }

  assert_hostile_argv() {
    local override="$1" capture="$2" startup="$3" hostile_path="$4"
    local source_marker="$5" arbitrary_marker="$6" startup_marker="$7" function_marker="$8"
    local index item
    local -a expected_argv captured
    expected_argv=(alpha 'two words' $'line one\nline two\tend' "*?[abc];\$(not-executed)" '')
    BASH_ENV="$startup" PATH="$hostile_path" RA8_JUST_CAPTURE="$capture" \
      RA8_JUST="$override" /bin/bash -p "$0" "${expected_argv[@]}"
    unset -f mkdir
    unset RA8_SELFTEST_FUNCTION_MARKER
    [[ ! -e "$source_marker" && ! -e "$arbitrary_marker" && ! -e "$startup_marker" &&
      ! -e "$function_marker" ]] || die "selftest: hostile shell state reached production"
    while IFS= read -r -d '' item; do
      captured+=("$item")
    done <"$capture"
    [[ "${captured[0]:-}" == "${#expected_argv[@]}" ]] ||
      die "selftest: argv count changed (${captured[0]:-missing} != ${#expected_argv[@]})"
    [[ "${#captured[@]}" -eq $((${#expected_argv[@]} + 1)) ]] ||
      die "selftest: captured argv boundary count changed"
    for ((index = 0; index < ${#expected_argv[@]}; index++)); do
      [[ "${captured[index + 1]}" == "${expected_argv[index]}" ]] ||
        die "selftest: argv boundary $index changed"
    done
  }

  assert_hostile_entry() {
    local scratch="$1" override="$2" capture="$3"
    local source_bin="$scratch/source/.venv/bin" arbitrary_bin="$scratch/arbitrary"
    local source_marker="$scratch/source-bash.ran" arbitrary_marker="$scratch/arbitrary.ran"
    local startup="$scratch/bash-env" startup_marker="$scratch/startup.ran"
    local function_marker="$scratch/function.ran" hostile_path
    mkdir -p "$source_bin" "$arbitrary_bin"
    write_fake_bash "$source_bin/bash" "$source_marker"
    write_fake_bash "$arbitrary_bin/bash" "$arbitrary_marker"
    printf '/usr/bin/printf x >"%s"\n' "$startup_marker" >"$startup"
    hostile_path="$source_bin:$arbitrary_bin:${PATH:-/usr/bin:/bin}"

    if PATH="$hostile_path" /usr/bin/env bash -c true; then
      die "selftest: source Bash control did not fail"
    fi
    if PATH="$arbitrary_bin:$source_bin:${PATH:-/usr/bin:/bin}" /usr/bin/env bash -c true; then
      die "selftest: arbitrary Bash control did not fail"
    fi
    [[ -f "$source_marker" && -f "$arbitrary_marker" ]] ||
      die "selftest: PATH Bash controls did not fire"
    /bin/rm -f "$source_marker" "$arbitrary_marker"

    RA8_SELFTEST_FUNCTION_MARKER="$function_marker"
    export RA8_SELFTEST_FUNCTION_MARKER
    mkdir() {
      /usr/bin/printf x >"$RA8_SELFTEST_FUNCTION_MARKER"
      return 73
    }
    if mkdir; then
      die "selftest: local exported-function control did not fail"
    fi
    [[ -f "$function_marker" ]] || die "selftest: local exported-function control did not fire"
    /bin/rm -f "$function_marker"
    export -f mkdir
    if (
      unset SSH_CLIENT
      BASH_ENV="$startup" /bin/bash -c mkdir
    ); then
      die "selftest: startup/function controls did not fail"
    fi
    [[ -f "$startup_marker" && -f "$function_marker" ]] ||
      die "selftest: startup/function controls did not fire"
    /bin/rm -f "$startup_marker" "$function_marker"

    assert_hostile_argv "$override" "$capture" "$startup" "$hostile_path" \
      "$source_marker" "$arbitrary_marker" "$startup_marker" "$function_marker"
  }

  assert_resolution() {
    local expected="$1" actual="$2" label="$3"
    [[ "$actual" == "$expected" ]] ||
      die "selftest: $label selected '$actual', expected '$expected'"
  }

  assert_root_job_probe() {
    local scratch="$1" real_just="$2" fake_bin="$1/cpu-path"
    local marker="$1/fake-nproc.ran" selected
    mkdir -p "$fake_bin"
    cat >"$fake_bin/nproc" <<FAKE_NPROC
#!/bin/bash -p
/usr/bin/printf x >"$marker"
/usr/bin/printf '999\\n'
FAKE_NPROC
    chmod +x "$fake_bin/nproc"

    selected="$(PATH="$fake_bin:/usr/bin:/bin" nproc)"
    [[ "$selected" == 999 && -s "$marker" ]] ||
      die "selftest: hostile CPU-probe control did not fire"
    /bin/rm -f "$marker"

    selected="$(PATH="$fake_bin:/usr/bin:/bin" RA8_TOOL_VENV='' "$real_just" --evaluate RA8_MAX_JOBS)"
    [[ "$selected" =~ ^[1-9][0-9]*$ ]] ||
      die "selftest: root Just CPU default is not a positive integer: $selected"
    [[ "$selected" != 999 && ! -e "$marker" ]] ||
      die "selftest: root Just CPU probe executed caller PATH"
  }

  cmd_selftest() {
    local scratch override path_bin fallback selected capture real_just
    scratch="$(mktemp -d "${TMPDIR:-/tmp}/ra8-run-just.XXXXXXXX")"
    trap 'rm -rf "$scratch"' RETURN
    real_just="$(resolve_just)"
    mkdir -p "$scratch/path" "$scratch/home/.local/bin" "$scratch/empty"
    override="$scratch/override-just"
    path_bin="$scratch/path/just"
    fallback="$scratch/home/.local/bin/just"
    write_fake_just "$override"
    write_fake_just "$path_bin"
    write_fake_just "$fallback"

    selected="$(RA8_JUST="$override" PATH="$scratch/path" HOME="$scratch/home" resolve_just)"
    assert_resolution "$override" "$selected" "explicit override"
    selected="$(RA8_JUST='' PATH="$scratch/path" HOME="$scratch/home" resolve_just)"
    assert_resolution "$path_bin" "$selected" "PATH lookup"
    selected="$(RA8_JUST='' PATH="$scratch/empty" HOME="$scratch/home" resolve_just)"
    assert_resolution "$fallback" "$selected" "user-local fallback"

    if (RA8_JUST="$scratch/missing" PATH="$scratch/empty" HOME='' resolve_just) >/dev/null 2>&1; then
      die "selftest: an invalid explicit override did not fail closed"
    fi
    if (RA8_JUST='' PATH="$scratch/empty" HOME='' resolve_just) >/dev/null 2>&1; then
      die "selftest: a missing Just executable did not fail closed"
    fi
    capture="$scratch/argv.capture"
    assert_hostile_entry "$scratch" "$override" "$capture"
    assert_root_job_probe "$scratch" "$real_just"
    echo "run_just.sh --selftest: PASS (resolution, failure, argv, shell entry, CPU probe)"
  }

  if [[ "${1:-}" == "--selftest" ]]; then
    [[ "$#" -eq 1 ]] || die "--selftest takes no arguments"
    cmd_selftest
    exit 0
  fi

  clean_exec() {
    local name
    local -a environment=()
    while IFS= read -r name; do
      case "$name" in
        BASH_FUNC_* | BASH_ENV | ENV | PYTHONHOME | PYTHONPATH) continue ;;
      esac
      environment+=("$name=${!name}")
    done < <(compgen -e)
    exec /usr/bin/env -i "${environment[@]}" "$@"
  }

  just_bin="$(resolve_just)" || exit $?
  clean_exec "$just_bin" "$@"
else
  [[ "$-" == *p* ]]
fi
