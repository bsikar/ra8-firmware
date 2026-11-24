#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Emulator end-to-end verification for dfu_bootloader trial watchdog and revert.

Stages an authentic ECDSA-P256 signed Slot A image (seq=1), an authentic signed
Slot B trial payload (seq=2) that never refreshes WDT0 or confirms, and the
dual-copy boot-state MRAM record at 0x020FE000 into a composite ELF, then runs
ra8_emulator (--cpu zig) to verify:

- Trial boots emit "dfu-bootloader: arming trial watchdog", execute the Slot B
  trial payload, reset on WDT0 underflow (RSTSR1.WDTRF), and spend a try in
  the persistent MRAM record.
- Once tries are exhausted, the bootloader reverts to Slot A without arming WDT0
  and launches Slot A to run cleanly at k_ra8_dfu_run_base (0x22020000), marking
  0x22010000 with 0x9710C0DE.

If --key is supplied, payloads are signed via scripts/secrets/rot_sign.py and
the bootloader ELF is left untouched. Otherwise an ephemeral P-256 test keypair
is generated in memory and its SEC1 public point replaces root_public_key in a
temporary copy of the bootloader ELF only.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

# Code-MRAM map matching libs/ra8_dfu/inc/ra8_dfu.h.
SLOT_A_BASE = 0x02020000
SLOT_B_BASE = 0x0208F000
HDR_OFFSET = 0x0006EFE0
HDR_MAGIC = 0x52413844  # "RA8D"
RUN_BASE = 0x22020000
BOOT_STATE_COPY0 = 0x020FE000
BOOT_STATE_MAGIC = 0x52413842  # "RA8B"
ROT_MAGIC = 0x524F5431  # "ROT1"
ROT_TRAILER_LEN = 116
PAGE_SIZE = 32

SLOT_A_MARKER = 0x9710C0DE
SLOT_B_MARKER = 0xBAD00002
MARKER_ADDR = 0x22010000

# NIST P-256 (secp256r1) curve parameters for self-contained test signing.
_P256_P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
_P256_A = _P256_P - 3
_P256_N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
_P256_GX = 0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296
_P256_GY = 0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5
_TEST_PRIV = 0x1A2B3C4D5E6F708192A3B4C5D6E7F809102132435465768798A9BACBDCEDF012


def _ec_add(
    p1: tuple[int, int] | None, p2: tuple[int, int] | None
) -> tuple[int, int] | None:
    """Add two affine points on NIST P-256."""
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    x1, y1 = p1
    x2, y2 = p2
    if x1 == x2:
        if (y1 + y2) % _P256_P == 0:
            return None
        slope = (3 * x1 * x1 + _P256_A) * pow(2 * y1, -1, _P256_P) % _P256_P
    else:
        slope = (y2 - y1) * pow((x2 - x1) % _P256_P, -1, _P256_P) % _P256_P
    x3 = (slope * slope - x1 - x2) % _P256_P
    y3 = (slope * (x1 - x3) - y1) % _P256_P
    return (x3, y3)


def _ec_mul(scalar: int, point: tuple[int, int]) -> tuple[int, int]:
    """Compute scalar * point on NIST P-256 via binary double-and-add."""
    acc: tuple[int, int] | None = None
    cur: tuple[int, int] | None = point
    k = scalar
    while k:
        if k & 1:
            acc = _ec_add(acc, cur)
        cur = _ec_add(cur, cur)
        k >>= 1
    assert acc is not None
    return acc


def test_pubkey_sec1() -> bytes:
    """Return the 65-byte uncompressed SEC1 public key for the ephemeral test signer."""
    qx, qy = _ec_mul(_TEST_PRIV, (_P256_GX, _P256_GY))
    return b"\x04" + qx.to_bytes(32, "big") + qy.to_bytes(32, "big")


