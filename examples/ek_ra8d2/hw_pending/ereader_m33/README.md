# ereader_m33 -- render the held e-reader page on the M33, M85 parks (#150)

The #150 power-saving model: an e-reader spends almost all its time idle on a
rendered page, so the M85 @ 1 GHz is wasted holding it. This app hands the
**reader** to the Cortex-M33 @ 250 MHz and parks the M85 -- "power saving =
drop to the slow core" -- and then runs the full **MODE-SWITCH cycle**: the M85
parks (CGC clock-gate + WFI), the M33 holds the page and polls a simulated touch,
and on each page turn the M33 wakes the M85 over IPC to do the heavy next-page
work before it re-parks.

## What it teaches

- **The render runs on the secondary core, through the production gfx stack.**
  The M33 validates the baked, already-inflated `RABOOK1` blob
  (`rabook_fixture.h`, a two-chapter demo book), walks chapter 0's DOM
  iteratively (no recursion) through the header-only `ra8_book.h` inline
  accessors to collect its opening page of text, then RENDERS that page into an
  **RGB565 framebuffer in external SDRAM** with the real `ra8_gfx` text path
  (`ra8_gfx_init` / `ra8_gfx_clear` / `ra8_gfx_text_out` + the bundled 8x16 font).
  `ra8_gfx` is three dependency-clean, zero-heap, scalar (no-Helium) TUs, so it
  links into the freestanding M33 image with no logging backend, no panel
  driver, and no malloc.
- **The framebuffer lives in modelled SDRAM (0x68000000).** The M33 places its
  256x64 RGB565 plane in a `.sdram_bss` (NOLOAD) section the CPU1 linker script
  pins at the SDRAM base -- the same arena-in-SDRAM pattern the sibling
  `compile_on_m33` uses -- and **publishes** the framebuffer base, geometry,
  format, glyph count and a CRC-32 into the shared SRAM mailbox, the way
  `compile_on_m33` publishes its emitted blob.
- **The M33 self-CRCs its pixels; the gate asserts the CRC against a golden.**
  On the ra8_emulator the two cores share only the on-chip SRAM mailbox;
  each core's external-SDRAM window is a separate mapping, so the parked M85
  cannot read the bytes the M33 wrote at 0x68000000. The M33 therefore reads its
  own SDRAM framebuffer back to fold a CRC-32 (reading the pixels back is itself
  the proof they landed) and publishes it; the M85 narrates that value. On
  silicon the single physical SDRAM is shared, so an M85 re-read would match.
