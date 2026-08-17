# blink_m33

The canonical "put the work on the M33" template (issue #152). The Cortex-M85
does the minimum -- release the Cortex-M33, then sleep -- and the M33 owns the
application. Here that application is blinking LED1; anything portable drops
into `cpu1_main.c` the same way.

The two cores are compiled independently for two different architectures and
stitched into a single `.hex`: the M33 image is embedded in the M85 ELF as
`.cpu1_image` by the shared `ra8_add_cpu1_image()` CMake helper, so one SWD
flash brings up both cores and there is no objcopy dance to copy-paste.

Release sequence, HUM Ch 2.9.1 "CPU control registers": `CPU1INITVTOR` <- the
M33 MRAM base, clear `CPU1WAITCR.CPUWAIT`, write `CPU1ACTCSR` <- KEY|ACTREQ,
poll `ACT`. The M85 then drops into a `WFI` idle loop -- heavy core asleep, lean
core working, which is the low-power posture the offload work builds on.

The proof of life is deliberately the LED and not a log line. The M33 drives
PORT6 `PCNTR1` itself and the M85 never writes that pin, so a blinking LED1 can
only mean the M33 left reset and is running its own code. (The emulator echoes
only the primary core's ITM, so the M33 is silent there by design.)

| Region    | Origin     | Size  | Holds                       |
|-----------|------------|-------|-----------------------------|
| MRAM      | 0x02000000 |       | M85 code plus `.cpu1_image` |
| MRAM_CPU1 | 0x020C0000 | 256 K | M33 code, rodata, vectors   |
| SRAM_CPU1 | 0x22190000 | 64 K  | M33 `.data`, `.bss`, stack  |
