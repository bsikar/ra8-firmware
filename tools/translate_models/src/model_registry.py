"""Pinned model metadata, upstream sources, and Ethos-U55 compile defaults."""

from __future__ import annotations

from dataclasses import dataclass

OCR_MODEL_KOREAN = 1
OCR_MODEL_JAPANESE = 2
OCR_MODEL_CHINESE = 3

ROTATION_NONE = 0
ROTATION_CCW_90 = 1
ROTATION_CW_90 = 2
ROTATION_180 = 3

COORD_HALF_PIXEL = 0

SCORE_MODE_LOGITS = 0
SCORE_MODE_PROBS = 1

PINNED_VELA_VERSION = "5.1.0"
PINNED_VELA_ARGS: tuple[str, ...] = (
    "--accelerator-config",
    "ethos-u55-256",
    "--optimise",
    "Performance",
    "--memory-mode",
    "Shared_Sram",
    "--system-config",
    "Ethos_U55_High_End_Embedded",
)
ETHOS_U55_SRAM_BUDGET_KIB = 1664.0


@dataclass(frozen=True)
class RemoteAsset:
    """Upstream downloadable artifact with immutable SHA-256 verification."""

    filename: str
    url: str
    sha256: str
    license_id: str
    attribution: str


@dataclass(frozen=True)
class PreprocessSpec:
    """Image geometry, orientation, and normalization parameters matching RA8 C ABI."""

    width: int
    height: int
    mean: tuple[float, float, float]
    inv_std: tuple[float, float, float]
    input_scale: float
    rotation: int = ROTATION_NONE
    coord_mode: int = COORD_HALF_PIXEL


@dataclass(frozen=True)
class ModelPipelineSpec:
    """Complete reproducible configuration for one detector or OCR recognizer model."""

    name: str
    kind: str
    ocr_model_id: int
    onnx_asset: RemoteAsset
    dict_asset: RemoteAsset | None
    preprocess: PreprocessSpec
    score_mode: int
    drop_softmax: bool
    prune_strategy: str
    vertical_adapted: bool = False


DETECTOR_PREPROCESS = PreprocessSpec(
    width=256,
    height=256,
    mean=(0.485, 0.456, 0.406),
    inv_std=(1.0 / 0.229, 1.0 / 0.224, 1.0 / 0.225),
    input_scale=1.0 / 255.0,
)


def _rec_preprocess(width: int, *, rotation: int = ROTATION_NONE) -> PreprocessSpec:
    """Standard PP-OCR [-1, 1] RGB recognizer preprocessing spec."""
    return PreprocessSpec(
        width=width,
        height=48,
        mean=(0.5, 0.5, 0.5),
        inv_std=(2.0, 2.0, 2.0),
        input_scale=1.0 / 255.0,
        rotation=rotation,
    )


FONT_ASSETS: dict[str, RemoteAsset] = {
    "ja": RemoteAsset(
        filename="NotoSansJP.otf",
        url="https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/Japanese/NotoSansCJKjp-Regular.otf",
        sha256="68a3fc98800b2a27b371f2fb79991daf3633bd89309d4ffaa6946fd587f375b5",
        license_id="OFL-1.1",
        attribution="Noto Sans CJK JP (SIL Open Font License 1.1)",
    ),
    "ko": RemoteAsset(
        filename="NotoSansKR.otf",
        url="https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/Korean/NotoSansCJKkr-Regular.otf",
        sha256="6bcb2a0703aa137e874fc2dffa85f6c21ba9a67fa329e81b8c801663af7e992a",
        license_id="OFL-1.1",
        attribution="Noto Sans CJK KR (SIL Open Font License 1.1)",
    ),
    "sc": RemoteAsset(
        filename="NotoSansSC.otf",
        url="https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf",
        sha256="2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b",
        license_id="OFL-1.1",
        attribution="Noto Sans CJK SC (SIL Open Font License 1.1)",
    ),
    "tc": RemoteAsset(
        filename="NotoSansTC.otf",
        url="https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/TraditionalChinese/NotoSansCJKtc-Regular.otf",
        sha256="dce08bd4fd91aa8aa76ed8fea4b694c2dfb8550f67871e326843212ddbeb88b4",
        license_id="OFL-1.1",
        attribution="Noto Sans CJK TC (SIL Open Font License 1.1)",
    ),
}

