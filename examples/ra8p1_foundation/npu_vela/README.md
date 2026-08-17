# npu_vela

The loader demonstration: instead of hand-building a command stream in SRAM the
way `npu_smoke` does, this app loads a committed, generated model container
through the on-target loader and runs it on the Ethos-U55. The loader validates
the container and maps it into a job -- the command stream plus every resolved
region base, with baked weights and input inside the blob and the output
activation carved from a runtime SRAM arena -- then submits, runs, waits, and
asserts the output equals the input plus a constant, byte for byte.

Its output checkword matches `npu_smoke`'s, which is the point: it proves the
loader-built job is byte-identical to the hand-built one.

## It does not close the Vela work

The container's command stream is the documented stand-in convention declared
in the NPU headers, **not** a Vela-compiled program. So the app exercises the
full offline-build to load to submit to run path deterministically, while
lowering a real quantized `.tflite` into a genuine Ethos-U55 command stream
with Arm's Vela compiler, and pinning a golden to *that*, remains open (#227).

The emulator maps the Ethos-U55 window only for the RA8P1 device, decodes the
stand-in command stream and applies the op to the tensor arenas, so the run is
deterministic there. The host unit test byte-pins the loader's extracted
command stream and region layout against mock MMIO.

No RA8P1 board exists to run it on, and the stand-in command stream would not
complete a real inference on silicon if one did.
