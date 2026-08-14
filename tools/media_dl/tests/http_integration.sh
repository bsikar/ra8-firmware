#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# Real-libcurl series -> chapter -> image integration against a local server.

set -euo pipefail

MEDIA_DL="${1:?media_dl executable required}"
PYTHON="${2:?Python interpreter required}"
CMAKE="${3:?CMake executable required}"
OPENSSL="${4:?OpenSSL executable required}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/mdl-http.XXXXXX")"
WEB="${WORK}/web"
PORT_FILE="${WORK}/port"
SERVER_PID=""
TLS_SERVER_PID=""
TLS_PORT_FILE="${WORK}/tls-port"
TLS_CERT="${WORK}/tls-cert.pem"
TLS_KEY="${WORK}/tls-key.pem"

cleanup() {
  if [[ -n "${SERVER_PID}" ]]; then
    kill "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
  if [[ -n "${TLS_SERVER_PID}" ]]; then
    kill "${TLS_SERVER_PID}" 2>/dev/null || true
    wait "${TLS_SERVER_PID}" 2>/dev/null || true
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

# A real TLS server qualifies the HTTPS-only direct-artifact path. The one-run
# CA/key are generated in the private scratch directory; CURL_CA_BUNDLE trusts
# only this fixture certificate and production verification remains enabled.
"${OPENSSL}" req -x509 -newkey rsa:2048 -nodes -days 1 \
  -subj '/CN=127.0.0.1' -addext 'subjectAltName=IP:127.0.0.1' \
  -keyout "${TLS_KEY}" -out "${TLS_CERT}" >/dev/null 2>&1
"${PYTHON}" - "${WEB}" "${TLS_PORT_FILE}" "${TLS_CERT}" "${TLS_KEY}" <<'PY' &
import http.server
import os
import socketserver
import ssl
import sys

root, port_file, cert_file, key_file = sys.argv[1:]
os.chdir(root)

with socketserver.TCPServer(("127.0.0.1", 0), http.server.SimpleHTTPRequestHandler) as server:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert_file, key_file)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    with open(port_file, "w", encoding="ascii") as out:
        out.write(str(server.server_address[1]))
    server.serve_forever()
PY
TLS_SERVER_PID=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do
  [[ -s "${TLS_PORT_FILE}" ]] && break
  sleep 0.1
done
[[ -s "${TLS_PORT_FILE}" ]]
TLS_BASE="https://127.0.0.1:$(<"${TLS_PORT_FILE}")"

printf '%s\n' \
  'name = LocalFixture' \
  'host = 127.0.0.1' \
  'kind = manga' \
  'chapter_url_contains = /series/chapter-' \
  'chapter_order = asc' \
  'series_title_selector = meta:og:title' \
  'series_summary_selector = meta:og:description' \
  'series_author_selector = label:Author(s):' \
  'series_artist_selector = label:Artist(s):' \
  'series_cover_selector = meta:og:image' \
  'chapter_title_selector = meta:og:title' \
  'language = en' \
  'reading_direction = ltr' \
  "search_url = ${BASE}/search.html?q={q}" \
  'search_result_contains = /series/' \
  "browse_url = ${BASE}/browse.html" \
  'page_img_attr = data-src' \
  'page_img_url_contains = /image-' \
  'img_delay_min = 0' \
  'img_delay_max = 0' \
  'chapter_delay_min = 0' \
  'chapter_delay_max = 0' >"${WORK}/site.conf"
cat >"${WEB}/series/index.html" <<HTML
<meta property="og:title" content="Local Fixture Series">
<meta property="og:description" content="A local &amp; rigorously verified story.">
<meta property="og:image" content="/series/cover.bin">
<div><b>Author(s):</b><a href="/authors/writer">Fixture Writer</a></div>
<div><b>Artist(s):</b><a href="/artists/artist">Fixture Artist</a></div>
<a href="${BASE}/series/chapter-1.html">Chapter 1</a>
<a href="${BASE}/series/chapter-108-5.html">Chapter 108.5</a>
<a href="${BASE}/series/chapter-special.html">Special Chapter</a>
HTML
cat >"${WEB}/series/chapter-1.html" <<HTML
<meta property="og:title" content="Chapter One: Arrival">
<img data-src="${BASE}/series/image-1.jpg">
HTML
cat >"${WEB}/series/chapter-108-5.html" <<HTML
<meta property="og:title" content="Chapter 108.5: Interlude">
<img data-src="${BASE}/series/image-2.jpg">
HTML
cat >"${WEB}/series/chapter-special.html" <<HTML
<meta property="og:title" content="A Numberless Special">
<img data-src="${BASE}/series/image-3.jpg">
HTML
printf '<img src="%s/series/page-debug.jpg">\n' "${BASE}" >"${WEB}/page.html"
printf '<img src="%s/series/page-invalid.jpg">\n' "${BASE}" >"${WEB}/page-invalid.html"
printf '<p>no images here</p>\n' >"${WEB}/page-empty.html"
printf '<a href="%s/series/">Local Series</a>\n<a href="%s/series/chapter-1.html">Chapter 1</a>\n' \
  "${BASE}" "${BASE}" >"${WEB}/browse.html"
