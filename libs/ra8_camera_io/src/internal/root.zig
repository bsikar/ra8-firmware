//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure core of the camera-to-stream bridge: the C ABI record layouts the
//! bridge passes through, and the one decision the bridge actually makes.
//!
//! No externs and no exported symbols, so the whole file is host-testable.
//! The codec and the sink are link-time seams the membrane owns.

const std = @import("std");

/// `ra8_err_t` values this bridge can produce on its own. Anything else the
/// codec or the sink returns is forwarded untouched.
pub const err = struct {
    pub const ok: u16 = 0;
    pub const invalid_size: u16 = 0x105;
    pub const null_ptr: u16 = 0x504;
};

/// `ra8_camera_format_t`, an explicitly-sized C enum.
pub const Format = enum(u8) {
    rgb888 = 0,
    uyvy422 = 1,
    jpeg = 2,
    _,
};

/// `ra8_camera_buffer_t`: caller-owned writable storage the codec may use.
pub const Buffer = extern struct {
    data: ?[*]u8 = null,
    capacity: u32 = 0,
};

/// `ra8_camera_frame_t`: immutable view of one captured or encoded image.
/// `data` is borrowed and may alias the input for a passthrough codec.
pub const Frame = extern struct {
    data: ?[*]const u8 = null,
    bytes: u32 = 0,
    stride_bytes: u32 = 0,
    width: u16 = 0,
    height: u16 = 0,
    format: Format = .rgb888,
};

const ptr_bytes = @sizeOf(usize);

// Both records lead with a pointer, so every later field sits at the offset
// the C compiler derives from the same alignment rule. Spelling the offsets
// as `alignForward` of the running size makes the 32-bit Arm layout follow
// from the same expressions the host checks.
comptime {
    const al = std.mem.alignForward;
    std.debug.assert(@offsetOf(Buffer, "data") == 0);
    std.debug.assert(@offsetOf(Buffer, "capacity") == al(usize, ptr_bytes, 4));
    std.debug.assert(@sizeOf(Buffer) == al(usize, ptr_bytes + 4, ptr_bytes));

    std.debug.assert(@offsetOf(Frame, "data") == 0);
    std.debug.assert(@offsetOf(Frame, "bytes") == al(usize, ptr_bytes, 4));
    std.debug.assert(@offsetOf(Frame, "stride_bytes") == @offsetOf(Frame, "bytes") + 4);
    std.debug.assert(@offsetOf(Frame, "width") == @offsetOf(Frame, "stride_bytes") + 4);
    std.debug.assert(@offsetOf(Frame, "height") == @offsetOf(Frame, "width") + 2);
    std.debug.assert(@offsetOf(Frame, "format") == @offsetOf(Frame, "height") + 2);
    std.debug.assert(@sizeOf(Format) == 1);
}

/// What the bridge returns once the sink has answered.
///
/// The sink's own error wins: `*out_written` still reports its count, but a
/// short prefix only becomes this library's `invalid_size` when the sink
/// claimed success. A sink that errors has already said why.
pub fn writeOutcome(stream_err: u16, written: u32, frame_bytes: u32) u16 {
    if (stream_err != err.ok) return stream_err;
    return if (written == frame_bytes) err.ok else err.invalid_size;
}

test "a successful sink that took the whole frame is ok" {
    try std.testing.expectEqual(err.ok, writeOutcome(err.ok, 128, 128));
}

test "a successful sink that took a prefix is the bridge's own invalid_size" {
    try std.testing.expectEqual(err.invalid_size, writeOutcome(err.ok, 64, 128));
}

test "a zero-byte frame the sink accepted is ok, not a short write" {
    try std.testing.expectEqual(err.ok, writeOutcome(err.ok, 0, 0));
}

test "the sink's error wins over a short count" {
    try std.testing.expectEqual(@as(u16, 0x300), writeOutcome(0x300, 0, 128));
}

test "the sink's error wins even when the count is complete" {
    try std.testing.expectEqual(@as(u16, 0x301), writeOutcome(0x301, 128, 128));
}

test "a sink that overran is not ok either" {
    try std.testing.expectEqual(err.invalid_size, writeOutcome(err.ok, 200, 128));
}
