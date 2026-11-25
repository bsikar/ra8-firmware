//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests verifying the `ra8_fonts` catalog metadata and SFNT checks.

const std = @import("std");
const fonts = @import("ra8_fonts");

test "every FontId has a unique entry and non-empty metadata" {
    const ids = std.meta.tags(fonts.FontId);
    try std.testing.expectEqual(ids.len, fonts.all_fonts.len);
    for (ids, 0..) |id, idx| {
        const entry = fonts.spec(id);
        try std.testing.expectEqual(id, entry.id);
        try std.testing.expect(entry.rel_path.len > 0);
        try std.testing.expect(entry.family.len > 0);
        try std.testing.expect(entry.license_rel_path.len > 0);
        for (ids[idx + 1 ..]) |other_id| {
            const other = fonts.spec(other_id);
            try std.testing.expect(!std.mem.eql(u8, entry.rel_path, other.rel_path));
        }
    }
}

test "findByPath resolves bundled font paths and rejects unknown ones" {
    const latin1 = fonts.findByPath("literata_latin1.ttf").?;
    try std.testing.expectEqual(fonts.FontId.literata_latin1, latin1.id);
    try std.testing.expect(!latin1.bold);

    const ui_bold = fonts.findByPath("RA8UISans/RA8UISans-Bold.ttf").?;
    try std.testing.expectEqual(fonts.FontId.ui_sans_bold, ui_bold.id);
    try std.testing.expect(ui_bold.bold);

    try std.testing.expect(fonts.findByPath("does_not_exist.ttf") == null);
}

test "baked font header and symbol identifiers match literata_latin1.h" {
    try std.testing.expectEqualStrings("literata_latin1.h", fonts.baked_header_name);
    try std.testing.expectEqualStrings("g_ra8_font_literata_latin1", fonts.baked_symbol_name);
    try std.testing.expectEqualStrings("g_ra8_font_literata_latin1_len", fonts.baked_length_symbol_name);
}

test "isSfntHeader recognizes TrueType/OTF magic bytes and rejects short or invalid inputs" {
    try std.testing.expect(fonts.isSfntHeader(&.{ 0x00, 0x01, 0x00, 0x00, 0x00 }));
    try std.testing.expect(fonts.isSfntHeader("OTTO\x00\x0c"));
    try std.testing.expect(fonts.isSfntHeader("true\x00\x0c"));
    try std.testing.expect(!fonts.isSfntHeader("OT"));
    try std.testing.expect(!fonts.isSfntHeader("NOTF\x00"));
}
