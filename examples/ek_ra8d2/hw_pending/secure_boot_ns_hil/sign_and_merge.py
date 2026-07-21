#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Sign the secure_boot_ns_hil Non-Secure image and merge it with the Secure hex.

The BLXNS root-of-trust proof (#172) needs the NS image SIGNED: the Secure verifier
reads the NS image's ``.ns_rot_header`` for the body length, locates the appended
``ra8_rot_trailer_t`` at ns_base + body_len, and checks the ECDSA-P256 signature
before BLXNS. This build step produces TWO flashable merged hexes from the Secure
hex + the NS image body:

  * ``<genuine>``  = Secure hex + signed NS image      (verify passes -> NS runs)
  * ``<tampered>`` = Secure hex + signed NS image with  (digest mismatch -> denied)
                     one body byte flipped after signing

Signing needs the held-out RoT private key (``tools/rot_sign.py``). This step
DEGRADES GRACEFULLY: if the key is not present it prints the exact command to run
later (with the key) and exits 0 -- the unsigned NS body is left for reference and
the build still succeeds. Run this script by hand (``--key <path>``) once the key
is available, or set ``RA8_ROT_KEY`` before the build.

It shells out to ``objcopy`` (bin<->ihex), ``tools/rot_sign.py`` (sign), and
``scripts/utils/merge_ihex.py`` (merge); no third-party Python packages.
"""

import argparse
import subprocess
import sys
from pathlib import Path

# The NS image is flashed at the Secure MRAM LMA (matches ns_image.ld NS_LOAD).
NS_LOAD_ADDR = 0x02080000
# Byte flipped to build the tampered case: the first .text byte, immediately
# after the 64-byte vector table + 8-byte .ns_rot_header. Definitely inside the
# signed body and NOT the header/vectors, so the header still parses but the body
# digest no longer matches -> the RoT gate default-denies.
TAMPER_OFFSET = 0x48
TAMPER_XOR = 0x01
IMG_VERSION = 1

# The .ns_rot_header the NS linker embeds (matches ra8_tz_secure_boot.h /
# ns_image.ld): "NSR1" magic + the signed body_len, at this byte offset.
NS_ROT_HEADER_OFFSET = 0x40
NS_ROT_HEADER_MAGIC = 0x3152534E  # "NSR1" little-endian


def _read_header_body_len(body: bytes) -> int:
    """Return the body_len the NS linker recorded in the .ns_rot_header.

    The header (magic + body_len) sits at NS_ROT_HEADER_OFFSET. This is the
    authoritative signed-body length the firmware verifier reads; the raw objcopy
    output can be SHORTER when a trailing empty section (e.g. an empty .data)
    contributes only alignment, so the caller pads up to this length before
    signing -- guaranteeing the trailer lands at ns_base + body_len.
    """
    off = NS_ROT_HEADER_OFFSET
    if len(body) < off + 8:
        _die = f"NS image too small ({len(body)} B) to hold the .ns_rot_header"
        raise SystemExit(_die)
    magic = int.from_bytes(body[off : off + 4], "little")
    if magic != NS_ROT_HEADER_MAGIC:
        _die = f"NS .ns_rot_header magic {magic:#010x} != {NS_ROT_HEADER_MAGIC:#010x} (NSR1)"
        raise SystemExit(_die)
    return int.from_bytes(body[off + 4 : off + 8], "little")


def _run(cmd: list[str]) -> None:
    """Run a subprocess, echoing the command; raise on failure."""
    sys.stdout.write("  $ " + " ".join(str(c) for c in cmd) + "\n")
    subprocess.run(cmd, check=True)  # noqa: S603 -- args are this build's own literals


def _bin_to_hex(objcopy: str, bin_path: Path, hex_path: Path) -> None:
    """Convert a raw binary at NS_LOAD_ADDR into an Intel HEX file."""
    _run(
        [
            objcopy,
            "-I",
            "binary",
            "-O",
            "ihex",
            f"--change-addresses={NS_LOAD_ADDR:#x}",
            str(bin_path),
            str(hex_path),
        ]
    )


def _sign(rot_sign: str, key: Path, body: Path, out: Path) -> None:
    """Append a signed ra8_rot_trailer_t to ``body`` -> ``out``."""
    _run(
        [
            sys.executable,
            rot_sign,
            "sign",
            "--key",
            str(key),
            "--image",
            str(body),
            "--out",
            str(out),
            "--img-version",
            str(IMG_VERSION),
        ]
    )


def _merge(merge_tool: str, secure_hex: Path, ns_hex: Path, out_hex: Path) -> None:
    """Merge the Secure hex and an NS hex into one flashable hex."""
    _run([sys.executable, merge_tool, str(secure_hex), str(ns_hex), str(out_hex)])


def _tampered_copy(signed: Path, out: Path) -> None:
    """Copy ``signed`` and flip one body byte so the RoT digest check fails."""
    data = bytearray(signed.read_bytes())
    data[TAMPER_OFFSET] ^= TAMPER_XOR
    out.write_bytes(data)


def _emit_manual_recipe(args: argparse.Namespace, ns_bin: Path) -> None:
    """Print the exact command to sign+merge later, when the key is available."""
    sys.stdout.write(
        "sign_and_merge: RoT signing key not found -- NS image left UNSIGNED.\n"
        f"  (looked for: {args.key or '<none given>'})\n"
        f"  Unsigned NS body: {ns_bin}\n"
        "  The genuine/tampered merged hexes were NOT produced. On the bench,\n"
        "  with the key present, run:\n\n"
        f"    python3 {Path(__file__).resolve()} \\\n"
        f"      --secure-hex {args.secure_hex} \\\n"
        f"      --ns-elf {args.ns_elf} \\\n"
        f"      --objcopy {args.objcopy} \\\n"
        f"      --rot-sign {args.rot_sign} \\\n"
        f"      --merge {args.merge} \\\n"
        f"      --out-genuine {args.out_genuine} \\\n"
        f"      --out-tampered {args.out_tampered} \\\n"
        "      --key $HOME/ra8d2-rot-signing-key.pem\n\n"
        "  Then flash --out-genuine (NS runs) and --out-tampered (NS denied).\n"
    )


def main() -> int:
    """Sign the NS image and emit the genuine and tampered merged hexes.

    Pipeline: extract the raw NS body from its ELF with objcopy, pad it to the
    `body_len` the linker recorded in `.ns_rot_header`, sign the padded body,
    convert to ihex at the LMA, and merge with the Secure hex. The tampered
    output is the same signed image with one body byte flipped AFTER signing, so
    it differs from the genuine one only in that the digest no longer matches.

    The padding step is not incidental. objcopy drops a trailing empty section's
    alignment tail, so the raw output can be a few bytes short of the linker's
    loaded-image extent. Signing the short body would put the trailer at the
    wrong address and hash different bytes than the firmware does -- verification
    would fail on a correctly-signed image. Padding to `body_len` makes the
    signed body exactly what the firmware hashes.

    Degrades deliberately when the RoT key is absent: it prints the exact
    command to run later with the key and returns 0, leaving the unsigned NS
    body for reference so the build still succeeds. **A 0 return therefore does
    not mean the images were produced.** Check for the output files, not the
    exit status.

    Returns:
        0 on success, and also 0 on the no-key degrade path.

    Raises:
        SystemExit: The NS body is larger than the header's `body_len`, or a
            subprocess (objcopy, rot_sign, merge_ihex) failed.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--secure-hex", required=True, help="Secure ELF's ihex")
    parser.add_argument("--ns-elf", required=True, help="Non-Secure ELF")
    parser.add_argument("--objcopy", required=True, help="arm-none-eabi-objcopy path")
    parser.add_argument("--rot-sign", required=True, help="tools/rot_sign.py path")
    parser.add_argument("--merge", required=True, help="scripts/utils/merge_ihex.py path")
    parser.add_argument("--out-genuine", required=True, help="output genuine merged hex")
    parser.add_argument("--out-tampered", required=True, help="output tampered merged hex")
    parser.add_argument("--key", default="", help="RoT private-key PEM (empty -> degrade)")
    args = parser.parse_args()

    out_dir = Path(args.out_genuine).parent
    ns_bin = out_dir / "secure_boot_ns_hil_ns.bin"

    # 1. Extract the raw NS body from its ELF (this is what rot_sign.py signs).
    _run([args.objcopy, "-O", "binary", str(args.ns_elf), str(ns_bin)])

    # Pad the raw body up to the body_len the linker recorded in .ns_rot_header.
    # objcopy drops a trailing empty section's alignment tail, so the raw output
    # can be a few bytes short of the linker's loaded-image extent; padding to
    # body_len makes the signed body match exactly what the firmware hashes and
    # puts the trailer at ns_base + body_len.
    raw = ns_bin.read_bytes()
    body_len = _read_header_body_len(raw)
    if len(raw) > body_len:
        _die = f"NS body {len(raw)} B exceeds header body_len {body_len} B"
        raise SystemExit(_die)
    body = out_dir / "secure_boot_ns_hil_ns_body.bin"
    body.write_bytes(raw + b"\x00" * (body_len - len(raw)))

    key = Path(args.key) if args.key else None
    if key is None or not key.is_file():
        _emit_manual_recipe(args, ns_bin)
        return 0

    # 2. Genuine: sign the (padded) NS body, convert to ihex at the LMA, merge.
    signed = out_dir / "secure_boot_ns_hil_ns_signed.bin"
    signed_hex = out_dir / "secure_boot_ns_hil_ns_signed.hex"
    _sign(args.rot_sign, key, body, signed)
    _bin_to_hex(args.objcopy, signed, signed_hex)
    _merge(args.merge, Path(args.secure_hex), signed_hex, Path(args.out_genuine))

    # 3. Tampered: flip one body byte after signing, convert, merge.
    tampered = out_dir / "secure_boot_ns_hil_ns_tampered.bin"
    tampered_hex = out_dir / "secure_boot_ns_hil_ns_tampered.hex"
    _tampered_copy(signed, tampered)
    _bin_to_hex(args.objcopy, tampered, tampered_hex)
    _merge(args.merge, Path(args.secure_hex), tampered_hex, Path(args.out_tampered))

    sys.stdout.write(
        "sign_and_merge: OK\n"
        f"  genuine  -> {args.out_genuine} (flash: NS liveness advances)\n"
        f"  tampered -> {args.out_tampered} "
        f"(body byte {TAMPER_OFFSET:#x} flipped: RoT denies, NS never runs)\n"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