cp "${WEB}/browse.html" "${WEB}/search.html"
printf '<a href="%s/not-a-series/">Other</a>\n' "${BASE}" >"${WEB}/empty.html"
printf '<p>results markup unavailable</p>\n' >"${WEB}/broken.html"
sed -e 's#/search.html?q={q}#/empty.html?q={q}#' -e 's#/browse.html#/empty.html#' \
  "${WORK}/site.conf" >"${WORK}/empty.conf"
sed -e 's#/search.html?q={q}#/broken.html?q={q}#' -e 's#/browse.html#/broken.html#' \
  "${WORK}/site.conf" >"${WORK}/broken.conf"
sed 's#chapter_url_contains = /series/chapter-#chapter_url_contains = /never-a-chapter/#' \
  "${WORK}/site.conf" >"${WORK}/no-chapters.conf"
printf '\377\330\377fixture-one' >"${WEB}/series/image-1.jpg"
printf '\377\330\377fixture-decimal' >"${WEB}/series/image-2.jpg"
printf '\377\330\377fixture-special' >"${WEB}/series/image-3.jpg"
printf '\211PNG\r\n\032\nfixture-cover' >"${WEB}/series/cover.bin"
printf '\211PNG\r\n\032\nfixture-page-mode' >"${WEB}/series/page-debug.jpg"
printf 'not-an-image' >"${WEB}/series/page-invalid.jpg"

# Representative usage failures are exit 2 and never fall through to another mode.
expect_usage() {
  set +e
  "${MEDIA_DL}" "$@" >"${WORK}/usage.log" 2>&1
  local rc=$?
  set -e
  [[ ${rc} -eq 2 ]]
}
expect_usage --list --series "${BASE}/series/" --config "${WORK}/site.conf"
expect_usage --search alpha --config "${WORK}/site.conf" --format cbz
expect_usage --pack "${WORK}"
expect_usage --verify "${WORK}" --out "${WORK}/other"
expect_usage --list --list

# Help/version short-circuit dispatch, report stable public identity, and do
# not create the default downloads tree. Empty offline library operations are
# honest successes, while verify distinguishes an empty/missing target.
(cd "${WORK}" && "${MEDIA_DL}" --help >"${WORK}/help.out" 2>"${WORK}/help.err")
grep -q '^usage:$' "${WORK}/help.err"
grep -q -- '--ca-file <FILE>' "${WORK}/help.err"
(cd "${WORK}" && "${MEDIA_DL}" --version >"${WORK}/version.out" 2>"${WORK}/version.err")
grep -qx 'media_dl 0.1.0' "${WORK}/version.out"
[[ ! -e "${WORK}/downloads" ]]
"${MEDIA_DL}" --list --out "${WORK}/empty-library" >"${WORK}/list-empty.log"
grep -q 'no tracked series under' "${WORK}/list-empty.log"
[[ ! -e "${WORK}/empty-library" ]]
"${MEDIA_DL}" --update-all --config "${WORK}/site.conf" --out "${WORK}/empty-library" \
  >"${WORK}/update-all-empty.log"
grep -q 'no tracked series to update under' "${WORK}/update-all-empty.log"
[[ ! -e "${WORK}/empty-library" ]]
mkdir -p "${WORK}/verify-empty"
set +e
"${MEDIA_DL}" --verify "${WORK}/verify-empty" >"${WORK}/verify-empty.log" 2>&1
VERIFY_EMPTY_RC=$?
"${MEDIA_DL}" --verify "${WORK}/verify-missing" >"${WORK}/verify-missing.log" 2>&1
VERIFY_MISSING_RC=$?
set -e
[[ ${VERIFY_EMPTY_RC} -eq 1 ]]
[[ ${VERIFY_MISSING_RC} -eq 1 ]]
grep -q 'no tracked series or recognized artifacts' "${WORK}/verify-empty.log"
grep -q 'is not a readable directory' "${WORK}/verify-missing.log"

