# ra8_io_cache_demo

Drives the `ra8_io` caching block device (#160) with no external hardware.

The cache is a decorator: it wraps any other `ra8_io_blockdev_t` and keeps a
fixed set of recently-used 512-byte sectors in a **caller-owned** buffer, so
repeated reads of the same blocks -- filesystem metadata, a re-read page -- skip
the slow medium. Reads are served on a hit and fill on a miss, writes are
write-through, eviction is least-recently-used, and it allocates nothing. It is
the layer that sits between the filesystem and the media.

Here it wraps an in-SRAM RAM block device (#156), the *cached* device is bridged
to `ra8_fs` (#158) and formatted, mounted and registered in the VFS, so every FAT
access flows through the cache. A file is written and then read back several
times through its VFS path and byte-compared each pass: the first pass fills the
cache and the re-reads touch the same metadata and data sectors, so the run
requires a non-zero hit count from `ra8_io_blockdev_cache_stats`. The exact
hit and miss numbers depend on FAT geometry and are not the assertion.
