#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Offline Ethos-U55 model build step for issue #227.

Two responsibilities, deliberately separated so the golden pipeline runs in CI
WITHOUT the heavy, optional Vela toolchain:

1. `compile` runs the pinned ethos-u-vela on a real quantized .tflite to
   produce a _vela.tflite, in which the "ethos-u" custom op wraps a Vela
   command stream. This one needs Vela, and SKIPS with a clear notice and exit
   0 when it is absent -- so a CI job that never installs Vela still passes.
   Read that exit 0 as "not run", never as "the model compiled".

2. `emit` / `check` turn a committed model DESCRIPTOR
   (tools/vela/models/*.json) into the lean, linkable ".npub" container defined
   by libs/ra8_hal/inc/ra8_npu_blob.h, baked as a C header the firmware links.
   `emit` writes the header; `check` regenerates it in memory and diffs it
   against the committed golden, failing on drift. Neither needs Vela, so this
   pair IS the regenerate-and-diff gate the issue asks for.

What the committed descriptor actually is matters for reading a green run: it
is a SIM model -- the tiny, documented "SE55" command-stream convention in
libs/ra8_hal/inc/ra8_npu_fake_cmd.h -- and NOT a real Vela program. It is the
only Ethos-U55 command stream this repo can produce deterministically without
a Vela install and without inventing NPU opcodes. The ra8_emulator NPU model and
the ra8_npu driver both decode that same convention, so the whole
submit -> run -> read-output path is exercised end to end, but a passing gate
says nothing about real Vela output. Distilling a REAL _vela.tflite into a
.npub is wired below as a documented follow-up (see `compile`), pending a
validated Vela output and an RA8P1 board.

Usage:
    python3 tools/vela/vela_gen.py emit  tools/vela/models/npu_addk_fake.json \
            -o tools/vela/generated/ra8_npu_model_addk_fake.h
    python3 tools/vela/vela_gen.py check tools/vela/models/npu_addk_fake.json \
            tools/vela/generated/ra8_npu_model_addk_fake.h
    python3 tools/vela/vela_gen.py compile model_int8.tflite -o build/vela
"""

from __future__ import annotations

import argparse
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# ---------------------------------------------------------------------------
# Container constants -- MUST match libs/ra8_hal/inc/ra8_npu_blob.h exactly.
# ---------------------------------------------------------------------------
BLOB_MAGIC = 0x3155504E  # "NPU1" little-endian
BLOB_VERSION = 1
HEADER_WORDS = 8
WORD_BYTES = 4
HEADER_BYTES = HEADER_WORDS * WORD_BYTES
REGION_WORDS = 4
REGION_DESC_BYTES = REGION_WORDS * WORD_BYTES
RFLAG_BAKED = 0x1
FNV_OFFSET = 0x811C9DC5
FNV_PRIME = 0x01000193
U32_MASK = 0xFFFFFFFF
BYTE_MASK = 0xFF

# Fake command-stream convention -- MUST match libs/ra8_hal/inc/ra8_npu_fake_cmd.h.
FAKE_MAGIC = 0x5E550000
FAKE_OP = {"copy": 0x0001, "addk": 0x0002}

ROLE = {
    "weights": 0,
    "scratch": 1,
    "input": 2,
    "output": 3,
    "other": 4,
}

# Ethos-U55 accelerator config Vela targets for the RA8P1 SKU.
ACCEL_ETHOS_U55_256 = 256

# BASEPn region base-pointer pairs the Ethos-U55 exposes (k_ra8_npu_region_count).
MAX_REGIONS = 8

# Pinned Vela accelerator argument (see tools/vela/README.md / requirements.txt).
VELA_ACCEL_CONFIG = "ethos-u55-256"


def _fnv1a(data: bytes) -> int:
    """FNV-1a 32-bit digest over `data` (matches ra8_npu_blob.h / the loader)."""
    digest = FNV_OFFSET
    for byte in data:
        digest = ((digest ^ byte) * FNV_PRIME) & U32_MASK
    return digest


def _seed_bytes(size: int, mul: int, add: int) -> bytes:
    """Deterministic byte pattern: out[i] = (i*mul + add) & 0xFF."""
    return bytes(((i * mul + add) & BYTE_MASK) for i in range(size))


def _region_payload(region: dict) -> bytes:
    """Build the baked bytes for one region per its `fill` rule."""
    size = int(region["size"])
    fill = region.get("fill", "zero")
    if fill == "seed":
        return _seed_bytes(size, int(region.get("mul", 1)), int(region.get("add", 0)))
    if fill == "const":
        return bytes([int(region.get("value", 0)) & BYTE_MASK]) * size
    return bytes(size)


def _build_command_stream(desc: dict) -> bytes:
    """Pack the SE55 stand-in command stream (5 little-endian 32-bit words)."""
    op_name = desc["op"]
    if op_name not in FAKE_OP:
        msg = f"unknown op '{op_name}' (expected one of {sorted(FAKE_OP)})"
        raise ValueError(msg)
    words = [
        FAKE_MAGIC | FAKE_OP[op_name],
        int(desc["src_region"]),
        int(desc["dst_region"]),
        int(desc["count"]),
        int(desc.get("addk", 0)),
    ]
    return b"".join(struct.pack("<I", w & U32_MASK) for w in words)


def build_blob(desc: dict) -> bytes:
    """Assemble a full .npub blob (header + region table + cmd + baked data)."""
    regions = desc["regions"]
    if len(regions) > MAX_REGIONS:
        msg = f"too many regions: {len(regions)} > {MAX_REGIONS} (k_ra8_npu_region_count)"
        raise ValueError(msg)
    cmd = _build_command_stream(desc)
    cmd_offset = HEADER_BYTES + (len(regions) * REGION_DESC_BYTES)

    # Baked region data blocks follow the command stream, in descriptor order.
    table = bytearray()
    baked = bytearray()
    data_cursor = cmd_offset + len(cmd)
    for region in regions:
        role = ROLE[region["role"]]
        size = int(region["size"])
        if region.get("mode", "runtime") == "baked":
            payload = _region_payload(region)
            if len(payload) != size:
                msg = "baked payload length must equal region size"
                raise ValueError(msg)
            table += struct.pack("<IIII", role, RFLAG_BAKED, size, data_cursor)
            baked += payload
            data_cursor += size
        else:
            table += struct.pack("<IIII", role, 0, size, 0)

    total = data_cursor
    payload = bytes(table) + cmd + bytes(baked)
    checksum = _fnv1a(payload)
    header = struct.pack(
        "<IIIIIIII",
        BLOB_MAGIC,
        BLOB_VERSION,
        total,
        len(regions),
        cmd_offset,
        len(cmd),
        int(desc.get("accel", ACCEL_ETHOS_U55_256)),
        checksum,
    )
    blob = header + payload
    if len(blob) != total:
        msg = f"blob length {len(blob)} != declared total {total}"
        raise ValueError(msg)
    return blob


def emit_header(desc: dict, blob: bytes) -> str:
    """Render `blob` as a self-contained, linkable C byte-array header."""
    symbol = desc["symbol"]
    name = desc["name"]
    # 16 bytes/row keeps each data line at 97 columns (2 indent + 16*"0xXX, "
    # minus the trailing space), inside the .clang-format ColumnLimit of 100, so
    # the emitted golden is already clang-format-22 clean and `make vela-check`
    # (generator output vs committed header) and the format gate never disagree.
    per_row = 16
    rows = []
    for start in range(0, len(blob), per_row):
        chunk = blob[start : start + per_row]
        rows.append("  " + "".join(f"0x{b:02X}, " for b in chunk).rstrip())
    body = "\n".join(rows)
    lines = [
        "/*",
        " * Copyright (c) 2026 Brighton Sikarskie",
        " * SPDX-License-Identifier: MIT",
        " *",
        f" * GENERATED by tools/vela/vela_gen.py from tools/vela/models/{name}.json.",
        " * DO NOT EDIT BY HAND. Regenerate with `make vela-regen`; the committed copy",
        " * is diffed against a fresh render by `make vela-check` (needs no Vela).",
        " *",
        " * A '.npub' Ethos-U55 model container (see libs/ra8_hal/inc/ra8_npu_blob.h):",
        " * an Ethos-U55 command stream plus its tensor region layout, baked as a byte",
        " * array the firmware links and the on-target loader (ra8_npu_loader.h) maps",
        " * into an ra8_npu_job_t. The command stream here is the documented SE55 stand-in",
        " * convention (ra8_npu_fake_cmd.h), NOT a real Vela program -- see vela_gen.py.",
        " */",
        "",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"/** @brief Raw bytes of the '{name}' .npub model container. */",
        f"static const uint8_t s_{symbol}_data[] = {{",
        body,
        "};",
        "",
        f"/** @brief Base pointer of the '{name}' .npub blob. */",
        f"static inline const uint8_t* {symbol}_blob(void)",
        "{",
        f"  return s_{symbol}_data;",
        "}",
        "",
        f"/** @brief Byte length of the '{name}' .npub blob. */",
        f"static inline uint32_t {symbol}_bytes(void)",
        "{",
        f"  return (uint32_t)sizeof(s_{symbol}_data);",
        "}",
        "",
    ]
    return "\n".join(lines)


