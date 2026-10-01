//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The J1 parallel-RGB connector pin tables, one per pixel format, and the
//! rule that separates a GLCDC peripheral output from the GPIO, I2C and
//! clock-input lines sharing the connector. UM Table 33 p 42.

const std = @import("std");
const vocab = @import("vocab.zig");

const Pin = vocab.Pin;

/// One row of a table. The name keeps its sentinel so `abi.zig` can hand the
/// same table out over the C ABI; everything in this archive reads it as a
/// slice.
pub const Entry = struct {
    signal: [:0]const u8,
    pin: u16,
};

/// 24-bit, 8 bits per colour. Order follows the UM table, J1-1 to J1-38.
pub const rgb888 = [_]Entry{
    .{ .signal = "BLEN", .pin = Pin.pack(5, 14) },
    .{ .signal = "SDA1", .pin = Pin.pack(5, 11) },
    .{ .signal = "INT", .pin = Pin.pack(1, 11) },
    .{ .signal = "SCL1", .pin = Pin.pack(5, 12) },
    .{ .signal = "RST", .pin = Pin.pack(6, 6) },
    .{ .signal = "TCON0", .pin = Pin.pack(8, 6) },
    .{ .signal = "CLK", .pin = Pin.pack(5, 15) },
    .{ .signal = "TCON2", .pin = Pin.pack(8, 7) },
    .{ .signal = "TCON1", .pin = Pin.pack(8, 5) },
    .{ .signal = "EXTCLK", .pin = Pin.pack(7, 10) },
    .{ .signal = "TCON3", .pin = Pin.pack(5, 13) },
    .{ .signal = "B1", .pin = Pin.pack(9, 15) },
    .{ .signal = "B0", .pin = Pin.pack(9, 14) },
    .{ .signal = "B3", .pin = Pin.pack(9, 2) },
    .{ .signal = "B2", .pin = Pin.pack(9, 3) },
    .{ .signal = "B5", .pin = Pin.pack(9, 11) },
    .{ .signal = "B4", .pin = Pin.pack(9, 10) },
    .{ .signal = "B7", .pin = Pin.pack(9, 13) },
    .{ .signal = "B6", .pin = Pin.pack(9, 12) },
    .{ .signal = "G1", .pin = Pin.pack(2, 7) },
    .{ .signal = "G0", .pin = Pin.pack(9, 4) },
    .{ .signal = "G3", .pin = Pin.pack(11, 6) },
    .{ .signal = "G2", .pin = Pin.pack(11, 7) },
    .{ .signal = "G5", .pin = Pin.pack(11, 1) },
    .{ .signal = "G4", .pin = Pin.pack(11, 5) },
    .{ .signal = "G7", .pin = Pin.pack(11, 3) },
    .{ .signal = "G6", .pin = Pin.pack(11, 4) },
    .{ .signal = "R1", .pin = Pin.pack(11, 0) },
    .{ .signal = "R0", .pin = Pin.pack(11, 2) },
    .{ .signal = "R3", .pin = Pin.pack(7, 11) },
    .{ .signal = "R2", .pin = Pin.pack(7, 7) },
    .{ .signal = "R5", .pin = Pin.pack(7, 13) },
    .{ .signal = "R4", .pin = Pin.pack(7, 12) },
    .{ .signal = "R7", .pin = Pin.pack(7, 15) },
    .{ .signal = "R6", .pin = Pin.pack(7, 14) },
};

/// 18-bit. The board ties off the low two bits of each channel, so the
/// DATA0..DATA3, DATA8..DATA9 and DATA16..DATA17 lines are absent and the
/// remaining data pins shift up one position against RGB888.
pub const rgb666 = [_]Entry{
    .{ .signal = "BLEN", .pin = Pin.pack(5, 14) },
    .{ .signal = "SDA1", .pin = Pin.pack(5, 11) },
    .{ .signal = "INT", .pin = Pin.pack(1, 11) },
    .{ .signal = "SCL1", .pin = Pin.pack(5, 12) },
    .{ .signal = "RST", .pin = Pin.pack(6, 6) },
    .{ .signal = "TCON0", .pin = Pin.pack(8, 6) },
    .{ .signal = "CLK", .pin = Pin.pack(5, 15) },
    .{ .signal = "TCON2", .pin = Pin.pack(8, 7) },
    .{ .signal = "TCON1", .pin = Pin.pack(8, 5) },
    .{ .signal = "EXTCLK", .pin = Pin.pack(7, 10) },
    .{ .signal = "TCON3", .pin = Pin.pack(5, 13) },
    .{ .signal = "B3", .pin = Pin.pack(9, 15) },
    .{ .signal = "B2", .pin = Pin.pack(9, 14) },
    .{ .signal = "B5", .pin = Pin.pack(9, 2) },
    .{ .signal = "B4", .pin = Pin.pack(9, 3) },
    .{ .signal = "B7", .pin = Pin.pack(9, 11) },
    .{ .signal = "B6", .pin = Pin.pack(9, 10) },
    .{ .signal = "G3", .pin = Pin.pack(9, 13) },
    .{ .signal = "G2", .pin = Pin.pack(9, 12) },
    .{ .signal = "G5", .pin = Pin.pack(2, 7) },
    .{ .signal = "G4", .pin = Pin.pack(9, 4) },
    .{ .signal = "G7", .pin = Pin.pack(11, 6) },
    .{ .signal = "G6", .pin = Pin.pack(11, 7) },
    .{ .signal = "R3", .pin = Pin.pack(11, 1) },
    .{ .signal = "R2", .pin = Pin.pack(11, 5) },
    .{ .signal = "R5", .pin = Pin.pack(11, 3) },
    .{ .signal = "R4", .pin = Pin.pack(11, 4) },
    .{ .signal = "R7", .pin = Pin.pack(11, 0) },
    .{ .signal = "R6", .pin = Pin.pack(11, 2) },
};

