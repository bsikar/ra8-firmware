//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the stub clock profile and the ops that serve it.

const std = @import("std");
const stub = @import("clock_stub");
const abi = @import("clock_stub_abi");
const ops = @import("clock_stub_ops_abi");

const Err = abi.Err;
const Module = abi.Module;

/// Stand-in for the facade's `fw_clock_bind`, which lives in `libs/if` and is
/// linked by the composition root, not by this unit's host tests. It records
/// what the bind convenience handed it so the bind path is testable without
/// the real facade.
var bind_calls: usize = 0;
var last_iface: ?*const ops.Iface = null;

pub export fn fw_clock_bind(clk: ?*ops.Handle, iface: ?*const ops.Iface, ctx: ?*anyopaque) callconv(.c) u16 {
    bind_calls += 1;
    last_iface = iface;

    if (clk) |h| {
        h.iface = iface;
        h.ctx = ctx;
        h.bound = true;
    }

    return Err.ok;
}

fn module(kind: u8, index: u8) Module {
    return .{ .kind = kind, .index = index };
}

test "profile carries the core at its stub rate" {
    try std.testing.expectEqual(@as(?u32, 48_000_000), stub.resolve(stub.core_kind, 0));
}

test "profile carries uart 0 and spi 0 at their stub rates" {
    try std.testing.expectEqual(@as(?u32, 24_000_000), stub.resolve(stub.uart_kind, 0));
    try std.testing.expectEqual(@as(?u32, 48_000_000), stub.resolve(stub.spi_kind, 0));
}

test "profile refuses a non-zero instance index" {
    try std.testing.expectEqual(@as(?u32, null), stub.resolve(stub.core_kind, 1));
    try std.testing.expectEqual(@as(?u32, null), stub.resolve(stub.uart_kind, 2));
}

test "profile refuses kinds the stub board does not carry" {
    try std.testing.expectEqual(@as(?u32, null), stub.resolve(0, 0)); // none
    try std.testing.expectEqual(@as(?u32, null), stub.resolve(6, 0)); // timer
    try std.testing.expectEqual(@as(?u32, null), stub.resolve(19, 0)); // memory
}

test "membrane fills a carried row" {
    var row: abi.Row = undefined;
    try std.testing.expectEqual(Err.ok, abi.fill(module(stub.spi_kind, 0), &row));
    try std.testing.expectEqual(@as(u32, 48_000_000), row.rate_hz);
    try std.testing.expect(row.has_rate);
}

test "membrane zeroes and refuses an absent row" {
    var row: abi.Row = .{ .rate_hz = 0xAABBCCDD, .has_rate = true };
    try std.testing.expectEqual(Err.not_found, abi.fill(module(stub.core_kind, 7), &row));
    try std.testing.expectEqual(@as(u32, 0), row.rate_hz);
    try std.testing.expect(!row.has_rate);
}

test "membrane refuses a null row" {
    try std.testing.expectEqual(Err.invalid_arg, abi.fill(module(stub.core_kind, 0), null));
}

test "ops read a carried rate through the facade shape" {
    var hz: u32 = 0;
    try std.testing.expectEqual(Err.ok, ops.fw_clock_stub_iface().rate_for(null, module(stub.uart_kind, 0), &hz));
    try std.testing.expectEqual(@as(u32, 24_000_000), hz);
}

test "ops refuse an absent module from rate_for" {
    var hz: u32 = 0;
    try std.testing.expectEqual(Err.not_found, ops.fw_clock_stub_iface().rate_for(null, module(10, 0), &hz));
}

test "ops refuse a null rate output" {
    try std.testing.expectEqual(Err.invalid_arg, ops.fw_clock_stub_iface().rate_for(null, module(stub.core_kind, 0), null));
}

test "ops gate a carried module without side effect" {
    try std.testing.expectEqual(Err.ok, ops.fw_clock_stub_iface().set_gate(null, module(stub.core_kind, 0), true));
    try std.testing.expectEqual(Err.ok, ops.fw_clock_stub_iface().set_gate(null, module(stub.spi_kind, 0), false));
}

test "ops refuse gating an absent module" {
    try std.testing.expectEqual(Err.not_found, ops.fw_clock_stub_iface().set_gate(null, module(stub.core_kind, 3), true));
}

test "ops report resolvability through has_module" {
    var present: bool = false;
    try std.testing.expectEqual(Err.ok, ops.fw_clock_stub_iface().has_module(null, module(stub.uart_kind, 0), &present));
    try std.testing.expect(present);

    try std.testing.expectEqual(Err.ok, ops.fw_clock_stub_iface().has_module(null, module(17, 0), &present));
    try std.testing.expect(!present);
}

test "ops refuse a null presence output" {
    try std.testing.expectEqual(Err.invalid_arg, ops.fw_clock_stub_iface().has_module(null, module(stub.core_kind, 0), null));
}

test "the stub iface is one singleton the facade can borrow" {
    const first = ops.fw_clock_stub_iface();
    const second = ops.fw_clock_stub_iface();
    try std.testing.expect(first == second);

    // The ops are non-optional function pointers by type, so "no op unset"
    // holds structurally; the malformed-binding guard is the C facade's job.
    _ = first.rate_for(null, .{ .kind = stub.core_kind, .index = 0 }, null);
}

test "the bind convenience hands the facade the stub ops" {
    bind_calls = 0;
    last_iface = null;

    var h: ops.Handle = .{ .iface = null, .ctx = null, .bound = false };
    try std.testing.expectEqual(Err.ok, ops.fw_clock_stub_bind(&h));
    try std.testing.expectEqual(@as(usize, 1), bind_calls);
    try std.testing.expect(h.bound);
    try std.testing.expect(last_iface == ops.fw_clock_stub_iface());
    try std.testing.expect(h.iface == last_iface);
    try std.testing.expect(h.ctx == null);
}
