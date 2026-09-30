//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! End-to-end tests for the ABI membrane. This root defines the library's two
//! link-time seams itself, so the exported bridge is driven for real: the
//! codec and the sink here ARE the symbols the archive resolves against.

const std = @import("std");
const bridge = @import("bridge");

const Frame = bridge.Frame;
const Buffer = bridge.Buffer;

/// What the fake codec publishes and what the fake sink does, rewritten per
/// test. A single instance because the seams are link-time symbols.
const Seam = struct {
    encode_err: u16 = 0,
    frame: Frame = .{},
    write_err: u16 = 0,
    /// Bytes the sink claims to have accepted, or null to accept everything.
    accept: ?u32 = null,

    saw_write: bool = false,
    saw_ptr: ?[*]const u8 = null,
    saw_len: u32 = 0,
    codec_calls: u32 = 0,
};

var seam: Seam = .{};

export fn ra8_camera_codec_encode(
    codec: ?*anyopaque,
    input: ?*const Frame,
    output_buffer: ?*const Buffer,
    out_frame: ?*Frame,
) callconv(.c) u16 {
    _ = codec;
    _ = input;
    _ = output_buffer;
    seam.codec_calls += 1;
    if (seam.encode_err != 0) return seam.encode_err;
    if (out_frame) |slot| slot.* = seam.frame;
    return 0;
}

export fn ra8_io_stream_write(
    stream: ?*anyopaque,
    buf: ?[*]const u8,
    len: u32,
    out_written: ?*u32,
) callconv(.c) u16 {
    _ = stream;
    seam.saw_write = true;
    seam.saw_ptr = buf;
    seam.saw_len = len;
    if (out_written) |slot| slot.* = seam.accept orelse len;
    return seam.write_err;
}

var sink_handle: u32 = 0;
const sink: *anyopaque = @ptrCast(&sink_handle);

fn call(out_written: ?*u32) u16 {
    return bridge.ra8_camera_codec_encode_to_stream(null, null, null, sink, out_written);
}

const payload = [_]u8{ 1, 2, 3, 4, 5, 6, 7, 8 };

fn framed(bytes: u32) Frame {
    return .{ .data = &payload, .bytes = bytes, .width = 2, .height = 2 };
}

test "a whole frame through both seams is ok and reports its count" {
    seam = .{ .frame = framed(payload.len) };
    var written: u32 = 0xDEAD;
    try std.testing.expectEqual(@as(u16, 0), call(&written));
    try std.testing.expectEqual(@as(u32, payload.len), written);
    try std.testing.expectEqual(@as(u32, payload.len), seam.saw_len);
    try std.testing.expect(seam.saw_ptr == @as([*]const u8, &payload));
}

test "a null sink returns null_ptr and leaves the counter untouched" {
    seam = .{ .frame = framed(payload.len) };
    var written: u32 = 0xDEAD;
    const rc = bridge.ra8_camera_codec_encode_to_stream(null, null, null, null, &written);
    try std.testing.expectEqual(@as(u16, 0x504), rc);
    try std.testing.expectEqual(@as(u32, 0xDEAD), written);
    try std.testing.expectEqual(@as(u32, 0), seam.codec_calls);
    try std.testing.expect(!seam.saw_write);
}

test "a codec fault is forwarded verbatim, never reaches the sink, and zeroes the counter" {
    seam = .{ .encode_err = 0x207 };
    var written: u32 = 0xDEAD;
    try std.testing.expectEqual(@as(u16, 0x207), call(&written));
    try std.testing.expectEqual(@as(u32, 0), written);
    try std.testing.expect(!seam.saw_write);
}

test "a sink error wins, and the counter still reports what it took" {
    seam = .{ .frame = framed(payload.len), .write_err = 0x301, .accept = 3 };
    var written: u32 = 0;
    try std.testing.expectEqual(@as(u16, 0x301), call(&written));
    try std.testing.expectEqual(@as(u32, 3), written);
}

test "a nominally successful short write becomes invalid_size" {
    seam = .{ .frame = framed(payload.len), .accept = 3 };
    var written: u32 = 0;
    try std.testing.expectEqual(@as(u16, 0x105), call(&written));
    try std.testing.expectEqual(@as(u32, 3), written);
}

test "a zero-byte frame is forwarded as a zero-length request, null pointer included" {
    seam = .{ .frame = .{ .data = null, .bytes = 0 } };
    try std.testing.expectEqual(@as(u16, 0), call(null));
    try std.testing.expect(seam.saw_write);
    try std.testing.expect(seam.saw_ptr == null);
    try std.testing.expectEqual(@as(u32, 0), seam.saw_len);
}

test "a null out_written is accepted on every path" {
    seam = .{ .frame = framed(payload.len) };
    try std.testing.expectEqual(@as(u16, 0), call(null));
    seam = .{ .encode_err = 0x207 };
    try std.testing.expectEqual(@as(u16, 0x207), call(null));
    seam = .{ .frame = framed(payload.len), .accept = 1 };
    try std.testing.expectEqual(@as(u16, 0x105), call(null));
}
