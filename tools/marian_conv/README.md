# Bergamot Marian NMT Converter (`tools/marian_conv`)

Converts pinned **Mozilla Bergamot (Firefox Translations)** `intgemm8` Marian models, SentencePiece
`.spm` vocabularies, and `lex.50.50.*.s2t.bin` lexical shortlists into deterministic, CRC-32-checked
firmware containers (`TMNT`, `TVOC`, `TSLT`) consumed by `libs/ra8_translate`.

Container wire specifications live in [`docs/formats/TMNT.md`](../../docs/formats/TMNT.md).

## Pinned Upstream Models (`MPL-2.0`)

All pinned models come from Mozilla Remote Settings (`translations-models`, `MPL-2.0`,
https://github.com/mozilla/firefox-translations-models/blob/main/LICENSE) and are stored as
`base-memory` architectures with SSRU recurrent decoder cells (`dec-cell: ssru`) and zero step-0
decoder initialization (`bos = -1`):

| Pair | Version | Architecture | License | Artifacts (SHA-256 verified) |
|---|---|---|---|---|
| `ko-en` (primary) | `2.1` | `base-memory` (SSRU) | `MPL-2.0` | `model.koen.intgemm.alphas.bin`, `vocab.koen.spm`, `lex.50.50.koen.s2t.bin` |
| `ja-en` | `2.1` | `base-memory` (SSRU) | `MPL-2.0` | `model.jaen.intgemm.alphas.bin`, `vocab.jaen.spm`, `lex.50.50.jaen.s2t.bin` |
| `zh-en` | `2.1` | `base-memory` (SSRU) | `MPL-2.0` | `model.zhen.intgemm.alphas.bin`, `vocab.zhen.spm`, `lex.50.50.zhen.s2t.bin` |

Exact CDN paths, byte sizes, and SHA-256 digests are declared in [`src/manifest.py`](src/manifest.py).
Adding another pair is a single `LanguagePairSpec` entry in `PAIRS`. Converted `.tmnt`/`.tvoc`/`.tslt`
artifacts are kept out of git; Zig-verified reference goldens (single-sequence token IDs, per-step
logits + SHA-256 digests, and batched `TSLT` shortlist parity) live in
[`goldens/koen_reference_golden.json`](goldens/koen_reference_golden.json),
[`goldens/jaen_reference_golden.json`](goldens/jaen_reference_golden.json), and
[`goldens/zhen_reference_golden.json`](goldens/zhen_reference_golden.json).

## Usage

```bash
# List pinned pairs and license metadata
python tools/marian_conv/src/marian_conv.py pairs

# Download & SHA-256 verify raw Bergamot v2.1 assets into a local scratch cache
python tools/marian_conv/src/marian_conv.py fetch --pair ko-en --cache-dir /tmp/bergamot_cache

# Convert into <pair_tag>.tmnt, <pair_tag>.tvoc, <pair_tag>.tslt, and <pair_tag>.manifest.json
python tools/marian_conv/src/marian_conv.py convert --pair ko-en \
  --input-dir /tmp/bergamot_cache/koen --out-dir /tmp/bergamot_out

# Inspect converted containers
python tools/marian_conv/src/marian_conv.py inspect \
  --tmnt /tmp/bergamot_out/koen.tmnt --tvoc /tmp/bergamot_out/koen.tvoc

# Build the Zig C ABI parity runner & emit Zig-verified greedy/logit goldens
zig build --build-file tools/marian_conv/zig_runner/build.zig -Doptimize=ReleaseSafe
python tools/marian_conv/src/marian_conv.py emit-golden --pair ko-en \
  --model-dir /tmp/bergamot_out \
  --out-json tools/marian_conv/goldens/koen_reference_golden.json \
  --zig-runner tools/marian_conv/zig_runner/zig-out/bin/marian_zig_runner
```

## Verification

```bash
pytest tools/marian_conv/tests/test_marian_conv.py
zig build --build-file tools/marian_conv/zig_runner/build.zig test --summary all
```
