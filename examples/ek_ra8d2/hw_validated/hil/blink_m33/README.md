# blink_m33 -- Cortex-M33 secondary-core LED blink template

The canonical "target the M33" template (issue #152). The **Cortex-M85**
(primary core) does the minimum -- release the **Cortex-M33** secondary core,
then sleep -- and the M33 owns the work. Here the work is blinking LED1; any
portable app drops into `cpu1_main.c` the same way.

## What it shows

1. **Two images, one per core.** The M85 ELF carries an embedded Cortex-M33
   image (`.cpu1_image`, built from `cpu1_main.c`). They are compiled
   independently for two different CPU architectures and stitched into one
   `.hex`, so a single SWD flash brings up both cores. The embedding is done by
   the shared `ra8_add_cpu1_image()` CMake helper (`cmake/ra8_add_app.cmake`) --
   one call, no copy-paste objcopy dance.

2. **The co-processor / low-power model.** The M85 releases the M33 with
   `ra8_cpu1_release()` (HUM Ch 2.9.1 "CPU control registers": `CPU1INITVTOR` <-
   0x020C0000, clear `CPU1WAITCR.CPUWAIT`, `CPU1ACTCSR` <- KEY|ACTREQ, poll
   `ACT`), then drops into a `WFI` idle loop. The heavy M85 asleep + the lean
   M33 working is the low-power posture issue #150 builds on.

3. **Honest proof of life.** The M33 blinks LED1 (BLUE, P600 = PORT6 pin 0) by
   driving PORT6 `PCNTR1` directly. The M85 never writes that pin, so a blinking
   LED can only mean the M33 left reset and is running its own code.

## Run it

```sh
make emu-blink_m33      # both cores in ra8_emulator, with the live board view
make blink_m33          # cross-compile blink_m33.elf (+ blink_m33_cpu1.elf)
```

In the board view, watch **LED1 (BLUE)** toggle; the `[itm]` stream shows the
M85's release log (`ra8_cpu1_release rc (0 = ok)=0`) and then goes quiet as the
M85 idles. (ra8_emulator echoes only the primary core's ITM, so the M33 stays
silent by design -- the LED is its output.)

## Memory map

| Region    | Origin     | Size   | Holds                          |
|-----------|------------|--------|--------------------------------|
| MRAM      | 0x02000000 | ...    | M85 code + the `.cpu1_image`   |
| MRAM_CPU1 | 0x020C0000 | 256 K  | M33 code + rodata + vectors    |
| SRAM_CPU1 | 0x22190000 | 64 K   | M33 `.data`, `.bss`, stack     |

## Status

`hw_pending` -- emulator-validated (the `ra8_cpu1_release` path is JTAG-proven on
silicon via `cpu1_pingpong`), not yet bench-validated on this board.