# Browse/search list only canonical series; recent chapter cards are not hits.
"${MEDIA_DL}" --config "${WORK}/site.conf" --browse --allow-private \
  --contact https://github.com/bsikar/ra8-firmware >"${WORK}/browse.log"
grep -q 'Local Series' "${WORK}/browse.log"
! grep -q 'Chapter 1' "${WORK}/browse.log"
"${MEDIA_DL}" --config "${WORK}/site.conf" --search 'local series' --allow-private \
  --contact https://github.com/bsikar/ra8-firmware >"${WORK}/search.log"
grep -q 'Local Series' "${WORK}/search.log"
! grep -q 'Chapter 1' "${WORK}/search.log"

# A genuine empty result is a successful listing without --pick, but a failed
# requested selection. A link-free response is diagnosed as changed markup.
"${MEDIA_DL}" --config "${WORK}/empty.conf" --search absent --allow-private \
  --contact https://github.com/bsikar/ra8-firmware >"${WORK}/search-empty.log" 2>&1
grep -q "no results for 'absent'" "${WORK}/search-empty.log"
"${MEDIA_DL}" --config "${WORK}/empty.conf" --browse --allow-private \
  --contact https://github.com/bsikar/ra8-firmware >"${WORK}/browse-empty.log" 2>&1
grep -q 'nothing to browse' "${WORK}/browse-empty.log"
set +e
"${MEDIA_DL}" --config "${WORK}/empty.conf" --search absent --pick 1 \
  --out "${WORK}/empty-pick" --format loose --allow-private \
  --contact https://github.com/bsikar/ra8-firmware >"${WORK}/search-empty-pick.log" 2>&1
SEARCH_EMPTY_PICK_RC=$?
"${MEDIA_DL}" --config "${WORK}/broken.conf" --browse --allow-private \
  --contact https://github.com/bsikar/ra8-firmware >"${WORK}/browse-broken.log" 2>&1
BROWSE_BROKEN_RC=$?
set -e
[[ ${SEARCH_EMPTY_PICK_RC} -eq 1 ]]
[[ ${BROWSE_BROKEN_RC} -eq 1 ]]
grep -q 'returned no results' "${WORK}/search-empty-pick.log"
grep -q 'markup may have changed' "${WORK}/browse-broken.log"
[[ ! -e "${WORK}/empty-pick" ]]

# Pick hands discovery result 1 to the normal series downloader.
"${MEDIA_DL}" --config "${WORK}/site.conf" --search local --pick 1 \
  --out "${WORK}/picked" --format loose --allow-private \
  --contact https://github.com/bsikar/ra8-firmware
PICKED_PAGE="$(find "${WORK}/picked" -type f -name 'page_0001.jpg' -print -quit)"
[[ -n "${PICKED_PAGE}" && -s "${PICKED_PAGE}" ]]
"${MEDIA_DL}" --config "${WORK}/site.conf" --browse --pick 1 --chapters 1 \
  --out "${WORK}/browse-picked" --format loose --allow-private \
  --contact https://github.com/bsikar/ra8-firmware
BROWSE_PICKED_PAGE="$(find "${WORK}/browse-picked" -type f -name 'page_0001.jpg' -print -quit)"
[[ -n "${BROWSE_PICKED_PAGE}" && -s "${BROWSE_PICKED_PAGE}" ]]

# A descriptor whose chapter selector matches nothing fails before creating a
# library tree, with a diagnostic distinct from transport failure.
set +e
"${MEDIA_DL}" --config "${WORK}/no-chapters.conf" --series "${BASE}/series/" \
  --out "${WORK}/no-chapters" --format loose --allow-private \
  --contact https://github.com/bsikar/ra8-firmware >"${WORK}/no-chapters.log" 2>&1
NO_CHAPTERS_RC=$?
set -e
[[ ${NO_CHAPTERS_RC} -eq 1 ]]
grep -q 'no chapters (check chapter_url_contains)' "${WORK}/no-chapters.log"
[[ ! -e "${WORK}/no-chapters" ]]

# A pre-existing symlink at the untrusted series slug may not redirect state or
# downloaded bytes outside the canonical library root.
mkdir -p "${WORK}/symlink-library" "${WORK}/outside-series"
printf 'outside sentinel\n' >"${WORK}/outside-series/keep"
ln -s "${WORK}/outside-series" "${WORK}/symlink-library/series"
set +e
"${MEDIA_DL}" --config "${WORK}/site.conf" --series "${BASE}/series/" \
  --chapters 1 --out "${WORK}/symlink-library" --format loose --allow-private \
  --contact https://github.com/bsikar/ra8-firmware >"${WORK}/series-symlink.log" 2>&1
