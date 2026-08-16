# libs/ra8_fs -- the platform's filesystem

`ra8_fs` is a hand-written FAT12/FAT16/FAT32 + exFAT implementation sitting on a
three-callback block-device seam (`ra8_fs_backend_t`: `read_block`,
`write_block`, `get_capacity`, plus an optional `erase_blocks`). It needs no
RTOS, no heap and no vendor SDK. Everything in this firmware reaches storage
through it -- from the e-reader shelf to the USB mass-storage selftests to the
ThreadX demos -- and `ra8_io`'s VFS and formatter seams are built on it.

It is the ONLY filesystem in this tree. The vendored Eclipse FileX that used to
sit beside it was retired by #611: its whole first-party consumer surface was
two ThreadX HIL demos doing create / list / read-back / delete on OSPI NOR via
LevelX -- a subset `ra8_fs` already implemented -- so the demos were ported onto
`ra8_fs` (over `port/levelx/src/lx_fs_backend.c`, with the `ra8_fs_set_lock()`
seam bound to a ThreadX mutex) and the ~50.9k-line FileX snapshot was deleted.
LevelX stays: `cmake/levelx_standalone.cmake` builds it with no ThreadX and no
filesystem above it, `libs/ra8_cache_store/` consumes that mode, and the two
ported demos mount `ra8_fs` on it.

## Capabilities

Every line below was read out of the code in this tree at the time of writing.

