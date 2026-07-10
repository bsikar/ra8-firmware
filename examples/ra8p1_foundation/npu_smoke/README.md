# npu_smoke -- RA8P1 Ethos-U55 NPU foundation

Exercises the full `ra_npu` command/queue driver surface on the Renesas
**RA8P1** (`R7KA8P1KFLCAC`) -- the defining RA8P1 feature over the RA8D2. It
proves the Arm Ethos-U55 NPU driver (`libs/ra_hal/{inc,src}/ra_npu.{h,c}`)
compiles and links for the RA8P1 and documents the intended call sequence.

## What it does

1. `ra_npu_init()` -- release the NPU module-stop (MSTPCRA bit 16) and soft-reset.
2. `ra_npu_read_id()` -- read the `NPU_ID` presence/revision probe.
3. `ra_npu_submit()` -- program `QBASE`/`QSIZE` at a placeholder command stream in
   SRAM plus one tensor-region base (`BASEP0`).
4. `ra_npu_run()` -- kick the job (`CMD.transition_to_running_state`).
5. `ra_npu_read_status()` -- read back `STATUS` into a debugger-visible global.

## Build

```sh
cd examples/ra8p1_foundation/npu_smoke
make                 # -> build/npu_smoke.elf, built with cmake/toolchain-ra8p1.cmake
make size
```

`cmake/toolchain-ra8p1.cmake` adds `-DRA_DEVICE_RA8P1`, which makes
`libs/ra_core/inc/ra_device.h` define `RA_HAS_NPU` and compiles the otherwise
device-gated `ra_npu.c`. A `#error` guard in `main.c` fails the build loudly if
it is ever configured with the RA8D2 toolchain.

## Status

**Build-foundation only -- NOT hardware-validated.** There is no RA8P1 board
yet, and the "command stream" here is a zeroed placeholder, not a real
Vela-compiled program, so a real inference would not complete on silicon. The
host unit test `tests/test_ra_npu.c` asserts the exact register submission
sequence with mock MMIO. A Vela command-stream compiler, a TFLite-micro runtime,
and on-silicon inference bring-up are tracked as follow-up issues on the RA8P1
epic (#220).
