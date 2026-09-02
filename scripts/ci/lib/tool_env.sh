#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/ci/lib/tool_env.sh -- deterministic tool resolution for every gate.
#
# Normally sourced: scripts/ci.sh sources this once and calls
# use_pinned_tool_path at the top of run_one_gate, so EVERY gate -- however the
# shell was entered -- resolves the same pinned binaries. Direct execution is
# reserved for ``--selftest``; the toolchain-parity gate runs that contract
# test before it trusts the selected binaries.
#
# Why (#333): the self-hosted runner and the dev box resolve tools through
# PATH, and PATH differs between a login shell and a non-interactive one.
# Measured on the dev box, `ssh dev '<cmd>'` and `ssh dev 'bash -lc "<cmd>"'`
# resolved DIFFERENT binaries for the same tool -- shellcheck 0.9.0 vs 0.11.0,
# shfmt 3.6.0 vs 3.13.1, ruff absent vs present in ~/.local/bin. GitHub Actions
# runs each step in a non-login shell too, so "green under a login shell, red in
# CI" (and the reverse) was structural. Several agents even reported
# `lint-py-shell` FAIL with "ruff not found" and installed a ruff that was
# present the whole time, just invisible to a non-interactive PATH.
#
# The fix is one PATH contract for every gate and every pinned tool. Python-
# managed gate dependencies resolve from the isolated /opt venv on long-lived
# hosts and containers, or from a per-job venv exported through GITHUB_PATH.
# A container declares that environment through RA8_TOOL_VENV; in that mode a
# bind-mounted repository .venv is excluded completely because its absolute
# entrypoint shebangs belong to the host filesystem, not the container.
# Determinism is only half of it: check_tool_versions.py then
# makes the WRONG version FAIL LOUD, so a deterministic-but-wrong toolchain
# cannot pass either.
#
# The whole guarded block is idempotent so any number of scripts can source it.
if [ -z "${_RA8_TOOL_ENV_SH:-}" ]; then
  _RA8_TOOL_ENV_SH=1
  _RA8_MANAGED_AUTH_CACHE_KEY=""
  _RA8_MANAGED_AUTH_CACHE_BIN=""
  _RA8_AUTHENTICATED_TOOL_VENV_BIN=""
  _RA8_SELECTED_MANAGED_BIN=""
  _RA8_SELECTED_REPO_BIN=""

  _ra8_managed_authentication_key() {
    local repo_root="$1" selected="$2"
    /usr/bin/python3 -I "${repo_root}/scripts/dev/managed_python_env.py" cache-key \
      --env "${selected}" --pyproject "${repo_root}/pyproject.toml" \
      --lock "${repo_root}/uv.lock" --group ci
  }

  _ra8_restore_managed_authentication() {
    local key="$1"
    _RA8_AUTHENTICATED_TOOL_VENV_BIN=""
    if [ -n "${_RA8_MANAGED_AUTH_CACHE_KEY}" ] &&
      [ "${key}" = "${_RA8_MANAGED_AUTH_CACHE_KEY}" ]; then
      _RA8_AUTHENTICATED_TOOL_VENV_BIN="${_RA8_MANAGED_AUTH_CACHE_BIN}"
      return 0
    fi
    return 1
  }

  _ra8_authenticated_tool_venv_bin() {
    local repo_root="$1" selected="${2:-}" key verified_bin verified_key
    _RA8_AUTHENTICATED_TOOL_VENV_BIN=""
    [ -n "${selected}" ] || return 0
    selected="${selected%/}"
    key="$(_ra8_managed_authentication_key "${repo_root}" "${selected}")" || return 1
    _ra8_restore_managed_authentication "${key}" && return 0
    verified_bin="$(
      /usr/bin/python3 -I "${repo_root}/scripts/dev/managed_python_env.py" verify \
        --env "${selected}" --pyproject "${repo_root}/pyproject.toml" \
        --lock "${repo_root}/uv.lock" --group ci --print-bin
    )" || return 1
    verified_key="$(_ra8_managed_authentication_key \
      "${repo_root}" "${selected}")" || return 1
    [ "${key}" = "${verified_key}" ] || {
      echo "tool_env.sh: managed environment inputs changed during authentication" >&2
      return 1
    }
    _RA8_MANAGED_AUTH_CACHE_KEY="${verified_key}"
    _RA8_MANAGED_AUTH_CACHE_BIN="${verified_bin}"
    _RA8_AUTHENTICATED_TOOL_VENV_BIN="${verified_bin}"
  }

  _ra8_select_python_bins() {
    local repo_root="$1" repo_bin="$2" managed_bin="" repo_selected=""
    _RA8_SELECTED_MANAGED_BIN=""
    _RA8_SELECTED_REPO_BIN=""
    _ra8_authenticated_tool_venv_bin \
      "${repo_root}" "${RA8_TOOL_VENV:-}" || return 1
    managed_bin="${_RA8_AUTHENTICATED_TOOL_VENV_BIN}"
    if [ -z "${managed_bin}" ] && [ -x "${repo_bin}/python3" ]; then
      repo_selected="${repo_bin}"
    elif [ -z "${managed_bin}" ] && [ -d /opt/ra8-python-tools ]; then
      _ra8_authenticated_tool_venv_bin \
        "${repo_root}" /opt/ra8-python-tools || return 1
      managed_bin="${_RA8_AUTHENTICATED_TOOL_VENV_BIN}"
    fi
    _RA8_SELECTED_MANAGED_BIN="${managed_bin}"
    _RA8_SELECTED_REPO_BIN="${repo_selected}"
  }

  # use_pinned_tool_path -- arrange the directories where the pinned host tools
  # live, so a non-login shell resolves exactly what a login shell does.
  #
  # Precedence is a contract, from highest to lowest:
  #
  #   1. RA8_TOOL_BIN, when an operator explicitly sets that override;
  #   2. RA8_TOOL_VENV/bin when the runtime declares a managed environment;
  #   3. the repository .venv/bin for native development, but only when no
  #      managed environment was declared;
  #   4. /opt/ra8-python-tools/bin, the managed-host fallback;
  #   5. pinned platform prefixes (/opt/homebrew/bin, /usr/local/bin);
  #   6. ~/.local/bin, retained only as the lowest-priority legacy tool prefix;
  #   7. every other inherited PATH entry, in its original order.
  #
  # In particular, discovering ~/.local/bin must never prepend it ahead of a
  # repository venv: that once selected a user-installed gcovr 8.6 instead of
  # the repository-pinned gcovr 7.0 during coverage-tree. Existing occurrences
  # are removed and reinserted in the order above. The rebuilt PATH is deduped,
  # so repeated calls are exactly idempotent rather than growing PATH forever.
  use_pinned_tool_path() {
    local dir existing found repo_root repo_bin managed_bin="" repo_selected=""
    local ordered_path
    local original_path="${PATH:-}"
    local -a preferred_dirs=() inherited_dirs=() ordered_dirs=()
    repo_root="${REPO_ROOT:-}"
    if [ -z "${repo_root}" ]; then
      repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
    fi
    repo_bin="${repo_root}/.venv/bin"
    _ra8_select_python_bins "${repo_root}" "${repo_bin}" || return 1
    managed_bin="${_RA8_SELECTED_MANAGED_BIN}"
    repo_selected="${_RA8_SELECTED_REPO_BIN}"
    preferred_dirs=("${RA8_TOOL_BIN:-}" "${managed_bin}" "${repo_selected}")
    preferred_dirs+=(
      /opt/homebrew/bin
      /usr/local/bin
      "${HOME:-}/.local/bin"
    )
    IFS=: read -r -a inherited_dirs <<<"${original_path}"
    for dir in "${preferred_dirs[@]}"; do
      [ -n "${dir}" ] && [ -d "${dir}" ] || continue
      found=0
      for existing in "${ordered_dirs[@]}"; do
        if [ "${existing}" = "${dir}" ]; then
          found=1
          break
        fi
      done
      [ "${found}" -eq 0 ] && ordered_dirs+=("${dir}")
    done

    for dir in "${inherited_dirs[@]}"; do
      # Empty inherited fields mean the current directory. Preserve that
      # meaning explicitly; unlike preferred entries, inherited directories
      # remain present even when they do not exist yet.
      [ -n "${dir}" ] || dir=.
      if [ -n "${managed_bin}" ]; then
        case "${dir}" in
          "${repo_root}/.venv" | "${repo_root}/.venv/"*) continue ;;
        esac
      fi
      found=0
      for existing in "${ordered_dirs[@]}"; do
        if [ "${existing}" = "${dir}" ]; then
          found=1
          break
        fi
      done
      [ "${found}" -eq 0 ] && ordered_dirs+=("${dir}")
    done

    ordered_path="$(
      IFS=:
      printf '%s' "${ordered_dirs[*]}"
    )"
    PATH="${ordered_path}"
    export PATH
  }

  _ra8_tool_path_count() {
    local path_value="$1" wanted="$2" entry count=0
    local -a entries=()
    IFS=: read -r -a entries <<<"${path_value}"
    for entry in "${entries[@]}"; do
      [ "${entry}" = "${wanted}" ] && count=$((count + 1))
    done
    printf '%s\n' "${count}"
  }

  _ra8_tool_path_override_selftest() {
    local override_bin="$1" repo_bin="$2" first second resolved
    local RA8_TOOL_BIN="${override_bin}" PATH="${PATH}" RA8_TOOL_VENV
    unset RA8_TOOL_VENV
    use_pinned_tool_path
    first="${PATH%%:*}"
    second="${PATH#*:}"
    second="${second%%:*}"
    resolved="$(ra8-tool-path-probe)"
    [ "${first}" = "${override_bin}" ] && [ "${second}" = "${repo_bin}" ] || {
      echo "tool_env.sh --selftest: operator override precedence is wrong: ${PATH}" >&2
      return 1
    }
    [ "${resolved}" = operator ] || {
      echo "tool_env.sh --selftest: RA8_TOOL_BIN did not win resolution" >&2
      return 1
    }
  }

  _ra8_tool_path_managed_refusal_selftest() {
    local repo_root="$1" managed_venv="$2" inherited="$3"
    local REPO_ROOT="${repo_root}" RA8_TOOL_VENV="${managed_venv}"
    local PATH="${repo_root}/.venv/bin:${inherited}:${repo_root}/.venv/bin"
    local RA8_TOOL_BIN="" failure
    unset RA8_TOOL_BIN
    if failure="$(use_pinned_tool_path 2>&1)"; then
      echo "tool_env.sh --selftest: arbitrary managed venv did not fail closed" >&2
      return 1
    fi
    case "${failure}" in
      *"managed environment"* | *"managed-environment"*) ;;
      *)
        echo "tool_env.sh --selftest: arbitrary managed venv gave no useful error: ${failure}" >&2
        return 1
        ;;
    esac
  }

  _ra8_tool_path_cache_selftest() {
    local _RA8_MANAGED_AUTH_CACHE_KEY="expected"
    local _RA8_MANAGED_AUTH_CACHE_BIN="/trusted/bin"
    local _RA8_AUTHENTICATED_TOOL_VENV_BIN=""
    _ra8_restore_managed_authentication expected
    [ "${_RA8_AUTHENTICATED_TOOL_VENV_BIN}" = /trusted/bin ] || {
      echo "tool_env.sh --selftest: exact managed-auth cache key did not restore" >&2
      return 1
    }
    if _ra8_restore_managed_authentication changed; then
      echo "tool_env.sh --selftest: changed managed-auth cache key was accepted" >&2
      return 1
    fi
    [ -z "${_RA8_AUTHENTICATED_TOOL_VENV_BIN}" ] || {
      echo "tool_env.sh --selftest: rejected cache key retained a managed bin" >&2
      return 1
    }
  }

  _ra8_tool_path_fixture_authority() {
    local source_root="$1" scratch="$2"
    mkdir -p "${scratch}/repo/scripts/dev"
    /bin/cp "${source_root}/scripts/dev/managed_python_env.py" \
      "${scratch}/repo/scripts/dev/managed_python_env.py"
    /bin/cp "${source_root}/pyproject.toml" "${scratch}/repo/pyproject.toml"
    /bin/cp "${source_root}/uv.lock" "${scratch}/repo/uv.lock"
  }

  _ra8_tool_path_fixture_bins() {
    local repo_bin="$1" managed_bin="$2" legacy_bin="$3" override_bin="$4"
    printf '#!/usr/bin/env bash\nprintf "repo-venv\\n"\n' >"${repo_bin}/ra8-tool-path-probe"
    printf '#!/usr/bin/env bash\nexit 0\n' >"${repo_bin}/python3"
    printf '#!/usr/bin/env bash\nprintf "legacy\\n"\n' >"${legacy_bin}/ra8-tool-path-probe"
    printf '#!/usr/bin/env bash\nprintf "operator\\n"\n' >"${override_bin}/ra8-tool-path-probe"
    printf '#!/usr/bin/env bash\nprintf "managed\\n"\n' >"${managed_bin}/ra8-tool-path-probe"
    printf '#!/usr/bin/env bash\nexit 0\n' >"${managed_bin}/python3"
    chmod +x \
      "${repo_bin}/ra8-tool-path-probe" \
      "${repo_bin}/python3" \
      "${managed_bin}/ra8-tool-path-probe" \
      "${managed_bin}/python3" \
      "${legacy_bin}/ra8-tool-path-probe" \
      "${override_bin}/ra8-tool-path-probe"
  }

  _ra8_tool_path_selftest() {
    set -euo pipefail
    local scratch repo_bin managed_bin legacy_bin override_bin first resolved once source_root
    local REPO_ROOT="${REPO_ROOT:-}" HOME="${HOME:-}" PATH="${PATH:-}"
    local RA8_TOOL_BIN="${RA8_TOOL_BIN:-}" RA8_TOOL_VENV
    scratch="$(mktemp -d "${TMPDIR:-/tmp}/ra8-tool-env.XXXXXXXX")"
    trap 'rm -rf "${scratch}"' RETURN
    source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
    repo_bin="${scratch}/repo/.venv/bin"
    managed_bin="${scratch}/managed/bin"
    legacy_bin="${scratch}/home/.local/bin"
    override_bin="${scratch}/override/bin"
    mkdir -p \
      "${repo_bin}" "${managed_bin}" "${legacy_bin}" "${override_bin}"
    _ra8_tool_path_fixture_authority "${source_root}" "${scratch}"
    _ra8_tool_path_fixture_bins \
      "${repo_bin}" "${managed_bin}" "${legacy_bin}" "${override_bin}"

    REPO_ROOT="${scratch}/repo"
    HOME="${scratch}/home"
    PATH="${legacy_bin}:/usr/bin:${repo_bin}:${legacy_bin}"
    unset RA8_TOOL_BIN RA8_TOOL_VENV
    use_pinned_tool_path
    first="${PATH%%:*}"
    resolved="$(ra8-tool-path-probe)"
    [ "${first}" = "${repo_bin}" ] || {
      echo "tool_env.sh --selftest: repository venv is not first: ${PATH}" >&2
      return 1
    }
    [ "${resolved}" = repo-venv ] || {
      echo "tool_env.sh --selftest: repository venv did not win resolution" >&2
      return 1
    }
    [ "$(_ra8_tool_path_count "${PATH}" "${repo_bin}")" -eq 1 ]
    [ "$(_ra8_tool_path_count "${PATH}" "${legacy_bin}")" -eq 1 ]

    once="${PATH}"
    use_pinned_tool_path
    [ "${PATH}" = "${once}" ] || {
      echo "tool_env.sh --selftest: repeated call changed PATH" >&2
      return 1
    }

    _ra8_tool_path_override_selftest "${override_bin}" "${repo_bin}"
    _ra8_tool_path_managed_refusal_selftest \
      "${scratch}/repo" "${scratch}/managed" "${legacy_bin}:/usr/bin"
    _ra8_tool_path_cache_selftest

    echo "tool_env.sh --selftest: PASS (managed refusal, native venv, override, idempotence)"
  }

  # require_tool_versions -- fail the calling gate unless each named tool
  # resolves to its project-pinned version. The pins live in one place
  # (.devcontainer/Dockerfile) and are read by check_tool_versions.py, whose
  # --selftest proves the comparator. A gate calls this for the tools it uses,
  # right where require_cmd already guards their presence, turning "wrong
  # version" from a silent divergence into a loud failure (CLAUDE.md: gates fail
  # loudly on a missing OR wrong tool).
  require_tool_versions() {
    python3 scripts/checks/check_tool_versions.py "$@"
  }
fi

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  case "${1:-}" in
    --selftest) _ra8_tool_path_selftest ;;
    *)
      echo "Usage: scripts/ci/lib/tool_env.sh --selftest" >&2
      exit 2
      ;;
  esac
fi
