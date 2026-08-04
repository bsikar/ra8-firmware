# libs/ra8_fs -- the filesystem for the bare-metal world

`ra8_fs` is a hand-written FAT12/FAT16/FAT32 + exFAT implementation sitting on a
three-callback block-device seam (`ra8_fs_backend_t`: `read_block`,
`write_block`, `get_capacity`, plus an optional `erase_blocks`). It needs no
RTOS, no heap and no vendor SDK, which is why it is the filesystem the default
world of this firmware uses -- everything from the e-reader shelf to the USB
mass-storage selftests reaches storage through it, and `ra8_io`'s VFS and
formatter seams are built on it.

The vendored Eclipse FileX under [`libs/third_party/filex/`](../third_party/filex/)
is the *other* filesystem in this tree, and it is the ThreadX-world one:
`cmake/filex.cmake` refuses to configure unless `RA8_USE_THREADX=ON`, because
FileX's protection macros call `tx_mutex_*`. The two coexist on purpose. What
separates them is not a feature list, it is whether an RTOS is present at all.

## Comparison

Every row below was read out of the code in this tree at the time of writing --
`libs/ra8_fs/` and the vendored FileX 6.5.0 snapshot -- not from either
project's marketing.

| Capability | `ra8_fs` | FileX 6.5.0 (vendored) |
|---|---|---|
| FAT12 / FAT16 / FAT32, read + write | Yes | Yes |
| exFAT | Yes -- mount, streaming read, whole-file write, rename, unlink, format | **No.** The vendored snapshot ships no exFAT source at all; the only occurrences of the word are historical entries in `docs/revision_history.txt` |
| Long file names, read | Yes -- LFN chains are reassembled and matched (`src/ra8_fs_fat_lfn.c`) | Yes |
| Long file names, write | **No** -- creates 8.3 short names only | Yes, up to `FX_MAX_LONG_NAME_LEN` (256), written by `fx_directory_entry_write` |
| `mkdir` | Yes on FAT12/16/32, including nested paths, with rollback on failure. Not on exFAT | Yes (`fx_directory_create`) |
| `rmdir` | **No** | Yes (`fx_directory_delete`) |
| Create / write timestamps | **No** -- date and time fields are left at 0 | Yes (`fx_file_date_time_set`, `fx_system_date_set` / `fx_system_time_set`) |
| Unicode / UTF-16 name API | No | Yes -- 13 `fx_unicode_*` modules (create, rename, name get, short-name get) |
| Formatter | Yes. FAT12/16/32 as a superfloppy at LBA 0 with auto cluster-size selection; exFAT into **MBR partition 1 aligned at 1 MiB**, with the spec's compressed up-case table, validated `fsck.exfat`-clean (#568) | Yes (`fx_media_format`), FAT only. Writes no partition table |
| Mounts a card a PC partitioned | Yes. `priv_read_boot_sector()` tries LBA 0 as a superfloppy, and on failure follows MBR partition entry 0; if that entry is the `0xEE` protective type it walks the GPT to the first Basic Data partition. Where it landed is recorded in `ra8_fs_mount_t::partition_base_lba` | Not as built here. `_fx_partition_offset_calculate` exists but nothing in the vendored tree or in `port/filex/` calls it; our media driver's `FX_DRIVER_BOOT_READ` is LBA 0 |
| Multi-partition scanning | No -- the first partition entry only | n/a (see above) |
| Streaming file I/O (open / seek / read / write) | Yes on FAT12/16/32. On exFAT, streaming reads yes; writes are whole-file via `ra8_fs_write_file()` (open-for-write returns `k_ra8_err_not_supported`) | Yes |
| FAT32 FSInfo free-cluster cache | Written at format, then never consulted -- free space is always a linear FAT scan | Read and validated at `fx_media_open`, written back at flush and close |
| Fault-tolerant journaling | **No** | Yes -- 19 `fx_fault_tolerant_*` modules, opt-in behind `FX_ENABLE_FAULT_TOLERANT`, which this tree's build does not define |
| Media check / repair | **No** | Yes (`fx_media_check`) |
| RTOS required | **No.** Bare-metal; nothing in the library references a scheduler | **Yes.** `cmake/filex.cmake` raises a `FATAL_ERROR` when `RA8_USE_FILEX=ON` without `RA8_USE_THREADX=ON` |
| Dynamic allocation | None. One static 512-byte scratch sector, 4 file slots, 2 mount slots | None in the library; the caller supplies the `FX_MEDIA` control block and a sector-cache buffer to `fx_media_open` (the demo passes 512 bytes) plus a ThreadX thread and stack |
| Concurrency model | Not thread-safe by contract; callers serialise | A `TX_MUTEX` per media (`FX_SINGLE_THREAD` is not defined in this build) |
| Backends in tree | Any object with the three callbacks: SD-over-SPI, native SDHI, OSPI NOR, MRAM, SDRAM, in-RAM scratch, USB MSC, plus the host-test mock | One media driver, `port/filex/src/fx_media_driver_ra8_sdhi.c`, plus the LevelX NOR adapter used by `threadx_filex_levelx_demo` |
| Verification | First-party. Held to the 90% per-file line-coverage floor with **no allowlist** (`scripts/checks/check_coverage_floor.py`; `ra8_fs` has no row in `.github/coverage-baseline.txt` or `.github/mcdc-baseline.txt`), MC/DC vectors on its compound decisions, MISRA via `scripts/checks/misra_check.sh` (ratcheted in `.github/misra-baseline.txt`), clang-tidy, the ASCII / Doxygen / annotation gates | SOUP. Explicitly out of scope for the coverage floor (`OUT_OF_SCOPE_PREFIXES`), for MISRA (`-ilibs/third_party`), and for the first-party style rules; compiled with `-w`. Accepted on service history, Eclipse Foundation process and pre-Eclipse SGS-TUV Saar pre-certifications -- see [`docs/SOUP/filex.md`](../../docs/SOUP/filex.md). Byte-identity against the upstream pin is re-verified every CI run |
| Host tests | 25 test binaries (`tests/test_ra8_fs*.c`) plus a libFuzzer harness (`tests/fuzz/fuzz_ra8_fs_fat.c`) | None. SOUP is not re-tested here |
| Size | 13 `.c` files | 212 `.c` files in `common/src` |
| Apps using it | 29 example `CMakeLists.txt` reference it | 2 enable `RA8_USE_FILEX`: `threadx_filex_demo` and `threadx_filex_levelx_demo` |

