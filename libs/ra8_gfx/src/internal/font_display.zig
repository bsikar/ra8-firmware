//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Native-resolution display glyph atlases for ra8-ui labels.

const std = @import("std");

pub const Glyph = struct {
    codepoint: u32,
    left: i16,
    top: i16,
    advance: i16,
    width: u8,
    height: u8,
    offset: u32,
};

pub const Atlas = struct {
    bytes: []const u8,
    ascent: i32,
    descent: i32,
    glyph_count: usize,

    pub fn glyphAt(self: Atlas, index: usize) Glyph {
        const base = 8 + index * 16;
        return .{
            .codepoint = readU32(self.bytes, base),
            .left = @bitCast(readU16(self.bytes, base + 4)),
            .top = @bitCast(readU16(self.bytes, base + 6)),
            .advance = @bitCast(readU16(self.bytes, base + 8)),
            .width = self.bytes[base + 10],
            .height = self.bytes[base + 11],
            .offset = readU32(self.bytes, base + 12),
        };
    }

    pub fn coverageByte(self: Atlas, pixel_index: u32) u8 {
        const coverage_offset = 8 + self.glyph_count * 16;
        return self.bytes[coverage_offset + pixel_index / 4];
    }
};

const body = struct {
    const serif_regular = @embedFile("display_atlases/serif_regular_body.bin");
    const serif_bold = @embedFile("display_atlases/serif_bold_body.bin");
    const sans_regular = @embedFile("display_atlases/sans_regular_body.bin");
    const sans_bold = @embedFile("display_atlases/sans_bold_body.bin");
};
const title = struct {
    const serif_regular = @embedFile("display_atlases/serif_regular_title.bin");
    const serif_bold = @embedFile("display_atlases/serif_bold_title.bin");
    const sans_regular = @embedFile("display_atlases/sans_regular_title.bin");
    const sans_bold = @embedFile("display_atlases/sans_bold_title.bin");
};
const clock = struct {
    const serif_regular = @embedFile("display_atlases/serif_regular_clock.bin");
    const serif_bold = @embedFile("display_atlases/serif_bold_clock.bin");
    const sans_regular = @embedFile("display_atlases/sans_regular_clock.bin");
    const sans_bold = @embedFile("display_atlases/sans_bold_clock.bin");
};

pub fn get(face: u8, weight: u8, size: u8) ?Atlas {
    const selected = switch (size) {
        6 => select(body, face, weight),
        7 => select(title, face, weight),
        8 => select(clock, face, weight),
        else => return null,
    };
    return .{
        .bytes = selected,
        .glyph_count = readU16(selected, 4),
        .ascent = selected[6],
        .descent = selected[7],
    };
}

fn select(comptime set: type, face: u8, weight: u8) []const u8 {
    if (face == 1) return if (weight == 1) set.serif_bold else set.serif_regular;
    return if (weight == 1) set.sans_bold else set.sans_regular;
}

fn readU16(bytes: []const u8, offset: usize) u16 {
    return std.mem.readInt(u16, bytes[offset..][0..2], .little);
}

fn readU32(bytes: []const u8, offset: usize) u32 {
    return std.mem.readInt(u32, bytes[offset..][0..4], .little);
}
