#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# docs_site.sh -- assemble the whole documentation site from its generators (#900).
#
# Usage:
#   bash scripts/builders/docs_site.sh             -- build + assemble into build/docs/site/
#   bash scripts/builders/docs_site.sh --out DIR   -- assemble into DIR instead
#   bash scripts/builders/docs_site.sh --only ID   -- restrict to one slot (repeatable)
#   bash scripts/builders/docs_site.sh --no-build  -- assemble whatever the generators already produced
#
# ADR-0005 publishes one site from four generators: MkDocs owns the Markdown
# hub at /, Doxygen owns the C ABI headers at /api/c/, zig autodoc owns the
# native Zig modules at /api/zig/, rustdoc owns the crates at /api/rust/.
# Each of those has its own builder; this script is the only thing that knows
# how they fit together, so the layout lives in exactly one place
# (config/docs_site_slots.json) instead of in four scripts and a CI job.
#
# It is deliberately state-aware. The generators land on different branches, so
# a slot is wired, pending or absent, and scripts/checks/check_docs_site_slots.py
# fails when a state stops matching the tree. A pending slot is skipped and
# named in the summary and in the generated root index, never silently dropped.
#
# Fails closed: a failing gate, a failing generator, a wired slot whose builder
# produced no index.html, or an assembled site with no landing page all stop
# the build with a non-zero exit. Output lives under build/ and is never
# committed.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CHECKER="${ROOT_DIR}/scripts/checks/check_docs_site_slots.py"
MANIFEST="${ROOT_DIR}/config/docs_site_slots.json"

OUT_DIR=""
NO_BUILD=0
ONLY=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --out)
      shift
      [[ $# -gt 0 ]] || {
        echo "docs_site.sh: --out needs a directory" >&2
        exit 2
      }
      OUT_DIR="$1"
      ;;
    --only)
      shift
      [[ $# -gt 0 ]] || {
        echo "docs_site.sh: --only needs a slot id" >&2
        exit 2
      }
      ONLY+=("$1")
      ;;
    --no-build) NO_BUILD=1 ;;
    -h | --help)
      sed -n '4,12p' "${BASH_SOURCE[0]}"
      exit 0
      ;;
    *)
      echo "docs_site.sh: unknown argument '$1'" >&2
      exit 2
      ;;
  esac
  shift
done

[[ -r "${MANIFEST}" ]] || {
  echo "docs_site.sh: ${MANIFEST} not found" >&2
  exit 1
}
[[ -r "${CHECKER}" ]] || {
  echo "docs_site.sh: ${CHECKER} not found" >&2
  exit 1
}

if [[ -z "${OUT_DIR}" ]]; then
  OUT_DIR="${ROOT_DIR}/$(python3 -c '
import json, sys
print(json.load(open(sys.argv[1]))["site_root"])
' "${MANIFEST}")"
fi

# The gate runs first: it is fast, needs no generator, and a stale slot state is
# worth knowing about before a Doxygen run rather than after one.
PLAN="$(python3 "${CHECKER}" --emit-plan)"

STAGE="${OUT_DIR}.staging"
rm -rf "${STAGE}"
mkdir -p "${STAGE}"

wants() {
  local id="$1"
  [[ ${#ONLY[@]} -eq 0 ]] && return 0
  local want
  for want in "${ONLY[@]}"; do
    [[ "${want}" == "${id}" ]] && return 0
  done
  return 1
}

MOUNTED=()
SKIPPED=()

# US-separated, not tab: bash treats tabs as IFS whitespace, so a slot with
# an empty field (the hub mounts at the root) would shift every later field.
while IFS=$'\x1f' read -r id state mount builder default_path title; do
  [[ -n "${id}" ]] || continue
  if ! wants "${id}"; then
    continue
  fi
  if [[ "${state}" != "wired" ]]; then
    SKIPPED+=("${id} (${state}): ${title}")
    echo "docs_site.sh: skipping ${id} -- ${state}"
    continue
  fi

  produced="${ROOT_DIR}/${default_path}"
  if [[ "${NO_BUILD}" -eq 0 ]]; then
    echo "docs_site.sh: building ${id} via ${builder}"
    bash "${ROOT_DIR}/${builder}"
  fi

  if [[ ! -f "${produced}/index.html" ]]; then
    echo "docs_site.sh: ${id} produced no index.html under ${produced}" >&2
    [[ "${NO_BUILD}" -eq 1 ]] && echo "docs_site.sh: --no-build was passed; run without it to generate ${id}" >&2
    exit 1
  fi

  target="${STAGE}"
  [[ -n "${mount}" ]] && target="${STAGE}/${mount}"
  mkdir -p "${target}"
  cp -R "${produced}/." "${target}/"
  MOUNTED+=("${id}|${mount}|${title}")
  echo "docs_site.sh: mounted ${id} at /${mount}"
done <<< "${PLAN}"

# The hub owns the landing page. Until it is wired, the site still needs an
# entry point, so write a plain one naming what is mounted and what is not.
if [[ ! -f "${STAGE}/index.html" ]]; then
  {
    cat << 'HTML_HEAD'
<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>ra8-firmware documentation</title>
  </head>
  <body>
    <h1>ra8-firmware documentation</h1>
    <p>
      Placeholder landing page, written by
      <code>scripts/builders/docs_site.sh</code> because the Markdown hub is
      not mounted in this build. The hub replaces it as soon as its slot is
      wired.
    </p>
    <h2>Mounted</h2>
    <ul>
HTML_HEAD
    for entry in "${MOUNTED[@]:-}"; do
      [[ -n "${entry}" ]] || continue
      IFS='|' read -r m_id m_mount m_title <<< "${entry}"
      printf '      <li><a href="./%s/index.html">%s</a> <code>/%s/</code></li>\n' \
        "${m_mount}" "${m_title}" "${m_mount}"
    done
    cat << 'HTML_MID'
    </ul>
    <h2>Not in this build</h2>
    <ul>
HTML_MID
    for entry in "${SKIPPED[@]:-}"; do
      [[ -n "${entry}" ]] || continue
      printf '      <li>%s</li>\n' "${entry}"
    done
    cat << 'HTML_TAIL'
    </ul>
  </body>
</html>
HTML_TAIL
  } > "${STAGE}/index.html"
fi

if [[ ! -f "${STAGE}/index.html" ]]; then
  echo "docs_site.sh: assembled site has no landing page" >&2
  exit 1
fi

rm -rf "${OUT_DIR}"
mkdir -p "$(dirname "${OUT_DIR}")"
mv "${STAGE}" "${OUT_DIR}"

echo
echo "docs_site.sh: site assembled at ${OUT_DIR}"
for entry in "${MOUNTED[@]:-}"; do
  [[ -n "${entry}" ]] || continue
  IFS='|' read -r m_id m_mount m_title <<< "${entry}"
  echo "  mounted  /${m_mount}  ${m_title}"
done
for entry in "${SKIPPED[@]:-}"; do
  [[ -n "${entry}" ]] || continue
  echo "  skipped  ${entry}"
done
