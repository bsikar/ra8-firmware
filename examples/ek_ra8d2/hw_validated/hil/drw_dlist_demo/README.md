# drw_dlist_demo

Builds one DRW display list in SRAM that clears a small ARGB8888 framebuffer
and fills a rectangle in it, kicks the list, waits for the display-list reader
to go idle, and hashes the framebuffer. Needs no external hardware.

The display-list route exists because the immediate path races. A loop that has
the CPU clear the framebuffer while the engine fills it puts two bus initiators
on the same SRAM and eventually latches `STATUS.BUSERRMFB`, wedging the engine.
Moving the clear into the list gives the DRW the framebuffer end to end and
leaves the CPU only reading the finished result, which stays stable across
hundreds of iterations. `drw_fill_demo` is the immediate-mode sibling, and the
two produce a byte-identical framebuffer.

`tools/ra8_emulator` models the display-list reader, executing the list out of
emulated memory with each ORIGIN write triggering the same render a CPU ORIGIN
write would, so the emulator and the bench agree byte for byte.

The pinned hash is a real silicon capture. If it ever changes, re-derive it from
a bench framebuffer dump -- never from the emulator, and never by writing down
whatever the demo happens to print.

## Notes (HUM R01UH1065EJ0130 Rev.1.30, Ch 62 "2D Drawing Engine")

- The display-list word format is not documented in the HUM. It is decoded from
  the vendored TES D/AVE 2D reference and bench-verified; see `ra8_drw_regs.h`,
  `ra8_drw_dlr_index_t` and `ra8_drw_dlr_word_t`.
- `DLISTSTART` (Ch 62.2.32 p 3705) kicks the reader; an `ORIGIN` write inside
  the list (Ch 62.2.31 p 3705) is the render trigger.
- `HWREVISION` reads `0x0FBE0107` once the graphics power domain is up.
- The framebuffer cache is left off (`enable_caches = false`) so the CPU reads
  the freshly-rasterized pixels.
