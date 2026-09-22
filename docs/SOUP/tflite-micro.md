# SOUP Justification: TensorFlow Lite for Microcontrollers

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting TensorFlow Lite for
Microcontrollers (TFLite-micro) into this firmware as Software Of Unknown
Provenance (SOUP).

## Component identity

- **Name**: TensorFlow Lite for Microcontrollers (TFLite-micro)
- **Version**: no upstream release tag; pinned to upstream commit
  `fddd3707a3c5733af4cb866f18650441e6712504` (2026 default branch).
- **Upstream URL**: https://github.com/tensorflow/tflite-micro
- **Local path**: `libs/third_party/tflite-micro/`
  - Vendored as a **lean subset** (see below), preserving the upstream
    repository-root include layout so `#include "tensorflow/lite/micro/..."`
    resolves unchanged.

### Vendored subset (what is IN)

- **Core runtime**: `tensorflow/lite/micro/` (MicroInterpreter, MicroAllocator,
  the arena allocators, the greedy/linear memory planners, the op resolver,
  the tflite_bridge), minus tests, benchmarks, examples, and every
  target-specific port directory (`cortex_m_generic/`, `cmsis_nn/`,
  `xtensa/`, `arc_*/`, `ceva/`, `hexagon/`, `riscv32_generic/`, `bluepill/`,
  `chre/`, `compression/`, `python/`, `tools/`, `models/`, `docs/`).
- **Shared TFLite runtime** it depends on: `tensorflow/lite/core/`,
  `tensorflow/lite/c/`, `tensorflow/lite/kernels/internal/` (+ `kernel_util`,
  `op_macros.h`, `padding.h`), `tensorflow/lite/schema/`,
  `tensorflow/lite/types/`, the top-level `tensorflow/lite/*.h`, and the
  `tensorflow/compiler/mlir/lite/` error-reporter + schema helpers.
- **Reference kernels** (the modest op set for the first Ethos-U models):
  `conv`, `depthwise_conv`, `fully_connected`, `add`, `mul`, `reshape`,
  `softmax`, `pooling` (AVERAGE_POOL_2D), plus the shared `*_common.cc`,
  `kernel_util.cc`, and `micro_tensor_utils.cc`.
- **Ethos-U custom operator**: the portable stub
  `tensorflow/lite/micro/kernels/ethosu.cc` (`Register_ETHOSU()` returns
  `nullptr`) is present in the vendored tree but **excluded from the build** by
  `cmake/tflite_micro.cmake`. In its place the build compiles the first-party
  kernel `libs/ra8_hal/src/ra8_ethosu_kernel.cc`, whose `tflite::Register_ETHOSU()`
  dispatches the Ethos-U55 command stream to the NPU through the first-party
  `ra8_ethosu_shim` / `ra8_npu` driver (see "Phase 2" below -- landed).
- Two declaration-only headers `signal/micro/kernels/{irfft,rfft}.h` are kept
  because `micro_ops.h` includes them; the signal (audio/FFT) op
  implementations and kissfft are NOT vendored.

### Deliberately OUT (leanness)

Audio/FFT signal ops and kissfft, every optimized kernel backend
(CMSIS-NN / Xtensa / ARC / CEVA), all `*_test.cc` / benchmarks / examples /
model data, and all kernels outside the op set above. The `create_tflm_tree`
default output (which pulls every kernel + kissfft) was intentionally NOT used.

### Build dependencies (vendored as sibling SOUP components)

TFLite-micro does not stand alone; three of its build dependencies are vendored
as their own `libs/third_party/` components (each with its own SOUP doc and SBOM
entry), not nested under this tree:

- `flatbuffers` (`docs/SOUP/flatbuffers.md`) -- the `.tflite` model format.
- `gemmlowp` (`docs/SOUP/gemmlowp.md`) -- fixed-point math for quantized kernels.
- `ruy` (`docs/SOUP/ruy.md`) -- a profiler instrumentation stub header.

The Arm `ethos-u-core-driver` is deliberately NOT vendored: this project owns a
first-party NPU driver (`libs/ra8_hal` `ra8_npu`) that the Ethos-U operator will
call in Phase 2.

## Provenance

- **Origin**: Google / the TensorFlow Authors (TFLite-micro project).
- **License**: Apache-2.0 (`LICENSE`, mirrored at
  `libs/third_party/tflite-micro/LICENSE`).
- **How it entered our tree**: vendored subset of the upstream repository at the
  pinned commit above. The subset is reproducible: check out that commit and
  copy the directory set listed under "Vendored subset".

## Use case in this firmware

- On-device neural-network inference on the RA8P1 (R7KA8P1KFLCAC), whose Arm
  Ethos-U55 NPU is driven through the first-party `ra8_npu` driver. TFLite-micro
  parses the Vela-compiled model, plans the tensor arena, walks the operator
  graph, and hands Ethos-U subgraphs to the NPU.
- Integrity claim category: data-handling (model parsing, tensor arithmetic).
  Models are trusted, locally staged assets baked into firmware or loaded from
  the project's own storage -- there is no network-driven model path.

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4:

