//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of inc/ra8_io_spi_bus_spi_b.h (RA8FW-701): binds the SPI_B
//! peripheral (SPI0, SPI1) behind the ra8_io SPI bus vtable. Each trampoline
//! forwards to the unmodified ra8_spi_* driver with the channel carried in
//! ctx. Replaces ra8_io_spi_bus_spi_b.c, which is deleted. Iface mirrors
//! struct ra8_io_spi_bus_iface (src/ra8_io_spi_bus_internal.h); the Zig front
//! end (ra8_io_spi_bus_abi.zig, RA8FW-707) dispatches through it.

const tag = "ra8_io_spi_bus_spi_b";

/// ra8_err_t values this unit returns (ra8_err.h).
pub const ok: c_int = 0;
pub const err_invalid_arg: c_int = 0x103;
pub const err_null_ptr: c_int = 0x504;

/// ra8_spi_regs.h: k_ra8_spi_b_channel_count (SPI0 + SPI1).
pub const channel_count: u8 = 2;

/// struct ra8_io_spi_bus_iface (src/ra8_io_spi_bus_internal.h). `width` is
/// ra8_spi_bit_width_t, a uint8_t enum.
pub const Iface = extern struct {
    xfer8: ?*const fn (ctx: ?*anyopaque, tx: u8, rx: ?*u8) callconv(.c) c_int,
    write_read: ?*const fn (ctx: ?*anyopaque, tx: ?*const anyopaque, rx: ?*anyopaque, len: u32, width: u8) callconv(.c) c_int,
    set_clock: ?*const fn (ctx: ?*anyopaque, baud_hz: u32, pclk_hz: u32) callconv(.c) c_int,
};

/// ::ra8_io_spi_bus_t (inc/ra8_io_spi_bus.h).
pub const Bus = extern struct {
    iface: ?*const Iface,
    ctx: ?*anyopaque,
};

comptime {
    if (@sizeOf(Iface) != 3 * @sizeOf(usize)) @compileError("Iface must be three pointers");
    if (@sizeOf(Bus) != 2 * @sizeOf(usize)) @compileError("Bus must be two pointers");
}

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_spi_xfer8(channel: u8, tx: u8, rx: ?*u8) c_int;
extern fn ra8_spi_write_read(channel: u8, tx: ?*const anyopaque, rx: ?*anyopaque, len: u32, width: u8) c_int;
extern fn ra8_spi_set_clock(channel: u8, baud_hz: u32, pclka_hz: u32) c_int;

/// The channel rides in ctx as an integer, as the C backend did.
pub fn channelOf(ctx: ?*anyopaque) u8 {
    return @truncate(@intFromPtr(ctx));
}

fn xfer8(ctx: ?*anyopaque, tx: u8, rx: ?*u8) callconv(.c) c_int {
    return ra8_spi_xfer8(channelOf(ctx), tx, rx);
}

fn writeRead(ctx: ?*anyopaque, tx: ?*const anyopaque, rx: ?*anyopaque, len: u32, width: u8) callconv(.c) c_int {
    return ra8_spi_write_read(channelOf(ctx), tx, rx, len, width);
}

fn setClock(ctx: ?*anyopaque, baud_hz: u32, pclk_hz: u32) callconv(.c) c_int {
    return ra8_spi_set_clock(channelOf(ctx), baud_hz, pclk_hz);
}

pub const iface = Iface{ .xfer8 = &xfer8, .write_read = &writeRead, .set_clock = &setClock };

export fn ra8_io_spi_bus_bind_spi_b(bus: ?*Bus, channel: u8) c_int {
    const b = bus orelse {
        ra8_log_emit_error(tag, "bus must not be nullptr");
        return err_null_ptr;
    };
    if (channel >= channel_count) return err_invalid_arg;
    b.iface = &iface;
    b.ctx = @ptrFromInt(@as(usize, channel));
    return ok;
}
