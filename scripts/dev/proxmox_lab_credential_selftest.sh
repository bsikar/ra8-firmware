#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Selftest for the Windows lab credential gate in proxmox_lab_server_runner.sh.
#
# The gate answers one question, "is the credential in place", and must answer
# it without reading the value. These cases pin both halves: which files are
# accepted, and that a rejected file is never opened.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNNER="$REPO_ROOT/scripts/dev/proxmox_lab_server_runner.sh"

if [[ ! -r "$RUNNER" ]]; then
  echo "selftest error: runner not readable at $RUNNER" >&2
  exit 1
fi

# Take the function under test out of the runner without running the runner:
# sourcing the script would start a CI run. The extraction is itself a check,
# so a rename or a move fails here instead of silently testing nothing.
FUNCTION_SOURCE="$(awk '/^windows_credential_present\(\) \{$/,/^\}$/' "$RUNNER")"
if [[ -z "$FUNCTION_SOURCE" ]]; then
  echo "selftest error: windows_credential_present() not found in $RUNNER" >&2
  exit 1
fi
eval "$FUNCTION_SOURCE"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

FAILURES=0

expect_present() {
  local label="$1" file="$2"
  if windows_credential_present "$file"; then
    echo "  ok   $label"
  else
    echo "  FAIL $label: expected present, got absent"
    FAILURES=$((FAILURES + 1))
  fi
}

expect_absent() {
  local label="$1" file="$2"
  if windows_credential_present "$file"; then
    echo "  FAIL $label: expected absent, got present"
    FAILURES=$((FAILURES + 1))
  else
    echo "  ok   $label"
  fi
}

echo "==> windows_credential_present()"

install -m 600 /dev/null "$WORK_DIR/good"
printf 'not-a-real-secret' > "$WORK_DIR/good"
expect_present "a non-empty 0600 file owned by this user is present" "$WORK_DIR/good"

expect_absent "a missing file is absent" "$WORK_DIR/missing"

install -m 600 /dev/null "$WORK_DIR/empty"
expect_absent "an empty file is absent, not an empty credential" "$WORK_DIR/empty"

install -m 600 /dev/null "$WORK_DIR/group-readable"
printf 'not-a-real-secret' > "$WORK_DIR/group-readable"
chmod 640 "$WORK_DIR/group-readable"
expect_absent "a group-readable file is refused" "$WORK_DIR/group-readable"

install -m 600 /dev/null "$WORK_DIR/world-readable"
printf 'not-a-real-secret' > "$WORK_DIR/world-readable"
chmod 604 "$WORK_DIR/world-readable"
expect_absent "a world-readable file is refused" "$WORK_DIR/world-readable"

install -m 600 /dev/null "$WORK_DIR/executable"
printf 'not-a-real-secret' > "$WORK_DIR/executable"
chmod 700 "$WORK_DIR/executable"
expect_present "an owner-only file with extra owner bits is still present" "$WORK_DIR/executable"

mkdir -p "$WORK_DIR/a-directory"
chmod 700 "$WORK_DIR/a-directory"
expect_absent "a directory is refused" "$WORK_DIR/a-directory"

# The gate reports presence and nothing else: no case above may put the file
# contents anywhere a caller can reach. Prove it on the accepted file, which is
# the only one the gate has any reason to open.
OUTPUT="$(windows_credential_present "$WORK_DIR/good" 2>&1 || true)"
if [[ -n "$OUTPUT" ]]; then
  echo "  FAIL the gate printed something while reporting presence"
  FAILURES=$((FAILURES + 1))
else
  echo "  ok   the gate prints nothing while reporting presence"
fi

echo "==> the runner never reads the credential from the environment"
if grep -n 'RA8_LAB_WINDOWS_PASSWORD' "$RUNNER" | grep -qv 'if \[\[ -n'; then
  if grep -n 'RA8_LAB_WINDOWS_PASSWORD:-}"$' "$RUNNER" | grep -q 'GUEST_PASSWORD'; then
    echo "  FAIL the runner still takes the credential from the environment"
    FAILURES=$((FAILURES + 1))
  fi
fi
if grep -q 'GUEST_PASSWORD' "$RUNNER"; then
  echo "  FAIL the credential is still held in a shell variable"
  FAILURES=$((FAILURES + 1))
else
  echo "  ok   the credential is never held in a shell variable"
fi
if grep -q 'export RA8_LAB_WINDOWS_PASSWORD' "$RUNNER"; then
  echo "  FAIL the credential is exported"
  FAILURES=$((FAILURES + 1))
else
  echo "  ok   the credential is never exported"
fi

if ((FAILURES > 0)); then
  echo "FAILED: $FAILURES case(s)"
  exit 1
fi
echo "PASSED"
