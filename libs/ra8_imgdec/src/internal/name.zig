//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The one naming table: a container bit to its canonical extension and
//! MIME type, and the sniff-then-name shortcut `identify`.

const abi = @import("abi.zig");
const sniff_mod = @import("sniff.zig");
const vocab = @import("vocab.zig");

const Fault = vocab.Fault;
const Format = vocab.Format;

/// One row of the table, with slices rather than bare pointers.
pub const Row = struct {
    format: u32,
    ext: [:0]const u8,
    mime: [:0]const u8,
};

pub const rows = [_]Row{
    .{ .format = Format.jpeg, .ext = "jpg", .mime = "image/jpeg" },
    .{ .format = Format.png, .ext = "png", .mime = "image/png" },
    .{ .format = Format.webp, .ext = "webp", .mime = "image/webp" },
    .{ .format = Format.gif, .ext = "gif", .mime = "image/gif" },
    .{ .format = Format.bmp, .ext = "bmp", .mime = "image/bmp" },
    .{ .format = Format.tga, .ext = "tga", .mime = "image/x-tga" },
};

comptime {
    // Every defined format bit needs exactly one row, which is what makes
    // `Format.mask` and the table length two spellings of the same fact.
    if (Format.mask + 1 != @as(u32, 1) << rows.len) {
        @compileError("every defined format bit needs exactly one name row");
    }
}

/// The row naming `format`, or `Fault.NotFound` when nothing names it.
pub fn name(format: u32) Fault!Row {
    for (rows) |row| {
        if (row.format == format) return row;
    }
    return Fault.NotFound;
}

/// Sniff `bytes`, then name what was found.
pub fn identify(bytes: []const u8) Fault!Row {
    return name(try sniff_mod.sniff(bytes));
}

/// The row as the C record, with the two names NUL-terminated.
pub fn toAbi(row: Row) abi.Name {
    return .{ .format = row.format, .ext = row.ext.ptr, .mime = row.mime.ptr };
}
