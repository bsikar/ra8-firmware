# epub_open

Opens a real `.epub` off a microSD card and runs the `ra8_epub` parse stack on
it, on the target (#114). `epub_parse` proved the parser runs on the M85 from a
baked in-memory blob; this closes the storage gap by reading the book through
`ra8_fs` -- the exact path the e-reader uses -- so the byte-twiddling parse
layer meets real SD timing and the FAT read path instead of a `.rodata` array.

It self-provisions a known two-chapter book onto the card if it is not already
there, then opens it with `ra8_epub_open_streamed_fs()`: the production streamed
open (#230), which keeps no whole-file buffer and seeks the card for every ZIP
read. It asserts the spine count, a byte-exact CRC-32 over chapter 0's
decompressed XHTML, and a non-empty Dublin Core title, so a pass means the bytes
were right and not merely that nothing crashed.

miniz runs through the `ra8_epub_miniz_alloc` arena and the bounded XML reader
uses explicit caller workspace, so no allocation reaches the trapped firmware
heap.

## The gate is a memprobe, not the console

An SD app drives the SCI0 Simple-SPI bus, and an emulator that folds every SCI
channel onto one console line interleaves the SCI8 banner with SPI traffic. So
this gates on SWD-readable globals, like the sibling SD apps: `g_eoh_heartbeat`
advances only on the success idle loop, which is reached only after every
assertion passed, and each failing stage stamps a non-zero `g_eoh_err` and parks
without bumping it. A steadily advancing heartbeat with a zero error code proves
the whole SD -> `ra8_fs` -> `ra8_epub` pipeline ran.

Needs a microSD in Pmod2 (J25) and may format it; an unseated card is the first
thing to rule out when this fails. Rendering and reflow belong to `ereader_ui`
-- this is open and parse only, against one fixed known-good book.
