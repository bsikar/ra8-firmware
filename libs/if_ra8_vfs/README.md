# libs/if_ra8_vfs

The seam between the portable filesystem facade and the platform storage
stack. `fw_if_fs_ra8_vfs` is a composition root: it binds one already-mounted
`ra8_io_vfs` volume into the portable `fw_fs` facade, so portable code (the
reader, the shelf, module code behind the syscall boundary) works on plain
paths like `/books/a.cbz` and never sees the mount name, the medium, or the
filesystem beneath it.

`ra8_fs` and `ra8_io` are documented on their own pages. This page is the
contract of the seam itself: what the adapter guarantees, what it refuses to
guarantee, and who consumes it.

## The contract

- **One mount, one facade.** `fw_fs_ra8_vfs_init()` takes a live `ra8_fs`
  mount, the registered VFS name without the `:`, and a media property, and
  returns a complete `fw_fs_t`. Portable `/books/a.cbz` becomes
  `flash:/books/a.cbz` internally; the prefix never leaks back out.
- **The path is bounded.** The full-path scratch is
  `k_fw_fs_path_cap + k_ra8_io_vfs_name_max + 2` (`name:` + path + NUL). A
  path that cannot fit returns an error; nothing truncates silently.
- **No allocation.** The caller owns `fw_fs_ra8_vfs_state_t` (two path
  scratch buffers plus bookkeeping). Dispatch is allocation-free.
- **One state, no reentrancy.** The adapter is deliberately non-reentrant;
  serialization belongs to the caller above it.
- **Symlinks are structurally absent.** FAT/exFAT has none, and the generic
  lexical path guard runs before anything opens.
- **Staged transactions publish a new name.** A transaction writes to a
  stage leaf (`TX` + six hex digits + `.TMP`) and renames into place.
  Replacing an existing destination is rejected without changing it; there is
  no durable sync and no crash-atomic replacement on the current stack, and
  the adapter does not advertise either.
- **Errors are the C ABI's `u16` codes.** Every adapter call returns a
  `ra8_err_t`; the value set is fixed in `src/internal/root.zig` and mirrors
  `ra8_err.h` verbatim.

## Who consumes it, and with what test

| Consumer | Path | Test |
|---|---|---|
| Host end-to-end suite | `tests/misc/src/test_lx_fs_vfs_e2e.c` binds the adapter (`internal_bring_up`) over a FAT12 volume on a LevelX NOR fake, then drives open/write/read/stat/listdir/cursor/space, staged publishes, and unlink/rmdir/unmount through the facade | `test_lx_fs_vfs_e2e`, green end to end (RA8FW-1008, PR #3893) |
| `vfs_port_demo` | `examples/ek_ra8d2/hw_pending/vfs_port_demo` mounts a `ram` volume on the board and binds it the same way | Board demo only; no host test yet (tracked under this ticket) |

The library's own unit tests (`tests/stream_test.zig`, `tests/abi_test.zig`,
`tests/internal_test.zig`, with `tests/vfs_fake.zig` standing in for the VFS)
cover the adapter logic without linking the stack; they exercise the seam from
inside. The consumer rows above exercise it from outside.

## Where the truth lives

`inc/fw_if_fs_ra8_vfs.h` is the authority on the surface: limits, state
layout, and every field documented in place. `src/internal/root.zig` carries
the pure adapter logic (no externs), so the ABI layer owns every call that
leaves this library.
