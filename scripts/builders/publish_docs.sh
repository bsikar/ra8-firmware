#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# publish_docs.sh -- publish the generated Doxygen HTML to the gh-pages branch.
#
# This does NOT build: run `just docs::build` first, or use `just docs::push`, which
# chains the build and this publish step. It takes the already-built
# build/docs/html/ tree and force-pushes it as a single fresh commit onto the
# orphan `gh-pages` branch, so that branch never accumulates a history of
# generated HTML (each publish fully replaces the previous one).
#
# Remote / auth:
#   - CI:    GitHub Actions provides GITHUB_TOKEN + GITHUB_REPOSITORY; the push
#            uses a Git askpass helper, keeping the token out of argv and URLs.
#   - Local: no env needed; the push targets the `origin` remote using your
#            existing git credentials (SSH or HTTPS).
#
# One-time repo setup (owner): Settings -> Pages -> Build and deployment ->
# Source = "Deploy from a branch", Branch = gh-pages / (root).
#

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

  SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
  # shellcheck source=scripts/dev/git_environment.sh
  . "${ROOT_DIR}/scripts/dev/git_environment.sh"
  HTML_DIR="${ROOT_DIR}/build/docs/html"
  BRANCH="gh-pages"

  if [[ -n "${GITHUB_TOKEN:-}" && -z "${GITHUB_REPOSITORY:-}" ]] ||
    [[ -z "${GITHUB_TOKEN:-}" && -n "${GITHUB_REPOSITORY:-}" ]]; then
    echo "publish_docs.sh: GITHUB_TOKEN and GITHUB_REPOSITORY must be set together." >&2
    exit 2
  fi

  if [[ ! -f "${HTML_DIR}/index.html" ]]; then
    echo "publish_docs.sh: ${HTML_DIR}/index.html not found -- run 'just docs::build' first." >&2
    exit 1
  fi

  # Human-readable source revision, recorded in the gh-pages commit message.
  SRC_REV="$("$RA8_TRUSTED_GIT" -C "${ROOT_DIR}" rev-parse --short HEAD)"

  CI_AUTH=0
  PUBLISH_TOKEN=""
  if [[ -n "${GITHUB_TOKEN:-}" ]]; then
    if [[ ! "$GITHUB_REPOSITORY" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]]; then
      echo "publish_docs.sh: invalid GITHUB_REPOSITORY owner/name." >&2
      exit 2
    fi
    REMOTE="https://github.com/${GITHUB_REPOSITORY}.git"
    ASKPASS="${ROOT_DIR}/scripts/git/github_askpass.sh"
    [[ -x "$ASKPASS" ]] || {
      echo "publish_docs.sh: GitHub askpass helper is not executable: $ASKPASS" >&2
      exit 2
    }
    CI_AUTH=1
    PUBLISH_TOKEN="$GITHUB_TOKEN"
    unset GITHUB_TOKEN
  else
    REMOTE="$("$RA8_TRUSTED_GIT" -C "${ROOT_DIR}" remote get-url origin)"
    ASKPASS=""
  fi

  # Hooks export repository-local GIT_* routing. The independent publication
  # repository is created in a strict subshell so the operator's SSH/credential
  # transport remains available to the later, explicitly bounded push.
  if [[ -e "${HTML_DIR}/.git" || -L "${HTML_DIR}/.git" ]]; then
    echo "publish_docs.sh: refusing generated HTML containing a .git entry." >&2
    exit 1
  fi

  PUBLISH_ROOT="$(mktemp -d)"
  WORK_DIR="${PUBLISH_ROOT}/work"
  TEMPLATE_DIR="${PUBLISH_ROOT}/empty-template"
  mkdir -p "$WORK_DIR" "$TEMPLATE_DIR"
  trap 'rm -rf "${PUBLISH_ROOT}"' EXIT

  cp -R "${HTML_DIR}/." "${WORK_DIR}/"
  # Disable Jekyll so GitHub Pages serves doxygen's _-prefixed asset files as-is.
  touch "${WORK_DIR}/.nojekyll"

  (
    install_sanitized_git_environment
    cd "${WORK_DIR}"
    "$RA8_TRUSTED_GIT" -c core.hooksPath=/dev/null init -q --template="$TEMPLATE_DIR"
    "$RA8_TRUSTED_GIT" -c core.hooksPath=/dev/null checkout -q -b "${BRANCH}"
    "$RA8_TRUSTED_GIT" -c core.hooksPath=/dev/null add -A
    "$RA8_TRUSTED_GIT" \
      -c core.hooksPath=/dev/null \
      -c user.name="ra8-docs" \
      -c user.email="ra8-docs@users.noreply.github.com" \
      commit -q -m "docs: publish from ${SRC_REV}"
  )
  if ((CI_AUTH)); then
    (
      install_sanitized_git_environment
      GITHUB_TOKEN="$PUBLISH_TOKEN" GIT_ASKPASS="$ASKPASS" GIT_TERMINAL_PROMPT=0 \
        "$RA8_TRUSTED_GIT" -C "$WORK_DIR" -c core.hooksPath=/dev/null push -q -f "${REMOTE}" "${BRANCH}"
    )
  else
    run_git_network_with_inherited_transport \
      -C "$WORK_DIR" -c core.hooksPath=/dev/null push -q -f "${REMOTE}" "${BRANCH}"
  fi

  echo "publish_docs.sh: published build/docs/html -> ${BRANCH} (source ${SRC_REV})."
else
  [[ "$-" == *p* ]]
fi
