#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# docs_hub.sh -- build the ra8-firmware Markdown documentation hub.
#
# Usage:
#   bash scripts/builders/docs_hub.sh          -- build into build/docs/hub/
#   bash scripts/builders/docs_hub.sh --serve  -- serve with live reload
#   bash scripts/builders/docs_hub.sh --clean  -- drop the generator venv too
#
# The hub renders the prose under docs/ (ADR-0005). It does NOT render any
# API reference: doxygen owns the C ABI, zig autodoc owns the Zig library
# internals, rustdoc owns the Rust crates, and the site assembler mounts
# those under /api/.
#
# The generators are pinned in docs/hub-requirements.txt and installed into a
# venv under build/tools/, which is git-ignored. First run needs the network;
# later runs reuse the venv offline. Generated HTML is never committed.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CONFIG="${ROOT_DIR}/mkdocs.yml"
REQUIREMENTS="${ROOT_DIR}/docs/hub-requirements.txt"
VENV_DIR="${ROOT_DIR}/build/tools/docs-hub-venv"
OUTPUT_DIR="${ROOT_DIR}/build/docs/hub"

SERVE=0
for arg in "$@"; do
  case "${arg}" in
    --serve) SERVE=1 ;;
    --clean)
      rm -rf "${VENV_DIR}" "${OUTPUT_DIR}"
      echo "docs_hub.sh: removed ${VENV_DIR} and ${OUTPUT_DIR}"
      exit 0
      ;;
    -h | --help)
      sed -n '3,20p' "${BASH_SOURCE[0]}"
      exit 0
      ;;
    *)
      echo "docs_hub.sh: unknown argument '${arg}'" >&2
      exit 2
      ;;
  esac
done

if [[ ! -f "${CONFIG}" ]]; then
  echo "docs_hub.sh: ${CONFIG} not found." >&2
  exit 1
fi

# Nav coverage first. It needs no generator and no network, so a missing page
# is reported in a second rather than after an install.
python3 "${SCRIPT_DIR}/../checks/check_docs_hub_nav.py"

STAMP="${VENV_DIR}/.requirements.sha256"
WANT_STAMP="$(python3 -c '
import hashlib, sys
print(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest())
' "${REQUIREMENTS}")"

if [[ ! -x "${VENV_DIR}/bin/mkdocs" ]] || [[ "$(cat "${STAMP}" 2>/dev/null || true)" != "${WANT_STAMP}" ]]; then
  echo "docs_hub.sh: provisioning pinned generators into ${VENV_DIR}"
  rm -rf "${VENV_DIR}"
  python3 -m venv "${VENV_DIR}"
  "${VENV_DIR}/bin/pip" install --quiet --upgrade pip
  "${VENV_DIR}/bin/pip" install --quiet --requirement "${REQUIREMENTS}"
  printf '%s' "${WANT_STAMP}" >"${STAMP}"
else
  echo "docs_hub.sh: reusing ${VENV_DIR} (pins unchanged)"
fi

MKDOCS="${VENV_DIR}/bin/mkdocs"
echo "docs_hub.sh: using $("${MKDOCS}" --version)"

cd "${ROOT_DIR}"

if [[ "${SERVE}" -eq 1 ]]; then
  exec "${MKDOCS}" serve --config-file "${CONFIG}"
fi

# --strict is the gate: a broken internal link or an unknown config key fails
# the build instead of publishing a 404 for a reader to find.
"${MKDOCS}" build --strict --config-file "${CONFIG}"

INDEX_HTML="${OUTPUT_DIR}/index.html"
if [[ ! -f "${INDEX_HTML}" ]]; then
  echo "docs_hub.sh: ERROR -- expected ${INDEX_HTML} was not produced." >&2
  exit 1
fi

echo
echo "docs_hub.sh: hub written to ${OUTPUT_DIR}"
