//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Generated character-class tables, lifted verbatim from the CPython the
//! Python predecessor of this gate ran under (3.13, Unicode 15.1).
//!
//! Two classes, kept apart because the predecessor used both at different
//! call sites. They are MEASURED IDENTICAL, not assumed so: walking all
//! 0x110000 code points on CPython 3.11.14, `re.match(r"\s", chr(cp))` and
//! `chr(cp).isspace()` answer for exactly the same 29 code points, U+001C-
//! U+001F included. The tables stay separate so that a future CPython which
//! does split them can be carried without hunting down the call sites:
//!   * `re_space`  -- what `\s` matched in a `str` pattern, i.e. the gap
//!     `__attribute__\s*\(\(` tolerates and the run `ATTR-OK:\s*\S` skips.
//!   * `str_space` -- what `str.isspace()` reported, i.e. what `str.strip()`
//!     and `str.lstrip()` removed when a name was normalised, a snippet
//!     trimmed, or a line tested for a leading comment marker.
//!
//! Regenerate with (the same one-liner that produced them):
//!   python3 -c 'import re,sys;ivs=lambda p:[(a,b) for a,b in __import__("itertools").groupby(range(0x110000))]'
//! in practice: walk range(0x110000), keep the code points where
//! `re.match(r"\s", chr(cp))` (resp. `chr(cp).isspace()`) answers, and fold
//! the survivors into closed intervals.

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
