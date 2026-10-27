//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the stub entropy profile and the ops that serve it.

const std = @import("std");
const stub = @import("rng_stub");
const abi = @import("rng_stub_abi");

const Err = abi.Err;

/// Stand-in for the facade's `fw_rng_bind`, which lives in `libs/if` and is
/// linked by the composition root, not by this unit's host tests. It records
/// what the bind convenience handed it so the bind path is testable without
/// the real facade.
var bind_calls: usize = 0;
var last_iface: ?*const abi.Iface = null;

pub export fn fw_rng_bind(rng: ?*abi.Handle, iface: ?*const abi.Iface, ctx: ?*anyopaque) callconv(.c) u32 {
    bind_calls += 1;
    last_iface = iface;

    if (rng) |h| {
        h.iface = iface;
        h.ctx = ctx;
        h.bound = true;
    }

    return Err.ok;
}

test "profile's first fill is the count-zero pattern" {
    stub.reset();

    var got: [16]u8 = undefined;
    stub.fill(&got);

    for (&got, 0..) |byte, i| {
        try std.testing.expectEqual(@as(u8, @truncate(i)), byte);
    }
}

test "profile's consecutive fills differ" {
    stub.reset();

    var first: [8]u8 = undefined;
    var second: [8]u8 = undefined;
    stub.fill(&first);
    stub.fill(&second);

    for (&first, 0..) |byte, i| {
        try std.testing.expectEqual(@as(u8, @truncate(i)), byte);
        try std.testing.expectEqual(@as(u8, @truncate(i + 1)), second[i]);
    }
    try std.testing.expect(!std.mem.eql(u8, &first, &second));
}

test "profile always reports ready" {
    stub.reset();
    try std.testing.expect(stub.ready());
}

test "a zero-length fill costs the profile nothing" {
    stub.reset();

    const before = stub.fills();
    stub.fill(&.{});

    try std.testing.expectEqual(before, stub.fills());
}

test "reset restores the count-zero pattern" {
    stub.reset();

    var once: [4]u8 = undefined;
    stub.fill(&once);
    stub.fill(&once);
    stub.reset();
    stub.fill(&once);

    for (&once, 0..) |byte, i| {
        try std.testing.expectEqual(@as(u8, @truncate(i)), byte);
    }
}

test "op fill hands the pattern through the facade shape" {
    stub.reset();

    var got: [8]u8 = undefined;
    try std.testing.expectEqual(Err.ok, abi.fw_rng_stub_fill(null, &got, got.len));

    for (&got, 0..) |byte, i| {
        try std.testing.expectEqual(@as(u8, @truncate(i)), byte);
    }
}

test "op fill refuses a null buffer with a nonzero length" {
    stub.reset();

    try std.testing.expectEqual(Err.invalid_arg, abi.fw_rng_stub_fill(null, null, 4));
}

test "op fill satisfies a zero-length request without touching state" {
    stub.reset();

    const before = stub.fills();
    try std.testing.expectEqual(Err.ok, abi.fw_rng_stub_fill(null, null, 0));
    try std.testing.expectEqual(before, stub.fills());
}

test "op ready answers true through the facade shape" {
    stub.reset();

    var ready: bool = false;
    try std.testing.expectEqual(Err.ok, abi.fw_rng_stub_ready(null, &ready));
    try std.testing.expect(ready);
}

test "op ready refuses a null report" {
    try std.testing.expectEqual(Err.invalid_arg, abi.fw_rng_stub_ready(null, null));
}

test "the stub iface is one singleton the facade can borrow" {
    const first = abi.fw_rng_stub_iface();
    const second = abi.fw_rng_stub_iface();
    try std.testing.expect(first == second);

    // The ops are non-optional function pointers by type, so "no op unset"
    // holds structurally; the malformed-binding guard is the C facade's job.
    _ = first.fill(null, null, 0);
}

test "the bind convenience hands the facade the stub ops" {
    stub.reset();
    bind_calls = 0;
    last_iface = null;

    var h: abi.Handle = .{ .iface = null, .ctx = null, .bound = false };
    try std.testing.expectEqual(Err.ok, abi.fw_rng_stub_bind(&h));
    try std.testing.expectEqual(@as(usize, 1), bind_calls);
    try std.testing.expect(h.bound);
    try std.testing.expect(last_iface == abi.fw_rng_stub_iface());
    try std.testing.expect(h.iface == last_iface);
    try std.testing.expect(h.ctx == null);
}