def sign_with_ephemeral_key(body: bytes, img_version: int) -> bytes:
    """Append a 116-byte ROT1 trailer signed with the ephemeral P-256 key."""
    digest = hashlib.sha256(body).digest()
    material = struct.pack("<I", img_version) + digest
    z = int.from_bytes(hashlib.sha256(material).digest(), "big")
    k = (int.from_bytes(hashlib.sha256(material + b"det").digest(), "big") % (_P256_N - 1)) + 1
    rx, _ = _ec_mul(k, (_P256_GX, _P256_GY))
    r = rx % _P256_N
    s = (pow(k, -1, _P256_N) * (z + r * _TEST_PRIV)) % _P256_N
    if s > _P256_N // 2:
        s = _P256_N - s
    sig = r.to_bytes(32, "big") + s.to_bytes(32, "big")
    trailer = struct.pack("<5I", ROT_MAGIC, 1, img_version, len(body), 64) + digest + sig
    assert len(trailer) == ROT_TRAILER_LEN
    return body + trailer


def sign_with_pem(repo_root: Path, key_path: Path, body: bytes, img_version: int) -> bytes:
    """Sign body using scripts/secrets/rot_sign.py and a PEM private key."""
    rot_sign = repo_root / "scripts" / "secrets" / "rot_sign.py"
    with tempfile.TemporaryDirectory() as td:
        body_path = Path(td) / "body.bin"
        out_path = Path(td) / "signed.bin"
        body_path.write_bytes(body)
        subprocess.run(
            [
                sys.executable,
                str(rot_sign),
                "sign",
                "--key",
                str(key_path),
                "--image",
                str(body_path),
                "--out",
                str(out_path),
                "--img-version",
                str(img_version),
            ],
            check=True,
        )
        return out_path.read_bytes()


def read_prod_pubkey(repo_root: Path) -> bytes:
    """Extract the 65-byte root_public_key constant from libs/ra8_rot/src/rot_abi.zig."""
    rot_abi = repo_root / "libs" / "ra8_rot" / "src" / "rot_abi.zig"
    text = rot_abi.read_text(encoding="ascii")
    m = re.search(r"const\s+root_public_key\s*=.*?\{(.*?)\};", text, re.S)
    if not m:
        raise RuntimeError(f"root_public_key not found in {rot_abi}")
    pub = bytes(int(tok, 16) for tok in re.findall(r"0x[0-9a-fA-F]{2}", m.group(1)))
    if len(pub) != 65 or pub[0] != 0x04:
        raise RuntimeError(f"unexpected root_public_key format ({len(pub)} bytes)")
    return pub


def make_wfi_payload(marker: int, banner: bytes) -> bytes:
    """Assemble a minimal copy-to-run Thumb-2 image linked at 0x22020000.

    Writes ``marker`` to 0x22010000, prints ``banner`` out SCI8 (which
    dfu_bootloader already initialized), stops SysTick (clearing any pending
    SysTick flag) so emulator WFI skips directly to WDT0 expiry boundaries while
    keeping WDT0 counting, enables interrupts, and parks in WFI.
    """
    pool_start = 0x80
    msg_off = 0xA0

    def ldr_lit(pc_off: int, reg: int, pool_idx: int) -> int:
        aligned_pc = (pc_off + 4) & ~3
        diff = (pool_start + 4 * pool_idx) - aligned_pc
        assert 0 <= diff < 1024 and (diff % 4) == 0
        return 0x4800 | (reg << 8) | (diff >> 2)

    vecs = [0x22060000, RUN_BASE + 0x41] + [RUN_BASE + 0x73] * 14
    insns = [
        ldr_lit(0x40, 2, 0),  # marker
        ldr_lit(0x42, 3, 1),  # 0x22010000
        0x601A,               # str r2, [r3, #0]
        ldr_lit(0x46, 0, 2),  # banner ptr (RUN_BASE + msg_off)
        ldr_lit(0x48, 1, 3),  # SCI8 base (0x40358800)
        ldr_lit(0x4A, 4, 4),  # CSR.TDRE bit 29 (0x20000000)
        0x7802,               # 0x4C: ldrb r2, [r0, #0]
        0x2A00,               # 0x4E: cmp r2, #0
        0xD006,               # 0x50: beq -> 0x60
        0x6C8B,               # 0x52: ldr r3, [r1, #0x48] (CSR)
        0x4223,               # 0x54: tst r3, r4
        0xD0FC,               # 0x56: beq -> 0x52
        0x604A,               # 0x58: str r2, [r1, #4] (TDR)
        0x3001,               # 0x5A: adds r0, #1
        0xE7F6,               # 0x5C: b -> 0x4C
        0xBF00,               # 0x5E: nop
        0x2200,               # 0x60: movs r2, #0
        ldr_lit(0x62, 3, 5),  # 0xE000E010 (SYST_CSR)
        0x601A,               # 0x64: str r2, [r3, #0]
        ldr_lit(0x66, 2, 6),  # 0x02000000 (PENDSTCLR)
        ldr_lit(0x68, 3, 7),  # 0xE000ED04 (SCB_ICSR)
        0x601A,               # 0x6A: str r2, [r3, #0]
        0xB662,               # 0x6C: cpsie i
        0xBF30,               # 0x6E: wfi
        0xE7FD,               # 0x70: b -> 0x6E
        0x4770,               # 0x72: bx lr (ISR)
    ] + [0xBF00] * 6
    pool = [
        marker,
        MARKER_ADDR,
        RUN_BASE + msg_off,
        0x40358800,
        0x20000000,
        0xE000E010,
        0x02000000,
        0xE000ED04,
    ]
    blob = (
        struct.pack("<16I", *vecs)
        + struct.pack(f"<{len(insns)}H", *insns)
        + struct.pack(f"<{len(pool)}I", *pool)
        + banner
        + b"\x00"
    )
    assert len(struct.pack("<16I", *vecs) + struct.pack(f"<{len(insns)}H", *insns) + struct.pack(f"<{len(pool)}I", *pool)) == msg_off
    pad = (PAGE_SIZE - (len(blob) % PAGE_SIZE)) % PAGE_SIZE
    return blob + (b"\x00" * pad)


