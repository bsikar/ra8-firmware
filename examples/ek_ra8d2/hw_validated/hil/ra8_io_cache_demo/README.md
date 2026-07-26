# ra8_io_cache_demo -- caching block device (Phase 5, #160)

A single self-contained app that drives the `ra8_io` caching block device with no
external hardware, so it runs headlessly in `board_sim`.

The caching block device is a decorator: it wraps any other `ra8_io_blockdev_t`
and keeps a fixed set of recently-used 512-byte sectors in a caller-owned cache,
so repeated reads of the same blocks (filesystem metadata, a re-read page) skip
the slow medium. Reads are served from the cache on a hit and fill it on a miss;
writes are write-through. Eviction is least-recently-used. It is the cache layer
between the filesystem/VFS and the media.

What it exercises:

1. **Slow backend (Phase 1, #156):** a RAM block device over a 256 KiB in-SRAM
   buffer (`ra8_io_blockdev_ram`).
2. **Cache decorator (#160):** the backend is wrapped with
   `ra8_io_blockdev_cache_init` over 32 cached sectors (16 KiB), all in
   caller-owned SRAM (zero allocation).
3. **Filesystem over the cache (Phase 3, #158):** the *cached* device is bridged
   to `ra8_fs` (`ra8_io_blockdev_as_fs_backend`), formatted + mounted as FAT12, and
   registered in the VFS as `"ram"` -- so every FAT access flows through the
   cache.
4. **Re-read workload:** a file is written, then read back eight times through
   `"ram:/HELLO.TXT"` and byte-compared each pass. The first pass fills the
   cache; the re-reads touch the same metadata and data sectors and are served
   as hits.
5. **Hit/miss observability:** `ra8_io_blockdev_cache_stats` reports the counters,
   and the run requires a non-zero hit count.

## Build

```
make            # -> build/ra8_io_cache_demo.elf
```

## Run in the simulator

```
BOARD_SIM_WALL_S=12 tools/ra8_emulator/build/ra8_emulator build/ra8_io_cache_demo.elf
```

Expected console output (exact hit/miss counts depend on FAT geometry):

```
[uart] SCI8: ra8_io_cache_demo: boot
[uart] SCI8: ra8_io_cache_demo: re-read x8 hits=H misses=M ram:/HELLO.TXT PASS
```

## Status

`hw_pending`: the logic is proven in `board_sim` (the RAM backend is pure
memory, so no peripheral model is needed). The same code runs on silicon;
promote to `hw_validated` after a bench run captures the PASS line over the
J-Link UART.
