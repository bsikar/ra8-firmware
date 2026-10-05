//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of inc/ra8_io_i2c_bus_i3c_compat.h (RA8FW-711): binds the I3C
//! block's legacy-I2C mode behind the ra8_io I2C bus vtable. Each trampoline
//! forwards to the unmodified ra8_i3c_* data path with the channel carried in
//! ctx. Replaces ra8_io_i2c_bus_i3c_compat.c, which is deleted. Iface and Bus
//! are the RIIC unit's mirror of struct ra8_io_i2c_bus_iface, so the layout
//! is pinned in one place.

const riic = @import("ra8_io_i2c_bus_riic_abi.zig");

const tag = "ra8_io_i2c_bus_i3c";

pub const ok = riic.ok;
pub const err_invalid_arg = riic.err_invalid_arg;
pub const err_null_ptr = riic.err_null_ptr;
pub const Iface = riic.Iface;
pub const Bus = riic.Bus;
pub const channelOf = riic.channelOf;

/// ra8_i3c_i2c_regs.h: k_ra8_i3c_i2c_channel_count (HUM Ch 40.1.1, p 2445).
pub const channel_count: u8 = 1;

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_i3c_write(channel: u8, addr: u8, data: ?[*]const u8, len: u32, restart: bool) c_int;
extern fn ra8_i3c_read(channel: u8, addr: u8, buf: ?[*]u8, len: u32, restart: bool) c_int;
extern fn ra8_i3c_transfer(channel: u8, addr: u8, wr: ?[*]const u8, wr_len: u32, rd: ?[*]u8, rd_len: u32) c_int;

/// The facade's send_stop is the driver's restart inverted: a write that
/// keeps the bus ends in a repeated START instead of a STOP.
fn write(ctx: ?*anyopaque, addr: u8, data: ?[*]const u8, len: u32, send_stop: bool) callconv(.c) c_int {
    return ra8_i3c_write(channelOf(ctx), addr, data, len, !send_stop);
}

/// A facade read always ends the transaction, so it never asks for a restart.
fn read(ctx: ?*anyopaque, addr: u8, data: ?[*]u8, len: u32) callconv(.c) c_int {
    return ra8_i3c_read(channelOf(ctx), addr, data, len, false);
}

fn transfer(ctx: ?*anyopaque, addr: u8, wr: ?[*]const u8, wr_len: u32, rd: ?[*]u8, rd_len: u32) callconv(.c) c_int {
    return ra8_i3c_transfer(channelOf(ctx), addr, wr, wr_len, rd, rd_len);
}

pub const iface = Iface{ .write = &write, .read = &read, .transfer = &transfer };

export fn ra8_io_i2c_bus_bind_i3c_compat(bus: ?*Bus, channel: u8) c_int {
    const b = bus orelse {
        ra8_log_emit_error(tag, "bus must not be nullptr");
        return err_null_ptr;
    };
    if (channel >= channel_count) return err_invalid_arg;
    b.iface = &iface;
    b.ctx = @ptrFromInt(@as(usize, channel));
    return ok;
}
