# ra8_io_compress_demo

Drives the `ra8_io` fabric (#155) with the opt-in DEFLATE compression layer
(#161) on a stock EK-RA8D2 with no external hardware: build an in-SRAM RAM block
device, bridge it to `ra8_fs`, format and mount a FAT12 volume and register it in
the VFS, then compress a payload, write the compressed blob through the VFS,
read it back, inflate it, and require the result to be byte-identical to the
original. This mirrors the firmware's real compress-on-write /
decompress-on-read path -- the `.rabook` RBKC chunk streams -- routed entirely
through the unified fabric.

**The compression is heap-free.** This firmware traps `_sbrk`, so the compressor
state is a caller-provided scratch buffer in SRAM rather than anything the
library allocates for itself.

## Compression is opt-in on purpose

`ra8_io_compress.h` is deliberately **not** part of the `ra8_io.h` umbrella,
because it pulls in the vendored miniz. An app that wants it includes the header
directly and adds `miniz` to its `ra8_add_app(... LIBS ...)`, as this one does.
Plain `ra8_io` consumers such as `ra8_io_demo` therefore carry no compressor and
no miniz dependency at all.
