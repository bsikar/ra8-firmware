# libs/ra_io

`ra_io` is one central peripheral-agnostic I/O fabric with swappable backends:
an application performs an operation and names the target rather than coupling
to a specific peripheral. The facade spans a 512-byte LBA block-device vtable
(one backend per medium: SD-over-SPI, native SDHI, OSPI NOR, MRAM, SDRAM,
in-RAM scratch), targetable streams, a VFS mount table and file operations,
pluggable filesystem formats, a page/block cache, and compression.

`ra_io` is tagged `[Ring 4 / PAL]`. Include `ra_io.h` to pull in the whole
facade, or a single backend/feature header directly.

## Storage seams: blockdev (Ring 4, R/W/erase) vs vsource (Ring 2, read-only view)

There are two distinct storage seams in the tree, and the split between them is
**intentional, ring-respecting design -- not drift**:

| Seam | Header | Ring | Shape |
|------|--------|------|-------|
| `ra_io_blockdev` | `libs/ra_io/inc/ra_io_blockdev.h` | `[Ring 4 / PAL]` | Read / write / erase over 512-byte logical blocks (LBA), plus capability query. The storage fabric filesystems and caches sit on. |
| `ra_vsource` | `libs/ra_mem/inc/ra_vsource.h` | `[Ring 2 / Core]` | Read-only byte-offset view feeding the issue #147 page cache (`ra_vmem`). Storage-agnostic by design: it binds a generic `offset -> bytes` callback so Layer 2 has no storage dependency. |

They are deliberately **not unified**. Unifying them would require the Ring-2
`ra_vsource` to depend on the Ring-4 `ra_io_blockdev`, which inverts ring
ordering -- a lower ring may not depend on a higher one (see
[`docs/RING_AND_WORLD.md`](../../docs/RING_AND_WORLD.md)). A read-only
byte-addressed cache source and a read/write block fabric are different
responsibilities at different rings; keeping them separate is correct.

### The sanctioned bridge

The dependency that **is** allowed runs from Ring 4 down to Ring 2. The adapter
`ra_io_blockdev_vsource.h` (Ring 4) exposes a bound `ra_io_blockdev_t` as a
read-only `ra_vsource_read_fn`, performing the LBA<->byte translation once
(bounce-buffering unaligned head/tail sectors through a caller-owned, zero-malloc
context). Apps wire a block device into the page cache with it instead of
re-rolling the arithmetic inline:

```c
static ra_io_blockdev_vsource_ctx_t s_vsrc_ctx;
(void)ra_io_blockdev_vsource_init(&s_vsrc_ctx, &bd);

uint32_t oid = 0;
(void)ra_vsource_add_paged(&vs, ra_io_blockdev_vsource_read, &s_vsrc_ctx,
                           0U, file_size_bytes, &oid);
```

This adapter is the one sanctioned bridge between the two seams; it does not
make them the same seam.
