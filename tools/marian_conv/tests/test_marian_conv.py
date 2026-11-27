# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Offline unit and Zig-runtime integration tests for `tools/marian_conv`."""

from __future__ import annotations

import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

SRC_DIR = Path(__file__).resolve().parents[1] / "src"
GOLDENS_DIR = Path(__file__).resolve().parents[1] / "goldens"
RUNNER_DIR = Path(__file__).resolve().parents[1] / "zig_runner"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

from bergamot_io import (  # noqa: E402, I001
    MARIAN_BINARY_VERSION,
    MARIAN_TYPE_FLOAT32,
    MARIAN_TYPE_INTGEMM8,
    SHORTLIST_MAGIC,
    parse_bergamot_shortlist,
    parse_marian_binary,
    parse_spm,
)
from containers import TMNT_FLAG_DECODER_SSRU, TMNT_FLAG_TIED_EMBED, TMNT_FLAG_ZERO_BOS  # noqa: E402
from manifest import PAIRS, get_pair_spec  # noqa: E402
from marian_conv import convert_pair_from_dir, generate_golden  # noqa: E402
from ref_marian import TmntRunner, TvocReader  # noqa: E402

_SHA256_HEX_LEN = 64
_TINY_VOCAB_SIZE = 6
_MAX_STEP_LOGIT_DIFF = 1e-4
_MAX_QUANT_BOUNDARY_DIFF = 0.25
_KOEN_GOLDEN_CASES = 5
_JAZH_GOLDEN_CASES = 3


def _require(cond: bool, msg: str = "assertion failed") -> None:
    """Fail test with an explicit AssertionError without bare `assert` statements."""
    if not cond:
        raise AssertionError(msg)


def _pb_varint(val: int) -> bytes:
    """Encode an unsigned integer as protobuf base-128 varint."""
    out = bytearray()
    while True:
        byte = val & 0x7F
        val >>= 7
        if val:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def _pb_bytes(field_no: int, payload: bytes) -> bytes:
    """Encode one length-delimited protobuf field."""
    return _pb_varint((field_no << 3) | 2) + _pb_varint(len(payload)) + payload


def _pb_varint_field(field_no: int, val: int) -> bytes:
    """Encode one varint protobuf field."""
    return _pb_varint((field_no << 3) | 0) + _pb_varint(val)


def _build_synthetic_spm() -> bytes:
    """Construct a valid 6-piece SentencePiece `ModelProto` matching Bergamot flags."""
    u32_none = 0xFFFFFFFF
    pieces = [
        ("</s>", 0.0, 3),
        ("<unk>", 0.0, 2),
        ("\u2581hi", -1.0, 1),
        ("\u2581ok", -1.5, 1),
        ("<0x0A>", -5.0, 6),
        ("\u2581bye", -2.5, 1),
    ]
    buf = bytearray()
    for txt, score, kind in pieces:
        sp = (
            _pb_bytes(1, txt.encode("utf-8"))
            + _pb_varint((2 << 3) | 5)
            + struct.pack("<f", score)
            + _pb_varint_field(3, kind)
        )
        buf.extend(_pb_bytes(1, sp))
    trainer = (
        _pb_varint_field(3, 1)
        + _pb_varint_field(35, 1)
        + _pb_varint_field(40, 1)
        + _pb_varint_field(41, u32_none)
        + _pb_varint_field(42, 0)
        + _pb_varint_field(43, u32_none)
    )
    normalizer = _pb_varint_field(3, 1) + _pb_varint_field(4, 1) + _pb_varint_field(5, 1)
    buf.extend(_pb_bytes(2, trainer))
    buf.extend(_pb_bytes(3, normalizer))
    return bytes(buf)


def _build_synthetic_shortlist(vocab: int = 6) -> bytes:
    """Construct a valid Bergamot `BinaryShortlist` with `first_k = 1`."""
    row_offs = np.array([0, 0, 0, 1, 1, 1, 1], dtype="<u8")
    trg_ids = np.array([3], dtype="<u4")
    hdr = struct.pack("<6Q", SHORTLIST_MAGIC, 0, 1, 50, vocab + 1, int(trg_ids.size))
    return hdr + row_offs.tobytes() + trg_ids.tobytes()


