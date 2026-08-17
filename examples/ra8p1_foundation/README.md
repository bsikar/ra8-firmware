# examples/ra8p1_foundation/

The second target. These apps prove the platform is genuinely multi-chip rather
than an RA8D2 codebase with a device macro, and they exercise the Ethos-U55 NPU
-- the RA8P1's defining feature over the RA8D2. There is no RA8P1 board on the
bench, so they are gated by building, by the host tests, and in the emulator.

| App | |
|---|---|
| [`blink_ra8p1/`](blink_ra8p1/) | `ra8_core` + `ra8_hal` compile and link for the R7KA8P1KFLCAC. The foundation everything else here stands on. |
| [`npu_smoke/`](npu_smoke/) | The bare `ra8_npu` command/queue driver surface, and the call sequence it expects. |
| [`npu_vela/`](npu_vela/) | Loads a committed `.npub` container through `ra8_npu_load()` instead of hand-building a command stream. Built by [`tools/vela`](../../tools/vela/README.md). |
| [`npu_infer/`](npu_infer/) | The pieces a TFLite-Micro Ethos-U runtime needs above the driver -- quantization, arenas, dispatch -- checked end to end. |

`make <appname>` from the repo root, same as any other example.
