//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the object-source registry. The backing read callback is a
//! plain Zig function with the C calling convention, so the whole layer,
//! registry and loader alike, runs here with no C in the link.

const std = @import("std");

const vsource = @import("vsource");

const Err = vsource.Err;

const store_bytes = 256;

/// A paged backing that fills each byte with `offset & 0xFF`, so a caller can
/// tell exactly which absolute offset every byte came from.
const Store = struct {
    var reads: u32 = 0;
    var fail_with: u16 = 0;

    fn read(_: ?*anyopaque, offset: u64, buf: [*]u8, len: u32) callconv(.c) u16 {
        reads += 1;
        if (fail_with != 0) return fail_with;
        if (offset + len > store_bytes) return Err.out_of_range.code();
        for (buf[0..len], 0..) |*b, i| b.* = @truncate(offset + i);
        return Err.ok.code();
    }

    fn reset() void {
        reads = 0;
        fail_with = 0;
    }
};

fn xipBytes() [store_bytes]u8 {
    var bytes: [store_bytes]u8 = undefined;
    for (&bytes, 0..) |*b, i| b.* = @truncate(i);
    return bytes;
}

test "init rejects an empty array and leaves the registry unbound" {
    var vs: vsource.Registry = .{};
    try std.testing.expectEqual(Err.invalid_size, vsource.init(&vs, &.{}));
    try std.testing.expectEqual(@as(?[*]vsource.Obj, null), vs.objs);
    try std.testing.expectEqual(@as(u32, 0), vs.count);
}

test "ids are handed out in registration order" {
    var objs: [3]vsource.Obj = undefined;
    var vs: vsource.Registry = .{};
    try std.testing.expectEqual(Err.ok, vsource.init(&vs, &objs));

    var first: u32 = 0xFFFF;
    var second: u32 = 0xFFFF;
    try std.testing.expectEqual(Err.ok, vsource.addPaged(&vs, Store.read, null, 0, 100, &first));
    const xip = xipBytes();
    try std.testing.expectEqual(Err.ok, vsource.addXip(&vs, &xip, store_bytes, &second));
    try std.testing.expectEqual(@as(u32, 0), first);
    try std.testing.expectEqual(@as(u32, 1), second);
    try std.testing.expectEqual(@as(u32, 2), vs.count);
}

test "a zero-size object is refused and the registry is unchanged" {
    var objs: [2]vsource.Obj = undefined;
    var vs: vsource.Registry = .{};
    _ = vsource.init(&vs, &objs);

    var id: u32 = 0;
    try std.testing.expectEqual(Err.invalid_size, vsource.addPaged(&vs, Store.read, null, 0, 0, &id));
    const xip = xipBytes();
    try std.testing.expectEqual(Err.invalid_size, vsource.addXip(&vs, &xip, 0, &id));
    try std.testing.expectEqual(@as(u32, 0), vs.count);
}

test "a full registry reports no_mem" {
    var objs: [1]vsource.Obj = undefined;
    var vs: vsource.Registry = .{};
    _ = vsource.init(&vs, &objs);

    var id: u32 = 0;
    try std.testing.expectEqual(Err.ok, vsource.addPaged(&vs, Store.read, null, 0, 8, &id));
    try std.testing.expectEqual(Err.no_mem, vsource.addPaged(&vs, Store.read, null, 0, 8, &id));
    try std.testing.expectEqual(@as(u32, 1), vs.count);
}

test "an unbound registry answers out_of_range rather than dereferencing" {
    const vs: vsource.Registry = .{};
    var frame: [16]u8 = undefined;
    try std.testing.expectEqual(Err.out_of_range, vsource.load(&vs, 0, 0, &frame));
    try std.testing.expectEqual(Err.out_of_range, vsource.xipPtr(&vs, 0, 0, 4).failed);
}

test "an unknown id is out_of_range, not a read of slot zero" {
    var objs: [2]vsource.Obj = undefined;
    var vs: vsource.Registry = .{};
    _ = vsource.init(&vs, &objs);
    var id: u32 = 0;
    _ = vsource.addPaged(&vs, Store.read, null, 0, 100, &id);
    Store.reset();

    var frame: [16]u8 = undefined;
    try std.testing.expectEqual(Err.out_of_range, vsource.load(&vs, 9, 0, &frame));
    try std.testing.expectEqual(@as(u32, 0), Store.reads);
}

test "a paged frame carries the object's absolute offsets" {
    var objs: [1]vsource.Obj = undefined;
    var vs: vsource.Registry = .{};
    _ = vsource.init(&vs, &objs);
    var id: u32 = 0;
    _ = vsource.addPaged(&vs, Store.read, null, 32, 100, &id);
    Store.reset();

    var frame: [16]u8 = undefined;
    try std.testing.expectEqual(Err.ok, vsource.load(&vs, id, 64, &frame));
    // base 32 + offset 64 == 96, so the frame opens at 96.
    try std.testing.expectEqual(@as(u8, 96), frame[0]);
    try std.testing.expectEqual(@as(u8, 96 + 15), frame[15]);
    try std.testing.expectEqual(@as(u32, 1), Store.reads);
}