- **Service history**: TFLite-micro is the reference microcontroller inference
  runtime from the TensorFlow project, widely deployed across Cortex-M and
  Ethos-U products and used as the conformance runtime for Arm's Ethos-U tooling.
- **Open-source community process**: active Google-led project with continuous
  integration, a published contribution process, and per-op unit tests upstream.
- **Vendor alignment**: the Ethos-U operator protocol vendored here is the same
  one Arm's Vela compiler emits command streams for, so the offline compiler and
  the on-device runtime are version-matched by construction.
- **Bug tracker review**: upstream issues reviewed at pin time; the vendored op
  set is limited to mature reference kernels.

## Risk mitigation

- Runs only on trusted, locally staged models (no untrusted-network model path).
- Static-memory build (`TF_LITE_STATIC_MEMORY`): the interpreter uses a
  caller-owned arena with no heap after init (NASA Rule 3 friendly).
- Compiled behind the SOUP boundary (`-w -fno-strict-aliasing`, C++17,
  `-fno-rtti -fno-exceptions -fno-threadsafe-statics -fno-use-cxa-atexit`) via
  `cmake/tflite_micro.cmake`, isolated as an OBJECT library so it never drags
  its include tree into first-party code.
- Exempt from the first-party C23 / Doxygen / MC/DC gates as vendored SOUP under
  `libs/third_party/`; this document is the case-by-case justification for that
  exemption.

## Phase 2: Ethos-U operator -> ra8_npu adapter (adapter landed; #228 remains open)

Phase 1 vendored the portable `ethosu.cc` stub. Phase 2 replaces it, in-build,
with a first-party kernel: `cmake/tflite_micro.cmake` drops the vendored stub
from the object library (a SOUP *file-selection* change -- see "Deviations")
and compiles `libs/ra8_hal/src/ra8_ethosu_kernel.cc` in its place. That kernel's
`tflite::Register_ETHOSU()` returns a real registration whose `invoke` translates
the node's tensors into the Arm Ethos-U operator ABI and drives the NPU through
the first-party `ra8_ethosu_shim` (no Arm `ethos-u-core-driver` is vendored). The
`examples/ra8p1_foundation/npu_infer` app links the runtime (`USES tflite_micro`)
and asserts the operator is registered via `ra8_ethosu_kernel_available()`; a full
Vela-model-driven inference additionally needs the offline Vela compiler
(`tools/vela`) and silicon and remains a follow-up. Those unexecuted model and
CPU-fallback paths are why issue #228 remains open.

The real dispatch upstream lives at
`tensorflow/lite/micro/kernels/ethos_u/ethosu.cc` and calls the Arm
`ethos-u-core-driver` (`#include <ethosu_driver.h>`):

```
struct ethosu_driver* drv = ethosu_reserve_driver();
result = ethosu_invoke_v3(drv, cms_data, cms_data_size,
                          base_addrs, base_addrs_size, num_tensors, ext_ctx);
ethosu_release_driver(drv);
```

Phase 2 replaces that with the first-party `ra8_npu` driver
(`libs/ra8_hal/inc/ra8_npu.h`). The mapping is direct:

| Arm core-driver call                | `ra8_npu` equivalent |
| ----------------------------------- | ------------------- |
| `ethosu_reserve_driver()`           | one-time `ra8_npu_init()` at boot; reserve becomes a no-op (single NPU) |
| `ethosu_invoke_v3(cms, len, base_addrs, sizes, n, ...)` | fill `ra8_npu_job_t{ .cmd_stream = cms, .cmd_stream_bytes = len, .region_base[i] = base_addrs[i] }`, then `ra8_npu_submit(&job)` + `ra8_npu_run()` + `ra8_npu_wait()` |
| `ethosu_release_driver(drv)`        | no-op (single NPU) |

The command stream (`cms_data`) is the Vela output; the base-address array maps
onto `ra8_npu_job_t.region_base[]` (region 0 = weight/bias arena). A first
end-to-end test needs: (1) a tiny Vela-compiled `.tflite` for Ethos-U55, baked
as a byte array; (2) the adapter above wired into an `examples/ra8p1_foundation`
app that reuses the `npu_smoke` boot files; (3) the ra8_emulator Ethos-U model
(added separately) to gate it headlessly before on-silicon bring-up.

## Deviations / patches

None to the vendored file *contents*: every file under
`libs/third_party/tflite-micro/` is unmodified upstream content. The changes from
upstream are both *file-selection* choices, not source edits:

1. the lean subset (which files are vendored), documented above; and
2. the Ethos-U operator: the vendored portable stub
   `tensorflow/lite/micro/kernels/ethosu.cc` is left byte-for-byte unmodified but
   **excluded from the build** by `cmake/tflite_micro.cmake`, which compiles the
   first-party `libs/ra8_hal/src/ra8_ethosu_kernel.cc` in its place (Phase 2,
   above). This mirrors upstream's own build-time choice between
   `kernels/ethosu.cc` (stub) and `kernels/ethos_u/ethosu.cc` (real), so no
   vendored file is patched.

## Last review date

- Reviewed: 2026-07-10
- Expected re-review by: 2027-07-10
