# npu_vela -- RA8P1 Ethos-U55 .npub container loader end-to-end

An end-to-end **loader demonstration** on the Renesas **RA8P1** (`R7KA8P1KFLCAC`),
on the way to issue #227: instead of hand-building a command stream in SRAM (as
`npu_smoke` does), this app **loads a committed, generated `.npub` model
container** through the on-target loader `ra8_npu_load()` and runs it on the
Ethos-U55 NPU.

The container's command stream is the documented **SE55 sim convention**
(`libs/ra8_hal/inc/ra8_npu_sim_cmd.h`), **not** a Vela-compiled program. So this
app exercises the full offline-build -> load -> submit -> run path
deterministically, but it does **not** close #227: lowering a real quantized
`.tflite` into a genuine Ethos-U55 command stream with Arm's Vela compiler, and
pinning a golden to *that*, remains open (see **Status**).

## What it does

1. `ra8_npu_init()` -- release the NPU module-stop (MSTPCRA bit 16) and soft-reset.
2. `ra8_npu_read_id()` -- read the `NPU_ID` presence/revision probe.
3. `ra8_npu_load()` -- validate the committed `.npub` container
   (`tools/vela/generated/ra8_npu_model_addk_sim.h`, produced offline by
   `tools/vela/vela_gen.py`) and map it into an `ra8_npu_job_t`: the command
   stream plus every resolved region base (baked weights/input inside the blob,
   the output activation carved from a runtime SRAM arena).
4. `ra8_npu_submit()` + `ra8_npu_run()` + `ra8_npu_wait()` -- program `QBASE`/
   `QSIZE`/`BASEPn`, kick the job, and block for completion.
5. Read the output arena back and assert it equals `input + K` byte-for-byte,
   then print the verdict over the SCI8 console:

```
npu-vela: id=0x10060000 load=OK run=OK out=0x........ verdict=PASS
```

## Build

```sh
cd examples/ra8p1_foundation/npu_vela
make                 # -> build/npu_vela.elf, built with cmake/toolchain-ra8p1.cmake
make size
```

`cmake/toolchain-ra8p1.cmake` adds `-DRA8_DEVICE_RA8P1`, which makes
`libs/ra8_core/inc/ra8_device.h` define `RA8_HAS_NPU` and compiles the otherwise
device-gated `ra8_npu.c` + `ra8_npu_loader.c`. A `#error` guard in `main.c` fails
the build loudly if it is ever configured with the RA8D2 toolchain.

## Run in the emulator

```sh
tools/ra8_emulator/build/ra8_emulator build/npu_vela.elf --device ra8p1
```

ra8_emulator only maps the Ethos-U55 window (`0x40140000`) under `--device ra8p1`;
its NPU model decodes the container's command stream (the documented SE55 sim
convention, see `libs/ra8_hal/inc/ra8_npu_sim_cmd.h`) and applies the op to the
tensor arenas, so the run is deterministic. The output checkword matches
`npu_smoke`'s, proving the loader-built job is byte-identical to the hand-built
one.

## Status

**Build-foundation only -- NOT hardware-validated.** There is no RA8P1 board
yet, and the container's command stream is the SE55 sim convention, not a real
Vela-compiled program, so a real inference would not complete on silicon. The
host unit test `tests/test_ra8_npu_loader.c` byte-pins the loader's extracted
command stream + region layout with mock MMIO. **Issue #227 (a Vela-compiled
Ethos-U55 command stream, with a golden pinned to real Vela output) therefore
stays open** -- this app is a foundation on the way to it, not its closure.
Distilling a real `_vela.tflite` into a `.npub`, a TFLite-micro runtime, and
on-silicon inference bring-up are tracked as follow-ups on the RA8P1 epic
(#220).
