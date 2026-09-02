#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Compatibility wrapper for the former `test-docker` entry point. Image
# selection, staleness, runtime/Colima handling, TTY behavior, and the writable
# mount now belong to devcontainer_run.sh; the test recipe remains the single
# definition of how host tests are built and run.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
rebuild=()

case "${1:-}" in
  "") ;;
  --rebuild)
    rebuild=(--rebuild)
    shift
    ;;
  -h | --help)
    echo "Usage: scripts/ci/test-docker.sh [--rebuild]"
    exit 0
    ;;
  *)
    echo "Unknown flag: $1" >&2
    exit 2
    ;;
esac
[[ "$#" -eq 0 ]] || {
  echo "Unknown flag: $1" >&2
  exit 2
}

exec bash "$SCRIPT_DIR/devcontainer_run.sh" "${rebuild[@]}" -- \
  just quality::local::test
