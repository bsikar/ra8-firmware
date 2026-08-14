# `ra8_mdl_storage_vfs`

This Ring-4 adapter is the production binding between the media-download RPC
coordinator's transactional storage seam and `ra8_io_vfs`. It keeps the
coordinator independent of SD, OSPI, MRAM, SDRAM, RAM, FAT, and exFAT. The
composition root mounts a device under a VFS name and selects it in the
destination:

```c
static ra8_mdl_storage_vfs_t s_storage;
static ra8_mdl_storage_iface_t s_iface;

const ra8_mdl_storage_vfs_config_t cfg = {
  .stage_leaf = "MDLSTAGE.TMP", /* reserved by this application */
  .validate = validate_rabook,
  .validate_ctx = &s_validator,
};

(void)ra8_mdl_storage_vfs_init(&s_storage, &cfg, &s_iface);
/* `sd:` selects the mounted SD volume; the adapter contains no SD branch. */
(void)s_iface.begin(s_iface.ctx, "sd:/BOOKS/STORY.RBK");
```

## Publication contract

The adapter creates the configured staging leaf beside the destination. That
leaf is an application-reserved name: startup recovery may unlink a stale
regular file with that exact name, but never a directory. Each concurrently
active adapter must receive a distinct reserved leaf.

`validate` closes the staged file, checks it is a regular file of the exact
tracked length, and invokes the optional format validator. `commit` checks the
destination again and performs one same-mount `ra8_io_vfs_rename`.

The current VFS offers a serialized same-mount **no-replace** rename, not
atomic replacement. A successful call is atomic to filesystem readers because
the filesystem lock spans it; the native capability table still correctly
advertises `atomic_rename == false` because FAT/exFAT rename is not
power-loss-atomic. An existing destination is therefore refused and preserved.
There is no reader-visible backup dance and no destructive truncate of the
final path.

Before close, the adapter requests `ra8_io_vfs_file_sync`; a format that
advertises sync must complete it, while an explicit `not_supported` result is
accepted and retained as a documented capability limit. On native `ra8_fs`,
writes reach sectors synchronously and close commits file metadata (and FAT32
FSInfo where applicable), but the current native format advertises neither
software sync nor durable sync. The block-device contract has no cache-flush or
durable-barrier operation, so this adapter does not claim power-loss durability
beyond successful backend acknowledgements.

All paths and adapter state live in caller-owned fixed arrays; open streams use
the VFS's fixed pool and format-neutral stream facade. No dynamic allocation,
POSIX API, or device-specific code is used.

Firmware applications list `ra8_mdl_storage_vfs`, `ra8_c6link`, and their
selected VFS, filesystem, and storage-backend libraries in
`ra8_add_app(LIBS ...)`; the first-party build recipe deliberately keeps those
composition dependencies explicit.

Qualification is in `tests/test_ra8_mdl_storage_vfs.c`; it runs through a real
RAM block device, FAT16, and VFS mount while injecting capacity, removal, and
rename-write faults.
