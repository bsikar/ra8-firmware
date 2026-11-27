# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Host reference tokenizer and int8 `TMNT` Marian executor matching Zig."""

from __future__ import annotations

import hashlib
import math
import struct
import zlib
from dataclasses import dataclass

import numpy as np
from containers import (
    TMNT_DESC_BYTES,
    TMNT_DTYPE_F32,
    TMNT_DTYPE_I8,
    TMNT_DTYPE_NONE,
    TMNT_FLAG_DECODER_SSRU,
    TMNT_FLAG_PRE_NORM,
    TMNT_FLAG_SCALE_EMBED,
    TMNT_FLAG_ZERO_BOS,
    TMNT_HEADER_BYTES,
    TMNT_MAGIC,
    TSLT_HEADER_BYTES,
    TSLT_MAGIC,
    TVOC_FLAG_ADD_DUMMY_PREFIX,
    TVOC_FLAG_BYTE_FALLBACK,
    TVOC_FLAG_ESCAPE_WS,
    TVOC_FLAG_NORM_FULLWIDTH,
    TVOC_FLAG_REMOVE_EXTRA_WS,
    TVOC_HEADER_BYTES,
    TVOC_MAGIC,
    TVOC_RECORD_BYTES,
)

K_METASPACE = "\u2581"
_KIND_NORMAL = 0
_KIND_UNKNOWN = 1
_KIND_CONTROL = 2
_KIND_USER_DEFINED = 3
_KIND_BYTE = 4
_KIND_UNUSED = 5
_BYTE_TOKEN_LEN = 6
_FW_ASCII_LO = 0xFF01
_FW_ASCII_HI = 0xFF5E
_FW_SPACE = 0x3000
_NBSP = 0x00A0
_MAX_PIECE_CP = 48
_SIGMOID_CLAMP = 87.0
_ACT_QMAX = 127.0
_ROUND_HALF = 0.5


@dataclass(frozen=True)
class TvocPiece:
    """Validated piece entry parsed back from a `TVOC` binary buffer."""

    text: str
    kind: int
    score: float


