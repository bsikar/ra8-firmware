//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the host-only Zig ABI fixture.

const std = @import("std");
const implementation = @import("internal/root.zig");

const AbiError = enum(u16) {
    ok = 0,
    invalid_arg = 0x103,
    invalid_size = 0x105,
    null_ptr = 0x504,
};

const AbiConfig = extern struct {
    value: u32,
    factor: u16,
    enabled: u8,
    reserved0: u8,
};

comptime {
    if (@sizeOf(AbiError) != 2) @compileError("ABI fixture error width");
    if (@intFromEnum(AbiError.ok) != 0) @compileError("ABI fixture success value");
    if (@intFromEnum(AbiError.invalid_arg) != 0x103) @compileError("ABI fixture invalid arg value");
    if (@intFromEnum(AbiError.invalid_size) != 0x105) @compileError("ABI fixture invalid size value");
    if (@intFromEnum(AbiError.null_ptr) != 0x504) @compileError("ABI fixture null pointer value");
    if (@sizeOf(AbiConfig) != 8) @compileError("ABI fixture structure size");
    if (@alignOf(AbiConfig) != 4) @compileError("ABI fixture structure alignment");
    if (@offsetOf(AbiConfig, "value") != 0) @compileError("ABI fixture value offset");
    if (@offsetOf(AbiConfig, "factor") != 4) @compileError("ABI fixture factor offset");
    if (@offsetOf(AbiConfig, "enabled") != 6) @compileError("ABI fixture enabled offset");
    if (@offsetOf(AbiConfig, "reserved0") != 7) @compileError("ABI fixture reserved offset");
}

pub export fn ra8_abi_fixture_apply(
    config: ?*const AbiConfig,
    out_result: ?*u32,
) callconv(.c) AbiError {
    const output = out_result orelse return .null_ptr;
    const input = config orelse return .null_ptr;
    const result = implementation.apply(.{
        .value = input.value,
        .factor = input.factor,
        .enabled = input.enabled,
        .reserved0 = input.reserved0,
    }) catch |err| switch (err) {
        error.InvalidBoolean, error.ReservedBitsSet => return .invalid_arg,
        error.Overflow => return .invalid_size,
    };
    output.* = result;
    return .ok;
}

test "private implementation maps expected failures" {
    try std.testing.expectError(
        error.InvalidBoolean,
        implementation.apply(.{ .value = 1, .factor = 1, .enabled = 2, .reserved0 = 0 }),
    );
    try std.testing.expectError(
        error.Overflow,
        implementation.apply(.{ .value = std.math.maxInt(u32), .factor = 2, .enabled = 1, .reserved0 = 0 }),
    );
}