SERIES_SYMLINK_RC=$?
set -e
[[ ${SERIES_SYMLINK_RC} -eq 1 ]]
grep -q 'refusing unsafe series path' "${WORK}/series-symlink.log"
grep -qx 'outside sentinel' "${WORK}/outside-series/keep"
[[ ! -e "${WORK}/outside-series/.mdl_state" ]]

# Direct series download establishes a tracked library and archive.
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

# The application (not merely extraction helpers) must persist the descriptor
# metadata, fetch/type the cover by bytes, feed both into the real CBZ exporter,
# and retain fractional and numberless chapter identity in document order.
STATE="${WORK}/out/series/.mdl_state"
"${PYTHON}" - "${STATE}" "${ARCHIVE}" "${WEB}/series/cover.bin" "${BASE}/series/" <<'PY'
import pathlib
import sys
import zipfile

state_path, archive_path, cover_path = map(pathlib.Path, sys.argv[1:4])
source_url = sys.argv[4]
lines = state_path.read_text(encoding="utf-8").splitlines()
expected = {
    "T\tLocal Fixture Series",
    "D\tA local & rigorously verified story.",
    "W\tFixture Writer",
    "A\tFixture Artist",
    "K\tcover.png",
    "L\ten",
    "R\t0",
}
missing = expected.difference(lines)
assert not missing, f"missing state metadata: {sorted(missing)}"
with zipfile.ZipFile(archive_path) as archive:
    names = set(archive.namelist())
    assert "cover/cover.png" in names, names
    assert archive.read("cover/cover.png") == cover_path.read_bytes()
    comic = archive.read("ComicInfo.xml").decode("utf-8")
    assert "<Series>Local Fixture Series</Series>" in comic
    assert "<Summary>A local &amp; rigorously verified story.</Summary>" in comic
    assert "<Writer>Fixture Writer</Writer>" in comic
    assert "<Artist>Fixture Artist</Artist>" in comic
    assert f"<Web>{source_url}</Web>" in comic
PY

# Repeating the same completed series invocation is resumable/idempotent: the
# verified page stays byte-identical, no duplicate page appears, and no staging
# debris escapes into the library.
REPEAT_PAGE_HASH="$("${CMAKE}" -E sha256sum "${PAGE}")"
"${MEDIA_DL}" --config "${WORK}/site.conf" --series "${BASE}/series/" \
  --chapters 1 --out "${WORK}/out" --format cbz --allow-private \
  --contact https://github.com/bsikar/ra8-firmware
AFTER_REPEAT_PAGE_HASH="$("${CMAKE}" -E sha256sum "${PAGE}")"
[[ "${REPEAT_PAGE_HASH%% *}" == "${AFTER_REPEAT_PAGE_HASH%% *}" ]]
[[ "$(find "$(dirname "${PAGE}")" -maxdepth 1 -type f -name 'page_*' | wc -l)" -eq 1 ]]
[[ -z "$(find "${WORK}/out" -name '.mdl-tmp-*' -print -quit)" ]]

# A descriptor that promises a cover may not degrade into a coverless success:
# wrong-magic bytes fail before chapter transfer and leave no state/page output.
cp "${WEB}/series/cover.bin" "${WORK}/cover.save"
printf 'not-an-image' >"${WEB}/series/cover.bin"
set +e
"${MEDIA_DL}" --config "${WORK}/site.conf" --series "${BASE}/series/" \
  --chapters 1 --out "${WORK}/bad-cover" --format loose --allow-private \
  --contact https://github.com/bsikar/ra8-firmware >"${WORK}/bad-cover.log" 2>&1
BAD_COVER_RC=$?
set -e
[[ ${BAD_COVER_RC} -eq 1 ]]
[[ ! -e "${WORK}/bad-cover/series/.mdl_state" ]]
[[ -z "$(find "${WORK}/bad-cover" -type f -name 'page_*' -print -quit)" ]]
mv "${WORK}/cover.save" "${WEB}/series/cover.bin"

"${MEDIA_DL}" \
  --config "${WORK}/site.conf" \
  --series "${BASE}/series/" \
  --from 108.5 \
  --chapters 2 \
  --out "${WORK}/ordered" \
  --format loose \
  --allow-private \
  --contact https://github.com/bsikar/ra8-firmware
