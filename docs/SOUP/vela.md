# SOUP Justification: Arm Ethos-U Vela (host build tool)

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for the Arm Ethos-U Vela compiler. Vela is a
**host-side build tool**, not firmware SOUP: no Vela code is linked into the
firmware image. It is recorded here because it goes through the same vendor
process as the runtime components (owner requirement) and because the model
command streams it emits are a build input to the shipped firmware.

## Component identity

- **Name**: Arm Ethos-U Vela
- **Version**: pinned `ethos-u-vela==5.1.0`
  (the `vela` dependency group in `pyproject.toml`, resolved by `uv.lock`).
- **Upstream URL**:
  https://gitlab.arm.com/artificial-intelligence/ethos-u/ethos-u-vela
  (PyPI: https://pypi.org/project/ethos-u-vela/)
- **Local path**: not vendored as source. Pinned as a documented build
  dependency in the `vela` group in `pyproject.toml`, resolved by
  `uv.lock`; usage notes in
  `tools/vela/README.md`.

## Provenance

- **Origin**: Arm Limited (Ethos-U project).
- **License**: Apache-2.0.
- **How it is consumed**: installed on the developer / CI host into a throwaway
  uv-managed environment from committed `pyproject.toml` and `uv.lock`;
  run offline at build time. It is
  never cross-compiled or linked into the RA8P1 image.

## Use case in this firmware

- Offline compilation of a quantized `.tflite` model into an
  Ethos-U55-optimized `.tflite`: NPU-supported operators are fused into a single
  `ethos-u` custom operator wrapping a Vela **command stream**; unsupported
  operators remain as CPU kernels for the on-device runtime.
- The `_vela.tflite` output is baked into a Phase-2 `examples/ra8p1_foundation`
  application and executed by the vendored TFLite-micro `MicroInterpreter`. The
  `ethos-u` operator dispatches the command stream to the NPU via the
  first-party `ra8_npu` driver (see `docs/SOUP/tflite-micro.md`, "Phase 2").

## Qualification basis

- **Vendor tool**: Vela is Arm's own reference compiler for the Ethos-U NPU
  family; the command-stream format it emits is the contract the on-device
  Ethos-U operator implements. Compiler and runtime are version-matched by
  construction.
- **Build-time only**: a defect in Vela cannot corrupt firmware at runtime; it
  can only produce an incorrect model artifact, which is caught by end-to-end
  model output verification (Phase 2 test).
- **Reproducibility**: pinned to an exact version so command-stream output is
  deterministic across hosts.

## Risk mitigation

- Runs in an isolated virtualenv on the host; not part of the firmware trust
  boundary.
- Its output (`_vela.tflite`) is validated by comparing on-device inference
  results against a golden reference before a model is accepted (Phase 2).

## Deviations / patches

None. Consumed as the unmodified upstream PyPI release.

## Last review date

- Reviewed: 2026-07-10
- Expected re-review by: 2027-07-10
