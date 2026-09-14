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

pub fn apply(config: Config) ApplyError!u32 {
    if (config.enabled > 1) return error.InvalidBoolean;
    if (config.reserved0 != 0) return error.ReservedBitsSet;
    if (config.enabled == 0) return config.value;
    return std.math.mul(u32, config.value, config.factor) catch error.Overflow;
}
