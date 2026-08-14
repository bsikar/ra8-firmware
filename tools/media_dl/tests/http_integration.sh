#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# Real-libcurl series -> chapter -> image integration against a local server.

set -euo pipefail

MEDIA_DL="${1:?media_dl executable required}"
PYTHON="${2:?Python interpreter required}"
CMAKE="${3:?CMake executable required}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/mdl-http.XXXXXX")"
WEB="${WORK}/web"
PORT_FILE="${WORK}/port"
SERVER_PID=""

cleanup() {
  if [[ -n "${SERVER_PID}" ]]; then
    kill "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
  rm -rf "${WORK}"
}
trap cleanup EXIT INT TERM
mkdir -p "${WEB}/series" "${WORK}/out"

"${PYTHON}" - "${WEB}" "${PORT_FILE}" <<'PY' &
import http.server
import os
import socketserver
import sys

root, port_file = sys.argv[1:]
os.chdir(root)

class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        if self.path.endswith("/image-1.jpg"):
            self.send_header("ETag", '"stable-fixture-etag"')
        super().end_headers()

    def do_GET(self):
        if (self.path.endswith("/image-1.jpg") and
                self.headers.get("If-None-Match") == '"stable-fixture-etag"'):
            self.send_response(http.HTTPStatus.NOT_MODIFIED)
            self.end_headers()
            return
        super().do_GET()

with socketserver.TCPServer(("127.0.0.1", 0), Handler) as server:
    with open(port_file, "w", encoding="ascii") as out:
        out.write(str(server.server_address[1]))
    server.serve_forever()
PY
SERVER_PID=$!

for _ in 1 2 3 4 5 6 7 8 9 10; do
  [[ -s "${PORT_FILE}" ]] && break
  sleep 0.1
done
[[ -s "${PORT_FILE}" ]]
PORT="$(<"${PORT_FILE}")"
BASE="http://127.0.0.1:${PORT}"

printf '%s\n' \
  'name = LocalFixture' \
  'host = 127.0.0.1' \
  'kind = manga' \
  'chapter_url_contains = /series/chapter-' \
  'chapter_order = asc' \
  'page_img_attr = data-src' \
  'page_img_url_contains = /image-' \
  'img_delay_min = 0' \
  'img_delay_max = 0' \
  'chapter_delay_min = 0' \
  'chapter_delay_max = 0' >"${WORK}/site.conf"
printf '<a href="%s/series/chapter-1.html">Chapter 1</a>\n' "${BASE}" >"${WEB}/series/index.html"
printf '<img data-src="%s/series/image-1.jpg">\n' "${BASE}" >"${WEB}/series/chapter-1.html"
printf '\377\330\377fixture-one' >"${WEB}/series/image-1.jpg"

"${MEDIA_DL}" \
  --config "${WORK}/site.conf" \
  --series "${BASE}/series/" \
  --chapters 1 \
  --out "${WORK}/out" \
  --format cbz \
  --allow-private \
  --contact https://github.com/bsikar/ra8-firmware

PAGE="$(find "${WORK}/out" -type f -name 'page_0001.jpg' -print -quit)"
ARCHIVE="$(find "${WORK}/out" -type f -name '*.cbz' -print -quit)"
[[ -n "${PAGE}" && -s "${PAGE}" ]]
[[ -n "${ARCHIVE}" && -s "${ARCHIVE}" ]]
BEFORE="$("${CMAKE}" -E sha256sum "${PAGE}")"
BEFORE="${BEFORE%% *}"

# Same URL and deliberately unchanged ETag, but different bytes: --refetch must
# make an unconditional request and replace the verified local page.
printf '\377\330\377fixture-two' >"${WEB}/series/image-1.jpg"
"${MEDIA_DL}" \
  --config "${WORK}/site.conf" \
  --series "${BASE}/series/" \
  --chapters 1 \
  --out "${WORK}/out" \
  --format cbz \
  --allow-private \
  --refetch \
  --contact https://github.com/bsikar/ra8-firmware
AFTER="$("${CMAKE}" -E sha256sum "${PAGE}")"
AFTER="${AFTER%% *}"
[[ "${BEFORE}" != "${AFTER}" ]]

printf 'media_dl local HTTP integration: PASS\n'
