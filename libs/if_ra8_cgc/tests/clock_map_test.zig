//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Vectors for the RA8 module-to-clock table.
//!
//! The table is where every wrong answer in this adapter would live, so it is
//! exercised exhaustively and the instance-index arithmetic is pinned against
//! the module-stop ids. The ops themselves call `ra8_cgc` and `ra8_mstp`, so
//! they stay with the untouched C suite in `tests/if/src`, which drives them
//! through the public port facade.

const std = @import("std");
const map = @import("clock_map");

const Kind = map.Kind;
const Mstp = map.Mstp;

/// Module kinds this adapter is expected to resolve at index zero.
const resolvable = [_]Kind{
    .core,    .uart,   .spi,      .i2c,    .can,    .adc,    .dac,
    .display, .camera, .ethernet, .sdhost, .crypto, .memory,
};

/// Module kinds this adapter deliberately carries no row for.
const absent = [_]Kind{ .none, .timer, .pwm, .dma, .usb, .rtc, .watchdog };

test "every row this adapter claims resolves at index 0" {
    for (resolvable) |kind| {
        const row = try map.resolve(@intFromEnum(kind), 0);
        try std.testing.expect(row.domain != null or row.gate != null);
    }
}

test "kinds with no row report not_found" {
    for (absent) |kind| {
        try std.testing.expectError(error.NotFound, map.resolve(@intFromEnum(kind), 0));
    }
}

test "uart 0..9 walk SCI0 down to SCI9" {
    for (0..10) |i| {
        const row = try map.resolve(@intFromEnum(Kind.uart), @intCast(i));
        try std.testing.expectEqual(Mstp.sci0 - @as(u16, @intCast(i)), row.gate.?);
        try std.testing.expectEqual(map.Domain.pclka, row.domain.?);
    }
}

test "the multi-instance runs descend within one MSTPCRx word" {
    const i2c2 = try map.resolve(@intFromEnum(Kind.i2c), 2);
    try std.testing.expectEqual(Mstp.iic0 - 2, i2c2.gate.?);

    const spi1 = try map.resolve(@intFromEnum(Kind.spi), 1);
    try std.testing.expectEqual(Mstp.spi0 - 1, spi1.gate.?);

    const can1 = try map.resolve(@intFromEnum(Kind.can), 1);
    try std.testing.expectEqual(Mstp.canfd0 - 1, can1.gate.?);

    const sd1 = try map.resolve(@intFromEnum(Kind.sdhost), 1);
    try std.testing.expectEqual(Mstp.sdhi0 - 1, sd1.gate.?);

    const dac1 = try map.resolve(@intFromEnum(Kind.dac), 1);
    try std.testing.expectEqual(Mstp.dac12_0 - 1, dac1.gate.?);
}

test "one past the last instance of a run is not_found" {
    try std.testing.expectError(error.NotFound, map.resolve(@intFromEnum(Kind.uart), 10));
    try std.testing.expectError(error.NotFound, map.resolve(@intFromEnum(Kind.i2c), 3));
    try std.testing.expectError(error.NotFound, map.resolve(@intFromEnum(Kind.core), 1));
}

test "a kind outside the enumeration is rejected, not clamped" {
    try std.testing.expectError(error.BadKind, map.resolve(map.kind_count, 0));
    try std.testing.expectError(error.BadKind, map.resolve(255, 0));
}

test "the core and memory rows read a rate but cannot be gated" {
    const core = try map.resolve(@intFromEnum(Kind.core), 0);
    try std.testing.expectEqual(map.Domain.cpuclk0, core.domain.?);
    try std.testing.expect(core.gate == null);

    const memory = try map.resolve(@intFromEnum(Kind.memory), 0);
    try std.testing.expectEqual(map.Domain.fclk, memory.domain.?);
    try std.testing.expect(memory.gate == null);
}

test "gate-only rows carry no feed domain" {
    const gate_only = [_]Kind{ .adc, .dac, .display, .ethernet, .crypto };
    for (gate_only) |kind| {
        const row = try map.resolve(@intFromEnum(kind), 0);
        try std.testing.expect(row.domain == null);
        try std.testing.expect(row.gate != null);
    }
}

test "the camera row is the only PCLKD row" {
    const camera = try map.resolve(@intFromEnum(Kind.camera), 0);
    try std.testing.expectEqual(map.Domain.pclkd, camera.domain.?);
    try std.testing.expectEqual(Mstp.ceu, camera.gate.?);
}
