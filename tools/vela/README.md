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
`ra_npu` driver (see `docs/SOUP/tflite-micro.md`, "Phase 2").
