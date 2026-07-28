# Vela -- offline Ethos-U model compiler (host build tool)

[Arm Ethos-U Vela](https://gitlab.arm.com/artificial-intelligence/ethos-u/ethos-u-vela)
is the offline compiler that turns a quantized `.tflite` model into an
Ethos-U55-optimized `.tflite`: the operators the NPU supports are fused and
replaced by a single `ethos-u` custom operator that wraps a Vela-generated
**command stream**, while unsupported operators fall back to CPU kernels in the
on-device runtime.

## This is a build-time HOST tool, not firmware

- **Nothing from Vela is linked into firmware.** It runs on the developer / CI
  host. The on-device inference runtime is the vendored TFLite-micro
  (`libs/third_party/tflite-micro/`), which executes the Vela command stream via
  the Ethos-U operator.
- It is pinned here (a documented, version-pinned build dependency) rather than
  vendored as source SOUP, because it is a Python tool run at build time, and
  the repo keeps host tools under `tools/<name>/`.
- Its qualification record lives at [`docs/SOUP/vela.md`](../../docs/SOUP/vela.md)
  and it is listed in [`THIRD_PARTY_LICENSES.md`](../../THIRD_PARTY_LICENSES.md)
  as a build tool, so it still goes through the vendor process even though it
  ships no code into the tree.

## Version pin

`ethos-u-vela==5.1.0` (Apache-2.0). Pinned so the emitted command stream is
reproducible; the Ethos-U operator protocol Vela targets matches the vendored
TFLite-micro `ethosu` op.

## Usage (Phase 2)

```sh
python3 -m venv .venv-vela && . .venv-vela/bin/activate
pip install -r tools/vela/requirements.txt

# Compile a quantized model for the RA8P1 Ethos-U55 (macs/cycle per the SKU):
vela --accelerator-config ethos-u55-256 \
     --output-dir build/vela \
     model_int8.tflite
# -> build/vela/model_int8_vela.tflite  (bake this as a byte array for TFLM)
```

The `_vela.tflite` output is what a Phase-2 `examples/ra8p1_foundation` app bakes
in and hands to the vendored TFLite-micro `MicroInterpreter`; the `ethos-u`
operator then dispatches the command stream to the NPU through the first-party
`ra8_npu` driver (see `docs/SOUP/tflite-micro.md`, "Phase 2").

## Offline build step + on-target loader (issue #227)

A bare-metal target does not parse a TFLite flatbuffer at run time. The offline
build step distills a model into a lean, linkable `.npub` container -- an
Ethos-U55 command stream plus its tensor region layout -- and the on-target
loader maps that container straight into an `ra8_npu_job_t` for `ra8_npu_submit()`.

- **`tools/vela/vela_gen.py`** -- the offline build step.
  - `emit <descriptor.json> -o <header.h>` turns a committed model descriptor
    (`tools/vela/models/*.json`) into the `.npub` container defined by
    `libs/ra8_hal/inc/ra8_npu_blob.h`, baked as a C byte-array header the firmware
    links.
  - `check <descriptor.json> <header.h>` regenerates the header in memory and
    diffs it against the committed golden (fails on drift).
  - `compile <model.tflite>` runs the pinned Vela on a real quantized model
    (optional -- skips with a notice if Vela is not installed).
- **`libs/ra8_hal/inc/ra8_npu_blob.h`** -- the `.npub` container format.
- **`libs/ra8_hal/{inc,src}/ra8_npu_loader.{h,c}`** -- the front half of the
  on-target loader: validate the container, resolve every region base (baked
  regions -> the blob; runtime regions -> a caller arena), and fill an
  `ra8_npu_job_t`.
- **`tools/vela/generated/ra8_npu_model_addk_fake.h`** -- the committed golden,
  pinned byte-for-byte by `tests/test_ra8_npu_loader.c`.

### Make targets

```sh
make vela-check                 # regenerate-and-diff the golden (NO Vela needed)
make vela-regen                 # rewrite the golden after a descriptor change
make vela-compile TFLITE=m.tflite  # run the pinned Vela on a real .tflite (optional)
```

`make vela-check` needs no Vela toolchain, so a CI job that never installs Vela
still passes; the committed golden's command stream is the documented SE55 stand-in
convention (`libs/ra8_hal/inc/ra8_npu_fake_cmd.h`), the only Ethos-U55 stream this
repo can produce deterministically without a Vela install. Distilling a REAL
`_vela.tflite` into a `.npub` is a documented follow-up on the RA8P1 NPU epic
(it needs a validated Vela output and an RA8P1 board to confirm on silicon).
