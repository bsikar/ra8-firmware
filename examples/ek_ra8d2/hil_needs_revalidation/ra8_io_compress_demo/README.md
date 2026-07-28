# ra8_io_compress_demo

End-to-end demo of the **ra8_io** I/O fabric (epic #155) with the opt-in
**DEFLATE compression** layer (Phase 6, #161), on a stock EK-RA8D2 with no
external hardware.

## What it does

1. Builds a 256 KiB RAM block device in SRAM (Phase 1, #156).
2. Bridges it to `ra8_fs`, formats a FAT12 volume, mounts it, and registers it in
   the VFS as `"ram"` (Phase 3, #158).
3. Generates a 4096-byte, highly-compressible payload and DEFLATEs it with
   `ra8_io_compress()` into a staging buffer, using a caller-provided scratch
   buffer in SRAM (Phase 6, #161). The compression is **heap-free** -- this
   firmware traps `_sbrk`, so the compressor state is the caller's `s_scratch`.
4. Writes the *compressed* blob to `ram:/STORY.RBK` through the VFS.
5. Reads the blob back, inflates it with `ra8_io_decompress()`, and verifies the
   result is byte-identical to the original payload.
6. Reports on the SCI8 J-Link OB console through a `ra8_io` UART stream sink.

This mirrors the firmware's real compress-on-write / decompress-on-read path
(the `.rabook` RBKC chunk streams) but routed entirely through the unified fabric.

## Opting into compression

`ra8_io_compress.h` is **not** part of the `ra8_io.h` umbrella -- it pulls in the
vendored miniz. An app opts in by including it directly and adding `miniz` to its
`ra8_add_app(... LIBS ...)`, as this app's `CMakeLists.txt` does. Plain ra8_io
consumers (e.g. `ra8_io_demo`) carry no compressor and no miniz dependency.

## Expected output (ra8_emulator or J-Link RTT/UART)

```
ra8_io_compress_demo: boot
ra8_io_compress_demo: 4096 -> <N> -> 4096 bytes ram:/STORY.RBK PASS
```

`<N>` is the compressed blob size (well under 4096 for this repetitive payload).

## Build

```
make            # -> build/ra8_io_compress_demo.elf / .hex / .bin
```

## Status

`hw_pending`: written and proven in ra8_emulator; not yet bench-validated on a
physical EK-RA8D2.
