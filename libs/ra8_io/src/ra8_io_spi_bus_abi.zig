//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of the ra8_io SPI bus front end (inc/ra8_io_spi_bus.h, RA8FW-707):
//! validate the bus, then dispatch through the bound backend vtable. Replaces
//! ra8_io_spi_bus.c, which is deleted. The vtable shape is the one the spi_b
//! unit pins against src/ra8_io_spi_bus_internal.h.

const spib = @import("ra8_io_spi_bus_spi_b_abi.zig");

pub const Iface = spib.Iface;
pub const Bus = spib.Bus;

const tag = "ra8_io_spi_bus";

/// ra8_err_t values this unit returns (ra8_err.h).
pub const ok: c_int = 0;
pub const err_not_initialized: c_int = 0x10F;
pub const err_null_ptr: c_int = 0x504;

/// ra8_spi_bus_ops_t (ra8_hal/inc/ra8_spi_bus_ops.h): the Ring-3 seam.
pub const Ops = extern struct {
    xfer8: ?*const fn (ctx: ?*anyopaque, tx: u8, rx: ?*u8) callconv(.c) c_int,
    ctx: ?*anyopaque,
};

comptime {
    if (@sizeOf(Ops) != 2 * @sizeOf(usize)) @compileError("Ops must be two pointers");
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

export fn ra8_io_spi_bus_xfer8(bus: ?*const Bus, tx: u8, rx: ?*u8) c_int {
    const i = switch (vtable(bus)) {
        .iface => |i| i,
        .err => |e| return e,
    };
    const op = i.xfer8 orelse return missing("backend xfer8 op missing");
    return op(bus.?.ctx, tx, rx);
}

export fn ra8_io_spi_bus_write_read(bus: ?*const Bus, tx: ?*const anyopaque, rx: ?*anyopaque, len: u32, width: u8) c_int {
    const i = switch (vtable(bus)) {
        .iface => |i| i,
        .err => |e| return e,
    };
    const op = i.write_read orelse return missing("backend write_read op missing");
    return op(bus.?.ctx, tx, rx, len, width);
}

export fn ra8_io_spi_bus_set_clock(bus: ?*const Bus, baud_hz: u32, pclk_hz: u32) c_int {
    const i = switch (vtable(bus)) {
        .iface => |i| i,
        .err => |e| return e,
    };
    const op = i.set_clock orelse return missing("backend set_clock op missing");
    return op(bus.?.ctx, baud_hz, pclk_hz);
}

/// The trampoline as_ops installs: ctx is the bound bus itself.
fn opsXfer8(ctx: ?*anyopaque, tx: u8, rx: ?*u8) callconv(.c) c_int {
    const bus: *const Bus = @ptrCast(@alignCast(ctx orelse return missing("ctx (spi bus) must not be nullptr")));
    return ra8_io_spi_bus_xfer8(bus, tx, rx);
}

export fn ra8_io_spi_bus_as_ops(bus: ?*const Bus, out: ?*Ops) c_int {
    switch (vtable(bus)) {
        .iface => {},
        .err => |e| return e,
    }
    const o = out orelse return missing("out must not be nullptr");
    o.xfer8 = &opsXfer8;
    o.ctx = @constCast(@ptrCast(bus.?));
    return ok;
}
