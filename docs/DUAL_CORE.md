# Dual-core (CPU0 / CPU1) on the RA8D2

The Renesas RA8D2 ships two Arm cores:

| Core | Arch       | Clock    | TCM      | Use case in this firmware                              |
|------|------------|----------|----------|--------------------------------------------------------|
| CPU0 | Cortex-M85 | 1 GHz    | 64 KiB   | Primary core. All apps under `examples/` run here.     |
| CPU1 | Cortex-M33 | 250 MHz  | 32 KiB   | Secondary core. Off by default; used by opt-in apps.   |

CPU0 is the boot core. On power-up the chip jumps to the M85 reset
vector and CPU1 is held inactive (CPU1ACTCSR.ACT = 0). CPU1 is
released by CPU0 firmware once the SoC is initialized.

## Memory map split

The default split used by `examples/ek_ra8d2/hw_validated/hil/cpu1_pingpong`:

| Region    | Origin       | Length  | Owner | Purpose                          |
|-----------|--------------|---------|-------|----------------------------------|
| MRAM      | `0x02000000` | 768 KiB | CPU0  | M85 code + rodata + vectors      |
| MRAM_CPU1 | `0x020C0000` | 256 KiB | CPU1  | M33 code + rodata + vectors      |
| SRAM      | `0x22000000` | 1024 KiB | CPU0 | M85 .data, .bss, main stack      |
| SRAM_CPU1 | `0x22190000` | 64 KiB  | CPU1  | M33 .data, .bss, main stack      |
| ITCM      | `0x00000000` | 64 KiB  | CPU0  | M85 instruction TCM              |
| DTCM      | `0x20000000` | 64 KiB  | CPU0  | M85 data TCM                     |

CPU1 has its own TCM banks but the demo does not use them; sticking to
plain MRAM/SRAM keeps the linker scripts simple.

The CPU1 image is linked with `linker_script_cpu1.ld`; its vector
table is placed at the start of `MRAM_CPU1` so `CPU1INITVTOR` latches
the right address.

## IPC channel layout

The **validated** cross-core path (`cpu1_pingpong`) is a shared-SRAM mailbox,
not the IPC peripheral. A small `volatile` struct is pinned at `0x22100000`
(the start of the upper on-chip SRAM region, below the M33's `0x22190000`
bank); both linker scripts leave that word unclaimed, so a write by one core is
seen by the other. The IPC FIFOs are unusable for that demo because `IPCSAR`'s
security attribution is mutually exclusive (Secure XOR Non-secure) and the M33
is permanently Non-secure (SECEXT disabled, HUM Ch 2): a single channel cannot
bridge the Secure M85 and the Non-secure M33.

The IPC peripheral's channel map is exercised only by the bench-pending
`cpu1_pingpong_ipc` variant, which first programs `IPCSAR` to make a channel
pair Non-secure. The HAL convention (encoded in `ra8_ipc_channel_for_send` /
`ra8_ipc_channel_for_recv`):

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
| 1. SoC boot ROM   |                 |  (held inactive:   |
|    -> Reset_Handler|                |   CPU1ACTCSR.ACT=0)|
| 2. SystemInit     |                 |                   |
| 3. main():        |                 |                   |
|    ra8_cpu1_release |                 |                   |
|      -> CPU1INITVTOR                 |                   |
|      -> clear CPUWAIT                |                   |
|      -> CPU1ACTCSR = 0xA501          |                   |
|      -> poll ACT  |---- release --->| cpu1_reset_handler|
| 4. (shared-SRAM   |                 |   -> cpu1_main()  |
|     mailbox setup) |                |                   |
| 5. write mailbox->|=== SRAM @ ======>| read mailbox      |
| 6. read reply  <--|<== 0x22100000 ==|<-- write reply    |
| 7. loop           |                 | loop              |
+-------------------+                 +-------------------+
```

The release sequence matches `libs/ra8_hal/src/ra8_dual_core.c` (the source of
truth) and HUM Ch 2.9.1 "CPU control registers" (p 128-130). All three
registers live in the CPU_CTRL block at base `0x4000F000`:

1. Write the CPU1 reset vector base into `CPU1INITVTOR` (@ 0x044, 128-byte
   aligned). The M33 loads its initial SP + PC from that vector table.
2. Clear `CPU1WAITCR.CPUWAIT` (@ 0x054, bit 0) so the M33 runs on activation
   rather than parking in the post-reset wait.
3. Write `CPU1ACTCSR` (@ 0x064) = `0xA501` -- KEY `0xA5` in bits 15:8 plus
   `ACTREQ` (bit 0) -- to request activation.
4. Poll `CPU1ACTCSR.ACT` (bit 7) until set; the M33 is now running.

To halt CPU1 again, `ra8_cpu1_halt()` re-asserts the inactive state.

### Note: these are the real registers, not FSP placeholders

Earlier drafts named FSP-style `LPCSR` / `VTORC1` / `MSPC1` registers at
`0x4001E000` -- those do NOT exist on the RA8D2, and writes there are silently
dropped. The RA8D2 HUM Ch 2.9.1 names the CPU_CTRL registers above explicitly,
and `ra8_dual_core.c` is JTAG-confirmed against them (cpu1_pingpong:
`CPU1INITVTOR=0x020C0000`, `CPU1ACTCSR.ACT` set, rc=0). The shared-SRAM mailbox
(rather than the IPC peripheral) is the validated cross-core path: IPC is
blocked by `IPCSAR` secure-only attribution while the M33 boots Non-Secure.

## When to use CPU1 vs CPU0

Use **CPU0** (default) for:
- Anything requiring Helium / MVE (M85-only).
- Workloads that want the full 1 GHz clock and the M85 cache.
- TrustZone-secure side code.
- All existing HAL drivers, ThreadX apps, networking, USB.

Use **CPU1** (opt-in) for:
- Hard-real-time control loops needing deterministic latency
  without competing with the M85 cache.
- Sensor pre-processing pipelines the M85 can wake on demand.
- Power-isolated workloads -- CPU1 can be halted with
  `ra8_cpu1_halt()` to drop its 250 MHz contribution when idle.

CPU1 has neither MVE nor an FPU on the M85's scale; do not move
signal-processing code there blindly.

The two cores in their full context -- worlds, modules, mailbox and the radio:

<img src="diagrams/system_map.svg" alt="RA8 system map: the M85 and M33 cores, the TrustZone split, the mailbox between them, and the ESP32-C6 over SPI" width="100%">
