#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# docs_capi.sh -- build the C ABI Doxygen reference into the /api/c/ slot.
#
# Usage:
#   bash scripts/builders/docs_capi.sh          -- build into build/docs/api/c/
#   bash scripts/builders/docs_capi.sh --gate   -- undocumented-warning build
#                                                  into build/docs-capi-gate/api/c/
#   bash scripts/builders/docs_capi.sh --out DIR  -- build into DIR/api/c/
#
# ADR-0005 gives the C ABI reference its own slot in the hub. This builder is
# additive: the legacy whole-repo site (scripts/builders/docs.sh, build/docs/html)
# is untouched, and so is every gate that reads it. Writing into build/docs/api/c
# puts this next to it, in the layout the assembler slice will publish.
#
# Everything it covers comes from config/c_abi_doc_headers.json via
# scripts/checks/check_c_abi_doc_headers.py, which runs first and fails the build
# on drift, so a library that grows public headers cannot quietly miss the
# reference. Doxyfile.capi carries no INPUT of its own.
#
# Fails closed: a missing gate, a missing doxygen, a doxygen that exits non-zero
# or an output tree with no index.html all stop the build.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
DOXYFILE="${ROOT_DIR}/Doxyfile.capi"
CHECKER="${ROOT_DIR}/scripts/checks/check_c_abi_doc_headers.py"

GATE_MODE=0
OUT_DIR=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --gate) GATE_MODE=1 ;;
    --out)
      shift
      [[ $# -gt 0 ]] || {
        echo "docs_capi.sh: --out needs a directory" >&2
        exit 2
      }
      OUT_DIR="$1"
      ;;
    -h | --help)
      sed -n '4,16p' "${BASH_SOURCE[0]}"
      exit 0
      ;;
    *)
      echo "docs_capi.sh: unknown argument '$1'" >&2
      exit 2
      ;;
  esac
  shift
done

if [[ -z "${OUT_DIR}" ]]; then
  if [[ "${GATE_MODE}" -eq 1 ]]; then
    OUT_DIR="${ROOT_DIR}/build/docs-capi-gate"
  else
    OUT_DIR="${ROOT_DIR}/build/docs"
  fi
fi
HTML_DIR="${OUT_DIR}/api/c"
WARN_LOG="${OUT_DIR}/doxygen-capi-warnings.log"

for required in "${DOXYFILE}" "${CHECKER}"; do
  if [[ ! -f "${required}" ]]; then
    echo "docs_capi.sh: ${required} not found." >&2
    exit 1
  fi
done

cd "${ROOT_DIR}"

echo "docs_capi.sh: checking the C ABI header manifest."
python3 "${CHECKER}" --check

mapfile -t INPUT_ROOTS < <(python3 "${CHECKER}" --emit-inputs)
if [[ "${#INPUT_ROOTS[@]}" -eq 0 ]]; then
  echo "docs_capi.sh: ERROR -- the manifest documents no include roots." >&2
  exit 1
fi
mapfile -t EXCLUDED_HEADERS < <(python3 "${CHECKER}" --emit-excludes)

DOXYGEN_BIN="$(/bin/bash -p "${SCRIPT_DIR}/provision_doxygen.sh")"
echo "docs_capi.sh: using doxygen $("${DOXYGEN_BIN}" --version) (${DOXYGEN_BIN})"

RA8_PROJECT_VERSION="$(tr -d '[:space:]' <"${ROOT_DIR}/VERSION")"
export RA8_PROJECT_VERSION

OVERRIDES=""
OVERRIDES+="OUTPUT_DIRECTORY=${OUT_DIR}"$'\n'
OVERRIDES+="WARN_LOGFILE=${WARN_LOG}"$'\n'
printf -v INPUT_LINE 'INPUT=%s' "${INPUT_ROOTS[*]}"
OVERRIDES+="${INPUT_LINE}"$'\n'
if [[ "${#EXCLUDED_HEADERS[@]}" -gt 0 ]]; then
  printf -v EXCLUDE_LINE 'EXCLUDE=%s' "${EXCLUDED_HEADERS[*]}"
  OVERRIDES+="${EXCLUDE_LINE}"$'\n'
fi

if command -v dot >/dev/null 2>&1; then
  echo "docs_capi.sh: graphviz detected -- authored diagram blocks will render."
else
  echo "docs_capi.sh: graphviz NOT detected -- generating text-only output."
  OVERRIDES+=$'HAVE_DOT=NO\n'
fi

if [[ "${GATE_MODE}" -eq 1 ]]; then
  # The gate build reports on the documentation of the public surface itself:
  # an undocumented exported symbol is a hole in the C ABI reference, which is
  # exactly what this slot exists to make visible.
  OVERRIDES+=$'WARN_IF_UNDOCUMENTED=YES\n'
fi

# Doxygen overwrites what it regenerates but never removes what it stops
# producing, so a header that leaves the manifest would otherwise keep its page.
# Only this slot is cleaned: the legacy site next door in build/docs/html is not
# ours to delete.
rm -rf "${HTML_DIR}"
mkdir -p "${HTML_DIR}"

{
  cat "${DOXYFILE}"
  printf '%s' "${OVERRIDES}"
} | "${DOXYGEN_BIN}" -

INDEX_HTML="${HTML_DIR}/index.html"
if [[ ! -f "${INDEX_HTML}" ]]; then
  echo "docs_capi.sh: ERROR -- expected ${INDEX_HTML} was not produced." >&2
  exit 1
fi

echo
echo "docs_capi.sh: C ABI reference written to ${HTML_DIR}"
echo "docs_capi.sh: input roots: ${#INPUT_ROOTS[@]}"
if [[ -f "${WARN_LOG}" ]]; then
  WARN_COUNT=$(wc -l <"${WARN_LOG}" | tr -d ' ')
  echo "docs_capi.sh: doxygen warning lines: ${WARN_COUNT} (see ${WARN_LOG})"
fi
