#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Offline Ethos-U55 model build step for issue #227.

Two responsibilities, deliberately separated so the golden pipeline runs in CI
WITHOUT the heavy, optional Vela toolchain:

1. `compile` runs the pinned ethos-u-vela on a real quantized .tflite to
   produce a _vela.tflite, in which the "ethos-u" custom op wraps a Vela
   command stream. An explicit compile request fails if the locked Vela tool is
   absent or does not produce the expected output; `just setup` installs it.

2. `distill` turns a REAL `_vela.tflite` into the same container: it reads the
   `ethos-u` custom operator, takes its command stream straight out of the
   model's constant buffer, and records the BASEPn region layout the on-target
   kernel programs. It needs the Vela package installed (it reuses Vela's own
   vendored TFLite schema bindings to read the flatbuffer), so it is an opt-in
   step, not part of the ordinary Vela-free gate.

3. `emit` / `check` turn a committed model DESCRIPTOR
   (tools/vela/models/*.json) into the lean, linkable ".npub" container defined
   by libs/ra8_hal/inc/ra8_npu_blob.h, baked as a C header the firmware links.
   `emit` writes the header; `check` regenerates it in memory and diffs it
   against the committed golden, failing on drift. Neither needs Vela, so this
   pair IS the regenerate-and-diff gate the issue asks for.

Two kinds of command stream live in the tree, and reading a green run means
knowing which one a golden holds. The descriptor path (`emit` / `check`) builds
a SIM model: the tiny, documented "SE55" convention in
libs/ra8_hal/inc/ra8_npu_fake_cmd.h, which the ra8_emulator NPU model and the
ra8_npu driver both decode, so the submit -> run -> read-output path is
exercised end to end without a Vela install. It is NOT a real Vela program, and
a passing `check` says nothing about real Vela output. The distill path
(`distill`) carries a REAL Ethos-U55 command stream, produced by the pinned Vela
from a real quantized .tflite and copied byte for byte out of the compiled
model. Nothing in this repo interprets those opcodes; running one is what still
needs an RA8P1 board.

Usage:
    python3 tools/vela/src/vela_gen.py emit  tools/vela/models/npu_addk_fake.json \
            -o tools/vela/generated/ra8_npu_model_addk_fake.h
    python3 tools/vela/src/vela_gen.py check tools/vela/models/npu_addk_fake.json \
            tools/vela/generated/ra8_npu_model_addk_fake.h
    python3 tools/vela/src/vela_gen.py compile model_int8.tflite -o build/vela
    python3 tools/vela/src/vela_gen.py distill build/vela/conv_int8_vela.tflite \
            --symbol ra8_npu_model_conv_int8_vela \
            -o tools/vela/generated/ra8_npu_model_conv_int8_vela.h
"""

from __future__ import annotations

import argparse
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]

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

RFLAG_ALIAS = 0x2

# Ethos-U55 accelerator config Vela targets for the RA8P1 SKU.
ACCEL_ETHOS_U55_256 = 256

# Vela's own markers inside a compiled _vela.tflite (see `distill`).
VELA_CUSTOM_CODE = b"ethos-u"
VELA_CMD_STREAM_TENSOR = b"ethos_u_command_stream"
VELA_SCRATCH_PREFIX = b"scratch"
VELA_READ_ONLY_TENSOR = b"read_only"

# TFLite TensorType -> element size in bytes (only the types Vela can emit for
# an Ethos-U55 graph's arenas and activations).
TFLITE_TYPE_BYTES = {
    0: 4,  # FLOAT32
    1: 2,  # FLOAT16
    2: 4,  # INT32
    3: 1,  # UINT8
    4: 8,  # INT64
    6: 1,  # BOOL
    7: 2,  # INT16
    9: 1,  # INT8
    16: 1,  # UINT4 (packed; Vela does not use it for arenas)
}

# BASEPn region base-pointer pairs the Ethos-U55 exposes (k_ra8_npu_region_count).
MAX_REGIONS = 8

# Pinned Vela accelerator argument (see tools/vela/README.md and the uv lock).
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


def pack_blob(regions: list[dict], cmd: bytes, accel: int) -> bytes:
    """Pack a region list plus a command stream into a `.npub` blob.

    The single container writer, shared by the descriptor path (`emit`) and the
    real-Vela path (`distill`), so the two cannot drift into two dialects of the
    same format.

    Args:
        regions: One dict per BASEPn slot, in slot order. Each carries `role`
            (an ROLE key), `size` in bytes, and `mode`: "baked" with a `payload`
            of exactly `size` bytes, "runtime" (caller-allocated), or "alias"
            with `alias` naming a STRICTLY EARLIER slot of the same size.
        cmd: Command-stream bytes (non-empty).
        accel: Accelerator config recorded in the header (informational).

    Returns:
        The whole blob, header first.

    Raises:
        ValueError: Too many regions, an empty command stream, a baked payload
            whose length differs from its declared size, or an alias that names
            a forward slot, itself, or a slot of a different size.
    """
    if len(regions) > MAX_REGIONS:
        msg = f"too many regions: {len(regions)} > {MAX_REGIONS} (k_ra8_npu_region_count)"
        raise ValueError(msg)
    if len(cmd) == 0:
        msg = "command stream must not be empty (cmd_bytes > 0)"
        raise ValueError(msg)
    cmd_offset = HEADER_BYTES + (len(regions) * REGION_DESC_BYTES)

    table = bytearray()
    baked = bytearray()
    data_cursor = cmd_offset + len(cmd)
    for index, region in enumerate(regions):
        role = ROLE[region["role"]]
        size = int(region["size"])
        mode = region.get("mode", "runtime")
        if mode == "baked":
            payload = region["payload"]
            if len(payload) != size:
                msg = "baked payload length must equal region size"
                raise ValueError(msg)
            table += struct.pack("<IIII", role, RFLAG_BAKED, size, data_cursor)
            baked += payload
            data_cursor += size
        elif mode == "alias":
            target = int(region["alias"])
            if target >= index:
                msg = f"region {index} aliases {target}, which is not an earlier region"
                raise ValueError(msg)
            if int(regions[target]["size"]) != size:
                msg = f"region {index} aliases {target} but declares a different size"
                raise ValueError(msg)
            table += struct.pack("<IIII", role, RFLAG_ALIAS, size, target)
        else:
            table += struct.pack("<IIII", role, 0, size, 0)

    total = data_cursor
    payload_bytes = bytes(table) + cmd + bytes(baked)
    header = struct.pack(
        "<IIIIIIII",
        BLOB_MAGIC,
        BLOB_VERSION,
        total,
        len(regions),
        cmd_offset,
        len(cmd),
        int(accel),
        _fnv1a(payload_bytes),
    )
    blob = header + payload_bytes
    if len(blob) != total:
        msg = f"blob length {len(blob)} != declared total {total}"
        raise ValueError(msg)
    return blob


def build_blob(desc: dict) -> bytes:
    """Assemble a full .npub blob from a committed model descriptor."""
    regions = []
    for region in desc["regions"]:
        entry = {"role": region["role"], "size": int(region["size"])}
        if region.get("mode", "runtime") == "baked":
            entry["mode"] = "baked"
            entry["payload"] = _region_payload(region)
        regions.append(entry)
    return pack_blob(regions, _build_command_stream(desc), desc.get("accel", ACCEL_ETHOS_U55_256))


def _provenance_note(desc: dict) -> list[str]:
    """Header lines saying where this golden came from and how to regenerate it."""
    name = desc["name"]
    if desc.get("stream") == "vela":
        return [
            f"GENERATED by tools/vela/src/vela_gen.py distill from {desc['source']},",
            "itself produced by the pinned ethos-u-vela from",
            f"{desc['model_source']}.",
            "DO NOT EDIT BY HAND. Regenerating it needs the Vela install (see",
            "tools/vela/README.md); `just quality::local::vela_check` does NOT cover",
            "this golden, because re-deriving it is not a Vela-free step.",
        ]
    return [
        f"GENERATED by tools/vela/src/vela_gen.py from tools/vela/models/{name}.json.",
        "DO NOT EDIT BY HAND. Regenerate with `just quality::local::vela_regen`; the",
        "committed copy is diffed by `just quality::local::vela_check` (needs no Vela).",
    ]


def _stream_note(desc: dict) -> list[str]:
    """Header lines stating WHICH kind of command stream this golden holds."""
    if desc.get("stream") == "vela":
        return [
            "The command stream here is a REAL Ethos-U55 program: Vela's own",
            f"'{VELA_CUSTOM_CODE.decode('ascii')}' command stream, copied byte for byte out of the",
            "compiled model's constant buffer. Nothing in this repo decodes those",
            "opcodes, and no part of this tree has run one; executing it needs an",
            "RA8P1 board.",
        ]
    return [
        "The command stream here is the documented SE55 stand-in convention",
        "(ra8_npu_fake_cmd.h), NOT a real Vela program -- see vela_gen.py.",
    ]


def emit_header(desc: dict, blob: bytes) -> str:
    """Render `blob` as a self-contained, linkable C byte-array header."""
    symbol = desc["symbol"]
    name = desc["name"]
    # 16 bytes/row keeps each data line at 97 columns (2 indent + 16*"0xXX, "
    # minus the trailing space), inside the .clang-format ColumnLimit of 100, so
    # the emitted golden is already clang-format-22 clean and
    # `just quality::local::vela_check`
    # (generator output vs committed header) and the format gate never disagree.
    per_row = 16
    rows = []
    for start in range(0, len(blob), per_row):
        chunk = blob[start : start + per_row]
        rows.append("  " + "".join(f"0x{b:02X}, " for b in chunk).rstrip())
    body = "\n".join(rows)
    # The attribution is the CLOSING tag group of the @file block, not a separate
    # comment above it: scripts/checks/check-copyright.py requires @copyright and
    # the SPDX line INSIDE the file-header block, and the block has to be a
    # doxygen one (/**) so the generated golden documents itself like any other
    # first-party header.
    lines = [
        "/**",
        f" * @file {desc['header_name']}",
        f" * @brief Baked '.npub' Ethos-U55 model container for '{name}'",
        " * @ingroup grp_hal_system",
        " *",
        " * @details",
        *(f" * {line}" for line in _provenance_note(desc)),
        " *",
        " * A '.npub' Ethos-U55 model container (see libs/ra8_hal/inc/ra8_npu_blob.h):",
        " * an Ethos-U55 command stream plus its tensor region layout, baked as a byte",
        " * array the firmware links and the on-target loader (ra8_npu_loader.h) maps",
        " * into an ra8_npu_job_t.",
        " *",
        *(f" * {line}" for line in _stream_note(desc)),
        " *",
        " * @copyright Copyright (c) 2026 Brighton Sikarskie",
        " * SPDX-License-Identifier: MIT",
        " * @since 0.1.0",
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
        print(
            f"vela_gen: MISSING golden {golden} (run `just quality::local::vela_regen`)",
            file=sys.stderr,
        )
        return 1
    current = golden.read_text(encoding="ascii")
    if current != fresh:
        print(
            f"vela_gen: DRIFT -- {golden} is stale vs {args.descriptor}.\n"
            "          Run `just quality::local::vela_regen` and commit the result.",
            file=sys.stderr,
        )
        return 1
    print(f"vela_gen: {golden} is up to date")
    return 0


def cmd_compile(args: argparse.Namespace) -> int:
    """compile: run the pinned Vela on a real .tflite."""
    vela = shutil.which("vela")
    if vela is None:
        print(
            "vela_gen: locked ethos-u-vela is missing; run just setup and retry",
            file=sys.stderr,
        )
        return 1
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
    if not produced.is_file():
        print(f"vela_gen: Vela did not create expected output: {produced}", file=sys.stderr)
        return 1
    print(f"vela_gen: Vela wrote {produced}")
    print(
        f"vela_gen: NEXT -- distill it: distill {produced} --symbol <prefix> -o <header.h>",
    )
    return 0


def _vela_schema():  # noqa: ANN202  (Vela's vendored Model class, unstubbed)
    """Import Vela's own vendored TFLite schema binding for `Model`.

    Returns:
        The `Model` class from the installed ethos-u-vela package.

    Raises:
        RuntimeError: The Vela package is not importable, so a `_vela.tflite`
            cannot be read here; `just setup` installs it.
    """
    try:
        from ethosu.vela.tflite.Model import Model  # noqa: PLC0415 -- optional dep
    except ImportError as exc:  # pragma: no cover -- depends on the environment
        msg = "ethos-u-vela is not installed, so distill cannot read a _vela.tflite"
        raise RuntimeError(msg) from exc
    return Model


def _tensor_bytes(tensor) -> int:  # noqa: ANN001 -- flatbuffer binding object
    """Byte size of a runtime tensor, from its shape and element type."""
    elem = TFLITE_TYPE_BYTES.get(tensor.Type())
    if elem is None:
        msg = f"unsupported TFLite tensor type {tensor.Type()}"
        raise ValueError(msg)
    count = 1
    for axis in range(tensor.ShapeLength()):
        count *= int(tensor.Shape(axis))
    return count * elem


def _tensor_role(tensor, index: int, subgraph, const: bytes) -> str:  # noqa: ANN001
    """Classify one Vela tensor into a `.npub` region role."""
    name = tensor.Name() or b""
    if name.startswith(VELA_SCRATCH_PREFIX):
        return "scratch"
    if len(const) > 0:
        return "weights"
    inputs = {int(subgraph.Inputs(i)) for i in range(subgraph.InputsLength())}
    if index in inputs:
        return "input"
    outputs = {int(subgraph.Outputs(i)) for i in range(subgraph.OutputsLength())}
    if index in outputs:
        return "output"
    return "other"


def _const_bytes(model, tensor) -> bytes:  # noqa: ANN001
    """Constant buffer bytes backing @p tensor (empty when runtime-allocated)."""
    buf = model.Buffers(tensor.Buffer())
    length = buf.DataLength()
    if length == 0:
        return b""
    return bytes(buf.Data(i) for i in range(length))


def _ethos_operator(  # noqa: ANN202  (returns Vela's vendored Operator, unstubbed)
    model,  # noqa: ANN001  (Vela's vendored schema Model, unstubbed)
    subgraph,  # noqa: ANN001  (Vela's vendored schema SubGraph, unstubbed)
):
    """Return the single `ethos-u` custom operator in @p subgraph.

    Raises:
        ValueError: The model holds no `ethos-u` operator (nothing was lowered
            onto the NPU), or holds more than one (a mixed graph, which this lean
            container cannot express: it carries exactly one command stream).
    """
    found = []
    for i in range(subgraph.OperatorsLength()):
        op = subgraph.Operators(i)
        code = model.OperatorCodes(op.OpcodeIndex())
        if code.CustomCode() == VELA_CUSTOM_CODE:
            found.append(op)
    if len(found) == 0:
        msg = f"no '{VELA_CUSTOM_CODE.decode('ascii')}' operator: nothing was lowered onto the NPU"
        raise ValueError(msg)
    if len(found) > 1:
        msg = f"{len(found)} NPU operators: a mixed graph needs one .npub per command stream"
        raise ValueError(msg)
    return found[0]


def distill_blob(path: Path, accel: int = ACCEL_ETHOS_U55_256) -> tuple[bytes, dict]:
    """Distill a Vela `_vela.tflite` into a `.npub` blob plus a layout report.

    The region table is built in the order the on-target kernel programs BASEPn
    (libs/ra8_hal/src/ra8_ethosu_kernel.cc): the custom operator's input 0 is the
    command stream and becomes the blob's cmd_stream, every later input follows in
    order, then every output. A tensor that appears more than once (Vela's
    `scratch_fast` operand aliases `scratch` in Shared_Sram mode) is emitted again
    as an ALIAS of its first slot, so the slot count still matches what the
    command stream was compiled against without claiming a second buffer.

    Args:
        path: A Vela-compiled `_vela.tflite`.
        accel: Accelerator config recorded in the header (informational).

    Returns:
        The blob bytes and a report dict (`regions`, `cmd_bytes`, `total`).

    Raises:
        ValueError: Not a single-subgraph Vela output, or its command-stream
            tensor is missing or carries no constant bytes.
    """
    model = _vela_schema().GetRootAs(bytearray(path.read_bytes()), 0)
    if model.SubgraphsLength() != 1:
        msg = f"expected 1 subgraph, found {model.SubgraphsLength()}"
        raise ValueError(msg)
    subgraph = model.Subgraphs(0)
    op = _ethos_operator(model, subgraph)

    cmd_tensor = subgraph.Tensors(int(op.Inputs(0)))
    if cmd_tensor.Name() != VELA_CMD_STREAM_TENSOR:
        msg = f"operator input 0 is {cmd_tensor.Name()!r}, not the command stream"
        raise ValueError(msg)
    cmd = _const_bytes(model, cmd_tensor)
    if len(cmd) == 0:
        msg = "the command-stream tensor carries no constant bytes"
        raise ValueError(msg)

    slots = [int(op.Inputs(i)) for i in range(1, op.InputsLength())]
    slots += [int(op.Outputs(i)) for i in range(op.OutputsLength())]
    regions: list[dict] = []
    report: list[dict] = []
    first_slot: dict[int, int] = {}
    for index, tindex in enumerate(slots):
        tensor = subgraph.Tensors(tindex)
        const = _const_bytes(model, tensor)
        role = _tensor_role(tensor, tindex, subgraph, const)
        name = (tensor.Name() or b"").decode("ascii")
        if tindex in first_slot:
            target = first_slot[tindex]
            regions.append(
                {"role": role, "size": regions[target]["size"], "mode": "alias", "alias": target},
            )
            report.append({"slot": index, "name": name, "role": role, "alias": target})
            continue
        first_slot[tindex] = index
        if len(const) > 0:
            regions.append({"role": role, "size": len(const), "mode": "baked", "payload": const})
            report.append({"slot": index, "name": name, "role": role, "baked": len(const)})
        else:
            size = _tensor_bytes(tensor)
            regions.append({"role": role, "size": size, "mode": "runtime"})
            report.append({"slot": index, "name": name, "role": role, "runtime": size})

    blob = pack_blob(regions, cmd, accel)
    return blob, {"regions": report, "cmd_bytes": len(cmd), "total": len(blob)}


def cmd_distill(args: argparse.Namespace) -> int:
    """distill: real _vela.tflite -> .npub C header (needs the Vela install)."""
    source = Path(args.tflite)
    if not source.is_file():
        print(f"vela_gen: no such _vela.tflite: {source}", file=sys.stderr)
        return 1
    blob, report = distill_blob(source)
    desc = {
        "name": args.name or source.stem,
        "symbol": args.symbol,
        "stream": "vela",
        "source": source.name,
        "model_source": args.model_source,
        "header_name": Path(args.output).name,
    }
    header = emit_header(desc, blob)
    for region in report["regions"]:
        print(f"vela_gen: BASEP{region['slot']} {region}")
    print(f"vela_gen: cmd_stream {report['cmd_bytes']} B, blob {report['total']} B")
    out = Path(args.output)
    if args.check:
        if not out.is_file():
            print(f"vela_gen: MISSING golden {out}", file=sys.stderr)
            return 1
        if out.read_text(encoding="ascii") != header:
            print(f"vela_gen: DRIFT -- {out} is stale vs {source}", file=sys.stderr)
            return 1
        print(f"vela_gen: {out} matches the distilled blob")
        return 0
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(header, encoding="ascii")
    print(f"vela_gen: wrote {out} ({len(header)} bytes)")
    return 0


def main(argv: list[str]) -> int:
    """Parse the subcommand line and dispatch to the matching `cmd_*` handler.

    A subcommand is required, so a bare invocation is an argparse usage error
    rather than a default action.

    Every handler returns 0 only after its requested operation ran and its
    expected output or golden comparison was verified.

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

    p_compile = sub.add_parser("compile", help="run the pinned Vela on a .tflite")
    p_compile.add_argument("tflite", help="quantized INT8/INT16 .tflite model")
    p_compile.add_argument("-o", "--output", default="build/vela", help="Vela output dir")
    p_compile.set_defaults(func=cmd_compile)

    p_distill = sub.add_parser("distill", help="real _vela.tflite -> .npub C header")
    p_distill.add_argument("tflite", help="Vela-compiled _vela.tflite")
    p_distill.add_argument("-o", "--output", required=True, help="output C header path")
    p_distill.add_argument("--symbol", required=True, help="C symbol prefix for the blob")
    p_distill.add_argument("--name", default="", help="model name for the header prose")
    p_distill.add_argument(
        "--model-source",
        default="tools/vela/models/conv_int8_fixture.py",
        help="in-tree origin of the .tflite Vela compiled, recorded in the header",
    )
    p_distill.add_argument(
        "--check",
        action="store_true",
        help="re-derive and diff against the committed golden instead of writing it",
    )
    p_distill.set_defaults(func=cmd_distill)

    args = parser.parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
