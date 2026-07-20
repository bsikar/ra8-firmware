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
  # Every AUTOMATIC graph is disabled here: call/caller, include/included-by,
  # collaboration, class and directory graphs each spawn a dot run per entity
  # (thousands of them), and the include graphs additionally exceed
  # DOT_GRAPH_MAX_NODES on the hub headers and warn about it.
  #
  # HAVE_DOT itself stays ON, so author-written diagram blocks are parsed,
  # validated and rendered. Turning it off made doxygen ignore every such block
  # and warn about it, which this gate reported as a failure -- a warning gate
  # that cannot see the diagrams it is meant to validate is not checking them.
  #
  # (Deliberately avoiding the literal doxygen command spellings in this
  # comment: scripts/ is inside INPUT, so writing them here would start a
  # verbatim block that never closes.)
  OVERRIDES+=$'CALL_GRAPH=NO\nCALLER_GRAPH=NO\n'
  OVERRIDES+=$'INCLUDE_GRAPH=NO\nINCLUDED_BY_GRAPH=NO\n'
  OVERRIDES+=$'COLLABORATION_GRAPH=NO\nCLASS_GRAPH=NO\n'
  OVERRIDES+=$'GRAPHICAL_HIERARCHY=NO\nDIRECTORY_GRAPH=NO\n'
fi

# Start from an empty output tree. Doxygen overwrites what it regenerates but
# never removes what it no longer produces, so a rebuild at a different base
# leaves orphan pages and orphan dot_inline_dotgraph_*.svg files behind. Anything
# that reads this directory to decide whether the docs are correct -- notably
# check_doc_diagrams.py -- would then be measuring a mixture of two builds, and
# leftover renders can just as easily mask a diagram the current tree drops as
# invent one it does not. Cleaning here makes "what is in this directory" mean
# exactly "what this build produced".
rm -rf "${OUTPUT_DIR}"
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

# Record which authored diagram set this build rendered. check_doc_diagrams.py
# compares the fingerprint back against the working tree and refuses to judge
# output it cannot attribute to the tree under test, so a half-stale directory
# reports "rebuild" instead of an invented count mismatch -- or, worse, a pass
# built on an orphan render.
python3 "${SCRIPT_DIR}/utils/check_doc_diagrams.py" --write-stamp --html "${OUTPUT_HTML}"

if [[ "${OPEN_AFTER}" -eq 1 ]]; then
  if command -v open >/dev/null 2>&1; then
    open "${INDEX_HTML}"
  elif command -v xdg-open >/dev/null 2>&1; then
    xdg-open "${INDEX_HTML}"
  else
    echo "build_docs.sh: --open requested but no 'open'/'xdg-open' found."
  fi
fi
