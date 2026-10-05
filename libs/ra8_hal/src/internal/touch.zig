//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! GT911 touch logic (RA8FW-790), from ra8_touch.c: register pointer
//! packing, point decode, cfg checks and the frame read order. Generic
//! over a bus with read(reg, buf) u16 and writeByte(reg, value) u16.

const std = @import("std");

pub const ok: u16 = 0;
pub const err_hw_init_failed: u16 = 0x201;
pub const err_hw_error: u16 = 0x204;

pub const addr_low: u8 = 0x5D;
pub const addr_high: u8 = 0x14;
pub const reg_product: u16 = 0x8140;
pub const reg_status: u16 = 0x814E;
pub const reg_point0: u16 = 0x814F;
pub const cmd_clear_status: u8 = 0x00;
pub const count_mask: u8 = 0x0F;
pub const ready_mask: u8 = 0x80;
pub const max_points: u8 = 5;
pub const point_bytes: usize = 8;
pub const irq_pin_count: u8 = 32;

/// ra8_touch_point_t.
pub const Point = extern struct { x: u16, y: u16, track_id: u8, pressure: u8 };

comptime {
    std.debug.assert(@sizeOf(Point) == 6);
    std.debug.assert(@offsetOf(Point, "track_id") == 4);
    std.debug.assert(@offsetOf(Point, "pressure") == 5);
}

/// The GT911 register pointer goes out big-endian.
pub fn packReg(reg: u16) [2]u8 {
    return .{ @truncate(reg >> 8), @truncate(reg) };
}

/// One 8-byte record: track, x lsb/msb, y lsb/msb, size lsb (pressure).
pub fn decodeOne(raw: [*]const u8) Point {
    return .{
        .track_id = raw[0],
        .x = @as(u16, raw[1]) | (@as(u16, raw[2]) << 8),
        .y = @as(u16, raw[3]) | (@as(u16, raw[4]) << 8),
        .pressure = raw[5],
    };
}

/// Decodes min(n, max_count, 5) records and reports that count.
pub fn decodeBlock(raw: [*]const u8, n: u8, out: [*]Point, max_count: u8, got: *u8) void {
    const emit = @min(n, max_count, max_points);
    for (0..emit) |i| out[i] = decodeOne(raw + i * point_bytes);
    got.* = emit;
}

pub fn validAddr(target: u8) bool {
    return target == addr_low or target == addr_high;
}

/// 0 or anything above 5 means the full 5.
pub fn clampCap(cap: u8) u8 {
    return if (cap == 0 or cap > max_points) max_points else cap;
}

pub fn clampEmit(status: u8, max_count: u8, cap: u8) u8 {
    return @min(status & count_mask, max_count, cap);
}

/// PRODUCT_ID must start with '9' ("911").
pub fn checkProductId(bus: anytype) u16 {
    var product = [_]u8{0} ** 4;
    if (bus.read(reg_product, &product) != ok) return err_hw_init_failed;
    if (product[0] != '9') return err_hw_init_failed;
    return ok;
}

/// Status, then the point block, then the ack. A frame that is not ready
/// and a failed status read both return before the ack, as the C did.
pub fn readFrame(bus: anytype, out: [*]Point, max_count: u8, cap: u8, got: *u8) u16 {
    var status = [_]u8{0};
    if (bus.read(reg_status, &status) != ok) {
        got.* = 0;
        return err_hw_error;
    }
    if (status[0] & ready_mask == 0) {
        got.* = 0;
        return ok;
    }
    const emit = clampEmit(status[0], max_count, cap);
    if (emit > 0) {
        var raw = [_]u8{0} ** (max_points * point_bytes);
        if (bus.read(reg_point0, raw[0 .. emit * point_bytes]) != ok) {
            got.* = 0;
            _ = bus.writeByte(reg_status, cmd_clear_status);
            return err_hw_error;
        }
        decodeBlock(&raw, emit, out, max_count, got);
    } else {
        got.* = 0;
    }
    _ = bus.writeByte(reg_status, cmd_clear_status);
    return ok;
}