def _pack_marian_intgemm(mat_out_in: np.ndarray, quant_mult: float) -> bytes:
    """Pack a raw `[out, in]` int8 matrix plus trailing `<f32 quantMult>`."""
    return np.ascontiguousarray(mat_out_in, dtype=np.int8).tobytes() + struct.pack("<f", quant_mult)


def _build_synthetic_marian_bin() -> bytes:
    """Build an authentic minimal Bergamot SSRU Marian `.bin` container."""
    vocab, d, d_ff = 6, 4, 4
    wemb = np.zeros((vocab, d), dtype=np.int8)
    wemb[3] = [1, 0, 0, 0]
    wemb[0] = [5, 0, 0, 0]
    wemb[2] = [0, 1, 0, 0]

    yml = (
        b"version: v1.12.14\x00\n"
        b"dim-emb: 4\n"
        b"transformer-dim-ffn: 4\n"
        b"transformer-heads: 2\n"
        b"enc-depth: 1\n"
        b"dec-depth: 1\n"
        b"dec-cell: ssru\n"
        b"transformer-preprocess: ''\n"
        b"tied-embeddings-all: true\n"
    )

    # Step 0 (norm=[-1,-1,1,1]) scores tok 0 = -100 vs tok 3 = -5 -> picks 3;
    # Step 1 (embed(3) makes dim 0 positive) scores tok 0 ~ +173 vs tok 3 ~ +22 -> picks 0 (EOS).
    logit_bias = np.array([0.0, -5.0, -5.0, 5.0, -5.0, -5.0], dtype="<f4")
    wemb[3] = [10, 0, 0, 0]
    wemb[0] = [100, 0, 0, 0]

    items: list[tuple[str, int, tuple[int, ...], bytes]] = [
        ("special:model.yml", 0x0101, (len(yml),), yml),
        ("Wemb", MARIAN_TYPE_INTGEMM8, (vocab, d), _pack_marian_intgemm(wemb, 1.0)),
        ("decoder_ff_logit_out_b", MARIAN_TYPE_FLOAT32, (1, vocab), logit_bias.tobytes()),
    ]
    zeros_i8 = np.zeros((d, d), dtype=np.int8)
    zeros_f32 = np.zeros(d, dtype="<f4")
    ones_f32 = np.ones(d, dtype="<f4")
    for p in ("encoder_l1_self", "decoder_l1_context"):
        items.extend(
            (f"{p}_{w}", MARIAN_TYPE_INTGEMM8, (d, d), _pack_marian_intgemm(zeros_i8, 1.0))
            for w in ("Wq", "Wk", "Wv", "Wo")
        )
        items.append((f"{p}_Wo_ln_scale", MARIAN_TYPE_FLOAT32, (1, d), ones_f32.tobytes()))
        items.append((f"{p}_Wo_ln_bias", MARIAN_TYPE_FLOAT32, (1, d), zeros_f32.tobytes()))
    items.extend(
        (f"decoder_l1_rnn_{w}", MARIAN_TYPE_INTGEMM8, (d, d), _pack_marian_intgemm(zeros_i8, 1.0))
        for w in ("W", "Wf")
    )
    items.append(("decoder_l1_rnn_ffn_ln_scale", MARIAN_TYPE_FLOAT32, (1, d), ones_f32.tobytes()))
    items.append(("decoder_l1_rnn_ffn_ln_bias", MARIAN_TYPE_FLOAT32, (1, d), zeros_f32.tobytes()))
    for p in ("encoder_l1_ffn", "decoder_l1_ffn"):
        items.append(
            (f"{p}_W1", MARIAN_TYPE_INTGEMM8, (d, d_ff), _pack_marian_intgemm(zeros_i8, 1.0))
        )
        items.append(
            (f"{p}_W2", MARIAN_TYPE_INTGEMM8, (d_ff, d), _pack_marian_intgemm(zeros_i8, 1.0))
        )
        items.append((f"{p}_ffn_ln_scale", MARIAN_TYPE_FLOAT32, (1, d), ones_f32.tobytes()))
        items.append((f"{p}_ffn_ln_bias", MARIAN_TYPE_FLOAT32, (1, d), zeros_f32.tobytes()))
    return _serialize_marian_items(items)


