//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of inc/ra8_io_spi_bus_sci_spi.h (RA8FW-703): binds an SCI channel
//! in simple-SPI mode (SCI0..SCI9) behind the ra8_io SPI bus vtable. Each
//! trampoline forwards to the unmodified ra8_sci_spi_* driver with the
//! channel carried in ctx. SCI SPI frames are 8-bit only. Replaces
//! ra8_io_spi_bus_sci_spi.c, which is deleted. Iface and Bus come from the
//! SPI_B unit so there is one Zig mirror of the C vtable.

const spib = @import("ra8_io_spi_bus_spi_b_abi.zig");
pub const Iface = spib.Iface;
pub const Bus = spib.Bus;
const channelOf = spib.channelOf;

const tag = "ra8_io_spi_bus_sci_spi";

/// ra8_err_t values this unit returns (ra8_err.h).
pub const ok: c_int = 0;
pub const err_invalid_arg: c_int = 0x103;
pub const err_not_supported: c_int = 0x107;
pub const err_null_ptr: c_int = 0x504;

/// ra8_sci_regs.h: k_ra8_sci_channel_count (SCI0..SCI9).
pub const channel_count: u8 = 10;
/// ra8_spi.h: k_ra8_spi_width_8, the only frame width SCI SPI carries.
pub const width_8: u8 = 7;

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_sci_spi_xfer8(channel: u8, tx: u8, rx: ?*u8) c_int;
extern fn ra8_sci_spi_xfer(channel: u8, tx: ?[*]const u8, rx: ?[*]u8, len: u32) c_int;
extern fn ra8_sci_spi_set_clock(channel: u8, baud_hz: u32, pclk_hz: u32) c_int;

fn xfer8(ctx: ?*anyopaque, tx: u8, rx: ?*u8) callconv(.c) c_int {
    return ra8_sci_spi_xfer8(channelOf(ctx), tx, rx);
}

fn writeRead(ctx: ?*anyopaque, tx: ?*const anyopaque, rx: ?*anyopaque, len: u32, width: u8) callconv(.c) c_int {
    if (width != width_8) return err_not_supported;
    return ra8_sci_spi_xfer(channelOf(ctx), @ptrCast(tx), @ptrCast(rx), len);
}

fn setClock(ctx: ?*anyopaque, baud_hz: u32, pclk_hz: u32) callconv(.c) c_int {
    return ra8_sci_spi_set_clock(channelOf(ctx), baud_hz, pclk_hz);
}

pub const iface = Iface{ .xfer8 = &xfer8, .write_read = &writeRead, .set_clock = &setClock };

export fn ra8_io_spi_bus_bind_sci_spi(bus: ?*Bus, channel: u8) c_int {
    const b = bus orelse {
        ra8_log_emit_error(tag, "bus must not be nullptr");
        return err_null_ptr;
    };
    if (channel >= channel_count) return err_invalid_arg;
    b.iface = &iface;
    b.ctx = @ptrFromInt(@as(usize, channel));
    return ok;
}
