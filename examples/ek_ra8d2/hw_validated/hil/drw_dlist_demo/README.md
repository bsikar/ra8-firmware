# drw_dlist_demo

DRW ("D/AVE 2D") **display-list** clear+fill demo for the bare EK-RA8D2 EVM.
Exercises the `ra8_drw` display-list builder (`ra8_drw_dlist_begin` /
`ra8_drw_dlist_add_fill` / `ra8_drw_dlist_end` / `ra8_drw_dlist_run`).

## What it does

Brings up SCI + LEDs + the DRW engine pointed at a 32x32 ARGB8888 framebuffer
in SRAM, then builds ONE display list in SRAM that:

1. clears the whole framebuffer to `0x00000000`, and
2. fills a 16x16 green (`0xFF00FF00`) rectangle at (8, 8).

Once a second it kicks the list (`ra8_drw_dlist_run` -> DLISTSTART), waits
(bounded) for the display-list reader to go idle, hashes the whole framebuffer
FNV-1a-32, and reports `drw: dlist crc=E6B215C5 PASS` (or `... FAIL` with the
observed hash) on the J-Link OB CDC channel.

- LED1 toggles on a match; LED2 on a mismatch.
- `g_dlist_crc` / `g_dlist_match` / `g_dlist_rev` (HWREVISION) / `g_dlist_buserr`
  / `g_dlist_heartbeat` mirror the result for headless J-Link probing.

No external hardware required.

## Why the display-list route (issue #247)

The register/immediate path (`ra8_drw_fill_rect`, `drw_fill_demo`) renders a
byte-clean single fill on silicon, but a tight loop that has the **CPU** clear
the framebuffer and the **engine** fill it races two bus masters on the shared
SRAM and eventually latches `STATUS.BUSERRMFB`, wedging the engine. The display
list moves the clear onto the engine too: the DRW clears and fills end to end
and the CPU only reads the finished framebuffer, so there is no write race.
Bench-verified on an EK-RA8D2 (VTref 3.3V, over J-Link):

- `HWREVISION` = `0x0FBE0107` (graphics power domain up).
- The 19-word list renders a byte-clean framebuffer, FNV-1a-32 `0xE6B215C5`
  (CRC32 `0x84EDFC3A`) -- identical to the register-mode fill.
- `PERFCOUNT2` (framebuffer writes) climbs ~1280 per iteration, so the DLR
  actively rasterizes each pass.
- Across hundreds of clear+fill iterations `STATUS.BUSERRMFB` never latched and
  the wait-idle never timed out -- loop-stable.

## SIM == HIL

`tools/board_sim` models the DLR (`board_periph_drw.c`): a DLISTSTART write
executes the list from emulated memory, each ORIGIN write triggering the same
render as a CPU ORIGIN write. board_sim therefore produces the byte-identical
framebuffer and the identical `crc=E6B215C5 PASS` banner, so `hil.conf` gates
the same string on the bench (`uart_scrape`) and in SIL.

## Golden

`E6B215C5` is a real silicon capture. If it ever changes, re-derive it from a
bench framebuffer dump -- never from the simulator, and never by writing down
whatever the demo happens to print.

## Notes (HUM R01UH1065EJ0130 Rev.1.30, Ch 62 "2D Drawing Engine")

- The display-list word format is not documented in the HUM; it is decoded from
  the vendored TES D/AVE 2D reference and bench-verified (see `ra8_drw_regs.h`,
  `ra8_drw_dlr_index_t` / `ra8_drw_dlr_word_t`).
- `DLISTSTART` (HUM Ch 62.2.32 p 3705) kicks the reader; `ORIGIN` (Ch 62.2.31
  p 3705) inside the list is the render trigger.
- The FB cache is left off (`enable_caches = false`) so the CPU reads the
  freshly-rasterized pixels directly.
