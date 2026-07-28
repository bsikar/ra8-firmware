# SOUP Justification: FlatBuffers

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting FlatBuffers into this firmware
as Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: FlatBuffers
- **Version**: 25.9.23 (`include/flatbuffers/base.h`
  `FLATBUFFERS_VERSION_MAJOR/MINOR/REVISION = 25/9/23`), pinned to upstream tag
  `v25.9.23` (commit `187240970746d00bbd26b0f5873ed54d2477f9f3`). The pin
  previously recorded `edbe17738352418245d7228e7fd9f12c3ddc34c4`, which is the
  annotated *tag object* rather than the commit it points at -- a distinction
  that matters because the weekly OSV scan materialises each pin as a commit
  and would have queried an oid that is not one (#548).
- **Upstream URL**: https://github.com/google/flatbuffers
- **Local path**: `libs/third_party/flatbuffers/`
  - Files in tree: `include/flatbuffers/*.h` (30 headers) and `LICENSE`.
    Headers only -- the read/verify path. No `flatc` compiler, no codegen, no
    library sources are vendored.

## Provenance

- **Origin**: Google.
- **License**: Apache-2.0 (`LICENSE`).
- **How it entered our tree**: vendored header set from the upstream `v25.9.23`
  release. This version is not arbitrary -- it is the exact FlatBuffers pin
  TFLite-micro declares in `tools/make/flatbuffers_download.sh`, so the model
  schema the runtime was generated against matches the headers here.

## Use case in this firmware

- The `.tflite` model format is a FlatBuffer. TFLite-micro's
  `schema_generated.h` and `flatbuffer_utils` consume these headers to read the
  model (operators, tensors, quantization params) at load time.
- Integrity claim category: data-handling (model deserialization of trusted,
  locally staged models).

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4:

- **Service history**: FlatBuffers is a mature, widely deployed zero-copy
  serialization library from Google, used in production across mobile, games,
  and the entire TensorFlow Lite ecosystem since 2014.
- **Open-source community process**: active Google-led project with CI, a
  published release cadence, and a versioned schema-evolution contract.
- **Version matching**: pinned to the same tag TFLite-micro vendors, eliminating
  schema/runtime skew.

## Risk mitigation

- Only the read path is used, on trusted locally staged models.
- Compiled behind the SOUP boundary as part of the `tflite_micro` object library
  (`cmake/tflite_micro.cmake`), never pulled into first-party include paths
  except through that interface target.
- Exempt from the first-party gates as vendored SOUP under `libs/third_party/`.

## Deviations / patches

None. The vendored headers are unmodified.

## Last review date

- Reviewed: 2026-07-10
- Expected re-review by: 2027-07-10
