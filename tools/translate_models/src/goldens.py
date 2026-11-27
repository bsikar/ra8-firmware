"""Deterministic Q16 OCR preprocessing reference, evaluation metrics, and golden emitter."""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Protocol

from model_registry import COORD_HALF_PIXEL, ROTATION_CCW_90, ROTATION_NONE

if TYPE_CHECKING:
    from model_registry import ModelPipelineSpec, PreprocessSpec

_FP_SHIFT = 16
_FP_UNIT = 1 << _FP_SHIFT
_FP_MASK = _FP_UNIT - 1
_FP_NORM_ROUND = 1 << 31
_FP_NORM_SHIFT = 32
_INT8_MIN = -128.0
_INT8_MAX = 127.0
_RGB_CHANNELS = 3
_CRC_MASK = 0xFFFFFFFF


class ScoreMatrix(Protocol):
    """2D float/int score matrix `(steps, classes)` supporting shape and indexing."""

    @property
    def shape(self) -> tuple[int, ...]:
        """Return matrix dimensions `(T, C)`."""
        ...

    def __getitem__(self, idx: tuple[int, int]) -> float:
        """Return scalar score at `(step, class_idx)`."""
        ...


@dataclass(frozen=True)
class QuantParams:
    """Affine INT8 tensor quantization scale and signed zero-point."""

    scale: float
    zero_point: int


def _round_half_away_f32(val: float) -> int:
    """Round float scalar half-away-from-zero matching Zig `@round`."""
    if val >= 0.0:
        return int(val + 0.5)
    return -int(-val + 0.5)


def _map_coord_q16(dst_idx: int, dst_len: int, rot_len: int) -> int:
    """Map destination index to Q16.16 source coordinate under half_pixel policy."""
    if rot_len <= 1:
        return 0
    max_fp = (rot_len - 1) << _FP_SHIFT
    num = ((dst_idx * 2 + 1) * rot_len) - dst_len
    if num <= 0:
        return 0
    scaled = (num << (_FP_SHIFT - 1)) // dst_len
    return min(scaled, max_fp)


def _f32(val: float) -> float:
    """Round Python float64 value to single-precision IEEE-754 float32."""
    return float(struct.unpack("<f", struct.pack("<f", val))[0])


