//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Source pixel formats the host surface blits from (RA8FW-950). The value
//! of each format is its bytes per pixel, the same convention as the widget
//! Paint table's pixel format. Each reads out as argb8888; only argb8888
//! carries its own alpha, the rest are opaque.

pub const Format = enum(u8) {
    gray8 = 1,
    rgb565 = 2,
    rgb888 = 3,
    argb8888 = 4,

    pub fn bytes(self: Format) usize {
        return @backingInt(self);
    }
};

/// The pixel at byte `offset` of `pixels` as argb8888, or null when it runs
/// past the end. Multi-byte formats are little-endian, as stored in memory.
pub fn read(pixels: []const u8, offset: usize, format: Format) ?u32 {
    if (offset + format.bytes() > pixels.len) return null;
    const p = pixels[offset..];
    return switch (format) {
        .gray8 => 0xFF00_0000 | @as(u32, p[0]) * 0x01_0101,
        .rgb565 => widen565(@as(u16, p[0]) | @as(u16, p[1]) << 8),
        .rgb888 => 0xFF00_0000 | @as(u32, p[2]) << 16 | @as(u32, p[1]) << 8 | p[0],
        .argb8888 => @as(u32, p[0]) | @as(u32, p[1]) << 8 | @as(u32, p[2]) << 16 | @as(u32, p[3]) << 24,
    };
}

fn widen565(value: u16) u32 {
    const r: u32 = (value >> 11) & 0x1F;
    const g: u32 = (value >> 5) & 0x3F;
    const b: u32 = value & 0x1F;
    return 0xFF00_0000 | ((r << 3) | (r >> 2)) << 16 | ((g << 2) | (g >> 4)) << 8 | ((b << 3) | (b >> 2));
}