def _serialize_marian_items(items: list[tuple[str, int, tuple[int, ...], bytes]]) -> bytes:
    """Serialize `(name, dtype, shape, payload)` tuples into a Marian `.bin` container."""
    headers = bytearray()
    name_bytes = bytearray()
    shape_bytes = bytearray()
    payloads = bytearray()
    for name, dtype, shape, data in items:
        raw_name = name.encode("utf-8") + b"\x00"
        headers.extend(struct.pack("<QQQQ", len(raw_name), dtype, len(shape), len(data)))
        name_bytes.extend(raw_name)
        shape_bytes.extend(struct.pack(f"<{len(shape)}i", *shape))
        payloads.extend(data)
    meta = (
        struct.pack("<QQ", MARIAN_BINARY_VERSION, len(items)) + headers + name_bytes + shape_bytes
    )
    pad = ((len(meta) + 255) & ~255) - len(meta)
    return meta + bytes(pad) + payloads


def _stage_synthetic_inputs(tmp_path: Path) -> Path:
    """Write synthetic `ko-en` input files matching pinned filenames into `tmp_path`."""
    spec = get_pair_spec("ko-en")
    in_dir = tmp_path / "inputs"
    in_dir.mkdir(parents=True, exist_ok=True)
    (in_dir / spec.model.filename).write_bytes(_build_synthetic_marian_bin())
    (in_dir / spec.vocab.filename).write_bytes(_build_synthetic_spm())
    (in_dir / spec.shortlist.filename).write_bytes(_build_synthetic_shortlist())
    return in_dir


def test_manifest_registry_pins_and_licenses() -> None:
    """Verify all configured language pairs carry valid SHA-256 pins and MPL-2.0 metadata."""
    _require({"ko-en", "ja-en", "zh-en"}.issubset(PAIRS.keys()), "missing required pair keys")
    for pair_key, spec in PAIRS.items():
        _require(spec.pair == pair_key, f"mismatched pair key {pair_key}")
        _require(spec.license_spdx == "MPL-2.0", f"unexpected license {spec.license_spdx}")
        _require(spec.nmt_model_id >= 1, "nmt_model_id must be >= 1")
        for pin in (spec.model, spec.vocab, spec.shortlist):
            _require(len(pin.sha256) == _SHA256_HEX_LEN, f"invalid sha256 len for {pin.filename}")
            _require(int(pin.sha256, 16) >= 0, f"non-hex sha256 for {pin.filename}")
            _require(pin.size > 0, f"non-positive size for {pin.filename}")
            _require(pin.url().startswith("https://"), f"expected https URL for {pin.filename}")


def test_parsers_convert_determinism_and_pin_enforcement(tmp_path: Path) -> None:
    """Verify binary parsing, deterministic conversion, NUL-free version, and pin check."""
    in_dir = _stage_synthetic_inputs(tmp_path)
    with pytest.raises(ValueError, match=r"size .* != pinned"):
        convert_pair_from_dir("ko-en", in_dir, tmp_path / "blocked")

    out_a = tmp_path / "out_a"
    out_b = tmp_path / "out_b"
    meta_a = convert_pair_from_dir("ko-en", in_dir, out_a, max_seq=16, allow_unpinned=True)
    meta_b = convert_pair_from_dir("ko-en", in_dir, out_b, max_seq=16, allow_unpinned=True)
    _require(meta_a["output_sha256"] == meta_b["output_sha256"], "conversion non-deterministic")
    _require(
        meta_a["marian_version"] == "v1.12.14", f"unexpected version {meta_a['marian_version']}"
    )

    runner = TmntRunner((out_a / "koen.tmnt").read_bytes())
    expect_flags = TMNT_FLAG_DECODER_SSRU | TMNT_FLAG_TIED_EMBED | TMNT_FLAG_ZERO_BOS
    _require((runner.model_flags & expect_flags) == expect_flags, "missing expected TMNT flags")
    _require(runner.shortlist_first_k == 1, "shortlist_first_k mismatch")

    tok = TvocReader.open((out_a / "koen.tvoc").read_bytes())
    enc = tok.encode("hi", add_eos=True)
    _require(enc == [2, 0], f"unexpected encoding {enc}")
    _require(tok.decode(enc) == "hi", "unexpected decode output")

    model = parse_marian_binary((in_dir / get_pair_spec("ko-en").model.filename).read_bytes())
    spm = parse_spm((in_dir / get_pair_spec("ko-en").vocab.filename).read_bytes())
    sl = parse_bergamot_shortlist(
        (in_dir / get_pair_spec("ko-en").shortlist.filename).read_bytes(), _TINY_VOCAB_SIZE
    )
    _require(len(spm.pieces) == _TINY_VOCAB_SIZE, "unexpected spm piece count")
    _require(sl.first_k == 1, "unexpected shortlist first_k")
    _require(model.config["dec-cell"] == "ssru", "expected ssru dec-cell")