/// 16-bit, 5/6/5. R6, R7 and the low green bits are not carried.
pub const rgb565 = [_]Entry{
    .{ .signal = "BLEN", .pin = Pin.pack(5, 14) },
    .{ .signal = "SDA1", .pin = Pin.pack(5, 11) },
    .{ .signal = "INT", .pin = Pin.pack(1, 11) },
    .{ .signal = "SCL1", .pin = Pin.pack(5, 12) },
    .{ .signal = "RST", .pin = Pin.pack(6, 6) },
    .{ .signal = "TCON0", .pin = Pin.pack(8, 6) },
    .{ .signal = "CLK", .pin = Pin.pack(5, 15) },
    .{ .signal = "TCON2", .pin = Pin.pack(8, 7) },
    .{ .signal = "TCON1", .pin = Pin.pack(8, 5) },
    .{ .signal = "EXTCLK", .pin = Pin.pack(7, 10) },
    .{ .signal = "TCON3", .pin = Pin.pack(5, 13) },
    .{ .signal = "B4", .pin = Pin.pack(9, 15) },
    .{ .signal = "B3", .pin = Pin.pack(9, 14) },
    .{ .signal = "B6", .pin = Pin.pack(9, 2) },
    .{ .signal = "B5", .pin = Pin.pack(9, 3) },
    .{ .signal = "G2", .pin = Pin.pack(9, 11) },
    .{ .signal = "B7", .pin = Pin.pack(9, 10) },
    .{ .signal = "G4", .pin = Pin.pack(9, 13) },
    .{ .signal = "G3", .pin = Pin.pack(9, 12) },
    .{ .signal = "G6", .pin = Pin.pack(2, 7) },
    .{ .signal = "G5", .pin = Pin.pack(9, 4) },
    .{ .signal = "R3", .pin = Pin.pack(11, 6) },
    .{ .signal = "G7", .pin = Pin.pack(11, 7) },
    .{ .signal = "R5", .pin = Pin.pack(11, 1) },
    .{ .signal = "R4", .pin = Pin.pack(11, 5) },
    .{ .signal = "R7", .pin = Pin.pack(11, 3) },
    .{ .signal = "R6", .pin = Pin.pack(11, 4) },
};

pub const Fmt = struct {
    pub const rgb888_id: u8 = 0;
    pub const rgb666_id: u8 = 1;
    pub const rgb565_id: u8 = 2;
};

pub fn tableFor(fmt: u8) ?[]const Entry {
    return switch (fmt) {
        Fmt.rgb888_id => &rgb888,
        Fmt.rgb666_id => &rgb666,
        Fmt.rgb565_id => &rgb565,
        else => null,
    };
}

/// A colour-data line is `[RGB]` followed by a digit. The digit is what
/// rules out BLEN, which also starts with B.
pub fn isColorData(signal: []const u8) bool {
    if (signal.len < 2) return false;
    switch (signal[0]) {
        'R', 'G', 'B' => {},
        else => return false,
    }
    return std.ascii.isDigit(signal[1]);
}

/// True for the pins that carry a GLCDC peripheral signal: TCON0..TCON3,
/// CLK and the colour data lines. False for BLEN, RST, SDA1, SCL1, INT and
/// EXTCLK, which stay GPIO, I2C or clock input.
pub fn isOutput(signal: []const u8) bool {
    if (std.mem.startsWith(u8, signal, "TCON")) return true;
    if (std.mem.startsWith(u8, signal, "CLK")) return true;
    return isColorData(signal);
}