def preprocess_crop_rgb_to_u8_and_i8(
    src_rgb: bytes,
    src_size: tuple[int, int],
    crop: tuple[int, int, int, int],
    spec: PreprocessSpec,
    quant: tuple[float, int] | QuantParams,
) -> tuple[bytes, bytes]:
    """Run bit-identical Python Q16 crop+rotate+resize+quantize matching `ra8_translate`.

    Args:
        src_rgb: Packed `src_w * src_h * 3` RGB888 source pixels.
        src_size: Source image dimensions `(width, height)` in pixels.
        crop: Sub-rectangle `(x, y, width, height)` inside source image.
        spec: Target geometry, rotation (`none` or `ccw_90`), and normalization config.
        quant: Positive affine TFLite input `(scale, zero_point)` or `QuantParams`.

    Returns:
        Tuple of `(resized_rgb_u8_bytes, quantized_rgb_i8_bytes)`.

    Raises:
        ValueError: Invalid crop, rotation mode, or mismatched source buffer size.
    """
    src_w, src_h = src_size
    quant_scale, zero_point = (
        (quant.scale, quant.zero_point) if isinstance(quant, QuantParams) else quant
    )
    cx, cy, cw, ch_box = crop
    bad_crop = (
        len(src_rgb) != src_w * src_h * _RGB_CHANNELS
        or cx < 0
        or cy < 0
        or cx + cw > src_w
        or cy + ch_box > src_h
    )
    if bad_crop:
        msg = f"invalid crop {crop} for {src_w}x{src_h} buffer len={len(src_rgb)}"
        raise ValueError(msg)
    if spec.coord_mode != COORD_HALF_PIXEL or spec.rotation not in (ROTATION_NONE, ROTATION_CCW_90):
        msg = f"unsupported coord_mode={spec.coord_mode} or rotation={spec.rotation}"
        raise ValueError(msg)

    rot_w, rot_h = (ch_box, cw) if spec.rotation == ROTATION_CCW_90 else (cw, ch_box)
    dst_w, dst_h = spec.width, spec.height
    stride = src_w * _RGB_CHANNELS
    zp_f = _f32(float(zero_point))
    in_sc = _f32(spec.input_scale)
    q_sc = _f32(quant_scale)
    gains = [_f32(_f32(in_sc * _f32(spec.inv_std[c])) / q_sc) for c in range(_RGB_CHANNELS)]
    biases = [
        _f32(_f32(_f32(-_f32(spec.mean[c]) * _f32(spec.inv_std[c])) / q_sc) + zp_f)
        for c in range(_RGB_CHANNELS)
    ]

    u8_out = bytearray(dst_w * dst_h * _RGB_CHANNELS)
    i8_out = bytearray(dst_w * dst_h * _RGB_CHANNELS)
    max_lx_fp = (cw - 1) << _FP_SHIFT
    max_x, max_y = cx + cw - 1, cy + ch_box - 1
    out_off = 0
    for dy in range(dst_h):
        v_fp = _map_coord_q16(dy, dst_h, rot_h)
        for dx in range(dst_w):
            u_fp = _map_coord_q16(dx, dst_w, rot_w)
            sx_fp, sy_fp = (
                (max_lx_fp - v_fp, u_fp) if spec.rotation == ROTATION_CCW_90 else (u_fp, v_fp)
            )
            sx_fp += cx << _FP_SHIFT
            sy_fp += cy << _FP_SHIFT
            sx0, sy0 = sx_fp >> _FP_SHIFT, sy_fp >> _FP_SHIFT
            sx1 = sx0 + 1 if sx0 < max_x else max_x
            sy1 = sy0 + 1 if sy0 < max_y else max_y
            fx, fy = sx_fp & _FP_MASK, sy_fp & _FP_MASK
            ifx, ify = _FP_UNIT - fx, _FP_UNIT - fy
            row0, row1 = sy0 * stride, sy1 * stride
            col0, col1 = sx0 * _RGB_CHANNELS, sx1 * _RGB_CHANNELS
            w00, w10, w01, w11 = ifx * ify, fx * ify, ifx * fy, fx * fy
            for ch in range(_RGB_CHANNELS):
                acc = (
                    src_rgb[row0 + col0 + ch] * w00
                    + src_rgb[row0 + col1 + ch] * w10
                    + src_rgb[row1 + col0 + ch] * w01
                    + src_rgb[row1 + col1 + ch] * w11
                )
                px_val = (acc + _FP_NORM_ROUND) >> _FP_NORM_SHIFT
                u8_out[out_off] = px_val
                scaled = _f32(_f32(_f32(float(px_val)) * gains[ch]) + biases[ch])
                clamped = max(_INT8_MIN, min(_INT8_MAX, scaled))
                q_val = _round_half_away_f32(clamped)
                i8_out[out_off] = struct.pack("<b", max(-128, min(127, q_val)))[0]
                out_off += 1
    return bytes(u8_out), bytes(i8_out)


def decode_ctc_greedy(scores: ScoreMatrix, tokens: list[str] | tuple[str, ...]) -> str:
    """Greedy CTC collapse matching `ra8_translate_ctc_decode` (blank=0)."""
    out: list[str] = []
    prev = 0
    for step in range(scores.shape[0]):
        best = 0
        best_val = float(scores[step, 0])
        for cls_idx in range(1, scores.shape[1]):
            val = float(scores[step, cls_idx])
            if val > best_val:
                best_val = val
                best = cls_idx
        if best not in (0, prev) and best < len(tokens):
            out.append(tokens[best])
        prev = best
    return "".join(out)


def levenshtein_distance(a: str, b: str) -> int:
    """Compute character/codepoint Levenshtein edit distance."""
    dp = list(range(len(b) + 1))
    for ch_a in a:
        nxt = [dp[0] + 1]
        for j, ch_b in enumerate(b, 1):
            cost = 0 if ch_a == ch_b else 1
            nxt.append(min(dp[j] + 1, nxt[-1] + 1, dp[j - 1] + cost))
        dp = nxt
    return dp[-1]


