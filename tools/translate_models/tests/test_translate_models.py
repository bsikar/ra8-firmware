"""Offline stdlib unit tests for `tools/translate_models` (no Vela/TF/Torch required)."""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

SRC_DIR = Path(__file__).resolve().parents[1] / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

from build_translate_models import check_committed_artifacts  # noqa: E402
from goldens import QuantParams, preprocess_crop_rgb_to_u8_and_i8  # noqa: E402
from model_registry import (  # noqa: E402
    MODEL_SPECS,
    OCR_MODEL_CHINESE,
    OCR_MODEL_JAPANESE,
    OCR_MODEL_KOREAN,
    ROTATION_CCW_90,
    PreprocessSpec,
)
from tchr_format import (  # noqa: E402
    DeterministicPrng,
    build_pruned_vocab,
    decode_tchr,
    encode_tchr,
)

_EXPECTED_PATCH_I8_LEN = 12


class TranslateModelsTest(unittest.TestCase):
    """Offline tests covering TCHR wire format, script pruning, PRNG, and manifests."""

    def _check_eq(self, got: object, want: object) -> None:
        """Fail unittest case with readable diff if `got != want`."""
        if got != want:
            self.fail(f"{got!r} != {want!r}")

    def _check_true(self, *, cond: bool, msg: str) -> None:
        """Fail unittest case when required predicate is false."""
        if not cond:
            self.fail(msg)

    def _expect_value_error(self, fn: object, *args: object, **kwargs: object) -> None:
        """Fail unless invoking callable raises `ValueError`."""
        raised = False
        try:
            fn(*args, **kwargs)  # type: ignore[operator]
        except ValueError:
            raised = True
        self._check_true(cond=raised, msg=f"expected ValueError from {fn!r}")

    def test_tchr_roundtrip_and_crc_tamper_detection(self) -> None:
        """TCHR encodes canonical headers and detects single-bit payload tampering."""
        tokens = ["", "\u4eca", "\u5929", " "]
        raw = encode_tchr(tokens, ocr_model=OCR_MODEL_CHINESE)
        table = decode_tchr(raw)
        self._check_eq(table.ocr_model, OCR_MODEL_CHINESE)
        self._check_eq(table.blank_index, 0)
        self._check_eq(table.tokens, tuple(tokens))

        tampered = bytearray(raw)
        tampered[-1] ^= 0x01
        self._expect_value_error(decode_tchr, bytes(tampered))

    def test_tchr_rejects_bad_blank_or_header_magic(self) -> None:
        """TCHR encoder and decoder reject non-empty blank entries and corrupt magic."""
        self._expect_value_error(encode_tchr, ["x", "y"], ocr_model=OCR_MODEL_KOREAN)
        raw = bytearray(encode_tchr(["", "a"], ocr_model=OCR_MODEL_KOREAN))
        struct.pack_into("<I", raw, 0, 0x12345678)
        self._expect_value_error(decode_tchr, bytes(raw))

    def test_script_pruning_keeps_core_characters(self) -> None:
        """Japanese, Korean, and Chinese pruning rules retain blank, space, and script chars."""
        raw_ja = ["\u3042", "\u30ab", "\u4eca", "\u9f8d", "a"]
        _, ids_ja, pruned_ja = build_pruned_vocab(raw_ja, len(raw_ja) + 2, "japanese_jis_l1")
        self._check_eq(pruned_ja[0], "")
        self._check_true(cond="\u3042" in pruned_ja, msg="missing hiragana in pruned_ja")
        self._check_true(cond="\u4eca" in pruned_ja, msg="missing JIS L1 kanji in pruned_ja")
        self._check_true(cond=" " in pruned_ja, msg="missing trailing space in pruned_ja")
        self._check_eq(len(ids_ja), len(pruned_ja))

        raw_ko = ["\uac00", "\ub098", "\u5929"]
        _, _, pruned_ko = build_pruned_vocab(raw_ko, len(raw_ko) + 2, "korean_ksx1001")
        self._check_eq(pruned_ko, ["", "\uac00", "\ub098", " "])

        raw_zh = ["\u4eca", "\u5929", "\u9580"]
        _, _, pruned_zh = build_pruned_vocab(raw_zh, len(raw_zh) + 2, "chinese_gb2312_big5")
        self._check_eq(pruned_zh, ["", "\u4eca", "\u5929", "\u9580", " "])

    def test_deterministic_prng_is_repeatable(self) -> None:
        """Deterministic SplitMix PRNG yields identical draws for matching seeds."""
        a = DeterministicPrng(42)
        b = DeterministicPrng(42)
        seq_a = [a.randint(1, 100) for _ in range(16)]
        seq_b = [b.randint(1, 100) for _ in range(16)]
        self._check_eq(seq_a, seq_b)

    def test_registry_specs_consistent(self) -> None:
        """All 4 pipeline specs record Apache-2.0 source and valid geometries."""
        self._check_eq(set(MODEL_SPECS.keys()), {"det", "ko", "zh", "ja"})
        self._check_eq(MODEL_SPECS["ja"].ocr_model_id, OCR_MODEL_JAPANESE)
        for spec in MODEL_SPECS.values():
            self._check_eq(spec.onnx_asset.license_id, "Apache-2.0")
            self._check_true(cond=spec.preprocess.width > 0, msg="width must be positive")
            self._check_true(cond=spec.preprocess.height > 0, msg="height must be positive")

    def test_q16_preprocess_and_ccw90_rotation(self) -> None:
        """Pure-Python Q16 crop+ccw_90+quantize matches half-pixel Zig geometry."""
        src = bytes([10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120])
        spec = PreprocessSpec(
            width=2,
            height=2,
            mean=(0.5, 0.5, 0.5),
            inv_std=(2.0, 2.0, 2.0),
            input_scale=1.0 / 255.0,
            rotation=ROTATION_CCW_90,
        )
        u8_bytes, i8_bytes = preprocess_crop_rgb_to_u8_and_i8(
            src,
            (2, 2),
            (0, 0, 2, 2),
            spec,
            QuantParams(scale=1.0 / 128.0, zero_point=0),
        )
        self._check_eq(list(u8_bytes), [40, 50, 60, 100, 110, 120, 10, 20, 30, 70, 80, 90])
        self._check_eq(len(i8_bytes), _EXPECTED_PATCH_I8_LEN)

    def test_committed_artifacts_valid_if_present(self) -> None:
        """Verify committed manifests and TCHR tables when generated directory exists."""
        gen_dir = Path(__file__).resolve().parents[1] / "generated"
        if (gen_dir / "model_manifests.json").is_file():
            self._check_eq(check_committed_artifacts(gen_dir), [])


if __name__ == "__main__":
    unittest.main()
