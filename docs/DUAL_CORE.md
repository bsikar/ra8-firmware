# Dual-core (CPU0 / CPU1) on the RA8D2

The Renesas RA8D2 ships two Arm cores:

| Core | Arch       | Clock    | TCM      | Use case in this firmware                              |
|------|------------|----------|----------|--------------------------------------------------------|
| CPU0 | Cortex-M85 | 1 GHz    | 64 KiB   | Primary core. All apps under `examples/` run here.     |
| CPU1 | Cortex-M33 | 250 MHz  | 32 KiB   | Secondary core. Off by default; used by opt-in apps.   |

CPU0 is the boot core. On power-up the chip jumps to the M85 reset
vector and CPU1 is held in the LPCSTPCH1=1 stopped state. CPU1 is
released by CPU0 firmware once the SoC is initialized.

## Memory map split

The default split used by `examples/ek_ra8d2/cpu1_pingpong`:

| Region    | Origin       | Length  | Owner | Purpose                          |
|-----------|--------------|---------|-------|----------------------------------|
| MRAM      | `0x02000000` | 768 KiB | CPU0  | M85 code + rodata + vectors      |
| MRAM_CPU1 | `0x020C0000` | 256 KiB | CPU1  | M33 code + rodata + vectors      |
| SRAM      | `0x22000000` | 960 KiB | CPU0  | M85 .data, .bss, main stack      |
| SRAM_CPU1 | `0x223F0000` | 64 KiB  | CPU1  | M33 .data, .bss, main stack      |
| ITCM      | `0x00000000` | 64 KiB  | CPU0  | M85 instruction TCM              |
| DTCM      | `0x20000000` | 64 KiB  | CPU0  | M85 data TCM                     |

CPU1 has its own TCM banks but the demo does not use them; sticking to
plain MRAM/SRAM keeps the linker scripts simple.

The CPU1 image is linked with `linker_script_cpu1.ld`; its vector
table is placed at the start of `MRAM_CPU1` so VTORC1 latches the
right address.

## IPC channel layout

The IPC peripheral exposes two units, each with two FIFO channels.
The HAL convention (encoded in `ra_ipc_channel_for_send` /
`ra_ipc_channel_for_recv`):

| Channel id | Unit | FIFO   | Direction       | Used by              |
|-----------:|------|--------|-----------------|----------------------|
| 0          | IPC0 | FIFO00 | CPU1 -> CPU0    | M33 send / M85 recv  |
| 1          | IPC0 | FIFO01 | CPU1 -> CPU0    | M33 send / M85 recv  |
| 2          | IPC1 | FIFO10 | CPU0 -> CPU1    | M85 send / M33 recv  |
| 3          | IPC1 | FIFO11 | CPU0 -> CPU1    | M85 send / M33 recv  |

Each channel carries 32-bit words backed by a 4-deep FIFO (HUM Ch 3.1
p 204).

## Boot sequence

```
+-------------------+                 +-------------------+
|     CPU0 (M85)    |                 |     CPU1 (M33)    |
+-------------------+                 +-------------------+
| 1. SoC boot ROM   |                 |  (held in stop:   |
|    -> Reset_Handler|                |   LPCSTPCH1 = 1)  |
| 2. SystemInit     |                 |                   |
| 3. main():        |                 |                   |
|    ra_cpu1_release|                 |                   |
|      -> VTORC1    |                 |                   |
|      -> MSPC1     |                 |                   |
|      -> clear     |                 |                   |
|         LPCSTPCH1 |                 |                   |
|      -> poll STAT |---- release --->| Cpu1_Reset_Handler|
| 4. ra_ipc_init    |                 |   -> cpu1_main()  |
| 5. send 0x1234 -->|=== IPC1 ch2 ===>| recv 0x1234       |
|                   |<== IPC0 ch0 ====|<-- send 0x4321    |
| 6. recv 0x4321    |                 |                   |
| 7. loop           |                 | loop              |
+-------------------+                 +-------------------+
```

The release sequence matches HUM Ch 11.4:

1. Write CPU1 reset vector base into SYSC `VTORC1` (128-byte aligned).
2. Write CPU1 initial main stack into SYSC `MSPC1` (8-byte aligned).
3. Clear `LPCSR.LPCSTPCH1` (bit 1).
4. Poll `LPCSR.LPCSTPCH1_STAT` (bit 17) for clear.

To halt CPU1 again, set `LPCSR.LPCSTPCH1 = 1`.

### HUM gotcha (provisional naming)

The publicly-released RA8D2 HUM draft committed under
`docs/reference/` does not name the multi-core control registers as
explicitly as the FSP source does for the closely-related RA8M1 /
RA8D1 family. We use the FSP names (`LPCSR`, `VTORC1`, `MSPC1`) at
the SYSC offsets `0x490 / 0x494 / 0x498`. If a future HUM revision
renames these, only the offset enum + accessors in
`libs/ra_hal/src/ra_dual_core.c` need to change.

## When to use CPU1 vs CPU0

Use **CPU0** (default) for:
- Anything requiring Helium / MVE (M85-only).
- Workloads that want the full 1 GHz clock and the M85 cache.
- TrustZone-secure side code.
- All existing HAL drivers, ThreadX apps, networking, USB, GUIX.

Use **CPU1** (opt-in) for:
- Hard-real-time control loops needing deterministic latency
  without competing with the M85 cache.
- Sensor pre-processing pipelines the M85 can wake on demand.
- Power-isolated workloads -- CPU1 can be halted with
  `ra_cpu1_halt()` to drop its 250 MHz contribution when idle.

CPU1 has neither MVE nor an FPU on the M85's scale; do not move
signal-processing code there blindly.
