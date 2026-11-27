#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Reproducible offline NPU model pipeline for `ra8_translate`.

Downloads (or verifies) pinned Apache-2.0 ONNX detector/OCR models and SIL OFL
fonts, converts fixed-shape NHWC Keras models without onnx2tf network calls,
quantizes to INT8 TFLite, compiles with pinned Ethos-U Vela, distills `.npub`
containers via `tools/vela/src/vela_gen.py`, emits `.tchr` tables, evaluates
float vs INT8 accuracy, and updates committed manifests and golden fixtures.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path
from typing import Protocol

SRC_DIR = Path(__file__).resolve().parent
TOOL_ROOT = SRC_DIR.parent
REPO_ROOT = TOOL_ROOT.parents[1]
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

from goldens import (  # noqa: E402
    evaluate_detector_and_emit_golden,
    evaluate_recognizer_and_emit_golden,
)
from model_registry import (  # noqa: E402
    ETHOS_U55_SRAM_BUDGET_KIB,
    FONT_ASSETS,
    MODEL_SPECS,
    PINNED_VELA_ARGS,
    PINNED_VELA_VERSION,
    ModelPipelineSpec,
    RemoteAsset,
)
from tchr_format import (  # noqa: E402
    DeterministicPrng,
    build_keep_set,
    build_pruned_vocab,
    decode_tchr,
    encode_tchr,
)

_ASCII_PRINTABLE_MIN = 0x20
_ASCII_PRINTABLE_MAX = 0x7E
_ASCII_UPPER_BOUND = 0x7F


class _VelaGenModule(Protocol):
    """Typed interface for `tools/vela/src/vela_gen.distill_blob`."""

    def distill_blob(self, path: Path) -> tuple[bytes, dict[str, object]]:
        """Distill `.npub` blob and region placement report from a Vela `.tflite`."""
        ...


def _sha256_bytes(data: bytes) -> str:
    """Compute lowercase hexadecimal SHA-256 digest."""
    return hashlib.sha256(data).hexdigest()


def _download_asset(asset: RemoteAsset, dst_dir: Path, *, force: bool = False) -> Path:
    """Download an upstream asset if absent and verify SHA-256 when pinned."""
    dst = dst_dir / asset.filename
    dst_dir.mkdir(parents=True, exist_ok=True)
    if not dst.is_file() or force:
        if not asset.url.startswith("https://"):
            msg = f"refusing non-HTTPS URL {asset.url!r}"
            raise ValueError(msg)
        with urllib.request.urlopen(asset.url) as resp:  # noqa: S310 -- validated https URL
            dst.write_bytes(resp.read())
    digest = _sha256_bytes(dst.read_bytes())
    if asset.sha256 and digest != asset.sha256:
        msg = f"SHA-256 mismatch for {dst}: got {digest}, expected {asset.sha256}"
        raise ValueError(msg)
    return dst


def _load_vela_gen() -> _VelaGenModule:
    """Load `tools/vela/src/vela_gen.py` without duplicating `.npub` code."""
    vela_src = REPO_ROOT / "tools" / "vela" / "src" / "vela_gen.py"
    spec = importlib.util.spec_from_file_location("vela_gen", vela_src)
    if spec is None or spec.loader is None:
        msg = f"cannot load vela_gen from {vela_src}"
        raise RuntimeError(msg)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod  # type: ignore[return-value]


def _parse_vela_stdout(stdout_text: str) -> dict[str, float | int]:
    """Extract SRAM, Flash, CPU-fallback, and NPU op counts from Vela output."""
    sram_kib = 0.0
    flash_kib = 0.0
    cpu_ops = 0
    npu_ops = 0
    for raw in stdout_text.splitlines():
        line = raw.strip()
        if line.startswith("Total SRAM used"):
            sram_kib = float(line.split()[-2])
        elif line.startswith("Total Flash used"):
            flash_kib = float(line.split()[-2])
        elif line.startswith("CPU operators"):
            cpu_ops = int(line.split("=")[1].strip().split()[0])
        elif line.startswith("NPU operators"):
            npu_ops = int(line.split("=")[1].strip().split()[0])
    total = cpu_ops + npu_ops
    return {
        "total_sram_kib": sram_kib,
        "total_flash_kib": flash_kib,
        "cpu_operators": cpu_ops,
        "npu_operators": npu_ops,
        "npu_offload_pct": (100.0 * npu_ops / total) if total else 0.0,
    }


