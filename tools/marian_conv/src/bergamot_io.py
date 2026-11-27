# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Parsers for Bergamot Marian `.bin`, SentencePiece `.spm`, and `.s2t.bin`."""

from __future__ import annotations

import math
import struct
from dataclasses import dataclass

import numpy as np

MARIAN_BINARY_VERSION = 1
MARIAN_TYPE_INT8 = 0x0101
MARIAN_TYPE_FLOAT32 = 0x0404
MARIAN_TYPE_INTGEMM8 = 0x4101
SHORTLIST_MAGIC = 20191221
SHORTLIST_HEADER_BYTES = 48

SPM_NONE_ID = 0xFFFFFFFF

_YAML_QUOTED_MIN_LEN = 2
_MATRIX_NDIM = 2
_MARIAN_BIN_HEADER_BYTES = 16
_PB_WIRE_VARINT = 0
_PB_WIRE_F64 = 1
_PB_WIRE_BYTES = 2
_PB_WIRE_F32 = 5
_PB_PIECE_FIELD = 1
_PB_TRAINER_SPEC_FIELD = 2
_PB_NORM_SPEC_FIELD = 3
_PB_NORM_ESCAPE_WS_FIELD = 5
_MIN_SHORTLIST_OFFSETS = 2


@dataclass(frozen=True)
class MarianTensor:
    """Decoded tensor from a Marian `.bin` container."""

    name: str
    dtype: int
    shape: tuple[int, ...]
    data: np.ndarray
    scale: float


@dataclass(frozen=True)
class MarianModel:
    """Complete parsed Bergamot Marian model with YAML config and tensors."""

    config: dict[str, object]
    tensors: dict[str, MarianTensor]


@dataclass(frozen=True)
class SpmPiece:
    """Single SentencePiece vocabulary record parsed from `ModelProto`."""

    text: str
    score: float
    kind: int


@dataclass(frozen=True)
class SpmVocab:
    """Parsed SentencePiece vocabulary and normalization flags."""

    model_type: int
    pieces: list[SpmPiece]
    unk_id: int
    bos_id: int
    eos_id: int
    pad_id: int
    byte_fallback: bool
    add_dummy_prefix: bool
    remove_extra_ws: bool
    escape_ws: bool


@dataclass(frozen=True)
class BergamotShortlist:
    """Parsed Bergamot binary lexical shortlist (`BinaryShortlist`)."""

    first_k: int
    best_k: int
    row_offsets: np.ndarray
    target_ids: np.ndarray


def _parse_simple_yaml(text: str) -> dict[str, object]:
    """Parse Marian's `special:model.yml` scalar/list subset deterministically."""
    result: dict[str, object] = {}
    cur_list_key: str | None = None
    for raw_line in text.splitlines():
        stripped = raw_line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if stripped.startswith("- ") and cur_list_key is not None:
            lst = result[cur_list_key]
            if isinstance(lst, list):
                lst.append(_parse_yaml_scalar(stripped[2:].strip()))
            continue
        cur_list_key = None
        if ":" not in raw_line:
            continue
        key, val_raw = raw_line.split(":", 1)
        key = key.strip()
        val_s = val_raw.strip()
        if not val_s:
            result[key] = []
            cur_list_key = key
        elif val_s == "[]":
            result[key] = []
        else:
            result[key] = _parse_yaml_scalar(val_s)
    return result


def _parse_yaml_scalar(val: str) -> object:
    """Convert one YAML scalar string token to Python primitive."""
    if len(val) >= _YAML_QUOTED_MIN_LEN and val[0] == val[-1] and val[0] in ("'", '"'):
        return val[1:-1]
    if val == "true":
        return True
    if val == "false":
        return False
    if val.lstrip("-").isdigit():
        return int(val)
    return val


def _quantize_float_tensor(arr: np.ndarray) -> tuple[np.ndarray, float]:
    """Symmetrically quantize a float32 weight matrix to int8 `[-127, 127]`."""
    amax = float(np.max(np.abs(arr))) if arr.size else 0.0
    if amax == 0.0:
        return np.zeros(arr.shape, dtype=np.int8), 1.0
    scale = amax / 127.0
    q = np.rint(arr / np.float32(scale)).clip(-127, 127).astype(np.int8)
    return q, scale


