//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Malformed frames at the decoder: nothing it is fed may be read out of
//! bounds, and nothing it accepts may be anything but the canonical encoding.
//!
//! Two drivers share one property. The seeded loops run on every
//! `zig build test` and are reproducible; the `std.testing.fuzz` entry is the
//! same check under `zig build test --fuzz`, seeded with the golden frames.

const std = @import("std");
const testing = std.testing;

const rpc = @import("ra8_rpc");
const messages = @import("messages.zig");

comptime {
    _ = @import("messages.zig");
}

const Limits = struct {
    /// The largest payload any test message can have.
    const max_payload = blk: {
        var max: usize = 0;
        for (messages.cases) |case| max = @max(max, rpc.codec.maxSize(@TypeOf(case.value)));
        break :blk max;
    };
    /// Room for the longest golden frame and a tail of junk after it.
    const input = 2 * (rpc.frame.Header.bytes + max_payload);
    const rounds_per_seed = 20_000;
    const random_rounds = 50_000;
};

const corpus = blk: {
    var frames: [messages.cases.len][]const u8 = undefined;
    for (messages.cases, &frames) |case, *frame| frame.* = case.golden;
    break :blk frames;
};

/// How far an input got.
const Outcome = enum { rejected, framed, decoded };

/// The property: `in` is refused, or it is a frame lying wholly inside `in`
/// whose payload, for every message type that accepts it, is exactly what
/// that message encodes back to.
fn check(in: []const u8) !Outcome {
    const frame = rpc.frame.split(in, Limits.max_payload) catch return .rejected;
    const header = rpc.frame.Header.bytes;
    try testing.expectEqual(in.len, header + frame.payload.len + frame.rest.len);
    try testing.expectEqual(@intFromPtr(in.ptr) + header, @intFromPtr(frame.payload.ptr));

    var outcome: Outcome = .framed;
    inline for (messages.cases) |case| {
        if (try canonical(@TypeOf(case.value), frame.payload)) outcome = .decoded;
    }
    return outcome;
}

fn canonical(comptime T: type, payload: []const u8) !bool {
    const value = rpc.codec.decode(T, payload) catch return false;
    var out: [rpc.codec.maxSize(T)]u8 = undefined;
    try testing.expectEqualSlices(u8, payload, try rpc.codec.encode(T, value, &out));
    return true;
}

/// Damage a copy of `seed` one to three times: cut it short, flip a bit,
/// overwrite a byte, or append junk.
fn mutate(random: std.Random, seed: []const u8, buf: *[Limits.input]u8) []const u8 {
    @memcpy(buf[0..seed.len], seed);
    var len = seed.len;
    for (0..random.intRangeAtMost(u8, 1, 3)) |_| switch (random.uintLessThan(u8, 4)) {
        0 => len = random.uintAtMost(usize, len),
        1 => if (len > 0) {
            buf[random.uintLessThan(usize, len)] ^= @as(u8, 1) << random.int(u3);
        },
        2 => if (len > 0) {
            buf[random.uintLessThan(usize, len)] = random.int(u8);
        },
        else => {
            const extra = random.uintAtMost(usize, buf.len - len);
            random.bytes(buf[len..][0..extra]);
            len += extra;
        },
    };
    return buf[0..len];
}

test "every golden frame passes the property untouched" {
    for (corpus) |frame| try testing.expectEqual(Outcome.decoded, try check(frame));
}

test "damaged golden frames are refused or decode canonically" {
    var prng = std.Random.DefaultPrng.init(0x5241_3846_5735_3138);
    var seen = std.EnumArray(Outcome, usize).initFill(0);
    var buf: [Limits.input]u8 = undefined;
    for (corpus) |seed| {
        for (0..Limits.rounds_per_seed) |_| {
            seen.getPtr(try check(mutate(prng.random(), seed, &buf))).* += 1;
        }
    }
    // A run that only ever rejected, or only ever accepted, tested one side.
    for (seen.values) |count| try testing.expect(count > 0);
}

test "random bytes are refused or decode canonically" {
    var prng = std.Random.DefaultPrng.init(0x0518);
    var buf: [Limits.input]u8 = undefined;
    for (0..Limits.random_rounds) |_| {
        const in = buf[0..prng.random().uintAtMost(usize, buf.len)];
        prng.random().bytes(in);
        _ = try check(in);
    }
}

test "fuzz: frames are refused or decode canonically" {
    const one = struct {
        fn one(_: void, input: []const u8) anyerror!void {
            _ = try check(input);
        }
    }.one;
    try testing.fuzz({}, one, .{ .corpus = &corpus });
}
