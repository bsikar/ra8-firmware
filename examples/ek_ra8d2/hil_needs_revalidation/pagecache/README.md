# pagecache

On-silicon exercise of the `reflow` import-time pagination cache (#117, the
#79 cache) against a real microSD volume through `ra8_fs` -- the exact storage
path the e-reader uses -- so the serialise / persist / load / invalidate logic
meets real SD timing and the FAT read and write paths rather than only a host
unit test.

From a fixed chapter laid out with an SD-loaded font it checks three things:

1. **Round-trip.** Lay out live, serialise to a blob, write it to the card, read
   it back, load it into a fresh engine and re-serialise -- the reloaded blob
   must be byte-for-byte equal to the live one. Since the serialised form is the
   per-page glyph and page data, equal blobs mean the layout was restored
   exactly.
2. **Invalidation.** Load that same blob into an engine initialised at a
   different font size and require `k_ra8_err_invalid_state`. The key mismatch
   has to be caught, or a stale page gets served.
3. **Reset survival.** A second boot against a persisted card must report a hit
   and serve from the on-disk cache without a fresh layout.

The gate is a memprobe rather than the console, for the same reason as the
sibling SD apps: an SD app drives the SCI0 Simple-SPI bus, and an emulator that
folds every SCI channel onto one line interleaves the SCI8 banner with SPI
traffic. `g_pc_heartbeat` advances only after every assertion passes; any
failure stamps `g_pc_err` with the first failing stage and parks in `wfi`
without bumping it.

Needs a PMOD MicroSD in Pmod2 (J25) with a card inserted, and **may format the
card**. The font and cache file self-provision, so a blank FAT card works, and
an unseated card is the first thing to rule out when this fails. Rendering the
cached pages to pixels is `sd_font_render` / `ereader_ui`; this is the
serialise/load layer only.