@dataclass(frozen=True)
class TvocReader:
    """In-memory reader + Unigram/decode implementation over serialized `TVOC`."""

    flags: int
    model_type: int
    vocab_size: int
    unk_id: int
    bos_id: int
    eos_id: int
    pad_id: int
    min_normal_score: float
    pieces: list[TvocPiece]
    piece_to_id: dict[str, int]
    byte_lookup: dict[int, int]
    user_defined: list[tuple[str, int]]

    @classmethod
    def open(cls, raw: bytes) -> TvocReader:
        """Validate header + CRC-32 of `TVOC` bytes and index vocabulary records."""
        if len(raw) < TVOC_HEADER_BYTES:
            msg = "TVOC buffer shorter than 56-byte header"
            raise ValueError(msg)
        magic, ver, mtype, flags, total_bytes, crc = struct.unpack_from("<IHBBII", raw, 0)
        if magic != TVOC_MAGIC or ver != 1:
            msg = f"invalid TVOC magic/version ({magic:#x}, {ver})"
            raise ValueError(msg)
        if len(raw) != total_bytes:
            msg = f"TVOC len {len(raw)} != header total_bytes {total_bytes}"
            raise ValueError(msg)
        check_buf = bytearray(raw)
        check_buf[12:16] = b"\x00\x00\x00\x00"
        if (zlib.crc32(check_buf) & 0xFFFFFFFF) != crc:
            msg = "TVOC CRC-32 mismatch"
            raise ValueError(msg)

        vocab_size, unk_id, bos_id, eos_id, pad_id, min_sc, p_off, _, pool_off, _ = (
            struct.unpack_from("<IIIIIfIIII", raw, 16)
        )
        pieces: list[TvocPiece] = []
        piece_to_id: dict[str, int] = {}
        byte_lookup: dict[int, int] = {}
        user_defined: list[tuple[str, int]] = []
        for idx in range(vocab_size):
            t_off, t_len, kind, _, score = struct.unpack_from(
                "<IHBBf", raw, p_off + idx * TVOC_RECORD_BYTES
            )
            txt = raw[pool_off + t_off : pool_off + t_off + t_len].decode("utf-8")
            pieces.append(TvocPiece(text=txt, kind=kind, score=score))
            if kind == _KIND_NORMAL and txt not in piece_to_id:
                piece_to_id[txt] = idx
            elif kind == _KIND_USER_DEFINED:
                user_defined.append((txt, idx))
            elif (
                kind == _KIND_BYTE
                and len(txt) == _BYTE_TOKEN_LEN
                and txt.startswith("<0x")
                and txt.endswith(">")
            ):
                byte_lookup[int(txt[3:5], 16)] = idx

        user_defined.sort(key=lambda item: len(item[0]), reverse=True)
        return cls(
            flags=flags,
            model_type=mtype,
            vocab_size=vocab_size,
            unk_id=unk_id,
            bos_id=bos_id,
            eos_id=eos_id,
            pad_id=pad_id,
            min_normal_score=min_sc,
            pieces=pieces,
            piece_to_id=piece_to_id,
            byte_lookup=byte_lookup,
            user_defined=user_defined,
        )

    @staticmethod
    def _normalize_char(raw_ch: str, *, norm_fw: bool) -> str:
        """Map fullwidth ASCII/ideographic spaces to ASCII when enabled."""
        if not norm_fw:
            return raw_ch
        code = ord(raw_ch)
        if _FW_ASCII_LO <= code <= _FW_ASCII_HI:
            return chr(code - _FW_ASCII_LO + 0x21)
        if code in (_FW_SPACE, _NBSP):
            return " "
        return raw_ch

    def normalize(self, text: str) -> str:
        """Apply the exact `TVOC` normalizer rules matching `tokenizer.zig`."""
        remove_extra = (self.flags & TVOC_FLAG_REMOVE_EXTRA_WS) != 0
        escape_ws = (self.flags & TVOC_FLAG_ESCAPE_WS) != 0
        add_dummy = (self.flags & TVOC_FLAG_ADD_DUMMY_PREFIX) != 0
        norm_fw = (self.flags & TVOC_FLAG_NORM_FULLWIDTH) != 0
        ws_rep = K_METASPACE if escape_ws else " "

        out: list[str] = []
        has_content = False
        pending_ws = False
        for raw_ch in text:
            ch = self._normalize_char(raw_ch, norm_fw=norm_fw)
            if ch in (" ", "\t", "\n", "\r", "\x0b", "\x0c", "\u00a0", "\u3000"):
                if remove_extra:
                    if has_content:
                        pending_ws = True
                else:
                    if not has_content and add_dummy:
                        out.append(ws_rep)
                    out.append(ws_rep)
                    has_content = True
            else:
                if not has_content:
                    if add_dummy:
                        out.append(ws_rep)
                    has_content = True
                elif pending_ws:
                    out.append(ws_rep)
                    pending_ws = False
                out.append(ch)
        return "".join(out)

    def _unigram_dp_pass(self, seg: str) -> tuple[list[int], list[int], list[bool]]:
        """Run Viterbi forward DP pass over normalized segment `seg`."""
        unk_score = self.min_normal_score - 10.0
        n = len(seg)
        scores = [-1e30] * (n + 1)
        toks = [self.unk_id] * (n + 1)
        prevs = [-1] * (n + 1)
        is_unk = [False] * (n + 1)
        scores[0] = 0.0

        for pos in range(n):
            if prevs[pos] < 0 and pos > 0:
                continue
            has_single = False
            for end in range(pos + 1, min(n + 1, pos + _MAX_PIECE_CP + 1)):
                pid = self.piece_to_id.get(seg[pos:end])
                if pid is None:
                    continue
                if end == pos + 1:
                    has_single = True
                cand = scores[pos] + self.pieces[pid].score
                if prevs[end] < 0 or cand > scores[end]:
                    scores[end] = cand
                    toks[end] = pid
                    prevs[end] = pos
                    is_unk[end] = False
            if not has_single:
                cand = scores[pos] + unk_score
                if prevs[pos + 1] < 0 or cand > scores[pos + 1]:
                    scores[pos + 1] = cand
                    toks[pos + 1] = self.unk_id
                    prevs[pos + 1] = pos
                    is_unk[pos + 1] = True
        return toks, prevs, is_unk

    def _encode_unigram_segment(self, seg: str) -> list[int]:
        """Run SentencePiece Viterbi Unigram DP matching `tokenizer.zig`."""
        if not seg:
            return []
        toks, prevs, is_unk = self._unigram_dp_pass(seg)
        spans: list[tuple[int, int, int, bool]] = []
        cur = len(seg)
        while cur > 0:
            prv = prevs[cur]
            spans.append((prv, cur, toks[cur], is_unk[cur]))
            cur = prv
        spans.reverse()

        out: list[int] = []
        byte_fb = (self.flags & TVOC_FLAG_BYTE_FALLBACK) != 0
        last_unk = False
        for start, stop, tid, unk in spans:
            if not unk:
                out.append(tid)
                last_unk = False
            elif byte_fb:
                out.extend(
                    self.byte_lookup.get(byte, self.unk_id)
                    for byte in seg[start:stop].encode("utf-8")
                )
                last_unk = False
            elif not last_unk:
                out.append(self.unk_id)
                last_unk = True
        return out

    def encode(self, text: str, *, add_eos: bool = True) -> list[int]:
        """Encode raw UTF-8 text using the `TVOC` Unigram + user-defined rules."""
        norm = self.normalize(text)
        ids: list[int] = []
        pos = 0
        seg_start = 0
        while pos < len(norm):
            matched_ud: tuple[str, int] | None = None
            for u_txt, u_id in self.user_defined:
                if norm.startswith(u_txt, pos):
                    matched_ud = (u_txt, u_id)
                    break
            if matched_ud is not None:
                if pos > seg_start:
                    ids.extend(self._encode_unigram_segment(norm[seg_start:pos]))
                ids.append(matched_ud[1])
                pos += len(matched_ud[0])
                seg_start = pos
            else:
                pos += 1
        if pos > seg_start:
            ids.extend(self._encode_unigram_segment(norm[seg_start:pos]))
        if add_eos:
            ids.append(self.eos_id)
        return ids

    def decode(self, ids: list[int]) -> str:
        """Decode token IDs back to UTF-8 text matching `tokenizer.zig`."""
        strip_dummy = (self.flags & TVOC_FLAG_ADD_DUMMY_PREFIX) != 0 and (
            self.flags & TVOC_FLAG_ESCAPE_WS
        ) != 0
        escape_ws = (self.flags & TVOC_FLAG_ESCAPE_WS) != 0
        parts: list[str] = []
        byte_run = bytearray()
        saw_content = False
        for tid in ids:
            piece = self.pieces[tid]
            if piece.kind != _KIND_BYTE and byte_run:
                parts.append(byte_run.decode("utf-8", errors="replace"))
                byte_run.clear()
                saw_content = True
            if piece.kind in (_KIND_CONTROL, _KIND_UNUSED):
                continue
            if piece.kind == _KIND_UNKNOWN:
                parts.append("<unk>")
                saw_content = True
            elif piece.kind in (_KIND_NORMAL, _KIND_USER_DEFINED):
                txt = piece.text
                if not saw_content and strip_dummy and txt.startswith(K_METASPACE):
                    txt = txt[len(K_METASPACE) :]
                parts.append(txt.replace(K_METASPACE, " ") if escape_ws else txt)
                saw_content = True
            elif piece.kind == _KIND_BYTE:
                byte_run.append(int(piece.text[3:5], 16))
        if byte_run:
            parts.append(byte_run.decode("utf-8", errors="replace"))
        return "".join(parts)


