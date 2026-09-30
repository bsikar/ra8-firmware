//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The five XML predefined entities and the escaping policy built on them.
//!
//! Every metacharacter is escaped in both text and attribute positions, which
//! is more than element content strictly requires. That is deliberate: one
//! policy for both positions means a string cannot become unsafe by being
//! moved from one to the other.

/// Byte counts the emitter reasons about (`ra8_xml_writer_limits_t`).
pub const limits = struct {
    /// Longest entity form, `&quot;`, without a terminator.
    pub const entity_bytes: usize = 6;
};

/// The entity form of `byte`, or null when it may be copied verbatim.
pub fn form(byte: u8) ?[]const u8 {
    return switch (byte) {
        '&' => "&amp;",
        '<' => "&lt;",
        '>' => "&gt;",
        '"' => "&quot;",
        '\'' => "&apos;",
        else => null,
    };
}

/// Bytes `src` occupies once escaped, terminator excluded.
pub fn escapedLen(src: []const u8) usize {
    var total: usize = 0;
    for (src) |byte| total += if (form(byte)) |ent| ent.len else 1;
    return total;
}

/// Escape `src` into `out`, returning the bytes written.
///
/// Refuses as a whole: when the escaped form does not fit, nothing of `out` is
/// considered written, so no caller can be handed a truncated fragment that
/// happens to still parse.
pub fn escape(src: []const u8, out: []u8) error{NoSpace}![]u8 {
    const needed = escapedLen(src);
    if (needed > out.len) return error.NoSpace;

    var written: usize = 0;
    for (src) |byte| {
        if (form(byte)) |ent| {
            @memcpy(out[written..][0..ent.len], ent);
            written += ent.len;
        } else {
            out[written] = byte;
            written += 1;
        }
    }
    return out[0..written];
}