## When to use which

**Bare-metal app -> `ra8_fs`.** That is nearly every app here, and it is the
only option without a scheduler. **ThreadX app -> FileX**, which is already in
that world and brings long-name writes, `rmdir`, timestamps and optional
journalling with it. Reaching a file through `ra8_io`'s VFS
(`ra8_io_vfs_open()`) is still `ra8_fs` underneath -- `ra8_io` adds mount
points, caching and format probing, not a second filesystem.

The bold **No** cells are a scoping decision, not an oversight, and the
authoritative copy of that list is the header's "What this deliberately skips"
section. If a consumer needs one of them, that is an issue against `ra8_fs`,
not a reason to drag ThreadX into a bare-metal app.

<!-- disambig
this: libs/ra8_fs
that: libs/third_party/filex
symbol: ra8_fs_format
symbol: ra8_fs_write_file
symbol: fx_media_format
symbol: fx_directory_delete
users: ra8_fs = 29
users: RA8_USE_FILEX = 2
files: libs/ra8_fs/src/*.c = 13
files: libs/third_party/filex/common/src/*.c = 212
files: tests/test_ra8_fs*.c = 25
files: libs/third_party/filex/common/src/fx_fault_tolerant_*.c = 19
files: libs/third_party/filex/common/src/fx_unicode_*.c = 13
-->

