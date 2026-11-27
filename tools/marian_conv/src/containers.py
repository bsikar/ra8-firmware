# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Deterministic packers for `TVOC`, `TSLT`, and `TMNT` containers."""

from __future__ import annotations

import math
import struct
import zlib
from dataclasses import dataclass

import numpy as np
from bergamot_io import (
    SPM_NONE_ID,
    BergamotShortlist,
    MarianModel,
    MarianTensor,
    SpmVocab,
    weight_as_i8,
)

TVOC_MAGIC = 0x434F5654
TVOC_VERSION = 1
TVOC_HEADER_BYTES = 56
TVOC_RECORD_BYTES = 12
TVOC_FLAG_ADD_DUMMY_PREFIX = 1 << 0
TVOC_FLAG_REMOVE_EXTRA_WS = 1 << 1
TVOC_FLAG_ESCAPE_WS = 1 << 2
TVOC_FLAG_BYTE_FALLBACK = 1 << 3
TVOC_FLAG_NORM_FULLWIDTH = 1 << 4
_SPM_TO_TVOC_KIND = {1: 0, 2: 1, 3: 2, 4: 3, 5: 5, 6: 4}

TSLT_MAGIC = 0x544C5354
TSLT_VERSION = 1
TSLT_HEADER_BYTES = 16

TMNT_MAGIC = 0x544E4D54
TMNT_VERSION = 1
TMNT_HEADER_BYTES = 84
TMNT_DESC_BYTES = 24
TMNT_FLAG_PRE_NORM = 1 << 0
TMNT_FLAG_SCALE_EMBED = 1 << 1
TMNT_FLAG_TIED_EMBED = 1 << 2
TMNT_FLAG_DECODER_SSRU = 1 << 3
TMNT_FLAG_ZERO_BOS = 1 << 4
TMNT_DTYPE_NONE = 0
TMNT_DTYPE_I8 = 1
TMNT_DTYPE_F32 = 2

DEFAULT_MAX_SEQ = 256
LN_EPS_MARIAN = 1e-6


@dataclass(frozen=True)
class SlotTensor:
    """Resolved tensor descriptor and raw array payload for one `TMNT` slot."""

    dtype: int
    rows: int
    cols: int
    scale: float
    zero_point: int
    payload: np.ndarray | None = None
    alias_slot: int | None = None


def _align_up4(val: int) -> int:
    """Round an offset up to a 4-byte boundary."""
    return (val + 3) & ~3


def build_sinusoidal_pos_embed(max_seq: int, d_model: int) -> np.ndarray:
    """Compute exact Marian/Bergamot sinusoidal positions `[max_seq, d_model]`."""
    num_timescales = d_model // 2
    if num_timescales <= 1:
        return np.zeros((max_seq, d_model), dtype="<f4")
    log_inc = math.log(10000.0) / float(num_timescales - 1)
    pos = np.arange(max_seq, dtype=np.float64)[:, None]
    idx = np.arange(num_timescales, dtype=np.float64)[None, :]
    angles = pos * np.exp(-idx * log_inc)
    out = np.zeros((max_seq, d_model), dtype="<f4")
    out[:, :num_timescales] = np.sin(angles).astype(np.float32)
    out[:, num_timescales : 2 * num_timescales] = np.cos(angles).astype(np.float32)
    return out


def pack_tvoc(vocab: SpmVocab, *, norm_fullwidth: bool = True) -> bytes:
    """Serialize an `SpmVocab` into a CRC-32-checked `TVOC` binary buffer."""
    vocab_size = len(vocab.pieces)
    flags = 0
    if vocab.byte_fallback:
        flags |= TVOC_FLAG_BYTE_FALLBACK
    if vocab.add_dummy_prefix:
        flags |= TVOC_FLAG_ADD_DUMMY_PREFIX
    if vocab.escape_ws:
        flags |= TVOC_FLAG_ESCAPE_WS
    if vocab.remove_extra_ws:
        flags |= TVOC_FLAG_REMOVE_EXTRA_WS
    if norm_fullwidth:
        flags |= TVOC_FLAG_NORM_FULLWIDTH

    normal_scores = [p.score for p in vocab.pieces if _SPM_TO_TVOC_KIND.get(p.kind, p.kind) == 0]
    min_normal = min(normal_scores) if normal_scores else 0.0

    pool = bytearray()
    records = bytearray()
    encoded_texts: list[bytes] = []
    for piece in vocab.pieces:
        raw_txt = piece.text.encode("utf-8")
        encoded_texts.append(raw_txt)
        off = len(pool)
        pool.extend(raw_txt)
        tvoc_kind = _SPM_TO_TVOC_KIND.get(piece.kind, 0)
        records.extend(struct.pack("<IHBBf", off, len(raw_txt), tvoc_kind, 0, piece.score))

    sorted_ids = sorted(range(vocab_size), key=lambda pid: (encoded_texts[pid], pid))
    sorted_tbl = struct.pack(f"<{vocab_size}I", *sorted_ids)

    piece_off = TVOC_HEADER_BYTES
    sorted_off = piece_off + vocab_size * TVOC_RECORD_BYTES
    pool_off = sorted_off + vocab_size * 4
    total_bytes = pool_off + len(pool)

    header = struct.pack(
        "<IHBBIIIIIIIfIIII",
        TVOC_MAGIC,
        TVOC_VERSION,
        vocab.model_type,
        flags,
        total_bytes,
        0,
        vocab_size,
        vocab.unk_id,
        vocab.bos_id,
        vocab.eos_id,
        vocab.pad_id,
        min_normal,
        piece_off,
        sorted_off,
        pool_off,
        0,
    )
    buf = bytearray(header)
    buf.extend(records)
    buf.extend(sorted_tbl)
    buf.extend(pool)
    crc = zlib.crc32(buf) & 0xFFFFFFFF
    struct.pack_into("<I", buf, 12, crc)
    return bytes(buf)


