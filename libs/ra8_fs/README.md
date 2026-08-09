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
FileX's protection macros call `tx_mutex_*`. The two coexist on purpose.

What used to separate them was not a feature list, it was whether an RTOS was
present at all. That is no longer quite true: `ra8_fs_set_lock()` (#608) is a
two-callback seam an RTOS-world caller binds to its own mutex, so one verified
filesystem can serve both worlds. The seam is optional and the default is still
no lock, so the bare-metal path is unchanged -- but "you need a scheduler, so
you need the other filesystem" has stopped being a reason on its own.

## Comparison

Every row below was read out of the code in this tree at the time of writing --
`libs/ra8_fs/` and the vendored FileX 6.5.0 snapshot -- not from either
project's marketing.

| Capability | `ra8_fs` | FileX 6.5.0 (vendored) |
|---|---|---|
| FAT12 / FAT16 / FAT32, read + write | Yes | Yes |
| exFAT | Yes -- mount, streaming read AND streaming write (open / append / truncate through the same seam as FAT; the entry set keeps `NoFatChain` while the run is contiguous and materialises a real FAT chain when it is not), rename, unlink, format, and a real DIRECTORY TREE: `mkdir`, `rmdir`, nested path resolution and per-directory listing | **No.** The vendored snapshot ships no exFAT source at all; the only occurrences of the word are historical entries in `docs/revision_history.txt` |
| Long file names, read | Yes -- LFN chains are reassembled and matched (`src/ra8_fs_fat_lfn.c`) | Yes |
| Long file names, write | Yes -- up to 247 characters (19 VFAT groups) behind a generated `LONGNA~1.TXT` alias, on create, `mkdir` and `rename`; `unlink` / `rmdir` take the chain away with the entry (`src/ra8_fs_fat_lfn_write.c`, #600). A name differing from 8.3 only in case travels in the `DIR_NTRes` flags instead | Yes, up to `FX_MAX_LONG_NAME_LEN` (256), written by `fx_directory_entry_write` |
| `mkdir` | Yes on FAT12/16/32 **and exFAT**, including nested paths, with rollback on failure. On exFAT a directory is a File+Stream+Name set with the Directory attribute over one zeroed cluster -- there are no "." / ".." entries, because exFAT has none. It GROWS when full: FAT subdirectories and the FAT32 root extend their chains, and an exFAT directory extends the same way its files do (contiguous with `NoFatChain` while it can, a real FAT chain when it fragments), so a folder's ceiling is the free space rather than one cluster (#677). An existing name is refused, never replaced | Yes (`fx_directory_create`) |
| `rmdir` | Yes on FAT12/16/32 **and exFAT**, including nested paths. Refuses the root, refuses a file, and proves the directory is empty before freeing a cluster (`k_ra8_err_not_empty` otherwise). Only remnants are discounted -- FAT's 0xE5 slots and orphaned long-name entries, exFAT's entries with the in-use bit clear -- so a directory another implementation left remnants in is still removable | Yes (`fx_directory_delete`) |
| Create / write timestamps | Yes on FAT and exFAT -- create, modify and access fields on create, content write, truncate, close and rename, from a caller-injected clock (`ra8_fs_set_clock()`). With no clock installed every stamp is the legal FAT epoch 1980-01-01, never the zeros that made macOS show 31 Dec 1969. exFAT additionally carries the 10 ms increments and the `UtcOffset` bytes | Yes (`fx_file_date_time_set`, `fx_system_date_set` / `fx_system_time_set`) |
| Non-ASCII file names | Yes -- the API is UTF-8 on both formats and the disk is UTF-16LE, converted in one place (`src/ra8_fs_utf.c`, #606). The whole of Unicode is storable, supplementary planes included: a 4-byte UTF-8 character becomes a surrogate pair on disk and comes back as the same 4 bytes. Malformed UTF-8 -- an over-long form, a raw surrogate, a truncated sequence -- is REFUSED rather than patched. Lookup folds across the BMP through the canonical Microsoft up-case table, and exFAT's `NameHash` uses that same table, so a name this library writes is one a host recomputes the hash for (proven against an independently expanded on-disk table, and `fsck.exfat`-clean). A volume carrying a DIFFERENT up-case table refuses non-ASCII names with `k_ra8_err_not_supported` rather than filing them under a hash nothing agrees with. Directory names and every component of a nested exFAT path go through the same conversion, so a folder called anything is enterable by its own name. Limits are in UTF-16 units: 247 on FAT, 64 on exFAT | Yes -- 13 `fx_unicode_*` modules (create, rename, name get, short-name get), FAT only |
| Formatter | Yes. FAT12/16/32 as a superfloppy at LBA 0 with auto cluster-size selection; exFAT into **MBR partition 1 aligned at 1 MiB**, with the spec's compressed up-case table, validated `fsck.exfat`-clean (#568) | Yes (`fx_media_format`), FAT only. Writes no partition table |
| Mounts a card a PC partitioned | Yes. `priv_read_boot_sector()` tries LBA 0 as a superfloppy, and on failure follows MBR partition entry 0; if that entry is the `0xEE` protective type it walks the GPT to the first Basic Data partition. Where it landed is recorded in `ra8_fs_mount_t::partition_base_lba` | Not as built here. `_fx_partition_offset_calculate` exists but nothing in the vendored tree or in `port/filex/` calls it; our media driver's `FX_DRIVER_BOOT_READ` is LBA 0 |
| Multi-partition scanning | No -- the first partition entry only | n/a (see above) |
| Streaming file I/O (open / seek / read / write) | Yes on FAT12/16/32 and on exFAT (#602). An exFAT file grows a cluster at a time out of the allocation bitmap, so its size is bounded by free space rather than by RAM, and `ValidDataLength` is tracked apart from `DataLength` so bytes past the written prefix read as zero | Yes |
| Truncate / extend (`ftruncate`) | Yes -- `ra8_fs_truncate()` (#680) sets an open, writable file to any length in either direction. A shrink frees the tail clusters and lowers the length; a grow zero-fills the gap -- on FAT the fresh clusters and the last cluster's slack are written zero, on exFAT `DataLength` rises while `ValidDataLength` stays at the written prefix so the format serves the gap as zero, and a grow past the contiguous space converts the run to a real FAT chain. Both filesystems are `fsck`-clean across shrink, grow and the chain transition | Yes (`fx_file_truncate` / `fx_file_extended_truncate` / `fx_file_allocate`) |
| FAT32 FSInfo free-cluster cache | Yes -- all three signatures validated at mount, then used to seed a per-mount free count and next-free hint, and written back when a file is closed or the volume unmounted. A count that cannot be trusted is written as the format's `0xFFFFFFFF` "unknown" rather than guessed. The allocator scans from the hint through a one-sector FAT cache instead of rescanning from cluster 2 | Read and validated at `fx_media_open`, written back at flush and close |
| Fault-tolerant journaling | **No** | Yes -- 19 `fx_fault_tolerant_*` modules, opt-in behind `FX_ENABLE_FAULT_TOLERANT`, which this tree's build does not define |
| Media check / repair | **No** | Yes (`fx_media_check`) |
| RTOS required | **No.** Bare-metal; nothing in the library references a scheduler | **Yes.** `cmake/filex.cmake` raises a `FATAL_ERROR` when `RA8_USE_FILEX=ON` without `RA8_USE_THREADX=ON` |
| Dynamic allocation | None. One static 512-byte scratch sector, 4 file slots, 2 mount slots | None in the library; the caller supplies the `FX_MEDIA` control block and a sector-cache buffer to `fx_media_open` (the demo passes 512 bytes) plus a ThreadX thread and stack |
| Concurrency model | Single-threaded by default; `ra8_fs_set_lock()` installs a caller-supplied acquire/release pair taken across every public entry point (one lock per library -- the file table, mount table and scratch sector are shared). No lock primitive, RTOS header or scheduler concept in the library | A `TX_MUTEX` per media (`FX_SINGLE_THREAD` is not defined in this build) |
| `stat` | Yes -- `ra8_fs_stat()` reads the directory entry without opening it, so a directory reports as one and no file slot is spent on a metadata query | Yes (`fx_directory_information_get`) |
| Free-space query (statvfs) | Yes -- `ra8_fs_free_space()` (#678) reports total / free / used clusters and 64-bit byte totals. FAT32 answers in O(1) from the cached FSInfo count; FAT12/16 and exFAT count once from the FAT / allocation bitmap (through the one-sector FAT cache) and cache the result | Yes (`fx_media_space_available` / `fx_media_extended_space_available`) |
| Volume label, read + set | Yes -- `ra8_fs_get_label()` / `ra8_fs_set_label()` (#682). On FAT the boot `BS_VolLab` and the root `ATTR_VOLUME_ID` entry are kept in step (an unlabelled volume is the spec sentinel `"NO NAME    "`, #634); on exFAT the Volume Label directory entry is rewritten in place | Yes (`fx_media_volume_get` / `fx_media_volume_set`) |
| Set arbitrary timestamps (`utime`) | Yes -- `ra8_fs_utime()` (#682) sets a named entry's create / modify / access times to caller-chosen values (any NULL left unchanged), so a backup/restore preserves original times rather than stamping the restore moment; exFAT's entry-set SetChecksum is recomputed | Yes (`fx_file_date_time_set`) |
| Backends in tree | Any object with the three callbacks: SD-over-SPI, native SDHI, OSPI NOR, MRAM, SDRAM, in-RAM scratch, USB MSC, plus the host-test mock | One media driver, `port/filex/src/fx_media_driver_ra8_sdhi.c`, plus the LevelX NOR adapter used by `threadx_filex_levelx_demo` |
| Verification | First-party. Held to the 90% per-file line-coverage floor with **no allowlist** (`scripts/checks/check_coverage_floor.py`; `ra8_fs` has no row in `.github/coverage-baseline.txt` or `.github/mcdc-baseline.txt`), MC/DC vectors on its compound decisions, MISRA via `scripts/checks/misra_check.sh` (ratcheted in `.github/misra-baseline.txt`), clang-tidy, the ASCII / Doxygen / annotation gates | SOUP. Explicitly out of scope for the coverage floor (`OUT_OF_SCOPE_PREFIXES`), for MISRA (`-ilibs/third_party`), and for the first-party style rules; compiled with `-w`. Accepted on service history, Eclipse Foundation process and pre-Eclipse SGS-TUV Saar pre-certifications -- see [`docs/SOUP/filex.md`](../../docs/SOUP/filex.md). Byte-identity against the upstream pin is re-verified every CI run |
| Host tests | 58 test binaries (`tests/test_ra8_fs*.c`) plus a libFuzzer harness (`tests/fuzz/fuzz_ra8_fs_fat.c`). The exFAT directory suites end every scenario with a structural scan of the volume -- entry-set checksums, name hashes, and the referenced clusters against the allocation bitmap in both directions -- and each scenario's image is `fsck.exfat -n` clean | None. SOUP is not re-tested here |
| Size | 28 `.c` files | 212 `.c` files in `common/src` |
| Apps using it | 29 example `CMakeLists.txt` reference it | 2 enable `RA8_USE_FILEX`: `threadx_filex_demo` and `threadx_filex_levelx_demo` |

## When to use which

**Bare-metal app -> `ra8_fs`.** That is nearly every app here, and it is the
only option without a scheduler. **ThreadX app -> either**: FileX is already in
that world and brings long-name writes and optional journalling with it, but
`ra8_fs` plus a ten-line `tx_mutex_*` binding over `ra8_fs_set_lock()` is now a
real alternative, and the only one of the two that does exFAT. Reaching a file
through `ra8_io`'s VFS
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
symbol: ra8_fs_free_space
symbol: ra8_fs_truncate
symbol: ra8_fs_get_label
symbol: ra8_fs_set_label
symbol: ra8_fs_utime
symbol: fx_media_format
symbol: fx_directory_delete
users: ra8_fs = 29
users: RA8_USE_FILEX = 2
files: libs/ra8_fs/src/*.c = 28
files: libs/third_party/filex/common/src/*.c = 212
files: tests/test_ra8_fs*.c = 58
files: libs/third_party/filex/common/src/fx_fault_tolerant_*.c = 19
files: libs/third_party/filex/common/src/fx_unicode_*.c = 13
-->

