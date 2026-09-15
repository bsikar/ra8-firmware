//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Private implementation for the host-only Zig C ABI fixture.

const std = @import("std");

pub const Config = struct {
    value: u32,
    factor: u16,
    enabled: u8,
    reserved0: u8,
};

pub const ApplyError = error{
    InvalidBoolean,
    ReservedBitsSet,
    Overflow,
};

pub const max_bytes: usize = 32;

pub const Handle = struct {
    bytes: [max_bytes]u8 = [_]u8{0} ** max_bytes,
    bytes_live: bool = false,
};

var handle_slot = Handle{};
var handle_live = false;
var fail_next_allocation = false;

pub const CreateError = error{NoMemory};
pub const BytesCreateError = error{NoMemory};
pub const BytesReleaseError = error{ InvalidState, InvalidPointer };

pub fn failNextAllocation() void {
    fail_next_allocation = true;
}

fn allocationFails() bool {
    if (!fail_next_allocation) return false;
    fail_next_allocation = false;
    return true;
}

pub fn apply(config: Config) ApplyError!u32 {
    if (config.enabled > 1) return error.InvalidBoolean;
    if (config.reserved0 != 0) return error.ReservedBitsSet;
    if (config.enabled == 0) return config.value;
    return std.math.mul(u32, config.value, config.factor) catch error.Overflow;
}

pub fn create() CreateError!*Handle {
    if (allocationFails()) return error.NoMemory;
    if (handle_live) return error.NoMemory;
    handle_slot = .{};
    handle_live = true;
    return &handle_slot;
}

pub fn resolve(raw: *anyopaque) ?*Handle {
    if (!handle_live or @intFromPtr(raw) != @intFromPtr(&handle_slot)) return null;
    return &handle_slot;
}

pub fn destroy(handle: *Handle) bool {
    if (handle.bytes_live) return false;
    handle.* = .{};
    handle_live = false;
    return true;
}

pub fn bytesCreate(handle: *Handle, input: []const u8) BytesCreateError![]u8 {
    if (allocationFails()) return error.NoMemory;
    if (handle.bytes_live) return error.NoMemory;
    @memcpy(handle.bytes[0..input.len], input);
    handle.bytes_live = true;
    return handle.bytes[0..input.len];
}

pub fn bytesRelease(raw: [*]u8) BytesReleaseError!void {
    if (!handle_live or !handle_slot.bytes_live) return error.InvalidState;
    if (@intFromPtr(raw) != @intFromPtr(&handle_slot.bytes)) return error.InvalidPointer;
    handle_slot.bytes_live = false;
}