"${PYTHON}" - "${WORK}/ordered/series/.mdl_state" <<'PY'
import pathlib
import sys

records = [line.split("\t") for line in pathlib.Path(sys.argv[1]).read_text(
    encoding="utf-8").splitlines() if line.startswith("C\t")]
assert [record[1] for record in records] == ["chapter-108-5.html", "chapter-special.html"], records
assert records[0][2:4] == ["1", "108.5"], records[0]
assert records[1][2] == "0", records[1]
assert records[0][-1] == "Chapter 108.5: Interlude", records[0]
assert records[1][-1] == "A Numberless Special", records[1]
PY

# Direct mode must perform actual certificate/hostname verification, reject an
# untrusted certificate, then download, structurally validate, and atomically
# publish the same CBZ once the fixture CA is supplied.
cp "${ARCHIVE}" "${WEB}/direct.cbz"
set +e
NO_PROXY=127.0.0.1 "${MEDIA_DL}" "${TLS_BASE}/direct.cbz" \
  --out "${WORK}/direct-untrusted" --allow-private \
  --contact https://github.com/bsikar/ra8-firmware >"${WORK}/tls-untrusted.log" 2>&1
TLS_UNTRUSTED_RC=$?
set -e
[[ ${TLS_UNTRUSTED_RC} -eq 1 ]]
mkdir -p "${WORK}/direct"
CURL_CA_BUNDLE="" SSL_CERT_FILE="" NO_PROXY=127.0.0.1 \
  "${MEDIA_DL}" "${TLS_BASE}/direct.cbz" --out "${WORK}/direct" --ca-file "${TLS_CERT}" \
  --allow-private --contact https://github.com/bsikar/ra8-firmware
[[ -s "${WORK}/direct/direct.cbz" ]]
DIRECT_HASH="$("${CMAKE}" -E sha256sum "${WORK}/direct/direct.cbz")"
SOURCE_HASH="$("${CMAKE}" -E sha256sum "${WEB}/direct.cbz")"
[[ "${DIRECT_HASH%% *}" == "${SOURCE_HASH%% *}" ]]
CURL_CA_BUNDLE="" SSL_CERT_FILE="" NO_PROXY=127.0.0.1 \
  "${MEDIA_DL}" "${TLS_BASE}/direct.cbz" --out "${WORK}/direct" --ca-file "${TLS_CERT}" \
  --allow-private --contact https://github.com/bsikar/ra8-firmware
DIRECT_REPEAT_HASH="$("${CMAKE}" -E sha256sum "${WORK}/direct/direct.cbz")"
[[ "${DIRECT_REPEAT_HASH%% *}" == "${SOURCE_HASH%% *}" ]]
[[ -z "$(find "${WORK}/direct" -name '.mdl-tmp-*' -print -quit)" ]]

# The dispatch and verifier agree on marker-bearing multi-dot artifact names.
cp "${WEB}/direct.cbz" "${WEB}/direct.INCOMPLETE.cbz"
CURL_CA_BUNDLE="" SSL_CERT_FILE="" NO_PROXY=127.0.0.1 \
  "${MEDIA_DL}" "${TLS_BASE}/direct.INCOMPLETE.cbz" --out "${WORK}/direct" \
  --ca-file "${TLS_CERT}" --allow-private \
  --contact https://github.com/bsikar/ra8-firmware
[[ -s "${WORK}/direct/direct.INCOMPLETE.cbz" ]]

# The same verified HTTPS path accepts a standard fixed-layout EPUB and proves
# byte identity after structural validation; it is not CBZ-specific.
PAGE_DIR="$(dirname "${PAGE}")"
"${MEDIA_DL}" --pack "${PAGE_DIR}" --format epub
EPUB="${PAGE_DIR}.epub"
cp "${EPUB}" "${WEB}/direct.epub"
CURL_CA_BUNDLE="" SSL_CERT_FILE="" NO_PROXY=127.0.0.1 \
  "${MEDIA_DL}" "${TLS_BASE}/direct.epub" --out "${WORK}/direct" --ca-file "${TLS_CERT}" \
  --allow-private --contact https://github.com/bsikar/ra8-firmware
[[ -s "${WORK}/direct/direct.epub" ]]
DIRECT_EPUB_HASH="$("${CMAKE}" -E sha256sum "${WORK}/direct/direct.epub")"
SOURCE_EPUB_HASH="$("${CMAKE}" -E sha256sum "${WEB}/direct.epub")"
[[ "${DIRECT_EPUB_HASH%% *}" == "${SOURCE_EPUB_HASH%% *}" ]]

