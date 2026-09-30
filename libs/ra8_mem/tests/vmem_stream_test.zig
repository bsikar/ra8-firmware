// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//
// The stream adapter against a fake page cache. The fake is the point: it can
// fail a chosen frame, which is the case the real cache will not reproduce on
// demand and the case #764 was about.

const std = @import("std");
const vmem_stream = @import("vmem_stream");

const Err = vmem_stream.Err;
const Stream = vmem_stream.Stream;

const frame_bytes: u32 = 16;
const object_bytes: u64 = 100;
const object_id: u32 = 7;

/// Backing bytes: byte i is `i` mod 251, so any window has a checkable value.
var backing: [object_bytes]u8 = undefined;

/// Scratch the fake hands out as the pinned frame.
var frame: [frame_bytes]u8 = undefined;

/// Frame base that `get` refuses, or null to serve everything.
var fail_at: ?u64 = null;
/// Error `get` raises at `fail_at`.
var fail_with: Err = .invalid_state;
/// Error `put` raises, once, on the next release.
var put_fails_with: ?Err = null;
/// Frames currently pinned; the adapter promises at most one.
var pinned: i32 = 0;
var max_pinned: i32 = 0;

fn reset() void {
    for (&backing, 0..) |*byte, i| byte.* = @intCast(i % 251);
    fail_at = null;
    fail_with = .invalid_state;
    put_fails_with = null;
    pinned = 0;
    max_pinned = 0;
}

const FakeCache = struct {
    pub fn get(_: ?*vmem_stream.Vmem, id: u32, offset: u64, bytes: u32) vmem_stream.Frame {
        std.debug.assert(id == object_id);
        std.debug.assert(bytes == frame_bytes);
        std.debug.assert(offset % frame_bytes == 0);
        if (fail_at) |bad| {
            if (offset == bad) return .{ .failed = fail_with };
        }
        const end = @min(offset + frame_bytes, object_bytes);
        const span = backing[@intCast(offset)..@intCast(end)];
        @memset(&frame, 0);
        @memcpy(frame[0..span.len], span);
        pinned += 1;
        max_pinned = @max(max_pinned, pinned);
        return .{ .page = &frame };
    }

    pub fn put(_: ?*vmem_stream.Vmem, _: []const u8) Err {
        pinned -= 1;
        if (put_fails_with) |err| {
            put_fails_with = null;
            return err;
        }
        return .ok;
    }
};

fn bound() Stream {
    return .{
        .vm = null,
        .object_id = object_id,
        .frame_bytes = frame_bytes,
        .size = object_bytes,
    };
}

fn read(stream: *const Stream, offset: u64, buf: []u8) vmem_stream.Read {
    return vmem_stream.readChecked(FakeCache, stream, offset, buf);
}

test "init binds from the cache config" {
    var cfg = std.mem.zeroes(vmem_stream.Cfg);
    cfg.frame_bytes = frame_bytes;
    var vm = vmem_stream.Vmem{ .cfg = cfg };

    var stream = Stream{};
    try std.testing.expectEqual(Err.ok, vmem_stream.init(&stream, &vm, object_id, object_bytes));
    try std.testing.expectEqual(@as(u32, frame_bytes), stream.frame_bytes);
    try std.testing.expectEqual(object_bytes, stream.size);
    try std.testing.expectEqual(object_id, stream.object_id);
    try std.testing.expectEqual(&vm, stream.vm.?);
}

test "init rejects a zero size and a zero frame" {
    var cfg = std.mem.zeroes(vmem_stream.Cfg);
    cfg.frame_bytes = frame_bytes;
    var vm = vmem_stream.Vmem{ .cfg = cfg };

    var stream = Stream{};
    try std.testing.expectEqual(Err.invalid_size, vmem_stream.init(&stream, &vm, object_id, 0));
    try std.testing.expectEqual(@as(u32, 0), stream.frame_bytes);

    var zero = vmem_stream.Vmem{ .cfg = std.mem.zeroes(vmem_stream.Cfg) };
    try std.testing.expectEqual(
        Err.invalid_size,
        vmem_stream.init(&stream, &zero, object_id, object_bytes),
    );
    try std.testing.expectEqual(@as(u32, 0), stream.frame_bytes);
}

test "an unbound stream is rejected before anything is read" {
    reset();
    const stream = Stream{};
    var buf: [4]u8 = undefined;
    const got = read(&stream, 0, &buf);
    try std.testing.expectEqual(Err.invalid_state, got.err);
    try std.testing.expectEqual(@as(u32, 0), got.copied);
}

test "a zero-length request is rejected, and is checked after binding" {
    reset();
    const stream = bound();
    var buf: [0]u8 = undefined;
    const got = read(&stream, 0, &buf);
    try std.testing.expectEqual(Err.invalid_size, got.err);
    try std.testing.expectEqual(@as(u32, 0), got.copied);
}

test "a window inside one frame copies the object's bytes" {
    reset();
    const stream = bound();
    var buf: [8]u8 = undefined;
    const got = read(&stream, 4, &buf);
    try std.testing.expectEqual(Err.ok, got.err);
    try std.testing.expectEqual(@as(u32, 8), got.copied);
    try std.testing.expectEqualSlices(u8, backing[4..12], &buf);
}