- **The mode-switch: M85 parks, M33 holds + wakes it on a page turn.** After the
  page-0 verdict the M85 enters the cycle. It **parks** -- writes the CGC
  clock-gate (HOCO stop via the `ra8_lpm` clock-stop matrix) and drops into
  Sleep-mode WFI -- handing the live core to the slow M33. The M33 holds the page
  and polls a **simulated touch**; on each page turn it bumps `turn_req` and
  **pokes the M85 over IPC0** (`ra8_ipc_send_event`, the same #149 wake the
  `compile_on_m33` driver uses). The woken M85 restores its clock, does the
  "heavy" next-page work the 1 GHz core owns, acks `turn_ack`, and re-parks; the
  M33 then **re-renders** the held page (re-folding the identical CRC) and signals
  `turn_done`. This repeats `k_erm33_max_turns` (3) times, fully deterministically,
  then both cores park for good. The M85 narrates each wake and a final handoff
  verdict (ra8_emulator echoes only the primary core's ITM).

## How to run (no hardware needed)

```sh
make emu-ereader_m33                                   # watch both cores live
examples/ek_ra8d2/hw_pending/ereader_m33/emu_render_gate.sh    # page-0 CRC gate
examples/ek_ra8d2/hw_pending/ereader_m33/emu_handoff_gate.sh   # full mode-switch
```

`make emu-ereader_m33` cross-builds Debug (so log lines are compiled in), builds
ra8_emulator, and boots the M85 ELF; ra8_emulator sees the embedded `.cpu1_image` and
spins up a second Unicorn engine for the M33. `emu_render_gate.sh` asserts the
first rendered framebuffer's CRC. `emu_handoff_gate.sh` drives the **full
park / wake / re-render cycle** headlessly using only existing ra8_emulator
mechanisms (dual-core + IPC + WFI + SDRAM) and asserts: the page-0 verdict, one
wake banner per scripted page turn, and the final handoff verdict carrying the
re-render count + the stable CRC.

## Expected output

```
[itm] [M85] INFO: ==== RA8D2 ereader_m33 demo (#150 M85-park / M33-hold) ====
[itm] [M85] INFO: IPC0 receive IRQ armed -- M85 can WFI-wake on a page turn
[itm] [M85] INFO: releasing Cortex-M33 secondary core ...
[itm] [M85] INFO: M33 reader is alive
[itm] [M85] INFO: M33 held-page pixels CRC32 (decimal)=3233445075
[itm] [M85] INFO: ereader_m33: rgb565 256x64 sdram crc=C0BA74D3 PASS
[itm] [M85] INFO: entering #150 mode-switch cycle; M33 holds the page + polls touch
[itm] [M85] INFO: woke from low-power for page turn=1
[itm] [M85] INFO: woke from low-power for page turn=2
[itm] [M85] INFO: woke from low-power for page turn=3
[itm] [M85] INFO: ereader_m33: handoff turns=3 crc=C0BA74D3 PARKED
[itm] [M85] INFO: M85 parked in low-power WFI; M33 holds the page
  IPC           : sends=3 wakes=3
```

`crc=C0BA74D3` is the CRC-32 the M33 folded over the 256x64 RGB565 pixels; it is
non-zero (the plane is not blank) and deterministic (page text, font, and
geometry are fixed), and it stays identical across every re-render. ra8_emulator's
`IPC: sends=3 wakes=3` line corroborates the wake: the M33 poked IPC exactly
three times and the M85's NVIC took three wakes -- one per page turn.

## What ra8_emulator cannot model (HIL follow-ups)

This increment proves the **mode-switch control flow** end to end in ra8_emulator:
the M85 really parks and is woken out of WFI by the M33's IPC poke, and the M33
holds + re-renders deterministically. What the emulator cannot show -- and what
HIL on real silicon must validate (#30) -- is the actual **power delta** of the
M85 park (the CGC clock-gate + WFI is functionally exercised but its current draw
is not modelled), and the **real touch input** (the simulated page-dwell stands
in for a GT911 touch-controller poll). The remaining #150 display work is also
HIL-bound: pointing the GLCDC scan-out plane at the M33's framebuffer for a real
display-plane handoff, and (optionally) feeding the render through the full
`ra8_reflow` pagination engine (~735 KiB of SDRAM-resident state).

## Files

| File                    | Role                                                     |
|-------------------------|----------------------------------------------------------|
| `main.c`                | M85: arm IPC wake, release M33, park/wake cycle, verdict  |
| `cpu1_main.c`           | M33: render + publish, hold + page-turn loop, re-render   |
| `ereader_m33.h`         | Shared mailbox (+ turn handshake) + SDRAM/format/cycle    |
| `rabook_fixture.h`      | Baked, inflated two-chapter `RABOOK1` demo book          |
| `linker_script.ld`      | M85 memory map; pins `.cpu1_image` at MRAM_CPU1          |
| `linker_script_cpu1.ld` | M33 memory map (MRAM_CPU1 + SRAM_CPU1 + SDRAM)           |
| `system_init.c`         | M85 core bring-up (D-cache off)                          |
| `vector_table.c`        | M85 vector table + Reset_Handler                         |
| `trustzone_init.c`      | SAU scaffold (not invoked in single-world build)         |
| `CMakeLists.txt`        | Builds both images; links ra8_gfx + ra8_ipc into the M33   |
| `Makefile`              | Per-app build / flash / debug wrapper                    |
| `emu_render_gate.sh`    | ra8_emulator CRC gate (asserts the page-0 render CRC)       |
| `emu_handoff_gate.sh`   | ra8_emulator gate for the full #150 park/wake/re-render cycle|
