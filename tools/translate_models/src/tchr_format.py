"""TCHR CTC character-table codec and deterministic script-pruning filters."""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass

TCHR_MAGIC = 0x52484354
TCHR_VERSION = 1
TCHR_HEADER_BYTES = 36
TCHR_MAX_CLASSES = 65536
TCHR_MAX_GLYPH_BYTES = 64
TCHR_INDEX_NONE = 0xFFFFFFFF

_VALID_OCR_MODELS = (1, 2, 3)
_MIN_CLASSES = 2
_ASCII_LO = 0x20
_ASCII_HI = 0x7F
_HDR_FMT = "<IHHII B3s IIII"


@dataclass(frozen=True)
class TchrTable:
    """Parsed contents of a binary TCHR CTC character table."""

    ocr_model: int
    blank_index: int
    unk_index: int
    tokens: tuple[str, ...]
    container_crc32: int


class DeterministicPrng:
    """Tiny dependency-free 64-bit SplitMix PRNG for reproducible fixtures."""

    def __init__(self, seed: int) -> None:
        """Seed PRNG state from an integer seed."""
        self.state = seed & 0xFFFFFFFFFFFFFFFF

    def next_u64(self) -> int:
        """Advance state and return next 64-bit unsigned integer."""
        self.state = (self.state + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
        return (z ^ (z >> 31)) & 0xFFFFFFFFFFFFFFFF

    def randrange(self, stop: int) -> int:
        """Return deterministic integer in [0, stop)."""
        if stop <= 0:
            msg = f"randrange stop must be positive, got {stop}"
            raise ValueError(msg)
        return self.next_u64() % stop

    def randint(self, lo: int, hi: int) -> int:
        """Return deterministic integer in inclusive range [lo, hi]."""
        return lo + self.randrange(hi - lo + 1)

    def randfloat(self) -> float:
        """Return deterministic float in [0.0, 1.0)."""
        return (self.next_u64() >> 11) / float(1 << 53)

    def choice(self, seq: list[str] | tuple[str, ...]) -> str:
        """Pick one deterministic string element from non-empty sequence."""
        return seq[self.randrange(len(seq))]

    def choice_int(self, seq: list[int] | tuple[int, ...]) -> int:
        """Pick one deterministic integer element from non-empty sequence."""
        return seq[self.randrange(len(seq))]


def _validate_tchr_args(
    tokens: list[str] | tuple[str, ...],
    ocr_model: int,
    blank_index: int,
    unk_index: int,
) -> None:
    """Validate class bounds, script ID, blank glyph, and unknown index."""
    class_count = len(tokens)
    if class_count < _MIN_CLASSES or class_count > TCHR_MAX_CLASSES:
        msg = f"invalid class_count={class_count}"
        raise ValueError(msg)
    if ocr_model not in _VALID_OCR_MODELS:
        msg = f"invalid ocr_model={ocr_model}"
        raise ValueError(msg)
    if not (0 <= blank_index < class_count):
        msg = f"blank_index={blank_index} out of range"
        raise ValueError(msg)
    if tokens[blank_index] != "":
        msg = "blank class entry must be empty string"
        raise ValueError(msg)
    if unk_index != TCHR_INDEX_NONE and (unk_index == blank_index or unk_index >= class_count):
        msg = f"invalid unk_index={unk_index}"
        raise ValueError(msg)


def encode_tchr(
    tokens: list[str] | tuple[str, ...],
    *,
    ocr_model: int,
    blank_index: int = 0,
    unk_index: int = TCHR_INDEX_NONE,
) -> bytes:
    """Serialize a token table into canonical TCHR wire bytes.

    Args:
        tokens: Ordered class glyph strings (blank entry must be "").
        ocr_model: Script ID matching `ra8_translate_ocr_model_t` (1=ko, 2=ja, 3=zh).
        blank_index: Class index representing the CTC blank symbol.
        unk_index: Unknown glyph class index or `TCHR_INDEX_NONE`.

    Returns:
        Complete serialized `TCHR` container bytes with IEEE CRC-32 checksum.

    Raises:
        ValueError: Invalid class counts, blank string mismatch, or oversized glyph.
    """
    _validate_tchr_args(tokens, ocr_model, blank_index, unk_index)
    pool = bytearray()
    offsets: list[int] = [0]
    for idx, glyph in enumerate(tokens):
        raw = glyph.encode("utf-8")
        if idx != blank_index and (len(raw) == 0 or len(raw) > TCHR_MAX_GLYPH_BYTES):
            msg = f"glyph[{idx}] utf-8 byte len={len(raw)} out of range [1, {TCHR_MAX_GLYPH_BYTES}]"
            raise ValueError(msg)
        pool.extend(raw)
        offsets.append(len(pool))

    offset_bytes = struct.pack(f"<{len(offsets)}I", *offsets)
    total_bytes = TCHR_HEADER_BYTES + len(offset_bytes) + len(pool)
    frame = bytearray(
        struct.pack(
            _HDR_FMT,
            TCHR_MAGIC,
            TCHR_VERSION,
            0,
            total_bytes,
            0,
            ocr_model,
            b"\x00\x00\x00",
            len(tokens),
            blank_index,
            unk_index,
            len(pool),
        ),
    )
    frame.extend(offset_bytes)
    frame.extend(pool)
    crc = zlib.crc32(frame) & 0xFFFFFFFF
    struct.pack_into("<I", frame, 12, crc)
    return bytes(frame)


def decode_tchr(data: bytes) -> TchrTable:
    """Validate and decode a serialized `TCHR` binary container.

    Args:
        data: Serialized bytes matching `ra8_translate_ctc.h` `TCHR` format.

    Returns:
        Decoded `TchrTable` after validating magic, bounds, offsets, UTF-8, and CRC-32.

    Raises:
        ValueError: Malformed header, truncated payload, invalid offsets, or CRC mismatch.
    """
    if len(data) < TCHR_HEADER_BYTES:
        msg = f"TCHR buffer too short: {len(data)} < {TCHR_HEADER_BYTES}"
        raise ValueError(msg)
    (
        magic,
        version,
        flags,
        total_bytes,
        crc_recorded,
        ocr_model,
        _pad,
        class_count,
        blank_index,
        unk_index,
        pool_bytes,
    ) = struct.unpack_from(_HDR_FMT, data, 0)
    if magic != TCHR_MAGIC or version != TCHR_VERSION or flags != 0:
        msg = f"invalid TCHR header magic=0x{magic:08X} version={version} flags={flags}"
        raise ValueError(msg)
    if total_bytes != len(data):
        msg = f"TCHR length mismatch: header={total_bytes}, actual={len(data)}"
        raise ValueError(msg)

    masked = bytearray(data)
    masked[12:16] = b"\x00\x00\x00\x00"
    if (zlib.crc32(masked) & 0xFFFFFFFF) != crc_recorded:
        msg = "TCHR CRC-32 mismatch"
        raise ValueError(msg)

    pool_off = TCHR_HEADER_BYTES + (class_count + 1) * 4
    if pool_off + pool_bytes != total_bytes:
        msg = "TCHR section layout mismatch"
        raise ValueError(msg)
    offsets = struct.unpack_from(f"<{class_count + 1}I", data, TCHR_HEADER_BYTES)
    if offsets[0] != 0 or offsets[-1] != pool_bytes:
        msg = "TCHR offsets boundary mismatch"
        raise ValueError(msg)
    tokens: list[str] = []
    for idx in range(class_count):
        start, end = offsets[idx], offsets[idx + 1]
        if end < start or end > pool_bytes:
            msg = f"TCHR non-monotonic offset at class {idx}"
            raise ValueError(msg)
        tokens.append(data[pool_off + start : pool_off + end].decode("utf-8"))
    return TchrTable(
        ocr_model=ocr_model,
        blank_index=blank_index,
        unk_index=unk_index,
        tokens=tuple(tokens),
        container_crc32=crc_recorded,
    )


def _ascii_printable() -> set[str]:
    """Return printable ASCII characters 0x20..0x7E."""
    return {chr(code) for code in range(_ASCII_LO, _ASCII_HI)}


def keep_set_korean() -> set[str]:
    """Build KS X 1001 common Hangul syllables + ASCII/punct keep set."""
    keep = _ascii_printable()
    for hi in range(0xB0, 0xC9):
        for lo in range(0xA1, 0xFF):
            try:
                keep.add(bytes([hi, lo]).decode("euc-kr"))
            except UnicodeDecodeError:
                continue
    return keep


def keep_set_japanese() -> set[str]:
    """Build JIS X 0208 ku 1..5 (punct/kana) + ku 16..47 (Level-1 kanji) keep set."""
    keep = _ascii_printable()
    for ku in (*range(1, 6), *range(16, 48)):
        b1 = 0xA0 + ku
        for ten in range(1, 95):
            try:
                keep.add(bytes([b1, 0xA0 + ten]).decode("euc-jp"))
            except UnicodeDecodeError:
                continue
    return keep


def keep_set_chinese() -> set[str]:
    """Build GB2312 Level-1 + Big5 Level-1 Simplified/Traditional Hanzi + punct keep set."""
    keep = _ascii_printable()
    for b1 in range(0xB0, 0xD8):
        for b2 in range(0xA1, 0xFF):
            try:
                keep.add(bytes([b1, b2]).decode("gb2312"))
            except UnicodeDecodeError:
                continue
    for b1 in range(0xA4, 0xC7):
        for b2 in (*range(0x40, 0x7F), *range(0xA1, 0xFF)):
            try:
                keep.add(bytes([b1, b2]).decode("big5"))
            except UnicodeDecodeError:
                continue
    keep.update(
        "\u3002\uff0c\uff01\uff1f\u3001\uff1a\u300c\u300d\u300e\u300f\uff08\uff09\u2014\u2026",
    )
    return keep


def build_keep_set(strategy: str) -> set[str] | None:
    """Resolve named pruning strategy into target character keep set."""
    strategies: dict[str, set[str] | None] = {
        "none": None,
        "korean_ksx1001": keep_set_korean(),
        "japanese_jis_l1": keep_set_japanese(),
        "chinese_gb2312_big5": keep_set_chinese(),
    }
    if strategy not in strategies:
        msg = f"unknown prune_strategy={strategy!r}"
        raise ValueError(msg)
    return strategies[strategy]


def build_pruned_vocab(
    dict_lines: list[str],
    output_dim: int,
    strategy: str,
) -> tuple[list[str], list[int], list[str]]:
    """Map an upstream PaddleOCR dictionary and output size to pruned class IDs.

    Args:
        dict_lines: Lines read from an upstream PaddleOCR dictionary text file.
        output_dim: Output class dimension of the upstream ONNX model.
        strategy: Pruning strategy name (`korean_ksx1001`, `japanese_jis_l1`, etc.).

    Returns:
        Tuple of `(full_tokens, keep_class_ids, pruned_tokens)`.

    Raises:
        ValueError: If `output_dim` disagrees with dictionary length by > 2 tokens.
    """
    n_dict = len(dict_lines)
    if output_dim == n_dict + 2:
        full_tokens = ["", *dict_lines, " "]
    elif output_dim == n_dict + 1:
        full_tokens = ["", *dict_lines]
    elif output_dim > n_dict + 2:
        full_tokens = ["", *dict_lines, *[""] * (output_dim - n_dict - 1)]
    else:
        msg = f"dict length {n_dict} exceeds model output_dim={output_dim}"
        raise ValueError(msg)

    keep_chars = build_keep_set(strategy)
    if keep_chars is None:
        all_ids = list(range(len(full_tokens)))
        return full_tokens, all_ids, list(full_tokens)

    want = set(keep_chars) | {" "}
    keep_ids = [0] + [idx for idx in range(1, len(full_tokens)) if full_tokens[idx] in want]
    pruned = [full_tokens[idx] for idx in keep_ids]
    return full_tokens, keep_ids, pruned
