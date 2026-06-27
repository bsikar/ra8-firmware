# ereader_m33 -- render the e-reader page on the M33, M85 parks (#150)

The #150 power-saving model: an e-reader spends almost all its time idle on a
rendered page, so the M85 @ 1 GHz is wasted holding it. This app hands the
**reader** to the Cortex-M33 @ 250 MHz and parks the M85 -- "power saving =
drop to the slow core."

## What it teaches

- **Reader data path AND pixel render on the secondary core.** The M33 walks a
  real compiled book: it validates the baked, already-inflated `RABOOK1` blob
  (`rabook_fixture.h`, a two-chapter demo book), then for each chapter walks its
  DOM iteratively (no recursion) through the header-only `ra_book.h` inline
  accessors and paginates the extracted text at `k_erm33_page_chars` characters
  per page. For each page it RENDERS the text into a small 1-bpp framebuffer it
  owns in shared SRAM, using a compact baked-font mono blit (`m33_font.h`, the
  public-domain 8x16 IBM VGA glyphs). No SD card, no decompressor, no heap --
  the M33 image links nothing beyond its own code.
- **M85 parks, M33 renders + holds.** After releasing the M33 the M85 reads the
  framebuffer back for every page and logs a per-page CRC (board_sim echoes only
  the primary core's ITM), then drops into low-power WFI once the M33 reaches the
  last page. A non-zero, page-varying CRC is the proof the M33 produced real
  pixels, not a blank plane.
- **Producer/consumer over shared SRAM.** `ereader_m33.h` pins a 10-word
  progress mailbox at `0x22100000` plus a 2 KiB 1-bpp framebuffer at
  `0x22101000` (both in SRAM2, free on both linker scripts, inside the board_sim
  SRAM window). The M33 renders each page then HOLDS it (`frame_seq`) until the
  M85 acknowledges (`frame_ack`), so the M85's read of the pixels is race-free.

## How to run (no hardware needed)

```sh
make sim-ereader_m33
```

This cross-builds Debug (so log lines are compiled in), builds the board_sim
emulator, and boots the M85 ELF. board_sim sees the embedded `.cpu1_image` and
spins up a second Unicorn engine for the M33 sharing the SRAM buffer.

## Expected output

```
[itm] [M85] INFO: ==== RA8D2 ereader_m33 demo ====
[itm] [M85] INFO: releasing Cortex-M33 secondary core ...
[itm] [M85] INFO: M33 reader is alive
[itm] [M85] INFO: M85 idle; narrating M33 page turns ...
[itm] [M85] INFO: M33 turned to page=1
[itm] [M85] INFO: M33 page pixels CRC32=<nonzero, varies per page>
...
[itm] [M85] INFO: M33 turned to page=14
[itm] [M85] INFO: M33 page pixels CRC32=<nonzero>
[itm] [M85] INFO: M33 walked total pages=14
[itm] [M85] INFO: ereader_m33 PASS
```

Each `CRC32=` line is a CRC-32 the M85 computes over the 2 KiB framebuffer the
M33 just rendered. The values are non-zero (the plane is not blank) and vary
page to page (each page draws different text) -- that is the proof the M33
produced real pixels, on its own, while the M85 only read them.

## What the next increment needs

This increment proves the M33 renders genuine page pixels into a framebuffer it
owns in shared SRAM. The next step is wiring that plane to the real display:
an M33-side `ra_gfx` path and the GLCDC scan-out plane / panel handoff -- tracked
in #150.

## Files

| File                    | Role                                                   |
|-------------------------|--------------------------------------------------------|
| `main.c`                | M85: release M33, CRC each rendered page, verdict, park |
| `cpu1_main.c`           | M33: validate book, walk chapters, render pages, report |
| `ereader_m33.h`         | Shared mailbox + framebuffer address + constants       |
| `m33_font.h`            | Baked 8x16 public-domain mono font for the M33 blit    |
| `rabook_fixture.h`      | Baked, inflated two-chapter `RABOOK1` demo book         |
| `linker_script.ld`      | M85 memory map; pins `.cpu1_image` at MRAM_CPU1         |
| `linker_script_cpu1.ld` | M33 memory map (MRAM_CPU1 + SRAM_CPU1)                  |
| `system_init.c`         | M85 core bring-up (D-cache off)                         |
| `vector_table.c`        | M85 vector table + Reset_Handler                        |
| `trustzone_init.c`      | SAU scaffold (not invoked in single-world build)        |
| `CMakeLists.txt`        | Builds both images; embeds M33 into M85                 |
| `Makefile`              | Per-app build / flash / debug wrapper                   |
