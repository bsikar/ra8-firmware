#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# One typed authority for the four rig coordinates consumed by HIL shell and
# Ansible. This file is both a Bash 3.2-compatible sourced library and a small
# privileged CLI. It validates values only; rig_env.sh feeds it values from the
# adjacent declarative parser and never executes the selected environment file.

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
      printf 'error: sourced rig contract refuses inherited Bash functions\n' >&2
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

  ra8_rig_contract_describe() {
    printf '%s\t%s\t%s\t%s\n' \
      PI_HOST ssh_target required '' \
      JLINK_SN identifier required '' \
      JLINK_DEVICE identifier optional R7KA8D2KF_CPU0 \
      PI_REPO repo_path optional ra8-firmware
  }

  ra8_rig_contract_lookup() {
    local wanted="$1" name kind presence default
    while IFS=$'\t' read -r name kind presence default; do
      if [[ "$name" == "$wanted" ]]; then
        printf '%s\t%s\t%s\n' "$kind" "$presence" "$default"
        return 0
      fi
    done < <(ra8_rig_contract_describe)
    return 1
  }

  ra8_rig_contract_default() {
    local row
    row="$(ra8_rig_contract_lookup "$1")" || return 2
    printf '%s\n' "${row##*$'\t'}"
  }

  _ra8_rig_reject_common() {
    local name="$1" value="$2"
    if [[ "$value" == *[[:cntrl:]]* ]]; then
      printf 'error: %s contains a control character\n' "$name" >&2
      return 2
    fi
    if [[ "$value" == *[[:space:]]* ]]; then
      printf 'error: %s contains whitespace\n' "$name" >&2
      return 2
    fi
    return 0
  }

  _ra8_rig_validate_dns() {
    local host="$1" rest label
    [[ ${#host} -le 253 ]] || return 2
    [[ "$host" != .* && "$host" != *. && "$host" != *..* ]] || return 2
    rest="$host"
    while :; do
      label="${rest%%.*}"
      [[ -n "$label" && ${#label} -le 63 ]] || return 2
      [[ "$label" != -* && "$label" != *- ]] || return 2
      [[ "$label" != *[!A-Za-z0-9-]* ]] || return 2
      [[ "$rest" == *.* ]] || break
      rest="${rest#*.}"
    done
    return 0
  }

  _ra8_rig_validate_ipv4() {
    local host="$1" first second third fourth extra octet
    IFS=. read -r first second third fourth extra <<<"$host"
    [[ -n "$first" && -n "$second" && -n "$third" && -n "$fourth" ]] || return 2
    [[ -z "$extra" ]] || return 2
    for octet in "$first" "$second" "$third" "$fourth"; do
      [[ "$octet" =~ ^[0-9]{1,3}$ ]] || return 2
      ((10#$octet <= 255)) || return 2
    done
    return 0
  }

  _ra8_rig_validate_ssh_target() {
    local value="$1" user='' host="$1"
    _ra8_rig_reject_common PI_HOST "$value" || return 2
    [[ ${#value} -le 320 && "$value" != -* ]] || {
      printf 'error: PI_HOST is too long or starts with an option prefix\n' >&2
      return 2
    }
    if [[ "$value" == *@* ]]; then
      [[ "$value" != *@*@* ]] || {
        printf 'error: PI_HOST must contain at most one user separator\n' >&2
        return 2
      }
      user="${value%%@*}"
      host="${value#*@}"
      if [[ -z "$user" || ${#user} -gt 64 || "$user" != [A-Za-z0-9_]* ||
        "$user" == *[!A-Za-z0-9_.-]* ]]; then
        printf 'error: PI_HOST has a malformed SSH user\n' >&2
        return 2
      fi
    fi
    if [[ -z "$host" || "$host" == -* ]]; then
      printf 'error: PI_HOST has an empty or option-like host\n' >&2
      return 2
    fi
    if [[ "$host" == *.* && "$host" != *[!0-9.]* ]]; then
      _ra8_rig_validate_ipv4 "$host" || {
        printf 'error: PI_HOST has a malformed IPv4 address\n' >&2
        return 2
      }
    elif ! _ra8_rig_validate_dns "$host"; then
      printf 'error: PI_HOST has a malformed DNS name\n' >&2
      return 2
    fi
  }

  _ra8_rig_validate_identifier() {
    local name="$1" value="$2"
    _ra8_rig_reject_common "$name" "$value" || return 2
    if [[ ${#value} -gt 128 || "$value" != [A-Za-z0-9_]* ||
      "$value" == *[!A-Za-z0-9_.-]* ]]; then
      printf 'error: %s must be an identifier and cannot start with punctuation\n' "$name" >&2
      return 2
    fi
  }

  _ra8_rig_validate_repo_path() {
    local value="$1" rest segment
    _ra8_rig_reject_common PI_REPO "$value" || return 2
    if [[ ${#value} -gt 1024 || "$value" == / || "$value" == */ ||
      "$value" == *//* || "$value" == *[!A-Za-z0-9_./-]* ]]; then
      printf 'error: PI_REPO is not a safe absolute or relative path\n' >&2
      return 2
    fi
    rest="${value#/}"
    [[ -n "$rest" ]] || return 2
    while :; do
      segment="${rest%%/*}"
      if [[ -z "$segment" || "$segment" == . || "$segment" == .. ||
        "$segment" == -* ]]; then
        printf 'error: PI_REPO contains an unsafe path segment\n' >&2
        return 2
      fi
      [[ "$rest" == */* ]] || break
      rest="${rest#*/}"
    done
  }

  ra8_rig_validate_value() {
    local name="$1" value="$2" allow_empty="${3:-false}" row kind
    row="$(ra8_rig_contract_lookup "$name")" || {
      printf 'error: %s is not a declared rig field\n' "$name" >&2
      return 2
    }
    if [[ -z "$value" ]]; then
      [[ "$allow_empty" == true ]] && return 0
      printf 'error: %s is not set. Copy .env.example to .env and fill it in.\n' "$name" >&2
      return 2
    fi
    kind="${row%%$'\t'*}"
    case "$kind" in
      ssh_target) _ra8_rig_validate_ssh_target "$value" ;;
      identifier) _ra8_rig_validate_identifier "$name" "$value" ;;
      repo_path) _ra8_rig_validate_repo_path "$value" ;;
      *)
        printf 'error: %s has unknown rig contract kind %s\n' "$name" "$kind" >&2
        return 2
        ;;
    esac
  }

  ra8_rig_validate_loaded() {
    local allow_empty="${1:-true}" name _kind _presence _default value
    while IFS=$'\t' read -r name _kind _presence _default; do
      value="${!name:-}"
      ra8_rig_validate_value "$name" "$value" "$allow_empty" || return 2
    done < <(ra8_rig_contract_describe)
  }

  ra8_rig_require() {
    local name
    [[ $# -gt 0 ]] || {
      printf 'error: rig_require needs at least one declared field\n' >&2
      return 2
    }
    for name in "$@"; do
      ra8_rig_validate_value "$name" "${!name:-}" false || return 2
    done
  }

  _ra8_rig_selftest_valid() {
    local count=0 field value
    while IFS=$'\t' read -r field value; do
      ra8_rig_validate_value "$field" "$value" false >/dev/null || return 1
      count=$((count + 1))
    done <<'VALID'
PI_HOST	star
PI_HOST	star.local
PI_HOST	sikar@10.0.40.103
PI_HOST	bench-user@bench-star
PI_HOST	192.168.1.20
JLINK_SN	123456789
JLINK_SN	J-Link_1.2
JLINK_DEVICE	R7KA8D2KF_CPU0
PI_REPO	ra8-firmware
PI_REPO	work/ra8_firmware
PI_REPO	/home/ra8-hil/ra8-firmware
PI_REPO	.ra8/firmware
VALID

    ra8_rig_validate_value PI_HOST '' true >/dev/null || return 1
    printf '%d\n' "$((count + 1))"
  }

  _ra8_rig_selftest_invalid() {
    local count=0 field value
    while IFS=$'\t' read -r field value; do
      if ra8_rig_validate_value "$field" "$value" false >/dev/null 2>&1; then
        printf 'selftest: unsafe %s value passed: %q\n' "$field" "$value" >&2
        return 1
      fi
      count=$((count + 1))
    done <<'INVALID'
PI_HOST	-oProxyCommand=bad
PI_HOST	user@@host
PI_HOST	@host
PI_HOST	user@
PI_HOST	user name@host
PI_HOST	.user@host
PI_HOST	host;command
PI_HOST	1.2.3.999
PI_HOST	.host
PI_HOST	host.
PI_HOST	bad..host
PI_HOST	bad_host
JLINK_SN	-1234
JLINK_SN	.serial
JLINK_SN	serial value
JLINK_DEVICE	device;command
PI_REPO	/
PI_REPO	../repo
PI_REPO	a/../b
PI_REPO	./repo
PI_REPO	repo/
PI_REPO	a//b
PI_REPO	-repo
PI_REPO	a/-repo
PI_REPO	repo path
PI_REPO	repo'quote
PI_REPO	$HOME/repo
INVALID

    value=$'host\ncommand'
    if ra8_rig_validate_value PI_HOST "$value" false >/dev/null 2>&1; then return 1; fi
    count=$((count + 1))
    value=$'repo\033path'
    if ra8_rig_validate_value PI_REPO "$value" false >/dev/null 2>&1; then return 1; fi
    count=$((count + 1))
    if ra8_rig_validate_value UNKNOWN value false >/dev/null 2>&1; then return 1; fi
    printf '%d\n' "$((count + 1))"
  }

  _ra8_rig_descendant_probe() {
    if /bin/bash -c 'declare -F probe >/dev/null'; then
      printf 'child=1\n'
    else
      printf 'child=0\n'
    fi
  }

  _ra8_rig_selftest_descendant() {
    local raw_function='BASH_FUNC_probe%%=() { printf imported; }'
    local control protected source_blocked source_clean
    control="$(/usr/bin/env "$raw_function" /bin/bash -c \
      'if declare -F probe >/dev/null; then printf "child=1\n"; else printf "child=0\n"; fi')"
    [[ "$control" == child=1 ]] || {
      printf 'selftest: raw function control did not import\n' >&2
      return 1
    }
    protected="$(/usr/bin/env "$raw_function" /bin/bash -p \
      "${BASH_SOURCE[0]}" --descendant-selftest)"
    [[ "$protected" == child=0 ]] || {
      printf 'selftest: protected descendant imported a raw function\n' >&2
      return 1
    }
    source_blocked="$(/usr/bin/env "$raw_function" /bin/bash -p -c \
      "if source \"\$1\"; then printf 'source=allowed\\n'; else printf 'source=blocked\\n'; fi" \
      rig-source "${BASH_SOURCE[0]}" 2>/dev/null)"
    [[ "$source_blocked" == source=blocked ]] || {
      printf 'selftest: sourced contract accepted a raw function environment\n' >&2
      return 1
    }
    source_clean="$(/bin/bash -p -c \
      "source \"\$1\"; ra8_rig_validate_value PI_HOST host false; printf 'source=clean\\n'" \
      rig-source "${BASH_SOURCE[0]}")"
    [[ "$source_clean" == source=clean ]] || {
      printf 'selftest: clean privileged source path failed\n' >&2
      return 1
    }
    printf '2\n'
  }

  _ra8_rig_contract_selftest() {
    local must_pass must_fire descendant_fire
    must_pass="$(_ra8_rig_selftest_valid)" || return 1
    must_fire="$(_ra8_rig_selftest_invalid)" || return 1
    descendant_fire="$(_ra8_rig_selftest_descendant)" || return 1
    must_fire=$((must_fire + descendant_fire))
    printf 'rig_contract.sh --selftest: PASS (%d must-pass, %d must-fire)\n' \
      "$must_pass" "$must_fire"
  }

  _ra8_rig_contract_main() {
    local default
    case "${1:-}" in
      --describe)
        [[ $# -eq 1 ]] || return 2
        ra8_rig_contract_describe
        ;;
      --validate)
        [[ $# -eq 3 ]] || return 2
        ra8_rig_validate_value "$2" "$3" false
        ;;
      --default)
        [[ $# -eq 2 ]] || return 2
        default="$(ra8_rig_contract_default "$2")" || return 2
        ra8_rig_validate_value "$2" "$default" false || return 2
        printf '%s\n' "$default"
        ;;
      --selftest)
        [[ $# -eq 1 ]] || return 2
        _ra8_rig_contract_selftest
        ;;
      --descendant-selftest)
        [[ $# -eq 1 ]] || return 2
        _ra8_rig_descendant_probe
        ;;
      *)
        printf 'usage: rig_contract.sh --describe | --default FIELD | --validate FIELD VALUE | --selftest\n' >&2
        return 2
        ;;
    esac
  }

  if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    _ra8_rig_contract_main "$@"
  fi
else
  [[ "$-" == *p* ]]
fi
