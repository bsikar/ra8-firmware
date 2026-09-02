#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Root-of-trust image signer + key ceremony for the RA8D2 secure-boot chain.

The firmware verifier ``ra8_rot_verify_image`` (libs/ra8_dfu/src/ra8_rot.c) accepts a
signed image of the form::

    [ body (body_len bytes) ] [ ra8_rot_trailer_t ]

and default-denies anything without a valid trailer.  Nothing in-tree could
*produce* that trailer, so ``RA8_ENABLE_ROOT_OF_TRUST`` could never be turned on.
This tool closes that gap.  It is a build/release-time host tool (never a CI
gate); it shells out to ``openssl`` so it needs no third-party Python packages.

The trailer is 116 bytes, little-endian, matching ra8_rot.h::ra8_rot_trailer_t::

    uint32 magic        = 0x524F5431  ("ROT1")
    uint32 version      = 1
    uint32 img_version                (monotonic anti-rollback counter)
    uint32 body_len                   (bytes the digest covers)
    uint32 sig_len      = 64
    uint8  digest[32]   = SHA-256(body)
    uint8  sig[64]      = ECDSA-P256 raw r||s over the digest

The public key is the 65-byte uncompressed P-256 point ``0x04 || X || Y`` that is
embedded in ra8_rot.c::s_rot_root_pubkey.

Commands::

    rot_sign.py keygen  --key priv.pem [--pubkey-c pubkey.h]
    rot_sign.py sign    --key priv.pem --image body.bin --out signed.bin
                        [--img-version N]
    rot_sign.py selftest

