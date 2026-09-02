# libs/ra8_fs

The platform's filesystem: a hand-written FAT12/FAT16/FAT32 and exFAT
implementation over a three-callback block-device seam (read a block, write a
block, report capacity, plus an optional erase). No RTOS, no heap, no vendor
SDK. Everything in this firmware that touches storage arrives here eventually,
`ra8_io`'s VFS and formatter seams included. Both formats are read-write, with
directory trees, long names, rename, unlink, truncate, timestamps, attributes,
labels, free-space accounting and a formatter; the header enumerates the
surface and is the authority on it.

It is the only filesystem in the tree, deliberately. The vendored FileX that
used to sit beside it was retired by #611 once its entire first-party consumer
surface turned out to be a subset of what `ra8_fs` already did. LevelX stays,
but as wear levelling only -- built with no ThreadX and no filesystem above it.

## The parts that are not obvious

**Nothing is allocated.** Static state only: a fixed-role sector arena, a
one-sector FAT cache, a fixed number of file and mount slots. Exhausting a
slot is an error return, never a fallback allocation.

**No RTOS, and no RTOS concept in the library.** It is single-threaded by
default; `ra8_fs_set_lock()` installs a caller-supplied acquire/release pair
that is then taken across every public entry point. Bind it to your mutex once
at init, or do not bind it at all.

**`ra8_fs_check()` never repairs.** It is a read-only fsck on both formats, and
that is the whole design: repairing a volume already known to be inconsistent,
in a firmware with no journal, is how a recoverable card becomes an empty one.

**Malformed UTF-8 is refused, not patched.** The API is UTF-8 and the disk is
UTF-16LE, converted in exactly one place (#606), so all of Unicode is storable,
supplementary planes included. Lookup folds case through the canonical
Microsoft up-case table -- the same table exFAT's `NameHash` is computed over,
so the two cannot disagree.

**Timestamps default to 1980-01-01, not to zero.** With no clock injected the
fields carry the legal FAT epoch; zeros are what made macOS display 31 Dec 1969
on volumes this firmware had written.

**Mount probes in a fixed order:** LBA 0 as a superfloppy, then MBR partition
entry 0, following a `0xEE` protective entry into the GPT. An extended
partition's logical chain is never walked; a specific MBR or GPT entry can be
named instead.

**Sizes are 64-bit on exFAT and capped by the format on FAT.** FAT12/16/32 keep
their own 4 GiB ceiling and enforce it at the boundary rather than wrapping.

**Two capabilities are simulation-verified only.** Sector sizes above 512, and
volumes living past 2 TiB (#683), are exercised against fakes in the host
tests. No 4Kn medium and no multi-terabyte volume has ever been on the bench.

Journaling is absent, and therefore so is repair. The header carries the
authoritative list of what else is deliberately skipped; if a consumer needs
one of those, that is an issue against `ra8_fs` rather than a local workaround.

## ra8_fs or ra8_io?

Call `ra8_fs` when you already hold a block device and want a filesystem on it.
Reach storage through `ra8_io`'s VFS (`ra8_io_vfs_open()`) when you want mount
points, caching and format probing. It is the same filesystem underneath, not a
second one: `ra8_io_blockdev_as_fs_backend()` bridges any `ra8_io` block device
into an `ra8_fs_backend_t`.

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
users: ra8_io = 22
files: libs/ra8_fs/src/*.c = 32
files: tests/storage/src/test_ra8_fs*.c = 67
-->
