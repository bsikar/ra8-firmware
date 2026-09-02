#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Git askpass adapter for GitHub Actions. Git supplies the prompt as argv; the
# token remains in the inherited environment and never enters a remote URL or
# process argument.

set -euo pipefail

case "${1:-}" in
  *Username*) printf '%s\n' "x-access-token" ;;
  *Password*) printf '%s\n' "${GITHUB_TOKEN:?GITHUB_TOKEN is required}" ;;
  *) exit 1 ;;
esac
