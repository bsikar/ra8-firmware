#!/usr/bin/env bash
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
# scripts/utils/init_fuzz_corpora.sh -- seed the libFuzzer corpus
# directories under tests/fuzz/corpus/<target>/ with known-good
# inputs so each harness starts from real coverage on the first
# invocation. Idempotent: re-running overwrites the seed files in
# place but does not touch any crash reproducers added by the
# fuzzer or by hand.
#
# This script is invoked automatically by the top-level `make fuzz`
# target before the harnesses run. It can also be run manually:
#
#     bash scripts/utils/init_fuzz_corpora.sh
#
# See docs/FUZZING.md for the corpus policy.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CORPUS_ROOT="${ROOT}/tests/fuzz/corpus"

mkdir -p \
    "${CORPUS_ROOT}/fuzz_ra_jpeg_sw" \
    "${CORPUS_ROOT}/fuzz_ra_epub" \
    "${CORPUS_ROOT}/fuzz_ra_modem_at" \
    "${CORPUS_ROOT}/fuzz_ra_net_arp" \
    "${CORPUS_ROOT}/fuzz_ra_net_ipv4" \
    "${CORPUS_ROOT}/fuzz_ra_ble_att" \
    "${CORPUS_ROOT}/fuzz_ra_usb_pal" \
    "${CORPUS_ROOT}/fuzz_ra_tls"

# -----------------------------------------------------------------------------
# fuzz_ra_jpeg_sw -- minimal baseline JPEGs at five sizes.
# -----------------------------------------------------------------------------
JPEG_DIR="${CORPUS_ROOT}/fuzz_ra_jpeg_sw"
python3 "${SCRIPT_DIR}/gen_jpeg_fixture.py" --width  8 --height  8 -o "${JPEG_DIR}/seed_8x8.jpg"
python3 "${SCRIPT_DIR}/gen_jpeg_fixture.py" --width 16 --height 16 -o "${JPEG_DIR}/seed_16x16.jpg"
python3 "${SCRIPT_DIR}/gen_jpeg_fixture.py" --width 32 --height 24 -o "${JPEG_DIR}/seed_32x24.jpg"
python3 "${SCRIPT_DIR}/gen_jpeg_fixture.py" --width 64 --height 64 -o "${JPEG_DIR}/seed_64x64.jpg"
python3 "${SCRIPT_DIR}/gen_jpeg_fixture.py" --width  1 --height  1 -o "${JPEG_DIR}/seed_1x1.jpg"

# -----------------------------------------------------------------------------
# fuzz_ra_epub -- two minimal valid EPUB containers.
# An EPUB is a ZIP whose first entry must be `mimetype` stored
# uncompressed. Python's zipfile module supports STORED entries
# directly, so we construct the archives without an external `zip`.
# -----------------------------------------------------------------------------
EPUB_DIR="${CORPUS_ROOT}/fuzz_ra_epub"
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
# fuzz_ra_modem_at -- ten sample AT response strings.
# CRLF line endings; the parser is line-oriented.
# -----------------------------------------------------------------------------
AT_DIR="${CORPUS_ROOT}/fuzz_ra_modem_at"
printf 'OK\r\n'                              > "${AT_DIR}/seed_ok.txt"
printf 'ERROR\r\n'                           > "${AT_DIR}/seed_error.txt"
printf '+CSQ: 25,99\r\nOK\r\n'               > "${AT_DIR}/seed_csq.txt"
printf '+CME ERROR: 100\r\n'                 > "${AT_DIR}/seed_cme_error.txt"
printf '+CREG: 0,1\r\nOK\r\n'                > "${AT_DIR}/seed_creg.txt"
printf '+CGATT: 1\r\nOK\r\n'                 > "${AT_DIR}/seed_cgatt.txt"
printf 'AT\r\r\nOK\r\n'                      > "${AT_DIR}/seed_echo_ok.txt"
printf '+CGDCONT: 1,"IP","internet"\r\nOK\r\n' > "${AT_DIR}/seed_cgdcont.txt"
printf 'NO CARRIER\r\n'                      > "${AT_DIR}/seed_no_carrier.txt"
printf '+CMTI: "SM",3\r\n'                   > "${AT_DIR}/seed_cmti.txt"

