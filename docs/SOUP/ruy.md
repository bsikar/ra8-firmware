# SOUP Justification: ruy (profiler instrumentation stub)

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting the ruy profiler instrumentation
header into this firmware as Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: ruy (profiler instrumentation subset)
- **Version**: no upstream release tag; pinned to upstream commit
  `d37128311b445e758136b8602d1bbd2a755e115d` (2021-05-11).
- **Upstream URL**: https://github.com/google/ruy
- **Local path**: `libs/third_party/ruy/`
  - Files in tree: `ruy/profiler/instrumentation.h` and `LICENSE`. A **single**
    header -- the ruy GEMM backend is NOT vendored.

## Provenance

- **Origin**: Google (ruy -- matrix multiplication library).
- **License**: Apache-2.0 (`LICENSE`).
- **How it entered our tree**: vendored single header from the upstream commit
  above. The exact commit is recorded by `scripts/gen/sbom_registry.py` and
  `docs/sbom/upstream/ruy.manifest`, keeping the checked-in header and generated
  SBOM under one provenance authority.

## Use case in this firmware

- TFLite-micro's kernel utilities include `ruy/profiler/instrumentation.h` for
  the `ruy::profiler::ScopeLabel` markers sprinkled through the kernels. With
  profiling disabled (the default) these are no-ops, so the header is a
  compile-time dependency only -- no ruy code executes.
- Integrity claim category: none at runtime (no-op instrumentation).

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4:

- **Service history**: ruy is Google's matrix-multiplication library backing
  TensorFlow Lite on CPU; the profiler header is a small, stable part of it.
- **Open-source community process**: Google-authored, open GitHub project.
- **Minimal surface**: exactly one header, providing no-op profiling scopes; the
  attack/defect surface is negligible.
- **Version matching**: pinned to the same commit TFLite-micro vendors.

## Risk mitigation

- Single header; profiling compiled out, so it contributes no executable code.
- Compiled behind the SOUP boundary as part of the `tflite_micro` object library
  (`cmake/tflite_micro.cmake`).
- Exempt from the first-party gates as vendored SOUP under `libs/third_party/`.

## Deviations / patches

None. The vendored header is unmodified.

## Last review date

- Reviewed: 2026-07-10
- Expected re-review by: 2027-07-10
