#!/usr/bin/env bash
#
# publish_docs.sh -- publish the generated Doxygen HTML to the gh-pages branch.
#
# This does NOT build: run `make docs` first, or use `make docs-push`, which
# chains the build and this publish step. It takes the already-built
# build/docs/html/ tree and force-pushes it as a single fresh commit onto the
# orphan `gh-pages` branch, so that branch never accumulates a history of
# generated HTML (each publish fully replaces the previous one).
#
# Remote / auth:
#   - CI:    GitHub Actions provides GITHUB_TOKEN + GITHUB_REPOSITORY; the push
#            targets an x-access-token HTTPS URL derived from them.
#   - Local: no env needed; the push targets the `origin` remote using your
#            existing git credentials (SSH or HTTPS).
#
# One-time repo setup (owner): Settings -> Pages -> Build and deployment ->
# Source = "Deploy from a branch", Branch = gh-pages / (root).
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
HTML_DIR="${ROOT_DIR}/build/docs/html"
BRANCH="gh-pages"

if [[ ! -f "${HTML_DIR}/index.html" ]]; then
  echo "publish_docs.sh: ${HTML_DIR}/index.html not found -- run 'make docs' first." >&2
  exit 1
fi

# Human-readable source revision, recorded in the gh-pages commit message.
SRC_REV="$(git -C "${ROOT_DIR}" rev-parse --short HEAD)"

if [[ -n "${GITHUB_TOKEN:-}" && -n "${GITHUB_REPOSITORY:-}" ]]; then
  REMOTE="https://x-access-token:${GITHUB_TOKEN}@github.com/${GITHUB_REPOSITORY}.git"
else
  REMOTE="$(git -C "${ROOT_DIR}" remote get-url origin)"
fi

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

cp -R "${HTML_DIR}/." "${WORK_DIR}/"
# Disable Jekyll so GitHub Pages serves doxygen's _-prefixed asset files as-is.
touch "${WORK_DIR}/.nojekyll"

cd "${WORK_DIR}"
git init -q
git checkout -q -b "${BRANCH}"
git add -A
git \
  -c user.name="ra8-docs" \
  -c user.email="ra8-docs@users.noreply.github.com" \
  commit -q -m "docs: publish from ${SRC_REV}"
git push -q -f "${REMOTE}" "${BRANCH}"

echo "publish_docs.sh: published build/docs/html -> ${BRANCH} (source ${SRC_REV})."
