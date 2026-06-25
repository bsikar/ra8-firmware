#!/usr/bin/env bash
#
# build_docs.sh -- generate the ra8d2-firmware Doxygen HTML documentation.
#
# Usage:
#   bash scripts/build_docs.sh         -- build into build/docs/html/
#   bash scripts/build_docs.sh --open  -- build, then open index.html
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
OUTPUT_HTML="${ROOT_DIR}/build/docs/html"
WARN_LOG="${ROOT_DIR}/build/docs/doxygen-warnings.log"

OPEN_AFTER=0
for arg in "$@"; do
  case "${arg}" in
    --open) OPEN_AFTER=1 ;;
    -h | --help)
      sed -n '3,12p' "${BASH_SOURCE[0]}"
      exit 0
      ;;
    *)
      echo "build_docs.sh: unknown argument '${arg}'" >&2
      exit 2
      ;;
  esac
done

if ! command -v doxygen >/dev/null 2>&1; then
  cat >&2 <<EOF
build_docs.sh: 'doxygen' was not found on PATH.

Install it before running this script:
  Debian/Ubuntu:  sudo apt install doxygen graphviz
  macOS:          brew install doxygen graphviz
  Fedora:         sudo dnf install doxygen graphviz

Graphviz is optional (used for call/caller graphs); the build will fall
back to text-only output automatically when 'dot' is missing.
EOF
  exit 1
fi

if [[ ! -f "${DOXYFILE}" ]]; then
  echo "build_docs.sh: ${DOXYFILE} not found." >&2
  exit 1
fi

DOT_OVERRIDE=""
if command -v dot >/dev/null 2>&1; then
  echo "build_docs.sh: graphviz detected -- enabling call/caller graphs."
else
  echo "build_docs.sh: graphviz NOT detected -- generating text-only docs."
  DOT_OVERRIDE=$'HAVE_DOT=NO\nCALL_GRAPH=NO\nCALLER_GRAPH=NO\n'
fi

mkdir -p "${ROOT_DIR}/build/docs"
cd "${ROOT_DIR}"

if [[ -n "${DOT_OVERRIDE}" ]]; then
  {
    cat "${DOXYFILE}"
    printf '%s' "${DOT_OVERRIDE}"
  } | doxygen -
else
  doxygen "${DOXYFILE}"
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