def build_slot_segments(base: int, seq: int, signed_img: bytes) -> list[tuple[int, bytes]]:
    """Build (address, bytes) PT_LOAD pairs for a signed image + slot header."""
    img_len = len(signed_img) - ROT_TRAILER_LEN
    if img_len <= 0 or (img_len % PAGE_SIZE) != 0:
        raise ValueError(f"signed image body {img_len}B must be positive and 32B-aligned")
    crc = zlib.crc32(signed_img[:img_len]) & 0xFFFFFFFF
    hdr = struct.pack("<5I", HDR_MAGIC, seq, img_len, crc, RUN_BASE) + (b"\x00" * 12)
    return [(base, signed_img), (base + HDR_OFFSET, hdr)]


def make_boot_state(
    write_seq: int, trial_slot: int, tries_left: int, confirmed: int, slot_seq: int
) -> bytes:
    """Serialize one 32-byte ra8_dfu_boot_state_t record with trailing IEEE CRC32."""
    body = struct.pack(
        "<IIBBBBIIII",
        BOOT_STATE_MAGIC,
        write_seq,
        trial_slot,
        tries_left,
        confirmed,
        0,
        slot_seq,
        0,
        0,
        0,
    )
    crc = zlib.crc32(body) & 0xFFFFFFFF
    return body + struct.pack("<I", crc)


def patch_elf_with_slots(
    elf_bytes: bytes,
    segments: list[tuple[int, bytes]],
    replace_pubkey: tuple[bytes, bytes] | None = None,
) -> bytes:
    """Append PT_LOAD segments to a little-endian ELF32 image cleanly."""
    if elf_bytes[:4] != b"\x7fELF" or elf_bytes[4] != 1 or elf_bytes[5] != 1:
        raise ValueError("input must be little-endian ELF32")
    data = bytearray(elf_bytes)
    if replace_pubkey is not None:
        old_pk, new_pk = replace_pubkey
        idx = data.find(old_pk)
        if idx < 0 or data.find(old_pk, idx + 1) != -1:
            raise RuntimeError(f"expected unique RoT pubkey in bootloader ELF (idx={idx})")
        data[idx : idx + len(old_pk)] = new_pk

    (e_phoff,) = struct.unpack_from("<I", data, 28)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 42)
    if e_phentsize != 32:
        raise RuntimeError(f"unexpected ELF32 e_phentsize={e_phentsize}")

    new_phoff = len(data)
    all_ph = bytearray(data[e_phoff : e_phoff + e_phentsize * e_phnum])
    extra_blobs = bytearray()
    blobs_start = new_phoff + (e_phnum + len(segments)) * e_phentsize
    for addr, blob in segments:
        off = blobs_start + len(extra_blobs)
        extra_blobs += blob
        all_ph += struct.pack("<8I", 1, off, addr, addr, len(blob), len(blob), 5, 4)

    struct.pack_into("<I", data, 28, new_phoff)
    struct.pack_into("<H", data, 44, e_phnum + len(segments))
    return bytes(data) + bytes(all_ph) + bytes(extra_blobs)


