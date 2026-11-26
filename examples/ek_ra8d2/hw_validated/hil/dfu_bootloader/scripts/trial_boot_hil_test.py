# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
"""Physical EK-RA8D2 HIL bench verification for dfu_bootloader trial-boot revert.

Stages confirmed Slot A (seq=1) + unconfirmed WFI Slot B (seq=2) +
``boot_state`` (tries_left=3, confirmed=0) into a stripped Code-MRAM-only
Intel HEX image (``0x0200_0000..0x020F_FFFF``), validates it through
``preflash_guard.sh``, flashes the EK-RA8D2 bench over J-Link inside a held
``bench.sh`` reservation, captures SCI8 UART across the 4-boot exhaustion and
Slot A revert sequence, and verifies ``0x2201_0000 == 0x9710C0DE`` plus the
exhausted ``boot_state`` copy via J-Link ``mem32``.

Run:
    python3 examples/ek_ra8d2/hw_validated/hil/dfu_bootloader/scripts/trial_boot_hil_test.py
        --mode selftest
    python3 examples/ek_ra8d2/hw_validated/hil/dfu_bootloader/scripts/trial_boot_hil_test.py
        --mode hil --bootloader build-dfu_bootloader/dfu_bootloader.elf
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))

import trial_boot_emu_test as emu  # noqa: E402 -- sibling script in same directory

MRAM_BASE = 0x0200_0000
MRAM_END = 0x0210_0000
OTP_AR_COUNTER_ADDR = 0x02E0_7600
BOOT_STATE_COPY1 = emu.BOOT_STATE_COPY0 + 0x20
DEFAULT_IMG_VERSION = 7
DEFAULT_TRIES = 3
DEFAULT_UART = "/dev/serial/by-id/usb-SEGGER_J-Link_001086567198-if00"
DEFAULT_JLINK_SN = "1086567198"
DEFAULT_JLINK_DEVICE = "R7KA8D2KF_CPU0"
DEFAULT_HIL_ROOT = Path("/home/bsikar/ra8ci")
_HEX_REC_DATA = 0
_HEX_REC_EXT_LINEAR = 4
_HEX_REC_MAX_BYTES = 16
_HEX_PAGE_SPAN = 0x10000
_MIN_TRAILING_RECORDS = 2


def _find_repo_root() -> Path:
    """Walk parents until the repository CMakeLists.txt marker is found."""
    cur = Path(__file__).resolve()
    for parent in cur.parents:
        if (parent / "CMakeLists.txt").is_file() and (parent / "libs" / "ra8_rot").is_dir():
            return parent
    msg = "cannot locate repository root"
    raise RuntimeError(msg)


def _ihex_line(payload: bytes) -> str:
    """Format one raw Intel HEX record payload with trailing twos-complement checksum."""
    csum = (-sum(payload)) & 0xFF
    return f":{payload.hex().upper()}{csum:02X}"


def segments_to_ihex(addr: int, data: bytes) -> list[str]:
    """Encode a contiguous byte blob into 16-byte Intel HEX records at ``addr``."""
    out: list[str] = []
    cur_ext: int | None = None
    off = 0
    while off < len(data):
        cur_addr = addr + off
        ext = (cur_addr >> 16) & 0xFFFF
        if ext != cur_ext:
            cur_ext = ext
            rec = bytes([2, 0, 0, _HEX_REC_EXT_LINEAR, (ext >> 8) & 0xFF, ext & 0xFF])
            out.append(_ihex_line(rec))
        page_rem = _HEX_PAGE_SPAN - (cur_addr & 0xFFFF)
        chunk_len = min(_HEX_REC_MAX_BYTES, len(data) - off, page_rem)
        chunk = data[off : off + chunk_len]
        hdr = bytes([chunk_len, (cur_addr >> 8) & 0xFF, cur_addr & 0xFF, _HEX_REC_DATA])
        out.append(_ihex_line(hdr + chunk))
        off += chunk_len
    return out


def parse_ihex_ranges(hex_text: str) -> list[tuple[int, int]]:
    """Return merged ``[start, end)`` byte intervals covered by Intel HEX data records."""
    ext = 0
    ranges: list[list[int]] = []
    for raw in hex_text.splitlines():
        line = raw.strip()
        if not line.startswith(":"):
            continue
        rec = bytes.fromhex(line[1:])
        rec_len, offset, rec_type = rec[0], (rec[1] << 8) | rec[2], rec[3]
        if rec_type == _HEX_REC_EXT_LINEAR:
            ext = ((rec[4] << 8) | rec[5]) << 16
        elif rec_type == _HEX_REC_DATA and rec_len > 0:
            start = ext + offset
            end = start + rec_len
            if ranges and ranges[-1][1] == start:
                ranges[-1][1] = end
            else:
                ranges.append([start, end])
    return [(s, e) for s, e in ranges]


def validate_mram_only_hex(hex_text: str) -> list[tuple[int, int]]:
    """Reject empty images or records outside Code MRAM (``0x0200_0000..0x0210_0000``)."""
    ranges = parse_ihex_ranges(hex_text)
    if not ranges:
        msg = "composite HEX contains no data records"
        raise ValueError(msg)
    for start, end in ranges:
        if start < MRAM_BASE or end > MRAM_END:
            msg = f"HEX range 0x{start:08X}..0x{end:08X} escapes Code MRAM"
            raise ValueError(msg)
    return ranges


def build_composite_hex(
    bootloader_elf: Path,
    repo_root: Path,
    out_hex: Path,
    *,
    img_version: int = DEFAULT_IMG_VERSION,
    key_path: Path | None = None,
) -> list[tuple[int, int]]:
    """Assemble an option-setting-free composite Intel HEX for bench execution."""
    body_a = emu.make_wfi_payload(emu.SLOT_A_MARKER, b"dfu-payload: Slot A active\r\n")
    body_b = emu.make_wfi_payload(emu.SLOT_B_MARKER, b"dfu-payload: Slot B trial hang\r\n")

    elf_bytes = bytearray(bootloader_elf.read_bytes())
    if key_path is not None:
        slot_a = emu.sign_with_pem(repo_root, key_path, body_a, img_version)
        slot_b = emu.sign_with_pem(repo_root, key_path, body_b, img_version)
    else:
        old_pk = emu.read_prod_pubkey(repo_root)
        new_pk = emu.test_pubkey_sec1()
        idx = elf_bytes.find(old_pk)
        if idx < 0 or elf_bytes.find(old_pk, idx + 1) != -1:
            msg = f"expected unique RoT pubkey in {bootloader_elf} (idx={idx})"
            raise RuntimeError(msg)
        elf_bytes[idx : idx + len(old_pk)] = new_pk
        slot_a = emu.sign_with_ephemeral_key(body_a, img_version)
        slot_b = emu.sign_with_ephemeral_key(body_b, img_version)

    with tempfile.TemporaryDirectory() as td:
        patched_elf = Path(td) / "bootloader_patched.elf"
        base_hex = Path(td) / "bootloader_stripped.hex"
        patched_elf.write_bytes(bytes(elf_bytes))
        objcopy = shutil.which("arm-none-eabi-objcopy") or "arm-none-eabi-objcopy"
        subprocess.run(  # noqa: S603 -- fixed argv invoking toolchain objcopy
            [
                objcopy,
                "-O",
                "ihex",
                "--remove-section=.option_setting*",
                "--remove-section=.flash_gap*",
                str(patched_elf),
                str(base_hex),
            ],
            check=True,
        )
        raw_lines = base_hex.read_text(encoding="ascii").splitlines()
        lines = [ln.strip() for ln in raw_lines if ln.strip()]

    tail: list[str] = [
        ln
        for ln in lines[-_MIN_TRAILING_RECORDS:]
        if ln.startswith(":04000005") or ln == ":00000001FF"
    ]
    body_lines = [ln for ln in lines if ln not in tail]
    segments = (
        emu.build_slot_segments(emu.SLOT_A_BASE, 1, slot_a)
        + emu.build_slot_segments(emu.SLOT_B_BASE, 2, slot_b)
        + [(emu.BOOT_STATE_COPY0, emu.make_boot_state(1, 1, DEFAULT_TRIES, 0, 2))]
    )
    for seg_addr, seg_bytes in sorted(segments, key=lambda kv: kv[0]):
        body_lines.extend(segments_to_ihex(seg_addr, seg_bytes))
    body_lines.extend(tail or [":00000001FF"])

    hex_text = "\n".join(body_lines) + "\n"
    ranges = validate_mram_only_hex(hex_text)
    out_hex.write_text(hex_text, encoding="ascii")
    return ranges


def verify_hil_transcript(
    uart_text: str,
    jlink_text: str,
    *,
    tries: int = DEFAULT_TRIES,
) -> None:
    """Assert trial boot count, Slot A revert banner, SRAM marker, and exhausted copy1."""
    arms = len(re.findall(r"dfu-bootloader: arming trial watchdog", uart_text))
    trial_boots = len(re.findall(r"dfu-bootloader: Slot B \(trial\) -> copy-to-run", uart_text))
    reverts = len(re.findall(r"dfu-bootloader: Slot A -> copy-to-run", uart_text))
    slot_a_runs = len(re.findall(r"dfu-payload: Slot A active", uart_text))
    if arms != tries or trial_boots != tries:
        msg = f"expected {tries} Slot B trial boot(s), saw boots={trial_boots} arms={arms}"
        raise RuntimeError(msg)
    if reverts != 1 or slot_a_runs < 1:
        msg = f"expected 1 Slot A revert and run banner, saw revert={reverts} runs={slot_a_runs}"
        raise RuntimeError(msg)

    marker_pat = rf"{emu.MARKER_ADDR:08X}\s*=\s*{emu.SLOT_A_MARKER:08X}"
    if not re.search(marker_pat, jlink_text, re.IGNORECASE):
        msg = f"missing Slot A SRAM marker {emu.SLOT_A_MARKER:08X} at {emu.MARKER_ADDR:08X}"
        raise RuntimeError(msg)

    copy1_pat = rf"{BOOT_STATE_COPY1:08X}\s*=\s*52413842\s+{tries + 1:08X}\s+00000001\s+00000002"
    if not re.search(copy1_pat, jlink_text, re.IGNORECASE):
        msg = (
            f"expected exhausted boot_state copy1 (tries=0, confirmed=0) at {BOOT_STATE_COPY1:08X}"
        )
        raise RuntimeError(msg)


def _star_runner_source(args: argparse.Namespace, hex_text: str) -> str:
    """Generate a self-contained Python runner using a clean TemporaryDirectory on star."""
    return (
        "import os, select, subprocess, tempfile, threading, time\n"
        "from pathlib import Path\n"
        f"PORT = {args.uart!r}\n"
        f"HEX_TEXT = {hex_text!r}\n"
        f"JLINK = ['JLinkExe', '-USB', {args.jlink_sn!r}, '-Device', {args.jlink_device!r}, "
        "'-If', 'SWD', '-Speed', '1000', '-AutoConnect', '1', '-ExitOnError', '1']\n"
        "subprocess.run(['stty', '-F', PORT, '115200', 'raw', '-echo'], check=True)\n"
        "with tempfile.TemporaryDirectory() as td:\n"
        "    tdir = Path(td)\n"
        "    hex_file = tdir / 'composite.hex'\n"
        "    run_cmd = tdir / 'run.jlink'\n"
        "    post_cmd = tdir / 'post.jlink'\n"
        "    hex_file.write_text(HEX_TEXT, encoding='ascii')\n"
        "    run_cmd.write_text(f'r\\nerase\\nloadfile {hex_file}\\nr\\ng\\nq\\n')\n"
        "    post_cmd.write_text(\n"
        f"        'h\\nmem32 0x{emu.MARKER_ADDR:08X} 4\\n'\n"
        f"        'mem32 0x{emu.BOOT_STATE_COPY0:08X} 16\\n'\n"
        f"        'mem32 0x{OTP_AR_COUNTER_ADDR:08X} 4\\nq\\n')\n"
        "    fd = open(PORT, 'rb', buffering=0)\n"
        "    chunks = []\n"
        "    stop_ev = threading.Event()\n"
        "    def reader():\n"
        "        while not stop_ev.is_set():\n"
        "            r, _, _ = select.select([fd], [], [], 0.1)\n"
        "            if r:\n"
        "                b = os.read(fd.fileno(), 4096)\n"
        "                if b:\n"
        "                    chunks.append(b)\n"
        "    t = threading.Thread(target=reader, daemon=True)\n"
        "    t.start()\n"
        "    time.sleep(0.2)\n"
        "    chunks.clear()\n"
        "    flash = subprocess.run(JLINK + ['-CommandFile', str(run_cmd)], "
        "capture_output=True, text=True, check=False)\n"
        "    print(flash.stdout)\n"
        "    if flash.returncode != 0:\n"
        "        stop_ev.set()\n"
        "        t.join(timeout=1.0)\n"
        "        fd.close()\n"
        "        raise SystemExit(flash.returncode)\n"
        f"    deadline = time.monotonic() + {float(args.timeout)}\n"
        "    while time.monotonic() < deadline:\n"
        "        joined = b''.join(chunks)\n"
        "        if b'dfu-payload: Slot A active' in joined:\n"
        "            time.sleep(0.5)\n"
        "            break\n"
        "        if b'awaiting DFU_DNLOAD' in joined:\n"
        "            break\n"
        "        time.sleep(0.2)\n"
        "    stop_ev.set()\n"
        "    t.join(timeout=1.0)\n"
        "    fd.close()\n"
        "    post = subprocess.run(JLINK + ['-CommandFile', str(post_cmd)], "
        "capture_output=True, text=True, check=True)\n"
        "    print('===HIL_JLINK_BEGIN===')\n"
        "    print(post.stdout)\n"
        "    print('===HIL_JLINK_END===')\n"
        "    print('===HIL_UART_BEGIN===')\n"
        "    print(b''.join(chunks).decode('utf-8', errors='replace'))\n"
        "    print('===HIL_UART_END===')\n"
    )


def _run_preflash_guard(hil_root: Path, hex_path: Path) -> None:
    """Enforce ``preflash_guard.sh`` on the generated composite HEX before touching hardware."""
    guard_sh = hil_root / "scripts" / "hil" / "lib" / "preflash_guard.sh"
    if not guard_sh.is_file():
        return
    quoted_root = shlex.quote(str(hil_root))
    quoted_hex = shlex.quote(str(hex_path))
    snippet = f"cd {quoted_root} && source {shlex.quote(str(guard_sh))} && "
    snippet += f"ra8_preflash_guard {quoted_hex}"
    subprocess.run(["/bin/bash", "-p", "-c", snippet], check=True)  # noqa: S603 -- fixed argv running repo guard script


def _extract_block(text: str, begin_marker: str, end_marker: str) -> str:
    """Extract captured text between two delimiter markers."""
    pat = re.compile(rf"{re.escape(begin_marker)}\r?\n(.*?){re.escape(end_marker)}", re.DOTALL)
    m = pat.search(text)
    if not m:
        msg = f"missing capture delimiter {begin_marker}"
        raise RuntimeError(msg)
    return m.group(1)


def run_hil_bench(args: argparse.Namespace, repo_root: Path) -> None:
    """Build composite HEX, guard against OTP writes, run on EK-RA8D2, and assert output."""
    with tempfile.TemporaryDirectory() as td:
        hex_path = Path(td) / "trial_boot_composite_hil.hex"
        ranges = build_composite_hex(
            args.bootloader,
            repo_root,
            hex_path,
            img_version=args.img_version,
            key_path=args.key,
        )
        for start, end in ranges:
            print(f"[hil] staged MRAM range 0x{start:08X}..0x{end:08X} ({end - start} B)")
        _run_preflash_guard(args.hil_root, hex_path)

        hex_text = hex_path.read_text(encoding="ascii")
        runner_src = _star_runner_source(args, hex_text)
        ssh_bin = shutil.which("ssh") or "ssh"
        remote_dir = subprocess.run(  # noqa: S603 -- fixed ssh mktemp
            [ssh_bin, "-n", args.star_host, "mktemp -d"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
        qdir = shlex.quote(remote_dir)
        try:
            subprocess.run(  # noqa: S603 -- upload self-contained runner to secure temp dir
                [ssh_bin, args.star_host, f"cat > {qdir}/runner.py"],
                input=runner_src,
                text=True,
                check=True,
            )
            star_cmd = [ssh_bin, "-n", args.star_host, f"python3 {qdir}/runner.py"]
            bench_sh = args.hil_root / "scripts" / "hil" / "bench.sh"
            if bench_sh.is_file() and not os.environ.get("RA8_BENCH_HELD"):
                cmd = [
                    "/bin/bash",
                    "-p",
                    str(bench_sh),
                    "run",
                    "--intent",
                    "dfu_bootloader trial-boot revert HIL",
                    "--for",
                    "5m",
                    "--",
                    *star_cmd,
                ]
            else:
                cmd = star_cmd

            proc = subprocess.run(cmd, capture_output=True, text=True, check=False)  # noqa: S603 -- fixed bench command
            combined = proc.stdout + proc.stderr
            sys.stdout.write(combined)
            if proc.returncode != 0:
                msg = f"HIL bench runner exited with code {proc.returncode}"
                raise RuntimeError(msg)
        finally:
            subprocess.run(  # noqa: S603 -- clean remote temporary dir
                [ssh_bin, "-n", args.star_host, f"rm -rf {qdir}"],
                check=False,
            )
        combined = proc.stdout + proc.stderr
        sys.stdout.write(combined)
        if proc.returncode != 0:
            msg = f"HIL bench runner exited with code {proc.returncode}"
            raise RuntimeError(msg)

    uart_text = _extract_block(combined, "===HIL_UART_BEGIN===", "===HIL_UART_END===")
    jlink_text = _extract_block(combined, "===HIL_JLINK_BEGIN===", "===HIL_JLINK_END===")
    verify_hil_transcript(uart_text, jlink_text, tries=DEFAULT_TRIES)
    print(
        f"HIL PASS: {DEFAULT_TRIES}x Slot B trial reset -> Slot A revert "
        f"(0x{emu.MARKER_ADDR:08X}=0x{emu.SLOT_A_MARKER:08X})."
    )


def run_selftest() -> None:
    """Run offline checks for Intel HEX generation, MRAM bounds guard, and log parser."""
    hex_lines = (
        segments_to_ihex(0x0200_0000, b"\x01\x02\x03\x04")
        + segments_to_ihex(0x020F_DFE0, b"\xaa" * 64)
        + [":00000001FF"]
    )
    ranges = validate_mram_only_hex("\n".join(hex_lines) + "\n")
    expected_ranges = [(0x0200_0000, 0x0200_0004), (0x020F_DFE0, 0x020F_E020)]
    if ranges != expected_ranges:
        msg = f"unexpected parsed ranges {ranges!r}"
        raise RuntimeError(msg)

    bad_records = segments_to_ihex(OTP_AR_COUNTER_ADDR, b"\x07\x00\x00\x00")
    bad_hex = "\n".join([*bad_records, ":00000001FF"])
    try:
        validate_mram_only_hex(bad_hex)
    except ValueError:
        pass
    else:
        msg = "validate_mram_only_hex failed to reject OTP Extra-MRAM write"
        raise RuntimeError(msg)

    sample_uart = "\r\n".join(
        [
            "ra8d2 dfu-bootloader (immutable, 128K @ 0x02000000)",
            "dfu-bootloader: Slot B (trial) -> copy-to-run @0x22020000",
            "dfu-bootloader: arming trial watchdog",
        ]
        * 3
        + [
            "ra8d2 dfu-bootloader (immutable, 128K @ 0x02000000)",
            "dfu-bootloader: Slot A -> copy-to-run @0x22020000",
            "dfu-payload: Slot A active",
        ]
    )
    sample_jlink = (
        "22010000 = 9710C0DE 00000000 00000000 00000000\n"
        "020FE000 = 52413842 00000003 00000101 00000002\n"
        "020FE020 = 52413842 00000004 00000001 00000002\n"
    )
    verify_hil_transcript(sample_uart, sample_jlink, tries=DEFAULT_TRIES)
    print("selftest PASS: Intel HEX encoding, MRAM bounds guard, and HIL log verifier passed.")


def main() -> int:
    """Parse CLI arguments and run offline selftest or EK-RA8D2 bench verification."""
    repo_root = _find_repo_root()
    ap = argparse.ArgumentParser(description="Verify dfu_bootloader trial revert on EK-RA8D2 HIL.")
    ap.add_argument("--mode", choices=("selftest", "hil"), default="selftest")
    ap.add_argument("--bootloader", type=Path, help="path to dfu_bootloader.elf")
    ap.add_argument("--key", type=Path, default=None, help="optional RoT private PEM key")
    ap.add_argument("--img-version", type=int, default=DEFAULT_IMG_VERSION)
    ap.add_argument("--hil-root", type=Path, default=DEFAULT_HIL_ROOT)
    ap.add_argument("--star-host", default="star")
    ap.add_argument("--uart", default=DEFAULT_UART)
    ap.add_argument("--jlink-sn", default=DEFAULT_JLINK_SN)
    ap.add_argument("--jlink-device", default=DEFAULT_JLINK_DEVICE)
    ap.add_argument("--timeout", type=int, default=30)
    args = ap.parse_args()

    if args.mode == "selftest":
        run_selftest()
        return 0

    if args.bootloader is None or not args.bootloader.is_file():
        sys.stderr.write("error: --bootloader <dfu_bootloader.elf> is required for --mode hil\n")
        return 2

    run_hil_bench(args, repo_root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
