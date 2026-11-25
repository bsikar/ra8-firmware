//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Catalog and metadata for the curated font assets owned by `ra8_fonts`.

const std = @import("std");

/// Identifiers for bundled TrueType font assets in `ra8_fonts`.
pub const FontId = enum {
    literata_regular,
    literata_latin1,
    ui_sans_regular,
    ui_sans_bold,
};

/// Metadata describing one curated TrueType asset in `ra8_fonts`.
pub const FontSpec = struct {
    id: FontId,
    /// Relative path inside the `ra8_fonts` asset directory.
    rel_path: []const u8,
    /// Display family name tracked for font licensing and lookup.
    family: []const u8,
    /// License notice file alongside the font assets.
    license_rel_path: []const u8,
    bold: bool,
};

/// Default C header exposing the baked Latin-1 fallback symbols.
pub const baked_header_name = "literata_latin1.h";
/// C symbol name of the baked Latin-1 byte array declared in `literata_latin1.h`.
pub const baked_symbol_name = "g_ra8_font_literata_latin1";
/// C symbol name of the byte length declared in `literata_latin1.h`.
pub const baked_length_symbol_name = "g_ra8_font_literata_latin1_len";

/// Complete ordered catalog of bundled font assets.
pub const all_fonts = [_]FontSpec{
    .{
        .id = .literata_regular,
        .rel_path = "Literata-Regular.ttf",
        .family = "Literata",
        .license_rel_path = "Literata-OFL.txt",
        .bold = false,
    },
    .{
        .id = .literata_latin1,
        .rel_path = "literata_latin1.ttf",
        .family = "Literata",
        .license_rel_path = "Literata-OFL.txt",
        .bold = false,
    },
    .{
        .id = .ui_sans_regular,
        .rel_path = "RA8UISans/RA8UISans-Regular.ttf",
        .family = "RA8UISans",
        .license_rel_path = "DejaVu-LICENSE.txt",
        .bold = false,
    },
    .{
        .id = .ui_sans_bold,
        .rel_path = "RA8UISans/RA8UISans-Bold.ttf",
        .family = "RA8UISans",
        .license_rel_path = "DejaVu-LICENSE.txt",
        .bold = true,
    },
};

/// Returns the catalog specification for `id`.
pub fn spec(id: FontId) FontSpec {
    for (all_fonts) |entry| {
        if (entry.id == id) return entry;
    }
    unreachable;
}

/// Finds a catalog entry matching `rel_path` (exact match within the asset tree).
pub fn findByPath(rel_path: []const u8) ?FontSpec {
    for (all_fonts) |entry| {
        if (std.mem.eql(u8, entry.rel_path, rel_path)) return entry;
    }
    return null;
}

/// Returns true if `bytes` begins with a standard TrueType or OpenType sfnt tag.
pub fn isSfntHeader(bytes: []const u8) bool {
    if (bytes.len < 4) return false;
    const tag: [4]u8 = bytes[0..4].*;
    return std.mem.eql(u8, &tag, &.{ 0x00, 0x01, 0x00, 0x00 }) or
        std.mem.eql(u8, &tag, "OTTO") or
        std.mem.eql(u8, &tag, "true");
}
