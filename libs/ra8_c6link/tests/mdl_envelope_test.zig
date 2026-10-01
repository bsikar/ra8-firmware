//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the CustomRpc envelope rules.

const std = @import("std");
const envelope = @import("implementation").mdl_envelope;

const Bound = envelope.Bound;
const Kind = envelope.Kind;
const ResponseView = envelope.ResponseView;

fn reply(operation: u32, msg_id: u32, len: usize, present: bool) ResponseView {
    return .{
        .custom_msg_id = msg_id,
        .operation = operation,
        .body_len = len,
        .body_present = present,
    };
}

fn good(operation: u32) ResponseView {
    return reply(operation, operation, 16, true);
}

test "the three media operations have distinct stable ids" {
    try std.testing.expectEqual(@as(u32, 0x4D44_0301), Bound.rpc_start);
    try std.testing.expectEqual(@as(u32, 0x4D44_0302), Bound.rpc_next);
    try std.testing.expectEqual(@as(u32, 0x4D44_0303), Bound.rpc_cancel);
    try std.testing.expect(Bound.rpc_start != Bound.rpc_next);
    try std.testing.expect(Bound.rpc_next != Bound.rpc_cancel);
}

test "each operation expects its own inner response" {
    try std.testing.expectEqual(Kind.accepted, envelope.kindFor(Bound.rpc_start).?);
    try std.testing.expectEqual(Kind.chunk, envelope.kindFor(Bound.rpc_next).?);
    try std.testing.expectEqual(Kind.cancelled, envelope.kindFor(Bound.rpc_cancel).?);
}

test "kind values match the C extractor variants" {
    try std.testing.expectEqual(@as(u8, 1), @intFromEnum(Kind.accepted));
    try std.testing.expectEqual(@as(u8, 2), @intFromEnum(Kind.chunk));
    try std.testing.expectEqual(@as(u8, 3), @intFromEnum(Kind.cancelled));
}

test "an id from another protocol family names no media operation" {
    try std.testing.expect(envelope.kindFor(0) == null);
    try std.testing.expect(envelope.kindFor(0x4D44_0300) == null);
    try std.testing.expect(envelope.kindFor(0x4D44_0304) == null);
    try std.testing.expect(envelope.kindFor(std.math.maxInt(u32)) == null);
    try std.testing.expect(!envelope.operationValid(0x4D44_0304));
}

test "operationValid agrees with kindFor across the family boundary" {
    var op: u32 = 0x4D44_02FE;
    while (op <= 0x4D44_0306) : (op += 1) {
        try std.testing.expectEqual(envelope.kindFor(op) != null, envelope.operationValid(op));
    }
}

test "a well formed reply to each operation is accepted as its own kind" {
    try std.testing.expectEqual(Kind.accepted, envelope.accept(&good(Bound.rpc_start)).?);
    try std.testing.expectEqual(Kind.chunk, envelope.accept(&good(Bound.rpc_next)).?);
    try std.testing.expectEqual(Kind.cancelled, envelope.accept(&good(Bound.rpc_cancel)).?);
}

test "a reply naming a different operation is refused" {
    const crossed = reply(Bound.rpc_next, Bound.rpc_start, 16, true);
    try std.testing.expect(!envelope.responseCorrelates(&crossed));
    try std.testing.expect(envelope.accept(&crossed) == null);
}

test "every crossed pair of media operations is refused" {
    const ops = [_]u32{ Bound.rpc_start, Bound.rpc_next, Bound.rpc_cancel };
    for (ops) |asked| {
        for (ops) |named| {
            const view = reply(asked, named, 16, true);
            try std.testing.expectEqual(asked == named, envelope.responseCorrelates(&view));
        }
    }
}

test "a call on an unknown operation cannot be satisfied by any reply" {
    const view = reply(0x4D44_0304, 0x4D44_0304, 16, true);
    try std.testing.expect(!envelope.responseCorrelates(&view));
    try std.testing.expect(envelope.accept(&view) == null);
}

test "an absent body is refused" {
    const view = reply(Bound.rpc_next, Bound.rpc_next, 0, false);
    try std.testing.expect(!envelope.bodyPresent(&view));
    try std.testing.expect(envelope.accept(&view) == null);
}

test "a present but zero length body is refused like an absent one" {
    const empty = reply(Bound.rpc_next, Bound.rpc_next, 0, true);
    const absent = reply(Bound.rpc_next, Bound.rpc_next, 0, false);
    try std.testing.expectEqual(envelope.bodyPresent(&absent), envelope.bodyPresent(&empty));
    try std.testing.expect(envelope.accept(&empty) == null);
}

test "a length without a pointer is refused" {
    const view = reply(Bound.rpc_start, Bound.rpc_start, 64, false);
    try std.testing.expect(!envelope.bodyPresent(&view));
    try std.testing.expect(envelope.accept(&view) == null);
}

test "one body byte is enough" {
    const view = reply(Bound.rpc_start, Bound.rpc_start, 1, true);
    try std.testing.expect(envelope.bodyPresent(&view));
    try std.testing.expectEqual(Kind.accepted, envelope.accept(&view).?);
}

test "identity is checked before the body" {
    const crossed_and_empty = reply(Bound.rpc_start, Bound.rpc_cancel, 0, false);
    try std.testing.expect(envelope.accept(&crossed_and_empty) == null);
}

test "accept never returns a kind the operation does not expect" {
    const ops = [_]u32{ Bound.rpc_start, Bound.rpc_next, Bound.rpc_cancel };
    for (ops) |op| {
        const kind = envelope.accept(&good(op)).?;
        try std.testing.expectEqual(envelope.kindFor(op).?, kind);
    }
}

test "a defaulted view is refused" {
    const view: ResponseView = .{};
    try std.testing.expect(!envelope.responseCorrelates(&view));
    try std.testing.expect(!envelope.bodyPresent(&view));
    try std.testing.expect(envelope.accept(&view) == null);
}

test "a huge body is still a body" {
    const view = reply(Bound.rpc_next, Bound.rpc_next, std.math.maxInt(usize), true);
    try std.testing.expectEqual(Kind.chunk, envelope.accept(&view).?);
}
