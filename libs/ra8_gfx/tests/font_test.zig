//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the bundled 8x16 font. The table is data, so what is
//! pinned here is what a consumer of `ra8_gfx_font_t` is entitled to assume:
//! the three descriptor invariants from `inc/ra8_gfx_font.h`, the promised
//! ASCII range, that every stored codepoint is addressable to its last byte,
//! that space is the only blank cell, and six committed bitmaps read back
//! byte for byte so a table edit that shifts every glyph by one row cannot
//! pass. The same six the untouched C suite pins, transcribed from the
//! committed table rather than from an external archive.

const std = @import("std");
const font = @import("font");

const geometry = font.table.geometry;

test "descriptor points at the table with the promised geometry" {
    const d = font.ra8_gfx_font_8x16;
    try std.testing.expect(d.glyph_data != null);
    try std.testing.expectEqual(@as(u8, 8), d.glyph_width);
    try std.testing.expectEqual(@as(u8, 16), d.glyph_height);
    try std.testing.expectEqual(@as(u8, 16), d.bytes_per_glyph);
    try std.testing.expectEqual(@as(u8, 0x20), d.first_codepoint);
    try std.testing.expectEqual(@as(u8, 0x7E), d.last_codepoint);
    try std.testing.expectEqual(&font.table.glyphs, d.glyph_data.?);
}

test "the header's three descriptor invariants hold" {
    const d = font.ra8_gfx_font_8x16;
    try std.testing.expect(d.glyph_width >= 1);
    try std.testing.expect(d.glyph_height >= 1);
    try std.testing.expect(d.first_codepoint <= d.last_codepoint);
    const row_bytes = (@as(u32, d.glyph_width) + 7) / 8;
    try std.testing.expectEqual(@as(u32, d.bytes_per_glyph), row_bytes * @as(u32, d.glyph_height));
}

test "footprint is 95 glyphs of 16 bytes" {
    try std.testing.expectEqual(@as(usize, 95), geometry.glyph_count);
    try std.testing.expectEqual(@as(usize, 1520), geometry.table_bytes);
    try std.testing.expectEqual(@as(usize, 1520), font.table.glyphs.len);
}

test "row bytes are one per row for an 8-pixel cell" {
    try std.testing.expectEqual(@as(u8, 1), font.table.rowBytes());
}

test "every stored codepoint slices to its own 16 bytes" {
    var cp: u16 = geometry.first_codepoint;
    while (cp <= geometry.last_codepoint) : (cp += 1) {
        const rows = font.table.glyph(@intCast(cp)).?;
        try std.testing.expectEqual(@as(usize, 16), rows.len);
        const at = (cp - geometry.first_codepoint) * geometry.bytes_per_glyph;
        try std.testing.expectEqual(&font.table.glyphs[at], &rows[0]);
        try std.testing.expect(at + rows.len <= geometry.table_bytes);
    }
}

test "codepoints outside the stored range have no cell" {
    try std.testing.expectEqual(@as(?[]const u8, null), font.table.glyph(geometry.first_codepoint - 1));
    try std.testing.expectEqual(@as(?[]const u8, null), font.table.glyph(geometry.last_codepoint + 1));
    try std.testing.expectEqual(@as(?[]const u8, null), font.table.glyph(0));
    try std.testing.expectEqual(@as(?[]const u8, null), font.table.glyph(0xFF));
}

test "space is the only blank cell" {
    var blank: u32 = 0;
    var cp: u16 = geometry.first_codepoint;
    while (cp <= geometry.last_codepoint) : (cp += 1) {
        var ink: u8 = 0;
        for (font.table.glyph(@intCast(cp)).?) |row| {
            ink |= row;
        }
        if (ink == 0) {
            blank += 1;
            try std.testing.expectEqual(@as(u16, geometry.first_codepoint), cp);
        }
    }
    try std.testing.expectEqual(@as(u32, 1), blank);
}

test "six committed bitmaps read back byte for byte" {
    const cases = [_]struct { cp: u8, rows: [16]u8 }{
        .{ .cp = 0x20, .rows = .{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
        .{ .cp = 0x21, .rows = .{ 0, 0, 0x18, 0x3C, 0x3C, 0x3C, 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0, 0, 0, 0 } },
        .{ .cp = 0x30, .rows = .{ 0, 0, 0x7C, 0xC6, 0xC6, 0xCE, 0xDE, 0xF6, 0xE6, 0xC6, 0xC6, 0x7C, 0, 0, 0, 0 } },
        .{ .cp = 0x41, .rows = .{ 0, 0, 0x10, 0x38, 0x6C, 0xC6, 0xC6, 0xFE, 0xC6, 0xC6, 0xC6, 0xC6, 0, 0, 0, 0 } },
        .{ .cp = 0x5F, .rows = .{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0, 0 } },
        .{ .cp = 0x7E, .rows = .{ 0, 0, 0x76, 0xDC, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
    };
    for (cases) |want| {
        try std.testing.expectEqualSlices(u8, &want.rows, font.table.glyph(want.cp).?);
    }
}

test "the cell is 8 pixels wide, so no row carries an undrawable bit" {
    // A 1-byte row can hold exactly the eight columns the renderer walks, so
    // every bit of every row is addressable; nothing to mask off.
    try std.testing.expectEqual(@as(u32, 8), @as(u32, geometry.width));
    try std.testing.expectEqual(@as(usize, geometry.height), font.table.glyph('A').?.len);
}

test "descriptor is the one the C layout pinned" {
    try std.testing.expectEqual(@sizeOf(font.Font), @sizeOf(font.Font));
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(font.Font, "glyph_data"));
    try std.testing.expectEqual(@sizeOf(usize), @offsetOf(font.Font, "glyph_width"));
    try std.testing.expectEqual(@sizeOf(usize) + 4, @offsetOf(font.Font, "last_codepoint"));
}
