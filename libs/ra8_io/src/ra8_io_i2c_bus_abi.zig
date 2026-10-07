//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of the ra8_io I2C bus front end (inc/ra8_io_i2c_bus.h, RA8FW-714):
//! validate the bus, then dispatch through the bound backend vtable, and
//! bridge a bound bus onto the Ring-3 ra8_i2c_bus_ops_t seam. Replaces
//! ra8_io_i2c_bus.c, which is deleted. The vtable shape is the one the RIIC
//! unit pins against src/ra8_io_i2c_bus_internal.h.

const riic = @import("ra8_io_i2c_bus_riic_abi.zig");

pub const Iface = riic.Iface;
pub const Bus = riic.Bus;

const tag = "ra8_io_i2c_bus";

/// ra8_err_t values this unit returns (ra8_err.h).
pub const ok: c_int = 0;
pub const err_not_initialized: c_int = 0x10F;
pub const err_null_ptr: c_int = 0x504;

/// ra8_i2c_bus_ops_t (ra8_hal/inc/ra8_i2c_bus_ops.h): the Ring-3 seam.
pub const Ops = extern struct {
    write: ?*const fn (ctx: ?*anyopaque, addr: u8, data: ?[*]const u8, len: u32, send_stop: bool) callconv(.c) c_int,
    read: ?*const fn (ctx: ?*anyopaque, addr: u8, data: ?[*]u8, len: u32) callconv(.c) c_int,
    transfer: ?*const fn (ctx: ?*anyopaque, addr: u8, wr: ?[*]const u8, wr_len: u32, rd: ?[*]u8, rd_len: u32) callconv(.c) c_int,
    ctx: ?*anyopaque,
};

comptime {
    if (@sizeOf(Ops) != 4 * @sizeOf(usize)) @compileError("Ops must be four pointers");
}

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

/// The bound vtable, or the error the C front end returned for this bus.
fn vtable(bus: ?*const Bus) union(enum) { iface: *const Iface, err: c_int } {
    const b = bus orelse return .{ .err = err_null_ptr };
    const i = b.iface orelse return .{ .err = err_not_initialized };
    return .{ .iface = i };
}

/// RA8_CHECK_NULL_PTR: log the missing piece once, report a null pointer.
fn missing(message: [*:0]const u8) c_int {
    ra8_log_emit_error(tag, message);
    return err_null_ptr;
}

export fn ra8_io_i2c_bus_write(bus: ?*const Bus, addr: u8, data: ?[*]const u8, len: u32, send_stop: bool) c_int {
    const i = switch (vtable(bus)) {
        .iface => |i| i,
        .err => |e| return e,
    };
    const op = i.write orelse return missing("backend write op missing");
    return op(bus.?.ctx, addr, data, len, send_stop);
}

export fn ra8_io_i2c_bus_read(bus: ?*const Bus, addr: u8, data: ?[*]u8, len: u32) c_int {
    const i = switch (vtable(bus)) {
        .iface => |i| i,
        .err => |e| return e,
    };
    const op = i.read orelse return missing("backend read op missing");
    return op(bus.?.ctx, addr, data, len);
}

export fn ra8_io_i2c_bus_transfer(bus: ?*const Bus, addr: u8, wr: ?[*]const u8, wr_len: u32, rd: ?[*]u8, rd_len: u32) c_int {
    const i = switch (vtable(bus)) {
        .iface => |i| i,
        .err => |e| return e,
    };
    const op = i.transfer orelse return missing("backend transfer op missing");
    return op(bus.?.ctx, addr, wr, wr_len, rd, rd_len);
}

/// The trampolines as_ops installs: ctx is the bound bus itself.
fn busOf(ctx: ?*anyopaque) ?*const Bus {
    return @ptrCast(@alignCast(ctx));
}

fn opsWrite(ctx: ?*anyopaque, addr: u8, data: ?[*]const u8, len: u32, send_stop: bool) callconv(.c) c_int {
    const bus = busOf(ctx) orelse return missing("ctx (i2c bus) must not be nullptr");
    return ra8_io_i2c_bus_write(bus, addr, data, len, send_stop);
}

fn opsRead(ctx: ?*anyopaque, addr: u8, data: ?[*]u8, len: u32) callconv(.c) c_int {
    const bus = busOf(ctx) orelse return missing("ctx (i2c bus) must not be nullptr");
    return ra8_io_i2c_bus_read(bus, addr, data, len);
}

fn opsTransfer(ctx: ?*anyopaque, addr: u8, wr: ?[*]const u8, wr_len: u32, rd: ?[*]u8, rd_len: u32) callconv(.c) c_int {
    const bus = busOf(ctx) orelse return missing("ctx (i2c bus) must not be nullptr");
    return ra8_io_i2c_bus_transfer(bus, addr, wr, wr_len, rd, rd_len);
}

export fn ra8_io_i2c_bus_as_ops(bus: ?*const Bus, out: ?*Ops) c_int {
    switch (vtable(bus)) {
        .iface => {},
        .err => |e| return e,
    }
    const o = out orelse return missing("out must not be nullptr");
    o.write = &opsWrite;
    o.read = &opsRead;
    o.transfer = &opsTransfer;
    o.ctx = @ptrCast(@constCast(bus.?));
    return ok;
}