def pack_tslt(shortlist: BergamotShortlist, vocab_size: int) -> bytes:
    """Serialize a `BergamotShortlist` into a `TSLT` v1 CSR shortlist blob."""
    if len(shortlist.row_offsets) != vocab_size + 1:
        msg = f"shortlist row_offsets len {len(shortlist.row_offsets)} != {vocab_size + 1}"
        raise ValueError(msg)
    first_k = min(shortlist.first_k, vocab_size, 0xFFFF)

    out_offs = np.zeros(vocab_size + 1, dtype="<u4")
    cand_chunks: list[np.ndarray] = []
    total_entries = 0
    for src_id in range(vocab_size):
        out_offs[src_id] = total_entries
        s0 = int(shortlist.row_offsets[src_id])
        s1 = int(shortlist.row_offsets[src_id + 1])
        if s1 > s0:
            row = np.unique(shortlist.target_ids[s0:s1]).astype("<u4", copy=False)
            cand_chunks.append(row)
            total_entries += int(row.size)
    out_offs[vocab_size] = total_entries

    entries = (
        np.concatenate(cand_chunks).astype("<u4", copy=False)
        if cand_chunks
        else np.empty(0, dtype="<u4")
    )
    header = struct.pack(
        "<IHHI",
        TSLT_MAGIC,
        TSLT_VERSION,
        first_k,
        vocab_size,
    ) + struct.pack("<I", total_entries)
    return header + out_offs.tobytes() + entries.tobytes()


def _slot_i8(tensor: MarianTensor, rows: int, cols: int) -> SlotTensor:
    """Build an affine `int8` `SlotTensor` verifying expected dimensions."""
    mat, scale = weight_as_i8(tensor)
    if tuple(mat.shape) != (rows, cols):
        msg = f"{tensor.name} shape {tuple(mat.shape)} != expected ({rows}, {cols})"
        raise ValueError(msg)
    return SlotTensor(
        dtype=TMNT_DTYPE_I8,
        rows=rows,
        cols=cols,
        scale=scale,
        zero_point=0,
        payload=np.ascontiguousarray(mat, dtype=np.int8),
    )


def _slot_f32_req(tensor: MarianTensor, elems: int) -> SlotTensor:
    """Build a required `float32` vector `SlotTensor`."""
    flat = np.ascontiguousarray(tensor.data, dtype="<f4").reshape(-1)
    if flat.size != elems:
        msg = f"{tensor.name} size {flat.size} != expected {elems}"
        raise ValueError(msg)
    return SlotTensor(
        dtype=TMNT_DTYPE_F32,
        rows=elems,
        cols=1,
        scale=1.0,
        zero_point=0,
        payload=flat,
    )


def _slot_f32_opt(tensors: dict[str, MarianTensor], name: str, elems: int) -> SlotTensor:
    """Build an optional `float32` bias slot (`dtype=none` if omitted)."""
    t = tensors.get(name)
    if t is None:
        return SlotTensor(dtype=TMNT_DTYPE_NONE, rows=0, cols=0, scale=1.0, zero_point=0)
    return _slot_f32_req(t, elems)


def _append_attn_slots(
    slots: list[SlotTensor], tensors: dict[str, MarianTensor], prefix: str, d_model: int
) -> None:
    """Append 10 standard attention + LayerNorm slots for `prefix`."""
    for suffix in ("Wq", "Wk", "Wv", "Wo"):
        b_suffix = "b" + suffix[1:]
        slots.append(_slot_i8(tensors[f"{prefix}_{suffix}"], d_model, d_model))
        slots.append(_slot_f32_opt(tensors, f"{prefix}_{b_suffix}", d_model))
    slots.append(_slot_f32_req(tensors[f"{prefix}_Wo_ln_scale"], d_model))
    slots.append(_slot_f32_req(tensors[f"{prefix}_Wo_ln_bias"], d_model))


