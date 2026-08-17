# npu_infer

The inference-runtime companion to `npu_smoke`, which covers the bare
command/queue driver. This app drives the pieces a real TensorFlow Lite for
Microcontrollers Ethos-U runtime needs on top of that driver, and checks each
end to end on the RA8P1:

- **Quantization both ways** -- a float input arena is quantized to the UINT8
  tensor the NPU reads, and the UINT8 result is dequantized back to float.
- **IRQ-driven completion** -- the job is awaited on the NPU interrupt, cleared
  from the ISR, rather than busy-waited.
- **The TFLite-Micro Ethos-U operator is really linked and registered**, which
  makes this app the proof that the vendored TFLite-Micro runtime builds and
  links end to end for the RA8P1.

## Scope

The NPU operator run in the emulator is the tiny deterministic add-constant of
the documented stand-in convention, not a real Vela command stream -- that
needs the offline Vela compiler and silicon. The emulator's NPU model is honest
about it and rejects a real Vela stream it cannot interpret. So the quantize,
NPU op, dequantize pipeline and the interrupt completion path are checkable
byte for byte here, while the model-driven interpreter path is proven to link
and register and runs end to end on silicon later.

There is no RA8P1 HIL rig, so this app is discovered by the emulator suite and
not by the bench runner.
