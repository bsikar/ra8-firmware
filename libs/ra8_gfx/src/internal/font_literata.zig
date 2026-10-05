//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Constant-time access to the generated, compact Literata glyph atlas.

const std = @import("std");

/// The atlas bytes generated from the licensed 20 px Literata face.
pub const bytes = @embedFile("literata_atlas.bin");

/// Number of glyph records stored after the header.
pub const glyph_count: usize = readU16(4);

/// Typographic ascent used by both drawing and measurement.
pub const ascent: i16 = bytes[6];

/// Typographic descent used by both drawing and measurement.
pub const descent: i16 = bytes[7];

/// One glyph record decoded from the fixed-width atlas directory.
pub const Glyph = struct {
    codepoint: u32,
    left: i16,
    top: i16,
    advance: i16,
    width: u8,
    height: u8,
    offset: u32,
};

const header_bytes: usize = 8;
const record_bytes: usize = 16;
const coverage_offset: usize = header_bytes + glyph_count * record_bytes;

comptime {
    if (bytes.len < header_bytes) @compileError("Literata atlas is shorter than its header");
    if (!std.mem.eql(u8, bytes[0..4], "R8LA")) @compileError("Literata atlas magic mismatch");
    if (bytes.len < coverage_offset) @compileError("Literata atlas directory is truncated");
}

/// Decode one fixed-width glyph directory record.
pub fn glyphAt(index: usize) Glyph {
    const base = header_bytes + index * record_bytes;
    return .{
        .codepoint = readU32(base),
        .left = @bitCast(readU16(base + 4)),
        .top = @bitCast(readU16(base + 6)),
        .advance = @bitCast(readU16(base + 8)),
        .width = bytes[base + 10],
        .height = bytes[base + 11],
        .offset = readU32(base + 12),
    };
}

/// Read one packed coverage byte for a pixel index in the flat glyph atlas.
pub fn coverageByte(pixel_index: u32) u8 {
    return bytes[coverage_offset + pixel_index / 4];
}

fn readU16(offset: usize) u16 {
    return std.mem.readInt(u16, bytes[offset..][0..2], .little);
}

fn readU32(offset: usize) u32 {
    return std.mem.readInt(u32, bytes[offset..][0..4], .little);
}