def run_emulator_check(
    emu: str,
    bootloader_elf: Path,
    repo_root: Path,
    tries: int,
    key_path: Path | None,
) -> None:
    """Run the composed multi-boot WDT0 trial-reset + revert check in ra8_emulator."""
    body_a = make_wfi_payload(SLOT_A_MARKER, b"dfu-payload: Slot A active\r\n")
    body_b = make_wfi_payload(SLOT_B_MARKER, b"dfu-payload: Slot B trial hang\r\n")
    # Both slots use the same monotonic security version (img_version=7) while
    # advancing slot header seq (2 > 1), matching how an unconfirmed trial must
    # not irrevocably burn a higher anti-rollback OTP floor prior to confirm.
    if key_path is not None:
        slot_a = sign_with_pem(repo_root, key_path, body_a, 7)
        slot_b = sign_with_pem(repo_root, key_path, body_b, 7)
        pubkey_patch = None
    else:
        slot_a = sign_with_ephemeral_key(body_a, 7)
        slot_b = sign_with_ephemeral_key(body_b, 7)
        pubkey_patch = (read_prod_pubkey(repo_root), test_pubkey_sec1())

    segments = (
        build_slot_segments(SLOT_A_BASE, 1, slot_a)
        + build_slot_segments(SLOT_B_BASE, 2, slot_b)
        + [(BOOT_STATE_COPY0, make_boot_state(1, 1, tries, 0, 2))]
    )
    composite = patch_elf_with_slots(
        bootloader_elf.read_bytes(), segments, replace_pubkey=pubkey_patch
    )

    with tempfile.TemporaryDirectory() as td:
        elf_path = Path(td) / "trial_boot_composite.elf"
        elf_path.write_bytes(composite)
        cmd = [
            emu,
            str(elf_path),
            "--cpu",
            "zig",
            "--console",
            "--instructions",
            "120000000000",
            "--until",
            "dfu-payload: Slot A active",
            "--dump-mem",
            f"0x{MARKER_ADDR:08X}",
            "4",
            "--dump-mem",
            f"0x{BOOT_STATE_COPY0:08X}",
            "16",
        ]
        proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
        output = proc.stdout + proc.stderr
        sys.stdout.write(output)
        if proc.returncode != 0:
            raise RuntimeError(f"ra8_emulator exited with code {proc.returncode}")

        trial_lines = len(re.findall(r"dfu-bootloader: arming trial watchdog", output))
        slot_b_runs = len(re.findall(r"dfu-payload: Slot B trial hang", output))
        revert_lines = len(re.findall(r"dfu-bootloader: Slot A -> copy-to-run", output))
        slot_a_runs = len(re.findall(r"dfu-payload: Slot A active", output))
        resets_m = re.search(r"WDT0:.*?underflows=(\d+)", output)
        underflows = int(resets_m.group(1)) if resets_m else -1
        marker_ok = bool(
            re.search(
                rf"@{MARKER_ADDR:#010x}.*?\+0x0000\s+0x{SLOT_A_MARKER:08X}",
                output,
                re.I | re.S,
            )
        )

        if trial_lines != tries or slot_b_runs != tries:
            raise RuntimeError(
                f"expected {tries} trial arm(s) & Slot B run(s), saw arms={trial_lines} runs={slot_b_runs}"
            )
        if underflows != tries:
            raise RuntimeError(f"expected {tries} WDT0 underflow reset(s), saw {underflows}")
        if revert_lines != 1 or slot_a_runs < 1:
            raise RuntimeError(f"expected clean Slot A revert & run, saw revert={revert_lines} runs={slot_a_runs}")
        if not marker_ok:
            raise RuntimeError(f"expected reverted Slot A marker 0x{SLOT_A_MARKER:08X} at 0x{MARKER_ADDR:08X}")
        print(
            f"PASS: {tries} trial boot(s) armed WDT0, reset {underflows}x via WDTRF, "
            f"reverted cleanly to Slot A (0x{MARKER_ADDR:08X}=0x{SLOT_A_MARKER:08X})."
        )