def _decode_marian_tensor(
    name: str, dtype: int, shape: tuple[int, ...], buf: bytes
) -> MarianTensor:
    """Decode one tensor payload into canonical `[out, in]` int8 or f32 arrays."""
    if dtype == MARIAN_TYPE_INTGEMM8:
        prod = math.prod(shape)
        quant_mult = struct.unpack_from("<f", buf, prod)[0]
        if not math.isfinite(quant_mult) or quant_mult <= 0.0:
            msg = f"invalid quantMult {quant_mult} in tensor {name}"
            raise ValueError(msg)
        raw_i8 = np.frombuffer(buf[:prod], dtype=np.int8).copy()
        if len(shape) == _MATRIX_NDIM and name != "Wemb" and not name.endswith("QuantMultA"):
            raw_i8 = raw_i8.reshape((shape[1], shape[0]))
        elif len(shape) >= 1:
            raw_i8 = raw_i8.reshape(shape)
        return MarianTensor(
            name=name,
            dtype=dtype,
            shape=tuple(raw_i8.shape),
            data=raw_i8,
            scale=1.0 / quant_mult,
        )
    if dtype == MARIAN_TYPE_FLOAT32:
        f32 = np.frombuffer(buf, dtype="<f4").astype(np.float32, copy=True)
        if shape:
            f32 = f32.reshape(shape)
        return MarianTensor(name=name, dtype=dtype, shape=shape, data=f32, scale=1.0)
    if dtype == MARIAN_TYPE_INT8:
        raw_u8 = np.frombuffer(buf, dtype=np.uint8).copy()
        return MarianTensor(name=name, dtype=dtype, shape=shape, data=raw_u8, scale=1.0)
    msg = f"unsupported Marian tensor dtype {dtype:#x} for {name}"
    raise ValueError(msg)


def parse_marian_binary(raw: bytes) -> MarianModel:
    """Parse a Bergamot/Marian `.bin` binary container."""
    if len(raw) < _MARIAN_BIN_HEADER_BYTES:
        msg = "Marian binary too short for file header"
        raise ValueError(msg)
    ver, hdr_count = struct.unpack_from("<QQ", raw, 0)
    if ver != MARIAN_BINARY_VERSION:
        msg = f"unsupported Marian binary version {ver}"
        raise ValueError(msg)

    pos = 16
    entries = [struct.unpack_from("<QQQQ", raw, pos + i * 32) for i in range(hdr_count)]
    pos += hdr_count * 32

    names: list[str] = []
    for name_len, _, _, _ in entries:
        names.append(raw[pos : pos + name_len].decode("utf-8").rstrip("\x00"))
        pos += name_len

    shapes: list[tuple[int, ...]] = []
    for _, _, shape_len, _ in entries:
        shapes.append(struct.unpack_from(f"<{shape_len}i", raw, pos))
        pos += shape_len * 4

    cur = (pos + 255) & ~255
    config: dict[str, object] = {}
    tensors: dict[str, MarianTensor] = {}
    for (_, dtype, _, data_bytes), name, shape in zip(entries, names, shapes, strict=True):
        buf = raw[cur : cur + data_bytes]
        if len(buf) != data_bytes:
            msg = f"truncated Marian tensor payload for {name}"
            raise ValueError(msg)
        cur += data_bytes
        if name == "special:model.yml":
            config = _parse_simple_yaml(buf.decode("utf-8"))
        elif not name.endswith("_QuantMultA"):
            tensors[name] = _decode_marian_tensor(name, dtype, shape, buf)

    return MarianModel(config=config, tensors=tensors)


def weight_as_i8(tensor: MarianTensor) -> tuple[np.ndarray, float]:
    """Return a 2D weight tensor in `[rows, cols]` int8 with float scale."""
    if tensor.dtype == MARIAN_TYPE_INTGEMM8:
        return tensor.data, tensor.scale
    if tensor.dtype == MARIAN_TYPE_FLOAT32:
        mat = (
            tensor.data.T
            if len(tensor.shape) == _MATRIX_NDIM and tensor.name != "Wemb"
            else tensor.data
        )
        return _quantize_float_tensor(mat)
    msg = f"tensor {tensor.name} dtype {tensor.dtype:#x} cannot convert to int8"
    raise ValueError(msg)


def _read_varint(buf: bytes, idx: int) -> tuple[int, int]:
    """Decode one protobuf unsigned base-128 varint."""
    shift = 0
    val = 0
    while True:
        byte = buf[idx]
        idx += 1
        val |= (byte & 0x7F) << shift
        if not (byte & 0x80):
            return val, idx
        shift += 7


def _iter_pb_fields(buf: bytes) -> list[tuple[int, int, int | bytes]]:
    """Decode raw `(field_no, wire_type, value)` entries from a protobuf message."""
    idx = 0
    out: list[tuple[int, int, int | bytes]] = []
    while idx < len(buf):
        tag, idx = _read_varint(buf, idx)
        field_no = tag >> 3
        wire_type = tag & 7
        if wire_type == _PB_WIRE_VARINT:
            val, idx = _read_varint(buf, idx)
            out.append((field_no, wire_type, val))
        elif wire_type == _PB_WIRE_F64:
            out.append((field_no, wire_type, buf[idx : idx + 8]))
            idx += 8
        elif wire_type == _PB_WIRE_BYTES:
            ln, idx = _read_varint(buf, idx)
            out.append((field_no, wire_type, buf[idx : idx + ln]))
            idx += ln
        elif wire_type == _PB_WIRE_F32:
            out.append((field_no, wire_type, buf[idx : idx + 4]))
            idx += 4
        else:
            msg = f"unsupported protobuf wire type {wire_type}"
            raise ValueError(msg)
    return out


