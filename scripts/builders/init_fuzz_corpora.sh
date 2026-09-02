#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/builders/init_fuzz_corpora.sh -- seed the libFuzzer corpus
# directories under tests/fuzz/corpus/<target>/ with known-good
# inputs so each harness starts from real coverage on the first
# invocation. Idempotent: re-running overwrites the seed files in
# place but does not touch any crash reproducers added by the
# fuzzer or by hand.
#
# This script is invoked automatically by `just quality::local::fuzz`
# target before the harnesses run. It can also be run manually:
#
#     bash scripts/builders/init_fuzz_corpora.sh
#
# See docs/FUZZING.md for the corpus policy.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CORPUS_ROOT="${ROOT}/tests/fuzz/corpus"

# Every registered harness needs its corpus directory to exist before the
# sweep runs it -- run_fuzz.sh fails hard on a missing corpus dir, and the
# self-hosted runner's `git clean -xffd` deletes any untracked (empty) dir
# between jobs, so the directories cannot simply be committed. Derive the list
# from the ONE registry run_fuzz.sh uses (RA8_FUZZ_TARGETS in
# tests/fuzz/CMakeLists.txt) rather than repeating it here: a hand-maintained
# parallel list is exactly how fuzz_ra8_rar was registered and swept but never
# got a corpus dir, reddening the nightly. Deriving it means a new harness can
# never be forgotten.
_fuzz_targets="$(bash "${ROOT}/scripts/checks/run_fuzz.sh" --list)"
if [ -z "${_fuzz_targets}" ]; then
  echo "init_fuzz_corpora.sh: FATAL -- run_fuzz.sh --list returned no targets" >&2
  exit 1
fi
while IFS= read -r _target; do
  [ -n "${_target}" ] || continue
  mkdir -p "${CORPUS_ROOT}/${_target}"
done <<EOF
${_fuzz_targets}
EOF

# -----------------------------------------------------------------------------
# fuzz_ra8_jpeg_sw -- minimal baseline JPEGs at five sizes.
# -----------------------------------------------------------------------------
JPEG_DIR="${CORPUS_ROOT}/fuzz_ra8_jpeg_sw"
python3 "${SCRIPT_DIR}/../gen/gen_jpeg_fixture.py" --width 8 --height 8 -o "${JPEG_DIR}/seed_8x8.jpg"
python3 "${SCRIPT_DIR}/../gen/gen_jpeg_fixture.py" --width 16 --height 16 -o "${JPEG_DIR}/seed_16x16.jpg"
python3 "${SCRIPT_DIR}/../gen/gen_jpeg_fixture.py" --width 32 --height 24 -o "${JPEG_DIR}/seed_32x24.jpg"
python3 "${SCRIPT_DIR}/../gen/gen_jpeg_fixture.py" --width 64 --height 64 -o "${JPEG_DIR}/seed_64x64.jpg"
python3 "${SCRIPT_DIR}/../gen/gen_jpeg_fixture.py" --width 1 --height 1 -o "${JPEG_DIR}/seed_1x1.jpg"

# -----------------------------------------------------------------------------
# fuzz_epub -- two minimal valid EPUB containers.
# An EPUB is a ZIP whose first entry must be `mimetype` stored
# uncompressed. Python's zipfile module supports STORED entries
# directly, so we construct the archives without an external `zip`.
# -----------------------------------------------------------------------------
EPUB_DIR="${CORPUS_ROOT}/fuzz_epub"
python3 - "${EPUB_DIR}/seed_minimal.epub" "${EPUB_DIR}/seed_two_chapters.epub" <<'PY'
import sys
import zipfile

CONTAINER_XML = b"""<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""

OPF_ONE = b"""<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="bookid">urn:uuid:00000000-0000-0000-0000-000000000001</dc:identifier>
    <dc:title>Seed Book</dc:title>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
    <item id="ch1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine>
    <itemref idref="ch1"/>
  </spine>
</package>
"""

OPF_TWO = b"""<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="bookid">urn:uuid:00000000-0000-0000-0000-000000000002</dc:identifier>
    <dc:title>Seed Book Two Chapters</dc:title>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
    <item id="ch1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>
    <item id="ch2" href="chapter2.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine>
    <itemref idref="ch1"/>
    <itemref idref="ch2"/>
  </spine>