def _append_ffn_slots(
    slots: list[SlotTensor],
    tensors: dict[str, MarianTensor],
    prefix: str,
    d_model: int,
    d_ff: int,
) -> None:
    """Append 6 FFN + LayerNorm slots for `prefix`."""
    slots.append(_slot_i8(tensors[f"{prefix}_W1"], d_ff, d_model))
    slots.append(_slot_f32_opt(tensors, f"{prefix}_b1", d_ff))
    slots.append(_slot_i8(tensors[f"{prefix}_W2"], d_model, d_ff))
    slots.append(_slot_f32_opt(tensors, f"{prefix}_b2", d_model))
    slots.append(_slot_f32_req(tensors[f"{prefix}_ffn_ln_scale"], d_model))
    slots.append(_slot_f32_req(tensors[f"{prefix}_ffn_ln_bias"], d_model))


def _build_tmnt_slots(
    model: MarianModel,
    vocab: SpmVocab,
    max_seq: int,
) -> tuple[list[SlotTensor], int, int, int, int, int, int]:
    """Assemble ordered `TMNT` slot descriptors from a parsed Bergamot model."""
    cfg = model.config
    ts = model.tensors
    d_model = int(cfg["dim-emb"])  # type: ignore[arg-type]
    d_ff = int(cfg["transformer-dim-ffn"])  # type: ignore[arg-type]
    num_heads = int(cfg["transformer-heads"])  # type: ignore[arg-type]
    num_enc = int(cfg["enc-depth"])  # type: ignore[arg-type]
    num_dec = int(cfg["dec-depth"])  # type: ignore[arg-type]
    vocab_size = len(vocab.pieces)

    is_ssru = cfg.get("dec-cell") == "ssru" or cfg.get("transformer-decoder-autoreg") == "rnn"
    tied = bool(cfg.get("tied-embeddings-all", False) or cfg.get("tied-embeddings", False))
    pre_norm = "n" in str(cfg.get("transformer-preprocess", ""))

    flags = TMNT_FLAG_SCALE_EMBED
    if pre_norm:
        flags |= TMNT_FLAG_PRE_NORM
    if tied:
        flags |= TMNT_FLAG_TIED_EMBED
    if is_ssru:
        flags |= TMNT_FLAG_DECODER_SSRU
    if vocab.bos_id == SPM_NONE_ID:
        flags |= TMNT_FLAG_ZERO_BOS

    wemb_slot = _slot_i8(ts["Wemb"], vocab_size, d_model)
    pos_arr = build_sinusoidal_pos_embed(max_seq, d_model)
    pos_slot = SlotTensor(
        dtype=TMNT_DTYPE_F32, rows=max_seq, cols=d_model, scale=1.0, zero_point=0, payload=pos_arr
    )
    if tied:
        lm_w_slot = SlotTensor(
            dtype=TMNT_DTYPE_I8,
            rows=vocab_size,
            cols=d_model,
            scale=wemb_slot.scale,
            zero_point=0,
            alias_slot=0,
        )
    else:
        lm_w_slot = _slot_i8(ts["decoder_ff_logit_out_W"], vocab_size, d_model)
    lm_b_slot = _slot_f32_opt(ts, "decoder_ff_logit_out_b", vocab_size)

    none_slot = SlotTensor(dtype=TMNT_DTYPE_NONE, rows=0, cols=0, scale=1.0, zero_point=0)
    slots: list[SlotTensor] = [
        wemb_slot,
        pos_slot,
        lm_w_slot,
        lm_b_slot,
        none_slot,
        none_slot,
        none_slot,
        none_slot,
    ]
    for el in range(1, num_enc + 1):
        _append_attn_slots(slots, ts, f"encoder_l{el}_self", d_model)
        _append_ffn_slots(slots, ts, f"encoder_l{el}_ffn", d_model, d_ff)
    for dl in range(1, num_dec + 1):
        if is_ssru:
            p = f"decoder_l{dl}_rnn"
            slots.append(_slot_i8(ts[f"{p}_W"], d_model, d_model))
            slots.append(_slot_i8(ts[f"{p}_Wf"], d_model, d_model))
            slots.append(_slot_f32_opt(tensors=ts, name=f"{p}_bf", elems=d_model))
            slots.append(_slot_f32_req(ts[f"{p}_ffn_ln_scale"], d_model))
            slots.append(_slot_f32_req(ts[f"{p}_ffn_ln_bias"], d_model))
        else:
            _append_attn_slots(slots, ts, f"decoder_l{dl}_self", d_model)
        _append_attn_slots(slots, ts, f"decoder_l{dl}_context", d_model)
        _append_ffn_slots(slots, ts, f"decoder_l{dl}_ffn", d_model, d_ff)

    return slots, flags, num_heads, num_enc, num_dec, d_model, d_ff


