//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the host-only Zig ABI fixture.

const std = @import("std");
const implementation = @import("internal/root.zig");

pub const AbiError = enum(u16) {
    ok = 0,
    no_mem = 0x102,
    invalid_arg = 0x103,
    invalid_state = 0x104,
    invalid_size = 0x105,
    busy = 0x109,
    null_ptr = 0x504,
};

pub const AbiHandle = opaque {};
const InputError = error{ InvalidSize, NullPointer };
const AbiLimit = enum(u32) {
    max_bytes = 32,
};

pub const AbiConfig = extern struct {
    value: u32,
    factor: u16,
    enabled: u8,
    reserved0: u8,
};

comptime {
    if (@sizeOf(AbiError) != 2) @compileError("ABI fixture error width");
    if (@intFromEnum(AbiError.ok) != 0) @compileError("ABI fixture success value");
    if (@intFromEnum(AbiError.no_mem) != 0x102) @compileError("ABI fixture no-memory value");
    if (@intFromEnum(AbiError.invalid_arg) != 0x103) @compileError("ABI fixture invalid arg value");
    if (@intFromEnum(AbiError.invalid_state) != 0x104) @compileError("ABI fixture invalid-state value");
    if (@intFromEnum(AbiError.invalid_size) != 0x105) @compileError("ABI fixture invalid size value");
    if (@intFromEnum(AbiError.busy) != 0x109) @compileError("ABI fixture busy value");
    if (@intFromEnum(AbiError.null_ptr) != 0x504) @compileError("ABI fixture null pointer value");
    if (@sizeOf(AbiLimit) != 4) @compileError("ABI fixture limit width");
    if (@intFromEnum(AbiLimit.max_bytes) != implementation.max_bytes) {
        @compileError("ABI fixture maximum byte value");
    }
    if (@sizeOf(AbiConfig) != 8) @compileError("ABI fixture structure size");
    if (@alignOf(AbiConfig) != 4) @compileError("ABI fixture structure alignment");
    if (@offsetOf(AbiConfig, "value") != 0) @compileError("ABI fixture value offset");
    if (@offsetOf(AbiConfig, "factor") != 4) @compileError("ABI fixture factor offset");
    if (@offsetOf(AbiConfig, "enabled") != 6) @compileError("ABI fixture enabled offset");
    if (@offsetOf(AbiConfig, "reserved0") != 7) @compileError("ABI fixture reserved offset");
}

fn resolveHandle(raw: *AbiHandle) ?*implementation.Handle {
    return implementation.resolve(@ptrCast(raw));
}

fn validateInput(input: ?[*]const u8, input_len: u32) InputError![]const u8 {
    if (input_len > implementation.max_bytes) return error.InvalidSize;
    const bytes = input orelse {
        if (input_len == 0) return &.{};
        return error.NullPointer;
    };
    return bytes[0..input_len];
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

pub export fn ra8_abi_fixture_create(out_handle: ?*?*AbiHandle) callconv(.c) AbiError {
    const output = out_handle orelse return .null_ptr;
    const handle = implementation.create() catch return .no_mem;
    output.* = @ptrCast(handle);
    return .ok;
}

pub export fn ra8_abi_fixture_test_fail_next_allocation() callconv(.c) void {
    implementation.failNextAllocation();
}

pub export fn ra8_abi_fixture_destroy(in_out_handle: ?*?*AbiHandle) callconv(.c) AbiError {
    const output = in_out_handle orelse return .null_ptr;
    const raw = output.* orelse return .ok;
    const handle = resolveHandle(raw) orelse return .invalid_arg;
    if (!implementation.destroy(handle)) return .busy;
    output.* = null;
    return .ok;
}

pub export fn ra8_abi_fixture_copy(
    raw_handle: ?*AbiHandle,
    input: ?[*]const u8,
    input_len: u32,
    output: ?[*]u8,
    capacity: u32,
    out_len: ?*u32,
) callconv(.c) AbiError {
    const length_output = out_len orelse return .null_ptr;
    const bytes_output = output orelse return .null_ptr;
    const raw = raw_handle orelse return .null_ptr;
    if (resolveHandle(raw) == null) return .invalid_arg;
    const bytes = validateInput(input, input_len) catch |err| switch (err) {
        error.InvalidSize => return .invalid_size,
        error.NullPointer => return .null_ptr,
    };
    if (capacity < input_len) return .invalid_size;
    @memcpy(bytes_output[0..input_len], bytes);
    length_output.* = input_len;
    return .ok;
}

pub export fn ra8_abi_fixture_bytes_create(
    raw_handle: ?*AbiHandle,
    input: ?[*]const u8,
    input_len: u32,
    out_bytes: ?*?[*]u8,
    out_len: ?*u32,
) callconv(.c) AbiError {
    const bytes_output = out_bytes orelse return .null_ptr;
    const length_output = out_len orelse return .null_ptr;
    const raw = raw_handle orelse return .null_ptr;
    const handle = resolveHandle(raw) orelse return .invalid_arg;
    const bytes = validateInput(input, input_len) catch |err| switch (err) {
        error.InvalidSize => return .invalid_size,
        error.NullPointer => return .null_ptr,
    };
    const owned = implementation.bytesCreate(handle, bytes) catch return .no_mem;
    bytes_output.* = owned.ptr;
    length_output.* = @intCast(owned.len);
    return .ok;
}

pub export fn ra8_abi_fixture_bytes_release(
    in_out_bytes: ?*?[*]u8,
) callconv(.c) AbiError {
    const output = in_out_bytes orelse return .null_ptr;
    const bytes = output.* orelse return .ok;
    implementation.bytesRelease(bytes) catch |err| switch (err) {
        error.InvalidState => return .invalid_state,
        error.InvalidPointer => return .invalid_arg,
    };
    output.* = null;
    return .ok;
}
