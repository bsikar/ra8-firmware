//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Root of the `ra8_io` archive: every ported unit, referenced so its C ABI
//! exports are emitted.

pub const log = @import("ra8_io_log_abi.zig");
pub const stream_ram = @import("ra8_io_stream_ram_abi.zig");
pub const stream_uart = @import("ra8_io_stream_uart_abi.zig");
pub const stream_usbcdc = @import("ra8_io_stream_usbcdc_abi.zig");
pub const blockdev_sdram = @import("blockdev_sdram_abi.zig");

comptime {
    _ = log;
    _ = stream_ram;
    _ = stream_uart;
    _ = stream_usbcdc;
    _ = blockdev_sdram;
}
