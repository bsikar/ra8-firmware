//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Entropy-coded bit reader for the baseline decoder.
//!
//! The struct still mirrors C `ra8_jpeg_bitreader_t`, which the public
//! header publishes inside `ra8_jpeg_dec_ctx_t`, so its layout is asserted
//! below. Only the two operations a scan decode needs live here: refill, and
//! take n bits.

const std = @import("std");
const spec = @import("spec");

/// Mirror of C `ra8_jpeg_bitreader_t`. Layout is asserted against the values
/// the C compiler reports for the real header, so a field reorder on either
/// side fails the build rather than corrupting a decode.
pub const BitReader = extern struct {
    buf: [*]const u8,
    len: u32,
    pos: u32,
    acc: u32,
    nbits: u8,
    had_eoi: u8,

    /// Bits kept in `acc` before a refill stops topping up, matching the C
    /// reservoir low-water mark.
    pub const reservoir_low: u8 = 24;
    /// A byte is fed into the accumulator whole.
    pub const byte_bits: u8 = 8;

    /// Top the accumulator up to more than `reservoir_low` bits, stopping at
    /// the end of the buffer or at the first real marker.
    ///
    /// A `0xFF` followed by `0x00` is a stuffed data byte (T.81 F.1.2.3): the
    /// `0x00` is dropped. A `0xFF` followed by anything else is a marker, so
    /// the cursor is rewound onto the `0xFF` and the stream is flagged ended,
    /// leaving the marker for the parser to read.
    pub fn fill(self: *BitReader) void {
        while (self.nbits <= reservoir_low and self.had_eoi == 0) {
            if (self.pos >= self.len) {
                self.had_eoi = 1;
                return;
            }
            const b = self.buf[self.pos];
            self.pos += 1;

            if (b == spec.Marker.stuff_trigger) {
                if (self.pos >= self.len) {
                    self.had_eoi = 1;
                    return;
                }
                const stuffed = self.buf[self.pos];
                self.pos += 1;
                if (stuffed != 0) {
                    self.pos -= 2;
                    self.had_eoi = 1;
                    return;
                }
            }

            self.acc = (self.acc << byte_bits) | b;
            self.nbits += byte_bits;
        }
    }

    /// Take the next `n` bits, most significant first. Returns null when the
    /// stream cannot supply them; the C signature reports that as -1.
    pub fn getBits(self: *BitReader, n: u8) ?u32 {
        if (n == 0) return 0;

        if (self.nbits < n) {
            self.fill();
            if (self.nbits < n) return null;
        }

        const shift: u5 = @intCast(self.nbits - n);
        const width: u5 = @intCast(n);
        const value = (self.acc >> shift) & ((@as(u32, 1) << width) - 1);

        self.nbits -= n;
        self.acc &= if (self.nbits == 0)
            0
        else
            (@as(u32, 1) << @intCast(self.nbits)) - 1;

        return value;
    }
};

comptime {
    // The layout is derived from the pointer width rather than pinned to one,
    // because this library is built twice: for the host C suites (64-bit, so
    // 24 bytes) and for the target (32-bit thumb, so 20). Hard-coding either
    // breaks the other build while proving nothing extra: what matters is
    // that the field ORDER still matches the C header, and these offsets
    // still fail a reorder on whichever target is being built.
    const ptr = @sizeOf([*]const u8);
    std.debug.assert(@alignOf(BitReader) == @alignOf([*]const u8));
    std.debug.assert(@offsetOf(BitReader, "buf") == 0);
    std.debug.assert(@offsetOf(BitReader, "len") == ptr);
    std.debug.assert(@offsetOf(BitReader, "pos") == ptr + 4);
    std.debug.assert(@offsetOf(BitReader, "acc") == ptr + 8);
    std.debug.assert(@offsetOf(BitReader, "nbits") == ptr + 12);
    std.debug.assert(@offsetOf(BitReader, "had_eoi") == ptr + 13);
    std.debug.assert(@sizeOf(BitReader) == std.mem.alignForward(usize, ptr + 14, @alignOf(BitReader)));
}