# -----------------------------------------------------------------------------
# fuzz_ra_net_arp -- five Ethernet/ARP frames (request + reply variants).
# Layout: dst[6] src[6] type[2]=0x0806 + ARP payload (28 bytes for IPv4).
# Total frame size 42 bytes (below 60-byte Ethernet minimum but the
# harness only requires >= 14).
# -----------------------------------------------------------------------------
ARP_DIR="${CORPUS_ROOT}/fuzz_ra_net_arp"
python3 - "${ARP_DIR}" <<'PY'
import os
import sys

OUTDIR = sys.argv[1]


def arp(dst, src, op, sha, spa, tha, tpa):
    eth = bytes(dst) + bytes(src) + b"\x08\x06"
    payload = (
        b"\x00\x01"          # HTYPE = Ethernet
        + b"\x08\x00"        # PTYPE = IPv4
        + b"\x06\x04"        # HLEN=6, PLEN=4
        + bytes([0, op])
        + bytes(sha) + bytes(spa)
        + bytes(tha) + bytes(tpa)
    )
    return eth + payload


BCAST = [0xFF] * 6
HOST  = [0x02, 0x00, 0x00, 0x00, 0x00, 0x01]
PEER  = [0x02, 0x00, 0x00, 0x00, 0x00, 0x02]
HOST_IP = [192, 168, 1, 10]
PEER_IP = [192, 168, 1, 11]
GW_IP   = [192, 168, 1, 1]

frames = {
    "seed_arp_request_for_host.bin":
        arp(BCAST, PEER, 1, PEER, PEER_IP, [0]*6, HOST_IP),
    "seed_arp_request_for_gw.bin":
        arp(BCAST, HOST, 1, HOST, HOST_IP, [0]*6, GW_IP),
    "seed_arp_reply_to_host.bin":
        arp(HOST, PEER, 2, PEER, PEER_IP, HOST, HOST_IP),
    "seed_arp_gratuitous.bin":
        arp(BCAST, PEER, 1, PEER, PEER_IP, [0]*6, PEER_IP),
    "seed_arp_reply_from_gw.bin":
        arp(HOST, [0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0x01], 2,
            [0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0x01], GW_IP, HOST, HOST_IP),
}

for name, blob in frames.items():
    with open(os.path.join(OUTDIR, name), "wb") as fh:
        fh.write(blob)
PY

# -----------------------------------------------------------------------------
# fuzz_ra_net_ipv4 -- five Ethernet/IPv4 frames covering ICMP echo,
# UDP DNS query, TCP SYN, UDP DHCP discover, ICMP echo reply.
# -----------------------------------------------------------------------------
IPV4_DIR="${CORPUS_ROOT}/fuzz_ra_net_ipv4"
python3 - "${IPV4_DIR}" <<'PY'
import os
import struct
import sys

OUTDIR = sys.argv[1]


def csum16(buf):
    s = 0
    if len(buf) % 2:
        buf = buf + b"\x00"
    for i in range(0, len(buf), 2):
        s += (buf[i] << 8) | buf[i + 1]
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def ipv4(proto, src, dst, payload, ident=0x4242):
    total_len = 20 + len(payload)
    hdr = bytearray(20)
    hdr[0] = 0x45                      # version=4 IHL=5
    hdr[1] = 0x00
    struct.pack_into(">H", hdr, 2, total_len)
    struct.pack_into(">H", hdr, 4, ident)
    struct.pack_into(">H", hdr, 6, 0)  # flags + frag offset
    hdr[8] = 64                        # TTL
    hdr[9] = proto
    hdr[10:12] = b"\x00\x00"
    hdr[12:16] = bytes(src)
    hdr[16:20] = bytes(dst)
    struct.pack_into(">H", hdr, 10, csum16(bytes(hdr)))
    return bytes(hdr) + payload


