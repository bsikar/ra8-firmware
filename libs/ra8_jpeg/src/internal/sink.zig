//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The encoder's output half (#2795): a byte cursor over the caller's buffer
//! plus the MSB-first entropy bit accumulator, with T.81 sec F.1.2.3 byte
//! stuffing. Capacity exhaustion latches `overflow` rather than writing past
//! the slice, so the whole encode can run to completion and be rejected once.

const spec = @import("spec");

pub const Sink = struct {
    /// Caller's destination buffer; its length is the capacity.
    dst: []u8,
    /// Write cursor into `dst`.
    pos: usize = 0,
    /// MSB-first accumulator of not-yet-whole entropy bytes.
    bit_buf: u32 = 0,
    /// Bits currently valid in `bit_buf`; always below 8 between pushes.
    bit_cnt: u8 = 0,
    /// Latched on the first write that would pass the end of `dst`.
    overflow: bool = false,

    const byte_bits: u8 = 8;

    /// Widest push the 32-bit accumulator can take alongside 7 carried bits.
    /// A wider one means a corrupt Huffman table, so it fails closed instead
    /// of shifting by more than the accumulator holds.
    pub const max_push_bits: u8 = 24;

    /// Append one byte, or latch overflow.
    pub fn byte(self: *Sink, value: u8) void {
        if (self.pos >= self.dst.len) {
            self.overflow = true;
            return;
        }
        self.dst[self.pos] = value;
        self.pos += 1;
    }

    /// Append a 16-bit big-endian word: marker codes and length fields.
    pub fn word(self: *Sink, value: u16) void {
        self.byte(@truncate(value >> byte_bits));
        self.byte(@truncate(value));
    }

    /// Push `n` bits of `code` MSB-first, flushing whole bytes and stuffing a
    /// `0x00` after every emitted `0xFF`.
    pub fn bits(self: *Sink, code: u32, n: u8) void {
        if (n == 0) return;
        if (n > max_push_bits) {
            self.overflow = true;
            return;
        }
        const width: u5 = @intCast(n);
        self.bit_buf = (self.bit_buf << width) | (code & mask(width));
        self.bit_cnt += n;

        while (self.bit_cnt >= byte_bits) {
            const shift: u5 = @intCast(self.bit_cnt - byte_bits);
            const out: u8 = @truncate(self.bit_buf >> shift);
            self.byte(out);
            if (out == spec.Marker.stuff_trigger) self.byte(0);
            self.bit_cnt -= byte_bits;
        }
    }

    /// Pad the trailing partial byte with ones so the scan ends on a byte
    /// boundary, T.81 sec F.1.2.3.
    pub fn flushBits(self: *Sink) void {
        if (self.bit_cnt == 0) return;
        const pad_width: u5 = @intCast(byte_bits - self.bit_cnt);
        const padded = (self.bit_buf << pad_width) | mask(pad_width);
        self.bits(padded & 0xFF, byte_bits - self.bit_cnt);
    }
};

/// Low `width` bits set.
pub fn mask(width: u5) u32 {
    return (@as(u32, 1) << width) - 1;
}