MODEL_SPECS: dict[str, ModelPipelineSpec] = {
    "det": ModelPipelineSpec(
        name="det",
        kind="detector",
        ocr_model_id=0,
        onnx_asset=RemoteAsset(
            filename="ch_ppocr_mobile_v2.0_det_infer.onnx",
            url="https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv1/ch_ppocr_mobile_v2.0_det_infer.onnx",
            sha256="6800a946ab4222d7b96b71706cbd2c8e926b45ed4c0ddeebd0b221186f321f1f",
            license_id="Apache-2.0",
            attribution="PaddleOCR DBNet Mobile v2.0 via SWHL/RapidOCR (Apache-2.0)",
        ),
        dict_asset=None,
        preprocess=DETECTOR_PREPROCESS,
        score_mode=SCORE_MODE_PROBS,
        drop_softmax=False,
        prune_strategy="none",
    ),
    "ko": ModelPipelineSpec(
        name="ko",
        kind="recognizer",
        ocr_model_id=OCR_MODEL_KOREAN,
        onnx_asset=RemoteAsset(
            filename="ko_monkt_rec.onnx",
            url="https://huggingface.co/monkt/paddleocr-onnx/resolve/main/languages/korean/rec.onnx",
            sha256="322f140154c820fcb83c3d24cfe42c9ec70dd1a1834163306a7338136e4f1eaa",
            license_id="Apache-2.0",
            attribution="PaddleOCR PP-OCRv3 Korean Mobile via monkt/paddleocr-onnx (Apache-2.0)",
        ),
        dict_asset=RemoteAsset(
            filename="ko_monkt_dict.txt",
            url="https://huggingface.co/monkt/paddleocr-onnx/resolve/main/languages/korean/dict.txt",
            sha256="a88071c68c01707489baa79ebe0405b7beb5cca229f4fc94cc3ef992328802d7",
            license_id="Apache-2.0",
            attribution="PaddleOCR Korean character dictionary (Apache-2.0)",
        ),
        preprocess=_rec_preprocess(256),
        score_mode=SCORE_MODE_LOGITS,
        drop_softmax=True,
        prune_strategy="korean_ksx1001",
    ),
    "zh": ModelPipelineSpec(
        name="zh",
        kind="recognizer",
        ocr_model_id=OCR_MODEL_CHINESE,
        onnx_asset=RemoteAsset(
            filename="ch_PP-OCRv4_rec_infer.onnx",
            url="https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv4/ch_PP-OCRv4_rec_infer.onnx",
            sha256="48fc40f24f6d2a207a2b1091d3437eb3cc3eb6b676dc3ef9c37384005483683b",
            license_id="Apache-2.0",
            attribution="PaddleOCR PP-OCRv4 Chinese Mobile via SWHL/RapidOCR (Apache-2.0)",
        ),
        dict_asset=RemoteAsset(
            filename="ppocr_keys_v1.txt",
            url="https://raw.githubusercontent.com/PaddlePaddle/PaddleOCR/release/2.7/ppocr/utils/ppocr_keys_v1.txt",
            sha256="28b2362ad4ab2dc38769aa72feb535e3a9ddb3fd2a7585a05920e6393b1dc7f7",
            license_id="Apache-2.0",
            attribution="PaddleOCR PP-OCRv1 Simplified/Traditional Hanzi dictionary (Apache-2.0)",
        ),
        preprocess=_rec_preprocess(256),
        score_mode=SCORE_MODE_LOGITS,
        drop_softmax=True,
        prune_strategy="chinese_gb2312_big5",
    ),
    "ja": ModelPipelineSpec(
        name="ja",
        kind="recognizer",
        ocr_model_id=OCR_MODEL_JAPANESE,
        onnx_asset=RemoteAsset(
            filename="ja_v3_rec.onnx",
            url="https://huggingface.co/tobiichioriguchi/japan_PP-OCRv3_mobile_rec_onnx/resolve/main/inference.onnx",
            sha256="2e397c27461751befc82eca78c9b19e87322ebee34e7316468b21856f9977d64",
            license_id="Apache-2.0",
            attribution="PaddleOCR PP-OCRv3 Japanese Mobile via tobiichioriguchi (Apache-2.0)",
        ),
        dict_asset=RemoteAsset(
            filename="japan_dict.txt",
            url="https://raw.githubusercontent.com/PaddlePaddle/PaddleOCR/release/2.7/ppocr/utils/dict/japan_dict.txt",
            sha256="1dcfcb41eec90576a945b3084f22ade11ced506e24f14879245b071698f308e8",
            license_id="Apache-2.0",
            attribution="PaddleOCR Japanese character dictionary (Apache-2.0)",
        ),
        preprocess=_rec_preprocess(256, rotation=ROTATION_CCW_90),
        score_mode=SCORE_MODE_LOGITS,
        drop_softmax=True,
        prune_strategy="japanese_jis_l1",
        vertical_adapted=True,
    ),
}