def eth_ipv4(dst_mac, src_mac, ip_payload):
    return bytes(dst_mac) + bytes(src_mac) + b"\x08\x00" + ip_payload


HOST_MAC = [0x02, 0x00, 0x00, 0x00, 0x00, 0x01]
PEER_MAC = [0x02, 0x00, 0x00, 0x00, 0x00, 0x02]
BCAST    = [0xFF] * 6
HOST_IP  = [192, 168, 1, 10]
PEER_IP  = [192, 168, 1, 11]
DNS_IP   = [8, 8, 8, 8]
ANY_IP   = [0, 0, 0, 0]
BCAST_IP = [255, 255, 255, 255]

# ICMP echo request
icmp_payload = b"\x08\x00\x00\x00\x00\x01\x00\x01" + b"abcdefgh"
icmp_payload = icmp_payload[:2] + struct.pack(">H", csum16(icmp_payload)) + icmp_payload[4:]
frames = {
    "seed_icmp_echo_request.bin":
        eth_ipv4(HOST_MAC, PEER_MAC, ipv4(1, PEER_IP, HOST_IP, icmp_payload)),
}

# ICMP echo reply
icmp_reply = b"\x00\x00\x00\x00\x00\x01\x00\x01" + b"abcdefgh"
icmp_reply = icmp_reply[:2] + struct.pack(">H", csum16(icmp_reply)) + icmp_reply[4:]
frames["seed_icmp_echo_reply.bin"] = \
    eth_ipv4(HOST_MAC, PEER_MAC, ipv4(1, PEER_IP, HOST_IP, icmp_reply))

# UDP DNS query
dns_q = (
    b"\x12\x34" + b"\x01\x00" + b"\x00\x01" + b"\x00\x00" * 3
    + b"\x07example\x03com\x00" + b"\x00\x01" + b"\x00\x01"
)
udp_dns = struct.pack(">HHHH", 53535, 53, 8 + len(dns_q), 0) + dns_q
frames["seed_udp_dns_query.bin"] = \
    eth_ipv4(PEER_MAC, HOST_MAC, ipv4(17, HOST_IP, DNS_IP, udp_dns))

# TCP SYN
tcp = bytearray(20)
struct.pack_into(">H", tcp, 0, 49152)   # src port
struct.pack_into(">H", tcp, 2, 80)      # dst port
struct.pack_into(">I", tcp, 4, 0xDEADBEEF)
struct.pack_into(">I", tcp, 8, 0)
tcp[12] = 0x50                          # data offset = 5*4 = 20
tcp[13] = 0x02                          # SYN
struct.pack_into(">H", tcp, 14, 64240)
frames["seed_tcp_syn.bin"] = \
    eth_ipv4(PEER_MAC, HOST_MAC, ipv4(6, HOST_IP, PEER_IP, bytes(tcp)))

# UDP DHCP discover (very abbreviated)
dhcp = bytearray(240)
dhcp[0] = 1                             # BOOTREQUEST
dhcp[1] = 1                             # Ethernet
dhcp[2] = 6                             # MAC len
struct.pack_into(">I", dhcp, 4, 0xCAFEBABE)
dhcp[28:34] = bytes(HOST_MAC)
dhcp[236:240] = b"\x63\x82\x53\x63"     # magic cookie
udp_dhcp = struct.pack(">HHHH", 68, 67, 8 + len(dhcp), 0) + bytes(dhcp)
frames["seed_udp_dhcp_discover.bin"] = \
    eth_ipv4(BCAST, HOST_MAC, ipv4(17, ANY_IP, BCAST_IP, udp_dhcp))

for name, blob in frames.items():
    with open(os.path.join(OUTDIR, name), "wb") as fh:
        fh.write(blob)
PY

echo "Seeded fuzz corpora under ${CORPUS_ROOT}/."
