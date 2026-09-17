//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The one Unicode character class CPython's `re` module applies to the
//! patterns `roadmap_stats.py` carried, generated from the interpreter this
//! repository pins and checked in so the Zig tool needs no Python at runtime
//! (#858).
//!
//! `space_intervals` is `\s` for a str pattern, which CPython resolves to
//! `Py_UNICODE_ISSPACE`. That is also what `str.isspace` tests, and therefore
//! what `str.strip` removes, so ONE table serves both the three regexes
//! (`^###\s+(.+?)\s*$`, `` `\[([ x~!])\]`\s*Status: `` and
//! `^\s*\[([ x~!])\]`) and the `.strip()` in front of the fence test. That
//! equality was verified against the pinned interpreter when the predecessor
//! migrations generated this table, not assumed.
//!
//! No `\w` table: none of this tool's patterns spells `\w`, and the status
//! and checkbox marks are the literal class `[ x~!]`, so a non-ASCII
//! look-alike never marked a checkbox and must not start marking one now.

/// One inclusive code-point run.
pub const Interval = struct { low: u21, high: u21 };

/// Binary search a sorted, non-overlapping interval table.
pub fn inTable(table: []const Interval, code_point: u21) bool {
    var low: usize = 0;
    var high: usize = table.len;
    while (low < high) {
        const mid = low + (high - low) / 2;
        const entry = table[mid];
        if (code_point < entry.low) {
            high = mid;
        } else if (code_point > entry.high) {
            low = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

/// `\s` for a CPython str pattern, equal to `str.isspace`.
pub const space_intervals = [_]Interval{
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
