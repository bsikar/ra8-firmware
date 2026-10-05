//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ra8_io_stream_ram_init/used and the bound write against fake ra8_log and
//! ra8_io_stream entry points (RA8FW-698).

const std = @import("std");
const io = @import("ra8_io");
const ram = io.stream_ram;
const Stream = io.log.Stream;

var errors_logged: u32 = 0;
var bound_iface: ?*const ram.Iface = null;
var bound_ctx: ?*anyopaque = null;

export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {
    errors_logged += 1;
}

export fn ra8_io_stream_bind(_: *Stream, iface: *const ram.Iface, context: ?*anyopaque) c_int {
    bound_iface = iface;
    bound_ctx = context;
    return ram.ok;
}

// The log unit in the same archive needs these to link; unused here.
export fn ra8_log_set_byte_sink(_: ?io.log.ByteSink, _: ?*anyopaque) void {}
export fn ra8_io_stream_write(_: *Stream, _: [*]const u8, _: u32, _: ?*u32) c_int {
    return ram.ok;
}

// The uart unit in the same archive needs these to link; unused here.
export fn ra8_sci_write_polling(_: u8, _: [*]const u8, _: u32) c_int {
    return 0;
}
export fn ra8_sci_flush(_: u8) c_int {
    return 0;
}

extern fn ra8_io_stream_ram_init(s: ?*Stream, state: ?*ram.State, buf: ?[*]u8, cap: u32) c_int;
extern fn ra8_io_stream_ram_used(state: ?*const ram.State, out_used: ?*u32) c_int;

fn reset() void {
    errors_logged = 0;
    bound_iface = null;
    bound_ctx = null;
}

fn write(bytes: []const u8, out: ?*u32) c_int {
    return bound_iface.?.write.?(bound_ctx, bytes.ptr, @intCast(bytes.len), out);
}

test "init rejects each null argument and logs once per call" {
    reset();
    var s = Stream{ .iface = null, .ctx = null };
    var st: ram.State = undefined;
    var buf: [4]u8 = undefined;
    try std.testing.expectEqual(ram.err_null_ptr, ra8_io_stream_ram_init(null, &st, &buf, 4));
    try std.testing.expectEqual(ram.err_null_ptr, ra8_io_stream_ram_init(&s, null, &buf, 4));
    try std.testing.expectEqual(ram.err_null_ptr, ra8_io_stream_ram_init(&s, &st, null, 4));
    try std.testing.expectEqual(@as(u32, 3), errors_logged);
    try std.testing.expect(bound_iface == null);
}

test "init rejects a zero capacity without logging" {
    reset();
    var s = Stream{ .iface = null, .ctx = null };
    var st: ram.State = undefined;
    var buf: [4]u8 = undefined;
    try std.testing.expectEqual(ram.err_invalid_size, ra8_io_stream_ram_init(&s, &st, &buf, 0));
    try std.testing.expectEqual(@as(u32, 0), errors_logged);
}

test "init binds the ram vtable with the state as context" {
    reset();
    var s = Stream{ .iface = null, .ctx = null };
    var st: ram.State = undefined;
    var buf: [4]u8 = undefined;
    try std.testing.expectEqual(ram.ok, ra8_io_stream_ram_init(&s, &st, &buf, 4));
    try std.testing.expect(bound_iface == &ram.iface);
    try std.testing.expect(bound_ctx == @as(?*anyopaque, &st));
    try std.testing.expect(ram.iface.flush == null);
    try std.testing.expectEqual(@as(u32, 0), st.len);
}

test "writes append and used reports the count" {
    reset();
    var s = Stream{ .iface = null, .ctx = null };
    var st: ram.State = undefined;
    var buf: [8]u8 = undefined;
    _ = ra8_io_stream_ram_init(&s, &st, &buf, 8);
    var n: u32 = 0;
    try std.testing.expectEqual(ram.ok, write("ab", &n));
    try std.testing.expectEqual(ram.ok, write("cd", null));
    var used: u32 = 0;
    try std.testing.expectEqual(ram.ok, ra8_io_stream_ram_used(&st, &used));
    try std.testing.expectEqual(@as(u32, 2), n);
    try std.testing.expectEqual(@as(u32, 4), used);
    try std.testing.expectEqualSlices(u8, "abcd", buf[0..4]);
}

test "a write past capacity keeps what fits and returns no_mem" {
    reset();
    var s = Stream{ .iface = null, .ctx = null };
    var st: ram.State = undefined;
    var buf: [3]u8 = undefined;
    _ = ra8_io_stream_ram_init(&s, &st, &buf, 3);
    var n: u32 = 99;
    try std.testing.expectEqual(ram.err_no_mem, write("wxyz", &n));
    try std.testing.expectEqual(@as(u32, 3), n);
    try std.testing.expectEqualSlices(u8, "wxy", &buf);
    try std.testing.expectEqual(ram.err_no_mem, write("q", &n));
    try std.testing.expectEqual(@as(u32, 0), n);
}

test "write rejects a null context or buffer" {
    reset();
    var st = ram.State{ .buf = null, .cap = 0, .len = 0 };
    try std.testing.expectEqual(ram.err_null_ptr, ram.iface.write.?(null, "a", 1, null));
    try std.testing.expectEqual(ram.err_null_ptr, ram.iface.write.?(&st, null, 1, null));
    try std.testing.expectEqual(@as(u32, 2), errors_logged);
}

test "used rejects null arguments" {
    reset();
    const st = ram.State{ .buf = null, .cap = 0, .len = 0 };
    var used: u32 = 0;
    try std.testing.expectEqual(ram.err_null_ptr, ra8_io_stream_ram_used(null, &used));
    try std.testing.expectEqual(ram.err_null_ptr, ra8_io_stream_ram_used(&st, null));
    try std.testing.expectEqual(@as(u32, 2), errors_logged);
}