def _load_desc(path: Path) -> dict:
    """Read and minimally validate a model descriptor JSON."""
    desc = json.loads(path.read_text(encoding="ascii"))
    for key in ("name", "symbol", "op", "regions", "src_region", "dst_region", "count"):
        if key not in desc:
            msg = f"{path}: descriptor missing required key '{key}'"
            raise ValueError(msg)
    return desc


def cmd_emit(args: argparse.Namespace) -> int:
    """emit: descriptor -> committed C header."""
    desc = _load_desc(Path(args.descriptor))
    header = emit_header(desc, build_blob(desc))
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(header, encoding="ascii")
    print(f"vela_gen: wrote {out} ({len(header)} bytes)")
    return 0


def cmd_check(args: argparse.Namespace) -> int:
    """check: regenerate the header and diff it against the committed copy."""
    desc = _load_desc(Path(args.descriptor))
    fresh = emit_header(desc, build_blob(desc))
    golden = Path(args.header)
    if not golden.is_file():
        print(f"vela_gen: MISSING golden {golden} (run `make vela-regen`)", file=sys.stderr)
        return 1
    current = golden.read_text(encoding="ascii")
    if current != fresh:
        print(
            f"vela_gen: DRIFT -- {golden} is stale vs {args.descriptor}.\n"
            "          Run `make vela-regen` and commit the result.",
            file=sys.stderr,
        )
        return 1
    print(f"vela_gen: {golden} is up to date")
    return 0