def _render_detector_sample_page(
    fonts_dir: Path,
) -> tuple[bytes, int, int, tuple[int, int, int, int]]:
    """Render deterministic 272x272 synthetic manga page with crop margin + bubbles + furigana."""
    import PIL.Image as PilImage  # noqa: PLC0415
    import PIL.ImageDraw as PilDraw  # noqa: PLC0415
    import PIL.ImageFont as PilFont  # noqa: PLC0415

    font_ja = PilFont.truetype(str(fonts_dir / "NotoSansJP.otf"), 20)
    font_sc = PilFont.truetype(str(fonts_dir / "NotoSansSC.otf"), 18)
    tiny = PilFont.truetype(str(fonts_dir / "NotoSansJP.otf"), 9)

    img = PilImage.new("RGB", (272, 272), (244, 242, 238))
    draw = PilDraw.Draw(img)

    draw.ellipse([26, 30, 176, 104], fill=(255, 255, 255), outline=(18, 18, 18), width=2)
    draw.text((42, 54), "\u4eca\u5929\u5f88\u51b7", fill=(10, 10, 10), font=font_sc)

    draw.ellipse([162, 122, 248, 246], fill=(255, 255, 255), outline=(18, 18, 18), width=2)
    draw.text((196, 142), "\u591c\n\u306e\n\u98a8", fill=(10, 10, 10), font=font_ja)
    draw.text((222, 144), "\u3088\n\u308b", fill=(20, 20, 20), font=tiny)
    return img.tobytes(), 272, 272, (8, 8, 256, 256)


