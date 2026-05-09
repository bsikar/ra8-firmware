# cpu1_pingpong

Dual-core (Cortex-M85 CPU0 + Cortex-M33 CPU1) ping-pong demo for the
EK-RA8D2.

## Behaviour

CPU0 (M85): releases CPU1, sends 0x1234 over IPC1, waits for 0x4321
on IPC0, loops.

CPU1 (M33), built as a separate ELF (`cpu1_pingpong_cpu1.elf`):
waits for 0x1234, replies with 0x4321, loops.

## Build

```sh
cd examples/ek_ra8d2/cpu1_pingpong
make
```

Produces `build/cpu1_pingpong.hex` (CPU0) and
`build/cpu1_pingpong_cpu1.hex` (CPU1).

## See also

- `docs/DUAL_CORE.md`
- `libs/ra_hal/inc/ra_dual_core.h`
- `libs/ra_hal/inc/ra_ipc.h`
