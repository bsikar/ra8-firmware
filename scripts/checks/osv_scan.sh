#!/usr/bin/env bash
#
# scripts/checks/osv_scan.sh -- OSV CVE scan of the vendored SOUP.
#
# Runs the official osv-scanner release binary in two legs, both driven
# by the committed SBOM (docs/sbom/ra8-firmware.cdx.json):
#
#   1. SBOM purl leg -- `osv-scanner scan source -L <sbom>`. OSV.dev has
#      no purl-queryable ecosystem for git-vendored C/C++ code (verified
#      empirically: pkg:github purls return zero advisories even for
#      versions with dozens of published CVEs, because osv-scanner maps
#      the `github` purl type to the GitHub Actions ecosystem). This leg
#      is therefore a forward-compatibility net: it starts resolving the
#      moment any component gains a purl in an OSV-supported ecosystem.
#   2. Commit leg -- the leg that actually resolves advisories today.
#      OSV.dev indexes C/C++ vulnerabilities as GIT commit ranges
#      (OSS-Fuzz + converted CVEs) queryable only by commit hash.
#      `gen_sbom.py --commits` emits the pinned upstream commit of every
#      commit-pinned component; each pin is materialized as a stub git
#      checkout (a `.git` whose HEAD names the pinned commit) so
#      osv-scanner's gitrepo extractor issues the exact commit queries.
#
# Both legs write a JSON report into the output directory (uploaded as a
# CI artifact by .github/workflows/osv-scan.yml) and a human summary to
# stdout.
#
# Usage:
#   bash scripts/checks/osv_scan.sh --scanner <osv-scanner-binary> \
#                                  [--output-dir <dir>]
#
# Exit codes:
#   0  -- both legs clean (no known advisories)
#   1  -- at least one advisory found (either leg)
#   2  -- usage error
#   other -- infrastructure failure (network, bad binary, ...)
#
# osv-scanner's own exit codes (v2.4.0): 0 clean, 1 vulnerabilities
# found, 128 no package sources (benign for an all-clean leg), other
# values are hard errors.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SBOM_PATH="${REPO_ROOT}/docs/sbom/ra8-firmware.cdx.json"

SCANNER=""
OUTPUT_DIR="${REPO_ROOT}/build/osv-scan"

usage() {
  echo "usage: $0 --scanner <osv-scanner-binary> [--output-dir <dir>]" >&2
}

while [ $# -gt 0 ]; do
  case "$1" in
    --scanner)
      SCANNER="$2"
      shift 2
      ;;
    --output-dir)
      OUTPUT_DIR="$2"
      shift 2
      ;;
    *)
      usage
      exit 2
      ;;
  esac
done

if [ -z "${SCANNER}" ]; then
  usage
  exit 2
fi
if ! "${SCANNER}" --version >/dev/null 2>&1; then
  echo "osv_scan: '${SCANNER}' is not a runnable osv-scanner binary" >&2
  exit 2
fi
if [ ! -f "${SBOM_PATH}" ]; then
  echo "osv_scan: SBOM missing at ${SBOM_PATH}; run 'make sbom'" >&2
  exit 2
fi

mkdir -p "${OUTPUT_DIR}"
STUB_ROOT="$(mktemp -d)"
trap 'rm -rf "${STUB_ROOT}"' EXIT

"${SCANNER}" --version

# Render a one-line-per-advisory summary from an osv-scanner JSON report.
summarize() {
  python3 - "$1" <<'EOF'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as fh:
    doc = json.load(fh)
count = 0
for result in doc.get("results", []):
    source = result.get("source", {}).get("path", "?")
    for pkg in result.get("packages", []):
        for vuln in pkg.get("vulnerabilities", []):
            count += 1
            print(f"  {vuln.get('id')}  ({source})")
print(f"  -> {count} advisories")
EOF
}

# Run one scanner leg: $1 = report path, $2 = leg name, rest = args.
# Returns 0 (clean), 1 (findings), or dies on infrastructure errors.
run_leg() {
  report="$1"
  leg="$2"
  shift 2
  echo "=== osv_scan leg: ${leg} ==="
  rc=0
  "${SCANNER}" scan source --format json --output-file "${report}" "$@" || rc=$?
  case "${rc}" in
    0)
      echo "osv_scan: ${leg}: clean"
      return 0
      ;;
    1)
      echo "osv_scan: ${leg}: ADVISORIES FOUND"
      summarize "${report}"
      return 1
      ;;
    128)
      # "No package sources found" -- tolerated so that a future
      # SBOM shape change cannot silently pass as a clean scan of
      # zero packages: the leg still prints what happened.
      echo "osv_scan: ${leg}: no scannable packages (osv-scanner exit 128)"
      return 0
      ;;
    *)
      echo "osv_scan: ${leg}: osv-scanner failed with exit ${rc}" >&2
      exit "${rc}"
      ;;
  esac
}

# Leg 1: purl scan of the committed SBOM.
sbom_rc=0
run_leg "${OUTPUT_DIR}/osv-sbom.json" "sbom-purls" \
  -L "${SBOM_PATH}" || sbom_rc=$?

# Leg 2: commit scan of every pinned upstream commit in the SBOM registry.
# Each pin becomes a stub git checkout whose HEAD names the pinned commit;
# osv-scanner's gitrepo extractor turns each into an exact OSV commit query.
pin_count=0
while read -r key commit; do
  [ -n "${key}" ] || continue
  stub="${STUB_ROOT}/${key//\//_}/.git"
  mkdir -p "${stub}/refs/heads" "${stub}/objects/info" "${stub}/objects/pack"
  echo "ref: refs/heads/pin" >"${stub}/HEAD"
  echo "${commit}" >"${stub}/refs/heads/pin"
  printf '[core]\n\trepositoryformatversion = 0\n\tbare = false\n' >"${stub}/config"
  pin_count=$((pin_count + 1))
done < <(python3 "${REPO_ROOT}/scripts/gen/gen_sbom.py" --commits)

if [ "${pin_count}" -eq 0 ]; then
  echo "osv_scan: gen_sbom.py --commits produced no pins" >&2
  exit 2
fi
echo "osv_scan: materialized ${pin_count} pinned upstream commits"

commits_rc=0
run_leg "${OUTPUT_DIR}/osv-commits.json" "pinned-commits" \
  -r --include-git-root --allow-no-lockfiles "${STUB_ROOT}" || commits_rc=$?

if [ "${sbom_rc}" -ne 0 ] || [ "${commits_rc}" -ne 0 ]; then
  echo "osv_scan: FAIL -- known advisories affect the vendored SOUP" >&2
  echo "osv_scan: reports: ${OUTPUT_DIR}/osv-sbom.json ${OUTPUT_DIR}/osv-commits.json" >&2
  exit 1
fi
echo "osv_scan: PASS -- no known advisories for the catalogued SOUP"