# A corrupt HTTPS body cannot create or replace a published artifact.
printf 'corrupt-container' >"${WEB}/bad.cbz"
cp "${WEB}/direct.cbz" "${WORK}/direct/bad.cbz"
BAD_BEFORE_HASH="$("${CMAKE}" -E sha256sum "${WORK}/direct/bad.cbz")"
set +e
CURL_CA_BUNDLE="" SSL_CERT_FILE="" NO_PROXY=127.0.0.1 \
  "${MEDIA_DL}" "${TLS_BASE}/bad.cbz" --out "${WORK}/direct" --ca-file "${TLS_CERT}" \
  --allow-private --contact https://github.com/bsikar/ra8-firmware >"${WORK}/tls-bad.log" 2>&1
TLS_BAD_RC=$?
set -e
[[ ${TLS_BAD_RC} -eq 1 ]]
grep -q 'failed structural validation' "${WORK}/tls-bad.log"
BAD_AFTER_HASH="$("${CMAKE}" -E sha256sum "${WORK}/direct/bad.cbz")"
[[ "${BAD_BEFORE_HASH%% *}" == "${BAD_AFTER_HASH%% *}" ]]
[[ -z "$(find "${WORK}/direct" -name '.mdl-tmp-*' -print -quit)" ]]

# Verify treats a recognized symlink as corrupt rather than following it.
mkdir -p "${WORK}/verify-symlink"
ln -s "${WEB}/direct.cbz" "${WORK}/verify-symlink/outside.cbz"
set +e
"${MEDIA_DL}" --verify "${WORK}/verify-symlink" >"${WORK}/verify-symlink.log" 2>&1
VERIFY_SYMLINK_RC=$?
set -e
[[ ${VERIFY_SYMLINK_RC} -eq 1 ]]
grep -q 'not a regular file' "${WORK}/verify-symlink.log"
[[ -s "${WEB}/direct.cbz" ]]

# Library/list/update/verify dispatch uses the tracked state and propagates errors.
"${MEDIA_DL}" --list --out "${WORK}/out" >"${WORK}/list.log"
grep -q "${BASE}/series/" "${WORK}/list.log"
"${MEDIA_DL}" --config "${WORK}/site.conf" --series "${BASE}/series/" --update \
  --out "${WORK}/out" --format loose --allow-private \
  --contact https://github.com/bsikar/ra8-firmware
"${MEDIA_DL}" --update-all --config "${WORK}/site.conf" --out "${WORK}/out" \
  --format loose --allow-private --contact https://github.com/bsikar/ra8-firmware
"${MEDIA_DL}" --verify "${WORK}/out"
cp "${PAGE}" "${WORK}/page.save"
printf 'corrupt' >"${PAGE}"
set +e
"${MEDIA_DL}" --verify "${WORK}/out" >"${WORK}/verify-corrupt.log" 2>&1
VERIFY_RC=$?
set -e
[[ ${VERIFY_RC} -eq 1 ]]
mv "${WORK}/page.save" "${PAGE}"
"${MEDIA_DL}" --verify "${WORK}/out"

# Page, pack, and init-site are distinct command paths.
mkdir -p "${WORK}/page-mode"
printf 'stale jpeg' >"${WORK}/page-mode/page_0001.jpg"
"${MEDIA_DL}" "${BASE}/page.html" --attr src --max 1 --out "${WORK}/page-mode" \
  --allow-private --contact https://github.com/bsikar/ra8-firmware
[[ -s "${WORK}/page-mode/page_0001.png" ]]
[[ ! -e "${WORK}/page-mode/page_0001.jpg" ]]
set +e
"${MEDIA_DL}" "${BASE}/page-invalid.html" --attr src --max 1 --out "${WORK}/page-invalid" \
  --allow-private --contact https://github.com/bsikar/ra8-firmware >"${WORK}/page-invalid.log" 2>&1
PAGE_INVALID_RC=$?
set -e
[[ ${PAGE_INVALID_RC} -eq 1 ]]
[[ -z "$(find "${WORK}/page-invalid" -type f -print -quit)" ]]
mkdir -p "${WORK}/page-preserve"
printf 'existing page sentinel\n' >"${WORK}/page-preserve/page_0001.jpg"
PAGE_SENTINEL_HASH="$("${CMAKE}" -E sha256sum "${WORK}/page-preserve/page_0001.jpg")"
set +e
"${MEDIA_DL}" "${BASE}/page-invalid.html" --attr src --max 1 \
  --out "${WORK}/page-preserve" --allow-private \
  --contact https://github.com/bsikar/ra8-firmware >"${WORK}/page-preserve.log" 2>&1