def cmd_compile(args: argparse.Namespace) -> int:
    """compile: run the pinned Vela on a real .tflite (optional; skip if absent)."""
    vela = shutil.which("vela")
    if vela is None:
        print(
            "vela_gen: ethos-u-vela is not installed -- skipping compile "
            "(this is fine; the .npub golden pipeline needs no Vela).\n"
            "          Install it with: pip install -r tools/vela/requirements.txt",
        )
        return 0
    tflite = Path(args.tflite)
    if not tflite.is_file():
        print(f"vela_gen: no such .tflite: {tflite}", file=sys.stderr)
        return 1
    out_dir = Path(args.output)
    out_dir.mkdir(parents=True, exist_ok=True)
    argv = [
        vela,
        "--accelerator-config",
        VELA_ACCEL_CONFIG,
        "--output-dir",
        str(out_dir),
        str(tflite),
    ]
    print(f"vela_gen: running {' '.join(argv)}")
    proc = subprocess.run(argv, check=False)  # noqa: S603 -- resolved path, fixed argv
    if proc.returncode != 0:
        return proc.returncode
    produced = out_dir / f"{tflite.stem}_vela.tflite"
    print(f"vela_gen: Vela wrote {produced}")
    # TODO(#227 follow-up): distilling this real _vela.tflite (the ethos-u custom
    # op command stream + region layout) into a .npub requires a validated Vela
    # output and an RA8P1 board to confirm the extracted stream runs on silicon.
    # Until then the committed golden uses the SE55 stand-in descriptor via `emit`.
    print(
        "vela_gen: NOTE -- extracting the command stream from _vela.tflite into a "
        ".npub is a documented follow-up; use `emit` on a model descriptor for now.",
    )
    return 0


def main(argv: list[str]) -> int:
    """Parse the subcommand line and dispatch to the matching `cmd_*` handler.

    A subcommand is required, so a bare invocation is an argparse usage error
    rather than a default action.

    Note the asymmetry in what a 0 means across the three: `emit` and `check`
    return 0 only when they really ran, but `compile` also returns 0 when Vela
    is not installed and nothing was compiled. See the module docstring.

    Args:
        argv: Argument list WITHOUT the program name (callers pass
            `sys.argv[1:]`).

    Returns:
        The handler's status, for `sys.exit`.
    """
    parser = argparse.ArgumentParser(description="Offline Ethos-U55 model build step (#227).")
    sub = parser.add_subparsers(dest="command", required=True)

    p_emit = sub.add_parser("emit", help="descriptor -> committed C header")
    p_emit.add_argument("descriptor", help="model descriptor JSON")
    p_emit.add_argument("-o", "--output", required=True, help="output C header path")
    p_emit.set_defaults(func=cmd_emit)

    p_check = sub.add_parser("check", help="regenerate and diff against the golden header")
    p_check.add_argument("descriptor", help="model descriptor JSON")
    p_check.add_argument("header", help="committed golden C header to diff")
    p_check.set_defaults(func=cmd_check)

    p_compile = sub.add_parser("compile", help="run the pinned Vela on a .tflite (optional)")
    p_compile.add_argument("tflite", help="quantized INT8/INT16 .tflite model")
    p_compile.add_argument("-o", "--output", default="build/vela", help="Vela output dir")
    p_compile.set_defaults(func=cmd_compile)

    args = parser.parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
