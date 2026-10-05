//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of inc/ra8_io_i2c_bus_riic.h (RA8FW-709): binds the RIIC peripheral
//! (IIC0..IIC2) behind the ra8_io I2C bus vtable. Each trampoline forwards to
//! the unmodified ra8_i2c_* polling driver with the channel carried in ctx;
//! argument checks stay with that driver. Replaces ra8_io_i2c_bus_riic.c,
//! which is deleted. Iface mirrors struct ra8_io_i2c_bus_iface
//! (src/ra8_io_i2c_bus_internal.h), which the Zig front end
//! (ra8_io_i2c_bus_abi.zig) dispatches through.

const tag = "ra8_io_i2c_bus_riic";

/// ra8_err_t values this unit returns (ra8_err.h).
pub const ok: c_int = 0;
pub const err_invalid_arg: c_int = 0x103;
pub const err_null_ptr: c_int = 0x504;

/// ra8_i2c_regs.h: k_ra8_i2c_channel_count (HUM Ch 39.1, three channels).
pub const channel_count: u8 = 3;

/// struct ra8_io_i2c_bus_iface (src/ra8_io_i2c_bus_internal.h).
pub const Iface = extern struct {
    write: ?*const fn (ctx: ?*anyopaque, addr: u8, data: ?[*]const u8, len: u32, send_stop: bool) callconv(.c) c_int,
    read: ?*const fn (ctx: ?*anyopaque, addr: u8, data: ?[*]u8, len: u32) callconv(.c) c_int,
    transfer: ?*const fn (ctx: ?*anyopaque, addr: u8, wr: ?[*]const u8, wr_len: u32, rd: ?[*]u8, rd_len: u32) callconv(.c) c_int,
};

/// ::ra8_io_i2c_bus_t (inc/ra8_io_i2c_bus.h).
pub const Bus = extern struct {
    iface: ?*const Iface,
    ctx: ?*anyopaque,
};

comptime {
    if (@sizeOf(Iface) != 3 * @sizeOf(usize)) @compileError("Iface must be three pointers");
    if (@sizeOf(Bus) != 2 * @sizeOf(usize)) @compileError("Bus must be two pointers");
}

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_i2c_write(channel: u8, addr: u8, data: ?[*]const u8, len: u32, send_stop: bool) c_int;
extern fn ra8_i2c_read(channel: u8, addr: u8, data: ?[*]u8, len: u32) c_int;
extern fn ra8_i2c_transfer(channel: u8, addr: u8, wr: ?[*]const u8, wr_len: u32, rd: ?[*]u8, rd_len: u32) c_int;

/// The channel rides in ctx as an integer, as the C backend did.
pub fn channelOf(ctx: ?*anyopaque) u8 {
    return @truncate(@intFromPtr(ctx));
}

fn write(ctx: ?*anyopaque, addr: u8, data: ?[*]const u8, len: u32, send_stop: bool) callconv(.c) c_int {
    return ra8_i2c_write(channelOf(ctx), addr, data, len, send_stop);
}

fn read(ctx: ?*anyopaque, addr: u8, data: ?[*]u8, len: u32) callconv(.c) c_int {
    return ra8_i2c_read(channelOf(ctx), addr, data, len);
}

fn transfer(ctx: ?*anyopaque, addr: u8, wr: ?[*]const u8, wr_len: u32, rd: ?[*]u8, rd_len: u32) callconv(.c) c_int {
    return ra8_i2c_transfer(channelOf(ctx), addr, wr, wr_len, rd, rd_len);
}

pub const iface = Iface{ .write = &write, .read = &read, .transfer = &transfer };

export fn ra8_io_i2c_bus_bind_riic(bus: ?*Bus, channel: u8) c_int {
    const b = bus orelse {
        ra8_log_emit_error(tag, "bus must not be nullptr");
        return err_null_ptr;
    };
    if (channel >= channel_count) return err_invalid_arg;
    b.iface = &iface;
    b.ctx = @ptrFromInt(@as(usize, channel));
    return ok;
}
