#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# check_stack_usage.sh -- thin wrapper around stack_usage_check.py.
#
# Runs the python aggregator over whichever app(s) have .su files in
# their build directories and prints a one-line summary. Intended to be
# safe to run from any cwd and from CI.
#
# Usage:
#   bash scripts/checks/check_stack_usage.sh [--frame-limit N] [--quiet]
#
# Exits non-zero only if a critical-path module violates its budget --
# see stack_usage_check.py for the rules. Soft violations are reported
# but do not fail this script (the per-target -Wstack-usage=N warning
# is the build-time gate).
#
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"

PY="${PYTHON:-python3}"

# Find the app whose build dir was most recently touched, just for the
# summary line. The python script itself walks every app.
LATEST_APP=""
LATEST_MTIME=0
shopt -s nullglob
while IFS= read -r -d '' build_dir; do
  [ -d "${build_dir}" ] || continue
  if mt=$(stat -f %m "${build_dir}" 2>/dev/null) ||
    mt=$(stat -c %Y "${build_dir}" 2>/dev/null); then
    if [ "${mt}" -gt "${LATEST_MTIME}" ]; then
      LATEST_MTIME=${mt}
      LATEST_APP="$(basename "$(dirname "${build_dir}")")"
    fi
  fi
done < <(find "${REPO_ROOT}/examples" -type d -name 'build*' -print0)
shopt -u nullglob

if [ -n "${LATEST_APP}" ]; then
  echo "stack-usage: most-recently-built app = ${LATEST_APP}"
else
  echo "stack-usage: no built apps found; nothing to summarise."
fi

exec "${PY}" "${SCRIPT_DIR}/stack_usage_check.py" \
  --repo-root "${REPO_ROOT}" \
  "$@"
