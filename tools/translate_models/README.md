# `tools/translate_models` -- Offline NPU Model Pipeline for `ra8_translate`

Host build tool that fetches pinned Apache-2.0 OCR ONNX checkpoints and SIL OFL
fonts, converts fixed-shape NHWC Keras models completely offline (no `onnx2tf`
network calls), quantizes to full-integer INT8 TFLite, compiles with pinned
Arm Ethos-U Vela (`5.1.0`), distills `.npub` containers via
[`tools/vela/src/vela_gen.py`](../vela/src/vela_gen.py), emits compact `.tchr`
CTC character tables for [`libs/ra8_translate`](../../libs/ra8_translate/README.md),
and generates deterministic Zig/binary golden test fixtures.

Never ships on the MCU. Committed outputs stay compact (`.tchr`, manifest JSON,
and golden fixtures); raw `.onnx`, `.tflite`, and `.npub` binaries are emitted
into git-ignored `build/translate_models/`.

## Pinned Assets and Licenses

| Role | Upstream Model / Font | SHA-256 | License |
| --- | --- | --- | --- |
| `det` | `ch_ppocr_mobile_v2.0_det_infer.onnx` (`SWHL/RapidOCR`) | `6800a946ab4222d7b96b71706cbd2c8e926b45ed4c0ddeebd0b221186f321f1f` | Apache-2.0 |
| `ko` | `ko_monkt_rec.onnx` (`monkt/paddleocr-onnx`) | `322f140154c820fcb83c3d24cfe42c9ec70dd1a1834163306a7338136e4f1eaa` | Apache-2.0 |
| `ko` dict | `ko_monkt_dict.txt` (`monkt/paddleocr-onnx`) | `a88071c68c01707489baa79ebe0405b7beb5cca229f4fc94cc3ef992328802d7` | Apache-2.0 |
| `zh` | `ch_PP-OCRv4_rec_infer.onnx` (`SWHL/RapidOCR`) | `48fc40f24f6d2a207a2b1091d3437eb3cc3eb6b676dc3ef9c37384005483683b` | Apache-2.0 |
| `zh` dict | `ppocr_keys_v1.txt` (`PaddleOCR release/2.7`) | `28b2362ad4ab2dc38769aa72feb535e3a9ddb3fd2a7585a05920e6393b1dc7f7` | Apache-2.0 |
| `ja` | `inference.onnx` (`tobiichioriguchi/japan_PP-OCRv3_mobile_rec_onnx`) | `2e397c27461751befc82eca78c9b19e87322ebee34e7316468b21856f9977d64` | Apache-2.0 |
| `ja` dict | `japan_dict.txt` (`PaddleOCR release/2.7`) | `1dcfcb41eec90576a945b3084f22ade11ced506e24f14879245b071698f308e8` | Apache-2.0 |
| Fonts | `NotoSansCJK-{JP,KR,SC,TC}-Regular.otf` | pinned in `src/model_registry.py` | OFL-1.1 |

## Commands

Run offline verification of committed `.tchr` tables, manifests, unit tests, and
real-model `libs/ra8_translate` Zig C ABI golden parity:

```bash
python3 -m unittest discover -s tools/translate_models/tests -v
python3 tools/translate_models/src/build_translate_models.py --check
zig build test --build-file tools/translate_models/build.zig
```

Regenerate all `.npub` containers, `.tchr` tables, manifests, and goldens in a
CUDA PyTorch (`cu128`) + TensorFlow + `ethos-u-vela==5.1.0` environment:

```bash
python3 tools/translate_models/src/build_translate_models.py --build det ko zh ja
```

## Vela Contract and Architecture Notes

- **Pinned compiler:** `ethos-u-vela==5.1.0` with `--accelerator-config ethos-u55-256`,
  `--optimise Performance`, `--memory-mode Shared_Sram`, `--system-config Ethos_U55_High_End_Embedded`.
- **SRAM budget & CPU fallback ops:** all four compiled models fit inside the
  `1664 KiB` Ethos-U55 SRAM budget with **0 CPU-fallback operators** (`[]`,
  100.0% NPU offload inside the compiled `.npub` graphs). Image preprocessing
  (`ra8_translate_ocr_preprocess_i8`), DB post-processing/furigana suppression
  (`ra8_translate_det_post_i8`), and CTC greedy decoding
  (`ra8_translate_ctc_decode_i8`) are handled explicitly in Zig/C by
  `libs/ra8_translate`.
- **Vocabulary pruning:**
  - `ko`: KS X 1001 Hangul syllables (`2,446` total classes incl. blank/ASCII/punct/space) -> `generated/ko_charset.tchr`.
  - `zh`: GB2312 + Big5 Level-1 Hanzi (`5,100` total classes) -> `generated/zh_charset.tchr`.
  - `ja`: JIS X 0208 Level-1 Kanji + Kana (`3,191` total classes) -> `generated/ja_charset.tchr`,
    adapted on RTX 5070 CUDA (`cuda:0`, `batch=16`, `steps=700`) for both `ccw_90`
    vertical columns and horizontal text (`ja_vertical_adapt.py`).

## Measured Pipeline Metrics (RTX 5070 + Vela 5.1.0)

Measured from `generated/model_manifests.json`; two back-to-back runs (`B1`, `B2`)
verified byte-for-byte identical `.npub` SHA-256 digests across all four models:

| Model | Input Shape | Classes | Max Diff | Float vs INT8 Eval | SRAM (KiB) | CPU / NPU Ops | `.npub` Size / SHA-256 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `det` | `1x256x256x3` | n/a | `1.49e-08` | Mask IoU `0.9526`, F1 `0.9757` | `1218.83` | `0` / `80` (`100%`) | `650,352 B` (`96a09f94...`) |
| `ko` | `1x48x256x3` | `2,446` | `2.09e-06` | CER float `0.0000` / int8 `0.0714` | `562.86` | `0` / `250` (`100%`) | `2,476,360 B` (`d8a71a6e...`) |
| `zh` | `1x48x256x3` | `5,100` | `2.09e-06` | CER float `0.0000` / int8 `0.2000` | `572.69` | `0` / `322` (`100%`) | `2,854,016 B` (`19f0b89c...`) |
| `ja` | `1x48x256x3` | `3,191` | `2.21e-06` | CER float `0.0000` / int8 `0.1538` | `445.48` | `0` / `355` (`100%`) | `2,140,812 B` (`b75eb14f...`) |
