//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Allocation-free reader for R8LA v2 two-bit coverage runs.

pub const Decoder = struct {
    bytes: []const u8,
    cursor: usize,
    remaining: u8 = 0,
    current: u8 = 0,

    pub fn init(bytes: []const u8, offset: u32) Decoder {
        return .{ .bytes = bytes, .cursor = offset };
    }

    /// Return the next two-bit coverage value from a run-coded stream.
    pub fn next(self: *Decoder) u8 {
        if (self.remaining == 0) {
            const run = self.bytes[self.cursor];
            self.cursor += 1;
            self.current = run >> 6;
            self.remaining = (run & 0x3f) + 1;
        }
        self.remaining -= 1;
        return self.current;
    }
};