</package>
"""

CHAPTER1 = b"""<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Ch1</title></head>
<body><h1>Chapter 1</h1><p>Hello.</p></body></html>
"""

CHAPTER2 = b"""<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Ch2</title></head>
<body><h1>Chapter 2</h1><p>Goodbye.</p></body></html>
"""


def write_epub(path, opf, chapters):
    with zipfile.ZipFile(path, "w") as z:
        # mimetype must be the first entry, STORED, no extra fields.
        zi = zipfile.ZipInfo("mimetype")
        zi.compress_type = zipfile.ZIP_STORED
        z.writestr(zi, b"application/epub+zip")
        z.writestr("META-INF/container.xml", CONTAINER_XML,
                   compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/content.opf", opf,
                   compress_type=zipfile.ZIP_DEFLATED)
        for name, body in chapters:
            z.writestr("OEBPS/" + name, body,
                       compress_type=zipfile.ZIP_DEFLATED)


write_epub(sys.argv[1], OPF_ONE, [("chapter1.xhtml", CHAPTER1)])
write_epub(sys.argv[2], OPF_TWO,
           [("chapter1.xhtml", CHAPTER1), ("chapter2.xhtml", CHAPTER2)])
PY

# -----------------------------------------------------------------------------
# fuzz_ra8_modem_at -- ten sample AT response strings.
# CRLF line endings; the parser is line-oriented.
# -----------------------------------------------------------------------------
AT_DIR="${CORPUS_ROOT}/fuzz_ra8_modem_at"
printf 'OK\r\n' >"${AT_DIR}/seed_ok.txt"
printf 'ERROR\r\n' >"${AT_DIR}/seed_error.txt"
printf '+CSQ: 25,99\r\nOK\r\n' >"${AT_DIR}/seed_csq.txt"
printf '+CME ERROR: 100\r\n' >"${AT_DIR}/seed_cme_error.txt"
printf '+CREG: 0,1\r\nOK\r\n' >"${AT_DIR}/seed_creg.txt"
printf '+CGATT: 1\r\nOK\r\n' >"${AT_DIR}/seed_cgatt.txt"
printf 'AT\r\r\nOK\r\n' >"${AT_DIR}/seed_echo_ok.txt"
printf '+CGDCONT: 1,"IP","internet"\r\nOK\r\n' >"${AT_DIR}/seed_cgdcont.txt"
printf 'NO CARRIER\r\n' >"${AT_DIR}/seed_no_carrier.txt"
printf '+CMTI: "SM",3\r\n' >"${AT_DIR}/seed_cmti.txt"

# -----------------------------------------------------------------------------
# fuzz_ra8_canfd -- five raw CFDRF[0] frame-injection blobs.
# Layout: 12-byte header (ID, PTR, FDSTS little-endian) + payload bytes.
# -----------------------------------------------------------------------------
CANFD_DIR="${CORPUS_ROOT}/fuzz_ra8_canfd"
python3 - "${CANFD_DIR}" <<'PY'
import os
import struct
import sys

OUTDIR = sys.argv[1]


def frame(id_word, ptr_word, fdsts_word, payload):
    return struct.pack("<III", id_word, ptr_word, fdsts_word) + payload


frames = {
    "seed_classic_id123_dlc8.bin": frame(0x123, (8 << 28), 0, bytes(range(8))),
    "seed_extended_id1fffabcd.bin": frame((0x1FFFABCD | (1 << 31)),
                                          (8 << 28), 0, b"\xAA" * 8),
    "seed_canfd_brs_dlc15.bin": frame(0x456, (15 << 28), 0x3, b"\x55" * 64),
    "seed_min_payload.bin": frame(0x000, 0, 0, b""),
    "seed_max_payload.bin": frame(0x7FF, (15 << 28), 0, b"\xFF" * 64),
}

for name, blob in frames.items():
    with open(os.path.join(OUTDIR, name), "wb") as fh:
        fh.write(blob)
PY

# -----------------------------------------------------------------------------
# fuzz_ra8_etha -- five short Ethernet frames (ARP, IPv4, VLAN, runt, full).
# -----------------------------------------------------------------------------
ETHA_DIR="${CORPUS_ROOT}/fuzz_ra8_etha"
python3 - "${ETHA_DIR}" <<'PY'
import os
import sys

OUTDIR = sys.argv[1]
DST = bytes([0xFF] * 6)
SRC = bytes([0x02, 0x00, 0x00, 0x00, 0x00, 0x01])

frames = {
    "seed_arp.bin":     DST + SRC + b"\x08\x06" + b"\x00" * 28,
    "seed_ipv4.bin":    DST + SRC + b"\x08\x00" + b"\x45\x00\x00\x14" + b"\x00" * 16,
    "seed_vlan.bin":    DST + SRC + b"\x81\x00\x00\x01" + b"\x08\x00" + b"\x00" * 20,
    "seed_runt.bin":    DST + SRC + b"\x00\x00",
    "seed_min_hdr.bin": DST + SRC + b"\x00\x00",
}

for name, blob in frames.items():
    with open(os.path.join(OUTDIR, name), "wb") as fh:
        fh.write(blob)
PY

# -----------------------------------------------------------------------------
# fuzz_ra8_fs_fat -- four sparse FAT BPB seeds. Real FAT mount almost
# always rejects these; the goal is to give libFuzzer something with
# the BPB byte layout to mutate from.
# -----------------------------------------------------------------------------
FAT_DIR="${CORPUS_ROOT}/fuzz_ra8_fs_fat"
python3 - "${FAT_DIR}" <<'PY'
import os
import struct
import sys

OUTDIR = sys.argv[1]


def bpb(bytes_per_sec=512, sec_per_clus=8, rsvd=1, num_fats=2,
        root_entries=512, tot_sec16=0, media=0xF8, fat_sz16=128,
        sec_per_track=63, num_heads=2, hidden_sec=0, tot_sec32=131072):
    sec = bytearray(512)
    sec[0:3] = b"\xEB\x3C\x90"
    sec[3:11] = b"MSDOS5.0"
    struct.pack_into("<H", sec, 11, bytes_per_sec)
    sec[13] = sec_per_clus
    struct.pack_into("<H", sec, 14, rsvd)
    sec[16] = num_fats
    struct.pack_into("<H", sec, 17, root_entries)
    struct.pack_into("<H", sec, 19, tot_sec16)
    sec[21] = media
    struct.pack_into("<H", sec, 22, fat_sz16)
    struct.pack_into("<H", sec, 24, sec_per_track)
    struct.pack_into("<H", sec, 26, num_heads)
    struct.pack_into("<I", sec, 28, hidden_sec)
    struct.pack_into("<I", sec, 32, tot_sec32)
    sec[510] = 0x55
    sec[511] = 0xAA
    return bytes(sec)


seeds = {
    "seed_fat16_basic.bin":   bpb(),
    "seed_fat16_4kclus.bin":  bpb(sec_per_clus=8, fat_sz16=64),
    "seed_fat16_min.bin":     bpb(sec_per_clus=1, fat_sz16=16, tot_sec32=8192),
    "seed_zero.bin":          bytes(512),
}

for name, blob in seeds.items():
    with open(os.path.join(OUTDIR, name), "wb") as fh:
        fh.write(blob)
PY

# -----------------------------------------------------------------------------
# fuzz_ra8_jpeg_sw_block -- seed scan-data fragments for the focused
# Huffman-decoder fuzzer. The harness prefixes a fixed JFIF header so
# these inputs land directly in the entropy-coded segment.
# -----------------------------------------------------------------------------
JPEG_BLOCK_DIR="${CORPUS_ROOT}/fuzz_ra8_jpeg_sw_block"
printf '\x00\x00\x00' >"${JPEG_BLOCK_DIR}/seed_zero_dc.bin"
printf '\xFF\x00\xFF\x00\xFF\x00' >"${JPEG_BLOCK_DIR}/seed_byte_stuffed.bin"
printf '\xAA\xAA\xAA\xAA' >"${JPEG_BLOCK_DIR}/seed_alternating.bin"
printf '\x55\x55\x55\x55\x55\x55\x55\x55' >"${JPEG_BLOCK_DIR}/seed_low_freq.bin"

# -----------------------------------------------------------------------------
# fuzz_ra8_stb_image -- one valid 1x1 24-bpp BMP (fastest format for stb to
# reach a full decode) plus a truncated header that stbi_info rejects.
# -----------------------------------------------------------------------------
STB_IMAGE_DIR="${CORPUS_ROOT}/fuzz_ra8_stb_image"
python3 - "${STB_IMAGE_DIR}" <<'PY'
import os
import struct
import sys

OUTDIR = sys.argv[1]

# BITMAPINFOHEADER 1x1 24-bpp, bottom-up, one BGR pixel + row padding.
pixels = b"\x00\x00\xFF\x00"          # blue=0 green=0 red=255, pad to 4 bytes
dib = struct.pack("<IiiHHIIiiII", 40, 1, 1, 1, 24, 0, len(pixels), 2835, 2835, 0, 0)
offset = 14 + len(dib)
filesz = offset + len(pixels)
fhdr = b"BM" + struct.pack("<IHHI", filesz, 0, 0, offset)
with open(os.path.join(OUTDIR, "seed_1x1.bmp"), "wb") as fh:
    fh.write(fhdr + dib + pixels)

# Malformed: BMP magic then garbage -- stbi_info must reject without a crash.
with open(os.path.join(OUTDIR, "seed_malformed.bin"), "wb") as fh:
    fh.write(b"BM" + b"\xFF" * 30)
PY

# -----------------------------------------------------------------------------
# fuzz_reflow_xml -- one minimal valid OPF package document plus a
# malformed (unbalanced) XML fragment for the XML parse entries.
# -----------------------------------------------------------------------------
REFLOW_XML_DIR="${CORPUS_ROOT}/fuzz_reflow_xml"
cat >"${REFLOW_XML_DIR}/seed_opf.xml" <<'XML'
<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="bookid">urn:uuid:00000000-0000-0000-0000-000000000001</dc:identifier>
    <dc:title>Seed Book</dc:title>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
    <item id="ch1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine>
    <itemref idref="ch1"/>
  </spine>
</package>
XML
printf '<package><metadata><dc:title>oops' >"${REFLOW_XML_DIR}/seed_malformed.xml"

# -----------------------------------------------------------------------------
# fuzz_ra8_stbtt -- the bundled Latin-1 TTF (a real, complete font gets stb to
# a valid stbtt_InitFont state instantly) plus a garbage blob.
# -----------------------------------------------------------------------------
STBTT_DIR="${CORPUS_ROOT}/fuzz_ra8_stbtt"
cp "${ROOT}/libs/ra8_fonts/literata_latin1.ttf" "${STBTT_DIR}/seed_literata_latin1.ttf"
printf 'OTTOnot-a-real-font\x00\x00\x00\x00' >"${STBTT_DIR}/seed_garbage.bin"

# -----------------------------------------------------------------------------
# fuzz_ra8_webp -- the four committed WebP decode fixtures (both VP8L lossless
# and VP8 lossy give the decoder immediate coverage) plus a truncated RIFF
# header the decoder must reject cleanly.
# -----------------------------------------------------------------------------
WEBP_DIR="${CORPUS_ROOT}/fuzz_ra8_webp"
cp "${ROOT}/tests/fixtures/webp/fixture_lossless.webp" "${WEBP_DIR}/seed_lossless.webp"
cp "${ROOT}/tests/fixtures/webp/fixture_lossy.webp" "${WEBP_DIR}/seed_lossy.webp"
cp "${ROOT}/tests/fixtures/webp/fixture_wide.webp" "${WEBP_DIR}/seed_wide.webp"
cp "${ROOT}/tests/fixtures/webp/fixture_tall.webp" "${WEBP_DIR}/seed_tall.webp"
printf 'RIFF\x08\x00\x00\x00WEBP' >"${WEBP_DIR}/seed_truncated.bin"

# -----------------------------------------------------------------------------
# fuzz_jof -- one hand-built valid JOF raw-codec atlas (16x16
# gray8, 8x8 tiles) so the reader starts past the header validation, plus a
# truncated header the parser must reject cleanly.
# -----------------------------------------------------------------------------
JOF_DIR="${CORPUS_ROOT}/fuzz_jof"
python3 - "${JOF_DIR}" <<'PY'
import os
import struct
import sys

OUTDIR = sys.argv[1]
W = H = 16
T = 8
BPP = 1
cols = rows = W // T
count = cols * rows

hdr = b"JOF1" + struct.pack("<HHHHBBHI", W, H, T, T, BPP, 0, 0, count) + b"\x00" * 12
tiles = []
offs = []
pos = len(hdr)
for ty in range(rows):
    for tx in range(cols):
        px = bytes(((x + tx * T) * 3 + (y + ty * T) * 7) & 0xFF for y in range(T) for x in range(T))
        offs.append((pos, len(px)))
        tiles.append(px)
        pos += len(px)
index = b"".join(struct.pack("<II", o, n) for (o, n) in offs)
index_off = pos
total = index_off + len(index) + 16
footer = struct.pack("<III", index_off, count, total) + b"JOFE"
atlas = hdr + b"".join(tiles) + index + footer
assert len(atlas) == total
with open(os.path.join(OUTDIR, "seed_raw_16x16.rta"), "wb") as fh:
    fh.write(atlas)
with open(os.path.join(OUTDIR, "seed_truncated.bin"), "wb") as fh:
    fh.write(atlas[:24])
PY

# -----------------------------------------------------------------------------
# fuzz_jof_produce -- a small gray8 PNG and a small baseline JPEG
# (both real decodable sources for immediate stripe-decoder coverage) plus a
# bare PNG signature the sniffer accepts but the chunk walk must reject.
# -----------------------------------------------------------------------------
RTAP_DIR="${CORPUS_ROOT}/fuzz_jof_produce"
python3 - "${RTAP_DIR}" <<'PY'
import os
import struct
import sys
import zlib

OUTDIR = sys.argv[1]
W = H = 16

def chunk(tag: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + tag
        + payload
        + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
    )

raw = b"".join(b"\x00" + bytes(((x * 3) ^ y) & 0xFF for x in range(W)) for y in range(H))
ihdr = struct.pack(">IIBBBBB", W, H, 8, 0, 0, 0, 0)
png = (
    b"\x89PNG\r\n\x1a\n"
    + chunk(b"IHDR", ihdr)
    + chunk(b"IDAT", zlib.compress(raw))
    + chunk(b"IEND", b"")
)
with open(os.path.join(OUTDIR, "seed_gray_16x16.png"), "wb") as fh:
    fh.write(png)
with open(os.path.join(OUTDIR, "seed_sig_only.bin"), "wb") as fh:
    fh.write(b"\x89PNG\r\n\x1a\n")
PY
python3 "${SCRIPT_DIR}/../gen/gen_jpeg_fixture.py" --width 16 --height 16 \
  -o "${RTAP_DIR}/seed_16x16.jpg"

# -----------------------------------------------------------------------------
# fuzz_unarch_{xz,tar,gzip} + fuzz_ra8_decomp_limits -- one honest seed
# per wrapper (a small XZ stream at the wrapper's dictionary scale, a
# two-member ustar comic, a gzip member) plus a raw-word seed for the
# limits-seam harness.
# -----------------------------------------------------------------------------
python3 - "${CORPUS_ROOT}" <<'PY'
import gzip
import io
import lzma
import os
import struct
import sys
import tarfile

ROOT = sys.argv[1]

def tar_bytes() -> bytes:
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tf:
        for name, payload in ((b"page1.png", b"PAGE-ONE"), (b"page2.png", b"PAGE-TWO")):
            info = tarfile.TarInfo(name.decode("ascii"))
            info.size = len(payload)
            info.mtime = 0
            tf.addfile(info, io.BytesIO(payload))
    return buf.getvalue()

tar = tar_bytes()
with open(os.path.join(ROOT, "fuzz_unarch_tar", "seed_two_pages.tar"), "wb") as fh:
    fh.write(tar)

filters = [{"id": lzma.FILTER_LZMA2, "preset": 6, "dict_size": 1 << 16}]
xz = lzma.compress(tar, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64, filters=filters)
with open(os.path.join(ROOT, "fuzz_unarch_xz", "seed_two_pages.tar.xz"), "wb") as fh:
    fh.write(xz)

gz_buf = io.BytesIO()
with gzip.GzipFile(fileobj=gz_buf, mode="wb", filename="pages.tar", mtime=0) as gf:
    gf.write(tar)
with open(os.path.join(ROOT, "fuzz_unarch_gzip", "seed_two_pages.tar.gz"), "wb") as fh:
    fh.write(gz_buf.getvalue())

# Limits seam: eight LE64 words -- a valid tightened policy followed by a
# hostile declared pair, so the first run already walks every charge arm.
words = (1 << 20, 8, 4096, 64, 128, 2, 1 << 8, 1 << 16)
with open(os.path.join(ROOT, "fuzz_ra8_decomp_limits", "seed_policy.bin"), "wb") as fh:
    fh.write(struct.pack("<8Q", *words))
PY

echo "Seeded fuzz corpora under ${CORPUS_ROOT}/."
