//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Root of the `ra8_io` archive: every ported unit, referenced so its C ABI
//! exports are emitted.

pub const log = @import("ra8_io_log_abi.zig");
pub const stream_ram = @import("ra8_io_stream_ram_abi.zig");
pub const stream_uart = @import("ra8_io_stream_uart_abi.zig");
pub const stream_usbcdc = @import("ra8_io_stream_usbcdc_abi.zig");
pub const spi_bus = @import("ra8_io_spi_bus_abi.zig");
pub const spi_bus_spi_b = @import("ra8_io_spi_bus_spi_b_abi.zig");
pub const spi_bus_sci_spi = @import("ra8_io_spi_bus_sci_spi_abi.zig");
pub const i2c_bus_riic = @import("ra8_io_i2c_bus_riic_abi.zig");
pub const i2c_bus_i3c_compat = @import("ra8_io_i2c_bus_i3c_compat_abi.zig");
pub const i2c_bus = @import("ra8_io_i2c_bus_abi.zig");
pub const netif = @import("ra8_io_netif_abi.zig");
pub const netif_net_pal = @import("ra8_io_netif_net_pal_abi.zig");
pub const netif_c6link = @import("ra8_io_netif_c6link_abi.zig");
pub const blockdev_sdram = @import("blockdev_sdram_abi.zig");
pub const blockdev_usbmsc = @import("ra8_io_blockdev_usbmsc_abi.zig");
pub const blockdev_sdhi = @import("ra8_io_blockdev_sdhi_abi.zig");
pub const stream_blockdev = @import("ra8_io_stream_blockdev_abi.zig");
pub const blockdev_sdspi = @import("ra8_io_blockdev_sdspi_abi.zig");
pub const blockdev_ram = @import("ra8_io_blockdev_ram_abi.zig");
pub const blockdev = @import("ra8_io_blockdev_abi.zig");
pub const blockdev_mram = @import("ra8_io_blockdev_mram_abi.zig");
pub const blockdev_cache = @import("ra8_io_blockdev_cache_abi.zig");
pub const blockdev_vsource = @import("ra8_io_blockdev_vsource_abi.zig");
pub const vfs_namespace = @import("ra8_io_vfs_namespace_abi.zig");
pub const stream = @import("ra8_io_stream_abi.zig");
pub const vfs = @import("ra8_io_vfs_abi.zig");
pub const fsfmt = @import("ra8_io_fsfmt_abi.zig");

comptime {
    _ = log;
    _ = stream_ram;
    _ = stream_uart;
    _ = stream_usbcdc;
    _ = spi_bus;
    _ = spi_bus_spi_b;
    _ = spi_bus_sci_spi;
    _ = i2c_bus_riic;
    _ = i2c_bus_i3c_compat;
    _ = i2c_bus;
    _ = netif;
    _ = netif_net_pal;
    _ = netif_c6link;
    _ = blockdev_sdram;
    _ = blockdev_usbmsc;
    _ = blockdev_sdhi;
    _ = stream_blockdev;
    _ = blockdev_sdspi;
    _ = blockdev_ram;
    _ = blockdev;
    _ = blockdev_mram;
    _ = blockdev_cache;
    _ = blockdev_vsource;
    _ = vfs_namespace;
    _ = stream;
    _ = vfs;
    _ = fsfmt;
}
