//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! What a non-fatal error report carries when the caller left it out (#2875).
//!
//! A report describing a failure must not itself become a NULL dereference
//! inside the formatter, so the sink substitutes house strings rather than
//! passing NULL through. That substitution is the sink's only policy, which
//! is why it is the only thing in this file.

/// Stand-ins for the strings a caller may omit.
pub const substitute = struct {
    pub const tag: [*:0]const u8 = "ERR_SINK";
    pub const message: [*:0]const u8 = "(no message)";
};

/// The tag a report goes out under: the caller's, or the house tag.
pub fn tagOr(tag: ?[*:0]const u8) [*:0]const u8 {
    return tag orelse substitute.tag;
}

/// The message a report goes out under: the caller's, or the house message.
pub fn messageOr(message: ?[*:0]const u8) [*:0]const u8 {
    return message orelse substitute.message;
}
