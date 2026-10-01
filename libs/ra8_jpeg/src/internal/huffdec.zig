//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Huffman decoding tables for the baseline decoder.
//!
//! The encoder builds its tables from a fixed specification (`huffman.zig`);
//! the decoder builds them from whatever a DHT segment carried, so this is a
//! separate concern with a separate table type, fixed in layout by C.

const std = @import("std");
const spec = @import("spec");

/// Mirror of C `ra8_jpeg_htab_t`, one decoded Huffman table. Lives inside
/// `ra8_jpeg_dec_ctx_t`, which the C parser still allocates, so the layout is
/// asserted against what the C compiler reports.
pub const Table = extern struct {
    bits: [spec.Huff.lengths]u8,
    vals: [spec.Huff.max_symbols]u8,
    huffcode: [spec.Huff.max_symbols]u16,
    huffsize: [spec.Huff.max_symbols]u8,
    mincode: [spec.Huff.lengths]i32,
    maxcode: [spec.Huff.lengths]i32,
    valptr: [spec.Huff.lengths]u16,
    total: u16,

    /// Derive canonical codes from the BITS list, then the per-length
    /// min/max/valptr lookup, T.81 Annex C and sec F.2.2.3.
    ///
    /// `bits` and `vals` are filled by the DHT parser before this runs.
    pub fn build(self: *Table) void {
        self.total = self.assignSizes();
        self.assignCodes();
        self.assignLookup();
    }

    /// Spread the BITS counts into one code length per symbol, returning how
    /// many symbols the table holds.
    ///
    /// The C original wrote a 0 sentinel at `huffsize[total]`, which for a
    /// full 256-symbol table wrote one byte past the array and into the next
    /// field. Carrying `total` instead is the same information without the
    /// out-of-bounds write.
    fn assignSizes(self: *Table) u16 {
        var k: u16 = 0;
        for (self.bits, 0..) |count, i| {
            var j: u8 = 0;
            while (j < count) : (j += 1) {
                self.huffsize[k] = @intCast(i + 1);
                k += 1;
            }
        }
        return k;
    }

    /// Walk the symbols in length order, handing out consecutive codes and
    /// shifting left at every length change.
    fn assignCodes(self: *Table) void {
        if (self.total == 0) return;

        var code: u16 = 0;
        var size: u8 = self.huffsize[0];
        var k: u16 = 0;

        while (k < self.total) {
            while (k < self.total and self.huffsize[k] == size) {
                self.huffcode[k] = code;
                code += 1;
                k += 1;
            }
            if (k >= self.total) break;
            while (self.huffsize[k] != size) {
                code <<= 1;
                size += 1;
            }
        }
    }

    /// Per code length, the first and last code and where its symbols start.
    /// A length with no codes gets `maxcode = -1`, which no comparison can
    /// satisfy, so its `mincode` and `valptr` are never read.
    fn assignLookup(self: *Table) void {
        var j: u16 = 0;
        for (self.bits, 0..) |count, i| {
            if (count == 0) {
                self.maxcode[i] = -1;
                continue;
            }
            self.valptr[i] = j;
            self.mincode[i] = self.huffcode[j];
            j = (j + count) - 1;
            self.maxcode[i] = self.huffcode[j];
            j += 1;
        }
    }

    /// Read one symbol, one bit at a time, T.81 sec F.2.2.3 Figure F.16.
    /// Returns null on a malformed or truncated code; the C signature reports
    /// that as -1.
    pub fn decode(self: *const Table, reader: anytype) ?u8 {
        reader.fill();
        if (reader.nbits == 0) return null;

        var code: i32 = @intCast(reader.getBits(1) orelse return null);

        for (0..spec.Huff.lengths) |i| {
            if (code <= self.maxcode[i]) {
                const offset: u16 = @intCast(code - self.mincode[i]);
                const j = self.valptr[i] + offset;
                if (j >= self.total) return null;
                return self.vals[j];
            }
            const bit = reader.getBits(1) orelse return null;
            code = (code << 1) | @as(i32, @intCast(bit));
        }
        return null;
    }
};

/// Sign-extend an `n`-bit magnitude to its signed value, T.81 sec F.2.2.1
/// "EXTEND".
pub fn extend(value: i32, n: u8) i32 {
    if (n == 0) return 0;

    const threshold = @as(i32, 1) << @intCast(n - 1);
    if (value >= threshold) return value;

    const mask: u32 = @as(u32, 0xFFFFFFFF) << @intCast(n);
    return value + @as(i32, @bitCast(mask)) + 1;
}

comptime {
    std.debug.assert(@sizeOf(Table) == 1204);
    std.debug.assert(@alignOf(Table) == 4);
    std.debug.assert(@offsetOf(Table, "bits") == 0);
    std.debug.assert(@offsetOf(Table, "vals") == 16);
    std.debug.assert(@offsetOf(Table, "huffcode") == 272);
    std.debug.assert(@offsetOf(Table, "huffsize") == 784);
    std.debug.assert(@offsetOf(Table, "mincode") == 1040);
    std.debug.assert(@offsetOf(Table, "maxcode") == 1104);
    std.debug.assert(@offsetOf(Table, "valptr") == 1168);
    std.debug.assert(@offsetOf(Table, "total") == 1200);
}
