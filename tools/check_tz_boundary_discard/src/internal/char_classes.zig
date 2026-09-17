//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Whitespace intervals GENERATED from the toolchain's CPython (3.11.14), the
//! interpreter the predecessor `scripts/checks/check_tz_boundary_discard.py`
//! ran under. The gate leans on two whitespace notions: `re`'s `\s` (every
//! `\s*` / `\s+` / `\S` in its four patterns) and `str.strip()` / `str.lstrip()`
//! (the snippet trim and the comment-position test). On this interpreter the
//! two tables are IDENTICAL, so one table serves both; they are known to
//! disagree on other builds, so the regeneration one-liner below prints both
//! and the equality is asserted in tests rather than assumed.
//!
//! Regenerate with:
//!   python3 -c 'import re;sp=re.compile(r"\s");
//!   f=lambda p:[(a,b) for a,b in __import__("itertools").groupby(range(0x110000))]'
//! or, readably:
//!   python3 - <<EOF
//!   import re
//!   sp = re.compile(r"\s")
//!   def intervals(pred):
//!       out, start = [], None
//!       for cp in range(0x110000):
//!           if pred(cp):
//!               if start is None: start = cp
//!           elif start is not None:
//!               out.append((start, cp - 1)); start = None
//!       return out
//!   print(intervals(lambda cp: bool(sp.fullmatch(chr(cp)))))
//!   print(intervals(lambda cp: chr(cp).isspace()))
//!   EOF

const std = @import("std");

pub const Interval = struct { low: u21, high: u21 };

/// `re` `\s` for str patterns, as ten closed intervals.
pub const re_space_intervals = [_]Interval{
    .{ .low = 0x0009, .high = 0x000D },
    .{ .low = 0x001C, .high = 0x0020 },
    .{ .low = 0x0085, .high = 0x0085 },
    .{ .low = 0x00A0, .high = 0x00A0 },
    .{ .low = 0x1680, .high = 0x1680 },
    .{ .low = 0x2000, .high = 0x200A },
    .{ .low = 0x2028, .high = 0x2029 },
    .{ .low = 0x202F, .high = 0x202F },
    .{ .low = 0x205F, .high = 0x205F },
    .{ .low = 0x3000, .high = 0x3000 },
};

/// `str.isspace()`, the table `str.strip()` and `str.lstrip()` trim with.
pub const str_space_intervals = re_space_intervals;

pub fn inTable(table: []const Interval, code_point: u21) bool {
    for (table) |interval| {
        if (code_point < interval.low) return false;
        if (code_point <= interval.high) return true;
    }
    return false;
}