def _run_vela_compile(tflite_path: Path, out_dir: Path) -> tuple[Path, dict[str, float | int]]:
    """Invoke pinned `vela` CLI on an INT8 `.tflite` file and return report."""
    vela_bin = shutil.which("vela") or str(Path(sys.executable).parent / "vela")
    if not Path(vela_bin).is_file():
        msg = "pinned `vela` binary not found in environment"
        raise FileNotFoundError(msg)
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [vela_bin, str(tflite_path), *PINNED_VELA_ARGS, "--output-dir", str(out_dir)]
    res = subprocess.run(cmd, capture_output=True, text=True, check=False)  # noqa: S603
    if res.returncode != 0:
        msg = f"vela failed ({res.returncode}): {res.stderr}"
        raise RuntimeError(msg)
    report = _parse_vela_stdout(res.stdout)
    vela_tflite = out_dir / f"{tflite_path.stem}_vela.tflite"
    if not vela_tflite.is_file():
        msg = f"expected Vela output missing: {vela_tflite}"
        raise FileNotFoundError(msg)
    return vela_tflite, report


def _render_det_cal_images(
    fonts_dir: Path,
    spec: ModelPipelineSpec,
    count: int = 32,
) -> list[object]:
    """Render synthetic Noto CJK comic pages for detector INT8 calibration."""
    import numpy as np  # noqa: PLC0415
    import PIL.Image as PilImage  # noqa: PLC0415
    import PIL.ImageDraw as PilDraw  # noqa: PLC0415
    import PIL.ImageFont as PilFont  # noqa: PLC0415

    prng = DeterministicPrng(101)
    f_ja = PilFont.truetype(str(fonts_dir / "NotoSansJP.otf"), 18)
    f_sc = PilFont.truetype(str(fonts_dir / "NotoSansSC.otf"), 18)
    f_ko = PilFont.truetype(str(fonts_dir / "NotoSansKR.otf"), 18)
    samples = [
        ("\u4eca\u5929\u5f88\u51b7", f_sc),
        ("\u591c\u306e\u98a8", f_ja),
        ("\uc870\uc6a9\ud55c \ubc29", f_ko),
        ("Hello!", f_sc),
    ]
    mean = np.array(spec.preprocess.mean, dtype=np.float32)
    inv_std = np.array(spec.preprocess.inv_std, dtype=np.float32)
    out: list[object] = []
    for idx in range(count):
        bg = prng.randint(232, 252)
        img = PilImage.new("RGB", (256, 256), (bg, bg - 2, bg - 4))
        draw = PilDraw.Draw(img)
        n_bub = prng.randint(2, 4)
        for bub in range(n_bub):
            x0, y0 = prng.randint(10, 120), prng.randint(10, 180)
            x1 = min(246, x0 + prng.randint(90, 130))
            y1 = min(246, y0 + prng.randint(36, 72))
            draw.ellipse([x0, y0, x1, y1], fill=(255, 255, 255), outline=(15, 15, 15), width=2)
            txt, fnt = samples[(idx + bub) % len(samples)]
            draw.text((x0 + 12, (y0 + y1) // 2 - 10), txt, fill=(10, 10, 10), font=fnt)
        arr = np.asarray(img, dtype=np.float32) * spec.preprocess.input_scale
        out.append(((arr - mean) * inv_std).astype(np.float32))
    return out


def _render_rec_cal_images(
    spec: ModelPipelineSpec,
    tokens: list[str],
    fonts_dir: Path,
    count: int = 120,
) -> list[object]:
    """Render diverse synthetic CJK calibration strips matching pipeline geometry."""
    import numpy as np  # noqa: PLC0415
    import PIL.Image as PilImage  # noqa: PLC0415
    import PIL.ImageDraw as PilDraw  # noqa: PLC0415
    import PIL.ImageFont as PilFont  # noqa: PLC0415
    from ja_vertical_adapt import (  # noqa: PLC0415
        build_ja_char_pool,
        render_ja_horiz_rgb,
        render_ja_vert_ccw90_rgb,
    )

    width = spec.preprocess.width
    prng = DeterministicPrng(77 if spec.name == "ko" else 88 if spec.name == "zh" else 99)
    if spec.name == "ja":
        f_jp = {
            sz: PilFont.truetype(str(fonts_dir / "NotoSansJP.otf"), sz) for sz in (22, 26, 30, 32)
        }
        pool = build_ja_char_pool(tokens)
        imgs: list[object] = []
        for idx in range(count):
            sz = int(prng.choice([22, 26, 30, 32]))
            txt = "".join(str(prng.choice(pool)) for _ in range(prng.randint(3, 6)))
            if idx % 4 == 0:
                imgs.append(render_ja_horiz_rgb(txt, f_jp[sz], width=width, bg=255, fg=0))
            else:
                stretch = idx % 2 == 1
                imgs.append(
                    render_ja_vert_ccw90_rgb(
                        txt,
                        f_jp[sz],
                        (sz, width),
                        (255, 0),
                        stretch=stretch,
                    ),
                )
        return imgs

    font_names = ["NotoSansKR.otf"] if spec.name == "ko" else ["NotoSansSC.otf", "NotoSansTC.otf"]
    fonts = {
        (fn, sz): PilFont.truetype(str(fonts_dir / fn), sz)
        for fn in font_names
        for sz in (24, 28, 30)
    }
    pool = [c for c in tokens[1:] if len(c) == 1 and ord(c) > _ASCII_UPPER_BOUND]
    out: list[object] = []
    for idx in range(count):
        fn = font_names[idx % len(font_names)]
        sz = int(prng.choice([24, 28, 30]))
        fnt = fonts[(fn, sz)]
        bg = int(prng.choice([255, 245, 232]))
        if spec.name == "ko":
            words = [
                "".join(str(prng.choice(pool)) for _ in range(prng.randint(1, 3)))
                for _ in range(prng.randint(2, 4))
            ]
            txt = " ".join(words)
        else:
            txt = "".join(str(prng.choice(pool)) for _ in range(prng.randint(3, 6)))
        d0 = PilDraw.Draw(PilImage.new("RGB", (width * 2, 48), (bg, bg, bg)))
        bb = d0.textbbox((0, 0), txt, font=fnt)
        tw, th = max(1, bb[2] - bb[0]), max(1, bb[3] - bb[1])
        im = PilImage.new("RGB", (tw + 16, 48), (bg, bg, bg))
        ty = max(2, (48 - th) // 2 - bb[1])
        PilDraw.Draw(im).text((8 - bb[0], ty), txt, fill=(0, 0, 0), font=fnt)
        if tw + 16 > width:
            im = im.resize((width, 48), PilImage.Resampling.BILINEAR)
        canvas = PilImage.new("RGB", (width, 48), (bg, bg, bg))
        canvas.paste(im, (0, 0))
        out.append((np.asarray(canvas, dtype=np.float32) / 255.0 - 0.5) * 2.0)
    return out


def _eval_cases_for_spec(
    spec: ModelPipelineSpec,
    fonts_dir: Path,
) -> list[tuple[str, Path, int, bool]]:
    """Build deterministic held-out synthetic evaluation cases (no eval_corpus overlap)."""
    if spec.name == "ko":
        fk = fonts_dir / "NotoSansKR.otf"
        phrases = [
            "\uc624\ub298 \ube44\uac00 \uc640\uc694",
            "\ubc84\uc2a4\uac00 \uc654\uc5b4\uc694",
            "\uc5ec\uae30 \uc549\uc73c\uc138\uc694",
            "\ub530\ub73b\ud55c \ucc28 \uc8fc\uc138\uc694",
            "\uc870\uc6a9\ud55c \ubc29",
            "\ub0b4\uc77c \ub9cc\ub098\uc694",
            "\uac10\uc0ac\ud569\ub2c8\ub2e4",
            "\uc9c0\uae08 \uba87 \uc2dc\uc608\uc694?",
        ]
        return [(p, fk, 26, False) for p in phrases]
    if spec.name == "zh":
        sc, tc = fonts_dir / "NotoSansSC.otf", fonts_dir / "NotoSansTC.otf"
        return [
            ("\u4eca\u5929\u5f88\u51b7", sc, 30, False),
            ("\u8bf7\u559d\u70ed\u8336", sc, 30, False),
            ("\u6211\u4eec\u51fa\u53d1\u5427", sc, 30, False),
            ("\u524d\u9762\u6709\u6865", sc, 30, False),
            ("\u8acb\u559d\u71b1\u8336", tc, 30, False),
            ("\u6211\u5011\u51fa\u767c\u5427", tc, 30, False),
            ("\u9019\u672c\u66f8\u5f88\u597d", tc, 30, False),
            ("\u5b89\u975c\u4e00\u9ede", tc, 30, False),
        ]
    fj = fonts_dir / "NotoSansJP.otf"
    vert = [
        "\u591c\u306e\u98a8",
        "\u672c\u3092\u8aad\u3080",
        "\u661f\u304c\u898b\u3048\u308b",
        "\u99c5\u3067\u5f85\u3064",
    ]
    horiz = [
        "\u9759\u304b\u306a\u90e8\u5c4b",
        "\u304a\u8336\u3092\u3069\u3046\u305e",
        "\u5f85\u3063\u3066\u304f\u3060\u3055\u3044",
        "\u3042\u308a\u304c\u3068\u3046",
    ]
    return [(p, fj, 30, True) for p in vert] + [(p, fj, 30, False) for p in horiz]


def _zig_escape_utf8(text: str) -> str:
    r"""Format UTF-8 text as pure 7-bit ASCII Zig string literal with `\xNN` escapes."""
    out: list[str] = []
    for byte in text.encode("utf-8"):
        is_safe = _ASCII_PRINTABLE_MIN <= byte <= _ASCII_PRINTABLE_MAX and byte not in (
            ord("\\"),
            ord('"'),
        )
        out.append(chr(byte) if is_safe else f"\\x{byte:02x}")
    return "".join(out)


def _format_golden_zig(manifest: dict[str, object]) -> str:
    """Render concise Zig golden fixture module referencing generated binary buffers."""
    models = manifest["models"]  # type: ignore[index]
    det = models["det"]
    det_g = det["eval"]
    det_tfl = det["tflite"]
    banner = (
        "//! @generated by tools/translate_models/src/build_translate_models.py"
        " -- DO NOT EDIT BY HAND."
    )
    lines = [
        banner,
        "//! SPDX-License-Identifier: MIT",
        "//! Copyright (c) 2026 Brighton Sikarskie",
        "",
        "pub const TchrAsset = struct {",
        "    name: []const u8,",
        "    ocr_model: u8,",
        "    class_count: u32,",
        "    bytes: []const u8,",
        "    sha256: []const u8,",
        "};",
        "",
        "pub const DetGolden = struct {",
        "    src_width: u16,",
        "    src_height: u16,",
        "    crop_x: u16,",
        "    crop_y: u16,",
        "    crop_w: u16,",
        "    crop_h: u16,",
        "    in_scale: f32,",
        "    in_zp: i8,",
        "    out_scale: f32,",
        "    out_zp: i8,",
        "    in_i8_crc32: u32,",
        "    out_i8_crc32: u32,",
        "    src_rgb: []const u8,",
        "    out_i8: []const u8,",
        "};",
        "",
        "pub const RecGolden = struct {",
        "    tag: []const u8,",
        "    ocr_model: u8,",
        "    dst_width: u16,",
        "    dst_height: u16,",
        "    vertical_ccw90: bool,",
        "    src_width: u16,",
        "    src_height: u16,",
        "    crop_x: u16,",
        "    crop_y: u16,",
        "    crop_w: u16,",
        "    crop_h: u16,",
        "    in_scale: f32,",
        "    in_zp: i8,",
        "    in_i8_crc32: u32,",
        "    out_steps: u32,",
        "    out_classes: u32,",
        "    out_scale: f32,",
        "    out_zp: i8,",
        "    ref_utf8: []const u8,",
        "    expected_utf8: []const u8,",
        "    src_rgb: []const u8,",
        "    logits_i8: []const u8,",
        "    tchr: []const u8,",
        "};",
        "",
        "pub const det_golden: DetGolden = .{",
        f"    .src_width = {det_g['page_src_width']},",
        f"    .src_height = {det_g['page_src_height']},",
        f"    .crop_x = {det_g['page_crop'][0]},",
        f"    .crop_y = {det_g['page_crop'][1]},",
        f"    .crop_w = {det_g['page_crop'][2]},",
        f"    .crop_h = {det_g['page_crop'][3]},",
        f"    .in_scale = {det_tfl['input_scale']:.9g},",
        f"    .in_zp = {det_tfl['input_zero_point']},",
        f"    .out_scale = {det_tfl['output_scale']:.9g},",
        f"    .out_zp = {det_tfl['output_zero_point']},",
        f"    .in_i8_crc32 = 0x{det_g['preprocess_i8_crc32']:08X},",
        f"    .out_i8_crc32 = 0x{det_g['out_i8_crc32']:08X},",
        '    .src_rgb = @embedFile("goldens/det_page_src.rgb"),',
        '    .out_i8 = @embedFile("goldens/det_out_256x256.i8"),',
        "};",
        "",
    ]

    rec_entries: list[str] = []
    for name in ("ko", "zh", "ja"):
        info = models[name]
        prep = info["preprocess"]
        tfl = info["tflite"]
        lines.append(f'pub const {name}_tchr = @embedFile("{info["tchr_relpath"]}");')
        for sample in info["eval"]["samples"]:
            tag = sample.get("golden_tag")
            if not tag:
                continue
            cx, cy, cw, ch = sample["crop"]
            esc_ref = _zig_escape_utf8(sample["ref"])
            esc_i8 = _zig_escape_utf8(sample["int8"])
            rec_entries.extend(
                [
                    "    .{",
                    f'        .tag = "{tag}",',
                    f"        .ocr_model = {info['ocr_model_id']},",
                    f"        .dst_width = {prep['width']},",
                    f"        .dst_height = {prep['height']},",
                    f"        .vertical_ccw90 = {'true' if sample['vertical'] else 'false'},",
                    f"        .src_width = {sample['src_w']},",
                    f"        .src_height = {sample['src_h']},",
                    f"        .crop_x = {cx},",
                    f"        .crop_y = {cy},",
                    f"        .crop_w = {cw},",
                    f"        .crop_h = {ch},",
                    f"        .in_scale = {tfl['input_scale']:.9g},",
                    f"        .in_zp = {tfl['input_zero_point']},",
                    f"        .in_i8_crc32 = 0x{sample['preprocess_i8_crc32']:08X},",
                    f"        .out_steps = {sample['out_steps']},",
                    f"        .out_classes = {sample['out_classes']},",
                    f"        .out_scale = {tfl['output_scale']:.9g},",
                    f"        .out_zp = {tfl['output_zero_point']},",
                    f'        .ref_utf8 = "{esc_ref}",',
                    f'        .expected_utf8 = "{esc_i8}",',
                    f'        .src_rgb = @embedFile("goldens/{tag}_src.rgb"),',
                    f'        .logits_i8 = @embedFile("goldens/{tag}_logits.i8"),',
                    f"        .tchr = {name}_tchr,",
                    "    },",
                ]
            )

    lines.extend(["", "pub const rec_goldens = [_]RecGolden{", *rec_entries, "};", ""])
    return "\n".join(lines)


def _prepare_recognizer_onnx(
    spec: ModelPipelineSpec,
    onnx_src: Path,
    dirs: tuple[Path, Path, Path, Path],
) -> tuple[Path, list[str], str, str, dict[str, float] | None]:
    """Prune CTC vocabulary/head, run JA vertical adapt if enabled, and emit `.tchr`."""
    import onnx  # noqa: PLC0415
    from ja_vertical_adapt import AdaptConfig, adapt_japanese_vertical  # noqa: PLC0415
    from onnx_to_tf import prune_onnx_classifier_head  # noqa: PLC0415

    models_dir, fonts_dir, work_dir, gen_dir = dirs
    if spec.dict_asset is None:
        msg = f"recognizer {spec.name} missing dict_asset"
        raise ValueError(msg)
    dict_path = _download_asset(spec.dict_asset, models_dir)
    raw_lines = dict_path.read_text(encoding="utf-8").splitlines()
    model = onnx.load(str(onnx_src))
    out_dim = model.graph.output[0].type.tensor_type.shape.dim[-1].dim_value
    _full, keep_ids, tokens = build_pruned_vocab(raw_lines, out_dim, spec.prune_strategy)
    pruned_onnx = work_dir / f"{spec.name}_pruned.onnx"
    prune_onnx_classifier_head(onnx_src, keep_ids, pruned_onnx)
    work_onnx = pruned_onnx
    adapt_summary: dict[str, float] | None = None
    if spec.vertical_adapted:
        adapted_onnx = work_dir / f"{spec.name}_adapted.onnx"
        adapt_summary = adapt_japanese_vertical(
            pruned_onnx,
            tokens,
            fonts_dir / "NotoSansJP.otf",
            adapted_onnx,
            AdaptConfig(width=spec.preprocess.width),
        )
        work_onnx = adapted_onnx

    tchr_bytes = encode_tchr(tokens, ocr_model=spec.ocr_model_id)
    tchr_rel = f"{spec.name}_charset.tchr"
    (gen_dir / tchr_rel).write_bytes(tchr_bytes)
    return work_onnx, tokens, tchr_rel, _sha256_bytes(tchr_bytes), adapt_summary


def _build_one_model(
    spec: ModelPipelineSpec,
    *,
    cache_dir: Path,
    build_dir: Path,
    gen_dir: Path,
    vela_gen: _VelaGenModule,
) -> dict[str, object]:
    """Execute prune/adapt -> Keras -> INT8 TFLite -> Vela -> .npub -> eval for one model."""
    from onnx_to_tf import (  # noqa: PLC0415
        convert_onnx_to_keras,
        export_int8_tflite,
        verify_float_parity,
    )

    models_dir = cache_dir / "models"
    fonts_dir = cache_dir / "fonts"
    for f_asset in FONT_ASSETS.values():
        _download_asset(f_asset, fonts_dir)
    onnx_src = _download_asset(spec.onnx_asset, models_dir)
    onnx_sha = _sha256_bytes(onnx_src.read_bytes())

    work_dir = build_dir / spec.name
    work_dir.mkdir(parents=True, exist_ok=True)
    work_onnx = onnx_src
    tokens: list[str] = []
    tchr_rel = ""
    tchr_sha = ""
    adapt_summary: dict[str, float] | None = None

    if spec.kind == "recognizer":
        work_onnx, tokens, tchr_rel, tchr_sha, adapt_summary = _prepare_recognizer_onnx(
            spec,
            onnx_src,
            (models_dir, fonts_dir, work_dir, gen_dir),
        )

    keras_model = convert_onnx_to_keras(
        work_onnx,
        height=spec.preprocess.height,
        width=spec.preprocess.width,
        drop_softmax=spec.drop_softmax,
    )
    max_diff = verify_float_parity(work_onnx, keras_model, spec)
    cal_imgs = (
        _render_det_cal_images(fonts_dir, spec)
        if spec.kind == "detector"
        else _render_rec_cal_images(spec, tokens, fonts_dir)
    )

    tflite_path = work_dir / f"{spec.name}_int8.tflite"
    tfl_bytes, tfl_meta = export_int8_tflite(keras_model, cal_imgs, tflite_path)
    vela_tflite, vela_report = _run_vela_compile(tflite_path, work_dir / "vela")
    sram_used = float(vela_report["total_sram_kib"])
    if sram_used > ETHOS_U55_SRAM_BUDGET_KIB:
        msg = f"{spec.name} SRAM {sram_used} KiB exceeds {ETHOS_U55_SRAM_BUDGET_KIB} KiB"
        raise RuntimeError(msg)

    blob_bytes, distill_rep = vela_gen.distill_blob(vela_tflite)
    npub_path = build_dir / f"ra8_translate_{spec.name}.npub"
    npub_path.write_bytes(blob_bytes)

    goldens_dir = gen_dir / "goldens"
    if spec.kind == "detector":
        eval_rep = evaluate_detector_and_emit_golden(
            spec,
            work_onnx,
            tflite_path,
            tfl_meta,  # type: ignore[arg-type]
            (fonts_dir, goldens_dir),
        )
    else:
        eval_rep = evaluate_recognizer_and_emit_golden(
            spec,
            tokens,
            (work_onnx, tflite_path, goldens_dir),
            tfl_meta,  # type: ignore[arg-type]
            _eval_cases_for_spec(spec, fonts_dir),
        )

    return {
        "name": spec.name,
        "kind": spec.kind,
        "ocr_model_id": spec.ocr_model_id,
        "source_url": spec.onnx_asset.url,
        "source_license": spec.onnx_asset.license_id,
        "source_attribution": spec.onnx_asset.attribution,
        "source_sha256": onnx_sha,
        "onnx_to_keras_max_diff": max_diff,
        "preprocess": {
            "width": spec.preprocess.width,
            "height": spec.preprocess.height,
            "mean": list(spec.preprocess.mean),
            "inv_std": list(spec.preprocess.inv_std),
            "input_scale": spec.preprocess.input_scale,
            "rotation": spec.preprocess.rotation,
            "coord_mode": spec.preprocess.coord_mode,
        },
        "class_count": len(tokens),
        "tchr_relpath": tchr_rel,
        "tchr_sha256": tchr_sha,
        "adapt_summary": adapt_summary,
        "tflite": tfl_meta,
        "tflite_sha256": _sha256_bytes(tfl_bytes),
        "vela": vela_report,
        "npub_bytes": len(blob_bytes),
        "npub_sha256": _sha256_bytes(blob_bytes),
        "npub_regions": distill_rep["regions"],
        "npub_places": distill_rep["places"],
        "eval": eval_rep,
    }


def check_committed_artifacts(gen_dir: Path) -> list[str]:
    """Validate committed `.tchr`, `model_manifests.json`, and golden binaries offline."""
    errors: list[str] = []
    manifest_path = gen_dir / "model_manifests.json"
    if not manifest_path.is_file():
        return [f"missing manifest: {manifest_path}"]
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("pinned_vela_version") != PINNED_VELA_VERSION:
        errors.append(
            f"unexpected vela version in manifest: {manifest.get('pinned_vela_version')}",
        )

    for key in ("det", "ko", "zh", "ja"):
        info = manifest.get("models", {}).get(key)
        if not info:
            errors.append(f"missing model entry {key!r}")
            continue
        if info["vela"]["total_sram_kib"] > ETHOS_U55_SRAM_BUDGET_KIB:
            errors.append(f"{key} SRAM exceeds budget")
        if info["kind"] == "recognizer":
            tchr_path = gen_dir / info["tchr_relpath"]
            if not tchr_path.is_file():
                errors.append(f"missing tchr: {tchr_path}")
            else:
                raw = tchr_path.read_bytes()
                if _sha256_bytes(raw) != info["tchr_sha256"]:
                    errors.append(f"sha256 mismatch on {tchr_path}")
                table = decode_tchr(raw)
                if len(table.tokens) != info["class_count"]:
                    errors.append(f"{key} class_count mismatch")
                keep = build_keep_set(MODEL_SPECS[key].prune_strategy)
                if keep is not None and any(
                    tok not in keep for tok in table.tokens[1:] if tok != " "
                ):
                    errors.append(f"{key} token outside pruned strategy")
    return errors


def main(argv: list[str] | None = None) -> int:
    """CLI entrypoint for building or verifying offline translation models."""
    parser = argparse.ArgumentParser(description="Build/verify RA8 translation NPU models.")
    parser.add_argument(
        "--build",
        nargs="*",
        choices=["det", "ko", "zh", "ja"],
        help="Build named models.",
    )
    parser.add_argument(
        "--cache-dir",
        default="build/translate_models_cache",
        help="Asset download cache.",
    )
    parser.add_argument(
        "--build-dir",
        default="build/translate_models",
        help="TFLite/.npub build output.",
    )
    parser.add_argument("--check", action="store_true", help="Verify committed artifacts offline.")
    args = parser.parse_args(argv)

    gen_dir = TOOL_ROOT / "generated"
    gen_dir.mkdir(parents=True, exist_ok=True)
    if args.check:
        errs = check_committed_artifacts(gen_dir)
        if errs:
            for err in errs:
                print(f"translate_models check error: {err}", file=sys.stderr)
            return 1
        print("translate_models --check: OK")
        return 0

    targets = args.build or ["det", "ko", "zh", "ja"]
    cache_arg = Path(args.cache_dir)
    build_arg = Path(args.build_dir)
    cache_dir = cache_arg if cache_arg.is_absolute() else REPO_ROOT / cache_arg
    build_dir = build_arg if build_arg.is_absolute() else REPO_ROOT / build_arg
    vela_gen = _load_vela_gen()

    manifest_path = gen_dir / "model_manifests.json"
    manifest: dict[str, object] = (
        json.loads(manifest_path.read_text(encoding="utf-8"))
        if manifest_path.is_file()
        else {
            "pinned_vela_version": PINNED_VELA_VERSION,
            "pinned_vela_args": list(PINNED_VELA_ARGS),
            "sram_budget_kib": ETHOS_U55_SRAM_BUDGET_KIB,
            "models": {},
        }
    )
    models_map = manifest["models"]  # type: ignore[index]
    for name in targets:
        models_map[name] = _build_one_model(  # type: ignore[index]
            MODEL_SPECS[name],
            cache_dir=cache_dir,
            build_dir=build_dir,
            gen_dir=gen_dir,
            vela_gen=vela_gen,
        )

    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    if all(k in models_map for k in ("det", "ko", "zh", "ja")):  # type: ignore[operator]
        (gen_dir / "translate_model_goldens.zig").write_text(
            _format_golden_zig(manifest),
            encoding="utf-8",
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
