//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The one naming table: every format bit has exactly one row, and identify is
//! the sniff and the lookup in one call.

const std = @import("std");
const name = @import("name");

const Format = struct {
    const jpeg: u32 = 1 << 0;
    const png: u32 = 1 << 1;
    const webp: u32 = 1 << 2;
    const gif: u32 = 1 << 3;
    const bmp: u32 = 1 << 4;
    const tga: u32 = 1 << 5;
};

test "every defined format bit has a row" {
    for ([_]u32{ Format.jpeg, Format.png, Format.webp, Format.gif, Format.bmp, Format.tga }) |bit| {
        const row = try name.name(bit);
        try std.testing.expectEqual(bit, row.format);
        try std.testing.expect(row.ext.len != 0);
        try std.testing.expect(row.mime.len != 0);
    }
}

test "the table has one row per bit and no more" {
    try std.testing.expectEqual(@as(usize, 6), name.rows.len);
}

test "the canonical names are the ones the tree already used" {
    try std.testing.expectEqualStrings("jpg", (try name.name(Format.jpeg)).ext);
    try std.testing.expectEqualStrings("image/jpeg", (try name.name(Format.jpeg)).mime);
    try std.testing.expectEqualStrings("x-tga", (try name.name(Format.tga)).mime[6..]);
}

test "the empty set is not a format and has no row" {
    try std.testing.expectError(error.NotFound, name.name(0));
}

test "a two-bit mask is not a format and has no row" {
    try std.testing.expectError(error.NotFound, name.name(Format.png | Format.gif));
}

test "identify sniffs then names" {
    const row = try name.identify("GIF89a");
    try std.testing.expectEqual(Format.gif, row.format);
    try std.testing.expectEqualStrings("gif", row.ext);
}

test "identify carries the sniff fault through unchanged" {
    try std.testing.expectError(error.NotFound, name.identify(&[_]u8{ 1, 2, 3 }));
    try std.testing.expectError(error.InvalidSize, name.identify(&[_]u8{}));
}

test "the c record ends both names with a nul" {
    const row = try name.name(Format.webp);
    const record = name.toAbi(row);
    try std.testing.expectEqualStrings("webp", std.mem.span(record.ext.?));
    try std.testing.expectEqualStrings("image/webp", std.mem.span(record.mime.?));
}
