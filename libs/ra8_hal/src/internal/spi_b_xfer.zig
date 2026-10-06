//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! SPI_B polled transfer framing (RA8FW-899, was part of ra8_spi_b.c).
//! HUM Ch 43.2.7 "SPCMDm" p 2893: SPB[20:16] = frame bits - 1.
//! Exports live in src/spi_b_xfer_abi.zig.

const std = @import("std");

pub const width_8: u8 = 7;
pub const width_16: u8 = 15;
pub const width_32: u8 = 31;

pub const spcmd_spb_mask: u32 = 0x001F_0000;
pub const spcmd_spb_shift = 16;

/// Bytes per frame for a supported SPB code, else null.
pub fn unitBytes(width: u8) ?u8 {
    return switch (width) {
        width_8 => 1,
        width_16 => 2,
        width_32 => 4,
        else => null,
    };
}

/// SPCMD0 with SPB replaced by `width`.
pub fn withWidth(spcmd0: u32, width: u8) u32 {
    return (spcmd0 & ~spcmd_spb_mask) | ((@as(u32, width) << spcmd_spb_shift) & spcmd_spb_mask);
}

/// All-ones filler clocked out when there is no TX buffer.
pub fn dummy(width: u8) u32 {
    return switch (width) {
        width_32 => 0xFFFF_FFFF,
        width_16 => 0xFFFF,
        else => 0xFF,
    };
}

/// Frame `idx` of a native-order u8/u16/u32 array.
pub fn load(buf: [*]const u8, idx: u32, width: u8) u32 {
    return switch (width) {
        width_32 => std.mem.readInt(u32, buf[idx * 4 ..][0..4], .little),
        width_16 => std.mem.readInt(u16, buf[idx * 2 ..][0..2], .little),
        else => buf[idx],
    };
}

/// Store frame `idx` into a native-order u8/u16/u32 array, truncating.
pub fn store(buf: [*]u8, idx: u32, width: u8, value: u32) void {
    switch (width) {
        width_32 => std.mem.writeInt(u32, buf[idx * 4 ..][0..4], value, .little),
        width_16 => std.mem.writeInt(u16, buf[idx * 2 ..][0..2], @truncate(value), .little),
        else => buf[idx] = @truncate(value),
    }
}
