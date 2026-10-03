//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Every test message against its checked-in frame, in both directions.

const std = @import("std");
const testing = std.testing;

const rpc = @import("ra8_rpc");
const messages = @import("messages.zig");

comptime {
    _ = @import("messages.zig");
}

test "every message encodes to its golden frame, byte for byte" {
    inline for (messages.cases) |case| {
        const T = @TypeOf(case.value);
        var out: [rpc.frame.maxSize(T)]u8 = undefined;
        const frame = try rpc.frame.encode(T, case.kind, case.value, &out);
        try testing.expectEqualSlices(u8, case.golden, frame);
    }
}

test "every golden frame decodes to its message" {
    inline for (messages.cases) |case| {
        const T = @TypeOf(case.value);
        const frame = try rpc.frame.split(case.golden, rpc.codec.maxSize(T));
        try testing.expectEqual(case.kind, frame.kind);
        try testing.expectEqual(@as(usize, 0), frame.rest.len);
        try testing.expectEqualDeep(case.value, try rpc.codec.decode(T, frame.payload));
    }
}

test "a decoded golden frame encodes back to the same bytes" {
    inline for (messages.cases) |case| {
        const T = @TypeOf(case.value);
        const frame = try rpc.frame.split(case.golden, rpc.codec.maxSize(T));
        const value = try rpc.codec.decode(T, frame.payload);

        var out: [rpc.frame.maxSize(T)]u8 = undefined;
        const again = try rpc.frame.encode(T, frame.kind, value, &out);
        try testing.expectEqualSlices(u8, case.golden, again);
    }
}

test "the fixtures hold at least one frame per shape of field" {
    try testing.expectEqual(@as(usize, 6), messages.cases.len);
}