def _u64_to_sp_id(raw_id: object) -> int:
    """Convert protobuf unsigned varint (`uint64(-1)` == absent) to uint32 ID."""
    if not isinstance(raw_id, int) or raw_id >= SPM_NONE_ID:
        return SPM_NONE_ID
    return raw_id


def parse_spm(raw: bytes) -> SpmVocab:
    """Parse a SentencePiece `.spm` `ModelProto` file using pure stdlib."""
    pieces: list[SpmPiece] = []
    tspec: dict[int, int | bytes] = {}
    nspec: dict[int, int | bytes] = {}
    for field_no, _, val in _iter_pb_fields(raw):
        if not isinstance(val, bytes):
            continue
        if field_no == _PB_PIECE_FIELD:
            sub = {sf: sv for sf, _, sv in _iter_pb_fields(val)}
            text = sub[1].decode("utf-8") if isinstance(sub.get(1), bytes) else ""
            score = struct.unpack("<f", sub[2])[0] if isinstance(sub.get(2), bytes) else 0.0
            kind_raw = sub.get(3, 1)
            kind = kind_raw if isinstance(kind_raw, int) else 1
            pieces.append(SpmPiece(text=text, score=score, kind=kind))
        elif field_no == _PB_TRAINER_SPEC_FIELD:
            tspec = {sf: sv for sf, _, sv in _iter_pb_fields(val)}
        elif field_no == _PB_NORM_SPEC_FIELD:
            nspec = {sf: sv for sf, _, sv in _iter_pb_fields(val)}

    if not pieces:
        msg = "SentencePiece ModelProto contains zero pieces"
        raise ValueError(msg)

    model_type_raw = tspec.get(3, 1)
    sp_type = model_type_raw if isinstance(model_type_raw, int) else 1
    tvoc_model_type = 0 if sp_type == 1 else 1
    return SpmVocab(
        model_type=tvoc_model_type,
        pieces=pieces,
        unk_id=_u64_to_sp_id(tspec.get(40, 0)),
        bos_id=_u64_to_sp_id(tspec.get(41, SPM_NONE_ID)),
        eos_id=_u64_to_sp_id(tspec.get(42, SPM_NONE_ID)),
        pad_id=_u64_to_sp_id(tspec.get(43, SPM_NONE_ID)),
        byte_fallback=bool(tspec.get(35, 0)),
        add_dummy_prefix=bool(nspec.get(3, 1)),
        remove_extra_ws=bool(nspec.get(4, 1)),
        escape_ws=bool(nspec.get(_PB_NORM_ESCAPE_WS_FIELD, 1))
        if _PB_NORM_ESCAPE_WS_FIELD in nspec
        else True,
    )


def parse_bergamot_shortlist(raw: bytes, expect_vocab: int | None = None) -> BergamotShortlist:
    """Parse a Bergamot `lex.50.50.<pair>.s2t.bin` binary shortlist."""
    if len(raw) < SHORTLIST_HEADER_BYTES:
        msg = "Bergamot shortlist truncated before 48-byte header"
        raise ValueError(msg)
    magic_hash0, _, first_k, best_k, num_offs, num_trgs = struct.unpack_from("<6Q", raw, 0)
    magic = magic_hash0 & 0xFFFFFFFF
    if magic != SHORTLIST_MAGIC:
        msg = f"unexpected Bergamot shortlist magic {magic} (expected {SHORTLIST_MAGIC})"
        raise ValueError(msg)
    if num_offs < _MIN_SHORTLIST_OFFSETS:
        msg = f"invalid Bergamot shortlist offset count {num_offs}"
        raise ValueError(msg)
    if expect_vocab is not None and num_offs != expect_vocab + 1:
        msg = f"shortlist vocab+1 {num_offs} != expected {expect_vocab + 1}"
        raise ValueError(msg)
    expected_len = SHORTLIST_HEADER_BYTES + num_offs * 8 + num_trgs * 4
    if len(raw) != expected_len:
        msg = f"shortlist size {len(raw)} != expected {expected_len}"
        raise ValueError(msg)

    row_offs = np.frombuffer(raw, dtype="<u8", count=num_offs, offset=SHORTLIST_HEADER_BYTES).copy()
    trg_ids = np.frombuffer(
        raw, dtype="<u4", count=num_trgs, offset=SHORTLIST_HEADER_BYTES + num_offs * 8
    ).copy()
    if int(row_offs[0]) != 0 or int(row_offs[-1]) != num_trgs:
        msg = "corrupt Bergamot shortlist offset bounds"
        raise ValueError(msg)
    return BergamotShortlist(
        first_k=int(first_k),
        best_k=int(best_k),
        row_offsets=row_offs,
        target_ids=trg_ids,
    )