PAGE_PRESERVE_RC=$?
"${MEDIA_DL}" "${BASE}/page-empty.html" --attr src --max 1 \
  --out "${WORK}/page-empty" --allow-private \
  --contact https://github.com/bsikar/ra8-firmware >"${WORK}/page-empty.log" 2>&1
PAGE_EMPTY_RC=$?
set -e
[[ ${PAGE_PRESERVE_RC} -eq 1 ]]
[[ ${PAGE_EMPTY_RC} -eq 1 ]]
AFTER_PAGE_SENTINEL_HASH="$("${CMAKE}" -E sha256sum "${WORK}/page-preserve/page_0001.jpg")"
[[ "${PAGE_SENTINEL_HASH%% *}" == "${AFTER_PAGE_SENTINEL_HASH%% *}" ]]
[[ -z "$(find "${WORK}/page-preserve" \( -name '*.download' -o -name '.mdl-tmp-*' \) \
  -print -quit)" ]]
grep -q 'page contains no supported image URLs' "${WORK}/page-empty.log"
[[ -z "$(find "${WORK}/page-empty" -type f -print -quit)" ]]
"${MEDIA_DL}" --pack "${PAGE_DIR}" --format cbz
PACKED_CBZ="${PAGE_DIR}.cbz"
PACKED_ONCE_HASH="$("${CMAKE}" -E sha256sum "${PACKED_CBZ}")"
"${MEDIA_DL}" --pack "${PAGE_DIR}" --format cbz
PACKED_TWICE_HASH="$("${CMAKE}" -E sha256sum "${PACKED_CBZ}")"
[[ "${PACKED_ONCE_HASH%% *}" == "${PACKED_TWICE_HASH%% *}" ]]
mkdir -p "${WORK}/pack-empty"
cp "${PACKED_CBZ}" "${WORK}/pack-empty.cbz"
PACK_EMPTY_BEFORE="$("${CMAKE}" -E sha256sum "${WORK}/pack-empty.cbz")"
set +e
"${MEDIA_DL}" --pack "${WORK}/pack-empty" --format cbz >"${WORK}/pack-empty.log" 2>&1
PACK_EMPTY_RC=$?
set -e
[[ ${PACK_EMPTY_RC} -eq 1 ]]
PACK_EMPTY_AFTER="$("${CMAKE}" -E sha256sum "${WORK}/pack-empty.cbz")"
[[ "${PACK_EMPTY_BEFORE%% *}" == "${PACK_EMPTY_AFTER%% *}" ]]
grep -q 'FAILED' "${WORK}/pack-empty.log"
[[ -z "$(find "${WORK}" -maxdepth 1 -name '.mdl-tmp-*pack-empty.cbz' -print -quit)" ]]
mkdir -p "${WORK}/init"
(cd "${WORK}/init" && "${MEDIA_DL}" --init-site https://reader.example/books/)
[[ -s "${WORK}/init/reader.conf" ]]
printf '# user edit\n' >>"${WORK}/init/reader.conf"
INIT_BEFORE="$("${CMAKE}" -E sha256sum "${WORK}/init/reader.conf")"
set +e
(cd "${WORK}/init" && "${MEDIA_DL}" --init-site https://reader.example/books/ \
  >"${WORK}/init-repeat.log" 2>&1)
INIT_REPEAT_RC=$?
set -e
[[ ${INIT_REPEAT_RC} -eq 1 ]]
INIT_AFTER="$("${CMAKE}" -E sha256sum "${WORK}/init/reader.conf")"
[[ "${INIT_BEFORE%% *}" == "${INIT_AFTER%% *}" ]]
grep -q 'refusing to overwrite existing site descriptor' "${WORK}/init-repeat.log"
[[ -z "$(find "${WORK}/init" -name '.mdl-tmp-*' -print -quit)" ]]

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

# Corrupt state is a hard stop and is never replaced with an empty new state.
cp "${STATE}" "${WORK}/state.save"
printf 'V\t999\nmalformed\n' >"${STATE}"
CORRUPT_STATE_HASH="$("${CMAKE}" -E sha256sum "${STATE}")"
set +e
"${MEDIA_DL}" --list --out "${WORK}/out" >"${WORK}/list-corrupt.log" 2>&1
LIST_CORRUPT_RC=$?
"${MEDIA_DL}" --update-all --config "${WORK}/site.conf" --out "${WORK}/out" \
  --format loose --allow-private --contact https://github.com/bsikar/ra8-firmware \
  >"${WORK}/update-all-corrupt.log" 2>&1
UPDATE_ALL_CORRUPT_RC=$?
"${MEDIA_DL}" --config "${WORK}/site.conf" --series "${BASE}/series/" \
  --chapters 1 --out "${WORK}/out" --format loose --allow-private \
  --contact https://github.com/bsikar/ra8-firmware >"${WORK}/state-corrupt.log" 2>&1
CORRUPT_STATE_RC=$?
set -e
[[ ${LIST_CORRUPT_RC} -eq 1 ]]
[[ ${UPDATE_ALL_CORRUPT_RC} -eq 1 ]]
[[ ${CORRUPT_STATE_RC} -eq 1 ]]
grep -q 'state file unreadable' "${WORK}/list-corrupt.log"
grep -q '1 series failed to update' "${WORK}/update-all-corrupt.log"
AFTER_CORRUPT_STATE_HASH="$("${CMAKE}" -E sha256sum "${STATE}")"
[[ "${CORRUPT_STATE_HASH%% *}" == "${AFTER_CORRUPT_STATE_HASH%% *}" ]]
mv "${WORK}/state.save" "${STATE}"

# Removal refuses missing, corrupt, mismatched, and symlink markers without
# touching their trees. Both a plain slug and a URL may target a valid series.
mkdir -p "${WORK}/out/untracked" "${WORK}/out/junk" "${WORK}/out/not-series"
printf 'keep\n' >"${WORK}/out/untracked/keep"
printf 'not state\n' >"${WORK}/out/junk/.mdl_state"
printf 'keep\n' >"${WORK}/out/junk/keep"
cp "${STATE}" "${WORK}/out/not-series/.mdl_state"
printf 'keep\n' >"${WORK}/out/not-series/keep"
mkdir -p "${WORK}/outside-remove"
cp "${STATE}" "${WORK}/outside-remove/.mdl_state"
printf 'outside keep\n' >"${WORK}/outside-remove/keep"
ln -s "${WORK}/outside-remove" "${WORK}/out/symlink-remove"
set +e
"${MEDIA_DL}" --remove untracked --out "${WORK}/out" >"${WORK}/remove-untracked.log" 2>&1
REMOVE_UNTRACKED_RC=$?
"${MEDIA_DL}" --remove junk --out "${WORK}/out" >"${WORK}/remove-junk.log" 2>&1
REMOVE_JUNK_RC=$?
"${MEDIA_DL}" --remove not-series --out "${WORK}/out" >"${WORK}/remove-mismatch.log" 2>&1
REMOVE_MISMATCH_RC=$?
"${MEDIA_DL}" --remove symlink-remove --out "${WORK}/out" >"${WORK}/remove-symlink.log" 2>&1
REMOVE_SYMLINK_RC=$?
set -e
[[ ${REMOVE_UNTRACKED_RC} -eq 1 ]]
[[ ${REMOVE_JUNK_RC} -eq 1 ]]
[[ ${REMOVE_MISMATCH_RC} -eq 1 ]]
[[ ${REMOVE_SYMLINK_RC} -eq 1 ]]
grep -q 'missing or unsafe .mdl_state' "${WORK}/remove-untracked.log"
grep -q 'state is unreadable or corrupt' "${WORK}/remove-junk.log"
grep -q 'state identity does not match target' "${WORK}/remove-mismatch.log"
grep -q 'unsafe series path' "${WORK}/remove-symlink.log"
grep -qx 'keep' "${WORK}/out/untracked/keep"
grep -qx 'keep' "${WORK}/out/junk/keep"
grep -qx 'keep' "${WORK}/out/not-series/keep"
grep -qx 'outside keep' "${WORK}/outside-remove/keep"
[[ -L "${WORK}/out/symlink-remove" ]]
"${MEDIA_DL}" --remove series --out "${WORK}/browse-picked"
[[ ! -e "${WORK}/browse-picked/series" ]]
"${MEDIA_DL}" --remove "${BASE}/series/" --out "${WORK}/out"
[[ ! -e "${WORK}/out/series/.mdl_state" ]]
expect_usage --remove series --out "${WORK}/out" --timeout 1

printf 'media_dl local HTTP integration: PASS\n'
