# shellcheck shell=bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/ci/lib/tool_env.sh -- deterministic tool resolution for every gate.
#
# SOURCED, NEVER EXECUTED. scripts/ci.sh sources this once and calls
# use_pinned_tool_path at the top of run_one_gate, so EVERY gate -- however the
# shell was entered -- resolves the same pinned binaries.
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
# The fix is the same discipline the coverage job already applied by hand
# (`echo "$HOME/.local/bin" >> "$GITHUB_PATH"` so its pip --user gcovr resolved)
# -- generalised to every gate and every pinned tool, so no gate needs its own
# PATH plumbing. Determinism is only half of it: check_tool_versions.py then
# makes the WRONG version FAIL LOUD, so a deterministic-but-wrong toolchain
# cannot pass either.
#
# The whole guarded block is idempotent so any number of scripts can source it.
if [ -z "${_RA8_TOOL_ENV_SH:-}" ]; then
  _RA8_TOOL_ENV_SH=1

  # use_pinned_tool_path -- prepend the directories where the pinned host tools
  # live, so a non-login shell resolves exactly what a login shell does.
  #
  # ~/.local/bin (pip --user: ruff, gcovr, cmake-format, yamllint) is the
  # directory a login shell adds and a non-login shell does not, so it is the
  # one that actually differs; /usr/local/bin (shellcheck, shfmt, actionlint,
  # hadolint, clang-format-22) is normally already present and is re-asserted
  # only for completeness. RA8_TOOL_BIN is an operator override for a box that
  # keeps its pinned tools elsewhere.
  #
  # Ordering matches a login shell: ~/.local/bin wins over /usr/local/bin, which
  # is why a directory already on PATH is left in place (not reordered) while a
  # missing one is prepended. Only directories that exist are added, and an
  # already-present directory is never duplicated, so repeated calls are inert.
  use_pinned_tool_path() {
    local dir
    for dir in \
      /usr/local/bin \
      "${HOME:-}/.local/bin" \
      "${RA8_TOOL_BIN:-}"; do
      [ -n "${dir}" ] && [ -d "${dir}" ] || continue
      case ":${PATH}:" in
        *":${dir}:"*) continue ;;
      esac
      PATH="${dir}:${PATH}"
    done
    export PATH
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