| Capability | `ra8_fs` |
|---|---|
| FAT12 / FAT16 / FAT32, read + write | Yes |
| exFAT | Yes -- mount, streaming read AND streaming write (open / append / truncate through the same seam as FAT; the entry set keeps `NoFatChain` while the run is contiguous and materialises a real FAT chain when it is not), rename, unlink, format, and a real DIRECTORY TREE: `mkdir`, `rmdir`, nested path resolution and per-directory listing |
| Long file names | Read: LFN chains reassembled and matched (`src/ra8_fs_fat_lfn.c`). Write: up to 247 characters (19 VFAT groups) behind a generated `LONGNA~1.TXT` alias, on create, `mkdir` and `rename`; `unlink` / `rmdir` take the chain away with the entry (#600). A name differing from 8.3 only in case travels in the `DIR_NTRes` flags instead |
| `mkdir` / `rmdir` | Yes on FAT12/16/32 **and exFAT**, including nested paths, with rollback on failure. Directories GROW when full (#677). `rmdir` refuses the root, refuses a file, and proves the directory is empty before freeing a cluster (`k_ra8_err_not_empty` otherwise) |
| Timestamps | Create, modify and access fields on create, content write, truncate, close and rename, from a caller-injected clock (`ra8_fs_set_clock()`). With no clock installed every stamp is the legal FAT epoch 1980-01-01, never the zeros that made macOS show 31 Dec 1969. exFAT additionally carries the 10 ms increments and the `UtcOffset` bytes |
| Non-ASCII file names | The API is UTF-8 on both formats and the disk is UTF-16LE, converted in one place (`src/ra8_fs_utf.c`, #606). All of Unicode is storable, supplementary planes included. Malformed UTF-8 is REFUSED rather than patched. Lookup folds across the BMP through the canonical Microsoft up-case table; exFAT's `NameHash` uses that same table. Limits are in UTF-16 units: 247 on FAT, 64 on exFAT |
| Formatter | FAT12/16/32 as a superfloppy at LBA 0 with auto cluster-size selection; exFAT into MBR partition 1 aligned at 1 MiB, with the spec's compressed up-case table, validated `fsck.exfat`-clean (#568) |
| Partitioned media | `ra8_fs_mount()` tries LBA 0 as a superfloppy, then MBR partition entry 0, following a `0xEE` protective entry into the GPT. `ra8_fs_mount_partition()` selects any MBR primary entry (0-3) or GPT entry-array index. Extended/logical MBR chains are not walked |
| Streaming file I/O | open / seek / read / write on FAT12/16/32 and exFAT (#602); an exFAT file grows a cluster at a time out of the allocation bitmap, `ValidDataLength` tracked apart from `DataLength` |
| Files past 4 GiB | Yes on exFAT (#676): the whole length model is 64-bit. FAT12/16/32 keep the format's own 4 GiB - 1 ceiling, enforced with `k_ra8_err_invalid_size` |
| Sector sizes | 512 / 1024 / 2048 / 4096 (#683), cross-checked against the BPB / VBR at mount. SIMULATION-VERIFIED ONLY past 512: no 4Kn medium has been on the bench (`tests/test_ra8_fs_4kn.c`) |
| Media past 2 TiB | 64-bit LBAs end to end (#683). SIMULATION-VERIFIED ONLY: `tests/test_ra8_fs_large_media.c` drives a volume planted past LBA 2^32 on a 3 TiB sparse fake |
| Truncate / extend | `ra8_fs_truncate()` (#680) in either direction, `fsck`-clean across shrink, grow and the chain transition |
| FAT32 FSInfo | Validated at mount, seeds the free count and next-free hint, written back on close/unmount; the allocator scans from the hint through a one-sector FAT cache |
| Media check | `ra8_fs_check()` (#610), a read-only fsck on both formats. No REPAIR: fixing a volume already known inconsistent, in a firmware with no journal, is how a recoverable card becomes an empty one |
| Free space | `ra8_fs_free_space()` (#678): total / free / used clusters and 64-bit byte totals; O(1) on FAT32 via FSInfo |
| Volume label / utime / attributes | `ra8_fs_get_label()` / `ra8_fs_set_label()` / `ra8_fs_utime()` (#682); read-only / hidden / system / archive both ENFORCED and MUTABLE via `ra8_fs_set_attr()` (#681) |
| RTOS required | No. Single-threaded by default; `ra8_fs_set_lock()` (#608) installs a caller-supplied acquire/release pair taken across every public entry point. No lock primitive, RTOS header or scheduler concept in the library |
| Dynamic allocation | None. Static state only: the 4 KiB scratch sector, a four-buffer fixed-role sector arena (16 KiB), a one-sector FAT cache, 4 file slots, 2 mount slots |
| Backends in tree | Any object with the three callbacks: SD-over-SPI, native SDHI, OSPI NOR (raw via `ra8_io`, or wear-levelled via LevelX through `port/levelx/src/lx_fs_backend.c`), MRAM, SDRAM, in-RAM scratch, USB MSC, plus the host-test mock |
| Verification | Held to the 90% per-file line-coverage floor with **no allowlist** (`scripts/checks/check_coverage_floor.py`; `ra8_fs` has no row in `.github/coverage-baseline.txt` or `.github/mcdc-baseline.txt`), MC/DC vectors on its compound decisions, MISRA via `scripts/checks/misra_check.sh` (ratcheted in `.github/misra-baseline.txt`), clang-tidy, the ASCII / Doxygen / annotation gates |
| Host tests | 67 test binaries (`tests/test_ra8_fs*.c`) plus a libFuzzer harness (`tests/fuzz/fuzz_ra8_fs_fat.c`), plus the LevelX-backed stack twin `tests/test_lx_fs_backend.c`. The exFAT directory suites end every scenario with a structural scan of the volume, and each scenario's image is `fsck.exfat -n` clean |

Deliberately not implemented: fault-tolerant journaling (no journal, hence no
repair -- see the media-check row) and exFAT names past 64 UTF-16 units. The
authoritative copy of that list is the header's "What this deliberately skips"
section. If a consumer needs one of them, that is an issue against `ra8_fs`.

## ra8_fs or ra8_io?

Call `ra8_fs` when you hold a block device and want a filesystem on it. Reach
storage through `ra8_io`'s VFS (`ra8_io_vfs_open()`) when you want mount
points, caching and format probing -- it is still `ra8_fs` underneath, not a
second filesystem, and `ra8_io_blockdev_as_fs_backend()` bridges any `ra8_io`
block device into an `ra8_fs_backend_t`. In an RTOS world, bind
`ra8_fs_set_lock()` to your mutex once at init (the ThreadX demos show the
ten-line adapter).

<!-- disambig
this: libs/ra8_fs
that: libs/ra8_io
symbol: ra8_fs_format
symbol: ra8_fs_mount_partition
symbol: ra8_fs_write_file
symbol: ra8_fs_free_space
symbol: ra8_fs_truncate
symbol: ra8_fs_get_label
symbol: ra8_fs_set_label
symbol: ra8_fs_utime
symbol: ra8_fs_set_attr
symbol: ra8_fs_check
symbol: ra8_fs_set_lock
symbol: ra8_io_vfs_open
symbol: ra8_io_blockdev_as_fs_backend
users: ra8_fs = 32
users: ra8_io = 16
files: libs/ra8_fs/src/*.c = 32
files: tests/test_ra8_fs*.c = 67
-->
