# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Pinned Bergamot (Firefox Translations) model registry and provenance metadata."""

from __future__ import annotations

from dataclasses import dataclass

DEFAULT_CDN_BASE = "https://firefox-settings-attachments.cdn.mozilla.net/"
LICENSE_SPDX = "MPL-2.0"
LICENSE_URL = "https://github.com/mozilla/firefox-translations-models/blob/main/LICENSE"
REMOTE_SETTINGS_RECORDS_URL = (
    "https://firefox.settings.services.mozilla.com/v1/buckets/main/"
    "collections/translations-models/records"
)


@dataclass(frozen=True)
class ArtifactPin:
    """One pinned upstream binary artifact fetched from Mozilla Remote Settings."""

    filename: str
    location: str
    size: int
    sha256: str

    def url(self, cdn_base: str = DEFAULT_CDN_BASE) -> str:
        """Return full download URL for this pinned artifact."""
        return f"{cdn_base.rstrip('/')}/{self.location.lstrip('/')}"


@dataclass(frozen=True)
class LanguagePairSpec:
    """Declarative Bergamot pair definition for reproducible conversion."""

    pair: str
    nmt_model_id: int
    source_lang: str
    target_lang: str
    pair_tag: str
    architecture: str
    source_version: str
    license_spdx: str
    license_url: str
    model: ArtifactPin
    vocab: ArtifactPin
    shortlist: ArtifactPin


# Adding a new language pair (such as ja-en or zh-en after ko-en) is a single
# entry in PAIRS pointing to its pinned model, vocab, and shortlist hashes.
PAIRS: dict[str, LanguagePairSpec] = {
    "ko-en": LanguagePairSpec(
        pair="ko-en",
        nmt_model_id=1,
        source_lang="ko",
        target_lang="en",
        pair_tag="koen",
        architecture="base-memory",
        source_version="2.1",
        license_spdx=LICENSE_SPDX,
        license_url=LICENSE_URL,
        model=ArtifactPin(
            filename="model.koen.intgemm.alphas.bin",
            location="main-workspace/translations-models/bf6d1f85-351f-4934-934d-8a7b0d118270.bin",
            size=43977787,
            sha256="7cb30cbc0a86e242084756445268a2346136771c92b884d9b4ebb077f67d5507",
        ),
        vocab=ArtifactPin(
            filename="vocab.koen.spm",
            location="main-workspace/translations-models/409950da-3a7f-4a0a-bd3e-9f655d7150eb.spm",
            size=1410063,
            sha256="1c72b740ab793cdc3a8f16913dd6b4e806c77421077dd2d85edeb7be38418598",
        ),
        shortlist=ArtifactPin(
            filename="lex.50.50.koen.s2t.bin",
            location="main-workspace/translations-models/66a51334-41dd-4f08-bd22-6016035d6904.bin",
            size=8617516,
            sha256="b26291f063ce216a74239275f0485534a38111203e59d42a2a56ba62d6eeb196",
        ),
    ),
    "ja-en": LanguagePairSpec(
        pair="ja-en",
        nmt_model_id=2,
        source_lang="ja",
        target_lang="en",
        pair_tag="jaen",
        architecture="base-memory",
        source_version="2.1",
        license_spdx=LICENSE_SPDX,
        license_url=LICENSE_URL,
        model=ArtifactPin(
            filename="model.jaen.intgemm.alphas.bin",
            location="main-workspace/translations-models/2b066368-11b5-4cee-92e8-e6156b334f80.bin",
            size=43977787,
            sha256="3a603e20bfe1be86071913f9e23ab5129075bc0a8490151020ac4821e4f17302",
        ),
        vocab=ArtifactPin(
            filename="vocab.jaen.spm",
            location="main-workspace/translations-models/6f4898e3-ebaa-4a79-a1af-93a4f65b96fa.spm",
            size=1443222,
            sha256="5cb217758bae05877bb3f0c2f612e4e7c1e4cb03c10db11f4a47098d7ae62919",
        ),
        shortlist=ArtifactPin(
            filename="lex.50.50.jaen.s2t.bin",
            location="main-workspace/translations-models/4e6b3270-8a2f-491d-b717-f99749328622.bin",
            size=9348172,
            sha256="525f412f0d210536c2933c78ae395fa0bf2b5ee6cc5dda61ebc2e79410ebaee4",
        ),
    ),
    "zh-en": LanguagePairSpec(
        pair="zh-en",
        nmt_model_id=3,
        source_lang="zh-Hans",
        target_lang="en",
        pair_tag="zhen",
        architecture="base-memory",
        source_version="2.1",
        license_spdx=LICENSE_SPDX,
        license_url=LICENSE_URL,
        model=ArtifactPin(
            filename="model.zhen.intgemm.alphas.bin",
            location="main-workspace/translations-models/715efbde-c974-4696-981a-83c0c5f82ad5.bin",
            size=43977787,
            sha256="5cd149601802fc8a18124a1c1306144dbbedc058630c4ddb2d53aa76fa9c7c06",
        ),
        vocab=ArtifactPin(
            filename="vocab.zhen.spm",
            location="main-workspace/translations-models/17ff5d42-6d4c-42f8-ad3c-b8042b5bafa7.spm",
            size=1359697,
            sha256="dff594318ab7d8b7b60b844ab98ebe6b932ae8045fab15235404c787715965b3",
        ),
        shortlist=ArtifactPin(
            filename="lex.50.50.zhen.s2t.bin",
            location="main-workspace/translations-models/ae5f53d2-59ec-4aad-944d-999e50d6a65c.bin",
            size=9219192,
            sha256="8524dd4c93ffd7f0ae7be32f77a7c14fea7cdbbebe34ea6ad0ec368d5ddd8b13",
        ),
    ),
}


def get_pair_spec(pair: str) -> LanguagePairSpec:
    """Look up a pinned language-pair specification or raise KeyError."""
    if pair not in PAIRS:
        msg = f"unsupported pair {pair!r}; expected one of {sorted(PAIRS)}"
        raise KeyError(msg)
    return PAIRS[pair]