def test_reference_golden_execution_and_zig_runner_parity(tmp_path: Path) -> None:
    """Verify synthetic SSRU decode parity across Python `TmntRunner` and Zig `zig_runner`."""
    in_dir = _stage_synthetic_inputs(tmp_path)
    out_dir = tmp_path / "converted"
    convert_pair_from_dir("ko-en", in_dir, out_dir, max_seq=16, allow_unpinned=True)

    zig_exe = RUNNER_DIR / "zig-out" / "bin" / "marian_zig_runner.exe"
    if not zig_exe.exists():
        zig_exe = RUNNER_DIR / "zig-out" / "bin" / "marian_zig_runner"
    zig_bin = shutil.which("zig")
    if not zig_exe.exists() and zig_bin is not None:
        subprocess.run(  # noqa: S603
            [zig_bin, "build", "--build-file", str(RUNNER_DIR / "build.zig"), "-j2"],
            check=True,
        )

    golden = generate_golden(
        "ko-en",
        out_dir,
        ["hi"],
        max_tgt_len=4,
        zig_runner=zig_exe if zig_exe.exists() else None,
    )
    case = golden["cases"][0]  # type: ignore[index]
    _require(case["src_ids"] == [2, 0], f"unexpected src_ids {case['src_ids']}")
    _require(case["greedy_ids"] == [3, 0], f"unexpected greedy_ids {case['greedy_ids']}")
    _require(case["decoded_en"] == "ok", f"unexpected decoded_en {case['decoded_en']}")


def test_committed_goldens_are_ascii_and_zig_verified() -> None:
    """Verify committed `ko-en`/`ja-en`/`zh-en` goldens, ASCII purity, and Zig SHA-256."""
    expected = [
        ("koen_reference_golden.json", "ko-en", _KOEN_GOLDEN_CASES, _MAX_STEP_LOGIT_DIFF),
        ("jaen_reference_golden.json", "ja-en", _JAZH_GOLDEN_CASES, _MAX_STEP_LOGIT_DIFF),
        ("zhen_reference_golden.json", "zh-en", _JAZH_GOLDEN_CASES, _MAX_QUANT_BOUNDARY_DIFF),
    ]
    for filename, pair_name, n_cases, logit_tol in expected:
        raw = (GOLDENS_DIR / filename).read_bytes()
        raw.decode("ascii")
        data = json.loads(raw)
        _require(data["schema"] == 1, f"{filename}: schema != 1")
        _require(data["pair"] == pair_name, f"{filename}: pair != {pair_name}")
        _require(data["source_version"] == "2.1", f"{filename}: source_version != 2.1")
        _require(data["license_spdx"] == "MPL-2.0", f"{filename}: license != MPL-2.0")
        _require(data["zig_verified"] is True, f"{filename}: missing zig_verified=True")
        _require(data["marian_version"].startswith("v1.12.14"), f"{filename}: bad marian_version")
        ver = data["zig_verification"]
        _require(ver["max_step_logit_diff"] < logit_tol, f"{filename}: logit diff exceeded tol")
        _require(
            0 < ver["batch_shortlist_weight_bytes"] < ver["batch_full_weight_bytes"],
            f"{filename}: shortlist did not reduce batched weight traffic",
        )
        _require(len(data["cases"]) == n_cases, f"{filename}: unexpected case count")
        for case in data["cases"]:
            _require(case["greedy_ids"][-1] == 0, f"{filename}: case did not end on EOS")
            _require(
                len(case["steps"]) == len(case["greedy_ids"]), f"{filename}: step len mismatch"
            )
            for step in case["steps"]:
                _require(len(step["zig_logits_sha256"]) == _SHA256_HEX_LEN, "invalid logit sha256")
