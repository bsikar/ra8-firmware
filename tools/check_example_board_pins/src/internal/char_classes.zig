//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Generated character-class tables, lifted verbatim from the CPython the
//! Python predecessor of this gate ran under (3.13, Unicode 15.1).
//!
//! Three classes, deliberately kept apart because the predecessor used all
//! three and they do NOT agree:
//!   * `re_space`  -- what `\\s` matched in a `str` pattern (ENCODING_RE's gaps).
//!   * `re_digit`  -- what `\\d` matched (the port/pin numbers).
//!   * `str_space` -- what `str.isspace()` reported, i.e. what `str.strip()`
//!     removed when a finding's snippet was trimmed, and what
//!     `str.splitlines()` did NOT use.
//! They are generated, not hand-written: `\\s` and `str.isspace()` differ on
//! U+001C-U+001F, and `\\d` is every Nd code point rather than ASCII 0-9.

pub const re_space = [_][2]u21{
    .{ 0x9, 0xD },
    .{ 0x1C, 0x20 },
    .{ 0x85, 0x85 },
    .{ 0xA0, 0xA0 },
    .{ 0x1680, 0x1680 },
    .{ 0x2000, 0x200A },
    .{ 0x2028, 0x2029 },
    .{ 0x202F, 0x202F },
    .{ 0x205F, 0x205F },
    .{ 0x3000, 0x3000 },
};

pub const re_digit = [_][2]u21{
    .{ 0x30, 0x39 },
    .{ 0x660, 0x669 },
    .{ 0x6F0, 0x6F9 },
    .{ 0x7C0, 0x7C9 },
    .{ 0x966, 0x96F },
    .{ 0x9E6, 0x9EF },
    .{ 0xA66, 0xA6F },
    .{ 0xAE6, 0xAEF },
    .{ 0xB66, 0xB6F },
    .{ 0xBE6, 0xBEF },
    .{ 0xC66, 0xC6F },
    .{ 0xCE6, 0xCEF },
    .{ 0xD66, 0xD6F },
    .{ 0xDE6, 0xDEF },
    .{ 0xE50, 0xE59 },
    .{ 0xED0, 0xED9 },
    .{ 0xF20, 0xF29 },
    .{ 0x1040, 0x1049 },
    .{ 0x1090, 0x1099 },
    .{ 0x17E0, 0x17E9 },
    .{ 0x1810, 0x1819 },
    .{ 0x1946, 0x194F },
    .{ 0x19D0, 0x19D9 },
    .{ 0x1A80, 0x1A89 },
    .{ 0x1A90, 0x1A99 },
    .{ 0x1B50, 0x1B59 },
    .{ 0x1BB0, 0x1BB9 },
    .{ 0x1C40, 0x1C49 },
    .{ 0x1C50, 0x1C59 },
    .{ 0xA620, 0xA629 },
    .{ 0xA8D0, 0xA8D9 },
    .{ 0xA900, 0xA909 },
    .{ 0xA9D0, 0xA9D9 },
    .{ 0xA9F0, 0xA9F9 },
    .{ 0xAA50, 0xAA59 },
    .{ 0xABF0, 0xABF9 },
    .{ 0xFF10, 0xFF19 },
    .{ 0x104A0, 0x104A9 },
    .{ 0x10D30, 0x10D39 },
    .{ 0x11066, 0x1106F },
    .{ 0x110F0, 0x110F9 },
    .{ 0x11136, 0x1113F },
    .{ 0x111D0, 0x111D9 },
    .{ 0x112F0, 0x112F9 },
    .{ 0x11450, 0x11459 },
    .{ 0x114D0, 0x114D9 },
    .{ 0x11650, 0x11659 },
    .{ 0x116C0, 0x116C9 },
    .{ 0x11730, 0x11739 },
    .{ 0x118E0, 0x118E9 },
    .{ 0x11950, 0x11959 },
    .{ 0x11C50, 0x11C59 },
    .{ 0x11D50, 0x11D59 },
    .{ 0x11DA0, 0x11DA9 },
    .{ 0x16A60, 0x16A69 },
    .{ 0x16AC0, 0x16AC9 },
    .{ 0x16B50, 0x16B59 },
    .{ 0x1D7CE, 0x1D7FF },
    .{ 0x1E140, 0x1E149 },
    .{ 0x1E2F0, 0x1E2F9 },
    .{ 0x1E950, 0x1E959 },
    .{ 0x1FBF0, 0x1FBF9 },
};

pub const str_space = [_][2]u21{
    .{ 0x9, 0xD },
    .{ 0x1C, 0x20 },
    .{ 0x85, 0x85 },
    .{ 0xA0, 0xA0 },
    .{ 0x1680, 0x1680 },
    .{ 0x2000, 0x200A },
    .{ 0x2028, 0x2029 },
    .{ 0x202F, 0x202F },
    .{ 0x205F, 0x205F },
    .{ 0x3000, 0x3000 },
};

fn inTable(comptime table: []const [2]u21, code_point: u21) bool {
    var low: usize = 0;
    var high: usize = table.len;
    while (low < high) {
        const mid = low + (high - low) / 2;
        if (code_point < table[mid][0]) {
            high = mid;
        } else if (code_point > table[mid][1]) {
            low = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

/// True when CPython's `\s` matched this code point in a `str` pattern.
pub fn isReSpace(code_point: u21) bool {
    return inTable(&re_space, code_point);
}

/// True when CPython's `\d` matched this code point in a `str` pattern.
pub fn isReDigit(code_point: u21) bool {
    return inTable(&re_digit, code_point);
}

/// True when CPython's `str.isspace()` reported this code point as space.
pub fn isStrSpace(code_point: u21) bool {
    return inTable(&str_space, code_point);
}