test "the tail past the object end reads back as zeros" {
    var objs: [1]vsource.Obj = undefined;
    var vs: vsource.Registry = .{};
    _ = vsource.init(&vs, &objs);
    var id: u32 = 0;
    _ = vsource.addPaged(&vs, Store.read, null, 0, 100, &id);
    Store.reset();

    var frame: [64]u8 = undefined;
    @memset(&frame, 0xAA);
    // 36 bytes of object left at offset 64, so the last 28 are padding.
    try std.testing.expectEqual(Err.ok, vsource.load(&vs, id, 64, &frame));
    try std.testing.expectEqual(@as(u8, 64), frame[0]);
    try std.testing.expectEqual(@as(u8, 99), frame[35]);
    for (frame[36..]) |b| try std.testing.expectEqual(@as(u8, 0), b);
}

test "offset at or past the object end is out_of_range" {
    var objs: [1]vsource.Obj = undefined;
    var vs: vsource.Registry = .{};
    _ = vsource.init(&vs, &objs);
    var id: u32 = 0;
    _ = vsource.addPaged(&vs, Store.read, null, 0, 100, &id);

    var frame: [16]u8 = undefined;
    try std.testing.expectEqual(Err.out_of_range, vsource.load(&vs, id, 100, &frame));
    try std.testing.expectEqual(Err.out_of_range, vsource.load(&vs, id, 1000, &frame));
}

test "the backing's own error code reaches the caller unflattened" {
    var objs: [1]vsource.Obj = undefined;
    var vs: vsource.Registry = .{};
    _ = vsource.init(&vs, &objs);
    var id: u32 = 0;
    _ = vsource.addPaged(&vs, Store.read, null, 0, 100, &id);
    Store.reset();
    Store.fail_with = 0x311;

    var frame: [16]u8 = undefined;
    try std.testing.expectEqual(Err.from(0x311), vsource.load(&vs, id, 0, &frame));
    Store.reset();
}

test "an xip frame is a copy of the mapped bytes, no callback involved" {
    var objs: [1]vsource.Obj = undefined;
    var vs: vsource.Registry = .{};
    _ = vsource.init(&vs, &objs);
    const xip = xipBytes();
    var id: u32 = 0;
    _ = vsource.addXip(&vs, &xip, store_bytes, &id);
    Store.reset();

    var frame: [16]u8 = undefined;
    try std.testing.expectEqual(Err.ok, vsource.load(&vs, id, 32, &frame));
    try std.testing.expectEqualSlices(u8, xip[32..48], &frame);
    try std.testing.expectEqual(@as(u32, 0), Store.reads);
}

test "xipPtr hands back the mapped address itself" {
    var objs: [1]vsource.Obj = undefined;
    var vs: vsource.Registry = .{};
    _ = vsource.init(&vs, &objs);
    const xip = xipBytes();
    var id: u32 = 0;
    _ = vsource.addXip(&vs, &xip, store_bytes, &id);

    const got = vsource.xipPtr(&vs, id, 32, 16);
    try std.testing.expectEqual(@as([*]const u8, @ptrCast(&xip[32])), got.ptr);
}

test "xipPtr refuses a paged object and a span past the end" {
    var objs: [2]vsource.Obj = undefined;
    var vs: vsource.Registry = .{};
    _ = vsource.init(&vs, &objs);
    var paged: u32 = 0;
    var mapped: u32 = 0;
    _ = vsource.addPaged(&vs, Store.read, null, 0, 100, &paged);
    const xip = xipBytes();
    _ = vsource.addXip(&vs, &xip, store_bytes, &mapped);

    try std.testing.expectEqual(Err.not_supported, vsource.xipPtr(&vs, paged, 0, 4).failed);
    try std.testing.expectEqual(Err.out_of_range, vsource.xipPtr(&vs, mapped, 250, 16).failed);
    try std.testing.expectEqual(Err.out_of_range, vsource.xipPtr(&vs, mapped, store_bytes, 0).failed);
    // The last byte of the object is still in range.
    var last: [1]u8 = .{0};
    try std.testing.expectEqual(Err.ok, vsource.load(&vs, mapped, store_bytes - 1, &last));
    try std.testing.expectEqual(@as(u8, store_bytes - 1), last[0]);
}

test "an object with neither backing is an error, not a null call" {
    // A forged registry: the C would have called through a null read pointer.
    var objs = [_]vsource.Obj{.{ .size = 64 }};
    const vs: vsource.Registry = .{ .objs = &objs, .cap = 1, .count = 1 };

    var frame: [16]u8 = undefined;
    try std.testing.expectEqual(Err.invalid_state, vsource.load(&vs, 0, 0, &frame));
}
