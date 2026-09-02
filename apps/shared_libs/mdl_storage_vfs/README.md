# `mdl_storage_vfs`

The Ring-4 binding between the media-download coordinator's transactional
storage seam and `ra8_io_vfs`. It is what keeps the coordinator ignorant of SD,
OSPI, MRAM, SDRAM, RAM, FAT and exFAT: the composition root mounts a device
under a VFS name, the destination path selects it, and this adapter contains no
per-device branch of any kind.

## Publication contract

A transfer stages into a configured leaf beside its destination. That leaf name
is reserved to the application: startup recovery may unlink a stale regular
file bearing it, but never a directory, and two concurrently active adapters
must be given different leaves.

Validation closes the staged file, checks it is a regular file of exactly the
tracked length, then runs the optional format validator. Commit re-checks the
destination and performs a single same-mount rename.

**The rename is no-replace, not atomic replacement.** It is atomic to
filesystem readers because the filesystem lock spans it, but the native
capability table still advertises `atomic_rename == false`, because FAT and
exFAT rename is not power-loss-atomic. An existing destination is therefore
refused and left intact. There is no reader-visible backup dance and no
destructive truncate of the final path.

**No power-loss durability is claimed.** The adapter requests a file sync
before close; a format that advertises sync must complete it, while an explicit
`not_supported` result is accepted and retained as a documented capability
limit. The block-device contract has no cache-flush or durable-barrier
operation, so a successful backend acknowledgement is the strongest claim
available and the adapter does not exceed it.

Paths and adapter state live in caller-owned fixed arrays, and open streams
come from the VFS's fixed pool: no dynamic allocation, no POSIX API, no
device-specific code. Qualification runs against a real RAM block device, a
real FAT16 volume and a real VFS mount with capacity, removal and rename-write
faults injected -- not against a mocked VFS.

## Strict `.rabook` binding

`mdl_rabook_vfs` is the no-heap format binding for chunked `.rabook`
(`RBKC`) artifacts. It is not another file abstraction: it adapts an already
open VFS file to `book_chunked`, reads every chunk through the VFS, and
applies the existing strict streamed RABOOK1 structural and CRC validator. The
inflater, table, compressed chunk, inflated chunk and strict-validator scratch
spans are all supplied by the application and remain caller-owned.

It installs directly as the storage adapter's `validate` callback. Stage
validation closes its file before returning and retains only decoded metadata
and the independently verified transfer digest; opening the published path
validates strictly all over again, so bytes that changed after staging cannot
inherit the stage verdict. A rejected artifact -- corrupted in transit, or
coherently hashed but structurally invalid -- leaves neither a published nor a
staged object behind.
