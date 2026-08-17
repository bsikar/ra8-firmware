# libs/ra8_io

`ra8_io` is one central peripheral-agnostic I/O fabric with swappable backends:
an application performs an operation and names the target, rather than coupling
itself to a specific peripheral. It spans a 512-byte LBA block-device vtable
with one backend per medium, SPI and I2C controller-bus vtables with one
backend per twin peripheral implementation, targetable streams, a VFS mount
table and file operations, pluggable filesystem formats, a page/block cache,
and compression. `ra8_io.h` pulls in the whole facade; a single backend or
feature header can be included on its own instead.

`ra8_io` is tagged `[Ring 4 / PAL]`, and most of what is surprising about its
shape follows from that one fact.

## Two storage seams, deliberately not unified

`ra8_io_blockdev` is read / write / erase over 512-byte logical blocks -- the
storage fabric that filesystems and caches sit on. `ra8_vsource` (over in
`ra8_mem`) is a read-only byte-offset view feeding the #147 page cache, and
binds a generic `offset -> bytes` callback so that layer carries no storage
dependency at all.

They look like they ought to be one seam. They cannot be: `ra8_vsource` is
`[Ring 2 / Core]`, so unifying them would make a lower ring depend on a higher
one, which the ring ordering forbids (see
[`docs/RING_AND_WORLD.md`](../../docs/RING_AND_WORLD.md)). A read-only
byte-addressed cache source and a read/write block fabric are different
responsibilities at different rings, and keeping them apart is the design.

The dependency that *is* allowed runs downward, Ring 4 to Ring 2.
`ra8_io_blockdev_vsource.h` exposes a bound block device as a read-only
`ra8_vsource` read function, performing the LBA-to-byte translation once and
bounce-buffering unaligned head and tail sectors through a caller-owned,
zero-malloc context. Wire a block device into the page cache with that adapter
instead of re-rolling the arithmetic inline. It is the one sanctioned bridge
between the seams, and it does not make them the same seam.

## One transfer vtable over each twin peripheral pair

The RA8D2 implements SPI twice (dedicated SPI_B; SCI in Simple-SPI mode) and
I2C twice (classic RIIC; the I3C block's I2C-compatibility mode), with
byte-identical controller transfer surfaces. `ra8_io_spi_bus.h` and
`ra8_io_i2c_bus.h` wrap each pair behind one caller-allocated handle, so which
physical peripheral a board revision routes a device to becomes a bind-time
decision (#198 / #199).

The two facades stay separate on purpose: SPI has out-of-band chip select and
is full-duplex, I2C carries an in-band 7-bit address and is half-duplex. Bus
bring-up remains the caller's -- the facades own transfers only.

The ring rule applies here too. A Ring-3 device driver may not depend on this
Ring-4 facade, so those drivers take the Ring-3 seams `ra8_spi_bus_ops_t` /
`ra8_i2c_bus_ops_t` in their configs, and `ra8_io_spi_bus_as_ops()` /
`ra8_io_i2c_bus_as_ops()` fill those seams from a bound bus -- the exact
analogue of `ra8_io_blockdev_as_fs_backend()`.

An app needing only the bus facades can take the `ra8_io_bus` pseudo-library
rather than `ra8_io`: it compiles just those translation units, which depend
only on `ra8_hal`, so none of the full fabric's companion libraries come along.
