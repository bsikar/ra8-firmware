<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 Brighton Sikarskie
-->

# vfs_port_demo

First consumer of `libs/if_ra8_vfs` outside the unit tests.

`if_ra8_vfs` is the composition-root adapter that binds one live named VFS
mount into the backend-neutral `fw_if_fs` facade, so portable code works in
`/books/a.bin` and never sees the mount name or the medium underneath. Only
`tests/misc/src/test_fw_if_fs.c` referenced it; nothing in `apps/` or
`examples/` ever bound a volume through it. This app does, end to end.

## What it checks

| Leg           | Check                                                                     |
| ------------- | ------------------------------------------------------------------------- |
| `bind`        | RAM blockdev, FAT12 format, mount, `ra8_io_vfs` name, adapter init.       |
| `caps`        | Namespace, stream and transaction support advertised.                     |
| `caps`        | Durable sync, atomic replace and symlinks NOT advertised.                 |
| `caps`        | Advertised workspace sizes fit the buffers this app reserves.             |
| `round-trip`  | mkdir, write, read back byte for byte, stat the size.                     |
| `listing`     | The directory cursor delivers the new leaf exactly once.                  |
| `transaction` | Staged `create_new`: write, validate, commit, destination published.      |
| `transaction` | A second `create_new` to the same name is refused, published file intact. |
| `path-guard`  | `/../escape` is refused before any backend sees it.                       |
| `unwind`      | Unlink both files, release the VFS name, unmount the volume.              |

The capability leg is the interesting one. The adapter's header is explicit
that FAT plus this stack exposes neither a durable medium flush nor
crash-atomic replacement, so the app asserts those bits are absent rather
than assuming a filesystem offers them.

## Determinism

The medium is a 256 KB RAM disk in `.bss` that this file owns, formatted
fresh on every boot, so no card has to be inserted and every leg is
repeatable. A board is needed only to confirm the console path, which is why
this lives in `hw_pending`.

Footprint is dominated by the RAM disk and the FAT caches: about 290 KB of
`.bss`, 27% of SRAM. Shrink `k_vfs_disk_blocks` if that gets in the way.

## Build and run

```sh
cmake --preset ra8d2-debug
ninja -C cmake-build-debug vfs_port_demo.elf
```

Flash and watch SCI8 (J-Link OB VCOM, 115200 8N1). A good run prints one
verdict per leg then:

```
vfs_port_demo: ALL PASS
```

## Related

- `libs/if_ra8_vfs/inc/fw_if_fs_ra8_vfs.h`
- `libs/if/inc/fw_if_fs.h`
- `tests/misc/src/test_fw_if_fs.c`
