# dualcore_background_m33 -- M33 as autonomous co-processor, M85 yields

Demonstrates the **producer/consumer** dual-core pattern: the Cortex-M33
runs a counting loop entirely independently while the Cortex-M85 does nothing
but wait. Contrast with `dualcore_mailbox`, where the M85 drives every
request/reply round.

## What it teaches

- **M33 as autonomous co-processor.** Once released, the M33 runs its own
  loop without any M85 involvement. The M85 only observes shared SRAM.
- **Producer/consumer with shared SRAM.** `dualcore_background.h` pins a
  three-word struct at `0x22100000` (the start of the upper on-chip SRAM
  region, below the M33's 64 KiB bank at `0x22190000`), visible to both
  cores. The M33 writes; the M85 reads.
- **M85 yielding.** After releasing the M33 and confirming it booted (via
  the signature field), the M85 sits in a bounded poll on `done` and does
  nothing else until the M33 finishes.

## How to run (no hardware needed)

```sh
make emu-dualcore_background_m33
```

This cross-builds Debug (so log lines are compiled in), builds the ra8_emulator
emulator, and boots the M85 ELF. ra8_emulator sees the embedded `.cpu1_image`
and spins up a second Unicorn engine for the M33 sharing the SRAM buffer.

## Expected output

```
  cpu1 engine   : Cortex-M33, shared SRAM (dual-core)
[itm] [M85] INFO: ==== RA8D2 dualcore_background_m33 demo ====
[itm] [M85] INFO: Cortex-M85 primary core online
[itm] [M85] INFO: shared block in SRAM at 0x22100000
[itm] [M85] INFO: releasing Cortex-M33 secondary core ...
[itm] [M85] INFO: ra8_cpu1_release rc (0 = ok)=0
[itm] [M85] INFO: M33 is alive
[itm] [M85] INFO: M85 yielding -- waiting for M33 to finish counting ...
[itm] [M85] INFO: M33 counted to=1000
[itm] [M85] INFO: dualcore_background_m33 PASS
```

## Files

| File                      | Role                                                |
|---------------------------|-----------------------------------------------------|
| `main.c`                  | M85: release M33, wait, read counter, log verdict   |
| `cpu1_main.c`             | M33: stamp signature, count 1000x, set done         |
| `dualcore_background.h`   | Shared struct + address + constants                 |
| `linker_script.ld`        | M85 memory map; pins `.cpu1_image` at MRAM_CPU1     |
| `linker_script_cpu1.ld`   | M33 memory map (MRAM_CPU1 + SRAM_CPU1)              |
| `system_init.c`           | M85 core bring-up (D-cache off)                     |
| `vector_table.c`          | M85 vector table + Reset_Handler                    |
| `trustzone_init.c`        | SAU scaffold (not invoked in single-world build)    |
| `CMakeLists.txt`          | Builds both images; embeds M33 into M85             |
| `Makefile`                | Per-app build / flash / debug wrapper               |
