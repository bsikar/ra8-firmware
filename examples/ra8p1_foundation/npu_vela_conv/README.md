# npu_vela_conv

The real-Vela counterpart to `npu_vela`. Where `npu_vela` runs the stand-in
add-constant container, this app runs the conv_int8 model compiled by Arm's
Vela 5.1.0 for the Ethos-U55-256 and checks it against the TFLite Micro golden.

The main is Zig (`src/main.zig`), with no C of its own. It imports two
generated files under `tools/vela/generated/`:

- `ra8_npu_model_conv_int8_vela.zig`: the distilled `.npub` container, byte for
  byte the bytes of the C header beside it.
- `conv_int8_vela_golden.zig`: the 256-byte input and the 256-byte output TFLM
  produces for it.

The flow is the firmware's own loader and driver end to end: `ra8_npu_load()`
resolves the container into a 1 KiB arena (scratch, input and output regions),
the golden input is copied into the input region (BASEP3), then
`ra8_npu_submit()`, `ra8_npu_run()` and `ra8_npu_wait()`. The console prints
`npu_vela_conv: PASS` only when all 256 output bytes (BASEP4) equal the golden.

Running it under `ra8_emulator --part ra8p1` is tracked on the emulator side.
