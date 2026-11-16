//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for the sfnt table-directory bounds guard (RA8FW-1240), the
//! validator run before `stbtt_InitFont()` so a crafted @font-face font cannot
//! drive the out-of-bounds read in stb_truetype's `stbtt__find_table()`.
//! Ported from tests/graphics/src/test_ra8_stbtt_guard.c, whose direct-guard
//! vectors moved here; that file keeps the end-to-end call-site tests
//! (reflow_bind_font, reflow_register_face, epub_render_glyph) and the fuzz
//! regression. The accept-side control vector is a synthetic three-table sfnt
//! (cmap/head/maxp, every internal read in bounds) instead of the Ahem face,
//! which the C test still exercises end to end.

const std = @import("std");
const guard = @import("stbtt_guard");

fn inBounds(font: []const u8) bool {
    return guard.ra8_stbtt_sfnt_dir_in_bounds(font.ptr, font.len, 0);
}

/// A crafted font with a valid sfnt tag but a directory that overruns the
/// 16-byte buffer (numTables = 20 needs 12 + 20*16 = 332 bytes).
const bad_dir_truncated = [16]u8{
    0x00, 0x01, 0x00, 0x00, // sfnt version 1.0
    0x00, 0x14, // numTables = 20 -> directory 332 B > 16 B
    0x00, 0x00,
    0x00, 0x00,
    0x00, 0x00,
    0x00, 0x00,
    0x00, 0x00,
};

/// A crafted font whose single directory record fits but declares a table at
/// offset 0x10000000 -- far past the 28-byte buffer.
const bad_dir_record = [28]u8{
    0x00, 0x01, 0x00, 0x00, // sfnt version 1.0
    0x00, 0x01, // numTables = 1
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // search hints (ignored)
    0x00, 0x00, 0x00, 0x00, // record 0: tag
    0x00, 0x00, 0x00, 0x00, // record 0: checksum
    0x10, 0x00, 0x00, 0x00, // record 0: offset 0x10000000
    0x00, 0x00, 0x01, 0x00, // record 0: length 0x100
};

/// A crafted font whose cmap record fits but whose in-table encoding count
/// overruns the buffer (the fuzz-found out-of-bounds read).
const bad_cmap_subdir = [32]u8{
    0x00, 0x01, 0x00, 0x00, // sfnt version 1.0
    0x00, 0x01, // numTables = 1
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // search hints (ignored)
    0x63, 0x6D, 0x61, 0x70, // record 0: tag "cmap"
    0x00, 0x00, 0x00, 0x00, // record 0: checksum
    0x00, 0x00, 0x00, 0x1C, // record 0: offset 28
    0x00, 0x00, 0x00, 0x04, // record 0: length 4 (fits)
    0x00, 0x00, // cmap version 0
    0xFF, 0xFF, // cmap numTables = 65535 (huge)
};

/// A 10-byte buffer: shorter than the 12-byte sfnt offset table.
const hdr_too_small = [10]u8{
    0x00, 0x01, 0x00, 0x00, // sfnt version 1.0
    0x00, 0x05, // numTables field, never reached
    0x00, 0x00,
    0x00, 0x00,
};

/// A minimal, in-bounds 12-byte offset table declaring zero tables. Memory
/// safe (nothing to walk), so the guard accepts it even though
/// `stbtt_InitFont` would later reject it for lacking real tables -- the guard
/// is a memory-safety gate, not a semantic validator.
const zero_tables = [12]u8{
    0x00, 0x01, 0x00, 0x00, // sfnt version 1.0
    0x00, 0x00, // numTables = 0
    0x00, 0x00,
    0x00, 0x00,
    0x00, 0x00,
};

/// A synthetic, well-formed sfnt: offset table + three records (cmap, head,
/// maxp) whose declared extents and in-table internal reads all fit. 132 bytes.
const valid_three_table: [132]u8 = blk: {
    var font: [132]u8 = @splat(0);
    // Offset table: sfnt version 1.0, numTables = 3, search hints (ignored).
    font[0..4].* = .{ 0x00, 0x01, 0x00, 0x00 };
    font[4..6].* = .{ 0x00, 0x03 };
    font[6..12].* = .{ 0x00, 0x30, 0x00, 0x01, 0x00, 0x00 };
    // Record 0: "cmap", offset 60, length 12.
    font[12..16].* = .{ 0x63, 0x6D, 0x61, 0x70 };
    font[20..24].* = .{ 0x00, 0x00, 0x00, 0x3C };
    font[24..28].* = .{ 0x00, 0x00, 0x00, 0x0C };
    // Record 1: "head", offset 72, length 54.
    font[28..32].* = .{ 0x68, 0x65, 0x61, 0x64 };
    font[36..40].* = .{ 0x00, 0x00, 0x00, 0x48 };
    font[40..44].* = .{ 0x00, 0x00, 0x00, 0x36 };
    // Record 2: "maxp", offset 126, length 6.
    font[44..48].* = .{ 0x6D, 0x61, 0x78, 0x70 };
    font[52..56].* = .{ 0x00, 0x00, 0x00, 0x7E };
    font[56..60].* = .{ 0x00, 0x00, 0x00, 0x06 };
    // cmap table at 60: version 0, numTables = 1, one encoding record. The
    // internal check needs cmap + 4 + 8 * 1 = 72 <= 132.
    font[62..64].* = .{ 0x00, 0x01 };
    font[64..72].* = .{ 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0C };
    // head table at 72 (54 zero bytes): the internal check needs 72 + 52 <= 132.
    // maxp table at 126 (6 zero bytes): the internal check needs 126 + 6 <= 132.
    break :blk font;
};

test "null data rejected" {
    try std.testing.expect(!guard.ra8_stbtt_sfnt_dir_in_bounds(null, 200, 0));
}

test "header shorter than the offset table rejected" {
    try std.testing.expect(!inBounds(&hdr_too_small));
}

test "truncated directory rejected" {
    try std.testing.expect(!inBounds(&bad_dir_truncated));
}

test "out-of-range table record rejected" {
    try std.testing.expect(!inBounds(&bad_dir_record));
}

test "cmap in-table subdir overrun rejected" {
    try std.testing.expect(!inBounds(&bad_cmap_subdir));
}

test "valid fonts accepted" {
    try std.testing.expect(inBounds(&valid_three_table));
    try std.testing.expect(inBounds(&zero_tables));
}