def _render_rec_patch(
    text: str,
    font_path: Path,
    fsize: int,
    *,
    dst_width: int,
    vertical_col: bool,
) -> tuple[bytes, int, int, tuple[int, int, int, int]]:
    """Render RGB888 synthetic patch with non-zero crop rectangle for E2E parity."""
    import PIL.Image as PilImage  # noqa: PLC0415
    import PIL.ImageDraw as PilDraw  # noqa: PLC0415
    import PIL.ImageFont as PilFont  # noqa: PLC0415

    font = PilFont.truetype(str(font_path), fsize)
    if vertical_col:
        col_w = int(fsize * 1.4)
        step_y = int(fsize * 1.1)
        col_h = max(48, len(text) * step_y + 12)
        src_w, src_h = col_w + 16, col_h + 16
        page = PilImage.new("RGB", (src_w, src_h), (255, 255, 255))
        draw = PilDraw.Draw(page)
        for idx, ch in enumerate(text):
            bb = draw.textbbox((0, 0), ch, font=font)
            cw = bb[2] - bb[0]
            px = 8 + (col_w - cw) // 2 - bb[0]
            py = 8 + 6 + idx * step_y - bb[1]
            draw.text((px, py), ch, fill=(0, 0, 0), font=font)
        return page.tobytes(), src_w, src_h, (8, 8, col_w, col_h)

    probe = PilDraw.Draw(PilImage.new("RGB", (dst_width * 2, 48), (255, 255, 255)))
    bb = probe.textbbox((0, 0), text, font=font)
    tw, th = max(1, bb[2] - bb[0]), max(1, bb[3] - bb[1])
    line_w = max(dst_width, tw + 16)
    src_w, src_h = line_w + 16, 64
    im = PilImage.new("RGB", (src_w, src_h), (255, 255, 255))
    ty = 8 + max(2, (48 - th) // 2 - bb[1])
    PilDraw.Draw(im).text((16 - bb[0], ty), text, fill=(0, 0, 0), font=font)
    return im.tobytes(), src_w, src_h, (8, 8, line_w, 48)


def _binary_map_f1_iou(prob_a: object, prob_b: object, thresh: float = 0.3) -> tuple[float, float]:
    """Compute binary mask IoU and pixel F1 between float and int8 detector maps."""
    import numpy as np  # noqa: PLC0415

    ma = np.asarray(prob_a) > thresh
    mb = np.asarray(prob_b) > thresh
    inter = int(np.logical_and(ma, mb).sum())
    union = int(np.logical_or(ma, mb).sum())
    tot = int(ma.sum()) + int(mb.sum())
    return float(inter / max(1, union)), float((2.0 * inter) / max(1, tot))


def evaluate_detector_and_emit_golden(
    spec: ModelPipelineSpec,
    onnx_path: Path,
    tflite_path: Path,
    tfl_meta: dict[str, int | float | list[int]],
    dirs: tuple[Path, Path],
) -> dict[str, object]:
    """Run ORT float32 vs TFLite INT8 detector evaluation and write golden fixtures."""
    import numpy as np  # noqa: PLC0415
    import onnxruntime as ort  # noqa: PLC0415
    from ai_edge_litert.interpreter import Interpreter  # noqa: PLC0415

    fonts_dir, goldens_dir = dirs
    page_rgb, sw, sh, crop = _render_detector_sample_page(fonts_dir)

    in_scale = float(tfl_meta["input_scale"])  # type: ignore[arg-type]
    in_zp = int(tfl_meta["input_zero_point"])  # type: ignore[arg-type]
    out_scale = float(tfl_meta["output_scale"])  # type: ignore[arg-type]
    out_zp = int(tfl_meta["output_zero_point"])  # type: ignore[arg-type]

    u8_bytes, in_i8_bytes = preprocess_crop_rgb_to_u8_and_i8(
        page_rgb,
        (sw, sh),
        crop,
        spec.preprocess,
        QuantParams(in_scale, in_zp),
    )
    arr_u8 = np.frombuffer(u8_bytes, dtype=np.uint8).reshape(1, 256, 256, 3)
    arr_norm = (
        (arr_u8.astype(np.float32) * spec.preprocess.input_scale)
        - np.array(spec.preprocess.mean, np.float32)
    ) * np.array(spec.preprocess.inv_std, np.float32)
    x_bgr = arr_norm.transpose(0, 3, 1, 2)[:, ::-1, :, :].copy()

    so = ort.SessionOptions()
    so.intra_op_num_threads = 2
    sess = ort.InferenceSession(str(onnx_path), sess_options=so, providers=["CPUExecutionProvider"])
    prob_f32 = sess.run(None, {sess.get_inputs()[0].name: x_bgr})[0][0, 0]

    interp = Interpreter(model_path=str(tflite_path))
    interp.allocate_tensors()
    id0 = interp.get_input_details()[0]
    od0 = interp.get_output_details()[0]
    in_i8 = np.frombuffer(in_i8_bytes, dtype=np.int8).reshape(1, 256, 256, 3)
    interp.set_tensor(id0["index"], in_i8)
    interp.invoke()
    out_i8 = interp.get_tensor(od0["index"])[0, ..., 0]
    prob_i8 = (out_i8.astype(np.float32) - out_zp) * out_scale

    iou, f1 = _binary_map_f1_iou(prob_f32, prob_i8)
    goldens_dir.mkdir(parents=True, exist_ok=True)
    (goldens_dir / "det_page_src.rgb").write_bytes(page_rgb)
    (goldens_dir / "det_out_256x256.i8").write_bytes(out_i8.astype(np.int8).tobytes())

    return {
        "float_vs_int8_mask_iou": iou,
        "float_vs_int8_mask_f1": f1,
        "page_src_width": sw,
        "page_src_height": sh,
        "page_crop": list(crop),
        "preprocess_i8_crc32": zlib.crc32(in_i8_bytes) & _CRC_MASK,
        "preprocess_i8_head_bytes": list(struct.unpack("<32b", in_i8_bytes[:32])),
        "out_map_width": 256,
        "out_map_height": 256,
        "out_i8_crc32": zlib.crc32(out_i8.astype(np.int8).tobytes()) & _CRC_MASK,
    }


def _should_emit_rec_golden(
    spec_name: str,
    idx: int,
    vertical: bool,
    emitted: set[str],
) -> str | None:
    """Choose tag (`<lang>_horiz` or `ja_vert`) once per recognizer orientation."""
    tag = f"{spec_name}_{'vert' if vertical else 'horiz'}"
    if idx == 0 and tag not in emitted:
        emitted.add(tag)
        return tag
    if spec_name == "ja" and tag not in emitted:
        emitted.add(tag)
        return tag
    return None


def evaluate_recognizer_and_emit_golden(
    spec: ModelPipelineSpec,
    tokens: list[str],
    paths: tuple[Path, Path, Path],
    tfl_meta: dict[str, int | float | list[int]],
    eval_cases: list[tuple[str, Path, int, bool]],
) -> dict[str, object]:
    """Run ORT float32 vs TFLite INT8 recognizer CER eval and emit golden fixtures."""
    import numpy as np  # noqa: PLC0415
    import onnxruntime as ort  # noqa: PLC0415
    from ai_edge_litert.interpreter import Interpreter  # noqa: PLC0415
    from model_registry import PreprocessSpec as PrepCls  # noqa: PLC0415

    onnx_path, tflite_path, goldens_dir = paths
    in_scale = float(tfl_meta["input_scale"])  # type: ignore[arg-type]
    in_zp = int(tfl_meta["input_zero_point"])  # type: ignore[arg-type]

    so = ort.SessionOptions()
    so.intra_op_num_threads = 2
    sess = ort.InferenceSession(str(onnx_path), sess_options=so, providers=["CPUExecutionProvider"])
    interp = Interpreter(model_path=str(tflite_path))
    interp.allocate_tensors()
    id0 = interp.get_input_details()[0]
    od0 = interp.get_output_details()[0]

    results: list[dict[str, object]] = []
    emitted_tags: set[str] = set()
    tot_f_ed, tot_q_ed, tot_chars = 0, 0, 0
    goldens_dir.mkdir(parents=True, exist_ok=True)

    for idx, (text, font_path, fsize, vertical) in enumerate(eval_cases):
        raw_rgb, sw, sh, crop = _render_rec_patch(
            text,
            font_path,
            fsize,
            dst_width=spec.preprocess.width,
            vertical_col=vertical,
        )
        prep_spec = PrepCls(
            width=spec.preprocess.width,
            height=spec.preprocess.height,
            mean=spec.preprocess.mean,
            inv_std=spec.preprocess.inv_std,
            input_scale=spec.preprocess.input_scale,
            rotation=ROTATION_CCW_90 if vertical else ROTATION_NONE,
        )
        u8_bytes, in_i8_bytes = preprocess_crop_rgb_to_u8_and_i8(
            raw_rgb,
            (sw, sh),
            crop,
            prep_spec,
            QuantParams(in_scale, in_zp),
        )
        arr_u8 = np.frombuffer(u8_bytes, dtype=np.uint8).reshape(
            1,
            spec.preprocess.height,
            spec.preprocess.width,
            _RGB_CHANNELS,
        )
        arr_norm = (
            (arr_u8.astype(np.float32) * spec.preprocess.input_scale)
            - np.array(spec.preprocess.mean, np.float32)
        ) * np.array(spec.preprocess.inv_std, np.float32)
        xbgr = arr_norm.transpose(0, 3, 1, 2)[:, ::-1, :, :].copy()

        yf = sess.run(None, {sess.get_inputs()[0].name: xbgr})[0][0]
        sf = decode_ctc_greedy(yf, tokens)

        in_i8 = np.frombuffer(in_i8_bytes, dtype=np.int8).reshape(
            1,
            spec.preprocess.height,
            spec.preprocess.width,
            _RGB_CHANNELS,
        )
        interp.set_tensor(id0["index"], in_i8)
        interp.invoke()
        out_i8 = interp.get_tensor(od0["index"])[0]
        sq = decode_ctc_greedy(out_i8.astype(np.int32), tokens)

        tot_f_ed += levenshtein_distance(sf, text)
        tot_q_ed += levenshtein_distance(sq, text)
        tot_chars += len(text)
        entry: dict[str, object] = {
            "ref": text,
            "float": sf,
            "int8": sq,
            "vertical": vertical,
            "src_w": sw,
            "src_h": sh,
            "crop": list(crop),
            "preprocess_i8_crc32": zlib.crc32(in_i8_bytes) & _CRC_MASK,
            "preprocess_i8_head_bytes": list(struct.unpack("<32b", in_i8_bytes[:32])),
            "out_steps": int(out_i8.shape[0]),
            "out_classes": int(out_i8.shape[1]),
            "out_i8_crc32": zlib.crc32(out_i8.astype(np.int8).tobytes()) & _CRC_MASK,
        }
        tag = _should_emit_rec_golden(spec.name, idx, vertical, emitted_tags)
        if tag is not None:
            (goldens_dir / f"{tag}_src.rgb").write_bytes(raw_rgb)
            (goldens_dir / f"{tag}_logits.i8").write_bytes(out_i8.astype(np.int8).tobytes())
            entry["golden_tag"] = tag
        results.append(entry)

    return {
        "float_cer": tot_f_ed / max(1, tot_chars),
        "int8_cer": tot_q_ed / max(1, tot_chars),
        "samples": results,
    }
