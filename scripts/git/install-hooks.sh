#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/git/install-hooks.sh -- point this clone at the tracked git hooks.
#
# Sets core.hooksPath to scripts/git/ so the commit-msg, pre-commit, and
# pre-push gates (AI-attribution ban, ASCII, Doxygen, copyright, ...) run for
# EVERY committer automatically. Without this, a fresh clone has no hooks in
# .git/hooks and every gate is silently skipped locally.
#
# Idempotent and silent when already configured. It is invoked automatically
# from the top-level Makefile and the top-level CMake configure step, so no
# clone can skip it; it can also be run by hand right after cloning:
#
#     ./scripts/git/install-hooks.sh        (or: make hooks)
#
#
set -euo pipefail

# No-op outside a git work tree (release tarball / vendored copy).
root="$(git rev-parse --show-toplevel 2>/dev/null)" || exit 0
[ -n "${root}" ] || exit 0

want="scripts/git"
current="$(git -C "${root}" config --local --get core.hooksPath 2>/dev/null || true)"

if [ "${current}" != "${want}" ]; then
  git -C "${root}" config core.hooksPath "${want}"
  echo "[hooks] core.hooksPath -> ${want} (commit-msg, pre-commit, pre-push now active)"
fi
