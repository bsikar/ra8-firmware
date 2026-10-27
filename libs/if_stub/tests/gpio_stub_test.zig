//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the stub GPIO profile and the ops that serve it.

const std = @import("std");
const stub = @import("gpio_stub");
const abi = @import("gpio_stub_abi");

const Err = abi.Err;

/// Stand-in for the facade's `fw_gpio_bind`, which lives in `libs/if` and is
/// linked by the composition root, not by this unit's host tests. It records
/// what the bind convenience handed it so the bind path is testable without
/// the real facade.
var bind_calls: usize = 0;
var last_iface: ?*const abi.Iface = null;

pub export fn fw_gpio_bind(gpio: ?*abi.Handle, iface: ?*const abi.Iface, ctx: ?*anyopaque) callconv(.c) u16 {
    bind_calls += 1;
    last_iface = iface;

    if (gpio) |h| {
        h.iface = iface;
        h.ctx = ctx;
        h.bound = true;
    }

    return Err.ok;
}

fn cfg(direction: u8, pull: u8) abi.PinCfg {
    return .{ .direction = direction, .pull = pull };
}

test "profile carries pins 0 through 3" {
    stub.reset();

    try stub.configure(0, .{ .direction = stub.direction_output, .pull = stub.pull_none });
    try stub.configure(3, .{ .direction = stub.direction_input, .pull = stub.pull_up });
}

test "profile refuses pins the stub board does not carry" {
    stub.reset();

    try std.testing.expectError(error.NotFound, stub.configure(4, .{ .direction = stub.direction_output, .pull = stub.pull_none }));
    try std.testing.expectError(error.NotFound, stub.write(100, true));
    try std.testing.expectError(error.NotFound, stub.read(999));
}

test "profile rejects a none direction and an out-of-range pull" {
    stub.reset();

    try std.testing.expectError(error.InvalidArg, stub.configure(0, .{ .direction = stub.direction_none, .pull = stub.pull_none }));
    try std.testing.expectError(error.InvalidArg, stub.configure(0, .{ .direction = stub.direction_output, .pull = 3 }));
}

test "write drives an output pin and read samples it" {
    stub.reset();

    try stub.configure(1, .{ .direction = stub.direction_output, .pull = stub.pull_none });
    try stub.write(1, true);
    try std.testing.expect(try stub.read(1));
    try stub.write(1, false);
    try std.testing.expect(!(try stub.read(1)));
}

test "write to an input pin is refused" {
    stub.reset();

    try stub.configure(2, .{ .direction = stub.direction_input, .pull = stub.pull_up });
    try std.testing.expectError(error.InvalidState, stub.write(2, true));
}

test "an input pin samples the board's tied level" {
    stub.reset();

    try stub.configure(2, .{ .direction = stub.direction_input, .pull = stub.pull_up });
    try std.testing.expect(!(try stub.read(2)));
}

test "an unconfigured pin reads as an input at the tied level" {
    stub.reset();

    try std.testing.expect(!(try stub.read(0)));
    try std.testing.expectError(error.InvalidState, stub.write(0, true));
}

test "reconfiguring an output resets its driven level" {
    stub.reset();

    try stub.configure(1, .{ .direction = stub.direction_output, .pull = stub.pull_none });
    try stub.write(1, true);
    try stub.configure(1, .{ .direction = stub.direction_output, .pull = stub.pull_none });
    try std.testing.expect(!(try stub.read(1)));
}

test "reset restores the unconfigured board" {
    stub.reset();
    try stub.configure(1, .{ .direction = stub.direction_output, .pull = stub.pull_none });
    try stub.write(1, true);
    stub.reset();
    try std.testing.expectError(error.InvalidState, stub.write(1, true));
}

test "op configure hands a config through the facade shape" {
    stub.reset();

    try std.testing.expectEqual(Err.ok, abi.fw_gpio_stub_configure(null, 0, cfg(stub.direction_output, stub.pull_none)));
    try std.testing.expectEqual(Err.ok, abi.fw_gpio_stub_write(null, 0, true));

    var level: bool = false;
    try std.testing.expectEqual(Err.ok, abi.fw_gpio_stub_read(null, 0, &level));
    try std.testing.expect(level);
}

test "op configure answers not_found for a pin the board does not carry" {
    stub.reset();

    try std.testing.expectEqual(Err.not_found, abi.fw_gpio_stub_configure(null, 4, cfg(stub.direction_output, stub.pull_none)));
}

test "op write answers invalid_state for an input pin" {
    stub.reset();

    try std.testing.expectEqual(Err.ok, abi.fw_gpio_stub_configure(null, 2, cfg(stub.direction_input, stub.pull_up)));
    try std.testing.expectEqual(Err.invalid_state, abi.fw_gpio_stub_write(null, 2, true));
}

test "op read refuses a null report" {
    try std.testing.expectEqual(Err.invalid_arg, abi.fw_gpio_stub_read(null, 0, null));
}

test "the stub iface is one singleton the facade can borrow" {
    const first = abi.fw_gpio_stub_iface();
    const second = abi.fw_gpio_stub_iface();
    try std.testing.expect(first == second);

    // The ops are non-optional function pointers by type, so "no op unset"
    // holds structurally; the malformed-binding guard is the C facade's job.
    _ = first.configure(null, 0, cfg(stub.direction_none, stub.pull_none));
}

test "the bind convenience hands the facade the stub ops" {
    stub.reset();
    bind_calls = 0;
    last_iface = null;

    var h: abi.Handle = .{ .iface = null, .ctx = null, .bound = false };
    try std.testing.expectEqual(Err.ok, abi.fw_gpio_stub_bind(&h));
    try std.testing.expectEqual(@as(usize, 1), bind_calls);
    try std.testing.expect(h.bound);
    try std.testing.expect(last_iface == abi.fw_gpio_stub_iface());
    try std.testing.expect(h.iface == last_iface);
    try std.testing.expect(h.ctx == null);
}
