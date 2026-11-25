# `tools/translate_eval`

License-clean multilingual (`ja`, `zh`, `ko` -> `en`) evaluation set and host
benchmarking harness for `libs/ra8_translate`.

## Evaluation Set Provenance & Licensing

`tools/translate_eval/corpus/eval_corpus.json` contains **12 author-original
synthetic comic/webtoon pages** (4 per language, 24 labeled speech bubbles)
authored under `MIT` specifically for this repository. Zero third-party or
academic-only manga dataset images or annotations are included.

## What It Measures

1. **Detection (`Det P`, `Det R`, `Det F1`)**:
   Greedy 1-to-1 bounding-box matching at `IoU >= 0.50`, executing
   `ra8_translate_det_post_f32` (`float`) vs `ra8_translate_det_post_i8`
   (`host_i8`, including furigana suppression).
2. **OCR (`OCR CER`)**:
   Codepoint-level Levenshtein Character Error Rate (`edits / ref_codepoints`)
   over UTF-8 scalar values, executing `ra8_translate_ctc_decode_f32` vs
   `ra8_translate_ctc_decode_i8` on `TCHR` charsets.
3. **Translation (`NMT chrF`)**:
   Character n-gram F-score (`chrF6`, `n = 1..6`, `beta = 2.0`, reported in
   percent `0..100`) comparing pre-quant float32 scoring (`float`) vs quantized
   `TMNT` `ra8_translate_marian_greedy_decode` + `ra8_translate_tok_decode`
   (`host_i8`) and reporting total weight bytes read.

## Running From One Command

```sh
zig build --build-file tools/translate_eval/build.zig run
```

Additional CLI flags:

- `--json` -- emit machine-readable JSON metrics
- `--corpus <path>` -- evaluate an external corpus JSON matching schema 1
- `--emu-results <path>` -- score target emulator trace JSON alongside `float`
  and `host_i8`

Host test suite:

```sh
zig build --build-file tools/translate_eval/build.zig test --summary all
```

## Baseline Results (Synthetic Evaluation Set)

| Lang | Mode | Pages | Bubbles | Det P | Det R | Det F1 | OCR CER | NMT chrF | Furigana Dropped | Weight Bytes |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `ja` | `float` | 4 | 8 | 1.000 | 1.000 | 1.000 | 0.0000 | 100.00 | 2 | 0 |
| `zh` | `float` | 4 | 8 | 1.000 | 1.000 | 1.000 | 0.0000 | 100.00 | 0 | 0 |
| `ko` | `float` | 4 | 8 | 1.000 | 1.000 | 1.000 | 0.0000 | 100.00 | 0 | 0 |
| `overall` | `float` | 12 | 24 | 1.000 | 1.000 | 1.000 | 0.0000 | 100.00 | 2 | 0 |
| `ja` | `host_i8` | 4 | 8 | 1.000 | 0.875 | 0.933 | 0.0263 | 96.94 | 2 | 139496 |
| `zh` | `host_i8` | 4 | 8 | 1.000 | 0.875 | 0.933 | 0.0278 | 95.05 | 0 | 147772 |
| `ko` | `host_i8` | 4 | 8 | 1.000 | 0.875 | 0.933 | 0.0185 | 93.97 | 0 | 206680 |
| `overall` | `host_i8` | 12 | 24 | 1.000 | 0.875 | 0.933 | 0.0234 | 95.31 | 2 | 493948 |

Note: physical `.npub` / trained Bergamot NMT weights await separate model
conversion/training slices (`RA8FW-661..670`), so default runs report measured
`float` vs `host_i8` C ABI metrics over deterministic synthetic `TCHR`/`TVOC`/
`TMNT` fixtures and mark `emulator` as `n/a` unless `--emu-results` is passed.
