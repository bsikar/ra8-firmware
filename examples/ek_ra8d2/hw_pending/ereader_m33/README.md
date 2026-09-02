# ereader_m33

The #150 power-saving model: an e-reader spends almost all its time idle on a
rendered page, so holding that page on the M85 @ 1 GHz is waste. This app hands
the reader to the Cortex-M33 @ 250 MHz and parks the M85, then runs the full
mode-switch cycle -- the M85 clock-gates and drops into WFI, the M33 holds the
page and polls touch, and on each page turn the M33 pokes the M85 over IPC to do
the heavy next-page work before it re-parks.

## What it teaches

- **The render runs on the secondary core, through the production gfx stack.**
  The M33 validates a baked `RABOOK1` blob, walks the chapter DOM iteratively
  (no recursion) through the header-only `book.h` accessors, and renders the
  page into an RGB565 framebuffer in external SDRAM with the real `ra8_gfx` text
  path. `ra8_gfx` is dependency-clean, zero-heap and scalar (no Helium), so it
  links into the freestanding M33 image with no logging backend, no panel driver
  and no malloc.
- **The framebuffer lives in SDRAM at `0x68000000`,** in a NOLOAD `.sdram_bss`
  section the CPU1 linker script pins at the SDRAM base -- the same
  arena-in-SDRAM pattern `compile_on_m33` uses -- and the M33 publishes its base,
  geometry, format, glyph count and CRC-32 into the shared SRAM mailbox.
- **The M33 CRCs its own pixels, and that is not an accident.** Under emulation
  the two cores share only the on-chip SRAM mailbox; each core's external-SDRAM
  window is a separate mapping, so the parked M85 cannot read the bytes the M33
  wrote. The M33 therefore reads its own framebuffer back to fold the CRC --
  reading the pixels back is itself the proof they landed -- and publishes the
  value for the M85 to narrate. On silicon the single physical SDRAM is shared,
  so an M85 re-read would match.
- **The wake path is real.** The M33 bumps a turn request and pokes IPC0
  (`ra8_ipc_send_event`, the same #149 wake `compile_on_m33` uses); the woken
  M85 restores its clock, does the work the fast core owns, acks, and re-parks;
  the M33 re-renders the held page, re-folding the identical CRC. The CRC staying
  stable across every re-render is the assertion.

## Blocked on

The control flow is fully exercisable off-target -- the M85 really parks and is
really woken out of WFI by the M33's poke. What no emulator can show is the
**power delta** of the park (the clock-gate and WFI are functionally exercised,
but current draw is not modelled) and **real touch input** (an emulated
page-dwell stands in for a GT911 poll). Both need the bench (#30). The remaining
#150 display work -- pointing the GLCDC scan-out plane at the M33's framebuffer
for a true display-plane handoff -- is HIL-bound for the same reason.