def run_selftest(repo_root: Path) -> None:
    """Verify binary staging, boot-state CRC, ELF patching, and P-256 trailers offline."""
    pub = test_pubkey_sec1()
    assert len(pub) == 65 and pub[0] == 0x04
    body_a = make_wfi_payload(SLOT_A_MARKER, b"dfu-payload: Slot A active\r\n")
    assert len(body_a) % PAGE_SIZE == 0
    signed = sign_with_ephemeral_key(body_a, 7)
    assert len(signed) == len(body_a) + ROT_TRAILER_LEN
    segs = build_slot_segments(SLOT_A_BASE, 1, signed)
    assert segs[0][0] == SLOT_A_BASE and segs[1][0] == SLOT_A_BASE + HDR_OFFSET
    st = make_boot_state(1, 1, 3, 0, 2)
    assert len(st) == 32 and struct.unpack_from("<I", st, 0)[0] == BOOT_STATE_MAGIC
    assert struct.unpack_from("<I", st, 28)[0] == (zlib.crc32(st[:28]) & 0xFFFFFFFF)

    prod_pk = read_prod_pubkey(repo_root)
    assert len(prod_pk) == 65
    minimal_elf = bytearray(64 + 32)
    minimal_elf[0:52] = struct.pack(
        "<16sHHIIIIIHHHHHH",
        b"\x7fELF\x01\x01\x01" + b"\x00" * 9,
        2, 40, 1, 0x02000001, 64, 0, 0, 52, 32, 1, 0, 0, 0,
    )
    minimal_elf[64:96] = struct.pack("<8I", 1, 0, 0x02000000, 0x02000000, 96, 96, 5, 4)
    minimal_elf.extend(prod_pk)
    patched = patch_elf_with_slots(bytes(minimal_elf), segs, replace_pubkey=(prod_pk, pub))
    assert pub in patched and prod_pk not in patched
    e_phnum = struct.unpack_from("<H", patched, 44)[0]
    assert e_phnum == 3
    print("selftest PASS: payload, ROT1 trailer, boot-state CRC, and ELF patch checks passed.")


def _find_repo_root() -> Path:
    cur = Path(__file__).resolve().parent
    while cur != cur.parent:
        if (cur / "libs" / "ra8_rot" / "src" / "rot_abi.zig").is_file():
            return cur
        cur = cur.parent
    raise RuntimeError("repository root not found from script location")


def main() -> int:
    """Parse CLI arguments and run emulator verification or offline selftest."""
    repo_root = _find_repo_root()
    ap = argparse.ArgumentParser(description="Verify dfu_bootloader trial watchdog + revert.")
    ap.add_argument(
        "--mode",
        choices=("watchdog-reset", "revert", "selftest"),
        default="watchdog-reset",
        help="watchdog-reset=1 trial try + reset + revert; revert=3 tries + revert; selftest=offline",
    )
    ap.add_argument("--bootloader", type=Path, help="path to dfu_bootloader.elf")
    ap.add_argument(
        "--emu",
        default=shutil.which("ra8_emulator") or os.environ.get("RA8_EMULATOR", "ra8_emulator"),
        help="ra8_emulator binary path",
    )
    ap.add_argument("--key", type=Path, default=None, help="optional RoT private PEM key")
    args = ap.parse_args()

    if args.mode == "selftest":
        run_selftest(repo_root)
        return 0

    if args.bootloader is None or not args.bootloader.is_file():
        sys.stderr.write("error: --bootloader <dfu_bootloader.elf> is required\n")
        return 2

    tries = 1 if args.mode == "watchdog-reset" else 3
    run_emulator_check(args.emu, args.bootloader, repo_root, tries, args.key)
    return 0


if __name__ == "__main__":
    sys.exit(main())