@dataclass(frozen=True)
class TmntSlotView:
    """One tensor slice unpacked directly from a validated `TMNT` buffer."""

    dtype: int
    rows: int
    cols: int
    scale: float
    zero_point: int
    array: np.ndarray | None


class TmntRunner:
    """Host quantized int8 executor mirroring `libs/ra8_translate/src/internal/marian.zig`."""

    def __init__(self, raw: bytes) -> None:
        """Parse, verify CRC-32, and bind all tensors from a `TMNT` buffer."""
        if len(raw) < TMNT_HEADER_BYTES:
            msg = "TMNT buffer shorter than 84-byte header"
            raise ValueError(msg)
        vals = struct.unpack_from("<IHHIIIBBBBHHHHI4If7I", raw, 0)
        (
            magic,
            ver,
            self.model_flags,
            total_bytes,
            meta_crc,
            payload_crc,
            self.nmt_model,
            self.num_heads,
            self.num_enc_layers,
            self.num_dec_layers,
            self.d_model,
            self.d_ff,
            self.max_seq,
            _,
            self.vocab_size,
            self.unk_id,
            self.bos_id,
            self.eos_id,
            self.pad_id,
            self.ln_eps,
            _,
            _,
            sl_off,
            sl_bytes,
            tensor_count,
            weights_off,
            weights_bytes,
        ) = vals
        if magic != TMNT_MAGIC or ver != 1 or len(raw) != total_bytes:
            msg = f"invalid TMNT header (magic={magic:#x}, ver={ver}, len={len(raw)})"
            raise ValueError(msg)
        dir_end = TMNT_HEADER_BYTES + tensor_count * TMNT_DESC_BYTES
        zeroed = bytearray(raw[:dir_end])
        zeroed[12:16] = b"\x00\x00\x00\x00"
        if (zlib.crc32(zeroed) & 0xFFFFFFFF) != meta_crc:
            msg = "TMNT meta_crc32 mismatch"
            raise ValueError(msg)
        if (zlib.crc32(raw[weights_off:total_bytes]) & 0xFFFFFFFF) != payload_crc:
            msg = "TMNT payload_crc32 mismatch"
            raise ValueError(msg)
        self.weights_bytes = weights_bytes
        self.slots: list[TmntSlotView] = []
        for idx in range(tensor_count):
            off, _, rows, cols, scale, dtype, zp, _, _ = struct.unpack_from(
                "<IIIIfBbbb", raw, TMNT_HEADER_BYTES + idx * TMNT_DESC_BYTES
            )
            arr: np.ndarray | None = None
            if dtype == TMNT_DTYPE_I8:
                arr = np.frombuffer(raw, dtype=np.int8, count=rows * cols, offset=off).reshape(
                    (rows, cols)
                )
            elif dtype == TMNT_DTYPE_F32:
                arr = np.frombuffer(raw, dtype="<f4", count=rows * cols, offset=off).astype(
                    np.float32
                )
                if cols > 1:
                    arr = arr.reshape((rows, cols))
            self.slots.append(TmntSlotView(dtype, rows, cols, scale, zp, arr))

        self.shortlist_first_k = 0
        self.shortlist_offs: np.ndarray | None = None
        self.shortlist_entries: np.ndarray | None = None
        if sl_bytes > 0:
            sl_raw = raw[sl_off : sl_off + sl_bytes]
            s_magic, _, s_fk, s_voc, _ = struct.unpack_from("<IHHII", sl_raw, 0)
            if s_magic != TSLT_MAGIC or s_voc != self.vocab_size:
                msg = "invalid embedded TSLT section"
                raise ValueError(msg)
            self.shortlist_first_k = s_fk
            self.shortlist_offs = np.frombuffer(
                sl_raw, dtype="<u4", count=s_voc + 1, offset=TSLT_HEADER_BYTES
            )
            self.shortlist_entries = np.frombuffer(
                sl_raw, dtype="<u4", offset=TSLT_HEADER_BYTES + (s_voc + 1) * 4
            )

    def _matvec(self, x: np.ndarray, w_slot: int, b_slot: int | None) -> np.ndarray:
        """Execute symmetric activation-quantized `int8` matrix-vector multiply."""
        w = self.slots[w_slot]
        assert_arr = w.array
        if assert_arr is None:
            msg = f"missing array payload in slot {w_slot}"
            raise ValueError(msg)
        x_f32 = x.astype(np.float32, copy=False)
        amax = float(np.max(np.abs(x_f32)))
        if amax == 0.0:
            out = np.zeros(w.rows, dtype=np.float32)
        else:
            x_scale = np.float32(amax / _ACT_QMAX)
            inv_scale = np.float32(_ACT_QMAX / amax)
            scaled = x_f32 * inv_scale
            mag = np.trunc(
                np.clip(np.abs(scaled) + np.float32(_ROUND_HALF), 0.0, _ACT_QMAX)
            ).astype(np.int32)
            xq = np.where(scaled >= 0.0, mag, -mag)
            dot = assert_arr.astype(np.int32) @ xq
            if w.zero_point != 0:
                dot = dot - np.int32(w.zero_point) * int(np.sum(xq))
            out = dot.astype(np.float32) * (x_scale * np.float32(w.scale))
        if b_slot is not None:
            b = self.slots[b_slot]
            if b.dtype == TMNT_DTYPE_F32 and b.array is not None:
                out = out + b.array
        return out

    def _ln(self, x: np.ndarray, g_slot: int, b_slot: int) -> np.ndarray:
        """Apply LayerNorm matching `marian_ops.zig` (`f64` reductions, `eps = hdr.ln_eps`)."""
        g = self.slots[g_slot].array
        b = self.slots[b_slot].array
        if g is None or b is None:
            msg = f"missing LN vector in slots ({g_slot}, {b_slot})"
            raise ValueError(msg)
        xf = x.astype(np.float32, copy=False)
        mean = np.float32(np.sum(xf.astype(np.float64)) / float(xf.size))
        diff = (xf - mean).astype(np.float64)
        var = np.float32(np.sum(diff * diff) / float(xf.size))
        inv = np.float32(1.0) / np.sqrt(var + np.float32(self.ln_eps))
        return ((xf - mean) * inv) * g + b

    def _embed(self, tok_id: int, pos: int, *, zero_token: bool) -> np.ndarray:
        """Embed token + position exactly as `marian_ops.zig`."""
        pos_slot = self.slots[1]
        if pos_slot.dtype != TMNT_DTYPE_F32 or pos_slot.array is None:
            msg = "reference runner expects explicit slot-1 pos_embed table"
            raise ValueError(msg)
        pv = pos_slot.array[pos]
        if zero_token:
            return pv.copy()
        emb = self.slots[0]
        if emb.array is None:
            msg = "missing embed_w in slot 0"
            raise ValueError(msg)
        amp = (
            np.float32(math.sqrt(self.d_model))
            if (self.model_flags & TMNT_FLAG_SCALE_EMBED)
            else np.float32(1.0)
        )
        row = (emb.array[tok_id].astype(np.int32) - emb.zero_point).astype(np.float32)
        return (row * np.float32(emb.scale)) * amp + pv

    def _attend(self, q_vec: np.ndarray, k_seq: np.ndarray, v_seq: np.ndarray) -> np.ndarray:
        """Compute multi-head scaled dot-product attention over `k_seq`, `v_seq`."""
        heads = self.num_heads
        hd = self.d_model // heads
        inv_sq = np.float32(1.0 / math.sqrt(hd))
        q_h = q_vec.reshape(heads, hd)
        k_h = k_seq.reshape(-1, heads, hd)
        v_h = v_seq.reshape(-1, heads, hd)
        scores = np.einsum("hd,thd->ht", q_h, k_h) * inv_sq
        scores = scores - scores.max(axis=1, keepdims=True)
        probs = np.exp(scores)
        probs = probs / probs.sum(axis=1, keepdims=True)
        ctx = np.einsum("ht,thd->hd", probs, v_h)
        return ctx.reshape(self.d_model).astype(np.float32)

    def _encode(self, src_ids: list[int]) -> np.ndarray:
        """Run all encoder layers over `src_ids` and return `[T_src, d_model]`."""
        pre_norm = (self.model_flags & TMNT_FLAG_PRE_NORM) != 0
        enc = np.stack([self._embed(tok, pos, zero_token=False) for pos, tok in enumerate(src_ids)])
        for el in range(self.num_enc_layers):
            base = 8 + el * 16
            x_in = (
                np.stack([self._ln(enc[s], base + 8, base + 9) for s in range(len(src_ids))])
                if pre_norm
                else enc
            )
            k_seq = np.stack(
                [self._matvec(x_in[s], base + 2, base + 3) for s in range(len(src_ids))]
            )
            v_seq = np.stack(
                [self._matvec(x_in[s], base + 4, base + 5) for s in range(len(src_ids))]
            )
            sub = np.stack(
                [
                    self._matvec(
                        self._attend(self._matvec(x_in[s], base + 0, base + 1), k_seq, v_seq),
                        base + 6,
                        base + 7,
                    )
                    for s in range(len(src_ids))
                ]
            )
            enc = enc + sub
            if not pre_norm:
                enc = np.stack([self._ln(enc[s], base + 8, base + 9) for s in range(len(src_ids))])
            fb = base + 10
            for s in range(len(src_ids)):
                fin = self._ln(enc[s], fb + 4, fb + 5) if pre_norm else enc[s]
                hid = np.maximum(np.float32(0.0), self._matvec(fin, fb + 0, fb + 1))
                enc[s] = enc[s] + self._matvec(hid, fb + 2, fb + 3)
                if not pre_norm:
                    enc[s] = self._ln(enc[s], fb + 4, fb + 5)
        if self.slots[4].dtype == TMNT_DTYPE_F32:
            enc = np.stack([self._ln(enc[s], 4, 5) for s in range(len(src_ids))])
        return enc

    def _run_decoder_layer(
        self,
        dl: int,
        x: np.ndarray,
        dec_base: int,
        state: dict[str, object],
    ) -> np.ndarray:
        """Execute one decoder layer (SSRU or self-attn, cross-attn, FFN)."""
        is_ssru = bool(state["is_ssru"])
        pre_norm = bool(state["pre_norm"])
        cells: list[np.ndarray] = state["cells"]  # type: ignore[assignment]
        self_k: list[list[np.ndarray]] = state["self_k"]  # type: ignore[assignment]
        self_v: list[list[np.ndarray]] = state["self_v"]  # type: ignore[assignment]
        cross_k: list[np.ndarray] = state["cross_k"]  # type: ignore[assignment]
        cross_v: list[np.ndarray] = state["cross_v"]  # type: ignore[assignment]

        if is_ssru:
            xin = self._ln(x, dec_base + 3, dec_base + 4) if pre_norm else x
            u = self._matvec(xin, dec_base + 0, None)
            fg = self._matvec(xin, dec_base + 1, dec_base + 2)
            gate = np.float32(1.0) / (
                np.float32(1.0) + np.exp(-np.clip(fg, -_SIGMOID_CLAMP, _SIGMOID_CLAMP))
            )
            c_new = gate * cells[dl] + (np.float32(1.0) - gate) * u
            cells[dl] = c_new
            x = x + np.maximum(np.float32(0.0), c_new)
            if not pre_norm:
                x = self._ln(x, dec_base + 3, dec_base + 4)
            cb = dec_base + 5
        else:
            xin = self._ln(x, dec_base + 8, dec_base + 9) if pre_norm else x
            self_k[dl].append(self._matvec(xin, dec_base + 2, dec_base + 3))
            self_v[dl].append(self._matvec(xin, dec_base + 4, dec_base + 5))
            q = self._matvec(xin, dec_base + 0, dec_base + 1)
            ctx = self._attend(q, np.stack(self_k[dl]), np.stack(self_v[dl]))
            x = x + self._matvec(ctx, dec_base + 6, dec_base + 7)
            if not pre_norm:
                x = self._ln(x, dec_base + 8, dec_base + 9)
            cb = dec_base + 10

        cin = self._ln(x, cb + 8, cb + 9) if pre_norm else x
        ctx = self._attend(self._matvec(cin, cb + 0, cb + 1), cross_k[dl], cross_v[dl])
        x = x + self._matvec(ctx, cb + 6, cb + 7)
        if not pre_norm:
            x = self._ln(x, cb + 8, cb + 9)

        fb = cb + 10
        fin = self._ln(x, fb + 4, fb + 5) if pre_norm else x
        hid = np.maximum(np.float32(0.0), self._matvec(fin, fb + 0, fb + 1))
        x = x + self._matvec(hid, fb + 2, fb + 3)
        if not pre_norm:
            x = self._ln(x, fb + 4, fb + 5)
        return x

    def greedy_decode_with_logits(
        self, src_ids: list[int], max_tgt_len: int, *, top_k: int = 8
    ) -> list[dict[str, object]]:
        """Run greedy decode and return per-step chosen token ID + logits."""
        enc = self._encode(src_ids)
        is_ssru = (self.model_flags & TMNT_FLAG_DECODER_SSRU) != 0
        pre_norm = (self.model_flags & TMNT_FLAG_PRE_NORM) != 0
        zero_bos = (self.model_flags & TMNT_FLAG_ZERO_BOS) != 0
        dec_stride = 21 if is_ssru else 26
        dec_start = 8 + self.num_enc_layers * 16

        cross_k: list[np.ndarray] = []
        cross_v: list[np.ndarray] = []
        for dl in range(self.num_dec_layers):
            cb = dec_start + dl * dec_stride + (5 if is_ssru else 10)
            cross_k.append(
                np.stack([self._matvec(enc[s], cb + 2, cb + 3) for s in range(len(src_ids))])
            )
            cross_v.append(
                np.stack([self._matvec(enc[s], cb + 4, cb + 5) for s in range(len(src_ids))])
            )

        state: dict[str, object] = {
            "is_ssru": is_ssru,
            "pre_norm": pre_norm,
            "cells": [np.zeros(self.d_model, dtype=np.float32) for _ in range(self.num_dec_layers)],
            "self_k": [[] for _ in range(self.num_dec_layers)],
            "self_v": [[] for _ in range(self.num_dec_layers)],
            "cross_k": cross_k,
            "cross_v": cross_v,
        }
        steps: list[dict[str, object]] = []
        cur_tok = self.bos_id

        for pos in range(max_tgt_len):
            x = self._embed(cur_tok, pos, zero_token=(pos == 0 and zero_bos))
            for dl in range(self.num_dec_layers):
                x = self._run_decoder_layer(dl, x, dec_start + dl * dec_stride, state)
            if self.slots[6].dtype != TMNT_DTYPE_NONE:
                x = self._ln(x, 6, 7)

            logits = self._matvec(x, 2, 3)
            next_tok = int(np.argmax(logits))
            top_indices = np.argsort(logits)[-top_k:][::-1]
            steps.append(
                {
                    "step": pos,
                    "selected_id": next_tok,
                    "selected_logit": float(logits[next_tok]),
                    "logits_sha256": hashlib_f32(logits),
                    "top_candidates": [
                        {"id": int(idx), "logit": float(logits[idx])} for idx in top_indices
                    ],
                    "logits": logits,
                }
            )
            cur_tok = next_tok
            if next_tok == self.eos_id:
                break
        return steps


def hashlib_f32(vec: np.ndarray) -> str:
    """Compute SHA-256 over little-endian IEEE-754 float32 logits."""
    return hashlib.sha256(vec.astype("<f4", copy=False).tobytes()).hexdigest()