def _encode_tmnt_slots(
    slots: list[SlotTensor], weights_off: int
) -> tuple[bytearray, list[bytes], int]:
    """Serialize tensor descriptors and aligned payload chunks from `slots`."""
    tensor_count = len(slots)
    cursor = weights_off
    descs = bytearray()
    chunks: list[bytes] = []
    slot_offs = [0] * tensor_count
    slot_sizes = [0] * tensor_count

    for idx, slot in enumerate(slots):
        if slot.dtype == TMNT_DTYPE_NONE:
            descs.extend(struct.pack("<IIIIfBbbb", 0, 0, 0, 0, 1.0, 0, 0, 0, 0))
            continue
        if slot.alias_slot is not None:
            src_idx = slot.alias_slot
            off = slot_offs[src_idx]
            sz = slot_sizes[src_idx]
            slot_offs[idx] = off
            slot_sizes[idx] = sz
        else:
            aligned = _align_up4(cursor)
            if aligned > cursor:
                chunks.append(bytes(aligned - cursor))
                cursor = aligned
            if slot.payload is None:
                msg = f"non-alias slot {idx} missing payload array"
                raise ValueError(msg)
            raw_bytes = slot.payload.tobytes()
            off = cursor
            sz = len(raw_bytes)
            slot_offs[idx] = off
            slot_sizes[idx] = sz
            chunks.append(raw_bytes)
            cursor += sz
        descs.extend(
            struct.pack(
                "<IIIIfBbbb",
                off,
                sz,
                slot.rows,
                slot.cols,
                slot.scale,
                slot.dtype,
                slot.zero_point,
                0,
                0,
            )
        )

    weights_end = _align_up4(cursor)
    if weights_end > cursor:
        chunks.append(bytes(weights_end - cursor))
    return descs, chunks, weights_end


def pack_tmnt(
    model: MarianModel,
    vocab: SpmVocab,
    *,
    nmt_model_id: int,
    max_seq: int = DEFAULT_MAX_SEQ,
    shortlist_blob: bytes | None = None,
) -> bytes:
    """Serialize a `MarianModel` (+ optional `TSLT`) into a `TMNT` v1 container."""
    slots, flags, num_heads, num_enc, num_dec, d_model, d_ff = _build_tmnt_slots(
        model, vocab, max_seq
    )
    vocab_size = len(vocab.pieces)
    eos_id = vocab.eos_id if vocab.eos_id != SPM_NONE_ID else 0
    unk_id = vocab.unk_id if vocab.unk_id != SPM_NONE_ID else 0
    bos_id = vocab.bos_id if vocab.bos_id != SPM_NONE_ID else eos_id
    pad_id = vocab.pad_id if vocab.pad_id != SPM_NONE_ID else eos_id

    tensor_count = len(slots)
    dir_end = TMNT_HEADER_BYTES + tensor_count * TMNT_DESC_BYTES
    weights_off = _align_up4(dir_end)
    descs, weight_chunks, weights_end = _encode_tmnt_slots(slots, weights_off)
    weights_bytes = weights_end - weights_off

    sl_off = weights_end if shortlist_blob is not None else 0
    sl_bytes = len(shortlist_blob) if shortlist_blob is not None else 0
    total_bytes = weights_end + sl_bytes

    header = struct.pack(
        "<IHHIIIBBBBHHHHI4If7I",
        TMNT_MAGIC,
        TMNT_VERSION,
        flags,
        total_bytes,
        0,
        0,
        nmt_model_id,
        num_heads,
        num_enc,
        num_dec,
        d_model,
        d_ff,
        max_seq,
        0,
        vocab_size,
        unk_id,
        bos_id,
        eos_id,
        pad_id,
        LN_EPS_MARIAN,
        0,
        0,
        sl_off,
        sl_bytes,
        tensor_count,
        weights_off,
        weights_bytes,
    )
    out = bytearray(header)
    out.extend(descs)
    if weights_off > dir_end:
        out.extend(bytes(weights_off - dir_end))
    for chunk in weight_chunks:
        out.extend(chunk)
    if shortlist_blob is not None:
        out.extend(shortlist_blob)

    payload_crc = zlib.crc32(out[weights_off:total_bytes]) & 0xFFFFFFFF
    struct.pack_into("<I", out, 16, payload_crc)
    meta_crc = zlib.crc32(out[0:dir_end]) & 0xFFFFFFFF
    struct.pack_into("<I", out, 12, meta_crc)
    return bytes(out)
