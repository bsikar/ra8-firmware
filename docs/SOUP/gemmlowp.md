# SOUP Justification: gemmlowp (fixed-point headers)

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting the gemmlowp fixed-point headers
into this firmware as Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: gemmlowp (fixed-point subset)
- **Version**: no upstream release tag; pinned to upstream commit
  `719139ce755a0f31cbf1c37f7f98adcc7fc9f425` (2018-09-04).
- **Upstream URL**: https://github.com/google/gemmlowp
- **Local path**: `libs/third_party/gemmlowp/`
  - Files in tree: `fixedpoint/*.h` (`fixedpoint.h` + the per-ISA
    `fixedpoint_{neon,sse,avx,msa}.h`, all guarded by `#ifdef` so none is
    active on Cortex-M85), `internal/detect_platform.h`, and `LICENSE`.
    Header-only subset -- the full gemmlowp GEMM machinery is NOT vendored.

## Provenance

- **Origin**: Google (gemmlowp -- low-precision GEMM library).
- **License**: Apache-2.0 (`LICENSE`).
- **How it entered our tree**: vendored header subset from the upstream commit
  above. The exact commit is recorded by `scripts/gen/sbom_registry.py` and
  `docs/sbom/upstream/gemmlowp.manifest`, keeping the checked-in subset and
  generated SBOM under one provenance authority.

## Use case in this firmware

- TFLite-micro's quantized reference kernels (`conv`, `depthwise_conv`,
  `fully_connected`, `softmax`, ...) include `fixedpoint/fixedpoint.h` for
  saturating fixed-point rounding/rescaling math. These are the only gemmlowp
  facilities the vendored kernels touch.
- Integrity claim category: data-handling (fixed-point arithmetic on trusted
  tensor data).

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4:

- **Service history**: gemmlowp's fixed-point headers are the canonical
  quantized-math primitives underneath TensorFlow Lite; in production across the
  TFLite ecosystem for years.
- **Open-source community process**: Google-authored, open GitHub project.
- **Version matching**: pinned to the same commit TFLite-micro vendors, so the
  kernels and the fixed-point headers are the version pair they were tested as.

## Risk mitigation

- Header-only, `#ifdef`-guarded ISA selection: on Cortex-M85 none of the NEON /
  SSE / AVX / MSA paths compile in, so only the portable scalar path is used.
- Compiled behind the SOUP boundary as part of the `tflite_micro` object library
  (`cmake/tflite_micro.cmake`).
- Exempt from the first-party gates as vendored SOUP under `libs/third_party/`.

## Deviations / patches

None. The vendored headers are unmodified.

## Last review date

- Reviewed: 2026-07-10
- Expected re-review by: 2027-07-10
