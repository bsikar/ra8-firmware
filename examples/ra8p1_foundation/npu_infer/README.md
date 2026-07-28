# npu_infer -- RA8P1 Ethos-U55 inference runtime smoke (#228)

The inference-runtime companion to [`npu_smoke`](../npu_smoke), which exercises
the bare `ra8_npu` command/queue driver. `npu_infer` drives the pieces a real
TensorFlow Lite for Microcontrollers Ethos-U runtime needs on top of the driver,
and checks each end-to-end on the RA8P1 profile:

1. **Input/output quantization** (`ra8_npu_quant`) -- a float input arena is
   quantized to the UINT8 tensor the NPU reads (affine `scale` + `zero_point`),
   and the UINT8 result is dequantized back to float.
2. **IRQ-driven completion** (`ra8_npu_irq_arm` / `ra8_npu_wait_irq`, with
   `ra8_npu_irq_handler` routed through `ra8_isr`) -- the job is awaited on the
   NPU interrupt (`ra8_npu_clear_irq` from the ISR), not a busy-wait.
3. **TFLite-micro Ethos-U operator wiring** (`ra8_ethosu_kernel_available`) --
   asserts the real first-party `tflite::Register_ETHOSU()` is linked and
   registered. This app links the vendored TFLite-micro runtime via
   `USES tflite_micro`, so it is also the proof that `RA8_USE_TFLITE_MICRO`
   builds + links end-to-end for the RA8P1.

## Build + run

```sh
make                                 # -> ./build/npu_infer.elf (RA8P1)
../../../tools/ra8_emulator/build/ra8_emulator ./build/npu_infer.elf --device ra8p1
```

Expected console line:

```
npu-infer: id=0x10060000 tflm=OK irq=OK out=0x........ verdict=PASS
```

## Scope / honesty

The NPU "operator" run under ra8_emulator is the tiny deterministic add-constant of
the documented `ra8_npu_fake_cmd.h` convention -- NOT a real Vela command stream,
which needs the offline Vela compiler (`tools/vela`, a follow-up) and silicon.
ra8_emulator's honest NPU model rejects a real Vela stream it cannot interpret, so
the quantize -> NPU op -> dequantize pipeline plus the IRQ completion path are
checkable byte-for-byte in the emulator here, while the TFLite-micro
`MicroInterpreter` model-driven path is proven to **link + register** and runs
end-to-end on silicon later.

EIL-only: there is no RA8P1 HIL rig, so this app is discovered by
`scripts/emu/eil_all.sh` (ra8_emulator) but not by `scripts/hil/all.sh`.
