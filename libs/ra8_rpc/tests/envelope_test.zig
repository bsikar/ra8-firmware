//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The numbers the envelope puts on the wire, pinned.

const std = @import("std");
const testing = std.testing;

const rpc = @import("ra8_rpc");
const messages = @import("messages.zig");
const Env = messages.Env;

comptime {
    _ = @import("messages.zig");
    _ = @import("mock_queue.zig");
    _ = @import("service.zig");
}

test "the frame kinds keep their numbers" {
    try testing.expectEqual(@as(u16, 1), rpc.Kind.hello);
    try testing.expectEqual(@as(u16, 2), rpc.Kind.request);
    try testing.expectEqual(@as(u16, 3), rpc.Kind.response);
    try testing.expectEqual(@as(u16, 4), rpc.Kind.event);
    try testing.expectEqual(@as(u16, 5), rpc.Kind.fault);
}

test "the refusal codes keep their numbers" {
    const Code = rpc.Code;
    try testing.expectEqual(@as(u16, 1), @intFromEnum(Code.unknown_method));
    try testing.expectEqual(@as(u16, 2), @intFromEnum(Code.bad_args));
    try testing.expectEqual(@as(u16, 3), @intFromEnum(Code.failed));
    try testing.expectEqual(@as(u16, 4), @intFromEnum(Code.version_mismatch));
    try testing.expectEqual(@as(u16, 5), @intFromEnum(Code.bad_magic));
    try testing.expectEqual(@as(u16, 6), @intFromEnum(Code.not_ready));
    try testing.expectEqual(@as(u16, 0x0100), Code.first_app);
}

test "the magic is the bytes RA8R and the version is one" {
    var bytes: [4]u8 = undefined;
    std.mem.writeInt(u32, &bytes, rpc.Protocol.magic, .little);
    try testing.expectEqualSlices(u8, "RA8R", &bytes);
    try testing.expectEqual(@as(u16, 1), rpc.Protocol.version);
}

test "a hello is accepted only with this magic and this version" {
    try (rpc.Hello{ .caps = 0 }).check();
    const magic: rpc.Hello = .{ .magic = rpc.Protocol.magic + 1, .caps = 0 };
    try testing.expectError(error.BadMagic, magic.check());
    const version: rpc.Hello = .{ .version = rpc.Protocol.version + 1, .caps = 0 };
    try testing.expectError(error.VersionMismatch, version.check());
}

test "the largest frame is a request with a full body" {
    const header = rpc.frame.Header.bytes;
    try testing.expectEqual(@as(usize, header + 4 + 2 + 4 + 32), Env.max_frame);
    try testing.expectEqual(@as(usize, 4 + 1 + 4 + 32), rpc.codec.maxSize(Env.Response));
    try testing.expectEqual(@as(usize, 2 + 4 + 32), rpc.codec.maxSize(Env.Event));
}

test "a response result is one byte of tag, zero for ok and one for err" {
    var out: [rpc.codec.maxSize(Env.Response)]u8 = undefined;
    const ok = try rpc.codec.encode(Env.Response, .{ .id = 0, .result = .{ .ok = "" } }, &out);
    try testing.expectEqual(@as(u8, 0), ok[4]);
    const err: Env.Response = .{ .id = 0, .result = .{ .err = .failed } };
    try testing.expectEqual(@as(u8, 1), (try rpc.codec.encode(Env.Response, err, &out))[4]);
}
