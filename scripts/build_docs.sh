#!/usr/bin/env bash
#
# build_docs.sh -- generate the ra8-firmware Doxygen HTML documentation.
#
# Usage:
#   bash scripts/build_docs.sh         -- build into build/docs/html/
#   bash scripts/build_docs.sh --open  -- build, then open index.html
#   bash scripts/build_docs.sh --gate  -- warning-gate build into
#                                         build/docs-gate/ (used by CI)
#
# Always builds with the project-pinned doxygen release, resolved (and
# downloaded on first use) by scripts/utils/provision_doxygen.sh. The
# in-tree HTML header template and the vendored doxygen-awesome theme are
# only valid for that exact version -- see docs/DOCS.md.
#
# Auto-detects whether `dot` (graphviz) is on PATH. If absent, falls
# back to text-only output (HAVE_DOT=NO) so the build still succeeds.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DOXYFILE="${ROOT_DIR}/Doxyfile"

OPEN_AFTER=0
GATE_MODE=0
for arg in "$@"; do
  case "${arg}" in
    --open) OPEN_AFTER=1 ;;
    --gate) GATE_MODE=1 ;;
    -h | --help)
      sed -n '3,16p' "${BASH_SOURCE[0]}"
      exit 0
      ;;
    *)
      echo "build_docs.sh: unknown argument '${arg}'" >&2
      exit 2
      ;;
  esac
done

if [[ "${GATE_MODE}" -eq 1 ]]; then
  OUTPUT_DIR="${ROOT_DIR}/build/docs-gate"
else
  OUTPUT_DIR="${ROOT_DIR}/build/docs"
fi
OUTPUT_HTML="${OUTPUT_DIR}/html"
WARN_LOG="${OUTPUT_DIR}/doxygen-warnings.log"

if [[ ! -f "${DOXYFILE}" ]]; then
  echo "build_docs.sh: ${DOXYFILE} not found." >&2
  exit 1
fi

DOXYGEN_BIN="$(bash "${SCRIPT_DIR}/utils/provision_doxygen.sh")"
echo "build_docs.sh: using doxygen $("${DOXYGEN_BIN}" --version) (${DOXYGEN_BIN})"

# PROJECT_NUMBER in the Doxyfile expands $(RA8_PROJECT_VERSION) so the docs
# always carry the version from the top-level VERSION file.
RA8_PROJECT_VERSION="$(tr -d '[:space:]' <"${ROOT_DIR}/VERSION")"
export RA8_PROJECT_VERSION

OVERRIDES=""
if command -v dot >/dev/null 2>&1; then
  echo "build_docs.sh: graphviz detected -- enabling call/caller graphs."
else
  echo "build_docs.sh: graphviz NOT detected -- generating text-only docs."
  OVERRIDES+=$'HAVE_DOT=NO\nCALL_GRAPH=NO\nCALLER_GRAPH=NO\n'
fi

if [[ "${GATE_MODE}" -eq 1 ]]; then
  # The firmware.yml "Doxygen warnings" gate: same Doxyfile, same pinned
  # doxygen, but a separate output tree, no *automatic* graphs (speed), and
  # private members extracted so their doc blocks are checked too.
  OVERRIDES+="OUTPUT_DIRECTORY=${OUTPUT_DIR}"$'\n'
  OVERRIDES+="WARN_LOGFILE=${WARN_LOG}"$'\n'
  OVERRIDES+=$'EXTRACT_PRIVATE=YES\nWARN_IF_UNDOCUMENTED=YES\n'
  # Only the AUTOMATIC per-function call/caller graphs are disabled here: those
  # are the expensive ones (one dot run per documented function, thousands of
  # them). HAVE_DOT itself stays ON so hand-written @dot blocks are actually
  # parsed and rendered -- turning it off made doxygen emit "ignoring \dot
  # command because HAVE_DOT is not set" for every such block, which this gate
  # then reported as a failure. A warning gate that cannot see the diagrams it
  # is meant to validate is not checking them.
  OVERRIDES+=$'CALL_GRAPH=NO\nCALLER_GRAPH=NO\n'
fi

mkdir -p "${OUTPUT_DIR}"
cd "${ROOT_DIR}"

# Regenerate the directory/generation-driven navigation trees (the grouped
# "Guides & Reference" doc sections and the per-tier "Example Applications"
# index) so the sidebar can never drift from the on-disk tree. Output lands in
# docs/generated/ (gitignored) and is consumed by the Doxyfile INPUT.
echo "build_docs.sh: regenerating navigation (scripts/utils/gen_doxygen_nav.py)."
python3 "${SCRIPT_DIR}/utils/gen_doxygen_nav.py"

if [[ -n "${OVERRIDES}" ]]; then
  {
    cat "${DOXYFILE}"
    printf '%s' "${OVERRIDES}"
  } | "${DOXYGEN_BIN}" -
else
  "${DOXYGEN_BIN}" "${DOXYFILE}"
fi

echo
echo "build_docs.sh: documentation written to ${OUTPUT_HTML}"
if [[ -f "${WARN_LOG}" ]]; then
  WARN_COUNT=$(wc -l <"${WARN_LOG}" | tr -d ' ')
  echo "build_docs.sh: doxygen warning lines: ${WARN_COUNT} (see ${WARN_LOG})"
fi

INDEX_HTML="${OUTPUT_HTML}/index.html"
if [[ ! -f "${INDEX_HTML}" ]]; then
  echo "build_docs.sh: ERROR -- expected ${INDEX_HTML} was not produced." >&2
  exit 1
fi

if [[ "${OPEN_AFTER}" -eq 1 ]]; then
  if command -v open >/dev/null 2>&1; then
    open "${INDEX_HTML}"
  elif command -v xdg-open >/dev/null 2>&1; then
    xdg-open "${INDEX_HTML}"
  else
    echo "build_docs.sh: --open requested but no 'open'/'xdg-open' found."
  fi
fi