``selftest`` generates a throwaway key, signs a random body, then re-verifies the
signature with openssl and checks the trailer layout -- proving the produced
signature is valid ECDSA-P256 over SHA-256(body) with a 116-byte trailer.
"""

import argparse
import hashlib
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import NoReturn

# Mirrors libs/ra8_dfu/inc/ra8_rot.h.
ROT_MAGIC = 0x524F5431  # "ROT1"
ROT_VERSION = 1
SELFTEST_IMAGE_VERSION = 7
DIGEST_BYTES = 32
SIG_BYTES = 64
PUBKEY_BYTES = 65
BODY_MAX = 0x00100000  # 1 MiB signable-body cap (k_ra8_rot_body_max)
TRAILER_BYTES = 5 * 4 + DIGEST_BYTES + SIG_BYTES  # 116
INT_BYTES = 32  # P-256 field element / half-signature width

# ASN.1 DER / SEC1 tag bytes used when (de)serialising the openssl signature.
DER_SEQUENCE_TAG = 0x30
DER_INTEGER_TAG = 0x02
DER_HIGH_BIT = 0x80
EC_UNCOMPRESSED_TAG = 0x04

_OPENSSL = shutil.which("openssl")


def _fail(message: str) -> NoReturn:
    """Print ``message`` to stderr and exit non-zero (checkable, no traceback)."""
    sys.stderr.write(f"rot_sign.py: {message}\n")
    raise SystemExit(1)


def _openssl(args: list[str], stdin: bytes | None = None) -> bytes:
    """Run openssl with ``args``; return stdout bytes; exit on failure."""
    if _OPENSSL is None:
        _fail("openssl not found on PATH")
    # The arg list is built from this tool's own literals, not untrusted input.
    proc = subprocess.run(  # noqa: S603 -- resolved OpenSSL and fixed option vocabulary
        [_OPENSSL, *args], input=stdin, capture_output=True, check=False
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr.decode(errors="replace"))
        _fail(f"openssl {args[0]} failed (exit {proc.returncode})")
    return proc.stdout


def _der_int(buf: bytes, off: int) -> tuple[int, int]:
    """Parse one DER INTEGER at ``buf[off]``; return (value_offset, value_len)."""
    if buf[off] != DER_INTEGER_TAG:
        _fail("malformed DER signature (expected INTEGER)")
    length = buf[off + 1]
    return off + 2, length


def _der_sig_to_raw(der: bytes) -> bytes:
    """Convert a DER ``SEQUENCE{INTEGER r, INTEGER s}`` to 64-byte r||s."""
    if der[0] != DER_SEQUENCE_TAG:
        _fail("malformed DER signature (expected SEQUENCE)")
    # Skip the SEQUENCE tag + single-byte length (an ECDSA-P256 sig is < 128 B).
    r_off, r_len = _der_int(der, 2)
    s_off, s_len = _der_int(der, r_off + r_len)
    r = int.from_bytes(der[r_off : r_off + r_len], "big")
    s = int.from_bytes(der[s_off : s_off + s_len], "big")
    return r.to_bytes(INT_BYTES, "big") + s.to_bytes(INT_BYTES, "big")


def _raw_sig_to_der(r: int, s: int) -> bytes:
    """Encode two integers as a DER ``SEQUENCE{INTEGER r, INTEGER s}``."""

    def _int_enc(value: int) -> bytes:
        data = value.to_bytes(INT_BYTES, "big").lstrip(b"\x00") or b"\x00"
        if data[0] & DER_HIGH_BIT:  # keep the ASN.1 integer positive
            data = b"\x00" + data
        return bytes([DER_INTEGER_TAG, len(data)]) + data

    body = _int_enc(r) + _int_enc(s)
    return bytes([DER_SEQUENCE_TAG, len(body)]) + body


def keygen(key_path: Path) -> bytes:
    """Generate a P-256 private key at ``key_path``; return the 65-byte pubkey."""
    pem = _openssl(["ecparam", "-name", "prime256v1", "-genkey", "-noout"])
    key_path.write_bytes(pem)
    return public_key(key_path)


def public_key(key_path: Path) -> bytes:
    """Return the 65-byte uncompressed 0x04||X||Y public key for ``key_path``."""
    der = _openssl(["ec", "-in", str(key_path), "-pubout", "-outform", "DER"])
    # The SubjectPublicKeyInfo for P-256 ends in the 65-byte uncompressed point.
    point = der[-PUBKEY_BYTES:]
    if len(point) != PUBKEY_BYTES or point[0] != EC_UNCOMPRESSED_TAG:
        _fail("could not extract uncompressed P-256 public key")
    return point


def sign_body(key_path: Path, body: bytes, img_version: int) -> bytes:
    """Return ``body`` with an appended ra8_rot_trailer_t signed by ``key_path``."""
    if len(body) == 0 or len(body) > BODY_MAX:
        _fail(f"body length {len(body)} out of range (1..{BODY_MAX})")
    digest = hashlib.sha256(body).digest()
    # The signature authenticates SHA-256(img_version_le || body_digest), not the
    # bare body digest, so a forged img_version cannot ride on a validly-signed
    # body (T5-05). `openssl dgst -sha256 -sign` hashes its input, so signing the
    # concatenation yields ECDSA(SHA-256(img_version_le || digest)) -- exactly
    # what ra8_rot_verify_image recomputes via internal_bind_version. The trailer's
    # own `digest` field still stores SHA-256(body) for the tamper pre-check.
    to_sign = struct.pack("<I", img_version) + digest
    with tempfile.NamedTemporaryFile() as sign_file:
        sign_file.write(to_sign)
        sign_file.flush()
        der = _openssl(["dgst", "-sha256", "-sign", str(key_path), sign_file.name])
    sig = _der_sig_to_raw(der)
    header = struct.pack("<5I", ROT_MAGIC, ROT_VERSION, img_version, len(body), SIG_BYTES)
    trailer = header + digest + sig
    if len(trailer) != TRAILER_BYTES:
        _fail(f"trailer size {len(trailer)} != {TRAILER_BYTES}")
    return body + trailer


def _pubkey_c_header(pubkey: bytes) -> str:
    """Render the 65-byte pubkey as a C initialiser for s_rot_root_pubkey."""
    rows = [
        "    " + ", ".join(f"0x{b:02X}U" for b in pubkey[i : i + 8]) + ","
        for i in range(0, PUBKEY_BYTES, 8)
    ]
    body = "\n".join(rows)
    return (
        "/* Provisioned root public key -- paste into "
        "libs/ra8_dfu/src/ra8_rot.c::s_rot_root_pubkey. */\n"
        "static const uint8_t s_rot_root_pubkey[k_ra8_rot_pubkey_bytes] = {\n"
        f"{body}\n}};\n"
    )


def cmd_keygen(args: argparse.Namespace) -> int:
    """Run the `keygen` subcommand: mint a root keypair and emit its C header.

    This is the key ceremony. The private key written to `--key` is the root of
    the entire secure-boot chain: losing it means no future image can ever be
    signed for a board already provisioned with the matching public key, and
    leaking it means anyone can. It is written wherever the caller says, with no
    passphrase and no permission hardening -- protecting it is the operator's
    job, not this tool's.

    The public half is emitted as a C array for
    `libs/ra8_dfu/src/ra8_rot.c::s_rot_root_pubkey`. Without `--pubkey-c` it
    goes to stdout, so the ceremony can be eyeballed before anything is pasted.

    An existing `--key` path is overwritten without confirmation.

    Args:
        args: Parsed namespace with `key` and optional `pubkey_c`.

    Returns:
        0. Failures surface as exceptions from `keygen`, not a status code.
    """
    pubkey = keygen(Path(args.key))
    sys.stdout.write(f"wrote private key: {args.key}\n")
    if args.pubkey_c:
        Path(args.pubkey_c).write_text(_pubkey_c_header(pubkey), encoding="ascii")
        sys.stdout.write(f"wrote public-key C header: {args.pubkey_c}\n")
    else:
        sys.stdout.write(_pubkey_c_header(pubkey))
    return 0


def cmd_sign(args: argparse.Namespace) -> int:
    """Run the `sign` subcommand: append a signed trailer to an image body.

    Output is the input body followed by the 116-byte `ra8_rot_trailer_t`, so
    `--out` is always exactly 116 bytes longer than `--image`. Signing is not
    idempotent: feeding an already-signed image back in signs the body AND the
    old trailer, producing a double-trailered image the verifier rejects. Always
    sign the raw build artifact.

    `--img-version` is the monotonic anti-rollback counter, and the device
    refuses any image whose value is below what it has already accepted.
    Signing with a number lower than one already deployed produces a
    correctly-signed image that the target will still refuse -- and re-using a
    number is what silently permits a rollback.

    Args:
        args: Parsed namespace with `key`, `image`, `out` and `img_version`.

    Returns:
        0. Failures surface as exceptions from `sign_body`.
    """
    body = Path(args.image).read_bytes()
    signed = sign_body(Path(args.key), body, args.img_version)
    Path(args.out).write_bytes(signed)
    sys.stdout.write(f"signed {len(body)} body bytes -> {args.out} ({len(signed)} bytes)\n")
    return 0


def cmd_selftest(_args: argparse.Namespace) -> int:
    """Run the `selftest` subcommand: a throwaway-key sign/verify round trip.

    Proves this tool emits exactly the byte layout `ra8_rot_verify_image()`
    parses -- magic, version, field widths and offsets, digest placement, and
    that `img_version` survives the round trip. It checks the FORMAT contract,
    not the crypto: openssl owns ECDSA correctness.

    The keypair is generated into a temporary directory and destroyed with it,
    so this touches no provisioned key and is safe to run anywhere. It needs
    `openssl` on PATH and no hardware.

    Failures exit non-zero from `_require` rather than returning, so the caller
    never sees a false 0.

    Args:
        _args: Unused; the subcommand takes no options.

    Returns:
        0 when every check passes.
    """
    with tempfile.TemporaryDirectory() as tmp:
        key = Path(tmp) / "k.pem"
        pubkey = keygen(key)
        _require(pubkey[0] == EC_UNCOMPRESSED_TAG, "pubkey point-format")
        _require(len(pubkey) == PUBKEY_BYTES, "pubkey length")
        body = hashlib.sha256(b"rot-selftest").digest() * 4  # 128 arbitrary bytes
        signed = sign_body(key, body, img_version=SELFTEST_IMAGE_VERSION)
        _require(len(signed) == len(body) + TRAILER_BYTES, "signed image length")

        magic, ver, imgv, blen, slen = struct.unpack("<5I", signed[len(body) : len(body) + 20])
        _require(magic == ROT_MAGIC, "trailer magic")
        _require(ver == ROT_VERSION, "trailer version")
        _require(imgv == SELFTEST_IMAGE_VERSION, "img_version round-trip")
        _require(blen == len(body), "body_len")
        _require(slen == SIG_BYTES, "sig_len")
        digest = signed[len(body) + 20 : len(body) + 20 + DIGEST_BYTES]
        _require(digest == hashlib.sha256(body).digest(), "trailer digest == SHA-256(body)")

        # Re-verify the raw r||s signature against the public key via openssl:
        # rebuild the DER form and check ECDSA-P256 over the version-bound
        # material SHA-256(img_version_le || digest) -- the same bytes
        # ra8_rot_verify_image reconstructs. _openssl exits on a failed verify, so
        # reaching the print means the signature is valid over that material.
        signed_material_input = struct.pack("<I", imgv) + digest
        raw = signed[len(body) + 20 + DIGEST_BYTES :]
        der = _raw_sig_to_der(
            int.from_bytes(raw[:INT_BYTES], "big"), int.from_bytes(raw[INT_BYTES:], "big")
        )
        with tempfile.TemporaryDirectory() as vtmp:
            vdir = Path(vtmp)
            (vdir / "pub.pem").write_bytes(_openssl(["ec", "-in", str(key), "-pubout"]))
            (vdir / "body.bin").write_bytes(signed_material_input)
            (vdir / "sig.der").write_bytes(der)
            _openssl(
                [
                    "dgst",
                    "-sha256",
                    "-verify",
                    str(vdir / "pub.pem"),
                    "-signature",
                    str(vdir / "sig.der"),
                    str(vdir / "body.bin"),
                ]
            )
    sys.stdout.write("rot_sign.py selftest: PASS -- sign/verify round-trip + trailer layout OK.\n")
    return 0


def _require(cond: bool, what: str) -> None:
    """Exit non-zero unless ``cond`` holds (a checkable assert for the selftest)."""
    if not cond:
        _fail(f"selftest FAILED: {what}")


def main() -> int:
    """Parse the subcommand line and dispatch to the matching `cmd_*` handler.

    Three subcommands -- `keygen`, `sign`, `selftest` -- and one is required, so
    a bare invocation is an argparse error rather than a default action. That
    is deliberate for a tool whose default could otherwise overwrite a root key.

    Returns:
        The handler's status, for `sys.exit`.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)

    keygen_p = sub.add_parser("keygen", help="generate a P-256 root keypair")
    keygen_p.add_argument("--key", required=True, help="output private-key PEM path")
    keygen_p.add_argument("--pubkey-c", help="output C header with s_rot_root_pubkey")
    keygen_p.set_defaults(func=cmd_keygen)

    sign_p = sub.add_parser("sign", help="append a signed ra8_rot_trailer_t to an image")
    sign_p.add_argument("--key", required=True, help="private-key PEM from keygen")
    sign_p.add_argument("--image", required=True, help="raw image body .bin")
    sign_p.add_argument("--out", required=True, help="output signed image path")
    sign_p.add_argument("--img-version", type=int, default=1, help="anti-rollback version")
    sign_p.set_defaults(func=cmd_sign)

    selftest_p = sub.add_parser("selftest", help="sign+verify round-trip self-check")
    selftest_p.set_defaults(func=cmd_selftest)

    args = parser.parse_args()
    return int(args.func(args))


if __name__ == "__main__":
    sys.exit(main())