test "a window spanning frames is stitched in order" {
    reset();
    const stream = bound();
    var buf: [40]u8 = undefined;
    const got = read(&stream, 10, &buf);
    try std.testing.expectEqual(Err.ok, got.err);
    try std.testing.expectEqual(@as(u32, 40), got.copied);
    try std.testing.expectEqualSlices(u8, backing[10..50], &buf);
}

test "a frame-aligned whole-object read walks every frame" {
    reset();
    const stream = bound();
    var buf: [object_bytes]u8 = undefined;
    const got = read(&stream, 0, &buf);
    try std.testing.expectEqual(Err.ok, got.err);
    try std.testing.expectEqual(@as(u32, object_bytes), got.copied);
    try std.testing.expectEqualSlices(u8, &backing, &buf);
}

test "only one frame is pinned at a time" {
    reset();
    const stream = bound();
    var buf: [object_bytes]u8 = undefined;
    _ = read(&stream, 0, &buf);
    try std.testing.expectEqual(@as(i32, 1), max_pinned);
    try std.testing.expectEqual(@as(i32, 0), pinned);
}

test "a read running past the end is a short ok, not an error" {
    reset();
    const stream = bound();
    var buf: [32]u8 = undefined;
    const got = read(&stream, object_bytes - 10, &buf);
    try std.testing.expectEqual(Err.ok, got.err);
    try std.testing.expectEqual(@as(u32, 10), got.copied);
    try std.testing.expectEqualSlices(u8, backing[object_bytes - 10 ..], buf[0..10]);
}

test "a read starting at or past the end copies nothing and does not fail" {
    reset();
    const stream = bound();
    var buf: [8]u8 = undefined;

    const at_end = read(&stream, object_bytes, &buf);
    try std.testing.expectEqual(Err.ok, at_end.err);
    try std.testing.expectEqual(@as(u32, 0), at_end.copied);

    const past = read(&stream, object_bytes + 4096, &buf);
    try std.testing.expectEqual(Err.ok, past.err);
    try std.testing.expectEqual(@as(u32, 0), past.copied);

    try std.testing.expectEqual(@as(i32, 0), max_pinned);
}

// The #764 case: a failing frame must not read as a clean end of file.
test "a failed frame reports the cache's error with the bytes copied so far" {
    reset();
    fail_at = 32;
    fail_with = .invalid_state;
    const stream = bound();

    var buf: [48]u8 = undefined;
    const got = read(&stream, 0, &buf);
    try std.testing.expectEqual(Err.invalid_state, got.err);
    try std.testing.expectEqual(@as(u32, 32), got.copied);
    try std.testing.expectEqualSlices(u8, backing[0..32], buf[0..32]);
    try std.testing.expectEqual(@as(i32, 0), pinned);
}

test "a failure on the very first frame copies nothing but still is not ok" {
    reset();
    fail_at = 0;
    const stream = bound();

    var buf: [8]u8 = undefined;
    const got = read(&stream, 0, &buf);
    try std.testing.expectEqual(Err.invalid_state, got.err);
    try std.testing.expectEqual(@as(u32, 0), got.copied);
}

test "the cache's own error code passes through unflattened" {
    reset();
    fail_at = 0;
    fail_with = Err.from(0x311);
    const stream = bound();

    var buf: [8]u8 = undefined;
    const got = read(&stream, 0, &buf);
    try std.testing.expectEqual(@as(u16, 0x311), got.err.code());
}

test "a failed release is reported, and the bytes before it are not claimed" {
    reset();
    put_fails_with = .invalid_arg;
    const stream = bound();

    var buf: [24]u8 = undefined;
    const got = read(&stream, 0, &buf);
    try std.testing.expectEqual(Err.invalid_arg, got.err);
    try std.testing.expectEqual(@as(u32, 0), got.copied);
}

test "a one-byte read at every offset returns that byte" {
    reset();
    const stream = bound();
    var offset: u64 = 0;
    while (offset < object_bytes) : (offset += 1) {
        var buf: [1]u8 = undefined;
        const got = read(&stream, offset, &buf);
        try std.testing.expectEqual(Err.ok, got.err);
        try std.testing.expectEqual(@as(u32, 1), got.copied);
        try std.testing.expectEqual(backing[@intCast(offset)], buf[0]);
    }
}

test "every window of every length reads the object's bytes" {
    reset();
    const stream = bound();
    var buf: [object_bytes]u8 = undefined;
    var offset: u64 = 0;
    while (offset < object_bytes) : (offset += 1) {
        var len: u32 = 1;
        while (len <= object_bytes) : (len += 7) {
            const got = read(&stream, offset, buf[0..len]);
            try std.testing.expectEqual(Err.ok, got.err);
            const expect = @min(@as(u64, len), object_bytes - offset);
            try std.testing.expectEqual(@as(u32, @intCast(expect)), got.copied);
            try std.testing.expectEqualSlices(
                u8,
                backing[@intCast(offset)..@intCast(offset + expect)],
                buf[0..got.copied],
            );
        }
    }
}

test "a size that is not a whole number of frames still serves its last partial frame" {
    reset();
    var stream = bound();
    stream.size = 20; // one full frame plus four bytes
    var buf: [32]u8 = undefined;
    const got = read(&stream, 0, &buf);
    try std.testing.expectEqual(Err.ok, got.err);
    try std.testing.expectEqual(@as(u32, 20), got.copied);
    try std.testing.expectEqualSlices(u8, backing[0..20], buf[0..20]);
}
