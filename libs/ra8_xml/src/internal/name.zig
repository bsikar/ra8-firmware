//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The XML Name production, as much of it as this emitter accepts.
//!
//! ASCII only, which is narrower than the specification's NameStartChar. A
//! firmware document names its own elements, so the wider Unicode ranges buy
//! nothing and would cost a decoder in a library that otherwise never looks
//! past a byte.

/// Name-length bounds (`ra8_xml_writer_limits_t`).
pub const limits = struct {
    /// Element name bytes a frame holds, NUL included.
    pub const name_cap: usize = 64;
    /// Longest name the emitter accepts, terminator excluded.
    pub const name_bytes: usize = name_cap - 1;
};

/// Whether `byte` may open a Name.
pub fn isStart(byte: u8) bool {
    return switch (byte) {
        'A'...'Z', 'a'...'z', '_', ':' => true,
        else => false,
    };
}

/// Whether `byte` may continue a Name.
pub fn isChar(byte: u8) bool {
    return switch (byte) {
        '0'...'9', '.', '-' => true,
        else => isStart(byte),
    };
}

/// Whether `text` is a legal, non-empty Name.
pub fn isValid(text: []const u8) bool {
    if (text.len == 0) return false;
    if (!isStart(text[0])) return false;
    for (text[1..]) |byte| {
        if (!isChar(byte)) return false;
    }
    return true;
}
