# ereader_m33 -- render the held e-reader page on the M33, M85 parks (#150)

The #150 power-saving model: an e-reader spends almost all its time idle on a
rendered page, so the M85 @ 1 GHz is wasted holding it. This app hands the
**reader** to the Cortex-M33 @ 250 MHz and parks the M85 -- "power saving =
drop to the slow core."

## What it teaches

- **The render runs on the secondary core, through the production gfx stack.**
  The M33 validates the baked, already-inflated `RABOOK1` blob
  (`rabook_fixture.h`, a two-chapter demo book), walks chapter 0's DOM
  iteratively (no recursion) through the header-only `ra_book.h` inline
  accessors to collect its opening page of text, then RENDERS that page into an
  **RGB565 framebuffer in external SDRAM** with the real `ra_gfx` text path
  (`ra_gfx_init` / `ra_gfx_clear` / `ra_gfx_text_out` + the bundled 8x16 font).
  `ra_gfx` is three dependency-clean, zero-heap, scalar (no-Helium) TUs, so it
  links into the freestanding M33 image with no logging backend, no panel
  driver, and no malloc.
- **The framebuffer lives in modelled SDRAM (0x68000000).** The M33 places its
  256x64 RGB565 plane in a `.sdram_bss` (NOLOAD) section the CPU1 linker script
  pins at the SDRAM base -- the same arena-in-SDRAM pattern the sibling
  `compile_on_m33` uses -- and **publishes** the framebuffer base, geometry,
  format, glyph count and a CRC-32 into the shared SRAM mailbox, the way
  `compile_on_m33` publishes its emitted blob.
- **The M33 self-CRCs its pixels; the gate asserts the CRC against a golden.**
  On the board_sim emulator the two cores share only the on-chip SRAM mailbox;
  each core's external-SDRAM window is a separate mapping, so the parked M85
  cannot read the bytes the M33 wrote at 0x68000000. The M33 therefore reads its
  own SDRAM framebuffer back to fold a CRC-32 (reading the pixels back is itself
  the proof they landed) and publishes it; the M85 narrates that value. On
  silicon the single physical SDRAM is shared, so an M85 re-read would match.
- **M85 parks, M33 holds the page.** After validating the published descriptor
  the M85 logs the verdict banner and drops into low-power WFI; the M33 holds
  the rendered page -- the idle posture an e-reader spends almost all its time
  in, now at a fraction of the power.

## How to run (no hardware needed)

```sh
make sim-ereader_m33                                  # watch both cores live
examples/ek_ra8d2/hw_pending/ereader_m33/sim_render_gate.sh   # CRC-gated check
```

`make sim-ereader_m33` cross-builds Debug (so log lines are compiled in), builds
board_sim, and boots the M85 ELF; board_sim sees the embedded `.cpu1_image` and
spins up a second Unicorn engine for the M33. `sim_render_gate.sh` does the same
headlessly and asserts the rendered framebuffer's CRC against the golden.

## Expected output

```
[itm] [M85] INFO: ==== RA8D2 ereader_m33 demo ====
[itm] [M85] INFO: releasing Cortex-M33 secondary core ...
[itm] [M85] INFO: M33 reader is alive
[itm] [M85] INFO: M85 idle; waiting for the M33 to render the held page ...
[itm] [M85] INFO: M33 published fb_base=1744830464
[itm] [M85] INFO: M33 held-page glyphs=128
[itm] [M85] INFO: M33 held-page pixels CRC32 (decimal)=3233445075
[itm] [M85] INFO: ereader_m33: rgb565 256x64 sdram crc=C0BA74D3 PASS
[itm] [M85] INFO: M85 parked in low-power WFI; M33 holds the page
```

`fb_base=1744830464` is `0x68000000` -- the SDRAM base. The `crc=C0BA74D3` is the
CRC-32 the M33 folded over the 256x64 RGB565 pixels it rendered; it is non-zero
(the plane is not blank) and deterministic (the page text, font, and geometry
are fixed), which is the golden `sim_render_gate.sh` asserts.

## What the next increment needs

This increment proves the M33 renders a real, CRC-stable page through `ra_gfx`
into modelled SDRAM. The next steps for #150 are: feeding the render through the
full `ra_reflow` pagination engine (it needs ~735 KiB of engine state, so it
must live in SDRAM); pointing the GLCDC scan-out plane at the M33's framebuffer
for a real display-plane handoff; and the M85 CGC clock-gate / WFI park plus the
wake-on-touch path back to the M85 -- sketched in the #150 design notes.

## Files

| File                    | Role                                                     |
|-------------------------|----------------------------------------------------------|
| `main.c`                | M85: release M33, read the published CRC, verdict, park  |
| `cpu1_main.c`           | M33: validate book, collect text, ra_gfx-render, publish |
| `ereader_m33.h`         | Shared mailbox + SDRAM/format constants                  |
| `rabook_fixture.h`      | Baked, inflated two-chapter `RABOOK1` demo book          |
| `linker_script.ld`      | M85 memory map; pins `.cpu1_image` at MRAM_CPU1          |
| `linker_script_cpu1.ld` | M33 memory map (MRAM_CPU1 + SRAM_CPU1 + SDRAM)           |
| `system_init.c`         | M85 core bring-up (D-cache off)                          |
| `vector_table.c`        | M85 vector table + Reset_Handler                         |
| `trustzone_init.c`      | SAU scaffold (not invoked in single-world build)         |
| `CMakeLists.txt`        | Builds both images; links ra_gfx into the M33 image      |
| `Makefile`              | Per-app build / flash / debug wrapper                    |
| `sim_render_gate.sh`    | board_sim CRC gate (asserts the rendered page's CRC)     |
