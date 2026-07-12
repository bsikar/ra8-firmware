# libs/ra8_io

`ra8_io` is one central peripheral-agnostic I/O fabric with swappable backends:
an application performs an operation and names the target rather than coupling
to a specific peripheral. The facade spans a 512-byte LBA block-device vtable
(one backend per medium: SD-over-SPI, native SDHI, OSPI NOR, MRAM, SDRAM,
in-RAM scratch), SPI and I2C controller-bus vtables (one backend per twin
peripheral implementation), targetable streams, a VFS mount table and file
operations, pluggable filesystem formats, a page/block cache, and compression.

`ra8_io` is tagged `[Ring 4 / PAL]`. Include `ra8_io.h` to pull in the whole
facade, or a single backend/feature header directly.

## Storage seams: blockdev (Ring 4, R/W/erase) vs vsource (Ring 2, read-only view)

There are two distinct storage seams in the tree, and the split between them is
**intentional, ring-respecting design -- not drift**:

| Seam | Header | Ring | Shape |
|------|--------|------|-------|
| `ra8_io_blockdev` | `libs/ra8_io/inc/ra8_io_blockdev.h` | `[Ring 4 / PAL]` | Read / write / erase over 512-byte logical blocks (LBA), plus capability query. The storage fabric filesystems and caches sit on. |
| `ra8_vsource` | `libs/ra8_mem/inc/ra8_vsource.h` | `[Ring 2 / Core]` | Read-only byte-offset view feeding the issue #147 page cache (`ra8_vmem`). Storage-agnostic by design: it binds a generic `offset -> bytes` callback so Layer 2 has no storage dependency. |

They are deliberately **not unified**. Unifying them would require the Ring-2
`ra8_vsource` to depend on the Ring-4 `ra8_io_blockdev`, which inverts ring
ordering -- a lower ring may not depend on a higher one (see
[`docs/RING_AND_WORLD.md`](../../docs/RING_AND_WORLD.md)). A read-only
byte-addressed cache source and a read/write block fabric are different
responsibilities at different rings; keeping them separate is correct.

### The sanctioned bridge

The dependency that **is** allowed runs from Ring 4 down to Ring 2. The adapter
`ra8_io_blockdev_vsource.h` (Ring 4) exposes a bound `ra8_io_blockdev_t` as a
read-only `ra8_vsource_read_fn`, performing the LBA<->byte translation once
(bounce-buffering unaligned head/tail sectors through a caller-owned, zero-malloc
context). Apps wire a block device into the page cache with it instead of
re-rolling the arithmetic inline:

```c
static ra8_io_blockdev_vsource_ctx_t s_vsrc_ctx;
(void)ra8_io_blockdev_vsource_init(&s_vsrc_ctx, &bd);

uint32_t oid = 0;
(void)ra8_vsource_add_paged(&vs, ra8_io_blockdev_vsource_read, &s_vsrc_ctx,
                           0U, file_size_bytes, &oid);
```

This adapter is the one sanctioned bridge between the two seams; it does not
make them the same seam.

## Bus facades: one transfer vtable over each twin peripheral pair

The RA8D2 implements SPI twice (dedicated SPI_B; SCI in Simple-SPI mode) and
I2C twice (classic RIIC; the I3C block's I2C-compatibility mode), with
byte-identical controller transfer surfaces. `ra8_io_spi_bus.h` and
`ra8_io_i2c_bus.h` wrap each pair behind one caller-allocated handle so the
physical peripheral a board revision routes to a device is a bind-time
decision (issues #198 / #199):

```c
ra8_io_i2c_bus_t bus = {};
(void)ra8_io_i2c_bus_bind_i3c_compat(&bus, 0U);  /* today's board          */
/* (void)ra8_io_i2c_bus_bind_riic(&bus, 1U);        future board revision  */
(void)ra8_io_i2c_bus_transfer(&bus, addr_7b, reg_ptr, 2U, out, len);
```

The two facades are deliberately separate: SPI has out-of-band chip select
and is full-duplex, I2C carries an in-band 7-bit address and is half-duplex.
Bus bring-up (`ra8_spi_init` / `ra8_sci_spi_init` / `ra8_i2c_init` /
`ra8_i3c_init`) stays with the caller -- the facades own transfers only.

The same ring rule as the storage seams applies: a Ring-3 device driver
(`ra8_epaper`, `ra8_touch`, `ra8_smbus`) may not depend on this Ring-4 facade.
Those drivers consume the Ring-3 seams `ra8_spi_bus_ops_t` /
`ra8_i2c_bus_ops_t` (`libs/ra8_hal/inc/ra8_*_bus_ops.h`) in their configs, and
`ra8_io_spi_bus_as_ops()` / `ra8_io_i2c_bus_as_ops()` are the sanctioned
Ring-4 bridges that fill those seams from a bound bus -- the exact analogue
of `ra8_io_blockdev_as_fs_backend()`.

Apps that only need the bus facades (no storage fabric) list the
`ra8_io_bus` pseudo-lib in `ra8_add_app(... LIBS ...)`: it compiles just the
`ra8_io_spi_bus*` / `ra8_io_i2c_bus*` TUs, which depend only on `ra8_hal`, so
none of the full fabric's companion libraries are dragged in.
